/**
 * @file utils_ufsrvuid_test_standalone.c
 * @brief utils_ufsrvuid_test_standalone
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 3/09/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

/* ============================================================================
 *
* clang -std=gnu11 -D_GNU_SOURCE -O1 -I include -I src \
    tests/utils/utils_ufsrvuid_test_standalone.c -o /tmp/test_ufsrvuid

/tmp/test_ufsrvuid            # audit mode
/tmp/test_ufsrvuid --strict   # CI gate (any XFAIL -> exit 1)

With sanitizers (exercises the fork-caged OOB/misaligned probes):

clang -std=gnu11 -D_GNU_SOURCE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I include -I src tests/utils/utils_ufsrvuid_test_standalone.c -o /tmp/test_ufsrvuid_asan

/tmp/test_ufsrvuid_asan

 * test_ufsrvuid_adversarial.c — adversarial test suite + executable audit
 * for uflib/ufsrvuid.c (UfsrvUid, Crockford Base32, 128-bit ULID-style ids)
 *
 * BUG index (full write-up in the audit):
 *  BUG-01 CreateFromEncodedText: unbounded 26-byte read, no length arg   CRIT
 *  BUG-02 decode: no alphabet validation -> silent garbage               CRIT
 *  BUG-03 dec[(int)char]: signed index -> OOB read for bytes >= 0x80     CRIT
 *  BUG-04 GetSequenceIdFromEncoded(NULL): strlen(NULL) crash             CRIT
 *  BUG-05 error sentinel 0 == system-user sequence id                    CRIT
 *  BUG-06 *(unsigned long*)&data[] punning: aliasing + alignment UB      HIGH
 *  BUG-07 unsigned long assumed 64-bit (LP64-only)                       HIGH
 *  BUG-08 host-endian field layout vs little-endian canonical strings    HIGH
 *  BUG-09 instance_id >= 2^23 spills into timestamp                      HIGH
 *  BUG-10 timestamp unvalidated: pre-epoch wrap; 2^41 overflow ~2084     HIGH
 *  BUG-11 ConvertSerialise: no NUL on caller path; no capacity arg       HIGH
 *  BUG-12 __attribute__((const)) on pointer-reading functions           MED
 *  BUG-13 system identity mutable; two divergent statics                MED
 *  BUG-14 non-canonical first char aliases mod 8                        MED
 *  BUG-15 unchecked calloc                                               MED
 *  BUG-16 Crockford decode conventions (case, I/L/O, '-') undecided     MED
 *  BUG-17 NULL args crash various APIs                                   LOW
 *  BUG-18 dead ULIDUINT128 block; Encoding not static const              LOW
 *  BUG-19 GetTimestamp returns since-epoch, undocumented                 LOW
 *
 * STATUS (post-fix, 2026-09-03):
 *   FIXED — BUG-01/02/03/04/05/06/07/09/10/11/12/13/14/15/17/18/19
 *   OPEN  — BUG-08 (endianness), BUG-16 (policy)
 *
 * Build (white-box — the suite #includes ufsrvuid.c for access to dec[]):
 *   gcc -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I<dir-containing-uflib/> test_ufsrvuid_adversarial.c -o test_ufsrvuid
 *   UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./test_ufsrvuid
 *   ./test_ufsrvuid --strict        # CI gate once the fixes land
 * Do NOT also link ufsrvuid.o — the .c is compiled in below.
 * (If you see 'macro redefined' for IS_EMPTY/IS_PRESENT, your headers
 *  already provide them: delete the stub block.)
 * Also build once at -O2: the BUG-12 canary only bites under optimization.
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#define HAVE_CAGE 1
#endif

#ifndef IS_EMPTY
#define IS_EMPTY(x) ((const void *)(x) == NULL)
#endif
#ifndef IS_PRESENT
#define IS_PRESENT(x) ((const void *)(x) != NULL)
#endif

/* ---- code under test (white-box) ---------------------------------------- */
#include "ufsrvuid.c"                     /* <== adjust path */

/* ==========================================================================
 * result framework
 * ========================================================================== */
static int n_pass, n_fail, n_xfail, n_xpass, n_advisory;

static void group(const char *name) { printf("\n== %s ==\n", name); }

