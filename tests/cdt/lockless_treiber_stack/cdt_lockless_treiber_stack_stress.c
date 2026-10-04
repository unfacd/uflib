#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>

#define POOL_PER_PRODUCER 256

struct Node {
    struct LocklessTreiberStackNode base;
    _Atomic int                     busy;
    int                             ticket;
};

static double
sNowS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int
sWalk(struct LocklessTreiberStackNode *head, int total, unsigned char *seen, int *dup)
{
    int n = 0;
    while (head != NULL) {
        struct Node *node = (struct Node *)head;
        if (node->ticket < 0 || node->ticket >= total) {
            return -1;
        }
        if (seen[node->ticket]++) {
            (*dup)++;
        }
        n++;
        head = atomic_load_explicit(&head->next, memory_order_relaxed);
    }
    return n;
}

/* ── Scenario 1/2: push then drain, exact ticket accounting ─────────────── */

struct PushArg {
    LocklessTreiberStack *stack;
    struct Node          *nodes;
    int                   per;
    int                   first;
    _Atomic int          *accepted;
    _Atomic int          *fail;
};

static void *
sProducer(void *arg)
{
    struct PushArg *a = (struct PushArg *)arg;

    for (int i = 0; i < a->per; i++) {
        struct Node *n = &a->nodes[a->first + i];
        if (lockless_treiber_stack_push(a->stack, &n->base)) {
            atomic_fetch_add(a->accepted, 1);
        } else {
            atomic_store(a->fail, 1);
        }
    }
    return NULL;
}

static int
sRunPushDrain(int nprod, int per)
{
    const int total = nprod * per;
    LocklessTreiberStack *stack = lockless_treiber_stack_create();
    struct Node *nodes = calloc((size_t)total, sizeof(*nodes));
    unsigned char *seen = calloc((size_t)total, 1);
    pthread_t *th = calloc((size_t)nprod, sizeof(*th));
    struct PushArg *args = calloc((size_t)nprod, sizeof(*args));
    _Atomic int accepted = 0, fail = 0;
    int rc = 0, dup = 0;

    if (!stack || !nodes || !seen || !th || !args) {
        rc = 1;
        goto out;
    }

    for (int i = 0; i < total; i++) {
        lockless_treiber_stack_node_init(&nodes[i].base);
        nodes[i].ticket = i;
    }

    for (int p = 0; p < nprod; p++) {
        args[p].stack    = stack;
        args[p].nodes    = nodes;
        args[p].per      = per;
        args[p].first    = p * per;
        args[p].accepted = &accepted;
        args[p].fail     = &fail;
        pthread_create(&th[p], NULL, sProducer, &args[p]);
    }
    for (int p = 0; p < nprod; p++) {
        pthread_join(th[p], NULL);
    }

    int drained = sWalk(lockless_treiber_stack_steal_all(stack), total, seen, &dup);
    int missing = 0;
    for (int i = 0; i < total; i++) {
        if (seen[i] != 1) {
            missing++;
        }
    }

    if (drained != total || missing != 0 || dup != 0 || atomic_load(&fail) != 0 ||
        atomic_load(&accepted) != total) {
        rc = 1;
    }
    if (lockless_treiber_stack_steal_all(stack) != NULL) {
        rc = 1;
    }

    printf("[lf-stress:push-drain] P=%d n/P=%d accepted=%d drained=%d missing=%d dup=%d  %s\n",
           nprod, per, atomic_load(&accepted), drained, missing, dup, rc ? "FAIL" : "ok");

out:
    free(nodes);
    free(seen);
    free(th);
    free(args);
    lockless_treiber_stack_destroy(stack);
    return rc;
}

/* ── Scenario 3: producers pushing while one consumer drains ────────────── */

struct ConsArg {
    LocklessTreiberStack *stack;
    struct Node          *nodes;
    unsigned char        *seen;
    int                   total;
    _Atomic int          *drained;
    _Atomic int          *dup;
    _Atomic int          *bad;
    _Atomic int          *producers_done;
};

