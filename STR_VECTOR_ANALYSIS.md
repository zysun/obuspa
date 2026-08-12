# STR_VECTOR 容器分析

> 针对 `src/core/str_vector.c` 的 API 方法与优化点总结

---

## 一、概述

`str_vector_t` 是 OB-USP-Agent 中基础动态数组容器，用于存储和操作一组字符串。典型用途包括数据模型路径列表、实例名列表等。

### 数据结构

```c
typedef struct {
    char **vector;      // 字符串指针数组
    int num_entries;    // 当前条目数
} str_vector_t;
```

---

## 二、API 方法一览

| 函数 | 功能 | 复杂度 | 备注 |
|------|------|--------|------|
| `STR_VECTOR_Init` | 初始化 | O(1) | 置 `vector=NULL, num_entries=0` |
| `STR_VECTOR_Clone` | 从 `char**` 深拷贝 | O(n) | `USP_STRDUP` 逐条拷贝，`NULL` 转空串 |
| `STR_VECTOR_Add` | 追加字符串 | O(1) 均摊 | `realloc` 扩容 + `strdup` 深拷贝 |
| `STR_VECTOR_Add_IfNotExist` | 去重追加 | O(n) | 先 `Find`，存在则跳过 |
| `STR_VECTOR_Find` | 查找 | O(n) | 线性遍历 + `strcmp`，返回索引或 `INVALID` |
| `STR_VECTOR_RemoveByIndex` | 按索引删除 | O(n) | 释放该串 + `memmove` 前移 + 空时释放数组 |
| `STR_VECTOR_Destroy` | 销毁 | O(n) | 逐条释放 + 释放数组 + 重置 |
| `STR_VECTOR_Dump` | 调试打印 | O(n) | `USP_DUMP` 输出全部条目 |
| `STR_VECTOR_ConvertToKeyValueVector` | 转 KV 向量 | O(n) | **零拷贝**：字符串指针直接变 key |
| `STR_VECTOR_ToList` | 转逗号分隔字符串 | O(n) | `", "` 连接，返回动态分配字符串 |
| `STR_VECTOR_ToSortedList` | 排序后转列表 | O(n log n) | `Sort` + `ToList`，空返回 `NULL` |
| `STR_VECTOR_Compare` | 比较两向量 | O(n) | 假定已排序，逐条 `strcmp` |
| `STR_VECTOR_Sort` | 排序 | O(n log n) | `qsort` + 自然数比较器 |
| `STR_VECTOR_RemoveUnusedEntries` | 压缩空条目 | O(n) | 移除所有 `NULL` 条目并前移 |

---

## 三、设计特点

### 3.1 内存所有权模型

- `Add` / `Clone` 做**深拷贝**（`USP_STRDUP`），调用方传入的字符串所有权不变
- `Destroy` 负责释放所有内部字符串
- 所有内存走项目的 `USP_MALLOC` / `USP_REALLOC` / `USP_SAFE_FREE` 宏（带内存泄漏追踪）

### 3.2 零拷贝转换（`ConvertToKeyValueVector`）

```c
pair->key = sv->vector[i];   // 直接转移指针，不复制
pair->value = NULL;
...
USP_FREE(sv->vector);        // 只释放数组本身，不释放字符串
sv->num_entries = 0;         // 源向量标记为空
```

字符串所有权从 `str_vector_t` 转移给 `kv_vector_t`，避免一次完整内存复制。

### 3.3 自然数排序（Natural Sort）

`STR_VECTOR_Sort` 不是字典序，而是**自然数序**，针对数据模型路径设计：

```
普通 strcmp:  "Device.DeviceInfo.2" > "Device.DeviceInfo.11"  (因为 '2' > '1')
自然数排序:   "Device.DeviceInfo.2" < "Device.DeviceInfo.11"  (因为 2 < 11)
```

实现逻辑（`NaturalStrCmp`）：
1. 逐字符比较，跳过相同的字符
2. 遇到不同字符时，用 `TEXT_UTILS_CountConsecutiveDigits` 统计两串接下来的连续数字位数
3. **数字位数多者更大**（`delta = num_digits_s1 - num_digits_s2`）
4. 位数相同则按普通字符比较

用途：`GetInstancesResponse` 中返回实例路径时，保证 `{i}` 实例号按数值顺序排列。

---

## 四、性能分析

### 4.1 `STR_VECTOR_Add` 的扩容策略

