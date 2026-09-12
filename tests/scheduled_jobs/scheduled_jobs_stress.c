/**
 * @file
 * @brief Standalone multi-threaded stress test for V1 scheduled_jobs.
 *
 * Exercises the job scheduler under sustained concurrent insert + peek +
 * removeMin load.  Uses the V1 API as-is.
 *
 * Usage:
 *   scheduled_jobs_stress --threads 4 --duration 5 --capacity 128
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include <uflib/scheduled_jobs/scheduled_jobs.h>
#include <uflib/main_types.h>

/* ── Fake clock ──────────────────────────────────────────────────── */

static volatile long long s_fake_time_us = 0;

static long long sFakeGetTime(void)
{
    return s_fake_time_us;
}

/* ── Job type ────────────────────────────────────────────────────── */

static ScheduledJobType s_job_type;

static int sFakeOnRun(void *ctx, void *data)
{
    (void)ctx;
    (void)data;
    return 0;
}

static int sFakeCompareKeys(void *a, void *b)
{
    long long la = *(long long *)a;
    long long lb = *(long long *)b;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

/* ── GetScheduledJobsStore (consumer-defined) ────────────────────── */

static ScheduledJobs s_store;

ScheduledJobs *GetScheduledJobsStore(void)
{
    return &s_store;
}

/* ── Stats ───────────────────────────────────────────────────────── */

static volatile long long s_total_inserts   = 0;
static volatile long long s_total_peeks     = 0;
static volatile long long s_total_removes   = 0;
static volatile long long s_insert_errors   = 0;
static volatile long long s_peek_misses     = 0;
static volatile long long s_remove_misses   = 0;

/* ── Worker thread ───────────────────────────────────────────────── */

typedef struct {
    int           id;
    volatile int *stop_flag;
} ThreadArg;

static void *sWorkerThread(void *arg_ptr)
{
    ThreadArg *arg = (ThreadArg *)arg_ptr;
    ScheduledJobs *store = GetScheduledJobsStore();

    while (!*(arg->stop_flag)) {
        int op = rand() % 10;

        if (op < 5) {
            /* Insert */
            ScheduledJob *job = calloc(1, sizeof(ScheduledJob));
            if (!job) continue;
            job->job_type_ptr = &s_job_type;
            job->when_to_schedule = (rand() % 1000) + 1;
            job->context_data = job;  /* self-reference for cleanup */

            int rc = InsertScheduledJob(store, job);
            if (rc == 0) {
                __sync_fetch_and_add(&s_total_inserts, 1);
            } else {
                __sync_fetch_and_add(&s_insert_errors, 1);
                free(job);
            }
        } else if (op < 8) {
            /* Peek */
            ScheduledJobContext ctx = {0};
            ScheduledJobContext *r = GetScheduledJob(store, LOCK_HINT_NONE, &ctx);
            if (r) {
                __sync_fetch_and_add(&s_total_peeks, 1);
            } else {
                __sync_fetch_and_add(&s_peek_misses, 1);
            }
        } else {
            /* RemoveMin */
            ScheduledJobContext ctx = {0};
            ScheduledJobContext *r = GetRemScheduledJob(store, LOCK_HINT_NONE, &ctx);
            if (r) {
                __sync_fetch_and_add(&s_total_removes, 1);
                free(r->scheduled_job_ptr);  /* was calloc'd on insert */
            } else {
                __sync_fetch_and_add(&s_remove_misses, 1);
            }
        }
    }

    return NULL;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int num_threads = 4;
    int duration_s  = 5;
    int capacity    = 128;

    static struct option long_opts[] = {
        {"threads",  required_argument, 0, 't'},
        {"duration", required_argument, 0, 'd'},
        {"capacity", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:d:c:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't': num_threads = atoi(optarg); break;
        case 'd': duration_s  = atoi(optarg); break;
        case 'c': capacity    = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s [--threads N] [--duration S] [--capacity C]\n", argv[0]);
            return 1;
        }
    }

    printf("ScheduledJobs V1 Stress Test\n");
    printf("  threads:  %d\n", num_threads);
    printf("  duration: %d s\n", duration_s);
    printf("  capacity: %d\n", capacity);

    /* Init */
    s_fake_time_us = 0;
    InitScheduledJobsStore(&s_store, capacity);

    memset(&s_job_type, 0, sizeof(s_job_type));
    s_job_type.type_name      = "stress_job";
    s_job_type.frequency_mode = PERIODIC;
    s_job_type.frequency      = 100;
    s_job_type.callbacks.on_get_time     = sFakeGetTime;
    s_job_type.callbacks.on_run          = sFakeOnRun;
    s_job_type.callbacks.on_compare_keys = sFakeCompareKeys;
    RegisterScheduledJobType(&s_store, &s_job_type);

    /* Start workers */
    volatile int stop_flag = 0;
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    ThreadArg *args    = calloc(num_threads, sizeof(ThreadArg));

    for (int i = 0; i < num_threads; i++) {
        args[i].id = i;
        args[i].stop_flag = &stop_flag;
        pthread_create(&threads[i], NULL, sWorkerThread, &args[i]);
    }

    /* Run */
    printf("\nRunning for %d seconds...\n", duration_s);
    sleep(duration_s);
    stop_flag = 1;

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Drain remaining jobs */
    size_t drained = 0;
    while (1) {
        ScheduledJobContext ctx = {0};
        ScheduledJobContext *r = GetRemScheduledJob(&s_store, LOCK_HINT_NONE, &ctx);
        if (!r) break;
        free(r->scheduled_job_ptr);
        drained++;
    }

    /* Report */
    long long total_ops = s_total_inserts + s_total_peeks + s_total_removes;
    double ops_per_sec = (double)total_ops / duration_s;

    printf("\n=== Results ===\n");
    printf("  Total ops:        %lld\n", total_ops);
    printf("  Throughput:       %.0f ops/s\n", ops_per_sec);
    printf("  Inserts:          %lld\n", s_total_inserts);
    printf("  Peeks:            %lld\n", s_total_peeks);
    printf("  Removes:          %lld\n", s_total_removes);
    printf("  Insert errors:    %lld\n", s_insert_errors);
    printf("  Peek misses:      %lld\n", s_peek_misses);
    printf("  Remove misses:    %lld\n", s_remove_misses);
    printf("  Drained at end:   %zu\n", drained);
    printf("  Final size:       %zu\n",
           GetScheduleJobsSetsize(&s_store, LOCK_HINT_NONE));

    /* Errors that matter: insert failures when store not full,
     * peek/remove misses when store non-empty (racy but shouldn't
     * be consistently zero). */
    long long errors = s_insert_errors;

    free(threads);
    free(args);
    DestructScheduledJobs(&s_store);

    if (errors > 0) {
        printf("\nFAILED — %lld errors\n", errors);
        return 1;
    }

    printf("\nPASSED — zero errors\n");
    return 0;
}
