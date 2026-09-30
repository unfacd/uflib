/**
 * @file ufconfig_bench.c
 * @brief Lookup and load cost, measured at the module's choke points.
 *
 * Not registered with CTest: it takes arguments and runs for a measured
 * duration, so it is a tool rather than a gate.  Run it directly:
 *
 *     ufconfig_bench --sample <sample.strict.lua>            # print a table
 *     ufconfig_bench --sample <f> --record baseline.tsv     # record a baseline
 *     ufconfig_bench --sample <f> --baseline baseline.tsv   # compare
 *
 * ## Why this exists
 *
 * TD-008 records lookup costs in the design document — 34 ns declared, 317 ns
 * at 64 siblings, 15 463 ns at 4096 — and nothing in the tree produces them.
 * They cannot be regenerated, so no change can be measured against them, and a
 * regression would be indistinguishable from a slower machine.  This is the
 * missing instrument.
 *
 * ## What makes the numbers usable
 *
 * - **Deterministic workloads.** Documents are generated from parameters
 *   (width, depth), so the same shapes are measured on every run.  A benchmark
 *   that measures a differently-shaped document each time measures nothing.
 * - **Duration-calibrated iterations.** Each benchmark doubles its iteration
 *   count until it runs for at least MIN_CALIBRATION_NS.  A measurement shorter
 *   than the clock's resolution is noise with a decimal point.
 * - **Repetition and spread.** Each benchmark runs REPEATS times and the median
 *   is reported, with the spread printed alongside.  Without the spread there
 *   is no way to tell an effect from a fluctuation.
 * - **The comparison respects the noise.** A difference is only reported as a
 *   regression or an improvement when it exceeds the larger of the two runs'
 *   spreads and a floor.  A tool that reports noise as regression gets ignored,
 *   and then it may as well not exist.
 * - **Scaling, not just absolutes.** The width curves matter more than any
 *   single number: the slope answers "is this O(w) or O(1)?", and the slope is
 *   what travels between machines.
 *
 * ## Build
 *
 * Optimised, sanitizers OFF.  ASan changes timing enough that no number from an
 * instrumented build is comparable with any number from an uninstrumented one.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <uflib/ufconfig/ufconfig.h>

#include "ufconfig_priv.h" /* ConfigFindPath and the canonicaliser, measured directly */

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPEATS             15
#define MIN_CALIBRATION_NS  50000000LL /* 50 ms per measurement */
#define MAX_ITERATIONS      200000000LL
#define SPREAD_NOISE_FLOOR 1.5        /* percent: below this a difference is not a finding */

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

/* ── timing ─────────────────────────────────────────────────────────────── */

static int64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static volatile uint64_t g_sink; /* consumes results so nothing is optimised away */

typedef void (*BenchBody)(void *ctx, long iters);

static int cmp_i64(const void *a, const void *b)
{
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return (x > y) - (x < y);
}

/* ── results ────────────────────────────────────────────────────────────── */

#define MAX_RESULTS 64
typedef struct
{
  const char *group;
  char        name[96];
  long        iters;
  double      median_ns; /* per operation, BEST of REPEATS — see bench() */
  double      spread_pct;
  int         unstable;
} Result;

static Result g_res[MAX_RESULTS];
static int    g_nres;

/*!
 * @brief Runs @p body until it is long enough to time, then REPEATS times more.
 *
 * The calibration pass picks an iteration count; the timed passes report the
 * median so a single scheduling hiccup cannot move the headline number.
 */
