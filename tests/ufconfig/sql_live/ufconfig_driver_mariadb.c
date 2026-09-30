/**
 * @file ufconfig_driver_mariadb.c
 * @brief A SQL driver that actually talks to MariaDB, supplied by the test.
 *
 * The driver compiled into the library reaches libmariadb only when the library
 * is configured with UFLIB_CAPABILITY_SQL, and this file exists so that the *core*
 * can be exercised against a real server without that option: it is written the
 * way a consumer would write it — registering itself through
 * UfConfigRegisterDriver and implementing the seven operations — against a
 * store the library has never seen.
 *
 * That is the whole point of the exercise.  This is not the library's driver
 * and does not pretend to be: it is a second implementation of the same
 * interface, which is what makes it evidence that the interface is
 * implementable from outside.
 *
 * ## Layout, matching the DDL
 *
 * The three tables in `src/ufconfig/driver/ufconfig_mariadb.sql`, which this
 * test applies to the database itself: one namespace row, one config row per
 * (namespace, config_name), and one entry row per flattened field keyed
 * `(config_id, path)`.  Applying the shipped DDL rather than a private copy is
 * deliberate — it is the only thing in the tree that executes that file, so the
 * schema a deployment applies is the schema this test has passed against.
 *
 * Statements are built with mysql_real_escape_string rather than the prepared
 * statement API.  Its Redis twin builds commands the same way, with
 * printf-style arguments, and a test harness that spends its length on binding
 * boilerplate is a harness nobody reads.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <mariadb/mysql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_NS_MAX   191
#define LIVE_NAME_MAX 191
#define LIVE_PATH_MAX 512

typedef struct LiveDsn {
    char     host[256];
    char     user[128];
    char     pass[128];
    char     db[128];
    char     sock[256];
    unsigned port;
} LiveDsn;

typedef struct LiveCtx {
    MYSQL *db;
    char   ns[LIVE_NS_MAX + 1];
    char   cfg[LIVE_NAME_MAX + 1];
} LiveCtx;

/* The spec's DSN is the key/value form: host=…;port=…;user=…;password=…;
   database=…;socket=…  Anything unrecognised is ignored rather than refused, so
   a caller may pass the same string it gives the library's own driver. */
static int sParseDsn(LiveDsn *d, const char *s) {
    memset(d, 0, sizeof(*d));
    if (!s || !*s) return 0;
    d->port = 3306;

    char *copy = strdup(s);
    if (!copy) return 0;

    char *save = NULL;
    for (char *it = strtok_r(copy, ";", &save); it; it = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(it, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (strcmp(it, "host") == 0) snprintf(d->host, sizeof(d->host), "%s", eq);
        else if (strcmp(it, "user") == 0) snprintf(d->user, sizeof(d->user), "%s", eq);
        else if (strcmp(it, "password") == 0 || strcmp(it, "pass") == 0) snprintf(d->pass, sizeof(d->pass), "%s", eq);
        else if (strcmp(it, "database") == 0 || strcmp(it, "db") == 0) snprintf(d->db, sizeof(d->db), "%s", eq);
        else if (strcmp(it, "socket") == 0) snprintf(d->sock, sizeof(d->sock), "%s", eq);
        else if (strcmp(it, "port") == 0) {
            unsigned long p = strtoul(eq, NULL, 10);
            if (p && p <= 65535) d->port = (unsigned)p;
        }
    }
    free(copy);
    return d->host[0] || d->sock[0];
}

/* Doubled worst case, which is what mysql_real_escape_string can produce. */
static char *sEscape(MYSQL *db, const char *s) {
    size_t n = strlen(s);
    char  *p = (char *)malloc(n * 2 + 1);
    if (!p) return NULL;
    unsigned long m = mysql_real_escape_string(db, p, s, (unsigned long)n);
    p[m] = '\0';
    return p;
}

static int sExec(MYSQL *db, const char *sql) { return mysql_query(db, sql) == 0; }

/* Every failure path below reports the server's own message.  A driver that
   returns BACKEND and says nothing costs whoever runs the harness an hour of
   bisecting statements — which is exactly what happened the first time this
   was run. */
static UfConfigStatus sBackendError(LiveCtx *c, const char *where) {
    fprintf(stderr, "    [sql-live] %s: %u: %s\n", where, mysql_errno(c->db), mysql_error(c->db));
    return UF_CONFIG_ERR_BACKEND;
}

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

static void sHexEncode(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

/* 1 when the configuration exists, 0 when it does not, -1 on a backend error.
   The three-way return is what lets read_all tell "never written" from
   "unreachable", which the core reports as NOT_CONFIGURED and BACKEND. */
static int sFindConfig(LiveCtx *c, unsigned long long *id, unsigned long long *version,
                       char digest_hex[65], int *digest_set) {
    char *ns = sEscape(c->db, c->ns);
    char *cf = sEscape(c->db, c->cfg);
    if (!ns || !cf) { free(ns); free(cf); return -1; }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT c.config_id,c.version,c.digest FROM ufconfig_config c "
             "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
             "WHERE n.namespace_name='%s' AND c.config_name='%s'", ns, cf);
    free(ns);
    free(cf);

    if (!sExec(c->db, sql)) return -1;
    MYSQL_RES *res = mysql_store_result(c->db);
    if (!res) return -1;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return 0; }

    /* Both out-params are optional: `stat` wants only the version and
       `read_one` only the id, so neither may be written unconditionally. */
    if (id) *id = strtoull(row[0], NULL, 10);
    if (version) *version = row[1] ? strtoull(row[1], NULL, 10) : 0;
    if (digest_set) {
        unsigned long *lens = mysql_fetch_lengths(res);
        *digest_set = (row[2] && lens && lens[2] == 32);
        if (*digest_set && digest_hex) sHexEncode((const unsigned char *)row[2], 32, digest_hex);
    }
    mysql_free_result(res);
    return 1;
}

