# Cetus sharding版本管理手册

## 前言

**有配置修改均能动态生效，配置更改后请务必修改原始配置文件，以确保下次重启时配置能够保留。**

## 查看帮助

`select * from help`或
`select help`

查看管理端口用法。

| Command                                  | Description                              |
| :--------------------------------------- | :--------------------------------------- |
| select conn\_details from backends                                                  | display the idle conns                                     |
| select * from backends                                                             | list the backends and their state                          |
| show connectionlist [\<num\>]                                                        | show \<num\> connections                                     |
| select * from groups                                                               | list the backends and their groups                         |
| show allow\_ip/deny\_ip                                                              | show allow\_ip rules of module, currently admin|proxy|shard |
| add allow\_ip/deny\_ip '\<user\>@\<address\>'                                            | add address to white list of module                        |
| delete allow\_ip/deny\_ip '\<user\>@\<address\>'                                         | delete address from white list of module                   |
| set reduce\_conns (true\|false)                                                      | reduce idle connections if set to true                     |
| set maintain (true\|false)                                                          | Accelerate to close the connection                |
| set charset\_check (true\|false)                                                     | check the client charset is equal to the default charset   |
| refresh\_conns                                                                      | refresh all server connections                             |
| show maintain status                                                               | show maintain status                                       |
| show variables [like '%pattern%']                                                  |                                                            |
| select version                                                                     | cetus version                                              |
| select * from user\_pwd [where user='<name>']                                       |                                                            |
| select * from app\_user\_pwd [where user='<name>']                                   |                                                            |
| update user\_pwd set password='xx' where user='<name>'                              |                                                            |
| update app\_user\_pwd set password='xx' where user='<name>'                          |                                                            |
| delete from user\_pwd where user='<name>'                                           |                                                            |
| delete from app\_user\_pwd where user='<name>'                                       |                                                            |
| insert into backends values ('\<ip:port@group\>', '(ro\|rw)', '\<state\>')              | add mysql instance to backends list                        |
| update backends set (type\|state)=x where (backend\_ndx=\<index\>\|address=\<'ip:port'\>) | update mysql instance type or state                        |
| delete from backends where (backend\_ndx=\<index\>|address=\<'ip:port'\>)               |                                                            |
| remove backend where (backend\_ndx=<index>|address='<ip:port>')                     |                                                            |
| remove backend backend\_ndx                                                         |                                                            |
| add master \<'ip:port@group'\>                                                       |                                                            |
| add slave \<'ip:port@group'\>                                                        |                                                            |
| stats get [\<item\>]                                                                 | show query statistics                                      |
| config get [\<item\>]                                                                | show config                                                |
| config set \<key\>=\<value\>                                                           |                                                            |
| stats reset                                                                        | reset query statistics                                     |
| select \* from help                                                                 | show this help                                             |
| select help                                                                        | show this help                                             |
| cetus                                                                              | Show overall status of Cetus                               |
| create vdb \<id\> (groupA:xx, groupB:xx) using \<method\>                              | Method example: hash(int,4) range(str)                     |
| create sharded table \<schema\>.\<table\> vdb \<id\> shardkey \<key\>                      | Create sharded table                                       |
| select \* from vdb                                                                  | Show all vdb                                               |
| select sharded table                                                               | Show all sharded table                                     |
| create single table \<schema\>.\<table\> on \<group\>                                    | Create single-node table                                   |
| select single table                                                                | Show single tables                                         |
| sql log status                                                                     | show sql log status                                        |
| sql log start                                                                      | start sql log thread                                       |
| sql log stop                                                                       | stop sql log thread                                        |
| kill query \<tid\>                                                                   | kill session when the thread id is equal to tid            |

结果说明：

sharding版本管理端口提供了多种语句对cetus进行管理，具体用法见以下说明。

## 后端配置

### 查看后端

`select * from backends`

查看后端信息。

