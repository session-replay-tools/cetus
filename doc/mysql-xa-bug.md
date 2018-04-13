# MySQL xa事务问题说明

## 发现的问题
cetus测试过程中，发现MySQL的xa 事务有bug，MySQL版本为5.7.21。测试方法为：开启一个模拟银行账户转账的测试任务，随机kill -9 杀掉后端MySQL实例，并重启实例。发现有两种悬挂事务的情况：

## 问题的现象
### 1、只有主库存在悬挂事务

MySQL主库已接受到xa commit通知，xa commit未完成前，kill -9 杀掉MySQL主库,再启动MySQL主库，主库出现悬挂事务，而从库该分布式事务已提交。主库此时需要执行xa commit语句，提交分布式事务，这个操作同步到从库后，会导致从库sql应用进程报错，提示找不到该分布式事务。

### 2、只有从库出现悬挂事务

cetus向后端分片发送xa prepare，分片MySQL主库接收到xa prepare，xa prepare未完成前，kill -9 杀掉MySQL主库，再启动MySQL主库，用xa事务已回滚，主库未出现悬挂事务；从库出现悬挂事务。这种情况下，从库需要回滚xa事务，才能保证数据的一致性。

以上两种情况，主库的xa事务状态，跟binlog记录的事务状态不一致。在MySQL官方文档找到解释，MySQL异常关闭，有可能导致数据库状态和binlog不一致。这些bug，在非正常关闭MySQL时才出现，正常关闭mysql不会出现这个问题。如果出现xa事务悬挂，可以用[Cetus xa悬挂处理工具](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-xa.md)自动处理。
      
## 官方的描述

XA is not fully resilient to an unexpected halt with respect to the binary log (on the master). If there is an unexpected halt before XA PREPARE, between XA PREPARE and XA COMMIT (or XA ROLLBACK), or after XA COMMIT (or XA ROLLBACK), the server and binary log are correctly recovered and taken to a consistent state. However, if there is an unexpected halt in the middle of the execution of one of these statements, the server may not be able to recover to a correct state, leaving the server and the binary log in an inconsistent state.
已提交至官方的xa bug：
XA prepare is logged ahead of engine prepare
https://bugs.mysql.com/bug.php?id=76233

## 详细的验证

我们对MySQL 5.7.21 XA bug的详细验证主要包括从外部看到的现象和代码层分析两部分：

[MySQL 5.7.21 XA bug 外部现象](https://github.com/Lede-Inc/cetus/blob/master/doc/MySQL-5.7.21-XA-bug-phenomena.pdf)

[MySQL 5.7.21 XA bug 代码层分析](https://github.com/Lede-Inc/cetus/blob/master/doc/MySQL-5.7.21-XA-bug-code-analysis.pdf)
