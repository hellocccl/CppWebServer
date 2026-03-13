# CppWebServer 中 Reactor / 模拟 Proactor 是如何体现的

这份文档专门解释一件事：

```text
在你这个项目里，Reactor 和模拟 Proactor 不是概念口号，
而是确实对应了两条不同的事件处理路径。
```

这里不空讲定义，直接结合项目中的真实代码说明：
- 模式是从哪里配置进来的
- 主线程和工作线程分别做什么
- 读 socket、处理业务、写 socket 分别由谁负责
- 为什么这个项目里叫“模拟 Proactor”而不是真 Proactor
- 两种模型最终怎么和 epoll、keep-alive、线程池配合起来

主要对应文件：
- `src/main.cpp`
- `include/server.h`
- `src/server.cpp`

---

## 1. 先说结论：这个项目里两种模型的本质区别

最核心的区别只有一句话：

### Reactor

```text
主线程只负责事件分发，
工作线程负责读 socket + 业务处理 + 写 socket。
```

### 模拟 Proactor

```text
主线程先把请求从 socket 中读出来，
工作线程只负责业务处理 + 写 socket。
```

所以你项目里的关键差别，不在于“有没有线程池”，而在于：

```text
到底是谁来执行 recv 读请求
```

这也是面试里最应该先讲清楚的一点。

---

## 2. 这个模式是怎么配置进项目的

### 2.1 启动参数 `-a`

在 `src/main.cpp` 中，项目支持：

```bash
./server -a 0
./server -a 1
```

含义是：
- `-a 0`：模拟 Proactor
- `-a 1`：Reactor

也就是说，模型不是写死在代码里的，而是运行时选择。

### 2.2 参数传入 `Server`

`main.cpp` 最终会这样构造服务器：

```cpp
Server server(8080, 4, "../www", actor_model, trig_mode);
```

其中 `actor_model` 就是 `-a` 解析后的结果。

### 2.3 保存到 `Server` 成员里

在 `include/server.h` 中，有这个字段：

```cpp
int actor_model_;
```

注释已经明确写了：

```cpp
// 0 -> 模拟 Proactor（主线程读，线程池处理业务和写）
// 1 -> Reactor（线程池负责读 + 业务 + 写）
```

这句话其实已经把整个项目的模型差异总结出来了。

### 2.4 构造函数中确定模式

在 `src/server.cpp` 的构造函数里：

```cpp
actor_model_(actor_model == 1 ? 1 : 0)
```

这表示：
- 传 `1` 才是 Reactor
- 其它值都回退成模拟 Proactor

这种写法能避免非法参数把状态搞乱。

---

## 3. 两种模型分叉之前，大家都走同一条路

无论你选 Reactor 还是模拟 Proactor，前面的流程完全一样。

### 3.1 启动服务器

在 `Server::init()` 中：

1. 创建 `listenfd`
2. `bind + listen`
3. 创建 `epoll`
4. 把 `listenfd` 注册进 epoll

到这里，两种模型还没有区别。

### 3.2 主循环等待事件

在 `Server::run()` 中：

```cpp
int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 10000);
```

主线程通过 `epoll_wait` 等待：
- 新连接事件
- 连接上的读事件

这一步两种模型也完全一样。

### 3.3 新连接到来时的处理也一样

当 `fd == server_fd_`，说明有新连接：

1. `accept`
2. 拿到 `client_fd`
3. 设置非阻塞
4. 注册到 epoll
5. 记录活跃时间

这部分与 Reactor/模拟 Proactor 无关，都是网络接入阶段。

---

## 4. 真正分叉的地方：connfd 可读之后

两种模型的区别，发生在这里：

```cpp
else if (events[i].events & EPOLLIN) {
    ...
}
```

当某个连接 fd 可读，说明客户端发来了 HTTP 请求。

项目会先做一件共同的事情：

```cpp
epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr);
```

目的：
- 先把这个连接从 epoll 中摘掉
- 防止线程池处理期间，同一个 fd 又被主线程重复拿到

这一步非常关键，它保证了：

```text
同一个连接不会被两个线程同时处理
```

之后，代码就开始按 `actor_model_` 分叉。

---

## 5. Reactor 在这个项目里是怎么体现的

当：

```cpp
if (actor_model_ == 1)
```

就进入 Reactor 路径。

### 5.1 主线程做什么

主线程只做了这几件事：

1. 发现 `connfd` 可读
2. 从 epoll 中删除该 fd
3. 把 fd 交给线程池

代码是：

```cpp
pool_.enqueue([this, client_fd]() {
    Server::handle_client(this, client_fd);
});
```

也就是说，主线程没有去读这个 socket。

### 5.2 工作线程做什么

线程池任务最后会走到：

```cpp
handle_client_impl(client_fd);
```

在这里，工作线程先把连接改成阻塞模式：

```cpp
set_blocking(client_fd);
```

然后自己执行：

```cpp
read_http_request(client_fd, raw_request);
```

这说明：

```text
Reactor 模式下，真正执行 recv 的是工作线程
```

### 5.3 读完之后继续做业务

工作线程在读完整个 HTTP 请求后，会继续：

