## udf_sharding_hash.so使用手册

### 1 工具介绍

在MySQL实例，使用udf_sharding_hash.so创建函数。通过这个函数，可以算出记录所在桶号。

### 2 工具用法

查询MySQL plugin_dir：
```
mysql> show variables like 'plugin_dir'; 
+---------------+--------------------------+
| Variable_name | Value                    |
+---------------+--------------------------+
| plugin_dir    | /usr/lib64/mysql/plugin/ |
+---------------+--------------------------+
```
将udf_sharding_hash.so包上传至/usr/lib64/mysql/plugin/目录

创建MySQL函数：
```
mysql> create function sharding_hash returns integer soname 'udf_sharding_hash.so';
```

删除MySQL函数：
```
mysql> drop function sharding_hash;
```

使用方法：
```
select sharding_hash('col_value',X);
```
col_value为列值，X为分片表桶的数量，与sharding.json文件'"num": X' 中的X含义一致。

具体使用例子：
```
mysql> select sharding_hash('test',4);
+-------------------------+
| sharding_hash('test',4) |
+-------------------------+
|                       2 |
+-------------------------+

mysql> select sharding_hash(123,4);
+----------------------+
| sharding_hash(123,4) |
+----------------------+
|                    3 |
+----------------------+
```
