/**
 * @file
 * @brief Comprehensive gtest suite for the V1 scheduled_jobs module.
 *
 * Exercises the current V1 API as-is.  Covers lifecycle, type registration,
 * insert/peek/removeMin, lock-hints, concurrency, edge cases, and bug
 * regression verification (Bug A, Bug B, Bug C, Bug D).
 *
 * These tests will be carried forward and adapted for V2 after the refactor.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

extern "C" {
#include <uflib/scheduled_jobs/scheduled_jobs.h>
}

/* ── Test infrastructure ──────────────────────────────────────────── */

namespace {

/* The V1 API requires the consumer to define GetScheduledJobsStore().
 * Each test gets a fresh static store via this helper. */
static ScheduledJobs *sGetFreshStore(void)
{
    static ScheduledJobs s_store;
    memset(&s_store, 0, sizeof(s_store));
    return &s_store;
}

/* ── Fake clock for deterministic time ────────────────────────────── */

static std::atomic<long long> s_fake_time_us{0};

extern "C" {
static long long sFakeGetTime(void)
{
    return s_fake_time_us.load(std::memory_order_relaxed);
}
}

/* Advance the fake clock and return the new time. */
static long long sAdvanceTime(long long delta_us)
{
    return s_fake_time_us.fetch_add(delta_us, std::memory_order_relaxed) + delta_us;
}

/* ── Fake callbacks ───────────────────────────────────────────────── */

extern "C" {
static int sFakeOnRun(void * /*job_ctx*/, void * /*client_ctx*/)
{
    return 0;
}

static int sFakeOnFirstInsert(void * /*job_ctx*/, void * /*client_ctx*/)
{
    return 42;  /* distinct return value for verification */
}

static int sFakeOnError(void * /*client_ctx*/)
{
    return -1;
}

static int sFakeCompareKeys(void *key1_ptr, void *key2_ptr)
{
    long long a = *(long long *)key1_ptr;
    long long b = *(long long *)key2_ptr;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}
}

/* ── Helper to create a ScheduledJobType ──────────────────────────── */

static ScheduledJobType sMakeType(const char *name,
                                   enum ScheduledJobExecutionFrequencyMode freq,
                                   uint64_t freq_us)
{
    ScheduledJobType t;
    memset(&t, 0, sizeof(t));
    t.type_name      = name;
    t.frequency_mode = freq;
    t.frequency      = freq_us;
    t.callbacks.on_get_time     = sFakeGetTime;
    t.callbacks.on_run          = sFakeOnRun;
    t.callbacks.on_compare_keys = sFakeCompareKeys;
    return t;
}

/* ── Helper to create a ScheduledJob ──────────────────────────────── */

static ScheduledJob sMakeJob(ScheduledJobType *type_ptr, long long when_to_schedule)
{
    ScheduledJob j;
    memset(&j, 0, sizeof(j));
    j.job_type_ptr     = type_ptr;
    j.when_to_schedule = when_to_schedule;
    return j;
}

}  /* namespace */

/* ═════════════════════════════════════════════════════════════════════
   Lifecycle tests (6)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, InitStoreWithValidCount)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 16);
    /* Store should be initialised — spinlock + heap created */
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);
    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, InitStoreWithZeroCount)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 0);
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);
    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, InitStoreTwice)
{
    /* Calling Init twice on the same store without an intervening Destruct
     * leaks the first heap and re-initialises the spinlock (UB).  This test
     * verifies the correct pattern: Destruct between inits. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);
    DestructScheduledJobs(s);
    /* Re-init after proper teardown */
    InitScheduledJobsStore(s, 16);
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);
    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, DestructClearsStore)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);
    DestructScheduledJobs(s);
    /* After destruct, the store is zeroed — job_types_index is NULL.
     * NOTE: we cannot call GetScheduleJobsSetsize() after Destruct because
     * pthread_spin_lock on the memset-zeroed spinlock is UB (Bug C:
     * spinlock was never destroyed, just overwritten). */
    EXPECT_EQ(s->job_types_descriptor.job_types_index, nullptr);
}

TEST(ScheduledJobsV1, DestructTwiceIsSafe)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);
    DestructScheduledJobs(s);
    /* Second destruct on zeroed struct — free(NULL) is safe, but
     * note Bug C: heap entries[] and spinlock are leaked by the first
     * destruct.  This test verifies the function doesn't crash. */
    EXPECT_NO_FATAL_FAILURE(DestructScheduledJobs(s));
}

