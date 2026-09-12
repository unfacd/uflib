/**
 * @file recycler_v2_stress.c
 * @brief End-to-end multi-threaded stress test — simulates real server demand
 *
 * Creates multiple type pools (Session, Fence, Descriptor) with marshal
 * callbacks, then spawns worker threads that randomly Get/Put/Referenced/
 * UnReferenced across pools with varied hold times.  Small pool sizes ensure
 * frequent exhaustion, expansion, and CLOCK-sweep exercise.
 *
 * Usage:
 *   recycler_v2_stress --threads N --duration S [--no-marshal]
 *
 * Zero-error assertion is a gating condition.  Built but NOT in CTest
 * (requires CLI args).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#include <uflib/recycler_v2/recycler_v2.h>
#include "recycler_v2_priv.h"

/* ── Payload: per-object verification + simulated server state ──────────── */

typedef struct {
    int              pool_id;      ///< which pool this object belongs to
    _Atomic uint64_t seq;          ///< monotonic write counter (data verification)
    _Atomic uint64_t last_seq;     ///< last seq value seen on this object
    uint8_t          padding[48];  ///< simulate real object size (~64 bytes)
} ServerPayload;

/* ── Global counters ─────────────────────────────────────────────────────── */

static atomic_bool  sRunning          = true;

static atomic_ulong sTotalGets        = 0;
static atomic_ulong sTotalPuts        = 0;
static atomic_ulong sTotalRefs        = 0;
static atomic_ulong sTotalUnrefs      = 0;

static atomic_ulong sGetErrors        = 0;  ///< data verification failures
static atomic_ulong sPutErrors        = 0;  ///< refcount/enqueue failures
static atomic_ulong sNullGets         = 0;  ///< hard exhaustion
static atomic_ulong sMarshalCalls     = 0;  ///< marshal callback invoked
static atomic_ulong sUnmarshalCalls   = 0;  ///< unmarshal callback invoked
static atomic_ulong sNewInstances     = 0;  ///< RecyclerV2GetNewInstance success
static atomic_ulong sDestroyInstances = 0;  ///< RecyclerV2DestroyInstance success
static atomic_ulong sAliasErrors      = 0;  ///< multi-holder path violations

/* ── Callbacks ───────────────────────────────────────────────────────────── */

static int
sInitCallback(ClientContextData *data_ptr, size_t oid)
{
    ServerPayload *p = (ServerPayload *)data_ptr;
    p->pool_id = (int)(oid >> 20);  // groupid from oid encoding
    atomic_store(&p->seq, 0);
    atomic_store(&p->last_seq, 0);
    return 0;
}

static int
sInitGetCallback(InstanceHolderV2 *ih, ContextData *ctx,
                 size_t oid, unsigned long flags)
{
    (void)ih; (void)ctx; (void)oid; (void)flags;
    return 0;
}

static int
sInitPutCallback(InstanceHolderV2 *ih, ContextData *ctx,
                 unsigned long flags)
{
    (void)ih; (void)ctx; (void)flags;
    return 0;
}

static int
sDestructCallback(InstanceHolderV2 *ih, ContextData *ctx,
                  unsigned long flags)
{
    (void)ih; (void)ctx; (void)flags;
    return 0;
}

/* ── Marshal/unmarshal callbacks ─────────────────────────────────────────── */

static int
sMarshalCallback(ClientContextData *obj_ptr, uint8_t *out_buf,
                 size_t buf_sz, size_t *out_len_ptr)
{
    ServerPayload *p = (ServerPayload *)obj_ptr;
    size_t needed = sizeof(p->pool_id) + sizeof(uint64_t);
    if (buf_sz < needed) return -1;
    memcpy(out_buf, &p->pool_id, sizeof(p->pool_id));
    memcpy(out_buf + sizeof(p->pool_id), &p->last_seq, sizeof(uint64_t));
    *out_len_ptr = needed;
    atomic_fetch_add(&sMarshalCalls, 1);
    return 0;
}

