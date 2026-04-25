## Lab2 实现步骤计划：用内存 B+Tree 替换 ObLsm MemTable 的 SkipList（保持 comparator/ordering/versioning 语义不变）

本文是 Lab2 的“可执行实现计划”，目标是在 **不改变 MemTable 对外行为** 的前提下，把 `ObMemTable` 内部的有序结构从 SkipList 替换为 **内存 B+Tree**，并满足实验对 split 日志与统计的要求。

---

## 0. 先明确“绝对不能改”的语义（本实验最重要约束）

实验明确要求：

> B+-tree must preserve the same comparator, ordering, and versioning semantics currently used by the MemTable.  
> do not replace the existing MemTable ordering with a simpler user-key-only ordering unless that is already the current behavior.

在当前 `src/oblsm/memtable/ob_memtable.cpp` 中，MemTable 的 key 并不是纯 user key，而是 **internal key = user key + seq(8B)**，并且比较器使用 `ObInternalKeyComparator`：

- **排序主键**：user key 字典序（lexicographical）
- **同 user key 的版本排序**：seq **逆序**（seq 越大越靠前，代表更新版本更“新”）

因此：你的 B+Tree 必须按 **internal key** 建序并迭代输出，不能只按 user key 排序。

---

## 1. 阅读/对照清单（从代码里找“契约”）

实现前建议把这些文件当成“契约文档”：

- `src/oblsm/memtable/ob_memtable.{h,cpp}`
  - entry 编码格式（Arena 分配 + 序列化布局）
  - `ObMemTableIterator` 的 `key()/value()/seek()/next()` 契约
- `src/oblsm/util/ob_coding.h`
  - `SEQ_SIZE`、`get_length_prefixed_string`、`extract_user_key` 等
- `src/oblsm/util/ob_comparator.{h,cpp}`
  - `ObInternalKeyComparator::compare` 的真实排序规则（user key + seq）
- `src/oblsm/ob_lsm_impl.cpp`
  - `put()`→`try_freeze_memtable()`→`background_compaction()`→`build_sstable()` 调用链
  - `new_iterator()` 如何把 mem/imem/sst 的 iterator 做 merge（要求每个子 iterator 自身有序）

---

## 2. 设计目标：只替换“有序容器”，保留 entry 编码与上层使用方式

### 2.1 保留（推荐）

- 保留 `ObMemTable::put()` 的 entry 编码（写入一段连续内存）
- 保留 `ObArena arena_` 作为 entry 的内存拥有者
- MemTable 有序结构中存放的“值”仍建议是 `const char *entry_ptr`
  - 好处：`key()`/`value()` 解析逻辑保持与现有一致

### 2.2 替换

- 把 `ObSkipList<const char*, KeyComparator> table_` 换成 `BPlusTree<const char*> tree_`
- 把 `ObMemTableIterator` 的底层迭代由“skiplist iterator”换成“B+tree iterator”

---

## 3. 内存 B+Tree 的接口（建议最小可用集合）

建议在 `src/oblsm/memtable/` 下新增两个文件，例如：

- `ob_memtable_bptree.h`
- `ob_memtable_bptree.cpp`

并提供一个只服务 MemTable 的 B+Tree：

### 3.1 Key 提取与比较（关键）

由于 tree 存 `entry_ptr`，比较时需要从 entry 中提取 internal key：

- `string_view internal_key = get_length_prefixed_string(entry_ptr);`
- 用 `ObInternalKeyComparator` 对 internal key 做三路比较

注意：`get_length_prefixed_string` 读取的是 `size_t` 前缀；entry 的内部布局必须与 `ObMemTable::put` 一致。

### 3.2 B+Tree 支持的操作

- `insert(const char *entry_ptr)`
- `Iterator`（必须支持有序遍历 + seek）
  - `seek(const string_view &lookup_key)`：定位到第一个 `internal_key >= lookup_key` 的 entry
  - `seek_to_first()`
  - `next()`
  - `valid()`
  - `entry()` / `key()` / `value()`（任选一种暴露方式；但最终 MemTableIterator 需返回 key/value 的 string_view）

> 注：MemTable 的 `seek()` 目前是直接把 `lookup_key.data()` 传给 skiplist iterator，这块逻辑本身并不完善；替换时建议按照“lookup_key 是 internal/lookup key 形式”的真实语义实现正确的 lower_bound 行为，并保持 `ObLsmImpl` 的读取正确。

---

## 4. B+Tree 参数化要求（必须）

实验要求两个可调参数：

- `internal_max_children`
- `leaf_max_entries`

建议实现方式之一（易写报告）：

