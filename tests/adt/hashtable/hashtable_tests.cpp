/**
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// hashtable_tests.cpp
// v1 HashTable: default (multi-key) versus opt-in unique-key semantics, growth,
// removal, and concurrent claims.

#include "gtest/gtest.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <uflib/adt/adt_hashtable.h>
}

namespace
{
  struct KeyedItem
  {
    const char *key;
    int         id;
    int         refs;
  };

  struct NumItem
  {
    long id;
    int  tag;
  };

  HashTable *
  MakeKeyedTable(HashTable *storage, const char *name, bool unique)
  {
    HashTable *hash = HashTableLockingInstantiate(storage, (int)offsetof(KeyedItem, key), KEY_SIZE_ZERO,
                                                  HASH_ITEM_IS_PTR_TYPE, name, HASHTABLE_DEFAULT_EXTRACTOR);
    if (hash && unique) HashTableSetUniqueKeys(hash, true);

    return hash;
  }

  HashTable *
  MakeNumTable(HashTable *storage, const char *name, bool unique)
  {
    HashTable *hash = HashTableLockingInstantiate(storage, (int)offsetof(NumItem, id), (int)sizeof(long),
                                                  HASH_ITEM_NOT_PTR_TYPE, name, HASHTABLE_DEFAULT_EXTRACTOR);
    if (hash && unique) HashTableSetUniqueKeys(hash, true);

    return hash;
  }
} // namespace

// Default semantics must not change: two containers sharing a key are both stored.
TEST(HashTableKeys, DefaultAllowsDuplicateKeys)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "default_duplicates", false);
  ASSERT_NE(hash, nullptr);
  ASSERT_FALSE(HASHTABLE_ISUNIQUEKEYS(hash));

  KeyedItem a = {"same-name", 1};
  KeyedItem b = {"same-name", 2};

  bool is_added = false;

  EXPECT_EQ(AddToHashWithIndication(hash, &a, &is_added), &a);
  EXPECT_FALSE(is_added);
  EXPECT_EQ(AddToHashWithIndication(hash, &b, &is_added), &b);
  EXPECT_FALSE(is_added);

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 2);
  EXPECT_EQ(HashLookup(hash, (void*)"same-name", true), &a);

  HashTableDestruct(hash, false);
}

// The same container inserted twice is still rejected under both semantics.
TEST(HashTableKeys, DefaultRejectsSameContainer)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "default_same_container", false);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"one", 1};

  EXPECT_EQ(AddToHash(hash, &a), &a);
  EXPECT_EQ(AddToHash(hash, &a), &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// Unique mode: the second claimant is handed the stored container, not its own.
TEST(HashTableKeys, UniqueKeyRejectsSecondClaim)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_second_claim", true);
  ASSERT_NE(hash, nullptr);
  ASSERT_TRUE(HASHTABLE_ISUNIQUEKEYS(hash));

  KeyedItem winner = {"contested", 1};
  KeyedItem loser  = {"contested", 2};

  bool is_added = false;

  EXPECT_EQ(AddToHashWithIndication(hash, &winner, &is_added), &winner);
  EXPECT_FALSE(is_added);

  EXPECT_EQ(AddToHashWithIndication(hash, &loser, &is_added), &winner);
  EXPECT_TRUE(is_added);

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);
  EXPECT_EQ(HashLookup(hash, (void*)"contested", true), &winner);

  HashTableDestruct(hash, false);
}

// The no-indication wrapper still exposes the loser's verdict through the pointer.
TEST(HashTableKeys, UniqueKeyWithoutIndicationReturnsStoredContainer)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_no_indication", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem winner = {"claimed", 1};
  KeyedItem loser  = {"claimed", 2};

  EXPECT_EQ(AddToHash(hash, &winner), &winner);
  EXPECT_NE(AddToHash(hash, &loser), &loser);
  EXPECT_EQ(AddToHash(hash, &loser), &winner);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// Uniqueness has to survive table growth, including keys inserted before and after it.
TEST(HashTableKeys, UniqueKeySurvivesGrowth)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_growth", true);
  ASSERT_NE(hash, nullptr);

  std::vector<std::string> names;
  std::vector<KeyedItem>   items;

  names.reserve(4096);
  items.reserve(4096);

  for (int i = 0; i < 4096; ++i) {
    names.emplace_back("key-" + std::to_string(i));
  }
  for (int i = 0; i < 4096; ++i) {
    items.push_back(KeyedItem{names[i].c_str(), i});
  }
  for (auto &item : items) {
    ASSERT_EQ(AddToHash(hash, &item), &item);
  }

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 4096);

  KeyedItem pretender = {"key-17", -1};
  bool      is_added  = false;

  EXPECT_EQ(AddToHashWithIndication(hash, &pretender, &is_added), &items[17]);
  EXPECT_TRUE(is_added);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 4096);
  EXPECT_EQ(HashLookup(hash, (void*)"key-17", true), &items[17]);

  HashTableDestruct(hash, false);
}

// Removing the holder must release the key for a later claim.
TEST(HashTableKeys, UniqueKeyReleasedOnRemoval)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_release", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem first  = {"recycled-name", 1};
  KeyedItem second = {"recycled-name", 2};

  EXPECT_EQ(AddToHash(hash, &first), &first);
  EXPECT_EQ(AddToHash(hash, &second), &first);

  EXPECT_EQ(RemoveFromHash(hash, &first), &first);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 0);

  EXPECT_EQ(AddToHash(hash, &second), &second);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);
  EXPECT_EQ(HashLookup(hash, (void*)"recycled-name", true), &second);

  HashTableDestruct(hash, false);
}

// An empty-string key is a key like any other and must not bypass uniqueness.
TEST(HashTableKeys, UniqueKeyHandlesEmptyStringKey)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_empty_key", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem winner = {"", 1};
  KeyedItem loser  = {"", 2};

  EXPECT_EQ(AddToHash(hash, &winner), &winner);
  EXPECT_EQ(AddToHash(hash, &loser), &winner);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// Fixed-width keys are compared by value, not by address.
TEST(HashTableKeys, UniqueKeyUsesFixedWidthKeyValue)
{
  HashTable  storage{};
  HashTable *hash = MakeNumTable(&storage, "unique_fixed_key", true);
  ASSERT_NE(hash, nullptr);

  NumItem winner = {42, 1};
  NumItem loser  = {42, 2};
  NumItem other  = {43, 3};

  bool is_added = false;

  EXPECT_EQ(AddToHashWithIndication(hash, &winner, &is_added), &winner);
  EXPECT_EQ(AddToHashWithIndication(hash, &loser, &is_added), &winner);
  EXPECT_TRUE(is_added);
  EXPECT_EQ(AddToHashWithIndication(hash, &other, &is_added), &other);
  EXPECT_FALSE(is_added);

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 2);

  HashTableDestruct(hash, false);
}

// The registry case that motivated the option: N threads race for one name and
// exactly one of them owns it, with no external locking.
TEST(HashTableKeys, UniqueKeyConcurrentClaimsHaveSingleWinner)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_concurrent_claim", true);
  ASSERT_NE(hash, nullptr);

  constexpr int          kClaimants = 8;
  std::vector<KeyedItem> claimants(kClaimants);
  std::atomic<int>       winners{0};

  for (int i = 0; i < kClaimants; ++i) {
    claimants[i] = KeyedItem{"contended-name", i};
  }

  std::vector<std::thread> threads;
  threads.reserve(kClaimants);

  for (int i = 0; i < kClaimants; ++i) {
    threads.emplace_back([hash, &claimants, &winners, i]() {
      if (AddToHash(hash, &claimants[i]) == &claimants[i]) winners.fetch_add(1);
    });
  }
  for (auto &t : threads) t.join();

  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// Distinct names must all be accepted concurrently: uniqueness must not serialise
// unrelated claims into a single entry.
TEST(HashTableKeys, UniqueKeyConcurrentDistinctClaimsAllSucceed)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_concurrent_distinct", true);
  ASSERT_NE(hash, nullptr);

  constexpr int kClaimants = 8;

  std::vector<std::string> names;
  std::vector<KeyedItem>   claimants(kClaimants);
  names.reserve(kClaimants);
  for (int i = 0; i < kClaimants; ++i) names.emplace_back("distinct-" + std::to_string(i));
  for (int i = 0; i < kClaimants; ++i) claimants[i] = KeyedItem{names[i].c_str(), i};

  std::atomic<int>         winners{0};
  std::vector<std::thread> threads;
  threads.reserve(kClaimants);

  for (int i = 0; i < kClaimants; ++i) {
    threads.emplace_back([hash, &claimants, &winners, i]() {
      if (AddToHash(hash, &claimants[i]) == &claimants[i]) winners.fetch_add(1);
    });
  }
  for (auto &t : threads) t.join();

  EXPECT_EQ(winners.load(), kClaimants);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), kClaimants);

  HashTableDestruct(hash, false);
}

// ---------------------------------------------------------------------------
// Contract consistency across the other insertion paths
// ---------------------------------------------------------------------------

namespace
{
  void
  IncrementRefs(RecyclerClientData *data, int count)
  {
    KeyedItem *item = (KeyedItem*)data;
    item->refs      += count;
  }

  void *
  EvictFirst(ContextData *ctx)
  {
    return ctx;
  }
} // namespace

// AddToHashWithReference must reference the container that ends up holding the key.
TEST(HashTableContract, ReferencePathHonoursUniqueKey)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "contract_reference", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem winner = {"ref-name", 1, 0};
  KeyedItem loser  = {"ref-name", 2, 0};

  EXPECT_EQ(AddToHashWithReference(hash, &winner, IncrementRefs), &winner);
  EXPECT_EQ(winner.refs, 1);

  EXPECT_EQ(AddToHashWithReference(hash, &loser, IncrementRefs), &winner);
  EXPECT_EQ(loser.refs, 0) << "the rejected container must not be referenced";
  EXPECT_EQ(winner.refs, 1) << "a refused claim must not add a second reference to the holder";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// Same call on a default table keeps the old behaviour: same container, counted once.
TEST(HashTableContract, ReferencePathKeepsDefaultSemantics)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "contract_reference_default", false);
  ASSERT_NE(hash, nullptr);

  KeyedItem item = {"ref-default", 1, 0};

  EXPECT_EQ(AddToHashWithReference(hash, &item, IncrementRefs), &item);
  EXPECT_EQ(AddToHashWithReference(hash, &item, IncrementRefs), &item);
  EXPECT_EQ(item.refs, 1) << "the container is held once, however often it is offered";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// At capacity with the key already present, the evict path must report the holder and evict nothing.
TEST(HashTableContract, EvictPathReturnsHolderWithoutEvicting)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "contract_evict_unique", true);
  ASSERT_NE(hash, nullptr);

  hash->flag_resizable = 0;
  hash->max_size       = 4;

  KeyedItem items[4] = {{"c-0", 0, 0}, {"c-1", 1, 0}, {"c-2", 2, 0}, {"c-3", 3, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);
  ASSERT_EQ(HASHTABLE_ENTRIES(hash), 4);

  KeyedItem pretender = {"c-2", 99, 0};
  void *    evicted   = nullptr;

  EXPECT_EQ(AddToHashEvictIfNecessary(hash, (const void*)"c-2", &pretender, EvictFirst, &items[0], &evicted),
            &items[2]);
  EXPECT_EQ(evicted, nullptr) << "an existing key needs no eviction";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 4);

  HashTableDestruct(hash, false);
}

// The reference-carrying evict path must reference the holder, not the rejected container.
TEST(HashTableContract, EvictWithReferencePathReferencesHolder)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "contract_evict_reference", true);
  ASSERT_NE(hash, nullptr);

  hash->flag_resizable = 0;
  hash->max_size       = 4;

  KeyedItem items[4] = {{"r-0", 0, 0}, {"r-1", 1, 0}, {"r-2", 2, 0}, {"r-3", 3, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);

  KeyedItem pretender = {"r-1", 99, 0};
  void *    evicted   = nullptr;

  EXPECT_EQ(AddToHashEvictIfNecessaryWithReference(hash, (const void*)"r-1", &pretender, EvictFirst, &evicted,
                                                   IncrementRefs, IncrementRefs), &items[1]);
  EXPECT_EQ(pretender.refs, 0) << "the rejected container must not be referenced";
  EXPECT_EQ(items[1].refs, 0) << "the holder was already installed, so this call takes no reference";
  EXPECT_EQ(evicted, nullptr);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 4);

  HashTableDestruct(hash, false);
}

// ---------------------------------------------------------------------------
// Merge, enumeration, removal chains, eviction, fixed size, lifecycle, locks
// ---------------------------------------------------------------------------

TEST(HashTableCoverage, MergeMakesEverySourceKeyFindableAndLeavesSourceIntact)
{
  HashTable dest_storage{};
  HashTable src_storage{};

  HashTable *dest = MakeKeyedTable(&dest_storage, "cov_merge_dest", false);
  HashTable *src  = MakeKeyedTable(&src_storage, "cov_merge_src", false);
  ASSERT_NE(dest, nullptr);
  ASSERT_NE(src, nullptr);

  KeyedItem local = {"d-local", 1, 0};
  KeyedItem one   = {"s-one", 2, 0};
  KeyedItem two   = {"s-two", 3, 0};

  ASSERT_EQ(AddToHash(dest, &local), &local);
  ASSERT_EQ(AddToHash(src, &one), &one);
  ASSERT_EQ(AddToHash(src, &two), &two);

  MergeHashEntries(dest, src);

  EXPECT_EQ(HashLookup(dest, (void*)"d-local", true), &local);
  EXPECT_EQ(HashLookup(dest, (void*)"s-one", true), &one);
  EXPECT_EQ(HashLookup(dest, (void*)"s-two", true), &two);
  EXPECT_EQ(HASHTABLE_ENTRIES(dest), 3);

  EXPECT_EQ(HASHTABLE_ENTRIES(src), 2);
  EXPECT_EQ(HashLookup(src, (void*)"s-one", true), &one);

  HashTableDestruct(src, false);
  HashTableDestruct(dest, false);
}

TEST(HashTableCoverage, EnumerationReturnsExactlyTheInsertedEntries)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "cov_enumerate", false);
  ASSERT_NE(hash, nullptr);

  constexpr int            kItems = 24;
  std::vector<std::string> names;
  std::vector<KeyedItem>   items;
  names.reserve(kItems);
  items.reserve(kItems);
  for (int i = 0; i < kItems; ++i) names.emplace_back("cov-enum-" + std::to_string(i));
  for (int i = 0; i < kItems; ++i) items.push_back(KeyedItem{names[i].c_str(), i, 0});
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);

  std::vector<void*> slots(kItems, nullptr);
  const int          found = GetHashEntries(hash, slots.data(), (long)slots.size());

  EXPECT_EQ(found, kItems);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), kItems);

  for (int i = 0; i < found; ++i) {
    bool known = false;
    for (auto &item : items) {
      if (slots[i] == &item) {
        known = true;
        break;
      }
    }
    EXPECT_TRUE(known) << "enumeration returned a pointer that was never inserted";
  }

  std::vector<void*> bounded(5, nullptr);
  EXPECT_EQ(GetHashEntries(hash, bounded.data(), (long)bounded.size()), 5);

  HashTableDestruct(hash, false);
}

TEST(HashTableCoverage, LookupWithReferenceIncrementsTheHolderOnHitOnly)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "cov_lookup_ref", false);
  ASSERT_NE(hash, nullptr);

  KeyedItem item = {"cov-ref", 1, 0};
  ASSERT_EQ(AddToHash(hash, &item), &item);

  EXPECT_EQ(HashLookupWithReference(hash, (const void*)"cov-ref", IncrementRefs), &item);
  EXPECT_EQ(item.refs, 1);

  EXPECT_EQ(HashLookupWithReference(hash, (const void*)"cov-absent", IncrementRefs), nullptr);
  EXPECT_EQ(item.refs, 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableCoverage, RemoveFromHashWithReferenceDecrementsTheRemovedHolder)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "cov_remove_ref", false);
  ASSERT_NE(hash, nullptr);

  KeyedItem item = {"cov-remove-ref", 1, 3};
  ASSERT_EQ(AddToHash(hash, &item), &item);

  auto decrement = [](RecyclerClientData *data, int count) {
    KeyedItem *target = (KeyedItem*)data;
    target->refs      -= count;
  };

  EXPECT_EQ(RemoveFromHashWithReference(hash, &item, decrement), &item);
  EXPECT_EQ(item.refs, 2);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 0);
  EXPECT_EQ(HashLookup(hash, (void*)"cov-remove-ref", true), nullptr);

  HashTableDestruct(hash, false);
}

TEST(HashTableCoverage, SurvivorsStayFindableThroughBulkRemoval)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "cov_removal_chain", false);
  ASSERT_NE(hash, nullptr);

  constexpr int            kItems = 300;
  std::vector<std::string> names;
  std::vector<KeyedItem>   items;
  names.reserve(kItems);
  items.reserve(kItems);
  for (int i = 0; i < kItems; ++i) names.emplace_back("chain-" + std::to_string(i));
  for (int i = 0; i < kItems; ++i) items.push_back(KeyedItem{names[i].c_str(), i, 0});
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);

  int removed = 0;
  for (int i = 0; i < kItems; ++i) {
    if (i % 3 == 0) {
      ASSERT_EQ(RemoveFromHash(hash, &items[i]), &items[i]);
      ++removed;
    }
  }
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), kItems - removed);

  for (int i = 0; i < kItems; ++i) {
    if (i % 3 == 0) EXPECT_EQ(HashLookup(hash, (void*)items[i].key, true), nullptr);
    else EXPECT_EQ(HashLookup(hash, (void*)items[i].key, true), &items[i]);
  }

  HashTableDestruct(hash, false);
}

TEST(HashTableCoverage, EvictionAtCapacityReplacesTheVictim)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "cov_evict", false);
  ASSERT_NE(hash, nullptr);

  hash->flag_resizable = 0;
  hash->max_size       = 4;

  KeyedItem items[4] = {{"v-0", 0, 0}, {"v-1", 1, 0}, {"v-2", 2, 0}, {"v-3", 3, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);
  ASSERT_EQ(HASHTABLE_ENTRIES(hash), 4);

  KeyedItem newcomer = {"v-new", 9, 0};
  void *    evicted  = nullptr;

  EXPECT_EQ(AddToHashEvictIfNecessary(hash, (const void*)"v-new", &newcomer, EvictFirst, &items[2], &evicted),
            &newcomer);
  EXPECT_EQ(evicted, &items[2]);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 4);
  EXPECT_EQ(HashLookup(hash, (void*)"v-new", true), &newcomer);
  EXPECT_EQ(HashLookup(hash, (void*)"v-2", true), nullptr);

  HashTableDestruct(hash, false);
}

TEST(HashTableCoverage, FixedSizeTableRefusesBeyondCapacity)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "cov_fixed", false);
  ASSERT_NE(hash, nullptr);

  hash->flag_resizable = 0;
  hash->max_size       = 4;

  KeyedItem items[4] = {{"f-0", 0, 0}, {"f-1", 1, 0}, {"f-2", 2, 0}, {"f-3", 3, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);
  ASSERT_EQ(HASHTABLE_ENTRIES(hash), 4);

  KeyedItem overflow = {"f-overflow", 4, 0};

  EXPECT_EQ(AddToHash(hash, &overflow), nullptr) << "a full fixed-size table must not silently accept or evict";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 4);
  EXPECT_EQ(HashLookup(hash, (void*)"f-overflow", true), nullptr);
  for (auto &item : items) EXPECT_EQ(HashLookup(hash, (void*)item.key, true), &item);

  HashTableDestruct(hash, false);
}

TEST(HashTableCoverage, DestructReleasesTableAndReinstantiationResetsTheOption)
{
  HashTable *hash = MakeKeyedTable(nullptr, "cov_lifecycle", true);
  ASSERT_NE(hash, nullptr);
  ASSERT_TRUE(HASHTABLE_ISUNIQUEKEYS(hash));

  KeyedItem item = {"life", 1, 0};
  ASSERT_EQ(AddToHash(hash, &item), &item);

  HashTableDestruct(hash, HASHTABLE_SELF_DESTRUCT);

  HashTable  storage{};
  HashTable *again = MakeKeyedTable(&storage, "cov_lifecycle_again", false);
  ASSERT_NE(again, nullptr);
  EXPECT_FALSE(HASHTABLE_ISUNIQUEKEYS(again));
  EXPECT_EQ(HASHTABLE_ENTRIES(again), 0);

  KeyedItem fresh = {"life", 2, 0};
  EXPECT_EQ(AddToHash(again, &fresh), &fresh);

  HashTableDestruct(again, false);
}

TEST(HashTableCoverage, LockHelpersAreSafeOnBothTableKinds)
{
  HashTable  plain_storage{};
  HashTable *plain = MakeKeyedTable(&plain_storage, "cov_locks_plain", false);
  ASSERT_NE(plain, nullptr);

  EXPECT_EQ(HashTable_WrLock(plain, 0), 0);
  EXPECT_EQ(HashTable_UnLock(plain), 0);
  EXPECT_EQ(HashTable_RdLock(plain, 0), 0);
  EXPECT_EQ(HashTable_UnLock(plain), 0);

  HashTableDestruct(plain, false);

  HashTable  locking_storage{};
  HashTable *locking = MakeKeyedTable(&locking_storage, "cov_locks_locking", false);
  ASSERT_NE(locking, nullptr);

  EXPECT_EQ(HashTable_RdLock(locking, 1), 0);
  EXPECT_EQ(HashTable_UnLock(locking), 0);
  EXPECT_EQ(HashTable_WrLock(locking, 1), 0);
  EXPECT_EQ(HashTable_UnLock(locking), 0);

  KeyedItem item = {"post-lock", 1, 0};
  EXPECT_EQ(AddToHash(locking, &item), &item);

  HashTableDestruct(locking, false);
}

// ---------------------------------------------------------------------------
// Adversarial coverage: re-instantiation, contention, NULL arguments, eviction
// edge cases, unbootstrapped tables
// ---------------------------------------------------------------------------

TEST(HashTableAdversarial, ReinstantiationOfALiveTableStartsClean)
{
  HashTable  storage{};
  HashTable *first = MakeKeyedTable(&storage, "adv_reinstantiate", true);
  ASSERT_NE(first, nullptr);

  KeyedItem stale = {"adv-stale", 1, 0};
  ASSERT_EQ(AddToHash(first, &stale), &stale);
  ASSERT_EQ(HASHTABLE_ENTRIES(first), 1);

  HashTable *second = MakeKeyedTable(&storage, "adv_reinstantiate_2", false);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second, first);
  EXPECT_FALSE(HASHTABLE_ISUNIQUEKEYS(second)) << "re-instantiation must not inherit the option";
  EXPECT_EQ(HASHTABLE_ENTRIES(second), 0);
  EXPECT_EQ(HashLookup(second, (void*)"adv-stale", true), nullptr);

  KeyedItem fresh = {"adv-fresh", 2, 0};
  EXPECT_EQ(AddToHash(second, &fresh), &fresh);
  EXPECT_EQ(HASHTABLE_ENTRIES(second), 1);

  HashTableDestruct(second, false);
}

TEST(HashTableAdversarial, TryLocksFailWhileAnotherThreadHoldsTheWriteLock)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "adv_contention", false);
  ASSERT_NE(hash, nullptr);

  ASSERT_EQ(HashTable_WrLock(hash, 1), 0);

  std::atomic<int> rd_result{-999};
  std::atomic<int> wr_result{-999};

  std::thread reader([&]() { rd_result.store(HashTable_RdLock(hash, 1)); });
  reader.join();

  std::thread writer([&]() { wr_result.store(HashTable_WrLock(hash, 1)); });
  writer.join();

  EXPECT_NE(rd_result.load(), 0) << "read lock must be refused while a writer holds the table";
  EXPECT_NE(wr_result.load(), 0) << "write lock must be refused while a writer holds the table";

  EXPECT_EQ(HashTable_UnLock(hash), 0);

  std::atomic<int> after{-999};
  std::thread      again([&]() { after.store(HashTable_RdLock(hash, 1)); });
  again.join();
  EXPECT_EQ(after.load(), 0) << "lock must be grantable once released";
  HashTable_UnLock(hash);

  HashTableDestruct(hash, false);
}

TEST(HashTableAdversarial, NullAddIsRejected)
{
  KeyedItem item  = {"adv-null", 1, 0};
  bool      added = false;

  EXPECT_EQ(AddToHash(nullptr, &item), nullptr);
  EXPECT_EQ(AddToHashWithIndication(nullptr, &item, &added), nullptr);
  EXPECT_EQ(AddToHashWithReference(nullptr, &item, IncrementRefs), nullptr);
  EXPECT_EQ(AddToHashEvictIfNecessary(nullptr, (const void*)"adv-null", &item, EvictFirst, nullptr, nullptr), nullptr);
}

TEST(HashTableAdversarial, NullLookupIsRejected)
{
  EXPECT_EQ(HashLookup(nullptr, (void*)"adv-null", true), nullptr);
  EXPECT_EQ(HashLookupWithReference(nullptr, (const void*)"adv-null", IncrementRefs), nullptr);
}

TEST(HashTableAdversarial, NullRemoveIsRejected)
{
  KeyedItem item = {"adv-null", 1, 0};

  EXPECT_EQ(RemoveFromHash(nullptr, &item), nullptr);
  EXPECT_EQ(RemoveFromHashWithReference(nullptr, &item, IncrementRefs), nullptr);
}

TEST(HashTableAdversarial, NullEnumerateIsRejected)
{
  EXPECT_EQ(GetHashEntries(nullptr, nullptr, 0), -1);
}

TEST(HashTableAdversarial, NullSetUniqueKeysIsRejected)
{
  EXPECT_EQ(HashTableSetUniqueKeys(nullptr, true), nullptr);
}

TEST(HashTableAdversarial, NullLockHelpersAreSafe)
{
  EXPECT_EQ(HashTable_RdLock(nullptr, 0), 0);
  EXPECT_EQ(HashTable_WrLock(nullptr, 0), 0);
  EXPECT_EQ(HashTable_UnLock(nullptr), 0);
}

TEST(HashTableAdversarial, NullMergeAndDestructAreSafe)
{
  MergeHashEntries(nullptr, nullptr);
  HashTableDestruct(nullptr, false);
}

// The call is made in a child process: if the table dereferences the NULL
// container the crash is recorded as a failure instead of aborting the suite.
TEST(HashTableAdversarial, NullItemIsRejected)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "adv_null_item", false);
  ASSERT_NE(hash, nullptr);

  pid_t pid = fork();
  ASSERT_GE(pid, 0) << "could not fork";

  if (pid == 0) {
    bool accepted = true;
    (void)AddToHashWithIndication(hash, nullptr, &accepted);
    _exit(0);
  }

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);

  EXPECT_TRUE(WIFEXITED(status)) << "AddToHashWithIndication(hash, NULL, ...) terminated the process (signal " << (
    WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ')';
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 0);

  HashTableDestruct(hash, false);
}

TEST(HashTableAdversarial, LookupOnUnbootstrappedTableIsSafe)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "adv_unbootstrapped", false);
  ASSERT_NE(hash, nullptr);

  EXPECT_EQ(HASHTABLE_SIZE(hash), 0);
  EXPECT_EQ(HashLookup(hash, (void*)"never", true), nullptr);
  EXPECT_EQ(HashLookupWithReference(hash, (const void*)"never", IncrementRefs), nullptr);

  void *slot = nullptr;
  EXPECT_EQ(GetHashEntries(hash, &slot, 1), -1);
  EXPECT_EQ(RemoveFromHash(hash, &slot), nullptr);

  HashTableDestruct(hash, false);
}

TEST(HashTableAdversarial, EvictionWithNoVictimStillInstallsOrReportsFailure)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "adv_evict_null", false);
  ASSERT_NE(hash, nullptr);

  hash->flag_resizable = 0;
  hash->max_size       = 3;

  KeyedItem items[3] = {{"n-0", 0, 0}, {"n-1", 1, 0}, {"n-2", 2, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);
  ASSERT_EQ(HASHTABLE_ENTRIES(hash), 3);

  KeyedItem newcomer = {"n-new", 9, 0};
  void *    evicted  = reinterpret_cast<void*>(0x1);

  void *result = AddToHashEvictIfNecessary(hash, nullptr, &newcomer, nullptr, nullptr, &evicted);
  EXPECT_EQ(evicted, reinterpret_cast<void*>(0x1)) << "no victim callback ran, so nothing may be reported evicted";

  if (result == nullptr) EXPECT_EQ(HASHTABLE_ENTRIES(hash), 3);
  else EXPECT_EQ(HASHTABLE_ENTRIES(hash), 3) << "capacity must hold even when the evictor cannot supply a victim";

  HashTableDestruct(hash, false);
}

TEST(HashTableAdversarial, EvictionOfAVictimNotInTheTableIsHarmless)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "adv_evict_alien", false);
  ASSERT_NE(hash, nullptr);

  hash->flag_resizable = 0;
  hash->max_size       = 2;

  KeyedItem items[2] = {{"a-0", 0, 0}, {"a-1", 1, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);

  KeyedItem alien    = {"a-alien", 7, 0};
  KeyedItem newcomer = {"a-new", 9, 0};
  void *    evicted  = nullptr;

  (void)AddToHashEvictIfNecessary(hash, (const void*)"a-new", &newcomer, EvictFirst, &alien, &evicted);

  for (auto &item : items)
    EXPECT_EQ(HashLookup(hash, (void*)item.key, true), &item) <<
      "a victim that is not in the table must not remove a resident";

  HashTableDestruct(hash, false);
}

TEST(HashTableAdversarial, MergeWithEmptyOrUnallocatedTablesIsSafe)
{
  HashTable dest_storage{};
  HashTable src_storage{};

  HashTable *dest = MakeKeyedTable(&dest_storage, "adv_merge_dest", false);
  HashTable *src  = MakeKeyedTable(&src_storage, "adv_merge_src", false);
  ASSERT_NE(dest, nullptr);
  ASSERT_NE(src, nullptr);

  KeyedItem local = {"adv-merge-local", 1, 0};
  ASSERT_EQ(AddToHash(dest, &local), &local);

  MergeHashEntries(dest, src);
  EXPECT_EQ(HASHTABLE_ENTRIES(dest), 1);
  EXPECT_EQ(HashLookup(dest, (void*)"adv-merge-local", true), &local);

  HashTable *empty_dest = MakeKeyedTable(nullptr, "adv_merge_empty_dest", false);
  ASSERT_NE(empty_dest, nullptr);
  MergeHashEntries(empty_dest, src);
  EXPECT_EQ(HASHTABLE_ENTRIES(empty_dest), 0);

  MergeHashEntries(empty_dest, nullptr);
  MergeHashEntries(nullptr, src);

  KeyedItem inbound = {"adv-merge-inbound", 2, 0};
  ASSERT_EQ(AddToHash(src, &inbound), &inbound);
  MergeHashEntries(empty_dest, src);
  EXPECT_EQ(HashLookup(empty_dest, (void*)"adv-merge-inbound", true), &inbound);

  HashTableDestruct(empty_dest, HASHTABLE_SELF_DESTRUCT);
  HashTableDestruct(src, false);
  HashTableDestruct(dest, false);
}

TEST(HashTableAdversarial, DuplicateClaimOnUniqueTableUnderContentionYieldsOneHolder)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "adv_unique_contention", true);
  ASSERT_NE(hash, nullptr);

  constexpr int          kThreads = 6;
  std::vector<KeyedItem> claimants(kThreads);
  for (int i = 0; i < kThreads; ++i) claimants[i] = KeyedItem{"adv-contended", i, 0};

  std::atomic<int>         holders{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      for (int attempt = 0; attempt < 50; ++attempt) {
        if (HashLookupWithReference(hash, (const void*)"adv-contended", nullptr) == nullptr) {
          if (AddToHash(hash, &claimants[i]) == &claimants[i]) holders.fetch_add(1);
        }
      }
    });
  }
  for (auto &t : threads) t.join();

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1) << "a unique table must never hold two entries for one key";
  EXPECT_GE(holders.load(), 1);

  KeyedItem *stored = (KeyedItem*)HashLookup(hash, (void*)"adv-contended", true);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->key, claimants[stored->id].key);

  HashTableDestruct(hash, false);
}

// ---------------------------------------------------------------------------
// Known gaps: behaviour that must be reported, not left unexercised
// ---------------------------------------------------------------------------

namespace
{
  // Runs body in a forked child under an alarm, so a deadlock is reported as a
  // failure instead of hanging the suite.
  template <typename Body>
  int
  RunWithDeadline(Body body, unsigned seconds)
  {
    pid_t pid = fork();
    if (pid == 0) {
      alarm(seconds);
      body();
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
  }
} // namespace

TEST(HashTableGaps, SelfMergeTerminates)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "gap_self_merge", false);
  ASSERT_NE(hash, nullptr);

  KeyedItem items[5] = {{"self-0", 0, 0}, {"self-1", 1, 0}, {"self-2", 2, 0}, {"self-3", 3, 0}, {"self-4", 4, 0}};
  for (auto &item : items) ASSERT_EQ(AddToHash(hash, &item), &item);

  int status = RunWithDeadline([hash]() { MergeHashEntries(hash, hash); }, 3);

  EXPECT_TRUE(WIFEXITED(status)) <<
    "MergeHashEntries(h, h) did not terminate: read lock on the source then write lock on the "
    "same table is a same-thread upgrade (signal " << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ')';
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 5) << "a self-merge must not lose entries";

  HashTableDestruct(hash, false);
}

TEST(HashTableGaps, MergeIntoFullFixedDestinationRefusesAtomically)
{
  HashTable dest_storage{};
  HashTable src_storage{};

  HashTable *dest = MakeKeyedTable(&dest_storage, "gap_merge_full_dest", false);
  HashTable *src  = MakeKeyedTable(&src_storage, "gap_merge_full_src", false);
  ASSERT_NE(dest, nullptr);
  ASSERT_NE(src, nullptr);

  dest->flag_resizable = 0;
  dest->max_size       = 2;

  KeyedItem residents[2] = {{"dest-0", 10, 0}, {"dest-1", 11, 0}};
  KeyedItem incoming[2]  = {{"src-0", 20, 0}, {"src-1", 21, 0}};

  for (auto &item : residents) ASSERT_EQ(AddToHash(dest, &item), &item);
  for (auto &item : incoming) ASSERT_EQ(AddToHash(src, &item), &item);
  ASSERT_EQ(HASHTABLE_ENTRIES(dest), 2);

  EXPECT_EQ(MergeHashEntries(dest, src), HASHTABLE_MERGE_REFUSED) <<
    "a full fixed-size destination must refuse the snapshot and say so";

  EXPECT_EQ(HASHTABLE_ENTRIES(dest), 2) << "refusal must leave the destination unchanged";
  EXPECT_EQ(HASHTABLE_ENTRIES(src), 2) << "refusal must leave the source untouched";

  EXPECT_EQ(HashLookup(dest, (void*)"dest-0", true), &residents[0]);
  EXPECT_EQ(HashLookup(dest, (void*)"dest-1", true), &residents[1]);

  EXPECT_EQ(HashLookup(dest, (void*)"src-0", true), nullptr) <<
    "refusal must not admit a partial merge";
  EXPECT_EQ(HashLookup(dest, (void*)"src-1", true), nullptr) <<
    "refusal must not admit a partial merge";

  HashTableDestruct(src, false);
  HashTableDestruct(dest, false);
}

TEST(HashTableGaps, OppositeDirectionMergesDoNotDeadlock)
{
  HashTable storage_a{};
  HashTable storage_b{};

  HashTable *a = MakeKeyedTable(&storage_a, "gap_abba_a", false);
  HashTable *b = MakeKeyedTable(&storage_b, "gap_abba_b", false);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  KeyedItem in_a = {"abba-a", 1, 0};
  KeyedItem in_b = {"abba-b", 2, 0};
  ASSERT_EQ(AddToHash(a, &in_a), &in_a);
  ASSERT_EQ(AddToHash(b, &in_b), &in_b);

  int status = RunWithDeadline([a, b]() {
    std::thread one([a, b]() { MergeHashEntries(b, a); }); /* rdlock a, then wrlock b */
    std::thread two([a, b]() { MergeHashEntries(a, b); }); /* rdlock b, then wrlock a */
    one.join();
    two.join();
  }, 5);

  EXPECT_TRUE(WIFEXITED(status)) << "opposite-direction merges deadlocked (signal " << (
    WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ')';

  HashTableDestruct(b, false);
  HashTableDestruct(a, false);
}

