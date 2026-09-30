/**
 * @file ufconfig_driver_hiredis.c
 * @brief A Redis driver that actually talks to Redis, supplied by the test.
 *
 * The driver compiled into the library is an in-process stand-in: an array of
 * structs, no socket.  This one speaks RESP over a real connection, and it is
 * written the way a consumer would write it — registering itself through
 * UfConfigRegisterDriver and implementing the seven operations.  That is the
 * point of the exercise: it establishes that the interface is implementable
 * from outside the library, against a store the library has never seen.
 *
 * ## Layout, matching the store format
 *
 * One hash per configuration, keyed `{namespace}:config_name` — the braces are
 * a Redis Cluster hash tag, so every field of one configuration lands on the
 * same slot and a transaction over them stays legal in cluster mode.
 *
 * Within the hash: field name is the dotted path, value is the canonical-form
 * scalar text.  Names beginning `#` are the driver's own metadata and never
 * cross the boundary as fields — the grammar cannot produce an identifier
 * starting with `#`, so the namespace cannot collide with a real field.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <hiredis.h>

#include <stdlib.h>
#include <string.h>

#define LIVE_KEY_PREFIX_FMT  "{%s}:%s"
#define LIVE_META_VERSION    "#version"
#define LIVE_META_DIGEST     "#digest"

typedef struct LiveCtx {
    redisContext *redis;
    char          key[512];
} LiveCtx;

/* The vtype is advisory — UfConfigLoadPairs re-derives the type from the
   canonical-form text — but a driver should still describe what it holds. */
static UfConfigPairType sVTypeOf(const char *encoded) {
    if (!encoded) return UF_PAIR_STRING;
    if (strcmp(encoded, "{}") == 0) return UF_PAIR_EMPTY_TABLE;
    if (strcmp(encoded, "true") == 0 || strcmp(encoded, "false") == 0) return UF_PAIR_BOOL;
    if (encoded[0] == '"' || encoded[0] == '\'') return UF_PAIR_STRING;
    if (strchr(encoded, '.')) return UF_PAIR_FLOAT;
    return UF_PAIR_INT;
}

static UfConfigStatus sLiveOpen(void **ctx, const UfConfigBackendSpec *spec) {
    if (!ctx || !spec) return UF_CONFIG_ERR_INVALID_ARG;

    const char *host = spec->u.redis.host ? spec->u.redis.host : "127.0.0.1";
    int port = spec->u.redis.port ? (int)spec->u.redis.port : 6379;

    LiveCtx *c = (LiveCtx *)calloc(1, sizeof(*c));
    if (!c) return UF_CONFIG_ERR_NO_MEMORY;

    struct timeval timeout = { 5, 0 };
    c->redis = redisConnectWithTimeout(host, port, timeout);
    if (!c->redis || c->redis->err) {
        free(c);
        return UF_CONFIG_ERR_BACKEND;
    }

    if (spec->u.redis.password) {
        redisReply *r = redisCommand(c->redis, "AUTH %s %s",
                                     spec->u.redis.username ? spec->u.redis.username : "default",
                                     spec->u.redis.password);
        int ok = r && r->type == REDIS_REPLY_STATUS;
        freeReplyObject(r);
        if (!ok) { redisFree(c->redis); free(c); return UF_CONFIG_ERR_BACKEND; }
    }

    if (spec->u.redis.db) {
        redisReply *r = redisCommand(c->redis, "SELECT %u", spec->u.redis.db);
        int ok = r && r->type == REDIS_REPLY_STATUS;
        freeReplyObject(r);
        if (!ok) { redisFree(c->redis); free(c); return UF_CONFIG_ERR_BACKEND; }
    }

    if (!spec->u.redis.ns || !spec->u.redis.config_name) {
        redisFree(c->redis); free(c); return UF_CONFIG_ERR_INVALID_ARG;
    }
    snprintf(c->key, sizeof(c->key), LIVE_KEY_PREFIX_FMT,
             spec->u.redis.ns, spec->u.redis.config_name);

    *ctx = c;
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveReadAll(void *ctx, UfConfigFieldPair **out, size_t *n,
                                   UfConfigMeta *meta) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c) return UF_CONFIG_ERR_INVALID_ARG;

    redisReply *exists = redisCommand(c->redis, "EXISTS %s", c->key);
    long present = (exists && exists->type == REDIS_REPLY_INTEGER) ? exists->integer : 0;
    freeReplyObject(exists);
    if (!present) return UF_CONFIG_ERR_NOT_CONFIGURED;

    redisReply *r = redisCommand(c->redis, "HGETALL %s", c->key);
    if (!r || r->type != REDIS_REPLY_ARRAY) {
        freeReplyObject(r);
        return UF_CONFIG_ERR_BACKEND;
    }

    size_t count = r->elements / 2;
    UfConfigFieldPair *pairs = (UfConfigFieldPair *)calloc(count ? count : 1, sizeof(*pairs));
    if (!pairs) { freeReplyObject(r); return UF_CONFIG_ERR_NO_MEMORY; }

    size_t k = 0;
    for (size_t i = 0; i + 1 < r->elements; i += 2) {
        const char *name = r->element[i]->str;
        const char *val  = r->element[i + 1]->str;
        if (!name) continue;

        /* Metadata stays on this side of the boundary. */
        if (name[0] == '#') {
            if (meta && strcmp(name, LIVE_META_VERSION) == 0 && val) {
                meta->version = strtoull(val, NULL, 10);
                meta->fmt = 1;
            } else if (meta && strcmp(name, LIVE_META_DIGEST) == 0 && val) {
                snprintf(meta->digest_hex, sizeof(meta->digest_hex), "%s", val);
                meta->is_digest_set = 1;
            }
            continue;
        }

        pairs[k].path    = strdup(name);
        pairs[k].encoded = strdup(val ? val : "");
        pairs[k].vtype   = sVTypeOf(val);
        k++;
    }
    freeReplyObject(r);

    *out = pairs;
    *n = k;
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveWriteAll(void *ctx, const UfConfigFieldPair *in, size_t n,
                                    const UfConfigMeta *meta) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c) return UF_CONFIG_ERR_INVALID_ARG;

    /* MULTI/EXEC so a reader never observes a half-replaced configuration.
       DEL then HSET inside the transaction is what makes this a *replace*
       rather than a merge: a field the document no longer carries must not
       survive in the store. */
    redisReply *m = redisCommand(c->redis, "MULTI");
    int ok = m && m->type == REDIS_REPLY_STATUS;
    freeReplyObject(m);
    if (!ok) return UF_CONFIG_ERR_BACKEND;

    redisReply *d = redisCommand(c->redis, "DEL %s", c->key);
    freeReplyObject(d);

    for (size_t i = 0; i < n; i++) {
        if (!in[i].path) continue;
        redisReply *h = redisCommand(c->redis, "HSET %s %s %s", c->key,
                                     in[i].path, in[i].encoded ? in[i].encoded : "");
        freeReplyObject(h);
    }

    if (meta && meta->is_digest_set && meta->digest_hex[0]) {
        redisReply *h = redisCommand(c->redis, "HSET %s %s %s", c->key,
                                     LIVE_META_DIGEST, meta->digest_hex);
        freeReplyObject(h);
    }

    /* A driver declaring native_version has to maintain one, or the core's
       cheap change test has nothing to compare against and every reload reads
       the whole store. */
    redisReply *v = redisCommand(c->redis, "HINCRBY %s %s 1", c->key, LIVE_META_VERSION);
    freeReplyObject(v);

    redisReply *e = redisCommand(c->redis, "EXEC");
    ok = e && e->type == REDIS_REPLY_ARRAY;
    freeReplyObject(e);
    return ok ? UF_CONFIG_OK : UF_CONFIG_ERR_BACKEND;
}

