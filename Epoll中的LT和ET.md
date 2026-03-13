# CppWebServer 中 epoll 的完整使用过程

这份文档不讲抽象定义，直接按你这个项目的真实代码走一遍 epoll 的完整生命周期：从服务器启动，到连接接入，到请求处理，再到 keep-alive 重挂和超时关闭。

主要对应文件：
- `src/server.cpp`
- `include/server.h`

---

## 1. 先看清楚：这个项目里 epoll 负责什么

在这个项目中，epoll 的职责不是“处理 HTTP 业务”，而是做三件事：

1. 监听 `listenfd`，发现有没有新客户端连接进来。
2. 监听各个 `connfd`，发现哪个客户端连接上有数据可读。
3. 把“哪个 fd 就绪了”这件事高效通知给服务器主循环。

也就是说：
- epoll 负责“事件通知”
- 线程池负责“具体处理”
- HTTP 解析、静态资源、MySQL 注册/登录都发生在 epoll 通知之后

---

## 2. 项目里和 epoll 配合的核心成员

在 `Server` 类中，下面几个成员是理解 epoll 流程的核心：

```cpp
int server_fd_;      // 监听 socket，也叫 listenfd
int epfd_;           // epoll 实例 fd
std::unordered_map<int, time_t> last_active_; // 记录连接最后活跃时间
```

另外还有两个和 epoll 行为直接相关的模式字段：

```cpp
int actor_model_;      // 0: 模拟 Proactor  1: Reactor
int listen_trig_mode_; // 0: LT 1: ET
int conn_trig_mode_;   // 0: LT 1: ET
```

这几个字段决定了：
- 谁来读 socket
- listenfd 用 LT 还是 ET
- connfd 用 LT 还是 ET

---

## 3. 启动阶段：epoll 使用从哪里开始

入口在 `Server::init()`。

### 3.1 创建监听 socket

先创建 TCP 监听 socket：

```cpp
server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
```

这一步只是拿到一个 socket，还没有开始监听。

### 3.2 设置端口复用