// ---------------------------------------------------------------------------
// AddToHashUniquely: what the caller is told about the key it offered
// ---------------------------------------------------------------------------

TEST(HashTableUniqueInsert, FirstClaimStoresTheOfferedContainer)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_claim_first", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a            = {"claimant", 1, 0};
  bool      was_inserted = false;
  void *    resident     = nullptr;

  EXPECT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  EXPECT_TRUE(was_inserted);
  EXPECT_EQ(resident, &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, SecondInstanceWithSameKeyIsRefused)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_claim_second", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"same-key", 1, 0};
  KeyedItem b = {"same-key", 2, 0};

  bool  was_inserted = false;
  void *resident     = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  ASSERT_TRUE(was_inserted);

  void *result = AddToHashUniquely(hash, &b, &was_inserted, &resident);

  EXPECT_NE(result, nullptr) << "a refusal is not an error: the incumbent must be returned";
  EXPECT_EQ(result, &a) << "the refused caller must be told that another instance holds the key";
  EXPECT_FALSE(was_inserted);
  EXPECT_EQ(resident, &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, RefusedContainerIsNotStored)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_refused_absent", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"held", 1, 0};
  KeyedItem b = {"held", 2, 0};

  bool  was_inserted = false;
  void *resident     = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  ASSERT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &a);

  void *entries[8] = {nullptr};
  int   count      = GetHashEntries(hash, entries, 8);
  ASSERT_LE(count, 8);

  for (int i = 0; i < count; ++i) {
    EXPECT_NE(entries[i], &b) << "the refused container was stored anyway";
  }

  EXPECT_EQ(HashLookup(hash, (void*)"held", true), &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, ReOfferingTheResidentIsNotAnInsert)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_reoffer", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"idempotent", 1, 0};

  bool  was_inserted = false;
  void *resident     = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  ASSERT_TRUE(was_inserted);

  EXPECT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  EXPECT_FALSE(was_inserted) << "nothing was added: the offered container was already resident";
  EXPECT_EQ(resident, &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, ClaimAfterRemovalSucceeds)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_after_remove", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"released", 1, 0};
  KeyedItem b = {"released", 2, 0};

  bool  was_inserted = false;
  void *resident     = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  ASSERT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &a);
  ASSERT_FALSE(was_inserted);

  ASSERT_EQ(RemoveFromHash(hash, &a), &a);
  ASSERT_EQ(HASHTABLE_ENTRIES(hash), 0);

  EXPECT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &b);
  EXPECT_TRUE(was_inserted) << "the refused instance must be able to claim once the incumbent is gone";
  EXPECT_EQ(resident, &b);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, RemoveOfRefusedContainerDoesNotEvictIncumbent)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_remove_loser", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"survivor", 1, 0};
  KeyedItem b = {"survivor", 2, 0};

  bool  was_inserted = false;
  void *resident     = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  ASSERT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &a);

  EXPECT_EQ(RemoveFromHash(hash, &b), nullptr) << "a container that was never stored cannot be removed";
  EXPECT_EQ(HashLookup(hash, (void*)"survivor", true), &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, RefusedContainerCanClaimAnotherKey)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_reclaim", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"taken", 1, 0};
  KeyedItem b = {"taken", 2, 0};

  bool  was_inserted = false;
  void *resident     = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  ASSERT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &a);
  ASSERT_FALSE(was_inserted);

  b.key = "free";

  EXPECT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &b);
  EXPECT_TRUE(was_inserted) << "the refused container must be untouched by the refusal";
  EXPECT_EQ(resident, &b);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 2);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, NonUniqueTableRefusesWithoutSideEffects)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_on_default_table", false);
  ASSERT_NE(hash, nullptr);
  ASSERT_FALSE(HASHTABLE_ISUNIQUEKEYS(hash));

  KeyedItem a            = {"unclaimable", 1, 0};
  bool      was_inserted = true;
  void *    resident     = (void*)&storage;

  EXPECT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), nullptr) <<
    "a table without unique-key semantics cannot honour the contract";
  EXPECT_FALSE(was_inserted);
  EXPECT_EQ(resident, nullptr);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 0) << "the refusal must not insert";
  EXPECT_EQ(HashLookup(hash, (void*)"unclaimable", true), nullptr);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, NullArgumentsAreRejected)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_null_args", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a            = {"null-test", 1, 0};
  bool      was_inserted = true;
  void *    resident     = (void*)&storage;

  EXPECT_EQ(AddToHashUniquely(nullptr, &a, &was_inserted, &resident), nullptr);
  EXPECT_FALSE(was_inserted);
  EXPECT_EQ(resident, nullptr);

  was_inserted = true;
  resident     = (void*)&storage;

  EXPECT_EQ(AddToHashUniquely(hash, nullptr, &was_inserted, &resident), nullptr);
  EXPECT_FALSE(was_inserted);
  EXPECT_EQ(resident, nullptr);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 0);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, NullOutputParametersAreTolerated)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_null_outs", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"silent", 1, 0};

  EXPECT_EQ(AddToHashUniquely(hash, &a, nullptr, nullptr), &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);
  EXPECT_EQ(HashLookup(hash, (void*)"silent", true), &a);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, OutputsAreOverwrittenOnEveryPath)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_out_paths", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"path", 1, 0};
  KeyedItem b = {"path", 2, 0};

  bool  was_inserted = true;
  void *resident     = (void*)0x1;

  ASSERT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  EXPECT_TRUE(was_inserted);
  EXPECT_EQ(resident, &a);

  was_inserted = true;
  resident     = (void*)0x1;
  ASSERT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &a);
  EXPECT_FALSE(was_inserted);
  EXPECT_EQ(resident, &a);

  was_inserted = true;
  resident     = (void*)0x1;
  ASSERT_EQ(AddToHashUniquely(nullptr, &b, &was_inserted, &resident), nullptr);
  EXPECT_FALSE(was_inserted);
  EXPECT_EQ(resident, nullptr);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, FixedWidthKeyTableClaimsUniquely)
{
  HashTable  storage{};
  HashTable *hash = MakeNumTable(&storage, "unique_fixed_key", true);
  ASSERT_NE(hash, nullptr);

  NumItem a = {42, 1};
  NumItem b = {42, 2};

  bool  was_inserted = false;
  void *resident     = nullptr;

  EXPECT_EQ(AddToHashUniquely(hash, &a, &was_inserted, &resident), &a);
  EXPECT_TRUE(was_inserted);

  EXPECT_EQ(AddToHashUniquely(hash, &b, &was_inserted, &resident), &a);
  EXPECT_FALSE(was_inserted) << "the key is the numeric id, not the container address";
  EXPECT_EQ(resident, &a);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, ThousandOffersOnOneKeyLeaveOneEntry)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_repeated", true);
  ASSERT_NE(hash, nullptr);

  std::vector<KeyedItem> items;
  for (int i = 0; i < 1000; ++i) items.push_back(KeyedItem{"only-one", i, 0});

  int   winners = 0;
  void *holder  = nullptr;
  for (auto &item : items) {
    bool  was_inserted = false;
    void *resident     = nullptr;
    void *result       = AddToHashUniquely(hash, &item, &was_inserted, &resident);
    if (was_inserted) {
      winners++;
      holder = result;
    }
    else EXPECT_EQ(result, holder) << "every refusal must name the same incumbent";
  }

  EXPECT_EQ(winners, 1);
  EXPECT_EQ(holder, HashLookup(hash, (void*)"only-one", true));
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, UniqueClaimsSurviveGrowth)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_growth", true);
  ASSERT_NE(hash, nullptr);

  std::vector<std::string> keys;
  std::vector<KeyedItem>   items;
  keys.reserve(2000);
  items.reserve(2000);

  for (int i = 0; i < 2000; ++i) {
    keys.push_back("growth-" + std::to_string(i));
    items.push_back(KeyedItem{keys.back().c_str(), i, 0});
  }

  for (auto &item : items) {
    bool  was_inserted = false;
    void *resident     = nullptr;
    ASSERT_EQ(AddToHashUniquely(hash, &item, &was_inserted, &resident), &item);
    ASSERT_TRUE(was_inserted);
  }

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 2000);
  for (auto &item : items) {
    EXPECT_EQ(HashLookup(hash, (void*)item.key, true), &item);
  }

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, ResidentKeyMutationMustNotAdmitASecondEntry)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_mutated_key", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem resident     = {"alpha", 1, 0};
  bool      was_inserted = false;
  void *    holder       = nullptr;
  ASSERT_EQ(AddToHashUniquely(hash, &resident, &was_inserted, &holder), &resident);
  ASSERT_TRUE(was_inserted);

  resident.key = "beta";

  KeyedItem newcomer = {"beta", 2, 0};
  void *    result   = AddToHashUniquely(hash, &newcomer, &was_inserted, &holder);

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1) <<
    "a unique table admitted a second entry for key 'beta' because the incumbent's key was " "altered in place";
  EXPECT_FALSE(was_inserted) << "the key 'beta' was already held; the newcomer must be refused";
  EXPECT_EQ(result, &resident);

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, ConcurrentClaimsReportExactlyOneWinner)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_race", true);
  ASSERT_NE(hash, nullptr);

  constexpr int          kThreads = 8;
  std::vector<KeyedItem> items(kThreads);
  for (int i = 0; i < kThreads; ++i) items[i] = KeyedItem{"raced", i, 0};

  std::atomic<int> winners{0};
  std::atomic<int> mismatches{0};

  {
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&hash, &items, &winners, &mismatches, i]() {
        bool  was_inserted = false;
        void *resident     = nullptr;
        void *result       = AddToHashUniquely(hash, &items[i], &was_inserted, &resident);

        if (result == nullptr || resident != result) mismatches.fetch_add(1);
        else if (was_inserted) {
          if (result != &items[i]) mismatches.fetch_add(1);
          else winners.fetch_add(1);
        }
        else if (resident == &items[i]) mismatches.fetch_add(1);
      });
    }
    for (auto &thread : threads) thread.join();
  }

  EXPECT_EQ(mismatches.load(), 0) << "a claim report contradicted the resident it returned";
  EXPECT_EQ(winners.load(), 1) << "exactly one claimant may be told it claimed the key";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  void *stored = HashLookup(hash, (void*)"raced", true);
  ASSERT_NE(stored, nullptr);

  bool known = false;
  for (auto &item : items) known = known || (stored == &item);
  EXPECT_TRUE(known) << "the resident is not one of the offered containers";

  HashTableDestruct(hash, false);
}

