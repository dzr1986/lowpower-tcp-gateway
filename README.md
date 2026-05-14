# lowpower-tcp-gateway

这是一个面向低功耗终端设备的 TCP 网关服务，核心目标是把设备侧长连接、心跳保活、在线状态维护和远程唤醒控制统一收敛到一个服务里。

详细流程文档见 [docs/system-flow.md](docs/system-flow.md)。

## 主要功能

- 监听设备 TCP 连接，接收设备登录、心跳和业务消息。
- 维护设备会话状态，识别设备在线、离线和超时断开。
- 通过 Redis 存储或同步设备状态，便于外部系统查询和联动。
- 提供 HTTP 管理接口，支持查询设备在线状态和下发唤醒命令。
- 内置低功耗协议处理逻辑，包含帧编解码、认证载荷处理、CRC32、AES-128-CBC 和 Base64 相关能力。

## 运行架构

当前主程序入口在 src/main.cpp，服务启动后会初始化以下模块：

- AppConfig：加载 config.json 中的服务配置。
- RedisStore：连接 Redis，用于保存设备在线状态和唤醒相关数据。
- TcpGatewayServer：处理设备侧 TCP 长连接。
- HttpAdminServer：处理管理侧 HTTP 请求。
- WakeManager：统一管理唤醒命令、超时和重试逻辑。
- MqttBackend：与 MQTT 后台交互，接收后台命令并发布设备状态与命令事件。

## 设备侧能力

设备通过 TCP 端口接入网关。网关会处理以下典型流程：

- 设备登录与鉴权。
- 心跳保活与在线状态刷新。
- 空闲超时检测，超时后自动标记离线。
- 接收并处理服务端发起的唤醒类命令。

## HTTP 管理接口

服务默认监听 HTTP 端口 8080，当前代码中已经实现的接口包括：

### 查询设备在线状态

```http
GET /api/devices/{device_id}/online
```

返回设备是否在线，以及设备基础信息，例如 IMEI、固件版本、型号、信号和电量信息。

### 下发唤醒命令

```http
POST /api/devices/{device_id}/wake?cmd=take_photo
```

如果设备在线，网关会通过 WakeManager 生成消息 ID 并向目标设备下发唤醒指令。默认命令为 take_photo。

### 下发通用命令

```http
POST /api/devices/{device_id}/commands?cmd=take_photo
```

这是比 wake 更中性的后台命令入口，适合统一接入拍照、唤醒、状态刷新等远程动作。

## 配置说明

默认配置文件是 config.json，主要配置项如下：

- server.tcp_port：设备 TCP 接入端口，默认 9000。
- server.http_port：HTTP 管理端口，默认 8080。
- server.heartbeat_default_sec：默认心跳周期。
- server.idle_timeout_sec：设备空闲超时时间。
- redis.host / redis.port / redis.password / redis.db：Redis 连接参数。
- redis.prefix：Redis 键前缀。
- auth.enable：是否启用鉴权。
- auth.dev_default_secret：开发环境默认密钥。
- wake.ack_timeout_ms：唤醒确认超时。
- wake.max_retry：最大重试次数。
- wake.expire_ms：唤醒消息过期时间。
- server.worker_threads：io_context 工作线程数，默认可配置。
- mqtt.enable：是否启用 MQTT 后台桥接。
- mqtt.host / mqtt.port：MQTT Broker 地址。
- mqtt.topic_prefix：MQTT 主题前缀。
- mqtt.command_request_topic：后台命令请求主题模板。

## 构建方式

项目使用 CMake 构建，目标程序名为 lowpower_tcp_gateway。

### 依赖

编译需要以下基础依赖：

- C++17 编译器
- CMake 3.16+
- OpenSSL
- hiredis
- Asio
- nlohmann/json

### 编译命令

```bash
cmake -S . -B build
cmake --build build -j
```

### 启动命令

```bash
./build/lowpower_tcp_gateway config.json
```

## 适用场景

这个项目适合做低功耗物联网设备的接入层或边缘网关，例如：

- 电池供电的远程传感器
- 低频唤醒拍照设备
- 需要平台侧查询在线状态的终端
- 需要通过管理接口远程唤醒设备执行任务的场景
