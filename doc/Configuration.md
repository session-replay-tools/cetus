# 配置文档

## 常规配置

### daemon

Default: false

通过守护进程启动。

> daemon = true

### user

Default: root

启动进程的用户

> user = cetus

### basedir

基础路径，其它配置可以以此为基准配置相对路径。(必须是绝对路径)

> basedir = /usr/lib/cetus
 
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

### plugins

`可多项`

加载模块名称。

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

### default-username

默认用户名，在Proxy启动时自动创建连接使用的用户名，在*user-pwd*中需要有对应的配置

> default-username = default_user

### default-db

默认数据库，当连接未指定db时，使用的默认数据库名称

> default-db = test

### default-pool-size

Default: 100

当前连接数不足此值时，会自动创建连接

> default-pool-size = 200

### max-pool-size

Default: *default-pool-size * 2*

连接池的最大连接数，超过此数目的连接不会放入连接池

> default-pool-size = 300

### max-resp-size

Default: 10485760 (10MB)

每个后端返回结果集的最大数量

> max-resp-size = 1024

### user-pwd

`可在Admin模块动态更改`

Proxy连接后端时使用的用户名、密码。用户名应与*app-user-pwd*中一一对应

> user-pwd = user1@password1,user2@password2

### app-user-pwd

`可在Admin模块动态更改`

客户端连接Proxy时使用的用户名、密码。用户名应与*user-pwd*中一一对应

> app-user-pwd = user1@apppass1,user2@apppass2

### crypt-pwd

Default: false

使用加密格式保存后端密码(user-pwd)

> crypt-pwd = true

### crypt-app-pwd

Default: false

使用加密格式保存客户端访问Proxy的密码(app-user-pwd)

> crypt-app-pwd = true

### disable-sharding-mode

Default: false

不启用分库功能(默认启用)，当只需要读写分离功能时设置为*true*

> disable-sharding-mode = true

### disable-auto-connect

Default: false

禁用自动创建连接,连接将在新请求到来时创建

> disable-auto-connect = false

## Admin配置

### admin-address

Default: :4041

管理模块的IP和端口

> admin-address = 127.0.0.1:4441

### admin-allow-ip

`可在Admin模块中动态更改`

参数未设置时，不作限制；仅能限制IP不区分用户

> admin-allow-ip = 127.0.0.1,10.238.7.6

### admin-lua-script

`必要`

管理模块对应的lua脚本路径

> admin-lua-script = /usr/lib/cetus/lua/admin.lua

### admin-username

`必要`

管理模块的用户名

> admin-username = admin

### admin-password

`必要`

管理模块的密码明文

> admin-password = admin_pass

## 远端配置中心

目前分库配置仅能通过配置中心获取，因此要采用分库模式，必须配置远端db

### config-remote

Default: false

是否启用远端配置中心

> config-remote = true

### config-host

Default: 127.0.0.1

配置中心host

> config-host = 127.0.0.1

### config-port

Default: 3306

配置中心端口

> config-port = 3310

### config-user

Default: root

配置中心用户

> config-user = test_user

### config-pass

配置中心密码

> config-pass = password

### config-db

Default: proxy_management

配置中心db名称

> config-db = proxy_management

### config-apply-interval

Default: 0 (seconds)

与配置中心同步配置的时间间隔。
若设置为*0*，则配置更新不自动生效，仅能手动生效，且检测间隔为3秒。

> config-apply-interval = 0

### proxy-id

Proxy的标识，在配置中心用来区分不同proxy的配置

> proxy-id = 001

## 辅助线程配置

### disable-threads

Default: false

禁用辅助线程，包括: 配置变更检测、后端存活检测和只读库延迟检测等

> disable-threads = true

### connect-timeout

Default: 2(seconds)

检测线程连接后端mysql时的超时时间(秒)

> connect-timeout = 10

### check-slave-delay

Default: false

是否检查从库延迟，需要配置`proxy-id`选项，否则不会生效

> check-slave-delay = true

### slave-delay-down

Default: 60 (seconds)

从库延迟超过该秒，状态将被设置为DOWN

> slave-delay-down = 120

### slave-delay-recover

Default: *slave-delay-down / 2* (seconds)

从库延迟少于该秒数，状态将恢复为UP

> slave-delay-recover = 30

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

> max-open-files = 10240

