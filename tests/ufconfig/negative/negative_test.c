/**
 * @file negative_test.c
 * @brief Every schema rule, positively confirmed to reject.
 *
 * A document that loads proves the loader accepts valid input.  It says nothing
 * about whether it refuses invalid input — and a configuration loader that
 * accepts everything is worse than one that refuses too much, because the
 * failure surfaces later as a server reading a value nobody validated.
 *
 * Each case below breaks exactly one rule, on exactly one field, and asserts
 * the *specific* status that rule produces.  An assertion of the form
 * "VALIDATION or REQUIRED" would pass whichever fired and so would prove
 * neither; every case here names one.
 *
 * Documents are built in memory rather than shipped as files: a fixture per
 * rule would be thirty files differing by a word, and the point of each is the
 * one word.
 */

#include <uflib/ufconfig/ufconfig.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

static int g_pass, g_fail;

/* A document that loads cleanly.  Every case starts from this and breaks one
   thing, so a failure cannot come from something else being wrong. */
static const char *const kBase =
    "n = {\n"
    "  must      = \"present\",\n"
    "  typed_int = 3,\n"
    "  typed_bool = true,\n"
    "  ranged    = 5,\n"
    "  sized     = \"abc\",\n"
    "  enum      = \"B\",\n"
    "  pattern   = \"abc\",\n"
    "  ip4       = \"10.0.0.1\",\n"
    "  ip6       = \"::1\",\n"
    "  fqdn      = \"a.b.c\",\n"
    "  date      = \"2024-01-01\",\n"
    "  cidr      = \"10.0.0.0/8\",\n"
    "  size      = \"1MiB\",\n"
    "  list      = { \"a\", \"b\" },\n"
    "}\n"
    "return { n = n }\n";

/* Load `document` and require exactly `want`. */
static void Expect(const char *what, const char *document,
                   UfConfigStatus want, UfConfigLoadMode mode) {
    UfConfig *h = NULL;
    UfConfigCreate(&h, &(UfConfigDescriptor){ .fields = g_ufconfig_fields,
                                              .field_count = (size_t)g_ufconfig_field_count,
                                              .lookup = UfConfigLookupPath,
                                              .kind = UF_CONFIG_BACKEND_FILE });

    UfConfigLoadOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = mode;

    UfConfigStatus got = UfConfigLoadBuffer(h, document, strlen(document), &opt, NULL);
    const UfConfigError *e = UfConfigLastError();

    if (got == want) {
        g_pass++;
        printf("  ok    %-34s %-22s  %s\n", what, UfConfigStatusString(got),
               (e && e->field_path) ? e->field_path : "");
    } else {
        g_fail++;
        printf("  FAIL  %-34s expected %-14s got %-14s %s\n", what,
               UfConfigStatusString(want), UfConfigStatusString(got),
               (e && e->message[0]) ? e->message : "");
    }
    UfConfigDestroy(h);
}

/* The same document with `n.<field> = <value>` substituted in place of the
   line that set it.  Keeps every other field valid. */
static const char *Subst(const char *field, const char *value) {
    static char buf[4096];
    char from[128], to[128];
    snprintf(from, sizeof(from), "%s", field);

    /* Find the line whose KEY is exactly this field.
     *
     * A plain strstr does not do that.  "size" occurs inside "sized", and
     * "sized" is the earlier line, so the case that meant to test a unitless
     * file size was silently rewriting the sized line instead -- and passing a
     * valid document to an assertion that it be refused.  The key has to start
     * the line after indentation and be followed by optional space and '='. */
    size_t      flen = strlen(from);
    const char *at   = NULL;
    for (const char *p = kBase; (p = strstr(p, from)) != NULL; p += flen) {
        const char *ls = p;
        while (ls > kBase && (ls[-1] == ' ' || ls[-1] == '\t')) ls--;
        if (ls != kBase && ls[-1] != '\n') continue;
        const char *q = p + flen;
        while (*q == ' ') q++;
        if (*q != '=') continue;
        at = p;
        break;
    }
    if (!at) return kBase;
    const char *line_start = at;
    while (line_start > kBase && line_start[-1] != '\n') line_start--;
    const char *line_end = strchr(at, '\n');
    if (!line_end) return kBase;

    snprintf(to, sizeof(to), "%s", field);
    size_t pre = (size_t)(line_start - kBase);
    size_t post_off = (size_t)(line_end - kBase) + 1;
    snprintf(buf, sizeof(buf), "%.*s  %s = %s,\n%s",
             (int)pre, kBase, to, value, kBase + post_off);
    return buf;
}