static void report(int ok, int attributed, const char *bug, const char *what)
{
    if (ok) {
        if (attributed) { n_xpass++; printf("[XPASS %s] %s   (bug looks fixed: update audit/tests)\n", bug, what); }
        else            { n_pass++;  printf("[ ok ] %s\n", what); }
    } else {
        if (attributed) { n_xfail++; printf("[XFAIL %s] %s\n", bug, what); }
        else            { n_fail++;  printf("[FAIL] %s\n", what); }
    }
}
#define CHECK(cond, what)       report(!!(cond), 0, NULL, (what))
#define XCHECK(bug, cond, what) report(!!(cond), 1, (bug), (what))
#define ADVISE(what)            do { n_advisory++; printf("[adv ] %s\n", (what)); } while (0)

#if defined(__SANITIZE_ADDRESS__)
#  define HAVE_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define HAVE_ASAN 1
#  endif
#endif

/* ==========================================================================
 * independent reference codec (deliberately a different algorithm: explicit
 * MSB-first bit walking, so a bit-math bug in EITHER side becomes visible)
 * ========================================================================== */
static const char REF_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int ref_symval(unsigned char c)
{
    for (int i = 0; i < 32; i++) if ((unsigned char)REF_ALPHABET[i] == c) return i;
    return -1;
}

static void ref_encode(const uint8_t in[16], char out[27])
{
    int bit = 0;                                  /* bit 0 = MSB of in[0] */
    for (int j = 0; j < 26; j++) {
        int n = (j == 0) ? 3 : 5;                 /* char 0 carries 3 bits */
        unsigned v = 0;
        for (int i = 0; i < n; i++, bit++)
            v = (v << 1) | ((in[bit >> 3] >> (7 - (bit & 7))) & 1u);
        out[j] = REF_ALPHABET[v];
    }
    out[26] = 0;
}

static int ref_decode(const char *in, uint8_t out[16])  /* 0 ok, -1 bad char, -2 noncanon */
{
    memset(out, 0, 16);
    int bit = 0;
    for (int j = 0; j < 26; j++) {
        int v = ref_symval((unsigned char)in[j]);
        if (v < 0) return -1;
        int n = (j == 0) ? 3 : 5;
        if (j == 0 && v > 7) return -2;           /* encode() can never emit this */
        for (int i = n - 1; i >= 0; i--, bit++)
            if ((v >> i) & 1) out[bit >> 3] |= (uint8_t)(1u << (7 - (bit & 7)));
    }
    return 0;
}

/* field model: word0 = LE64(data[0..7]) = (ts<<23)|inst ; seq = LE64(data[8..15])
 * (this is what the punned reads compute on a little-endian host — and the
 * aliasing-free, endian-safe form the getters should use) */
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static uint64_t ref_seq (const uint8_t d[16]) { return le64(d + 8); }
static uint64_t ref_word0(const uint8_t d[16]) { return le64(d); }

/* deterministic PRNG (xorshift64) — reproducible failures */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17; return rng_state;
}

/* ==========================================================================
 * crash / OOB cage: dangerous probes run in forked children
 * ========================================================================== */
#if defined(HAVE_CAGE)
static int run_caged(void (*fn)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) { fn(); _exit(0); }             /* _exit: skip atexit/LSan */
    int st = 0; waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;   /* 1 = faulted */
}

static void probe_seqfrom_null (void) { UfsrvUidGetSequenceIdFromEncoded(NULL); }
static void probe_create_null   (void) { UfsrvUid u; UfsrvUidCreateFromEncodedText(NULL, &u); }
static void probe_generate_null (void) { UfsrvUid u; UfsrvUidGenerate(NULL, &u); }
static void probe_isequal_null  (void) { UfsrvUid a; memset(&a,0,sizeof a); UfsrvUidIsEqual(NULL, &a); }
static void probe_issys_null    (void) { UfsrvUidIsSystemUser(NULL); }
static void probe_serialise_null(void) { char b[27]; UfsrvUidConvertSerialise(NULL, b); }