static UfConfigStatus sLiveReadOne(void *ctx, const char *path, UfConfigFieldPair *out) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !path || !out) return UF_CONFIG_ERR_INVALID_ARG;

    redisReply *r = redisCommand(c->redis, "HGET %s %s", c->key, path);
    if (!r || r->type == REDIS_REPLY_NIL) { freeReplyObject(r); return UF_CONFIG_ERR_NOFIELD; }
    if (r->type != REDIS_REPLY_STRING) { freeReplyObject(r); return UF_CONFIG_ERR_BACKEND; }

    out->path    = strdup(path);
    out->encoded = strdup(r->str);
    out->vtype   = sVTypeOf(r->str);
    freeReplyObject(r);
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveWriteOne(void *ctx, const char *path, const UfConfigFieldPair *in) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !path || !in) return UF_CONFIG_ERR_INVALID_ARG;

    redisReply *r = redisCommand(c->redis, "HSET %s %s %s", c->key, path,
                                 in->encoded ? in->encoded : "");
    int ok = r && (r->type == REDIS_REPLY_INTEGER);
    freeReplyObject(r);
    if (!ok) return UF_CONFIG_ERR_BACKEND;

    redisReply *b = redisCommand(c->redis, "HINCRBY %s %s 1", c->key, LIVE_META_VERSION);
    freeReplyObject(b);
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveStat(void *ctx, UfConfigStamp *out) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !out) return UF_CONFIG_ERR_INVALID_ARG;

    redisReply *r = redisCommand(c->redis, "HGET %s %s", c->key, LIVE_META_VERSION);
    out->token = (r && r->type == REDIS_REPLY_STRING) ? strtoull(r->str, NULL, 10) : 0;
    freeReplyObject(r);
    return UF_CONFIG_OK;
}

static void sLiveClose(void *ctx) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c) return;
    if (c->redis) redisFree(c->redis);
    free(c);
}

const UfConfigDriver g_ufconfig_driver_hiredis = {
    .caps = {
        .api_version       = UF_CONFIG_DRIVER_API_VERSION,
        .kind              = UF_CONFIG_BACKEND_REDIS,
        .atomic_write_all  = true,
        .atomic_write_one  = true,
        .per_field_read    = true,
        .native_version    = true,
        .enumeration       = false,
        .multi_config_txn  = false,
        .write_one_creates = true,
        .max_path_bytes    = 512,
        .max_value_bytes   = 0,
        .max_fields        = 0,
        .name              = "redis-live",
    },
    .open      = sLiveOpen,
    .read_all  = sLiveReadAll,
    .write_all = sLiveWriteAll,
    .read_one  = sLiveReadOne,
    .write_one = sLiveWriteOne,
    .stat      = sLiveStat,
    .close     = sLiveClose,
};