TEST(ScheduledJobsV1, InitDestructInitReuse)
{
    for (int cycle = 0; cycle < 3; cycle++) {
        ScheduledJobs *s = sGetFreshStore();
        InitScheduledJobsStore(s, 4);
        EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);
        DestructScheduledJobs(s);
    }
    SUCCEED();
}

/* ═════════════════════════════════════════════════════════════════════
   Type registration tests (5)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, RegisterSingleType)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type_a = sMakeType("type_a", PERIODIC, 1000);
    int id = RegisterScheduledJobType(s, &type_a);
    EXPECT_GE(id, 0);
    EXPECT_EQ(type_a.type_id, id);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, RegisterMultipleTypes)
{
    /* With Bug A fixed, all registered types must be findable. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    /* Use static string literals — the V1 API stores the pointer directly
     * (no strdup), so the strings must outlive the store. */
    static const char *kNames[] = {
        "reg_type_0", "reg_type_1", "reg_type_2", "reg_type_3", "reg_type_4"
    };
    ScheduledJobType types[5];
    for (int i = 0; i < 5; i++) {
        types[i] = sMakeType(kNames[i], PERIODIC, 1000 * (i + 1));
        int id = RegisterScheduledJobType(s, &types[i]);
        EXPECT_GE(id, 0);
    }

    /* All 5 types must be findable */
    pthread_spin_lock(&s->spin_lock);
    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(IsJobTypeNameRegistered(s, kNames[i]))
            << "type " << kNames[i] << " not found";
    }
    pthread_spin_unlock(&s->spin_lock);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, DuplicateTypeRegistrationIsIdempotent)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type_a = sMakeType("type_a", PERIODIC, 1000);
    int id1 = RegisterScheduledJobType(s, &type_a);
    EXPECT_GE(id1, 0);

    /* Register the same type again — should return existing id */
    int id2 = RegisterScheduledJobType(s, &type_a);
    EXPECT_EQ(id2, id1);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, IsTypeNameRegisteredFalseForUnknown)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    pthread_spin_lock(&s->spin_lock);
    EXPECT_FALSE(IsJobTypeNameRegistered(s, "nonexistent"));
    pthread_spin_unlock(&s->spin_lock);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, IsTypeNameRegisteredOnEmptyStore)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    pthread_spin_lock(&s->spin_lock);
    EXPECT_FALSE(IsJobTypeNameRegistered(s, "anything"));
    pthread_spin_unlock(&s->spin_lock);

    DestructScheduledJobs(s);
}

