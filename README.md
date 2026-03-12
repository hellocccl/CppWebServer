# 用于学习 Linux 下的 C++ Web 服务器
## 技术栈：线程池 + epoll（Reactor/模拟Proactor + LT/ET 可选） + 定时器 + 日志（同步/异步） + MySQL 用户注册/登录

## 已实现功能
### 1) 长连接（Keep-Alive）
- HTTP/1.1 默认保持连接，若请求头里带 `Connection: close` 则关闭。
- HTTP/1.0 默认关闭，若请求头里带 `Connection: keep-alive` 则保持。

### 2) POST 请求
- `/post`：回显请求体，便于调试 POST 是否通。
- `/register`：将用户写入 MySQL 的 `users` 表。
- `/login`：从 MySQL 校验用户名和密码。

### 3) 静态文件服务（支持文本和图片）
- GET 路径会映射到 `www` 目录（并阻止 `..` 目录穿越）。
- 示例：`/sample.txt`、`/demo.svg`、`/index.html`。
- 已按后缀返回常见 MIME：`text/plain`、`image/png`、`image/jpeg`、`image/svg+xml` 等。

### 4) 日志写入模式可选（同步/异步）
- 支持启动参数 `-l LOGWrite`：
  - `0`：同步写入（默认）
  - `1`：异步写入（后台线程写日志文件）

### 5) 静态视频访问（mp4）
- `www` 目录下若存在 `xxx.mp4`，可直接通过 `GET /xxx.mp4` 访问。
- 服务器会返回 `Content-Type: video/mp4`，浏览器可按视频资源处理。

### 6) 并发模型可选（Reactor / 模拟 Proactor）
- 支持启动参数 `-a actor_model`：
  - `0`：模拟 Proactor（默认）
  - `1`：Reactor

### 7) 触发模式组合可选（LT/ET）
- 支持启动参数 `-m trig_mode`：
  - `0`：listenfd = LT, connfd = LT（默认）
  - `1`：listenfd = LT, connfd = ET
  - `2`：listenfd = ET, connfd = LT
  - `3`：listenfd = ET, connfd = ET

## MySQL 配置
当前代码固定使用以下配置（位于 `src/server.cpp` 顶部常量）：
```cpp
const char* kDbUser = "root";
const char* kDbPassword = "123456789";
const char* kDbName = "mydb";
```
默认连接地址：`127.0.0.1:3306`。

服务器启动时会自动执行：
1. `CREATE DATABASE IF NOT EXISTS mydb`
2. `CREATE TABLE IF NOT EXISTS users (...)`

`users` 表结构：
```sql
CREATE TABLE users (
  id INT PRIMARY KEY AUTO_INCREMENT,
  username VARCHAR(64) NOT NULL UNIQUE,
  passwd VARCHAR(128) NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

## Linux 依赖安装
```bash
sudo apt update
sudo apt install -y build-essential cmake libmysqlclient-dev mysql-server
```

## 编译与运行
```bash
cd CppWebServer
mkdir -p build
cd build
cmake ..
make -j
```

### 启动命令（日志 + 模型 + 触发模式）
```bash
# 默认：同步日志 + 模拟Proactor + LT+LT
./server

# 同步日志 + Reactor + ET+ET
./server -l 0 -a 1 -m 3

# 异步日志 + 模拟Proactor + LT+ET
./server -l 1 -a 0 -m 1

# 异步日志 + Reactor + ET+LT
./server -l 1 -a 1 -m 2

# 仅设置日志模式（显式同步）
./server -l 0

# 仅设置日志模式（异步）
./server -l 1
```

参数说明：
```text
./server [-l LOGWrite] [-a actor_model] [-m trig_mode]
LOGWrite:
  0 -> 同步写入（默认）
  1 -> 异步写入
actor_model:
  0 -> 模拟 Proactor（默认）
  1 -> Reactor
trig_mode:
  0 -> LT + LT（默认）
  1 -> LT + ET
  2 -> ET + LT
  3 -> ET + ET
