/**
 * @file ufconfig_stress.c
 * @brief Standalone reload-versus-read stress for the config module.
 *
 * Not registered with CTest: it takes arguments and runs for a caller-chosen
 * duration, so it is a tool rather than a gate.  Run it directly:
 *
 *     ./ufconfig_stress --threads 8 --duration 10 --root <dir>
 *
 * The property under test is the one the module's design exists to provide: a
 * reader holding a value across a reload must never observe freed memory.  The
 * readers therefore keep the borrowed pointer and touch it, rather than reading
 * it once and discarding it — a reader that only compares the status code would
 * pass against an implementation that freed the string underneath it.
 *
 * Zero errors is the assertion.  Run under ASan or TSAN for the interesting
 * failures; a plain build will only catch the ones that fault immediately.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

static UfConfig *g_cfg;
static char g_source[1024];
static atomic_int g_stop;
static atomic_long g_reads;
static atomic_long g_read_errors;
static atomic_long g_reloads;
static atomic_long g_serialises;
static atomic_long g_serialise_errors;
static volatile uint64_t g_sink;
static atomic_long g_reload_errors;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void *reader_thread(void *arg) {
    (void)arg;
    UfConfigValue value;
    char scratch[256];

    while (!atomic_load(&g_stop)) {
        /* Pin across the window in which the borrowed bytes are used.
         *
         * The pointer UfConfigGetField hands back outlives the call, and a
         * reload concurrent with it may release the arena that backs it — the
         * contract is explicit that a reload retires the previous arena only
         * while a pin is held.  Without this the pointer is used after the
         * call that produced it with nothing keeping it alive, which ASan
         * reports as a use-after-free and which is a defect in this reader
         * rather than in the handle.  Pinned per iteration rather than for the
         * whole run, so the retired list is released each time round instead
         * of accumulating one arena per reload.
         */
        UfConfigPin(g_cfg);
        memset(&value, 0, sizeof(value));
        UfConfigStatus st = UfConfigGetField(g_cfg, "ufsrv.main_listener_address", &value);
        if (st == UF_CONFIG_OK && value.kind == UF_CONFIG_KIND_STRING &&
            value.as.str.ptr != NULL) {
            /* Touch every byte.  A pointer into memory that was freed under us
               faults here, or is caught by ASan; merely reading the length
               would not be. */
            size_t n = value.as.str.len < sizeof(scratch) - 1 ? value.as.str.len
                                                              : sizeof(scratch) - 1;
            memcpy(scratch, value.as.str.ptr, n);
            scratch[n] = '\0';
            if (n == 0) atomic_fetch_add(&g_read_errors, 1);
        } else if (st != UF_CONFIG_ERR_NOFIELD) {
            atomic_fetch_add(&g_read_errors, 1);
        }
        UfConfigUnpin(g_cfg);
        atomic_fetch_add(&g_reads, 1);
    }
    return NULL;
}

/*!
 * @brief Serialises the whole tree, and walks the root namespace, in a loop.
 *
 * Neither of these takes a lock.  UfConfigToJsonAlloc walks the node graph from
 * ufconfig_canon.c, which contains no lock calls at all, and the namespace
 * accessors read h->root, h->by_id and h->unlisted unlocked too.  The reader
 * thread above only exercises UfConfigGetField, which does take the read lock,
 * so without this thread the harness says nothing about either path -- and a
 * concurrent reload replaces the tree and rebuilds both indexes underneath
 * them, while a concurrent setter rewrites the tag and the value of a node as
 * two separate stores, which a tagged union makes a type confusion rather than
 * a stale read.
 */