static void bench(const char *group, const char *name, BenchBody body, void *ctx)
{
  if (g_nres >= MAX_RESULTS) return;

  long iters = 1;
  while (iters < MAX_ITERATIONS) {
    int64_t t0 = now_ns();
    body(ctx, iters);
    if (now_ns() - t0 >= MIN_CALIBRATION_NS) break;
    iters *= 2;
  }

  int64_t samples[REPEATS];
  for (int r = 0; r < REPEATS; r++) {
    int64_t t0 = now_ns();
    body(ctx, iters);
    samples[r] = now_ns() - t0;
  }
  qsort(samples, REPEATS, sizeof(samples[0]), cmp_i64);

  /* The headline figure is the FASTEST of the repetitions, not the median.
     Everything that perturbs a measurement here — another process, an interrupt,
     a cache evicted by the allocator handing out a different arena address —
     can only ever make a run slower, never faster.  So the minimum is the
     closest estimate of the cost of the work itself, and it is far more stable
     between runs than the middle of a contaminated distribution.  The median is
     still reported as the spread, because how much a run is disturbed is
     itself worth seeing. */
  double best = (double)samples[0] / (double)iters;
  double med  = (double)samples[REPEATS / 2] / (double)iters;
  double hi   = (double)samples[REPEATS - 1] / (double)iters;
  (void)med;

  Result *r  = &g_res[g_nres++];
  r->group   = group;
  snprintf(r->name, sizeof(r->name), "%s", name);
  r->iters   = iters;
  r->median_ns  = best;
  r->spread_pct = best > 0 ? 100.0 * (hi - best) / best : 0.0;
  r->unstable   = r->spread_pct > 10.0;
}

/* ── workloads ──────────────────────────────────────────────────────────── */

#define MAX_DOC (8u * 1024u * 1024u)
static char g_doc[MAX_DOC];

/*!
 * @brief Builds the sample document plus a scope of @p width undeclared keys.
 *
 * The wide scope is undeclared deliberately.  A declared path is resolved
 * through the injected perfect hash and never walks, so a width curve over
 * declared fields would measure the hash and nothing else; this measures the
 * walk, which is what TD-008 is about.
 *
 * It is spliced into the sample rather than written from scratch because the
 * schema has required fields: a synthetic document is refused before any
 * lookup happens, and the benchmark would measure nothing.
 *
 * @param array_elems When non-zero, also nests an array of that many elements.
 */
static size_t build_scoped_doc(const char *sample_path, char *out, size_t cap, int width, int array_elems)
{
  FILE *f = fopen(sample_path, "rb");
  if (!f) return 0;
  size_t n = fread(out, 1, cap - 4096, f);
  fclose(f);
  out[n] = '\0';

  /* Splice before the final top-level return, and add the binding to it: a
     document that names its root in a return block drops anything it does not
     list. */
  char *ret = NULL;
  for (char *q = out; (q = strstr(q, "\nreturn {")) != NULL; q++) ret = q;
  if (!ret) return 0;

  size_t head = (size_t)(ret - out) + 1; /* keep the newline */

  char scope[MAX_DOC / 2];
  size_t o = (size_t)snprintf(scope, sizeof(scope), "big = {\n");
  for (int i = 0; i < width; i++) o += (size_t)snprintf(scope + o, sizeof(scope) - o, "  k%05d = %d,\n", i, i);
  if (array_elems > 0) {
    o += (size_t)snprintf(scope + o, sizeof(scope) - o, "  arr = {");
    for (int i = 0; i < array_elems; i++) o += (size_t)snprintf(scope + o, sizeof(scope) - o, "%d,", i);
    o += (size_t)snprintf(scope + o, sizeof(scope) - o, "},\n");
  }
  o += (size_t)snprintf(scope + o, sizeof(scope) - o, "}\n");

  /* Assembled into a scratch buffer, not in place: snprintf whose source and
     destination are the same buffer is undefined, and clobbers as it goes. */
  char *r2 = strstr(ret, "return {");
  if (!r2) return 0;
  size_t before = (size_t)(r2 - ret) + strlen("return {");

  static char tmp[MAX_DOC];
  size_t w = 0;
  w += (size_t)snprintf(tmp + w, sizeof(tmp) - w, "%.*s", (int)head, out);
  w += (size_t)snprintf(tmp + w, sizeof(tmp) - w, "%s", scope);
  w += (size_t)snprintf(tmp + w, sizeof(tmp) - w, "%.*s", (int)before, ret);
  w += (size_t)snprintf(tmp + w, sizeof(tmp) - w, " big = big,%s", r2 + strlen("return {"));

  memcpy(out, tmp, w);
  out[w] = '\0';
  return w;
}

/*!
 * @brief Loads @p doc with the real injected schema.
 *
 * The schema has to be the real one: the constructor refuses a descriptor with
 * no fields, and a document that does not satisfy the required fields is
 * rejected before any lookup happens.
 */