static int
sUnmarshalCallback(ClientContextData *obj_ptr, const uint8_t *in_buf,
                   size_t buf_len)
{
    ServerPayload *p = (ServerPayload *)obj_ptr;
    size_t needed = sizeof(p->pool_id) + sizeof(uint64_t);
    if (buf_len < needed) return -1;
    memcpy(&p->pool_id, in_buf, sizeof(p->pool_id));
    memcpy(&p->last_seq, in_buf + sizeof(p->pool_id), sizeof(uint64_t));
    atomic_store(&p->seq, 0);
    atomic_fetch_add(&sUnmarshalCalls, 1);
    return 0;
}

/* ── Ops table ───────────────────────────────────────────────────────────── */

static RecyclerV2PoolOps sOpsNoMarshal = {
    sInitCallback, sInitGetCallback, sInitPutCallback,
    sDestructCallback, NULL, NULL, NULL, NULL
};

static RecyclerV2PoolOps sOpsMarshal = {
    sInitCallback, sInitGetCallback, sInitPutCallback,
    sDestructCallback, sMarshalCallback, sUnmarshalCallback, NULL, NULL
};

/* ── Pool definitions (simulate a real server's type registry) ───────────── */

#define N_POOLS 3

static const char *sPoolNames[N_POOLS] = {
    "Session", "Fence", "Descriptor"
};

/* ── Worker: randomized operations across all pools ──────────────────────── */

static void *
sWorker(void *arg)
{
    RecyclerV2PoolHandle **handles = (RecyclerV2PoolHandle **)arg;

    /* Per-thread PRNG seed (weak but adequate for stress) */
    unsigned seed = (unsigned)(pthread_self() ^ time(NULL));

    while (atomic_load(&sRunning)) {
        /* Pick a random pool */
        int pi = rand_r(&seed) % N_POOLS;
        RecyclerV2PoolHandle *h = handles[pi];

        InstanceHolderV2 *ih = RecyclerV2Get(h, NULL, 0);
        if (!ih) {
            atomic_fetch_add(&sNullGets, 1);
            continue;
        }

        ServerPayload *p = (ServerPayload *)RecyclerV2GetInstance(ih);
        if (!p) {
            atomic_fetch_add(&sGetErrors, 1);
            free(ih);
            continue;
        }

        /* Data verification: write a fresh sequence number, verify it
         * exceeds the previous value (catches use-after-recycle). */
        uint64_t seq = atomic_fetch_add(&p->seq, 1) + 1;
        uint64_t prev = atomic_exchange(&p->last_seq, seq);
        if (seq <= prev) {
            atomic_fetch_add(&sGetErrors, 1);
        }
        atomic_fetch_add(&sTotalGets, 1);

        /* Simulate server work: varied hold times.
         * ~80% fast (spin), ~15% medium (yield), ~5% slow (usleep).
         * Slow holders keep objects busy, increasing exhaustion pressure. */
        int r = rand_r(&seed) % 100;
        if (r < 5) {
            usleep((unsigned)(rand_r(&seed) % 2000 + 500));   // 0.5–2.5 ms
        } else if (r < 20) {
            for (volatile int i = 0; i < 500; i++) {}         // medium
        }
        /* else: fast — immediate return */

        /* Occasionally bump refcount before Put (~10% of ops) to
         * exercise the multi-reference path. */
        if ((rand_r(&seed) % 10) == 0) {
            RecyclerV2Referenced(ih, 1);
            atomic_fetch_add(&sTotalRefs, 1);
            /* Simulate brief shared access */
            for (volatile int i = 0; i < 20; i++) {}
            RecyclerV2UnReferenced(ih, 1);
            atomic_fetch_add(&sTotalUnrefs, 1);
        }

        /* Occasionally exercise the multi-holder fallback path (~8% of ops):
         * RecyclerV2GetNewInstance() allocates a second holder and a
         * fallback-list entry, RecyclerV2DestroyInstance() removes it.  This
         * drives concurrent holder_fallback churn under load. */
        if ((rand_r(&seed) % 100) < 8) {
            InstanceHolderV2 *alias = RecyclerV2GetNewInstance(ih);
            if (!alias) {
                /* An alias MUST succeed while the primary is still leased. */
                atomic_fetch_add(&sAliasErrors, 1);
            } else {
                atomic_fetch_add(&sNewInstances, 1);
                /* The alias must resolve to the same object as the primary. */
                if (RecyclerV2GetInstance(alias) != p) {
                    atomic_fetch_add(&sAliasErrors, 1);
                }
                if (RecyclerV2DestroyInstance(alias) !=
                        RECYCLER_V2_INSTANCE_HOLDER_FOUND) {
                    atomic_fetch_add(&sAliasErrors, 1);
                } else {
                    atomic_fetch_add(&sDestroyInstances, 1);
                }
                free(alias);
            }
        }

        int rc = RecyclerV2Put(ih, NULL, 0);
        if (rc != 0) {
            atomic_fetch_add(&sPutErrors, 1);
        } else {
            atomic_fetch_add(&sTotalPuts, 1);
        }

        free(ih);
    }
    return NULL;
}

