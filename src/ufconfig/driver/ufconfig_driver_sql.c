/**
 * @file ufconfig_driver_sql.c
 * @brief MariaDB SQL driver for the ufconfig driver API.
 *
 * The store identity is (namespace, config_name).  SQL keeps that identity in
 * one central configuration table and puts the flattened fields in a single
 * child table keyed by config_id.  This keeps the hot lookups indexable without
 * turning every field into another normalized relation.
 *
 * DSN forms accepted by this driver:
 *   mariadb://user:password@host:3306/database
 *   mysql://user:password@host:3306/database
 *   key/value DSN: host=...;user=...;password=...;database=...;port=...;socket=...
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _POSIX_C_SOURCE 200809L
#include <uflib/ufconfig/ufconfig.h>

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

/* Built only when the SQL capability is on.  One macro, not two: the
   capability is declared in CMake beside the option, substituted into
   config_uflib.h as a `#cmakedefine`, and the connector is resolved with
   find_package behind that same capability — so a tree in which this macro is
   1 is a tree in which mariadb/mysql.h is known to be reachable.
 *
 * The guard sits *above* the client header deliberately: ufconfig_bench compiles
   this file by hand, with no /opt/include/mariadb on its include path, and a
   translation unit that reaches it there must compile to nothing rather than
   failing on a header far from the capability that would have provided it. */
#if UFLIB_CAPABILITY_SQL

#include <mariadb/mysql.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SQL_NS_MAX       191
#define SQL_NAME_MAX     191
#define SQL_PATH_MAX     512
#define SQL_DIGEST_BYTES 32

#define SQL_SCHEMA_TABLE   "ufconfig_namespace"
#define SQL_CONFIG_TABLE   "ufconfig_config"
#define SQL_ENTRY_TABLE    "ufconfig_entry"

/* The schema is deliberately fixed by the driver.  Values are always bound as
   parameters; these names are never derived from caller input. */
static const char *SQL_FIND_CONFIG =
    "SELECT c.config_id,c.format_version,c.version,c.schema_hash,c.digest "
    "FROM ufconfig_config c "
    "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
    "WHERE n.namespace_name=? AND c.config_name=?";

static const char *SQL_FIND_NAMESPACE =
    "SELECT namespace_id FROM ufconfig_namespace WHERE namespace_name=?";

static const char *SQL_CREATE_NAMESPACE =
    "INSERT INTO ufconfig_namespace(namespace_name) VALUES(?) "
    "ON DUPLICATE KEY UPDATE namespace_id=LAST_INSERT_ID(namespace_id)";

static const char *SQL_CREATE_CONFIG =
    "INSERT INTO ufconfig_config(namespace_id,config_name,format_version,version,schema_hash,digest) "
    "VALUES(?,?,1,0,?,?) "
    "ON DUPLICATE KEY UPDATE config_id=LAST_INSERT_ID(config_id)";

static const char *SQL_DELETE_ENTRIES =
    "DELETE FROM ufconfig_entry WHERE config_id=?";

/* Last-wins on a repeated path.  The core does not guarantee the flattened set
   is duplicate-free, and `(config_id, path)` is the primary key, so a plain
   INSERT fails the whole write with error 1062 the first time a path repeats —
   observed with this module's own fixture, where `ufsrv.server_id` is passed
   to the driver twice even though the document declares it once.  Redis gets
   this behaviour free from HSET; here it has to be asked for.  `write_one`
   already did it this way, so the two paths now agree. */
static const char *SQL_INSERT_ENTRY =
    "INSERT INTO ufconfig_entry(config_id,path,vtype,value) VALUES(?,?,?,?) "
    "ON DUPLICATE KEY UPDATE vtype=VALUES(vtype),value=VALUES(value)";

static const char *SQL_UPDATE_CONFIG =
    "UPDATE ufconfig_config SET format_version=?,version=version+1,schema_hash=?,digest=? "
    "WHERE config_id=?";

static const char *SQL_FIND_ENTRY =
    "SELECT path,vtype,value FROM ufconfig_entry WHERE config_id=? AND path=?";

static const char *SQL_UPSERT_ENTRY =
    "INSERT INTO ufconfig_entry(config_id,path,vtype,value) VALUES(?,?,?,?) "
    "ON DUPLICATE KEY UPDATE vtype=VALUES(vtype),value=VALUES(value)";

static const char *SQL_BUMP_VERSION =
    "UPDATE ufconfig_config SET version=version+1 WHERE config_id=?";

static const char *SQL_STAT =
    "SELECT c.version FROM ufconfig_config c "
    "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
    "WHERE n.namespace_name=? AND c.config_name=?";

static const char *SQL_READ_ENTRIES =
    "SELECT path,vtype,value FROM ufconfig_entry WHERE config_id=? ORDER BY path";

