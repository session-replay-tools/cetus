# Cetus 架构和实现

## 1.整体架构

Cetus 网络架构图如下所示：

![Cetus 架构图](https://github.com/Lede-Inc/cetus/blob/master/doc/picture/Cetus_framwork.png)

Cetus位于应用程序与MySQL数据库之间，作为前端应用与数据库的通讯。其中，前端应用连接LVS节点，LVS节点映射端口到多个Cetus服务，后者通过自身的连接池连接到后端的数据库。

## 2.功能实现

### 1. 功能模块

Cetus 主要的功能模块包括以下五个部分：

1.读写分离

2.分库

3.SQL解析

4.连接池

5.管理功能

功能模块间的交互关系如下：

![Cetus 功能模块图](https://github.com/Lede-Inc/cetus/blob/master/doc/picture/Cetus_module.png)

其中，SQL解析模块为后续读写分离和数据分片等功能解析出SQL类型、表名和查询条件等关键信息；连接池模块是自维护连接池，支持Cetus根据需求查询和检测后端，维护连接数，具有高效连接共享性、事务与Prepare的前后端绑定功能和热点连接重用与连接等待机制；管理功能模块通过用户在管理界面输入，独立认证并转到下一状态，给用户回复状态查询结果或调整参数。


### 2. 工作流程

Cetus 整体工作流程图如下：

![Cetus 工作流程图](https://github.com/Lede-Inc/cetus/blob/master/doc/picture/Cetus_dataflow.png)

其整体工作流程如下所述：

1.Cetus读取启动配置文件和其他配置并启动，监听客户端请求；

2.收到客户端新建连接请求后，Cetus经过用户鉴权和连接池判断连接数是否达到上限，确定是否新建连接；

3.连接建立和认证通过后，Cetus接收客户端发送来的SQL语句，并进行词法和语义分析，对SQL语句进行解析，分析SQL的请求类型，必要时改写SQL，然后选取相应的DB并转发；

4.等待后端处理查询，接收处理查询结果集，进行合并和修改，然后转发给客户端；

5.如收到客户端关闭连接的请求，Cetus判断是否需要关闭后端连接，关闭连接。