```cpp
process_request_and_respond(client_fd, raw_request);
```

这个函数里会完成：
- 解析 HTTP 请求
- 路由判断
- 静态文件读取
- MySQL 注册/登录
- 拼接 HTTP 响应
- `send_all()` 回包
- 判断是否 keep-alive

所以在 Reactor 模式下，线程池里的一个任务本质是：

```text
read -> parse -> business -> write
```

### 5.4 为什么这符合 Reactor

Reactor 的关键特征是：

```text
主线程（或事件循环线程）不真正完成 I/O，
它只是告诉工作线程“这个 fd 可以处理了”。
```

你这个项目里正是这样：
- 主线程负责 `epoll_wait`
- 主线程发现可读事件后只做分发
- 工作线程自己去读 socket

这就是 Reactor 在你项目中的落地方式。

---

## 6. 模拟 Proactor 在这个项目里是怎么体现的

当：

```cpp
else
```

也就是 `actor_model_ == 0`，项目走模拟 Proactor。

### 6.1 主线程先完成读取

主线程在这条路径里不会直接把 `client_fd` 扔给线程池。

它先做两步：

```cpp
set_blocking(client_fd);
read_http_request(client_fd, raw_request);
```

这表示：

```text
主线程自己先把 HTTP 请求完整读出来
```

这一步和 Reactor 最大的区别就在这里。

### 6.2 工作线程不再负责读

主线程读完后，再把“已经读好的请求”交给线程池：

```cpp
pool_.enqueue([this, client_fd, raw_request]() {
    process_request_and_respond(client_fd, raw_request);
});
```

注意线程池任务接收到的不是“一个等待读取的 fd”，而是：
- `client_fd`
- `raw_request`

所以工作线程可以直接进入业务处理，而不用再 `recv`。

### 6.3 线程池负责什么

在模拟 Proactor 路径里，线程池负责：

```text
parse -> business -> write
```

而不是：

```text
read -> parse -> business -> write
```

### 6.4 为什么这里不叫“真正的 Proactor”

真正的 Proactor 模型一般意味着：

```text
I/O 本身是异步完成的，
应用层拿到的是“完成通知”。
```

例如：
- 内核异步 I/O
- completion queue
- read/write completion event

但你这个项目里：
- 主线程仍然是同步调用 `read_http_request()`
- `recv` 还是应用线程主动去读的
- 只是读完后再把结果交给线程池处理

所以这不是严格意义上的内核异步 Proactor。

因此文档里写“模拟 Proactor”是准确的。

---

## 7. 两种模型最终都会汇合到哪里

不管是 Reactor 还是模拟 Proactor，最终都会汇合到同一个函数：

```cpp
process_request_and_respond(client_fd, raw_request);
```

这个设计很重要，因为它避免了两套业务逻辑重复维护。

### 7.1 共用的业务处理

这个函数统一处理：
- `GET /`
- 静态资源
- `/register`
- `/login`
- `/post`
- 404 / 500 等响应

所以差异只发生在“读请求之前”。

### 7.2 共用的回包逻辑

它们最终都调用：

```cpp
send_all(client_fd, response, sent);
```

所以两种模型的响应输出方式是一致的。

### 7.3 共用的 keep-alive 逻辑

发送完成后，两种模型都走同一套 keep-alive 判断：

1. 根据 HTTP 版本和 `Connection` 头算出 `keep_alive`
2. 如果保持连接：
   - `set_nonblocking(client_fd)`
   - `add_conn_fd_to_epoll(client_fd)`
   - 更新时间戳
3. 否则：
   - `close(client_fd)`

这意味着：

```text
Reactor 和模拟 Proactor 只影响“请求是由谁读出来的”，
不会影响后面的业务处理和长连接机制。
```

---

## 8. 这个项目里主线程和工作线程的职责对比

为了彻底看清楚差异，可以直接按线程职责来比较。

### 8.1 Reactor

主线程：
- `epoll_wait`
- 判断就绪 fd
- `DEL connfd`
- 投递线程池任务

工作线程：
- `set_blocking`
- `read_http_request`
- `process_request_and_respond`
- `send_all`
- keep-alive 时重新注册 epoll

### 8.2 模拟 Proactor

主线程：
- `epoll_wait`
- 判断就绪 fd
- `DEL connfd`
- `set_blocking`
- `read_http_request`
- 投递线程池任务

工作线程：
- `process_request_and_respond`
- `send_all`
- keep-alive 时重新注册 epoll

### 8.3 一眼看懂差别

最简化后就是：

Reactor：

```text
主线程发现事件
-> 工作线程读请求
-> 工作线程处理业务
-> 工作线程发响应
```

模拟 Proactor：

```text
主线程发现事件
-> 主线程读请求
-> 工作线程处理业务
-> 工作线程发响应
```

---

## 9. 为什么项目里要支持这两种模型

这不是为了炫技，而是为了让你能清楚对比两种服务器设计思路。

### 9.1 Reactor 的特点

优点：
- 主线程轻，只负责事件分发
- 事件循环更纯粹
- 更符合很多高性能网络库的组织方式

