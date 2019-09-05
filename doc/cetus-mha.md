# cetus + mha高可用方案
## 简介
cetus使用改进过的mha实现高可用。乐得DBA在原生mha中，加入修改cetus状态的模块和mha操作过程的短信、邮件通知功能。修改过的mha版本简称mha_ld。

mha切换包括故障切换和在线手工切换，以故障切换(failover)为例，简单介绍一下mha_ld的工作流程，未提及的操作请参考官方文档：
https://github.com/yoshinorim/mha4mysql-manager/wiki

![Cetus mha](https://github.com/Lede-Inc/cetus/blob/master/doc/picture/cetus_mha.jpg)

1、检测当前主库不达，准备开始切换

2、在cetus管理端修改当前主库的状态，将当前主库状态修改为“维护(maintaining)”，类型修改为“只读(ro)”，发送修改的通知短信

update backends set state='maintaining' , type='ro' where address='172.0.0.1:3306';

3、提升MySQL从库为新的主库

4、在cetus管理端修改新主库的状态，将当前主库状态修改为“unknown”，类型修改为“读写(rw)”，发送修改的通知短信

update backends set state='unknown' , type='rw' where address='172.0.0.2:3306';

## 安装
在master和node主机节点yum安装rpm包

yum install -y  perl-DBD-MySQL perl-Config-Tiny perl-Log-Dispatch perl-Parallel-ForkManager perl-Config-IniFiles


master和node主机节点，安装mha4mysql-node-0.56-0.el6.noarch.rpm包

rpm -ivh mha4mysql-node-0.56-0.el6.noarch.rpm 

master主机节点，安装mha4mysql-manager-0.56-0.el6.noarch.rpm包

rpm -ivh mha4mysql-manager-0.56-0.el6.noarch.rpm

使用 mha_ld/src 替换所有文件/usr/share/perl5/vendor_perl/MHA/目录的所有同名文件

使用 mha_ld/masterha_secondary_check替换masterha_secondary_check命令
 which masterha_secondary_check

/usr/bin/masterha_secondary_check

rm /usr/bin/masterha_secondary_check

cd /usr/bin/

上传修改后的masterha_secondary_check

chmod +x /usr/bin/masterha_secondary_check

## 配置
安装好mha_ld后，配置启动mha的cnf文件请参考mha_ld/sample.cnf，参数部分可以参考mha githup官方文档
https://github.com/yoshinorim/mha4mysql-manager/wiki/Configuration

配置cnf后，有一个变量proxy_conf（变量需要写绝对路径），文件内容参考mha_ld/cetus.cnf：

这个文件记录的是cetus的连接信息，含义解释如下：

middle_ipport=127.0.0.1:4306,127.0.0.14:4307

middle_user=admin

middle_pass=xxxxxxx

middle_ipport记录多组cetus。一组cetus由ip和端口组成，ip和端口用英文冒号分隔；每组cetus用英文逗号分隔。

middle_user记录每组cetus管理入口的用户名，与cetus配置文件中的admin-username变量值一致。同一个mha_ld维护的多组cetus，管理入口用户名必须一致。

middle_pass记录每组cetus管理入口的用户密码，与cetus配置文件中的admin-password变量值一致。同一个mha_ld维护的多组cetus，管理入口用户密码必须一致。



## 修改切换时的通知

在/usr/share/perl5/vendor_perl/MHA/ManagerConst.pm文件中，有一个变量MOBILE_PHONES，更改该变量，可以将需要收到短信通知的人加入列表中，例如：

our @MOBILE_PHONES = (

  1234567890,  # zhang

  2345678901,  # wang

);

另外，还有几个文件涉及到发送短信时使用的url，分别为HealthCheck.pm/MasterFailover.pm/MasterMonitor.pm/ProxyManager.pm，找到这些文件中send_alert函数，修改 curl调用即可。