static void *
sConsumer(void *arg)
{
    struct ConsArg *a = (struct ConsArg *)arg;

    for (;;) {
        struct LocklessTreiberStackNode *head =
            lockless_treiber_stack_steal_all(a->stack);
        if (head == NULL) {
            if (atomic_load_explicit(a->producers_done, memory_order_acquire)) {
                head = lockless_treiber_stack_steal_all(a->stack);
                if (head == NULL) {
                    break;
                }
            } else {
                sched_yield();
                continue;
            }
        }
        int dup = 0;
        int n = sWalk(head, a->total, a->seen, &dup);
        if (n < 0) {
            atomic_fetch_add(a->bad, 1);
        } else {
            atomic_fetch_add(a->drained, n);
            atomic_fetch_add(a->dup, dup);
        }
    }
    return NULL;
}

static int
sRunConcurrent(int nprod, int per)
{
    const int total = nprod * per;
    LocklessTreiberStack *stack = lockless_treiber_stack_create();
    struct Node *nodes = calloc((size_t)total, sizeof(*nodes));
    unsigned char *seen = calloc((size_t)total, 1);
    pthread_t *th = calloc((size_t)nprod, sizeof(*th));
    struct PushArg *args = calloc((size_t)nprod, sizeof(*args));
    _Atomic int accepted = 0, fail = 0, drained = 0, dup = 0, bad = 0, done = 0;
    struct ConsArg cons;
    pthread_t consumer;
    int rc = 0;

    if (!stack || !nodes || !seen || !th || !args) {
        rc = 1;
        goto out;
    }
    for (int i = 0; i < total; i++) {
        lockless_treiber_stack_node_init(&nodes[i].base);
        nodes[i].ticket = i;
    }

    cons.stack          = stack;
    cons.nodes          = nodes;
    cons.seen           = seen;
    cons.total          = total;
    cons.drained        = &drained;
    cons.dup            = &dup;
    cons.bad            = &bad;
    cons.producers_done = &done;
    pthread_create(&consumer, NULL, sConsumer, &cons);

    for (int p = 0; p < nprod; p++) {
        args[p].stack    = stack;
        args[p].nodes    = nodes;
        args[p].per      = per;
        args[p].first    = p * per;
        args[p].accepted = &accepted;
        args[p].fail     = &fail;
        pthread_create(&th[p], NULL, sProducer, &args[p]);
    }
    for (int p = 0; p < nprod; p++) {
        pthread_join(th[p], NULL);
    }
    atomic_store_explicit(&done, 1, memory_order_release);
    pthread_join(consumer, NULL);

    int missing = 0;
    for (int i = 0; i < total; i++) {
        if (seen[i] != 1) {
            missing++;
        }
    }
    if (missing != 0 || atomic_load(&dup) != 0 || atomic_load(&bad) != 0 ||
        atomic_load(&drained) != total || atomic_load(&fail) != 0 ||
        lockless_treiber_stack_steal_all(stack) != NULL) {
        rc = 1;
    }

    printf("[lf-stress:concurrent] P=%d n/P=%d drained=%d missing=%d dup=%d  %s\n",
           nprod, per, atomic_load(&drained), missing, atomic_load(&dup), rc ? "FAIL" : "ok");

out:
    free(nodes);
    free(seen);
    free(th);
    free(args);
    lockless_treiber_stack_destroy(stack);
    return rc;
}

/* ── Scenario 4: producers racing a single closer ───────────────────────── */

struct CloseRaceArg {
    LocklessTreiberStack *stack;
    struct Node          *nodes;
    int                   idx;
    _Atomic int          *accepted;
};

static void *
sCloseRaceProducer(void *arg)
{
    struct CloseRaceArg *a = (struct CloseRaceArg *)arg;

    if (lockless_treiber_stack_push(a->stack, &a->nodes[a->idx].base)) {
        atomic_store(&a->accepted[a->idx], 1);
    }
    return NULL;
}