/* The namespace is an identity, not a value: it is created if absent and the
   existing id is returned if not.  LAST_INSERT_ID(expr) is what makes the
   second case return the row it found rather than zero — the library's own
   driver relies on the same idiom, so this exercises it too. */
static int sEnsureConfig(LiveCtx *c, const UfConfigMeta *meta, unsigned long long *out_id) {
    char *ns = sEscape(c->db, c->ns);
    char *cf = sEscape(c->db, c->cfg);
    if (!ns || !cf) { free(ns); free(cf); return 0; }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO ufconfig_namespace(namespace_name) VALUES('%s') "
             "ON DUPLICATE KEY UPDATE namespace_id=LAST_INSERT_ID(namespace_id)", ns);
    if (!sExec(c->db, sql)) { fprintf(stderr, "    [sql-live] ensure/namespace insert: %u: %s\n", mysql_errno(c->db), mysql_error(c->db)); free(ns); free(cf); return 0; }
    unsigned long long ns_id = (unsigned long long)mysql_insert_id(c->db);
    if (!ns_id) {
        snprintf(sql, sizeof(sql),
                 "SELECT namespace_id FROM ufconfig_namespace WHERE namespace_name='%s'", ns);
        if (!sExec(c->db, sql)) { free(ns); free(cf); return 0; }
        MYSQL_RES *res = mysql_store_result(c->db);
        MYSQL_ROW row = res ? mysql_fetch_row(res) : NULL;
        if (!row) { if (res) mysql_free_result(res); free(ns); free(cf); return 0; }
        ns_id = strtoull(row[0], NULL, 10);
        mysql_free_result(res);
    }
    free(ns);

    unsigned long long schema_hash = meta ? meta->schema_hash : 0;
    snprintf(sql, sizeof(sql),
             "INSERT INTO ufconfig_config(namespace_id,config_name,format_version,version,schema_hash) "
             "VALUES(%llu,'%s',1,0,%llu) "
             "ON DUPLICATE KEY UPDATE config_id=LAST_INSERT_ID(config_id)",
             ns_id, cf, schema_hash);
    free(cf);
    if (!sExec(c->db, sql)) { fprintf(stderr, "    [sql-live] ensure/config insert: %u: %s\n", mysql_errno(c->db), mysql_error(c->db)); return 0; }

    *out_id = (unsigned long long)mysql_insert_id(c->db);
    return *out_id != 0;
}

