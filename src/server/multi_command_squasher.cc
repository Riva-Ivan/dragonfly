#include "server/multi_command_squasher.h"

#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/engine_shard_set.h"
#include "server/transaction.h"

namespace dfly {

using namespace std;
using namespace facade;

namespace {

template <typename F> void IterateKeys(CmdArgList args, KeyIndex keys, F&& f) {
  for (unsigned i = keys.start; i < keys.end; i += keys.step)
    f(args[i]);

  if (keys.bonus)
    f(args[keys.bonus]);
}

}  // namespace

MultiCommandSquasher::MultiCommandSquasher(absl::Span<StoredCmd> cmds, ConnectionContext* cntx)
    : cmds_{cmds}, cntx_{cntx}, base_cid_{cntx->transaction->GetCId()} {
  auto mode = cntx->transaction->GetMultiMode();
  track_keys_ = (mode == Transaction::LOCK_INCREMENTAL) || (mode == Transaction::NON_ATOMIC);
}

MultiCommandSquasher::ShardExecInfo& MultiCommandSquasher::PrepareShardInfo(ShardId sid) {
  if (sharded_.empty())
    sharded_.resize(shard_set->size());

  auto& sinfo = sharded_[sid];
  if (!sinfo.local_tx)
    sinfo.local_tx = new Transaction{cntx_->transaction};

  if (!sinfo.reply_chan)
    sinfo.reply_chan = make_unique<ReplyChan>(kChanBufferSize, 1);

  return sinfo;
}

MultiCommandSquasher::SquashResult MultiCommandSquasher::TrySquash(StoredCmd* cmd) {
  if (!cmd->descr->IsTransactional() || (cmd->descr->opt_mask() & CO::BLOCKING) ||
      (cmd->descr->opt_mask() & CO::GLOBAL_TRANS))
    return SquashResult::NOT_SQUASHED;

  auto keys = DetermineKeys(cmd->descr, cmd->ArgList());
  if (!keys.ok())
    return SquashResult::ERROR;

  // Check if all commands belong to one shard
  bool found_more = false;
  ShardId last_sid = kInvalidSid;
  IterateKeys(cmd->ArgList(), *keys, [&last_sid, &found_more](MutableSlice key) {
    if (found_more)
      return;
    ShardId sid = Shard(facade::ToSV(key), shard_set->size());
    if (last_sid == kInvalidSid || last_sid == sid) {
      last_sid = sid;
      return;
    }
    found_more = true;
  });

  if (found_more || last_sid == kInvalidSid)
    return SquashResult::NOT_SQUASHED;

  if (track_keys_)
    IterateKeys(cmd->ArgList(), *keys, [this](MutableSlice key) { collected_keys_.insert(key); });

  auto& sinfo = PrepareShardInfo(last_sid);

  sinfo.had_writes |= (cmd->descr->opt_mask() & CO::WRITE);
  sinfo.cmds.push_back(cmd);
  order_.push_back(last_sid);

  // Because the squashed hop is currently blocking, we cannot add more than the max channel size,
  // otherwise a deadlock occurs.
  bool need_flush = sinfo.cmds.size() >= kChanBufferSize - 1;
  return need_flush ? SquashResult::SQUASHED_FULL : SquashResult::SQUASHED;
}

void MultiCommandSquasher::ExecuteStandalone(StoredCmd* cmd) {
  DCHECK(order_.empty());  // check no squashed chain is interrupted

  auto* tx = cntx_->transaction;
  tx->MultiSwitchCmd(cmd->descr);
  if (cmd->descr->IsTransactional())
    tx->InitByArgs(cntx_->conn_state.db_index, cmd->ArgList());
  cmd->descr->Invoke(cmd->ArgList(), cntx_);
}

OpStatus MultiCommandSquasher::SquashedHopCb(Transaction* parent_tx, EngineShard* es) {
  auto& sinfo = sharded_[es->shard_id()];
  DCHECK(!sinfo.cmds.empty());

  auto* local_tx = sinfo.local_tx.get();
  facade::CapturingReplyBuilder crb;
  ConnectionContext local_cntx{local_tx, &crb};

  for (auto* cmd : sinfo.cmds) {
    local_tx->MultiSwitchCmd(cmd->descr);
    local_tx->InitByArgs(parent_tx->GetDbIndex(), cmd->ArgList());
    cmd->descr->Invoke(cmd->ArgList(), &local_cntx);

    sinfo.reply_chan->Push(crb.Take());
  }

  // ConnectionContext deletes the reply builder upon destruction, so
  // remove our local pointer from it.
  local_cntx.Inject(nullptr);
  return OpStatus::OK;
}

void MultiCommandSquasher::ExecuteSquashed() {
  if (order_.empty())
    return;

  VLOG(1) << "Executing " << order_.size() << " commands squashed";

  Transaction* tx = cntx_->transaction;

  if (track_keys_) {
    tmp_keylist_.assign(collected_keys_.begin(), collected_keys_.end());
    tx->PrepareSquashedMultiHop(base_cid_, CmdArgList{tmp_keylist_});
  } else {
    auto cb = [this](ShardId sid) { return !sharded_[sid].cmds.empty(); };
    tx->PrepareSquashedMultiHop(base_cid_, cb);
  }

  tx->ScheduleSingleHop([this](auto* tx, auto* es) { return SquashedHopCb(tx, es); });

  facade::CapturingReplyBuilder::Payload payload;
  RedisReplyBuilder* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  for (auto idx : order_) {
    CHECK(sharded_[idx].reply_chan->Pop(payload));
    CapturingReplyBuilder::Apply(move(payload), rb);
  }

  for (auto& sinfo : sharded_)
    sinfo.cmds.clear();

  order_.clear();
  collected_keys_.clear();
}

void MultiCommandSquasher::Run() {
  for (auto& cmd : cmds_) {
    auto res = TrySquash(&cmd);

    if (res == SquashResult::ERROR)
      break;

    if (res == SquashResult::NOT_SQUASHED || res == SquashResult::SQUASHED_FULL)
      ExecuteSquashed();

    if (res == SquashResult::NOT_SQUASHED)
      ExecuteStandalone(&cmd);
  }

  ExecuteSquashed();  // Flush leftover

  if (!sharded_.empty())
    cntx_->transaction->ReportWritesSquashedMulti(
        [this](ShardId sid) { return sharded_[sid].had_writes; });
}

}  // namespace dfly