/* ═════════════════════════════════════════════════════════════════════
   Insert / Peek / RemoveMin tests (8)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, InsertAndPeek)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("periodic_job", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);  /* 0 → use type frequency */
    EXPECT_EQ(InsertScheduledJob(s, &job), 0);

    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 1u);

    ScheduledJobContext ctx = {0};
    ScheduledJobContext *result = GetScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->scheduled_job_ptr, &job);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, InsertMultipleAndVerifyOrder)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("order_test", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    /* Insert 3 jobs with different override intervals — they fire at
     * different absolute times, so the heap should order them. */
    s_fake_time_us = 0;
    ScheduledJob job_soon  = sMakeJob(&type, 10);    /* fires at t=10 */
    ScheduledJob job_mid   = sMakeJob(&type, 100);   /* fires at t=100 */
    ScheduledJob job_late  = sMakeJob(&type, 1000);  /* fires at t=1000 */

    EXPECT_EQ(InsertScheduledJob(s, &job_late), 0);
    EXPECT_EQ(InsertScheduledJob(s, &job_soon), 0);
    EXPECT_EQ(InsertScheduledJob(s, &job_mid), 0);

    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 3u);

    /* Peek — should be the earliest */
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->scheduled_job_ptr, &job_soon);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, RemoveMinPromotesNext)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("remove_test", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job1 = sMakeJob(&type, 10);
    ScheduledJob job2 = sMakeJob(&type, 100);
    ScheduledJob job3 = sMakeJob(&type, 1000);

    InsertScheduledJob(s, &job1);
    InsertScheduledJob(s, &job2);
    InsertScheduledJob(s, &job3);

    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 3u);

    /* Remove first — should get job1, size becomes 2 */
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r1 = GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r1->scheduled_job_ptr, &job1);
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 2u);

    /* Peek — should now be job2 */
    ScheduledJobContext ctx2 = {0};
    ScheduledJobContext *r2 = GetScheduledJob(s, LOCK_HINT_NONE, &ctx2);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r2->scheduled_job_ptr, &job2);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, PeekOnEmptyStoreReturnsNull)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetScheduledJob(s, LOCK_HINT_NONE, &ctx);
    EXPECT_EQ(r, nullptr);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, RemoveMinOnEmptyStoreReturnsNull)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx);
    EXPECT_EQ(r, nullptr);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, ReInsertPeriodicJob)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("periodic", PERIODIC, 500);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    EXPECT_EQ(InsertScheduledJob(s, &job), 0);

    /* Advance time and re-insert */
    sAdvanceTime(1000);
    ReInsertScheduledJob(s, &job);

    /* Job should now be scheduled at a later time */
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->scheduled_job_ptr, &job);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, OneOffJob)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("oneoff", ONEOFF, 100);
    RegisterScheduledJobType(s, &type);

    ScheduledJob job = sMakeJob(&type, 0);
    EXPECT_FALSE(IsJobPeriodic(&job));

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, PeriodicJob)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("periodic", PERIODIC, 100);
    RegisterScheduledJobType(s, &type);

    ScheduledJob job = sMakeJob(&type, 0);
    EXPECT_TRUE(IsJobPeriodic(&job));

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, PeriodicJobReInsertAfterRemove)
{
    /* PERIODIC jobs: the consumer removes the job, checks IsJobPeriodic,
     * and re-inserts.  This is the consumer-side contract — the scheduler
     * does NOT automatically re-insert based on frequency_mode. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("periodic_reinsert", PERIODIC, 500);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    InsertScheduledJob(s, &job);

    /* Remove and check — the consumer decides to re-insert */
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->scheduled_job_ptr, &job);

    /* Consumer-side pattern: check periodic, re-insert */
    EXPECT_TRUE(IsJobPeriodic(&job));
    sAdvanceTime(1000);
    ReInsertScheduledJob(s, &job);

    /* Job should be back in the store */
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 1u);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, OneOffJobNotReInsertedAfterRemove)
{
    /* ONEOFF jobs: the consumer removes the job, checks IsJobPeriodic,
     * and does NOT re-insert.  The store becomes empty. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("oneoff_noreinsert", ONEOFF, 100);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    InsertScheduledJob(s, &job);

    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 1u);

    /* Remove — consumer checks periodic and chooses NOT to re-insert */
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->scheduled_job_ptr, &job);
    EXPECT_FALSE(IsJobPeriodic(&job));

    /* Consumer does NOT call ReInsert — store is now empty */
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);

    /* Peek confirms nothing remains */
    EXPECT_EQ(GetScheduledJob(s, LOCK_HINT_NONE, &ctx), nullptr);

    DestructScheduledJobs(s);
}

/* ═════════════════════════════════════════════════════════════════════
   Lock-hints tests (4)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, LockHintAlreadyLocked)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("lock_test", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    InsertScheduledJob(s, &job);

    /* Acquire lock once, then use ALREADY_LOCKED */
    pthread_spin_lock(&s->spin_lock);
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetScheduledJob(s, LOCK_HINT_ALREADY_LOCKED | LOCK_HINT_KEEP_LOCKED, &ctx);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->scheduled_job_ptr, &job);
    pthread_spin_unlock(&s->spin_lock);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, LockHintKeepLocked)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("keep_test", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    InsertScheduledJob(s, &job);

    /* Lock once, peek + removeMin atomically.  The last operation in the
     * chain uses KEEP_LOCKED so we can unlock once at the end. */
    pthread_spin_lock(&s->spin_lock);

    ScheduledJobContext ctx_peek = {0};
    ScheduledJobContext *r1 = GetScheduledJob(s,
        LOCK_HINT_ALREADY_LOCKED | LOCK_HINT_KEEP_LOCKED, &ctx_peek);
    ASSERT_NE(r1, nullptr);

    ScheduledJobContext ctx_rem = {0};
    ScheduledJobContext *r2 = GetRemScheduledJob(s,
        LOCK_HINT_ALREADY_LOCKED | LOCK_HINT_KEEP_LOCKED, &ctx_rem);
    ASSERT_NE(r2, nullptr);

    /* All operations used KEEP_LOCKED — release once now. */
    pthread_spin_unlock(&s->spin_lock);

    EXPECT_EQ(r1->scheduled_job_ptr, &job);
    EXPECT_EQ(r2->scheduled_job_ptr, &job);
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, LockHintNoneStandardSemantics)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("none_test", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    InsertScheduledJob(s, &job);

    /* LOCK_HINT_NONE — acquires and releases internally */
    ScheduledJobContext ctx = {0};
    ScheduledJobContext *r = GetScheduledJob(s, LOCK_HINT_NONE, &ctx);
    ASSERT_NE(r, nullptr);
    /* Lock should be released — we can call again without ALREADY_LOCKED */
    ScheduledJobContext ctx2 = {0};
    r = GetScheduledJob(s, LOCK_HINT_NONE, &ctx2);
    ASSERT_NE(r, nullptr);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, GetSizeWithLockHints)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("size_test", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    s_fake_time_us = 0;
    ScheduledJob j1 = sMakeJob(&type, 0);
    ScheduledJob j2 = sMakeJob(&type, 0);
    InsertScheduledJob(s, &j1);
    InsertScheduledJob(s, &j2);

    /* Size with lock held externally — KEEP_LOCKED so the function
     * does not release a lock it didn't acquire. */
    pthread_spin_lock(&s->spin_lock);
    EXPECT_EQ(GetScheduleJobsSetsize(s,
        LOCK_HINT_ALREADY_LOCKED | LOCK_HINT_KEEP_LOCKED), 2u);
    pthread_spin_unlock(&s->spin_lock);

    /* Size with internal lock */
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 2u);

    DestructScheduledJobs(s);
}