static UfConfig *open_doc(const char *doc, size_t len)
{
  UfConfigDescriptor d;
  memset(&d, 0, sizeof(d));
  d.fields      = g_ufconfig_fields;
  d.field_count = (size_t)g_ufconfig_field_count;
  d.lookup      = UfConfigLookupPath;
  d.kind        = UF_CONFIG_BACKEND_FILE;
  UfConfig *h   = NULL;
  if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return NULL;
  UfConfigLoadOptions o;
  memset(&o, 0, sizeof(o));
  o.size    = sizeof(o);
  o.version = 1;
  o.mode    = UF_CONFIG_LOAD_LENIENT;
  UfConfigStatus st = UfConfigLoadBuffer(h, doc, len, &o, NULL);
  if (st != UF_CONFIG_OK) {
    const UfConfigError *e = UfConfigLastError();
    fprintf(stderr, "open_doc(%zu bytes): %s: %s (at %s line %d)\n", len, UfConfigStatusString(st),
            e ? e->message : "?", (e && e->field_path) ? e->field_path : "-", e ? e->line : 0);
    UfConfigDestroy(h);
    return NULL;
  }
  return h;
}

static UfConfig *open_schema(const char *file)
{
  UfConfigDescriptor d;
  memset(&d, 0, sizeof(d));
  d.fields      = g_ufconfig_fields;
  d.field_count = (size_t)g_ufconfig_field_count;
  d.lookup      = UfConfigLookupPath;
  d.kind        = UF_CONFIG_BACKEND_FILE;
  UfConfig *h   = NULL;
  if (UfConfigCreate(&h, &d) != UF_CONFIG_OK) return NULL;
  UfConfigLoadOptions o;
  memset(&o, 0, sizeof(o));
  o.size    = sizeof(o);
  o.version = 1;
  o.mode    = UF_CONFIG_LOAD_LENIENT;
  if (UfConfigLoadFile(h, file, &o, NULL) != UF_CONFIG_OK) {
    UfConfigDestroy(h);
    return NULL;
  }
  return h;
}

/* ── benchmark bodies ───────────────────────────────────────────────────── */

typedef struct
{
  UfConfig *h;
  char      path[128];
  int64_t   ival;
} Ctx;

static void b_get(void *v, long n)
{
  Ctx           *c = v;
  UfConfigValue  val;
  uint64_t       acc = 0;
  for (long i = 0; i < n; i++) {
    memset(&val, 0, sizeof(val));
    if (UfConfigGetField(c->h, c->path, &val) == UF_CONFIG_OK) acc += (uint64_t)val.present;
  }
  g_sink += acc;
}

static void b_get_index(void *v, long n)
{
  Ctx          *c = v;
  UfConfigValue val;
  uint64_t      acc = 0;
  for (long i = 0; i < n; i++) {
    memset(&val, 0, sizeof(val));
    if (UfConfigGetFieldByIndex(c->h, (int)c->ival, &val) == UF_CONFIG_OK) acc += (uint64_t)val.present;
  }
  g_sink += acc;
}

static void b_set_int(void *v, long n)
{
  Ctx      *c = v;
  uint64_t  acc = 0;
  for (long i = 0; i < n; i++) acc += (UfConfigSetInt(c->h, c->path, i) == UF_CONFIG_OK);
  g_sink += acc;
}

static void b_unset_append(void *v, long n)
{
  Ctx      *c = v;
  uint64_t  acc = 0;
  for (long i = 0; i < n; i++) {
    acc += (UfConfigArrayAppendString(c->h, c->path, "x") == UF_CONFIG_OK);
    acc += (UfConfigArrayClear(c->h, c->path) == UF_CONFIG_OK);
  }
  g_sink += acc;
}

static void b_findpath(void *v, long n)
{
  Ctx     *c = v;
  UfNode  *root = c->h->root;
  uint64_t acc  = 0;
  for (long i = 0; i < n; i++) acc += (ConfigFindPath(root, c->path) != NULL);
  g_sink += acc;
}

static void b_canon(void *v, long n)
{
  Ctx           *c = v;
  unsigned char  dig[32];
  uint64_t       acc = 0;
  for (long i = 0; i < n; i++) acc += (ConfigCanonAndDigest(c->h->root, dig) == UF_CONFIG_OK);
  g_sink += acc;
}

