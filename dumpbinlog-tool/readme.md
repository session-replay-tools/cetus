## Cetus数据迁移追数工具使用手册

### 1 工具介绍

主要用途是：将binlog转换成SQL，用于Cetus数据迁移。

### 2 工具用法

#### 2.1 配置文件

配置文件`binlog.conf`中分为三个段，分别是`[BINLOG_MYSQL]`，`[OUTPUT_MYSQL]`和`[DEFAULT]`。

`[BINLOG_MYSQL]`用来配置产生Binlog的MySQL的账号信息；`[OUTPUT_MYSQL]`用来配置解析得到的SQL发往的MySQL的账号信息；`[DEFAULT]`则是用来配置该工具的一些选项。

#### 2.2 参数介绍

基本的参数说明如下所示：

```
# 产生Binlog的MySQL账号信息
[BINLOG_MYSQL]
host=172.17.0.4
port=3306
user=ght
password=123456

# 解析后得到的SQL发往的MySQL账号信息
# 扩容时可以配置成新搭建的Cetus的账号信息
[OUTPUT_MYSQL]
host=172.17.0.2 
port=6002
user=ght
password=123456

[DEFAULT]
# 解析Binlog的开始位置
log_file=binlog.000001
log_pos=351

# 需要跳过的schema，即解析到该schema中的SQL全部忽略
skip_schemas=proxy_heart_beat

# 设置日志级别
log_level=DEBUG

# 是否忽略DDL操作
ignore_ddl=true

# 配置只解析的分库表名
# 只有这些表的操作输出，其他的（如全局表）的操作会被丢弃
# 兼容Cetus的配置文件
only_sharding_table=/data/sharding.json
```

#### 2.3 断点续传介绍

进度日志记录在`workdir/progress.log`文件中。下次启动会自动从这里继续，如果不想续传，可以**启动前将该文件删除**。

#### 2.4 启动及选项
启动时，可以指定 `-d`参数，用以指定工作目录，即`workdir`。

启动命令类似如下：

```
chmod +x ./dumpbinlog.py

./dumpbinlog.py
```
