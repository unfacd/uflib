#include <uflib/ufcommand/ufcommand_telemetry.h>
#include "ufcommand_priv.h"
#include "ufcommand_telemetry_priv.h"
#include "ufcommand_type_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>

typedef struct UfCommandTelemetryNameRecord
{
  char *   name;
  uint64_t count;
} UfCommandTelemetryNameRecord;

typedef struct UfCommandTelemetryBindingRecord
{
  UfCommandModifier modifiers;
  char *            key;
  uint64_t          count;
} UfCommandTelemetryBindingRecord;

struct UfCommandTelemetry
{
  UfCommandTelemetryNameRecord *commands, *aliases;
  UfCommandTelemetryBindingRecord *bindings;
  size_t command_count, command_capacity, alias_count, alias_capacity, binding_count, binding_capacity;
};

static char *sDup(const char *s)
{
  size_t n;
  if (!s)return NULL;
  n       = strlen(s);
  char *p = malloc(n + 1);
  if (!p)return NULL;
  memcpy(p, s, n + 1);
  return p;
}

static UfCommandStatus sGrowNames(UfCommandTelemetryNameRecord **a, size_t *cap, size_t need)
{
  if (need <= *cap)return UFCOMMAND_OK;
  size_t n = *cap ? *cap * 2 : 16;
  if (n < need || n < *cap)return UFCOMMAND_NO_MEMORY;
  void *p = realloc(*a, n * sizeof(**a));
  if (!p)return UFCOMMAND_NO_MEMORY;
  memset((char*)p + (*cap) * sizeof(**a), 0, (n - *cap) * sizeof(**a));
  *a   = p;
  *cap = n;
  return UFCOMMAND_OK;
}

static UfCommandStatus sGrowBindings(UfCommandTelemetryBindingRecord **a, size_t *cap, size_t need)
{
  if (need <= *cap)return UFCOMMAND_OK;
  size_t n = *cap ? *cap * 2 : 16;
  if (n < need || n < *cap)return UFCOMMAND_NO_MEMORY;
  void *p = realloc(*a, n * sizeof(**a));
  if (!p)return UFCOMMAND_NO_MEMORY;
  memset((char*)p + (*cap) * sizeof(**a), 0, (n - *cap) * sizeof(**a));
  *a   = p;
  *cap = n;
  return UFCOMMAND_OK;
}

static UfCommandStatus sIncName(UfCommandTelemetryNameRecord **a, size_t *count, size_t *cap, const char *name)
{
  if (!name || !*name)return UFCOMMAND_INVALID_ARGUMENT;
  for (size_t i = 0; i < *count; i++)
    if (strcmp((*a)[i].name, name) == 0) {
      (*a)[i].count++;
      return UFCOMMAND_OK;
    }
  UfCommandStatus st = sGrowNames(a, cap, *count + 1);
  if (st != UFCOMMAND_OK)return st;
  (*a)[*count].name = sDup(name);
  if (!(*a)[*count].name)return UFCOMMAND_NO_MEMORY;
  (*a)[*count].count = 1;
  (*count)++;
  return UFCOMMAND_OK;
}

