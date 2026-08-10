# OB-USP-AGENT (obuspa) 项目总结

## 项目概述

**OB-USP-Agent** 是 **Broadband Forum（宽带论坛）** 主导的开源项目，是 **用户服务平台 (User Services Platform, USP)** 规范的一个**参考实现**，从「Agent（代理）」的角度实现。当前版本为 **v11.0.7**。

许可证：**BSD-3-Clause**

---

## 什么是 USP？

USP 是一种**远程管理和控制协议**，将管理实体分为 **Agent（代理）** 和 **Controller（控制器）**：

- **Agent** 运行在 CPE（客户终端设备，如家庭路由器、Wi-Fi 接入点、IoT 网关）中
- **Agent** 暴露一组 **Service Elements**（服务元素），本质是包含对象和参数的数据模型
- **Controller** 远程消费这些服务元素来管理设备

---

## 项目规模

| 指标 | 数值 |
|------|------|
| C 源文件 + 头文件 | **160 个** |
| 总代码行数 | **~142,000 行 C 代码** |
| 最大单文件 | `usp_broker.c` (~358KB)、`data_model.c` (~210KB)、`device_controller.c` (~192KB) |

---

## 核心架构

项目采用模块化的 C 语言架构，核心模块在 `src/core/` 下：

### 1. MTP（消息传输协议）层 — 支持 5 种传输协议

| 协议 | 源文件 | 说明 |
|------|--------|------|
| **STOMP** | `stomp.c` (154KB) | 基于 STOMP 协议（默认） |
| **CoAP** | `coap_client.c`, `coap_server.c`, `coap_common.c` | 基于 CoAP（受限制应用协议） |
| **MQTT** | `mqtt.c` (171KB) | 基于 MQTT 协议 |
| **WebSockets** | `wsclient.c`, `wsserver.c` | WebSocket 客户端/服务端 |
| **UDS** | `uds.c` (109KB) | Unix Domain Socket |

可编译时通过 `--disable-{stomp,coap,mqtt,websockets,uds}` 裁剪。

### 2. 数据模型层

- `data_model.c` (210KB) — 定义和注册整个 USP 数据模型，包含所有标准对象和参数
- `dm_access.c` — 数据模型访问接口
- `dm_exec.c` (105KB) — 数据模型执行引擎
- `dm_trans.c` — 数据模型事务处理
- `database.c` (55KB) — SQLite 持久化存储

### 3. 请求处理层（Handle 层）

实现 USP 协议定义的所有操作消息：

- `handle_get.c` — 处理 Get 请求
- `handle_set.c` — 处理 Set 请求
- `handle_add.c` — 处理 Add（创建实例）请求
- `handle_delete.c` — 处理 Delete 请求
- `handle_operate.c` — 处理 Operate（操作/命令）请求
- `handle_notify.c` — 处理 Notify（事件通知）
- `handle_get_supported_dm.c` — 获取支持的 DM
- `handle_get_supported_protocol.c` — 获取支持的协议版本
- `handle_get_instances.c` — 获取实例列表

### 4. 核心辅助模块

- `usp_broker.c` (358KB) — **UDS 消息代理**，进程间通信的核心
- `usp_record.c` — USP 记录编解码（基于 **Protocol Buffers**）
- `msg_handler.c` (82KB) — 消息分发和路由
- `path_resolver.c` (115KB) — 对象路径解析器（支持通配符、搜索表达式等）
- `usp_register.c` (66KB) — 服务元素注册管理
- `se_cache.c` (59KB) — 服务元素缓存
- `cli_server.c` (72KB) — CLI（命令行接口）服务器
- `e2e_context.c` — 端到端上下文传递

### 5. 设备特定模块（Device 系列）

- `device_controller.c` (192KB) — 控制器连接管理
- `device_security.c` (140KB) — 安全/TLS 证书管理
- `device_subscription.c` (148KB) — 订阅/通知机制
- `device_bulkdata.c` (124KB) — 批量数据采集
- `device_mtp.c` (85KB) — MTP 协议配置
- `device_mqtt.c` (91KB) — MQTT 设备特定配置
- `device_stomp.c` (53KB) — STOMP 设备配置
- `device_ctrust.c` (131KB) — 证书信任管理
- `device_iplcap.c` (64KB) — IP 能力
- `device_local_agent.c` (46KB) — 本地 Agent
- `device_time.c` — 时间/NTP 管理

### 6. 第三方及依赖库

- `src/protobuf-c/` — 内嵌的 protobuf-c 实现（用于 USP Record 和 USP Message 的 Protobuf 编解码）
- `src/libjson/` — 内嵌的 json 库（来自 ccan 项目）
- `src/vendor/` — 厂商扩展框架

### 7. 外部依赖

- **OpenSSL** — TLS/DTLS 加密
- **SQLite** — 数据库持久化
- **libcurl** — HTTP 通信
- **zlib** — 压缩
- **libmosquitto**（可选）— MQTT 支持
- **libwebsockets**（可选）— WebSocket 支持

---

## API 接口

- **`usp_api.h`** — USP Agent 核心暴露给 Vendor 的功能 API，包括数据模型注册、参数读写、事件通知等
- **`vendor_api.h`** — 厂商需要实现的回调接口，便于扩展自定义数据模型

---

## 构建系统

- **主构建系统**：GNU Autotools（`./configure && make`）
- **备用构建系统**：CMake（`CMakeLists.txt`）
- 支持 **Docker** 构建（`Dockerfile`）

---

## 测试与合规

- `conformance_test_results.txt` — 记录了 **TP-469 一致性测试**的详细结果，**全部 Pass**（覆盖 STOMP、CoAP、MQTT、UDS 等多种 MTP 下的 Add/Set/Delete/Get/Operate/Notify 操作）
- 项目已被 **prpl Foundation 的 prplWare** 和 **RDK 的 RDK-B** 集成使用

---

## 一句话总结

> OB-USP-Agent 是一个**生产级的、高度模块化的 C 语言 USP 协议 Agent 参考实现**，支持 5 种传输协议（STOMP/CoAP/MQTT/WebSocket/UDS），实现完整的数据模型管理、安全认证、订阅通知、批量数据采集等功能，总计约 14 万行代码，已通过 Broadband Forum TP-469 完整一致性测试，被 prplWare 和 RDK-B 等主流 CPE 软件平台集成。
