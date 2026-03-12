#include "logger.h"
#include "server.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // -l 参数：日志写入方式
    // 0 -> 同步写入（默认）
    // 1 -> 异步写入
    int log_write_mode = 0;

    // -a 参数：并发模型
    // 0 -> 模拟 Proactor（默认）
    // 1 -> Reactor
    int actor_model = 0;

    // -m 参数：listenfd / connfd 触发模式组合
    // 0 -> LT + LT（默认）
    // 1 -> LT + ET
    // 2 -> ET + LT
    // 3 -> ET + ET
    int trig_mode = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-l") {
            if (i + 1 >= argc) {
                std::cerr << "用法: ./server [-l LOGWrite]\n"
                          << "LOGWrite: 0(同步, 默认) 或 1(异步)\n"
                          << "-a: 0(Proactor, 默认) 或 1(Reactor)\n"
                          << "-m: 0(LT+LT, 默认) 1(LT+ET) 2(ET+LT) 3(ET+ET)\n";
                return 1;
            }
            std::string mode = argv[++i];
            if (mode == "0") {
                log_write_mode = 0;
            } else if (mode == "1") {
                log_write_mode = 1;
            } else {
                std::cerr << "非法 -l 参数: " << mode << "\n"
                          << "用法: ./server [-l LOGWrite]\n"
                          << "LOGWrite: 0(同步, 默认) 或 1(异步)\n"
                          << "-a: 0(Proactor, 默认) 或 1(Reactor)\n"
                          << "-m: 0(LT+LT, 默认) 1(LT+ET) 2(ET+LT) 3(ET+ET)\n";
                return 1;
            }
        } else if (arg == "-a") {
            if (i + 1 >= argc) {
                std::cerr << "用法: ./server [-l LOGWrite] [-a 0|1] [-m 0|1|2|3]\n";
                return 1;
            }
            std::string mode = argv[++i];
            if (mode == "0") {
                actor_model = 0;
            } else if (mode == "1") {
                actor_model = 1;
            } else {
                std::cerr << "非法 -a 参数: " << mode << "\n"
                          << "-a: 0(Proactor, 默认) 或 1(Reactor)\n";
                return 1;
            }
        } else if (arg == "-m") {
            if (i + 1 >= argc) {
                std::cerr << "用法: ./server [-l LOGWrite] [-a 0|1] [-m 0|1|2|3]\n";
                return 1;
            }
            std::string mode = argv[++i];
            if (mode == "0") {
                trig_mode = 0;
            } else if (mode == "1") {
                trig_mode = 1;
            } else if (mode == "2") {
                trig_mode = 2;
            } else if (mode == "3") {
                trig_mode = 3;
            } else {
                std::cerr << "非法 -m 参数: " << mode << "\n"
                          << "-m: 0(LT+LT, 默认) 1(LT+ET) 2(ET+LT) 3(ET+ET)\n";
                return 1;
            }
        } else {
            std::cerr << "未知参数: " << arg << "\n"
                      << "用法: ./server [-l LOGWrite] [-a 0|1] [-m 0|1|2|3]\n"
                      << "LOGWrite: 0(同步, 默认) 或 1(异步)\n"
                      << "-a: 0(Proactor, 默认) 或 1(Reactor)\n"
                      << "-m: 0(LT+LT, 默认) 1(LT+ET) 2(ET+LT) 3(ET+ET)\n";
            return 1;
        }
    }

    // 初始化日志系统
    if (!Logger::instance().init("server.log", log_write_mode)) {
        std::cerr << "日志文件打开失败，日志将只输出到控制台" << std::endl;
    } else {
        Logger::instance().info("日志系统初始化成功，日志文件: server.log");
        if (log_write_mode == 0) {
            Logger::instance().info("日志写入模式: 同步写入 (0)");
        } else {
            Logger::instance().info("日志写入模式: 异步写入 (1)");
        }
    }

    // 创建服务器：监听 8080，线程池 4 个线程
    Server server(8080, 4, "../www", actor_model, trig_mode);

    if (!server.init()) {
        Logger::instance().error("服务器初始化失败");
        return 1;
    }

    Logger::instance().info("线程池创建成功，工作线程数量 = 4");

    server.run();
    return 0;
}