当前实现每次扩容只增加 1 个元素：

```c
void STR_VECTOR_Add(str_vector_t *sv, char *str)
{
    int new_num_entries = sv->num_entries + 1;
    sv->vector = USP_REALLOC(sv->vector, new_num_entries*sizeof(char *));
    sv->vector[sv->num_entries] = USP_STRDUP(str);
    sv->num_entries = new_num_entries;
}
```

**问题**：每次 `Add` 都会调用一次 `realloc`。连续添加 n 个元素，`realloc` 调用 n 次。

### 4.2 实测数据（4000 次 Add）

| libc | 当前实现 (+1 realloc) | 容量翻倍 | 差距 |
|------|---------------------|---------|------|
| glibc（桌面） | ~0.22 ms | ~0.11 ms | 2x |
| **musl 1.2.5** | **~0.67-0.75 ms** | **~0.28 ms** | **2.5x** |

### 4.3 为什么 musl 上更明显？

musl 1.2.x 使用 **mallocng**（size-class 分组分配器），其 `realloc` 行为：

```
realloc(p, new_size):
    if new_size 和 p 当前落在同一个 size class:
        return p          # 原地不动，零拷贝
    else:
        new = malloc(new_size)   # 跨 size class
        memcpy(new, p, ...)
        free(p)
```

关键不是"调用多少次 realloc"，而是**跨多少次 size class 边界**。

4000 个指针（8 字节 → 32KB）要跨越约几十个 size class，每次跨越触发完整 `malloc + memcpy + free`。glibc 对小块有激进的原地扩展（top chunk），而 musl 跨 size class 必然搬移。

### 4.4 IPQ5322 (Cortex-A53) 目标平台影响

| 特性 | 影响 |
|------|------|
| ARM Cortex-A53 顺序执行 | 单核性能约桌面 x86 的 1/5~1/10 |
| 1.5 GHz | 内存密集操作更慢 |
| musl mallocng | 跨 size class 必然搬移 |

桌面 musl 测出的 0.67ms，在 IPQ5322 上预计放大到 **约 3~5ms**。

---

## 五、优化方案：容量翻倍（Capacity Doubling）

### 5.1 优化后的数据结构

```c
typedef struct {
    char **vector;
    int num_entries;
    int capacity;                    // ★ 新增：已分配容量
} str_vector_t;
```

### 5.2 优化后的 `Add`

```c
void STR_VECTOR_Add(str_vector_t *sv, char *str)
{
    if (sv->num_entries == sv->capacity)
    {
        sv->capacity = (sv->capacity == 0) ? 16 : sv->capacity * 2;
        sv->vector = USP_REALLOC(sv->vector, sv->capacity * sizeof(char *));
    }
    sv->vector[sv->num_entries++] = USP_STRDUP(str);
}
```

### 5.3 优化效果

- `realloc` 调用次数：**n 次 → O(log n) 次**（4000 次 → ~10 次）
- 总复杂度：**O(n²) → O(n) 均摊**
- musl 上实测：**2.5x 提速**
- 减少 mallocng 的分配/释放抖动，利于嵌入式长期运行的内存健康

### 5.4 需要同步修改的函数

引入 `capacity` 字段后，以下直接操作 `vector` / `num_entries` 的函数需同步修改，避免 `capacity` 与 `num_entries` 失配：

| 函数 | 修改点 |
|------|--------|
| `STR_VECTOR_Init` | 初始化 `capacity = 0` |
| `STR_VECTOR_Clone` | 分配时同步设置 `capacity` |
| `STR_VECTOR_RemoveByIndex` | 压缩后保持 `capacity` 不变（或缩容） |
| `STR_VECTOR_RemoveUnusedEntries` | 同上 |
| `STR_VECTOR_ConvertToKeyValueVector` | 转移后重置 `capacity = 0` |
| `STR_VECTOR_Destroy` | 重置 `capacity = 0` |

---

## 六、结论

| 维度 | 结论 |
|------|------|
| 功能完整性 | API 齐全，覆盖增删查改、排序、转换、比较 |
| 内存管理 | 所有权清晰，零拷贝转换是亮点 |
| 排序 | 自然数排序针对路径场景精心设计 |
| **性能瓶颈** | `Add` 每次 +1 realloc，musl 上跨 size class 搬移放大 |
| **优化建议** | 容量翻倍策略，低风险、2.5x 实测提升、减少碎片化 |
