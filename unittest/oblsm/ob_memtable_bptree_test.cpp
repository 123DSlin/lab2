/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "gtest/gtest.h"

#include "oblsm/memtable/ob_memtable.h"
#include "oblsm/ob_user_iterator.h"
#include "oblsm/util/ob_comparator.h"
#include "oblsm/util/ob_coding.h"

using namespace oceanbase;

static string make_lookup_key(const string &user_key, uint64_t seq)
{
  string lk;
  put_numeric<uint64_t>(&lk, static_cast<uint64_t>(user_key.size() + SEQ_SIZE));
  lk.append(user_key);
  put_numeric<uint64_t>(&lk, seq);
  return lk;
}

TEST(memtable_bptree, ordered_iteration_and_seek)
{
  // Small node sizes to force splits.
  auto mem = make_shared<ObMemTable>(/*internal_max_children*/ 4, /*leaf_max_entries*/ 3);

  uint64_t seq = 1;
  mem->put(seq++, "k", "v1");
  mem->put(seq++, "k", "v2");
  mem->put(seq++, "a", "va1");
  mem->put(seq++, "b", "vb1");
  mem->put(seq++, "a", "va2");

  std::unique_ptr<ObLsmIterator> it(mem->new_iterator());
  it->seek_to_first();

  // Expect user key order: a, a, b, k, k
  vector<string> user_keys;
  vector<uint64_t> seqs;
  while (it->valid()) {
    user_keys.emplace_back(extract_user_key(it->key()));
    seqs.emplace_back(extract_sequence(it->key()));
    it->next();
  }

  ASSERT_EQ(user_keys.size(), 5);
  EXPECT_EQ(user_keys[0], "a");
  EXPECT_EQ(user_keys[1], "a");
  EXPECT_EQ(user_keys[2], "b");
  EXPECT_EQ(user_keys[3], "k");
  EXPECT_EQ(user_keys[4], "k");

  // For same user key, seq should be in descending order.
  EXPECT_GT(seqs[0], seqs[1]); // a: va2 then va1
  EXPECT_GT(seqs[3], seqs[4]); // k: v2 then v1

  // Seek should land on the latest version of 'k' when using a very large seq.
  string lk = make_lookup_key("k", std::numeric_limits<uint64_t>::max());
  it.reset(mem->new_iterator());
  it->seek(string_view(lk.data(), lk.size()));
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(extract_user_key(it->key()), "k");
  EXPECT_EQ(string(it->value()), "v2");
}

TEST(memtable_bptree, basic_insertion_and_lookup)
{
  auto mem = make_shared<ObMemTable>(/*internal_max_children*/ 4, /*leaf_max_entries*/ 3);

  uint64_t seq = 1;
  struct KV {
    const char *k;
    const char *v;
  };
  vector<KV> kvs{
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
      {"delta", "4"},
      {"epsilon", "5"},
  };

  for (auto &kv : kvs) {
    mem->put(seq++, kv.k, kv.v);
  }

  for (auto &kv : kvs) {
    string lk = make_lookup_key(kv.k, std::numeric_limits<uint64_t>::max());
    std::unique_ptr<ObLsmIterator> it(mem->new_iterator());
    it->seek(string_view(lk.data(), lk.size()));
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(extract_user_key(it->key()), kv.k);
    EXPECT_EQ(string(it->value()), kv.v);
  }
}

TEST(memtable_bptree, multiple_writes_same_key_latest_visible)
{
  auto mem = make_shared<ObMemTable>(/*internal_max_children*/ 4, /*leaf_max_entries*/ 3);

  uint64_t seq = 1;
  mem->put(seq++, "k", "v1");
  mem->put(seq++, "k", "v2");
  mem->put(seq++, "k", "v3");

  // Lookup latest by seeking with a very large seq.
  string lk = make_lookup_key("k", std::numeric_limits<uint64_t>::max());
  std::unique_ptr<ObLsmIterator> it(mem->new_iterator());
  it->seek(string_view(lk.data(), lk.size()));
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(extract_user_key(it->key()), "k");
  EXPECT_EQ(string(it->value()), "v3");

  // Iteration order for the same user key should be seq descending: v3, v2, v1.
  it->seek_to_first();
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(extract_user_key(it->key()), "k");
  EXPECT_EQ(string(it->value()), "v3");
  it->next();
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(string(it->value()), "v2");
  it->next();
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(string(it->value()), "v1");
  it->next();
  EXPECT_FALSE(it->valid());
}

TEST(memtable_bptree, delete_tombstone_hides_key_in_user_iterator)
{
  auto mem = make_shared<ObMemTable>(/*internal_max_children*/ 4, /*leaf_max_entries*/ 3);

  uint64_t seq = 1;
  mem->put(seq++, "k", "v1");
  // Tombstone: empty value represents delete (see ObUserIterator: value.empty()).
  mem->put(seq++, "k", "");

  // Internal iterator should expose the tombstone as the latest version.
  {
    string lk = make_lookup_key("k", std::numeric_limits<uint64_t>::max());
    std::unique_ptr<ObLsmIterator> it(mem->new_iterator());
    it->seek(string_view(lk.data(), lk.size()));
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(extract_user_key(it->key()), "k");
    EXPECT_TRUE(it->value().empty());
  }

  // User iterator should hide deleted keys.
  {
    std::unique_ptr<ObLsmIterator> user_it(new_user_iterator(mem->new_iterator(),
                                                             std::numeric_limits<uint64_t>::max()));
    user_it->seek("k");
    EXPECT_FALSE(user_it->valid());
  }
}

TEST(memtable_bptree, ordered_output_is_strictly_sorted_for_dump_like_consumers)
{
  auto mem = make_shared<ObMemTable>(/*internal_max_children*/ 4, /*leaf_max_entries*/ 3);

  uint64_t seq = 1;
  // Insert out-of-order user keys and multiple versions to mimic real workloads.
  mem->put(seq++, "c", "vc1");
  mem->put(seq++, "a", "va1");
  mem->put(seq++, "b", "vb1");
  mem->put(seq++, "a", "va2");  // newer 'a'
  mem->put(seq++, "c", "vc2");  // newer 'c'
  mem->put(seq++, "b", "");     // tombstone for 'b'

  ObInternalKeyComparator cmp;
  std::unique_ptr<ObLsmIterator> it(mem->new_iterator());
  it->seek_to_first();
  ASSERT_TRUE(it->valid());

  string prev(it->key());
  it->next();
  while (it->valid()) {
    string curr(it->key());
    // Downstream dump/freeze/flush logic depends on MemTable producing strictly sorted output.
    EXPECT_LT(cmp.compare(prev, curr), 0);
    prev = std::move(curr);
    it->next();
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