static void b_serialise(void *v, long n)
{
  Ctx      *c = v;
  uint64_t  acc = 0;
  for (long i = 0; i < n; i++) {
    char *out = NULL;
    if (UfConfigToJsonAlloc(c->h, &out) == UF_CONFIG_OK) { acc += out ? strlen(out) : 0; free(out); }
  }
  g_sink += acc;
}

static void b_namespace(void *v, long n)
{
  Ctx      *c = v;
  uint64_t  acc = 0;
  for (long i = 0; i < n; i++) {
    UfConfigNamespace *ns = NULL;
    if (UfConfigNamespaceGet(c->h, c->path, &ns) == UF_CONFIG_OK) {
      acc += UfConfigNamespaceEntryCount(ns);
      free(ns);
    }
  }
  g_sink += acc;
}

static void b_reload(void *v, long n)
{
  Ctx              *c = v;
  UfConfigReloadReport rep;
  uint64_t          acc = 0;
  for (long i = 0; i < n; i++) {
    memset(&rep, 0, sizeof(rep));
    UfConfigStatus st = UfConfigReload(c->h, &rep);
    acc += (st == UF_CONFIG_OK || st == UF_CONFIG_NO_CHANGE);
  }
  g_sink += acc;
}

/* A load is measured per-document, not per-iteration, so the iteration count is
   forced to 1 and the reported figure is the whole operation. */
static void b_load_bare(void *v, long n)
{
  Ctx      *c   = v;
  uint64_t  acc = 0;
  for (long i = 0; i < n; i++) {
    UfConfig *h = open_doc(g_doc, (size_t)c->ival);
    if (h) { acc++; UfConfigDestroy(h); }
  }
  g_sink += acc;
}

/*!
 * @brief Reference measurement: a pointer chase over 4096 nodes.
 *
 * This exists to be divided by, and it does no module work at all.  Two runs of
 * an identical binary against an identical input still differ by 5-25% on the
 * slower benchmarks, because the machine is not the same machine twice: cache
 * and frequency state drift between runs, and repetitions taken back to back
 * inside one run cannot see it.  That is why a self-comparison reported 13
 * regressions and 8 improvements from nothing.
 *
 * So every delta is expressed relative to this.  It walks memory in the same
 * shape the tree walk does — dependent loads, no arithmetic to hide the
 * latency — so it tracks the drift that matters here rather than only CPU
 * clock.  If the machine was 10% slower during this run, this is 10% slower
 * too, and every real measurement is corrected by that much.
 */
#define REF_NODES 4096
static uint32_t g_ref_next[REF_NODES];

static void ref_init(void)
{
  for (uint32_t i = 0; i < REF_NODES; i++) g_ref_next[i] = (i * 1677u + 1u) % REF_NODES;
}

static void b_reference(void *v, long n)
{
  (void)v;
  uint32_t k   = 0;
  uint64_t acc = 0;
  for (long i = 0; i < n; i++) { k = g_ref_next[k]; acc += k; }
  g_sink += acc;
}

/* ── comparison ─────────────────────────────────────────────────────────── */

typedef struct
{
  char   name[96];
  double ns;
  double spread;
} Base;

static Base   g_base[MAX_RESULTS];
static int    g_nbase;

static int load_baseline(const char *file)
{
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char   name[96];
    double ns, spread;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%95s %*s %*s %lf %lf", name, &ns, &spread) == 3 && g_nbase < MAX_RESULTS) {
      snprintf(g_base[g_nbase].name, sizeof(g_base[g_nbase].name), "%s", name);
      g_base[g_nbase].ns     = ns;
      g_base[g_nbase].spread = spread;
      g_nbase++;
    }
  }
  fclose(f);
  return 0;
}

static Base *find_base(const char *name)
{
  for (int i = 0; i < g_nbase; i++) if (strcmp(g_base[i].name, name) == 0) return &g_base[i];
  return NULL;
}

static void record(const char *file)
{
  FILE *f = fopen(file, "w");
  if (!f) { fprintf(stderr, "cannot write %s\n", file); return; }
  fprintf(f, "# ufconfig bench baseline — regenerate deliberately, never automatically\n");
  fprintf(f, "# name\tgroup\titers\tns_per_op\tspread_pct\n");
  for (int i = 0; i < g_nres; i++)
    fprintf(f, "%s\t%s\t%ld\t%.4f\t%.2f\n", g_res[i].name, g_res[i].group, g_res[i].iters,
            g_res[i].median_ns, g_res[i].spread_pct);
  fclose(f);
  printf("recorded %d benchmarks to %s\n", g_nres, file);
}

