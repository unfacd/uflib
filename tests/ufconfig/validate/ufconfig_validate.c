/**
 * @file ufconfig_validate.c
 * @brief A standalone validator: load a config file, report what the schema
 *        made of it.
 *
 *     ufconfig-validate --config-file=<path> [options]
 *
 * It is a host application in miniature — it builds a descriptor from the
 * generated schema, injects it at UfConfigCreate, loads the file the caller
 * named, and prints the outcome.  Nothing is stubbed: the parse, the alias
 * resolution, the schema application, the transforms and the validators are
 * all the library's, reached the way a server reaches them.
 *
 * ## The truth table
 *
 *   --table=<file>   run every case listed in a TSV of <config-file> <status>
 *   --all            same, using the table beside the cases
 *
 * The table is the point.  A file whose result is not written down is a file
 * whose result nobody has agreed on; when the two disagree, that is the
 * finding, whether the cause is a bug in the library or a stale expectation.
 *
 * ## Exit status
 *
 *   0  every case matched its expectation
 *   1  at least one did not
 *   2  the harness could not run — bad arguments, unreadable file
 */

#include <uflib/ufconfig/ufconfig.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

static int g_pass, g_fail, g_total;

/* The status a truth-table row names, or a value no status can take. */
static UfConfigStatus StatusFromName(const char *name) {
    static const struct { const char *name; UfConfigStatus st; } map[] = {
        { "OK",             UF_CONFIG_OK },
        { "REQUIRED",       UF_CONFIG_ERR_REQUIRED },
        { "TYPE_MISMATCH",  UF_CONFIG_ERR_TYPE_MISMATCH },
        { "VALIDATION",     UF_CONFIG_ERR_VALIDATION },
        { "IMMUTABLE",      UF_CONFIG_ERR_IMMUTABLE },
        { "UNKNOWN_FIELD",  UF_CONFIG_ERR_UNKNOWN_FIELD },
        { "PARSE",          UF_CONFIG_ERR_PARSE },
        { "CYCLE",          UF_CONFIG_ERR_CYCLE },
        { "ALIAS_UNRESOLVED", UF_CONFIG_ERR_ALIAS_UNRESOLVED },
        { "NOT_A_TABLE",    UF_CONFIG_ERR_NOT_A_TABLE },
        { "LIMIT_EXCEEDED", UF_CONFIG_ERR_LIMIT_EXCEEDED },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].name, name) == 0) return map[i].st;
    }
    return (UfConfigStatus)-1;
}