```cpp
setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

目的是避免服务器刚退出又立刻启动时，端口还在 `TIME_WAIT` 导致 `bind` 失败。

### 3.3 把 listenfd 设成非阻塞

```cpp
set_nonblocking(server_fd_);
```

原因：
- epoll 通常配合非阻塞 fd 使用
- 特别是 listenfd 在 ET 模式下，必须非阻塞，否则 `accept` 循环可能卡住

### 3.4 `bind + listen`

```cpp
bind(server_fd_, ...);
listen(server_fd_, 10);
```

到这一步，监听 socket 已经准备好了，但还没有和 epoll 建立关系。

---

## 4. 创建 epoll 实例

接下来创建 epoll：

```cpp
epfd_ = epoll_create1(0);
```

这个 `epfd_` 可以理解成“事件收集器”。

后续所有要监听的 fd，都要通过 `epoll_ctl` 加进这个收集器里。

---

## 5. 第一次注册：把 listenfd 放进 epoll

### 5.1 构造 `epoll_event`

项目里会先构造：

```cpp
epoll_event ev;
ev.events = listen_epoll_events();
ev.data.fd = server_fd_;
```

这里最关键的是 `ev.events`。

### 5.2 `listen_epoll_events()` 决定 LT 还是 ET

这个函数根据启动参数 `-m` 返回：

- LT:
```cpp
EPOLLIN
```

- ET:
```cpp
EPOLLIN | EPOLLET
```

含义：
- `EPOLLIN`：只关心“可读事件”
- `EPOLLET`：把触发方式从 LT 改成 ET

### 5.3 用 `epoll_ctl` 加入 epoll

```cpp
epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev);
```

这一步之后，epoll 就开始监听 `listenfd` 了。

这意味着：
- 有新连接到来时
- `epoll_wait` 就会返回这个 `server_fd_`

---

## 6. 进入主事件循环：`epoll_wait`

启动完成后，服务器进入 `Server::run()` 的无限循环：

```cpp
while (true) {
    int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 10000);
    ...
}
```

这里的含义是：
- 最多一次返回 `MAX_EVENTS` 个就绪事件
- 最长等待 10 秒
- 返回值 `nfds` 是“本轮有多少个 fd 就绪”

### 6.1 `events[]` 里装的是什么

`events` 是一个 `epoll_event` 数组：

```cpp
epoll_event events[MAX_EVENTS];
```

假设本轮有 3 个 fd 就绪，`nfds == 3`，那么：
- `events[0]`
- `events[1]`
- `events[2]`

分别描述了这 3 个就绪事件。

项目通过：

```cpp
int fd = events[i].data.fd;
```

取出具体是哪个 fd 触发了。

### 6.2 为什么 `epoll_wait` 前后要检查超时连接

项目在每轮中会调用：

```cpp
check_timeout_connections();
```

目的不是 epoll 本身，而是配合 keep-alive：
- 长连接会一直保留在 epoll 中
- 如果客户端长时间不发新请求，连接不能永久占着
- 所以每轮循环都检查 `last_active_`
- 超过 30 秒的 fd 会被：

```cpp
epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
close(fd);
```

这一步是“epoll + 长连接”必须配套考虑的部分。

---

## 7. 第一类事件：listenfd 可读，表示有新连接

主循环里首先判断：

```cpp
if (fd == server_fd_) {
    ...
}
```

如果是 `server_fd_` 就绪，说明不是 HTTP 数据来了，而是“有新连接到了监听队列”。

### 7.1 为什么 listenfd 可读等于有新连接

对监听 socket 来说，“可读”不是有普通数据，而是：
- 内核的已完成连接队列里有连接可以 `accept`

所以这时不能 `recv`
而是要 `accept`

### 7.2 LT 和 ET 下 accept 写法为什么不同

项目里用了：

```cpp
bool continue_accept = true;
while (continue_accept) {
    int client_fd = accept(server_fd_, ...);
    ...
}
```

但内部会按模式区分：

- listenfd = LT
  - 本轮 `accept` 一个就可以退出
  - 因为剩下的连接下轮还会继续通知

- listenfd = ET
  - 必须循环 `accept` 到 `EAGAIN/EWOULDBLOCK`
  - 否则剩余连接可能再也得不到通知

这就是 ET 最常见的面试点。

### 7.3 `accept` 成功后会做什么

当 `accept` 成功，拿到新的 `client_fd`，项目会做这些事：

1. 打印客户端 IP 和端口
2. 把 `client_fd` 设为非阻塞
3. 用 `add_conn_fd_to_epoll(client_fd)` 把连接 fd 加入 epoll
4. 在 `last_active_` 中记录活跃时间

代码逻辑可以概括为：

```cpp
client_fd = accept(...);
set_nonblocking(client_fd);
add_conn_fd_to_epoll(client_fd);
last_active_[client_fd] = now;
```

### 7.4 `add_conn_fd_to_epoll()` 做了什么

它内部本质上就是：

```cpp
epoll_event client_ev;
client_ev.events = conn_epoll_events();
client_ev.data.fd = client_fd;
epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &client_ev);
```

也就是说：
- listenfd 用 `listen_epoll_events()`
- connfd 用 `conn_epoll_events()`

这两个事件位可以不同，这就是你项目里 `-m 0~3` 模式组合的来源。

---

## 8. 第二类事件：connfd 可读，表示客户端发来了 HTTP 请求

主循环中的第二个重要分支是：

```cpp
else if (events[i].events & EPOLLIN) {
    ...
}
```

这里只要某个连接 fd 可读，说明客户端发来了请求数据。

但这时项目不会立刻直接处理，而是先做一个关键操作。

### 8.1 为什么先 `EPOLL_CTL_DEL`

项目先执行：

```cpp
epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr);
```

这一步非常重要，原因是：

如果不先删除该 fd，那么：
- 线程池里还在处理这个连接
- 主线程下一轮 `epoll_wait` 可能又拿到同一个 fd
- 结果就是两个线程同时处理同一个连接

这会产生：
- 重复读
- 重复写
- 连接状态错乱
- keep-alive 逻辑被破坏

所以这里的策略是：

1. `connfd` 一旦就绪
2. 先从 epoll 中摘掉
3. 处理完后再决定是否挂回 epoll

这是一种非常稳的写法。

---

## 9. connfd 就绪后，Reactor 和模拟 Proactor 怎么分开

项目通过 `actor_model_` 分成两种路径。

---

## 10. 路径一：Reactor 模式的完整过程

当 `-a 1` 时走 Reactor。

### 10.1 主线程只负责分发

主线程在 `run()` 里只做这些事：

1. 发现 `connfd` 可读
2. `epoll_ctl(DEL, client_fd)`
3. 丢给线程池

代码逻辑：

```cpp
pool_.enqueue([this, client_fd]() {
    Server::handle_client(this, client_fd);
});
```

### 10.2 工作线程负责读请求

线程池任务会调用：

```cpp
handle_client_impl(client_fd);
```

这个函数里先把 `client_fd` 改成阻塞模式：

```cpp
set_blocking(client_fd);
```

然后调用：

```cpp
read_http_request(client_fd, raw_request);
```

也就是说，在 Reactor 模式下：
- `recv` 是工作线程做的
- 主线程不读这个 socket

这正是 Reactor 的核心特征：
- 主线程负责事件分发
- 工作线程负责 I/O + 业务

### 10.3 工作线程继续做业务和回包

读到完整请求后，会调用：

```cpp
process_request_and_respond(client_fd, raw_request);
```

这个函数负责：
- 解析 HTTP
- 路由分发
- 静态资源读取
- MySQL 注册/登录
- 组装响应
- `send_all()` 回包
- 判断是否 keep-alive

所以在 Reactor 模式里，工作线程做的是：

```text
读 socket -> 业务处理 -> 写 socket
```

---

## 11. 路径二：模拟 Proactor 模式的完整过程

当 `-a 0` 时走模拟 Proactor。

### 11.1 主线程先把请求读出来

主线程在 `run()` 中发现连接可读后，会先做：

```cpp
set_blocking(client_fd);
read_http_request(client_fd, raw_request);
```

也就是说：
- 主线程直接把请求数据从 socket 里读出来
- 线程池拿到的已经不是“一个待读取的 fd”
- 而是“一个已经读好的请求字符串”

### 11.2 线程池只处理业务和回包

然后主线程再投递：

```cpp
pool_.enqueue([this, client_fd, raw_request]() {
    process_request_and_respond(client_fd, raw_request);
});
```

此时线程池做的是：

```text
解析请求 -> 执行业务 -> 发响应
```

而不是先 `recv`

### 11.3 为什么叫“模拟 Proactor”

真正经典的 Proactor 是：
- 内核或异步 I/O 子系统先帮你把 I/O 做完
- 应用层拿到的是“完成通知”

你这个项目不是 Linux AIO 那种真正内核异步完成 I/O 的写法。

它只是“主线程先读完，再通知线程池处理业务”，所以通常叫：

```text
模拟 Proactor
```

---

## 12. `read_http_request()` 在 epoll 流程里的具体角色

这个函数是“从 socket 里把一整个 HTTP 请求读完整”的关键。

### 12.1 它不是读一次就结束

内部是循环：

```cpp
while (true) {
    int n = recv(client_fd, buf, sizeof(buf), 0);
    ...
}
```

### 12.2 它读到什么时候才算“请求完整”

逻辑是：

1. 先找请求头结尾 `\r\n\r\n`
2. 如果存在 `Content-Length`
3. 继续读到：

```text
header_end + 4 + content_length
```

为止

这意味着它不是“读到有数据就算完”，而是“读到完整 HTTP 报文才返回”。

这和 epoll 的关系是：
- epoll 只告诉你“这个 fd 现在可读”
- 但到底读多少，何时算完整，是 `read_http_request()` 决定的

---

## 13. 业务处理完成后，为什么还要重新加入 epoll

这一步和长连接直接相关。

在 `process_request_and_respond()` 里，发送响应后会判断：

```cpp
bool keep_alive = ...
```

### 13.1 如果不是长连接

直接：

```cpp
close(client_fd);
erase_conn_activity(client_fd);
```

这个连接生命周期到此结束。

### 13.2 如果是长连接

则做下面几步：

1. 把 `client_fd` 重新设为非阻塞
2. 再次 `epoll_ctl(ADD, client_fd, ...)`
3. 更新 `last_active_[client_fd]`

代码逻辑就是：

```cpp
set_nonblocking(client_fd);
add_conn_fd_to_epoll(client_fd);
last_active_[client_fd] = now;
```

为什么必须重新 `ADD`？

因为前面在处理前已经 `DEL` 过一次了。

如果不重新加回 epoll，那么这个连接虽然还开着，但：
- 主线程再也不会监听它
- 客户端发第二个请求时服务器感知不到

所以 keep-alive 的关键不是“连接不关闭”而已，而是：

```text
处理完一个请求后，重新把 connfd 放回 epoll
```

---

## 14. LT / ET 在这个完整流程里到底体现在哪

很多人以为 LT/ET 是“整个服务器都变了”，其实不是。

在你这个项目里，它主要影响两处：

### 14.1 影响 listenfd 注册时的事件位

```cpp
EPOLLIN            // LT
EPOLLIN | EPOLLET  // ET
```

这会直接决定 `accept` 是：
- 可以只取一个
- 还是必须取到 `EAGAIN`

### 14.2 影响 connfd 注册时的事件位

```cpp
EPOLLIN            // LT
EPOLLIN | EPOLLET  // ET
```

这会决定连接读事件的触发语义。

不过要注意一件事：

这个项目在真正处理请求时，会先：

1. `DEL` 出 epoll
2. 再临时切换成阻塞读完整请求

所以它不是那种“纯非阻塞 + ET + 一直读到 EAGAIN”的经典单线程写法。

它的好处是：
- 代码更直观
- 更适合学习 HTTP 整包处理
- 容易和线程池、keep-alive 结合

---

## 15. 一个请求从进入到结束的完整时序

下面用一条最典型的请求链路把整个 epoll 过程串起来。

### 15.1 阶段 A：服务器启动

```text
socket
-> setsockopt
-> set_nonblocking(listenfd)
-> bind
-> listen
-> epoll_create1
-> epoll_ctl(ADD, listenfd)
```

### 15.2 阶段 B：客户端建立 TCP 连接

```text
客户端 connect
-> listenfd 变为可读
-> epoll_wait 返回 listenfd
-> accept 得到 client_fd
-> set_nonblocking(client_fd)
-> epoll_ctl(ADD, client_fd)
```

### 15.3 阶段 C：客户端发 HTTP 请求

```text
客户端发送请求
-> connfd 变为可读
-> epoll_wait 返回 connfd
-> epoll_ctl(DEL, connfd)
-> 按 Reactor/模拟Proactor 分流
```

### 15.4 阶段 D：读请求并处理

Reactor:

```text
线程池任务
-> read_http_request(recv 循环)
-> process_request_and_respond
-> send_all
```

模拟 Proactor:

```text
主线程 read_http_request(recv 循环)
-> 线程池 process_request_and_respond
-> send_all
```

### 15.5 阶段 E：响应后决定连接去留

如果短连接：

```text
close(connfd)
```

如果长连接：

```text
set_nonblocking(connfd)
-> epoll_ctl(ADD, connfd)
-> 等待下一次 epoll_wait
```

### 15.6 阶段 F：如果客户端长期不发数据

```text
check_timeout_connections
-> epoll_ctl(DEL, connfd)
-> close(connfd)
```

---

## 16. 为什么这个 epoll 流程适合学习

这个项目的 epoll 写法有几个很好的学习点：

1. 它把 listenfd 和 connfd 的处理清晰分开了。
2. 它把 LT/ET 做成了可切换，不是写死的。
3. 它把 Reactor 和模拟 Proactor 做成了可切换，便于横向对比。
4. 它把 keep-alive 和超时回收都接进了 epoll 主循环，而不是只做一个最小 demo。
5. 它通过“先 DEL，再处理，再按需 ADD”的方式避免了多线程重复处理同一个连接。

这几件事放在一起，已经比很多只会 `epoll_create + epoll_wait` 的教学 demo 更完整了。

---

## 17. 你在面试里可以怎么描述这个项目的 epoll

可以直接这样说：

```text
我的服务器启动时先创建 listenfd，并把它注册到 epoll。
epoll_wait 返回后，如果是 listenfd 就绪，就 accept 新连接并把 connfd 注册进 epoll。
如果是 connfd 可读，就先把它从 epoll 中删除，避免线程池重复处理同一个连接。
然后根据配置选择 Reactor 或模拟 Proactor：
Reactor 由工作线程负责读请求和处理业务，
模拟 Proactor 由主线程先把请求读完，再交工作线程处理业务。
响应发完后，如果是 keep-alive，就重新把 connfd 加回 epoll；
如果不是，就直接关闭连接。并且我还有基于活跃时间的超时回收机制。
```

这段话已经能把项目的 epoll 主干讲清楚。

---

## 18. 最后给你一个极简版伪代码

```cpp
init() {
    listenfd = socket(...);
    set_nonblocking(listenfd);
    bind(listenfd, ...);
    listen(listenfd, ...);

    epfd = epoll_create1(0);
    epoll_ctl(epfd, ADD, listenfd, listen_event);
}

run() {
    while (true) {
        nfds = epoll_wait(epfd, events, ...);
        check_timeout_connections();

        for each event in events {
            if (fd == listenfd) {
                accept new client;
                set_nonblocking(client_fd);
                epoll_ctl(epfd, ADD, client_fd, conn_event);
            } else if (fd is readable) {
                epoll_ctl(epfd, DEL, client_fd, nullptr);

                if (Reactor) {
                    threadpool.enqueue(read + process + write);
                } else {
                    main thread read;
                    threadpool.enqueue(process + write);
                }
            } else {
                close(fd);
            }
        }
    }
}

after_response() {
    if (keep_alive) {
        set_nonblocking(client_fd);
        epoll_ctl(epfd, ADD, client_fd, conn_event);
    } else {
        close(client_fd);
    }
}
```

---

如果你下一步要准备面试，我建议继续补两份材料：
- “这个项目里 LT/ET 为什么能共存”
- “这个项目里先 DEL 再处理的线程安全意义”

这两点很容易被追问。
