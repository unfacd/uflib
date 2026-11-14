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
#include "ufconfig_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct FileCtx {
    char *path;
    int present;
} FileCtx;

static UfConfigOpStatus sFileOpen(void **ctx, const UfConfigBackendSpec *spec) {
    if (!spec || !spec->u.file.path) return UF_CONFIG_ERR_INVALID_ARG;
    FileCtx *f = (FileCtx *)calloc(1, sizeof(*f));
    f->path = strdup(spec->u.file.path);
    FILE *fp = fopen(f->path, "rb");
    f->present = fp != NULL;
    if (fp) fclose(fp);
    *ctx = f;
    return UF_CONFIG_OK;
}

static UfConfigOpStatus sFileReadAll(void *ctx, UfConfigFieldPair **out, size_t *n, UfConfigMeta *meta) {
    FileCtx *f = (FileCtx *)ctx;
    FILE *fp = fopen(f->path, "rb");
    if (!fp) return UF_CONFIG_ERR_NOT_CONFIGURED;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t nr = fread(buf, 1, (size_t)sz, fp); fclose(fp); buf[nr] = 0;
    UfConfigOpStatus st = UfConfigPairsFromLua(buf, nr, out, n);
    free(buf);
    if (meta) { memset(meta, 0, sizeof(*meta)); meta->fmt = 1; meta->version = 1; }
    return st;
}

static UfConfigOpStatus sFileWriteAll(void *ctx, const UfConfigFieldPair *in, size_t n, const UfConfigMeta *meta) {
    (void)meta;
    FileCtx *f = (FileCtx *)ctx;
    char *text = NULL;
    UfConfigOpStatus st = ConfigPairsToLuaStyle(in, n, &text);
    if (st) return st;
    size_t len = strlen(text);

    size_t pcap = strlen(f->path) + 16;
    char * tmp  = (char *)malloc(pcap);
    if (!tmp) {
        free(text);
        return UF_CONFIG_ERR_NO_MEMORY;
    }
    snprintf(tmp, pcap, "%s.tmp.XXXXXX", f->path);

    int fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        free(text);
        return UF_CONFIG_ERR_BACKEND;
    }

    struct stat sb;
    int ok = 1;
    if (stat(f->path, &sb) == 0 && fchmod(fd, sb.st_mode & 07777) != 0) ok = 0;
    size_t off = 0;
    while (ok && off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        ok = 0;
    }
    if (ok && fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    free(text);
    if (!ok) {
        unlink(tmp);
        free(tmp);
        return UF_CONFIG_ERR_BACKEND;
    }
    if (rename(tmp, f->path) != 0) {
        unlink(tmp);
        free(tmp);
        return UF_CONFIG_ERR_BACKEND;
    }
    free(tmp);

    char *dirpath = strdup(f->path);
    if (dirpath) {
        char *slash = strrchr(dirpath, '/');
        if (slash) {
            if (slash == dirpath) slash[1] = 0;
            else *slash = 0;
            int dfd = open(dirpath, O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
        free(dirpath);
    }

    f->present = 1;
    return UF_CONFIG_OK;
}

static UfConfigOpStatus sFileReadOne(void *ctx, const char *path, UfConfigFieldPair *out) {
    UfConfigFieldPair *all = NULL; size_t n = 0; UfConfigMeta m;
    UfConfigOpStatus st = sFileReadAll(ctx, &all, &n, &m);
    if (st) return st;
    st = UF_CONFIG_ERR_NOFIELD;
    for (size_t i = 0; i < n; i++)
        if (all[i].path && path && strcmp(all[i].path, path) == 0) { *out = all[i]; st = UF_CONFIG_OK; break; }
    free(all);
    return st;
}

static UfConfigOpStatus sFileWriteOne(void *ctx, const char *path, const UfConfigFieldPair *in) {
    (void)ctx; (void)path; (void)in;
    return UF_CONFIG_ERR_NOT_ATOMIC;
}

static UfConfigOpStatus sFileStat(void *ctx, UfConfigStamp *out) {
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
        .backend_store        = UF_CONFIG_BACKEND_FILE,
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