| backend_ndx | address        | state | type | slave delay | uuid | idle_conns | used_conns | total_conns | group  |
| :---------- | :------------- | :---- | :--- | :---------- | :--- | :--------- | :--------- | :---------- | :----- |
| 1           | 127.0.0.1:3306 | up    | rw   | NULL        | NULL | 100        | 0          | 100         | group1 |
| 2           | 127.0.0.1:3307 | up    | rw   | NULL        | NULL | 100        | 0          | 100         | group2 |
| 3           | 127.0.0.1:3308 | up    | rw   | NULL        | NULL | 100        | 0          | 100         | group3 |
| 4           | 127.0.0.1:3309 | up    | rw   | NULL        | NULL | 100        | 0          | 100         | group4 |

结果说明：

* backend_ndx: 后端序号，按照添加顺序排列；
* address: 后端地址，IP:PORT格式；
* state: 后端状态(unknown|up|down|maintaining|deleted)；
* type: 读写类型(rw|ro)；
* slave delay: 主从延迟时间(单位：毫秒)，只有cetus配有从库并且监测主从延迟才会有slave delay的值；
* uuid: 暂时无用；
* idle_conns: 空闲连接数；
* used_conns: 正在使用的连接数；
* total_conns: 总连接数；
* group: 后端分组。

```
状态说明
unknown:     后端初始状态，还未建立连接；
up:          能与后端正常建立连接；
down:        与后端无法联通(如果开启后端状态检测，能连通后自动变为UP)；
maintaining: 后端正在维护，无法建立连接或自动切换状态(此状态由管理员手动设置)；
deleted:      后端已被删除，无法再建立连接。
```

### 查看后端连接状态

`select conn_details from backends`

查看每个用户占用和空闲的后端连接数。

| backend_ndx | username | idle_conns | used_used_conns | total_used_conns |
| :---------- | :------- | :--------- | :-------------- | ---------------- |
| 1           | test1    | 2          | 0               | 0                |
| 2           | test2    | 11         | 0               | 0                |

结果说明：

* backend_ndx: 后端序号；
* username: 用户名；
* idle_conns: 空闲连接数；
* used_used_conns：正在使用的连接数。
* total_used_conns: 总的连接数。

### 查看后端分组情况

`select * from groups`

查看后端分组的详细信息。

| group | master         | slaves         |
| :---- | :------------- | :------------- |
| data1 | 127.0.0.1:3306 | 127.0.0.1:3316 |
| data2 | 127.0.0.1:3307 | 127.0.0.1:3317 |
| data3 | 127.0.0.1:3308 | 127.0.0.1:3318 |
| data4 | 127.0.0.1:3309 | 127.0.0.1:3319 |

结果说明：

* group: 后端分组序号；
* master: 读写后端；
* slaves: 只读后端。

### 添加后端

`add master '<ip:port@group>'`

添加一个读写类型的后端。

例如

>add master '127.0.0.1:3307@group1'

`add slave '<ip:port@group>'`

添加一个只读类型的后端。

例如

>add slave '127.0.0.1:3306@group1'

`insert into backends values ('<ip:port@group>', '(ro|rw)', '<state>')`

添加一个后端，同时指定读写类型。

例如

>insert into backends values ('127.0.0.1:3306@group1', 'rw', 'up');

### 删除后端

`remove backend <backend_ndx>` 或
`delete from backends where backend_ndx = <backend_ndx>`

删除一个指定序号的后端。

例如

>remove backend 1

`delete from backends where address = '<ip:port>'`

删除一个指定地址的后端。

例如

>delete from backends where address = '127.0.0.1:3306'

### 修改后端

`update backends se (type|state)='<value>' where (backend_ndx=<index>|address='<ip:port>')`

修改后端类型或状态。

例如

>update backends set type='rw' where address='127.0.0.1:3306'

>update backends set state='up' where backend_ndx=1

```
说明
update后端的state只包括up|down|maintaining三种状态，delete/remove后端可将后端的state设为deleted状态。
```