/* Load one file and report.  Returns the status the library gave. */
static UfConfigStatus RunOne(const char *path, UfConfigLoadMode mode,
                             int verbose, const char *expect_name) {
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) {
        fprintf(stderr, "cannot create a handle — the schema descriptor is incomplete\n");
        exit(2);
    }

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size    = sizeof(opt);
    opt.version = 1;
    opt.mode    = mode;

    UfConfigLoadReport report;
    memset(&report, 0, sizeof(report));

    UfConfigStatus st = UfConfigLoadFile(h, path, &opt, &report);
    const UfConfigError *e = UfConfigLastError();

    g_total++;

    if (expect_name) {
        UfConfigStatus want = StatusFromName(expect_name);
        if (st == want) {
            g_pass++;
            printf("  ok    %-38s %s\n", path, UfConfigStatusString(st));
        } else {
            g_fail++;
            printf("  FAIL  %-38s expected %-18s got %s\n", path, expect_name,
                   UfConfigStatusString(st));
            if (e && e->message[0]) printf("        %s\n", e->message);
            UfConfigDestroy(h);
            return st;
        }
    } else {
        printf("  file    %s\n", path);
        printf("  status  %s\n", UfConfigStatusString(st));
        if (e && e->message[0]) {
            printf("  detail  %s\n", e->message);
            printf("  field   %s\n", e->field_path ? e->field_path : "-");
            printf("  at      line %d, column %d\n", e->line, e->column);
        }
    }

    /* verbose: show what the schema made of every field, so a pass is
       inspectable rather than merely asserted */
    if (verbose && st == UF_CONFIG_OK) {
        printf("  fields  %d of %d declared\n", g_ufconfig_field_count, g_ufconfig_field_count);
        for (int i = 0; i < g_ufconfig_field_count; i++) {
            UfConfigValue v;
            memset(&v, 0, sizeof(v));
            if (UfConfigGetFieldByIndex(h, (UfConfigFieldId)i, &v) != UF_CONFIG_OK || !v.present)
                continue;
            const char *eff = "-";
            char buf[256];
            switch (v.kind) {
            case UF_CONFIG_KIND_INT:    snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i); eff = buf; break;
            case UF_CONFIG_KIND_FLOAT:  snprintf(buf, sizeof(buf), "%g", v.as.f); eff = buf; break;
            case UF_CONFIG_KIND_BOOL:   eff = v.as.b ? "true" : "false"; break;
            case UF_CONFIG_KIND_STRING: snprintf(buf, sizeof(buf), "\"%.*s\"", (int)v.as.str.len, v.as.str.ptr); eff = buf; break;
            case UF_CONFIG_KIND_ARRAY:  snprintf(buf, sizeof(buf), "[%zu]", v.as.array.count); eff = buf; break;
            default:                    eff = "(table)"; break;
            }
            printf("    %-42s %s\n", g_ufconfig_fields[i].path, eff);
        }
        if (report.note_count) printf("    %zu note(s)\n", report.note_count);
    }

    UfConfigDestroy(h);
    return st;
}

/* Every entry of the generated enum, by index, printing what the handle holds
   for it.  This is the generated accessor surface walked exhaustively rather
   than sampled: a field the schema declares but nothing ever set shows as
   absent, and one a transform changed shows both faces -- `effective` is what
   the schema made of it, `as written` is what the document said. */
static void PrintEnumGetters(const char *path, UfConfigLoadMode mode) {
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return;

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = mode;

    UfConfigStatus st = UfConfigLoadFile(h, path, &opt, NULL);
    printf("file    %s\n", path);
    printf("status  %s\n", UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        if (e && e->message[0]) printf("detail  %s\n", e->message);
    }
    if (st != UF_CONFIG_OK) {
        printf("\n  nothing loaded: the file was refused, so every declared field\n"
               "  would report absent.  The refusal is above.\n");
        UfConfigDestroy(h);
        return;
    }

    printf("\n  %-4s %-46s %-9s %-24s %s\n",
           "id", "enum / path", "present", "effective", "as written");

    for (int i = 0; i < g_ufconfig_field_count; i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        UfConfigStatus gs = UfConfigGetFieldByIndex(h, (UfConfigFieldId)i, &v);

        char eff[192] = "-", raw[192] = "-";
        if (gs == UF_CONFIG_OK && v.present) {
            switch (v.kind) {
            case UF_CONFIG_KIND_INT:
                snprintf(eff, sizeof(eff), "%lld", (long long)v.as.i);
                if (v.raw.str.ptr == NULL && v.desc) snprintf(raw, sizeof(raw), "(default)");
                else snprintf(raw, sizeof(raw), "%lld", (long long)v.raw.i);
                break;
            case UF_CONFIG_KIND_FLOAT:
                snprintf(eff, sizeof(eff), "%g", v.as.f);
                snprintf(raw, sizeof(raw), "%g", v.raw.f);
                break;
            case UF_CONFIG_KIND_BOOL:
                snprintf(eff, sizeof(eff), "%s", v.as.b ? "true" : "false");
                snprintf(raw, sizeof(raw), "%s", v.raw.b ? "true" : "false");
                break;
            case UF_CONFIG_KIND_STRING:
                snprintf(eff, sizeof(eff), "\"%.*s\"", (int)v.as.str.len, v.as.str.ptr);
                if (v.raw.str.ptr) snprintf(raw, sizeof(raw), "\"%.*s\"", (int)v.raw.str.len, v.raw.str.ptr);
                else snprintf(raw, sizeof(raw), "(default)");
                break;
            case UF_CONFIG_KIND_ARRAY:
                snprintf(eff, sizeof(eff), "[%zu elements]", v.as.array.count);
                snprintf(raw, sizeof(raw), "[%zu]", v.as.array.count);
                break;
            case UF_CONFIG_KIND_SCOPE: snprintf(eff, sizeof(eff), "{table}"); break;
            default: snprintf(eff, sizeof(eff), "kind %d", (int)v.kind); break;
            }
        }
        printf("  %-4d %-46s %-9s %-24.24s %s\n", i, g_ufconfig_fields[i].path,
               (gs == UF_CONFIG_OK && v.present) ? "yes" : "no", eff, raw);
    }
    printf("\n  %d enum entries, %d declared\n", g_ufconfig_field_count,
           g_ufconfig_field_count);
    UfConfigDestroy(h);
}

