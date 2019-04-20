# Cetus rpm说明

##  简介

Cetus rpm打包流程及安装

## 1 打包流程

### 1.1 安装依赖

Cetus编译的依赖包可以参考[Cetus 安装说明](https://github.com/cetus-tools/cetus/blob/master/doc/cetus-install.md)，除此之外，可能仍需要安装以下依赖包:

```
yum install libevent-devel openssl-devel tar -y
yum install rpm-build rpmdevtools -y
```

### 1.2 创建打包环境

```
rpmdev-setuptree
```

随后默认会在的`~/rpmbuild/`目录下创建{BUILD, RPMS, SOURCES, SPEC, SRPM}等文件夹。

### 1.3 脚本一键式打包

```
cd cetus/srcipt/
chmod +x ./build_cetus_rpm.sh
./build_cetus_rpm.sh -v 2 -r 6 -s 0
```

build\_cetus\_rpm.sh脚本接受3个参数： -v 指定version信息；-r 指定release信息；-s指定编译的是读写分离版本还是分库版本，1表示读写分离版本，0表示分库版本，默认为1。

**注意**：打的RPM包会被拷贝到执行./build_cetus_rpm.sh的目录。

## 2 安装

可以通过`--prefix`指定安装路径。

```
rpm -ivh --prefix=/home/user/cetus_install cetus-version-release.el6.x86_64.rpm
```
