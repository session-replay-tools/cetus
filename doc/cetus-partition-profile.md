# 分表(partition)版配置文件说明

## 分表(partition)版配置文件基本跟分库一样，不同之处在shard.conf文件：
### 1、配置partition-mode=true, 使cetus工作在分表模式
### 2、proxy-backend-addresses和proxy-read-only-backend-addresses配置具体参考读写分离，不需要像分库那样加group


