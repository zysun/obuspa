# OB-USP-Agent 软件架构

> 基于 main 函数入口的代码走读分析

---

## 一、整体架构：多线程 + 消息队列

```
┌────────────────────────────────────────────────────────────────────┐
│                         main() 线程                                 │
│                      (Data Model Thread)                           │
│                                                                     │
│  DM_EXEC_Main()  ── 主事件循环 (select-based)                      │
│     ├─ CLI_SERVER     (Unix Domain Socket)                         │
│     ├─ main_mq        接收 MTP 发来的 USP Record                    │
│     ├─ filter_mq      USP Broker 消息过滤                           │
│     └─ SYNC_TIMER     定时器                                       │
│         ↓                                                          │
│     MSG_HANDLER_HandleBinaryRecord() → HandleUspMessage()           │
│         ↓                                                          │
│     HandleGet / HandleSet / HandleAdd / HandleDelete / ...         │
└────────────────────────────────────────────────────────────────────┘
          ▲                    │
  dm_exec msg queue     MSG_HANDLER_QueueMessage()
  (Unix socket pair)    → QueueUspNoSessionRecord()
          │                    ↓
┌─────────┴──────────┐  ┌─────┴──────────────────────────────┐
│  MTP Thread: STOMP │  │  MTP Thread: MQTT   │  MTP: CoAP  │
│  MTP_EXEC_StompMain│  │  MTP_EXEC_MqttMain  │  ...        │
│    ↓ mq_stomp_rx   │  │    ↓ mq_mqtt_rx     │             │
│  STOMP_ProcessAll  │  │  MQTT_ProcessAll    │             │
│    ↓               │  │    ↓                │             │
│  DM_EXEC_PostUsp   │  │  DM_EXEC_PostUsp    │             │
│   Record()         │  │   Record()          │             │
│    ↑   ↓           │  │    ↑   ↓            │             │
│  TCP/TLS socket    │  │  TCP/TLS socket     │             │
└────────┬───────────┘  └────┬────────────────┴─────────────┘
         │                   │
    Controller          Controller
```

**核心思想**：主线程负责数据模型和消息分发，每种 MTP 协议独立线程负责网络 I/O，线程间通过 Unix socketpair 作为消息队列通信，使用 `select()` 实现非阻塞事件循环。

---

## 二、启动流程

```
main()
 ├─ OS_UTILS_SetDataModelThread()        标记本线程为数据模型线程
 ├─ USP_LOG_Init()                       日志初始化
 ├─ USP_ERR_Init()                       错误处理初始化
 ├─ USP_MEM_Init()                       内存管理初始化
 ├─ getopt_long_only()                   解析命令行参数
 │
 ├─ MAIN_Start(db_file)
 │   ├─ OPENSSL_init_ssl()              SSL 初始化
 │   ├─ curl_global_init()              libcurl 初始化
 │   ├─ SYNC_TIMER_Init()               定时器模块
 │   ├─ signal(SIGPIPE, SIG_IGN)        忽略 SIGPIPE
 │   ├─ DATABASE_Init(db_file)          SQLite 数据库初始化
 │   ├─ DM_EXEC_Init()                  创建跨线程消息队列(socketpair)
 │   ├─ MTP_EXEC_Init()                 创建各 MTP 的消息队列
 │   ├─ BDC_EXEC_Init()                 Bulk Data Collection 队列
 │   ├─ RETRY_WAIT_Init()              重试等待初始化
 │   ├─ DATA_MODEL_Init()              注册整个数据模型 Schema
 │   ├─ DATA_MODEL_Start()             启动数据模型对象
 │   └─ DEVICE_CONTROLLER_StartAllMtpClients()  启动所有 MTP 连接
 │
 ├─ OS_UTILS_CreateThread("MTP_STOMP")  创建 STOMP 线程
 ├─ OS_UTILS_CreateThread("MTP_CoAP")   创建 CoAP 线程
 ├─ OS_UTILS_CreateThread("MTP_MQTT")   创建 MQTT 线程
 ├─ OS_UTILS_CreateThread("MTP_WS...")  创建 WebSocket 线程
 ├─ OS_UTILS_CreateThread("MTP_UDS")    创建 UDS 线程
 ├─ OS_UTILS_CreateThread("BulkData")   创建批量数据采集线程
 │
 └─ DM_EXEC_Main()  ← 主线程在此进入事件循环，永不返回
```