/* ── Usage ───────────────────────────────────────────────────────────────── */

static void
sPrintUsage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --threads N --duration S [--no-marshal]\n", prog);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
    int  threads     = 4;
    int  duration    = 5;
    bool use_marshal = true;

    static struct option long_opts[] = {
        {"threads",    required_argument, 0, 't'},
        {"duration",   required_argument, 0, 'd'},
        {"no-marshal", no_argument,       0, 'n'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:d:nh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't': threads     = atoi(optarg); break;
        case 'd': duration    = atoi(optarg); break;
        case 'n': use_marshal = false;        break;
        case 'h': sPrintUsage(argv[0]); return 0;
        default:  sPrintUsage(argv[0]); return 1;
        }
    }

    RecyclerV2PoolOps *ops = use_marshal ? &sOpsMarshal : &sOpsNoMarshal;

    printf("=== RecyclerV2 Server Simulation Stress Test ===\n");
    printf("  Threads:   %d\n", threads);
    printf("  Duration:  %d s\n", duration);
    printf("  Pools:     %d (Session, Fence, Descriptor)\n", N_POOLS);
    printf("  Marshal:   %s\n", use_marshal ? "enabled" : "disabled");
    printf("==================================================\n\n");

    /* Create multiple type pools — small capacities so exhaustion
     * and expansion happen frequently under load.
     *
     * Pool layout per type:
     *   expansion_threshold=16 (15 usable + 1 sentinel)
     *   group_allocation_sz=4  (max 4 groups = 60 objects per pool)
     *   marshal_blob_max_sz=64 (enough for pool_id + last_seq)
     *
     * With N threads, 3 pools, and 60 max objects per pool (180 total),
     * exhaustion is guaranteed.  Expansion to all 4 groups happens
     * quickly; after that CLOCK sweep is the only relief. */
    RecyclerV2PoolHandle *handles[N_POOLS] = {NULL};

    for (int i = 0; i < N_POOLS; i++) {
        RecyclerV2PoolConfig cfg = {
            sPoolNames[i],                    // type_name
            sizeof(ServerPayload),            // blocksz
            4,                                // group_allocation_sz
            16,                               // expansion_threshold
            ops,                              // ops_ptr
            0.3f,                             // marshal_watermark
            use_marshal ? 64u : 0u,           // marshal_blob_max_sz
            NULL,                             // storage_root (default)
            NULL,                             // ufsrv_class
            NULL,                             // instance_id
            RECYCLER_V2_STORAGE_OVERWRITE     // storage_init_policy
        };
        handles[i] = RecyclerV2InitTypePool(&cfg);
        if (!handles[i]) {
            fprintf(stderr, "FATAL: RecyclerV2InitTypePool failed for '%s'\n",
                sPoolNames[i]);
            return 1;
        }
        printf("  Pool '%s': capacity=%zu (type=%u)\n",
            sPoolNames[i],
            RecyclerV2GetCapacity(handles[i]),
            handles[i]->type);
    }

    /* Pre-populate all pools — exhaust every pool to trigger expansion
     * to max groups, invoking all init callbacks, then return everything. */
    printf("\nPre-populating pools...\n");
    for (int i = 0; i < N_POOLS; i++) {
        InstanceHolderV2 **held = calloc(4096, sizeof(InstanceHolderV2 *));
        size_t count = 0;

        /* Lease until hard exhaustion — triggers expansion to max groups */
        while (count < 4096) {
            InstanceHolderV2 *ih = RecyclerV2Get(handles[i], NULL, 0);
            if (!ih) break;
            held[count++] = ih;
        }

        /* Return everything */
        for (size_t j = 0; j < count; j++) {
            RecyclerV2Put(held[j], NULL, 0);
            free(held[j]);
        }
        free(held);

        printf("  %s: %zu objects across %zu groups (capacity=%zu)\n",
            sPoolNames[i], count,
            count / 15 + 1,  // 15 usable per group
            RecyclerV2GetCapacity(handles[i]));
    }

    printf("\nRunning for %d seconds...\n\n", duration);

    pthread_t *tids = calloc((size_t)threads, sizeof(pthread_t));
    for (int i = 0; i < threads; i++) {
        pthread_create(&tids[i], NULL, sWorker, handles);
    }

    struct timespec ts = {duration, 0};
    nanosleep(&ts, NULL);
    atomic_store(&sRunning, false);

    for (int i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }
    free(tids);

    /* ── Quiescence invariant ──────────────────────────────────────────────
     * Every RecyclerV2Get() is paired with a RecyclerV2Put(), and every
     * RecyclerV2GetNewInstance() with a RecyclerV2DestroyInstance(), so no
     * object may remain leased once all workers have joined.  A non-zero
     * leased count indicates a leaked holder or object. */
    int leak_found = 0;
    for (int i = 0; i < N_POOLS; i++) {
        size_t leased = RecyclerV2GetLeasedCount(handles[i]);
        if (leased != 0) {
            fprintf(stderr,
                "LEAK: pool '%s' still has %zu leased object(s) after quiescence\n",
                sPoolNames[i], leased);
            leak_found = 1;
        }
    }

    /* ── Report ──────────────────────────────────────────────────────────── */

    unsigned long total_gets    = atomic_load(&sTotalGets);
    unsigned long total_puts    = atomic_load(&sTotalPuts);
    unsigned long total_refs    = atomic_load(&sTotalRefs);
    unsigned long total_unrefs  = atomic_load(&sTotalUnrefs);
    unsigned long total_ops     = total_gets + total_puts + total_refs + total_unrefs;
    unsigned long get_errs      = atomic_load(&sGetErrors);
    unsigned long put_errs      = atomic_load(&sPutErrors);
    unsigned long null_gets     = atomic_load(&sNullGets);
    unsigned long marshal_calls = atomic_load(&sMarshalCalls);
    unsigned long unmarshal_calls = atomic_load(&sUnmarshalCalls);
    unsigned long new_instances = atomic_load(&sNewInstances);
    unsigned long destroy_instances = atomic_load(&sDestroyInstances);
    unsigned long alias_errors = atomic_load(&sAliasErrors);

    printf("=== Results ===\n");
    printf("  Total ops:        %lu\n", total_ops);
    printf("  Throughput:       %lu ops/s\n",
           total_ops / (unsigned long)duration);
    printf("\n");
    printf("  Gets:             %lu\n", total_gets);
    printf("  Puts:             %lu\n", total_puts);
    printf("  Referenced:       %lu\n", total_refs);
    printf("  UnReferenced:     %lu\n", total_unrefs);
    printf("\n");
    printf("  Get errors:       %lu (data verification)\n", get_errs);
    printf("  Put errors:       %lu (refcount/enqueue)\n", put_errs);
    printf("  NULL gets:        %lu (hard exhaustion)\n", null_gets);
    printf("  GetNewInstance:   %lu\n", new_instances);
    printf("  DestroyInstance:  %lu\n", destroy_instances);
    printf("  Alias errors:     %lu (multi-holder violations)\n", alias_errors);
    if (use_marshal) {
        printf("  Marshal calls:    %lu\n", marshal_calls);
        printf("  Unmarshal calls:  %lu\n", unmarshal_calls);
    }
    printf("\n");
    printf("  Per-pool capacity:\n");
    for (int i = 0; i < N_POOLS; i++) {
        printf("    %-12s  capacity=%6zu  leased=%6zu\n",
            sPoolNames[i],
            RecyclerV2GetCapacity(handles[i]),
            RecyclerV2GetLeasedCount(handles[i]));
    }
    printf("===================\n");

    if (get_errs > 0 || put_errs > 0 || alias_errors > 0 || leak_found) {
        printf("FAIL: Non-zero error count.\n");
        return 1;
    }

    printf("Stress test complete — zero errors.\n");
    return 0;
}
