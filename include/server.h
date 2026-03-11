#pragma once

#include "thread_pool.h"
#include <string>
#include <unordered_set>

class WebServer {
public:
    WebServer(int port, int thread_count, const std::string& root_dir);
    ~WebServer();

    bool init();
    void run();

private:
    bool init_listen_socket();
    bool init_epoll();
    void add_fd(int fd);
    void del_fd(int fd);
    void handle_accept();
    void handle_readable(int client_fd);
    static int set_nonblocking(int fd);

private:
    int port_;
    int thread_count_;
    std::string root_dir_;

    int listen_fd_ = -1;
    int epoll_fd_ = -1;

    ThreadPool pool_;
    std::unordered_set<int> clients_;
};