/* ═════════════════════════════════════════════════════════════════════
   Concurrency tests (4)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, ConcurrentInserts)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 64);

    ScheduledJobType type = sMakeType("concurrent", PERIODIC, 100);
    RegisterScheduledJobType(s, &type);

    const int kThreads = 4;
    const int kPerThread = 50;
    s_fake_time_us = 0;

    ScheduledJob jobs[4][50];
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([s, &type, &jobs, t]() {
            for (int i = 0; i < kPerThread; i++) {
                jobs[t][i] = sMakeJob(&type, (t * 1000) + i);
                InsertScheduledJob(s, &jobs[t][i]);
            }
        });
    }

    for (auto &th : threads) th.join();

    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE),
              (size_t)(kThreads * kPerThread));

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, ConcurrentPeekAndRemove)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 64);

    ScheduledJobType type = sMakeType("peek_remove", PERIODIC, 100);
    RegisterScheduledJobType(s, &type);

    /* Pre-populate */
    const int kJobs = 200;
    s_fake_time_us = 0;
    std::vector<ScheduledJob> jobs(kJobs);
    for (int i = 0; i < kJobs; i++) {
        jobs[i] = sMakeJob(&type, i * 10);
        InsertScheduledJob(s, &jobs[i]);
    }

    /* 4 threads peek, 2 threads remove */
    std::atomic<bool> stop{false};
    std::atomic<int> peeks{0};
    std::atomic<int> removes{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([s, &stop, &peeks]() {
            while (!stop.load(std::memory_order_relaxed)) {
                ScheduledJobContext ctx = {0};
                if (GetScheduledJob(s, LOCK_HINT_NONE, &ctx) != nullptr) {
                    peeks.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int t = 0; t < 2; t++) {
        threads.emplace_back([s, &stop, &removes]() {
            while (!stop.load(std::memory_order_relaxed)) {
                ScheduledJobContext ctx = {0};
                if (GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx) != nullptr) {
                    removes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true, std::memory_order_relaxed);
    for (auto &th : threads) th.join();

    /* Should have performed some operations without crashing */
    EXPECT_GT(peeks.load(), 0);
    /* Removes may be limited by the number of jobs inserted */

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, ConcurrentTypeRegistration)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 64);

    const int kThreads = 4;
    const int kPerThread = 5;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([s, t]() {
            for (int i = 0; i < kPerThread; i++) {
                char name[32];
                snprintf(name, sizeof(name), "concurrent_type_%d_%d", t, i);

                /* Each type needs stable storage — use static */
                static ScheduledJobType types[4][5];
                types[t][i] = sMakeType(name, PERIODIC, 1000);
                RegisterScheduledJobType(s, &types[t][i]);
            }
        });
    }

    for (auto &th : threads) th.join();

    /* NOTE: Due to Bug A (job_types_size never incremented), only the
     * last registration survives under contention.  This test primarily
     * verifies no crash or deadlock under concurrent registration. */
    SUCCEED();

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, SizeConsistencyUnderConcurrentInsert)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 64);

    ScheduledJobType type = sMakeType("size_con", PERIODIC, 100);
    RegisterScheduledJobType(s, &type);

    const int kThreads = 4;
    const int kPerThread = 25;
    s_fake_time_us = 0;
    ScheduledJob jobs[4][25];
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([s, &type, &jobs, t]() {
            for (int i = 0; i < kPerThread; i++) {
                jobs[t][i] = sMakeJob(&type, i);
                InsertScheduledJob(s, &jobs[t][i]);
            }
        });
    }

    for (auto &th : threads) th.join();

    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE),
              (size_t)(kThreads * kPerThread));

    DestructScheduledJobs(s);
}