/* exactly-3-byte heap buffer: reading str[2..25] is a 24-byte heap over-read */
static void probe_overread_short(void)
{
    UfsrvUid u; char *s = malloc(3);
    if (s) memcpy(s, "AB", 3);
    UfsrvUidCreateFromEncodedText(s, &u);
    free(s);
}
/* 0xFF byte -> (char)0xFF == -1 -> dec[-1]: OOB read of the static table */
static void probe_highbit(void)
{
    UfsrvUid u; char s[27]; memset(s, '0', 26); s[26] = 0; s[5] = (char)0xFF;
    UfsrvUidCreateFromEncodedText(s, &u);
}
/* misaligned UfsrvUid (the documented AS_UFSRVUID() cast pattern) */
static void probe_misaligned(void)
{
    char raw[3 * (int)sizeof(UfsrvUid)];
    UfsrvUid *u = AS_UFSRVUID(raw + 1);
    memset(u, 0, sizeof *u);
    (void)UfsrvUidGetSequenceId(u);
}
#endif /* HAVE_CAGE */

/* ==========================================================================
 * known-answer vectors (hand-verified; the fixed-size enc[] field makes the
 * compiler reject any vector that is not exactly 26 characters)
 * ========================================================================== */
typedef struct {
    char     enc[CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1];
    uint8_t  dec[CONFIG_MAX_UFSRV_ID_SZ];
    uint64_t ts, inst, seq;
    const char *note;
} kat_t;

