/**
 * @file ufconfig_host_app.c
 * @brief A sample host application, end to end.
 *
 * This is what a consumer of the config module looks like.  It exists to show
 * the boundary in one place: what the application generates, what it owns, and
 * what it hands to the library.
 *
 * ## The division of labour
 *
 * The library is machinery — a parser, a validator, a canonicaliser and a
 * driver layer.  It holds no schema, because a schema is not a general-purpose
 * thing: it says which fields this application has, what they default to, and
 * what range or format they must satisfy.  That is the application's business,
 * and it changes when the application changes, not when the library does.
 *
 * So the flow is:
 *
 *   1. This application's build runs the generation pipeline over its own
 *      schema files.  That produces the artefacts compiled in below.
 *   2. At start-up the application fills a UfConfigDescriptor with them and
 *      calls UfConfigCreate.
 *   3. The library now has everything it needs and owns none of it.
 *
 * The three generated symbols below are the entire contract between the two
 * sides.  Note that the library never learns where they came from, and this
 * application never learns how they are used.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── The application's generated artefacts ───────────────────────────────────
 *
 * Produced by the generation pipeline from this application's schema, and
 * compiled into this application — not into the library.  Declared here by
 * hand rather than through a generated header, because a generated header is
 * this application's own file and the declarations are three lines:
 *
 *   g_ufconfig_fields        the field table
 *   g_ufconfig_field_count   how many entries it has
 *   UfConfigLookupPath       path -> index, or -1
 */
extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

/* ── The application's schema, expressed as a descriptor ───────────────────── */

static UfConfigDescriptor MakeDescriptor(void) {
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    /* No logger injected here so that this sample runs without one set up.
       A real application passes its own, and the module logs to it rather than
       opening a destination of its own:
         d.uf_logger = app_logger;   */
    d.uf_logger   = NULL;

    /* Where the configuration comes from.  Stated once, here; the load paths
       below consult it rather than being told again on every call.

       A file origin leaves these clear and uses UfConfigLoadFile(h, path).
       A networked origin is named by filling in the endpoint that applies: */
    d.kind = UF_CONFIG_BACKEND_FILE;
    if (getenv("UFCONFIG_DEMO_REDIS")) {
        d.kind = UF_CONFIG_BACKEND_REDIS;
        d.redis.address   = "10.0.0.5";
        d.redis.port      = 6379;
        d.redis.username  = "ufsrv";
        d.redis.password  = getenv("UFCONFIG_DEMO_REDIS_PASSWORD");
        d.redis.ns = "alice";
    } else if (getenv("UFCONFIG_DEMO_SQL")) {
        d.kind = UF_CONFIG_BACKEND_SQL;
        d.sql.address   = "db.ufsrv.unfacd.com";
        d.sql.port      = 3306;
        d.sql.username  = "ufsrv_user";
        d.sql.password  = getenv("UFCONFIG_DEMO_SQL_PASSWORD");
        d.sql.ns = "alice";
    }
    return d;
}

static void Report(const char *what, UfConfigStatus st) {
    printf("  %-28s %s\n", what, UfConfigStatusString(st));
    if (st != UF_CONFIG_OK && st != UF_CONFIG_NO_CHANGE) {
        const UfConfigError *e = UfConfigLastError();
        if (e && e->message[0]) {
            printf("  %-28s   at %s:%d — %s\n", "", e->field_path ? e->field_path : "-",
                   e->line, e->message);
        }
    }
}

