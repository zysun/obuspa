# dm_access_mutex 锁竞争分析与优化方案

## 问题描述

`dm_access_mutex` 是 `DM_EXEC_Main` 主循环中使用的递归互斥锁，保护整个数据模型消息处理流程。当 Bulk Data Collection (BDC) 的定时器回调在持锁期间执行耗时的数据采集和报告生成时，会阻塞所有入站 USP 消息处理（Get、Set 等），造成 Controller 端的请求超时。

---

## 问题链路追踪

### 锁持有范围

`dm_access_mutex` 的持有者只有 `DM_EXEC_Main`（`dm_exec.c:1947-1960`）：

```c
// dm_exec.c
OS_UTILS_LockMutex(&dm_access_mutex);          // ★ 上锁

while (FOREVER)
{
    UpdateSockSet(&set);

    OS_UTILS_UnlockMutex(&dm_access_mutex);     // select 期间释放
    num_sockets = SOCKET_SET_Select(&set);       // 阻塞等待 I/O 事件
    OS_UTILS_LockMutex(&dm_access_mutex);        // select 结束重新上锁

    ProcessSocketActivity(&set);                 // 处理 Get/Set/Add/Delete...
    SYNC_TIMER_Execute();                        // ★ 定时器回调在持锁状态执行
    DEVICE_SUBSCRIPTION_ProcessAllObjectLifeEventSubscriptions();
    DM_INST_VECTOR_NextLockPeriod();
}
```

### BDC 定时器回调的执行路径

```
DM_EXEC_Main()                         [持有 dm_access_mutex]
  │
  └─ SYNC_TIMER_Execute()              ★ 全程持锁 ★
       │
       └─ bulkdata_process_profile()   ← 注册为 SYNC_TIMER 回调
            │
            ├─ DEVICE_CONTROLLER_CanMtpConnect()
            ├─ DM_ACCESS_GetEnum()              读取 Protocol 枚举
            │
            ├─ bulkdata_process_profile_http()
            │   ├─ bulkdata_platform_get_profile_control_params()
            │   │    └─ DATA_MODEL_GetParameterValue() × N     ← 读控制参数
            │   ├─ bulkdata_calc_report_map()
            │   │    ├─ bulkdata_platform_get_param_refs()     ← 获取所有参数引用
            │   │    └─ GROUP_GET_VECTOR_GetValues()           ← 批量读取参数值
            │   ├─ bulkdata_generate_json_report()
            │   │    └─ 遍历 report_map，逐条转为 JSON           ← CPU 密集
            │   ├─ bulkdata_compress_report()
            │   │    └─ zlib deflate()                          ← CPU 密集
            │   └─ BDC_EXEC_PostReportToSend()                  ← 投递到 BDC 线程
            │
            ├─ bulkdata_process_profile_usp_event()  （类似流程）
            └─ bulkdata_process_profile_mqtt()       （类似流程）
```

### 时间分布估算（1000 个参数的报告）

| 阶段 | 典型耗时 | 是否持锁 |
|------|---------|---------|
| 参数采集 (`GROUP_GET_VECTOR_GetValues`) | ~50ms | ✅ 持锁 |
| JSON 序列化 (`bulkdata_generate_json_report`) | ~100ms | ✅ 持锁 |
| zlib 压缩 (`bulkdata_compress_report`) | ~200ms | ✅ 持锁 |
| 投递到 BDC 线程 (`BDC_EXEC_PostReportToSend`) | < 1ms | ✅ 持锁 |
| **合计** | **~350ms** | — |

在 350ms 内，所有 Controller 发来的 Get/Set/Add/Delete 请求被阻塞在消息队列中无法处理。

---

## 优化方案

### 方案 1：锁内轻量收集，重操作移到 BDC 线程（推荐）

**核心思路**：将耗时的 JSON 序列化和 zlib 压缩从 DM 线程移到 BDC 线程。DM 线程只做参数采集（轻量），BDC 线程负责 JSON 构建、压缩和 HTTP 发送（重量）。

```
改造前:
  DM 线程 [持锁] → 采集参数 → 生成JSON → 压缩 → 投递到 BDC

改造后:
  DM 线程 [持锁] → 采集参数 → 投递原始数据到 BDC
  BDC 线程 [无锁] → 生成JSON → 压缩 → HTTP POST
```

**实现要点**：

