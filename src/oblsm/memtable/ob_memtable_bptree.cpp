/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/memtable/ob_memtable_bptree.h"

#include <algorithm>

namespace oceanbase {

size_t ObMemTableBPlusTree::internal_child_index(const Internal *in, const string_view &internal_key) const
{
  // children count = keys + 1
  // keys[i] is the first key of child i+1 (right child)
  size_t lo = 0;
  size_t hi = in->keys.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    string_view mid_key = internal_key_from_entry(in->keys[mid]);
    if (comparator_.compare(mid_key, internal_key) <= 0) {
      // mid_key <= target: go right
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

ObMemTableBPlusTree::Leaf *ObMemTableBPlusTree::find_leaf(const string_view &internal_key, vector<Internal *> &path) const
{
  Node *n = root_.get();
  while (!n->is_leaf) {
    auto *in = static_cast<Internal *>(n);
    path.push_back(in);
    size_t idx = internal_child_index(in, internal_key);
    n = in->children[idx].get();
  }
  return static_cast<Leaf *>(n);
}

void ObMemTableBPlusTree::leaf_insert_sorted(Leaf *leaf, const char *entry_ptr)
{
  auto it = std::lower_bound(leaf->entries.begin(), leaf->entries.end(), entry_ptr, [this](const char *a, const char *b) {
    return compare_entry(a, b) < 0;
  });
  leaf->entries.insert(it, entry_ptr);
}

void ObMemTableBPlusTree::insert(const char *entry_ptr)
{
  string_view internal_key = internal_key_from_entry(entry_ptr);
  vector<Internal *> path;
  Leaf *leaf = find_leaf(internal_key, path);

  if (leaf->entries.size() < opt_.leaf_max_entries) {
    leaf_insert_sorted(leaf, entry_ptr);
    return;
  }
  split_leaf_and_insert(leaf, entry_ptr, path);
}

void ObMemTableBPlusTree::split_leaf_and_insert(Leaf *leaf, const char *entry_ptr, vector<Internal *> &path)
{
  // merge + insert then split
  vector<const char *> tmp = leaf->entries;
  auto it = std::lower_bound(tmp.begin(), tmp.end(), entry_ptr, [this](const char *a, const char *b) { return compare_entry(a, b) < 0; });
  tmp.insert(it, entry_ptr);

  const size_t total = tmp.size();
  const size_t mid = total / 2;

  auto right = unique_ptr<Leaf>(new Leaf());
  right->entries.assign(tmp.begin() + mid, tmp.end());
  leaf->entries.assign(tmp.begin(), tmp.begin() + mid);

  // link into leaf chain
  right->next = leaf->next;
  right->prev = leaf;
  if (leaf->next) {
    leaf->next->prev = right.get();
  }
  leaf->next = right.get();
  if (last_leaf_ == leaf) {
    last_leaf_ = right.get();
  }

  stats_.leaf_split_count++;
  // root is leaf and split creates a new root: still counts as leaf split per spec

  const char *sep_entry = right->entries.front();
  // insert into parent (may create a new root and change height)
  insert_into_parent(path, leaf, sep_entry, std::move(right));
  log_split();
}

void ObMemTableBPlusTree::insert_into_parent(vector<Internal *> &path, Leaf *left, const char *sep_entry, unique_ptr<Node> right_child)
{
  if (path.empty()) {
    // create new root
    auto new_root = unique_ptr<Internal>(new Internal());
    new_root->keys.push_back(sep_entry);
    new_root->children.reserve(2);
    new_root->children.emplace_back(std::move(root_));
    new_root->children.emplace_back(std::move(right_child));
    root_ = std::move(new_root);
    stats_.height++;
    // first/last leaf pointers remain valid
    return;
  }

  Internal *parent = path.back();
  path.pop_back();

  // Keep internal separator keys ordered by comparator.
  const string_view sep_key = internal_key_from_entry(sep_entry);
  auto it = std::lower_bound(parent->keys.begin(), parent->keys.end(), sep_key,
      [this](const char *entry, const string_view &k) { return comparator_.compare(internal_key_from_entry(entry), k) < 0; });
  const size_t key_pos = static_cast<size_t>(it - parent->keys.begin());
  parent->keys.insert(it, sep_entry);
  parent->children.insert(parent->children.begin() + key_pos + 1, std::move(right_child));

  if (parent->children.size() <= opt_.internal_max_children) {
    return;
  }

  // parent overflow: split internal and propagate
  // choose sep as middle key
  split_internal_and_insert(path, parent, nullptr, nullptr);
}

void ObMemTableBPlusTree::split_internal_and_insert(
    vector<Internal *> &path, Internal *node, const char * /*sep_entry*/, unique_ptr<Node> /*right_child*/)
{
  // Split the existing node (already contains the inserted key/child).
  const size_t children_cnt = node->children.size();
  ASSERT(children_cnt > opt_.internal_max_children, "only split on overflow");

  // keys size = children - 1
  const size_t keys_cnt = node->keys.size();
  const size_t mid_key_index = keys_cnt / 2;

  const char *promote = node->keys[mid_key_index];

  auto right = unique_ptr<Internal>(new Internal());
  // right takes keys after mid
  right->keys.assign(node->keys.begin() + mid_key_index + 1, node->keys.end());
  node->keys.erase(node->keys.begin() + mid_key_index, node->keys.end());

  // children split: left keeps [0 .. mid_key_index], right gets [mid_key_index+1 .. end]
  right->children.reserve(children_cnt - (mid_key_index + 1));
  for (size_t i = mid_key_index + 1; i < children_cnt; ++i) {
    right->children.emplace_back(std::move(node->children[i]));
  }
  node->children.erase(node->children.begin() + mid_key_index + 1, node->children.end());

  stats_.internal_split_count++;

  if (path.empty()) {
    auto new_root = unique_ptr<Internal>(new Internal());
    new_root->keys.push_back(promote);
    new_root->children.emplace_back(std::move(root_));
    new_root->children.emplace_back(std::move(right));
    root_ = std::move(new_root);
    stats_.height++;
    log_split();
    return;
  }

  Internal *parent = path.back();
  path.pop_back();

  // Keep internal separator keys ordered by comparator.
  const string_view promote_key = internal_key_from_entry(promote);
  auto it = std::lower_bound(parent->keys.begin(), parent->keys.end(), promote_key,
      [this](const char *entry, const string_view &k) { return comparator_.compare(internal_key_from_entry(entry), k) < 0; });
  const size_t key_pos = static_cast<size_t>(it - parent->keys.begin());
  parent->keys.insert(it, promote);
  parent->children.insert(parent->children.begin() + key_pos + 1, std::move(right));

  log_split();

  if (parent->children.size() > opt_.internal_max_children) {
    split_internal_and_insert(path, parent, nullptr, nullptr);
  }
}

// ---- Iterator ----
void ObMemTableBPlusTree::Iterator::seek_to_first()
{
  leaf_ = tree_->first_leaf_;
  index_ = 0;
}

void ObMemTableBPlusTree::Iterator::seek_to_last()
{
  leaf_ = tree_->last_leaf_;
  index_ = leaf_ ? leaf_->entries.size() : 0;
  if (leaf_ && !leaf_->entries.empty()) {
    index_ = leaf_->entries.size() - 1;
  }
}

void ObMemTableBPlusTree::Iterator::next()
{
  if (!valid()) {
    return;
  }
  index_++;
  if (leaf_ && index_ >= leaf_->entries.size()) {
    leaf_ = leaf_->next;
    index_ = 0;
  }
}

void ObMemTableBPlusTree::Iterator::seek(const string_view &lookup_key)
{
  const string_view internal_key = extract_internal_key(lookup_key);
  vector<Internal *> path;
  Leaf *leaf = tree_->find_leaf(internal_key, path);
  leaf_ = leaf;
  if (leaf_ == nullptr) {
    index_ = 0;
    return;
  }
  auto it = std::lower_bound(leaf_->entries.begin(), leaf_->entries.end(), internal_key,
      [this](const char *entry, const string_view &k) {
        return tree_->compare_entry_with_internal_key(entry, k) < 0;
      });
  index_ = static_cast<size_t>(it - leaf_->entries.begin());
  if (index_ >= leaf_->entries.size()) {
    // move to next leaf
    leaf_ = leaf_->next;
    index_ = 0;
  }
}

}  // namespace oceanbase

