#ifndef UFSRV_ALGORITHM_H
#define UFSRV_ALGORITHM_H

/*
* * Local fork of https://github.com/FreeRTOS/backoffAlgorithm as of 17/11/2022.
 * The MIT notice above covers the original Full-Jitter algorithm
 * (BackoffAlgorithm_WithJitter);
 * All later additions are Copyright (C) 2015-2026 unfacd works.
 *
 * backoffAlgorithm v1.3.0
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *

 */

/**
 * @file algorithm.h
 * @brief Exponential-backoff retry-delay strategies (local fork of FreeRTOS backoffAlgorithm).
 *
 * Computes backoff delays for retry attempts.  This header is a local fork of
 * Amazon's backoffAlgorithm v1.3.0 (SPDX MIT) that has diverged significantly
 * from upstream:
 *
 *   - BackoffAlgorithm_GetNextBackoff is now a dispatcher over a pluggable
 *     strategy provider, rather than the Full-Jitter algorithm directly.
 *   - Two deterministic strategies were added alongside the original:
 *       * BackoffAlgorithm_WithJitter         — random value within an
 *         exponentially growing ceiling (original upstream behaviour).
 *       * BackoffAlgorithm_NoJitter           — deterministic quadratic growth.
 *       * BackoffAlgorithm_ExponentialBackoff — deterministic base * 2^attemptsDone.
 *   - BackoffAlgorithm_InitializeParams* variants select the strategy, and
 *     BackoffAlgorithm_ResetAttempts resets the retry counter.
 *
 * All strategies are stateless and reentrant: concurrent calls on independent
 * contexts are safe.  Every public function is annotated with PUBLIC_API.
 *
 * <b>Implementation guide</b> — wire the API and choose a strategy:
 *
 *  1. Allocate a BackoffAlgorithmContext_t.
 *  2. Initialise it with one of the BackoffAlgorithm_InitializeParams* variants
 *     to select the strategy.
 *  3. On each retry, call BackoffAlgorithm_GetNextBackoff, wait for the delay,
 *     then retry.  Stop once it returns BackoffAlgorithmRetriesExhausted.
 *
 * Strategy choice:
 *   - Full Jitter (default)                -> BackoffAlgorithm_InitializeParams
 *   - NoJitter (deterministic quadratic)   -> BackoffAlgorithm_InitializeParamsWithoutJitter
 *   - Exponential (deterministic doubling) -> BackoffAlgorithm_InitializeParamsExponential
 *   - Custom provider                      -> BackoffAlgorithm_InitializeParamsWithProvider
 *   - Retry forever                        -> pass BACKOFF_ALGORITHM_RETRY_FOREVER as maxAttempts
 *   - Restart a retry cycle                -> BackoffAlgorithm_ResetAttempts
 *
 * @code{.c}
 * #include <uflib/exponential_backoff/algorithm.h>
 *
 * BackoffAlgorithmContext_t ctx;
 * BackoffAlgorithm_InitializeParamsExponential(&ctx, 1000, 10000, 5);
 *
 * while (operation_needs_retry()) {
 *     uint16_t delay = 0;
 *     if (BackoffAlgorithm_GetNextBackoff(&ctx, 0, &delay)
 *             == BackoffAlgorithmRetriesExhausted) {
 *         break;  // give up
 *     }
 *     // wait `delay` milliseconds, then retry
 * }
 * @endcode
 */

#include <uflib/uflib_defs.h>

#include <stdint.h>

/**
 * @brief Constant representing an unlimited number of retry attempts.
 */
#define BACKOFF_ALGORITHM_RETRY_FOREVER    ( UINT32_MAX )

/**
 * @brief Status returned by BackoffAlgorithm_GetNextBackoff.
 */
typedef enum BackoffAlgorithmStatus
{
    BackoffAlgorithmSuccess = 0,     ///< The next back-off value was computed.
    BackoffAlgorithmRetriesExhausted ///< All retry attempts have been exhausted.
} BackoffAlgorithmStatus_t;

/**
 * @brief A pluggable backoff-strategy provider.
 *
 * Computes the next backoff delay, advancing the retry state.  Deterministic
 * strategies ignore @p value; the Full-Jitter strategy uses it as a random
 * source.
 *
 * @param[in,out] context       The BackoffAlgorithmContext_t to advance.
 * @param[in]     value         Strategy-dependent input (random value, or unused).
 * @param[out]    next_backoff  Populated with the delay (milliseconds).
 *
 * @return BackoffAlgorithmStatus_t.
 */
typedef BackoffAlgorithmStatus_t (*backoff_strategy_provider)(void *context, uint32_t value, uint16_t *next_backoff);

/**
 * @brief Parameters required to compute the back-off delay for the next retry.
 */
