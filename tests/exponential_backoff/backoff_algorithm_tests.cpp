/**
 * @file backoff_algorithm_tests.cpp
 * @brief Adversarial + contract test suite for src/exponential_backoff/algorithm.c.
 *
 * The module exposes two retry-delay strategies over a public
 * BackoffAlgorithmContext_t:
 *
 *   - BackoffAlgorithm_WithJitter  — "Full Jitter": delay = rv % (nextJitterMax+1),
 *     nextJitterMax doubles each attempt up to maxBackoffDelay.
 *   - BackoffAlgorithm_NoJitter    — deterministic quadratic: delay =
 *     clamp(base + floor(((attemptsDone+1)^2 - 1) / 2), max).  Its "user_value"
 *     argument is ignored.
 *   - BackoffAlgorithm_GetNextBackoff — dispatches to ctx->backoff_provider.
 *
 * The suite is deliberately hostile: it asserts the *documented* contract and
 * feeds boundary and out-of-contract inputs.  Three tests guard against defects
 * that were found and fixed in this module:
 *
 *   - DefaultInitDoesNotRecurse / DefaultInitDispatchesToWithJitter:
 *     BackoffAlgorithm_InitializeParams must wire backoff_provider to the
 *     WithJitter strategy, not the GetNextBackoff dispatcher (which would
 *     recurse until the stack overflows).
 *   - BaseGreaterThanMaxDoesNotExceedMax: with backOffBase > maxBackOff the
 *     first delay must still never exceed maxBackoffDelay.
 *   - NullContextIsRejectedByAssert: BackoffAlgorithm_GetNextBackoff must guard
 *     a NULL context with assert, like its sibling functions.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "gtest/gtest.h"

#include <cstdint>
#include <cstddef>
#include <cstring>

#include <functional>

#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

extern "C" {
#include "uflib/exponential_backoff/algorithm.h"
}

namespace {

// Independent integer oracle for BackoffAlgorithm_NoJitter.
//
// The implementation computes, in double:
//     min(base + (pow(attemptsDone+1, 2) - 1) / 2, max), truncated to uint16_t.
// For any n = attemptsDone + 1 with n >= 1, floor((n^2 - 1) / 2) equals the
// integer expression (n*n - 1) / 2, and the final truncation matches an integer
// floor, so the pure-integer reference below is exact.  n == 0 is the uint32
// wrap (attemptsDone == UINT32_MAX) and is handled by a dedicated test.
static uint16_t
NoJitterRef(uint16_t base, uint16_t max, uint32_t attempts_done)
{
  const uint32_t n = attempts_done + 1u;           /* uint32 wrap */
  if (n == 0) {
    return 0;                                       /* unreachable here */
  }
  const uint64_t sq   = static_cast<uint64_t>(n) * static_cast<uint64_t>(n);
  const uint64_t term = (sq - 1u) / 2u;
  const uint64_t sum  = static_cast<uint64_t>(base) + term;
  return static_cast<uint16_t>(sum < static_cast<uint64_t>(max)
                               ? sum
                               : static_cast<uint64_t>(max));
}

// Run `fn` in a forked child that self-destructs via alarm() after
// `timeout_sec`.  Lets hostile inputs (infinite recursion, NULL deref) be
// probed without crashing the whole suite.
struct ChildOutcome {
  bool exited;
  int  exit_code;
  bool signaled;
  int  sig;
};

ChildOutcome
RunInChild(const std::function<void()> &fn, unsigned timeout_sec = 1)
{
  const pid_t pid = fork();
  if (pid < 0) {
    return ChildOutcome{false, -1, false, 0};
  }
  if (pid == 0) {
    alarm(timeout_sec);
    fn();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  ChildOutcome o{false, 0, false, 0};
  if (WIFEXITED(status)) { o.exited = true; o.exit_code = WEXITSTATUS(status); }
  else if (WIFSIGNALED(status)) { o.signaled = true; o.sig = WTERMSIG(status); }
  return o;
}

}  // namespace

