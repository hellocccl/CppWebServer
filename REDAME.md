# 用于学习 Linux 下的 C++ Web 服务器
## 技术栈：线程池 + epoll + 定时器 + 日志 + MySQL 用户注册/登录

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
./server
```

## 页面与接口
### 页面
- `GET /`：首页，包含注册/登录表单和静态资源链接
- `GET /hello`
- `GET /post.html`
- `GET /sample.txt`
- `GET /demo.svg`

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
```

## 本次最小改动清单
- `include/server.h`
  - 新增数据库初始化、注册/登录校验、静态路径解析、MIME 判断等函数声明。
- `src/server.cpp`
  - 接入 MySQL C API。
  - 新增 `/register`、`/login` 路由。
  - 扩展 GET 为通用静态文件服务（文本/图片）。
  - 增加 URL 解码、表单解析、路径安全检查、完整发送函数。
- `CMakeLists.txt`
  - Linux 下新增 `mysqlclient` 链接检查。
- `www/index.html`
  - 增加注册/登录表单与文本/图片访问入口。
- `www/sample.txt`、`www/demo.svg`
  - 新增文本和图片示例资源。

## 注意
当前密码以明文存储，仅用于学习。生产环境应使用安全哈希（如 bcrypt/argon2）并启用 HTTPS。
