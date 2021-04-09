# 启动配置选项

## 常规配置
### worker-processes

Default: 1

启动worker进程的数量，启动的数量最好小于等于cpu数目

> worker-processes = 4

### daemon

Default: false

通过守护进程启动

> daemon = true

### user

Default: root

启动进程的用户，只有以root身份运行时才能使用

> user = cetus

### basedir

基础路径，其它配置可以以此为基准配置相对路径。(必须是绝对路径)

> basedir = /usr/lib/cetus

### conf-dir

Default: conf

JSON配置文件路径，JSON文件包括包括：账号配置文件、变量处理配置文件、分库版本的分片规则配置文件

> conf-dir = /usr/lib/cetus/conf

### pid-file

`必要`

PID文件路径

> pid-file = /var/log/cetus.pid

### log-file

`必要`

日志文件路径

> log-file = /var/log/cetus.log

### log-level

可选值: debug | info | message | warning | error | critical(default)

日志级别

> log-level = info

### log-use-syslog

系统日志文件路径，与log-file不可同时设置。

> log-use-syslog = /var/log/cetus_sys.log

### log-xa-file

xa日志路径（分库中有效）

> log-xa-file = logs/cetus.log

### log-xa-in-detail

Default: false

记录xa日志详情（分库中有效）

> log-xa-in-detail = true

### plugins

`可多项`

加载模块名称

> plugins = admin,proxy

### plugin-dir

库文件路径

> plugin-dir = /usr/lib/cetus/plugins

## Proxy配置

### proxy-address

Default: :4040

Proxy监听的IP和端口

> proxy-address = 127.0.0.1:4440

### proxy-allow-ip

`可在Admin模块中动态更改`

Proxy允许访问的"用户@IP"

参数未设置时，没有限制；"User@IP"限制特定的用户和IP组合访问；"IP"允许该IP的所有用户访问

> proxy-allow-ip = root@127.0.0.1,10.238.7.6

### proxy-backend-addresses

Default: 127.0.0.1:3306

`可多项`

读写后端(主库)的IP和端口

> proxy-backend-addresses = 10.120.12.12:3306

分表（分库模式下partition-mode=true）

> proxy-backend-addresses = 10.120.12.12:3306

若是分库模式，需要同时指定group

> proxy-backend-addresses = 10.120.12.12:3306@data1

### proxy-read-only-backend-addresses

`可多项`

只读后端(从库)的IP和端口

> proxy-read-only-backend-addresses = 10.120.12.13:3307

分表（分库模式下partition-mode=true）

> proxy-read-only-backend-addresses = 10.120.12.13:3307

若是分库模式，需要同时指定group

> proxy-read-only-backend-addresses = 10.120.12.13:3307@data1

### proxy-connect-timeout

Default: : 2 (seconds)

连接Proxy的超时时间

> proxy-connect-timeout = 2

### proxy-read-timeout

Default: : 600 (seconds)

读Proxy的超时时间

> proxy-read-timeout = 600

### proxy-write-timeout

Default: : 600 (seconds)

写Proxy的超时时间

> proxy-write-timeout = 600

### default-username

默认用户名，在Proxy启动时自动创建连接使用的用户名

> default-username = default_user

### default-db

默认数据库，当连接未指定db时，使用的默认数据库名称

> default-db = test

### default-pool-size

Default: 100

每个worker进程启动时允许创建的连接数
当前连接数不足此值时，会自动创建连接
最小只能设置为10，如果设置小于10，则实际该值为10

> default-pool-size = 200

### max-pool-size

Default: default-pool-size * 2

每个worker进程允许创建的最大连接数，包括连接池里的空闲连接和正在使用的连接

> max-pool-size = 300

### max-alive-time

Default: 7200 (seconds)

后端连接最大存活时间

> max-alive-time = 7200

### master-preferred

设置为true时仅访问读写后端(主库)，除非利用注释强制走从库

> master-preferred = true

### read-master-percentage

读取主库的百分比

> read-master-percentage = 50

### reduce-connections

自动减少空闲连接

> reduce-connections = true

### default-charset

默认数据库字符标码方式

> default-charset = utf8

### enable-client-found-rows

Default: false

允许客户端使用CLIENT_FOUND_ROWS标志

> enable-client-found-rows = true

### worker-id

只针对分库版本有效
不同cetus实例的id号必须是不一样，否则容易有冲突

> worker-id = 1

## Admin配置

### admin-address

Default: :4041

管理模块的IP和端口

> admin-address = 127.0.0.1:4441

### admin-allow-ip

`可在Admin模块中动态更改`

参数未设置时，不作限制；仅能限制IP不区分用户

> admin-allow-ip = 127.0.0.1,10.238.7.6

### admin-username

`必要`

管理模块的用户名

> admin-username = admin

### admin-password

`必要`

管理模块的密码明文

> admin-password = admin_pass

## 远端配置中心

在同一Cetus集群，当Cetus实例数量较多时候，各个实例的本地配置文件的统一管理会变得复杂。Cetus除了提供通过本地配置文件的方式启动外，还提供了通过远程配置中心的方式启动。

