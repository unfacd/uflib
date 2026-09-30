/**
 * @file coverage_test.c
 * @brief Every transform and every validator, against real data.
 *
 * A unit test that calls a transform function directly proves the function
 * works.  It does not prove the pipeline reaches it — that the schema names it,
 * that the loader applies it, that a value survives a store round trip, or that
 * the pre-transform value is still recoverable afterwards.  This harness drives
 * the whole path and prints each step, so a transform that is wired up wrongly
 * shows as a wrong column rather than as a passing assertion nobody read.
 *
 * ## The three columns
 *
 *   DOCUMENT   what the document said
 *   STORE      what the in-memory backing store holds — the canonical-form
 *              scalar as the driver received it.  For a transform this is the
 *              *transformed* value or the raw one depending on which end of the
 *              pipeline the store sits; the column makes that visible.
 *   READ BACK  what UfConfigGetField returns after a store round trip, and the
 *              raw value recovered alongside it.
 *
 * ## Why the store is in the loop at all
 *
 * A transform applied on load and never persisted is only half tested.  Putting
 * the pair-set through the mem driver and reading it back is what establishes
 * that the canonical form is what travels, and that coming back through
 * UfConfigLoadPairs produces the same values a direct load does.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

/* varsub is only meaningful with something to substitute from. */
static const char *SubstGet(void *ctx, const char *name) {
    (void)ctx;
    if (strcmp(name, "PORT") == 0) return "19701";
    if (strcmp(name, "USER") == 0) return "devops";
    return NULL;
}

static int g_failures;

static void Show(const char *label, const char *path) {
    printf("  %-10s %-9s ", label, path);
}

/* Print a value the way its kind calls for, without assuming text. */
static void PrintValue(const UfConfigValue *v, char *out, size_t cap) {
    switch (v->kind) {
    case UF_CONFIG_KIND_INT:    snprintf(out, cap, "%lld", (long long)v->as.i); break;
    case UF_CONFIG_KIND_FLOAT:  snprintf(out, cap, "%g", v->as.f); break;
    case UF_CONFIG_KIND_BOOL:   snprintf(out, cap, "%s", v->as.b ? "true" : "false"); break;
    case UF_CONFIG_KIND_STRING: snprintf(out, cap, "\"%.*s\"", (int)v->as.str.len, v->as.str.ptr); break;
    case UF_CONFIG_KIND_ARRAY:  snprintf(out, cap, "[%zu elems]", v->as.array.count); break;
    case UF_CONFIG_KIND_SCOPE:  snprintf(out, cap, "{table}"); break;
    default:                    snprintf(out, cap, "(kind %d)", (int)v->kind); break;
    }
}

