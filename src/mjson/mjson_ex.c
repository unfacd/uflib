/**
 * @file mjson_ex.cpp
 * @brief mjson_ex
 *
*Notes / limitations of this implementation

Filters support only a single @.key op value expression (no && / || yet).
Recursive descent + wildcard on very large documents is still linear but can be slower than a pure DOM approach.
The implementation re-uses mjson_next and the original mjson_find for filter evaluation, keeping the code size modest.
Original API is completely untouched.

The original mjson_get_cb is a simple depth + index matcher.

The _ex version needs a small path compiler that turns the path string into a list of steps.
During the single SAX pass you maintain a small stack of matching states. When a wildcard or recursive step is active you accept more than one candidate and continue. Filters are evaluated by temporarily looking up a sibling key on the current object (still possible in a single pass with careful bookkeeping).
What stays impossible (by design)

Mutation / update / remove
Holding live node references
Complex nested filter expressions
Guaranteed order of recursive matches on very large documents

Summary


FunctionBehaviourmjson_findOriginal, unchangedmjson_find_exSupports [*], .., simple filters (first match)mjson_find_all_exSame paths, calls a callback for every matchmjson_get_*_exConvenience wrappers around mjson_find_ex

sample
const char *json =
    "{"
    "  \"store\": {"
    "    \"book\": ["
    "      {\"title\":\"Book A\",\"price\":8.95,\"category\":\"fiction\"},"
    "      {\"title\":\"Book B\",\"price\":12.99,\"category\":\"reference\"},"
    "      {\"title\":\"Book C\",\"price\":22.50,\"category\":\"fiction\"}"
    "    ],"
    "    \"bicycle\": {\"color\":\"red\",\"price\":19.95}"
    "  }"
    "}";
int len = strlen(json);
const char *tok;
int toklen;

// 1. Classic path – still works with original API
mjson_find(json, len, "$.store.book[1].title", &tok, &toklen);

// 2. Wildcard – first book
mjson_find_ex(json, len, "$.store.book[*]", &tok, &toklen);

// 3. Recursive descent – first price anywhere
mjson_find_ex(json, len, "$..price", &tok, &toklen);

// 4. Filter
mjson_find_ex(json, len, "$.store.book[?(@.price > 10)]", &tok, &toklen);

//5. Combined
mjson_find_ex(json, len,
              "$..book[?(@.category=='fiction' && @.price<20)].title",
              &tok, &toklen);

// 6. Find all matches with a callback
static void print_match(const char *tok, int toklen, int type, void *ud) {
  (void)type; (void)ud;
  printf("match: %.*s\n", toklen, tok);
}

mjson_find_all_ex(json, len, "$..price", print_match, NULL);
mjson_find_all_ex(json, len, "$.store.book[?(@.price>10)]", print_match, NULL);

 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 9/09/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include "mjson_ex.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Simple path step representation                                    */
/* ------------------------------------------------------------------ */

enum step_type {
  STEP_ROOT,
  STEP_KEY,        /* .name or ['name'] */
  STEP_INDEX,      /* [n] */
  STEP_WILDCARD,   /* [*] */
  STEP_RECURSIVE,  /* .. */
  STEP_FILTER      /* [?(@.key op value)] */
};

struct filter_expr {
  char key[64];
  char op;          /* '>', '<', '=', '!' (for !=) or 0 = existence */
  int  is_string;
  double num;
  char str[64];
};

struct path_step {
  enum step_type type;
  char key[64];
  int index;
  struct filter_expr filter;
};

#define MAX_STEPS 32

/* ------------------------------------------------------------------ */
/* Path compiler                                                      */
/* ------------------------------------------------------------------ */

static int parse_string_literal(const char **pp, char *out, int outlen) {
  const char *p = *pp;
  int i = 0;
  char quote = *p++;
  while (*p && *p != quote && i < outlen - 1) {
    if (*p == '\\' && p[1]) p++;
    out[i++] = *p++;
  }
  if (*p != quote) return -1;
  out[i] = '\0';
  *pp = p + 1;
  return 0;
}