---

## 三、数据流转

### 3.1 入站（Controller → Agent）

```
  Controller
     │  TCP/TLS
     ▼
  MTP Thread (STOMP/MQTT/CoAP/WebSocket/UDS)
     │  接收网络原始帧，解析 MTP 协议头
     │  提取出 USP Record (Protobuf 二进制)
     │  例如 STOMP: 解析 STOMP frame → 提取 payload → pbuf
     ▼
  DM_EXEC_PostUspRecord()
     │  通过 socketpair 发送 dm_exec_msg_t 到主线程
     │  类型为 kDmExecMsg_ProcessUspRecord
     ▼
  DM_EXEC_Main() 主循环
     │  ProcessMessageQueueSocketActivity()
     │  recv() 读取 dm_exec_msg_t
     ▼
  MSG_HANDLER_HandleBinaryRecord()
     ├─ usp_record__record__unpack()     Protobuf 解包 USP Record
     ├─ ValidateUspRecord()              校验合法性和权限
     └─ MSG_HANDLER_HandleBinaryMessage()
         ├─ usp__msg__unpack()           Protobuf 解包 USP Message
         ├─ DEVICE_CONTROLLER_GetCombinedRole 获取角色权限
         └─ HandleUspMessage()
             ├─ USP_SERVICE 检查（USP Broker/Service 场景）
             ├─ USP_BROKER_AttemptPassthru()  转发给 USP Service
             └─ switch(usp_msg_type):
                  ├─ MSG_HANDLER_HandleGet()         → handle_get.c
                  ├─ MSG_HANDLER_HandleSet()         → handle_set.c
                  ├─ MSG_HANDLER_HandleAdd()         → handle_add.c
                  ├─ MSG_HANDLER_HandleDelete()      → handle_delete.c
                  ├─ MSG_HANDLER_HandleOperate()     → handle_operate.c
                  ├─ MSG_HANDLER_HandleNotifyResp()  → handle_notify.c
                  ├─ MSG_HANDLER_HandleGetSupportedDM()
                  ├─ MSG_HANDLER_HandleGetInstances()
                  ├─ MSG_HANDLER_HandleGetSupportedProtocol()
                  ├─ HandleNotification()            处理 Notify
                  └─ HandleUspError()                处理 Error
```

### 3.2 出站（Agent → Controller）

```
  HandleXxx() 处理完请求
     │  构造响应 USP Message (Protobuf)
     ▼
  MSG_HANDLER_QueueMessage()
     ├─ usp__msg__pack()                    序列化 USP Message
     └─ MSG_HANDLER_QueueUspRecord()
         ├─ usp_record__record__pack()      封装为 USP Record
         └─ DEVICE_CONTROLLER_QueueBinaryRecordToSend()
             └─ MTP_EXEC_AddToSendQueue()   放入对应 MTP 发送队列
     ▼
  MTP Thread
     │  STOMP_ProcessAllSocketActivity()  检测到发送队列有数据
     │  / MQTT_ProcessAllSocketActivity()
     │  取出 mtp_send_item_t，添加 MTP 帧头
     │  send()  TCP/TLS 发出
     ▼
  Controller
```

### 3.3 USP Record 的两层封装

```
┌──────────────────────────────────────┐
│         MTP Frame (STOMP/MQTT/...)   │  ← MTP 协议头部
├──────────────────────────────────────┤
│         USP Record (Protobuf)        │  ← 协议版本、from_id、to_id、签名
│  ┌────────────────────────────────┐  │
│  │    USP Message (Protobuf)      │  │  ← Get/Set/Add/Delete/Operate 等
│  │    header: msg_id, msg_type    │  │
│  │    body: req / resp / error    │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
```

- **USP Message** (`usp-msg.pb-c.c`)：定义 Get/Set/Add/Delete/Operate/Notify 等操作
- **USP Record** (`usp-record.pb-c.c`)：在 Message 外层加上路由信息、安全签名等

