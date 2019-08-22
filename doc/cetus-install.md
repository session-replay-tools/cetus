# Cetus 安装说明

## 安装依赖

编译安装Cetus存在以下依赖：

- cmake
- gcc
- glib2-devel （version >= 2.6.0)
- zlib-devel
- flex
- mysql-devel 5.6+／mariadb-devel 
- gperftools-libs (由于malloc存在着潜在的内存碎片问题，建议采用gperftools-libs中的tcmalloc)

CentOS 系统可以通过以下命令安装依赖包：

```
yum install cmake gcc glib2-devel flex mysql-devel gperftools-libs zlib-devel -y 
```

SUSE 系统可以通过以下命令安装依赖包:

```
## 安装命令
zypper in cmake
zypper in gcc
zypper in glib2-devel
zypper in zlib-devel
zypper in flex
zypper in mysql-devel
zypper in gperftools-libs
## 注意1: cmake工具的版本
## Cetus目前要求cmake工具版本要大于等于2.8.11
## SUSE11某版本上cmake版本低于2.8.11，因此需要升级cmake
## 下述流程为源码编译cmake的步骤
zypper in gcc-c++
wget https://cmake.org/files/v3.9/cmake-3.9.2.tar.gz
tar -xvf cmake-3.9.2.tar.gz
cd cmake-3.9.2
./configure
make && make install
mv /usr/bin/cmake /usr/bin/cmake2.8.4
ln -s /usr/local/bin/cmake /usr/bin/cmake
cmake --version
## 注意2： 没有正确安装mysql-devel
## 可能会报错 error Only <glib.h> can be included directly
## 原因可能是mysql-devel没有正确安装，没有正确安装的原因可能是其依赖的库无法找到等等，可以手动安装依赖库等等
```

请确保在编译安装Cetus前已安装好相应的依赖。

**注意**：如果已经安装了gperftools-libs，但是编译cetus时仍然提示找不到tcmalloc，则可以通过建立软链接的形式，解决。
```
# 建立软链接
ln -s libtcmalloc.so.4 libtcmalloc.so
# cmake的时候提示如下，便成功链接tcmalloc
-- Looking for malloc in tcmalloc
-- Looking for malloc in tcmalloc - found
```

## 安装步骤

Cetus利用自动化建构系统CMake进行编译安装，其中描述构建过程的构建文件CMakeLists.txt已经在源码中的主目录和子目录中，下载源码并解压后具体安装步骤如下：

- 创建编译目录：在源码主目录下创建独立的目录build，并转到该目录下

```
mkdir build/
cd build/
```

- 编译：利用cmake进行编译，指令如下

```
读写分离版本：
CFLAGS='-g -Wpointer-to-int-cast' cmake ../ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/home/user/cetus_install -DSIMPLE_PARSER=ON

分库版本：
CFLAGS='-g -Wpointer-to-int-cast' cmake ../ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/home/user/cetus_install -DSIMPLE_PARSER=OFF

```

其中CFLAGS='-g -Wpointer-to-int-cast'在可执行程序中包含标准调试信息，CMAKE_BUILD_TYPE变量可以选择生成 debug 版和或release 版的程序，CMAKE_INSTALL_PREFIX变量确定软件的实际安装目录的绝对路径，安装目录建议以/home/user/日期.编译版本.分支.commit_id的方式命名；SIMPLE_PARSER变量确定软件的编译版本，设置为ON则编译读写分离版本，否则编译分库版本；-DWITH_OPENSSL=ON可以用来开启SSL/TLS服务。

该过程会检查您的系统是否缺少一些依赖库和依赖软件，可以根据错误代码安装相应依赖。

- 安装：执行make install进行安装

```
make install
```

- 配置：Cetus运行前还需要编辑配置文件

```
cd /home/user/cetus_install/conf/
cp XXX.json.example XXX.json
cp XXX.conf.example XXX.conf
vi XXX.json
vi XXX.conf
```

配置文件在make insatll后存在示例文件，以.example结尾，目录为/home/user/cetus_install/conf/，包括用户设置文件（users.json）、变量处理配置文件（variables.json）、分库版本的分片规则配置文件（sharding.json）、读写分离版本的启动配置文件（proxy.conf）和分库版本的启动配置文件（shard.conf）。

