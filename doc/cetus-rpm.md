# Cetus rpm说明

##  简介

Cetus rpm打包流程及安装

## 1 打包流程

### 1.1 创建打包环境

```
mkdir rpmbuild/
cd rpmbuild/ 
mkdir -pv {BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} 
```

### 1.2 下载源码，压缩成tar.gz格式，放入SOURCES中

```
git clone https://github.com/Lede-Inc/cetus.git
tar zcvf cetus-version.tar.gz cetus/
```

### 1.3 将cetus的原spec文件放入SPECS中,编辑sepc文件，修改版本号和释出号等信息

```
#版本号,与tar.gz包的一致
Version:        version
#释出号，也就是第几次制作rpm
Release:        release%{?dist}
```

### 1.4 增加日志段

```
%changelog
* Week month day year packager<email> - cetus-version-release
- do something
```

## 2 打RPM包例子

在Cetus源码目录./script中，提供了Cetus打RPM包需要的描述文件cetus.spec；与此同时，为了让用户更方便、快捷打RPM包，提供了完整的打包脚本。

在安装好依赖库之后，便可以进行打包流程。

### 2.1 打包前准备
安装打包需要的依赖库。
> yum install rpm-build
> 
> yum install rpmdevtools
> 
> rpmdev-setuptree

随后默认会在的`~/rpmbuild/`目录下创建{BUILD, RPMS, SOURCES, SPEC, SRPM}等文件夹。

### 2.2 打包
执行打包脚本：
>cd cetus/srcipt/
>
> chmod +x ./build_cetus_rpm.sh
> 
> ./build_cetus_rpm.sh -v 1 -r 1 -s 0
> 
build_cetus_rpm.sh脚本接受3个参数： -v 指定version信息；-r 指定release信息；-s指定编译的是读写分离版本还是分库版本，1表示读写分离版本，0表示分库版本，默认为1。

注意，打的RPM包会被拷贝到执行./build_cetus_rpm.sh的目录。

## 3 安装

安装命令如下：

```
rpm -ivh --prefix=/home/user/cetus_install cetus-version-release.el6.x86_64.rpm
```