// Dispatch-recording provider — C linkage so its type matches the
// backoff_strategy_provider typedef exactly.
extern "C" {
static void *                    g_ctx;
static uint32_t                  g_rv;
static uint16_t *                g_out;
static BackoffAlgorithmStatus_t
RecordProvider(void *ctx, uint32_t rv, uint16_t *out)
{
  g_ctx = ctx;
  g_rv  = rv;
  g_out = out;
  *out  = 42;
  return BackoffAlgorithmSuccess;
}
}

// BackoffAlgorithm_InitializeParams — field initialisation.

TEST(BackoffAlgorithmInit, SetsAllFields)
{
  BackoffAlgorithmContext_t ctx;
  memset(&ctx, 0xA5, sizeof(ctx));

  BackoffAlgorithm_InitializeParams(&ctx, 7, 500, 11);

  EXPECT_EQ(ctx.nextJitterMax, 7u);
  EXPECT_EQ(ctx.maxBackoffDelay, 500u);
  EXPECT_EQ(ctx.maxRetryAttempts, 11u);
  EXPECT_EQ(ctx.attemptsDone, 0u);
}

TEST(BackoffAlgorithmInit, ResetAttemptsZeroesCounter)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 10, 100, 5);

  ctx.attemptsDone = 12345;
  BackoffAlgorithm_ResetAttempts(&ctx);
  EXPECT_EQ(ctx.attemptsDone, 0u);

  // Other fields must be untouched by a reset.
  EXPECT_EQ(ctx.nextJitterMax, 10u);
  EXPECT_EQ(ctx.maxBackoffDelay, 100u);
  EXPECT_EQ(ctx.maxRetryAttempts, 5u);
}

// BackoffAlgorithm_GetNextBackoff — provider dispatch.

TEST(BackoffAlgorithmGetNextBackoff, DispatchesToProvider)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsWithProvider(&ctx, 10, 100, 5, RecordProvider);

  g_ctx = nullptr; g_rv = 0; g_out = nullptr;
  uint16_t out = 0;
  const BackoffAlgorithmStatus_t st = BackoffAlgorithm_GetNextBackoff(&ctx, 12345u, &out);

  EXPECT_EQ(st, BackoffAlgorithmSuccess);
  EXPECT_EQ(g_ctx, static_cast<void *>(&ctx));
  EXPECT_EQ(g_rv, 12345u);
  EXPECT_EQ(g_out, &out);
  EXPECT_EQ(out, 42u);
}

TEST(BackoffAlgorithmGetNextBackoff, DefaultInitDispatchesToWithJitter)
{
  // The default provider (after a plain InitializeParams) must be the full-jitter
  // strategy, so GetNextBackoff yields the same value as calling WithJitter
  // directly.  Guards against re-wiring the provider to the dispatcher.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 10, 100, 5);

  uint16_t out = 0;
  EXPECT_EQ(BackoffAlgorithm_GetNextBackoff(&ctx, 0x12345678u, &out), BackoffAlgorithmSuccess);
  EXPECT_EQ(out, 0x12345678u % 11u);   // nextJitterMax == base == 10 -> rv % (10 + 1)
  EXPECT_EQ(ctx.attemptsDone, 1u);
  EXPECT_EQ(ctx.nextJitterMax, 20u);   // doubled once (10 < 100/2)
}

// BackoffAlgorithm_WithJitter — full-jitter strategy contract.

TEST(BackoffAlgorithmWithJitter, ExhaustsAfterMaxAttempts)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 10, 100, 5);

  for (uint32_t i = 0; i < 5; ++i) {
    uint16_t out = 0;
    EXPECT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0x12345678u, &out), BackoffAlgorithmSuccess)
        << "attempt " << i;
    EXPECT_EQ(ctx.attemptsDone, i + 1);
  }
  EXPECT_EQ(ctx.attemptsDone, 5u);

  uint16_t out = 0xFFFF;
  EXPECT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0x12345678u, &out),
            BackoffAlgorithmRetriesExhausted);
  EXPECT_EQ(ctx.attemptsDone, 5u);   // not advanced past the cap
}

