# CppWebServer 中 `epoll` / `socket` / `MySQL` 函数总览

本文档整理了本项目里实际用到的系统调用和 MySQL C API，帮助你快速理解每个函数“是什么、做什么、在项目哪里用、注意什么”。

## 1. epoll 相关函数

### 1.0 `epoll_event` 结构体（先理解它）
- 常见定义（简化）：
```c
struct epoll_event {
    uint32_t events;   // 事件位掩码，如 EPOLLIN / EPOLLET
    epoll_data_t data; // 用户数据，项目里主要用 data.fd
};
```
- 在本项目中：
  - 监听 fd 和连接 fd 都是通过 `event.data.fd = xxx` 绑定到 epoll 的。
  - 事件循环里通过 `events[i].data.fd` 取回触发的 fd。
- 怎么用：
  - 注册监听 fd：
```c
epoll_event ev;
ev.events = listen_epoll_events(); // LT 或 ET
ev.data.fd = server_fd_;
epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev);
```
  - 注册连接 fd：
```c
epoll_event client_ev;
client_ev.events = conn_epoll_events(); // LT 或 ET
client_ev.data.fd = client_fd;
epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &client_ev);
```

### 1.1 `epoll_create1`
- 常见原型：
```c
int epoll_create1(int flags);
```
- 作用：
  - 创建一个 epoll 实例，返回 epoll 文件描述符。
- 本项目中：
  - 在服务启动初始化时创建 `epfd_`。
  - 位置：`src/server.cpp` 的 `Server::init()`。
- 注意点：
  - 返回 `-1` 表示失败，需要检查 `errno`。
  - `epfd_` 也要在析构时 `close`。

### 1.2 `epoll_ctl`
- 常见原型：
```c
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
```
- 作用：
  - 向 epoll 实例中添加、修改、删除被监听的 fd。
  - `op` 常见值：`EPOLL_CTL_ADD`、`EPOLL_CTL_MOD`、`EPOLL_CTL_DEL`。
- 本项目中：
  - `ADD`：把 `listenfd` 和 `connfd` 加入 epoll。
  - `DEL`：连接处理前先从 epoll 删除，避免重复触发；关闭连接前也会删除。
  - 位置：`src/server.cpp`（`add_conn_fd_to_epoll`、`run`、超时检查）。
- 注意点：
  - `DEL` 时 `event` 可以传 `nullptr`。
  - 忘记 `DEL` 或重复 `ADD` 可能导致事件混乱。

### 1.3 `epoll_wait`
- 常见原型：
```c
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
```
- 作用：
  - 阻塞等待事件就绪，返回就绪事件数量。
- 本项目中：
  - 主事件循环反复调用 `epoll_wait`，然后遍历 `events[]`。
  - 位置：`src/server.cpp` 的 `Server::run()`。
- 注意点：
  - 返回 `-1` 且 `errno == EINTR` 时通常继续循环。
  - `timeout` 毫秒单位，本项目用了 `10000`。

### 1.4 `EPOLLIN` / `EPOLLET`（事件标志）
- 作用：
  - `EPOLLIN`：可读事件。
  - `EPOLLET`：边缘触发（ET），不带则是水平触发（LT）。
- 本项目中：
  - 通过 `-m` 参数选择 listenfd/connfd 的 LT/ET 组合。
  - 位置：`listen_epoll_events()`、`conn_epoll_events()`。
- 注意点：
  - ET 常要求一次读/accept 到 `EAGAIN`，否则可能漏事件。

#### 1.4.1 事件位到底怎么写（本项目可直接对照）
- LT（水平触发）：
```c
ev.events = EPOLLIN;
```
- ET（边缘触发）：
```c
ev.events = EPOLLIN | EPOLLET;
```

#### 1.4.2 你项目里 `-m` 参数与事件位映射
- `-m 0`：`listenfd=LT`, `connfd=LT`
  - `listen_ev = EPOLLIN`
  - `conn_ev   = EPOLLIN`
- `-m 1`：`listenfd=LT`, `connfd=ET`
  - `listen_ev = EPOLLIN`
  - `conn_ev   = EPOLLIN | EPOLLET`
- `-m 2`：`listenfd=ET`, `connfd=LT`
  - `listen_ev = EPOLLIN | EPOLLET`
  - `conn_ev   = EPOLLIN`
- `-m 3`：`listenfd=ET`, `connfd=ET`
  - `listen_ev = EPOLLIN | EPOLLET`
  - `conn_ev   = EPOLLIN | EPOLLET`

#### 1.4.3 LT 和 ET 在“怎么写代码”上的核心差异
- `listenfd`：
  - LT：一次事件里 `accept` 一个也可以，剩余连接下轮还会通知。
  - ET：必须 `while(accept)` 循环直到 `EAGAIN/EWOULDBLOCK`，否则可能漏连接。
