/**
 * @file inlined_slab_tests.cpp
 * @brief Unit tests for InlinedSlab: geometry, LIFO, guard, bulk and policy.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "uflib/slab/inlined_slab.h"
}

namespace {

struct Slot {
  uint32_t link;
  uint32_t trailer;
  uint8_t  payload[56];
};

static_assert(sizeof(Slot) == 64, "slot must stay one cache line");

class InlinedSlabTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    descriptor = {
        .region = nullptr,
        .region_bytes = 0,
        .capacity = kCapacity,
        .slot_size = sizeof(Slot),
        .alignment = alignof(Slot) * 8,
        .link_offset = 0,
        .low_water = 0,
        .flags = INLINED_SLAB_FLAG_NONE,
    };
    bytes = InlinedSlabBytes(&descriptor);
    ASSERT_NE(bytes, 0u);
    region = static_cast<unsigned char *>(aligned_alloc(descriptor.alignment, bytes));
    ASSERT_NE(region, nullptr);
    memset(region, 0xEE, bytes);
    descriptor.region = region;
    descriptor.region_bytes = bytes;
  }

  void TearDown() override { free(region); }

  void Init(unsigned flags = INLINED_SLAB_FLAG_NONE, size_t low_water = 0)
  {
    descriptor.flags = flags;
    descriptor.low_water = low_water;
    ASSERT_EQ(InlinedSlabInit(&slab, &descriptor), 0);
  }

  static constexpr size_t kCapacity = 8;
  InlinedSlab           slab{};
  InlinedSlabDescriptor descriptor{};
  unsigned char *       region = nullptr;
  size_t                bytes = 0;
};

TEST_F(InlinedSlabTest, UninitialisedOrCorruptedControlBlockIsRejected)
{
  InlinedSlab zeroed{};
  EXPECT_EQ(InlinedSlabAvailable(&zeroed), 0u);
  EXPECT_EQ(InlinedSlabCapacity(&zeroed), 0u);
  EXPECT_EQ(InlinedSlabTake(&zeroed), nullptr);
  EXPECT_FALSE(InlinedSlabPut(&zeroed, region));
  EXPECT_EQ(InlinedSlabReset(&zeroed), -1);

  Init();
  ASSERT_NE(InlinedSlabTake(&slab), nullptr);
  slab.magic = 0; /* a consumer that scribbles the control block */
  EXPECT_EQ(InlinedSlabAvailable(&slab), 0u);
  EXPECT_EQ(InlinedSlabTake(&slab), nullptr);
  EXPECT_EQ(InlinedSlabReset(&slab), -1);
}

TEST_F(InlinedSlabTest, GeometryRejectsBadInput)
{
  InlinedSlabDescriptor probe = descriptor;
  probe.slot_size = 0;
  EXPECT_EQ(InlinedSlabBytes(&probe), 0u);
  probe = descriptor;
  probe.capacity = 0;
  EXPECT_EQ(InlinedSlabBytes(&probe), 0u);
  probe = descriptor;
  probe.alignment = 3; /* not a power of two */
  EXPECT_EQ(InlinedSlabBytes(&probe), 0u);
  probe = descriptor;
  probe.alignment = 128; /* above slot size */
  EXPECT_EQ(InlinedSlabBytes(&probe), 0u);
  probe = descriptor;
  probe.slot_size = INLINED_SLAB_MIN_SLOT_SIZE - 1;
  EXPECT_EQ(InlinedSlabBytes(&probe), 0u);
  EXPECT_EQ(InlinedSlabBytes(nullptr), 0u);
}

TEST_F(InlinedSlabTest, InitStartsFullAndTakesSlotZeroFirst)
{
  Init();
  EXPECT_EQ(InlinedSlabCapacity(&slab), kCapacity);
  EXPECT_EQ(InlinedSlabAvailable(&slab), kCapacity);
  EXPECT_EQ(InlinedSlabHighWater(&slab), 0u);

  for (size_t index = 0; index < kCapacity; index++) {
    void *slot = InlinedSlabTake(&slab);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot, region + index * sizeof(Slot));
  }
  EXPECT_EQ(InlinedSlabTake(&slab), nullptr);
  EXPECT_EQ(InlinedSlabAvailable(&slab), 0u);
  EXPECT_EQ(InlinedSlabHighWater(&slab), kCapacity);
}

