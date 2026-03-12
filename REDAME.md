# 用于学习linux下的服务器
## C++ 线程池 定时器 epoll 日志 Reactor Mysql(访问服务器数据库实现web端用户注册、登录功能)
## 使用webbench 测试 服务器 性能
### webbench -c 100 -t 30 ip:port
### Speed=61284 pages/min, 227772 bytes/sec.
### Requests: 30642 susceed, 0 failed.  qps(服务器每秒能处理多少个请求) = 61284/60 = 1021.4 

## 新增功能说明（长连接 + POST）
### 1) 长连接（Keep-Alive）
- HTTP/1.1 默认保持连接，若请求头里带 `Connection: close` 则主动关闭。
- HTTP/1.0 默认关闭，若请求头里带 `Connection: keep-alive` 则保持。
- 服务器在响应头里回写 `Connection: keep-alive/close`。

### 2) POST 请求
- 新增 `/post` 路由，支持 `POST`，返回 `text/plain`。
- 响应内容为：`POST OK` + 原始请求体。
- 其余路径的 `POST` 返回 404。

### 3) 简单测试方法（curl）
```bash
# GET（默认长连接）
curl -v http://127.0.0.1:PORT/

# GET（手动关闭）
curl -v -H "Connection: close" http://127.0.0.1:PORT/hello

# POST（返回 body）
curl -v -X POST http://127.0.0.1:PORT/post -d "hello=world"
```