根据具体编译安装的版本编辑相关配置文件，若使用读写分离功能则需配置users.json和proxy.conf，若使用sharding功能则需配置users.json、sharding.json和shard.conf，其中两个版本的variables.json均可选配。

配置文件的具体说明见[Cetus 读写分离版配置文件说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw-profile.md)和[Cetus 分库(sharding)版配置文件说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-shard-profile.md)。

- 启动：Cetus可以利用bin/cetus启动

```
读写分离版本：
bin/cetus --defaults-file=conf/proxy.conf [--conf-dir=/home/user/cetus_install/conf/]

分库版本：
bin/cetus --defaults-file=conf/shard.conf [--conf-dir=/home/user/cetus_install/conf/]

```

其中Cetus启动时可以添加命令行选项，--defaults-file选项用来加载启动配置文件（proxy.conf或者shard.conf），且在启动前保证启动配置文件的权限为660；--conf-dir是可选项，用来加载其他配置文件(.json文件)，默认为当前目录下conf文件夹。

Cetus可起动守护进程后台运行，也可在进程意外终止自动启动一个新进程，可通过启动配置选项进行设置。

## 注意事项

发现在某些最新版本的操作系统上（例如centos:7.5.1804），虽然成功安装了tcmalloc库，但是有可能cetus仍然没有正确的链接该库。以下方法可以检测cetus是否已经成功链接了tcmalloc库：

- cmake阶段

```
## cmake结束后，如果检测到系统安装有tcmalloc，会打印以下信息
-- Looking for malloc in tcmalloc
-- Looking for malloc in tcmalloc - found

## 如果检测不到tcmalloc，则会打印以下信，此时，需要检测tcmalloc安装是否正确
-- Looking for malloc in tcmalloc
-- Looking for malloc in tcmalloc - not found
```

- make install 阶段

```
## make install命令执行之后，编译出cetus可执行文件会被拷贝到安装目录，可以检测该执行文件的动态库是否动态链接了tcmalloc库
ldd ${cetus_install_path}/libexec/cetus|grep tcmalloc
```

如果发现已经安装过了tcmalloc相应的发行包，但是cetus却没有正确链接tcmalloc库，极大可能是没有找到libtcmalloc.so动态库，此时，应该手动建立软链接。建立方法：

```
## 以64位  centos7.5 为例
ls -alh /usr/lib64/|grep tcmalloc
lrwxrwxrwx  1 root root   20 Sep  4 00:58 libtcmalloc.so.4 -> libtcmalloc.so.4.4.5
-rwxr-xr-x  1 root root 295K Apr 11 01:41 libtcmalloc.so.4.4.5
lrwxrwxrwx  1 root root   33 Sep  4 00:58 libtcmalloc_and_profiler.so.4 -> libtcmalloc_and_profiler.so.4.4.5
-rwxr-xr-x  1 root root 315K Apr 11 01:41 libtcmalloc_and_profiler.so.4.4.5
lrwxrwxrwx  1 root root   26 Sep  4 00:58 libtcmalloc_debug.so.4 -> libtcmalloc_debug.so.4.4.5
-rwxr-xr-x  1 root root 351K Apr 11 01:41 libtcmalloc_debug.so.4.4.5
lrwxrwxrwx  1 root root   28 Sep  4 00:58 libtcmalloc_minimal.so.4 -> libtcmalloc_minimal.so.4.4.5
-rwxr-xr-x  1 root root 152K Apr 11 01:41 libtcmalloc_minimal.so.4.4.5
lrwxrwxrwx  1 root root   34 Sep  4 00:58 libtcmalloc_minimal_debug.so.4 -> libtcmalloc_minimal_debug.so.4.4.5
-rwxr-xr-x  1 root root 208K Apr 11 01:41 libtcmalloc_minimal_debug.so.4.4.5

## 发现没有 libtcmalloc.so，建立软链接
ln -s /usr/lib64/libtcmalloc.so.4.4.5 /usr/lib64/libtcmalloc.so

## 重新cmake
rm -rf CMakeCache.txt
根据所需的cetus版本执行对应cmake命令
```
