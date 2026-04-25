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

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