static const kat_t kats[] = {
  { "00000000000000000000000000",                                  /* 26 */
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    0, 0, 0, "all-zero id" },

  { "01000000000000000000000000",                                  /* 26 */
    { 0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    0, 1, 0, "system user: instance 1, sequence 0" },

  { "7ZZZZZZZZZZZZZZZZZZZZZZZZZ",                                  /* 26 */
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
      0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
    0x1FFFFFFFFFFULL, 0x7FFFFF, 0xFFFFFFFFFFFFFFFFULL,
    "all ones: every field at maximum" },

  { "00020000000000000000000000",                                  /* 26 */
    { 0,0,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    1, 0, 0, "timestamp = 1" },

  { "00000000000000200000000000",                                  /* 26 */
    { 0,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0 },
    0, 0, 1, "sequence = 1" },

  { "00000000000000000000000040",                                  /* 26 */
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x80 },
    0, 0, 0x8000000000000000ULL, "sequence = 2^63" },

  { "7ZZXZG00000000000000000000",                                  /* 26 */
    { 0xFF,0xFF,0x7F,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    0, 0x7FFFFF, 0, "instance id at maximum" },
};

/* ---- 1. platform assumptions -------------------------------------------- */
static void t_platform(void)
{
    group("platform assumptions (BUG-07 / BUG-08)");
    if (sizeof(unsigned long) != 8)
        report(0, 1, "BUG-07", "unsigned long is NOT 64-bit here: field punning is broken");
    else
        ADVISE("LP64 host: BUG-07 (32-bit/LLP64 portability) does not manifest here");
    unsigned short pe = 1;
    if (*(unsigned char *)&pe != 1)
        report(0, 1, "BUG-08", "big-endian host: canonical strings/system-user will not match");
    else
        ADVISE("little-endian host: BUG-08 does not manifest here");
}

/* ---- 2. white-box table invariants ------------------------------------- */
static void t_table(void)
{
    group("alphabet / dec[] table invariants (white-box)");
    int ok = 1, okh = 1;
    for (int c = 0; c < 256; c++) {
        if (dec[c] != 0xFF && dec[c] > 31) ok = 0;
        if (c >= 128 && dec[c] != 0xFF)    okh = 0;
    }
    CHECK(ok,  "dec[] entries are 0xFF or 0..31");
    CHECK(okh, "all bytes >= 0x80 are marked invalid (decoder now validates: BUG-02/03 fixed)");
    ok = 1;
    for (int i = 0; i < 32; i++)
        if (dec[(unsigned char)Encoding[i]] != i) ok = 0;
    CHECK(ok, "dec[] is the exact inverse of Encoding[]");
    CHECK(dec['I'] == 0xFF && dec['L'] == 0xFF &&
          dec['O'] == 0xFF && dec['U'] == 0xFF,
          "Crockford-excluded letters are marked invalid");
    CHECK(dec['0'] == 0 && dec['9'] == 9 && dec['A'] == 10 && dec['Z'] == 31,
          "spot-check digit/letter mappings");
}

/* ---- 3. known-answer vectors ------------------------------------------- */
static void t_kat(void)
{
    group("known-answer vectors: decode, re-encode, field extraction");
    for (size_t i = 0; i < sizeof kats / sizeof kats[0]; i++) {
        const kat_t *k = &kats[i];
        uint8_t rd[16]; UfsrvUid u;
        char s[CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1];
        memset(s, 0, sizeof s);
        printf("  -- %s: %s\n", k->note, k->enc);

        CHECK(ref_decode(k->enc, rd) == 0 && memcmp(rd, k->dec, 16) == 0,
              "independent reference agrees with the vector");
        UfsrvUidCreateFromEncodedText(k->enc, &u);
        CHECK(memcmp(u.data, k->dec, 16) == 0, "decode -> expected 16 bytes");
        UfsrvUidConvertSerialise(&u, s);
        CHECK(strcmp(s, k->enc) == 0, "encode(decode(x)) == x (canonical round-trip)");
        CHECK(ref_seq(k->dec) == k->seq && ref_word0(k->dec) == ((k->ts << 23) | k->inst),
              "reference field model agrees with the vector");
        CHECK((uint64_t)UfsrvUidGetTimestamp(&u) == k->ts, "timestamp field");
        CHECK((uint64_t)UfsrvUidGetInstanceId(&u) == k->inst, "instance field");
        CHECK((uint64_t)UfsrvUidGetSequenceId(&u) == k->seq, "sequence field");
    }
}

/* ---- 4. interop with real ULID strings ---------------------------------- */
static void t_interop(void)
{
    group("interoperability: standard ULID strings (same 26-char Crockford layout)");
    static const char *uls[] = {
        "01ARZ3NDEKTSV4RRFFQ69G5FAV",   /* the canonical ULID spec sample   */
        "01020CA9CT9G86G08000000000",   /* the example in your type header  */
    };
    for (size_t i = 0; i < sizeof uls / sizeof uls[0]; i++) {
        uint8_t rd[16]; UfsrvUid u;
        char re[CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1];
        memset(re, 0, sizeof re);
        CHECK(strlen(uls[i]) == 26 && ref_decode(uls[i], rd) == 0,
              "valid alphabet, canonical first char");
        UfsrvUidCreateFromEncodedText(uls[i], &u);
        CHECK(memcmp(u.data, rd, 16) == 0, "decode matches independent reference");
        UfsrvUidConvertSerialise(&u, re);
        CHECK(strcmp(re, uls[i]) == 0, "re-encoded identically");
    }
}

/* ---- 5. system user identity ------------------------------------------- */
static void t_systemuser(void)
{
    group("system user identity");
    CHECK(sizeof(UFSRV_SYSTEMUSER_UID) - 1 == CONFIG_MAX_UFSRV_ID_ENCODED_SZ,
          "UFSRV_SYSTEMUSER_UID constant is 26 chars");
    CHECK(memcmp(UfsrvUidRawSystemUser()->data, UfsrvUidRawDataSystemUser(), 16) == 0,
          "raw struct and raw array agree");
    CHECK(UfsrvUidIsSystemUser(UfsrvUidRawSystemUser()), "IsSystemUser(RawSystemUser())");
    { UfsrvUid u; UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, &u);
      CHECK(UfsrvUidIsSystemUser(&u), "decoded constant is the system user"); }
    CHECK((uint64_t)UfsrvUidGetInstanceId(UfsrvUidRawSystemUser()) == 1, "instance id == 1");
    CHECK((uint64_t)UfsrvUidGetSequenceId(UfsrvUidRawSystemUser()) == 0, "sequence id == 0");
    { UfsrvUidGeneratorDescriptor d; UfsrvUid u;
      memset(&d, 0, sizeof d);
      d.instance_id = 1; d.timestamp = (long long)CUSTOM_EPOCH_IN_MILLIS; d.uid = 0;
      UfsrvUidGenerate(&d, &u);
      CHECK(UfsrvUidIsSystemUser(&u), "generate(epoch, instance 1, seq 0) == system user"); }
    { UfsrvUid a, b;
      UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, &a);
      UfsrvUidCopy(&a, &b);
      CHECK(UfsrvUidIsEqual(&a, &b), "Copy + IsEqual round-trip"); }
    { UfsrvUid *p = UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, NULL);
      CHECK(p && UfsrvUidIsSystemUser(p), "decode allocates when out == NULL");
      free(p); }
    { UfsrvUidGeneratorDescriptor d; UfsrvUid *g;
      memset(&d, 0, sizeof d);
      d.instance_id = 1; d.timestamp = (long long)CUSTOM_EPOCH_IN_MILLIS; d.uid = 0;
      g = UfsrvUidGenerate(&d, NULL);
      CHECK(g && UfsrvUidIsSystemUser(g), "generate allocates when out == NULL");
      free(g); }
}