typedef struct SqlDsn {
    char *host;
    char *user;
    char *password;
    char *database;
    char *socket;
    unsigned port;
} SqlDsn;

typedef struct SqlCtx {
    MYSQL *db;
    char namespace_name[SQL_NS_MAX + 1];
    char config_name[SQL_NAME_MAX + 1];
} SqlCtx;

static void sDsnFree(SqlDsn *d) {
    if (!d) return;
    free(d->host); free(d->user); free(d->password); free(d->database);
    free(d->socket);
    memset(d, 0, sizeof(*d));
}

static int sHex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *sUrlDecode(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%') {
            if (i + 2 >= n) { free(p); return NULL; }
            int a = sHex((unsigned char)s[i + 1]);
            int b = sHex((unsigned char)s[i + 2]);
            if (a < 0 || b < 0) { free(p); return NULL; }
            p[k++] = (char)((a << 4) | b);
            i += 2;
        } else if (s[i] == '+') {
            p[k++] = ' ';
        } else {
            p[k++] = s[i];
        }
    }
    p[k] = '\0';
    return p;
}

static int sSetPart(char **dst, const char *a, size_t n) {
    char *p = sUrlDecode(a, n);
    if (!p) return 0;
    free(*dst);
    *dst = p;
    return 1;
}

static int sParseKv(SqlDsn *d, const char *s) {
    char *copy = strdup(s);
    if (!copy) return 0;
    char *save = NULL;
    for (char *item = strtok_r(copy, ";", &save); item; item = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(item, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (strcmp(item, "host") == 0) { if (!sSetPart(&d->host, eq, strlen(eq))) goto fail; }
        else if (strcmp(item, "user") == 0) { if (!sSetPart(&d->user, eq, strlen(eq))) goto fail; }
        else if (strcmp(item, "password") == 0 || strcmp(item, "pass") == 0) { if (!sSetPart(&d->password, eq, strlen(eq))) goto fail; }
        else if (strcmp(item, "database") == 0 || strcmp(item, "db") == 0) { if (!sSetPart(&d->database, eq, strlen(eq))) goto fail; }
        else if (strcmp(item, "socket") == 0) { if (!sSetPart(&d->socket, eq, strlen(eq))) goto fail; }
        else if (strcmp(item, "port") == 0) {
            char *end = NULL;
            unsigned long p = strtoul(eq, &end, 10);
            if (!*eq || !end || *end || p > 65535) goto fail;
            d->port = (unsigned)p;
        }
    }
    free(copy);
    return d->host || d->socket;
fail:
    free(copy);
    return 0;
}

static int sParseUri(SqlDsn *d, const char *s) {
    const char *p = strstr(s, "://");
    if (!p) return 0;
    p += 3;

    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    const char *authority_end = slash;

    const char *at = NULL;
    for (const char *q = p; q < authority_end; q++) if (*q == '@') at = q;

    const char *hostport = p;
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        if (colon) {
            if (!sSetPart(&d->user, p, (size_t)(colon - p)) ||
                !sSetPart(&d->password, colon + 1, (size_t)(at - colon - 1))) return 0;
        } else if (!sSetPart(&d->user, p, (size_t)(at - p))) return 0;
        hostport = at + 1;
    }

    if (hostport < authority_end && *hostport == '[') {
        const char *close = memchr(hostport, ']', (size_t)(authority_end - hostport));
        if (!close) return 0;
        if (!sSetPart(&d->host, hostport + 1, (size_t)(close - hostport - 1))) return 0;
        if (close + 1 < authority_end) {
            if (close[1] != ':') return 0;
            char *end = NULL;
            unsigned long port = strtoul(close + 2, &end, 10);
            if (end != authority_end || port > 65535) return 0;
            d->port = (unsigned)port;
        }
    } else {
        const char *colon = NULL;
        for (const char *q = hostport; q < authority_end; q++) if (*q == ':') colon = q;
        if (colon) {
            if (!sSetPart(&d->host, hostport, (size_t)(colon - hostport))) return 0;
            char *end = NULL;
            unsigned long port = strtoul(colon + 1, &end, 10);
            if (end != authority_end || port > 65535) return 0;
            d->port = (unsigned)port;
        } else if (!sSetPart(&d->host, hostport, (size_t)(authority_end - hostport))) return 0;
    }

    const char *db_start = slash + 1;
    const char *query = strchr(db_start, '?');
    size_t db_len = query ? (size_t)(query - db_start) : strlen(db_start);
    if (db_len && !sSetPart(&d->database, db_start, db_len)) return 0;

    if (query) {
        const char *q = query + 1;
        while (*q) {
            const char *amp = strchr(q, '&');
            size_t n = amp ? (size_t)(amp - q) : strlen(q);
            const char *eq = memchr(q, '=', n);
            if (eq && (size_t)(eq - q) == 6 && memcmp(q, "socket", 6) == 0) {
                if (!sSetPart(&d->socket, eq + 1, n - 7)) return 0;
            }
            if (!amp) break;
            q = amp + 1;
        }
    }
    return d->host || d->socket;
}

