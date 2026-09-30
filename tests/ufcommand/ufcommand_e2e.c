#include <uflib/ufcommand/ufcommand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct HarnessState {
    unsigned calls;
    char last_args[2048];
} HarnessState;

static void die(const char *where, UfCommandStatus st) {
    fprintf(stderr, "fatal: %s: %s\n", where, UfCommandStatusName(st));
    exit(2);
}

static UfCommandStatus fixture_handler(UfCommandContext *ctx, const char *name,
                                        const UfCommandArg *args, size_t argc, void *ud) {
    (void)ctx;
    (void)name;
    HarnessState *s = ud;
    s->calls++;
    s->last_args[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < argc; ++i) {
        int n = snprintf(s->last_args + used, sizeof(s->last_args) - used,
                         "%s%s", i ? "|" : "", args[i].value ? args[i].value : "");
        if (n < 0 || (size_t)n >= sizeof(s->last_args) - used) return UFCOMMAND_BUFFER_TOO_SMALL;
        used += (size_t)n;
    }
    return UFCOMMAND_OK;
}

static UfCommandStatus error_handler(UfCommandContext *ctx, const char *name,
                                      const UfCommandArg *args, size_t argc, void *ud) {
    (void)ctx; (void)name; (void)args; (void)argc;
    HarnessState *s = ud;
    s->calls++;
    s->last_args[0] = '\0';
    return UFCOMMAND_HANDLER_ERROR;
}

static UfCommandStatus add_fixture_commands(UfCommandRegistry *r, HarnessState *s) {
    const UfCommandDefinition defs[] = {
        {"status",       "show status",       fixture_handler, s, 0, 0},
        {"checkout",     "switch branch",     fixture_handler, s, 1, 1},
        {"remote",       "remote root",       fixture_handler, s, 0, 1},
        {"remote add",  "add remote",        fixture_handler, s, 2, 2},
        {"remote get",  "get remote",        fixture_handler, s, 1, 1},
        {"echo",         "echo arguments",    fixture_handler, s, 0, 3},
        {"build",        "build",             fixture_handler, s, 0, 2},
        {"config get",   "get config",        fixture_handler, s, 0, 0},
        {"config set",   "set config",        fixture_handler, s, 2, 2},
        {"fail",         "error fixture",     error_handler,   s, 0, 0}
    };
    for (size_t i = 0; i < ARRAY_LEN(defs); ++i) {
        UfCommandStatus st = UfCommandRegistryAdd(r, &defs[i]);
        if (st != UFCOMMAND_OK) return st;
    }
    return UFCOMMAND_OK;
}

static UfCommandStatus add_fixture_aliases(UfCommandRegistry *r) {
    const UfCommandAliasDefinition defs[] = {
        {"st",       "status"},
        {"co",       "checkout $1"},
        {"ra",       "remote add $1 $2"},
        {"say",      "echo $*"},
        {"say1",     "echo $1"},
        {"cfg",      "config get"},
        {"cfgset",   "config set $1 $2"},
        {"failalias", "fail"},
        {"cycle-a",  "cycle-b"},
        {"cycle-b",  "cycle-a"}
    };
    for (size_t i = 0; i < ARRAY_LEN(defs); ++i) {
        UfCommandStatus st = UfCommandRegistryAddAlias(r, &defs[i]);
        if (st != UFCOMMAND_OK) return st;
    }
    return UFCOMMAND_OK;
}

static UfCommandStatus add_fixture_bindings(UfCommandRegistry *r) {
    const UfCommandBinding defs[] = {
        {UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT, "P", "status"},
        {UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT, "B", "build"},
        {UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT, "C", "co main"},
        {UFCOMMAND_MOD_ALT, "X", "checkout main"},
        {UFCOMMAND_MOD_SHIFT, "E", "say bound hello"}
    };
    for (size_t i = 0; i < ARRAY_LEN(defs); ++i) {
        UfCommandStatus st = UfCommandRegistryAddBinding(r, &defs[i]);
        if (st != UFCOMMAND_OK) return st;
    }
    return UFCOMMAND_OK;
}