TEST(BackoffAlgorithmWithJitter, BackoffInRangeForBaseLeMax)
{
  // Invariant: when backOffBase <= maxBackOff, every delay is in [0, maxBackOff].
  const uint16_t bases[] = {0, 1, 2, 10, 100, 1000, 30000, 65535};
  const uint32_t rvs[]   = {0u, 1u, 2u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x55555555u};

  for (const uint16_t base : bases) {
    for (const uint16_t max : bases) {
      if (base > max) continue;
      BackoffAlgorithmContext_t ctx;
      BackoffAlgorithm_InitializeParams(&ctx, base, max, BACKOFF_ALGORITHM_RETRY_FOREVER);
      for (const uint32_t rv : rvs) {
        uint16_t out = 0;
        ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, rv, &out), BackoffAlgorithmSuccess);
        EXPECT_LE(out, max) << "base=" << base << " max=" << max << " rv=" << rv;
      }
    }
  }
}

TEST(BackoffAlgorithmWithJitter, DoublesJitterMaxThenClamps)
{
  // base=1, max=16: nextJitterMax traces 1 -> 2 -> 4 -> 8 -> 16 -> 16.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 1, 16, BACKOFF_ALGORITHM_RETRY_FOREVER);

  const uint16_t expected[] = {2, 4, 8, 16, 16};
  for (uint16_t want : expected) {
    uint16_t out = 0;
    ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0u, &out), BackoffAlgorithmSuccess);
    EXPECT_EQ(out, 0u) << "rv=0 forces delay 0";
    EXPECT_EQ(ctx.nextJitterMax, want);
  }
}

TEST(BackoffAlgorithmWithJitter, ZeroBaseAlwaysZero)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 0, 100, 5);

  for (int i = 0; i < 5; ++i) {
    uint16_t out = 0xFFFF;
    ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0xFFFFFFFFu, &out), BackoffAlgorithmSuccess);
    EXPECT_EQ(out, 0u);          // rv % (0 + 1) == 0
    EXPECT_EQ(ctx.nextJitterMax, 0u);   // 0 doubles to 0
  }
}

TEST(BackoffAlgorithmWithJitter, MaxZeroClampsBaseToZero)
{
  // maxBackOff=0 clamps the base to 0 at init, so every delay is 0.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 10, 0, BACKOFF_ALGORITHM_RETRY_FOREVER);

  EXPECT_EQ(ctx.nextJitterMax, 0u);   // base clamped to max at init

  uint16_t out = 0xFFFF;
  ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0xFFFFFFFFu, &out), BackoffAlgorithmSuccess);
  EXPECT_EQ(out, 0u);                 // rv % (0 + 1) == 0
  EXPECT_EQ(ctx.nextJitterMax, 0u);
}

TEST(BackoffAlgorithmWithJitter, Uint16MaxNoOverflow)
{
  // nextJitterMax + 1 must be computed in uint32: 65535 + 1 == 65536, no wrap.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 65535, 65535, 1);

  uint16_t out = 0;
  ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0xFFFFFFFFu, &out), BackoffAlgorithmSuccess);
  EXPECT_LE(out, 65535u);
}

TEST(BackoffAlgorithmWithJitter, RetryForeverNeverExhausts)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 10, 100, BACKOFF_ALGORITHM_RETRY_FOREVER);

  for (uint32_t i = 0; i < 10000; ++i) {
    uint16_t out = 0;
    ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0xDEADBEEFu, &out), BackoffAlgorithmSuccess);
    EXPECT_LE(out, 100u);
  }
  EXPECT_EQ(ctx.attemptsDone, 10000u);
}

TEST(BackoffAlgorithmWithJitter, RandomValueBoundaries)
{
  // base=1, max=1: delay = rv % 2.
  const struct { uint32_t rv; uint16_t want; } cases[] = {
    {0u,          0},
    {1u,          1},
    {2u,          0},
    {0xFFFFFFFFu, 1},   // odd -> 1
  };
  for (const auto &c : cases) {
    BackoffAlgorithmContext_t ctx;
    BackoffAlgorithm_InitializeParams(&ctx, 1, 1, 1);
    uint16_t out = 0xFFFF;
    ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, c.rv, &out), BackoffAlgorithmSuccess);
    EXPECT_EQ(out, c.want) << "rv=" << c.rv;
  }
}