static int sParseDsn(SqlDsn *d, const char *s) {
    memset(d, 0, sizeof(*d));
    if (!s || !*s) return 0;
    if (strstr(s, "://")) {
        if (!sParseUri(d, s)) { sDsnFree(d); return 0; }
    } else if (!sParseKv(d, s)) {
        sDsnFree(d);
        return 0;
    }
    if (!d->host && !d->socket) { sDsnFree(d); return 0; }
    if (!d->port) d->port = 3306;
    return 1;
}

static UfConfigPairType sVTypeOf(const char *encoded) {
    if (!encoded) return UF_PAIR_STRING;
    if (strcmp(encoded, "{}") == 0) return UF_PAIR_EMPTY_TABLE;
    if (strcmp(encoded, "true") == 0 || strcmp(encoded, "false") == 0) return UF_PAIR_BOOL;
    if (encoded[0] == '"' || encoded[0] == '\'') return UF_PAIR_STRING;
    if (strchr(encoded, '.') || strchr(encoded, 'e') || strchr(encoded, 'E')) return UF_PAIR_FLOAT;
    return UF_PAIR_INT;
}

static int sBindIn(MYSQL_STMT *st, MYSQL_BIND *b, unsigned count) {
    return mysql_stmt_bind_param(st, b) == 0 && count != 0;
}

static int sExec(MYSQL *db, const char *sql) {
    return mysql_query(db, sql) == 0;
}

static int sGetNamespace(SqlCtx *c, uint64_t *out_id, int create) {
    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, create ? SQL_CREATE_NAMESPACE : SQL_FIND_NAMESPACE,
                                  strlen(create ? SQL_CREATE_NAMESPACE : SQL_FIND_NAMESPACE)) != 0) {
        if (st) mysql_stmt_close(st);
        return 0;
    }

    MYSQL_BIND p[1];
    memset(p, 0, sizeof(p));
    unsigned long len = (unsigned long)strlen(c->namespace_name);
    p[0].buffer_type = MYSQL_TYPE_STRING;
    p[0].buffer = c->namespace_name;
    p[0].buffer_length = len;
    p[0].length = &len;
    if (!sBindIn(st, p, 1)) { mysql_stmt_close(st); return 0; }

    if (mysql_stmt_execute(st) != 0) { mysql_stmt_close(st); return 0; }
    if (create) {
        *out_id = (uint64_t)mysql_insert_id(c->db);
        mysql_stmt_close(st);
        return *out_id != 0;
    }

    uint64_t id = 0;
    my_bool is_null = 0;
    unsigned long len_out = 0;
    MYSQL_BIND r[1];
    memset(r, 0, sizeof(r));
    r[0].buffer_type = MYSQL_TYPE_LONGLONG;
    r[0].buffer = &id;
    r[0].is_unsigned = 1;
    r[0].is_null = &is_null;
    r[0].length = &len_out;
    if (mysql_stmt_bind_result(st, r) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st); return 0;
    }
    int rc = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (rc != 0 || is_null) return 0;
    *out_id = id;
    return 1;
}