### 3.4 CLI 通信模式

同一可执行文件有两种运行模式：

```
  模式1：Agent 服务端 (默认)
    main() → DM_EXEC_Main() → CLI_SERVER_Init()
       创建 Unix Domain Socket 监听
       接收 CLI 命令，调用 CLI_SERVER_ExecuteCliCommand()

  模式2：CLI 客户端 (-c 参数)
    main() → CLI_CLIENT_ExecCommand()
       ├─ 本地命令(version/help/dbset/dbget):
       │     HandleCliCommandLocally()
       │     直接打开 DB 操作，不连 Agent
       └─ 其他命令(get/set/add/del/...):
            HandleCliCommandRemotely()
            通过 UDS socket 连接到运行中的 Agent
            发送命令字符串，打印返回结果
```

---

## 四、核心模块分层

```
┌──────────────────────────────────────────┐
│          Handle 层（USP 消息处理）        │
│  handle_get/set/add/delete/operate/      │
│  notify/get_instances/get_supported_dm/  │
│  get_supported_protocol                  │
├──────────────────────────────────────────┤
│          Data Model 层                    │
│  data_model.c    — 注册 Schema           │
│  dm_access.c     — 参数读写              │
│  dm_exec.c       — 事件循环 + 队列       │
│  dm_trans.c      — 事务                  │
│  path_resolver.c — 路径解析(通配符/搜索)  │
│  database.c      — SQLite 持久化         │
│  se_cache.c      — Service Element 缓存  │
├──────────────────────────────────────────┤
│          USP 协议编解码层                 │
│  usp_record.c    — USP Record 编解码     │
│  protobuf-c/     — Protobuf 序列化       │
│  msg_handler.c   — 消息路由/分发         │
│  e2e_context.c   — 端到端会话(分段/重组) │
├──────────────────────────────────────────┤
│          MTP 传输层                       │
│  stomp.c / mqtt.c / coap_client+server.c │
│  wsclient.c+wsserver.c / uds.c           │
├──────────────────────────────────────────┤
│          Device 设备管理层               │
│  device_controller.c   — 控制器管理      │
│  device_security.c     — TLS/证书        │
│  device_subscription.c — 订阅/通知       │
│  device_bulkdata.c     — 批量数据        │
│  device_mtp.c          — MTP 配置        │
│  device_ctrust.c       — 信任链          │
├──────────────────────────────────────────┤
│          基础设施                         │
│  sync_timer.c   — 定时器                 │
│  usp_log.c      — 日志                   │
│  usp_mem.c      — 内存管理               │
│  os_utils.c     — 线程/OS 工具           │
│  retry_wait.c   — 重试退避               │
│  text_utils.c   — 字符串工具             │
│  iso8601.c      — 时间格式化             │
│  nu_ipaddr.c    — IP 地址工具            │
└──────────────────────────────────────────┘
```

---

## 五、关键设计要点

### 5.1 线程模型

| 线程 | 入口函数 | 职责 |
|------|----------|------|
| 主线程 (Data Model) | `DM_EXEC_Main()` | 消息分发、数据模型操作、定时器、CLI |
| STOMP MTP | `MTP_EXEC_StompMain()` | STOMP 协议网络 I/O |
| CoAP MTP | `MTP_EXEC_CoapMain()` | CoAP 协议网络 I/O |
| MQTT MTP | `MTP_EXEC_MqttMain()` | MQTT 协议网络 I/O |
| WebSocket Client | `WSCLIENT_Main()` | WebSocket 客户端 I/O |
| WebSocket Server | `WSSERVER_Main()` | WebSocket 服务端 I/O |
| UDS MTP | `MTP_EXEC_UdsMain()` | Unix Domain Socket I/O |
| Bulk Data | `BDC_EXEC_Main()` | 批量数据采集 HTTP POST |

所有 MTP 线程结构对称：`select()` + `ProcessAllSocketActivity()` + 消息队列收发。

### 5.2 线程间通信