- `connfd`：
  - LT：读不完问题相对小，下轮还会再通知。
  - ET：通常要“尽可能一次读完”（读到 `EAGAIN`），否则可能拿不到下一次通知。

#### 1.4.4 本项目里 epoll 的实际使用流程（非常关键）
1. 启动时：
   - `epoll_create1`
   - `epoll_ctl(ADD, listenfd, listen_event)`
2. 有新连接时：
   - `accept` 得到 `client_fd`
   - `epoll_ctl(ADD, client_fd, conn_event)`
3. `connfd` 可读时：
   - **先 `epoll_ctl(DEL, client_fd)`**
   - 再把任务交给线程池（Reactor 或模拟 Proactor 分支）
4. 响应完成后：
   - 若 keep-alive：`epoll_ctl(ADD, client_fd, conn_event)` 重新挂回 epoll
   - 否则：`close(client_fd)`

#### 1.4.5 为什么这里要先 `DEL` 再交线程池
- 目的：避免同一个 `client_fd` 在任务尚未处理完时再次被 `epoll_wait` 命中，导致并发重复处理同一连接。
- 这个策略在多线程服务器中很常见，能显著降低竞态问题。

#### 1.4.6 常见坑（你面试也经常会被问）
- ET 模式忘记循环 `accept/recv` 到 `EAGAIN`。
- fd 已经 `close` 了但没从连接状态表里移除。
- 重复 `ADD` 同一个 fd（应该 `MOD` 或先 `DEL`）。
- 忘记处理 `epoll_wait` 被信号中断（`errno == EINTR`）。

---

## 2. socket 与网络相关函数

### 2.1 `socket`
- 原型：
```c
int socket(int domain, int type, int protocol);
```
- 作用：
  - 创建套接字，监听端一般是 `AF_INET + SOCK_STREAM`（TCP）。
- 本项目中：
  - 创建 `server_fd_`。
  - 位置：`Server::init()`。

### 2.2 `setsockopt`
- 原型：
```c
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
```
- 作用：
  - 设置套接字选项。
- 本项目中：
  - 设置 `SO_REUSEADDR`，减少重启时端口占用问题。
  - 位置：`Server::init()`。

### 2.3 `bind`
- 原型：
```c
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```
- 作用：
  - 把 socket 绑定到 IP + 端口。
- 本项目中：
  - 绑定 `INADDR_ANY:port_`。
  - 位置：`Server::init()`。

### 2.4 `listen`
- 原型：
```c
int listen(int sockfd, int backlog);
```
- 作用：
  - 把 socket 变成监听 socket，等待客户端连接。
- 本项目中：
  - `backlog` 设置为 `10`。
  - 位置：`Server::init()`。

### 2.5 `accept`
- 原型：
```c
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
```
- 作用：
  - 从监听队列取出一个连接，得到新的 `client_fd`。
- 本项目中：
  - 在 `run()` 中处理新连接。
  - ET/LT 下行为略有不同：ET 会循环 accept 到 `EAGAIN`。
- 注意点：
  - 失败时要区分 `EAGAIN/EWOULDBLOCK` 和真实错误。

### 2.6 `recv`
- 原型：
```c
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
```
- 作用：
  - 从 socket 读取请求数据。
- 本项目中：
  - 在 `read_http_request()` 循环读取，直到拿到完整 HTTP 请求。
- 注意点：
  - 返回 `0` 表示对端关闭连接。
  - 返回 `<0` 要检查 `errno`。

### 2.7 `send`
- 原型：
```c
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
```
- 作用：
  - 向客户端发送响应。
- 本项目中：
  - `send_all()` 封装为循环发送，避免一次 `send` 发不完。
- 注意点：
  - TCP 发送可能短写，必须处理“部分发送”。

### 2.8 `close`
- 原型：
```c
int close(int fd);
```
- 作用：
  - 关闭文件描述符（socket、epoll fd 都是 fd）。
- 本项目中：
  - 连接结束、异常、超时、析构时都会关闭对应 fd。

### 2.9 `fcntl`
- 原型：
```c
int fcntl(int fd, int cmd, ...);
```
- 作用：
  - 修改 fd 属性，尤其是阻塞/非阻塞模式。
- 本项目中：
  - `set_nonblocking()`：`O_NONBLOCK`
  - `set_blocking()`：清除 `O_NONBLOCK`
- 注意点：
  - ET + 非阻塞是常见组合；本项目为读完整请求会临时改为阻塞。

### 2.10 `htons` / `ntohs`
- 作用：
  - 主机字节序与网络字节序转换（16 位端口号）。
- 本项目中：
  - `htons(port_)` 绑定端口。
  - `ntohs(client_addr.sin_port)` 打印客户端端口。

### 2.11 `inet_ntop`
- 原型：
```c
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
```
- 作用：
  - 把二进制 IP 转成人类可读字符串。
- 本项目中：
  - 接收新连接后打印客户端 IP。

