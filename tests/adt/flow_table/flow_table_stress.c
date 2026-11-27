/**
 * @file flow_table_stress.c
 * @brief Multi-million-operation churn run for FlowTable. Not a CTest target.
 *
 * Every check is explicit rather than assert(), so a Release build (which
 * defines NDEBUG) still exercises the table instead of compiling the calls
 * away. Run it under ASan/UBSan and under Valgrind for the corruption classes.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <uflib/adt/flow_table/flow_table.h>

#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      fprintf(stderr, "flow_table_stress: check failed at line %d: %s\n", __LINE__, #condition); \
      abort();                                                                                \
    }                                                                                         \
  } while (0)

static uint64_t sRandom = 88172645463325252ull;

static uint32_t
sNextRandom(void)
{
  sRandom ^= sRandom << 13;
  sRandom ^= sRandom >> 7;
  sRandom ^= sRandom << 17;
  return (uint32_t)(sRandom >> 16);
}

static FlowTableKeyV4
sKeyV4(uint32_t index)
{
  /* bijective in index, so every key is distinct */
  const FlowTableKeyV4 key = {
      .source_ip = 0x0A000000u + index * 2654435761u,
      .destination_ip = 0xC0A80000u ^ (index >> 3),
      .source_port = (uint16_t)(1024 + index),
      .destination_port = (uint16_t)(index % 7 ? 443 : 80),
  };
  return key;
}

/** Adversarial address bytes: the patterns an attacker would use to flatten the hash. */
static void
sAddressV6(uint8_t out[16], uint32_t index, uint32_t salt)
{
  uint64_t first = 0x9E3779B97F4A7C15ull;
  uint64_t second = 0xD6E8FEB86659FD93ull;
  if ((index + salt) & 1u) second ^= 0xffffffffull;
  for (int byte = 0; byte < 8; byte++) {
    out[byte] = (uint8_t)(first >> (byte * 8));
    out[8 + byte] = (uint8_t)(second >> (byte * 8));
  }
  out[12] ^= (uint8_t)(index >> 24);
  out[13] ^= (uint8_t)(index >> 16);
  out[14] ^= (uint8_t)(index >> 8);
  out[15] ^= (uint8_t)(index);
}

static FlowTableKeyV6
sKeyV6(uint32_t index)
{
  FlowTableKeyV6 key = {0};
  sAddressV6(key.source, index, 1);
  sAddressV6(key.destination, index * 2654435761u, 2);
  key.source_port = (uint16_t)(1024 + index);
  key.destination_port = (uint16_t)(index % 7 ? 443 : 80);
  return key;
}

static void
sRunV4(void)
{
  enum { kEntries = 1000000 };
  FlowTable *table = FlowTableCreate(FLOW_TABLE_KIND_V4, 0, 0xdeadbeefcafef00dull);
  CHECK(table != NULL);

  const FlowTableKeyV4 zero = {0};
  CHECK(FlowTableInsertV4(table, &zero, 1) == FLOW_TABLE_ERROR);
  CHECK(!FlowTableFindV4(table, &zero, NULL));
  CHECK(!FlowTableEraseV4(table, &zero));

  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = sKeyV4(index);
    CHECK(FlowTableInsertV4(table, &key, index) == FLOW_TABLE_INSERTED);
  }
  CHECK(FlowTableSize(table) == kEntries);

  for (uint32_t index = 0; index < kEntries; index += 1000) {
    const FlowTableKeyV4 key = sKeyV4(index);
    CHECK(FlowTableInsertV4(table, &key, index) == FLOW_TABLE_UPDATED);
  }
  CHECK(FlowTableSize(table) == kEntries);

  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = sKeyV4(index);
    uint32_t value = 0;
    CHECK(FlowTableFindV4(table, &key, &value));
    CHECK(value == index);
  }

  for (uint32_t index = 0; index < kEntries; index += 2) {
    const FlowTableKeyV4 key = sKeyV4(index);
    CHECK(FlowTableEraseV4(table, &key));
    CHECK(!FlowTableEraseV4(table, &key));
  }
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV4 key = sKeyV4(index);
    uint32_t value = 0;
    const bool hit = FlowTableFindV4(table, &key, &value);
    CHECK(hit == ((index & 1u) != 0u));
    if (hit) CHECK(value == index);
  }
  CHECK(FlowTableSize(table) == kEntries / 2);

  static FlowTableKeyV4 keys[4096];
  static uint32_t values[4096];
  static bool found[4096];
  for (int index = 0; index < 4096; index++) keys[index] = sKeyV4(sNextRandom() % kEntries);
  const size_t hits = FlowTableFindBatchV4(table, keys, 4096, values, found);
  size_t expected = 0;
  for (int index = 0; index < 4096; index++) {
    uint32_t value = 0;
    const bool hit = FlowTableFindV4(table, &keys[index], &value);
    CHECK(hit == found[index]);
    if (hit) {
      CHECK(values[index] == value);
      expected++;
    }
  }
  CHECK(hits == expected);

  for (int round = 0; round < 2000000; round++) { /* churn */
    const FlowTableKeyV4 key = sKeyV4(sNextRandom() % kEntries);
    if (sNextRandom() & 1) CHECK(FlowTableInsertV4(table, &key, 1) != FLOW_TABLE_ERROR);
    else (void)FlowTableEraseV4(table, &key);
  }

  FlowTableDestroy(table);
}

static void
sRunV6(void)
{
  enum { kEntries = 500000 };
  FlowTable *table = FlowTableCreate(FLOW_TABLE_KIND_V6, 0, 0xc0ffee1234567890ull);
  CHECK(table != NULL);

  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = sKeyV6(index);
    CHECK(FlowTableInsertV6(table, &key, index) == FLOW_TABLE_INSERTED);
  }
  CHECK(FlowTableSize(table) == kEntries);

  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = sKeyV6(index);
    uint32_t value = 0;
    CHECK(FlowTableFindV6(table, &key, &value));
    CHECK(value == index);
  }

  for (uint32_t index = 0; index < kEntries; index += 3) {
    const FlowTableKeyV6 key = sKeyV6(index);
    CHECK(FlowTableEraseV6(table, &key));
  }
  for (uint32_t index = 0; index < kEntries; index++) {
    const FlowTableKeyV6 key = sKeyV6(index);
    uint32_t value = 0;
    const bool hit = FlowTableFindV6(table, &key, &value);
    CHECK(hit == (index % 3 != 0));
    if (hit) CHECK(value == index);
  }

  for (int round = 0; round < 1000000; round++) {
    const FlowTableKeyV6 key = sKeyV6(sNextRandom() % kEntries);
    if (sNextRandom() & 1) CHECK(FlowTableInsertV6(table, &key, 1) != FLOW_TABLE_ERROR);
    else (void)FlowTableEraseV6(table, &key);
  }

  FlowTableDestroy(table);
}

int
main(void)
{
  sRunV4();
  sRunV6();
  puts("flow_table_stress: ok");
  return 0;
}