static UfConfigStatus sLiveOpen(void **ctx, const UfConfigBackendSpec *spec) {
    if (!ctx || !spec) return UF_CONFIG_ERR_INVALID_ARG;
    if (!spec->u.sql.dsn || !spec->u.sql.ns || !spec->u.sql.config_name)
        return UF_CONFIG_ERR_INVALID_ARG;

    LiveDsn d;
    if (!sParseDsn(&d, spec->u.sql.dsn)) return UF_CONFIG_ERR_INVALID_ARG;

    LiveCtx *c = (LiveCtx *)calloc(1, sizeof(*c));
    if (!c) return UF_CONFIG_ERR_NO_MEMORY;
    snprintf(c->ns, sizeof(c->ns), "%s", spec->u.sql.ns);
    snprintf(c->cfg, sizeof(c->cfg), "%s", spec->u.sql.config_name);

    c->db = mysql_init(NULL);
    if (!c->db) { free(c); return UF_CONFIG_ERR_NO_MEMORY; }
    if (!mysql_real_connect(c->db, d.host[0] ? d.host : NULL,
                            d.user[0] ? d.user : NULL,
                            d.pass[0] ? d.pass : NULL,
                            d.db[0] ? d.db : NULL,
                            d.port, d.sock[0] ? d.sock : NULL, 0)) {
        mysql_close(c->db);
        free(c);
        return UF_CONFIG_ERR_BACKEND;
    }
    *ctx = c;
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveReadAll(void *ctx, UfConfigFieldPair **out, size_t *n,
                                   UfConfigMeta *meta) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !out || !n) return UF_CONFIG_ERR_INVALID_ARG;
    *out = NULL;
    *n   = 0;
    if (meta) memset(meta, 0, sizeof(*meta));

    unsigned long long id = 0, version = 0;
    char               digest_hex[65] = {0};
    int                digest_set = 0;
    int found = sFindConfig(c, &id, &version, digest_hex, &digest_set);
    if (found < 0) return UF_CONFIG_ERR_BACKEND;
    if (!found) return UF_CONFIG_ERR_NOT_CONFIGURED;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT path,vtype,value FROM ufconfig_entry WHERE config_id=%llu ORDER BY path", id);
    if (!sExec(c->db, sql)) return UF_CONFIG_ERR_BACKEND;
    MYSQL_RES *res = mysql_store_result(c->db);
    if (!res) return UF_CONFIG_ERR_BACKEND;

    my_ulonglong rows = mysql_num_rows(res);
    UfConfigFieldPair *pairs = (UfConfigFieldPair *)calloc(rows ? (size_t)rows : 1, sizeof(*pairs));
    if (!pairs) { mysql_free_result(res); return UF_CONFIG_ERR_NO_MEMORY; }

    size_t k = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        unsigned long *lens = mysql_fetch_lengths(res);
        if (!row[0] || !lens) break;
        if (lens[0] > LIVE_PATH_MAX) {
            UfConfigFreePairs(pairs, k);
            mysql_free_result(res);
            return UF_CONFIG_ERR_LIMIT_EXCEEDED;
        }
        pairs[k].path    = strdup(row[0]);
        pairs[k].encoded = strdup(row[2] ? row[2] : "");
        if (!pairs[k].path || !pairs[k].encoded) {
            free((void *)pairs[k].path);
            free((void *)pairs[k].encoded);
            UfConfigFreePairs(pairs, k);
            mysql_free_result(res);
            return UF_CONFIG_ERR_NO_MEMORY;
        }
        pairs[k].vtype = (UfConfigPairType)strtoul(row[1] ? row[1] : "0", NULL, 10);
        if (pairs[k].vtype != UF_PAIR_INT && pairs[k].vtype != UF_PAIR_FLOAT &&
            pairs[k].vtype != UF_PAIR_BOOL && pairs[k].vtype != UF_PAIR_STRING &&
            pairs[k].vtype != UF_PAIR_EMPTY_TABLE)
            pairs[k].vtype = sVTypeOf(pairs[k].encoded);
        k++;
    }
    mysql_free_result(res);

    *out = pairs;
    *n   = k;
    if (meta) {
        meta->fmt      = 1;
        meta->version  = version;
        if (digest_set) {
            memcpy(meta->digest_hex, digest_hex, sizeof(digest_hex));
            meta->is_digest_set = 1;
        }
    }
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveWriteAll(void *ctx, const UfConfigFieldPair *in, size_t n,
                                    const UfConfigMeta *meta) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c) return UF_CONFIG_ERR_INVALID_ARG;

    /* A transaction, so a reader never observes a half-replaced configuration.
       DELETE then INSERT is what makes this a *replace* rather than a merge: a
       field the document no longer carries must not survive in the store. */
    if (!sExec(c->db, "START TRANSACTION")) return UF_CONFIG_ERR_BACKEND;

    unsigned long long id = 0;
    if (!sEnsureConfig(c, meta, &id)) { mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND; }

    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM ufconfig_entry WHERE config_id=%llu", id);
    if (!sExec(c->db, sql)) { mysql_rollback(c->db); return sBackendError(c, "write_all/delete"); }

    for (size_t i = 0; i < n; i++) {
        if (!in[i].path) continue;
        char *p = sEscape(c->db, in[i].path);
        char *v = sEscape(c->db, in[i].encoded ? in[i].encoded : "");
        if (!p || !v) { free(p); free(v); mysql_rollback(c->db); return UF_CONFIG_ERR_NO_MEMORY; }

        char isql[8192];
        /* snprintf truncates silently, and a truncated INSERT is either a
           syntax error or — worse — a shortened value written without
           complaint.  The caps above bound the path but not the value, so the
           only safe thing is to refuse what will not fit. */
        int wrote = snprintf(isql, sizeof(isql),
                 "INSERT INTO ufconfig_entry(config_id,path,vtype,value) VALUES(%llu,'%s',%u,'%s') "
                 "ON DUPLICATE KEY UPDATE vtype=VALUES(vtype),value=VALUES(value)",
                 id, p, (unsigned)in[i].vtype, v);
        free(p);
        free(v);
        if (wrote < 0 || (size_t)wrote >= sizeof(isql)) {
            fprintf(stderr, "    [sql-live] write_all/entry %zu: statement needs %d bytes\n", i, wrote);
            mysql_rollback(c->db);
            return UF_CONFIG_ERR_LIMIT_EXCEEDED;
        }
        if (!sExec(c->db, isql)) {
            fprintf(stderr, "    [sql-live] write_all/entry %zu (path %s): %u: %s\n",
                    i, in[i].path, mysql_errno(c->db), mysql_error(c->db));
            mysql_rollback(c->db);
            return UF_CONFIG_ERR_BACKEND;
        }
    }

    /* The digest is the core's, not the driver's: it is carried through, and
       cleared when the caller has none.  The version is the driver's, because
       it declares native_version — a cheap change test needs something to
       compare against. */
    if (meta && meta->is_digest_set && meta->digest_hex[0]) {
        char *dh = sEscape(c->db, meta->digest_hex);
        if (!dh) { mysql_rollback(c->db); return UF_CONFIG_ERR_NO_MEMORY; }
        snprintf(sql, sizeof(sql),
                 "UPDATE ufconfig_config SET version=version+1,digest=UNHEX('%s') WHERE config_id=%llu",
                 dh, id);
        free(dh);
    }
    else {
        snprintf(sql, sizeof(sql),
                 "UPDATE ufconfig_config SET version=version+1,digest=NULL WHERE config_id=%llu", id);
    }
    if (!sExec(c->db, sql)) { mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND; }

    if (!sExec(c->db, "COMMIT")) { mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND; }
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveReadOne(void *ctx, const char *path, UfConfigFieldPair *out) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !path || !out) return UF_CONFIG_ERR_INVALID_ARG;

    unsigned long long id = 0;
    int found = sFindConfig(c, &id, NULL, NULL, NULL);
    if (found < 0) return UF_CONFIG_ERR_BACKEND;
    if (!found) return UF_CONFIG_ERR_NOFIELD;

    char *p = sEscape(c->db, path);
    if (!p) return UF_CONFIG_ERR_NO_MEMORY;
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT path,vtype,value FROM ufconfig_entry WHERE config_id=%llu AND path='%s'", id, p);
    free(p);
    if (!sExec(c->db, sql)) return UF_CONFIG_ERR_BACKEND;

    MYSQL_RES *res = mysql_store_result(c->db);
    if (!res) return UF_CONFIG_ERR_BACKEND;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return UF_CONFIG_ERR_NOFIELD; }

    out->path    = strdup(row[0]);
    out->encoded = strdup(row[2] ? row[2] : "");
    out->vtype   = (UfConfigPairType)strtoul(row[1] ? row[1] : "0", NULL, 10);
    mysql_free_result(res);
    if (!out->path || !out->encoded) {
        free((void *)out->path);
        free((void *)out->encoded);
        return UF_CONFIG_ERR_NO_MEMORY;
    }
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveWriteOne(void *ctx, const char *path, const UfConfigFieldPair *in) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !path || !in) return UF_CONFIG_ERR_INVALID_ARG;

    if (!sExec(c->db, "START TRANSACTION")) return UF_CONFIG_ERR_BACKEND;

    unsigned long long id = 0;
    if (!sEnsureConfig(c, NULL, &id)) { mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND; }

    char *p = sEscape(c->db, path);
    char *v = sEscape(c->db, in->encoded ? in->encoded : "");
    if (!p || !v) { free(p); free(v); mysql_rollback(c->db); return UF_CONFIG_ERR_NO_MEMORY; }

    char sql[8192];
    int  wrote = snprintf(sql, sizeof(sql),
             "INSERT INTO ufconfig_entry(config_id,path,vtype,value) VALUES(%llu,'%s',%u,'%s') "
             "ON DUPLICATE KEY UPDATE vtype=VALUES(vtype),value=VALUES(value)",
             id, p, (unsigned)in->vtype, v);
    free(p);
    free(v);
    if (wrote < 0 || (size_t)wrote >= sizeof(sql)) {
        mysql_rollback(c->db);
        return UF_CONFIG_ERR_LIMIT_EXCEEDED;
    }
    if (!sExec(c->db, sql)) { mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND; }

    snprintf(sql, sizeof(sql), "UPDATE ufconfig_config SET version=version+1 WHERE config_id=%llu", id);
    if (!sExec(c->db, sql) || !sExec(c->db, "COMMIT")) {
        mysql_rollback(c->db);
        return UF_CONFIG_ERR_BACKEND;
    }
    return UF_CONFIG_OK;
}