static UfCommandStatus sIncBinding(UfCommandTelemetry *t, UfCommandModifier m, const char *key)
{
  if (!key || !*key)return UFCOMMAND_INVALID_ARGUMENT;
  for (size_t i = 0; i < t->binding_count; i++)
    if (t->bindings[i].modifiers == m && strcmp(t->bindings[i].key, key) == 0) {
      t->bindings[i].count++;
      return UFCOMMAND_OK;
    }
  UfCommandStatus st = sGrowBindings(&t->bindings, &t->binding_capacity, t->binding_count + 1);
  if (st != UFCOMMAND_OK)return st;
  t->bindings[t->binding_count].modifiers = m;
  t->bindings[t->binding_count].key       = sDup(key);
  if (!t->bindings[t->binding_count].key)return UFCOMMAND_NO_MEMORY;
  t->bindings[t->binding_count].count = 1;
  t->binding_count++;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandTelemetryCreate(UfCommandTelemetry **out)
{
  if (!out)return UFCOMMAND_INVALID_ARGUMENT;
  *out                  = NULL;
  UfCommandTelemetry *t = calloc(1, sizeof(*t));
  if (!t)return UFCOMMAND_NO_MEMORY;
  size_t n            = 16;
  t->command_capacity = t->alias_capacity = t->binding_capacity = 0;
  if (n) {
    if (sGrowNames(&t->commands, &t->command_capacity, n) != UFCOMMAND_OK ||
      sGrowNames(&t->aliases, &t->alias_capacity, n) != UFCOMMAND_OK || sGrowBindings(
        &t->bindings, &t->binding_capacity, n) != UFCOMMAND_OK) {
      UfCommandTelemetryDestroy(t);
      return UFCOMMAND_NO_MEMORY;
    }
  }
  *out = t;
  return UFCOMMAND_OK;
}

void UfCommandTelemetryDestroy(UfCommandTelemetry *t)
{
  if (!t)return;
  for (size_t i = 0; i < t->command_count; i++)free(t->commands[i].name);
  for (size_t i = 0; i < t->alias_count; i++)free(t->aliases[i].name);
  for (size_t i = 0; i < t->binding_count; i++)free(t->bindings[i].key);
  free(t->commands);
  free(t->aliases);
  free(t->bindings);
  free(t);
}

void UfCommandTelemetryReset(UfCommandTelemetry *t)
{
  if (!t)return;
  for (size_t i = 0; i < t->command_count; i++)t->commands[i].count = 0;
  for (size_t i = 0; i < t->alias_count; i++)t->aliases[i].count = 0;
  for (size_t i = 0; i < t->binding_count; i++)t->bindings[i].count = 0;
}

static UfCommandStatus sGetName(const UfCommandTelemetryNameRecord *a, size_t n, const char *name, uint64_t *out)
{
  if (!name || !out)return UFCOMMAND_INVALID_ARGUMENT;
  for (size_t i = 0; i < n; i++)
    if (strcmp(a[i].name, name) == 0) {
      *out = a[i].count;
      return UFCOMMAND_OK;
    }
  *out = 0;
  return UFCOMMAND_NOT_FOUND;
}

UfCommandStatus UfCommandTelemetryGetCommandCount(const UfCommandTelemetry *t, const char *n, uint64_t *o)
{
  if (!t)return UFCOMMAND_INVALID_ARGUMENT;
  return sGetName(t->commands, t->command_count, n, o);
}

UfCommandStatus UfCommandTelemetryGetAliasCount(const UfCommandTelemetry *t, const char *n, uint64_t *o)
{
  if (!t)return UFCOMMAND_INVALID_ARGUMENT;
  return sGetName(t->aliases, t->alias_count, n, o);
}

UfCommandStatus UfCommandTelemetryGetBindingCount(const UfCommandTelemetry *t, UfCommandModifier m, const char *k,
                                                  uint64_t *                o)
{
  if (!t || !k || !o)return UFCOMMAND_INVALID_ARGUMENT;
  for (size_t i = 0; i < t->binding_count; i++)
    if (t->bindings[i].modifiers == m && strcmp(t->bindings[i].key, k) == 0) {
      *o = t->bindings[i].count;
      return UFCOMMAND_OK;
    }
  *o = 0;
  return UFCOMMAND_NOT_FOUND;
}

static int sCmpName(const void *a, const void *b)
{
  return strcmp(((const UfCommandTelemetryNameRecord*)a)->name, ((const UfCommandTelemetryNameRecord*)b)->name);
}

static int sCmpBinding(const void *a, const void *b)
{
  const UfCommandTelemetryBindingRecord *x = a, *y = b;
  if (x->modifiers < y->modifiers)return -1;
  if (x->modifiers > y->modifiers)return 1;
  return strcmp(x->key, y->key);
}

static void sJsonEsc(FILE *f, const char *s)
{
  fputc('"', f);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      fputc('\\', f);
      fputc(c, f);
    }
    else if (c == '\n')fputs("\\n", f);
    else if (c == '\r')fputs("\\r", f);
    else if (c == '\t')fputs("\\t", f);
    else if (c < 32)fprintf(f, "\\u%04x", c);
    else fputc(c, f);
  }
  fputc('"', f);
}

static void sModJson(FILE *f, UfCommandModifier m)
{
  bool first = true;
  fputc('[', f);
  const struct
  {
    UfCommandModifier m;
    const char *      n;
  } x[] = {
    {UFCOMMAND_MOD_CTRL, "CTRL"}, {UFCOMMAND_MOD_SHIFT, "SHIFT"}, {UFCOMMAND_MOD_ALT, "ALT"},
    {UFCOMMAND_MOD_META, "META"}
  };
  for (size_t i = 0; i < 4; i++)
    if ((m & x[i].m) != 0) {
      if (!first)fputc(',', f);
      sJsonEsc(f, x[i].n);
      first = false;
    }
  fputc(']', f);
}

