/**
 * @file ufconfig_driver_mem.c
 * @brief The in-memory driver: a pair-set held in the process, for tests and embedding.
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
#include <stdlib.h>
#include <string.h>

typedef struct MemPair {
    char *path;
    UfConfigPairType vtype;
    char *encoded;
} MemPair;

typedef struct MemCtx {
    MemPair *p;
    size_t n, cap;
    UfConfigMeta meta;
    int present;
    int reverse;
} MemCtx;

static char *sDupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

static UfConfigStatus sMemOpen(void **ctx, const UfConfigBackendSpec *spec) {
    MemCtx *m = (MemCtx *)calloc(1, sizeof(*m));
    if (!m) return UF_CONFIG_ERR_NO_MEMORY;
    m->meta.fmt = 1;
    m->reverse = spec && spec->u.mem.name && strstr(spec->u.mem.name, "reverse");
    *ctx = m;
    return UF_CONFIG_OK;
}

static UfConfigStatus sMemReadAll(void *ctx, UfConfigFieldPair **out, size_t *n, UfConfigMeta *meta) {
    MemCtx *m = (MemCtx *)ctx;
    if (!m->present) return UF_CONFIG_ERR_NOT_CONFIGURED;
    UfConfigFieldPair *a = (UfConfigFieldPair *)calloc(m->n ? m->n : 1, sizeof(*a));
    for (size_t i = 0; i < m->n; i++) {
        size_t j = m->reverse ? (m->n - 1 - i) : i;
        a[i].path = m->p[j].path;
        a[i].vtype = m->p[j].vtype;
        a[i].encoded = m->p[j].encoded;
    }
    *out = a; *n = m->n;
    if (meta) *meta = m->meta;
    return UF_CONFIG_OK;
}

static UfConfigStatus sMemWriteAll(void *ctx, const UfConfigFieldPair *in, size_t n, const UfConfigMeta *meta) {
    MemCtx *m = (MemCtx *)ctx;
    for (size_t i = 0; i < m->n; i++) { free(m->p[i].path); free(m->p[i].encoded); }
    free(m->p);
    m->p = (MemPair *)calloc(n ? n : 1, sizeof(MemPair));
    m->n = n;
    for (size_t i = 0; i < n; i++) {
        m->p[i].path = sDupstr(in[i].path);
        m->p[i].vtype = in[i].vtype;
        m->p[i].encoded = sDupstr(in[i].encoded);
    }
    if (meta) m->meta = *meta;
    m->meta.version++;
    m->present = 1;
    return UF_CONFIG_OK;
}

static UfConfigStatus sMemReadOne(void *ctx, const char *path, UfConfigFieldPair *out) {
    MemCtx *m = (MemCtx *)ctx;
    if (!m->present) return UF_CONFIG_ERR_NOT_CONFIGURED;
    for (size_t i = 0; i < m->n; i++)
        if (m->p[i].path && path && strcmp(m->p[i].path, path) == 0) {
            out->path = m->p[i].path; out->vtype = m->p[i].vtype; out->encoded = m->p[i].encoded;
            return UF_CONFIG_OK;
        }
    return UF_CONFIG_ERR_NOFIELD;
}

static UfConfigStatus sMemWriteOne(void *ctx, const char *path, const UfConfigFieldPair *in) {
    MemCtx *m = (MemCtx *)ctx;
    if (!m->present) return UF_CONFIG_ERR_NOT_CONFIGURED;
    for (size_t i = 0; i < m->n; i++)
        if (m->p[i].path && strcmp(m->p[i].path, path) == 0) {
            free(m->p[i].encoded);
            m->p[i].encoded = sDupstr(in->encoded);
            m->p[i].vtype = in->vtype;
            m->meta.version++;
            return UF_CONFIG_OK;
        }
    return UF_CONFIG_ERR_NOFIELD;
}

static UfConfigStatus sMemStat(void *ctx, UfConfigStamp *out) {
    MemCtx *m = (MemCtx *)ctx;
    if (!m->present) return UF_CONFIG_ERR_NOT_CONFIGURED;
    out->token = m->meta.version;
    return UF_CONFIG_OK;
}

static void sMemClose(void *ctx) {
    MemCtx *m = (MemCtx *)ctx;
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) { free(m->p[i].path); free(m->p[i].encoded); }
    free(m->p); free(m);
}

const UfConfigDriver ufconfig_driver_mem = {
    .caps = {
        .api_version = UF_CONFIG_DRIVER_API_VERSION,
        .kind        = UF_CONFIG_BACKEND_MEM,
        .atomic_write_all = true, .atomic_write_one = true,
        .per_field_read = true, .native_version = true,
        .enumeration = true, .multi_config_txn = true,
        .write_one_creates = false,
        .max_path_bytes = 512, .max_value_bytes = 1u << 20, .max_fields = 65536,
        .name = "mem",
    },
    .open = sMemOpen, .read_all = sMemReadAll, .write_all = sMemWriteAll,
    .read_one = sMemReadOne, .write_one = sMemWriteOne,
    .stat = sMemStat, .close = sMemClose,
};