static UfCommandStatus make_registry(const char *config_path, UfCommandRegistry **out,
                                     HarnessState *s) {
    UfCommandRegistry *r = NULL;
    UfCommandStatus st = UfCommandRegistryCreate(NULL, &r);
    if (st != UFCOMMAND_OK) return st;
    st = add_fixture_commands(r, s);
    if (st == UFCOMMAND_OK) st = add_fixture_aliases(r);
    if (st == UFCOMMAND_OK) st = add_fixture_bindings(r);
    if (st == UFCOMMAND_OK && config_path) st = UfCommandRegistryLoadUserConfig(r, config_path);
    if (st != UFCOMMAND_OK) {
        UfCommandRegistryDestroy(r);
        return st;
    }
    *out = r;
    return UFCOMMAND_OK;
}

static UfCommandStatus make_roundtrip_registry(const char *config_path,
                                               UfCommandRegistry **out,
                                               HarnessState *s) {
    UfCommandRegistry *r = NULL;
    UfCommandStatus st = UfCommandRegistryCreate(NULL, &r);
    if (st != UFCOMMAND_OK) return st;
    st = add_fixture_commands(r, s);
    if (st == UFCOMMAND_OK) st = UfCommandRegistryLoadUserConfig(r, config_path);
    if (st != UFCOMMAND_OK) {
        UfCommandRegistryDestroy(r);
        return st;
    }
    *out = r;
    return UFCOMMAND_OK;
}

static void reset_state(HarnessState *s) {
    s->last_args[0] = '\0';
}

static int parse_modifiers(const char *text, UfCommandModifier *out) {
    *out = UFCOMMAND_MOD_NONE;
    if (!*text || strcmp(text, "NONE") == 0) return 1;
    char buf[128];
    if (strlen(text) >= sizeof(buf)) return 0;
    strcpy(buf, text);
    char *save = NULL;
    for (char *p = strtok_r(buf, "+", &save); p; p = strtok_r(NULL, "+", &save)) {
        if (strcmp(p, "CTRL") == 0) *out |= UFCOMMAND_MOD_CTRL;
        else if (strcmp(p, "SHIFT") == 0) *out |= UFCOMMAND_MOD_SHIFT;
        else if (strcmp(p, "ALT") == 0) *out |= UFCOMMAND_MOD_ALT;
        else if (strcmp(p, "META") == 0) *out |= UFCOMMAND_MOD_META;
        else return 0;
    }
    return 1;
}

