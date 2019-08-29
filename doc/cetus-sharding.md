# Cetus sharding版使用指南

## 简介

Cetus sharding版支持对后端的数据库进行分库，可以根据hash/range方式对大表进行分布式部署，提升系统整体的响应和容量。

## 安装部署

### 准备

**1.MySQL**

- 5.7.17以上版本（分布式事务功能需要）

- 数据库设计（即分库，根据业务将数据对象分成若干组）

- 创建用户和密码

- 确认Cetus可以远程登录MySQL

**2.Cetus**

- 根据MySQL后端信息配置users.json、sharding.json和shard.conf（variables.json可选配），具体配置说明详见[Cetus 分库(sharding)配置文件说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-shard-profile.md)

**3.LVS & keepalived**

- 确定LVS的监听ip和端口

- 根据实际情况和需求确定多个Cetus的LVS分发权重

- 配置keepalived.conf

### 安装

Cetus只支持linux系统，安装过程详见[Cetus 安装说明](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-install.md)

### 部署

Cetus 在部署时架构图如下图所示。

![Cetus sharding版本部署时架构图](https://github.com/Lede-Inc/cetus/blob/master/doc/picture/Cetus_deployment_sharding.png)

Cetus位于应用程序与MySQL数据库之间，作为前端应用与数据库的通讯。其中，前端应用连接LVS节点，LVS节点映射端口到多个Cetus服务，后者通过自身的连接池连接到后端的数据库。

MySQL和Cetus可以部署在同一台服务器上，也可以部署在各自独立的服务器上，但一般LVS & keepalived与MySQL、Cetus分布在不同的服务器上。

### 启动

Cetus可以利用bin/cetus启动

```
bin/cetus --defaults-file=conf/shard.conf [--conf-dir=/home/user/cetus_install/conf/]
```

​其中Cetus启动时可以添加命令行选项，--defaults-file选项用来加载启动配置文件，且在启动前保证启动配置文件的权限为660；--conf-dir是可选项，用来加载其他配置文件(.json文件)，默认为当前目录下conf文件夹。

​Cetus可起动守护进程后台运行，也可在进程意外终止自动启动一个新进程，可通过启动配置选项进行设置。

##  数据库设计

### 1.专用术语

**1.数据分片**

  按一定的规则，将数据表横向切分成若干部分并将其部署在一个或多个数据节点上。

**2.分片表**

  即Sharding表，将属于同一张表中的数据根据规则拆分成多个分片，分别存入底层不同的存储节点。

**3.分片键**

  即Sharding Key，Sharding表将根据此分片键再加上拆分规则，从而成为sharding表。

**4.拆分方法**

  目前支持的拆分规则有hash、range，其中hash支持数字类型、字符串类型，range支持数字类型、字符串类型、时间类型（date/datetime）。

  Hash分片即数据库中的Hash分区。首先，将Sharding表中Sharding Key的数据进行hash，然后根据制定的分片规则，进行数据存储。Sharding Key常选用数字类型。

  Range分片类似数据库中的Range分区。将Sharding表中Sharding Key的数据进行范围分片，根据规则，进行数据存储。Sharding Key常选用数字类型、时间类型（DATE/DATETIME）。

**5.全局表**

  Public表，即全局表，具有相同VDB的存储节点公共表数据是一致的，即都是全量数据，但不同VDB的存储节点公共表是不同的，如果想具有相同公共表，需要前端再处理，例如配置表conf，VDB1和VDB2都需要，需要前端应用分别写入VDB1、VDB2。

**6.单点全局表**

  Single表，即单点全局表，如果表的写操作频繁，且不合适做分片，也不需要跟其他分片表或单点全局表关联，可以把这种类型的表定义为单点全局表。单点全局表数据放在用户指定的唯一一个后端节点，只涉及单点全局表的写操作是单机事务，不会开启分布式事务，减少了事务的代价；缺点是单点全局表只能跟全局表做join关联。

**7.VDB**

  VDB（Virtual DataBase）非物理DB，即逻辑DB，主要对应分片表，表现在业务数据层，即代表此VDB内的数据有相同属性值，可以根据此属性值，进行数据的进一步拆分，从而构成分片表，例如订单数据和用户数据属于同一个VDB，可以根据用户ID进行分片，而仓储和商品可以放入另一个VDB。不同VDB之间的数据不能进行关联查询，只有在同一个VDB内才支持。

### 2.设计原则

按业务数据的内在联系（例如，支持同一业务模块，或经常需要一起存取或修改等），将数据对象分成若干个组，使同一分组的数据表高度“内聚”，不同分组之间的表高度“独立”；找出每个组中各表共同的“根元”，以此作为分片键，对数据进行分片。

VDB要在业务设计之初确定，后续的底层数据存储以及上层数据操作都会与此息息相关，所以需要开发、DBA一同来设计。

以下是具体样例：

**用户VDB（VDB_ACCOUNT）**

描述：与用户订单相关联的业务

分片键：ACCOUNT_ID

TB_ORDER（订单表）

TB_ACCOUNT（用户信息表）

TB_PAY_ORDER（支付订单表）

TB_CHARGE_ORDER（充值订单表）

TB_TRANSFER_RECORD（转账记录表）

TB_ACTIVITY_RECORD（活动记录表）

TB_REFUND_ORDER（退款记录表）

**产品VDB（VDB_PRODUCT）**

描述：与产品相关联的业务

分片键：PERIOD_ID

TB_PRODUCT（商品信息表）

TB_PRODUCT_PERIOD（商品期次表）

## 主要功能概述

### 1.连接池功能

Cetus内置了连接池功能。该连接池会在Cetus启动时，使用默认用户自动创建到后端的连接。创建的连接使用的database是配置的默认database，这些选项是由DBA根据后端数据库的实际情况进行的配置。以确保主要的业务请求过来不需要临时建立连接并切换数据库。

### 2.数据分片功能

Cetus支持对后端的数据库进行分库，可以根据hash/range方式对大表进行分布式部署，提升系统整体的响应和容量。由于分片处理后，数据的整体性被破坏，为了保证查询的结果符合预期，我们需要对SQL进行分析，并改写后发送到不同的后端。

### 3.分布式事务处理

如果前端执行SQL时，开启了事务（start transaction），则统一采用分布式事务处理（除非开启了单点事务的注释功能），如果未开启事务，直接发送SQL指令， Cetus 在处理时会判断是否开启分布式事务。

### 4.结果集压缩

由于当连接距离较远网络延迟较大时，结果集较大会很大幅度地增加数据传输时长，降低性能，因此针对高延迟场合，Cetus支持对结果集的压缩来提高性能。

网络延迟越大，开启结果集压缩功能的优势越大，当连接距离较近几乎没有网络延迟时，Cetus无形中增加了压缩和解压步骤，反而降低了性能，因此在延迟较小时不建议使用结果集压缩。

### 5.安全性管理

安全性管理功能包括后端管理、基本配置管理、查看连接信息、用户密码管理、IP许可管理、远程配置中心管理和整体信息查询。

用户可以通过登录管理端口，查看后端状态，增删改指定后端，查看和修改基本配置（包括连接池配置和从库延迟检测配置），查看当前连接的详细信息，用户连接Cetus的密码查询和增删改，查看以及增删改IP许可(可以为每个用户以及管理员用户指定允许访问的来源ip地址，但是无法为来自不同ip的同一用户提供不同的权限设置)，重载远程配置和查看Cetus总体状态等，具体管理操作相见[Cetus 分库(sharding)版管理手册](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-shard-admin.md)。

### 6.状态监控

Cetus内置监控功能，可通过配置选择开启或关闭。开启后Cetus会定期和后端进行通信，检测后端数据库的存活状态和主从延迟时间等。可通过登录管理端口SELECT * FROM backends查看。

后端状态包括unknown（后端初始状态，还未建立连接）、up（能与后端正常建立连接，可以正常提供服务）、down（与后端无法联通，无法正常提供服务）、maintaining（后端正在维护，无法建立连接）和delete（后端已被删除）。

主从延迟检测检测设有阈值slave-delay-down和slave-delay-recover，检测到的从库延迟超过slave-delay-down阈值后端状态将被设置为DOWN，从库延迟少于slave-delay-recover阈值后端状态将恢复为UP。

### 7.TCP流式

针对结果集过大情况，Cetus 采用了tcp stream流式，不需要先缓存完整结果集才转发给客户端，以避免内存炸裂问题，降低内存消耗，提高性能。

### 8.域名连接后端

支持利用域名连接数据库后端，Cetus设有相关的启动配置选项disable-dns-cache，选择是否开启解析连接到后端的域名功能，开启后可以通过设置域名并利用域名访问后端。

### 9.insert批量操作
支持在insert语句中写多个value，value之间用","隔开，例如：
INSERT INTO table (field1,field2,field3) VALUES ('a',"b","c"), ('a',"b","c"),('a',"b","c");

## 注意事项

### 1.连接池使用注意事项

所有的后端MySQL节点，都需要有相同的用户名及口令，在conf/users.json中指定，不在选项中指定的用户，不能用来登录Cetus，不能为每个不同的后端实例单独指定登录信息。用户连接Cetus时的用户名和密码不一定与后端一致，可在conf/users.json中查看。

### 2.set命令的支持说明

我们支持使用以下session级别的set命令： Set  names/autocommit。global级别的set命令统一不支持，其他未列出的set命令不在支持范围内，不建议使用。如果业务确实需要，建议联系DBA将行为配置为默认开启。

支持CLIENT_FOUND_ROWS 全局参数属性的统一设置，同一个Cetus只能选择打开或者关闭，不支持对每个连接单独设置这项属性，可以通过设置启动配置选项来打开，默认关闭。不支持CLIENT_LOCAL_FILES。其他未列出的需要额外设置的特性请测试后再确认。

### 3.环境变量修改建议

虽然我们支持客户端对连接环境变量进行修改，但是，我们不建议在程序中进行修改。因为一旦变量有改动。我们需要在执行SQL前对连接状态进行复位，会产生额外的请求到服务端，客户端响应的延迟也会增加。

### 4.代码处理

能用程序代码实现的函数尽量不用 SQL 函数，最好先在程序中计算出结果，再把值传入 SQL，如时间类函数 curdate()，字符串处理类函数 trim()。因为函数用于分库键将无法路由，导致查询语句需要发送到所有后端，单事务可以搞定的事情就变成了分布式事务，性能变差。

程序做分页任务时，尽量自己记录页偏移，因为 Cetus 做偏移时offset 会重写，SQL 把 offset 置为零，数据全部取回后再截取该页需要的数据，这样一来数据量无形中就变大很多倍，且不能有效利用数据库索引，性能比较差，而且一旦数据超出内存阈值前端将接收到错误。

### 5.不支持 Kill query

不支持在 Sql 执行过程中 kill query操作，一旦 Sql 语句开始执行就不能通过这种方式来终止，此时可以连接Cetus 管理后端，通过执行 show connectionlist 命令查看正在执行的 Sql，从而找到正在执行的后端信息，通过数据库中 kill query的命令进行终止操作。

### 6.不支持TLS

不支持TLS协议，目前不能在某种程度上使主从架构应用程序通讯本身预防窃听、干扰和消息伪造。

### 7.不支持多租户

目前分片版本还不支持。

### 8.不支持动态扩容

目前分库版暂不支持动态扩容，需要手工迁移数据，多数情况下需要“停机”扩容。

### 9.分区限制

目前只支持一级分区，且最多支持64个分库（全局表的更新数量太大，会导致分布式事务性能急剧恶化），建议4，8，16个分库，暂不支持二级分区；分区的自增主键最好用第三方，比如redis。

### 10.SQL书写规范

由于SQL需要进行完整解析器，建议大家在书写涉及分片的SQL时，要按照标准书写，在测试环境下测试通过后再上线，请遵循以下规范：

1. 在Select语句中尽量为表达式列或函数计算列添加别名，比如“select count(\*) rowcnt from ...”，以利于提高SQL解释器的分析水平。 
2. Sql文本要简洁，针对sharding表，如果有条件，请务必加sharding key做为过滤条件。
3. 开启事务推荐使用start transaction。
4. 少用子查询这种写法，必须用的话，可以用关联查询语法进行替换。
5. Update/delete操作要根据sharding key进行过滤后操作（仅针对分片表）。
6. For update语句不建议使用，锁开销严重，建议在应用端处理该业务逻辑，比如引入分布式锁或者先分配给redis等等。

### 11.SQL支持

Cetus sharding版能支持大多数的SQL语句，目前限制支持的功能有以下几种：

**不支持项：**

**1.不支持COUNT(DISTINCT)/SUM(DISTINCT)/AVG(DISTINCT)**

  全局表没有限制；针对分片表建议分开操作，即先用 distinct 获取所有后端节点的值，类似 select distinct val from xxx order by val，然后将数据整合到一起做去重计数／去重求和／去重求平均值的工作。

**2.不支持LAST_INSERT_ID**

  目前线上没有发现该用法，如希望获取全局唯一值建议使用 redis 获取，另外Cetus本身也提供了一种方法，select cetus_sequence()，即可返回一个 64 位递增不连续随机数字。

**3.不支持存储过程和视图**

**4.不支持批量sql语句的执行**

**5.不支持客户端的change user命令**

**6.不支持having多个条件**

**7.不支持含有any/all/some的子查询语句**

  不支持含有any/all/some的子查询语句，例如：select dept_no,emp_no from dept_emp where emp_no > any (select emp_no from dept_emp where dept_no='d001');若需要可转成关联查询语句。

**8.不支持load data infile**

**9.不支持handler语法**

**10.不支持lock tables语法**

**11.多个聚合函数情况下，不支持having条件，也不支持聚合函数之间乘除法**

**限制支持项：**

**1.ORDER BY的限制**

  针对全局表没限制；针对分片表，排序字段不超过8个列，ORDER BY需要使用列名或者别名，目前暂且不支持使用数字，ORDER BY目前不支持字段为枚举类型的排序。

**2.DISTINCT的限制**

  针对全局表没有限制；针对分片表，仅支持DISCTINCT字段同时也是ORDER BY字段，例如：select distinct col1 from tab1 order by col1，另外为了在使用上更加友好，对于order by未写全的，Cetus会进行补充，例如：selectdistinct col1,col2 from tab1 order by col1，Cetus会改写为 select distinct col1,col2 fromtab1 order by col1,col2，但如果写成select distinct * from tab1，Cetus则会返回错误，为了效率，提倡使用标准写法，以免造成不必要的资源开销。

**3.CASE WHEN/IF 的限制**

  全局表没有限制；针对分片表，不能用于DML语句中，也不能用在GROUP BY后，可以用于SELECT 后，也可以作为过滤条件。

**4.分页查询的限制**

  由于对分片的支持，我们带来的新限制。在结果集大于特定值时分页时，由于性能开销较大，可能无法返回准确值。

**5.JOIN的使用限制**

  不支持跨库的JOIN，非分片表可以在每个分片中都保存一份，以提高join的使用成功率。

**6.Where条件的限制**

   当Where条件中有分区列时，值不能有函数转换，也不能有算术表达式，必须是原子值，否则处理结果会不准确或者强制走全库查询，增加后端数据库的负担。

   目前支持有限的子查询类型以及有限的操作类型：支持子查询作为查询条件使用；支持子查询作为数据源使用。

**7.查询业务的限制**

   在做SQL查询时，应注意以下约束：只支持同一个 VDB 内的关联查询；针对 sharding 表，在查询条件中可以使用 sharding key 的要求加上该过滤条件，
   另外，使用 sharding key 时，不建议使用带有函数转换、算术表达式等逻辑处理, 会严重影响效率。

**8.PREPARE的限制**

  不支持服务器端 PREPARE,可以用客户端的 PREPARE 代替。

**9.中文列名的限制**

  对表列的中文列名或别名的使用有限制，使用中文列名或中文别名时必须加引号｀｀。

### 12.事务处理限制

跨库事务的有限支持，针对同一分区键分布的事务，我们默认通过分布式事务方式执行，如果需要考虑性能，可以考虑在所有数据操作都在同一分区时，手动通过注释走单机事务提交。 

不跨分区的事务需要在第一条语句中引用分区键，方便Cetus进行SQL转发和路由，并使用单机事务提升效率。

在分布式事务里面，要求用户尽量不要嵌入 select，因为 select 是会加锁的，会导致性能非常差。

能用单事务就不要走分布式事务，所谓单事务就是此事务确定只会根据分片键定位到一个后端节点。

DML语句的限制：Update/Delete 支持子查询，但子查询中不要有嵌套（仅针对分片表）；不支持对 sharding key 列进行 update（仅针对分片表）；Insert 使用时要写全列名，例如：insert into a(col1,col2) values(xx,xx)；Insert 不支持子查询,如有特殊业务需要用到,可以使用注释（仅针对分片表，详见注释功能）；Insert 支持多 value 语句，例如：insert into a(col1,col2) values(x,x),(xx,xx)；支持 replace into/insert on duplicate key 语法。


### 13.分区键的类型

用于分区的列，可以是“int”或“char”类型，“int”对应到MySQL中各种整数类型，“char”对应到各种定长和变长字符串类型，日期类型在SQL中按字符串处理的话可以支持。 后续会支持特定格式的字符型表示的时间类型,如“YYYY－MM－DD” 和 “YYYY－MM－DD HH24:MI:SS” 格式，时间格式不支持针对时区进行转换，统一使用本地时间。

### 14.分区相关注释

Cetus提供注释功能，用以解决日常维护时的需求（DBA同学经常使用）和一些针对前端业务的特殊需求（例如，强制该SQL只通过主库或者从库进行操作）。

注释书写样式 /\*# key=value \*/。

其中，以“/\*#”号开头（“/\*” 与“#”之间不允许有空格），“\*/”结尾， 中间以键值对形式书写，如果value包含[a-zA-Z0-9_-.]以外的其它特殊字符，需加双引号 。Key/value的值大小写均可，建议统一小写。

Sharding版支持的key类型：table|group|mode|transaction，支持的value包括all/readwrite/readonly/single_node。

**注：若使用注释请在连接Cetus时加上-c参数，如 mysql --prompt="proxy> " --comments -hxxx.xxx.xxx.xxx -Pxxxx -uxxxx -pxxx -c**

注释功能使用示例如下：

**1.Key类型为table的用法**

  用法：/\*#table=employee\*/

  SQL: select /\*# table=employee key=123\*/emp_no,emp_name from employee;

  说明：将SQL路由到key=123所在的分片上执行。

**2.Key类型为group的用法**

  用法：/\*# group=dataA\*/

  SQL: select /\*# group=dataA \*/ count(\*) from employee;

  说明：查询后端节点dataA中，表employee的记录数。

**3.Key类型为mode的用法**

  用法：/\*# mode=readwrite \*/

  SQL: select /\*# mode=readwrite \*/ count(\*) fromemployee;

  说明：此查询语句强制选择主库执行，查询操作默认选择从库执行。

**4.Key类型为transaction的用法**

  用法：/\*# transaction=single_node \*/

  SQL: update /\*# transaction=single_node \*/ departmentsset dept_name='ecbj' where dept_no='d010';

  说明：此dml语句将强制采用非分布式事务，一旦Cetus在执行时判断应该采用分布式事务，会返回错误。

**5.复合用法**

  用法：/\*# table=employee key=123\*/ /\*#mode=readwrite\*/

  SQL: select /\*# table=employee key=123\*/ /\*#mode=readwrite\*/ emp_no,emp_name from employee;

  说明：将SQL路由到key=123所在的分片上执行，且强制从该分片的主库读取。

  注意：table、group这两个key是互斥的，即table或者group分别可以和mode/transaction共用，但它俩不能同时出现，否则会返回错误。另外，请将注释部分写到第一个关键字之后。注释一旦使用，其优先级高于后续where条件（如果有的话）中的分区路由信息。

## Cetus应用示例

### 1.连接Cetus

```
    $ mysql --prompt="proxy> " --comments -h**.**.**.** -P**** -u**** -p***
    proxy> 
```

在连接Cetus时，使用在配置文件中确认好的用户名和密码登陆，登陆的ip和端口为Cetus监听的ip和端口。

可同时启动监听同一个ip不同端口的Cetus。

### 2.管理Cetus

```
   $  mysql --prompt="admin> " --comments -h**.**.**.** -P**** -u**** -p***
   admin>  show connectionist；
```

可以使用在配置文件中的admin用户名和密码，登陆地址为admin-address的MySQL对Cetus进行管理，例如在查询Cetus的连接详细信息时，可以登录后通过命令 show connectionlist，显示从前端到后端端口映射关系，以及涉及到的 xa 信息。

具体使用说明详见[Cetus 分库(sharding)版管理手册](https://github.com/Lede-Inc/cetus/blob/master/doc/cetus-shard-admin.md)

### 3.查看Cetus参数

```
   $  cd /home/user/cetus_install/
   bin/cetus --help | -h
```

可以查看帮助，获取Cetus启动配置选项的参数及含义。
