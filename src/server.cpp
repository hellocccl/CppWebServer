#include "server.h"

#include "http_request.h"
#include "logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

Server::Server(int port, int thread_count)
    : port_(port), server_fd_(-1), epfd_(-1), pool_(thread_count) {}

bool Server::set_nonblocking(int fd) {
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) {
        return false;
    }

    if (fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) == -1) {
        return false;
    }

    return true;
}

bool Server::init() {
    Logger::instance().info("服务器准备启动");

    // 1. 创建 socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        Logger::instance().error("创建 socket 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 允许端口复用
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::instance().error("setsockopt SO_REUSEADDR 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 设置监听 socket 为非阻塞
    if (!set_nonblocking(server_fd_)) {
        Logger::instance().error("设置 server_fd 非阻塞失败");
        return false;
    }

    // 2. 准备地址
    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    // 3. bind
    if (bind(server_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        Logger::instance().error("bind 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 4. listen
    if (listen(server_fd_, 10) == -1) {
        Logger::instance().error("listen 失败: " + std::string(strerror(errno)));
        return false;
    }

    Logger::instance().info("服务器启动成功，监听端口 = " + std::to_string(port_));

    // 5. 创建 epoll
    epfd_ = epoll_create1(0);
    if (epfd_ == -1) {
        Logger::instance().error("epoll_create1 失败: " + std::string(strerror(errno)));
        return false;
    }

    Logger::instance().info("epoll 实例创建成功");

    // 6. 把监听 fd 加入 epoll
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev) == -1) {
        Logger::instance().error("epoll_ctl ADD server_fd 失败: " + std::string(strerror(errno)));
        return false;
    }

    Logger::instance().info("已将监听 fd 加入 epoll, server_fd = " + std::to_string(server_fd_));
    return true;
}

void Server::handle_client(int client_fd) {
    char buffer[1024] = {0};

    int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        Logger::instance().error(
            "recv 失败或客户端已关闭连接, fd = " + std::to_string(client_fd) +
            ", errno = " + std::to_string(errno) +
            ", error = " + std::string(strerror(errno))
        );
        close(client_fd);
        return;
    }

    std::string raw_request(buffer);

    HttpRequest request;
    if (!request.parse(raw_request)) {
        Logger::instance().error("HTTP 请求解析失败, fd = " + std::to_string(client_fd));
        close(client_fd);
        return;
    }

    Logger::instance().info(
        "解析请求成功, fd = " + std::to_string(client_fd) +
        ", method = " + request.method() +
        ", path = " + request.path() +
        ", version = " + request.version()
    );

    std::string body;
    std::string status_line;
    std::string status_text;

    if (request.path() == "/") {
        status_line = "HTTP/1.1 200 OK\r\n";
        status_text = "200 OK";
        body =
            "<html>"
            "<body>"
            "<h1>首页</h1>"
            "<p>你访问的是 /</p>"
            "<p><a href=\"/hello\">访问 /hello</a></p>"
            "</body>"
            "</html>";
    } else if (request.path() == "/hello") {
        status_line = "HTTP/1.1 200 OK\r\n";
        status_text = "200 OK";
        body =
            "<html>"
            "<body>"
            "<h1>Hello 页面</h1>"
            "<p>你访问的是 /hello</p>"
            "<p><a href=\"/\">返回首页</a></p>"
            "</body>"
            "</html>";
    } else {
        status_line = "HTTP/1.1 404 Not Found\r\n";
        status_text = "404 Not Found";
        body =
            "<html>"
            "<body>"
            "<h1>404 Not Found</h1>"
            "<p>你访问的路径不存在</p>"
            "<p><a href=\"/\">返回首页</a></p>"
            "</body>"
            "</html>";
    }

    std::string response =
        status_line +
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    int sent = send(client_fd, response.c_str(), response.size(), 0);
    if (sent < 0) {
        Logger::instance().error(
            "send 失败, fd = " + std::to_string(client_fd) +
            ", errno = " + std::to_string(errno) +
            ", error = " + std::string(strerror(errno))
        );
    } else {
        Logger::instance().info(
            "响应发送成功, fd = " + std::to_string(client_fd) +
            ", status = " + status_text +
            ", bytes = " + std::to_string(sent)
        );
    }

    close(client_fd);
    Logger::instance().info("连接关闭, fd = " + std::to_string(client_fd));
}

void Server::run() {
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epfd_, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                Logger::instance().debug("epoll_wait 被信号中断，继续等待");
                continue;
            }
            Logger::instance().error("epoll_wait 失败: " + std::string(strerror(errno)));
            break;
        }

        Logger::instance().debug("epoll_wait 返回，就绪 fd 数量 = " + std::to_string(nfds));

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            // 有新连接到来
            if (fd == server_fd_) {
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);

                    int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            Logger::instance().error("accept 失败: " + std::string(strerror(errno)));
                            break;
                        }
                    }

                    char ip[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    int port = ntohs(client_addr.sin_port);

                    Logger::instance().info(
                        "有客户端连接, fd = " + std::to_string(client_fd) +
                        ", ip = " + std::string(ip) +
                        ", port = " + std::to_string(port)
                    );

                    if (!set_nonblocking(client_fd)) {
                        Logger::instance().error(
                            "设置 client_fd 非阻塞失败, fd = " + std::to_string(client_fd)
                        );
                        close(client_fd);
                        continue;
                    }

                    epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;

                    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
                        Logger::instance().error(
                            "epoll_ctl ADD client_fd 失败, fd = " + std::to_string(client_fd) +
                            ", error = " + std::string(strerror(errno))
                        );
                        close(client_fd);
                        continue;
                    }

                    Logger::instance().debug(
                        "客户端 fd 已加入 epoll, client_fd = " + std::to_string(client_fd)
                    );
                }
            }
            // 客户端可读
            else if (events[i].events & EPOLLIN) {
                int client_fd = fd;

                Logger::instance().debug(
                    "客户端 fd 可读，准备交给线程池处理, client_fd = " + std::to_string(client_fd)
                );

                if (epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr) == -1) {
                    Logger::instance().error(
                        "epoll_ctl DEL client_fd 失败, fd = " + std::to_string(client_fd) +
                        ", error = " + std::string(strerror(errno))
                    );
                    close(client_fd);
                    continue;
                }

                pool_.enqueue([client_fd]() {
                    Server::handle_client(client_fd);
                });

                Logger::instance().debug(
                    "任务已加入线程池, client_fd = " + std::to_string(client_fd)
                );
            }
            // 其它异常
            else {
                Logger::instance().error(
                    "fd = " + std::to_string(fd) +
                    " 发生异常事件, events = " + std::to_string(events[i].events) +
                    "，关闭连接"
                );
                epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
            }
        }
    }
}

Server::~Server() {
    if (epfd_ != -1) {
        close(epfd_);
    }
    if (server_fd_ != -1) {
        close(server_fd_);
    }
}