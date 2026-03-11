#include "logger.h"
#include "server.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    int port = 8080;
    int threads = 4;
    std::string root_dir = "../www";

    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }
    if (argc >= 3) {
        threads = std::stoi(argv[2]);
    }
    if (argc >= 4) {
        root_dir = argv[3];
    }

    if (!std::filesystem::exists(root_dir)) {
        std::cerr << "root dir not exists: " << root_dir << std::endl;
        return 1;
    }

    if (!Logger::instance().init("server.log")) {
        std::cerr << "logger init failed" << std::endl;
        return 1;
    }

    Logger::instance().info("server starting...");

    WebServer server(port, threads, root_dir);
    if (!server.init()) {
        Logger::instance().error("server init failed");
        return 1;
    }

    server.run();
    return 0;
}
