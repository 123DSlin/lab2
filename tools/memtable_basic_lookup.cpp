/* Basic manual-style verification for Lab2 MemTable.
 * Inserts multiple key/value pairs into ObMemTable and seeks them back using
 * the original lookup-key semantics (user key + very large seq).
 */

#include <limits>
#include <iostream>
#include "oblsm/memtable/ob_memtable.h"
#include "oblsm/util/ob_coding.h"

using namespace oceanbase;

static std::string make_lookup_key(const std::string &user_key, uint64_t seq)
{
  std::string lk;
  put_numeric<uint64_t>(&lk, static_cast<uint64_t>(user_key.size() + SEQ_SIZE));
  lk.append(user_key);
  put_numeric<uint64_t>(&lk, seq);
  return lk;
}

int main()
{
  auto mem = std::make_shared<ObMemTable>(/*internal_max_children*/ 4, /*leaf_max_entries*/ 3);

  uint64_t seq = 1;
  struct KV
  {
    const char *k;
    const char *v;
  };
  KV kvs[] = {
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
      {"delta", "4"},
      {"epsilon", "5"},
  };

  std::cout << "== MemTable basic insertion ==\n";
  for (auto &kv : kvs) {
    mem->put(seq++, kv.k, kv.v);
    std::cout << "put key=" << kv.k << " value=" << kv.v << "\n";
  }

  std::cout << "\n== Lookup (seek) verification ==\n";
  bool all_ok = true;
  for (auto &kv : kvs) {
    std::string lk = make_lookup_key(kv.k, std::numeric_limits<uint64_t>::max());
    std::unique_ptr<ObLsmIterator> it(mem->new_iterator());
    it->seek(string_view(lk.data(), lk.size()));

    bool ok = it->valid() && extract_user_key(it->key()) == kv.k && std::string(it->value()) == kv.v;
    all_ok = all_ok && ok;

    std::cout << "lookup key=" << kv.k << " -> "
              << (it->valid() ? ("found value=" + std::string(it->value())) : "NOT_FOUND")
              << " [" << (ok ? "OK" : "FAIL") << "]\n";
  }

  std::cout << "\nRESULT: " << (all_ok ? "PASS" : "FAIL") << "\n";
  return all_ok ? 0 : 2;
}