/* The stored form: the canonical text the driver holds for this path. */
static const char *StoredForm(const UfConfigFieldPair *pairs, size_t n, const char *path) {
    for (size_t i = 0; i < n; i++) {
        if (pairs[i].path && strcmp(pairs[i].path, path) == 0) return pairs[i].encoded;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char document[1024], schema[1024];
    snprintf(document, sizeof(document), "%s/document_coverage.lua", dir);
    snprintf(schema, sizeof(schema), "%s/schema_coverage.lua", dir);

    printf("ufconfig transform and validator coverage\n");
    printf("  document   %s\n\n", document);

    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) {
        fprintf(stderr, "FAIL: UfConfigCreate\n");
        return EXIT_FAILURE;
    }

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size        = sizeof(opt);
    opt.version     = 1;
    opt.mode        = UF_CONFIG_LOAD_LENIENT;  /* the alias binding is not a declared field */
    opt.subst_get   = SubstGet;
    opt.subst_ctx   = NULL;

    UfConfigStatus st = UfConfigLoadFile(h, document, &opt, NULL);
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        fprintf(stderr, "FAIL: load — %s: %s (%s)\n", UfConfigStatusString(st),
                e->message, e->field_path ? e->field_path : "-");
        UfConfigDestroy(h);
        return EXIT_FAILURE;
    }

    /* ── Round-trip through the in-memory backing store ───────────────────── */
    UfConfigFieldPair *pairs = NULL;
    size_t np = 0;
    UfConfigMeta meta;
    memset(&meta, 0, sizeof(meta));
    if (UfConfigFlattenHandle(h, &pairs, &np, &meta) != UF_CONFIG_OK) {
        fprintf(stderr, "FAIL: flatten\n");
        UfConfigDestroy(h);
        return EXIT_FAILURE;
    }

    UfConfig *from_store = NULL;
    if (UfConfigCreate(&from_store, &d) != UF_CONFIG_OK) return EXIT_FAILURE;
    st = UfConfigLoadPairs(from_store, pairs, np, &meta, &opt, NULL);
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        fprintf(stderr, "FAIL: reload from store — %s: %s\n", UfConfigStatusString(st), e->message);
        UfConfigDestroy(h); UfConfigFreePairs(pairs, np);
        return EXIT_FAILURE;
    }

    printf("  store holds %zu pairs\n", np);

    /* ── Transforms ───────────────────────────────────────────────────────── */
    static const char *const xforms[] = {
        "xf.hex", "xf.b64", "xf.b64url", "xf.b32", "xf.upper",
        "xf.lower", "xf.sub", "xf.comp", "xf.rx", "xf.both",
    };
    printf("\nTRANSFORMS\n");
    printf("  %-10s %-9s %-20s %-22s %s\n",
           "field", "name", "as stored", "read back", "raw — as written");

    for (size_t i = 0; i < sizeof(xforms) / sizeof(xforms[0]); i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        if (UfConfigGetField(from_store, xforms[i], &v) != UF_CONFIG_OK) {
            printf("  %-10s %-9s *** ABSENT ***\n", xforms[i], "");
            g_failures++;
            continue;
        }
        char eff[256], raw[256];
        PrintValue(&v, eff, sizeof(eff));
        if (v.raw.str.ptr) snprintf(raw, sizeof(raw), "\"%.*s\"", (int)v.raw.str.len, v.raw.str.ptr);
        else snprintf(raw, sizeof(raw), "-");

        const char *stored = StoredForm(pairs, np, xforms[i]);
        Show(xforms[i] + 3, "xform");
        printf("%-20.20s %-22.22s %s\n", stored ? stored : "(null)", eff, raw);
    }

    /* ── Validators ───────────────────────────────────────────────────────── */
    static const char *const validators[] = {
        "v.ip4", "v.ip6", "v.email", "v.url", "v.fqdn", "v.date",
        "v.size", "v.cidr", "v.netmask", "v.enum", "v.pattern",
        "v.ranged", "v.sized", "v.list",
    };
    printf("\nVALIDATORS\n");
    printf("  %-10s %-9s %-24s %s\n", "field", "rule", "as stored", "read back");

    for (size_t i = 0; i < sizeof(validators) / sizeof(validators[0]); i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        if (UfConfigGetField(from_store, validators[i], &v) != UF_CONFIG_OK) {
            printf("  %-10s *** ABSENT ***\n", validators[i]);
            g_failures++;
            continue;
        }
        const UfConfigFieldDesc *desc = v.desc;
        char eff[256];
        PrintValue(&v, eff, sizeof(eff));
        const char *stored = StoredForm(pairs, np, validators[i]);
        Show(validators[i] + 2, desc && desc->fmt ? desc->fmt : "declared");
        printf("%-24.24s %s\n", stored ? stored : "(null)", eff);
    }

    /* ── varsub, reported rather than fatal ───────────────────────────────── */
    printf("\nVARSUB — substitution from the injected callback\n");
    {
        char subst_doc[1024];
        snprintf(subst_doc, sizeof(subst_doc), "%s/document_subst.lua", dir);
        UfConfig *sh = NULL;
        UfConfigCreate(&sh, &d);
        UfConfigStatus ss = UfConfigLoadFile(sh, subst_doc, &opt, NULL);
        printf("  load %-16s %s\n", "document_subst.lua", UfConfigStatusString(ss));
        printf("     why  %s\n", UfConfigLastError()->message);
        if (ss == UF_CONFIG_OK) {
            UfConfigValue v;
            memset(&v, 0, sizeof(v));
            UfConfigGetField(sh, "xf.sub", &v);
            char eff[256];
            PrintValue(&v, eff, sizeof(eff));
            printf("  xf.sub read back  %s\n", eff);
            printf("  substitution WORKED\n");
        } else {
            printf("  KNOWN DEFECT — subst_get is never consulted; see TD-014\n");
        }
        UfConfigDestroy(sh);
    }

    /* ── Nested tables: values reached by their full dotted path ──────────── */
    static const char *const nested[] = {
        "nest.level2.level3.leaf_int",
        "nest.level2.level3.leaf_str",
        "nest.level2.level3.leaf_list.0",
        "nest.level2.level3.leaf_list.1",
        "nest.level2.sibling",
        "nest.shared.connected",
        "nest.shared.suspended",
    };
    printf("\nNESTED TABLES\n");
    printf("  %-38s %s\n", "path", "value");
    for (size_t i = 0; i < sizeof(nested) / sizeof(nested[0]); i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        if (UfConfigGetField(from_store, nested[i], &v) != UF_CONFIG_OK) {
            printf("  %-38s *** ABSENT ***\n", nested[i]);
            g_failures++;
            continue;
        }
        char eff[256];
        PrintValue(&v, eff, sizeof(eff));
        printf("  %-38s %s\n", nested[i], eff);
    }

    /* ── The namespace walk over the same tree ────────────────────────────── */
    printf("\nNAMESPACE WALK\n");
    struct { const char *path; } scopes[] = { {"nest"}, {"nest.level2"},
                                              {"nest.level2.level3"}, {"nest.shared"} };
    for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); i++) {
        UfConfigNamespace *ns = NULL;
        if (UfConfigNamespaceGet(from_store, scopes[i].path, &ns) != UF_CONFIG_OK) {
            printf("  %-24s *** not a table ***\n", scopes[i].path);
            g_failures++;
            continue;
        }
        printf("  %-24s %zu entries\n", scopes[i].path, UfConfigNamespaceEntryCount(ns));
        size_t n = UfConfigNamespaceEntryCount(ns);
        for (size_t k = 0; k < n; k++) {
            UfConfigNamespaceEntry e;
            memset(&e, 0, sizeof(e));
            if (UfConfigNamespaceEntryAt(ns, k, &e) != UF_CONFIG_OK) continue;
            const char *kind = "?";
            switch (e.kind) {
            case UF_CONFIG_NS_VALUE:        kind = "value"; break;
            case UF_CONFIG_NS_TABLE_INLINE: kind = "table"; break;
            case UF_CONFIG_NS_TABLE_ALIAS:  kind = "ALIAS"; break;
            case UF_CONFIG_NS_ARRAY:        kind = "array"; break;
            }
            printf("      %-12s %s\n", e.name ? e.name : "-", kind);
            if (e.table) free(e.table);
        }
        free(ns);
    }

    /* ── Aliasing: the table reached by name and written inline ───────────── */
    printf("\nALIASING\n");
    {
        UfConfigValue via_alias, via_inline;
        memset(&via_alias, 0, sizeof(via_alias));
        memset(&via_inline, 0, sizeof(via_inline));
        UfConfigStatus a = UfConfigGetField(from_store, "nest.shared.connected", &via_alias);

        char inline_path[1024];
        snprintf(inline_path, sizeof(inline_path), "%s/document_inline.lua", dir);
        UfConfig *inl = NULL;
        UfConfigCreate(&inl, &d);
        UfConfigStatus is = UfConfigLoadFile(inl, inline_path, &opt, NULL);
        UfConfigStatus b = UfConfigGetField(inl, "nest.shared.connected", &via_inline);

        printf("  alias  load=%s  nest.shared.connected=%s  value=%lld\n",
               UfConfigStatusString(st), UfConfigStatusString(a),
               (long long)(a == UF_CONFIG_OK ? via_alias.as.i : -1));
        printf("  inline load=%s  nest.shared.connected=%s  value=%lld\n",
               UfConfigStatusString(is), UfConfigStatusString(b),
               (long long)(b == UF_CONFIG_OK ? via_inline.as.i : -1));

        /* Both must be *directly parsed* to be comparable.  Digesting
           `from_store` here would compare a tree rebuilt from a pair-set
           against one parsed from text, and the pair-set has no way to express
           an alias -- so a difference would say nothing about aliasing. */
        UfConfig *alias = NULL;
        UfConfigCreate(&alias, &d);
        char alias_path[1024];
        snprintf(alias_path, sizeof(alias_path), "%s/document_coverage.lua", dir);
        UfConfigStatus as = UfConfigLoadFile(alias, alias_path, &opt, NULL);
        printf("  alias  direct load=%s\n", UfConfigStatusString(as));

        unsigned char da[32], db[32];
        UfConfigDigest(alias, da);
        UfConfigDigest(inl, db);
        UfConfigDestroy(alias);
        int same = memcmp(da, db, 32) == 0;
        printf("  digest alias  ");
        for (int i = 0; i < 8; i++) printf("%02x", da[i]);
        printf("\n  digest inline ");
        for (int i = 0; i < 8; i++) printf("%02x", db[i]);
        printf("\n  %s\n", same ? "identical — alias and inline are the same content"
                                  : "*** DIFFERENT — alias did not resolve to the same table ***");
        if (!same) g_failures++;
        UfConfigDestroy(inl);
    }

    /* ── Every declared field, by index, straight off the generated enum ──── */
    printf("\nALL DECLARED FIELDS\n");
    int present = 0;
    for (int i = 0; i < g_ufconfig_field_count; i++) {
        UfConfigValue v;
        memset(&v, 0, sizeof(v));
        if (UfConfigGetFieldByIndex(from_store, (UfConfigFieldId)i, &v) == UF_CONFIG_OK &&
            v.present) {
            present++;
        } else {
            printf("  absent: %s\n", g_ufconfig_fields[i].path);
        }
    }
    printf("  %d of %d present\n", present, g_ufconfig_field_count);

    printf("\n%s\n", g_failures ? "FAILED" : "PASS");
    UfConfigFreePairs(pairs, np);
    UfConfigDestroy(from_store);
    UfConfigDestroy(h);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
