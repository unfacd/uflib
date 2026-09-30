/**
 * @file sql_load.c
 * @brief Load a known document into MariaDB through the library's SQL driver.
 *
 * The first half of the round-trip check.  It does one thing: read a document
 * from disk, load it through the module, flatten it, and write every pair to
 * the store using the driver the library ships.
 *
 * The second half is `sql_compare.py`, which reads the rows back with a
 * different client entirely and diffs them against the same document.  That
 * split is the point: a harness whose reader shares code with its writer can
 * only prove the round trip is *stable*, never that it is *faithful*.
 *
 *     ./ufconfig_sql_load <dsn> <document> [namespace] [config_name]
 *
 * Applies `ufconfig_mariadb.sql` first — idempotently, since the DDL is
 * CREATE TABLE IF NOT EXISTS — so it runs against an empty schema.  It does not
 * clean up: `sql_compare.py` needs the rows to still be there, and it removes
 * them when it is done.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <mariadb/mysql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

#ifndef UFCONFIG_DDL_PATH
#define UFCONFIG_DDL_PATH "ufconfig_mariadb.sql"
#endif

/* Flat sequence of CREATE TABLE statements, `--` comments, no routines. */
static void ApplyDdl(MYSQL *db, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open the DDL at %s\n", path); exit(EXIT_FAILURE); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); fprintf(stderr, "no memory for the DDL\n"); exit(EXIT_FAILURE); }
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);

    char   stmt[4096];
    size_t k = 0;
    int    applied = 0;
    for (size_t i = 0; i <= got; i++) {
        char ch = (i < got) ? buf[i] : '\0';
        if (ch == '-' && i + 1 < got && buf[i + 1] == '-') {
            while (i < got && buf[i] != '\n') i++;
            continue;
        }
        if (ch == ';' || ch == '\0') {
            stmt[k] = '\0';
            int blank = 1;
            for (size_t j = 0; j < k; j++)
                if (stmt[j] != ' ' && stmt[j] != '\n' && stmt[j] != '\t' && stmt[j] != '\r') { blank = 0; break; }
            if (!blank) {
                if (mysql_query(db, stmt) != 0) {
                    fprintf(stderr, "applying the DDL: %s\n  statement: %s\n", mysql_error(db), stmt);
                    exit(EXIT_FAILURE);
                }
                applied++;
            }
            k = 0;
            if (ch == '\0') break;
            continue;
        }
        if (k + 1 < sizeof(stmt)) stmt[k++] = ch;
    }
    free(buf);
    printf("schema      %d statements applied\n", applied);
}

/* Enough of the DSN to reach the server for the DDL; the driver parses its own. */
static void ConnectForDdl(MYSQL **out, const char *dsn) {
    char host[256] = "127.0.0.1", user[128] = "root", pass[256] = "", db[128] = "ufsrv", sock[256] = "";
    unsigned port = 3306;
    char *copy = strdup(dsn), *save = NULL;
    for (char *it = copy ? strtok_r(copy, ";", &save) : NULL; it; it = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(it, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (!strcmp(it, "host")) snprintf(host, sizeof(host), "%s", eq);
        else if (!strcmp(it, "user")) snprintf(user, sizeof(user), "%s", eq);
        else if (!strcmp(it, "password") || !strcmp(it, "pass")) snprintf(pass, sizeof(pass), "%s", eq);
        else if (!strcmp(it, "database") || !strcmp(it, "db")) snprintf(db, sizeof(db), "%s", eq);
        else if (!strcmp(it, "socket")) snprintf(sock, sizeof(sock), "%s", eq);
        else if (!strcmp(it, "port")) { unsigned long p = strtoul(eq, NULL, 10); if (p && p <= 65535) port = (unsigned)p; }
    }
    free(copy);

    MYSQL *m = mysql_init(NULL);
    if (!m || !mysql_real_connect(m, host, user, pass[0] ? pass : NULL, db, port, sock[0] ? sock : NULL, 0)) {
        fprintf(stderr, "cannot reach %s:%u/%s: %s\n", host, port, db, m ? mysql_error(m) : "init failed");
        exit(EXIT_FAILURE);
    }
    *out = m;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dsn> <document> [namespace] [config_name]\n", argv[0]);
        return 2;
    }
    const char *dsn  = argv[1];
    const char *doc  = argv[2];
    const char *ns   = argc > 3 ? argv[3] : "ufcfgtest";
    const char *cfg  = argc > 4 ? argv[4] : "ufsrvwebsock";

    printf("ufconfig SQL load\n");

    MYSQL *ddl = NULL;
    ConnectForDdl(&ddl, dsn);
    ApplyDdl(ddl, UFCONFIG_DDL_PATH);
    mysql_close(ddl);

    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields          = g_ufconfig_fields;
    d.field_count     = (size_t)g_ufconfig_field_count;
    d.lookup          = UfConfigLookupPath;
    d.kind            = UF_CONFIG_BACKEND_SQL;
    d.sql.address     = dsn;
    d.sql.ns          = ns;
    d.sql.config_name = cfg;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) {
        fprintf(stderr, "UfConfigCreate: %s\n", UfConfigLastError()->message);
        return EXIT_FAILURE;
    }

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size    = sizeof(opt);
    opt.version = 1;
    opt.mode    = UF_CONFIG_LOAD_STRICT;

    if (UfConfigLoadFile(h, doc, &opt, NULL) != UF_CONFIG_OK) {
        fprintf(stderr, "loading %s: %s\n", doc, UfConfigLastError()->message);
        return EXIT_FAILURE;
    }

    UfConfigFieldPair *pairs = NULL;
    size_t             np    = 0;
    UfConfigMeta       meta;
    memset(&meta, 0, sizeof(meta));
    if (UfConfigFlattenHandle(h, &pairs, &np, &meta) != UF_CONFIG_OK) {
        fprintf(stderr, "flattening: %s\n", UfConfigLastError()->message);
        return EXIT_FAILURE;
    }

    const UfConfigDriver *drv = UfConfigDriverFindKind(UF_CONFIG_BACKEND_SQL);
    if (!drv) {
        fprintf(stderr, "the library has no SQL driver — build it with UFLIB_CAPABILITY_SQL=ON\n");
        return EXIT_FAILURE;
    }

    UfConfigBackendSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind              = UF_CONFIG_BACKEND_SQL;
    spec.u.sql.dsn         = dsn;
    spec.u.sql.ns          = ns;
    spec.u.sql.config_name = cfg;

    void *ctx = NULL;
    if (drv->open(&ctx, &spec) != UF_CONFIG_OK) { fprintf(stderr, "driver open failed\n"); return EXIT_FAILURE; }
    UfConfigStatus st = drv->write_all(ctx, pairs, np, &meta);
    drv->close(ctx);
    if (st != UF_CONFIG_OK) {
        fprintf(stderr, "driver write_all: %s\n", UfConfigStatusString(st));
        return EXIT_FAILURE;
    }

    printf("document    %s\n", doc);
    printf("target      %s / %s\n", ns, cfg);
    printf("flattened   %zu pairs written\n", np);

    UfConfigFreePairs(pairs, np);
    UfConfigDestroy(h);
    return EXIT_SUCCESS;
}