代价：
- 工作线程不仅做业务，还做 socket I/O
- 线程任务更重

### 9.2 模拟 Proactor 的特点

优点：
- 线程池拿到的就是完整请求，业务入口更直接
- 业务处理层可以少关心“读没读完整”
- 学习时更容易把“网络读取”和“业务处理”分层

代价：
- 主线程会承担同步读请求的成本
- 高并发时主线程压力更大

### 9.3 为什么你这个项目里叫“模拟 Proactor”更严谨

如果你在面试里直接说“我实现了 Proactor”，容易被追问：
- 你是不是用了真正异步 I/O？
- 内核什么时候通知你 read 完成？
- completion queue 在哪？

而你现在的实现并不是那种模型。

更准确的说法应该是：

```text
我实现了一个基于 epoll 的 Reactor，
以及一个由主线程先读、再交线程池处理的模拟 Proactor。
```

这个说法是稳的。

---

## 10. 它和 LT/ET 的关系是什么

很多人会把 Reactor/Proactor 和 LT/ET 混在一起，其实它们不是一层东西。

### 10.1 Reactor / 模拟 Proactor

回答的是：

```text
谁来读 socket、谁来处理业务
```

### 10.2 LT / ET

回答的是：

```text
epoll 以什么触发语义通知你 fd 就绪
```

所以在你的项目里：
- `-a` 决定并发模型
- `-m` 决定触发方式

它们是正交的，可以任意组合。

例如：
- `-a 1 -m 0`：Reactor + LT/LT
- `-a 1 -m 3`：Reactor + ET/ET
- `-a 0 -m 1`：模拟 Proactor + LT/ET

这也是你项目结构比较完整的地方。

---

## 11. 用一条请求把两种模型完整走一遍

下面用“客户端发一个 HTTP 请求”来对比。

### 11.1 Reactor 时序

```text
客户端发请求
-> epoll_wait 返回 connfd 可读
-> 主线程 DEL connfd
-> 主线程把 client_fd 交给线程池
-> 工作线程 read_http_request
-> 工作线程 process_request_and_respond
-> 工作线程 send_all
-> 若 keep-alive，则工作线程重新 ADD connfd 回 epoll
```

### 11.2 模拟 Proactor 时序

```text
客户端发请求
-> epoll_wait 返回 connfd 可读
-> 主线程 DEL connfd
-> 主线程 read_http_request
-> 主线程把 raw_request 交给线程池
-> 工作线程 process_request_and_respond
-> 工作线程 send_all
-> 若 keep-alive，则工作线程重新 ADD connfd 回 epoll
```

### 11.3 最明显的观察点

对比这两条时序，你只要盯住一个点就行：

```text
read_http_request 到底是谁调用的
```

这一个点，决定了整个模型的归类。

---

## 12. 为什么代码里把业务抽成 `process_request_and_respond`

这是这个实现里一个很好的工程设计点。

如果没有这个函数，代码可能会变成：
- Reactor 有一套完整业务处理逻辑
- 模拟 Proactor 再复制一套

这样会导致：
- 维护成本翻倍
- 改一个路由要改两处
- 很容易两边行为不一致

现在的写法是：
- 两个模型只在“前半段 I/O 读取”不同
- 后半段业务处理统一复用

这保证了：

```text
模型切换只改变并发路径，不改变业务结果
```

这是对的。

---

## 13. 如果你要在面试里怎么讲

可以直接按下面这段说：

```text
我这个项目支持 Reactor 和模拟 Proactor 两种事件处理模型，
通过启动参数 -a 选择。
在 Reactor 模式下，主线程只负责 epoll_wait 和事件分发，
连接就绪后把 fd 从 epoll 中删除，再交给线程池；
工作线程负责 read_http_request、业务处理和 send 回包。
在模拟 Proactor 模式下，主线程在连接就绪后先同步把请求读完整，
然后把已经读好的 raw_request 交给线程池去做业务处理和响应发送。
两种模型最终共用同一个 process_request_and_respond 函数，
所以业务逻辑一致，只是 I/O 读取责任不同。
```

这段话是准确的，而且和你代码完全对得上。

---

## 14. 最后给你一个极简伪代码

### 14.1 Reactor

```cpp
epoll_wait(...);
if (connfd readable) {
    epoll_ctl(DEL, connfd);
    threadpool.enqueue([fd] {
        read_http_request(fd, raw_request);
        process_request_and_respond(fd, raw_request);
    });
}
```

### 14.2 模拟 Proactor

```cpp
epoll_wait(...);
if (connfd readable) {
    epoll_ctl(DEL, connfd);
    read_http_request(fd, raw_request);
    threadpool.enqueue([fd, raw_request] {
        process_request_and_respond(fd, raw_request);
    });
}
```

你会发现，两段代码只有一个核心差异：

```text
read_http_request 是在主线程执行，还是在线程池里执行
```

---

## 15. 一句话总结

在这个项目中：

```text
Reactor = 工作线程读请求
模拟 Proactor = 主线程先读请求
两者共享同一套业务处理和 keep-alive 逻辑
```

这就是它们在整个项目中的具体体现。
