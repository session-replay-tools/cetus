# Cetus MySQL准备说明

MySQL建议使用5.7.16以上版本

## 读写分离版MySQL环境准备

若使用读写分离功能则需要搭建MySQL主从关系，若开启读写延迟检测需要创建心跳库和表

### 搭建MySQL主从关系

例如：

- 在主库上操作：

**创建repl用户**

假设192.0.0.1为MySQL从库ip，在主库上创建复制用的账号，并授权：

```

CREATE USER 'repl'@'192.0.0.1' IDENTIFIED BY 'xxxxxx';     
REVOKE ALL PRIVILEGES ,GRANT OPTION FROM 'repl'@'192.0.0.1';
GRANT RELOAD,LOCK TABLES, REPLICATION CLIENT ,REPLICATION SLAVE ON *.* TO 'repl'@'192.0.0.1';
FLUSH PRIVILEGES;
```


- 在从库上操作：

**开启主从复制**

假设MySQL主库ip为192.0.0.1，在每个从库上，配置主从复制并启动复制：

1) 非gtid

```
change master TO master_host='192.0.0.1',
master_user='repl',master_password='xxxxxx',
master_port=3306,master_log_file='mysql-bin.000001',
master_log_pos=1124;
start slave;
```


2) gtid

```
CHANGE MASTER TO
  MASTER_HOST='192.0.0.1',
  MASTER_USER='repl',
  MASTER_PASSWORD='xxxxxx',
  MASTER_PORT=3306,
  master_auto_position=1,
  MASTER_CONNECT_RETRY=10;
start slave;
```

## 创建数据库实例

创建数据库实例，以及Cetus连接MySQL的用户和密码。

例如：

假设Cetus所在的主机ip为192.0.0.1，直连主库在主库上创建数据库并授权：

```

CREATE database if not exists testdb;
GRANT USAGE ON *.* TO 'cetus_app'@'192.0.0.1' identified by 'cetus_app';
GRANT all ON `testdb`.* TO 'cetus_app'@'192.0.0.1';
```

### 主从延迟检测准备

在**主库**上创建心跳库和心跳表，从库会通过复制将心跳库和心跳表复制过来，因此**不需要在从库上再次创建**；与此同时，为用户授权

例如：

假设Cetus所在的主机ip为192.0.0.1，直连主库创建心跳表并授权：

```
create database if not exists proxy_heart_beat;
use proxy_heart_beat;       
CREATE TABLE if not exists `tb_heartbeat` (
  `p_id` varchar(128) NOT NULL ,
  `p_ts` timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`p_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

GRANT ALL ON `proxy_heart_beat`.* TO 'cetus_app'@'192.0.0.1';
```


**注意：创建心跳表时p_ts精度必须到小数点后，否则会影响主从延迟检测的准确度**

## sharding版MySQL环境准备

若使用sharding功能，则需要根据业务创建业务表，并进行分库设计,若开启主从延迟检测，请参考读写分离版本配置心跳库和心跳表。