/* Every field the configuration actually holds.
 *
 * Driven from the instance rather than the schema: `UfConfigFlattenHandle`
 * returns the pair-set the driver boundary would carry, and its paths are the
 * loaded ones.  A field the schema declares but the document omitted is not
 * here -- walk the enum for that, which is what --print-enum-getters does.
 *
 * Each row shows the path, what the store would hold for it, and what the
 * handle returns -- so a transform is visible as the stored and effective
 * columns differing. */
static void PrintLoadedFields(const char *path, UfConfigLoadMode mode) {
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return;

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = mode;

    UfConfigStatus st = UfConfigLoadFile(h, path, &opt, NULL);
    printf("file    %s\nstatus  %s\n", path, UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        if (e && e->message[0]) printf("detail  %s\n", e->message);
    }

    /* A refused load publishes nothing, so asking for its fields would report
       an empty configuration -- which reads as "it loaded and is empty" rather
       than "it did not load".  Say which. */
    if (st != UF_CONFIG_OK) {
        printf("\n  nothing loaded: the file was refused, so the handle holds no\n"
               "  configuration to enumerate.  The refusal is above.\n");
        UfConfigDestroy(h);
        return;
    }

    UfConfigFieldPair *pairs = NULL;
    size_t n = 0;
    UfConfigMeta meta;
    memset(&meta, 0, sizeof(meta));
    if (UfConfigFlattenHandle(h, &pairs, &n, &meta) != UF_CONFIG_OK) {
        printf("\n  the handle loaded but produced no pair-set\n");
        UfConfigDestroy(h);
        return;
    }

    printf("\n  %-4s %-42s %-22s %s\n", "#", "path", "as stored", "read back");
    for (size_t i = 0; i < n; i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        char eff[128] = "-";
        if (UfConfigGetField(h, pairs[i].path, &v) == UF_CONFIG_OK && v.present) {
            switch (v.kind) {
            case UF_CONFIG_KIND_INT:    snprintf(eff, sizeof(eff), "%lld", (long long)v.as.i); break;
            case UF_CONFIG_KIND_FLOAT:  snprintf(eff, sizeof(eff), "%g", v.as.f); break;
            case UF_CONFIG_KIND_BOOL:   snprintf(eff, sizeof(eff), "%s", v.as.b ? "true" : "false"); break;
            case UF_CONFIG_KIND_STRING: snprintf(eff, sizeof(eff), "\"%.*s\"", (int)v.as.str.len, v.as.str.ptr); break;
            case UF_CONFIG_KIND_ARRAY:  snprintf(eff, sizeof(eff), "[%zu]", v.as.array.count); break;
            case UF_CONFIG_KIND_SCOPE:  snprintf(eff, sizeof(eff), "{table}"); break;
            default:                    snprintf(eff, sizeof(eff), "kind %d", (int)v.kind); break;
            }
        }
        printf("  %-4zu %-42s %-22.22s %s\n", i, pairs[i].path,
               pairs[i].encoded ? pairs[i].encoded : "(null)", eff);
    }
    printf("\n  %zu loaded fields\n", n);

    UfConfigFreePairs(pairs, n);
    UfConfigDestroy(h);
}

