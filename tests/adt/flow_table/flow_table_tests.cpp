/**
 * @file flow_table_tests.cpp
 * @brief Unit tests for FlowTable: both key families, growth, deletion, batch lookup.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <vector>

extern "C" {
#include "uflib/adt/flow_table/flow_table.h"
}

namespace {

FlowTableKeyV4 key_v4(uint32_t index)
{
  return FlowTableKeyV4{
      .source_ip = 0x0A000000u + index * 2654435761u,
      .destination_ip = 0xC0A80000u ^ (index >> 3),
      .source_port = static_cast<uint16_t>(1024 + index),
      .destination_port = static_cast<uint16_t>(index % 7 ? 443 : 80),
  };
}

/** Distinct addresses that share a long prefix: only the full 128 bits tell them apart. */
void address_v6(uint8_t out[16], uint32_t index)
{
  static const uint8_t prefix[12] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  std::memcpy(out, prefix, sizeof(prefix));
  out[12] = static_cast<uint8_t>(index >> 24);
  out[13] = static_cast<uint8_t>(index >> 16);
  out[14] = static_cast<uint8_t>(index >> 8);
  out[15] = static_cast<uint8_t>(index);
}

FlowTableKeyV6 key_v6(uint32_t index)
{
  FlowTableKeyV6 key{};
  address_v6(key.source, index);
  address_v6(key.destination, index * 2654435761u);
  key.source_port = static_cast<uint16_t>(1024 + index);
  key.destination_port = static_cast<uint16_t>(index % 7 ? 443 : 80);
  return key;
}


/** Address bytes an attacker would pick to flatten a seeded hash. */
FlowTableKeyV6 adversarial_key(uint32_t index)
{
  static const uint64_t fixed_first = 0x9E3779B97F4A7C15ull;
  static const uint64_t fixed_second = 0xD6E8FEB86659FD93ull;
  FlowTableKeyV6 key{};
  for (int byte = 0; byte < 8; byte++) {
    key.source[byte] = static_cast<uint8_t>(fixed_first >> (byte * 8));
    key.source[8 + byte] = static_cast<uint8_t>(fixed_second >> (byte * 8));
    key.destination[byte] = static_cast<uint8_t>(fixed_second >> (byte * 8));
    key.destination[8 + byte] = static_cast<uint8_t>(fixed_first >> (byte * 8));
  }
  key.source[12] ^= static_cast<uint8_t>(index >> 24);
  key.source[13] ^= static_cast<uint8_t>(index >> 16);
  key.source[14] ^= static_cast<uint8_t>(index >> 8);
  key.source[15] ^= static_cast<uint8_t>(index);
  key.destination[15] ^= static_cast<uint8_t>(index * 31u);
  key.source_port = static_cast<uint16_t>(1024 + index);
  key.destination_port = 9000;
  return key;
}

class FlowTableV4Test : public ::testing::Test {
protected:
  void SetUp() override { table = FlowTableCreate(FLOW_TABLE_KIND_V4, 0, 0xdeadbeefcafef00dull); }
  void TearDown() override { FlowTableDestroy(table); }

  FlowTable *table = nullptr;
};

class FlowTableV6Test : public ::testing::Test {
protected:
  void SetUp() override { table = FlowTableCreate(FLOW_TABLE_KIND_V6, 0, 0xdeadbeefcafef00dull); }
  void TearDown() override { FlowTableDestroy(table); }

  FlowTable *table = nullptr;
};

TEST(FlowTableCreateTest, UnknownKindIsRejected)
{
  EXPECT_EQ(FlowTableCreate(static_cast<FlowTableKind>(7), 16, 1), nullptr);
  EXPECT_EQ(FlowTableKindOf(nullptr), -1);
}

TEST(FlowTableCreateTest, OverflowingCapacityIsRejected)
{
  /* capacity * slot_size would wrap for both families; Create must refuse
   * rather than allocate a truncated block that later inserts run past. */
  EXPECT_EQ(FlowTableCreate(FLOW_TABLE_KIND_V4, SIZE_MAX / 8, 1), nullptr);
  EXPECT_EQ(FlowTableCreate(FLOW_TABLE_KIND_V6, SIZE_MAX / 8, 1), nullptr);
  EXPECT_EQ(FlowTableCreate(FLOW_TABLE_KIND_V4, SIZE_MAX, 1), nullptr);
  EXPECT_EQ(FlowTableCreate(FLOW_TABLE_KIND_V6, SIZE_MAX, 1), nullptr);
}