TEST_F(InlinedSlabTest, ReturnedSlotIsReusedBeforeAnyOther)
{
  Init();
  void *first = InlinedSlabTake(&slab);
  ASSERT_NE(first, nullptr);
  EXPECT_TRUE(InlinedSlabPut(&slab, first));
  EXPECT_EQ(InlinedSlabTake(&slab), first);
}

TEST_F(InlinedSlabTest, PutRejectsForeignAndMisalignedPointers)
{
  Init();
  int outsider = 0;
  EXPECT_FALSE(InlinedSlabPut(&slab, &outsider));
  EXPECT_FALSE(InlinedSlabPut(&slab, region + 1));
  EXPECT_FALSE(InlinedSlabPut(&slab, region + bytes));
  EXPECT_FALSE(InlinedSlabPut(&slab, nullptr));
}

TEST_F(InlinedSlabTest, GuardRejectsDoubleFree)
{
  Init(INLINED_SLAB_FLAG_GUARD);
  void *slot = InlinedSlabTake(&slab);
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(InlinedSlabPut(&slab, slot));
  EXPECT_FALSE(InlinedSlabPut(&slab, slot));
  EXPECT_EQ(InlinedSlabAvailable(&slab), kCapacity);
}

TEST_F(InlinedSlabTest, GuardNeedsRoomForItsTrailer)
{
  /* Link at the very end of the slot leaves no room for the guard word. */
  InlinedSlabDescriptor probe = descriptor;
  probe.link_offset = sizeof(Slot) - sizeof(uint32_t);
  probe.flags = INLINED_SLAB_FLAG_GUARD;
  EXPECT_EQ(InlinedSlabInit(&slab, &probe), -1);
  probe = descriptor;
  probe.region = nullptr;
  EXPECT_EQ(InlinedSlabInit(&slab, &probe), -1);
  EXPECT_EQ(InlinedSlabInit(&slab, nullptr), -1);
}

TEST_F(InlinedSlabTest, BulkTakeStopsAtExhaustionAndBulkPutReturnsAll)
{
  Init();
  void *slots[12] = {nullptr};
  EXPECT_EQ(InlinedSlabTakeBulk(&slab, slots, 12), kCapacity);
  EXPECT_EQ(InlinedSlabAvailable(&slab), 0u);
  EXPECT_EQ(InlinedSlabPutBulk(&slab, slots, 12), kCapacity);
  EXPECT_EQ(InlinedSlabAvailable(&slab), kCapacity);
}

TEST_F(InlinedSlabTest, ResetRefusesWhileLeasedAndWorksWhenEmpty)
{
  Init();
  void *slot = InlinedSlabTake(&slab);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(InlinedSlabReset(&slab), -1);
  EXPECT_TRUE(InlinedSlabPut(&slab, slot));
  EXPECT_EQ(InlinedSlabReset(&slab), 0);
  EXPECT_EQ(InlinedSlabAvailable(&slab), kCapacity);
  EXPECT_EQ(InlinedSlabHighWater(&slab), 0u);
}

TEST_F(InlinedSlabTest, ZeroOnTakeClearsTheSlot)
{
  Init(INLINED_SLAB_FLAG_ZERO_ON_TAKE);
  void *slot = InlinedSlabTake(&slab);
  ASSERT_NE(slot, nullptr);
  const uint8_t *bytes_ptr = static_cast<const uint8_t *>(slot);
  for (size_t index = 0; index < sizeof(Slot); index++) EXPECT_EQ(bytes_ptr[index], 0u) << index;
}

TEST_F(InlinedSlabTest, ScarceTracksTheAdvisoryThreshold)
{
  Init(INLINED_SLAB_FLAG_NONE, 2);
  EXPECT_FALSE(InlinedSlabScarce(&slab));
  for (int index = 0; index < 6; index++) ASSERT_NE(InlinedSlabTake(&slab), nullptr);
  EXPECT_EQ(InlinedSlabAvailable(&slab), 2u);
  EXPECT_FALSE(InlinedSlabScarce(&slab));
  ASSERT_NE(InlinedSlabTake(&slab), nullptr);
  EXPECT_TRUE(InlinedSlabScarce(&slab));
}

}  // namespace
