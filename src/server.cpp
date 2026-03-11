#include "server.h"
#include "http_connection.h"
#include "logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// 构造函数：初始化 Web 服务器，设置端口号、线程数量和根目录
WebServer::WebServer(int port, int thread_count, const std::string& root_dir)
    : port_(port), thread_count_(thread_count), root_dir_(root_dir), pool_(thread_count) {}

// 析构函数：关闭所有客户端连接和服务器资源
WebServer::~WebServer() {
    for (int fd : clients_) {
        close(fd);
    }
    if (listen_fd_ != -1) close(listen_fd_);
    if (epoll_fd_ != -1) close(epoll_fd_);
}

// 初始化服务器，包括监听套接字和 epoll
bool WebServer::init() {
    if (!init_listen_socket()) return false; // 初始化监听套接字
    if (!init_epoll()) return false; // 初始化 epoll
    Logger::instance().info("服务器初始化成功");
    return true;
}

// 初始化监听套接字
bool WebServer::init_listen_socket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0); // 创建套接字
    if (listen_fd_ < 0) {
        Logger::instance().error("创建套接字失败");
        return false;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 设置地址复用

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Logger::instance().error(std::string("绑定失败: ") + std::strerror(errno));
        return false;
    }

    if (listen(listen_fd_, 128) < 0) {
        Logger::instance().error(std::string("监听失败: ") + std::strerror(errno));
        return false;
    }

    set_nonblocking(listen_fd_); // 设置非阻塞模式
    Logger::instance().info("监听端口 " + std::to_string(port_));
    return true;
}

// 初始化 epoll
bool WebServer::init_epoll() {
    epoll_fd_ = epoll_create1(0); // 创建 epoll 实例
    if (epoll_fd_ < 0) {
        Logger::instance().error("epoll_create1 失败");
        return false;
    }

    add_fd(listen_fd_); // 将监听套接字添加到 epoll 中
    return true;
}

// 将文件描述符添加到 epoll 中
void WebServer::add_fd(int fd) {
    epoll_event ev{};
    ev.data.fd = fd;
    // 使用 LT（水平方向触发）模式，逻辑更容易理解
    ev.events = EPOLLIN;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

// 从 epoll 中删除文件描述符并关闭
void WebServer::del_fd(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients_.erase(fd);
}

// 设置文件描述符为非阻塞模式
int WebServer::set_nonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 处理新客户端连接
void WebServer::handle_accept() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 没有更多连接
            }
            Logger::instance().error(std::string("接受连接失败: ") + std::strerror(errno));
            break;
        }

        set_nonblocking(client_fd); // 设置客户端套接字为非阻塞模式
        add_fd(client_fd); // 将客户端套接字添加到 epoll 中
        clients_.insert(client_fd);

        Logger::instance().info(
            "新客户端 fd=" + std::to_string(client_fd) +
            " ip=" + inet_ntoa(client_addr.sin_addr) +
            " port=" + std::to_string(ntohs(client_addr.sin_port)));
    }
}

// 处理可读事件的客户端
void WebServer::handle_readable(int client_fd) {
    // 将客户端 fd 交给线程池处理
    pool_.enqueue([this, client_fd]() {
        HttpConnection conn(client_fd, root_dir_);
        conn.process();
        // 本 demo 每次响应后直接关闭连接，简化 keep-alive 的复杂度
        del_fd(client_fd);
    });
}

// 运行服务器主循环
void WebServer::run() {
    constexpr int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1); // 等待事件
        if (n < 0) {
            if (errno == EINTR) {
                continue; // 被信号中断，继续等待
            }
            Logger::instance().error(std::string("epoll_wait 失败: ") + std::strerror(errno));
            continue;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                handle_accept(); // 处理新连接
            } else if (events[i].events & EPOLLIN) {
                handle_readable(fd); // 处理可读事件
            } else {
                del_fd(fd); // 其他情况，关闭连接
            }
        }
    }
}
