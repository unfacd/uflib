/**
 * @file backoff_algorithm_stress.c
 * @brief High-iteration property stress for src/exponential_backoff/algorithm.c.
 *
 * Standalone (build only, not registered with CTest) stress harness.  It runs a
 * config sweep through the two strategies and asserts the *documented* contract
 * on every call, exiting nonzero on the first violation:
 *
 *   - WithJitter: every delay in [0, maxBackOff] (for base <= max), attemptsDone
 *     advances by exactly one per success, and the (maxAttempts+1)-th call
 *     reports RetriesExhausted.
 *   - NoJitter: deterministic — must match an independent integer oracle,
 *     monotonic non-decreasing, clamped to max, and indifferent to the
 *     randomValue argument.
 *   - RETRY_FOREVER: never exhausts and always stays in range.
 *
 * The oracle is integer-only (floor((n^2-1)/2)) so it does not share the
 * implementation's floating-point path.
 *
 * Usage:
 *   backoff_algorithm_stress [--iterations N] [--threads T] [--seed S]
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include <uflib/exponential_backoff/algorithm.h>

/* ---------------------------------------------------------------------------
 * Deterministic splitmix64 PRNG (per-thread state, reproducible from --seed).
 * ------------------------------------------------------------------------- */

static uint64_t
splitmix64_next(uint64_t *state)
{
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* ---------------------------------------------------------------------------
 * Integer oracle for BackoffAlgorithm_NoJitter.
 * ------------------------------------------------------------------------- */

static uint16_t
nojitter_ref(uint16_t base, uint16_t max, uint32_t attempts_done)
{
  const uint32_t n = attempts_done + 1u;   /* uint32 wrap */
  if (n == 0) {
    return 0;   /* unreachable in the sweep (would require 2^32-1 calls) */
  }
  const uint64_t sq   = (uint64_t)n * (uint64_t)n;
  const uint64_t term = (sq - 1u) / 2u;
  const uint64_t sum  = (uint64_t)base + term;
  return (uint16_t)((sum < (uint64_t)max) ? sum : (uint64_t)max);
}

/* Integer oracle for BackoffAlgorithm_ExponentialBackoff. */
static uint16_t
exponential_ref(uint16_t base, uint16_t max, uint32_t attempts_done)
{
  const uint16_t effective_base = (base < max) ? base : max;   /* init clamps nextJitterMax */
  uint64_t delay = effective_base;
  for (uint32_t i = 0; i < attempts_done && delay < max; ++i) {
    delay *= 2u;
    if (delay > max) delay = max;
  }
  return (uint16_t)delay;
}

/* ---------------------------------------------------------------------------
 * Sweeps.  Each returns the number of contract violations seen.
 * ------------------------------------------------------------------------- */

static uint64_t
sweep_withjitter(uint64_t iters, uint64_t *rng)
{
  uint64_t fails = 0;

  for (uint64_t k = 0; k < iters; ++k) {
    const uint16_t max      = (uint16_t)(splitmix64_next(rng) & 0xFFFFu);
    const uint16_t base     = (uint16_t)(splitmix64_next(rng) % ((uint32_t)max + 1u));
    const uint32_t attempts = (uint32_t)(splitmix64_next(rng) % 64u) + 1u;

    BackoffAlgorithmContext_t ctx;
    BackoffAlgorithm_InitializeParams(&ctx, base, max, attempts);

    for (uint32_t i = 0; i < attempts; ++i) {
      const uint32_t rv = (uint32_t)splitmix64_next(rng);
      const uint16_t nmax_before = ctx.nextJitterMax;
      uint16_t out = 0;

      if (BackoffAlgorithm_WithJitter(&ctx, rv, &out) != BackoffAlgorithmSuccess) { ++fails; break; }
      if (out > nmax_before) ++fails;
      if (out > max)         ++fails;
      if (ctx.attemptsDone != i + 1u) ++fails;
    }

    uint16_t out = 0;
    if (BackoffAlgorithm_WithJitter(&ctx, 0u, &out) != BackoffAlgorithmRetriesExhausted) ++fails;
    if (ctx.attemptsDone != attempts) ++fails;
  }

  return fails;
}

static uint64_t
sweep_nojitter(uint64_t iters, uint64_t *rng)
{
  uint64_t fails = 0;

  for (uint64_t k = 0; k < iters; ++k) {
    const uint16_t max      = (uint16_t)(splitmix64_next(rng) & 0xFFFFu);
    const uint16_t base     = (uint16_t)(splitmix64_next(rng) % ((uint32_t)max + 1u));
    const uint32_t attempts = (uint32_t)(splitmix64_next(rng) % 64u) + 1u;

    BackoffAlgorithmContext_t ctx;
    BackoffAlgorithm_InitializeParamsWithoutJitter(&ctx, base, max, attempts);

    uint16_t prev = 0;
    for (uint32_t i = 0; i < attempts; ++i) {
      uint16_t out = 0;
      const BackoffAlgorithmStatus_t st =
          BackoffAlgorithm_NoJitter(&ctx, (uint32_t)splitmix64_next(rng), &out);
      if (st != BackoffAlgorithmSuccess) { ++fails; break; }
      if (out != nojitter_ref(base, max, i)) ++fails;
      if (out > max)   ++fails;
      if (out < prev)  ++fails;   /* monotonic non-decreasing */
      prev = out;
    }

    uint16_t out = 0;
    if (BackoffAlgorithm_NoJitter(&ctx, 0u, &out) != BackoffAlgorithmRetriesExhausted) ++fails;
  }

  return fails;
}

static uint64_t
sweep_exponential(uint64_t iters, uint64_t *rng)
{
  uint64_t fails = 0;

  for (uint64_t k = 0; k < iters; ++k) {
    const uint16_t max      = (uint16_t)(splitmix64_next(rng) & 0xFFFFu);
    const uint16_t base     = (uint16_t)(splitmix64_next(rng) % ((uint32_t)max + 1u));
    const uint32_t attempts = (uint32_t)(splitmix64_next(rng) % 64u) + 1u;

    BackoffAlgorithmContext_t ctx;
    BackoffAlgorithm_InitializeParamsExponential(&ctx, base, max, attempts);

    uint16_t prev = 0;
    for (uint32_t i = 0; i < attempts; ++i) {
      uint16_t out = 0;
      const BackoffAlgorithmStatus_t st =
          BackoffAlgorithm_ExponentialBackoff(&ctx, (uint32_t)splitmix64_next(rng), &out);
      if (st != BackoffAlgorithmSuccess) { ++fails; break; }
      if (out != exponential_ref(base, max, i)) ++fails;
      if (out > max)   ++fails;
      if (out < prev)  ++fails;   /* monotonic non-decreasing */
      prev = out;
    }

    uint16_t out = 0;
    if (BackoffAlgorithm_ExponentialBackoff(&ctx, 0u, &out) != BackoffAlgorithmRetriesExhausted) ++fails;
  }

  return fails;
}

static uint64_t
sweep_determinism(uint64_t iters, uint64_t *rng)
{
  uint64_t fails = 0;

  for (uint64_t k = 0; k < iters; ++k) {
    const uint16_t base = (uint16_t)(splitmix64_next(rng) & 0xFFFFu);
    const uint16_t max  = (uint16_t)(splitmix64_next(rng) & 0xFFFFu);

    BackoffAlgorithmContext_t a, b;
    BackoffAlgorithm_InitializeParamsWithoutJitter(&a, base, max, BACKOFF_ALGORITHM_RETRY_FOREVER);
    BackoffAlgorithm_InitializeParamsWithoutJitter(&b, base, max, BACKOFF_ALGORITHM_RETRY_FOREVER);

    uint16_t oa = 0, ob = 0;
    if (BackoffAlgorithm_NoJitter(&a, 0u, &oa) != BackoffAlgorithmSuccess) { ++fails; continue; }
    if (BackoffAlgorithm_NoJitter(&b, 0xFFFFFFFFu, &ob) != BackoffAlgorithmSuccess) { ++fails; continue; }
    if (oa != ob) ++fails;   /* randomValue argument must be ignored */
  }

  return fails;
}

static uint64_t
sweep_forever(uint64_t iters, uint64_t *rng)
{
  uint64_t fails = 0;

  BackoffAlgorithmContext_t ctx;
  BackoffAlgorithm_InitializeParams(&ctx, 16, 1024, BACKOFF_ALGORITHM_RETRY_FOREVER);

  for (uint64_t k = 0; k < iters; ++k) {
    const uint32_t rv = (uint32_t)splitmix64_next(rng);
    uint16_t out = 0;
    if (BackoffAlgorithm_WithJitter(&ctx, rv, &out) != BackoffAlgorithmSuccess) ++fails;
    if (out > 1024u) ++fails;
  }

  return fails;
}

/* ---------------------------------------------------------------------------
 * Thread entry point.
 * ------------------------------------------------------------------------- */

typedef struct {
  uint64_t iterations;
  uint64_t seed;
  uint64_t failures;
} stress_thread_args;

static void *
stress_thread_main(void *arg)
{
  stress_thread_args *a = (stress_thread_args *)arg;
  uint64_t rng = a->seed;

  a->failures = 0;
  a->failures += sweep_withjitter(a->iterations, &rng);
  a->failures += sweep_nojitter(a->iterations, &rng);
  a->failures += sweep_exponential(a->iterations, &rng);
  a->failures += sweep_determinism(a->iterations / 10u + 1u, &rng);
  a->failures += sweep_forever(100000u, &rng);
  return NULL;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  uint64_t iterations = 100000u;
  uint64_t threads    = 4u;
  uint64_t seed       = 0x123456789ABCDEF0ULL;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
      iterations = strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      threads = strtoull(argv[++i], NULL, 10);
      if (threads == 0) threads = 1;
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = strtoull(argv[++i], NULL, 0);
    }
  }

  stress_thread_args *args = calloc((size_t)threads, sizeof(*args));
  pthread_t *tids          = calloc((size_t)threads, sizeof(*tids));
  if (args == NULL || tids == NULL) {
    fprintf(stderr, "out of memory\n");
    free(args); free(tids);
    return 1;
  }

  for (uint64_t t = 0; t < threads; ++t) {
    args[t].iterations = iterations;
    args[t].seed       = seed + t * 0x9E3779B97F4A7C15ULL;
    if (pthread_create(&tids[t], NULL, stress_thread_main, &args[t]) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      free(args); free(tids);
      return 1;
    }
  }

  uint64_t total_fails = 0;
  for (uint64_t t = 0; t < threads; ++t) {
    pthread_join(tids[t], NULL);
    total_fails += args[t].failures;
  }

  printf("backoff_algorithm_stress: threads=%llu iterations=%llu seed=0x%llx "
         "contract_violations=%llu\n",
         (unsigned long long)threads, (unsigned long long)iterations,
         (unsigned long long)seed, (unsigned long long)total_fails);

  free(args);
  free(tids);
  return total_fails == 0 ? 0 : 1;
}
