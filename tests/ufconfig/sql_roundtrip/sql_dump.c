/**
 * @file sql_dump.c
 * @brief Load a configuration *from* the store and write it out in every
 *        serialised form.
 *
 * The reverse of `sql_load.c`: the store is the origin, and the artefact is the
 * deliverable.  `sql_check_dump.py` then diffs what was written against the
 * store it came from and against the document that was originally loaded.
 *
 *     ./ufconfig_sql_dump <dsn> <outdir> [namespace] [config_name]
 *
 * Writes `dump.json`, `dump.yaml`, `dump.ini` and `dump.lua` into <outdir>.
 *
 * ## The defect this exists to catch
 *
 * An `encrypted = true` field keeps its envelope in `raw` and holds the
 * plaintext in `eff`.  Every reader that wants the plaintext asks for the
 * effective value; every *serialiser* must ask for the raw one.  If a
 * serialiser ever reached for the effective value instead, this program would
 * write a decrypted secret to disk — and nothing else in the tree looks at
 * serialiser output from a store-sourced handle, so nothing else would notice.
 * The dump is therefore also an exfiltration check, and `sql_check_dump.py`
 * asserts that no `enc:v1:` field came back without its envelope.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

typedef UfConfigStatus (*SerialiseFn)(const UfConfig *h, char **out);

static int sWrite(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return 0; }
    size_t n = strlen(text);
    size_t wrote = fwrite(text, 1, n, f);
    fclose(f);
    if (wrote != n) { fprintf(stderr, "%s: short write\n", path); return 0; }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dsn> <outdir> [namespace] [config_name]\n", argv[0]);
        return 2;
    }
    const char *dsn    = argv[1];
    const char *outdir = argv[2];
    const char *ns     = argc > 3 ? argv[3] : "ufcfgtest";
    const char *cfg    = argc > 4 ? argv[4] : "ufsrvwebsock";

    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "%s: %s\n", outdir, strerror(errno));
        return EXIT_FAILURE;
    }

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

    /* The store is the origin here — no file is read at all. */
    UfConfigStatus st = UfConfigLoad(h, &opt, NULL);
    if (st != UF_CONFIG_OK) {
        fprintf(stderr, "loading from %s/%s: %s (%s)\n", ns, cfg,
                UfConfigStatusString(st), UfConfigLastError()->message);
        return EXIT_FAILURE;
    }

    static const struct { const char *ext; SerialiseFn fn; } forms[] = {
        { "json", UfConfigToJsonAlloc },
        { "yaml", UfConfigToYamlAlloc },
        { "ini",  UfConfigToIniAlloc },
        { "lua",  UfConfigToLuaStyleAlloc },
    };

    printf("ufconfig SQL dump\n");
    printf("source      %s / %s (loaded from the store)\n", ns, cfg);
    int failures = 0;
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        char *text = NULL;
        UfConfigStatus s = forms[i].fn(h, &text);
        if (s != UF_CONFIG_OK || !text) {
            printf("  %-5s FAILED: %s\n", forms[i].ext, UfConfigStatusString(s));
            failures++;
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/dump.%s", outdir, forms[i].ext);
        if (!sWrite(path, text)) { failures++; free(text); continue; }
        printf("  %-5s %7zu bytes -> %s\n", forms[i].ext, strlen(text), path);
        free(text);
    }

    UfConfigDestroy(h);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
