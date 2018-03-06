# Cetus 安装说明

## 安装依赖

​编译安装Cetus存在以下依赖：

- cmake
- gcc
- glib2-devel
- flex
- libevent-devel
- mysqldb-devel／mariadb-devel

请确保在编译安装Cetus前已安装好相应的依赖。

## 安装步骤

​Cetus利用自动化建构系统CMake进行编译安装，其中描述构建过程的构建文件CMakeLists.txt已经在源码中的主目录和子目录中，下载源码并解压后具体安装步骤如下：

- 创建编译目录：在源码主目录下创建独立的目录build，并转到该目录下

```
mkdir build/
cd build/
```

- 配置环境变量：设置环境变量CFLAGS如下

```
export CFLAGS='-D_USE_XOPEN -D_GNU_SOURCE'
```

- 编译：利用cmake进行编译，指令如下

```
cmake ../ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/home/user/cetus_install [-DSIMPLE_PARSER=ON]
```

​其中CMAKE_BUILD_TYPE变量可以选择生成 debug 版和或release 版的程序，CMAKE_INSTALL_PREFIX变量确定软件的实际安装目录的绝对路径；SIMPLE_PARSER变量是可选项，添加则编译读写分离版本，否则编译分库版本。

​该过程会检查您的系统是否缺少一些依赖库和依赖软件，可以根据错误代码安装相应依赖。

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

配置文件的具体说明见[Cetus 配置文件说明](https://git.ms.netease.com/dbproxy/cetus/wikis/cetus-profile)。

- 启动：Cetus可以利用bin/cetus启动

```
bin/cetus --defaults-file=conf/XXX.conf [--conf-dir＝/home/user/cetus_install/conf/]
```

​其中Cetus启动时可以添加命令行选项，--defaults-file选项用来加载启动配置文件，且在启动前保证启动配置文件的权限为660；--conf-dir是可选项，用来加载其他配置文件(.json文件)，默认为当前目录下conf文件夹。

​Cetus可起动守护进程后台运行，也可在进程意外终止自动启动一个新进程，可通过启动配置选项进行设置。