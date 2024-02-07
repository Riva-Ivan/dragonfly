# dragonfly编译手册

## 安装依赖

~~~
Ubuntu系统
sudo apt install ninja-build libunwind-dev libboost-fiber-dev libssl-dev \
     autoconf-archive libtool cmake g++ libzstd-dev bison libxml2-dev
apt install lua5.4
apt install zlib1g zlib1g-dev -y
~~~

## 下载源码

~~~
git clone --recursive https://github.com/dragonflydb/dragonfly && cd dragonfly
~~~

## 编译源码

修改helio/blaze.sh文件中的-release，用于生成调试符号，修改如下（屏蔽release构建类型）：

~~~
case "$ARG" in
    -release)
        # TARGET_BUILD_TYPE=Release
        # BUILD_SUF=opt
        shift
        ;;
~~~

~~~
./helio/blaze.sh -release
cd build-dbg && ninja dragonfly
~~~

## docker编译

docker中可以只用源码根目录的build.sh文件中的命令编译



代码作者使用vscode开发，源码下有vscode启动调试配置，不用添加。直接F5即可启动调试。

源码启动后中断，不能找到主程序，可修改.vscode/launch.json的启动断点配置，修改如下：

~~~
"stopAtEntry": true,
~~~

可在浏览器中通过ip地址:6379方式查看本地端口网页。

虚拟机启动时，有时会报线程多，缺少内存，可以在hyper-v中将内存设置为32G（在启动动态内存的上方）,关闭动态内存