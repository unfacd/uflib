// alias_map allocation-failure injection — standalone, not registered with CTest.
//
// For each mutating operation, fail the 1st, 2nd, 3rd ... malloc/calloc it
// performs (one run per failure point).  Every run must either
//   * report failure and leave the map byte-for-byte unchanged, or
//   * succeed and produce exactly the state a failure-free run produces
//     (the only tolerated failure is the best-effort table grow).
//
// Built with -Wl,--wrap=malloc,--wrap=calloc; run it under ASan so leaks and
// double frees on the rollback paths are caught.

#include <uflib/cdt/alias_map/alias_map.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);

static int g_fail_at;       // 0 = disarmed; otherwise fail the k-th allocation
static int g_calls;

static int
sShouldFail(void)
{
    return g_fail_at && ++g_calls == g_fail_at;
}

void *
__wrap_malloc(size_t n)
{
    return sShouldFail() ? NULL : __real_malloc(n);
}

void *
__wrap_calloc(size_t a, size_t b)
{
    return sShouldFail() ? NULL : __real_calloc(a, b);
}

void *
__wrap_realloc(void *p, size_t n)
{
    return sShouldFail() ? NULL : __real_realloc(p, n);
}

/* ── Fixture and state snapshot ────────────────────────────────────────── */

// Sits exactly at the grow threshold (12 aliases in 16 buckets), so any
// insertion of >= 1 alias triggers a rehash attempt.
static AliasMap *
sFixture(void)
{
    AliasMap   *map_ptr = AliasMapCreate();
    const char *g1[] = { "a", "b", "c" };
    const char *g2[] = { "d", "e" };
    const char *g3[] = { "f" };

    assert(map_ptr);
    assert(AliasMapAdd(map_ptr, g1, 3, "v1"));
    assert(AliasMapAdd(map_ptr, g2, 2, "v2"));
    assert(AliasMapAdd(map_ptr, g3, 1, "v3"));

    for (int i = 0; i < 6; i++) {
        char        key[16];
        const char *group[1];

        snprintf(key, sizeof key, "x%d", i);
        group[0] = key;
        assert(AliasMapAdd(map_ptr, group, 1, "xv"));
    }

    return map_ptr;
}

static int
sCmpStr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static const char *const KEYS[] = { "a", "b", "c", "d", "e", "f", "n1", "n2", "p", "q",
                                    "x0", "x1", "x2", "x3", "x4", "x5" };

// "key=value{sorted group}" for every key of interest, plus the total count.
static char *
sSnapshot(AliasMap *map_ptr)
{
    char *buf = malloc(8192), *w = buf;

    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
        char  *value = AliasMapGetCopy(map_ptr, KEYS[i]);
        size_t count;
        char **list;

        w += sprintf(w, "%s=%s{", KEYS[i], value ? value : "-");
        free(value);

        list = AliasMapAliasesOf(map_ptr, KEYS[i], &count);

        if (list) {
            qsort(list, count, sizeof *list, sCmpStr);

            for (size_t j = 0; j < count; j++) w += sprintf(w, "%s,", list[j]);

            free(list);
        }

        w += sprintf(w, "} ");
    }

    sprintf(w, "count=%zu", AliasMapAliasCount(map_ptr));

    return buf;
}

/* ── Operations under test ─────────────────────────────────────────────── */

static bool
sOpReplace(AliasMap *map_ptr)
{
    const char *aliases[] = { "c", "e", "f", "n1", "n2" }; /* 3 steals, 2 fresh */
    return AliasMapReplace(map_ptr, aliases, 5, "nv");
}

static bool
sOpReplaceFreshOnly(AliasMap *map_ptr)
{
    const char *aliases[] = { "n1", "n2", "p" };
    return AliasMapReplace(map_ptr, aliases, 3, "nv");
}

static bool
sOpReplaceStealOnly(AliasMap *map_ptr)
{
    const char *aliases[] = { "a", "d", "f" };
    return AliasMapReplace(map_ptr, aliases, 3, "nv");
}

static bool
sOpAdd(AliasMap *map_ptr)
{
    const char *aliases[] = { "p", "q" };
    return AliasMapAdd(map_ptr, aliases, 2, "pv");
}

static bool
sOpAddAlias(AliasMap *map_ptr)
{
    return AliasMapAddAlias(map_ptr, "a", "p");
}

static bool
sOpSetValue(AliasMap *map_ptr)
{
    return AliasMapSetValue(map_ptr, "a", "zz");
}

static bool
sOpAliasesOf(AliasMap *map_ptr)
{
    size_t count;
    char **list = AliasMapAliasesOf(map_ptr, "a", &count);
    bool   ok   = list != NULL;
    free(list);
    return ok;
}

static size_t g_visits;

static bool
sVisit(void *context_ptr, const char *value_ptr, const char *alias_ptr)
{
    (void)context_ptr;
    (void)value_ptr;
    (void)alias_ptr;

    g_visits++;

    return true;
}

