# Cetus rpm说明

##  简介

Cetus rpm打包流程及安装

## 打包流程

### 1. 创建打包环境

```
mkdir rpmbuild/
cd rpmbuild/ 
mkdir -pv {BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} 
```

### 2. 下载源码，压缩成tar.gz格式，放入SOURCES中

```
git clone https://github.com/Lede-Inc/cetus.git
tar zcvf cetus-version.tar.gz cetus/
```

### 3. 将cetus的原spec文件放入SPECS中,编辑sepc文件，修改版本号和释出号等信息

```
#版本号,与tar.gz包的一致
Version:        version
#释出号，也就是第几次制作rpm
Release:        release%{?dist}
```

### 4.增加日志段

```
%changelog
* Week month day year packager<email> - cetus-version-release
- do something
```

## 安装

安装命令如下：

```
rpm -ivh --prefix /home/user/cetus_install cetus-version-release.el7.x86_64.rpm
```