TEST(FlowTableCreateTest, KindIsReported)
{
  FlowTable *v4 = FlowTableCreate(FLOW_TABLE_KIND_V4, 0, 1);
  FlowTable *v6 = FlowTableCreate(FLOW_TABLE_KIND_V6, 0, 1);
  ASSERT_NE(v4, nullptr);
  ASSERT_NE(v6, nullptr);
  EXPECT_EQ(FlowTableKindOf(v4), FLOW_TABLE_KIND_V4);
  EXPECT_EQ(FlowTableKindOf(v6), FLOW_TABLE_KIND_V6);
  FlowTableDestroy(v4);
  FlowTableDestroy(v6);
}

TEST_F(FlowTableV4Test, StartsEmptyWithAMinimumCapacity)
{
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(FlowTableSize(table), 0u);
  EXPECT_GE(FlowTableCapacity(table), 16u);
  EXPECT_EQ(FlowTableCapacity(nullptr), 0u);
  EXPECT_EQ(FlowTableSize(nullptr), 0u);
}

TEST_F(FlowTableV4Test, ReservedZeroPortKeyIsRejected)
{
  const FlowTableKeyV4 zero{};
  EXPECT_EQ(FlowTableInsertV4(table, &zero, 1), FLOW_TABLE_ERROR);
  uint32_t value = 0;
  EXPECT_FALSE(FlowTableFindV4(table, &zero, &value));
  EXPECT_FALSE(FlowTableEraseV4(table, &zero));
  EXPECT_EQ(FlowTableSize(table), 0u);
}

TEST_F(FlowTableV4Test, InsertAndUpdateAreDistinguished)
{
  const FlowTableKeyV4 key = key_v4(1);
  EXPECT_EQ(FlowTableInsertV4(table, &key, 7), FLOW_TABLE_INSERTED);
  EXPECT_EQ(FlowTableInsertV4(table, &key, 9), FLOW_TABLE_UPDATED);
  EXPECT_EQ(FlowTableSize(table), 1u);

  uint32_t value = 0;
  ASSERT_TRUE(FlowTableFindV4(table, &key, &value));
  EXPECT_EQ(value, 9u);
}

TEST_F(FlowTableV4Test, LookupIsKeyOrderAndValueExact)
{
  const FlowTableKeyV4 key = key_v4(2);
  ASSERT_EQ(FlowTableInsertV4(table, &key, 42), FLOW_TABLE_INSERTED);

  FlowTableKeyV4 swapped = key;
  std::swap(swapped.source_ip, swapped.destination_ip);
  std::swap(swapped.source_port, swapped.destination_port);
  uint32_t value = 0xFFFFFFFFu;
  EXPECT_FALSE(FlowTableFindV4(table, &swapped, &value));
  EXPECT_EQ(value, 0xFFFFFFFFu) << "a miss must not touch the output";

  EXPECT_TRUE(FlowTableFindV4(table, &key, nullptr)) << "value pointer is optional";
}

TEST_F(FlowTableV4Test, EraseKeepsNeighbouringProbesResolvable)
{
  enum { kEntries = 2000u };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_EQ(FlowTableInsertV4(table, &key, index), FLOW_TABLE_INSERTED);
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries));

  for (uint32_t index = 0; index < kEntries; index += 2) {
    const FlowTableKeyV4 key = key_v4(index);
    EXPECT_TRUE(FlowTableEraseV4(table, &key)) << index;
    EXPECT_FALSE(FlowTableEraseV4(table, &key)) << "second erase must miss";
  }

  uint32_t value = 0;
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    const bool found = FlowTableFindV4(table, &key, &value);
    EXPECT_EQ(found, (index & 1u) != 0u) << index;
    if (found) EXPECT_EQ(value, index);
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries) / 2u);

  for (uint32_t index = 0; index < kEntries; index += 2) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_EQ(FlowTableInsertV4(table, &key, index), FLOW_TABLE_INSERTED);
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries));
}

