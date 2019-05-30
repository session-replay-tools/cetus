# 读写分离版配置文件说明

读写分离版配置文件包括用户配置文件（users.json）、变量处理配置文件（variables.json）和启动配置文件（proxy.conf），具体说明如下：

##  1.users.json

```
{
        "users":        [{
                        "user": "XXXX",
                        "client_pwd":   "XXXXXX",
                        "server_pwd":   "XXXXXX"
                }, {
                        "user": "XXXX",
                        "client_pwd":   "XXXXXX",
                        "server_pwd":   "XXXXXX"
                }]
}
```

users.json用来配置用户登陆信息，采用键值对的结构，其中键是固定的，值是用户在MySQL创建的登陆用户名和密码。

其中user的值是用户名；client_pwd的值是前端登录Cetus的密码；server_pwd的值是Cetus登录后端的密码。

例如：

```
{
       "users":        [{
                       "user": "root",
                       "client_pwd":   "123",
                       "server_pwd":   "123456"
               }, {
                       "user": "test",
                       "client_pwd":   "456",
                       "server_pwd":   "123456"
               }]
}
```

我们配置了2个用户名root和test。其中root用户前端登录Cetus的密码是123，Cetus登录后端的密码是123456；test用户前端登录Cetus的密码是456，Cetus登录后端的密码是123456。

##  2.variables.json

Cetus支持部分会话级系统变量的设置，可以通过在variables.json配置允许发送的值和静默处理的值，如下：

```
{
  "variables": [
    {
      "name": "XXXXX",
      "type": "XXXX",
      "allowed_values": ["XXX"]
    },
    {
      "name": "XXXXX",
      "type": "XXXX",
      "allowed_values": ["XXX"],
      "silent_values": ["XX"]
    }
  ]
}
```

variables.json同样采用键值对的结构，其中键是固定的，值是用用户自定义的。

其中name的值是需要设置的会话级系统变量的名称；type的值是变量的类型，可以为int,string或string-csv逗号分隔的字符串值；allowed_values的值是指定允许设定的变量值，可以使用通配符\*表示此变量设任意值都允许；silent_values的值是指定静默处理的值，可以使用通配符\*，表示此变量设任意值都静默处理。特别提醒，配置文件中配置的所有项，都要用双引号包裹起来，否则不生效。

**注意：配置过allowed_values才能走到静默处理流程**

例如：

```
{
 "variables": [
   {
     "name": "sql_mode",
     "type": "string-csv",
     "allowed_values":
     ["STRICT_TRANS_TABLES",
       "NO_AUTO_CREATE_USER",
       "NO_ENGINE_SUBSTITUTION"
     ]
   },
   {
     "name": "profiling",
     "type": "int",
     "allowed_values": ["0", "1"],
     "silent_values": ["*"]
   }
 ]
}
```

我们配置了sql_mode变量和profiling变量。其中sql_mode变量的类型是string-csv（逗号分隔的字符串值），指定了允许设定的变量有STRICT_TRANS_TABLES、NO_AUTO_CREATE_USER和NO_ENGINE_SUBSTITUTION；profiling变量的类型是int（整型），此变量允许的值是0和1，指定静默处理的值为所有，即0和1。

##  3.proxy.conf

```
[cetus]
# Loaded Plugins
plugins=XXX,XXX

# Defines the number of worker processes. 
worker-processes=XXX

# Proxy Configuration
proxy-address=XXX.XXX.XXX.XXX:XXXX
proxy-backend-addresses=XXX.XXX.XXX.XXX:XXXX
proxy-read-only-backend-addresses=XXX.XXX.XXX.XXX:XXXX

# Admin Configuration
admin-address=XXX.XXX.XXX.XXX:XXXX
admin-username=XXXX
admin-password=XXXXXX

# Backend Configuration
default-db=XXXX
default-username=XXXXX

# File and Log Configuration
log-file=XXXX
log-level=XXXX
```

proxy.conf是读写分离版本的启动配置文件，在启动Cetus时需要加载，配置文件采用key=value的形式，其中key是固定的，可参考[Cetus 启动配置选项说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-configuration.md)，value是用户自定义的。

例如：

```
[cetus]
# Loaded Plugins
plugins=proxy,admin

# Defines the number of worker processes. 
worker-processes=4

# Proxy Configuration
proxy-address=127.0.0.1:1234
proxy-backend-addresses=127.0.0.1:3306
proxy-read-only-backend-addresses=127.0.0.1:3307

# Admin Configuration
admin-address=127.0.0.1:5678
admin-username=admin
admin-password=admin

# Backend Configuration
default-db=test
default-username=test

# File and Log Configuration
log-file=cetus.log
log-level=debug
```

我们配置了读写分离版本的启动选项，其中plugins的值是加载插件的名称，读写分离版本需加载的插件为proxy和admin；

worker-processes为4，代表工作进程数量为4，建议设置数量小于等于cpu数目；

proxy-address的值是Proxy监听的IP和端口，我们设置为127.0.0.1:1234；proxy-backend-addresses的值是读写后端(主库)的IP和端口，我们设置为127.0.0.1:3306，只允许一个主库；proxy-read-only-backend-addresses的值是只读后端(从库)的IP和端口，我们设置为127.0.0.1:3307，可多项；

admin-address的值是管理模块的IP和端口，我们设置为127.0.0.1:5678；admin-username的值是管理模块的用户名，我们设置为admin；admin-password的值是管理模块的密码明文，我们设置为admin；

default-db的值是默认数据库，当连接未指定db时，使用的默认数据库名称，我们设置为test；default-username的值是默认登陆用户名，在Proxy启动时自动创建连接使用的用户名，我们设置为test；

log-file的值是日志文件路径，我们设置为当前安装路径下的cetus.log；log-level的值是日志记录级别，可选 info | message | warning | error | critical(default)，我们设置为debug；这些是必备启动选项，其他可选的性能配置详见[Cetus 启动配置选项说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-configuration.md)。

**注：**

**以上配置文件中.json文件名称不可变，.conf文件可自定义名称，并利用命令行加载**

**启动配置文件proxy.conf 常用参数：**

**1）default-pool-size=\<num\>，设置刚启动的连接数量（by a worker process），最小只能设置为10，如果设置小于10，则实际该值为10**

**2）max-pool-size=\<num\>，设置最大连接数量（by a worker process）**

**3）max-resp-size=\<num\>，设置最大响应大小，一旦超过此大小，则会报错给客户端**

**4）enable-client-compress=\[true\|false\]，支持客户端压缩**

**5）enable-tcp-stream=\[true\|false\]，启动tcp stream，无需等响应收完就发送给客户端**

**6）master-preferred=\[true\|false\]，除非注释强制访问从库，否则一律访问主库**

**7）reduce-connections=\[true\|false\]，自动减少过多的后端连接数量**

**8）max-alive-time=\<num\>，设置后端连接最大存活时间**

**9）enable-fast-stream=\[true\|false\]，启动fast stream，快速处理只读响应，release版本默认为false，开发版本默认为true**