/* ---- 6. randomised round-trips ----------------------------------------- */
static void t_fuzz_bytes(void)
{
    group("randomised round-trip: bytes -> string -> bytes (20k)");
    enum { N = 20000 };
    int bad = 0;
    for (int i = 0; i < N; i++) {
        uint8_t blob[16], back[16]; UfsrvUid u, v;
        char s_t[27], s_r[27];
        for (int b = 0; b < 16; b++) blob[b] = (uint8_t)rng();
        memcpy(u.data, blob, 16);
        UfsrvUidConvertSerialise(&u, s_t); s_t[26] = 0;   /* serialise itself does not: BUG-11 */
        ref_encode(blob, s_r);
        if (strcmp(s_t, s_r) != 0) bad++;                 /* their codec vs reference */
        if (ref_decode(s_t, back) != 0 || memcmp(back, blob, 16) != 0) bad++;
        UfsrvUidCreateFromEncodedText(s_t, &v);
        if (memcmp(v.data, blob, 16) != 0) bad++;
    }
    CHECK(bad == 0, "codec == reference, round-trip identity over 20k random 128-bit ids");
}

static void t_fuzz_strings(void)
{
    group("randomised round-trip: valid string -> bytes -> string (20k)");
    enum { N = 20000 };
    int bad = 0;
    for (int i = 0; i < N; i++) {
        uint8_t rd[16]; UfsrvUid u;
        char s[27], re[27];
        s[0] = REF_ALPHABET[rng() & 7];                   /* canonical first char */
        for (int j = 1; j < 26; j++) s[j] = REF_ALPHABET[rng() & 31];
        s[26] = 0;
        if (ref_decode(s, rd) != 0) { bad++; continue; }
        UfsrvUidCreateFromEncodedText(s, &u);
        if (memcmp(u.data, rd, 16) != 0) bad++;
        memset(re, 0, sizeof re);
        UfsrvUidConvertSerialise(&u, re);
        if (strcmp(re, s) != 0) bad++;
    }
    CHECK(bad == 0, "decode matches reference; re-encode identical over 20k random strings");
}

static void t_fuzz_fields(void)
{
    group("randomised field round-trip: (ts, instance, seq) -> uid -> fields (20k)");
    enum { N = 20000 };
    int bad = 0;
    for (int i = 0; i < N; i++) {
        uint64_t ts   = rng() & 0x1FFFFFFFFFFULL;         /* 41 bits */
        uint64_t inst = rng() & 0x7FFFFFULL;              /* 23 bits */
        uint64_t seq  = rng();                            /* 64 bits */
        UfsrvUidGeneratorDescriptor d; UfsrvUid u, v;
        char s[27];
        memset(&d, 0, sizeof d);
        d.instance_id = (unsigned int)inst;
        d.timestamp   = (long long)CUSTOM_EPOCH_IN_MILLIS + (long long)ts;
        d.uid         = (unsigned long)seq;
        UfsrvUidGenerate(&d, &u);
        if ((uint64_t)UfsrvUidGetTimestamp(&u)  != ts ||
            (uint64_t)UfsrvUidGetInstanceId(&u) != inst ||
            (uint64_t)UfsrvUidGetSequenceId(&u) != seq) { bad++; continue; }
        uint64_t w0 = (ts << 23) | inst;                  /* golden byte model */
        uint8_t expect[16];
        for (int b = 0; b < 8; b++) expect[b]     = (uint8_t)(w0  >> (8 * b));
        for (int b = 0; b < 8; b++) expect[8 + b] = (uint8_t)(seq >> (8 * b));
        if (memcmp(u.data, expect, 16) != 0) bad++;
        memset(s, 0, sizeof s);
        UfsrvUidConvertSerialise(&u, s);
        UfsrvUidCreateFromEncodedText(s, &v);
        if ((uint64_t)UfsrvUidGetSequenceId(&v) != seq) bad++;
    }
    CHECK(bad == 0, "generate/getters/serialise/deserialise survive 20k random field triples");
}