static int
sRunRaceClose(int nprod, int rounds)
{
    int rc = 0;

    for (int round = 0; round < rounds; round++) {
        LocklessTreiberStack *stack = lockless_treiber_stack_create();
        struct Node *nodes = calloc((size_t)nprod, sizeof(*nodes));
        _Atomic int *accepted = calloc((size_t)nprod, sizeof(*accepted));
        pthread_t *th = calloc((size_t)nprod, sizeof(*th));
        struct CloseRaceArg *args = calloc((size_t)nprod, sizeof(*args));

        if (!stack || !nodes || !accepted || !th || !args) {
            rc = 1;
            free(nodes);
            free(accepted);
            free(th);
            free(args);
            lockless_treiber_stack_destroy(stack);
            break;
        }

        for (int i = 0; i < nprod; i++) {
            lockless_treiber_stack_node_init(&nodes[i].base);
            nodes[i].ticket = i;
            atomic_store(&accepted[i], 0);
            args[i].stack    = stack;
            args[i].nodes    = nodes;
            args[i].idx      = i;
            args[i].accepted = accepted;
            pthread_create(&th[i], NULL, sCloseRaceProducer, &args[i]);
        }

        struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all_and_close(stack);

        for (int i = 0; i < nprod; i++) {
            pthread_join(th[i], NULL);
        }

        int in_chain = 0, accepted_count = 0, foreign = 0;
        while (head != NULL) {
            struct Node *node = (struct Node *)head;
            if (node->ticket < 0 || node->ticket >= nprod) {
                foreign++;
            } else if (atomic_load(&accepted[node->ticket]) != 1) {
                foreign++;
            }
            in_chain++;
            head = atomic_load_explicit(&head->next, memory_order_relaxed);
        }
        for (int i = 0; i < nprod; i++) {
            accepted_count += atomic_load(&accepted[i]);
        }

        if (in_chain != accepted_count || foreign != 0) {
            rc = 1;
            printf("[lf-stress:race-close] round=%d in_chain=%d accepted=%d foreign=%d FAIL\n",
                   round, in_chain, accepted_count, foreign);
        }
        if (lockless_treiber_stack_push(stack, &nodes[0].base)) {
            rc = 1;
            printf("[lf-stress:race-close] round=%d push after close accepted FAIL\n", round);
        }
        if (lockless_treiber_stack_steal_all(stack) != NULL) {
            rc = 1;
        }

        free(nodes);
        free(accepted);
        free(th);
        free(args);
        lockless_treiber_stack_destroy(stack);
    }

    printf("[lf-stress:race-close] P=%d rounds=%d  %s\n", nprod, rounds, rc ? "FAIL" : "ok");
    return rc;
}

/* ── Scenario 5: claim / release races ──────────────────────────────────── */

struct RaceArg {
    struct LocklessTreiberStackNode *node;
    _Atomic int                     *winners;
};

static void *
sClaimRacer(void *arg)
{
    struct RaceArg *a = (struct RaceArg *)arg;

    if (lockless_treiber_stack_claim(a->node)) {
        atomic_fetch_add(a->winners, 1);
    }
    return NULL;
}

static void *
sReleaseRacer(void *arg)
{
    struct RaceArg *a = (struct RaceArg *)arg;

    if (lockless_treiber_stack_release(a->node)) {
        atomic_fetch_add(a->winners, 1);
    }
    return NULL;
}