```c
// device_bulkdata.c - 改造后的 HTTP 发送路径
void bulkdata_process_profile_http(bulkdata_profile_t *bp)
{
    // ====== 阶段 1：DM 线程持锁，仅采集原始数据 ======
    err = bulkdata_calc_report_map(bp, &report_map, bulkdata_role);
    if (err != USP_ERR_OK)
        return;

    err = bulkdata_platform_get_profile_control_params(bp, &ctrl);
    if (err != USP_ERR_OK)
        return;

    // ====== 阶段 2：投递原始 map 到 BDC 线程，立即返回 ======
    // BDC_EXEC_PostReportMapToSend() 将 kv_vector_t 深拷贝后发到 bdc_mq_sockets
    // BDC 线程收到后自己做: JSON 序列化 → zlib 压缩 → HTTP POST
    err = BDC_EXEC_PostReportMapToSend(bp, &report_map, &ctrl);
}

// bdc_exec.c - BDC 线程中新增的处理逻辑
void ProcessBdcReportMap(bdc_exec_msg_t *msg)
{
    report_map_msg_t *rmm = &msg->params.report_map;
    
    // JSON 序列化（在 BDC 线程，不影响 DM 线程）
    char *json = bulkdata_generate_json_report_from_map(&rmm->report_map, rmm->timestamp);
    
    // zlib 压缩（在 BDC 线程）
    unsigned char *compressed = bulkdata_compress_report(&rmm->ctrl, json, len, &compressed_len);
    
    // HTTP POST
    StartSendingCompressedReport(compressed, compressed_len, &rmm->ctrl);
}
```

**效果对比**：

| 操作 | 改造前（持锁） | 改造后（持锁） |
|------|--------------|--------------|
| 参数采集 | ~50ms | ~50ms |
| JSON 序列化 | ~100ms | **0ms**（移出锁） |
| zlib 压缩 | ~200ms | **0ms**（移出锁） |
| 投递 | < 1ms | < 1ms |
| **锁持有总时间** | **~350ms** | **~50ms** |

锁持有时间降低约 **7 倍**。

**优点**：
- 改动集中在 `device_bulkdata.c` 和 `bdc_exec.c`，不影响核心 DM 逻辑
- 锁持有时间大幅降低
- BDC 线程本来就要处理 HTTP POST，JSON 生成和压缩放进来很自然

**缺点**：
- `kv_vector_t` 需要深拷贝传给 BDC 线程，增加一次内存分配
- 需要新增一种 `bdc_exec_msg_t` 消息类型

---

### 方案 2：SYNC_TIMER 分片执行

**核心思路**：把 `bulkdata_process_profile` 拆成多个阶段，每次 `SYNC_TIMER_Execute` 只执行一小片，执行完后重新调度自身。

```c
typedef enum {
    BDC_STAGE_COLLECT_PARAMS,
    BDC_STAGE_BUILD_JSON,
    BDC_STAGE_COMPRESS,
    BDC_STAGE_POST
} bdc_stage_t;

void bulkdata_process_profile(int id)
{
    bulkdata_profile_t *bp = bulkdata_find_profile(id);

    switch (bp->stage)
    {
    case BDC_STAGE_COLLECT_PARAMS:
        bulkdata_calc_report_map(bp, &bp->saved_report_map, &bp->saved_role);
        bp->stage = BDC_STAGE_BUILD_JSON;
        SYNC_TIMER_Reload(bulkdata_process_profile, id, time(NULL));
        break;

    case BDC_STAGE_BUILD_JSON:
        bp->saved_json = bulkdata_generate_json_report(bp, bp->saved_ctrl.report_timestamp);
        bp->stage = BDC_STAGE_COMPRESS;
        SYNC_TIMER_Reload(bulkdata_process_profile, id, time(NULL));
        break;

    case BDC_STAGE_COMPRESS:
        bp->saved_compressed = bulkdata_compress_report(&bp->saved_ctrl, bp->saved_json, len, &len);
        bp->stage = BDC_STAGE_POST;
        SYNC_TIMER_Reload(bulkdata_process_profile, id, time(NULL));
        break;

    case BDC_STAGE_POST:
        BDC_EXEC_PostReportToSend(id, bp->saved_compressed, len, ...);
        bp->stage = BDC_STAGE_COLLECT_PARAMS;
        break;
    }
}
```

```
时间线:
  [迭代 N]   Collect → 重新调度 → select() 可处理 Get/Set → 
  [迭代 N+1] JSON →     重新调度 → select() 可处理 Get/Set →
  [迭代 N+2] Compress → 重新调度 → select() 可处理 Get/Set →
  [迭代 N+3] Post →     完成
```

**优点**：
- 不改变线程模型，纯 DM 线程内改动
- 每次 `select()` 之间都有一个机会处理入站消息

**缺点**：
- 需要在 `bulkdata_profile_t` 中保存中间状态，内存管理复杂
- JSON 序列化、zlib 压缩通常需要整体完成，分片执行价值有限
- 状态机代码冗余

---

### 方案 3：整个数据采集移到 BDC 线程 + 批量 DoWorkSync

**核心思路**：BDC 数据采集完全不在 DM 线程执行，改为 BDC 线程通过 `USP_PROCESS_DoWorkSync` 逐批读取参数。

