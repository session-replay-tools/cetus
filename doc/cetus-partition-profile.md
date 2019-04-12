## 分表(partition)版配置文件说明

### 1 分表版本编译方法

分表版本编译方法和分片(shard)版本是一样的，例如:

```
CFLAGS='-g -Wpointer-to-int-cast' cmake ../ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/home/user/cetus_install -DSIMPLE_PARSER=OFF

```

详细的编译方法，详见[Cetus 安装说明](https://github.com/cetus-tools/cetus/blob/master/doc/cetus-install.md)。

### 2 分表版本配置方法

分表(partition)版配置文件基本跟分片（shard）版本一样，不同之处在shard.conf文件：

- 配置partition-mode=true, 使cetus工作在分表模式
- proxy-backend-addresses和proxy-read-only-backend-addresses配置具体参考读写分离，不需要像分库那样加group

### 3 分表版本的约定

- 子表名的约定

自表名的命名规则是：原表名_vdb分表名。

例如，配置文件sharding.json中配置如下：
```
"vdb": [
    {
      "id": 1,
      "type": "int",
      "method": "hash",
      "num": 4,
      "partitions": {"data1": [0,1], "data2": [2,3]}
    }],
"table": [   
    {"vdb": 1, "db": "ght", "table": "t1", "pkey": "id"}
    ]
} 
```
则表t1分2个子表，分表之后的各个子表名为：t1\_data1, t1\_data2。

- 子表的创建

目前Cetus不负责子表的创建。需要用户在Cetus中配置好分表规则后，自己手动在主库上创建各个子表。

### 4 举例

假设，目前需要将表tb，按照hash方式分成4个子表，每个子表表名分别为：tb\_hs0, tb\_hs1, tb\_hs2, tb\_hs3。则实现上述需求，配置步骤如下：

- 编译shard版本的Cetus，参考编译命令如下：

```
CFLAGS='-g -Wpointer-to-int-cast' cmake ../ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/home/user/cetus_install -DSIMPLE_PARSER=OFF
```

- 配置shard.conf文件，使Cetus工作在分表模式下。参考配置如下：

```
proxy-backend-addresses=172.17.0.3:3306
proxy-read-only-backend-addresses=172.17.0.4:3306
partition-mode=true
```

**特别注意**, 不需要在IP PORT后添加`@data1`

- 配置sharding.json，配置分表规则

```
{
  "vdb": [
    {
      "id": 1,
      "type": "int",
      "method": "hash",
      "num": 8,
      "partitions": {"hs0": [0,1], "hs1": [2,3], "hs2": [4,5], "hs3": [6,7]}
    }
  ],
 "table": [
    {"vdb": 1, "db": "ght", "table": "tb", "pkey": "id"}
  ]
}
```

- 创建子表

直连主库，创建对应的子表:

```
use ght;
create table tb_hs0 (id int);
create table tb_hs1 (id int);
create table tb_hs2 (id int);
create table tb_hs3 (id int);
```

- 启动Cetus

```
./bin/cetus --defaults-file=conf/shard.conf
```

- 验证

登录Cetus的代理端口，执行`shard_explain`命令，查看路由情况:

```
shard_explain select * from tb where id = 7;
+--------+-------------------------------------+
| groups | sql                                 |
+--------+-------------------------------------+
| hs3    | SELECT * FROM tb_hs3  WHERE id = 7; |
+--------+-------------------------------------+
1 row in set (0.00 sec)

shard_explain select * from tb where id = 5;
+--------+-------------------------------------+
| groups | sql                                 |
+--------+-------------------------------------+
| hs2    | SELECT * FROM tb_hs2  WHERE id = 5; |
+--------+-------------------------------------+
1 row in set (0.00 sec)

shard_explain select * from tb where id = 3;
+--------+-------------------------------------+
| groups | sql                                 |
+--------+-------------------------------------+
| hs1    | SELECT * FROM tb_hs1  WHERE id = 3; |
+--------+-------------------------------------+
1 row in set (0.00 sec)

shard_explain select * from tb where id = 1;
+--------+-------------------------------------+
| groups | sql                                 |
+--------+-------------------------------------+
| hs0    | SELECT * FROM tb_hs0  WHERE id = 1; |
+--------+-------------------------------------+
1 row in set (0.00 sec)
```