TEST(HashTableUniqueInsert, ConcurrentReOfferOfOneContainerReportsOneInsert)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "unique_same_container", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem     shared{"shared", 1, 0};
  constexpr int kThreads = 8;

  std::atomic<int> inserts{0};
  std::atomic<int> mismatches{0};

  {
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&hash, &shared, &inserts, &mismatches]() {
        bool  was_inserted = false;
        void *resident     = nullptr;
        void *result       = AddToHashUniquely(hash, &shared, &was_inserted, &resident);

        if (result != &shared || resident != &shared) mismatches.fetch_add(1);
        else if (was_inserted) inserts.fetch_add(1);
      });
    }
    for (auto &thread : threads) thread.join();
  }

  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_EQ(inserts.load(), 1) <<
    "re-offering an already resident container adds nothing, however many times it is offered";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

// ---------------------------------------------------------------------------
// AddToHashReplacing: the incumbent gives way to the caller's instance
// ---------------------------------------------------------------------------

TEST(HashTableReplace, IncumbentGivesWayToTheOfferedContainer)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_incumbent", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem incumbent = {"contested", 1, 0};
  KeyedItem claimant  = {"contested", 2, 0};

  ASSERT_EQ(AddToHashUniquely(hash, &incumbent, nullptr, nullptr), &incumbent);

  void *replaced = (void*)0x1;
  EXPECT_EQ(AddToHashReplacing(hash, &claimant, &replaced), &claimant);
  EXPECT_EQ(replaced, &incumbent) << "the displaced instance must be named back to the caller";
  EXPECT_EQ(HashLookup(hash, (void*)"contested", true), &claimant);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1) << "replacement must not leave both instances resident";

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, DisplacedInstanceLeavesNoTrace)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_absent", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem incumbent = {"gone", 1, 0};
  KeyedItem claimant  = {"gone", 2, 0};

  ASSERT_EQ(AddToHashUniquely(hash, &incumbent, nullptr, nullptr), &incumbent);
  ASSERT_EQ(AddToHashReplacing(hash, &claimant, nullptr), &claimant);

  void *entries[8] = {nullptr};
  int   count      = GetHashEntries(hash, entries, 8);
  ASSERT_LE(count, 8);

  for (int i = 0; i < count; ++i) {
    EXPECT_NE(entries[i], &incumbent) << "the displaced instance is still enumerated";
  }

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, FreeKeyIsAPlainInsert)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_free", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem claimant = {"unclaimed", 1, 0};

  void *replaced = (void*)0x1;
  EXPECT_EQ(AddToHashReplacing(hash, &claimant, &replaced), &claimant);
  EXPECT_EQ(replaced, nullptr) << "nothing was displaced";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, ReplacingTheResidentIsNotAReplacement)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_self", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem item = {"self", 1, 0};
  ASSERT_EQ(AddToHashUniquely(hash, &item, nullptr, nullptr), &item);

  void *replaced = (void*)0x1;
  EXPECT_EQ(AddToHashReplacing(hash, &item, &replaced), &item);
  EXPECT_EQ(replaced, nullptr) << "a container cannot displace itself";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, ReplacingLeavesEveryOtherKeyIntact)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_others", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem bystander_a = {"bystander-a", 1, 0};
  KeyedItem bystander_b = {"bystander-b", 2, 0};
  KeyedItem incumbent   = {"target", 3, 0};
  KeyedItem claimant    = {"target", 4, 0};

  ASSERT_EQ(AddToHashUniquely(hash, &bystander_a, nullptr, nullptr), &bystander_a);
  ASSERT_EQ(AddToHashUniquely(hash, &bystander_b, nullptr, nullptr), &bystander_b);
  ASSERT_EQ(AddToHashUniquely(hash, &incumbent, nullptr, nullptr), &incumbent);
  ASSERT_EQ(HASHTABLE_ENTRIES(hash), 3);

  ASSERT_EQ(AddToHashReplacing(hash, &claimant, nullptr), &claimant);

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 3);
  EXPECT_EQ(HashLookup(hash, (void*)"bystander-a", true), &bystander_a);
  EXPECT_EQ(HashLookup(hash, (void*)"bystander-b", true), &bystander_b);
  EXPECT_EQ(HashLookup(hash, (void*)"target", true), &claimant);

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, NonUniqueTableRefusesWithoutReplacing)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_plain", false);
  ASSERT_NE(hash, nullptr);

  KeyedItem a = {"plain", 1, 0};
  KeyedItem b = {"plain", 2, 0};
  ASSERT_EQ(AddToHash(hash, &a), &a);

  void *replaced = (void*)0x1;
  EXPECT_EQ(AddToHashReplacing(hash, &b, &replaced), nullptr);
  EXPECT_EQ(replaced, nullptr);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);
  EXPECT_EQ(HashLookup(hash, (void*)"plain", true), &a) << "the incumbent was displaced anyway";

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, NullArgumentsAreRejected)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_null", true);
  ASSERT_NE(hash, nullptr);

  KeyedItem item     = {"null", 1, 0};
  void *    replaced = (void*)0x1;

  EXPECT_EQ(AddToHashReplacing(nullptr, &item, &replaced), nullptr);
  EXPECT_EQ(replaced, nullptr);

  replaced = (void*)0x1;
  EXPECT_EQ(AddToHashReplacing(hash, nullptr, &replaced), nullptr);
  EXPECT_EQ(replaced, nullptr);
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 0);

  EXPECT_EQ(AddToHashReplacing(hash, &item, nullptr), &item) << "a NULL out-parameter must be tolerated";
  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1);

  HashTableDestruct(hash, false);
}