- 使用 Unix `socketpair()` 创建双向消息队列
- 主线程与每个 MTP 线程各有一对 socketpair
- 消息结构 `dm_exec_msg_t` 包含类型枚举（约 20 种消息类型）
- 发送侧用 `send()` + `MSG_DONTWAIT` 非阻塞
- 设计保证了消息不丢失、不阻塞

### 5.3 权限控制

每个入站消息处理前：
1. 解析消息来源 Endpoint ID
2. 调用 `DEVICE_CONTROLLER_GetCombinedRoleByEndpointId()` 查找该 Controller 在 `Device.LocalAgent.ControllerTrust.Role.{i}` 中分配的权限
3. `path_resolver.c` 对所有访问路径进行权限过滤，拒绝越权访问

### 5.4 USP Broker 模式

Agent 可作为 **USP Broker** 将消息透传给下游的 **USP Service**：
- `usp_broker.c`（358KB，最大源文件）实现注册、查找、转发
- USP Service 通过 `--register` 参数注册其负责的数据模型路径
- Broker 收到请求后，若目标路径属于某 Service，则直接透传
- 支持同步请求-响应模式：`DM_EXEC_SendRequestAndWaitForResponse()`

### 5.5 MTP 可插拔设计

所有 MTP 协议有统一的接口模式：
- `XXX_UpdateAllSockSet()` — 注册 socket 到 select
- `XXX_ProcessAllSocketActivity()` — 处理网络事件
- `XXX_AreAllResponsesSent()` — 优雅退出检查
- `XXX_Destroy()` — 释放资源
- 通过编译时宏 `ENABLE_STOMP` / `ENABLE_MQTT` / `ENABLE_COAP` / `ENABLE_WEBSOCKETS` / `ENABLE_UDS` 控制裁剪

---

## 六、关键源文件速查

| 文件 | 大小 | 说明 |
|------|------|------|
| `main.c` | 20KB | 程序入口、命令行解析、启动/停止 |
| `usp_broker.c` | 358KB | USP Broker：注册、查找、消息透传 |
| `data_model.c` | 210KB | 数据模型 Schema 注册 |
| `device_controller.c` | 192KB | Controller 连接管理 |
| `mqtt.c` | 171KB | MQTT 协议实现 |
| `stomp.c` | 154KB | STOMP 协议实现 |
| `device_subscription.c` | 148KB | 订阅/通知机制 |
| `device_security.c` | 140KB | TLS 证书/安全 |
| `device_ctrust.c` | 131KB | 证书信任管理 |
| `device_bulkdata.c` | 124KB | 批量数据采集 |
| `path_resolver.c` | 115KB | 路径解析(通配符/搜索表达式) |
| `uds.c` | 109KB | Unix Domain Socket MTP |
| `dm_exec.c` | 105KB | 主事件循环、消息队列 |
| `msg_handler.c` | 82KB | 消息路由分发 |
| `usserver.c` | 82KB | WebSocket 服务端 |
| `wsclient.c` | 102KB | WebSocket 客户端 |
| `cli_server.c` | 72KB | CLI 命令服务端 |
| `coap_server.c` | 86KB | CoAP 服务端 |
| `coap_client.c` | 64KB | CoAP 客户端 |
| `msg_utils.c` | 51KB | 消息工具函数 |
| `database.c` | 55KB | SQLite 数据库 |
| `se_cache.c` | 59KB | Service Element 缓存 |
| `usp_register.c` | 66KB | 服务元素注册 |
| `dm_access.c` | 36KB | 数据模型访问接口 |
| `text_utils.c` | 74KB | 字符串/文本工具 |

---

## 七、多线程同步机制

该项目没有使用锁作为主要同步手段，而是采用了一套以 **Unix socketpair 消息队列 + select 事件循环** 为核心的同步架构，辅以少量 mutex 和条件变量。

### 7.1 核心机制：socketpair 消息队列

线程之间**不共享数据结构**，而是通过 `socketpair(AF_UNIX, SOCK_DGRAM)` 创建双向消息通道：

```
主线程(DM)                     MTP线程(STOMP)
──────────                     ──────────────
main_mq_tx_socket  ──send()──→ mq_stomp_rx_socket
main_mq_rx_socket  ←──recv()──  mq_stomp_tx_socket
```