static int
sRunClaimRace(int ncons, int rounds)
{
    int rc = 0;

    const int releasers = ncons + 2;

    for (int round = 0; round < rounds; round++) {
        struct Node node;
        pthread_t *th = calloc((size_t)releasers, sizeof(*th));
        struct RaceArg arg;
        _Atomic int winners = 0;

        if (!th) {
            return 1;
        }

        lockless_treiber_stack_node_init(&node.base);
        arg.node    = &node.base;
        arg.winners = &winners;
        for (int i = 0; i < ncons; i++) {
            pthread_create(&th[i], NULL, sClaimRacer, &arg);
        }
        for (int i = 0; i < ncons; i++) {
            pthread_join(th[i], NULL);
        }
        if (atomic_load(&winners) != 1) {
            rc = 1;
            printf("[lf-stress:claim-race] round=%d winners=%d FAIL\n", round, atomic_load(&winners));
        }

        lockless_treiber_stack_node_init(&node.base);
        for (int i = 0; i < ncons; i++) {
            lockless_treiber_stack_node_retain(&node.base);
        }
        atomic_store(&winners, 0);
        for (int i = 0; i < releasers; i++) {
            pthread_create(&th[i], NULL, sReleaseRacer, &arg);
        }
        for (int i = 0; i < releasers; i++) {
            pthread_join(th[i], NULL);
        }
        if (atomic_load(&winners) != 1 ||
            atomic_load_explicit(&node.base.refcount, memory_order_relaxed) != 0) {
            rc = 1;
            printf("[lf-stress:release-race] round=%d last=%d refcount=%d FAIL\n",
                   round, atomic_load(&winners),
                   atomic_load_explicit(&node.base.refcount, memory_order_relaxed));
        }

        free(th);
    }

    printf("[lf-stress:claim-race] C=%d rounds=%d  %s\n", ncons, rounds, rc ? "FAIL" : "ok");
    return rc;
}

/* ── Scenario 6: sustained churn with slot recycling ────────────────────── */

struct ChurnArg {
    LocklessTreiberStack *stack;
    struct Node          *pool;
    double                deadline;
    _Atomic int          *pushed;
    _Atomic int          *drained;
    _Atomic int          *fail;
    _Atomic int          *producers_done;
};

static void *
sChurnProducer(void *arg)
{
    struct ChurnArg *a = (struct ChurnArg *)arg;
    int idx = 0;

    while (sNowS() < a->deadline) {
        struct Node *slot = &a->pool[idx % POOL_PER_PRODUCER];
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&slot->busy, &expected, 1,
                                                     memory_order_acq_rel,
                                                     memory_order_relaxed)) {
            sched_yield();
            continue;
        }
        lockless_treiber_stack_node_init(&slot->base);
        if (!lockless_treiber_stack_push(a->stack, &slot->base)) {
            atomic_store(&slot->busy, 0);
            atomic_store(a->fail, 1);
            break;
        }
        atomic_fetch_add(a->pushed, 1);
        idx++;
    }
    return NULL;
}

static void *
sChurnConsumer(void *arg)
{
    struct ChurnArg *a = (struct ChurnArg *)arg;

    for (;;) {
        struct LocklessTreiberStackNode *head =
            lockless_treiber_stack_steal_all(a->stack);
        if (head == NULL) {
            if (atomic_load_explicit(a->producers_done, memory_order_acquire)) {
                head = lockless_treiber_stack_steal_all(a->stack);
                if (head == NULL) {
                    break;
                }
            } else {
                sched_yield();
                continue;
            }
        }
        while (head != NULL) {
            struct Node *slot = (struct Node *)head;
            struct LocklessTreiberStackNode *next =
                atomic_load_explicit(&head->next, memory_order_relaxed);
            atomic_store(&slot->busy, 0);
            atomic_fetch_add(a->drained, 1);
            head = next;
        }
    }
    return NULL;
}