TEST(HashTableReplace, ConcurrentReplacementsNeverLeaveTwoResidents)
{
  HashTable  storage{};
  HashTable *hash = MakeKeyedTable(&storage, "replace_race", true);
  ASSERT_NE(hash, nullptr);

  constexpr int          kThreads = 8;
  std::vector<KeyedItem> items(kThreads);
  for (int i = 0; i < kThreads; ++i) items[i] = KeyedItem{"throne", i, 0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&hash, &items, i]() {
      for (int round = 0; round < 100; ++round) {
        AddToHashReplacing(hash, &items[i], nullptr);
      }
    });
  }
  for (auto &thread : threads) thread.join();

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), 1) << "concurrent replacements accumulated entries";

  void *resident = HashLookup(hash, (void*)"throne", true);
  ASSERT_NE(resident, nullptr);

  bool known = false;
  for (auto &item : items) known = known || (resident == &item);
  EXPECT_TRUE(known) << "the resident is not one of the offered containers";

  HashTableDestruct(hash, false);
}

namespace
{
  struct KeyedHolder
  {
    unsigned long rid; // bookkeeping word, as a real container carries ahead of the item
    KeyedItem *   item;
  };

  void *
  ExtractFromHolder(ItemContainer *container)
  {
    return ((KeyedHolder*)container)->item;
  }
} // namespace