**初始化**（`DM_EXEC_Init` / `MTP_EXEC_Init`）：

```c
// DM_EXEC_Init() 中创建主消息队列
err = socketpair(AF_UNIX, SOCK_DGRAM, 0, main_mq_sockets);
// main_mq_sockets[0] = 读端, main_mq_sockets[1] = 写端

// MTP_EXEC_Init() 中为每个 MTP 创建独立的唤醒队列
err = socketpair(AF_UNIX, SOCK_DGRAM, 0, mtp_stomp_mq_sockets);
err = socketpair(AF_UNIX, SOCK_DGRAM, 0, mtp_mqtt_mq_sockets);
err = socketpair(AF_UNIX, SOCK_DGRAM, 0, mtp_coap_mq_sockets);
```

**关键设计——非阻塞发送**：所有 `send()` 都带 `MSG_DONTWAIT` 标志，队列满时**直接丢弃**而非阻塞，避免死锁：

```c
bytes_sent = send(main_mq_tx_socket, &msg, sizeof(msg), MSG_DONTWAIT);
if (bytes_sent != sizeof(msg))
{
    // 队列满，丢弃消息并记录错误，不会阻塞调用线程
    FreeDmExecMessageArguments(&msg);
}
```

### 7.2 消息结构：统一的 `dm_exec_msg_t`

所有跨线程消息使用同一个结构体，通过 `type` 字段区分约 20 种消息类型：

```c
typedef struct {
    int type;  // 消息类型枚举
    union {
        process_usp_record_msg_t  usp_record;    // MTP→DM: 收到的 USP Record
        stomp_complete_msg_t      stomp_complete; // MTP→DM: STOMP 握手完成
        mqtt_complete_msg_t       mqtt_complete;  // MTP→DM: MQTT 握手完成
        mtp_thread_exited_msg_t   mtp_exited;     // MTP→DM: MTP 线程已退出
        do_work_msg_t             do_work;        // 外部→DM: 请求在 DM 线程执行回调
        // ...共约 20 种消息类型
    } params;
} dm_exec_msg_t;
```

### 7.3 Mutex：保护模块内部状态

每个主要模块持有一个 **递归互斥锁**（`PTHREAD_MUTEX_RECURSIVE`），保护该模块的全局数据结构：

| Mutex | 所在文件 | 保护范围 |
|-------|----------|----------|
| `dm_access_mutex` | `dm_exec.c` | DM 主循环整段代码，在 `select()` 期间解锁 |
| `stomp_access_mutex` | `stomp.c` | STOMP 所有连接列表 |
| `mqtt_access_mutex` | `mqtt.c` | MQTT 所有连接列表 |
| `coap_access_mutex` | `coap_common.c` | CoAP 所有连接列表 |
| `uds_access_mutex` | `uds.c` | UDS 所有连接列表 |
| `wsc_access_mutex` | `wsclient.c` | WebSocket 客户端连接 |
| `wss_access_mutex` | `wsserver.c` | WebSocket 服务端连接 |
| `mem_access_mutex` | `usp_mem.c` | 内存分配追踪链表 |
| `fd_vector_mutex` | `fd_vector.c` | 文件描述符全局映射表 |
| `can_mtp_connect_mutex` | `device_controller.c` | MTP 连接就绪标志 |

所有 mutex 通过 `OS_UTILS_InitMutex()` 统一创建：

```c
int OS_UTILS_InitMutex(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);  // ★ 递归锁
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
```

使用递归锁的原因是：mutex 持有者可能在持有锁期间，调用另一个也需要相同锁的函数（例如从 dm_exec 调用 stomp 的函数，stomp 内部又需要 `stomp_access_mutex`）。

### 7.4 核心同步模式：`dm_access_mutex` + `select()` 的锁释放

这是整个项目最精妙的设计。主线程在 `select()` 等待期间**释放锁**，让其他线程有机会向消息队列写入：

