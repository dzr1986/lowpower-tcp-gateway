cmake_minimum_required(VERSION 3.16)

project(lowpower_tcp_gateway)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

set(LOWPOWER_TCP_GATEWAY_SOURCES
    src/main.cpp
    src/AppConfig.cpp
    src/TcpGatewayServer.cpp
    src/HttpAdminServer.cpp
    src/protocol/LowPowerProtocol.cpp
    src/protocol/JsonLineProtocol.cpp
    src/session/DeviceSession.cpp
    src/service/WakeManager.cpp
    src/storage/RedisStore.cpp
)

add_executable(lowpower_tcp_gateway ${LOWPOWER_TCP_GATEWAY_SOURCES})

target_include_directories(lowpower_tcp_gateway
    PRIVATE
        include
)

target_link_libraries(lowpower_tcp_gateway
    PRIVATE
    pthread
    hiredis
    OpenSSL::Crypto
)