static bool
sOpForEach(AliasMap *map_ptr)
{
    g_visits = 0;

    bool ok = AliasMapForEach(map_ptr, sVisit, NULL);

    return ok && g_visits == 12;
}

static bool
sOpSnapshot(AliasMap *map_ptr)
{
    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    if (!snapshot_ptr) return false;

    assert(AliasMapSnapshotGroupCount(snapshot_ptr) == 9);
    assert(AliasMapSnapshotAliasCount(snapshot_ptr) == 12);

    bool ok = AliasMapSnapshotValueAt(snapshot_ptr, 0) != NULL &&
              AliasMapSnapshotAliasAt(snapshot_ptr, 0, 0) != NULL;

    AliasMapSnapshotDestroy(snapshot_ptr);

    return ok;
}

static bool
sOpGetInto(AliasMap *map_ptr)
{
    char   buffer[64];
    size_t len;
    return AliasMapGetInto(map_ptr, "a", buffer, sizeof buffer, &len);
}

typedef bool (*OpFn)(AliasMap *);

static void
sRun(const char *name, OpFn op)
{
    AliasMap *reference_ptr = sFixture();
    char     *before        = sSnapshot(reference_ptr);

    assert(op(reference_ptr));

    char *want = sSnapshot(reference_ptr);
    AliasMapDestroy(reference_ptr);

    int k, failures = 0;

    for (k = 1; k < 200; k++) {
        AliasMap *map_ptr = sFixture();
        char     *state;

        g_calls    = 0;
        g_fail_at  = k;

        bool ok    = op(map_ptr);
        int  calls = g_calls;

        g_fail_at = 0;

        state = sSnapshot(map_ptr);

        if (ok) {
            assert(strcmp(state, want) == 0);
        }
        else {
            assert(strcmp(state, before) == 0);
            failures++;
        }

        free(state);
        AliasMapDestroy(map_ptr);

        if (calls < k) break;   // the k-th allocation never happened
    }

    assert(k < 200);

    printf("%-22s %2d allocation points, %2d clean failures: ok\n",
           name, k - 1, failures);

    free(before);
    free(want);
}

/*! Descriptor construction must fail cleanly at every allocation point. */
static void
sRunCreate(void)
{
    const char *g1[] = { "a", "b" };
    const char *g2[] = { "c" };

    const AliasMapCollector collectors[] = {
        { "v1", g1, 2 },
        { "v2", g2, 1 },
    };

    int k, failures = 0;

    for (k = 1; k < 200; k++) {
        AliasMapDescriptor descriptor = {0};

        descriptor.collectors_ptr  = collectors;
        descriptor.collector_count = 2;

        g_calls   = 0;
        g_fail_at = k;

        AliasMap *map_ptr = AliasMapCreateWithDescriptor(&descriptor);
        int       calls   = g_calls;

        g_fail_at = 0;

        if (map_ptr) {
            assert(AliasMapAliasCount(map_ptr) == 3);
            AliasMapDestroy(map_ptr);
        }
        else {
            assert(descriptor.result.status == ALIASMAP_CREATE_NO_MEMORY);
            failures++;
        }

        if (calls < k) break;
    }

    assert(k < 200);
    printf("%-22s %2d allocation points, %2d clean failures: ok\n", "create", k - 1, failures);
}

/*! JSON construction must fail cleanly at every allocation point. */
static void
sRunCreateJson(void)
{
    static const char json[] =
        "[{\"value\": \"/p/one\", \"keys\": [\"one\", \"uno\"]},"
        " {\"value\": \"/p/two\", \"keys\": [\"two\"]}]";

    int k, failures = 0;

    for (k = 1; k < 200; k++) {
        AliasMapJsonDescriptor descriptor = {0};

        descriptor.json_ptr = json;

        g_calls   = 0;
        g_fail_at = k;

        AliasMap *map_ptr = AliasMapCreateWithJsonDescriptor(&descriptor);
        int       calls   = g_calls;

        g_fail_at = 0;

        if (map_ptr) {
            assert(AliasMapAliasCount(map_ptr) == 3);
            AliasMapDestroy(map_ptr);
        }
        else {
            assert(descriptor.result.status == ALIASMAP_CREATE_NO_MEMORY);
            failures++;
        }

        if (calls < k) break;
    }

    assert(k < 200);
    printf("%-22s %2d allocation points, %2d clean failures: ok\n",
           "create json", k - 1, failures);
}

int
main(void)
{
    sRun("replace (mixed)",      sOpReplace);
    sRun("replace (fresh only)", sOpReplaceFreshOnly);
    sRun("replace (steal only)", sOpReplaceStealOnly);
    sRun("add",                  sOpAdd);
    sRun("add_alias",            sOpAddAlias);
    sRun("set_value",            sOpSetValue);
    sRun("aliases_of",           sOpAliasesOf);
    sRun("snapshot",             sOpSnapshot);
    sRun("for_each",             sOpForEach);
    sRun("get_into",             sOpGetInto);
    sRunCreate();
    sRunCreateJson();

    return 0;
}
