gen_flex(jsonpath_lexer)

cur_gen_dir(gen_dir)

add_library(jsonpath ${gen_dir}/jsonpath_lexer.cc)
target_link_libraries(jsonpath base absl::strings TRDP::reflex)