---

## 3. MySQL C API 函数

> 本项目 MySQL 逻辑主要在 `src/server.cpp` 和 `src/db/mysql_user_store.cpp`。

### 3.1 `mysql_init`
- 原型：
```c
MYSQL *mysql_init(MYSQL *mysql);
```
- 作用：
  - 初始化 MySQL 连接句柄。
- 本项目中：
  - 每次建立数据库连接前调用。

### 3.2 `mysql_real_connect`
- 原型：
```c
MYSQL *mysql_real_connect(
    MYSQL *mysql,
    const char *host,
    const char *user,
    const char *passwd,
    const char *db,
    unsigned int port,
    const char *unix_socket,
    unsigned long clientflag
);
```
- 作用：
  - 真正连接 MySQL 服务器。
- 本项目中：
  - 初始化建库建表时连接一次。
  - 注册/登录时按需连接数据库。
- 注意点：
  - 失败时用 `mysql_error` 看具体原因。

### 3.3 `mysql_query`
- 原型：
```c
int mysql_query(MYSQL *mysql, const char *stmt_str);
```
- 作用：
  - 执行 SQL 字符串。
- 本项目中：
  - `SET NAMES utf8mb4`
  - `CREATE DATABASE`
  - `CREATE TABLE`
  - `INSERT`
  - `SELECT`

### 3.4 `mysql_select_db`
- 原型：
```c
int mysql_select_db(MYSQL *mysql, const char *db);
```
- 作用：
  - 切换当前数据库。
- 本项目中：
  - 创建数据库后切到 `mydb` 再建表。

### 3.5 `mysql_real_escape_string`
- 原型：
```c
unsigned long mysql_real_escape_string(
    MYSQL *mysql,
    char *to,
    const char *from,
    unsigned long length
);
```
- 作用：
  - 对用户输入做转义，降低 SQL 注入风险。
- 本项目中：
  - 注册和登录查询前对 `username/password` 转义。
- 注意点：
  - 更推荐预处理语句（prepared statement），本项目是学习版做法。

### 3.6 `mysql_store_result`
- 原型：
```c
MYSQL_RES *mysql_store_result(MYSQL *mysql);
```
- 作用：
  - 把查询结果集读取到客户端内存。
- 本项目中：
  - 登录校验 `SELECT password_hash ...` 后获取结果集。

### 3.7 `mysql_fetch_row`
- 原型：
```c
MYSQL_ROW mysql_fetch_row(MYSQL_RES *result);
```
- 作用：
  - 从结果集中逐行取数据。
- 本项目中：
  - 读取第一行密码字段，判断用户是否存在。

### 3.8 `mysql_free_result`
- 原型：
```c
void mysql_free_result(MYSQL_RES *result);
```
- 作用：
  - 释放结果集内存。
- 本项目中：
  - 查询完成后立即释放。

### 3.9 `mysql_errno` / `mysql_error`
- 原型：
```c
unsigned int mysql_errno(MYSQL *mysql);
const char *mysql_error(MYSQL *mysql);
```
- 作用：
  - 获取错误码 / 错误文本。
- 本项目中：
  - 插入用户时通过 `mysql_errno == 1062` 判断“用户名已存在”。
  - 其它地方统一打印数据库错误信息。

### 3.10 `mysql_close`
- 原型：
```c
void mysql_close(MYSQL *sock);
```
- 作用：
  - 关闭 MySQL 连接并释放连接资源。
- 本项目中：
  - 各种成功/失败分支都会关闭连接，避免泄漏。

---

## 4. 编译链接相关（MySQL）

- 头文件：`#include <mysql/mysql.h>`
- 链接库：`libmysqlclient`
- 在 `CMakeLists.txt` 中通过 `find_library(MYSQLCLIENT_LIB mysqlclient)` 并 `target_link_libraries(server PRIVATE ${MYSQLCLIENT_LIB})`。

---

## 5. 一眼看懂：请求处理链路中这些函数怎么串起来

1. 启动阶段：
   - `socket -> setsockopt -> bind -> listen -> epoll_create1 -> epoll_ctl(ADD listenfd)`
2. 连接到来：
   - `epoll_wait` 返回 listenfd 可读
   - `accept` 得到 connfd
   - `fcntl(O_NONBLOCK)` + `epoll_ctl(ADD connfd)`
3. 请求处理：
   - `epoll_wait` 返回 connfd 可读
   - `recv` 读取 HTTP 请求
   - 若是注册/登录，调用 MySQL：`mysql_init -> mysql_real_connect -> mysql_query...`
   - `send` 回响应
4. 连接结束或超时：
   - `epoll_ctl(DEL)` + `close`
   - 数据库连接使用后 `mysql_close`

---

如果你愿意，我可以下一步再给你补一份“这 3 类函数的常见错误码速查表（比如 `EAGAIN`、MySQL 1062）”放在同一个文档后面。