/* ---- 7. length gate (the one validation that works today) --------------- */
static void t_length(void)
{
    group("length gate (returns ULONG_MAX on rejection)");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("") == ULONG_MAX,                       "empty rejected");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("0100000000000000000000000") == ULONG_MAX,  "25 chars rejected");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("010000000000000000000000000") == ULONG_MAX, "27 chars rejected");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("01000000000000000000000000\n") == ULONG_MAX,
          "trailing newline (27) rejected");
}

/* ---- 8. invalid characters --------------------------------------------- */
static void t_invalid_chars(void)
{
    group("invalid characters (BUG-02 fixed; BUG-16 policy still open)");
    static const char bad[] = {
        'l','L','I','i','O','o','U','u',          /* Crockford-excluded + case  */
        'a','z',                                  /* valid letters, wrong case  */
        '-','.',' ','!','#','/','+','\t','\n'     /* punctuation / whitespace   */
    };
    char s[27];
    strcpy(s, "0100000000000000000000000");       /* 25 chars; we append 1 more */
    unsigned n_rejected = 0;
    for (size_t i = 0; i < sizeof bad; i++) {
        s[25] = bad[i]; s[26] = 0;
        unsigned long long got = UfsrvUidGetSequenceIdFromEncoded(s);
        printf("     final char 0x%02x -> sequence id 0x%016llx%s\n",
               (unsigned char)bad[i], got, got == ULONG_MAX ? "   <- rejected" : "");
        if (got == ULONG_MAX) n_rejected++;
    }
    CHECK(n_rejected == sizeof bad,
          "invalid characters rejected (return ULONG_MAX)");
    ADVISE("BUG-16: policy is now strict-reject; case-fold / I/L->1 / O->0 / '-' skip still undecided");

    s[25] = 'l';
    CHECK(UfsrvUidGetSequenceIdFromEncoded(s) == ULONG_MAX,
          "invalid final char rejected -> ULONG_MAX");

    { UfsrvUid u = {0};
      CHECK(UfsrvUidCreateFromEncodedText("010000000000l000000000000", &u) == NULL,
            "typo at position 13 rejected, output untouched"); }

    { UfsrvUid u = {0};
      CHECK(UfsrvUidCreateFromEncodedText("010 000000000000000000000", &u) == NULL,
            "interior space rejected"); }

    /* embedded NUL: the length gate rejects it; the direct API now validates too */
    { UfsrvUid u = {0}; char n[27];
      memcpy(n, UFSRV_SYSTEMUSER_UID, 26); n[5] = 0; n[26] = 0;
      CHECK(UfsrvUidGetSequenceIdFromEncoded(n) == ULONG_MAX,
            "embedded NUL: length gate rejects");
      CHECK(UfsrvUidCreateFromEncodedText(n, &u) == NULL,
            "embedded NUL: direct decode rejects (BUG-01/02 fixed)"); }
}

/* ---- 9. non-canonical first character ---------------------------------- */
static void t_noncanon(void)
{
    group("non-canonical first character rejected (BUG-14 fixed)");
    UfsrvUid u = {0};
    CHECK(UfsrvUidCreateFromEncodedText("80000000000000000000000000", &u) == NULL,
          "'8...' (first char > 7) rejected");
    CHECK(UfsrvUidCreateFromEncodedText("90000000000000000000000000", &u) == NULL,
          "'9...' rejected");
    CHECK(UfsrvUidCreateFromEncodedText("Z0000000000000000000000000", &u) == NULL,
          "'Z...' rejected");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("80000000000000000000000000") == ULONG_MAX,
          "non-canonical -> ULONG_MAX (rejected)");
    UfsrvUid v = {0};
    CHECK(UfsrvUidCreateFromEncodedText("70000000000000000000000000", &v) != NULL,
          "'7...' (canonical max leading char) still accepted");
}