// BackoffAlgorithm_NoJitter — deterministic quadratic strategy.

TEST(BackoffAlgorithmNoJitter, MatchesIntegerOracle)
{
  const uint16_t bases[] = {0, 1, 5, 10, 100, 1000, 65535};
  const uint16_t maxes[] = {0, 1, 5, 10, 100, 1000, 65535};
  const uint32_t attempts[] = {0, 1, 2, 3, 7, 30, 1000, 100000, 1000000000u,
                               UINT32_MAX - 1u};

  for (const uint16_t base : bases) {
    for (const uint16_t max : maxes) {
      for (const uint32_t done : attempts) {
        if (done == UINT32_MAX) continue;   // wrap handled elsewhere
        BackoffAlgorithmContext_t ctx;
        BackoffAlgorithm_InitializeParams(&ctx, base, max, BACKOFF_ALGORITHM_RETRY_FOREVER);
        ctx.attemptsDone = done;

        uint16_t out = 0xFFFF;
        const BackoffAlgorithmStatus_t st = BackoffAlgorithm_NoJitter(&ctx, 0u, &out);
        ASSERT_EQ(st, BackoffAlgorithmSuccess);
        EXPECT_EQ(out, NoJitterRef(base, max, done))
            << "base=" << base << " max=" << max << " done=" << done;
      }
    }
  }
}

TEST(BackoffAlgorithmNoJitter, DeterministicIgnoresRandomValue)
{
  BackoffAlgorithmContext_t a, b;
  BackoffAlgorithm_InitializeParamsWithoutJitter(&a, 10, 100, 5);
  BackoffAlgorithm_InitializeParamsWithoutJitter(&b, 10, 100, 5);

  uint16_t oa = 0, ob = 0;
  // The user_value/randomValue argument must have no effect on the result.
  ASSERT_EQ(BackoffAlgorithm_NoJitter(&a, 0u, &oa), BackoffAlgorithmSuccess);
  ASSERT_EQ(BackoffAlgorithm_NoJitter(&b, 0xFFFFFFFFu, &ob), BackoffAlgorithmSuccess);
  EXPECT_EQ(oa, ob);
  EXPECT_EQ(oa, NoJitterRef(10, 100, 0));
}

TEST(BackoffAlgorithmNoJitter, MonotonicNonDecreasing)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsWithoutJitter(&ctx, 5, 1000, BACKOFF_ALGORITHM_RETRY_FOREVER);

  uint16_t prev = 0;
  for (uint32_t i = 0; i < 100; ++i) {
    uint16_t out = 0;
    ASSERT_EQ(BackoffAlgorithm_NoJitter(&ctx, 0u, &out), BackoffAlgorithmSuccess);
    EXPECT_GE(out, prev) << "attempt " << i;
    prev = out;
  }
}

TEST(BackoffAlgorithmNoJitter, ClampsToMax)
{
  // With a small max, the quadratic term saturates to exactly maxBackoffDelay.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsWithoutJitter(&ctx, 10, 50, BACKOFF_ALGORITHM_RETRY_FOREVER);

  ctx.attemptsDone = 1000;
  uint16_t out = 0;
  ASSERT_EQ(BackoffAlgorithm_NoJitter(&ctx, 0u, &out), BackoffAlgorithmSuccess);
  EXPECT_EQ(out, 50u);
}

TEST(BackoffAlgorithmNoJitter, ExhaustsAfterMaxAttempts)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsWithoutJitter(&ctx, 10, 100, 4);

  for (uint32_t i = 0; i < 4; ++i) {
    uint16_t out = 0;
    EXPECT_EQ(BackoffAlgorithm_NoJitter(&ctx, 0u, &out), BackoffAlgorithmSuccess);
    EXPECT_EQ(ctx.attemptsDone, i + 1);
  }
  uint16_t out = 0xFFFF;
  EXPECT_EQ(BackoffAlgorithm_NoJitter(&ctx, 0u, &out), BackoffAlgorithmRetriesExhausted);
  EXPECT_EQ(ctx.attemptsDone, 4u);
}

