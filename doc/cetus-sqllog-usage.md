## 全量日志使用方法
### 1 前言
Cetus增加了全量日志的功能，即可以按需要输出经由Cetus的所有SQL。通过全量日志，可以方便的进行问题排查、记录和统计SQL、安全审计等。

开启全量日志，会有一些性能损耗，因此需要按需合理使用。

### 2 命令、参数与用法
默认情况下，全量日志功能是不开启的。该功能提供了3个命令、8个可配置参数和3个统计变量，下面依次介绍：

#### 2.1 命令
可以登录Cetus的admin端口，开启全量日志、关闭全量日志、或是查看全量日志模块的状态。

- 开启全量日志

> sql log start;

成功开启全量日志，需要提前将sql-log-switch参数设置为ON，然后执行上述命令，否则会报错如下：

> mysql> sql log start;
> 
> ERROR 1105 (07000): can not start sql log thread, because sql-log-switch = OFF

成功开启后，查看统计变量sql-log-state的状态变为：running，表示已经开启该功能。

- 关闭全量日志

> sql log stop;

上述命令可以随时关闭全量日志，减少大量日志输出所带来的性能损耗。

- 查看全量日志状态

> sql log status

上述命令可以将全量日志相关的变量进行汇总显示。

#### 2.2 参数

参数分为动态（Dynamic）变量和静态（Static）变量。动态变量可以动态修改，修改命令如下：

> config set key=vale;
> 
> 例如：
> 
> config set sql-log-switch = 'on';



- sql-log-buffersize

该参数可以指定全量日志的缓存大小，默认值为1M，该参数单位是B，不能动态配置。

- sql-log-switch

该参数控制全量日志功能是否可用，默认为OFF。该参数可配置的值有：OFF、ON和REALTIME。启动时若配置该参数为ON或是REALTIME，则启动后启动开启全量日志功能，该参数可以动态配置。ON和REALTIME的区别在于，REALTIME不仅将日志写入OS的缓存，同时调用fsync函数，将日志落盘。

- sql-log-prefix

该参数定义全量日志的文件名前缀，后缀默认均为.clg。该值默认为cetus，该参数不能动态配置。全量日志文件名组成为：前缀-进程号.后缀

- sql-log-path

该参数可以指定全量日志输出的路径，该值默认与basedir/logs/路径相同，如果路径不存在，尝试创建该目录，该参数不能动态配置。

- sql-log-maxsize

该值控制每个全量日志的最大容量，默认值为1024，0表示不限制文件大小，单位是M，该参数不能动态配置。如果当前日志量超过该值，则会rotate成历史日志文件。

- sql-log-mode

该值控制输出的全量日志的类型，默认为backend。该参数可配置的值包括：connect、client、front、backend、all，该参数支持动态配置。

- sql-log-idletime

该值控制全量日志的线程在没有日志可以写入文件的情况下，等待下写入的时间，默认500ms，单位毫秒，该参数支持动态设置。

- sql-log-maxnum

保留的历史文件的个数，默认为3，0表示不限制文件个数。

#### 2.3 统计信息

- sql-log-state

该统计变量表示当前全量日志线程是否启动，running表示启动，stopped表示未启动。

- sql-log-cached

该统计变量表示当前全量日志仍旧在内存中的大小，单位B。

- sql-log-cursize

该统计变量表示当前全量日志文件，写入的字节数，单位B。

### 3 日志格式
可以通过参数sql-log-mode来设置不同模式的日志，各种模式的日志格式均不同。基本的模式有三种：connect、client和backend。如果希望同时打印connect、client，则可以设置为：front，如果希望打印所有日志，则可以设置为：all。

connect模式打印的是客户端连接Cetus时，客户端发送的auth认证包的主要内容。

client模式打印的是客户端发送的原始SQL语句等相关内容，该模式下，当Cetus接收到客户端的SQL请求后，立即打印该日志。

backend模式打印的是发送到SQL的语句等相关内容，该模式下，当MySQL将全部结果集发送回Cetus后才会打印该日志。

- connect模式下日志格式

```
2018-08-03 08:04:09.117: #connect# ght@172.17.0.2:58426 Connect Cetus, C_id:2 C_db: C_charset:33 C_auth_plugin:mysql_native_password C_ssl:false C_cap:1ffa685 S_cap:80eff64f
```

> C\_id:客户端当前连接的ID，可以通过其来确定哪些是同一个连接
> 
> C\_db:客户端auth认证包中携带的database信息
>
> C\_charset:客户端auth认证包中携带的charset的信息
> 
> C\_auth\_plugin: 使用的认证插件
> 
> C\_ssl: 是否启用了ssl
> 
> C\_cap: 客户端发送的权值信息
> 
> S\_cap: Cetus发送的权值信息


- client模式下日志格式

