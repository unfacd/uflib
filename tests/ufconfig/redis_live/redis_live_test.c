/**
 * @file redis_live_test.c
 * @brief End-to-end against a real Redis: write through the driver, read back
 *        through UfConfigLoad.
 *
 * Not registered with CTest, because it needs a Redis to talk to.  Run it
 * against any reachable instance:
 *
 *     ./ufconfig_redis_live [host] [port] [namespace]
 *     # defaults: 127.0.0.1 63791 ufcfgtest
 *
 * ## What it establishes
 *
 * That the driver interface is implementable from outside the library, and that
 * the whole chain works against a store the library has never seen:
 *
 *   UfConfigDescriptor.kind = REDIS
 *     -> UfConfigDriverFindKind
 *     -> the driver this test registered
 *     -> a real socket
 *     -> RESP
 *     -> back through UfConfigLoadPairs into a validated tree
 *
 * The namespace defaults to something no deployment uses, and every key it
 * creates is deleted on the way out — including on failure, so a failed run
 * does not leave a configuration behind for the next one to read as a pass.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <hiredis.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const UfConfigDriver g_ufconfig_driver_hiredis;

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

static redisContext *g_cleanup_conn;
static char          g_cleanup_key[512];

static void CleanupKey(void) {
    if (!g_cleanup_conn || !g_cleanup_key[0]) return;
    redisReply *r = redisCommand(g_cleanup_conn, "DEL %s", g_cleanup_key);
    freeReplyObject(r);
}

static void Fail(const char *what) {
    const UfConfigError *e = UfConfigLastError();
    fprintf(stderr, "FAIL: %s%s%s\n", what,
            (e && e->message[0]) ? " — " : "",
            (e && e->message[0]) ? e->message : "");
    CleanupKey();
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int         port = argc > 2 ? atoi(argv[2]) : 63791;
    const char *ns   = argc > 3 ? argv[3] : "ufcfgtest";
    const char *cfg  = "ufsrvwebsock";

    /* Supplied by CMake, because CTest runs from the build directory and a
       path relative to this source file would be wrong at run time. */
