/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef MJSON_H
#define MJSON_H

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <stdbool.h>
#include <stdint.h>
typedef unsigned char char8_t;//AA+
#ifndef MJSON_ENABLE_PRINT
#define MJSON_ENABLE_PRINT 1
#endif

#ifndef MJSON_ENABLE_RPC
#define MJSON_ENABLE_RPC 1
#endif

#ifndef MJSON_ENABLE_BASE64
#define MJSON_ENABLE_BASE64 1
#endif

#ifndef MJSON_ENABLE_MERGE
#define MJSON_ENABLE_MERGE 1
#endif

#ifndef MJSON_ENABLE_PRETTY
#define MJSON_ENABLE_PRETTY 1
#endif

#ifndef MJSON_ENABLE_NEXT
#define MJSON_ENABLE_NEXT 1
#endif

#ifndef MJSON_RPC_LIST_NAME
#define MJSON_RPC_LIST_NAME "rpc.list"
#endif

#ifndef MJSON_DYNBUF_CHUNK
#define MJSON_DYNBUF_CHUNK 256 // Allocation granularity for print_dynamic_buf
#endif

#ifndef MJSON_REALLOC
#define MJSON_REALLOC realloc
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define MJSON_ERROR_INVALID_INPUT (-1)
#define MJSON_ERROR_TOO_DEEP (-2)
#define MJSON_TOK_INVALID 0
#define MJSON_TOK_KEY 1
#define MJSON_TOK_STRING 11
#define MJSON_TOK_NUMBER 12
#define MJSON_TOK_TRUE 13
#define MJSON_TOK_FALSE 14
#define MJSON_TOK_NULL 15
#define MJSON_TOK_ARRAY 91
#define MJSON_TOK_OBJECT 123
#define MJSON_TOK_IS_VALUE(t) ((t) > 10 && (t) < 20)

typedef int (*mjson_cb_t)(int event,
                          const char* buf,
                          int offset,
                          int len,
                          void* fn_data);

#ifndef MJSON_MAX_DEPTH
#define MJSON_MAX_DEPTH 20
#endif

int mjson(const char* buf, int len, mjson_cb_t cb, void* ud);
int mjson_find(const char* buf,
               int len,
               const char* jp,
               const char** tp,
               int* tl);
int32_t mjson_find8(const char8_t* buf,
                    int32_t len,
                    const char8_t* jp,
                    const char8_t** tp,
                    int32_t* tl);
int32_t mjson_find88(const char* buf,
                     int32_t len,
                     const char8_t* jp,
                     const char8_t** tp,
                     int32_t* tl);
int32_t mjson_find88(const char* buf,
                     int32_t len,
                     const char8_t* jp,
                     const char8_t** tp,
                     int32_t* tl);
int mjson_get_number(const char* buf, int len, const char* path, double* v);
int mjson_get_bool(const char* buf, int len, const char* path, int* v);
int mjson_get_string(const char* buf,
                     int len,
                     const char* path,
                     char* to,
                     int n);
int mjson_get_hex(const char* buf,
                  int len,
                  const char* path,
                  char* to,
                  int n);

#if MJSON_ENABLE_NEXT
int mjson_next(const char* buf,
               int len,
               int offset,
               int* key_offset,
               int* key_len,
               int* val_offset,
               int* val_len,
               int* vale_type);
#endif

#if MJSON_ENABLE_BASE64
int mjson_get_base64(const char* buf,
                     int len,
                     const char* path,
                     char* dst,
                     int dst_len);
int mjson_base64_dec(const char* src, int src_len, char* dst, int dst_len);
#endif

#if MJSON_ENABLE_PRINT
typedef int (*mjson_print_fn_t)(const char* buf, int len, void* fn_data);
typedef int (*mjson_vprint_fn_t)(mjson_print_fn_t fn, void*, va_list*);

struct mjson_fixedbuf
{
    char* ptr;
    int size, len;
};

int mjson_printf(mjson_print_fn_t fn, void* fn_data, const char* fmt, ...);
int mjson_vprintf(mjson_print_fn_t fn,
                  void* fn_data,
                  const char* fmt,
                  va_list* ap);
int mjson_print_str(mjson_print_fn_t fn,
                    void* fn_data,
                    const char* buf,
                    int len);
int mjson_print_int(mjson_print_fn_t fn,
                    void* fn_data,
                    int value,
                    int is_signed);