typedef struct BackoffAlgorithmContext
{
    /**
     * @brief Maximum backoff delay (milliseconds) between consecutive retries.
     */
    uint16_t maxBackoffDelay;

    /**
     * @brief Number of retry attempts completed; incremented on every successful
     * BackoffAlgorithm_GetNextBackoff call.
     */
    uint32_t attemptsDone;

    /**
     * @brief Maximum backoff value (milliseconds) for the next retry attempt.
     */
    uint16_t nextJitterMax;

    /**
     * @brief Maximum number of retry attempts.
     */
    uint32_t maxRetryAttempts;

    /**
     * @brief The strategy provider invoked by BackoffAlgorithm_GetNextBackoff.
     */
    backoff_strategy_provider backoff_provider;
} BackoffAlgorithmContext_t;

/**
 * @brief Initialise a backoff context with the default Full-Jitter strategy.
 *
 * Sets the numeric parameters, resets the retry counter, and wires
 * BackoffAlgorithm_WithJitter as the strategy provider.  @p backOffBase is
 * clamped to @p maxBackOff so the first delay can never exceed the maximum.
 *
 * @param[out] pContext    Context to initialise (must be non-NULL).
 * @param[in]  backOffBase Base delay (milliseconds); clamped to @p maxBackOff.
 * @param[in]  maxBackOff  Maximum delay (milliseconds) between retries.
 * @param[in]  maxAttempts Maximum retries, or #BACKOFF_ALGORITHM_RETRY_FOREVER.
 *
 * @code{.c}
 * BackoffAlgorithmContext_t ctx;
 * BackoffAlgorithm_InitializeParams(&ctx, 1000, 10000, 5);
 * @endcode
 */
PUBLIC_API void
BackoffAlgorithm_InitializeParams(BackoffAlgorithmContext_t *pContext,
                                  uint16_t backOffBase,
                                  uint16_t maxBackOff,
                                  uint32_t maxAttempts);

/**
 * @brief Initialise a backoff context with a custom strategy provider.
 *
 * Identical to BackoffAlgorithm_InitializeParams except the strategy is the
 * caller-supplied @p strategy_provider rather than the default Full-Jitter
 * strategy.
 *
 * @param[out] pContext           Context to initialise (must be non-NULL).
 * @param[in]  backOffBase        Base delay (milliseconds).
 * @param[in]  maxBackOff         Maximum delay (milliseconds) between retries.
 * @param[in]  maxAttempts        Maximum retries, or #BACKOFF_ALGORITHM_RETRY_FOREVER.
 * @param[in]  strategy_provider  The backoff_strategy_provider to invoke.
 *
 * @code{.c}
 * BackoffAlgorithmContext_t ctx;
 * BackoffAlgorithm_InitializeParamsWithProvider(
 *     &ctx, 1000, 10000, 5, (backoff_strategy_provider)BackoffAlgorithm_ExponentialBackoff);
 * @endcode
 */
PUBLIC_API void
BackoffAlgorithm_InitializeParamsWithProvider(BackoffAlgorithmContext_t *pContext,
                                              uint16_t backOffBase,
                                              uint16_t maxBackOff,
                                              uint32_t maxAttempts,
                                              backoff_strategy_provider strategy_provider);

/**
 * @brief Initialise a backoff context with the deterministic NoJitter strategy.
 *
 * Wires BackoffAlgorithm_NoJitter (deterministic quadratic growth) as the
 * strategy provider.  Use when no entropy source is available, or when
 * deterministic delays are required (e.g. testing).
 *
 * @param[out] pContext    Context to initialise (must be non-NULL).
 * @param[in]  backOffBase Base delay (milliseconds); the quadratic lower bound.
 * @param[in]  maxBackOff  Maximum delay (milliseconds) between retries.
 * @param[in]  maxAttempts Maximum retries, or #BACKOFF_ALGORITHM_RETRY_FOREVER.
 *
 * @code{.c}
 * BackoffAlgorithmContext_t ctx;
 * BackoffAlgorithm_InitializeParamsWithoutJitter(&ctx, 1, 300, 5);
 * @endcode
 */
PUBLIC_API void
BackoffAlgorithm_InitializeParamsWithoutJitter(BackoffAlgorithmContext_t *pContext,
                                               uint16_t backOffBase,
                                               uint16_t maxBackOff,
                                               uint32_t maxAttempts);

/**
 * @brief Initialise a backoff context with the deterministic exponential strategy.
 *
 * Wires BackoffAlgorithm_ExponentialBackoff (delay = base * 2^attemptsDone,
 * saturated at @p maxBackOff) as the strategy provider.
 *
 * @param[out] pContext    Context to initialise (must be non-NULL).
 * @param[in]  backOffBase Base delay (milliseconds).
 * @param[in]  maxBackOff  Maximum delay (milliseconds) between retries.
 * @param[in]  maxAttempts Maximum retries, or #BACKOFF_ALGORITHM_RETRY_FOREVER.
 *
 * @code{.c}
 * BackoffAlgorithmContext_t ctx;
 * BackoffAlgorithm_InitializeParamsExponential(&ctx, 1000, 10000, 5);
 * @endcode
 */