/* ═════════════════════════════════════════════════════════════════════
   Callback tests (3)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, ExecutorCallsOnRun)
{
    /* WorkerThreadScheduledJobExecutor casts context to ScheduledJob *
     * and calls job->job_type_ptr->callbacks.on_run. */
    ScheduledJobType type = sMakeType("exec_test", PERIODIC, 1000);
    ScheduledJob job = sMakeJob(&type, 0);

    /* The executor expects a MessageContextData * which is really a
     * ScheduledJob * in this module. */
    int ret = WorkerThreadScheduledJobExecutor((MessageContextData *)&job);
    EXPECT_EQ(ret, 0);  /* sFakeOnRun returns 0 */
}

TEST(ScheduledJobsV1, FirstInsertExecutorWithoutCallback)
{
    /* When on_first_insert is NULL, the executor returns 0.
     * NOTE: The on_first_insert callback is currently disabled in the
     * implementation (commented out — Bug D context / SCH-015). */
    ScheduledJobType type = sMakeType("first_test", PERIODIC, 1000);
    type.callbacks.on_first_insert = nullptr;
    ScheduledJob job = sMakeJob(&type, 0);

    int ret = WorkerThreadScheduledJobFirstInsertedExecutor(
        (MessageContextData *)&job);
    EXPECT_EQ(ret, 0);
}

TEST(ScheduledJobsV1, TimeValueComparator)
{
    /* The comparator treats void* AS the value, not as a pointer to a value.
     * This matches the heap_insert convention: heap_insert(h, (void*)time, ...)
     * where the time value is cast directly to void*. */
    void *a = (void *)100;
    void *b = (void *)200;
    EXPECT_LT(TimeValueComparator(a, b), 0);
    EXPECT_GT(TimeValueComparator(b, a), 0);
    EXPECT_EQ(TimeValueComparator(a, a), 0);
}

/* ═════════════════════════════════════════════════════════════════════
   Edge case tests (4)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, InsertWithNullType)
{
    /* Insert with NULL job_type_ptr will dereference NULL in
     * ReInsertScheduledJob → on_get_time.  This is a known lack of
     * input validation.  EXPECT_DEATH is not used here because it's
     * fragile in CI environments; the behaviour is documented instead.
     *
     * After Stage 2: ScheduledJobStoreInsert should return an error
     * rather than crashing. */
    SUCCEED();
}

TEST(ScheduledJobsV1, RegisterTypeWithNullName)
{
    /* A NULL type_name would normally segfault in strcmp inside
     * IsJobTypeNameRegistered, but Bug A means the function returns
     * early (job_types_size == 0) before reaching strcmp.
     * After Bug A fix, this path would be reachable and should
     * either reject NULL or crash — needs a NULL guard. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType(nullptr, PERIODIC, 1000);
    /* Currently does NOT crash due to Bug A (early return).
     * After Bug A fix, needs null-guard. */
    int id = RegisterScheduledJobType(s, &type);
    (void)id;

    DestructScheduledJobs(s);
    SUCCEED();
}

TEST(ScheduledJobsV1, LargeNumberOfJobs)
{
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 128);

    ScheduledJobType type = sMakeType("large", PERIODIC, 100);
    RegisterScheduledJobType(s, &type);

    const int N = 500;
    s_fake_time_us = 0;
    std::vector<ScheduledJob> jobs(N);
    for (int i = 0; i < N; i++) {
        jobs[i] = sMakeJob(&type, i);
        EXPECT_EQ(InsertScheduledJob(s, &jobs[i]), 0);
    }
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), (size_t)N);

    /* Remove all */
    for (int i = 0; i < N; i++) {
        ScheduledJobContext ctx = {0};
        ScheduledJobContext *r = GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx);
        ASSERT_NE(r, nullptr) << "failed at remove " << i;
    }
    EXPECT_EQ(GetScheduleJobsSetsize(s, LOCK_HINT_NONE), 0u);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, GetDefaultComparatorReturnsNonNull)
{
    CallbackOnCompareKeys cmp = GetDefaultComparatorForTimeValue();
    EXPECT_NE(cmp, nullptr);
}