/* Generated: calls every accessor the schema produced, by name.  See
   make_named_getters.py — this is the only place that reaches all of them, so
   it is what proves they exist, link, and have the signatures they claim. */
extern int UfConfigCallAllNamedGetters(UfConfig *h);
extern const int g_ufconfig_named_getter_count;
typedef struct {
    const char *path;
    const char *name;
    UfConfigStatus (*get)(const UfConfig *h, UfConfigValue *out);
} UfConfigNamedGetter;
extern const UfConfigNamedGetter g_ufconfig_named_getters[];

/* Render a value so two routes' answers can be compared as text.  Comparing
   the structs directly would compare borrowed pointers, which differ even when
   the values agree. */
static void RenderValue(const UfConfigValue *v, char *out, size_t cap) {
    if (!v->present) { snprintf(out, cap, "(absent)"); return; }
    switch (v->kind) {
    case UF_CONFIG_KIND_INT:    snprintf(out, cap, "int:%lld", (long long)v->as.i); break;
    case UF_CONFIG_KIND_FLOAT:  snprintf(out, cap, "float:%.17g", v->as.f); break;
    case UF_CONFIG_KIND_BOOL:   snprintf(out, cap, "bool:%s", v->as.b ? "true" : "false"); break;
    case UF_CONFIG_KIND_STRING: snprintf(out, cap, "str:%.*s", (int)v->as.str.len, v->as.str.ptr); break;
    case UF_CONFIG_KIND_ARRAY:  snprintf(out, cap, "array:%zu", v->as.array.count); break;
    case UF_CONFIG_KIND_SCOPE:  snprintf(out, cap, "table"); break;
    default:                    snprintf(out, cap, "kind:%d", (int)v->kind); break;
    }
}

/* The same field, reached three ways, must answer the same.
 *
 *   named   UfConfigGet<Xxx>(h, &v)                  -- what --print-named-getters drives
 *   by id   UfConfigGetFieldByIndex(h, id, &v)       -- what --print-enum-getters drives
 *   by path UfConfigGetField(h, path, &v)            -- what --print-fields drives
 *
 * The three are separate code paths: the named accessor goes through the index,
 * the path lookup through the perfect hash and then a tree walk.  A field one
 * of them can reach and another cannot is a defect that no single-route test
 * would show, and this is the only place the routes are compared.
 */
