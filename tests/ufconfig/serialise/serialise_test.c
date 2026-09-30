/**
 * @file serialise_test.c
 * @brief Golden-file tests for the serialisers: did they expose the values?
 *
 * The question is not whether the output is well-formed JSON or sensible YAML.
 * It is whether the values that were loaded came out the other side.  A
 * serialiser that emits structurally perfect output with a field missing has
 * failed at the only thing it is for, and a test that asks "is this valid
 * JSON?" would pass it.
 *
 * So each serialiser's output is compared **byte for byte** against a file in
 * this directory.  Those files are the agreement about what gets exposed.  When
 * one differs the question is not "how do I make the test pass" but "did the
 * emitter change, or did the configuration" — and if the emitter changed
 * deliberately, the golden is regenerated in the same commit so the change is
 * in the diff rather than hidden.
 *
 * A golden file also catches a value going *missing*, which is the failure mode
 * that matters: output that is one line shorter still parses.
 *
 *     ufconfig_serialise [--regenerate] [dir]
 *
 * With no arguments it compares and exits non-zero on any difference.
 * --regenerate writes the current output as the new golden — a deliberate act,
 * never something a failing test does on its own.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

static int g_pass, g_fail;

/* Read a whole file.  Returns NULL when it does not exist. */
static char *Slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}

static void WriteFile(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* Compare, and on difference say where.  A byte count alone sends the reader
   looking through two files for a needle. */
static int Compare(const char *name, const char *got, const char *want, size_t want_len) {
    size_t got_len = strlen(got);
    if (got_len == want_len && memcmp(got, want, want_len) == 0) {
        printf("  ok    %-16s %zu bytes\n", name, got_len);
        g_pass++;
        return 0;
    }

    g_fail++;
    printf("  FAIL  %-16s got %zu bytes, expected %zu\n", name, got_len, want_len);

    size_t lim = got_len < want_len ? got_len : want_len;
    for (size_t i = 0; i < lim; i++) {
        if (got[i] != want[i]) {
            size_t ls = i > 40 ? i - 40 : 0;
            /* show the line the divergence is on, not just the offset */
            size_t line = 1;
            for (size_t k = 0; k < i; k++) if (got[k] == '\n') line++;
            printf("        first difference at byte %zu (line %zu)\n", i, line);
            printf("        expected: ");
            for (size_t k = ls; k < i + 30 && k < want_len; k++) {
                char c = want[k];
                putchar(c == '\n' ? '$' : c);
                if (c == '\n') break;
            }
            printf("\n        got:      ");
            for (size_t k = ls; k < i + 30 && k < got_len; k++) {
                char c = got[k];
                putchar(c == '\n' ? '$' : c);
                if (c == '\n') break;
            }
            printf("\n");
            break;
        }
    }
    if (lim == want_len && got_len > want_len) printf("        got is longer; first extra byte at %zu\n", want_len);
    return 1;
}

int main(int argc, char **argv) {
    int regenerate = 0;
    const char *dir = ".";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--regenerate") == 0) regenerate = 1;
        else dir = argv[i];
    }

    char input[1024];
    snprintf(input, sizeof(input), "%s/input.lua", dir);

    UfConfigDescriptor d;
    memset(&d, 0, sizeof(d));
    d.fields      = g_ufconfig_fields;
    d.field_count = (size_t)g_ufconfig_field_count;
    d.lookup      = UfConfigLookupPath;
    d.kind        = UF_CONFIG_BACKEND_FILE;

    UfConfig *h = NULL;
    if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return 2;

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size    = sizeof(opt);
    opt.version = 1;
    opt.mode    = UF_CONFIG_LOAD_LENIENT;

    UfConfigStatus st = UfConfigLoadFile(h, input, &opt, NULL);
    if (st != UF_CONFIG_OK) {
        const UfConfigError *e = UfConfigLastError();
        fprintf(stderr, "cannot load %s — %s: %s\n", input,
                UfConfigStatusString(st), e->message);
        UfConfigDestroy(h);
        return 2;
    }

    printf("serialiser payload%s\n  input  %s\n\n",
           regenerate ? " — REGENERATING GOLDEN FILES" : "", input);

    struct {
        const char *name;
        const char *file;
        UfConfigStatus (*fn)(const UfConfig *h, char **out);
    } ser[] = {
        { "json", "expected.json", UfConfigToJsonAlloc },
        { "yaml", "expected.yaml", UfConfigToYamlAlloc },
        { "ini",  "expected.ini",  UfConfigToIniAlloc },
        { "lua",  "expected.lua",  UfConfigToLuaStyleAlloc },
    };

    for (size_t i = 0; i < sizeof(ser) / sizeof(ser[0]); i++) {
        char *out = NULL;
        UfConfigStatus gs = ser[i].fn(h, &out);
        if (gs != UF_CONFIG_OK || !out) {
            printf("  FAIL  %-16s %s\n", ser[i].name, UfConfigStatusString(gs));
            g_fail++;
            continue;
        }

        char golden[1024];
        snprintf(golden, sizeof(golden), "%s/%s", dir, ser[i].file);

        if (regenerate) {
            WriteFile(golden, out);
            printf("  wrote %-16s %zu bytes\n", ser[i].file, strlen(out));
            free(out);
            continue;
        }

        size_t want_len = 0;
        char *want = Slurp(golden, &want_len);
        if (!want) {
            printf("  FAIL  %-16s no golden file at %s\n", ser[i].name, golden);
            g_fail++;
            free(out);
            continue;
        }
        Compare(ser[i].name, out, want, want_len);
        free(want);
        free(out);
    }

    /* The payload question stated directly: every value the handle holds must
       appear somewhere in each output.  The goldens catch a change; this
       catches an omission even on a first run with no golden to compare. */
    printf("\n  payload present in every serialiser:\n");
    static const char *const needles[] = {
        "-42", "3.5", "true", "plain text", "alpha", "beta", "gamma",
        "11", "aliased", "7",
    };
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        int in_all = 1;
        for (size_t j = 0; j < sizeof(ser) / sizeof(ser[0]); j++) {
            char *out = NULL;
            if (ser[j].fn(h, &out) != UF_CONFIG_OK || !out) { in_all = 0; continue; }
            if (!strstr(out, needles[i])) in_all = 0;
            free(out);
        }
        printf("    %-14s %s\n", needles[i], in_all ? "present in json, yaml, ini and lua" : "*** MISSING SOMEWHERE ***");
        if (!in_all) g_fail++;
    }

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    UfConfigDestroy(h);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