static int sGetConfig(SqlCtx *c, uint64_t *out_id, unsigned *fmt, uint64_t *version,
                      uint64_t *schema_hash, unsigned char digest[32], int *digest_set) {
    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_FIND_CONFIG, strlen(SQL_FIND_CONFIG)) != 0) {
        if (st) mysql_stmt_close(st);
        return -1;
    }

    MYSQL_BIND p[2];
    memset(p, 0, sizeof(p));
    unsigned long nl = (unsigned long)strlen(c->namespace_name);
    unsigned long cl = (unsigned long)strlen(c->config_name);
    p[0].buffer_type = MYSQL_TYPE_STRING; p[0].buffer = c->namespace_name; p[0].buffer_length = nl; p[0].length = &nl;
    p[1].buffer_type = MYSQL_TYPE_STRING; p[1].buffer = c->config_name; p[1].buffer_length = cl; p[1].length = &cl;
    if (mysql_stmt_bind_param(st, p) != 0 || mysql_stmt_execute(st) != 0) { mysql_stmt_close(st); return -1; }

    uint64_t id = 0, ver = 0, sh = 0;
    unsigned f = 0;
    unsigned char dig[32] = {0};
    my_bool null_id = 0, null_fmt = 0, null_ver = 0, null_sh = 0, null_dig = 0;
    unsigned long lens[5] = {0};
    MYSQL_BIND r[5];
    memset(r, 0, sizeof(r));
    r[0].buffer_type = MYSQL_TYPE_LONGLONG; r[0].buffer = &id; r[0].is_unsigned = 1; r[0].is_null = &null_id; r[0].length = &lens[0];
    r[1].buffer_type = MYSQL_TYPE_LONG; r[1].buffer = &f; r[1].is_unsigned = 1; r[1].is_null = &null_fmt; r[1].length = &lens[1];
    r[2].buffer_type = MYSQL_TYPE_LONGLONG; r[2].buffer = &ver; r[2].is_unsigned = 1; r[2].is_null = &null_ver; r[2].length = &lens[2];
    r[3].buffer_type = MYSQL_TYPE_LONGLONG; r[3].buffer = &sh; r[3].is_unsigned = 1; r[3].is_null = &null_sh; r[3].length = &lens[3];
    r[4].buffer_type = MYSQL_TYPE_BLOB; r[4].buffer = dig; r[4].buffer_length = sizeof(dig); r[4].is_null = &null_dig; r[4].length = &lens[4];
    if (mysql_stmt_bind_result(st, r) != 0 || mysql_stmt_store_result(st) != 0) { mysql_stmt_close(st); return -1; }
    int rc = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (rc == MYSQL_NO_DATA) return 0;
    if (rc != 0 || null_id || null_fmt || null_ver || null_sh) return -1;

    *out_id = id; *fmt = f; *version = ver; *schema_hash = sh;
    if (digest_set) *digest_set = !null_dig && lens[4] == sizeof(dig);
    if (digest_set && *digest_set && digest) memcpy(digest, dig, sizeof(dig));
    return 1;
}

static int sEnsureConfig(SqlCtx *c, uint64_t *config_id, const UfConfigMeta *meta) {
    uint64_t ns_id = 0;
    if (!sGetNamespace(c, &ns_id, 1)) return 0;

    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_CREATE_CONFIG, strlen(SQL_CREATE_CONFIG)) != 0) {
        if (st) mysql_stmt_close(st);
        return 0;
    }

    uint64_t schema_hash = meta ? meta->schema_hash : 0;
    MYSQL_BIND p[4];
    memset(p, 0, sizeof(p));
    unsigned long cl = (unsigned long)strlen(c->config_name);
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &ns_id; p[0].is_unsigned = 1;
    p[1].buffer_type = MYSQL_TYPE_STRING; p[1].buffer = c->config_name; p[1].buffer_length = cl; p[1].length = &cl;
    p[2].buffer_type = MYSQL_TYPE_LONGLONG; p[2].buffer = &schema_hash; p[2].is_unsigned = 1;
    my_bool digest_null = 1;
    p[3].buffer_type = MYSQL_TYPE_BLOB; p[3].buffer = NULL; p[3].buffer_length = 0; p[3].is_null = &digest_null;
    if (mysql_stmt_bind_param(st, p) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st); return 0;
    }
    *config_id = (uint64_t)mysql_insert_id(c->db);
    mysql_stmt_close(st);
    return *config_id != 0;
}

static int sDeleteEntries(SqlCtx *c, uint64_t config_id) {
    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_DELETE_ENTRIES, strlen(SQL_DELETE_ENTRIES)) != 0) {
        if (st) mysql_stmt_close(st);
        return 0;
    }
    MYSQL_BIND p[1]; memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &config_id; p[0].is_unsigned = 1;
    int ok = mysql_stmt_bind_param(st, p) == 0 && mysql_stmt_execute(st) == 0;
    mysql_stmt_close(st);
    return ok;
}

static int sInsertEntries(SqlCtx *c, uint64_t config_id, const UfConfigFieldPair *in, size_t n) {
    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_INSERT_ENTRY, strlen(SQL_INSERT_ENTRY)) != 0) {
        if (st) mysql_stmt_close(st);
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        if (!in[i].path) continue;
        size_t plen = strlen(in[i].path);
        size_t vlen = in[i].encoded ? strlen(in[i].encoded) : 0;
        if (plen > SQL_PATH_MAX) { mysql_stmt_close(st); return 0; }

        uint64_t id = config_id;
        unsigned vtype = (unsigned)in[i].vtype;
        unsigned long pl = (unsigned long)plen, vl = (unsigned long)vlen;
        MYSQL_BIND p[4]; memset(p, 0, sizeof(p));
        p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &id; p[0].is_unsigned = 1;
        p[1].buffer_type = MYSQL_TYPE_STRING; p[1].buffer = (char *)in[i].path; p[1].buffer_length = pl; p[1].length = &pl;
        p[2].buffer_type = MYSQL_TYPE_LONG; p[2].buffer = &vtype; p[2].is_unsigned = 1;
        p[3].buffer_type = MYSQL_TYPE_BLOB; p[3].buffer = (char *)(in[i].encoded ? in[i].encoded : ""); p[3].buffer_length = vl; p[3].length = &vl;
        if (mysql_stmt_bind_param(st, p) != 0 || mysql_stmt_execute(st) != 0) {
            mysql_stmt_close(st); return 0;
        }
    }
    mysql_stmt_close(st);
    return 1;
}