static int
sRunChurn(int nprod, double duration)
{
    LocklessTreiberStack *stack = lockless_treiber_stack_create();
    struct Node *pool = calloc((size_t)nprod * POOL_PER_PRODUCER, sizeof(*pool));
    pthread_t *th = calloc((size_t)nprod, sizeof(*th));
    struct ChurnArg *args = calloc((size_t)nprod, sizeof(*args));
    _Atomic int pushed = 0, drained = 0, fail = 0, done = 0;
    struct ChurnArg cons;
    pthread_t consumer;
    int rc = 0;

    if (!stack || !pool || !th || !args) {
        rc = 1;
        goto out;
    }

    for (int p = 0; p < nprod; p++) {
        args[p].stack          = stack;
        args[p].pool           = &pool[(size_t)p * POOL_PER_PRODUCER];
        args[p].deadline       = sNowS() + duration;
        args[p].pushed         = &pushed;
        args[p].drained        = &drained;
        args[p].fail           = &fail;
        args[p].producers_done = &done;
    }

    cons = args[0];
    cons.deadline = 0.0;
    pthread_create(&consumer, NULL, sChurnConsumer, &cons);

    for (int p = 0; p < nprod; p++) {
        pthread_create(&th[p], NULL, sChurnProducer, &args[p]);
    }
    for (int p = 0; p < nprod; p++) {
        pthread_join(th[p], NULL);
    }
    atomic_store_explicit(&done, 1, memory_order_release);
    pthread_join(consumer, NULL);

    int busy_left = 0;
    for (int i = 0; i < nprod * POOL_PER_PRODUCER; i++) {
        if (atomic_load(&pool[i].busy) != 0) {
            busy_left++;
        }
    }

    if (atomic_load(&fail) != 0 || atomic_load(&pushed) != atomic_load(&drained) ||
        busy_left != 0 || lockless_treiber_stack_steal_all(stack) != NULL) {
        rc = 1;
    }

    printf("[lf-stress:churn] threads=%d duration=%.1fs pushed=%d drained=%d busy_left=%d  %s\n",
           nprod + 1, duration, atomic_load(&pushed), atomic_load(&drained), busy_left,
           rc ? "FAIL" : "ok");

out:
    free(pool);
    free(th);
    free(args);
    lockless_treiber_stack_destroy(stack);
    return rc;
}

int
main(int argc, char **argv)
{
    int nprod = 4, ncons = 4, per = 5000;
    double duration = 3.0;

    static struct option long_opts[] = {
        {"prod",     required_argument, 0, 'p'},
        {"cons",     required_argument, 0, 'c'},
        {"n",        required_argument, 0, 'n'},
        {"duration", required_argument, 0, 'd'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:n:d:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': nprod    = atoi(optarg); break;
        case 'c': ncons    = atoi(optarg); break;
        case 'n': per      = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'h':
            fprintf(stderr,
                "Usage: %s --prod P --cons C --n N [--duration S]\n"
                "  push-drain    P producers push, single-threaded drain, exact accounting\n"
                "  concurrent    P producers push while one consumer drains\n"
                "  race-close    P producers race one closer; refused pushes are absent\n"
                "  claim-race    C threads race claim, then race release, on one node\n"
                "  churn         sustained push/drain for --duration with slot recycling\n",
                argv[0]);
            return 0;
        default:
            return 1;
        }
    }

    if (nprod < 1 || ncons < 1 || per < 1 || duration < 0.0) {
        fprintf(stderr, "error: prod/cons/n must be >= 1 and duration >= 0\n");
        return 1;
    }

    printf("=== LocklessTreiberStack Multi-threaded Stress ===\n");
    printf("  producers=%d consumers=%d ops/producer=%d duration=%.1fs\n\n",
           nprod, ncons, per, duration);

    int rc = 0;
    rc |= sRunPushDrain(1, 1000);
    rc |= sRunPushDrain(nprod, per);
    rc |= sRunConcurrent(nprod, per);
    rc |= sRunRaceClose(nprod, 200);
    rc |= sRunClaimRace(ncons, 256);
    rc |= sRunChurn(nprod, duration);

    printf("\n%s\n", rc ? "RESULT: FAIL" : "RESULT: PASS");
    return rc;
}