远程配置中心中可以配置与本地配置文件中相同的启动参数，各个Cetus实例启动时，指定远程配置中心的url获取配置信息启动。这样就保证了同一个Cetus集群中各个Cetus实例启动配置的一致性。

目前Cetus的两个版本（读写分离版本和分库版本）均支持远程配置中心启动。

远程配置中心涉及3张表`settings`、`objects`和`services`，当这些表不存在时，Cetus会自动创建，但是在启动的时候，需要配置合理的参数，否则Cetus可能启动不起来，需要查看Cetus日志来查看具体报错问题。下面依次介绍这些表的作用。

- `settings`表

该表主要存储启动时加载的配置信息，即对应本地配置文件`proxy.conf`。其表结构如下：

> CREATE TABLE `settings` (
> 
>  `option_key` varchar(64) NOT NULL,
> 
>  `option_value` varchar(1024) NOT NULL,
> 
>  PRIMARY KEY (`option_key`)
> 
>)

在该表中，可以配置各种启动配置参数，例如：

```
replace into `settings` values ("admin-address", "0.0.0.0:6003");
replace into `settings` values ("admin-password", "admin");
replace into `settings` values ("admin-username", "admin");
replace into `settings` values ("basedir", "/home/ght/cetus_install");
replace into `settings` values ("conf-dir", "/home/ght/cetus_install/conf");
replace into `settings` values ("default-db", "test");
replace into `settings` values ("default-username", "ght");
replace into `settings` values ("log-file", "/home/ght/cetus_install/cetus.log");
replace into `settings` values ("plugin-dir", "/home/ght/cetus_install/lib/cetus/plugins");
replace into `settings` values ("plugins", "proxy,admin");
replace into `settings` values ("proxy-backend-addresses", "192.0.0.1:3306@data1,192.0.0.2:3306@data2,192.0.0.3:3306@data3,192.0.0.4:3306@data4");
```

- `objects`表

该表主要存储账号信息和分片信息，即对应本地配置文件的`users.json`文件和`sharding.json`文件。其表结构如下：

>CREATE TABLE `objects` (
>
>  `object_name` varchar(64) NOT NULL,
> 
>  `object_value` text NOT NULL,
> 
>  `mtime` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
> 
>  PRIMARY KEY (`object_name`)
> 
> )

在该表中，可以配置账户信息和分表规则信息，其中`object_values`存储的其实是json格式，例如：

```
replace into `objects` values ("sharding", '{"vdb": [{"id": 1,"type": "int","method": "hash","num": 8,"partitions": {"data1": [0,1], "data2": [2,3], "data3": [4,5], "data4": [6,7]}},{ "id": 2,"type": "int","method": "range","num": 0,"partitions": {"data1": 124999, "data2": 249999, "data3": 374999,"data4": 499999}}],"table": [{"vdb": 1, "db": "employees_hash", "table": "dept_emp", "pkey": "emp_no"},{"vdb": 1, "db": "employees_hash", "table": "employees", "pkey": "emp_no"},{"vdb": 1, "db": "employees_hash", "table": "titles", "pkey": "emp_no"},{"vdb": 2, "db":"employees_range", "table": "dept_emp", "pkey": "emp_no"},{"vdb": 2, "db": "employees_range", "table": "employees", "pkey": "emp_no"},{"vdb": 2, "db":"employees_range", "table": "titles", "pkey": "emp_no"}],"single_tables": [{"table": "regioncode", "db": "employees_hash", "group": "data1"},{"table": "countries",  "db": "employees_range", "group": "data2"}]}', now());

replace into `objects` values ("users", '{"users":[{"user": "ght","client_pwd":"Zxcvbnm,lp-1234","server_pwd":"Zxcvbnm,lp-1234"}, {"user": "tmp","client_pwd":"Zxcvbnm,lp-1234","server_pwd":"Zxcvbnm,lp-12345"}, {"user": "test2","client_pwd":"123456","server_pwd":   "Zxcvbnm,lp-1234"}, {"user": "dbtest","client_pwd":"Zxcvbnm,lp-1234","server_pwd":"Zxcvbnm,lp-1234"}]}', now());
```

- `services`表

该表主要用于存储Cetus启动时间。该表是Cetus启动的时候由Cetus自己写入的，所以不需要用户配置。其表结构如下：

>CREATE TABLE `services` (
>
>  `id` varchar(64) NOT NULL,
> 
>  `data` varchar(64) NOT NULL,
> 
>  `start_time` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
> 
>  PRIMARY KEY (`id`)
> 
> )

该表中的`id`字段记录了Cetus插件监听的IP/PORT，`data`字段记录了插件名称，`start_time`字段则记录了插件的启动时间。例如启动Cetus后，表中数据可能类似下面所示：

```
mysql> select * from services;
+--------------+-------+---------------------+
| id           | data  | start_time          |
+--------------+-------+---------------------+
| 0.0.0.0:6003 | admin | 2018-07-11 11:59:05 |
| :4040        | proxy | 2018-07-11 11:59:06 |
+--------------+-------+---------------------+
2 rows in set (0.00 sec)
```


