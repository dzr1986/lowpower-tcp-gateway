cmake_minimum_required(VERSION 3.16)

project(lowpower_tcp_gateway)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
find_library(MOSQUITTO_LIBRARY mosquitto)

set(LOWPOWER_TCP_GATEWAY_SOURCES
    src/main.cpp
    src/AppConfig.cpp
    src/backend/MqttBackend.cpp
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

if (MOSQUITTO_INCLUDE_DIR)
    target_include_directories(lowpower_tcp_gateway PRIVATE ${MOSQUITTO_INCLUDE_DIR})
endif()

if (MOSQUITTO_LIBRARY AND MOSQUITTO_INCLUDE_DIR)
    target_compile_definitions(lowpower_tcp_gateway PRIVATE LP_HAS_MQTT=1)
endif()

target_link_libraries(lowpower_tcp_gateway
    PRIVATE
    pthread
    hiredis
    OpenSSL::Crypto
)

if (MOSQUITTO_LIBRARY AND MOSQUITTO_INCLUDE_DIR)
    target_link_libraries(lowpower_tcp_gateway PRIVATE ${MOSQUITTO_LIBRARY})
else()
    message(STATUS "mosquitto not found, MQTT backend bridge will be disabled")
endif()