/**
 * @file sql_live_test.c
 * @brief End-to-end against a real MariaDB: write through the driver, read back
 *        through UfConfigLoad.
 *
 * Not registered with CTest, because it needs a MariaDB to talk to.  Run it
 * against any reachable instance:
 *
 *     ./ufconfig_sql_live [dsn] [namespace]
 *     # defaults: host=127.0.0.1;port=33061;user=root;database=ufconfig_live
 *     #           namespace: ufcfgtest
 *
 * ## What it establishes
 *
 * That the driver interface is implementable from outside the library, and that
 * the whole chain works against a store the library has never seen:
 *
 *   UfConfigDescriptor.kind = SQL
 *     -> UfConfigDriverFindKind
 *     -> the driver this test registered
 *     -> a real connection
 *     -> SQL
 *     -> back through UfConfigLoadPairs into a validated tree
 *
 * It also applies the shipped DDL (`src/ufconfig/driver/ufconfig_mariadb.sql`)
 * to the database before it starts, which is the only execution that file gets
 * anywhere in this tree — so a schema that cannot be created fails here rather
 * than in a deployment.
 *
 * The namespace defaults to something no deployment uses, and every row it
 * creates is deleted on the way out — including on failure, so a failed run
 * does not leave a configuration behind for the next one to read as a pass.
 * The tables themselves are left in place: they are shared, and a test that
 * dropped them would take a deployment's schema with it.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <mariadb/mysql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UFCONFIG_LIVE_USE_LIBRARY_DRIVER
extern const UfConfigDriver g_ufconfig_driver_mariadb;
#endif

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);
static MYSQL *g_cleanup_conn;
static char   g_cleanup_ns[192];

static void CleanupNamespace(void) {
    if (!g_cleanup_conn || !g_cleanup_ns[0]) return;
    char esc[400];
    mysql_real_escape_string(g_cleanup_conn, esc, g_cleanup_ns, (unsigned long)strlen(g_cleanup_ns));
    char sql[512];
    /* One row: the child tables cascade from it. */
    snprintf(sql, sizeof(sql), "DELETE FROM ufconfig_namespace WHERE namespace_name='%s'", esc);
    mysql_query(g_cleanup_conn, sql);
}

static void Fail(const char *what) {
    const UfConfigError *e = UfConfigLastError();
    fprintf(stderr, "FAIL: %s%s%s\n", what,
            (e && e->message[0]) ? " — " : "",
            (e && e->message[0]) ? e->message : "");
    CleanupNamespace();
    exit(EXIT_FAILURE);
}

static void FailSql(const char *what) {
    fprintf(stderr, "FAIL: %s — %s\n", what, mysql_error(g_cleanup_conn));
    CleanupNamespace();
    exit(EXIT_FAILURE);
}

/* The DDL is a flat sequence of CREATE TABLE statements with `--` comments and
   no routines, so a splitter on ';' that skips to end-of-line on `--` is
   sufficient.  It is deliberately strict: anything it cannot parse is reported
   rather than skipped, because a silently half-applied schema would show up
   later as a confusing failure in the driver. */
static void ApplyDdl(MYSQL *db, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open the DDL at %s\n", path);
        exit(EXIT_FAILURE);
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fprintf(stderr, "FAIL: cannot seek the DDL\n"); exit(EXIT_FAILURE); }
    long len = ftell(f);
    if (len < 0) { fclose(f); fprintf(stderr, "FAIL: cannot size the DDL\n"); exit(EXIT_FAILURE); }
    rewind(f);

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); fprintf(stderr, "FAIL: no memory for the DDL\n"); exit(EXIT_FAILURE); }
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);

    size_t executed = 0;
    char   stmt[4096];
    size_t k = 0;
    for (size_t i = 0; i <= got; i++) {
        char ch = (i < got) ? buf[i] : '\0';
        if (ch == '-' && i + 1 < got && buf[i + 1] == '-') {
            while (i < got && buf[i] != '\n') i++;
            continue;
        }
        if (ch == ';' || ch == '\0') {
            stmt[k] = '\0';
            /* Anything that is only whitespace is not a statement. */
            int blank = 1;
            for (size_t j = 0; j < k; j++) if (stmt[j] != ' ' && stmt[j] != '\n' && stmt[j] != '\t' && stmt[j] != '\r') { blank = 0; break; }
            if (!blank) {
                if (mysql_query(db, stmt) != 0) {
                    fprintf(stderr, "FAIL: applying the DDL — %s\n  statement: %s\n", mysql_error(db), stmt);
                    free(buf);
                    CleanupNamespace();
                    exit(EXIT_FAILURE);
                }
                executed++;
            }
            k = 0;
            if (ch == '\0') break;
            continue;
        }
        if (k + 1 < sizeof(stmt)) stmt[k++] = ch;
    }
    free(buf);
    printf("  ddl            %zu statements applied from %s\n", executed, path);
}

