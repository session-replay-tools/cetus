# Cetus 读写分离版使用指南

## 简介

Cetus 读写分离版将前端发来的读请求和写请求分别发送到不同的服务器后端，由于底层的数据库都是Master/Slave架构，做到读写分离能大大提高数据库的处理能力。

## 安装部署

### 准备

**1. MySQL**

- 搭建MySQL主从关系

- 若开启主从延迟检测需创建库proxy_heart_beat和表tb_heartbeat：

    CREATE DATABASE proxy_heart_beat;

    USE proxy_heart_beat;

    CREATE TABLE tb_heartbeat (
    p_id varchar(128) NOT NULL,
    p_ts timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (p_id)
    ) ENGINE = InnoDB DEFAULT CHARSET = utf8;

- 创建用户和密码（默认用户对tb_heartbeat有读写权限）

- 确认Cetus可以远程登录MySQL

**2.Cetus**

- 根据MySQL后端信息配置users.json和proxy.conf（variables.json可选配），具体配置说明详见[Cetus 读写分离版配置文件说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw-profile.md)

**3.LVS & keepalived**

- 确定LVS的监听ip和端口

- 根据实际情况和需求确定多个Cetus的LVS分发权重

- 配置keepalived.conf

### 安装

Cetus只支持linux系统，安装过程详见[Cetus 安装说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-install.md)

### 部署

Cetus 在部署时架构图如下图所示。

