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

#include <cstddef>
#include <cstdint>

#include "common/lang/string_view.h"
#include "common/lang/vector.h"
#include "common/lang/memory.h"
#include "common/log/log.h"

#include "oblsm/util/ob_coding.h"
#include "oblsm/util/ob_comparator.h"

namespace oceanbase {

/**
 * @brief An in-memory B+-tree used as MemTable's ordered structure.
 *
 * The tree stores pointers to MemTable entries (`const char *entry_ptr`).
 * Each entry encodes a length-prefixed internal key (user key + seq) and a value.
 *
 * Ordering is defined by ObInternalKeyComparator on internal keys.
 */
class ObMemTableBPlusTree
{
public:
  struct Options
  {
    size_t internal_max_children = 50;  ///< max number of children pointers in an internal node
    size_t leaf_max_entries      = 50;  ///< max number of entries in a leaf node
  };

  struct Stats
  {
    uint64_t leaf_split_count     = 0;
    uint64_t internal_split_count = 0;
    uint64_t height               = 1;  ///< root-only tree has height 1
  };

  explicit ObMemTableBPlusTree(const Options &opt) : opt_(opt)
  {
    // Keep the fanout meaningful to avoid pathological split behavior.
    if (opt_.internal_max_children < 3) {
      opt_.internal_max_children = 3;
    }
    if (opt_.leaf_max_entries < 2) {
      opt_.leaf_max_entries = 2;
    }
  }
  ~ObMemTableBPlusTree() = default;

  ObMemTableBPlusTree(const ObMemTableBPlusTree &)            = delete;
  ObMemTableBPlusTree &operator=(const ObMemTableBPlusTree &) = delete;

  /** Insert a new entry pointer (ordered by internal key). */
  void insert(const char *entry_ptr);

  const Stats &stats() const { return stats_; }

private:
  struct Node
  {
    bool is_leaf = false;
    virtual ~Node() = default;
  };

  struct Internal : Node
  {
    // separator keys are stored as entry pointers; the internal key is derived from the entry
    vector<const char *> keys;
    vector<unique_ptr<Node>> children;

    Internal() { is_leaf = false; }
  };

  struct Leaf : Node
  {
    vector<const char *> entries;
    Leaf *prev = nullptr;
    Leaf *next = nullptr;

    Leaf() { is_leaf = true; }
  };

public:
  class Iterator
  {
  public:
    explicit Iterator(const ObMemTableBPlusTree *tree) : tree_(tree) {}

    bool valid() const { return leaf_ != nullptr && index_ < leaf_->entries.size(); }
    void next();

    void seek_to_first();
    void seek_to_last();

    /**
     * @brief Seek to the first entry whose internal key >= internal key of lookup_key.
     * lookup_key format matches ObUserIterator: [uint64 length][user_key bytes][uint64 seq]
     */
    void seek(const string_view &lookup_key);

    const char *entry() const { return valid() ? leaf_->entries[index_] : nullptr; }

  private:
    const ObMemTableBPlusTree *tree_ = nullptr;
    Leaf                      *leaf_ = nullptr;
    size_t                     index_ = 0;
  };

private:
  string_view internal_key_from_entry(const char *entry_ptr) const
  {
    return get_length_prefixed_string(entry_ptr);
  }

  int compare_entry_with_internal_key(const char *entry_ptr, const string_view &internal_key) const
  {
    return comparator_.compare(internal_key_from_entry(entry_ptr), internal_key);
  }

  int compare_entry(const char *a, const char *b) const
  {
    return comparator_.compare(internal_key_from_entry(a), internal_key_from_entry(b));
  }

  void log_split() const
  {
    LOG_INFO("Split. %lu %lu %lu", stats_.leaf_split_count, stats_.internal_split_count, stats_.height);
  }

  // returns leaf and path of internal nodes visited
  Leaf *find_leaf(const string_view &internal_key, vector<Internal *> &path) const;

  // insert into leaf (no split)
  void leaf_insert_sorted(Leaf *leaf, const char *entry_ptr);

  // split leaf and insert separator into parent chain
  void split_leaf_and_insert(Leaf *leaf, const char *entry_ptr, vector<Internal *> &path);

  // insert separator into an internal node and split if needed, cascading upwards
  void insert_into_parent(vector<Internal *> &path, Leaf *left, const char *sep_entry, unique_ptr<Node> right_child);
  void split_internal_and_insert(vector<Internal *> &path, Internal *node, const char *sep_entry, unique_ptr<Node> right_child);

  // helper for internal navigation: find child index for internal_key
  size_t internal_child_index(const Internal *in, const string_view &internal_key) const;

private:
  Options                 opt_;
  mutable Stats           stats_;
  ObInternalKeyComparator comparator_;

  unique_ptr<Node> root_{new Leaf()};
  Leaf            *first_leaf_ = static_cast<Leaf *>(root_.get());
  Leaf            *last_leaf_  = static_cast<Leaf *>(root_.get());
};

}  // namespace oceanbase

