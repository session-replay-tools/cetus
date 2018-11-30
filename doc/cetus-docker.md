# Cetus Docker镜像使用

##  简介

Cetus镜像是关系型数据库中间件Cetus的Docker镜像,该镜像将后端mysql实例、配置库和cetus应用制作成一个镜像，方便用户部署使用。

## 使用方法

### 使用依赖

- docker环境

- mysql客户端

### 使用方法

- 拉取镜像：拉取镜像仓库的镜像，如下：

```
docker pull ledetech/cetus 
```

- 运行：使用docker run命令运行，run命令将容器内的端口以相同的端口号映射到本机，如果本机端口已经被占用，则需要修改本机映射后的端口号，如下：

```
docker run -d -P -it -p port1:port1 -p port2:port2 -p port3:port3 -p port4:port4 -p port5:port5 -p port6:port6 ledetech/cetus
```

- 连接：使用mysql客户端命令与cetus、mysql后端等进行交互，指令如下：

```
mysql -u username -p password -h host -P port
```

## 注意事项

- 用户以root身份运行。

详见docker hub （https://hub.docker.com/r/ledetech/cetus/）