static int CrossCheckRoutes(const char *path, UfConfigLoadMode mode) {
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return 1;

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = mode;

    UfConfigStatus st = UfConfigLoadFile(h, path, &opt, NULL);
    printf("file    %s\nstatus  %s\n", path, UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        if (e && e->message[0]) printf("detail  %s\n", e->message);
        UfConfigDestroy(h);
        return 0;
    }

    if (g_ufconfig_named_getter_count != g_ufconfig_field_count) {
        printf("\n  FAIL  %d accessors against %d declared fields\n",
               g_ufconfig_named_getter_count, g_ufconfig_field_count);
        UfConfigDestroy(h);
        return 1;
    }

    int mismatches = 0, checked = 0;
    printf("\n  %-4s %-42s %s\n", "id", "field", "named / by-id / by-path");
    for (int i = 0; i < g_ufconfig_field_count; i++) {
        UfConfigValue vn, vi, vp;
        memset(&vn, 0, sizeof(vn));
        memset(&vi, 0, sizeof(vi));
        memset(&vp, 0, sizeof(vp));

        UfConfigStatus sn = g_ufconfig_named_getters[i].get(h, &vn);
        UfConfigStatus si = UfConfigGetFieldByIndex(h, i, &vi);
        UfConfigStatus sp = UfConfigGetField(h, g_ufconfig_fields[i].path, &vp);

        char rn[256], ri[256], rp[256];
        RenderValue(&vn, rn, sizeof(rn));
        RenderValue(&vi, ri, sizeof(ri));
        RenderValue(&vp, rp, sizeof(rp));

        /* The statuses must agree too, not just the values: a route that
           returns OK with an absent value and one that returns NOFIELD are
           both "no value" but they are not the same answer. */
        int agree = (sn == si && si == sp) &&
                    (strcmp(rn, ri) == 0 && strcmp(ri, rp) == 0);
        checked++;
        if (!agree) {
            mismatches++;
            printf("  %-4d %-42s *** DISAGREE ***\n", i, g_ufconfig_fields[i].path);
            printf("       named  %-24s %s\n", UfConfigStatusString(sn), rn);
            printf("       by id  %-24s %s\n", UfConfigStatusString(si), ri);
            printf("       by path %-23s %s\n", UfConfigStatusString(sp), rp);
        }
    }
    printf("\n  %d fields checked across three routes, %d disagreements\n",
           checked, mismatches);

    /* And the paths themselves: the accessor table and the descriptor table
       must name the same fields, or the index used above is comparing the
       wrong pairs. */
    int path_mismatch = 0;
    for (int i = 0; i < g_ufconfig_field_count; i++) {
        if (strcmp(g_ufconfig_named_getters[i].path, g_ufconfig_fields[i].path) != 0) {
            printf("  %-4d accessor %s covers %s, descriptor says %s\n", i,
                   g_ufconfig_named_getters[i].name, g_ufconfig_named_getters[i].path,
                   g_ufconfig_fields[i].path);
            path_mismatch++;
        }
    }
    printf("  %d path mismatches between the accessor table and the descriptor table\n",
           path_mismatch);

    UfConfigDestroy(h);
    return mismatches + path_mismatch ? 1 : 0;
}

/* One row of the truth table. */

/* Every named getter, called by name, with what each returned. */
static void PrintNamedGetters(const char *path, UfConfigLoadMode mode) {
    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return;

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = mode;

    UfConfigStatus st = UfConfigLoadFile(h, path, &opt, NULL);
    printf("file    %s\nstatus  %s\n", path, UfConfigStatusString(st));
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        if (e && e->message[0]) printf("detail  %s\n", e->message);
        printf("\n  nothing loaded — every accessor would answer NOFIELD\n");
        UfConfigDestroy(h);
        return;
    }

    int ok = UfConfigCallAllNamedGetters(h);
    printf("\n  %d accessors, called by name\n", g_ufconfig_named_getter_count);
    printf("  %d answered OK\n", ok);
    if (ok != g_ufconfig_named_getter_count) {
        printf("  %d did not — a declared field the configuration does not hold\n",
               g_ufconfig_named_getter_count - ok);
    }
    printf("\n  every accessor compiled, linked and was called — a missing or\n"
           "  renamed one is a link error in named_getters.c, not a count here\n");
    UfConfigDestroy(h);
}

/* One row of the truth table. */
static int RunTable(const char *table_path, const char *case_dir,
                    UfConfigLoadMode mode, int verbose) {
    FILE *f = fopen(table_path, "r");
    if (!f) { fprintf(stderr, "cannot open the truth table: %s\n", table_path); return 2; }

    printf("truth table  %s\n\n", table_path);
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", case_dir, line);
        RunOne(path, mode, verbose, tab + 1);
    }
    fclose(f);
    printf("\n%d of %d matched\n", g_pass, g_total);
    return g_fail ? 1 : 0;
}

