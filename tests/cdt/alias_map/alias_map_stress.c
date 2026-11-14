// alias_map stress test — standalone, not registered with CTest.
//
// Each thread owns a disjoint slice of the key space, so the presence flag it
// keeps is exact, and the operation type is chosen independently of the key —
// every key therefore receives every operation over the course of a run.
//
// Two measurement modes:
//   --duration S   run for S seconds (soak)
//   --ops N        run exactly N operations (the mode to use for baselines)
//
// --foreach swaps the concurrent workload for a single-threaded enumeration
// benchmark over the same key/alias/value fixture: it times a whole-map
// AliasMapForEach() walk against an AliasMapSnapshotCreate() walk and reports
// ns per alias for both.  With --foreach, --ops means whole-map walks.
//
// Every worker starts at a barrier, so the measured window is the work, not the
// thread-creation time.  A sampled latency histogram (every 64th operation) is
// reported as p50/p90/p99/p99.9/max, and the end-state table geometry is taken
// from AliasMapDescribe(), so a throughput change can be attributed to table
// shape rather than guessed at.
//
//   ./alias_map_stress --threads 8 --ops 5000000 --capacity 65536 \
//                      --profile baseline.json
//   ./alias_map_stress --threads 8 --duration 10

#include <uflib/cdt/alias_map/alias_map.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LATENCY_SAMPLE_RATE 64u
#define LATENCY_BUCKET_COUNT 40u

static double g_warmup_seconds;

typedef struct {
    AliasMap         *map_ptr;
    size_t            key_base;
    size_t            key_count;
    unsigned          seed;
    bool             *present_ptr;
    unsigned long long ops;
    unsigned long long adds;
    unsigned long long gets;
    unsigned long long removes;
    unsigned long long latency_buckets[LATENCY_BUCKET_COUNT];
    unsigned long long latency_samples;
    unsigned long long latency_max_ns;
    atomic_ullong    *error_count_ptr;
} AliasMapStressWorker;

static double
sNowSeconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
sKeyName(char *buffer_ptr, size_t buffer_sz, size_t index)
{
    snprintf(buffer_ptr, buffer_sz, "k%zu", index);
}

static void
sAliasName(char *buffer_ptr, size_t buffer_sz, size_t index)
{
    snprintf(buffer_ptr, buffer_sz, "a%zu", index);
}

static void
sValueName(char *buffer_ptr, size_t buffer_sz, size_t index)
{
    snprintf(buffer_ptr, buffer_sz, "v/%zu", index);
}

static void
sLatencyRecord(AliasMapStressWorker *worker_ptr, unsigned long long elapsed_ns)
{
    unsigned bucket = 0;
    unsigned long long value = elapsed_ns;

    while (value > 1 && bucket + 1 < LATENCY_BUCKET_COUNT) {
        value >>= 1;
        bucket++;
    }

    worker_ptr->latency_buckets[bucket]++;
    worker_ptr->latency_samples++;

    if (elapsed_ns > worker_ptr->latency_max_ns) worker_ptr->latency_max_ns = elapsed_ns;
}

/*! Representative upper bound of a histogram bucket. */
static unsigned long long
sBucketValue(unsigned bucket)
{
    if (bucket == 0) return 1;

    return 1ULL << bucket;
}

/*! Percentile in nanoseconds from a merged histogram; 0 when no samples. */
static unsigned long long
sPercentile(const unsigned long long *buckets, unsigned long long samples, double percentile)
{
    if (samples == 0) return 0;

    unsigned long long target = (unsigned long long)((double)samples * percentile);
    unsigned long long seen   = 0;

    if (target == 0) target = 1;

    for (unsigned i = 0; i < LATENCY_BUCKET_COUNT; i++) {
        seen += buckets[i];

        if (seen >= target) return sBucketValue(i);
    }

    return sBucketValue(LATENCY_BUCKET_COUNT - 1);
}

/*!
 * One measured phase.  Runs until @p deadline (duration mode) or until
 * @p op_limit operations have completed (op mode, op_limit != 0).
 */