UfCommandStatus UfCommandTelemetryGetJson(const UfCommandTelemetry *t, char **out)
{
  if (!t || !out)return UFCOMMAND_INVALID_ARGUMENT;
  *out    = NULL;
  FILE *f = tmpfile();
  if (!f)return UFCOMMAND_IO_ERROR;
  UfCommandTelemetryNameRecord *   c = NULL, *a = NULL;
  UfCommandTelemetryBindingRecord *b = NULL;
  if (t->command_count) {
    c = malloc(t->command_count * sizeof(*c));
    if (!c) {
      fclose(f);
      return UFCOMMAND_NO_MEMORY;
    }
    memcpy(c, t->commands, t->command_count * sizeof(*c));
    qsort(c, t->command_count, sizeof(*c), sCmpName);
  }
  if (t->alias_count) {
    a = malloc(t->alias_count * sizeof(*a));
    if (!a) {
      free(c);
      fclose(f);
      return UFCOMMAND_NO_MEMORY;
    }
    memcpy(a, t->aliases, t->alias_count * sizeof(*a));
    qsort(a, t->alias_count, sizeof(*a), sCmpName);
  }
  if (t->binding_count) {
    b = malloc(t->binding_count * sizeof(*b));
    if (!b) {
      free(c);
      free(a);
      fclose(f);
      return UFCOMMAND_NO_MEMORY;
    }
    memcpy(b, t->bindings, t->binding_count * sizeof(*b));
    qsort(b, t->binding_count, sizeof(*b), sCmpBinding);
  }
  fputs("{\n  \"version\": 1,\n  \"commands\": [", f);
  for (size_t i = 0; i < t->command_count; i++) {
    if (i)fputc(',', f);
    fprintf(f, "\n    { \"name\": ");
    sJsonEsc(f, c[i].name);
    fprintf(f, ", \"count\": %"PRIu64" }", c[i].count);
  }
  if (t->command_count)fputc('\n', f);
  fputs("  ],\n  \"aliases\": [", f);
  for (size_t i = 0; i < t->alias_count; i++) {
    if (i)fputc(',', f);
    fprintf(f, "\n    { \"name\": ");
    sJsonEsc(f, a[i].name);
    fprintf(f, ", \"count\": %"PRIu64" }", a[i].count);
  }
  if (t->alias_count)fputc('\n', f);
  fputs("  ],\n  \"bindings\": [", f);
  for (size_t i = 0; i < t->binding_count; i++) {
    if (i)fputc(',', f);
    fprintf(f, "\n    { \"modifiers\": ");
    sModJson(f, b[i].modifiers);
    fprintf(f, ", \"key\": ");
    sJsonEsc(f, b[i].key);
    fprintf(f, ", \"count\": %"PRIu64" }", b[i].count);
  }
  if (t->binding_count)fputc('\n', f);
  fputs("  ]\n}\n", f);
  long n = ftell(f);
  if (n < 0) {
    free(c);
    free(a);
    free(b);
    fclose(f);
    return UFCOMMAND_IO_ERROR;
  }
  if (fseek(f, 0,SEEK_SET) != 0) {
    free(c);
    free(a);
    free(b);
    fclose(f);
    return UFCOMMAND_IO_ERROR;
  }
  char *s = malloc((size_t)n + 1);
  if (!s) {
    free(c);
    free(a);
    free(b);
    fclose(f);
    return UFCOMMAND_NO_MEMORY;
  }
  size_t got = fread(s, 1, (size_t)n, f);
  fclose(f);
  free(c);
  free(a);
  free(b);
  if (got != (size_t)n) {
    free(s);
    return UFCOMMAND_IO_ERROR;
  }
  s[n] = '\0';
  *out = s;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandTelemetrySaveJson(const UfCommandTelemetry *telemetry, const char *path)
{
  if (!telemetry || !path || !*path)return UFCOMMAND_INVALID_ARGUMENT;
  char *          json = NULL;
  UfCommandStatus st   = UfCommandTelemetryGetJson(telemetry, &json);
  if (st != UFCOMMAND_OK)return st;
  FILE *f = fopen(path, "wb");
  if (!f) {
    free(json);
    return UFCOMMAND_IO_ERROR;
  }
  // Close exactly once on every path: chaining these with && would skip the
  // fclose() when the write fails short, leaking the descriptor.
  size_t n      = strlen(json);
  bool   wrote  = fwrite(json, 1, n, f) == n;
  bool   closed = fclose(f) == 0;
  free(json);
  return (wrote && closed) ? UFCOMMAND_OK : UFCOMMAND_IO_ERROR;
}

/* Internal hooks used by the dispatcher. */
UfCommandStatus UfCommandTelemetryCommand(UfCommandTelemetry *t, const char *n)
{
  return t ? sIncName(&t->commands, &t->command_count, &t->command_capacity, n) : UFCOMMAND_OK;
}

UfCommandStatus UfCommandTelemetryAlias(UfCommandTelemetry *t, const char *n)
{
  return t ? sIncName(&t->aliases, &t->alias_count, &t->alias_capacity, n) : UFCOMMAND_OK;
}

UfCommandStatus UfCommandTelemetryBinding(UfCommandTelemetry *t, UfCommandModifier m, const char *k)
{
  return t ? sIncBinding(t, m, k) : UFCOMMAND_OK;
}

/* Attaching replaces whatever is attached.  Detaching with NULL destroys an
 * observer the registry created for itself (it was the registry's to destroy) but
 * only detaches one a host attached, because that one is the host's. */
UfCommandStatus UfCommandRegistrySetTelemetry(UfCommandRegistry *registry, UfCommandTelemetry *telemetry)
{
  if (!registry)return UFCOMMAND_INVALID_ARGUMENT;
  if (registry->telemetry_owned && registry->telemetry && registry->telemetry != telemetry) {
    UfCommandTelemetryDestroy(registry->telemetry);
  }
  registry->telemetry       = telemetry;
  registry->telemetry_owned = false;
  return UFCOMMAND_OK;
}

UfCommandTelemetry *UfCommandRegistryGetTelemetry(UfCommandRegistry *registry)
{
  return registry ? registry->telemetry : NULL;
}