int main(int argc, char **argv) {
    const char *document = argc > 1 ? argv[1] : "sample.strict.lua";

    printf("ufconfig host application\n\n");

    /* ── 1. Inject the schema ─────────────────────────────────────────────── */
    printf("schema\n");
    UfConfigDescriptor descriptor = MakeDescriptor();
    printf("  %-28s %zu fields, lookup %s\n", "injected",
           descriptor.field_count, descriptor.lookup ? "present" : "MISSING");
    /* The enum starts at 1, so a switch rather than an array index. */
    const char *origin_name;
    switch (descriptor.kind) {
    case UF_CONFIG_BACKEND_FILE:  origin_name = "FILE";  break;
    case UF_CONFIG_BACKEND_MEM:   origin_name = "MEM";   break;
    case UF_CONFIG_BACKEND_REDIS: origin_name = "REDIS"; break;
    case UF_CONFIG_BACKEND_SQL:   origin_name = "SQL";   break;
    default:                      origin_name = "?";     break;
    }
    printf("  %-28s %s\n", "origin", origin_name);
    if (descriptor.kind == UF_CONFIG_BACKEND_REDIS) {
        printf("  %-28s %s:%d ns=%s\n", "  redis",
               descriptor.redis.address, descriptor.redis.port,
               descriptor.redis.ns ? descriptor.redis.ns : "-");
    } else if (descriptor.kind == UF_CONFIG_BACKEND_SQL) {
        printf("  %-28s %s:%d ns=%s\n", "  sql",
               descriptor.sql.address, descriptor.sql.port,
               descriptor.sql.ns ? descriptor.sql.ns : "-");
    }

    UfConfig *config = NULL;
    UfConfigStatus st = UfConfigCreate(&config, &descriptor);
    Report("UfConfigCreate", st);
    if (st != UF_CONFIG_OK) return EXIT_FAILURE;

    /* ── 2. Load a document ───────────────────────────────────────────────── */
    printf("\nload\n");
    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size    = sizeof(opt);
    opt.version = 1;
    opt.mode    = UF_CONFIG_LOAD_STRICT;

    st = UfConfigLoadFile(config, document, &opt, NULL);
    Report("UfConfigLoadFile", st);
    if (st != UF_CONFIG_OK) {
        UfConfigDestroy(config);
        return EXIT_FAILURE;
    }

    /* ── 3. Read fields ───────────────────────────────────────────────────── */
    printf("\nread\n");
    static const char *const paths[] = {
        "ufsrv.server_run_mode",
        "ufsrv.main_listener_port",
        "ufsrv.main_listener_address",
        "ufnet.db_backend.port",
        "ufsrvwebsock.max_frame_size",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        st = UfConfigGetField(config, paths[i], &v);
        if (st != UF_CONFIG_OK) {
            printf("  %-34s %s\n", paths[i], UfConfigStatusString(st));
            continue;
        }
        switch (v.kind) {
        case UF_CONFIG_KIND_INT:
            printf("  %-34s %lld\n", paths[i], (long long)v.as.i);
            break;
        case UF_CONFIG_KIND_STRING:
            printf("  %-34s \"%.*s\"\n", paths[i], (int)v.as.str.len, v.as.str.ptr);
            break;
        case UF_CONFIG_KIND_BOOL:
            printf("  %-34s %s\n", paths[i], v.as.b ? "true" : "false");
            break;
        default:
            printf("  %-34s (kind %d)\n", paths[i], (int)v.kind);
            break;
        }
    }

    /* The same field by generated identifier — no path string, no walk. */
    UfConfigValue by_index;
    memset(&by_index, 0, sizeof(by_index));
    st = UfConfigGetFieldByIndex(config, UF_CONFIG_FIELD_UFSRV_MAIN_LISTENER_PORT, &by_index);
    if (st == UF_CONFIG_OK) {
        printf("  %-34s %lld\n", "by generated index", (long long)by_index.as.i);
    }

    /* ── 4. The digest the library computes over canonical form ───────────── */
    printf("\ncanonical digest\n");
    unsigned char digest[32];
    if (UfConfigDigest(config, digest) == UF_CONFIG_OK) {
        printf("  ");
        for (int i = 0; i < 32; i++) printf("%02x", digest[i]);
        printf("\n  (key order, whitespace and comments do not affect this)\n");
    }

    /* ── 5. Mutate a field the schema marks mutable ───────────────────────── */
    printf("\nmutate\n");
    st = UfConfigSetInt(config, "testharn.knob_a", 3);
    Report("set testharn.knob_a = 3", st);

    st = UfConfigSetInt(config, "ufsrv.main_listener_port", 1);
    Report("set main_listener_port", st);
    printf("  %-28s (the schema marks it immutable)\n", "");

    /* ── 6. Walk the whole tree through the namespace interface ───────────── */
    printf("\ntree\n");
    UfConfigNamespace *ns = NULL;
    UfConfigRootGet(config, &ns);
    if (ns) {
        size_t n = UfConfigNamespaceEntryCount(ns);
        for (size_t i = 0; i < n; i++) {
            UfConfigNamespaceEntry e;
            memset(&e, 0, sizeof(e));
            if (UfConfigNamespaceEntryAt(ns, i, &e) != UF_CONFIG_OK) continue;
            const char *k = "?";
            switch (e.kind) {
            case UF_CONFIG_NS_VALUE:        k = "value"; break;
            case UF_CONFIG_NS_TABLE_INLINE: k = "table"; break;
            case UF_CONFIG_NS_TABLE_ALIAS:  k = "alias"; break;
            case UF_CONFIG_NS_ARRAY:        k = "array"; break;
            }
            printf("  %-16s %-6s", e.name ? e.name : "-", k);
            if (e.kind == UF_CONFIG_NS_VALUE && e.value.kind == UF_CONFIG_KIND_INT)
                printf(" = %lld", (long long)e.value.as.i);
            else if (e.kind == UF_CONFIG_NS_VALUE && e.value.kind == UF_CONFIG_KIND_STRING)
                printf(" = \"%.*s\"", (int)e.value.as.str.len, e.value.as.str.ptr);
            printf("\n");
            if (e.table) free(e.table);
        }
        free(ns);
    }

    /* ── 7. Every serialiser the module offers ────────────────────────────── */
    printf("\nserialisers\n");
    struct { const char *name; UfConfigStatus (*fn)(const UfConfig *, char **); } ser[] = {
        { "json",  UfConfigToJsonAlloc },
        { "yaml",  UfConfigToYamlAlloc },
        { "ini",   UfConfigToIniAlloc },
        { "lua",   UfConfigToLuaStyleAlloc },
    };
    for (size_t i = 0; i < sizeof(ser) / sizeof(ser[0]); i++) {
        char *out = NULL;
        if (ser[i].fn(config, &out) == UF_CONFIG_OK && out) {
            /* First line only: the point is that each produces something and
               they differ, not to dump a whole document. */
            const char *nl = strchr(out, '\n');
            printf("  %-5s %zu bytes   %.*s\n", ser[i].name, strlen(out),
                   (int)(nl ? nl - out : 40), out);
            free(out);
        } else {
            printf("  %-5s (refused)\n", ser[i].name);
        }
    }

    /* ── 8. Every generated accessor, driven from the field table ─────────── */
    printf("\nall %d declared fields, via generated accessors\n", g_ufconfig_field_count);
    int present = 0, absent = 0;
    for (int i = 0; i < g_ufconfig_field_count; i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        if (UfConfigGetFieldByIndex(config, (UfConfigFieldId)i, &v) == UF_CONFIG_OK && v.present) {
            present++;
        } else {
            absent++;
        }
    }
    printf("  present %d   absent %d   total %d\n", present, absent,
           g_ufconfig_field_count);

    /* ── 9. Release ───────────────────────────────────────────────────────── */
    UfConfigDestroy(config);
    printf("\ndone — every borrowed pointer above is invalid from here\n");
    return EXIT_SUCCESS;
}