TEST_F(FlowTableV4Test, GrowsPastLoadAndKeepsEveryEntry)
{
  enum { kEntries = 20000u };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_EQ(FlowTableInsertV4(table, &key, index), FLOW_TABLE_INSERTED);
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries));
  EXPECT_GT(FlowTableCapacity(table), static_cast<size_t>(kEntries));

  uint32_t value = 0;
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_TRUE(FlowTableFindV4(table, &key, &value)) << index;
    EXPECT_EQ(value, index);
  }
}

TEST_F(FlowTableV4Test, UpdateAtTheGrowthThresholdDoesNotGrow)
{
  const size_t capacity = FlowTableCapacity(table);
  const size_t grow_at = capacity - capacity / 8;

  for (uint32_t index = 0; index < grow_at; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_EQ(FlowTableInsertV4(table, &key, index), FLOW_TABLE_INSERTED) << index;
  }
  ASSERT_EQ(FlowTableSize(table), grow_at);
  ASSERT_EQ(FlowTableCapacity(table), capacity);

  const FlowTableKeyV4 existing = key_v4(0);
  EXPECT_EQ(FlowTableInsertV4(table, &existing, 0xFEEDu), FLOW_TABLE_UPDATED);
  EXPECT_EQ(FlowTableCapacity(table), capacity) << "a pure update must not grow";
  EXPECT_EQ(FlowTableSize(table), grow_at);

  const FlowTableKeyV4 fresh = key_v4(static_cast<uint32_t>(grow_at));
  EXPECT_EQ(FlowTableInsertV4(table, &fresh, 7), FLOW_TABLE_INSERTED);
  EXPECT_GT(FlowTableCapacity(table), capacity) << "a miss at the threshold must grow";
}

TEST_F(FlowTableV4Test, BatchMatchesSingleLookups)
{
  enum { kEntries = 512u, kQueries = 4096u };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_EQ(FlowTableInsertV4(table, &key, index * 3u), FLOW_TABLE_INSERTED);
  }

  std::vector<FlowTableKeyV4> keys(kQueries);
  std::vector<uint32_t>       values(kQueries, 0xDEADBEEFu);
  std::unique_ptr<bool[]>     found(new bool[kQueries]()); /* vector<bool> is bit-packed */
  for (uint32_t query = 0; query < kQueries; query++) keys[query] = key_v4(query % (kEntries + 64u));

  const size_t hits = FlowTableFindBatchV4(table, keys.data(), kQueries, values.data(), found.get());

  size_t expected = 0;
  for (uint32_t query = 0; query < kQueries; query++) {
    uint32_t single = 0;
    const bool hit = FlowTableFindV4(table, &keys[query], &single);
    ASSERT_EQ(found[query], hit) << query;
    if (hit) {
      EXPECT_EQ(values[query], single) << query;
      expected++;
    } else {
      EXPECT_EQ(values[query], 0xDEADBEEFu) << "values are untouched on a miss";
    }
  }
  EXPECT_EQ(hits, expected);
}

TEST_F(FlowTableV4Test, ClearKeepsCapacityAndDropsEntries)
{
  for (uint32_t index = 0; index < 100; index++) {
    const FlowTableKeyV4 key = key_v4(index);
    ASSERT_EQ(FlowTableInsertV4(table, &key, index), FLOW_TABLE_INSERTED);
  }
  const size_t capacity = FlowTableCapacity(table); /* grown by the inserts */
  FlowTableClear(table);
  EXPECT_EQ(FlowTableSize(table), 0u);
  EXPECT_EQ(FlowTableCapacity(table), capacity);

  uint32_t value = 0;
  const FlowTableKeyV4 key = key_v4(0);
  EXPECT_FALSE(FlowTableFindV4(table, &key, &value));
}

TEST_F(FlowTableV6Test, StartsEmptyWithAMinimumCapacity)
{
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(FlowTableSize(table), 0u);
  EXPECT_GE(FlowTableCapacity(table), 16u);
}