// Unique-key, pointer-keyed table whose containers are not the items: the comparator must
// resolve the stored container through the extractor before reading its key field.
TEST(HashTableContract, UniqueKeyComparisonExtractsStoredContainer)
{
  HashTable  storage{};
  HashTable *hash = HashTableLockingInstantiate(&storage, (int)offsetof(KeyedItem, key), KEY_SIZE_ZERO,
                                                HASH_ITEM_IS_PTR_TYPE, "unique_stored_container_extraction",
                                                ExtractFromHolder);
  ASSERT_NE(hash, nullptr);
  ASSERT_TRUE(HashTableSetUniqueKeys(hash, true));

  constexpr int            kItems = 4096;
  std::vector<std::string> keys(kItems);
  std::vector<KeyedItem>   items(kItems);
  std::vector<KeyedHolder> holders(kItems);

  for (int i = 0; i < kItems; i++) {
    keys[i]    = "fence-" + std::to_string(i);
    items[i]   = KeyedItem{keys[i].c_str(), i, 0};
    holders[i] = KeyedHolder{(unsigned long)i + 1, &items[i]};
  }

  for (int i = 0; i < kItems; i++) {
    bool  was_inserted = false;
    void *resident     = nullptr;
    ASSERT_EQ(AddToHashUniquely(hash, &holders[i], &was_inserted, &resident), &holders[i]) << i;
    ASSERT_TRUE(was_inserted) << i;
  }

  EXPECT_EQ(HASHTABLE_ENTRIES(hash), (size_t)kItems);

  for (int i = 0; i < kItems; i++) {
    EXPECT_EQ(HashLookup(hash, (void*)keys[i].c_str(), true), &holders[i]) << i;
  }

  HashTableDestruct(hash, false);
}
