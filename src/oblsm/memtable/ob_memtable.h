/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/sys/rc.h"
#include "common/lang/string.h"
#include "common/lang/string_view.h"
#include "common/lang/memory.h"
#include "oblsm/memtable/ob_memtable_bptree.h"
#include "oblsm/util/ob_comparator.h"
#include "oblsm/util/ob_arena.h"
#include "oblsm/include/ob_lsm_iterator.h"

namespace oceanbase {

/**
 * @class ObMemTable
 * @brief MemTable implementation for LSM-Tree.
 *
 * The `ObMemTable` represents an in-memory structure that stores key-value pairs
 * before they are flushed to disk as SSTables. It supports key-value insertion,
 * querying, and iteration. The implementation currently uses a skip list as
 * the underlying data structure.
 */
class ObMemTable : public enable_shared_from_this<ObMemTable>
{
public:
  explicit ObMemTable(size_t internal_max_children = 50, size_t leaf_max_entries = 50)
  {
    ObMemTableBPlusTree::Options opt;
    opt.internal_max_children = internal_max_children;
    opt.leaf_max_entries      = leaf_max_entries;
    tree_.reset(new ObMemTableBPlusTree(opt));
  }

  ~ObMemTable() = default;

  /**
   * @brief Retrieves a shared pointer to the current ObMemTable instance.
   *
   * This method utilizes `std::enable_shared_from_this` to provide a shared pointer
   * to the current object. Useful when the current object needs to be shared safely
   * among multiple components.
   *
   * @return A shared pointer to the current `ObMemTable` instance.
   */
  shared_ptr<ObMemTable> get_shared_ptr() { return shared_from_this(); }

  /**
   * @brief Inserts a key-value pair into the memtable.
   *
   * Each entry is versioned using the provided `seq` number. If the same key is
   * inserted multiple times, the version with the highest sequence number will
   * take precedence when queried.
   *
   * @param seq A sequence number used for versioning the key-value entry.
   * @param key The key to be inserted.
   * @param value The value associated with the key.
   */
  void put(uint64_t seq, const string_view &key, const string_view &value);

  /**
   * @brief Estimates the memory usage of the memtable.
   *
   * Returns the approximate memory usage of the memtable, including the
   * skip list and associated memory allocations.
   *
   * @return The approximate memory usage in bytes.
   */
  size_t appro_memory_usage() const { return arena_.memory_usage(); }

  /**
   * @brief Creates a new iterator for traversing the contents of the memtable.
   *
   * This method returns a heap-allocated iterator for iterating over key-value
   * pairs stored in the memtable. The caller is responsible for managing the
   * lifetime of the returned iterator.
   *
   * @return A pointer to the newly created `ObLsmIterator` for the memtable.
   */
  ObLsmIterator *new_iterator();

private:
  friend class ObMemTableIterator;
  unique_ptr<ObMemTableBPlusTree> tree_;

  /**
   * @brief Memory arena used for memory management in the memtable.
   *
   * Allocates and tracks memory usage for the skip list and other internal
   * components of the memtable.
   */
  ObArena arena_;
};

/**
 * @class ObMemTableIterator
 * @brief An iterator for traversing the contents of an `ObMemTable`.
 */
class ObMemTableIterator : public ObLsmIterator
{
public:
  explicit ObMemTableIterator(shared_ptr<ObMemTable> mem) : mem_(mem), iter_(mem->tree_.get()) {}

  ObMemTableIterator(const ObMemTableIterator &)            = delete;
  ObMemTableIterator &operator=(const ObMemTableIterator &) = delete;

  ~ObMemTableIterator() override = default;

  void seek(const string_view &k) override;
  void seek_to_first() override { iter_.seek_to_first(); }
  void seek_to_last() override { iter_.seek_to_last(); }

  bool        valid() const override { return iter_.valid(); }
  void        next() override { iter_.next(); }
  string_view key() const override;
  string_view value() const override;

private:
  shared_ptr<ObMemTable>         mem_;
  ObMemTableBPlusTree::Iterator  iter_;
};

}  // namespace oceanbase