TEST_F(FlowTableV6Test, ReservedZeroPortKeyIsRejected)
{
  const FlowTableKeyV6 zero{};
  EXPECT_EQ(FlowTableInsertV6(table, &zero, 1), FLOW_TABLE_ERROR);
  uint32_t value = 0;
  EXPECT_FALSE(FlowTableFindV6(table, &zero, &value));
  EXPECT_FALSE(FlowTableEraseV6(table, &zero));
  EXPECT_EQ(FlowTableSize(table), 0u);
}

TEST_F(FlowTableV6Test, FullAddressIsComparedNotAPrefix)
{
  FlowTableKeyV6 first = key_v6(1);
  FlowTableKeyV6 second = first;
  second.source[15] ^= 0x01; /* one bit in the last source byte */

  ASSERT_EQ(FlowTableInsertV6(table, &first, 11), FLOW_TABLE_INSERTED);
  ASSERT_EQ(FlowTableInsertV6(table, &second, 22), FLOW_TABLE_INSERTED);
  EXPECT_EQ(FlowTableSize(table), 2u);

  uint32_t value = 0;
  ASSERT_TRUE(FlowTableFindV6(table, &first, &value));
  EXPECT_EQ(value, 11u);
  ASSERT_TRUE(FlowTableFindV6(table, &second, &value));
  EXPECT_EQ(value, 22u);

  second.destination[0] ^= 0x80;
  EXPECT_FALSE(FlowTableFindV6(table, &second, &value));
}

TEST_F(FlowTableV6Test, V4MappedAddressesDoNotAlias)
{
  FlowTableKeyV6 first{};
  FlowTableKeyV6 second{};
  const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  std::memcpy(first.source, mapped_prefix, sizeof(mapped_prefix));
  std::memcpy(second.source, mapped_prefix, sizeof(mapped_prefix));
  first.source[12] = 192; first.source[13] = 0; first.source[14] = 2; first.source[15] = 1;
  second.source[12] = 192; second.source[13] = 0; second.source[14] = 2; second.source[15] = 2;
  std::memcpy(first.destination, first.source, 16);
  std::memcpy(second.destination, second.source, 16);
  first.source_port = 5000; second.source_port = 5000;
  first.destination_port = 9000; second.destination_port = 9000;

  ASSERT_EQ(FlowTableInsertV6(table, &first, 1), FLOW_TABLE_INSERTED);
  ASSERT_EQ(FlowTableInsertV6(table, &second, 2), FLOW_TABLE_INSERTED);
  uint32_t value = 0;
  ASSERT_TRUE(FlowTableFindV6(table, &first, &value));
  EXPECT_EQ(value, 1u);
  ASSERT_TRUE(FlowTableFindV6(table, &second, &value));
  EXPECT_EQ(value, 2u);
}

TEST_F(FlowTableV6Test, EraseKeepsNeighbouringProbesResolvable)
{
  enum { kEntries = 2000u };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    ASSERT_EQ(FlowTableInsertV6(table, &key, index), FLOW_TABLE_INSERTED);
  }

  for (uint32_t index = 0; index < kEntries; index += 2) {
    const FlowTableKeyV6 key = key_v6(index);
    EXPECT_TRUE(FlowTableEraseV6(table, &key)) << index;
    EXPECT_FALSE(FlowTableEraseV6(table, &key)) << "second erase must miss";
  }

  uint32_t value = 0;
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    const bool found = FlowTableFindV6(table, &key, &value);
    EXPECT_EQ(found, (index & 1u) != 0u) << index;
    if (found) EXPECT_EQ(value, index);
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries) / 2u);
}

TEST_F(FlowTableV6Test, GrowsPastLoadAndKeepsEveryEntry)
{
  enum { kEntries = 20000u };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    ASSERT_EQ(FlowTableInsertV6(table, &key, index), FLOW_TABLE_INSERTED);
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries));
  EXPECT_GT(FlowTableCapacity(table), static_cast<size_t>(kEntries));

  uint32_t value = 0;
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    ASSERT_TRUE(FlowTableFindV6(table, &key, &value)) << index;
    EXPECT_EQ(value, index);
  }
}

