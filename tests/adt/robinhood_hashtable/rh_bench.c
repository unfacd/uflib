/**
 * @file rh_bench.c
 * @brief Profiling baseline for rh_template.h and the instantiations it ships.
 *
 * Four shapes, timed separately: the exported identifier map (u64 to pointer,
 * through id_64bit), a dense scalar index (u32 to u32, table-local), the IPv4
 * flow tuple (through flow4) and the same tuple through the public FlowTable API,
 * which is the path the media code takes. Each phase is timed as a whole over a fixed key
 * set, so the reported figure is nanoseconds per operation without per-op clock
 * overhead. Keys are generated once, before timing, and every result is folded
 * into a sink so no phase can be optimised away.
 *
 * Not a correctness test and not registered with CTest: it prints a table, and
 * the design document records the numbers it produced. Build with -O2 (Release).
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <uflib/adt/flow_table/flow_table.h>
#include <uflib/adt/robinhood_hashtable/flow4.h>
#include <uflib/adt/robinhood_hashtable/id_64bit.h>
#include <uflib/adt/robinhood_hashtable/rh_common.h>

/* The dense scalar shape: a u32 key and a u32 payload, 8-byte slot. */
#define RhtTemplateName U32Map
#define RhtTemplateKey uint32_t
#define RhtTemplateValue uint32_t
#define RhtTemplateHash(key, secret_a, secret_b) sRhtFoldMultiply((uint64_t)*(key) ^ (secret_a), (secret_b))
#define RhtTemplateKeyEq(left, right) (*(left) == *(right))
#define RhtTemplateSlotEmpty(slot) ((slot)->key == 0 && (slot)->value == 0)
#include <uflib/adt/robinhood_hashtable/rh_template.h>

enum { U64_KEYS = 2000000, U32_KEYS = 2000000, FLOW4_KEYS = 1000000 };

static volatile uint64_t sSink;

static uint64_t sRandomState = 88172645463325252ull;

static uint64_t
sRandom(void)
{
  sRandomState ^= sRandomState << 13;
  sRandomState ^= sRandomState >> 7;
  sRandomState ^= sRandomState << 17;
  return sRandomState;
}