static void Usage(const char *argv0) {
    printf(
        "usage: %s --config-file=<path> [--config-mode=MODE] [--verbose]\n"
        "       %s --table=<tsv> --cases=<dir> [options]\n\n"
        "  --config-file=PATH       validate one configuration file\n"
        "  --table=PATH             run a truth table of <file>\\t<expected status>\n"
        "  --cases=DIR              directory the table's filenames are relative to\n"
        "  --config-mode=MODE        strict (default) or lenient\n"
        "                             strict  \u2014 an undeclared field is an error\n"
        "                             lenient \u2014 an undeclared field is carried and reported\n"
        "  --verbose, -v        print every field the schema produced\n"
        "  --print-fields       loop every LOADED path and print it with its value\n"
        "  --print-named-getters call every generated accessor by name\n"
        "  --cross-check        reach each field three ways and require agreement\n"
        "  --print-enum-getters walk every generated enum id and print what the\n"
        "                       handle holds -- effective and as-written\n"
        "  --help, -h           this text\n\n"
        "Exit: 0 all matched, 1 a mismatch, 2 the harness could not run.\n",
        argv0, argv0);
}

int main(int argc, char **argv) {
    static const struct option long_opts[] = {
        { "config-file", required_argument, NULL, 'c' },
        { "table",       required_argument, NULL, 't' },
        { "cases",       required_argument, NULL, 'd' },
        { "config-mode", required_argument, NULL, 'm' },
        { "verbose",     no_argument,       NULL, 'v' },
        { "print-enum-getters", no_argument, NULL, 'e' },
        { "print-fields",       no_argument, NULL, 'f' },
        { "print-named-getters", no_argument, NULL, 'n' },
        { "cross-check",        no_argument, NULL, 'x' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    const char *config = NULL, *table = NULL, *cases = ".";
    UfConfigLoadMode mode = UF_CONFIG_LOAD_STRICT;
    int verbose = 0, enum_getters = 0, print_fields = 0, named_getters = 0, cross_check = 0;

    int c;
    while ((c = getopt_long(argc, argv, "c:t:d:m:vh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'c': config  = optarg; break;
        case 't': table   = optarg; break;
        case 'd': cases   = optarg; break;
        case 'm':
            if (strcmp(optarg, "strict") == 0)       mode = UF_CONFIG_LOAD_STRICT;
            else if (strcmp(optarg, "lenient") == 0) mode = UF_CONFIG_LOAD_LENIENT;
            else {
                fprintf(stderr, "unknown config mode %s\n", optarg);
                Usage(argv[0]);
                return 2;
            }
            break;
        case 'v': verbose = 1; break;
        case 'e': enum_getters = 1; break;
        case 'f': print_fields = 1; break;
        case 'n': named_getters = 1; break;
        case 'x': cross_check = 1; break;
        case 'h': Usage(argv[0]); return 0;
        default:  Usage(argv[0]); return 2;
        }
    }

    if (!config && !table) { Usage(argv[0]); return 2; }
    if (cross_check) {
        if (!config) { fprintf(stderr, "--cross-check needs --config-file\n"); return 2; }
        return CrossCheckRoutes(config, mode);
    }
    if (named_getters) {
        if (!config) { fprintf(stderr, "--print-named-getters needs --config-file\n"); return 2; }
        PrintNamedGetters(config, mode);
        return 0;
    }
    if (print_fields) {
        if (!config) { fprintf(stderr, "--print-fields needs --config-file\n"); return 2; }
        PrintLoadedFields(config, mode);
        return 0;
    }
    if (enum_getters) {
        if (!config) { fprintf(stderr, "--print-enum-getters needs --config-file\n"); return 2; }
        PrintEnumGetters(config, mode);
        return 0;
    }
    printf("config mode  %s\n\n", mode == UF_CONFIG_LOAD_STRICT ? "strict" : "lenient");
    if (table) return RunTable(table, cases, mode, verbose);
    RunOne(config, mode, verbose, NULL);
    return 0;
}