## 基本配置

### 查看连接池/通用配置

`config get [<item>]`

`config get`查看支持的配置类型
   * `pool`连接池配置
   * `common`通用配置

`config get common`查看通用配置
   * `common.check_slave_delay` 是否需要检测从库延迟
   * `common.slave_delay_down_threshold_sec` 若延迟大于此值(秒)，后端状态置为DOWN
   * `common.slave_delay_recover_threshold_sec` 若延迟小于此值(秒)，后端状态置为UP

`config get pool`查看连接池配置
   * `pool.default_pool_size` 默认连接池大小
   * `pool.max_pool_size` 最大连接数量
   * `pool.max_resp_len` 最大结果集长度
   * `pool.master_preferred` 是否只允许走主库

### 修改配置

`config set <key>=<value>`

例如

>config set slave-delay-down = 3

### 查看参数配置

`show variables [like '%<pattern>%']`

查看的参数均为启动配置选项中的参数，详见[Cetus 启动配置选项说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-configuration.md)。

## 查看/设置连接信息

### 查看当前连接的详细信息

`show connectionlist`

将当前全部连接的详细内容按表格显示出来。

| User  | Host           | db   | Command | Time | Trans | PS   | State      | Xa   | Xid  | Server | Info |
| ----- | -------------- | ---- | ------- | ---- | ----- | ---- | ---------- | ---- | ---- | ------ | ---- |
| test1 | 127.0.0.1:3306 | test | Sleep   | 0    | N     | N    | READ_QUERY | NX   | NULL | NULL   | NULL |
| test2 | 127.0.0.1:3307 | test | Sleep   | 0    | N     | N    | READ_QUERY | NX   | NULL | NULL   | NULL |

结果说明：

* User: 用户名;
* Host: 客户端的IP和端口;
* db: 数据库名称;
* Command: 执行的sql，"Sleep"代表当前空闲;
* Time: 已执行的时间;
* Trans: 是否在事务中（Y｜N）;
* PS：是否存在prepare（Y｜N）;
* State: 连接当前的状态，"READ_QUERY"代表在等待获取命令;
* Xa：分布式事务状态（NX|XS|XQ|XE|XP|XC|XR|XCO|XO）;
* Xid：分布式事务的xid;
* Server: 后端地址;
* Info: 暂未知。

```
Xa状态说明
NX:     未处于分布式事务状态中；
XS:     处于XA START状态；
XQ:     处于XA QUERY状态；
XE:     处于XA END状态；
XP:     处于XA PREPARE状态；
XC:     处于XA COMMIT状态；
XR:     处于XA ROLLBACK状态；
XCO:    处于XA CANDIDATE OVER状态；
XO:     处于XA OVER状态。
```

### 查看某用户对某后端的连接数

`select conn_num from backends where backend_ndx=<index> and user='<name>')`

例如

>select conn_num from backends where backend_ndx=2 and user='root');

### 设置是否减少空闲连接

`set reduce_conns (true|false)`

例如

>set reduce_conns true;

减少空闲连接。

### 设置是否加速关闭所有客户端连接

`set maintain (true|false)`

例如

>set maintain true;

加速关闭客户端与Cetus的连接，该参数通常与LVS配合使用。

### 查询是否加速关闭所有客户端连接

`show maintain status`

查询是否加速关闭所有客户端连接。

## 用户/密码管理

### 密码查询

`select * from user_pwd [where user='<name>']`

查询某个用户的后端密码。

**注意由于密码是非明文的，仅能显示字节码。**

>select * from user_pwd where user='root';

`select * from app_user_pwd [where user='<name>']`

查询某个用户连接proxy的密码，同样是非明文。

例如

>select * from app_user_pwd where user='test';

### 密码添加/修改

`update user_pwd set password='<password>' where user='<name>'`

添加或修改特定用户的后端密码(如果该用户不存在则添加，已存在则覆盖)。