static void *serialiser_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        char *out = NULL;
        if (UfConfigToJsonAlloc(g_cfg, &out) == UF_CONFIG_OK) {
            if (out) {
                g_sink += strlen(out);
                free(out);
            }
        } else {
            atomic_fetch_add(&g_serialise_errors, 1);
        }

        /* A namespace handle carries borrowed node pointers that later calls
           walk, so it is pinned for its lifetime -- the same obligation as the
           reader's borrowed value above, and the reason the header says a
           namespace must not outlive a reload. */
        UfConfigPin(g_cfg);
        UfConfigNamespace *ns = NULL;
        if (UfConfigNamespaceGet(g_cfg, "", &ns) == UF_CONFIG_OK) {
            g_sink += UfConfigNamespaceEntryCount(ns);
            free(ns);
        } else {
            atomic_fetch_add(&g_serialise_errors, 1);
        }
        UfConfigUnpin(g_cfg);
        atomic_fetch_add(&g_serialises, 1);
    }
    return NULL;
}

static void *reloader_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        UfConfigReloadReport report;
        memset(&report, 0, sizeof(report));
        UfConfigStatus st = UfConfigReload(g_cfg, &report);
        if (st != UF_CONFIG_OK && st != UF_CONFIG_NO_CHANGE) {
            atomic_fetch_add(&g_reload_errors, 1);
        }
        atomic_fetch_add(&g_reloads, 1);
        usleep(500);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int threads = 4;
    double duration = 5.0;
    const char *root = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: %s [--threads N] [--duration SECONDS] [--root DIR]\n", argv[0]);
            return 2;
        }
    }
    if (threads < 1 || threads > 64) {
        fprintf(stderr, "threads must be between 1 and 64\n");
        return 2;
    }

    snprintf(g_source, sizeof(g_source), "%s/examples/sample.strict.lua", root);

    /* The handle is bound to the schema the same way a server binds it.  The
       storm this runs is a reader holding a borrowed pointer across reloads,
       which is only meaningful if the configuration actually validated. */
    UfConfigDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fields      = g_ufconfig_fields;
    descriptor.field_count = (size_t)g_ufconfig_field_count;
    descriptor.lookup      = UfConfigLookupPath;
    descriptor.kind        = UF_CONFIG_BACKEND_FILE;

    if (UfConfigCreate(&g_cfg, &descriptor) != UF_CONFIG_OK) {
        fprintf(stderr, "UfConfigCreate failed — the descriptor is incomplete\n");
        return 1;
    }

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = UF_CONFIG_LOAD_LENIENT;

    if (UfConfigLoadFile(g_cfg, g_source, &opt, NULL) != UF_CONFIG_OK) {
        fprintf(stderr, "cannot load %s — no stress performed\n", g_source);
        UfConfigDestroy(g_cfg);
        return 1;
    }

    pthread_t readers[64];
    pthread_t reloader;

    for (int i = 0; i < threads; i++) {
        if (pthread_create(&readers[i], NULL, reader_thread, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    if (pthread_create(&reloader, NULL, reloader_thread, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }
    pthread_t serialiser;
    if (pthread_create(&serialiser, NULL, serialiser_thread, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }

    double start = now_seconds();
    usleep((useconds_t)(duration * 1e6));
    atomic_store(&g_stop, 1);

    for (int i = 0; i < threads; i++) pthread_join(readers[i], NULL);
    pthread_join(reloader, NULL);
    pthread_join(serialiser, NULL);

    double elapsed = now_seconds() - start;
    long reads = atomic_load(&g_reads);
    long reloads = atomic_load(&g_reloads);

    printf("threads=%d duration=%.1fs\n", threads, elapsed);
    printf("reads=%ld (%.0f/s)  errors=%ld\n", reads, (double)reads / elapsed,
           atomic_load(&g_read_errors));
    printf("serialises=%ld (%.0f/s)  errors=%ld\n", atomic_load(&g_serialises),
           (double)atomic_load(&g_serialises) / elapsed, atomic_load(&g_serialise_errors));
    printf("reloads=%ld (%.0f/s)  errors=%ld\n", reloads, (double)reloads / elapsed,
           atomic_load(&g_reload_errors));

    UfConfigDestroy(g_cfg);

    if (atomic_load(&g_read_errors) != 0 || atomic_load(&g_reload_errors) != 0 ||
        atomic_load(&g_serialise_errors) != 0) {
        fprintf(stderr, "FAIL: errors observed\n");
        return 1;
    }
    printf("zero errors\n");
    return 0;
}
