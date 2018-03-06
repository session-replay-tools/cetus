# 配置文件说明

配置文件包括用户配置文件（users.json）、变量处理配置文件（variables.json）、分库版本的分片规则配置文件（sharding.json）、读写分离版本的启动配置文件（proxy.conf）和分库版本的启动配置文件（shard.conf），具体说明如下：

##  1.users.json

```
{
        "users":        [{
                        "user": "dbtest",
                        "client_pwd":   "123456",
                        "server_pwd":   "123456"
                }, {
                        "user": "root",
                        "client_pwd":   "123",
                        "server_pwd":   "123456"
                }]
}
```

其中user是用户名；client_pwd是前端登录Cetus的密码；server_pwd是Cetus登录后端的密码。

##  2.variables.json

Cetus支持部分会话级系统变量的设置，可以通过配置允许发送的值和静默处理的值，如下

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
      "name": "connect_timeout",
      "type": "string",
      "allowed_values": ["*"],
      "silent_values": ["10", "100"]
    }
  ]
}
```

其中name是变量名称；type是变量类型，string，string-csv逗号分隔的字符串值，目前尚未支持int类型；allowed_values是指定允许设定的值，可以使用通配符\*，表示此变量设任意值都允许；silent_values是指定静默处理的值，可以使用通配符\*，表示此变量设任意值都静默处理，前提：配置过allowed_values才能走到静默处理流程。

## 3.sharding.json

```
{
  "vdb": [
    {
      "id": 1,
      "type": "int",
      "method": "hash",
      "num": 8,
      "partitions": {"data1": [0,1], "data2": [2,3], "data3": [4,5], "data4": [6,7]}
    },
    {
      "id": 2,
      "type": "int",
      "method": "range",
      "num": 0,
      "partitions": {"data1": 124999, "data2": 249999, "data3": 374999,"data4": 499999}
    }
  ],
  "table": [
    {"vdb": 1, "db": "employees_hash", "table": "dept_emp", "pkey": "emp_no"},
    {"vdb": 1, "db": "employees_hash", "table": "employees", "pkey": "emp_no"},
    {"vdb": 1, "db": "employees_hash", "table": "titles", "pkey": "emp_no"},
    {"vdb": 2, "db": "employees_range", "table": "dept_emp", "pkey": "emp_no"},
    {"vdb": 2, "db": "employees_range", "table": "employees", "pkey": "emp_no"},
    {"vdb": 2, "db": "employees_range", "table": "titles", "pkey": "emp_no"}
  ],
  "single_tables": [
    {"table": "regioncode", "db": "employees_hash", "group": "data1"},
    {"table": "countries",  "db": "employees_range", "group": "data2"}
  ]
}
```

其中vdb是逻辑db，包含属性有id、type、method、num和partitions，id是逻辑db的id，type是分片键的类型，method是分片方式，num是hash分片的底数（range分片的num为0），partitions是分组名和分片范围的键值对；table是分片表，包含属性有vdb、db、table和pkey，vdb是逻辑db的id，db是物理db名，table是分片表名，pkey是分片键；single_tables是未分片表，包含属性有table、db和group，table是表名，db是物理db名，group是分组名。

##  4.proxy.conf

```
[cetus]
# Loaded Plugins
plugins=proxy,admin

# Proxy Configuration
proxy-address=proxy-ip:proxy-port
proxy-backend-addresses=rw-ip:rw-port
proxy-read-only-backend-addresses=ro-ip:ro-port

# Admin Configuration
admin-address=admin-ip:admin-port
admin-username=admin
admin-password=admin

# Backend Configuration
default-db=test
default-username=dbtest

# File and Log Configuration
log-file=cetus.log
log-level=debug
```

其中plugins是加载插件的名称，读写分离版本需加载的插件为proxy和admin；proxy-address是Proxy监听的IP和端口；proxy-backend-addresses是读写后端(主库)的IP和端口，可多项；proxy-read-only-backend-addresses是只读后端(从库)的IP和端口，可多项；admin-address是管理模块的IP和端口；admin-username是管理模块的用户名；admin-password是管理模块的密码明文；default-db是默认数据库，当连接未指定db时，使用的默认数据库名称；default-username是默认用户名，在Proxy启动时自动创建连接使用的用户名；log-file是日志文件路径；log-level是日志记录级别，可选 info | message | warning | error | critical(default)；其他可选性能配置详见[Cetus 启动配置选项说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-configuration.md)。

##  5.shard.conf

```
[cetus]
# Loaded Plugins
plugins=shard,admin

# Proxy Configuration
proxy-address=proxy-ip:proxy-port
proxy-backend-addresses=ip1:port1@data1,ip2:port2@data2,ip3:port3@data3,ip4:port4@data4

# Admin Configuration
admin-address=admin-ip:admin-port
admin-username=admin
admin-password=admin

# Backend Configuration
default-db=test
default-username=dbtest

# Log Configuration
log-file=cetus.log
log-level=debug
```

其中其中plugins是加载插件的名称，分库（sharding）版本需加载的插件为shard和admin；proxy-address是Proxy监听的IP和端口；proxy-backend-addresses是后端的IP和端口，需要同时指定group（@group）；其他选项与proxy.conf含义相同；其他可选性能配置详见[Cetus 启动配置选项说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-configuration.md)。

**注：以上配置文件中.json文件名称不可变，.conf文件可自定义名称，并利用命令行加载**
