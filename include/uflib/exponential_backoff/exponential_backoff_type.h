

#ifndef UFSRV_EXPONENTIAL_BACKOFF_TYPE_H
#define UFSRV_EXPONENTIAL_BACKOFF_TYPE_H

#include <uflib/exponential_backoff/algorithm.h>

/* The maximum number of retries for the example code. */
#define EXPOBACKOFF_RETRY_MAX_ATTEMPTS            ( 5U )

/* The maximum back-off delay (in milliseconds) for between retries */
#define EXPOBACKOFF_RETRY_MAX_BACKOFF_DELAY_MS    ( 10000U )

/* The base back-off delay (in milliseconds) for retry configuration */
#define EXPOBACKOFF_RETRY_BACKOFF_BASE_MS         ( 1000U )

#define EXPOBACKOFF_NOJITTER_MIN    ( 1U ) //1 second
#define EXPOBACKOFF_NOJITTER_MAX    ( 300U ) //300 seconds

typedef int (*on_sleep_provider)(long);

typedef struct ExponentialBackoffDescriptor {
    BackoffAlgorithmContext_t retry_params;
    on_sleep_provider         on_sleep;
} ExponentialBackoffDescriptor;

#endif //UFSRV_EXPONENTIAL_BACKOFF_TYPE_H