```

## Reactor / Proactor / LT / ET 说明（结合本项目）
### 1) 当前项目里的 Reactor（`-a 1`）
- 主线程 `epoll_wait` 只负责分发事件。
- 连接可读后，主线程把任务丢给线程池。
- 工作线程负责：`recv` 读请求 + 业务处理 + `send` 回包。

### 2) 当前项目里的模拟 Proactor（`-a 0`）
- 主线程 `epoll_wait` 后，先把请求读出来（本项目是同步读，属于“模拟 Proactor”）。
- 然后把“已读完请求”交给线程池做业务处理与回包。
- 区别在于“谁负责读 socket”：Reactor 是工作线程读，模拟 Proactor 是主线程先读。

### 3) LT 与 ET 差异
- LT（水平触发）：只要缓冲区还有数据，`epoll_wait` 会持续通知，编程相对简单。
- ET（边缘触发）：状态从“无数据”变“有数据”时才通知一次，通常要求一次读到 `EAGAIN`，性能潜力更高但更容易写错。

### 4) 本项目中 listenfd 的 LT/ET 处理
- `listenfd=LT`：一次事件默认处理一个 `accept`，剩余连接下轮仍会通知。
- `listenfd=ET`：循环 `accept` 到 `EAGAIN`，避免漏掉连接。

### 5) 本项目中 connfd 的 LT/ET 处理
- `connfd=LT`：事件位使用 `EPOLLIN`。
- `connfd=ET`：事件位使用 `EPOLLIN | EPOLLET`。
- 连接保持时重新加入 epoll，会按照 `-m` 选择的 conn 模式注册。

## 页面与接口
### 页面
- `GET /`：首页，包含注册/登录表单和静态资源链接
- `GET /hello`
- `GET /post.html`
- `GET /sample.txt`
- `GET /demo.svg`
- `GET /xxx.mp4`（当 `www/xxx.mp4` 存在时）

### 接口（表单 `application/x-www-form-urlencoded`）
- `POST /register`
  - 参数：`username`、`password`
  - 成功：`200 OK`
  - 用户已存在：`409 Conflict`

- `POST /login`
  - 参数：`username`、`password`
  - 成功：`200 OK`
  - 用户不存在或密码错误：`401 Unauthorized`

## curl 快速测试
```bash
# 1) 注册
curl -v -X POST http://127.0.0.1:8080/register \
  -d "username=test1&password=123456"

# 2) 登录成功
curl -v -X POST http://127.0.0.1:8080/login \
  -d "username=test1&password=123456"

# 3) 登录失败（密码错误）
curl -v -X POST http://127.0.0.1:8080/login \
  -d "username=test1&password=wrong"

# 4) 文本文件
curl -v http://127.0.0.1:8080/sample.txt

# 5) 图片文件
curl -v http://127.0.0.1:8080/demo.svg

# 6) 视频文件（先确保 www/xxx.mp4 存在）
curl -I http://127.0.0.1:8080/xxx.mp4
curl -v http://127.0.0.1:8080/xxx.mp4 -o /tmp/xxx.mp4
```

浏览器测试：
```text
http://127.0.0.1:8080/xxx.mp4
```

## 本次最小改动清单
- `include/logger.h`
  - 新增日志写入模式 `write_mode_`。
  - 新增异步日志队列、条件变量、后台线程字段。
  - `init` 改为支持 `init(filename, write_mode)`。
- `src/logger.cpp`
  - 新增同步/异步双模式日志写入逻辑。
  - 新增异步消费者线程 `async_write_loop()`。
  - 新增安全停止逻辑 `stop_async_worker()` 与析构回收线程。
- `src/main.cpp`
  - 新增 `-l LOGWrite` 参数解析。
  - 新增 `-a actor_model` 参数解析（Reactor/模拟Proactor）。
  - 新增 `-m trig_mode` 参数解析（LT/ET 组合）。
  - 启动时按参数选择同步或异步日志。
- `include/server.h`
  - 新增数据库初始化、注册/登录校验、静态路径解析、MIME 判断等函数声明。
  - 新增并发模型与触发模式字段及辅助函数声明。
- `src/server.cpp`
  - 接入 MySQL C API。
  - 新增 `/register`、`/login` 路由。
  - 扩展 GET 为通用静态文件服务（文本/图片）。
  - 增加 URL 解码、表单解析、路径安全检查、完整发送函数。
  - 增加 `.mp4 -> video/mp4` 的 MIME 映射。
  - 新增 `listenfd/connfd` 的 LT/ET 事件注册逻辑。
  - 新增 Reactor / 模拟Proactor 两套事件处理分支。
- `CMakeLists.txt`
  - Linux 下新增 `mysqlclient` 链接检查。
- `www/index.html`
  - 增加注册/登录表单与文本/图片访问入口。
- `www/sample.txt`、`www/demo.svg`
  - 新增文本和图片示例资源。

## 注意
当前密码以明文存储，仅用于学习。生产环境应使用安全哈希（如 bcrypt/argon2）并启用 HTTPS。