例如

>update user_pwd set password='123456' where user='test'

`update app_user_pwd set password='<password>' where user='<name>'`

添加或修改特定用户连接Proxy的密码(如果该用户不存在则添加，已存在则覆盖)。

例如

>update app_user_pwd set password='123456' where user='root'

### 密码删除

`delete from user_pwd where user='<name>'`

删除特定用户的后端密码。

例如

>delete from user_pwd where user='root'

`delete from app_user_pwd where user='<name>'`

删除特定用户连接Proxy的密码。

例如

>delete from app_user_pwd where user='root'

## Shard端口IP白名单

### 查看Shard端口IP白名单

`show allow_ip`

查看shard模块的IP白名单。

若列表为空或为\*，则代表没有任何限制。

### 增加Shard端口IP白名单

`add allow_ip <address>`

向Shard端口白名单增加一个IP许可。(IP需要加引号)

\<address\>：[[user@]IP]

```
说明
其中配置的IP为特定IP（如192.0.0.1），也支持IP段（如192.0.0.*）。
```

例如

>add allow_ip "127.0.0.1"

>add allow_ip "test@127.0.0.1"

### 删除Shard端口的IP白名单

`delete allow_ip <address>`

删除白名单中的一个IP许可。(IP需要加引号)

\<address\>：[[user@]IP]

例如

>delete allow_ip "127.0.0.1"

>delete allow_ip "test@127.0.0.1"

## Shard端口IP黑名单

### 查看Shard端口IP黑名单

`show deny_ip`

查看shard模块的IP黑名单。

若列表为空，则代表没有任何限制。

### 增加Shard端口IP黑名单

`add deny_ip <address>`

向Shard端口的黑名单增加一个IP限制。(IP需要加引号)

\<address\>：[[user@]IP]

```
说明
其中配置的IP为特定IP（如192.0.0.1），也支持IP段（如192.0.0.*）。
```

例如

>add deny_ip "127.0.0.1"

>add deny_ip "test@127.0.0.1"

### 删除Shard端口IP黑名单

`delete deny_ip <address>`

删除Shard端口黑名单中的一个IP限制。(IP需要加引号)

\<address\>：[[user@]IP]

例如

>delete deny_ip "127.0.0.1"

>delete deny_ip "test@127.0.0.1"

**注意：IP白名单的优先级高于IP黑名单**

## 远程管理

### 重载分库配置

`reload shard`

需要"remote-conf-url = \<url>"和"disable-threads = false"启动选项。
从远端配置库中重载Shard配置。

### 保存最新配置

`save settings`

保存当前最新配置到cetus的安装主路径中（如/home/user/cetus_install/）。

```
说明
保存的当前最新配置为shard.conf，旧的配置依然存在，更名为shard.conf.old。
```

## 查看整体信息

### 查看统计信息

`stats get [<item>]`

`stats get`查看支持的统计类型
   * `client_query` 客户发来的SQL数量
   * `proxyed_query` 发往后端的SQL数量
   * `query_time_table` 查询时间直方图
   * `server_query_details` 每个后端接收的SQL数量
   * `query_wait_table` 等待时间直方图

`stats get client_query` `stats get proxyed_query`查看读/写SQL数量

`stats get server_query_details`查看各个后端读/写SQL数量

`stats get query_time_table` `stats get query_wait_table` 查看各时间值对应的SQL数量，如：

| name               | value |
| :----------------- | :---- |
| query_time_table.1 | 3     |
| query_time_table.2 | 5     |
| query_time_table.5 | 1     |

表示用时1毫秒的SQL有3条，用时2毫秒的SQL有5条，用时5毫秒的SQL有1条

```
说明
stats reset：重置统计信息 
```

### 查看总体状态

`cetus`

包括程序版本、连接数量、QPS、TPS等信息

### 查看当前cetus版本

`select version`

## 其他

### 减少系统占用的内存

`reduce memory`
