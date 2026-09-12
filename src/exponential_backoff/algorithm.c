/*
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
 */

/**
 * @brief Implementation of the backoff algorithm API for a "Full Jitter" exponential backoff
 * with jitter strategy.
 */

#include <assert.h>
#include <stddef.h>

/* Include API header. */
#include <uflib/standard_defs.h>
#include "uflib/exponential_backoff/algorithm.h"

/*-----------------------------------------------------------*/

BackoffAlgorithmStatus_t BackoffAlgorithm_GetNextBackoff( BackoffAlgorithmContext_t * pRetryContext,
                                                          uint32_t randomValue,
                                                          uint16_t * pNextBackOff )
{
  assert( pRetryContext != NULL );
  assert( pNextBackOff != NULL );

  return pRetryContext->backoff_provider(pRetryContext, randomValue, pNextBackOff);
}

BackoffAlgorithmStatus_t BackoffAlgorithm_WithJitter( BackoffAlgorithmContext_t * pRetryContext,
                                                          uint32_t randomValue,
                                                          uint16_t * pNextBackOff )
{
  BackoffAlgorithmStatus_t status = BackoffAlgorithmSuccess;

  assert( pRetryContext != NULL );
  assert( pNextBackOff != NULL );

  /* If maxRetryAttempts state of the context is set to the maximum, retry forever. */
  if( ( pRetryContext->maxRetryAttempts == BACKOFF_ALGORITHM_RETRY_FOREVER ) ||
      ( pRetryContext->attemptsDone < pRetryContext->maxRetryAttempts ) )
  {
    /* The next backoff value is a random value between 0 and the maximum jitter value
     * for the retry attempt. */

    /* Choose a random value for back-off time between 0 and the max jitter value. */
    *pNextBackOff = ( uint16_t ) ( randomValue % ( pRetryContext->nextJitterMax + ( uint32_t ) 1U ) );

    /* Increment the retry attempt. */
    pRetryContext->attemptsDone++;

    /* Double the max jitter value for the next retry attempt, only
     * if the new value will be less than the max backoff time value. */
    if( pRetryContext->nextJitterMax < ( pRetryContext->maxBackoffDelay / 2U ) )
    {
      pRetryContext->nextJitterMax += pRetryContext->nextJitterMax;
    }
    else
    {
      pRetryContext->nextJitterMax = pRetryContext->maxBackoffDelay;
    }
  }
  else
  {
    /* When max retry attempts are exhausted, let application know by
     * returning BackoffAlgorithmRetriesExhausted. Application may choose to
     * restart the retry process after calling BackoffAlgorithm_InitializeParams(). */
    status = BackoffAlgorithmRetriesExhausted;
  }

  return status;
}

#include <math.h>
BackoffAlgorithmStatus_t BackoffAlgorithm_NoJitter( BackoffAlgorithmContext_t *pRetryContext, uint32_t user_value, uint16_t *next_backoff)
{
  BackoffAlgorithmStatus_t status = BackoffAlgorithmSuccess;

  if ((pRetryContext->maxRetryAttempts == BACKOFF_ALGORITHM_RETRY_FOREVER) || (pRetryContext->attemptsDone < pRetryContext->maxRetryAttempts)) {
    /* The next backoff value is a random value between 0 and the maximum jitter value
     * for the retry attempt. */

    /* Choose a random value for back-off time between 0 and the max jitter value. */
    *next_backoff =  min(pRetryContext->nextJitterMax + (pow(pRetryContext->attemptsDone + 1, 2) - 1) / 2, pRetryContext->maxBackoffDelay);//nextJitterMax is taken to mean lower bound (current set to '1')

    /* Increment the retry attempt. */
    pRetryContext->attemptsDone++;
  } else {
    status = BackoffAlgorithmRetriesExhausted;
  }

  return status;
}

BackoffAlgorithmStatus_t BackoffAlgorithm_ExponentialBackoff( BackoffAlgorithmContext_t *pRetryContext, uint32_t user_value, uint16_t *next_backoff)
{
  BackoffAlgorithmStatus_t status = BackoffAlgorithmSuccess;
  uint64_t delay;

  if ((pRetryContext->maxRetryAttempts == BACKOFF_ALGORITHM_RETRY_FOREVER) || (pRetryContext->attemptsDone < pRetryContext->maxRetryAttempts)) {
    /* Deterministic exponential backoff: base * 2^attemptsDone, saturated at
     * maxBackoffDelay.  user_value is unused (deterministic), matching NoJitter. */
    delay = pRetryContext->nextJitterMax;
    for (uint32_t i = 0; i < pRetryContext->attemptsDone && delay < pRetryContext->maxBackoffDelay; ++i) {
      delay *= 2u;
      if (delay > pRetryContext->maxBackoffDelay) {
        delay = pRetryContext->maxBackoffDelay;
      }
    }
    *next_backoff = (uint16_t)delay;

    /* Increment the retry attempt. */
    pRetryContext->attemptsDone++;
  } else {
    status = BackoffAlgorithmRetriesExhausted;
  }

  return status;
}
/*-----------------------------------------------------------*/

void BackoffAlgorithm_InitializeParams( BackoffAlgorithmContext_t * pContext,
                                        uint16_t backOffBase,
                                        uint16_t maxBackOff,
                                        uint32_t maxAttempts )
{
  assert( pContext != NULL );

  /* Initialize the context with parameters used in calculating the backoff
   * value for the next retry attempt.  Clamp the base to the max delay so the
   * first backoff can never exceed maxBackoffDelay (the GetNextBackoff contract
   * guarantees the delay never exceeds the configured maximum). */
  pContext->nextJitterMax = (backOffBase < maxBackOff) ? backOffBase : maxBackOff;
  pContext->maxBackoffDelay = maxBackOff;
  pContext->maxRetryAttempts = maxAttempts;

  /* The total number of retry attempts is zero at initialization. */
  pContext->attemptsDone = 0;
  /* Default strategy is full jitter.  Do NOT wire the BackoffAlgorithm_GetNextBackoff
   * dispatcher here — that would make the first call recurse until the stack overflows. */
  pContext->backoff_provider = (backoff_strategy_provider)BackoffAlgorithm_WithJitter;
}

void BackoffAlgorithm_InitializeParamsWithoutJitter( BackoffAlgorithmContext_t * pContext, uint16_t backOffBase, uint16_t maxBackOff, uint32_t maxAttempts )
{
  BackoffAlgorithm_InitializeParamsWithProvider(pContext, backOffBase, maxBackOff, maxAttempts, (backoff_strategy_provider)BackoffAlgorithm_NoJitter);
}

void BackoffAlgorithm_InitializeParamsExponential( BackoffAlgorithmContext_t * pContext, uint16_t backOffBase, uint16_t maxBackOff, uint32_t maxAttempts )
{
  BackoffAlgorithm_InitializeParamsWithProvider(pContext, backOffBase, maxBackOff, maxAttempts, (backoff_strategy_provider)BackoffAlgorithm_ExponentialBackoff);
}

void BackoffAlgorithm_InitializeParamsWithProvider( BackoffAlgorithmContext_t * pContext,
                                                    uint16_t backOffBase,
                                                    uint16_t maxBackOff,
                                                    uint32_t maxAttempts,
                                                    backoff_strategy_provider strategy_provider)
{
  BackoffAlgorithm_InitializeParams(pContext, backOffBase, maxBackOff, maxAttempts);
  pContext->backoff_provider = strategy_provider;
}

void BackoffAlgorithm_ResetAttempts(BackoffAlgorithmContext_t * pContext)
{
  pContext->attemptsDone = 0;
}