static int sUpdateConfig(SqlCtx *c, uint64_t config_id, const UfConfigMeta *meta) {
    uint64_t schema_hash = meta ? meta->schema_hash : 0;
    unsigned fmt = (meta && meta->fmt) ? meta->fmt : 1;
    unsigned char digest[32] = {0};
    unsigned long digest_len = 0;
    if (meta && meta->is_digest_set && meta->digest_hex[0]) {
        size_t n = strlen(meta->digest_hex);
        if (n != 64) return 0;
        for (size_t i = 0; i < 32; i++) {
            int a = sHex((unsigned char)meta->digest_hex[i * 2]);
            int b = sHex((unsigned char)meta->digest_hex[i * 2 + 1]);
            if (a < 0 || b < 0) return 0;
            digest[i] = (unsigned char)((a << 4) | b);
        }
        digest_len = 32;
    }

    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_UPDATE_CONFIG, strlen(SQL_UPDATE_CONFIG)) != 0) {
        if (st) mysql_stmt_close(st);
        return 0;
    }
    MYSQL_BIND p[4]; memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_LONG; p[0].buffer = &fmt; p[0].is_unsigned = 1;
    p[1].buffer_type = MYSQL_TYPE_LONGLONG; p[1].buffer = &schema_hash; p[1].is_unsigned = 1;
    my_bool digest_null = (digest_len == 0);
    p[2].buffer_type = MYSQL_TYPE_BLOB; p[2].buffer = digest; p[2].buffer_length = digest_len; p[2].length = &digest_len; p[2].is_null = &digest_null;
    p[3].buffer_type = MYSQL_TYPE_LONGLONG; p[3].buffer = &config_id; p[3].is_unsigned = 1;
    int ok = mysql_stmt_bind_param(st, p) == 0 && mysql_stmt_execute(st) == 0 && mysql_stmt_affected_rows(st) == 1;
    mysql_stmt_close(st);
    return ok;
}

static int sBumpVersion(SqlCtx *c, uint64_t config_id) {
    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_BUMP_VERSION, strlen(SQL_BUMP_VERSION)) != 0) {
        if (st) mysql_stmt_close(st);
        return 0;
    }
    MYSQL_BIND p[1]; memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &config_id; p[0].is_unsigned = 1;
    int ok = mysql_stmt_bind_param(st, p) == 0 && mysql_stmt_execute(st) == 0 && mysql_stmt_affected_rows(st) == 1;
    mysql_stmt_close(st);
    return ok;
}

static UfConfigStatus sSqlOpen(void **ctx, const UfConfigBackendSpec *spec) {
    if (!ctx || !spec || !spec->u.sql.dsn || !spec->u.sql.ns || !spec->u.sql.config_name)
        return UF_CONFIG_ERR_INVALID_ARG;
    if (strlen(spec->u.sql.ns) > SQL_NS_MAX || strlen(spec->u.sql.config_name) > SQL_NAME_MAX)
        return UF_CONFIG_ERR_LIMIT_EXCEEDED;

    SqlDsn d;
    if (!sParseDsn(&d, spec->u.sql.dsn)) return UF_CONFIG_ERR_INVALID_ARG;

    SqlCtx *c = (SqlCtx *)calloc(1, sizeof(*c));
    if (!c) { sDsnFree(&d); return UF_CONFIG_ERR_NO_MEMORY; }
    snprintf(c->namespace_name, sizeof(c->namespace_name), "%s", spec->u.sql.ns);
    snprintf(c->config_name, sizeof(c->config_name), "%s", spec->u.sql.config_name);

    c->db = mysql_init(NULL);
    if (!c->db) { free(c); sDsnFree(&d); return UF_CONFIG_ERR_NO_MEMORY; }
    unsigned reconnect = 1;
    mysql_options(c->db, MYSQL_OPT_RECONNECT, &reconnect);
    if (!mysql_real_connect(c->db, d.host, d.user, d.password, d.database,
                            d.port, d.socket, CLIENT_FOUND_ROWS)) {
        mysql_close(c->db); free(c); sDsnFree(&d); return UF_CONFIG_ERR_BACKEND;
    }
    sDsnFree(&d);
    *ctx = c;
    return UF_CONFIG_OK;
}

