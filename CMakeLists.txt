cmake_minimum_required(VERSION 3.10)

project(SimpleWebServer)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# 头文件目录
include_directories(include)

# 源文件
set(SOURCES
    src/main.cpp
    src/logger.cpp
    src/threadpool.cpp
    src/http_request.cpp
    src/security/password_hash.cpp
    src/db/mysql_user_store.cpp
    src/server.cpp
)

# 生成可执行文件
add_executable(server ${SOURCES})

# Linux 下线程库
find_package(Threads REQUIRED)
target_link_libraries(server PRIVATE Threads::Threads)

# Linux 下链接 MySQL C API（mysql/mysql.h + libmysqlclient）
if(UNIX)
    find_library(MYSQLCLIENT_LIB mysqlclient)
    if(NOT MYSQLCLIENT_LIB)
        message(FATAL_ERROR "未找到 mysqlclient，请先安装 libmysqlclient-dev")
    endif()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(server PRIVATE ${MYSQLCLIENT_LIB} OpenSSL::Crypto)
endif()