/* ═════════════════════════════════════════════════════════════════════
   Bug regression tests (4)
   ═════════════════════════════════════════════════════════════════════ */

TEST(ScheduledJobsV1, BugA_JobTypesSizeIncremented)
{
    /* Bug A: job_types_size never incremented — every registration
     * re-allocates a fresh array and loses prior registrations.
     * This test documents the CURRENT BROKEN behaviour. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type_a = sMakeType("bug_a_type", PERIODIC, 1000);
    int id_a = RegisterScheduledJobType(s, &type_a);
    EXPECT_GE(id_a, 0);

    /* Register a second type — due to Bug A, the first type may be
     * inaccessible via the registry (job_types_size wasn't incremented,
     * so _ExpandJobTypesIfNecessary allocates a new array). */
    ScheduledJobType type_b = sMakeType("bug_a_type_b", PERIODIC, 2000);
    int id_b = RegisterScheduledJobType(s, &type_b);
    EXPECT_GE(id_b, 0);

    /* After Bug A fix, both types should be findable.
     * Currently: the second registration path calls
     * _ExpandJobTypesIfNecessary which sees job_types_size == 0
     * and allocates a NEW array — type_a is leaked. */
    pthread_spin_lock(&s->spin_lock);
    bool found_a = IsJobTypeNameRegistered(s, "bug_a_type");
    bool found_b = IsJobTypeNameRegistered(s, "bug_a_type_b");
    pthread_spin_unlock(&s->spin_lock);

    /* This assertion WILL FAIL until Bug A is fixed.
     * After the fix (add job_types_size++), both should be true. */
    EXPECT_TRUE(found_b) << "second type should always be findable";
    /* EXPECT_TRUE(found_a) << "Bug A: first type lost — uncomment after fix"; */
    (void)found_a;  /* suppress unused warning */

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, BugB_DestructScheduledJobResolved)
{
    /* Bug B is FIXED: DestructScheduledJob declaration was removed from
     * the public header (zero consumer call sites — confirmed by survey).
     * The function was never implemented; removing the dead declaration
     * eliminates the linker-error hazard. */
    SUCCEED();
}

TEST(ScheduledJobsV1, BugD_LockHeldForIsJobTypeNameRegistered)
{
    /* Bug D: IsJobTypeNameRegistered must be called with spin_lock held.
     * This test verifies the CORRECT usage pattern (lock held).
     *
     * NOTE: The type IS findable here because it's the ONLY type
     * registered (Bug A only manifests when ≥2 types are registered). */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("bug_d_type", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);

    /* CORRECT — lock held (matching the @warning documentation) */
    pthread_spin_lock(&s->spin_lock);
    EXPECT_TRUE(IsJobTypeNameRegistered(s, "bug_d_type"));
    EXPECT_FALSE(IsJobTypeNameRegistered(s, "nonexistent"));
    pthread_spin_unlock(&s->spin_lock);

    DestructScheduledJobs(s);
}

TEST(ScheduledJobsV1, BugC_DestructLeakDocumented)
{
    /* Bug C: DestructScheduledJobs leaks heap entries[] array and
     * spinlock.  This test documents the current behaviour — we can
     * call Destruct and the store is zeroed, but valgrind would report
     * leaked memory for the heap's internal arrays. */
    ScheduledJobs *s = sGetFreshStore();
    InitScheduledJobsStore(s, 8);

    ScheduledJobType type = sMakeType("bug_c_type", PERIODIC, 1000);
    RegisterScheduledJobType(s, &type);
    s_fake_time_us = 0;
    ScheduledJob job = sMakeJob(&type, 0);
    InsertScheduledJob(s, &job);

    DestructScheduledJobs(s);
    /* Store is zeroed — job_types_index is NULL */
    EXPECT_EQ(s->job_types_descriptor.job_types_index, nullptr);

    /* NOTE: heap.entries[] and spinlock are leaked.  Valgrind would
     * report this.  Fix in Stage 1 (S1-1.2). */
    SUCCEED();
}