- 在 B+Tree 构造函数传入两参数
- `ObMemTable` 在构造时创建 tree，并使用默认值
- 报告中声明“默认值是多少、如何修改”

---

## 5. Split 统计与 INFO 日志（必须，格式严格）

### 5.1 需要统计的值

- `leaf_split_count`
- `internal_split_count`
- `tree_height`

### 5.2 什么时候打日志

每次 split（leaf 或 internal）发生时，必须打印 INFO 日志，格式严格为：

`Split. <leaf split counts> <internal-node split counts> <the resulting tree height>`

并处理特殊规则：

- 初始 root 是 leaf，split 后创建新 root：这次算 **leaf split**（不算 internal split）

### 5.3 建议落点

- 在 B+Tree 内部维护 `struct Stats { ... }`
- split 的代码路径里更新 stats 并 `LOG_INFO("Split. %lu %lu %lu", ...)`
  - LOG 系统参考 `etc/observer.ini`

---

## 6. 实现步骤（建议按“先可跑，再正确，再优化”的顺序）

### Step A：只做骨架与编译通过（不替换逻辑）

- 新建 `ob_memtable_bptree.h/.cpp`
- 定义 Node/Iterator/Stats/参数结构，但暂不接入 `ObMemTable`
- 确保编译通过

### Step B：实现 B+Tree 的“有序迭代器”能力（优先）

目标：让 iterator 能正确 `seek_to_first/next/valid`，并能返回 entry_ptr。

- 先只支持插入后顺序遍历（可用最简单的 leaf 链表组织）
- 再补 `seek(lower_bound)`（必须）

验证点：

- 插入 N 条 key，遍历输出 internal key 必须有序
- 同 user key 不同 seq：seq 越大越靠前（internal key comparator 约束）

### Step C：实现 insert + split（满足日志/统计）

- 先实现 leaf 插入、leaf split
- 再实现 internal 节点插入与 internal split
- 维护 `tree_height`

验证点：

- 达到 leaf 满时一定 split，且日志格式完全符合要求
- root leaf split 的计数规则正确

### Step D：把 `ObMemTable` 的底层容器替换为 B+Tree

在 `src/oblsm/memtable/ob_memtable.{h,cpp}` 中：

- 用 B+Tree 替换 `table_`
- `put()`：保持 entry 编码不变，但把 `table_.insert(buf)` 改为 `tree_.insert(buf)`
- `new_iterator()`：返回一个新的 iterator（可继续叫 `ObMemTableIterator`，但内部改为 B+Tree iterator）
- `key()` / `value()`：保持现有解析方式（依赖 entry 格式）

### Step E：系统级回归（保证 freeze/flush 不被破坏）

从调用链检查：

- `ObLsmImpl::new_iterator()`：mem/imem/sst 合并读取必须仍正确
- `try_freeze_memtable()`→`background_compaction()`：freeze 后构建 SST 的过程依赖 MemTable 输出“有序”

即便当前某些 SST/WAL 代码仍有 stub，也要保证替换 MemTable 后不会引入新的行为偏差（尤其是迭代顺序/seek 的语义）。

---

## 7. 测试计划（对应 PDF 的最低要求）

建议写两层测试：

### 7.1 MemTable 级（直接测 `ObMemTable`）

- Basic insertion and lookup：插入多条，能从 iterator/查找路径取到正确记录
- Multiple writes to same key：同 user key，多 seq，顺序与可见性符合预期
- Delete-related behavior：如果当前实现用 tombstone（空 value 或特定编码），必须保持该语义
- Ordered iteration：遍历输出顺序与旧实现一致

### 7.2 ObLsm 级（系统行为）

- 触发 freeze：插入到超过 `memtable_size`，观察后台 flush 仍能消费 iterator
- 检查 `observer.log`：split 日志格式严格、计数单调正确

---

## 8. 可复用/参考的“代码提示点”

- entry 编码/解析：`src/oblsm/memtable/ob_memtable.cpp` + `src/oblsm/util/ob_coding.h`
- internal key comparator：`src/oblsm/util/ob_comparator.cpp`
- 上层依赖 iterator 有序的路径：`src/oblsm/ob_lsm_impl.cpp`（`new_iterator` / `build_sstable`）

---

## 9. 交付物清单（提交前自检）

- 编译通过（必需）
- MemTable 已替换为 B+Tree（不是旁路实现）
- 参数 `internal_max_children` / `leaf_max_entries` 可调整，报告写清楚配置方式
- split 统计与 INFO 日志格式完全符合要求
- 测试覆盖 PDF 至少 5 类场景，并在报告里给出结果