static void
sRunPhase(AliasMapStressWorker *worker_ptr, double deadline, unsigned long long op_limit,
          bool measure)
{
    bool *present = worker_ptr->present_ptr;

    while (true) {
        if (op_limit && worker_ptr->ops >= op_limit) break;

        size_t index = worker_ptr->key_base + (size_t)(rand_r(&worker_ptr->seed) % worker_ptr->key_count);
        size_t slot  = index - worker_ptr->key_base;
        char   key[32], alias[32], value[32], got[64];
        double started = 0.0;

        sKeyName(key, sizeof key, index);
        sAliasName(alias, sizeof alias, index);
        sValueName(value, sizeof value, index);

        if (measure && (worker_ptr->ops % LATENCY_SAMPLE_RATE) == 0) started = sNowSeconds();

        switch (rand_r(&worker_ptr->seed) % 3) {
        case 0: {
            if (present[slot]) break;

            const char *group[] = { key, alias };

            if (!AliasMapAdd(worker_ptr->map_ptr, group, 2, value)) {
                atomic_fetch_add(worker_ptr->error_count_ptr, 1);
                break;
            }

            present[slot] = true;
            worker_ptr->adds++;
            break;
        }
        case 1: {
            const char *name    = (rand_r(&worker_ptr->seed) & 1U) ? alias : key;
            size_t      out_len = 0;
            bool        found   = AliasMapGetInto(worker_ptr->map_ptr, name, got, sizeof got, &out_len);

            if (present[slot]) {
                if (!found || strcmp(got, value) != 0 || out_len != strlen(value))
                    atomic_fetch_add(worker_ptr->error_count_ptr, 1);
            }
            else if (found) {
                atomic_fetch_add(worker_ptr->error_count_ptr, 1);
            }

            worker_ptr->gets++;
            break;
        }
        default: {
            if (!present[slot]) break;

            /* The group must go as a whole: removing only `key` would leave
             * the sibling alias behind and invalidate the presence flag. */
            bool removed = AliasMapRemoveValue(worker_ptr->map_ptr, key) > 0;

            if (!removed) {
                atomic_fetch_add(worker_ptr->error_count_ptr, 1);
                break;
            }

            present[slot] = false;
            worker_ptr->removes++;
            break;
        }
        }

        worker_ptr->ops++;

        if (measure && started > 0.0)
            sLatencyRecord(worker_ptr, (unsigned long long)((sNowSeconds() - started) * 1e9));

        if (!op_limit && (worker_ptr->ops & 0xFFFU) == 0 && sNowSeconds() >= deadline) break;
    }
}

static pthread_barrier_t g_start_barrier;
static pthread_barrier_t g_measured_barrier;

typedef struct {
    unsigned long long op_limit;   /* 0 = duration mode */
    double             duration;
} AliasMapPhaseSpec;

static AliasMapPhaseSpec g_phase;

static void *
sWorker(void *arg_ptr)
{
    AliasMapStressWorker *worker_ptr = arg_ptr;

    pthread_barrier_wait(&g_start_barrier);

    if (g_warmup_seconds > 0.0) {
        double deadline = sNowSeconds() + g_warmup_seconds;

        sRunPhase(worker_ptr, deadline, 0, false);

        /* Warmup does not count: reset everything measured. */
        worker_ptr->ops    = 0;
        worker_ptr->adds   = 0;
        worker_ptr->gets   = 0;
        worker_ptr->removes = 0;

        pthread_barrier_wait(&g_measured_barrier);
    }

    double deadline = g_phase.duration > 0.0 ? sNowSeconds() + g_phase.duration : 0.0;

    sRunPhase(worker_ptr, deadline, g_phase.op_limit, true);

    return NULL;
}

static void
sAppendEscaped(FILE *out, const char *value_ptr)
{
    fputc('"', out);

    for (const char *p = value_ptr; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);

        fputc(*p, out);
    }

    fputc('"', out);
}

static unsigned long long
sJsonFieldUnsigned(const char *json, const char *field_ptr)
{
    char  needle[64];
    const char *at;

    snprintf(needle, sizeof needle, "\"%s\":", field_ptr);
    at = strstr(json, needle);

    return at ? strtoull(at + strlen(needle), NULL, 10) : 0;
}

static double
sJsonFieldDouble(const char *json, const char *field_ptr)
{
    char  needle[64];
    const char *at;

    snprintf(needle, sizeof needle, "\"%s\":", field_ptr);
    at = strstr(json, needle);

    return at ? strtod(at + strlen(needle), NULL) : 0.0;
}

/* ── Enumeration benchmark (--foreach) ─────────────────────────────────── */