TEST(BackoffAlgorithmNoJitter, AttemptsDoneWrapAtUint32Max)
{
  // Documented edge: at attemptsDone == UINT32_MAX, `attemptsDone + 1` wraps to
  // 0 in uint32, so the quadratic term becomes (pow(0,2)-1)/2 == -0.5 and the
  // delay falls back to base-1 instead of saturating at max.  attemptsDone then
  // rolls over to 0.  This is a latent defect only reachable after 2^32-1 calls.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 5, 100, BACKOFF_ALGORITHM_RETRY_FOREVER);
  ctx.attemptsDone = UINT32_MAX;

  uint16_t out = 0xFFFF;
  ASSERT_EQ(BackoffAlgorithm_NoJitter(&ctx, 0u, &out), BackoffAlgorithmSuccess);
  EXPECT_EQ(out, 4u);            // base(5) - 0.5 truncated -> 4
  EXPECT_EQ(ctx.attemptsDone, 0u);   // wrapped
}

TEST(BackoffAlgorithmNoJitter, WithoutJitterInitializesProvider)
{
  // The "WithoutJitter" public path must make GetNextBackoff dispatch to the
  // deterministic strategy (not the broken default dispatcher).
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsWithoutJitter(&ctx, 10, 100, 5);

  uint16_t out = 0xFFFF;
  ASSERT_EQ(BackoffAlgorithm_GetNextBackoff(&ctx, 0u, &out), BackoffAlgorithmSuccess);
  EXPECT_EQ(out, NoJitterRef(10, 100, 0));   // first NoJitter value == base
}

// BackoffAlgorithm_ExponentialBackoff — deterministic exponential strategy.

static uint16_t
ExponentialRef(uint16_t base, uint16_t max, uint32_t attempts_done)
{
  const uint16_t effective_base = (base < max) ? base : max;  // init clamps nextJitterMax
  uint64_t delay = effective_base;
  for (uint32_t i = 0; i < attempts_done && delay < max; ++i) {
    delay *= 2u;
    if (delay > max) delay = max;
  }
  return static_cast<uint16_t>(delay);
}

TEST(BackoffAlgorithmExponentialBackoff, MatchesExponentialOracle)
{
  const uint16_t bases[] = {0, 1, 5, 10, 100, 1000, 65535};
  const uint16_t maxes[] = {0, 1, 10, 100, 1000, 10000, 65535};
  const uint32_t attempts[] = {0, 1, 2, 3, 4, 5, 10, 20, 100, 1000000u};

  for (const uint16_t base : bases) {
    for (const uint16_t max : maxes) {
      for (const uint32_t done : attempts) {
        BackoffAlgorithmContext_t ctx;
        BackoffAlgorithm_InitializeParamsExponential(&ctx, base, max, BACKOFF_ALGORITHM_RETRY_FOREVER);
        ctx.attemptsDone = done;

        uint16_t out = 0xFFFF;
        ASSERT_EQ(BackoffAlgorithm_ExponentialBackoff(&ctx, 0u, &out), BackoffAlgorithmSuccess);
        EXPECT_EQ(out, ExponentialRef(base, max, done))
            << "base=" << base << " max=" << max << " done=" << done;
      }
    }
  }
}

TEST(BackoffAlgorithmExponentialBackoff, KnownSequence)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsExponential(&ctx, 1000, 10000, BACKOFF_ALGORITHM_RETRY_FOREVER);

  const uint16_t expected[] = {1000, 2000, 4000, 8000, 10000, 10000};
  for (const uint16_t want : expected) {
    uint16_t out = 0;
    ASSERT_EQ(BackoffAlgorithm_ExponentialBackoff(&ctx, 0u, &out), BackoffAlgorithmSuccess);
    EXPECT_EQ(out, want);
  }
}

TEST(BackoffAlgorithmExponentialBackoff, DeterministicIgnoresRandomValue)
{
  BackoffAlgorithmContext_t a, b;
  BackoffAlgorithm_InitializeParamsExponential(&a, 1000, 10000, 5);
  BackoffAlgorithm_InitializeParamsExponential(&b, 1000, 10000, 5);

  uint16_t oa = 0, ob = 0;
  ASSERT_EQ(BackoffAlgorithm_ExponentialBackoff(&a, 0u, &oa), BackoffAlgorithmSuccess);
  ASSERT_EQ(BackoffAlgorithm_ExponentialBackoff(&b, 0xFFFFFFFFu, &ob), BackoffAlgorithmSuccess);
  EXPECT_EQ(oa, ob);
  EXPECT_EQ(oa, 1000u);
}

