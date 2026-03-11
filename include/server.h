#ifndef SERVER_H
#define SERVER_H

#include "threadpool.h"

class Server {
private:
    int port_;          // 监听端口
    int server_fd_;     // 监听 socket
    int epfd_;          // epoll 实例 fd
    ThreadPool pool_;   // 线程池

    // 把 fd 设置为非阻塞
    bool set_nonblocking(int fd);

    // 处理一个客户端连接
    static void handle_client(int client_fd);

public:
    Server(int port, int thread_count);

    // 初始化服务器
    bool init();

    // 启动事件循环
    void run();

    ~Server();
};

#endif