typedef struct {
    unsigned long long walks;
    unsigned long long aliases;
    unsigned long long groups;
    double             elapsed_s;
    unsigned long long errors;
} EnumerationStats;

static unsigned long long g_visit_aliases;
static unsigned long long g_visit_groups;
static unsigned long long g_visit_sink;
static const char        *g_visit_last_value;

/*! Counting visitor; flags a group boundary whenever the value pointer changes. */
static bool
sVisitCount(void *context_ptr, const char *value_ptr, const char *alias_ptr)
{
    (void)context_ptr;

    if (value_ptr != g_visit_last_value) {
        g_visit_groups++;
        g_visit_last_value = value_ptr;
    }

    g_visit_aliases++;
    g_visit_sink += (unsigned char)alias_ptr[0];

    return true;
}

static void
sVisitReset(void)
{
    g_visit_aliases    = 0;
    g_visit_groups     = 0;
    g_visit_last_value = NULL;
}

/*! One snapshot walk; false when the snapshot could not be allocated. */
static bool
sSnapshotWalk(AliasMap *map_ptr, unsigned long long *aliases_out, unsigned long long *groups_out)
{
    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    if (!snapshot_ptr) return false;

    unsigned long long aliases = 0;
    size_t             groups  = AliasMapSnapshotGroupCount(snapshot_ptr);

    for (size_t g = 0; g < groups; g++) {
        size_t count = AliasMapSnapshotAliasCountAt(snapshot_ptr, g);

        for (size_t a = 0; a < count; a++)
            g_visit_sink += (unsigned char)AliasMapSnapshotAliasAt(snapshot_ptr, g, a)[0];

        aliases += count;
    }

    *aliases_out = aliases;
    *groups_out  = groups;

    AliasMapSnapshotDestroy(snapshot_ptr);

    return true;
}

static void
sWarmupWalks(AliasMap *map_ptr, double warmup)
{
    if (warmup <= 0.0) return;

    double until = sNowSeconds() + warmup;

    while (sNowSeconds() < until) {
        sVisitReset();
        AliasMapForEach(map_ptr, sVisitCount, NULL);
    }
}

static EnumerationStats
sMeasureForeach(AliasMap *map_ptr, unsigned long long walk_limit, double duration,
                unsigned long long alias_count, unsigned long long group_count)
{
    EnumerationStats stats   = {0};
    double           started = sNowSeconds();

    do {
        sVisitReset();

        if (!AliasMapForEach(map_ptr, sVisitCount, NULL) || g_visit_aliases != alias_count ||
            g_visit_groups != group_count)
            stats.errors++;

        stats.walks++;
        stats.aliases += g_visit_aliases;
        stats.groups  += g_visit_groups;
    } while (walk_limit ? stats.walks < walk_limit : sNowSeconds() - started < duration);

    stats.elapsed_s = sNowSeconds() - started;

    return stats;
}

static EnumerationStats
sMeasureSnapshot(AliasMap *map_ptr, unsigned long long walk_limit, double duration,
                 unsigned long long alias_count, unsigned long long group_count)
{
    EnumerationStats   stats   = {0};
    double             started = sNowSeconds();
    unsigned long long aliases = 0;
    unsigned long long groups  = 0;

    do {
        if (!sSnapshotWalk(map_ptr, &aliases, &groups) || aliases != alias_count ||
            groups != group_count)
            stats.errors++;

        stats.walks++;
        stats.aliases += aliases;
        stats.groups  += groups;
    } while (walk_limit ? stats.walks < walk_limit : sNowSeconds() - started < duration);

    stats.elapsed_s = sNowSeconds() - started;

    return stats;
}

/*! Build the concurrency fixture: capacity two-alias groups k<i>/a<i> -&gt; v/<i>. */
static bool
sLoadFixture(AliasMap *map_ptr, unsigned long capacity)
{
    for (unsigned long i = 0; i < capacity; i++) {
        char key[32], alias[32], value[32];

        sKeyName(key, sizeof key, i);
        sAliasName(alias, sizeof alias, i);
        sValueName(value, sizeof value, i);

        const char *group[] = { key, alias };

        if (!AliasMapAdd(map_ptr, group, 2, value)) return false;
    }

    return true;
}