PUBLIC_API void
BackoffAlgorithm_InitializeParamsExponential(BackoffAlgorithmContext_t *pContext,
                                             uint16_t backOffBase,
                                             uint16_t maxBackOff,
                                             uint32_t maxAttempts);

/**
 * @brief Reset the retry counter to zero.
 *
 * Does not change the strategy, base, maximum, or attempt limit.  Useful to
 * restart the retry cycle without re-initialising the context.
 *
 * @param[in,out] pContext Context whose attemptsDone is reset (must be non-NULL).
 *
 * @code{.c}
 * BackoffAlgorithm_ResetAttempts(&ctx);
 * @endcode
 */
PUBLIC_API void
BackoffAlgorithm_ResetAttempts(BackoffAlgorithmContext_t *pContext);

/**
 * @brief Compute the next backoff delay via the configured strategy provider.
 *
 * Dispatches to the backoff_provider selected at initialisation.  After a failed
 * operation the caller should obtain the delay here, wait that long, then retry
 * the operation.
 *
 * @param[in,out] pRetryContext Context holding the retry state (must be non-NULL).
 * @param[in]     randomValue   Passed to the provider.  The Full-Jitter strategy
 *                              uses it as a random value in [0, UINT32_MAX];
 *                              deterministic strategies ignore it.
 * @param[out]    pNextBackOff  Populated with the delay (milliseconds); never
 *                              exceeds the configured maximum.
 *
 * @return #BackoffAlgorithmSuccess, or #BackoffAlgorithmRetriesExhausted when the
 *         attempt limit is reached.
 *
 * @code{.c}
 * uint16_t delay = 0;
 * if (BackoffAlgorithm_GetNextBackoff(&ctx, rand(), &delay)
 *         == BackoffAlgorithmRetriesExhausted) {
 *     // give up
 * }
 * @endcode
 */
PUBLIC_API BackoffAlgorithmStatus_t
BackoffAlgorithm_GetNextBackoff(BackoffAlgorithmContext_t *pRetryContext,
                                uint32_t randomValue,
                                uint16_t *pNextBackOff);

/**
 * @brief Deterministic quadratic backoff (no jitter).
 *
 * Computes delay = min(base + (attemptsDone + 1)^2 / 2, maxBackoffDelay),
 * ignoring the random value.  The delay is monotonic non-decreasing and never
 * exceeds the configured maximum.
 *
 * @param[in,out] pRetryContext Context holding the retry state (must be non-NULL).
 * @param[in]     user_value    Unused (retained for the provider signature).
 * @param[out]    next_backoff  Populated with the delay (milliseconds).
 *
 * @return #BackoffAlgorithmSuccess, or #BackoffAlgorithmRetriesExhausted.
 */
PUBLIC_API BackoffAlgorithmStatus_t
BackoffAlgorithm_NoJitter(BackoffAlgorithmContext_t *pRetryContext,
                          uint32_t user_value,
                          uint16_t *next_backoff);

/**
 * @brief Deterministic exponential backoff (no jitter).
 *
 * Computes delay = min(base * 2^attemptsDone, maxBackoffDelay), ignoring the
 * random value.  Doubles each attempt and saturates at the configured maximum.
 *
 * @param[in,out] pRetryContext Context holding the retry state (must be non-NULL).
 * @param[in]     user_value    Unused (retained for the provider signature).
 * @param[out]    next_backoff  Populated with the delay (milliseconds).
 *
 * @return #BackoffAlgorithmSuccess, or #BackoffAlgorithmRetriesExhausted.
 */
PUBLIC_API BackoffAlgorithmStatus_t
BackoffAlgorithm_ExponentialBackoff(BackoffAlgorithmContext_t *pRetryContext,
                                    uint32_t user_value,
                                    uint16_t *next_backoff);

/**
 * @brief Full-Jitter backoff (random value within an exponentially growing ceiling).
 *
 * Computes delay = randomValue % (nextJitterMax + 1), then doubles nextJitterMax
 * each attempt up to maxBackoffDelay.  This is the original upstream strategy.
 *
 * @param[in,out] pRetryContext Context holding the retry state (must be non-NULL).
 * @param[in]     randomValue   Random value in [0, UINT32_MAX].
 * @param[out]    pNextBackOff  Populated with the delay (milliseconds).
 *
 * @return #BackoffAlgorithmSuccess, or #BackoffAlgorithmRetriesExhausted.
 */
PUBLIC_API BackoffAlgorithmStatus_t
BackoffAlgorithm_WithJitter(BackoffAlgorithmContext_t *pRetryContext,
                            uint32_t randomValue,
                            uint16_t *pNextBackOff);

#endif //UFSRV_ALGORITH_H