TEST_F(FlowTableV6Test, BatchMatchesSingleLookups)
{
  enum { kEntries = 512u, kQueries = 4096u };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    ASSERT_EQ(FlowTableInsertV6(table, &key, index * 5u), FLOW_TABLE_INSERTED);
  }

  std::vector<FlowTableKeyV6> keys(kQueries);
  std::vector<uint32_t>       values(kQueries, 0xDEADBEEFu);
  std::unique_ptr<bool[]>     found(new bool[kQueries]());
  for (uint32_t query = 0; query < kQueries; query++) keys[query] = key_v6(query % (kEntries + 64u));

  const size_t hits = FlowTableFindBatchV6(table, keys.data(), kQueries, values.data(), found.get());

  size_t expected = 0;
  for (uint32_t query = 0; query < kQueries; query++) {
    uint32_t single = 0;
    const bool hit = FlowTableFindV6(table, &keys[query], &single);
    ASSERT_EQ(found[query], hit) << query;
    if (hit) {
      EXPECT_EQ(values[query], single) << query;
      expected++;
    } else {
      EXPECT_EQ(values[query], 0xDEADBEEFu);
    }
  }
  EXPECT_EQ(hits, expected);
}

TEST(FlowTableFamilyTest, WrongFamilyIsRejectedEverywhere)
{
  FlowTable *v6 = FlowTableCreate(FLOW_TABLE_KIND_V6, 0, 1);
  ASSERT_NE(v6, nullptr);
  const FlowTableKeyV4 v4key = key_v4(1);
  const FlowTableKeyV6 v6key = key_v6(1);
  uint32_t value = 0;
  bool found = true;

  EXPECT_EQ(FlowTableInsertV4(v6, &v4key, 1), FLOW_TABLE_ERROR);
  EXPECT_FALSE(FlowTableFindV4(v6, &v4key, &value));
  EXPECT_FALSE(FlowTableEraseV4(v6, &v4key));
  EXPECT_EQ(FlowTableFindBatchV4(v6, &v4key, 1, &value, &found), 0u);
  EXPECT_EQ(FlowTableSize(v6), 0u);

  FlowTable *v4 = FlowTableCreate(FLOW_TABLE_KIND_V4, 0, 1);
  ASSERT_NE(v4, nullptr);
  EXPECT_EQ(FlowTableInsertV6(v4, &v6key, 1), FLOW_TABLE_ERROR);
  EXPECT_FALSE(FlowTableFindV6(v4, &v6key, &value));
  EXPECT_FALSE(FlowTableEraseV6(v4, &v6key));
  EXPECT_EQ(FlowTableFindBatchV6(v4, &v6key, 1, &value, &found), 0u);
  EXPECT_EQ(FlowTableSize(v4), 0u);

  FlowTableDestroy(v4);
  FlowTableDestroy(v6);
}

TEST(FlowTableInvalidArgumentsTest, NullArgumentsAreRejected)
{
  FlowTable *table = FlowTableCreate(FLOW_TABLE_KIND_V4, 0, 1);
  ASSERT_NE(table, nullptr);
  const FlowTableKeyV4 key = key_v4(3);
  uint32_t value = 0;
  bool found = true;

  EXPECT_EQ(FlowTableInsertV4(nullptr, &key, 1), FLOW_TABLE_ERROR);
  EXPECT_EQ(FlowTableInsertV4(table, nullptr, 1), FLOW_TABLE_ERROR);
  EXPECT_FALSE(FlowTableFindV4(nullptr, &key, nullptr));
  EXPECT_FALSE(FlowTableFindV4(table, nullptr, nullptr));
  EXPECT_FALSE(FlowTableEraseV4(nullptr, &key));
  EXPECT_FALSE(FlowTableEraseV4(table, nullptr));
  EXPECT_EQ(FlowTableFindBatchV4(nullptr, &key, 1, &value, &found), 0u);
  EXPECT_EQ(FlowTableFindBatchV4(table, nullptr, 1, &value, &found), 0u);
  EXPECT_EQ(FlowTableFindBatchV4(table, &key, 1, nullptr, &found), 0u);
  EXPECT_EQ(FlowTableFindBatchV4(table, &key, 1, &value, nullptr), 0u);

  FlowTableDestroy(table);
  FlowTableDestroy(nullptr);
  FlowTableClear(nullptr);
}