```
2018-08-03 08:11:16.054: #client# C_ip:172.17.0.2:58426 C_db: C_usr:ght C_tx:false C_retry:0 C_id:2 type:Query select * from test1
```
> C\_ip: 客户端的IP、Port
> 
> C\_db: 客户端当前的database信息
> 
> C\_usr: 客户端的用户名
> 
> C\_tx: 是否在事务中
> 
> C\_retry: 客户端连接重试测试
> 
> C\_id: 客户端当前连接的ID，可以通过其来确定哪些是同一个连接
> 
> type: SQL类型


- backend模式下 读写分离版本日志格式

```
2018-08-03 08:15:58.597: #backend-rw# C_ip:172.17.0.2:58426 C_db:ght C_usr:ght C_tx:true C_id:2 S_ip:172.17.0.3:3306 S_db:ght S_usr:ght S_id:24515 inj(type:3 bytes:0 rows:0) latency:0.759(ms) ERR type:Query select * from test1
```

> C\_ip: 客户端的IP、Port
> 
> C\_db: 客户端当前的database信息
> 
> C\_usr: 客户端的用户名
> 
> C\_tx: 是否在事务中
> 
> C\_id: 客户端当前连接的ID，可以通过其来确定哪些是同一个连接
> 
> S\_ip: MySQL端的IP、Port
> 
> S\_db: MySQL端当前的database信息
> 
> S\_usr: MySQL端的用户名
> 
> S\_id: MySQL当前连接的ID，可以通过其来确定哪些是同一个MySQL连接
> 
> inj-type: 语句的类型
> 
> inj-bytes: 结果集字节数
> 
> inj-rows: 影响行数
> 
> latency: 延迟
> 
> type: SQL类型

- backend模式下 分片版本日志格式

```
2018-08-09 10:34:51.619: #backend-sharding# C_ip:172.17.0.2:51428 C_db:ght C_usr:ght C_tx:false C_id:4 S_ip:172.17.0.3:3306 S_db:ght S_usr:ght S_id:27630 trans(in_xa:false xa_state:UNKNOWN) latency:1.504(ms) OK type:Query select * from s
```

> C\_ip: 客户端的IP、Port
> 
> C\_db: 客户端当前的database信息
> 
> C\_usr: 客户端的用户名
> 
> C\_tx: 是否在事务中
> 
> C\_id: 客户端当前连接的ID，可以通过其来确定哪些是同一个连接
> 
> S\_ip: MySQL端的IP、Port
> 
> S\_db: MySQL端当前的database信息
> 
> S\_usr: MySQL端的用户名
> 
> S\_id: MySQL当前连接的ID，可以通过其来确定哪些是同一个MySQL连接
> 
> in\_xa: 是否在XA事务中
> 
> xa\_state: 所处的XA事务的阶段
> 
> latency: 延迟
> 
> type: SQL类型

xa\_state的类型包括如下内容：
> UNKNOWN
>
> NEXT_ST_XA_START
>
> NEXT_ST_XA_QUERY
>
> NEXT_ST_XA_END
>
> NEXT_ST_XA_PREPARE
>
> NEXT_ST_XA_COMMIT
>
> NEXT_ST_XA_ROLLBACK
>
> NEXT_ST_XA_CANDIDATE_OVER
>
> NEXT_ST_XA_OVER

分片版本下，当调整连接状态时候，会打印如下日志：

```
2018-08-09 11:34:15.845: #backend-sharding# C_ip:172.17.0.2:51428 C_db:ght C_usr:ght C_tx:false C_id:4 trans(in_xa:false xa_state:UNKNOWN) attr_adj_state:8
```

> C\_ip: 客户端的IP、Port
> 
> C\_db: 客户端当前的database信息
> 
> C\_usr: 客户端的用户名
> 
> C\_tx: 是否在事务中
> 
> C\_id: 客户端当前连接的ID，可以通过其来确定哪些是同一个连接
>
> in\_xa: 是否在XA事务中
> 
> xa\_state: 所处的XA事务的阶段
> 
> attr\_adj\_state: 调整的属性信息

attr\_adj\_state属性信息包括如下内容：

> ATTR_START = 0
> 
> ATTR_DIF_CHANGE_USER = 1
> 
> ATTR_DIF_DEFAULT_DB = 2
> 
> ATTR_DIF_SQL_MODE = 4
> 
> ATTR_DIF_CHARSET = 8
> 
> ATTR_DIF_SET_OPTION = 16
> 
> ATTR_DIF_SET_AUTOCOMMIT = 32

### 4 用法

- 启动时开启关闭全量日志功能

启动前，配置sql-log-switch=ON或是sql-log-switch=REALTIME，Cetus启动后会自动开启全量日志功能。

- 动态开启关闭全量日志功能

Cetus运行后可以在设置sql-log-switch=ON或是sql-log-switch=REALTIME的情况下，通过执行sql log start；动态的开启全量日志功能；当然也可以通过执行sql log stop;动态关闭该功能。如果暂时不希望使用该功能，尽量配置sql-log-switch=OFF。

- 设置日志自动rotate

参数sql-log-maxsize设置单个文件的最大容量，sql-log-maxnum设置保留的历史日志的个数。将这两个参数合理配置，便可以实现全量日志的自动rotate功能。
