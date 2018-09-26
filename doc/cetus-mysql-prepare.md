# Cetus MySQL准备说明

MySQL建议使用5.7.16以上版本

## 创建数据库实例

创建数据库实例，以及Cetus连接MySQL的用户和密码。

例如：

Cetus所在的主机ip为192.0.0.1

CREATE database if not exists testdb;
GRANT USAGE ON *.* TO 'cetus_app'@'192.0.0.1' identified by 'cetus_app';
GRANT all ON `testdb`.* TO 'cetus_app'@'192.0.0.1';

## 读写分离版MySQL环境准备

若使用读写分离功能则需要搭建MySQL主从关系，若开启读写延迟检测需要创建心跳库和表

### 搭建MySQL主从关系

例如：

- 在主库上操作：

**创建repl用户**

192.0.0.1为MySQL从库ip

CREATE USER 'repl'@'192.0.0.1' IDENTIFIED BY 'xxxxxx';     
REVOKE ALL PRIVILEGES ,GRANT OPTION FROM 'repl'@'192.0.0.1';
GRANT RELOAD,LOCK TABLES, REPLICATION CLIENT ,REPLICATION SLAVE ON *.* TO 'repl'@'192.0.0.1';
FLUSH PRIVILEGES;

- 在从库上操作：

**开启主从复制**

MySQL主库ip为192.0.0.1

1) 非gtid
change master TO master_host='192.0.0.1',
master_user='repl',master_password='xxxxxx',
master_port=3306,master_log_file='mysql-bin.000001',
master_log_pos=1124;
start slave;

2) gtid
CHANGE MASTER TO
  MASTER_HOST='192.0.0.1',
  MASTER_USER='repl',
  MASTER_PASSWORD='xxxxxx',
  MASTER_PORT=3306,
  master_auto_position=1,
  MASTER_CONNECT_RETRY=10;
start slave;

### 主从延迟检测准备

创建心跳库和心跳表，并为用户授权

例如：

Cetus所在的主机ip为192.0.0.1

create database if not exists proxy_heart_beat;
use proxy_heart_beat;       
CREATE TABLE if not exists `tb_heartbeat` (
  `p_id` varchar(128) NOT NULL ,
  `p_ts` timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`p_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

GRANT ALL ON `proxy_heart_beat`.* TO 'cetus_app'@'192.0.0.1';

**注意：创建心跳表时p_ts精度必须到小数点后，否则会影响主从延迟检测的准确度**

## sharding版MySQL环境准备

若使用sharding功能，则需要根据业务创建业务表，并进行分库设计,若开启主从延迟检测，请参考读写分离版本配置心跳库和心跳表。
