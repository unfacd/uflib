/**
 * @file recycler_tests.cpp
 * @brief Baseline lifecycle and single-thread correctness tests for the Recycler V1
 *
 * Tests the stabilized V1 recycler API: init, get, put, refcount, expansion,
 * InstanceHolder, InstancesList, and callback lifecycle.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/recycler/recycler.h>
#include <uflib/recycler/recycler_defs.h>
#include "recycler_priv.h"
}

/* ── Test fixtures ──────────────────────────────────────────────────────── */

/** Minimal payload type for recycler testing. */
typedef struct {
	int    id;
	bool   initialized;
	void  *user_data;
} TestPayload;

/* ── Callback implementations ──────────────────────────────────────────── */

static int
sInitCallback(ClientContextData *data_ptr, size_t oid)
{
	TestPayload *p = (TestPayload *)data_ptr;
	p->id = (int)oid;
	p->initialized = true;
	return 0;
}

static int
sInitGetCallback(InstanceHolder *ih_ptr, ContextData *ctx, size_t oid, unsigned long flags)
{
	TestPayload *p = (TestPayload *)GetInstance(ih_ptr);
	p->initialized = true;  // restore after previous Put cleared it
	(void)ctx;
	(void)oid;
	(void)flags;
	return 0;  // success
}

static int
sInitPutCallback(InstanceHolder *ih_ptr, ContextData *ctx, unsigned long flags)
{
	TestPayload *p = (TestPayload *)GetInstance(ih_ptr);
	p->initialized = false;
	(void)ctx;
	(void)flags;
	return 0;
}

static char *
sPrintCallback(InstanceHolder *ih_ptr, ContextData *ctx, unsigned long flags)
{
	(void)ih_ptr;
	(void)ctx;
	(void)flags;
	return nullptr;
}

static int
sDestructCallback(InstanceHolder *ih_ptr, ContextData *ctx, unsigned long flags)
{
	(void)ih_ptr;
	(void)ctx;
	(void)flags;
	return 0;
}

static RecyclerPoolOps sTestOps = {
	sInitCallback,
	sInitGetCallback,
	sInitPutCallback,
	sPrintCallback,
	sDestructCallback,
	nullptr  // poolop_instantiator_callback
};

/* ── Lifecycle tests ──────────────────────────────────────────────────── */

TEST(RecyclerLifecycle, InitSingleType)
{
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"TestPayload", sizeof(TestPayload), 4, 64, &sTestOps);
	ASSERT_NE(handle, nullptr);
	EXPECT_EQ(handle->type, 1u);
	EXPECT_STREQ(handle->type_name, "TestPayload");
	EXPECT_EQ(handle->blocksz, sizeof(TestPayload));
}

TEST(RecyclerLifecycle, InitMultipleTypes)
{
	// Note: type numbers are sequential and global — earlier tests
	// may have already registered types, so we only check that
	// each new type gets a distinct, non-zero number.
	RecyclerPoolHandle *h1 = RecyclerInitTypePool(
		"TypeOne", 64, 4, 32, &sTestOps);
	RecyclerPoolHandle *h2 = RecyclerInitTypePool(
		"TypeTwo", 128, 4, 32, &sTestOps);
	ASSERT_NE(h1, nullptr);
	ASSERT_NE(h2, nullptr);
	EXPECT_NE(h1->type, h2->type);
	EXPECT_GT(h1->type, 0u);
	EXPECT_GT(h2->type, 0u);
}

TEST(RecyclerLifecycle, InitEmptyOps)
{
	// All-zero ops struct — callbacks are optional
	RecyclerPoolOps empty_ops = {0};
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"EmptyOps", sizeof(TestPayload), 4, 64, &empty_ops);
	EXPECT_NE(handle, nullptr);
}

/* ── Get/Put round-trip tests ─────────────────────────────────────────── */

TEST(RecyclerGetPut, SingleRoundTrip)
{
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"RoundTrip", sizeof(TestPayload), 4, 64, &sTestOps);
	ASSERT_NE(handle, nullptr);

	InstanceHolder *ih = RecyclerGet(handle->type, nullptr, 0);
	ASSERT_NE(ih, nullptr);
	EXPECT_TRUE(IsInstance(ih));

	TestPayload *p = (TestPayload *)GetInstance(ih);
	ASSERT_NE(p, nullptr);
	EXPECT_TRUE(p->initialized);
	EXPECT_GT(p->id, 0);

	int rc = RecyclerPut(handle->type, ih, nullptr, 0);
	EXPECT_EQ(rc, 0);
}

TEST(RecyclerGetPut, MultipleSequential)
{
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"MultiGet", sizeof(TestPayload), 4, 64, &sTestOps);
	ASSERT_NE(handle, nullptr);

	for (int i = 0; i < 20; i++) {
		InstanceHolder *ih = RecyclerGet(handle->type, nullptr, 0);
		ASSERT_NE(ih, nullptr);
		TestPayload *p = (TestPayload *)GetInstance(ih);
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(p->initialized, true);

		int rc = RecyclerPut(handle->type, ih, nullptr, 0);
		EXPECT_EQ(rc, 0);
	}
}

