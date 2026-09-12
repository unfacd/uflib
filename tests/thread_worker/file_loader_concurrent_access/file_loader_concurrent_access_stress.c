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
#define DEFAULT_FILES    100

typedef struct {
    char                     path[256];
    FileLoaderServiceHandle handle;
    bool                     registered;
    bool                     buffer_held;
} StressFileState;

typedef struct {
    FileLoaderService    *svc;
    StressFileState      *files;
    int                   num_files;
    _Atomic uint64_t      ops_total, ops_add, ops_get, ops_remove;
    _Atomic uint64_t      add_failures, get_failures, remove_failures;
    _Atomic bool         *stop;
} WorkerArgs;

static void sPrintUsage(const char *prog) {
    fprintf(stderr, "Usage: %s [--threads N] [--duration S] [--capacity C] [--files N]\n"
        "Defaults: %d threads, %ds, %u capacity, %d files\n",
        prog, DEFAULT_THREADS, DEFAULT_DURATION, DEFAULT_CAPACITY, DEFAULT_FILES);
}

static inline uint32_t sRand(uint64_t *state) {
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;
    return (uint32_t)(*state >> 32);
}

static void *sWorker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;
    uint64_t rng = (uint64_t)(uintptr_t)a + (uint64_t)time(NULL);
    FileLoaderServiceRegisterThread(a->svc);
    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        int fid = (int)(sRand(&rng) % (uint64_t)a->num_files);
        StressFileState *f = &a->files[fid];
        if (f->buffer_held) { f->buffer_held = false; continue; }
        int op = (int)(sRand(&rng) % 4);
        switch (op) {
        case 0: if (!f->registered) { FileLoaderServiceHandle h = FileLoaderServiceAddFile(a->svc, f->path);
            if (h) { f->handle = h; f->registered = true; atomic_fetch_add(&a->ops_add, 1); }
            else atomic_fetch_add(&a->add_failures, 1); } break;
        case 1: if (f->registered && !f->buffer_held) { size_t sz = 0;
            void *data = FileLoaderServiceGetFileContent(a->svc, f->handle, &sz);
            if (data) { f->buffer_held = true; atomic_fetch_add(&a->ops_get, 1); }
            else if (errno == ESTALE) { f->registered = false; f->handle = 0; }
            else if (errno != ETIMEDOUT) atomic_fetch_add(&a->get_failures, 1); } break;
        case 2: if (f->buffer_held) f->buffer_held = false; break;
        case 3: if (f->registered) { FileLoaderServiceRemoveFile(a->svc, f->path);
            f->registered = false; f->buffer_held = false; f->handle = 0;
            atomic_fetch_add(&a->ops_remove, 1); } break;
        }
        atomic_fetch_add(&a->ops_total, 1);
    }
    FileLoaderServiceUnregisterThread(a->svc);
    return NULL;
}

int main(int argc, char **argv) {
    int d = DEFAULT_DURATION; uint32_t cap = DEFAULT_CAPACITY;
    int nf = DEFAULT_FILES, nt = DEFAULT_THREADS;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"--duration") && i+1<argc) d = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--capacity") && i+1<argc) cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"--files") && i+1<argc) nf = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--threads") && i+1<argc) nt = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) { sPrintUsage(argv[0]); return 0; }
        else { fprintf(stderr,"Unknown: %s\n",argv[i]); return 1; }
    }
    if (d<1)d=1; if(cap<16)cap=16; if(nf<1)nf=1; if(nt<1)nt=1;
    printf("=== FileLoaderService Stress Test ===\n  Threads: %d  Duration: %ds  Capacity: %u  Files: %d\n", nt, d, cap, nf);

    StressFileState *files = (StressFileState *)calloc((size_t)nf, sizeof(*files));
    char tmpdir[] = "/tmp/fls_stress_XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); free(files); return 1; }
    for (int i = 0; i < nf; i++) {
        snprintf(files[i].path, sizeof(files[i].path), "%s/f_%d.txt", tmpdir, i);
        FILE *fp = fopen(files[i].path, "w");
        if (fp) { fprintf(fp, "stress test file %d\n", i); fclose(fp); }
    }

    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = cap;
    flc.service_config_params.loaded_size   = (cap < 64) ? cap/2 : 64;
    if (!FileLoaderServiceInit(&flc)) { fprintf(stderr,"Init failed\n"); free(files); return 1; }
    pthread_t mon = FileLoaderServiceGetThread(flc.service_handle);

    _Atomic bool stop = false;
    WorkerArgs *args = (WorkerArgs *)calloc((size_t)nt, sizeof(*args));
    pthread_t *threads = (pthread_t *)calloc((size_t)nt, sizeof(pthread_t));
    for (int t = 0; t < nt; t++) {
        args[t].svc = flc.service_handle; args[t].files = files;
        args[t].num_files = nf; args[t].stop = &stop;
        pthread_create(&threads[t], NULL, sWorker, &args[t]);
    }

    printf("Running %d seconds...\n", d);
    sleep((unsigned)d);
    atomic_store_explicit(&stop, true, memory_order_relaxed);
    for (int t = 0; t < nt; t++) pthread_join(threads[t], NULL);

    uint64_t total=0,adds=0,gets=0,rems=0,af=0,gf=0,rf=0;
    for (int t=0;t<nt;t++) {
        total+=atomic_load(&args[t].ops_total);adds+=atomic_load(&args[t].ops_add);
        gets+=atomic_load(&args[t].ops_get);rems+=atomic_load(&args[t].ops_remove);
        af+=atomic_load(&args[t].add_failures);gf+=atomic_load(&args[t].get_failures);
        rf+=atomic_load(&args[t].remove_failures);
    }
    printf("=== Results ===\n  Total: %lu ops (%.0f/s)\n  Add: %lu  Get: %lu  Remove: %lu\n",
        (unsigned long)total,(double)total/(double)d,(unsigned long)adds,(unsigned long)gets,(unsigned long)rems);
    printf("  Add failures: %lu  Get failures: %lu (must be 0)  Remove failures: %lu (must be 0)\n",
        (unsigned long)af,(unsigned long)gf,(unsigned long)rf);
    printf("  Size: %u  Heartbeat: %lu\n", FileLoaderServiceSize(flc.service_handle),
        (unsigned long)FileLoaderServiceHeartbeat(flc.service_handle));
    uint64_t errs=gf+rf;
    printf("  VERDICT: %s\n",errs>0?"FAIL":total==0?"INCONCLUSIVE":"PASS");

    FileLoaderServiceStop(flc.service_handle);
    pthread_join(mon, NULL);
    FileLoaderServiceDestroy(flc.service_handle);
    for (int i=0;i<nf;i++) unlink(files[i].path);
    rmdir(tmpdir); free(args); free(threads); free(files);
    return errs>0?1:0;
}