static UfConfigStatus sSqlReadAll(void *ctx, UfConfigFieldPair **out, size_t *n, UfConfigMeta *meta) {
    SqlCtx *c = (SqlCtx *)ctx;
    if (!c || !out || !n) return UF_CONFIG_ERR_INVALID_ARG;
    *out = NULL; *n = 0;
    if (meta) memset(meta, 0, sizeof(*meta));

    uint64_t config_id = 0, version = 0, schema_hash = 0;
    unsigned fmt = 0;
    unsigned char digest[32] = {0};
    int digest_set = 0;
    int found = sGetConfig(c, &config_id, &fmt, &version, &schema_hash, digest, &digest_set);
    if (found < 0) return UF_CONFIG_ERR_BACKEND;
    if (!found) return UF_CONFIG_ERR_NOT_CONFIGURED;
    if (fmt != 1) return UF_CONFIG_ERR_STORE_VERSION;

    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_READ_ENTRIES, strlen(SQL_READ_ENTRIES)) != 0) {
        if (st) mysql_stmt_close(st);
        return UF_CONFIG_ERR_BACKEND;
    }
    MYSQL_BIND p[1]; memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &config_id; p[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(st, p) != 0 || mysql_stmt_execute(st) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st); return UF_CONFIG_ERR_BACKEND;
    }

    my_ulonglong rows = mysql_stmt_num_rows(st);
    UfConfigFieldPair *pairs = (UfConfigFieldPair *)calloc(rows ? (size_t)rows : 1, sizeof(*pairs));
    if (!pairs) { mysql_stmt_free_result(st); mysql_stmt_close(st); return UF_CONFIG_ERR_NO_MEMORY; }

    char path[SQL_PATH_MAX + 1];
    char value[64001];
    unsigned vtype = 0;
    unsigned long pl = 0, vl = 0;
    my_bool pn = 0, vn = 0;
    MYSQL_BIND r[3]; memset(r, 0, sizeof(r));
    r[0].buffer_type = MYSQL_TYPE_STRING; r[0].buffer = path; r[0].buffer_length = sizeof(path); r[0].length = &pl; r[0].is_null = &pn;
    r[1].buffer_type = MYSQL_TYPE_LONG; r[1].buffer = &vtype; r[1].is_unsigned = 1; r[1].length = NULL; r[1].is_null = &vn;
    r[2].buffer_type = MYSQL_TYPE_BLOB; r[2].buffer = value; r[2].buffer_length = sizeof(value) - 1; r[2].length = &vl; r[2].is_null = NULL;
    if (mysql_stmt_bind_result(st, r) != 0) {
        free(pairs); mysql_stmt_free_result(st); mysql_stmt_close(st); return UF_CONFIG_ERR_BACKEND;
    }

    size_t k = 0;
    for (;;) {
        int rc = mysql_stmt_fetch(st);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == MYSQL_DATA_TRUNCATED || rc != 0 || pn || vn || pl > SQL_PATH_MAX || vl >= sizeof(value)) {
            for (size_t i = 0; i < k; i++) { free((void *)pairs[i].path); free((void *)pairs[i].encoded); }
            free(pairs); mysql_stmt_free_result(st); mysql_stmt_close(st);
            return rc == MYSQL_DATA_TRUNCATED ? UF_CONFIG_ERR_LIMIT_EXCEEDED : UF_CONFIG_ERR_BACKEND;
        }
        path[pl] = '\0'; value[vl] = '\0';
        pairs[k].path = strdup(path);
        pairs[k].encoded = strdup(value);
        if (!pairs[k].path || !pairs[k].encoded) {
            free((void *)pairs[k].path); free((void *)pairs[k].encoded);
            for (size_t i = 0; i < k; i++) { free((void *)pairs[i].path); free((void *)pairs[i].encoded); }
            free(pairs); mysql_stmt_free_result(st); mysql_stmt_close(st);
            return UF_CONFIG_ERR_NO_MEMORY;
        }
        pairs[k].vtype = (UfConfigPairType)vtype;
        if (pairs[k].vtype != UF_PAIR_INT && pairs[k].vtype != UF_PAIR_FLOAT &&
            pairs[k].vtype != UF_PAIR_BOOL && pairs[k].vtype != UF_PAIR_STRING &&
            pairs[k].vtype != UF_PAIR_EMPTY_TABLE)
            pairs[k].vtype = sVTypeOf(value);
        k++;
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);

    *out = pairs; *n = k;
    if (meta) {
        meta->fmt = fmt;
        meta->version = version;
        meta->schema_hash = schema_hash;
        if (digest_set) {
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < 32; i++) {
                meta->digest_hex[i * 2] = hex[digest[i] >> 4];
                meta->digest_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
            }
            meta->digest_hex[64] = '\0';
            meta->is_digest_set = 1;
        }
    }
    return UF_CONFIG_OK;
}