```c
void *DM_EXEC_Main(void *args)
{
    OS_UTILS_LockMutex(&dm_access_mutex);       // 获取主锁

    while (FOREVER)
    {
        UpdateSockSet(&set);                    // 准备 socket 集合

        OS_UTILS_UnlockMutex(&dm_access_mutex); // ★ 释放锁 ★
        num_sockets = SOCKET_SET_Select(&set);  // ★ 阻塞等待 I/O 事件 ★
        OS_UTILS_LockMutex(&dm_access_mutex);   // ★ 重新获取锁 ★

        ProcessSocketActivity(&set);            // 处理消息（受锁保护）
        SYNC_TIMER_Execute();                   // 处理定时器
        DEVICE_SUBSCRIPTION_ProcessAllObjectLifeEventSubscriptions();
        DM_INST_VECTOR_NextLockPeriod();
        // 检查退出标志...
    }
}
```

```
时间轴：
  Lock ──→ Unlock ──→ [select() 阻塞等待] ──→ Lock ──→ [处理消息] ──→ Unlock ──→ ...

在 select() 阻塞期间（锁释放）：
  • MTP 线程可以 send() 消息到消息队列
  • CLI 客户端可以连接并发送命令
  • 定时器到期...

消息处理期间（锁持有）：
  • 保证数据模型状态的一致性
  • 不会有并发修改 DM 数据
```

这意味着：
- **消息处理期间**：持有锁，数据模型状态一致
- **`select()` 等待期间**：释放锁，MTP 线程可以安全地向消息队列 `send()`
- MTP 线程的 `send()` 是非阻塞的（`MSG_DONTWAIT`），不会因等锁而死锁

### 7.5 同步请求-响应：`DoWorkSync` 模式（条件变量）

当外部线程需要**在 DM 线程中执行一段代码并等待结果**时使用：

```c
int USP_PROCESS_DoWorkSync(do_work_cb_t cb, void *arg1, void *arg2)
{
    work_sync_ctx_t ctx = { .done = false };
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    // 1. 向 DM 线程投递 DoWork 消息（非阻塞 send）
    USP_PROCESS_DoWork(DoWorkSyncCallback, &ctx, cb);

    // 2. 阻塞等待 DM 线程执行完毕并发回信号
    pthread_mutex_lock(&ctx.mutex);
    while (!ctx.done)
        pthread_cond_wait(&ctx.cond, &ctx.mutex);
    pthread_mutex_unlock(&ctx.mutex);

    return ctx.err;
}

// DM 线程中执行的回调：执行完成后发信号
static void DoWorkSyncCallback(void *arg1, void *arg2) {
    work_sync_ctx_t *ctx = arg1;
    do_work_cb_t cb = (do_work_cb_t)arg2;
    ctx->err = cb(ctx->arg1, ctx->arg2);  // 在 DM 线程中执行
    pthread_mutex_lock(&ctx->mutex);
    ctx->done = true;
    pthread_cond_signal(&ctx->cond);      // 唤醒等待者
    pthread_mutex_unlock(&ctx->mutex);
}
```

**使用场景**：Vendor API 中的同步读取数据模型参数（`USP_PROCESS_DM_GetParameterValue` 等）。

### 7.6 USP Broker 同步等待：Filter Queue 模式

当 Broker 向 USP Service 发请求并等待响应时，使用 "分流队列" 机制：

```c
Usp__Msg *DM_EXEC_SendRequestAndWaitForResponse(endpoint_id, req, mtpc, timeout)
{
    // 1. 设置分流标志——后续收到的 USP Record 都导向 filter_mq 而非 main_mq
    divert_to_filter_queue = true;

    // 2. 发送请求消息
    MSG_HANDLER_QueueMessage(endpoint_id, req, mtpc);

    // 3. 在 filter_mq 上阻塞等待响应（带超时）
    while (cur_time < end_time)
    {
        SOCKET_SET_Select(&set);
        recv(filter_mq_rx_socket, &msg, sizeof(msg), 0);

        // 4. 检查是否是我们要的响应 msg_id
        resp = IsMatchingMsgId(&msg, wanted_msg_id, ...);
        if (resp) {
            divert_to_filter_queue = false;  // 恢复正常流向
            return resp;                     // 返回响应给调用者
        }

        // 5. 不匹配的消息转发到 main_mq 给主循环处理
        send(main_mq_tx_socket, &msg, sizeof(msg), MSG_DONTWAIT);
    }

    return NULL;  // 超时
}
```