TEST_F(FlowTableV4Test, ReservedZeroPortKeyIsRejectedByEveryEntryPoint)
{
  FlowTableKeyV4 zero_ports{};
  zero_ports.source_ip = 0x01020304;
  zero_ports.destination_ip = 0x05060708;
  zero_ports.source_port = 0;
  zero_ports.destination_port = 0;

  EXPECT_EQ(FlowTableInsertV4(table, &zero_ports, 1), FLOW_TABLE_ERROR);

  uint32_t value = 0xFFFFFFFFu;
  bool     found = true;
  EXPECT_FALSE(FlowTableFindV4(table, &zero_ports, &value));
  EXPECT_EQ(value, 0xFFFFFFFFu);
  EXPECT_FALSE(FlowTableEraseV4(table, &zero_ports));
  EXPECT_EQ(FlowTableFindBatchV4(table, &zero_ports, 1, &value, &found), 0u);
  EXPECT_FALSE(found);
  EXPECT_EQ(FlowTableSize(table), 0u) << "a rejected erase must not underflow the count";
}

TEST_F(FlowTableV6Test, ReservedZeroPortKeyIsRejectedByEveryEntryPoint)
{
  FlowTableKeyV6 zero_ports{};
  zero_ports.source[0] = 0x20;
  zero_ports.destination[15] = 0x01;

  EXPECT_EQ(FlowTableInsertV6(table, &zero_ports, 1), FLOW_TABLE_ERROR);
  uint32_t value = 0xFFFFFFFFu;
  bool     found = true;
  EXPECT_FALSE(FlowTableFindV6(table, &zero_ports, &value));
  EXPECT_FALSE(FlowTableEraseV6(table, &zero_ports));
  EXPECT_EQ(FlowTableFindBatchV6(table, &zero_ports, 1, &value, &found), 0u);
  EXPECT_FALSE(found);
  EXPECT_EQ(FlowTableSize(table), 0u);
}

namespace {

/** Deterministic model test: every operation is checked against std::map. */
template <typename Key, typename KeyFor, typename Insert, typename Find, typename Erase, typename Batch>
void run_model_test(FlowTable *table, KeyFor key_for, Insert insert, Find find, Erase erase, Batch batch)
{
  enum { kKeys = 4096, kOps = 60000 };
  std::map<uint32_t, uint32_t> model;
  std::mt19937                 rng(0xC0FFEEu);

  for (int op = 0; op < kOps; op++) {
    const uint32_t id = rng() % kKeys;
    const Key      key = key_for(id);
    const uint32_t value = rng();
    switch (rng() % 3) {
      case 0: {
        const auto [slot, inserted] = model.insert_or_assign(id, value);
        (void)slot;
        const bool was_present = !inserted;
        EXPECT_EQ(insert(table, &key, value), was_present ? FLOW_TABLE_UPDATED : FLOW_TABLE_INSERTED);
        break;
      }
      case 1: {
        const bool present = model.erase(id) > 0;
        EXPECT_EQ(erase(table, &key), present);
        break;
      }
      default: {
        uint32_t got = 0;
        const auto it = model.find(id);
        const bool hit = find(table, &key, &got);
        ASSERT_EQ(hit, it != model.end());
        if (hit) EXPECT_EQ(got, it->second);
        break;
      }
    }
    ASSERT_EQ(FlowTableSize(table), model.size()) << "op " << op;

    if ((op % 500) == 0) { /* periodic full sweep catches placement corruption */
      for (uint32_t sweep = 0; sweep < static_cast<uint32_t>(kKeys); sweep++) {
        const Key sweep_key = key_for(sweep);
        uint32_t  got = 0;
        const auto it = model.find(sweep);
        ASSERT_EQ(find(table, &sweep_key, &got), it != model.end()) << "sweep " << sweep;
        if (it != model.end()) ASSERT_EQ(got, it->second) << "sweep " << sweep;
      }
      /* batch must agree with the model on every key */
      std::vector<Key> keys(kKeys);
      std::vector<uint32_t> values(kKeys, 0xDEADBEEFu);
      std::unique_ptr<bool[]> found(new bool[kKeys]());
      for (uint32_t sweep = 0; sweep < static_cast<uint32_t>(kKeys); sweep++) keys[sweep] = key_for(sweep);
      const size_t hits = batch(table, keys.data(), kKeys, values.data(), found.get());
      EXPECT_EQ(hits, model.size());
      for (uint32_t sweep = 0; sweep < static_cast<uint32_t>(kKeys); sweep++) {
        const auto it = model.find(sweep);
        ASSERT_EQ(found[sweep], it != model.end()) << "batch " << sweep;
        if (it != model.end()) ASSERT_EQ(values[sweep], it->second) << "batch " << sweep;
      }
    }
  }
}

}  // namespace