TEST(BackoffAlgorithmExponentialBackoff, ExhaustsAfterMaxAttempts)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsExponential(&ctx, 1000, 10000, 4);

  for (uint32_t i = 0; i < 4; ++i) {
    uint16_t out = 0;
    EXPECT_EQ(BackoffAlgorithm_ExponentialBackoff(&ctx, 0u, &out), BackoffAlgorithmSuccess);
    EXPECT_EQ(ctx.attemptsDone, i + 1);
  }
  uint16_t out = 0xFFFF;
  EXPECT_EQ(BackoffAlgorithm_ExponentialBackoff(&ctx, 0u, &out), BackoffAlgorithmRetriesExhausted);
  EXPECT_EQ(ctx.attemptsDone, 4u);
}

TEST(BackoffAlgorithmExponentialBackoff, InitializesExponentialProvider)
{
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParamsExponential(&ctx, 1000, 10000, 5);

  uint16_t out = 0xFFFF;
  ASSERT_EQ(BackoffAlgorithm_GetNextBackoff(&ctx, 0u, &out), BackoffAlgorithmSuccess);
  EXPECT_EQ(out, 1000u);   // first value == base
}

// Regression guards — these tests were failing against the original source and
// now lock in the fixed behaviour.

TEST(BackoffAlgorithmGetNextBackoff, DefaultInitDoesNotRecurse)
{
  // BackoffAlgorithm_InitializeParams wires backoff_provider to the dispatcher
  // BackoffAlgorithm_GetNextBackoff itself, so the first call recurses forever
  // and overflows the stack.  The default provider should be
  // BackoffAlgorithm_WithJitter.  The child is expected to return normally;
  // against the current source it dies with SIGSEGV (stack overflow).
  const ChildOutcome o = RunInChild([] {
    BackoffAlgorithmContext_t ctx;
    BackoffAlgorithm_InitializeParams(&ctx, 10, 100, 5);
    uint16_t out = 0;
    BackoffAlgorithm_GetNextBackoff(&ctx, 1u, &out);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "default-initialized GetNextBackoff recursed/crashed "
      << "(exited=" << o.exited << ", signaled=" << o.signaled
      << ", sig=" << o.sig << ", exit_code=" << o.exit_code << ")";
}

TEST(BackoffAlgorithmWithJitter, BaseGreaterThanMaxDoesNotExceedMax)
{
  // Header contract: "The value does not exceed the maximum backoff delay
  // configured in the context."  With base(100) > max(50), the first delay is
  // rv % (base+1) which reaches 67 > 50, so the contract is violated before the
  // jitter ceiling is clamped down on the next attempt.
  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 100, 50, 5);

  uint16_t out = 0;
  ASSERT_EQ(BackoffAlgorithm_WithJitter(&ctx, 0xFFFFFFFFu, &out), BackoffAlgorithmSuccess);
  EXPECT_LE(out, 50u) << "delay " << out << " exceeds maxBackoffDelay 50";
}

TEST(BackoffAlgorithmGetNextBackoff, NullContextIsRejectedByAssert)
{
  // NULL is a precondition violation.  GetNextBackoff must guard it with assert
  // (like WithJitter / InitializeParams) rather than dereference NULL and die
  // with SIGSEGV.  In this Debug build the assert aborts the child with SIGABRT.
  const ChildOutcome o = RunInChild([] {
    uint16_t out = 0;
    BackoffAlgorithm_GetNextBackoff(nullptr, 1u, &out);
  });
  EXPECT_TRUE(o.signaled && o.sig == SIGABRT)
      << "expected assert-abort (SIGABRT) on NULL context, got "
      << "(exited=" << o.exited << ", signaled=" << o.signaled
      << ", sig=" << o.sig << ", exit_code=" << o.exit_code << ")";
}