```
正常模式（divert_to_filter_queue = false）：
  MTP → DM_EXEC_PostUspRecord() → main_mq → DM_EXEC_Main() 处理

分流模式（divert_to_filter_queue = true）：
  MTP → DM_EXEC_PostUspRecord() → filter_mq → SendRequestAndWaitForResponse() 检查
                                                   │
                                                   ├── 匹配: 返回响应
                                                   └── 不匹配: 转发到 main_mq
```

### 7.7 唤醒机制

每个 MTP 线程有自己的 **唤醒 socketpair**。当 DM 线程需要唤醒 MTP 线程（例如有新消息要发送）时：

```c
void MTP_EXEC_MqttWakeup(void)
{
    char c = WAKEUP_MESSAGE;  // 'W'
    send(mq_mqtt_tx_socket, &c, 1, MSG_DONTWAIT);
}
```

MTP 线程的 `select()` 检测到唤醒 socket 可读时，调用 `ProcessMtpWakeupQueueSocketActivity()` 将唤醒字节读走，然后继续处理实际消息队列。

### 7.8 优雅退出

```
SIGTERM → SigTermHandler()
  │
  └─→ dm_exit_scheduled = true
        │
        └─→ DM_EXEC_Main() 检测到标志
              ├─ BDC_EXEC_ScheduleExit()
              ├─ MTP_EXEC_ScheduleExit()       → mtp_exit_scheduled = kScheduledAction_Signalled
              └─ MTP_EXEC_ActivateScheduledActions()
                    │
                    └─→ 各 MTP 线程:
                          1. 发送完队列中所有待发响应
                          2. STOMP_Destroy() / MQTT_Destroy() 释放网络资源
                          3. DM_EXEC_PostMtpThreadExited(STOMP_EXITED | MQTT_EXITED | ...)
                          4. pthread_exit()
                              │
                    cumulative_mtp_threads_exited 累积退出位掩码
                              │
                    所有 MTP 退出后 → DM 主线程释放所有资源 → 进程退出
```

退出采用 **分阶段信号** 设计（`scheduled_action_t` 枚举）：

| 阶段 | 枚举值 | 含义 |
|------|--------|------|
| 0 | `kScheduledAction_Off` | 未计划退出 |
| 1 | `kScheduledAction_Signalled` | 已标记（但不立即执行——等响应消息先入队） |
| 2 | `kScheduledAction_Activated` | 已激活（所有响应发送完后立即退出） |

这样保证了**退出前所有未发送的 USP 响应都能被送达 Controller**，避免数据丢失。

### 7.9 同步机制总结

| 机制 | 用途 | 关键 API |
|------|------|----------|
| **socketpair 消息队列** | 主要的跨线程通信 | `socketpair()` + `send(sock, &msg, MSG_DONTWAIT)` |
| **递归 Mutex** | 保护各模块全局数据 | `pthread_mutex_t` + `PTHREAD_MUTEX_RECURSIVE` |
| **dm_access_mutex + select 解锁** | DM 主循环在 I/O 等待时让出锁 | `Lock → select → Unlock → Lock` 循环 |
| **条件变量** | 同步等待 DM 线程执行完回调 | `pthread_cond_wait` / `pthread_cond_signal` |
| **Filter Queue 分流** | Broker 同步请求-响应等待 | `divert_to_filter_queue` + 阻塞等待 |
| **唤醒 socketpair** | DM 通知 MTP 有新数据要发送 | 单字节 `send()` 作为信号 |
| **SIGTERM 信号** | 优雅退出触发 | `sigaction(SIGTERM, ...)` |
| **分阶段退出** | 确保退出前所有响应已发送 | `mtp_exit_scheduled` 三阶段状态机 |

**整体设计哲学**：以消息传递代替共享内存，以非阻塞 send 避免死锁，以 select 前的锁释放保证吞吐，以分阶段退出保证数据不丢失。