TEST_F(FlowTableV4Test, DifferentialModelHoldsUnderRandomChurn)
{
  const auto key_for = [](uint32_t id) { return key_v4(id); };
  const auto insert = [](FlowTable *t, const FlowTableKeyV4 *k, uint32_t v) { return FlowTableInsertV4(t, k, v); };
  const auto find = [](const FlowTable *t, const FlowTableKeyV4 *k, uint32_t *v) { return FlowTableFindV4(t, k, v); };
  const auto erase = [](FlowTable *t, const FlowTableKeyV4 *k) { return FlowTableEraseV4(t, k); };
  const auto batch = [](const FlowTable *t, const FlowTableKeyV4 *k, size_t n, uint32_t *v, bool *f) {
    return FlowTableFindBatchV4(t, k, n, v, f);
  };
  run_model_test<FlowTableKeyV4>(table, key_for, insert, find, erase, batch);
}

TEST_F(FlowTableV6Test, DifferentialModelHoldsUnderRandomChurn)
{
  const auto key_for = [](uint32_t id) { return key_v6(id); };
  const auto insert = [](FlowTable *t, const FlowTableKeyV6 *k, uint32_t v) { return FlowTableInsertV6(t, k, v); };
  const auto find = [](const FlowTable *t, const FlowTableKeyV6 *k, uint32_t *v) { return FlowTableFindV6(t, k, v); };
  const auto erase = [](FlowTable *t, const FlowTableKeyV6 *k) { return FlowTableEraseV6(t, k); };
  const auto batch = [](const FlowTable *t, const FlowTableKeyV6 *k, size_t n, uint32_t *v, bool *f) {
    return FlowTableFindBatchV6(t, k, n, v, f);
  };
  run_model_test<FlowTableKeyV6>(table, key_for, insert, find, erase, batch);
}

TEST_F(FlowTableV6Test, AttackPatternAddressesRemainDistinct)
{
  enum { kEntries = 8192 };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = adversarial_key(index);
    ASSERT_EQ(FlowTableInsertV6(table, &key, index), FLOW_TABLE_INSERTED) << index;
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries));

  uint32_t value = 0;
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = adversarial_key(index);
    ASSERT_TRUE(FlowTableFindV6(table, &key, &value)) << index;
    EXPECT_EQ(value, index);
  }

  for (uint32_t index = 0; index < kEntries; index += 2) {
    const FlowTableKeyV6 key = adversarial_key(index);
    ASSERT_TRUE(FlowTableEraseV6(table, &key)) << index;
  }
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = adversarial_key(index);
    EXPECT_EQ(FlowTableFindV6(table, &key, &value), (index & 1u) != 0u) << index;
  }
  EXPECT_EQ(FlowTableSize(table), static_cast<size_t>(kEntries) / 2u);
}

TEST_F(FlowTableV6Test, SizeReturnsToZeroAfterErasingEverything)
{
  enum { kEntries = 3000 };
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    ASSERT_EQ(FlowTableInsertV6(table, &key, index), FLOW_TABLE_INSERTED);
  }
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = key_v6(index);
    ASSERT_TRUE(FlowTableEraseV6(table, &key)) << index;
  }
  EXPECT_EQ(FlowTableSize(table), 0u);
  uint32_t value = 0;
  const FlowTableKeyV6 gone = key_v6(0);
  EXPECT_FALSE(FlowTableFindV6(table, &gone, &value));
}

}  // namespace