static uint64_t
sNow(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void
sReport(const char *shape, const char *phase, size_t count, uint64_t elapsed)
{
  const double per_op = (double)elapsed / (double)count;
  printf("%-8s %-10s %9zu %10.2f %12.2f\n", shape, phase, count, per_op, 1e3 / per_op);
}

static void
sBenchId64(void)
{
  static uint64_t keys[U64_KEYS];
  static uint64_t absent[U64_KEYS];
  static int storage;
  for (size_t index = 0; index < U64_KEYS; index++) {
    keys[index] = sRandom() >> 8;
    absent[index] = keys[index] ^ 0x8000000000000000ull;
  }

  Id64Map *map = Id64MapCreate(0, 0x9E3779B97F4A7C15ull);
  uint64_t start = sNow();
  for (size_t index = 0; index < U64_KEYS; index++) Id64MapPut(map, keys[index], &storage);
  sReport("id_64bit", "put", U64_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < U64_KEYS; index++) {
    sSink += (uintptr_t)Id64MapGet(map, keys[index]);
  }
  sReport("id_64bit", "get-hit", U64_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < U64_KEYS; index++) {
    sSink += (uintptr_t)Id64MapGet(map, absent[index]);
  }
  sReport("id_64bit", "get-miss", U64_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < U64_KEYS; index++) {
    sSink += (uintptr_t)Id64MapRemove(map, keys[index]);
  }
  sReport("id_64bit", "remove", U64_KEYS, sNow() - start);
  Id64MapDestroy(map);
}

static void
sBenchU32(void)
{
  static uint32_t keys[U32_KEYS];
  static uint32_t absent[U32_KEYS];
  for (size_t index = 0; index < U32_KEYS; index++) {
    keys[index] = (uint32_t)(sRandom() >> 32);
    absent[index] = keys[index] ^ 0x80000000u;
  }
  U32Map *map = sU32MapCreate(0, 0, 0x9E3779B97F4A7C15ull);
  uint64_t start = sNow();
  for (size_t index = 0; index < U32_KEYS; index++) sU32MapInsert(map, &keys[index], (uint32_t)index + 1);
  sReport("u32->u32", "put", U32_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < U32_KEYS; index++) {
    uint32_t value = 0;
    sSink += sU32MapFind(map, &keys[index], &value) ? value : 0;
  }
  sReport("u32->u32", "get-hit", U32_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < U32_KEYS; index++) {
    uint32_t value = 0;
    sSink += sU32MapFind(map, &absent[index], &value) ? value : 0;
  }
  sReport("u32->u32", "get-miss", U32_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < U32_KEYS; index++) sU32MapErase(map, &keys[index]);
  sReport("u32->u32", "remove", U32_KEYS, sNow() - start);

  /* batch lookup, 16 keys per pipeline (the template's RH_TEMPLATE_BATCH) */
  static uint32_t values[U32_KEYS];
  static bool found[U32_KEYS];
  for (size_t index = 0; index < U32_KEYS; index++) sU32MapInsert(map, &keys[index], (uint32_t)index + 1);
  start = sNow();
  sSink += sU32MapFindBatch(map, keys, U32_KEYS, values, found);
  sReport("u32->u32", "get-batch", U32_KEYS, sNow() - start);
  sU32MapDestroy(map);
}

static void
sBenchFlow4(void)
{
  static Flow4Key keys[FLOW4_KEYS];
  static Flow4Key absent[FLOW4_KEYS];
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    keys[index].source_ip = 0x0A000000u + (uint32_t)index;
    keys[index].destination_ip = 0xC0A80000u ^ (uint32_t)(index >> 3);
    keys[index].source_port = (uint16_t)(1024 + index % 60000);
    keys[index].destination_port = (uint16_t)((index % 7) ? 443 : 80);
    absent[index] = keys[index];
    absent[index].source_port ^= 0x8000u;
  }

  Flow4Table *table = Flow4Create(0, 0, 0x9E3779B97F4A7C15ull);
  uint64_t start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) Flow4Insert(table, &keys[index], (uint32_t)index + 1);
  sReport("flow4", "put", FLOW4_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    uint32_t value = 0;
    sSink += Flow4Find(table, &keys[index], &value) ? value : 0;
  }
  sReport("flow4", "get-hit", FLOW4_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    uint32_t value = 0;
    sSink += Flow4Find(table, &absent[index], &value) ? value : 0;
  }
  sReport("flow4", "get-miss", FLOW4_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) Flow4Erase(table, &keys[index]);
  sReport("flow4", "remove", FLOW4_KEYS, sNow() - start);
  Flow4Destroy(table);
}

/* The same tuple through the public API: FlowTable's V4 entry points, whose
 * internals are the Flow4Map instantiation this file also benchmarks directly. */
static void
sBenchFlowTable(void)
{
  static Flow4Key keys[FLOW4_KEYS];
  static Flow4Key absent[FLOW4_KEYS];
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    keys[index].source_ip = 0x0A000000u + (uint32_t)index;
    keys[index].destination_ip = 0xC0A80000u ^ (uint32_t)(index >> 3);
    keys[index].source_port = (uint16_t)(1024 + index % 60000);
    keys[index].destination_port = (uint16_t)((index % 7) ? 443 : 80);
    absent[index] = keys[index];
    absent[index].source_port ^= 0x8000u;
  }

  FlowTable *table = FlowTableCreate(FLOW_TABLE_KIND_V4, 0, 0x9E3779B97F4A7C15ull);
  uint64_t start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    FlowTableInsertV4(table, &keys[index], (uint32_t)index + 1);
  }
  sReport("flowtbl4", "put", FLOW4_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    uint32_t value = 0;
    sSink += FlowTableFindV4(table, &keys[index], &value) ? value : 0;
  }
  sReport("flowtbl4", "get-hit", FLOW4_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) {
    uint32_t value = 0;
    sSink += FlowTableFindV4(table, &absent[index], &value) ? value : 0;
  }
  sReport("flowtbl4", "get-miss", FLOW4_KEYS, sNow() - start);

  start = sNow();
  for (size_t index = 0; index < FLOW4_KEYS; index++) FlowTableEraseV4(table, &keys[index]);
  sReport("flowtbl4", "remove", FLOW4_KEYS, sNow() - start);
  FlowTableDestroy(table);
}

int
main(void)
{
  puts("shape    phase           count   ns/op        Mops/s");
  sBenchId64();
  sBenchU32();
  sBenchFlow4();
  sBenchFlowTable();
  printf("sink=%llu\n", (unsigned long long)sSink);
  return 0;
}
