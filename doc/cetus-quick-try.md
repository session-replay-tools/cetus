# Cetus 快速入门

## 环境说明

MySQL建议使用5.7.16以上版本，若使用读写分离功能则需要搭建MySQL主从关系，若使用sharding功能则需要根据业务进行分库设计；创建用户和密码并确认Cetus可以远程登录MySQL，具体说明详见[Cetus MySQL准备说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-mysql-prepare.md)。

## 安装

Cetus只支持linux系统，安装步骤参考[Cetus 安装说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-install.md)，安装成功后Cetus提供了示例配置文件，在/home/user/cetus_install/conf/目录下，以.example结尾，用户可根据需求进行修改，配置修改根据安装的不同版本详见[Cetus 读写分离版配置文件说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw-profile.md)、[Cetus 分库(sharding)版配置文件说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-shard-profile.md)。

## 部署

若使用读写分离功能则可以配置一主多从结构，即配置一个主库，0个或多个从库；若使用sharding功能则可以根据分库规则配置多个后端数据库。

## 启动

Cetus有两种启动方式：1. 本地配置文件启动； 2. 远程配置库启动。用户可以根据实际情况任意选择启动方式。下述例子以**本地配置文件**启动方式为例叙述。

启动cetus之前，要保证配置文件（proxy.conf/shard.conf）的权限最小为660，可以通过以下命令修改权限：
```
chmod 660 proxy.conf
```

### 1. 命令行启动
```
bin/cetus --defaults-file=conf/proxy.conf|shard.conf [--conf-dir=/home/user/cetus_install/conf/]
```

### 2. service命令启动

源码路径下的scripts文件夹中的cetus.service文件提供了启动、关闭cetus的脚本，CentOS系统下的用户可以将其拷贝至系统/etc/init.d/目录下，将其改名为cetus，并将其CETUS_HOME修改成cetus的实际安装路径，同时根据安装的cetus的读写分离版本或是分库版本，修改CETUS_CONF。使用该脚本对cetus操作的命令如下：

```
service cetus start
service cetus stop
service cetus restart
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

具体使用说明根据版本情况详见[Cetus 读写分离版使用指南](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw.md)、[Cetus 分库(sharding)版使用指南](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-sharding.md)

### 2. 连接Cetus管理端口

**2.1 mysql客户端连接**

```
   $  mysql --prompt="admin> " --comments -h**.**.**.** -P**** -u**** -p***
   admin> show maintain status；
```

**2.2 Python连接**

支持Python的pymysql模块和MySQLdb模块连接管理端口。

- pymysql：

**注意：需要设置链接参数 autocommit=None**
```
#!/usr/bin/python
# -*- coding: utf-8 -*-
import pymysql as connector

conn = connector.connect(host="127.0.0.1", user="admin", passwd="admin", port=6666, autocommit=None )
cursor = conn.cursor()
cursor.execute("show maintain status")
data = cursor.fetchone()
print "maintain status: %s" % data
```

- MySQLdb：
```
#!/usr/bin/python
# -*- coding: utf-8 -*-
import MySQLdb as connector

conn = connector.connect(host="127.0.0.1", user="admin", passwd="admin", port=6666)
cursor = conn.cursor()
cursor.execute("show maintain status")
data = cursor.fetchone()
print "maintain status: %s" % data
```

可以使用在配置文件中的admin用户名和密码，登陆地址为admin-address的mysql对Cetus进行管理，例如在查询Cetus的后端详细信息时，可以登录后通过命令 select * from backends，显示后端端口的地址、状态、读写类型，以及读写延迟时间和连接数等信息。

具体使用说明根据版本情况详见[Cetus 读写分离版管理手册](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw-admin.md)、[Cetus 分库(sharding)版管理手册](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-shard-admin.md)

**注：Cetus读写分离和分库两个版本的使用约束详见[Cetus 使用约束说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-constraint.md)**

## MGR 支持

目前Cetus支持**单主模式**的MGR集群。对MGR支持通过在*.conf文件中设置参数：`group-replication-mode = 1`来实现，默认该参数为0，即只支持普通MySQL主从复制的集群。该参数支持通过admin端口动态的设置。

特别注意，*.conf文件中配置的参数default-username在开启MGR模式后，需要对performance_schema.global_status和performance_schema.replication_group_members表进行查询从而获得MGR集群相关信息，因此在对default-username授权时候，需要特别注意。例如授权可以参考：

```
## 创建default-username账号（例如*.conf配置：default-username=cetus_app, default-db=test）
CREATE USER 'cetus_app'@'172.17.0.*' IDENTIFIED BY 'Cetus_2019,2,18';

## default-username对default-db/业务库的相关表授权
GRANT SELECT, INSERT, UPDATE, DELETE ON test.* TO 'cetus_app'@'172.17.0.*';

## MGR涉及表授权
GRANT SELECT ON `performance_schema`.`replication_group_members` TO 'cetus_app'@'172.17.0.*';
GRANT SELECT ON `performance_schema`.`global_status` TO 'cetus_app'@'172.17.0.*';

```

## MySQL8 支持

由于MySQL8.0用户权限认证插件新增了caching\_sha2\_password，并且默认创建的用户权限认证插件为该插件，MySQL55/56/57不支持该认证方式。因此在使用MySQL55/56/57库编译的Cetus时，配置default\-username账号，应该在MySQL上创建时指定插件为mysql_native_password，否则Cetus的监控线程无法工作，影响Cetus的正常使用。

配置方法示例如下：

```
create user 'default-user'@'%' identified with mysql_native_password by 'my_password';
grant all privileges on *.* to 'default-user'@'%';
```

## 特别注意

1. 在使用cetus的时候，**不要**将后端MySQL的全局autocommit模式设置为OFF/0。如果需要使用隐式提交，可以在业务端配置该参数，例如在Java客户端的jdbcUrl中配置autoCommit=false。

2. 不要在分布式事务中使用隐式提交的SQL（如DDL），否则XA协议会报错：ERROR 1399 (XAE07): XAER_RMFAIL
