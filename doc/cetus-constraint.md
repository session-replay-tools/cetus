# Cetus使用约束说明

Cetus分为读写分离和分库两个版本，具有如下使用约束：

## 针对两个版本

### 1.不支持批量sql语句的执行

### 2.不支持TLS

### 3.不支持多租户

### 4.单进程工作模式，建议在docker容器使用

### 5.只支持linux系统

### 6.不支持客户端ctl+c操作，即不支持kill query操作

### 7.set命令的有限支持，不支持global级别的set命令，支持部分session级别的set命令

### 8.sql语句的有限支持，包括以下几点：

1）不支持将LAST_INSERT_ID 嵌套在INSERT或者其他的语句中

2）不支持客户端的change user命令

## 针对分库版

### 1.目前分库版不支持动态扩容

### 2.目前分库版不支持二级分区

### 3.分库版最多支持64个分库，建议4，8，16个分库

### 4.分库版不支持跨库join

### 5.分库版的自增主键最好用第三方，比如redis

### 6.分库版的sql限制比读写分离版的要多，除了以上针对两个版本的限制，还包括以下几点：

1）不支持AVG聚合函数

2）聚合函数不能当除数，且聚合函数跟聚合函数不能相乘

3）不支持COUNT(DISTINCT)/SUM(DISTINCT)

4）不支持存储过程和视图

5）不支持批量sql语句的执行

6）不支持having多个条件

7）不支持含有any/all/some的子查询语句

8）不支持load data infile

9）不支持handler语法

10）不支持lock tables语法

11）针对分片表，ORDER BY排序字段不超过8个列

12）针对分片表，仅支持DISCTINCT字段同时也是ORDER BY字段

13）针对分片表，CASE WHEN/IF不能用于DML语句中，也不能用在GROUP BY后

14）不支持跨库的JOIN

15）当Where条件中有分区列时值必须是原子值

16）在做SQL查询时只支持同一个 VDB 内的关联查询，针对 sharding 表，可以使用 sharding key 的要求加上该过滤条件

17）不支持服务器端 PREPARE

18）使用中文列名或中文别名时必须加引号

19） order by和group by的字段，必须出现在前面字段列表中

20）不支持session级别的profiling