int main(int argc, char **argv)
{
  const char *sample   = NULL;
  const char *rec_file = NULL;
  const char *base_file = NULL;
  int         pin      = -1;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample = argv[++i];
    else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec_file = argv[++i];
    else if (!strcmp(argv[i], "--baseline") && i + 1 < argc) base_file = argv[++i];
    else if (!strcmp(argv[i], "--pin") && i + 1 < argc) pin = atoi(argv[++i]);
    else { fprintf(stderr, "usage: %s --sample F [--record F] [--baseline F] [--pin CPU]\n", argv[0]); return 2; }
  }
  if (!sample) { fprintf(stderr, "--sample is required (a document the injected schema accepts)\n"); return 2; }

  /* Pinning removes the largest source of run-to-run variance, and variance is
     what makes a small regression unreadable. */
  if (pin >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(pin, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) perror("sched_setaffinity");
  }

  ref_init();
  bench("ref", "ref_pointer_chase", b_reference, NULL);

  /* ── the walk, against width: the curve that matters ─────────────────── */
  static const int widths[] = { 16, 64, 256, 1024, 4096 };
  for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
    int   width = widths[w];
    size_t len  = build_scoped_doc(sample, g_doc, sizeof(g_doc), width, 0);
    if (!len) { fprintf(stderr, "build_scoped_doc(%d) produced nothing from %s\n", width, sample); return 2; }
    UfConfig *h = open_doc(g_doc, len);
    if (!h) { fprintf(stderr, "wide %d failed to load (%zu bytes)\n", width, len); return 2; }

    Ctx c;
    memset(&c, 0, sizeof(c));
    c.h = h;
    snprintf(c.path, sizeof(c.path), "big.k%05d", width - 1); /* last: the worst case */

    char name[64];
    snprintf(name, sizeof(name), "findpath_undeclared_w%d", width);
    bench("walk", name, b_findpath, &c);
    snprintf(name, sizeof(name), "get_undeclared_w%d", width);
    bench("walk", name, b_get, &c);

    /* The write path takes the same walk, and it takes it under the write lock
       — which the read path does not.  Measured separately because a change
       that helps reads can leave writes where they were. */
    snprintf(name, sizeof(name), "set_undeclared_w%d", width);
    bench("write", name, b_set_int, &c);
    UfConfigDestroy(h);
  }

  /* ── array element by index, against position ────────────────────────── */
  {
    static const int aw = 512;
    size_t len = build_scoped_doc(sample, g_doc, sizeof(g_doc), 16, aw);
    UfConfig *h = open_doc(g_doc, len);
    if (!h) { fprintf(stderr, "array load failed\n"); return 2; }
    static const int idx[] = { 0, 1, 16, 128, 255, 511 };
    for (size_t k = 0; k < sizeof(idx) / sizeof(idx[0]); k++) {
      Ctx c;
      memset(&c, 0, sizeof(c));
      c.h = h;
      snprintf(c.path, sizeof(c.path), "big.arr.%d", idx[k]);
      char name[64];
      snprintf(name, sizeof(name), "findpath_array_idx%d", idx[k]);
      bench("array", name, b_findpath, &c);
      snprintf(name, sizeof(name), "get_array_idx%d", idx[k]);
      bench("array", name, b_get, &c);
    }

    /* Grow and reset, which is the pair a caller actually performs.  Both
       mutate the array, so this is the only benchmark that exercises the
       positional invariant under change. */
    Ctx cg;
    memset(&cg, 0, sizeof(cg));
    cg.h = h;
    snprintf(cg.path, sizeof(cg.path), "big.arr");
    bench("array", "array_append_then_clear", b_unset_append, &cg);

    UfConfigDestroy(h);
  }

  /* ── declared fields, through the injected schema ─────────────────────── */
  UfConfig *hs = open_schema(sample);
  if (!hs) { fprintf(stderr, "cannot load %s with the injected schema\n", sample); return 2; }

  {
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.h = hs;
    snprintf(c.path, sizeof(c.path), "ufsrv.main_listener_address");
    bench("declared", "get_declared_string", b_get, &c);
    bench("declared", "findpath_declared_string", b_findpath, &c);
    c.ival = 0;
    bench("declared", "get_byindex_0", b_get_index, &c);
    snprintf(c.path, sizeof(c.path), "root");
    bench("declared", "get_absent", b_get, &c);
    bench("declared", "canon_and_digest", b_canon, &c);
    bench("declared", "serialise_json", b_serialise, &c);
    snprintf(c.path, sizeof(c.path), "");
    bench("declared", "namespace_root", b_namespace, &c);
    bench("declared", "reload_file", b_reload, &c);
  }
  UfConfigDestroy(hs);

  /* ── load, against document width ─────────────────────────────────────── */
  for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
    int    width = widths[w];
    size_t len   = build_scoped_doc(sample, g_doc, sizeof(g_doc), width, 0);
    Ctx    c;
    memset(&c, 0, sizeof(c));
    c.ival = (int64_t)len;
    char name[64];
    snprintf(name, sizeof(name), "load_wide%d", width);
    bench("load", name, b_load_bare, &c);
  }

  /* ── report ───────────────────────────────────────────────────────────── */
  if (rec_file) record(rec_file);

  if (base_file) {
    if (load_baseline(base_file) != 0) { fprintf(stderr, "cannot read baseline %s\n", base_file); return 2; }

    /* Correct for machine drift before judging anything: scale > 1 means this
       run's machine is slower than the baseline's, and every figure is scaled
       back by that much. */
    double now_ref = 0, base_ref = 0;
    for (int i = 0; i < g_nres; i++) if (!strcmp(g_res[i].name, "ref_pointer_chase")) now_ref = g_res[i].median_ns;
    { Base *rb = find_base("ref_pointer_chase"); if (rb) base_ref = rb->ns; }
    double scale = (now_ref > 0 && base_ref > 0) ? base_ref / now_ref : 1.0;
    printf("reference: baseline %.2f ns, this run %.2f ns -> machine drift %+.2f%%\n\n",
           base_ref, now_ref, (scale - 1.0) * 100.0);
    printf("%-34s %10s %10s %9s %9s  %s\n", "benchmark", "base ns", "now ns", "delta", "spread", "verdict");
    int regressions = 0, improvements = 0;
    for (int i = 0; i < g_nres; i++) {
      Base *b = find_base(g_res[i].name);
      if (!b) { printf("%-34s %10s %10.1f %9s %8.2f%%  (new)\n", g_res[i].name, "-", g_res[i].median_ns, "-", g_res[i].spread_pct); continue; }
      if (!strcmp(g_res[i].name, "ref_pointer_chase")) continue; /* the yardstick is not a subject */
      double delta = (g_res[i].median_ns * scale - b->ns) / b->ns * 100.0;
      /* A difference smaller than the noise it was measured with is not a
         finding, however tempting the number looks. */
      double noise = b->spread > g_res[i].spread_pct ? b->spread : g_res[i].spread_pct;
      const char *verdict;
      if (g_res[i].unstable) verdict = "UNSTABLE (spread > 10%)";
      else if (delta > noise && delta > SPREAD_NOISE_FLOOR) { verdict = "REGRESSION"; regressions++; }
      else if (-delta > noise && -delta > SPREAD_NOISE_FLOOR) { verdict = "improvement"; improvements++; }
      else verdict = "~ within noise";
      printf("%-34s %10.1f %10.1f %8.2f%% %8.2f%%  %s\n", g_res[i].name, b->ns, g_res[i].median_ns, delta,
             g_res[i].spread_pct, verdict);
    }
    printf("\n%d regression(s), %d improvement(s), %d benchmark(s)\n", regressions, improvements, g_nres);
    return regressions ? 1 : 0;
  }

  printf("%-34s %-9s %12s %10s %9s  %s\n", "benchmark", "group", "iters", "ns/op(best)", "spread", "note");
  for (int i = 0; i < g_nres; i++)
    printf("%-34s %-9s %12ld %10.1f %8.2f%%  %s\n", g_res[i].name, g_res[i].group, g_res[i].iters,
           g_res[i].median_ns, g_res[i].spread_pct, g_res[i].unstable ? "unstable" : "");
  return 0;
}