/* ---- 10. field overflow on generation ---------------------------------- */
static void t_field_overflow(void)
{
    group("field clamping on generation (BUG-09 / BUG-10 fixed)");
    UfsrvUidGeneratorDescriptor d; UfsrvUid u;

    memset(&d, 0, sizeof d);                       /* instance overflow: */
    d.instance_id = 0x800000;                      /* 2^23: one bit too many */
    d.timestamp = (long long)CUSTOM_EPOCH_IN_MILLIS;
    UfsrvUidGenerate(&d, &u);
    CHECK((uint64_t)UfsrvUidGetTimestamp(&u) == 0,
          "instance overflow is masked: timestamp stays 0");
    CHECK((uint64_t)UfsrvUidGetInstanceId(&u) == 0,
          "instance 0x800000 masked to 0");

    memset(&d, 0, sizeof d);
    d.instance_id = 0xFFFFFF;
    d.timestamp = (long long)CUSTOM_EPOCH_IN_MILLIS;
    UfsrvUidGenerate(&d, &u);
    CHECK((uint64_t)UfsrvUidGetInstanceId(&u) == 0x7FFFFF &&
          (uint64_t)UfsrvUidGetTimestamp(&u) == 0,
          "instance 0xFFFFFF masked to 0x7FFFFF, timestamp untouched");

    memset(&d, 0, sizeof d);                       /* pre-epoch wall clock */
    d.timestamp = 0;
    UfsrvUidGenerate(&d, &u);
    CHECK((uint64_t)UfsrvUidGetTimestamp(&u) == 0,
          "pre-epoch timestamp clamped to 0 (no wrap)");

    memset(&d, 0, sizeof d);                       /* 41-bit boundary */
    d.timestamp = (long long)CUSTOM_EPOCH_IN_MILLIS + 2199023255551LL;  /* 2^41 - 1 */
    UfsrvUidGenerate(&d, &u);
    CHECK((uint64_t)UfsrvUidGetTimestamp(&u) == 2199023255551ULL,
          "ts = epoch + 2^41 - 1 round-trips (last valid value)");

    d.timestamp = (long long)CUSTOM_EPOCH_IN_MILLIS + 2199023255552LL;  /* 2^41 */
    UfsrvUidGenerate(&d, &u);
    CHECK((uint64_t)UfsrvUidGetTimestamp(&u) == 2199023255551ULL,
          "epoch + 2^41 clamped to 2^41 - 1 (no silent overflow)");
}

/* ---- 11. serialisation buffer contract ---------------------------------- */
static void t_serialise(void)
{
    group("serialisation always NUL-terminates (BUG-11 fixed)");
    UfsrvUid u; memset(&u, 0, sizeof u);
    char buf[CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1];
    memset(buf, 0xAA, sizeof buf);
    UfsrvUidConvertSerialise(&u, buf);
    CHECK(memcmp(buf, "00000000000000000000000000", 26) == 0, "26 chars written");
    CHECK(buf[26] == 0, "caller buffer is NUL-terminated (idiomatic C contract)");
    char *heap = UfsrvUidConvertSerialise(&u, NULL);
    CHECK(heap && strlen(heap) == 26 && heap[26] == 0,
          "heap path is NUL-terminated");
    free(heap);
    ADVISE("BUG-11: fixed — caller must pass a 27-byte buffer (26 chars + NUL)");
}

/* ---- 12. error signalling ----------------------------------------------- */
static void t_errors(void)
{
    group("error signalling (BUG-05 fixed: ULONG_MAX sentinel)");
    CHECK(UfsrvUidGetSequenceIdFromEncoded(UFSRV_SYSTEMUSER_UID) == 0,
          "system user: sequence id 0 is a VALID answer");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("") == ULONG_MAX,
          "empty string: ULONG_MAX is the ERROR answer -- now distinguishable");
    CHECK(UfsrvUidGetSequenceIdFromEncoded("0100000000000000000000000l") == ULONG_MAX,
          "invalid input -> ULONG_MAX");
    CHECK(UfsrvUidGetSequenceIdFromEncoded(UFSRV_SYSTEMUSER_UID) != ULONG_MAX,
          "system user (0) is distinct from error (ULONG_MAX)");
    ADVISE("BUG-15: fixed — calloc return values are now checked in Generate/Serialise/CreateFromEncoded");
}

/* ---- 13. global mutability ---------------------------------------------- */
static void t_mutability(void)
{
    group("system identity is read-only (BUG-13 fixed)");
    /* Compile-time lock: the return types are const-qualified, so a mutating
     * caller fails to compile. (The pre-fix version returned a mutable pointer
     * and this group mutated the shared static; that no longer compiles.) */
    _Static_assert(__builtin_types_compatible_p(const UfsrvUid *,
                    __typeof__(UfsrvUidRawSystemUser())),
                   "UfsrvUidRawSystemUser() must return const UfsrvUid *");
    _Static_assert(__builtin_types_compatible_p(const uint8_t *,
                    __typeof__(UfsrvUidRawDataSystemUser())),
                   "UfsrvUidRawDataSystemUser() must return const uint8_t *");

    const UfsrvUid *su = UfsrvUidRawSystemUser();
    CHECK(su && UfsrvUidIsSystemUser(su), "system user intact");
    const uint8_t *raw = UfsrvUidRawDataSystemUser();
    CHECK(raw && raw[0] == 0x01 && raw[15] == 0x00, "raw system bytes intact");
    CHECK(memcmp(su->data, raw, 16) == 0, "raw struct and raw array agree");
}

