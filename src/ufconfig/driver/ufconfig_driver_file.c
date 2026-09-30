/**
 * @file ufconfig_driver_file.c
 * @brief The file driver: a configuration read from and written to a document.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FileCtx {
    char *path;
    int present;
} FileCtx;

static UfConfigStatus sFileOpen(void **ctx, const UfConfigBackendSpec *spec) {
    if (!spec || !spec->u.file.path) return UF_CONFIG_ERR_INVALID_ARG;
    FileCtx *f = (FileCtx *)calloc(1, sizeof(*f));
    f->path = strdup(spec->u.file.path);
    FILE *fp = fopen(f->path, "rb");
    f->present = fp != NULL;
    if (fp) fclose(fp);
    *ctx = f;
    return UF_CONFIG_OK;
}

static UfConfigStatus sFileReadAll(void *ctx, UfConfigFieldPair **out, size_t *n, UfConfigMeta *meta) {
    FileCtx *f = (FileCtx *)ctx;
    FILE *fp = fopen(f->path, "rb");
    if (!fp) return UF_CONFIG_ERR_NOT_CONFIGURED;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t nr = fread(buf, 1, (size_t)sz, fp); fclose(fp); buf[nr] = 0;
    UfConfigStatus st = UfConfigPairsFromLua(buf, nr, out, n);
    free(buf);
    if (meta) { memset(meta, 0, sizeof(*meta)); meta->fmt = 1; meta->version = 1; }
    return st;
}

static UfConfigStatus sFileWriteAll(void *ctx, const UfConfigFieldPair *in, size_t n, const UfConfigMeta *meta) {
    (void)meta;
    FileCtx *f = (FileCtx *)ctx;
    FILE *fp = fopen(f->path, "wb");
    if (!fp) return UF_CONFIG_ERR_BACKEND;
    fputs("return {\n", fp);
    for (size_t i = 0; i < n; i++)
        fprintf(fp, "  -- %s = %s\n", in[i].path ? in[i].path : "", in[i].encoded ? in[i].encoded : "");
    fputs("}\n", fp);
    fclose(fp);
    f->present = 1;
    return UF_CONFIG_OK;
}

static UfConfigStatus sFileReadOne(void *ctx, const char *path, UfConfigFieldPair *out) {
    UfConfigFieldPair *all = NULL; size_t n = 0; UfConfigMeta m;
    UfConfigStatus st = sFileReadAll(ctx, &all, &n, &m);
    if (st) return st;
    st = UF_CONFIG_ERR_NOFIELD;
    for (size_t i = 0; i < n; i++)
        if (all[i].path && path && strcmp(all[i].path, path) == 0) { *out = all[i]; st = UF_CONFIG_OK; break; }
    free(all);
    return st;
}

static UfConfigStatus sFileWriteOne(void *ctx, const char *path, const UfConfigFieldPair *in) {
    (void)ctx; (void)path; (void)in;
    return UF_CONFIG_ERR_NOT_ATOMIC;
}

static UfConfigStatus sFileStat(void *ctx, UfConfigStamp *out) {
    FileCtx *f = (FileCtx *)ctx;
    FILE *fp = fopen(f->path, "rb");
    if (!fp) return UF_CONFIG_ERR_NOT_CONFIGURED;
    fclose(fp);
    out->token = 1;
    return UF_CONFIG_OK;
}

static void sFileClose(void *ctx) {
    FileCtx *f = (FileCtx *)ctx;
    if (!f) return;
    free(f->path); free(f);
}

const UfConfigDriver ufconfig_driver_file = {
    .caps = {
        .api_version = UF_CONFIG_DRIVER_API_VERSION,
        .kind        = UF_CONFIG_BACKEND_FILE,
        .atomic_write_all = true, .atomic_write_one = false,
        .per_field_read = false, .native_version = false,
        .enumeration = false, .multi_config_txn = false,
        .write_one_creates = false,
        .max_path_bytes = 512, .max_value_bytes = 1u << 20, .max_fields = 65536,
        .name = "file",
    },
    .open = sFileOpen, .read_all = sFileReadAll, .write_all = sFileWriteAll,
    .read_one = sFileReadOne, .write_one = sFileWriteOne,
    .stat = sFileStat, .close = sFileClose,
};
