# Cetus xa悬挂处理工具

##  简介

中间件Cetus处理分布式事务可能会由于网络、节点错误而中断，导致xa事务悬挂。Cetus xa悬挂处理工具是由python语言开发的，主要是在Cetus遇到分布式事务悬挂时自动处理悬挂事务的修复工具。

## 原理

该工具主要包括悬挂事务查找模块和悬挂事务处理模块。其中悬挂事务查找模块是通过读取MySQL中xa recover的结果，获取长时间处于悬挂的事务xid列表，将所有后端的xa悬挂事务对应的xid汇总并去重，再读取后端binlog日志的内容获得所有后端xa悬挂事务的xid对应的最终状态；悬挂事务处理模块主要是根据悬挂事务查找模块获取的最终状态，对悬挂事务进行简单的处理，即当悬挂事务的最终状态为PREPARE、ROLLBACK、END或START时进行回滚操作，当悬挂事务的最终状态为COMMIT时进行提交操作。

## 安装启动步骤

### 安装依赖

- python

- python需要的模块Pool

- python需要的模块MySQLdb

请确保在使用Cetus xa悬挂处理工具前已安装好相应的依赖。

### 启动步骤

- 配置：将xa悬挂处理工具记录的日志路径、Cetus的启动配置文件路径、Cetus的用户设置文件和临时文件的存放路径等信息填入xa悬挂处理工具前部的CONFIG中，如下：

```
CONFIG = {"logs": "/data/cetus/xa_suspension_logs/xa-suspension.log",
          "backend": "/home/mysql-cetus/cetus_install/conf/cetus.conf",
          "user": "/home/mysql-cetus/cetus_install/conf/users.json",
          "temp_file": "/data/cetus/xa_suspension_logs/"
         }
```

- 启动：将xa悬挂处理工具设置为可执行文件，并在后台运行，指令如下：

```
chmod +x xa-suspension.py
nohup ./xa-suspension.py &
```

## 注意事项

- 由于该工具主要是结合Cetus软件处理xa悬挂事务的，因此请确保使用该工具前已运行Cetus。
- 由于该工具主要是针对当天的悬挂事务进行处理，若需要在开启Cetus软件的同时处理悬挂事务，请确保及时开启该工具。
- 要确保cetus服务器和MySQL服务器的时间是同步的，且在同一个时区。
- 如果cetus端和MySQL端连接需要采用保活机制，请在cetus端的tcp层面设置keepalive，MySQL请不要设置，否则会出现网络隔离情况下数据不一致的情况。
- 一旦cetus所在的网络和MySQL的网络相隔离（互相不通），悬挂事务处理工具已经无法去rollback悬挂事务，请关闭悬挂事务处理工具，并杀死MySQL端的僵死线程或者重启MySQL，然后根据cetus的xa log来xa rollback MySQL的悬挂事务。