#ifndef UFCONFIG_TEST_DIR
#define UFCONFIG_TEST_DIR "."
#endif
    char document[1024];
    snprintf(document, sizeof(document), "%s/examples/sample.strict.lua",
             UFCONFIG_TEST_DIR);

    printf("ufconfig live Redis test\n");
    printf("  store          %s:%d\n", host, port);
    printf("  namespace      %s (config %s)\n", ns, cfg);

    /* A connection of our own, purely so the test can clean up after itself
       whatever happens to the handle. */
    g_cleanup_conn = redisConnect(host, port);
    if (!g_cleanup_conn || g_cleanup_conn->err) {
        fprintf(stderr, "FAIL: cannot reach %s:%d — %s\n", host, port,
                g_cleanup_conn ? g_cleanup_conn->errstr : "no context");
        return EXIT_FAILURE;
    }
    snprintf(g_cleanup_key, sizeof(g_cleanup_key), "{%s}:%s", ns, cfg);
    CleanupKey();

    /* ── Register the driver this test supplies ───────────────────────────── */
    UfConfigStatus st = UfConfigRegisterDriver(&g_ufconfig_driver_hiredis);
    printf("\nregister        %s", UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) { printf("\n"); Fail("UfConfigRegisterDriver"); }
    printf("  (as \"%s\")\n", g_ufconfig_driver_hiredis.caps.name);

    /* ── 1. Load from an empty store ──────────────────────────────────────── */
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_REDIS;
    d.redis.address     = host;
    d.redis.port        = port;
    d.redis.ns   = ns;
    d.redis.config_name = cfg;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) Fail("UfConfigCreate");

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = UF_CONFIG_LOAD_STRICT;

    st = UfConfigLoad(h, &opt, NULL);
    printf("\nload (empty)    %s\n", UfConfigStatusString(st));
    printf("  why           %s\n", UfConfigLastError()->message);
    if (st != UF_CONFIG_ERR_NOT_CONFIGURED) Fail("an empty store must report NOT_CONFIGURED");

    /* ── 2. Seed from a real document, through the driver ─────────────────── */
    UfConfig *seed = NULL;
    if (UfConfigCreate(&seed, &d) != UF_CONFIG_OK) Fail("UfConfigCreate (seed)");
    if (UfConfigLoadFile(seed, document, &opt, NULL) != UF_CONFIG_OK)
        Fail("loading the seed document");

    UfConfigFieldPair *pairs = NULL;
    size_t np = 0;
    UfConfigMeta meta;
    memset(&meta, 0, sizeof(meta));
    if (UfConfigFlattenHandle(seed, &pairs, &np, &meta) != UF_CONFIG_OK)
        Fail("flattening the seed handle");

    const UfConfigDriver *drv = UfConfigDriverFindKind(UF_CONFIG_BACKEND_REDIS);
    if (!drv) Fail("no driver registered for REDIS");

    UfConfigBackendSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = UF_CONFIG_BACKEND_REDIS;
    spec.u.redis.host = host;
    spec.u.redis.port = (unsigned)port;
    spec.u.redis.ns = ns;
    spec.u.redis.config_name = cfg;

    void *ctx = NULL;
    st = drv->open(&ctx, &spec);
    if (st != UF_CONFIG_OK) Fail("driver open");
    st = drv->write_all(ctx, pairs, np, &meta);
    if (st != UF_CONFIG_OK) { drv->close(ctx); Fail("driver write_all"); }
    drv->close(ctx);
    UfConfigFreePairs(pairs, np);
    UfConfigDestroy(seed);

    printf("\nseed            %zu fields written to %s\n", np, g_cleanup_key);

    /* ── 3. Load it back through the library ──────────────────────────────── */
    st = UfConfigLoad(h, &opt, NULL);
    printf("load            %s\n", UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) Fail("UfConfigLoad from a populated store");

    static const char *const probe[] = {
        "ufsrv.server_run_mode", "ufsrv.main_listener_port",
        "ufnet.db_backend.port", "ufsrvwebsock.max_frame_size",
    };
    printf("\nread back\n");
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        if (UfConfigGetField(h, probe[i], &v) != UF_CONFIG_OK) {
            printf("  %-32s MISSING\n", probe[i]);
            continue;
        }
        if (v.kind == UF_CONFIG_KIND_STRING)
            printf("  %-32s \"%.*s\"\n", probe[i], (int)v.as.str.len, v.as.str.ptr);
        else if (v.kind == UF_CONFIG_KIND_INT)
            printf("  %-32s %lld\n", probe[i], (long long)v.as.i);
        else
            printf("  %-32s (kind %d)\n", probe[i], (int)v.kind);
    }

    /* ── 4. The bytes really are in Redis ─────────────────────────────────── */
    redisReply *hl = redisCommand(g_cleanup_conn, "HLEN %s", g_cleanup_key);
    printf("\nHLEN %s  ->  %lld\n", g_cleanup_key,
           (hl && hl->type == REDIS_REPLY_INTEGER) ? hl->integer : -1);
    freeReplyObject(hl);

    /* ── 5. Replacing, not merging ────────────────────────────────────────── */
    /* A field the store holds but the document does not must disappear.

       It has to be injected out of band rather than omitted from a second
       document: every field this schema marks required has to be present in any
       document that loads at all, so there is no field a valid replacement
       could drop.  A sentinel written straight into the hash tests the property
       the driver actually claims — that write_all *replaces* the contents
       rather than merging into them. */
    redisReply *sentinel = redisCommand(g_cleanup_conn,
                                        "HSET %s ufsrv.sentinel_not_in_schema 1", g_cleanup_key);
    freeReplyObject(sentinel);

    redisReply *before = redisCommand(g_cleanup_conn, "HEXISTS %s ufsrv.sentinel_not_in_schema",
                                      g_cleanup_key);
    long had_it = (before && before->type == REDIS_REPLY_INTEGER) ? before->integer : 0;
    freeReplyObject(before);
    printf("\nsentinel placed  %s\n", had_it ? "yes" : "NO — test is not measuring anything");
    if (!had_it) Fail("could not place the sentinel");

    UfConfig *rewrite = NULL;
    if (UfConfigCreate(&rewrite, &d) != UF_CONFIG_OK) Fail("UfConfigCreate (rewrite)");
    if (UfConfigLoadFile(rewrite, document, &opt, NULL) != UF_CONFIG_OK)
        Fail("loading the document for the rewrite");

    pairs = NULL; np = 0;
    if (UfConfigFlattenHandle(rewrite, &pairs, &np, &meta) != UF_CONFIG_OK)
        Fail("flattening for the rewrite");
    ctx = NULL;
    if (drv->open(&ctx, &spec) != UF_CONFIG_OK) Fail("driver open (rewrite)");
    if (drv->write_all(ctx, pairs, np, &meta) != UF_CONFIG_OK) {
        drv->close(ctx); Fail("driver write_all (rewrite)");
    }
    drv->close(ctx);
    UfConfigFreePairs(pairs, np);
    UfConfigDestroy(rewrite);

    redisReply *after_sentinel = redisCommand(g_cleanup_conn,
                                              "HEXISTS %s ufsrv.sentinel_not_in_schema",
                                              g_cleanup_key);
    long still_there = (after_sentinel && after_sentinel->type == REDIS_REPLY_INTEGER)
                           ? after_sentinel->integer : -1;
    freeReplyObject(after_sentinel);

    redisReply *post = redisCommand(g_cleanup_conn, "HLEN %s", g_cleanup_key);
    long post_len = (post && post->type == REDIS_REPLY_INTEGER) ? post->integer : -1;
    freeReplyObject(post);

    printf("after rewrite    sentinel present=%s  HLEN=%ld\n",
           still_there ? "YES" : "no", post_len);
    if (still_there) Fail("write_all merged instead of replacing: a stale field survived");

    /* ── 6. Persist through the driver ────────────────────────────────────── */
    UfConfigValue knob;
    memset(&knob, 0, sizeof(knob));
    UfConfigSetInt(h, "testharn.knob_a", 3);
    st = UfConfigPersist(h);
    printf("\npersist         %s\n", UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) Fail("UfConfigPersist");

    redisReply *persisted = redisCommand(g_cleanup_conn, "HGET %s testharn.knob_a",
                                         g_cleanup_key);
    printf("  in redis      testharn.knob_a = %s\n",
           (persisted && persisted->type == REDIS_REPLY_STRING) ? persisted->str : "(absent)");
    int wrote = persisted && persisted->type == REDIS_REPLY_STRING;
    freeReplyObject(persisted);
    if (!wrote) Fail("persist did not reach the store");

    /* ── 7. Reload: unchanged, then changed ───────────────────────────────── */
    UfConfigReloadReport rr;
    memset(&rr, 0, sizeof(rr));
    st = UfConfigReload(h, &rr);
    printf("\nreload (same)   %s  entries=%zu\n", UfConfigStatusString(st), rr.count);
    if (st != UF_CONFIG_NO_CHANGE) Fail("a reload with nothing changed must report NO_CHANGE");

    /* Move the store underneath it, the way another instance would. */
    redisReply *bump = redisCommand(g_cleanup_conn,
                                    "HSET %s ufsrv.main_listener_port 1234", g_cleanup_key);
    freeReplyObject(bump);
    redisReply *bumpv = redisCommand(g_cleanup_conn, "HINCRBY %s #version 1", g_cleanup_key);
    freeReplyObject(bumpv);

    memset(&rr, 0, sizeof(rr));
    st = UfConfigReload(h, &rr);
    printf("reload (change) %s  entries=%zu\n", UfConfigStatusString(st), rr.count);
    if (st != UF_CONFIG_OK) Fail("reload after a store-side change");

    UfConfigValue moved;
    memset(&moved, 0, sizeof(moved));
    if (UfConfigGetField(h, "ufsrv.main_listener_port", &moved) != UF_CONFIG_OK)
        Fail("reading the field the store changed");
    printf("  picked up     ufsrv.main_listener_port = %lld\n", (long long)moved.as.i);
    if (moved.as.i != 1234) Fail("reload did not pick up the store's new value");

    UfConfigDestroy(h);
    CleanupKey();
    redisFree(g_cleanup_conn);

    printf("\nPASS — every key this test created has been removed\n");
    return EXIT_SUCCESS;
}