static int
sRunEnumerate(AliasMap *map_ptr, unsigned long capacity, unsigned long long walk_limit,
              double duration, double warmup, unsigned long seed, const char *profile_ptr,
              const char *label_ptr, const char *commit_ptr, bool json_stdout)
{
    if (!sLoadFixture(map_ptr, capacity)) {
        fprintf(stderr, "foreach: fixture load failed at capacity %lu\n", capacity);
        AliasMapDestroy(map_ptr);

        return 1;
    }

    BufferDescriptor *description = AliasMapDescribe(map_ptr, NULL);
    const char       *json        = description ? description->data : "{}";

    unsigned long long groups      = sJsonFieldUnsigned(json, "groups");
    unsigned long long aliases     = sJsonFieldUnsigned(json, "aliases");
    unsigned long long buckets     = sJsonFieldUnsigned(json, "buckets");
    unsigned long long max_chain   = sJsonFieldUnsigned(json, "max_chain");
    double             load_factor = sJsonFieldDouble(json, "load_factor");

    if (!json_stdout) {
        printf("=== AliasMap Enumeration ===\n");
        printf("  Groups:     %llu\n", groups);
        printf("  Aliases:    %llu\n", aliases);
        printf("  Walks:      %s\n", walk_limit ? "fixed walk count" : "duration");
        if (walk_limit) printf("  Count:      %llu\n", walk_limit);
        else            printf("  Duration:   %.1f s\n", duration);
        printf("  Warmup:     %.1f s\n", warmup);
        printf("============================\n\nRunning...\n");
    }

    sWarmupWalks(map_ptr, warmup);

    EnumerationStats foreach_stats = sMeasureForeach(map_ptr, walk_limit, duration, aliases, groups);

    sWarmupWalks(map_ptr, warmup);

    EnumerationStats snapshot_stats = sMeasureSnapshot(map_ptr, walk_limit, duration, aliases, groups);

    unsigned long long errors = foreach_stats.errors + snapshot_stats.errors;

    double foreach_ns_per_alias =
        foreach_stats.aliases ? foreach_stats.elapsed_s * 1e9 / (double)foreach_stats.aliases : 0.0;
    double snapshot_ns_per_alias =
        snapshot_stats.aliases ? snapshot_stats.elapsed_s * 1e9 / (double)snapshot_stats.aliases : 0.0;

    FILE *report = stdout;

    if (profile_ptr) {
        report = fopen(profile_ptr, "w");

        if (!report) {
            fprintf(stderr, "foreach: cannot open '%s'\n", profile_ptr);
            AliasMapDestroy(map_ptr);

            return 1;
        }
    }

    if (json_stdout || profile_ptr) {
        fprintf(report, "{\"schema\":1,\"label\":");
        sAppendEscaped(report, label_ptr);
        fprintf(report, ",\"commit\":");
        sAppendEscaped(report, commit_ptr);
        fprintf(report,
                ",\"params\":{\"mode\":\"foreach\",\"threads\":1,\"capacity\":%lu,\"duration_s\":%.3f,"
                "\"ops_requested\":%llu,\"seed\":%lu,\"warmup_s\":%.3f},"
                "\"results\":{\"ops\":%llu,\"elapsed_s\":%.4f,\"ops_per_s\":%.0f,"
                "\"adds\":0,\"gets\":0,\"removes\":0,"
                "\"adds_per_s\":0,\"gets_per_s\":0,\"removes_per_s\":0,\"errors\":%llu,"
                "\"latency\":{\"sample_rate\":0,\"samples\":0,"
                "\"p50_ns\":0,\"p90_ns\":0,\"p99_ns\":0,\"p999_ns\":0,\"max_ns\":0}},"
                "\"geometry\":{\"groups\":%llu,\"aliases\":%llu,\"buckets\":%llu,"
                "\"load_factor\":%.2f,\"max_chain\":%llu},"
                "\"enumerate\":{\"aliases_per_walk\":%llu,"
                "\"foreach\":{\"walks\":%llu,\"elapsed_s\":%.4f,\"ns_per_alias\":%.1f,\"aliases_per_s\":%.0f},"
                "\"snapshot\":{\"walks\":%llu,\"elapsed_s\":%.4f,\"ns_per_alias\":%.1f,\"aliases_per_s\":%.0f}}}\n",
                capacity, walk_limit ? 0.0 : duration, walk_limit, seed, warmup,
                foreach_stats.aliases,
                foreach_stats.elapsed_s,
                foreach_stats.elapsed_s > 0.0 ? (double)foreach_stats.aliases / foreach_stats.elapsed_s : 0.0,
                errors, groups, aliases, buckets, load_factor, max_chain, aliases,
                foreach_stats.walks, foreach_stats.elapsed_s, foreach_ns_per_alias,
                foreach_stats.elapsed_s > 0.0 ? (double)foreach_stats.aliases / foreach_stats.elapsed_s : 0.0,
                snapshot_stats.walks, snapshot_stats.elapsed_s, snapshot_ns_per_alias,
                snapshot_stats.elapsed_s > 0.0 ? (double)snapshot_stats.aliases / snapshot_stats.elapsed_s : 0.0);
    }

    if (report != stdout) fclose(report);

    if (!json_stdout) {
        printf("\n=== Results ===\n");
        printf("  Foreach:    %llu walks  %10.3f ms/walk  %6.1f ns/alias  %6.1f M aliases/s\n",
               foreach_stats.walks, foreach_stats.elapsed_s * 1e3 / (double)foreach_stats.walks,
               foreach_ns_per_alias,
               foreach_stats.elapsed_s > 0.0 ? (double)foreach_stats.aliases / foreach_stats.elapsed_s / 1e6 : 0.0);
        printf("  Snapshot:   %llu walks  %10.3f ms/walk  %6.1f ns/alias  %6.1f M aliases/s\n",
               snapshot_stats.walks, snapshot_stats.elapsed_s * 1e3 / (double)snapshot_stats.walks,
               snapshot_ns_per_alias,
               snapshot_stats.elapsed_s > 0.0 ? (double)snapshot_stats.aliases / snapshot_stats.elapsed_s / 1e6 : 0.0);

        if (foreach_ns_per_alias > 0.0)
            printf("  Ratio:      snapshot/foreach = %.2fx\n", snapshot_ns_per_alias / foreach_ns_per_alias);

        printf("  Geometry:   groups %llu  aliases %llu  buckets %llu  load %.2f  max chain %llu\n",
               groups, aliases, buckets, load_factor, max_chain);
        printf("  Errors:     %llu (must be zero)\n", errors);
        printf("===================\n");
    }

    if (description) {
        BufferDescriptorRelease(description);
        free(description);
    }

    AliasMapDestroy(map_ptr);

    return errors == 0 ? 0 : 1;
}