/* ---- 14. crash / OOB cage ---------------------------------------------- */
static void t_cage(void)
{
    group("crash / out-of-bounds probes (fork-caged)");
#if defined(HAVE_CAGE)
    CHECK(run_caged(probe_seqfrom_null)  == 0, "GetSequenceIdFromEncoded(NULL) does not crash");
    CHECK(run_caged(probe_create_null)   == 0, "CreateFromEncodedText(NULL) does not crash");
    CHECK(run_caged(probe_generate_null) == 0, "UfsrvUidGenerate(NULL, ...) does not crash");
    CHECK(run_caged(probe_isequal_null)  == 0, "UfsrvUidIsEqual(NULL, ...) does not crash");
    CHECK(run_caged(probe_issys_null)    == 0, "UfsrvUidIsSystemUser(NULL) does not crash");
    CHECK(run_caged(probe_serialise_null)== 0, "ConvertSerialise(NULL uid) does not crash");
#  if defined(HAVE_ASAN)
    CHECK(run_caged(probe_overread_short) == 0,
          "short string: no 24-byte heap over-read (BUG-01 fixed)");
    CHECK(run_caged(probe_highbit) == 0,
          "byte >= 0x80: no dec[-1] OOB table read (BUG-03 fixed)");
#  else
    ADVISE("memory-safety probes skipped: rebuild with -fsanitize=address to exercise them");
#  endif
    CHECK(run_caged(probe_misaligned) == 0,
          "misaligned UfsrvUid access does not fault (memcpy accessors, BUG-06 fixed)");
#else
    ADVISE("cage tests need fork(); skipped on this platform");
#endif
}

/* ---- 15. const-attribute CSE canary ------------------------------------ */
static void t_const_canary(void)
{
    group("__attribute__((const)) removed (BUG-12 fixed)");
    UfsrvUid *u = calloc(1, sizeof *u);            /* heap: forces real loads */
    if (!u) { ADVISE("canary skipped (OOM)"); return; }
    uint64_t s1 = UfsrvUidGetSequenceId(u);
    uint64_t seq = 0xDEADBEEFCAFEBABEULL;
    memcpy(u->data + 8, &seq, 8);                  /* mutate via another access path */
    uint64_t s2 = UfsrvUidGetSequenceId(u);
    CHECK(s1 != s2,
          "second read observes the mutation (no const-attribute CSE)");
    free(u);
}

int main(int argc, char **argv)
{
    int strict = (argc > 1 && strcmp(argv[1], "--strict") == 0);
    printf("ufsrvuid adversarial suite (mode: %s)\n"
           "  build with: -fsanitize=address,undefined  run with UBSAN_OPTIONS=halt_on_error=1\n",
           strict ? "STRICT" : "audit");

    t_platform();
    t_table();
    t_kat();
    t_interop();
    t_systemuser();
    t_fuzz_bytes();
    t_fuzz_strings();
    t_fuzz_fields();
    t_length();
    t_invalid_chars();
    t_noncanon();
    t_field_overflow();
    t_serialise();
    t_errors();
    t_mutability();
    t_cage();
    t_const_canary();

    printf("\n==== summary ====\n");
    printf("contract checks passed : %d\n", n_pass);
    printf("contract checks FAILED : %d   (suite bug or unattributed behaviour)\n", n_fail);
    printf("documented bugs (XFAIL): %d   (confirmed present)\n", n_xfail);
    printf("bugs apparently fixed  : %d   (XPASS: update audit + flip pins)\n", n_xpass);
    printf("advisories             : %d\n", n_advisory);
    printf("strict mode            : %s\n", strict ? "XFAILs are failures" : "XFAILs are reports");
    return (n_fail != 0 || (strict && n_xfail != 0)) ? 1 : 0;
}