int main(int argc, char **argv) {
    const char *dsn = argc > 1 ? argv[1]
                               : "host=127.0.0.1;port=33061;user=root;database=ufconfig_live";
    const char *ns  = argc > 2 ? argv[2] : "ufcfgtest";
    const char *cfg = "ufsrvwebsock";

    /* Supplied by CMake, because CTest runs from the build directory and a
       path relative to this source file would be wrong at run time. */
#ifndef UFCONFIG_TEST_DIR
#define UFCONFIG_TEST_DIR "."
#endif
#ifndef UFCONFIG_DDL_PATH
#define UFCONFIG_DDL_PATH "ufconfig_mariadb.sql"
#endif
    char document[1024];
    snprintf(document, sizeof(document), "%s/examples/sample.strict.lua", UFCONFIG_TEST_DIR);

    printf("ufconfig live SQL test\n");
    /* The DSN may carry a password, and this output is read, pasted and logged.
       Mask the value rather than echoing it. */
    char dsn_show[512];
    snprintf(dsn_show, sizeof(dsn_show), "%s", dsn);
    for (char *p = strstr(dsn_show, "password="); p; p = strstr(p + 1, "password=")) {
        char  *v = p + strlen("password=");
        char  *e = strchr(v, ';');
        size_t n = e ? (size_t)(e - v) : strlen(v);
        if (n) memset(v, '*', n);
    }
    printf("  dsn            %s\n", dsn_show);
    printf("  namespace      %s (config %s)\n", ns, cfg);

    /* A connection of our own, purely so the test can clean up after itself
       whatever happens to the handle. */
    g_cleanup_conn = mysql_init(NULL);
    if (!g_cleanup_conn) {
        fprintf(stderr, "FAIL: mysql_init\n");
        return EXIT_FAILURE;
    }
    /* The DSN is the same key/value form the driver under test parses; split it
       the crude way here rather than sharing the parser, so the test does not
       depend on the code it is testing. */
    char host[256] = "127.0.0.1", user[128] = "root", pass[128] = "", db[128] = "ufconfig_live", sock[256] = "";
    unsigned port = 33061;
    {
        char *copy = strdup(dsn);
        char *save = NULL;
        for (char *it = copy ? strtok_r(copy, ";", &save) : NULL; it; it = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(it, '=');
            if (!eq) continue;
            *eq++ = '\0';
            if (strcmp(it, "host") == 0) snprintf(host, sizeof(host), "%s", eq);
            else if (strcmp(it, "user") == 0) snprintf(user, sizeof(user), "%s", eq);
            else if (strcmp(it, "password") == 0 || strcmp(it, "pass") == 0) snprintf(pass, sizeof(pass), "%s", eq);
            else if (strcmp(it, "database") == 0 || strcmp(it, "db") == 0) snprintf(db, sizeof(db), "%s", eq);
            else if (strcmp(it, "socket") == 0) snprintf(sock, sizeof(sock), "%s", eq);
            else if (strcmp(it, "port") == 0) { unsigned long p = strtoul(eq, NULL, 10); if (p && p <= 65535) port = (unsigned)p; }
        }
        free(copy);
    }
    if (!mysql_real_connect(g_cleanup_conn, host[0] ? host : NULL, user[0] ? user : NULL,
                            pass[0] ? pass : NULL, db[0] ? db : NULL, port,
                            sock[0] ? sock : NULL, 0)) {
        fprintf(stderr, "FAIL: cannot reach %s:%u/%s — %s\n", host, port, db, mysql_error(g_cleanup_conn));
        return EXIT_FAILURE;
    }
    snprintf(g_cleanup_ns, sizeof(g_cleanup_ns), "%s", ns);
    CleanupNamespace();

    /* The queries below interpolate the namespace and the config name, so they
       are escaped once, here, rather than at each of the nine sites — the same
       treatment CleanupNamespace gives its own.  A namespace arrives from the
       command line, so it is not automatically safe. */
    char ns_esc[400], cfg_esc[400];
    mysql_real_escape_string(g_cleanup_conn, ns_esc, ns, (unsigned long)strlen(ns));
    mysql_real_escape_string(g_cleanup_conn, cfg_esc, cfg, (unsigned long)strlen(cfg));

    printf("\nschema\n");
    ApplyDdl(g_cleanup_conn, UFCONFIG_DDL_PATH);
    /* Idempotent by construction — CREATE TABLE IF NOT EXISTS — so running this
       twice must be uneventful.  Applying it a second time is the cheapest
       check that it is. */
    ApplyDdl(g_cleanup_conn, UFCONFIG_DDL_PATH);

    /* ── The driver under test ────────────────────────────────────────────── */
    UfConfigStatus st = UF_CONFIG_OK;
#ifdef UFCONFIG_LIVE_USE_LIBRARY_DRIVER
    /* Built by the second target in this directory, against a library
       configured with UFLIB_CAPABILITY_SQL=ON.  Nothing is registered here: the
       library registered its own driver at first use, so what this run proves
       is about the code that ships rather than about a driver written for the
       test.  Its absence is a hard failure, not a skip — reaching here at all
       means the target was built deliberately, and a target that silently
       tests nothing is the failure mode this whole file exists to avoid. */
    const UfConfigDriver *lib_drv = UfConfigDriverFindKind(UF_CONFIG_BACKEND_SQL);
    if (!lib_drv) {
        fprintf(stderr, "FAIL: the library has no SQL driver — rebuild with UFLIB_CAPABILITY_SQL=ON\n");
        CleanupNamespace();
        return EXIT_FAILURE;
    }
    printf("\ndriver          the library's own, as \"%s\"\n", lib_drv->caps.name);
#else
    st = UfConfigRegisterDriver(&g_ufconfig_driver_mariadb);
    printf("\nregister        %s", UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) { printf("\n"); Fail("UfConfigRegisterDriver"); }
    printf("  (as \"%s\")\n", g_ufconfig_driver_mariadb.caps.name);
#endif

    /* ── 1. Load from an empty store ──────────────────────────────────────── */
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_SQL;
    d.sql.address     = dsn;
    d.sql.ns          = ns;
    d.sql.config_name = cfg;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) Fail("UfConfigCreate");

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size    = sizeof(opt);
    opt.version = 1;
    opt.mode    = UF_CONFIG_LOAD_STRICT;

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
    size_t             np    = 0;
    UfConfigMeta       meta;
    memset(&meta, 0, sizeof(meta));
    if (UfConfigFlattenHandle(seed, &pairs, &np, &meta) != UF_CONFIG_OK)
        Fail("flattening the seed handle");

    const UfConfigDriver *drv = UfConfigDriverFindKind(UF_CONFIG_BACKEND_SQL);
    if (!drv) Fail("no driver registered for SQL");

    UfConfigBackendSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind              = UF_CONFIG_BACKEND_SQL;
    spec.u.sql.dsn         = dsn;
    spec.u.sql.ns          = ns;
    spec.u.sql.config_name = cfg;

    void *ctx = NULL;
    st = drv->open(&ctx, &spec);
    if (st != UF_CONFIG_OK) Fail("driver open");
    st = drv->write_all(ctx, pairs, np, &meta);
    if (st != UF_CONFIG_OK) { drv->close(ctx); Fail("driver write_all"); }
    drv->close(ctx);
    UfConfigFreePairs(pairs, np);
    UfConfigDestroy(seed);

    printf("\nseed            %zu fields written to %s/%s\n", np, ns_esc, cfg_esc);

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

    /* ── 4. The rows really are in MariaDB ────────────────────────────────── */
    {
        char sql[512];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM ufconfig_entry e JOIN ufconfig_config c ON c.config_id=e.config_id "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "WHERE n.namespace_name='%s' AND c.config_name='%s'", ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("counting entries");
        MYSQL_RES *res = mysql_store_result(g_cleanup_conn);
        MYSQL_ROW  row = res ? mysql_fetch_row(res) : NULL;
        printf("\nCOUNT(entry) for %s/%s  ->  %s\n", ns, cfg, row ? row[0] : "?");
        if (res) mysql_free_result(res);
    }

    /* ── 5. Replacing, not merging ────────────────────────────────────────── */
    /* A field the store holds but the document does not must disappear.

       It has to be injected out of band rather than omitted from a second
       document: every field this schema marks required has to be present in any
       document that loads at all, so there is no field a valid replacement
       could drop.  A sentinel row written straight into the table tests the
       property the driver actually claims — that write_all *replaces* the
       contents rather than merging into them. */
    {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO ufconfig_entry(config_id,path,vtype,value) "
                 "SELECT c.config_id,'ufsrv.sentinel_not_in_schema',2,'1' FROM ufconfig_config c "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "WHERE n.namespace_name='%s' AND c.config_name='%s'", ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("placing the sentinel");
    }
    long had_it = -1;
    {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM ufconfig_entry e JOIN ufconfig_config c ON c.config_id=e.config_id "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "WHERE n.namespace_name='%s' AND c.config_name='%s' AND e.path='ufsrv.sentinel_not_in_schema'",
                 ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("reading the sentinel back");
        MYSQL_RES *res = mysql_store_result(g_cleanup_conn);
        MYSQL_ROW  row = res ? mysql_fetch_row(res) : NULL;
        had_it = row ? strtol(row[0], NULL, 10) : -1;
        if (res) mysql_free_result(res);
    }
    printf("\nsentinel placed  %s\n", had_it == 1 ? "yes" : "NO — test is not measuring anything");
    if (had_it != 1) Fail("could not place the sentinel");

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

    long still_there = -1, post_len = -1;
    {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM ufconfig_entry e JOIN ufconfig_config c ON c.config_id=e.config_id "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "WHERE n.namespace_name='%s' AND c.config_name='%s' AND e.path='ufsrv.sentinel_not_in_schema'",
                 ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("re-reading the sentinel");
        MYSQL_RES *res = mysql_store_result(g_cleanup_conn);
        MYSQL_ROW  row = res ? mysql_fetch_row(res) : NULL;
        still_there = row ? strtol(row[0], NULL, 10) : -1;
        if (res) mysql_free_result(res);

        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM ufconfig_entry e JOIN ufconfig_config c ON c.config_id=e.config_id "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "WHERE n.namespace_name='%s' AND c.config_name='%s'", ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("re-counting entries");
        res = mysql_store_result(g_cleanup_conn);
        row = res ? mysql_fetch_row(res) : NULL;
        post_len = row ? strtol(row[0], NULL, 10) : -1;
        if (res) mysql_free_result(res);
    }
    printf("after rewrite    sentinel present=%s  entries=%ld\n",
           still_there ? "YES" : "no", post_len);
    if (still_there) Fail("write_all merged instead of replacing: a stale field survived");

    /* ── 6. Persist through the driver ────────────────────────────────────── */
    UfConfigSetInt(h, "testharn.knob_a", 3);
    st = UfConfigPersist(h);
    printf("\npersist         %s\n", UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) Fail("UfConfigPersist");

    {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "SELECT e.value FROM ufconfig_entry e JOIN ufconfig_config c ON c.config_id=e.config_id "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "WHERE n.namespace_name='%s' AND c.config_name='%s' AND e.path='testharn.knob_a'", ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("reading the persisted knob");
        MYSQL_RES *res = mysql_store_result(g_cleanup_conn);
        MYSQL_ROW  row = res ? mysql_fetch_row(res) : NULL;
        printf("  in mariadb    testharn.knob_a = %s\n", row && row[0] ? row[0] : "(absent)");
        int wrote = row && row[0];
        if (res) mysql_free_result(res);
        if (!wrote) Fail("persist did not reach the store");
    }

    /* ── 7. Reload: unchanged, then changed ───────────────────────────────── */
    UfConfigReloadReport rr;
    memset(&rr, 0, sizeof(rr));
    st = UfConfigReload(h, &rr);
    printf("\nreload (same)   %s  entries=%zu\n", UfConfigStatusString(st), rr.count);
    if (st != UF_CONFIG_NO_CHANGE) Fail("a reload with nothing changed must report NO_CHANGE");

    /* Move the store underneath it, the way another instance would. */
    {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "UPDATE ufconfig_entry e JOIN ufconfig_config c ON c.config_id=e.config_id "
                 "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "SET e.value='1234' WHERE n.namespace_name='%s' AND c.config_name='%s' "
                 "AND e.path='ufsrv.main_listener_port'", ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("moving the store");

        /* The version is how the core notices without reading every row. */
        snprintf(sql, sizeof(sql),
                 "UPDATE ufconfig_config c JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
                 "SET c.version=c.version+1 WHERE n.namespace_name='%s' AND c.config_name='%s'", ns_esc, cfg_esc);
        if (mysql_query(g_cleanup_conn, sql) != 0) FailSql("bumping the version");
    }

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
    CleanupNamespace();
    mysql_close(g_cleanup_conn);

    printf("\nPASS — every row this test created has been removed\n");
    return EXIT_SUCCESS;
}