static UfConfigStatus sSqlWriteAll(void *ctx, const UfConfigFieldPair *in, size_t n, const UfConfigMeta *meta) {
    SqlCtx *c = (SqlCtx *)ctx;
    if (!c || (n && !in)) return UF_CONFIG_ERR_INVALID_ARG;
    if (n > 0) {
        for (size_t i = 0; i < n; i++) {
            if (in[i].path && strlen(in[i].path) > SQL_PATH_MAX) return UF_CONFIG_ERR_LIMIT_EXCEEDED;
            if (in[i].encoded && strlen(in[i].encoded) > 64000) return UF_CONFIG_ERR_LIMIT_EXCEEDED;
        }
    }

    if (!sExec(c->db, "START TRANSACTION")) return UF_CONFIG_ERR_BACKEND;
    uint64_t config_id = 0;
    if (!sEnsureConfig(c, &config_id, meta) || !sDeleteEntries(c, config_id) ||
        !sInsertEntries(c, config_id, in, n) || !sUpdateConfig(c, config_id, meta)) {
        mysql_rollback(c->db);
        return UF_CONFIG_ERR_BACKEND;
    }
    if (!sExec(c->db, "COMMIT")) {
        mysql_rollback(c->db);
        return UF_CONFIG_ERR_BACKEND;
    }
    return UF_CONFIG_OK;
}

static UfConfigStatus sSqlReadOne(void *ctx, const char *path, UfConfigFieldPair *out) {
    SqlCtx *c = (SqlCtx *)ctx;
    if (!c || !path || !out) return UF_CONFIG_ERR_INVALID_ARG;
    if (strlen(path) > SQL_PATH_MAX) return UF_CONFIG_ERR_LIMIT_EXCEEDED;

    uint64_t config_id = 0, version = 0, schema_hash = 0;
    unsigned fmt = 0; unsigned char digest[32]; int digest_set = 0;
    int found = sGetConfig(c, &config_id, &fmt, &version, &schema_hash, digest, &digest_set);
    if (found < 0) return UF_CONFIG_ERR_BACKEND;
    if (!found || fmt != 1) return UF_CONFIG_ERR_NOFIELD;

    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_FIND_ENTRY, strlen(SQL_FIND_ENTRY)) != 0) {
        if (st) mysql_stmt_close(st);
        return UF_CONFIG_ERR_BACKEND;
    }
    unsigned long pl = (unsigned long)strlen(path);
    MYSQL_BIND p[2]; memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &config_id; p[0].is_unsigned = 1;
    p[1].buffer_type = MYSQL_TYPE_STRING; p[1].buffer = (char *)path; p[1].buffer_length = pl; p[1].length = &pl;
    if (mysql_stmt_bind_param(st, p) != 0 || mysql_stmt_execute(st) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st); return UF_CONFIG_ERR_BACKEND;
    }

    char out_path[SQL_PATH_MAX + 1], value[64001]; unsigned vtype = 0;
    unsigned long opl = 0, vl = 0; my_bool on = 0;
    MYSQL_BIND r[3]; memset(r, 0, sizeof(r));
    r[0].buffer_type = MYSQL_TYPE_STRING; r[0].buffer = out_path; r[0].buffer_length = sizeof(out_path); r[0].length = &opl; r[0].is_null = &on;
    r[1].buffer_type = MYSQL_TYPE_LONG; r[1].buffer = &vtype; r[1].is_unsigned = 1;
    r[2].buffer_type = MYSQL_TYPE_BLOB; r[2].buffer = value; r[2].buffer_length = sizeof(value) - 1; r[2].length = &vl;
    if (mysql_stmt_bind_result(st, r) != 0) { mysql_stmt_free_result(st); mysql_stmt_close(st); return UF_CONFIG_ERR_BACKEND; }
    int rc = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st); mysql_stmt_close(st);
    if (rc == MYSQL_NO_DATA) return UF_CONFIG_ERR_NOFIELD;
    if (rc == MYSQL_DATA_TRUNCATED || rc != 0 || on || opl > SQL_PATH_MAX || vl >= sizeof(value))
        return rc == MYSQL_DATA_TRUNCATED ? UF_CONFIG_ERR_LIMIT_EXCEEDED : UF_CONFIG_ERR_BACKEND;

    out_path[opl] = '\0'; value[vl] = '\0';
    out->path = strdup(out_path);
    out->encoded = strdup(value);
    if (!out->path || !out->encoded) { free((void *)out->path); free((void *)out->encoded); return UF_CONFIG_ERR_NO_MEMORY; }
    out->vtype = (UfConfigPairType)vtype;
    if (out->vtype != UF_PAIR_INT && out->vtype != UF_PAIR_FLOAT && out->vtype != UF_PAIR_BOOL &&
        out->vtype != UF_PAIR_STRING && out->vtype != UF_PAIR_EMPTY_TABLE)
        out->vtype = sVTypeOf(value);
    return UF_CONFIG_OK;
}