int main(void) {
    printf("ufconfig schema rejection\n\n");
    printf("  %-6s %-34s %-22s %s\n", "", "case", "status", "field");

    /* ── the baseline ─────────────────────────────────────────────────────── */
    {
        UfConfig *h = NULL;
        UfConfigCreate(&h, &(UfConfigDescriptor){ .fields = g_ufconfig_fields,
                                                  .field_count = (size_t)g_ufconfig_field_count,
                                                  .lookup = UfConfigLookupPath });
        UfConfigLoadOptions opt = { .size = sizeof(opt), .version = 1,
                                    .mode = UF_CONFIG_LOAD_LENIENT };
        UfConfigStatus st = UfConfigLoadBuffer(h, kBase, strlen(kBase), &opt, NULL);
        printf("  %-6s %-34s %s\n", st == UF_CONFIG_OK ? "ok" : "FAIL",
               "baseline document loads", UfConfigStatusString(st));
        if (st != UF_CONFIG_OK) {
            fprintf(stderr, "  the baseline itself is broken — every case below is meaningless: %s\n",
                    UfConfigLastError()->message);
            UfConfigDestroy(h);
            return EXIT_FAILURE;
        }
        g_pass++;
        UfConfigDestroy(h);
    }

    printf("\nSTRUCTURE\n");
    Expect("required field absent", "n = { }\nreturn { n = n }\n",
           UF_CONFIG_ERR_REQUIRED, UF_CONFIG_LOAD_STRICT);
    Expect("undeclared field, strict", "n = { must = \"x\", nope = 1 }\nreturn { n = n }\n",
           UF_CONFIG_ERR_UNKNOWN_FIELD, UF_CONFIG_LOAD_STRICT);
    Expect("undeclared field, lenient", "n = { must = \"x\", nope = 1 }\nreturn { n = n }\n",
           UF_CONFIG_OK, UF_CONFIG_LOAD_LENIENT);

    printf("\nTYPE\n");
    /* the same three cases under strict, to separate "lenient tolerates it"
       from "nothing catches it at all" */
    Expect("string on integer, strict",
           Subst("typed_int", "\"not a number\""), UF_CONFIG_ERR_TYPE_MISMATCH, UF_CONFIG_LOAD_STRICT);
    Expect("integer on boolean, strict",
           Subst("typed_bool", "1"), UF_CONFIG_ERR_TYPE_MISMATCH, UF_CONFIG_LOAD_STRICT);
    Expect("array over max, strict",
           Subst("list", "{ \"a\", \"b\", \"c\", \"d\" }"),
           UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_STRICT);
    Expect("string where integer declared",
           Subst("typed_int", "\"not a number\""), UF_CONFIG_ERR_TYPE_MISMATCH, UF_CONFIG_LOAD_LENIENT);
    Expect("integer where boolean declared",
           Subst("typed_bool", "1"), UF_CONFIG_ERR_TYPE_MISMATCH, UF_CONFIG_LOAD_LENIENT);

    printf("\nRANGE\n");
    Expect("below min", Subst("ranged", "0"), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("above max", Subst("ranged", "11"), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("at min, accepted", Subst("ranged", "1"), UF_CONFIG_OK, UF_CONFIG_LOAD_LENIENT);
    Expect("at max, accepted", Subst("ranged", "10"), UF_CONFIG_OK, UF_CONFIG_LOAD_LENIENT);

    printf("\nLENGTH AND ARRAY\n");
    Expect("string over max length",
           Subst("sized", "\"abcdefgh\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("empty string below min length",
           Subst("sized", "\"\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("array over array_max",
           Subst("list", "{ \"a\", \"b\", \"c\", \"d\" }"),
           UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);

    printf("\nENUMERATION AND PATTERN\n");
    Expect("value outside one_of",
           Subst("enum", "\"Z\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("value failing regex",
           Subst("pattern", "\"ABC\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);

    printf("\nFORMAT VALIDATORS\n");
    Expect("ip4 octet out of range",
           Subst("ip4", "\"999.1.1.1\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("ip4 with leading zero",
           Subst("ip4", "\"1.2.3.04\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("ip6 with two compactions",
           Subst("ip6", "\"1::2::3\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("fqdn with underscore",
           Subst("fqdn", "\"bad_host\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("impossible date",
           Subst("date", "\"2025-02-30\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("cidr without a prefix length",
           Subst("cidr", "\"10.0.0.0\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("cidr with an out-of-range length",
           Subst("cidr", "\"10.0.0.0/33\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);
    Expect("file size with no unit",
           Subst("size", "\"10\""), UF_CONFIG_ERR_VALIDATION, UF_CONFIG_LOAD_LENIENT);

    printf("\nSYNTAX, which must not reach the schema at all\n");
    Expect("evaluated expression", "n = { must = true and false }\n",
           UF_CONFIG_ERR_PARSE, UF_CONFIG_LOAD_LENIENT);
    Expect("function call", "n = { must = os.time() }\n",
           UF_CONFIG_ERR_PARSE, UF_CONFIG_LOAD_LENIENT);
    Expect("unterminated string", "n = { must = \"oops }\n",
           UF_CONFIG_ERR_PARSE, UF_CONFIG_LOAD_LENIENT);
    Expect("unresolved alias", "n = { must = missing_table }\n",
           UF_CONFIG_ERR_ALIAS_UNRESOLVED, UF_CONFIG_LOAD_LENIENT);

    printf("\nRESULT  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
