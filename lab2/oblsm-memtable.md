## ObLsm 引擎中 MemTable 的角色、存储与有序迭代（中文说明）

本文面向 `src/oblsm/` 的实现，说明三件事：

- MemTable 在 ObLsm 中的作用
- 记录在 MemTable 中如何存放（内存布局/编码格式）
- MemTableIterator 的有序迭代如何工作，以及它在 flush（落盘到 SSTable）阶段如何被使用

---

## 1. MemTable 在 ObLsm 中的角色（Write Path 视角）

在 `ObLsmImpl::put()` 中，写入路径是典型的 LSM 结构：

1. 先写 WAL（预写日志）：为崩溃恢复保留日志（当前仓库里 WAL 代码仍有未实现部分，但流程位置已明确）。
2. 再写 MemTable：作为内存中的“最新数据视图”，提升写入吞吐与读取延迟。

当 MemTable 的近似内存占用超过阈值（`options_.memtable_size`）时，会触发“冻结（freeze）”流程（见 `ObLsmImpl::try_freeze_memtable()`）：

- 当前可写 MemTable `mem_table_` 被转移到 `imem_tables_`（immutable memtable）
- 新建一个新的 `mem_table_` 用于继续接收写入
- 后台任务开始把冻结的 `imem` flush 成 SSTable（见 `ObLsmImpl::background_compaction()` → `build_sstable()`）

因此，MemTable 的核心职责是：

- 吸收最新写入（写缓冲）
- 为读请求提供最新数据（读优化）
- 在合适时机被冻结并被 flush 到磁盘（SSTable）

---

## 2. 记录在 MemTable 中如何存储（编码格式 + 数据结构）

### 2.1 底层数据结构：SkipList + Arena

`ObMemTable` 内部主要由两部分构成：

- SkipList：`ObSkipList<const char *, KeyComparator>`
  - 用来维持 key 的有序性，从而支持范围扫描/有序迭代。
- Arena：`ObArena arena_`
  - 用来一次性分配 entry 内存并集中管理，便于快速插入和估算内存使用（`appro_memory_usage()`）。

插入时（`ObMemTable::put`）会：

- 在 `arena_` 中分配一段连续内存 `buf`
- 把编码后的 entry 写入 `buf`
- 将 `buf` 指针（`const char*`）插入 SkipList（作为 SkipList 的 key）

也就是说：**SkipList 里存的是 entry 的指针，entry 的真实内容在 Arena 中。**

### 2.2 MemTable entry 编码格式（按实现精确描述）

`ObMemTable::put(seq, key, value)` 中写入 entry 的格式为（按顺序）：

- `internal_key_size`：`size_t`（通常 8 字节，随平台而定）
- `user_key bytes`：长度为 `user_key_size`
- `seq`：`uint64_t`（8 字节）
- `value_size`：`size_t`
- `value bytes`：长度为 `value_size`

其中：

- `SEQ_SIZE = 8`
- `internal_key_size = user_key_size + SEQ_SIZE`
- 所谓 “internal key” 实际就是 **user key + seq** 组合（seq 存在末尾）

可表示为：

```
| size_t internal_key_size |
| user_key bytes           |
| uint64_t seq             |
| size_t value_size        |
| value bytes              |
```

### 2.3 排序依据：InternalKeyComparator（user key + seq）

MemTable 的 `KeyComparator` 会从 entry 中读取“长度前缀的 internal key”（即 `internal_key_size + internal_key bytes`），然后调用 `ObInternalKeyComparator::compare` 比较。

`ObInternalKeyComparator` 的排序规则是：

1. 先按 **user key 字典序**比较
2. 如果 user key 相同，再按 **seq 逆序**（seq 越大越靠前）

这保证了同一个 user key 的多版本记录按“最新在前”的顺序出现，便于读取时优先得到最新版本。

---

## 3. MemTableIterator 的有序迭代如何工作

### 3.1 MemTableIterator 迭代的对象是什么？

`ObMemTableIterator` 是对 SkipList `Iterator` 的包装：

- SkipList iterator 返回的是 `const char *`（entry 起始地址）
- entry 内部包含 internal key 与 value 的编码内容

### 3.2 `key()` / `value()` 怎么解析？

- `key()`：
  - 对 entry 起始地址调用 `get_length_prefixed_string(ptr)`
  - 读取 `size_t len`，返回后续 `len` 字节的 `string_view`
  - 返回的是 **internal key（user key + seq）**

- `value()`：
  - 先取到 key slice（internal key）
  - 再从 `key_slice.data() + key_slice.size()` 位置继续取一个 length-prefixed string
  - 对应 entry 中的 `value_size + value bytes`

因此，MemTableIterator 的输出是：

- `key()`：internal key（含 seq）
- `value()`：value

上层如果需要对用户屏蔽 seq，通常会对 internal key 调用 `extract_user_key(...)` 得到 user key。

### 3.3 有序性从哪里来？

有序性来自 SkipList 的排序，排序比较器基于 internal key（user key + seq）。因此 MemTableIterator 的遍历顺序天然是：

- user key 升序
- 同 user key 下 seq 降序

---

## 4. 有序迭代在 flushing（落盘）阶段如何使用

在当前代码路径中，flush 发生在：

- `ObLsmImpl::try_freeze_memtable()`：把 `mem_table_` 冻结为 `imem`
- `ObLsmImpl::background_compaction()`：后台任务调用 `build_sstable(imem)`
- `build_sstable()`：创建 `ObSSTableBuilder` 并调用 `tb->build(imem, ...)`

虽然当前仓库中 `ObSSTableBuilder::build` 仍是 `UNIMPLEMENTED`，但从已有接口可以明确 flush 的期望方式：**依赖 MemTableIterator 的有序扫描**，典型逻辑是：

1. `auto it = mem_table->new_iterator(); it->seek_to_first();`
2. 顺序遍历所有 internal key/value
3. 逐条写入 `ObBlockBuilder::add(key, value)`
4. block 满时写出 block，并记录 `BlockMeta(first_key, last_key, offset, size)`
5. 最终写入 block meta 区域，形成完整 SSTable

由于 MemTableIterator 输出的是 internal key（含 seq），flush 到 SSTable 时会保持 internal key 的排序，这使得后续：

- SSTable 内部可以做 block 级定位与有序迭代
- 多个 SSTable 之间可以通过 merging iterator 做归并
- user iterator 能在归并结果上按 seq 做可见性处理/去重

---

## 5. 一句话总结（给后续任务用）

- MemTable 是 ObLsm 的内存写缓冲 + 最新读视图；超阈值后冻结为 `imem` 并在后台 flush 成 SSTable。
- MemTable 用 SkipList 保序、Arena 存 entry；entry 的 key 是 internal key（user key + seq）。
- MemTableIterator 按 internal key 有序遍历，是 flush/build SSTable 的基础输入。