static int compile_path(const char *path, struct path_step *steps, int max_steps) {
  const char *p = path;
  int n = 0;

  if (*p != '$') return -1;
  p++;

  steps[n].type = STEP_ROOT;
  n++;

  while (*p && n < max_steps) {
    if (*p == '.') {
      p++;
      if (*p == '.') {                     /* .. */
        steps[n].type = STEP_RECURSIVE;
        n++;
        p++;
        continue;
      }
      /* .key */
      steps[n].type = STEP_KEY;
      int i = 0;
      while (*p && *p != '.' && *p != '[' && i < (int)sizeof(steps[n].key)-1) {
        if (*p == '\\' && p[1]) p++;
        steps[n].key[i++] = *p++;
      }
      steps[n].key[i] = '\0';
      n++;
    } else if (*p == '[') {
      p++;
      if (*p == '*') {                     /* [*] */
        steps[n].type = STEP_WILDCARD;
        n++;
        p++;
        if (*p != ']') return -1;
        p++;
      } else if (*p == '?') {              /* [?(...)] */
        p++;
        if (*p != '(') return -1;
        p++;
        steps[n].type = STEP_FILTER;
        memset(&steps[n].filter, 0, sizeof(steps[n].filter));

        /* expect @.key */
        if (p[0] != '@' || p[1] != '.') return -1;
        p += 2;
        int i = 0;
        while (*p && *p != ' ' && *p != ')' && *p != '=' && *p != '!' &&
               *p != '<' && *p != '>' && i < 63) {
          steps[n].filter.key[i++] = *p++;
        }
        steps[n].filter.key[i] = '\0';

        /* optional operator + value */
        while (*p == ' ') p++;
        if (*p == ')') {
          steps[n].filter.op = 0;          /* existence */
        } else {
          if (p[0] == '!' && p[1] == '=') {
            steps[n].filter.op = '!';
            p += 2;
          } else if (*p == '=' || *p == '<' || *p == '>') {
            steps[n].filter.op = *p++;
            if (*p == '=') p++;            /* accept ==, <=, >= */
          } else {
            return -1;
          }
          while (*p == ' ') p++;
          if (*p == '\'' || *p == '"') {
            steps[n].filter.is_string = 1;
            if (parse_string_literal(&p, steps[n].filter.str,
                                     sizeof(steps[n].filter.str)) < 0)
              return -1;
          } else {
            char *end;
            steps[n].filter.num = strtod(p, &end);
            if (end == p) return -1;
            p = end;
          }
        }
        while (*p == ' ') p++;
        if (*p != ')') return -1;
        p++;
        if (*p != ']') return -1;
        p++;
        n++;
      } else {                             /* [n] */
        steps[n].type = STEP_INDEX;
        char *end;
        steps[n].index = (int)strtol(p, &end, 10);
        if (end == p) return -1;
        p = end;
        if (*p != ']') return -1;
        p++;
        n++;
      }
    } else {
      return -1;
    }
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* Filter evaluation (very small helper)                              */
/* ------------------------------------------------------------------ */

static int eval_filter(const char *obj, int objlen,
                       const struct filter_expr *f) {
  char path[80];
  snprintf(path, sizeof(path), "$.%s", f->key);

  if (f->op == 0) {                        /* existence */
    return mjson_find(obj, objlen, path, NULL, NULL) != MJSON_TOK_INVALID;
  }

  if (f->is_string) {
    char buf[128];
    if (mjson_get_string(obj, objlen, path, buf, sizeof(buf)) < 0)
      return 0;
    int cmp = strcmp(buf, f->str);
    if (f->op == '=') return cmp == 0;
    if (f->op == '!') return cmp != 0;
    return 0;
  } else {
    double v;
    if (!mjson_get_number(obj, objlen, path, &v)) return 0;
    if (f->op == '>') return v > f->num;
    if (f->op == '<') return v < f->num;
    if (f->op == '=') return v == f->num;
    if (f->op == '!') return v != f->num;
    return 0;
  }
}

/* ------------------------------------------------------------------ */
/* Core matching engine                                               */
/* ------------------------------------------------------------------ */

struct match_ctx {
  const struct path_step *steps;
  int nsteps;
  int cur;                    /* current step index */

  mjson_match_cb_t cb;
  void *userdata;
  int count;                  /* how many matches emitted */

  int first_only;
  const char **tokptr;
  int *toklen;
  int found_tok;
};

static int emit(struct match_ctx *ctx, const char *tok, int toklen, int type) {
  ctx->count++;
  if (ctx->cb) {
    ctx->cb(tok, toklen, type, ctx->userdata);
  }
  if (ctx->first_only) {
    if (ctx->tokptr) *ctx->tokptr = tok;
    if (ctx->toklen) *ctx->toklen = toklen;
    ctx->found_tok = type;
    return 1;                 /* stop */
  }
  return 0;
}

/* Recursive descent helper – walks every value */
static int walk(const char *s, int len, int off, int depth,
                struct match_ctx *ctx);

static int try_match(const char *s, int len, int off, int vlen, int type,
                     struct match_ctx *ctx) {
  if (ctx->cur >= ctx->nsteps) {
    return emit(ctx, s + off, vlen, type);
  }

  const struct path_step *st = &ctx->steps[ctx->cur];

  if (st->type == STEP_RECURSIVE) {
    ctx->cur++;
    int stop = walk(s, len, off, 0, ctx);
    ctx->cur--;
    return stop;
  }

  if (type == MJSON_TOK_OBJECT || type == '{' ) {
    /* need to iterate keys */
    int koff, klen, voff, vlen2, vtype;
    int pos = off;
    while ((pos = mjson_next(s, len, pos, &koff, &klen, &voff, &vlen2, &vtype)) > 0) {
      int matched = 0;
      if (st->type == STEP_KEY) {
        if (klen - 2 == (int)strlen(st->key) &&
            memcmp(s + koff + 1, st->key, klen - 2) == 0)
          matched = 1;
      } else if (st->type == STEP_WILDCARD) {
        matched = 1;
      } else if (st->type == STEP_FILTER) {
        /* filter applies to the value (object) */
        if (vtype == MJSON_TOK_OBJECT || vtype == '{') {
          if (eval_filter(s + voff, vlen2, &st->filter))
            matched = 1;
        }
      }

      if (matched) {
        ctx->cur++;
        int stop = try_match(s, len, voff, vlen2, vtype, ctx);
        ctx->cur--;
        if (stop) return 1;
      }
      if (vtype == '}' || vtype == ']') pos = voff + vlen2;
    }
  } else if (type == MJSON_TOK_ARRAY || type == '[') {
    int idx = 0;
    int koff, klen, voff, vlen2, vtype;
    int pos = off;
    while ((pos = mjson_next(s, len, pos, &koff, &klen, &voff, &vlen2, &vtype)) > 0) {
      int matched = 0;
      if (st->type == STEP_INDEX && idx == st->index) matched = 1;
      else if (st->type == STEP_WILDCARD) matched = 1;
      else if (st->type == STEP_FILTER) {
        if (vtype == MJSON_TOK_OBJECT || vtype == '{') {
          if (eval_filter(s + voff, vlen2, &st->filter))
            matched = 1;
        }
      }

      if (matched) {
        ctx->cur++;
        int stop = try_match(s, len, voff, vlen2, vtype, ctx);
        ctx->cur--;
        if (stop) return 1;
      }
      idx++;
      if (vtype == '}' || vtype == ']') pos = voff + vlen2;
    }
  }
  return 0;
}

static int walk(const char *s, int len, int off, int depth,
                struct match_ctx *ctx) {
  int koff, klen, voff, vlen, vtype;
  int pos = off;
  while ((pos = mjson_next(s, len, pos, &koff, &klen, &voff, &vlen, &vtype)) > 0) {
    /* try current position against remaining path */
    int stop = try_match(s, len, voff, vlen, vtype, ctx);
    if (stop) return 1;

    /* recurse into containers for recursive descent */
    if (vtype == MJSON_TOK_OBJECT || vtype == '{' ||
        vtype == MJSON_TOK_ARRAY  || vtype == '[') {
      stop = walk(s, len, voff, depth + 1, ctx);
      if (stop) return 1;
    }
    if (vtype == '}' || vtype == ']') pos = voff + vlen;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int mjson_find_ex(const char *s, int len, const char *path,
                  const char **tokptr, int *toklen) {
  struct path_step steps[MAX_STEPS];
  int n = compile_path(path, steps, MAX_STEPS);
  if (n <= 0) return MJSON_TOK_INVALID;

  struct match_ctx ctx = {0};
  ctx.steps = steps;
  ctx.nsteps = n;
  ctx.cur = 1;                /* skip ROOT */
  ctx.first_only = 1;
  ctx.tokptr = tokptr;
  ctx.toklen = toklen;
  ctx.found_tok = MJSON_TOK_INVALID;

  /* special case: path is just "$" */
  if (n == 1) {
    if (tokptr) *tokptr = s;
    if (toklen) *toklen = len;
    return (s[0] == '{') ? MJSON_TOK_OBJECT : MJSON_TOK_ARRAY;
  }

  try_match(s, len, 0, len, (s[0] == '{') ? MJSON_TOK_OBJECT : MJSON_TOK_ARRAY, &ctx);
  return ctx.found_tok;
}

int mjson_find_all_ex(const char *s, int len, const char *path,
                      mjson_match_cb_t cb, void *userdata) {
  struct path_step steps[MAX_STEPS];
  int n = compile_path(path, steps, MAX_STEPS);
  if (n <= 0) return -1;

  struct match_ctx ctx = {0};
  ctx.steps = steps;
  ctx.nsteps = n;
  ctx.cur = 1;
  ctx.cb = cb;
  ctx.userdata = userdata;

  try_match(s, len, 0, len, (s[0] == '{') ? MJSON_TOK_OBJECT : MJSON_TOK_ARRAY, &ctx);
  return ctx.count;
}

int mjson_get_number_ex(const char *s, int len, const char *path, double *v) {
  const char *p;
  int n, tok = mjson_find_ex(s, len, path, &p, &n);
  if (tok != MJSON_TOK_NUMBER) return 0;
  if (v) *v = strtod(p, NULL);
  return 1;
}

int mjson_get_bool_ex(const char *s, int len, const char *path, int *v) {
  int tok = mjson_find_ex(s, len, path, NULL, NULL);
  if (tok == MJSON_TOK_TRUE)  { if (v) *v = 1; return 1; }
  if (tok == MJSON_TOK_FALSE) { if (v) *v = 0; return 1; }
  return 0;
}

int mjson_get_string_ex(const char *s, int len, const char *path,
                        char *to, int n) {
  const char *p;
  int sz;
  if (mjson_find_ex(s, len, path, &p, &sz) != MJSON_TOK_STRING) return -1;
  /* reuse original unescaper via a tiny trick – call the public getter
     on a temporary buffer that contains only the matched token */
  return mjson_get_string(p, sz, "$", to, n);
}