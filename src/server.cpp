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

WebServer::WebServer(int port, int thread_count, const std::string& root_dir)
    : port_(port), thread_count_(thread_count), root_dir_(root_dir), pool_(thread_count) {}

WebServer::~WebServer() {
    for (int fd : clients_) {
        close(fd);
    }
    if (listen_fd_ != -1) close(listen_fd_);
    if (epoll_fd_ != -1) close(epoll_fd_);
}

bool WebServer::init() {
    if (!init_listen_socket()) return false;
    if (!init_epoll()) return false;
    Logger::instance().info("server init success");
    return true;
}

bool WebServer::init_listen_socket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        Logger::instance().error("socket failed");
        return false;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Logger::instance().error(std::string("bind failed: ") + std::strerror(errno));
        return false;
    }

    if (listen(listen_fd_, 128) < 0) {
        Logger::instance().error(std::string("listen failed: ") + std::strerror(errno));
        return false;
    }

    set_nonblocking(listen_fd_);
    Logger::instance().info("listen on port " + std::to_string(port_));
    return true;
}

bool WebServer::init_epoll() {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        Logger::instance().error("epoll_create1 failed");
        return false;
    }

    add_fd(listen_fd_);
    return true;
}

void WebServer::add_fd(int fd) {
    epoll_event ev{};
    ev.data.fd = fd;
    // 这里使用 LT(水平触发) 模式，逻辑更容易理解
    ev.events = EPOLLIN;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

void WebServer::del_fd(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients_.erase(fd);
}

int WebServer::set_nonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void WebServer::handle_accept() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            Logger::instance().error(std::string("accept failed: ") + std::strerror(errno));
            break;
        }

        set_nonblocking(client_fd);
        add_fd(client_fd);
        clients_.insert(client_fd);

        Logger::instance().info(
            "new client fd=" + std::to_string(client_fd) +
            " ip=" + inet_ntoa(client_addr.sin_addr) +
            " port=" + std::to_string(ntohs(client_addr.sin_port)));
    }
}

void WebServer::handle_readable(int client_fd) {
    // 把客户端fd丢给线程池处理
    pool_.enqueue([this, client_fd]() {
        HttpConnection conn(client_fd, root_dir_);
        conn.process();
        // 本 demo 每次响应后直接关闭连接，简化 keep-alive 的复杂度
        del_fd(client_fd);
    });
}

void WebServer::run() {
    constexpr int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            Logger::instance().error(std::string("epoll_wait failed: ") + std::strerror(errno));
            continue;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                handle_accept();
            } else if (events[i].events & EPOLLIN) {
                handle_readable(fd);
            } else {
                del_fd(fd);
            }
        }
    }
}
