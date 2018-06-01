# 启动配置选项

## 常规配置

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

配置文件路径，包括：用户设置文件、变量处理配置文件、分库版本的分片规则配置文件、读写分离版本的启动配置文件和分库版本的启动配置文件。

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

若是分库模式，需要同时指定group

> proxy-backend-addresses = 10.120.12.12:3306@data1

### proxy-read-only-backend-addresses

`可多项`

只读后端(从库)的IP和端口

> proxy-read-only-backend-addresses = 10.120.12.13:3307

若是分库模式，需要同时指定group

> proxy-read-only-backend-addresses = 10.120.12.13:3307@data1

### proxy-connect-timeout

Default: : 2 (seconds)

连接Proxy的超时时间

> proxy-connect-timeout = 1

### proxy-read-timeout

Default: : 10 (minutes)

读Proxy的超时时间

> proxy-read-timeout = 1

### proxy-write-timeout

Default: : 10 (minutes)

写Proxy的超时时间

> proxy-write-timeout = 1

### default-username

默认用户名，在Proxy启动时自动创建连接使用的用户名

> default-username = default_user

### default-db

默认数据库，当连接未指定db时，使用的默认数据库名称

> default-db = test

### default-pool-size

Default: 100

当前连接数不足此值时，会自动创建连接

> default-pool-size = 200

### max-pool-size

Default: default-pool-size * 2

连接池的最大连接数，超过此数目的连接不会放入连接池

> max-pool-size = 300

### max-alive-time

Default: 7200 (seconds)

后端连接最大存活时间

> max-alive-time = 7200

### max-resp-size

Default: 10485760 (10MB)

每个后端返回结果集的最大数量

> max-resp-size = 1048576

### master-preferred

`可在Admin模块中动态更改`

Proxy在读写分离时可以指定访问的库

参数未设置时，没有限制；设置为true时仅访问读写后端(主库)，除非利用注释强制走从库

> master-preferred = true

### read-master-percentage

读取主库的百分比

> read-master-percentage = 50

### reduce-connections

自动减少空闲连接

> reduce-connections = true

### default-charset

默认数据库字符标码方式

> default-charset = gbk

### enable-client-found-rows

Default: false

允许客户端使用FOUND_ROWS标志

> enable-client-found-rows = true

### worker_id

自增guid的worker id，最大值为63最小值为1

> worker_id = 4

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

可选择配置远端db，通过配置中心获取分库模式的配置

### remote-conf-url

远端配置中心信息

> remote-conf-url = mysql://dbuser:dbpassword@host:port/schema

或者

> remote-conf-url = sqlite://dbuser:dbpassword@host:port/schema

配置中心端口port可选填，默认3306

## 辅助线程配置

### disable-threads

Default: false

禁用辅助线程，包括: 配置变更检测、后端存活检测和只读库延迟检测等

> disable-threads = true

### check-slave-delay

Default: false

是否检查从库延迟

> check-slave-delay = true

### slave-delay-down

Default: 60 (seconds)

从库延迟超过该秒，状态将被设置为DOWN

> slave-delay-down = 10

### slave-delay-recover

Default: slave-delay-down / 2  (seconds)

从库延迟少于该秒数，状态将恢复为UP

> slave-delay-recover = 5

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

Default: 65536 (millisecond)

慢查询记录阈值(毫秒)

> long-query-time = 500

### log-backtrace-on-crash

Default: false

程序崩溃时启动gdb调试器

> log-backtrace-on-crash = true

### enable-back-compress

Default: false

启用后端传给Cetus的结果集压缩，一般不启用

> enable-back-compress ＝ true

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