static UfConfigStatus sLiveStat(void *ctx, UfConfigStamp *out) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c || !out) return UF_CONFIG_ERR_INVALID_ARG;

    unsigned long long version = 0;
    int found = sFindConfig(c, NULL, &version, NULL, NULL);
    if (found < 0) return UF_CONFIG_ERR_BACKEND;
    if (!found) return UF_CONFIG_ERR_NOT_CONFIGURED;

    out->token = version;
    return UF_CONFIG_OK;
}

static void sLiveClose(void *ctx) {
    LiveCtx *c = (LiveCtx *)ctx;
    if (!c) return;
    if (c->db) mysql_close(c->db);
    free(c);
}

const UfConfigDriver g_ufconfig_driver_mariadb = {
    .caps = {
        .api_version       = UF_CONFIG_DRIVER_API_VERSION,
        .kind              = UF_CONFIG_BACKEND_SQL,
        .atomic_write_all  = true,
        .atomic_write_one  = true,
        .per_field_read    = true,
        .native_version    = true,
        .enumeration       = false,
        .multi_config_txn  = false,
        .write_one_creates = true,
        .max_path_bytes    = LIVE_PATH_MAX,
        .max_value_bytes   = 0,
        .max_fields        = 0,
        .name              = "sql-live",
    },
    .open      = sLiveOpen,
    .read_all  = sLiveReadAll,
    .write_all = sLiveWriteAll,
    .read_one  = sLiveReadOne,
    .write_one = sLiveWriteOne,
    .stat      = sLiveStat,
    .close     = sLiveClose,
};