int
main(int argc, char *argv[])
{
    unsigned long  threads     = 8;
    unsigned long  capacity    = 65536;
    unsigned long  seed        = 1234;
    unsigned long long op_limit = 0;
    double         duration    = 5.0;
    double         warmup      = 0.0;
    const char    *profile_ptr = NULL;
    const char    *label_ptr   = "";
    const char    *commit_ptr  = "";
    bool           json_stdout = false;
    bool           enumerate   = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)        threads     = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)   duration    = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)        op_limit    = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc)   capacity    = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)       seed        = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)     warmup      = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc)    profile_ptr = argv[++i];
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)      label_ptr   = argv[++i];
        else if (strcmp(argv[i], "--commit") == 0 && i + 1 < argc)     commit_ptr  = argv[++i];
        else if (strcmp(argv[i], "--json") == 0)                       json_stdout = true;
        else if (strcmp(argv[i], "--foreach") == 0)                    enumerate   = true;
        else {
            fprintf(stderr,
                    "usage: %s [--threads N] [--duration SECONDS | --ops N] [--capacity KEYS]\n"
                    "          [--seed S] [--warmup SECONDS] [--profile FILE] [--json]\n"
                    "          [--label TEXT] [--commit SHA] [--foreach]\n"
                    "\n"
                    "  --foreach benchmarks AliasMapForEach against a snapshot walk over the\n"
                    "  same fixture; --ops then means whole-map walks.\n",
                    argv[0]);
            return 2;
        }
    }

    if (threads == 0 || capacity == 0 || (duration <= 0.0 && op_limit == 0) || warmup < 0.0) {
        fprintf(stderr, "%s: threads, capacity and duration must be positive\n", argv[0]);
        return 2;
    }

    if (!enumerate) {
        if (capacity < threads) capacity = threads;

        if (op_limit && op_limit < threads) op_limit = threads;
    }

    AliasMap *map_ptr = AliasMapCreate();

    if (!map_ptr) {
        fprintf(stderr, "%s: AliasMapCreate failed\n", argv[0]);
        return 1;
    }

    if (enumerate)
        return sRunEnumerate(map_ptr, capacity, op_limit, duration, warmup, seed, profile_ptr,
                             label_ptr, commit_ptr, json_stdout);

    pthread_t            *thread_array = calloc(threads, sizeof *thread_array);
    AliasMapStressWorker *worker_array = calloc(threads, sizeof *worker_array);
    bool                 **present_array = calloc(threads, sizeof *present_array);
    atomic_ullong          error_count = 0;

    if (!thread_array || !worker_array || !present_array) {
        free(thread_array);
        free(worker_array);
        free(present_array);
        AliasMapDestroy(map_ptr);
        return 1;
    }

    size_t per_thread = capacity / threads;

    pthread_barrier_init(&g_start_barrier, NULL, (unsigned)threads + 1);
    pthread_barrier_init(&g_measured_barrier, NULL, (unsigned)threads + 1);

    for (unsigned long i = 0; i < threads; i++) {
        present_array[i] = calloc(per_thread, sizeof **present_array);

        if (!present_array[i]) {
            fprintf(stderr, "%s: out of memory for presence flags\n", argv[0]);
            return 1;
        }

        worker_array[i] = (AliasMapStressWorker){
            .map_ptr         = map_ptr,
            .key_base        = i * per_thread,
            .key_count       = per_thread,
            .seed            = (unsigned)(seed + i),
            .present_ptr     = present_array[i],
            .error_count_ptr = &error_count,
        };
    }

    g_warmup_seconds = warmup;
    g_phase          = (AliasMapPhaseSpec){ .op_limit = op_limit / threads,
                                            .duration = op_limit ? 0.0 : duration };

    if (!op_limit) g_phase.op_limit = 0;

    if (!json_stdout) {
        printf("=== AliasMap Stress Test ===\n");
        printf("  Threads:    %lu\n", threads);
        printf("  Mode:       %s\n", op_limit ? "fixed operation count" : "duration");
        if (op_limit) printf("  Ops:        %llu\n", (unsigned long long)(g_phase.op_limit * threads));
        else          printf("  Duration:   %.1f s\n", duration);
        printf("  Keys:       %lu\n", capacity);
        printf("  Warmup:     %.1f s\n", warmup);
        printf("  Seed:       %lu\n", seed);
        printf("============================\n\nRunning...\n");
    }

    double started = 0.0;

    for (unsigned long i = 0; i < threads; i++) {
        if (pthread_create(&thread_array[i], NULL, sWorker, &worker_array[i]) != 0) {
            fprintf(stderr, "%s: pthread_create failed\n", argv[0]);
            return 1;
        }
    }

    pthread_barrier_wait(&g_start_barrier);

    if (warmup > 0.0) pthread_barrier_wait(&g_measured_barrier);

    started = sNowSeconds();

    unsigned long long ops = 0, adds = 0, gets = 0, removes = 0;
    unsigned long long latency_buckets[LATENCY_BUCKET_COUNT] = {0};
    unsigned long long latency_samples = 0, latency_max_ns = 0;

    for (unsigned long i = 0; i < threads; i++) {
        pthread_join(thread_array[i], NULL);

        ops     += worker_array[i].ops;
        adds    += worker_array[i].adds;
        gets    += worker_array[i].gets;
        removes += worker_array[i].removes;

        latency_samples += worker_array[i].latency_samples;
        if (worker_array[i].latency_max_ns > latency_max_ns) latency_max_ns = worker_array[i].latency_max_ns;

        for (unsigned b = 0; b < LATENCY_BUCKET_COUNT; b++)
            latency_buckets[b] += worker_array[i].latency_buckets[b];
    }

    double elapsed = sNowSeconds() - started;

    BufferDescriptor *description = AliasMapDescribe(map_ptr, NULL);
    const char       *description_json = description ? description->data : "{}";
    unsigned long long groups      = sJsonFieldUnsigned(description_json, "groups");
    unsigned long long aliases     = sJsonFieldUnsigned(description_json, "aliases");
    unsigned long long buckets     = sJsonFieldUnsigned(description_json, "buckets");
    unsigned long long max_chain   = sJsonFieldUnsigned(description_json, "max_chain");
    double             load_factor = sJsonFieldDouble(description_json, "load_factor");

    unsigned long long p50  = sPercentile(latency_buckets, latency_samples, 0.50);
    unsigned long long p90  = sPercentile(latency_buckets, latency_samples, 0.90);
    unsigned long long p99  = sPercentile(latency_buckets, latency_samples, 0.99);
    unsigned long long p999 = sPercentile(latency_buckets, latency_samples, 0.999);
    double             ops_per_s = elapsed > 0.0 ? (double)ops / elapsed : 0.0;

    for (unsigned long i = 0; i < threads; i++) free(present_array[i]);

    free(present_array);
    free(thread_array);
    free(worker_array);

    FILE *report = stdout;

    if (profile_ptr) {
        report = fopen(profile_ptr, "w");

        if (!report) {
            fprintf(stderr, "%s: cannot open '%s'\n", argv[0], profile_ptr);
            AliasMapDestroy(map_ptr);
            return 1;
        }
    }

    if (json_stdout || profile_ptr) {
        fprintf(report,
                "{\"schema\":1,\"label\":");
        sAppendEscaped(report, label_ptr);
        fprintf(report, ",\"commit\":");
        sAppendEscaped(report, commit_ptr);
        fprintf(report,
                ",\"params\":{\"mode\":\"%s\",\"threads\":%lu,\"capacity\":%lu,"
                "\"duration_s\":%.3f,\"ops_requested\":%llu,\"seed\":%lu,\"warmup_s\":%.3f},"
                "\"results\":{\"ops\":%llu,\"elapsed_s\":%.4f,\"ops_per_s\":%.0f,"
                "\"adds\":%llu,\"gets\":%llu,\"removes\":%llu,"
                "\"adds_per_s\":%.0f,\"gets_per_s\":%.0f,\"removes_per_s\":%.0f,"
                "\"errors\":%llu,"
                "\"latency\":{\"sample_rate\":%u,\"samples\":%llu,"
                "\"p50_ns\":%llu,\"p90_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"max_ns\":%llu}},"
                "\"geometry\":{\"groups\":%llu,\"aliases\":%llu,\"buckets\":%llu,"
                "\"load_factor\":%.2f,\"max_chain\":%llu}}\n",
                op_limit ? "ops" : "duration", threads, capacity,
                op_limit ? 0.0 : duration, op_limit, seed, warmup,
                ops, elapsed, ops_per_s,
                adds, gets, removes,
                elapsed > 0.0 ? (double)adds / elapsed : 0.0,
                elapsed > 0.0 ? (double)gets / elapsed : 0.0,
                elapsed > 0.0 ? (double)removes / elapsed : 0.0,
                (unsigned long long)error_count,
                LATENCY_SAMPLE_RATE, latency_samples,
                p50, p90, p99, p999, latency_max_ns,
                groups, aliases, buckets, load_factor, max_chain);
    }

    if (report != stdout) fclose(report);

    if (!json_stdout) {
        printf("\n=== Results ===\n");
        printf("  Total ops:        %llu\n", ops);
        printf("  Elapsed:          %.3f s\n", elapsed);
        printf("  Throughput:       %.0f ops/s\n", ops_per_s);
        printf("  Adds:             %llu (%.0f/s)\n", adds, elapsed > 0.0 ? (double)adds / elapsed : 0.0);
        printf("  Gets:             %llu (%.0f/s)\n", gets, elapsed > 0.0 ? (double)gets / elapsed : 0.0);
        printf("  Removes:          %llu (%.0f/s)\n", removes, elapsed > 0.0 ? (double)removes / elapsed : 0.0);
        printf("  Latency (1/%u):    p50 %lluns  p90 %lluns  p99 %lluns  p99.9 %lluns  max %lluns\n",
               LATENCY_SAMPLE_RATE, p50, p90, p99, p999, latency_max_ns);
        printf("  Geometry:         groups %llu  aliases %llu  buckets %llu  load %.2f  max chain %llu\n",
               groups, aliases, buckets, load_factor, max_chain);
        printf("  Errors:           %llu (must be zero)\n", (unsigned long long)error_count);
        printf("===================\n");
    }

    if (description) {
        BufferDescriptorRelease(description);
        free(description);
    }

    AliasMapDestroy(map_ptr);

    return error_count == 0 ? 0 : 1;
}