int mjson_print_long(mjson_print_fn_t fn,
                     void* fn_data,
                     long value,
                     int is_signed);
int mjson_print_buf(mjson_print_fn_t fn,
                    void* fn_data,
                    const char* buf,
                    int len);
int mjson_print_dbl(mjson_print_fn_t fn, void* fn_data, double d, int width);

int mjson_print_null(const char* ptr, int len, void* fn_data);
int mjson_print_fixed_buf(const char* ptr, int len, void* fn_data);
int mjson_print_dynamic_buf(const char* ptr, int len, void* fn_data);

int mjson_snprintf(char* buf, size_t len, const char* fmt, ...);
char* mjson_aprintf(const char* fmt, ...);

#if MJSON_ENABLE_PRETTY
int mjson_pretty(const char* s,
                 int n,
                 const char* pad,
                 mjson_print_fn_t fn,
                 void* fn_data);
#endif

#if MJSON_ENABLE_MERGE
int mjson_merge(const char* s,
                int n,
                const char* s2,
                int n2,
                mjson_print_fn_t fn,
                void* fn_data);
#endif

#endif // MJSON_ENABLE_PRINT

#if MJSON_ENABLE_RPC

void jsonrpc_init(mjson_print_fn_t response_cb, void* fn_data);
int mjson_globmatch(const char* s1, int n1, const char* s2, int n2);

struct jsonrpc_request
{
    struct jsonrpc_ctx* ctx;
    const char* frame;   // Points to the whole frame
    int frame_len;       // Frame length
    const char* params;  // Points to the "params" in the request frame
    int params_len;      // Length of the "params"
    const char* id;      // Points to the "id" in the request frame
    int id_len;          // Length of the "id"
    const char* method;  // Points to the "method" in the request frame
    int method_len;      // Length of the "method"
    mjson_print_fn_t fn; // Printer function
    void* fn_data;       // Printer function data
    void* userdata;      // Callback's user data as specified at export time
};

struct jsonrpc_method
{
    const char* method;
    int method_sz;
    void (*cb)(struct jsonrpc_request*);
    struct jsonrpc_method* next;
};

// Main RPC context, stores current request information and a list of
// exported RPC methods.
struct jsonrpc_ctx
{
    struct jsonrpc_method* methods;
    mjson_print_fn_t response_cb;
    void* response_cb_data;
};

// Registers function fn under the given name within the given RPC context
#define jsonrpc_ctx_export(ctx, name, fn)                                      \
  do {                                                                         \
    static struct jsonrpc_method m = { (name), sizeof(name) - 1, (fn), 0 };    \
    m.next = (ctx)->methods;                                                   \
    (ctx)->methods = &m;                                                       \
  } while (0)

void jsonrpc_ctx_init(struct jsonrpc_ctx* ctx,
                      mjson_print_fn_t response_cb,
                      void* response_cb_data);
void jsonrpc_return_error(struct jsonrpc_request* r,
                          int code,
                          const char* message,
                          const char* data_fmt,
                          ...);
void jsonrpc_return_success(struct jsonrpc_request* r,
                            const char* result_fmt,
                            ...);
void jsonrpc_return_success2(struct jsonrpc_request* r, bool add_endl,
                             const char* result_fmt,
                             ...);
void jsonrpc_ctx_process(struct jsonrpc_ctx* ctx,
                         const char* req,
                         int req_sz,
                         mjson_print_fn_t fn,
                         void* fn_data,
                         void* userdata);

extern struct jsonrpc_ctx jsonrpc_default_context;
extern void jsonrpc_list(struct jsonrpc_request* r);

#define jsonrpc_export(name, fn)                                               \
  jsonrpc_ctx_export(&jsonrpc_default_context, (name), (fn))

#define jsonrpc_process(buf, len, fn, fnd, ud)                                 \
  jsonrpc_ctx_process(&jsonrpc_default_context, (buf), (len), (fn), (fnd), (ud))

#define JSONRPC_ERROR_INVALID -32700    /* Invalid JSON was received */
#define JSONRPC_ERROR_NOT_FOUND -32601  /* The method does not exist */
#define JSONRPC_ERROR_BAD_PARAMS -32602 /* Invalid params passed */
#define JSONRPC_ERROR_INTERNAL -32603   /* Internal JSON-RPC error */

#endif // MJSON_ENABLE_RPC
#ifdef __cplusplus
}
#endif
#endif // MJSON_H