static void trim_eol(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static size_t split_tsv(char *s, char **fields, size_t max_fields) {
    size_t n = 0;
    char *start = s;
    for (char *p = s;; ++p) {
        if (*p == '\t' || *p == '\0') {
            if (n < max_fields) fields[n] = start;
            ++n;
            if (*p == '\0' || n == max_fields) break;
            *p = '\0';
            start = p + 1;
        }
    }
    return n;
}

static char *trim_left(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    return s;
}

static void emit_escaped(const char *s, FILE *out) {
    fputc('[', out);
    for (; *s; ++s) {
        if (*s == '\\') fputs("\\\\", out);
        else if (*s == ']') fputs("\\]", out);
        else fputc(*s, out);
    }
    fputc(']', out);
}

static void emit_actual(UfCommandStatus st, const UfCommandResult *res,
                        const HarnessState *s, unsigned call_delta, FILE *out) {
    fprintf(out, "status=%s|resolved=", UfCommandStatusName(st));
    emit_escaped(UfCommandResultGetResolvedCommand(res) ? UfCommandResultGetResolvedCommand(res) : "-", out);
    fprintf(out, "|argc=%zu|args=", UfCommandResultGetArgCount(res));
    emit_escaped(s->last_args, out);
    fprintf(out, "|handler_calls=%u", call_delta);
}

static int expected_matches(const char *actual, const char *expected) {
    return strcmp(actual, expected) == 0;
}

/* Duplicate-detection state, and a floor on how many cases the oracle must
 * contain: a truncated or replaced golden file must not read as a pass. */
#define GOLDEN_MAX_IDS 8192
#define GOLDEN_MIN_CASES 100
static char     seen_ids[GOLDEN_MAX_IDS][64];
static unsigned seen_id_line[GOLDEN_MAX_IDS];
static unsigned seen_id_count = 0;

/* The harness uses a deliberately simple golden format:
 *
 * id<TAB>scope<TAB>mode<TAB>input<TAB>expected
 *
 * scope: BASE | PERSIST | ROUNDTRIP
 * mode: EXEC | BIND | DEFAULT
 * BIND input: MODIFIERS:key, e.g. CTRL+SHIFT:P
 * expected: canonical status/resolution/args/handler-call record.
 */
static int run_golden(const char *path, UfCommandParser *p,
                      UfCommandRegistry *base, UfCommandRegistry *persist, UfCommandRegistry *roundtrip,
                      HarnessState *bs, HarnessState *ps, HarnessState *rs, int verbose) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "fatal: cannot open the oracle '%s': %s\n", path, strerror(errno));
        return 0;
    }
    char line[8192];
    unsigned line_no = 0, cases = 0, failures = 0;
    while (fgets(line, sizeof(line), f)) {
        ++line_no;
        trim_eol(line);
        char *t = trim_left(line);
        if (!*t || *t == '#') continue;
        char *fields[5] = {0};
        size_t nf = split_tsv(t, fields, ARRAY_LEN(fields));
        if (nf != 5) {
            fprintf(stderr, "golden line %u: expected 5 TSV fields, got %zu\n", line_no, nf);
            ++failures; continue;
        }
        ++cases;
        const char *id = fields[0];
        /* A row with a 5th field that is not a record would otherwise compare
         * against garbage and could only ever fail by accident. */
        if (strncmp(fields[4], "status=", 7) != 0) {
            fprintf(stderr, "line %u (%s): expected field is not an oracle record\n", line_no, id);
            ++failures;
            continue;
        }
        /* A duplicated case id silently inflates the corpus and hides the case it
         * displaced, so it is treated as an oracle defect rather than a pass. */
        int duplicate = 0;
        for (unsigned k = 0; k < seen_id_count; ++k) {
            if (strcmp(seen_ids[k], id) == 0) {
                fprintf(stderr, "line %u: duplicate case id '%s' (first at line %u)\n",
                        line_no, id, seen_id_line[k]);
                ++failures;
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        if (seen_id_count < GOLDEN_MAX_IDS) {
            snprintf(seen_ids[seen_id_count], sizeof(seen_ids[0]), "%s", id);
            seen_id_line[seen_id_count] = line_no;
            ++seen_id_count;
        }
        UfCommandRegistry *r = NULL;
        HarnessState *s = NULL;
        if (strcmp(fields[1], "BASE") == 0) { r = base; s = bs; }
        else if (strcmp(fields[1], "PERSIST") == 0) { r = persist; s = ps; }
        else if (strcmp(fields[1], "ROUNDTRIP") == 0) { r = roundtrip; s = rs; }
        else { fprintf(stderr, "line %u (%s): invalid scope %s\n", line_no, id, fields[1]); ++failures; continue; }
        reset_state(s);
        unsigned before = s->calls;
        UfCommandResult *res = NULL;
        UfCommandStatus st = UfCommandResultCreate(&res);
        if (st != UFCOMMAND_OK) { die("UfCommandResultCreate", st); }
        if (strcmp(fields[2], "EXEC") == 0) {
            st = UfCommandParserExecute(p, r, NULL, fields[3], res);
        } else if (strcmp(fields[2], "BIND") == 0) {
            char binding_input[1024];
            if (strlen(fields[3]) >= sizeof(binding_input)) {
                fprintf(stderr, "line %u (%s): binding input too long\n", line_no, id);
                ++failures; UfCommandResultDestroy(res); continue;
            }
            strcpy(binding_input, fields[3]);
            char *colon = strchr(binding_input, ':');
            if (!colon) { fprintf(stderr, "line %u (%s): invalid binding input\n", line_no, id); ++failures; UfCommandResultDestroy(res); continue; }
            *colon = '\0';
            UfCommandModifier m;
            if (!parse_modifiers(binding_input, &m)) { fprintf(stderr, "line %u (%s): invalid modifiers\n", line_no, id); ++failures; UfCommandResultDestroy(res); continue; }
            st = UfCommandParserExecuteBinding(p, r, NULL, m, colon + 1, res);
        } else if (strcmp(fields[2], "DEFAULT") == 0) {
            st = UfCommandParserExecuteDefaultBinding(p, r, NULL, fields[3], res);
        } else {
            fprintf(stderr, "line %u (%s): invalid mode %s\n", line_no, id, fields[2]);
            ++failures; UfCommandResultDestroy(res); continue;
        }
        unsigned delta = s->calls - before;
        char actual[8192];
        FILE *mem = fmemopen(actual, sizeof(actual) - 1, "w");
        if (!mem) { perror("fmemopen"); exit(2); }
        emit_actual(st, res, s, delta, mem);
        long written = ftell(mem);
        int closed = fclose(mem);
        /* fmemopen() discards writes past the end and does not report it, so a
         * record at the buffer boundary would be compared in truncated form. */
        if (closed != 0 || written < 0 || (size_t)written >= sizeof(actual) - 1) {
            fprintf(stderr, "line %u (%s): result record does not fit the harness buffer (%ld bytes)\n",
                    line_no, id, written);
            ++failures;
            UfCommandResultDestroy(res);
            continue;
        }
        if (!expected_matches(actual, fields[4])) {
            fprintf(stderr, "FAIL %s (line %u)\n  input:    %s\n  expected: %s\n  actual:   %s\n",
                    id, line_no, fields[3], fields[4], actual);
            ++failures;
        } else if (verbose) {
            printf("PASS %s\n", id);
        }
        UfCommandResultDestroy(res);
    }
    fclose(f);
    if (cases < GOLDEN_MIN_CASES) {
        fprintf(stderr, "oracle '%s' holds only %u case(s); at least %d are required, "
                        "so this file is truncated or is not the oracle\n",
                path, cases, GOLDEN_MIN_CASES);
        ++failures;
    }
    printf("E2E golden: %u cases, %u failures\n", cases, failures);
    return failures == 0;
}

int main(int argc, char **argv) {
    const char *golden = argc > 1 ? argv[1] : "tests/golden_e2e.tsv";
    const char *config = argc > 2 ? argv[2] : "tests/golden_e2e_config.lua";
    int verbose = argc > 3 && strcmp(argv[3], "--verbose") == 0;

    UfCommandParser *parser = NULL;
    UfCommandStatus st = UfCommandParserCreate(NULL, &parser);
    if (st != UFCOMMAND_OK) die("UfCommandParserCreate", st);

    HarnessState base_state = {0};
    HarnessState persist_state = {0};
    HarnessState roundtrip_state = {0};
    UfCommandRegistry *base = NULL;
    UfCommandRegistry *persist = NULL;
    UfCommandRegistry *roundtrip = NULL;
    st = make_registry(NULL, &base, &base_state);
    if (st != UFCOMMAND_OK) die("make base registry", st);
    st = make_registry(config, &persist, &persist_state);
    if (st != UFCOMMAND_OK) die("make persistent registry", st);

    const char *roundtrip_path = "ufcommand_e2e_roundtrip.lua";
    st = UfCommandRegistrySaveUserConfig(persist, roundtrip_path);
    if (st != UFCOMMAND_OK) die("save roundtrip registry", st);
    st = make_roundtrip_registry(roundtrip_path, &roundtrip, &roundtrip_state);
    remove(roundtrip_path);
    if (st != UFCOMMAND_OK) die("make roundtrip registry", st);

    int ok = run_golden(golden, parser, base, persist, roundtrip,
                        &base_state, &persist_state, &roundtrip_state, verbose);

    UfCommandRegistryDestroy(base);
    UfCommandRegistryDestroy(persist);
    UfCommandRegistryDestroy(roundtrip);
    UfCommandParserDestroy(parser);
    return ok ? 0 : 1;
}