![Cetus 读写分离版部署架构图](https://github.com/Lede-Inc/cetus/blob/master/doc/picture/Cetus_deployment_rw.png)

Cetus位于应用程序与MySQL数据库之间，作为前端应用与数据库的通讯。其中，前端应用连接LVS节点，LVS节点映射端口到多个Cetus服务，后者通过自身的连接池连接到后端的数据库。

同一个Cetus连接的MySQL数据库后端为一主一从或一主多从，不同Cetus共用后端。

MySQL和Cetus可以部署在同一台服务器上，也可以部署在各自独立的服务器上，但一般LVS & keepalived与MySQL、Cetus分布在不同的服务器上。

## 启动

Cetus可以利用bin/cetus启动

```
bin/cetus --defaults-file=conf/proxy.conf [--conf-dir=/home/user/cetus_install/conf/]
```

其中Cetus启动时可以添加命令行选项，--defaults-file选项用来加载启动配置文件，且在启动前保证启动配置文件的权限为660；--conf-dir是可选项，用来加载其他配置文件(.json文件)，默认为当前目录下conf文件夹。

Cetus可起动守护进程后台运行，也可在进程意外终止自动启动一个新进程，可通过启动配置选项进行设置。

## 主要功能概述

### 1.连接池功能

Cetus内置了连接池功能。该连接池会在Cetus启动时，使用默认用户自动创建到后端的连接。创建的连接使用的database是配置的默认database，这些选项是由DBA根据后端数据库的实际情况进行的配置。以确保主要的业务请求过来不需要临时建立连接并切换数据库。

### 2.读写分离功能

Cetus的读写分离功能可以通过解析SQL，结合请求状态将查询语句下发到后端的只读从库进行查询，从而减少主库的负载。提升系统可用容量。

Cetus能提供对分流策略的自定义。比如可以设置为：把30%的读流量，分流到主节点，70%的读流量分流到只读节点。

由于读写分离需要对SQL进行解析才能实现判断，所以可能出现误判的情况，请大家书写SQL时尽量按照标准语法进行。并且在发现可疑问题时，及时进行抓包，方便开发进行分析、判断。

### 3.结果集压缩

由于当连接距离较远网络延迟较大时，结果集较大会很大幅度地增加数据传输时长，降低性能，因此针对高延迟场合，Cetus支持对结果集的压缩来提高性能。

网络延迟越大，开启结果集压缩功能的优势越大，当连接距离较近几乎没有网络延迟时，Cetus无形中增加了压缩和解压步骤，反而降低了性能，因此在延迟较小时不建议使用结果集压缩。

### 4.安全性管理

安全性管理功能包括后端管理、基本配置管理、查看连接信息、用户密码管理、IP许可管理、远程配置中心管理和整体信息查询。

用户可以通过登录管理端口，查看后端状态，增删改指定后端，查看和修改基本配置（包括连接池配置和从库延迟检测配置），查看当前连接的详细信息，用户连接Cetus的密码查询和增删改，查看以及增删改IP许可(可以为每个用户以及管理员用户指定允许访问的来源ip地址，但是无法为来自不同ip的同一用户提供不同的权限设置)，重载远程配置和查看Cetus总体状态等，具体管理操作相见[Cetus 读写分离版管理手册](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw-admin.md)。

### 5.状态监控

Cetus内置监控功能，可通过配置选择开启或关闭。开启后Cetus会定期和后端进行通信，检测后端数据库的存活状态和主从延迟时间等。可通过登录管理端口SELECT * FROM backends查看。

后端状态包括unknown（后端初始状态，还未建立连接）、up（能与后端正常建立连接，可以正常提供服务）、down（与后端无法联通，无法正常提供服务）、maintaining（后端正在维护，无法建立连接）和delete（后端已被删除）。

主从延迟检测检测设有阈值slave-delay-down和slave-delay-recover，检测到的从库延迟超过slave-delay-down阈值后端状态将被设置为DOWN，从库延迟少于slave-delay-recover阈值后端状态将恢复为UP。

### 6.TCP流式

针对结果集过大情况，Cetus采用了tcp stream流式，不需要先缓存完整结果集才转发给客户端，以避免内存炸裂问题，降低内存消耗，提高性能。

### 7.支持prepare语句

支持用户使用prepare语句，方法有两种：A）客户端级别的prepare ；B）server端的prepare支持。

### 8.域名连接后端

支持利用域名连接数据库后端，Cetus设有相关的启动配置选项disable-dns-cache，选择是否开启解析连接到后端的域名功能，开启后可以通过设置域名并利用域名访问后端。

## 注意事项

### 1.连接池使用注意事项

所有的后端MySQL节点，都需要有相同的用户名及口令，在conf/users.json中指定，不在选项中指定的用户，不能用来登录Cetus，不能为每个不同的后端实例单独指定登录信息。用户连接Cetus时的用户名和密码不一定与后端一致，可在conf/users.json中查看。

### 2.set命令的支持说明

我们支持使用以下session级别的set命令： Set  names/ sql_mode/ autocommit，其中Set sql_mode 仅支持以下 3 个：STRICT_TRANS_TABLES/  NO_AUTO_CREATE_USER/ NO_ENGINE_SUBSTITUTION，支持set session transaction read write/readonly等指令。global级别的set命令统一不支持，其他未列出的set命令不在支持范围内，不建议使用。如果业务确实需要，建议联系DBA将行为配置为默认开启。

支持CLIENT_FOUND_ROWS 全局参数属性的统一设置，同一个Cetus只能选择打开或者关闭，不支持对每个连接单独设置这项属性，可以通过设置启动配置选项来打开，默认关闭。不支持CLIENT_LOCAL_FILES。其他未列出的需要额外设置的特性请测试后再确认。

### 3.环境变量修改建议

虽然我们支持客户端对连接环境变量进行修改，但是，我们不建议在程序中进行修改。因为一旦变量有改动。我们需要在执行SQL前对连接状态进行复位，会产生额外的请求到服务端，客户端响应的延迟也会增加。

### 4.使用注释来选择后端

开发需要尽量在处理业务逻辑时，避免对写入的数据进行立即查询。因为主从之间可能存在一定的同步延迟。所以可能出现写入数据和读取不一致的情况，如果应用对这项特别敏感，可以使用注释的方式，提示Cetus将读请求直接发送到主库进行查询，避免延迟。

注释的使用方式为在SQL中SELECT字段之后插入/\*# mode=READWRITE \*/；部分应用可能对数据准确性特别敏感，这种情况下，我们可以设置默认所有请求都走主节点，但是，对于部分后台的统计分析功能，主要分析历史数据时，我们可以通过SQL指定后端到只读节点，来减少批量查询业务对主库的影响，我们可以在SELECT字段后插入/\*# mode=READONLY \*/来指定Cetus将SQL发送到只读从库进行执行。

**注：若使用注释请在连接Cetus时加上-c参数，如 mysql --prompt="proxy> " --comments -hxxx.xxx.xxx.xxx -Pxxxx -uxxxx -pxxx -c**

### 5.不支持 Kill query

不支持在SQL执行过程中 kill query操作，一旦SQL语句开始执行就不能通过这种方式来终止，此时可以连接Cetus管理后端，通过执行 show connectionlist 命令查看正在执行的SQL，从而找到正在执行的后端信息，通过数据库中 kill query的命令进行终止操作。

### 6.不支持TLS

不支持TLS协议，目前不能在某种程度上使主从架构应用程序通讯本身预防窃听、干扰和消息伪造。

### 7.不支持多租户

目前读写版本还不支持，可以联系dba在数据库设计方面进行设置。

### 8.不支持分表

TODO

### 9.SQL支持

由于读写分离只需要对SQL进行路由，不需要进行SQL改写，而且每个数据库后端都是全量的数据，所以，能支持绝大多数的SQL语句。除下述需要注意的用法外，基本都支持。

**1.LAST_INSERT_ID特性有变化**

由于后端的连接存在复用。所以，在查询LAST_INSERT_ID时，会存在错误的情况，为避免这种情况，Cetus实现了缓存上次操作返回LAST_INSERT_ID的功能。目前只支持单独的SQL进行查询。不支持将LAST_INSERT_ID 嵌套在INSERT或者其他的语句中。建议的使用方式是在INSERT完之后。直接获取，或者在事后，直接使用 SELECT LAST_INSERT_ID() 来获取insert id。

**2.事务的处理**

事务中的所有操作都将发送到主库进行执行，避免数据不一致造成的干扰。

**3.尽量避免服务端PREPARE**

虽然Cetus读写分离版支持服务端的PREPARE，但是服务端的PREPARE会降低连接池的使用效率，除非应用程序不兼容。否则，不建议使用服务端PREPARE功能。

**4.DDL语句以及其他语句**

读写分离版本支持DDL语句，所有的非查询语句都将发送到主库，后续的变更需要依赖主从之间的同步保持一致。不建议程序直接调用存储过程，如需调用，需要进行详细的测试，确认满足应用的需求的情况下才能使用。

**5.不支持客户端的change user命令**

## 应用示例

### 1.连接Cetus

```
    $ mysql --prompt="proxy> " --comments -h**.**.**.** -P**** -u**** -p***
    proxy> 
```

在连接Cetus时，使用在配置文件中确认好的用户名和密码登陆，登陆的ip和端口为Cetus监听的proxy-address的ip和端口。

可同时启动监听同一个ip不同端口的Cetus。

### 2.管理Cetus

```
   $  mysql --prompt="admin> " --comments -h**.**.**.** -P**** -u**** -p***
   admin> select * from backends；
```

可以使用在配置文件中的admin用户名和密码，登陆地址为admin-address的MySQL对Cetus进行管理，例如在查询Cetus的后端详细信息时，可以登录后通过命令 select * from backends，显示后端端口的地址、状态、读写类型，以及读写延迟时间和连接数等信息。

具体使用说明详见[Cetus 读写分离版管理手册](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-rw-admin.md)

### 3.查看Cetus参数

```
   $  cd /home/user/cetus_install/
   bin/cetus --help | -h
```

可以查看帮助，获取Cetus启动配置选项的参数及含义。
