#include "logger.h"
#include "server.h"

#include <iostream>

int main() {
    // 初始化日志系统
    if (!Logger::instance().init("server.log")) {
        std::cerr << "日志文件打开失败，日志将只输出到控制台" << std::endl;
    } else {
        Logger::instance().info("日志系统初始化成功，日志文件: server.log");
    }

    // 创建服务器：监听 8080，线程池 4 个线程
    Server server(8080, 4, "../www");

    if (!server.init()) {
        Logger::instance().error("服务器初始化失败");
        return 1;
    }

    Logger::instance().info("线程池创建成功，工作线程数量 = 4");

    server.run();
    return 0;
}