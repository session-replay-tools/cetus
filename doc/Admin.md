# Admin手册

## 写在前面

**有配置修改均能动态生效，配置更改后请务必修改原始配置文件，以确保下次重启时配置能够保留。**

## 后端配置

### 查看后端

`SELECT * FROM backends`

查看后端信息。

| backend_ndx | address             | state | type | uuid | connected_clients |
|------------:|---------------------|-------|------|------|------------------:|
|           1 | 10.120.240.252:3309 | up    | rw   | NULL |                 0 |
|           2 | 10.120.240.254:3310 | up    | ro   | NULL |                20 |

结果说明：

* backend_ndx: 后端序号，按照添加顺序排列；
* address: 后端地址，IP:PORT格式；
* state: 后端状态(unknown|up|down|maintaining|delete)；
* type: 读写类型(rw|ro)；
* uuid: 暂时无用；
* connected_clients: 客户查看后端连接状态端连接的数量(正在处理请求的连接)。

```
状态说明

unknown:     后端初始状态，还未建立连接;
up:          能与后端正常建立连接；
down:        与后端无法联通(如果开启后端状态检测，能连通后自动变为UP);
maintaining: 后端正在维护，无法建立连接或自动切换状态(此状态由管理员手动设置);
delete:      后端已被删除，无法再建立连接。
```

### 查看后端连接状态

`SELECT CONN_DETAILS FROM backends`

查看每个用户占用和空闲的后端连接数。

| backend_ndx | username      | idle_conns | total_used_conns |
|------------:|---------------|-----------:|-----------------:|
|           1 | lott_quanzi   |          2 |                0 |
|           1 | zjhpettest    |         11 |                0 |

结果说明：

* backend_ndx: 后端序号；
* username: 用户名；
* idle_conns: 空闲连接数；
* total_used_conns: 正在使用的连接数。

### 添加后端

`ADD MASTER [ip:port]`

添加一个读写类型的后端。

>ADD MASTER 127.0.0.1:3307

`ADD SLAVE [ip:port]`

添加一个只读类型的后端。

>ADD SLAVE 127.0.0.1:3360

`INSERT INTO backends VALUES ("ip:port", "ro|rw")`

添加一个后端，同时指定读写类型。

>INSERT INTO backends values ("127.0.0.1:3306", "rw");

### 删除后端

`REMOVE BACKEND [backend_ndx]` 或
`DELETE FROM BACKENDS where backend_ndx = [backend_ndx]`

删除一个指定序号的后端。

>remove backend 1

`DELETE FROM BACKENDS where address = '[IP:PORT]'`

删除一个指定地址的后端。

>delete from backends where address = '127.0.0.1:3306'

### 修改后端

`UPDATE BACKENDS set type|state=[value] where address|backend_ndx=[value]`

修改后端类型或状态。

>update backends set type="rw" where address="127.0.0.1:3306"

>update backends set state="up" where backend_ndx=1

## 基本配置

### 获取基本配置

`config get common`

能获取的配置包括:

* check_slave_delay: 是否启用从库延迟检查（0为不检查）
* slave_delay_down: 从库延迟大于该时间时，设置从库状态为DOWN（单位：秒）
* slave_delay_recover: 从库延迟小于等于该时间时，设置从库状态为UP（单位：秒）

### 修改基本配置

`config set common.[option] = [value]`

例如

>config set common.slave_delay_down = 3

## 连接池配置

### 获取连接池配置

`config get pool`

能获取的配置包括：

* max_pool_size: 连接池最大连接数，超出后将不再维持超出部分的连接
* default_pool_size: 默认连接数量，连接数达到此值之前会一直新建连接
* master_preferred: 设置为1时仅访问主库(读写)
* max_resp_len: 允许每个后端返回数据的最大值

### 修改连接池配置

`config set pool.[option] = [value]`

修改某项配置

>config set pool.max_pool_size = 200

## 查看连接信息

### 查看当前连接的详细信息

`SHOW CONNECTIONLIST`

将当前全部连接的详细内容按表格显示出来。

| User          | Host                 | Server | db               | Command | Time | Trans | State      | Info |
|---------------|----------------------|--------|------------------|---------|------|-------|------------|------|
| ddz_develop   | 10.120.241.198:53728 | NULL   | ddz_tower_test   | Sleep   | 0    | N     | READ_QUERY | NULL |
| mail          | 10.120.241.183:37960 | NULL   | tech_mail_new_db | Sleep   | 0    | N     | READ_QUERY | NULL |

结果说明：

* User: 用户名;
* Host: 客户端的IP和端口;
* Server: 后端地址;
* db: 数据库名称;
* Command: 执行的sql，"Sleep"代表当前空闲;
* Time: 已执行的时间;
* Trans: 是否在事务中;
* State: 连接当前的状态，"READ_QUERY"代表在等待获取命令;
* Info: 暂未知。

## 用户/密码管理

### 密码查询

`SELECT * FROM user_pwd WHERE user=[username]`

查询某个用户的后端密码。
**注意由于密码是非明文的，仅能显示字节码。**

>select * from user_pwd where user="root";

`SELECT * FROM app_user_pwd WHERE user=[username]`

查询某个用户连接proxy的密码，同样是非明文。

>select * from app_user_pwd where user="test";

### 密码添加/修改

`UPDATE user_pwd SET password=[password1] WHERE user=[username]`

添加或修改特定用户的后端密码(如果该用户不存在则添加，已存在则覆盖)。
**需要Flush config生效**

>update user_pwd set password="123456" where user="test";

>flush config;

`UPDATE app_user_pwd SET password=[password1] WHERE user=[username]`

添加或修改特定用户连接Proxy的密码(如果该用户不存在则添加，已存在则覆盖)。
**需要Flush config生效**

>update app_user_pwd set password="123456" where user="root";

>flush config;

### 密码删除

`DELETE FROM user_pwd where user=[username]`

删除特定用户的后端密码。
**需要Flush config生效**

>delete from user_pwd where user="root"

>flush config;

`DELETE FROM app_user_pwd where user=[username]`

删除特定用户连接Proxy的密码。
**需要Flush config生效**

>delete from app_user_pwd where user="root"

>flush config;

### 查看未提交的修改

`SHOW CHANGES`

查看已修改，但未flush的变更。

### 清除未提交的修改

`CLEAR CHANGES`

清除已修改，但未flush的变更。

## IP许可

### 查看IP许可

`SHOW ALLOW_IP admin|proxy`

查看Admin或者Proxy的IP许可。
若列表为空，则代表没有任何限制。

### 增加IP许可

`ADD ALLOW_IP admin|proxy [[user@]IP]`

增加一个IP许可。(不要加引号)

* Admin: 仅能配置IP，不能限制用户(Admin有效用户只有一个)；
* Proxy: 仅配置IP，代表允许该IP来源所有用户的访问；配置User@IP，代表允许该IP来源的特定用户访问。

>add allow_ip admin 127.0.0.1

>add allow_ip proxy test@127.0.0.1

### 删除IP许可

`DELETE ALLOW_IP admin|proxy [[user@]IP]`

删除一个IP许可。(不要加引号)

>delete allow_ip admin 127.0.0.1

>delete allow_ip proxy test@127.0.0.1

## 远程管理

### 重载分库配置

`reload shard`

需要"remote-config = true"和"disable-threads = false"启动选项。
从远端配置库中重载Shard配置。

### 保存配置到本地文件

`SAVE SETTINGS [FILE]`

保存当前配置到指定路径的本地文件中。

>save settings /tmp/proxy.cnf
