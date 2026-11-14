/**
 * @file file_loader_concurrent_access_stress.c
 * @brief FileLoaderService — standalone multi-threaded stress test.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include <uflib/file_loader_service_concurrent/file_loader_concurrent_service.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_THREADS  4
#define DEFAULT_DURATION 5
#define DEFAULT_CAPACITY 4096

#define FIXTURE_COUNT 5

typedef struct {
    char          path[512];
    size_t        size;
    unsigned char digest[32];
} Fixture;

static Fixture sFixture[FIXTURE_COUNT];

typedef struct {
    _Atomic uint64_t loads_ok, loads_estale, loads_timeout, loads_error;
    _Atomic uint64_t digests_ok, digests_bad;
    _Atomic int      last_errno;
} FixtureStats;

static FixtureStats sStats[FIXTURE_COUNT];

static const uint32_t sSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t sRotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sSha256Block(uint32_t st[8], const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sRotr(w[i-15], 7) ^ sRotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = sRotr(w[i-2], 17) ^ sRotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3]; e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sRotr(e, 6) ^ sRotr(e, 11) ^ sRotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + sSha256K[i] + w[i];
        uint32_t S0 = sRotr(a, 2) ^ sRotr(a, 13) ^ sRotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sSha256(const unsigned char *data, size_t len, unsigned char out[32])
{
    uint32_t st[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    size_t   i    = 0;
    unsigned char blk[64];

    for (; i + 64 <= len; i += 64) sSha256Block(st, data + i);
    size_t rem = len - i;
    memset(blk, 0, sizeof blk);
    if (rem) memcpy(blk, data + i, rem);
    blk[rem] = 0x80;
    if (rem >= 56) {
        sSha256Block(st, blk);
        memset(blk, 0, sizeof blk);
    }
    uint64_t bits = (uint64_t)len * 8U;
    for (int k = 0; k < 8; k++) blk[63 - k] = (unsigned char)(bits >> (8 * k));
    sSha256Block(st, blk);
    for (int k = 0; k < 8; k++) {
        out[4*k]   = (unsigned char)(st[k] >> 24);
        out[4*k+1] = (unsigned char)(st[k] >> 16);
        out[4*k+2] = (unsigned char)(st[k] >> 8);
        out[4*k+3] = (unsigned char)(st[k]);
    }
}

static void sHexDecode(const char *hex, unsigned char out[32])
{
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        sscanf(hex + 2*i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static int sLoadFixtures(void)
{
    char    manifest[600];
    FILE   *fp;
    int     loaded = 0;

    snprintf(manifest, sizeof manifest, "%s/SHA256SUMS", FILE_LOADER_CONCURRENT_FIXTURE_DIR);
    fp = fopen(manifest, "r");
    if (!fp) { perror(manifest); return -1; }
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        char hex[80], name[64], line[256];
        if (!fgets(line, sizeof line, fp)) { fprintf(stderr, "fixture manifest is short at entry %d\n", i); fclose(fp); return -1; }
        if (sscanf(line, "%79s  %63s", hex, name) != 2) { fprintf(stderr, "malformed manifest line: %s", line); fclose(fp); return -1; }
        snprintf(sFixture[i].path, sizeof sFixture[i].path, "%s/%s", FILE_LOADER_CONCURRENT_FIXTURE_DIR, name);
        sHexDecode(hex, sFixture[i].digest);
        FILE *data = fopen(sFixture[i].path, "rb");
        if (!data) { perror(sFixture[i].path); fclose(fp); return -1; }
        if (fseek(data, 0, SEEK_END) != 0) { fclose(data); fclose(fp); return -1; }
        long n = ftell(data);
        rewind(data);
        if (n < 0) { fclose(data); fclose(fp); return -1; }
        sFixture[i].size = (size_t)n;
        unsigned char *buf = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
        unsigned char  dg[32];
        if (n > 0 && fread(buf, 1, (size_t)n, data) != (size_t)n) { fprintf(stderr, "short read: %s\n", sFixture[i].path); free(buf); fclose(data); fclose(fp); return -1; }
        fclose(data);
        sSha256(buf, (size_t)n, dg);
        free(buf);
        if (memcmp(dg, sFixture[i].digest, sizeof dg) != 0) {
            fprintf(stderr, "FIXTURE %s does not match SHA256SUMS\n", sFixture[i].path);
            fclose(fp);
            return -1;
        }
        loaded++;
    }
    fclose(fp);
    return loaded;
}

typedef struct {
    char path[256];
    int  fixture;
} StressFileState;

typedef struct {
    FileLoaderService    *svc;
    StressFileState      *files;
    int                   num_files;
    _Atomic uint64_t      ops_total, ops_add, ops_get, ops_remove;
    _Atomic uint64_t      add_failures, get_failures, remove_failures;
    _Atomic uint64_t      size_mismatches, content_mismatches, get_timeouts, empty_gets;
    _Atomic int           last_get_errno;
    _Atomic bool         *stop;
    FixtureStats         *stats;
} WorkerArgs;

static void sPrintUsage(const char *prog) {
    fprintf(stderr, "Usage: %s [--threads N] [--duration S] [--capacity C]\n"
        "  Registry paths are drawn from the fixed fixture set in tests/thread_worker/file_loader_concurrent_access/fixtures.\n"
        "Defaults: %d threads, %ds, %u capacity, %d files\n",
        prog, DEFAULT_THREADS, DEFAULT_DURATION, DEFAULT_CAPACITY, FIXTURE_COUNT);
}

static inline uint32_t sRand(uint64_t *state) {
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;
    return (uint32_t)(*state >> 32);
}

static int sMappedFileCount(const char *needle) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512]; int n = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

static void *sWorker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;
    uint64_t rng = (uint64_t)(uintptr_t)a + (uint64_t)time(NULL);
    FileLoaderServiceFileHandle *handles    = (FileLoaderServiceFileHandle *)calloc((size_t)a->num_files, sizeof(*handles));
    bool                     *registered = (bool *)calloc((size_t)a->num_files, sizeof(*registered));
    void                     *held       = NULL;
    int                       held_fid   = -1;

    if (!handles || !registered) { free(handles); free(registered); return NULL; }
    FileLoaderServiceRegisterThread(a->svc);
    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        int fid = (int)(sRand(&rng) % (uint64_t)a->num_files);
        StressFileState *f = &a->files[fid];
        if (held && held_fid == fid) {
            FileLoaderServiceReturnFile(a->svc, held);
            held = NULL; held_fid = -1;
            continue;
        }
        int op = (int)(sRand(&rng) % 4);
        switch (op) {
        case 0: if (!registered[fid]) { FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(a->svc, f->path);
            if (h) { handles[fid] = h; registered[fid] = true; atomic_fetch_add(&a->ops_add, 1); }
            else atomic_fetch_add(&a->add_failures, 1); } break;
        case 1: if (registered[fid] && !held) { size_t sz = 0;
            void *data = FileLoaderServiceGetFileContent(a->svc, handles[fid], &sz);
            if (data) {
                const Fixture *fx = &sFixture[f->fixture];
                held = data; held_fid = fid;
                atomic_fetch_add(&a->ops_get, 1);
                atomic_fetch_add(&a->stats[f->fixture].loads_ok, 1);
                if (sz != fx->size) {
                    if (atomic_fetch_add(&a->size_mismatches, 1) < 3)
                        fprintf(stderr, "SIZE-MISMATCH fid=%d fixture=%d got=%zu expected=%zu\n",
                                fid, f->fixture, sz, fx->size);
                } else {
                    unsigned char got[32];
                    sSha256((const unsigned char *)data, sz, got);
                    if (memcmp(got, fx->digest, sizeof got) != 0) {
                        atomic_fetch_add(&a->stats[f->fixture].digests_bad, 1);
                        if (atomic_fetch_add(&a->content_mismatches, 1) < 3) {
                            fprintf(stderr, "DIGEST-MISMATCH fid=%d fixture=%d len=%zu got=", fid, f->fixture, sz);
                            for (int k = 0; k < 32; k++) fprintf(stderr, "%02x", got[k]);
                            fprintf(stderr, "\n");
                        }
                    } else {
                        atomic_fetch_add(&a->stats[f->fixture].digests_ok, 1);
                    }
                }
                if (f->fixture == 0) atomic_fetch_add(&a->empty_gets, 1);
            }
            else if (errno == ESTALE) { registered[fid] = false; handles[fid] = 0;
                atomic_fetch_add(&a->stats[f->fixture].loads_estale, 1); }
            else if (errno == ETIMEDOUT) { atomic_fetch_add(&a->get_timeouts, 1);
                atomic_fetch_add(&a->stats[f->fixture].loads_timeout, 1); }
            else { atomic_fetch_add(&a->get_failures, 1); atomic_store(&a->last_get_errno, errno);
                atomic_fetch_add(&a->stats[f->fixture].loads_error, 1);
                atomic_store(&a->stats[f->fixture].last_errno, errno); } } break;
        case 2: if (held) { FileLoaderServiceReturnFile(a->svc, held); held = NULL; held_fid = -1; } break;
        case 3: if (registered[fid]) {
            if (held && held_fid == fid) { FileLoaderServiceReturnFile(a->svc, held); held = NULL; held_fid = -1; }
            FileLoaderServiceRemoveFile(a->svc, f->path);
            registered[fid] = false; handles[fid] = 0;
            atomic_fetch_add(&a->ops_remove, 1); } break;
        }
        atomic_fetch_add(&a->ops_total, 1);
    }
    if (held) FileLoaderServiceReturnFile(a->svc, held);
    FileLoaderServiceUnregisterThread(a->svc);
    free(handles);
    free(registered);
    return NULL;
}

int main(int argc, char **argv) {
    int d = DEFAULT_DURATION; uint32_t cap = DEFAULT_CAPACITY;
    int nt = DEFAULT_THREADS;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"--duration") && i+1<argc) d = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--capacity") && i+1<argc) cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"--threads") && i+1<argc) nt = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) { sPrintUsage(argv[0]); return 0; }
        else { fprintf(stderr,"Unknown: %s\n",argv[i]); return 1; }
    }
    if (d<1)d=1; if(cap<16)cap=16; if(nt<1)nt=1;
    printf("=== FileLoaderService Stress Test ===\n  Threads: %d  Duration: %ds  Capacity: %u  Registry: %d fixtures\n", nt, d, cap, FIXTURE_COUNT);

    if (sLoadFixtures() != FIXTURE_COUNT) { fprintf(stderr, "fixture load failed\n"); return 1; }
    int zero_len_fixtures = 0;
    for (int i = 0; i < FIXTURE_COUNT; i++) if (sFixture[i].size == 0) zero_len_fixtures++;
    if (zero_len_fixtures != 1) { fprintf(stderr, "fixture set must contain exactly one zero-length file, found %d\n", zero_len_fixtures); return 1; }

    StressFileState *files = (StressFileState *)calloc(FIXTURE_COUNT, sizeof(*files));
    if (!files) return 1;
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        files[i].fixture = i;
        snprintf(files[i].path, sizeof(files[i].path), "%s", sFixture[i].path);
    }
    printf("  Fixtures verified against SHA256SUMS in %s\n", FILE_LOADER_CONCURRENT_FIXTURE_DIR);

    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = cap;
    flc.service_config_params.loaded_size   = (cap < 64) ? cap/2 : 64;
    if (!FileLoaderServiceInit(&flc)) { fprintf(stderr,"Init failed\n"); free(files); return 1; }
    pthread_t mon = FileLoaderServiceGetThread(flc.service_handle);
    int preflight_failures = 0;
    {
        int zf = 0;
        for (int i = 0; i < FIXTURE_COUNT; i++) if (sFixture[i].size == 0) { zf = i; break; }
        FileLoaderServiceRegisterThread(flc.service_handle);
        FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, sFixture[zf].path);
        size_t sz = 12345;
        void  *data = NULL;
        for (int i = 0; i < 300 && data == NULL; i++) {
            data = FileLoaderServiceGetFileContent(flc.service_handle, h, &sz);
            if (data == NULL) usleep(20000);
        }
        printf("  Preflight zero-length file: %s (size=%zu, expected %zu)\n",
               data ? "readable" : "UNREADABLE", sz, sFixture[0].size);
        if (data == NULL || sz != 0) preflight_failures++;
        if (data) FileLoaderServiceReturnFile(flc.service_handle, data);
        FileLoaderServiceUnregisterThread(flc.service_handle);
    }

    _Atomic bool stop = false;
    WorkerArgs *args = (WorkerArgs *)calloc((size_t)nt, sizeof(*args));
    pthread_t *threads = (pthread_t *)calloc((size_t)nt, sizeof(pthread_t));
    for (int t = 0; t < nt; t++) {
        args[t].svc = flc.service_handle; args[t].files = files;
        args[t].num_files = FIXTURE_COUNT; args[t].stop = &stop; args[t].stats = sStats;
        pthread_create(&threads[t], NULL, sWorker, &args[t]);
    }

    printf("Running %d seconds...\n", d);
    sleep((unsigned)d);
    atomic_store_explicit(&stop, true, memory_order_relaxed);
    for (int t = 0; t < nt; t++) pthread_join(threads[t], NULL);

    uint64_t total=0,adds=0,gets=0,rems=0,af=0,gf=0,rf=0,mism=0,cmism=0,timeouts=0,emptygets=0;
    for (int t=0;t<nt;t++) {
        total+=atomic_load(&args[t].ops_total);adds+=atomic_load(&args[t].ops_add);
        gets+=atomic_load(&args[t].ops_get);rems+=atomic_load(&args[t].ops_remove);
        af+=atomic_load(&args[t].add_failures);gf+=atomic_load(&args[t].get_failures);
        rf+=atomic_load(&args[t].remove_failures);
        mism+=atomic_load(&args[t].size_mismatches);
        cmism+=atomic_load(&args[t].content_mismatches);
        timeouts+=atomic_load(&args[t].get_timeouts);
        emptygets+=atomic_load(&args[t].empty_gets);
    }
    printf("\n=== Report ===\n");

    printf("\n  Registry: %d files under %s\n", FIXTURE_COUNT, FILE_LOADER_CONCURRENT_FIXTURE_DIR);
    printf("  %-14s %10s  %-64s %10s %8s %8s %7s %10s %9s\n",
           "file", "size", "sha256", "loads_ok", "estale", "timeout", "errors", "digest_ok", "digest_bad");
    uint64_t loads_ok = 0, estale_total = 0, to_total = 0, err_total = 0, dg_ok = 0, dg_bad = 0;
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        char digest_hex[65];
        for (int k = 0; k < 32; k++) snprintf(digest_hex + 2*k, 3, "%02x", sFixture[i].digest[k]);
        uint64_t ok = atomic_load(&sStats[i].loads_ok), es = atomic_load(&sStats[i].loads_estale);
        uint64_t to = atomic_load(&sStats[i].loads_timeout), er = atomic_load(&sStats[i].loads_error);
        int      le = atomic_load(&sStats[i].last_errno);
        loads_ok += ok; estale_total += es; to_total += to; err_total += er;
        dg_ok += atomic_load(&sStats[i].digests_ok); dg_bad += atomic_load(&sStats[i].digests_bad);
        printf("  %-14s %10zu  %s %10lu %8lu %8lu %7lu %10lu %9lu\n",
               strrchr(sFixture[i].path, '/') + 1, sFixture[i].size, digest_hex,
               (unsigned long)ok, (unsigned long)es, (unsigned long)to, (unsigned long)er,
               (unsigned long)atomic_load(&sStats[i].digests_ok),
               (unsigned long)atomic_load(&sStats[i].digests_bad));
        if (er) printf("    %-14s last error: %d (%s)\n", strrchr(sFixture[i].path, '/') + 1, le, strerror(le));
    }

    printf("\n  Workers: %d, %d s each\n", nt, d);
    printf("  %6s %10s %8s %8s %8s %9s %9s %9s %10s %11s %9s\n",
           "worker", "ops", "add", "get", "remove", "add_fail", "get_fail", "rm_fail", "size_mm", "digest_mm", "timeouts");
    for (int t = 0; t < nt; t++) {
        printf("  %6d %10lu %8lu %8lu %8lu %9lu %9lu %9lu %10lu %11lu %9lu\n", t,
               (unsigned long)atomic_load(&args[t].ops_total),
               (unsigned long)atomic_load(&args[t].ops_add),
               (unsigned long)atomic_load(&args[t].ops_get),
               (unsigned long)atomic_load(&args[t].ops_remove),
               (unsigned long)atomic_load(&args[t].add_failures),
               (unsigned long)atomic_load(&args[t].get_failures),
               (unsigned long)atomic_load(&args[t].remove_failures),
               (unsigned long)atomic_load(&args[t].size_mismatches),
               (unsigned long)atomic_load(&args[t].content_mismatches),
               (unsigned long)atomic_load(&args[t].get_timeouts));
    }

    printf("\n  Load outcomes: attempted %lu, returned %lu, stale %lu, timeouts %lu, errors %lu\n",
           (unsigned long)(loads_ok + estale_total + to_total + err_total), (unsigned long)loads_ok,
           (unsigned long)estale_total, (unsigned long)to_total, (unsigned long)err_total);
    printf("  Content verified: digests ok %lu, bad %lu; size mismatches %lu\n",
           (unsigned long)dg_ok, (unsigned long)dg_bad, (unsigned long)mism);
    printf("  Operations: %lu total (%.0f/s) - add %lu, get %lu, remove %lu; failures add %lu, get %lu, remove %lu\n",
           (unsigned long)total, (double)total/(double)d, (unsigned long)adds, (unsigned long)gets, (unsigned long)rems,
           (unsigned long)af, (unsigned long)gf, (unsigned long)rf);
    printf("  Zero-length reads: %lu\n", (unsigned long)emptygets);
    printf("  Service: registry size %u, heartbeat %lu, monitor thread %lu\n", FileLoaderServiceSize(flc.service_handle),
           (unsigned long)FileLoaderServiceHeartbeat(flc.service_handle), (unsigned long)mon);

    FileLoaderServiceStop(flc.service_handle);
    FileLoaderServiceDestroy(flc.service_handle);

    int leaked = sMappedFileCount(FILE_LOADER_CONCURRENT_FIXTURE_DIR);
    printf("  Mappings left after destroy: %d (must be 0)\n", leaked);

    uint64_t errs = gf + rf + mism + cmism + timeouts + (uint64_t)preflight_failures + (leaked > 0 ? 1u : 0u);
    printf("\n  VERDICT: %s\n", errs>0?"FAIL":total==0?"INCONCLUSIVE":"PASS");

    free(args); free(threads); free(files);
    return errs>0?1:0;
}
