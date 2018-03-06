# Cetus 快速入门

## 环境说明

MySQL建议使用5.7.16以上版本，若使用读写分离功能则需要搭建MySQL主从关系，若使用sharding功能则需要根据业务进行分库设计；创建用户和密码并确认Cetus可以远程登录MySQL。

## 安装

Cetus只支持linux系统，安装步骤参考[Cetus 安装说明](https://git.ms.netease.com/dbproxy/cetus/wikis/cetus-install)。

## 部署

若使用读写分离功能则可以配置一主多从结构，即配置一个主库，0个或多个从库；若使用sharding功能则可以根据分库规则配置多个后端数据库。

## 启动

```
bin/cetus --defaults-file=conf/proxy.conf|shard.conf [--conf-dir＝/home/user/cetus_install/conf/]
```

## 连接

Cetus对外暴露两类端口：proxy｜shard端口和admin端口。proxy｜shard端口是Cetus的应用端口，用来与后台数据库和前端应用进行交互；admin端口是Cetus的管理端口，用户可以连接Cetus的管理端口对Cetus的后端状态、配置参数等进行查看和修改。

### 1. 连接Cetus应用端口

```
    $ mysql --prompt="proxy> " --comments -h**.**.**.** -P**** -u**** -p***
    proxy> 
```

在连接Cetus时，使用在配置文件中确认好的用户名和密码登陆，登陆的ip和端口为Cetus监听的proxy-address的ip和端口。

可同时启动监听同一个ip不同端口的Cetus。连接应用端口之后可正常发送Cetus兼容的sql语句。

### 2. 连接Cetus管理端口

```
   $  mysql --prompt="admin> " --comments -h**.**.**.** -P**** -u**** -p***
   admin> select * from backends；
```

可以使用在配置文件中的admin用户名和密码，登陆地址为admin-address的mysql对Cetus进行管理，例如在查询Cetus的后端详细信息时，可以登录后通过命令 select * from backends，显示后端端口的地址、状态、读写类型，以及读写延迟时间和连接数等信息。

具体使用说明根据版本情况详见[Cetus 读写分离版管理手册](https://git.ms.netease.com/dbproxy/cetus/wikis/cetus-rw-admin)、[Cetus 分库(sharding)版管理手册](https://git.ms.netease.com/dbproxy/cetus/wikis/cetus-shard-admin)