static UfConfigStatus sSqlWriteOne(void *ctx, const char *path, const UfConfigFieldPair *in) {
    SqlCtx *c = (SqlCtx *)ctx;
    if (!c || !path || !in) return UF_CONFIG_ERR_INVALID_ARG;
    if (strlen(path) > SQL_PATH_MAX || (in->encoded && strlen(in->encoded) > 64000)) return UF_CONFIG_ERR_LIMIT_EXCEEDED;

    if (!sExec(c->db, "START TRANSACTION")) return UF_CONFIG_ERR_BACKEND;
    uint64_t config_id = 0;
    if (!sEnsureConfig(c, &config_id, NULL)) { mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND; }

    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_UPSERT_ENTRY, strlen(SQL_UPSERT_ENTRY)) != 0) {
        if (st) mysql_stmt_close(st);
        mysql_rollback(c->db);
        return UF_CONFIG_ERR_BACKEND;
    }
    unsigned long pl = (unsigned long)strlen(path), vl = (unsigned long)(in->encoded ? strlen(in->encoded) : 0);
    unsigned vtype = (unsigned)in->vtype;
    MYSQL_BIND p[4]; memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &config_id; p[0].is_unsigned = 1;
    p[1].buffer_type = MYSQL_TYPE_STRING; p[1].buffer = (char *)path; p[1].buffer_length = pl; p[1].length = &pl;
    p[2].buffer_type = MYSQL_TYPE_LONG; p[2].buffer = &vtype; p[2].is_unsigned = 1;
    p[3].buffer_type = MYSQL_TYPE_BLOB; p[3].buffer = (char *)(in->encoded ? in->encoded : ""); p[3].buffer_length = vl; p[3].length = &vl;
    int ok = mysql_stmt_bind_param(st, p) == 0 && mysql_stmt_execute(st) == 0;
    mysql_stmt_close(st);
    if (!ok || !sBumpVersion(c, config_id) || !sExec(c->db, "COMMIT")) {
        mysql_rollback(c->db); return UF_CONFIG_ERR_BACKEND;
    }
    return UF_CONFIG_OK;
}

static UfConfigStatus sSqlStat(void *ctx, UfConfigStamp *out) {
    SqlCtx *c = (SqlCtx *)ctx;
    if (!c || !out) return UF_CONFIG_ERR_INVALID_ARG;
    uint64_t version = 0;
    MYSQL_STMT *st = mysql_stmt_init(c->db);
    if (!st || mysql_stmt_prepare(st, SQL_STAT, strlen(SQL_STAT)) != 0) {
        if (st) mysql_stmt_close(st);
        return UF_CONFIG_ERR_BACKEND;
    }
    MYSQL_BIND p[2]; memset(p, 0, sizeof(p));
    unsigned long nl = (unsigned long)strlen(c->namespace_name), cl = (unsigned long)strlen(c->config_name);
    p[0].buffer_type = MYSQL_TYPE_STRING; p[0].buffer = c->namespace_name; p[0].buffer_length = nl; p[0].length = &nl;
    p[1].buffer_type = MYSQL_TYPE_STRING; p[1].buffer = c->config_name; p[1].buffer_length = cl; p[1].length = &cl;
    if (mysql_stmt_bind_param(st, p) != 0 || mysql_stmt_execute(st) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st); return UF_CONFIG_ERR_BACKEND;
    }
    my_bool is_null = 0; unsigned long len = 0;
    MYSQL_BIND r[1]; memset(r, 0, sizeof(r));
    r[0].buffer_type = MYSQL_TYPE_LONGLONG; r[0].buffer = &version; r[0].is_unsigned = 1; r[0].is_null = &is_null; r[0].length = &len;
    if (mysql_stmt_bind_result(st, r) != 0) { mysql_stmt_free_result(st); mysql_stmt_close(st); return UF_CONFIG_ERR_BACKEND; }
    int rc = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st); mysql_stmt_close(st);
    if (rc == MYSQL_NO_DATA) return UF_CONFIG_ERR_NOT_CONFIGURED;
    if (rc != 0 || is_null) return UF_CONFIG_ERR_BACKEND;
    out->token = version;
    return UF_CONFIG_OK;
}

static void sSqlClose(void *ctx) {
    SqlCtx *c = (SqlCtx *)ctx;
    if (!c) return;
    if (c->db) mysql_close(c->db);
    free(c);
}

const UfConfigDriver ufconfig_driver_sql = {
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
        .max_path_bytes    = SQL_PATH_MAX,
        .max_value_bytes   = 64000,
        .max_fields        = 0,
        .name              = "sql",
    },
    .open      = sSqlOpen,
    .read_all  = sSqlReadAll,
    .write_all = sSqlWriteAll,
    .read_one  = sSqlReadOne,
    .write_one = sSqlWriteOne,
    .stat      = sSqlStat,
    .close     = sSqlClose,
};

#endif /* UFLIB_CAPABILITY_SQL */