在Cetus启动的时候，则不需要再指定`--defaults-file`，而是指定远端配置中心的url即可`remote-conf-url`。


### remote-conf-url

- 远端配置中心信息书写格式如下：

> remote-conf-url = mysql://dbuser:dbpassword@host:port/schema

或者

> remote-conf-url = sqlite://dbuser:dbpassword@host:port/schema

配置中心端口port可选填，默认3306

- 启动命令类似如下：

> /home/ght/cetus_install/bin/cetus --remote-conf-url=mysql://ght:123456@172.17.0.1:3306/test
 
### 重新load配置
当配置中心的某些配置需要修改，而需要Cetus重新加载修改后的配置时候，并不需要重新启动Cetus，Cetus的admin端口提供了重新读取配置中心配置信息的功能。

当修改表`settings`中的配置信息时，可以通过在Cetus的admin端口执行`config reload`命令，使Cetus重新从配置中心拉取配置信息，使得配置中心新修改的配置在Cetus上生效。

当修改表`objects`中的账号信息时，可以通过在Cetus的admin端口执行`config reload user`、`config reload variables`命令，使Cetus重新从配置中心拉取账号信息、静默处理的变量信息，使其在Cetus上生效。

**注意** ：目前cetus执行reload操作与远程配置库进行交互时，连接、读、写超时均为1秒，即如果由于远程配置库负载过大、网络抖动等原因导致超时超过1秒，会reload操作失败。与此同时，reload操作目前和SQL处理的线程为同一个线程，所以尽量少用该命令，或是业务低峰期使用该命令，后续会将其修改成异步形式，彻底不影响SQL的处理。


## 辅助线程配置

### disable-threads

Default: false

禁用辅助线程，包括: 后端存活检测、只读库延迟检测、MGR节点状态和角色检测等

> disable-threads = true

### check-slave-delay

Default: true

是否检查从库延迟。注意，cetus的延迟检测只单纯检测主从之间的延迟毫秒数（主库写入时间戳，从库读取时间戳，与本地时间做差值，计算主从延迟），而非检测io_thread/sql_thread是否正常工作。

> check-slave-delay = false

### slave-delay-down

Default: 10 (seconds)

从库延迟超过该秒，状态将被设置为DOWN

> slave-delay-down = 15

### slave-delay-recover

Default: 1  (seconds)

从库延迟少于该秒数，状态将恢复为UP

> slave-delay-recover = 5

**注：slave-delay-recover必须比slave-delay-down小，若用户配置的slave-delay-recover比slave-delay-down大则默认设置slave-delay-recover与slave-delay-down相等**

## MGR配置

### group-replication-mode

Default: 0 (普通MySQL集群)

当后端MySQL集群是单主模式的MGR时，该参数设置为1，Cetus可以自动检测MGR集群的主从状态及节点主从角色变换。目前Cetus只支持单主MGR模式。

> group-replication-mode = 1

## 其它

### verbose-shutdown

Default: false

程序退出时，记录下退出代码。

> verbose-shutdown = true

### keepalive

Default: false

当Proxy进程意外终止，会自动启动一个新进程

> keepalive = true

### max-open-files

Default: 根据操作系统

最大打开的文件数目(ulimit -n)

> max-open-files = 1024

### max-allowed-packet

Default: 33554432 (32MB)

最大允许报文大小

> max-allowed-packet = 1024

### disable-dns-cache

Default: false

禁用解析连接到后端的域名

> disable-dns-cache = true

### long-query-time

Default: 1000 (millisecond)

慢查询记录阈值(毫秒)，最大65536ms

> long-query-time = 500

### log-backtrace-on-crash

Default: false

程序崩溃时启动gdb调试器

> log-backtrace-on-crash = true

### enable-back-compress

Default: false

启用后端传给Cetus的结果集压缩，一般不启用

> enable-back-compress = true

### merged-output-size

Default: 8192

tcp流式结果集合并输出阈值，超过此大小，则输出

> merged-output-size = 2048

### default-query-cache-timeout

Default: 100

设置query cache的默认超时时间，单位为ms

> default-query-cache-timeout = 60

### enable-query-cache

Default: false

开启Proxy请求缓存

> enable-query-cache = true

### max-header-size

Default:  65536

设置响应中header最大大小，供tcp stream使用，如果响应头部特别大，需要设置更大的大小

> max-header-size = 131072

### enable-tcp-stream

Default: false

采用tcp stream来输出响应，规避内存炸裂等问题

> enable-tcp-stream = true

### enable-fast-stream

Default(release版本): false

采用fast stream来输出只读响应，提升响应速度，release版本默认为false，开发版本默认为true

> enable-fast-stream = true

### ssl

Default: false

前端支持SSL连接。需要在 `--conf-dir` 中提供：
- 私钥：`server-key.pem`
- 公钥证书：`server-cert.pem`
这两个文件可以使用[mysql工具生成](https://dev.mysql.com/doc/refman/8.0/en/creating-ssl-rsa-files-using-mysql.html)，
生成之后拷贝到`conf-dir`目录，程序会按照这两个固定名称加载文件。
