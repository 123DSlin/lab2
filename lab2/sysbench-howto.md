## Lab2 Sysbench 运行说明（配合 MemTable B+Tree 参数）

本仓库的 Lab2 要求对以下 3 组参数做 sysbench 性能测试，并报告结果：

- `internal_max_children = 10, leaf_max_entries = 10`
- `internal_max_children = 50, leaf_max_entries = 50`
- `internal_max_children = 100, leaf_max_entries = 100`

当前实现中，这两个参数通过环境变量注入到 `observer` 进程，并在 DB 初始化时传入 `ObLsmOptions`：

- `MINIOB_MEMTABLE_INTERNAL_MAX_CHILDREN`
- `MINIOB_MEMTABLE_LEAF_MAX_ENTRIES`

> 说明：参数读取发生在 `src/observer/storage/db/db.cpp` 的 `Db::init()` 中；只要重启 observer 并设置不同环境变量即可完成三组实验切换。

---

## 1. 编译

```bash
bash build.sh
```

---

## 2. 启动 observer（MySQL 协议 + LSM 存储引擎）

> Lab2 sysbench 必须以 MySQL 协议启动，并指定 LSM 存储引擎：`-P mysql -E lsm`

以 `10/10` 为例：

```bash
export MINIOB_MEMTABLE_INTERNAL_MAX_CHILDREN=10
export MINIOB_MEMTABLE_LEAF_MAX_ENTRIES=10

./build/bin/observer -f ./etc/observer.ini -P mysql -p 6789 -E lsm
```

---

## 3. sysbench 准备 / 运行

sysbench 脚本在 `test/sysbench/`：

- `miniob_insert.lua`
- `miniob_select.lua`
- `miniob_delete.lua`

在另一个终端执行（以 insert 为例）：

```bash
cd test/sysbench

# prepare
sysbench miniob_insert.lua \
  --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=6789 --mysql-db=test \
  --tables=5 --table_size=1000 \
  prepare

# run (time=60s)
sysbench miniob_insert.lua \
  --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=6789 --mysql-db=test \
  --tables=5 --table_size=1000 --time=60 \
  run
```

对 `miniob_select.lua` / `miniob_delete.lua` 同理。

---

## 4. 清理数据库文件（MiniOB DROP TABLE 未完全支持时）

Lab 文档提示：MiniOB 可能无法正确执行 sysbench 的 cleanup（DROP TABLE）。

一种简单的保证“每次实验从干净库开始”的方式：

1. 退出 `observer`
2. 删除系统库目录（默认在 `build/miniob/db/sys`）

```bash
rm -rf build/miniob/db/sys
```

然后重新启动 observer 并重新 prepare。

---

## 5. split 日志采集

每次节点 split 时会输出一条 INFO 日志：

```
Split. <leaf split counts> <internal-node split counts> <the resulting tree height>
```

默认日志文件为 `observer.log`（配置见 `etc/observer.ini`）。建议在 report 中记录：

- sysbench 结果（TPS/latency 等）
- 最后一次 split 日志信息（leaf/internal split count 与 height）