```c
// BDC 线程的主循环中
void ProcessBdcDataCollection(int profile_id)
{
    // 获取参数引用列表（可以缓存，避免每次都查）
    param_refs = bdc_get_cached_param_refs(profile_id);

    for (int i = 0; i < param_refs.num_entries; i += BATCH_SIZE)
    {
        // 批量读取参数——通过 DoWorkSync 在 DM 线程执行
        // 每次只阻塞 DM 线程 ~微秒级
        USP_PROCESS_DoWorkSync(collect_params_batch_cb, 
                               &param_refs, i, 
                               min(i + BATCH_SIZE, param_refs.num_entries));
    }

    // JSON 生成 + 压缩 在 BDC 线程中完成，完全不影响 DM 线程
    json = generate_json_report(report_map);
    compressed = compress_report(json, len);
    http_post(url, compressed, len);
}
```

**优点**：
- DM 线程的 `dm_access_mutex` 锁持有时间最短（每次仅一个批次读取）
- BDC 线程完全控制数据采集节奏

**缺点**：
- N 个参数需要 N/BATCH_SIZE 次 `DoWorkSync`，每次都有条件变量开销
- DM 线程仍会被短暂阻塞（但远小于 350ms）
- 需要合理选择 `BATCH_SIZE`（建议 100-500）

---

### 方案 4：dm_access_mutex 改为读写锁

**核心思路**：`dm_access_mutex` 从递归互斥锁改为 `pthread_rwlock_t`，让纯读操作（Get、BDC 采集）可以并发，只有写操作（Set、Add、Delete）互斥。

```c
// 当前（互斥锁）
OS_UTILS_LockMutex(&dm_access_mutex);    // 所有操作互斥
ProcessSocketActivity(&set);
SYNC_TIMER_Execute();
OS_UTILS_UnlockMutex(&dm_access_mutex);

// 改造后（读写锁）
// 写操作（Set/Add/Delete 处理时）
pthread_rwlock_wrlock(&dm_access_rwlock);
ProcessSetMessage(usp);
pthread_rwlock_unlock(&dm_access_rwlock);

// 读操作（Get 处理时、BDC 采集参数时）
pthread_rwlock_rdlock(&dm_access_rwlock);
ProcessGetMessage(usp);
pthread_rwlock_unlock(&dm_access_rwlock);
```

**注意**：当前 `dm_access_mutex` 是**递归锁**（`PTHREAD_MUTEX_RECURSIVE`），而 `pthread_rwlock_t` **不支持递归加锁**。改造需要审计所有代码路径，确保不会在同一线程中嵌套加锁。这是一项较大的重构工作。

**优点**：
- 多个 Get/BDC 读操作可以并发执行
- 在高读低写场景下吞吐量大幅提升

**缺点**：
- 读写锁不支持递归，需要大量代码改造
- 仍然不能解决 BDC 在 DM 线程内执行的问题（需要配合方案 3）
- 如果写操作频繁，读写锁的性能可能不如互斥锁

---

## 方案对比

| 维度 | 方案 1：重操作移 BDC | 方案 2：分片执行 | 方案 3：全移 BDC + DoWorkSync | 方案 4：读写锁 |
|------|---------------------|-----------------|------------------------------|---------------|
| 锁持有时间降低 | ★★★★★ (~7x) | ★★★ (每阶段间释放) | ★★★★ | ★★★ (仅读读并发) |
| 代码改动量 | ★★★ 中等 | ★★ 较大 | ★★★★ 较大 | ★★★★★ 很大 |
| 线程模型变化 | 新增 BDC 消息类型 | 不变 | 引入大量 DoWorkSync | 不变但递归→非递归 |
| 风险 | 低 | 中（状态机 bug） | 中（跨线程开销） | 高（回归风险） |
| 推荐度 | ✅ **首选** | 备选 | 与方案 1 结合 | 长期重构 |

---

## 推荐实施路径

### 第一阶段（短期）：方案 1

- 新增 `BDC_EXEC_PostReportMapToSend()`，将原始 `kv_vector_t` 投递到 BDC 线程
- 在 BDC 线程的 `ProcessBdcMessageQueueSocketActivity()` 中增加 JSON 生成 + 压缩逻辑
- 改动范围：`device_bulkdata.c` + `bdc_exec.c`，约 200-300 行新增代码

### 第二阶段（可选）：方案 1 + 部分方案 3

- 如果某些 Profile 的参数特别多（> 10000 条），可考虑将参数采集也分批：
  - DM 线程采集前 500 条 → 释放锁 → 重新调度 → 采集下 500 条
  - 全部采集完后投递到 BDC 线程

### 第三阶段（长期）：方案 4

- 当数据模型稳定且经过充分测试后，考虑将 `dm_access_mutex` 升级为读写锁
- 需要创建完整的递归锁审计清单