/* ── Refcount tests ────────────────────────────────────────────────────── */

TEST(RecyclerRefcount, IncrementAndDecrement)
{
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"Refcount", sizeof(TestPayload), 4, 64, &sTestOps);
	ASSERT_NE(handle, nullptr);

	InstanceHolder *ih = RecyclerGet(handle->type, nullptr, 0);
	ASSERT_NE(ih, nullptr);

	size_t rc0 = RecyclerTypeGetReferenceCount(handle->type, ih);
	EXPECT_GE(rc0, 2u);  // base(1) + get(1) = 2

	RecyclerTypeReferenced(handle->type, ih, 1);
	size_t rc1 = RecyclerTypeGetReferenceCount(handle->type, ih);
	EXPECT_EQ(rc1, rc0 + 1);

	RecyclerTypeUnReferenced(handle->type, ih, 1);
	size_t rc2 = RecyclerTypeGetReferenceCount(handle->type, ih);
	EXPECT_EQ(rc2, rc0);

	// Put expects refcount == 2 (base + 1 active reference)
	int ret = RecyclerPut(handle->type, ih, nullptr, 0);
	EXPECT_EQ(ret, 0);
}

/* ── InstanceHolder tests ──────────────────────────────────────────────── */

TEST(InstanceHolder, SetInstanceAndGetInstance)
{
	TestPayload payload = {42, true, nullptr};
	InstanceHolder holder;
	SetInstance(&holder, &payload);

	EXPECT_TRUE(IsInstance(&holder));
	EXPECT_FALSE(IsMarshaller(&holder));

	TestPayload *retrieved = (TestPayload *)GetInstance(&holder);
	EXPECT_EQ(retrieved, &payload);
	EXPECT_EQ(retrieved->id, 42);
}

TEST(InstanceHolder, SetMarshallerAndGetMarshaller)
{
	InstanceHolder holder;
	SetMarshaller(&holder, 0xABCD);

	EXPECT_TRUE(IsMarshaller(&holder));
	EXPECT_FALSE(IsInstance(&holder));

	uintptr_t id = GetMarshaller(&holder);
	EXPECT_EQ(id, 0xABCDu);
}

TEST(InstanceHolder, ModeTransitionInstanceToInstance)
{
	TestPayload p1 = {1}, p2 = {2};
	InstanceHolder holder;
	SetInstance(&holder, &p1);
	EXPECT_TRUE(IsInstance(&holder));

	// Overwrite with new Instance — must be clean
	SetInstance(&holder, &p2);
	EXPECT_TRUE(IsInstance(&holder));
	TestPayload *retrieved = (TestPayload *)GetInstance(&holder);
	EXPECT_EQ(retrieved, &p2);
}

/* ── Pool exhaustion test ──────────────────────────────────────────────── */

TEST(RecyclerExpansion, ExhaustAndExpand)
{
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"Exhaust", sizeof(TestPayload), 16, 8, &sTestOps);
	ASSERT_NE(handle, nullptr);

	std::vector<InstanceHolder *> holders;

	// Drain the pool
	for (int i = 0; i < 200; i++) {
		InstanceHolder *ih = RecyclerGet(handle->type, nullptr, 0);
		if (ih == nullptr) break;
		holders.push_back(ih);
	}

	EXPECT_GT(holders.size(), 8u);  // should have expanded beyond first group

	// Return all
	for (auto *ih : holders) {
		EXPECT_EQ(RecyclerPut(handle->type, ih, nullptr, 0), 0);
	}
}

/* ── Concurrent tests ─────────────────────────────────────────────────── */

TEST(RecyclerConcurrent, DisjointGetPut)
{
	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"Concurrent", sizeof(TestPayload), 16, 256, &sTestOps);
	ASSERT_NE(handle, nullptr);

	constexpr int kThreads    = 4;
	constexpr int kIters      = 100;
	constexpr int kPoolMargin = 256;  // pool is large enough for all threads

	std::vector<std::thread> threads;
	std::atomic<int> get_errors{0};
	std::atomic<int> put_errors{0};
	std::atomic<int> null_gets{0};

	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([handle, &get_errors, &put_errors, &null_gets]() {
			for (int i = 0; i < kIters; i++) {
				InstanceHolder *ih = RecyclerGet(handle->type, nullptr, 0);
				if (ih == nullptr) {
					null_gets.fetch_add(1);
					continue;
				}
				TestPayload *p = (TestPayload *)GetInstance(ih);
				if (p == nullptr || !p->initialized) {
					get_errors.fetch_add(1);
					// Still must put it back to avoid leaking
				}
				int rc = RecyclerPut(handle->type, ih, nullptr, 0);
				if (rc != 0) {
					put_errors.fetch_add(1);
				}
			}
		});
	}

	for (auto &th : threads) th.join();

	EXPECT_EQ(null_gets.load(), 0);
	EXPECT_EQ(get_errors.load(), 0);
	EXPECT_EQ(put_errors.load(), 0);
}
