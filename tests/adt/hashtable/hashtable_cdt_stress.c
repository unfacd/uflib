/**
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

/*
 * Standalone multithreaded correctness harness for the v1 HashTable CDT.
 *
 * It asserts invariants, not observed behaviour:
 *
 *   A  a unique-key table grants exactly one claimant per key, even when every
 *      thread races the same key at the same time
 *   B  a unique-key table never holds two entries for one key
 *   C  on a table with default semantics every key that was inserted and not
 *      removed is findable, and the entry count equals the live key count
 *   D  concurrent growth loses no key and leaves no duplicate container
 *   E  reference callbacks fire once per accepted insert and land on the
 *      container that actually holds the key
 *   F  remove/insert churn from several threads leaves the table consistent
 *
 * Usage: hashtable_cdt_stress [threads] [keys_per_thread] [rounds]
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uflib/adt/adt_hashtable.h"

#define KEY_LEN 48

/* HASH_ITEM_IS_PTR_TYPE means the keyed field must itself be a pointer, so the
 * storage lives beside it and the table dereferences the pointer for hashing. */
typedef struct CdtItem
{
  char        keybuf[KEY_LEN];
  const char *key;
  int         owner;
  int         refs;
} CdtItem;

static int g_threads = 8;
static int g_keys    = 2000;
static int g_rounds  = 3;
static int g_failures;

static void
Fail(const char *invariant, const char *detail)
{
  fprintf(stderr, "FAIL [%s] %s\n", invariant, detail);
  __sync_add_and_fetch(&g_failures, 1);
}

static void *
LookupKey(HashTable *hash, const char *key)
{
  return HashLookup(hash, (void*)key, true);
}

static void
IncrementRefs(RecyclerClientData *data, int count)
{
  CdtItem *item = (CdtItem*)data;
  __sync_add_and_fetch(&item->refs, count);
}

static HashTable *
MakeTable(const char *name, bool unique, HashTable *storage)
{
  HashTable *hash = HashTableLockingInstantiate(storage, (int)offsetof(CdtItem, key), KEY_SIZE_ZERO,
                                                HASH_ITEM_IS_PTR_TYPE, name, HASHTABLE_DEFAULT_EXTRACTOR);
  if (hash && unique) HashTableSetUniqueKeys(hash, true);
  return hash;
}

/* ── A: one winner per key under contention ─────────────────────────────── */

typedef struct ClaimArg
{
  HashTable *hash;
  int        index;
  char       key[KEY_LEN];
  CdtItem *  items;
  int        won;
  int        attempts;
} ClaimArg;

static void *
ClaimWorker(void *arg)
{
  ClaimArg *claim = (ClaimArg*)arg;

  for (int round = 0; round < g_rounds; ++round) {
    CdtItem *mine = &claim->items[round];
    snprintf(mine->keybuf, KEY_LEN, "contended-%d", claim->index);
    mine->key   = mine->keybuf;
    mine->owner = claim->index;
    mine->refs  = 0;

    if (AddToHash(claim->hash, mine) == mine) claim->won++;
    claim->attempts++;
  }

  return NULL;
}

static void
InvariantUniqueClaim(HashTable *storage)
{
  HashTable *hash = MakeTable("cdt_unique_claim", true, storage);
  if (!hash) {
    Fail("A", "table could not be instantiated");
    return;
  }

  if (g_threads > 32) g_threads = 32;

  const int slots = g_threads * g_rounds;

  CdtItem *  items = (CdtItem*)calloc((size_t)slots, sizeof(CdtItem));
  ClaimArg * args  = (ClaimArg*)calloc((size_t)g_threads, sizeof(ClaimArg));
  pthread_t *tids  = (pthread_t*)calloc((size_t)g_threads, sizeof(pthread_t));

  if (!items || !args || !tids) {
    Fail("A", "allocation failure");
    return;
  }

  for (int i = 0; i < g_threads; ++i) {
    args[i].hash     = hash;
    args[i].index    = i % 2; /* every thread targets one of two keys */
    args[i].items    = &items[i * g_rounds];
    args[i].won      = 0;
    args[i].attempts = 0;
    pthread_create(&tids[i], NULL, ClaimWorker, &args[i]);
  }
  for (int i = 0; i < g_threads; ++i) pthread_join(tids[i], NULL);

  long winners = 0;
  for (int i = 0; i < g_threads; ++i) winners += args[i].won;

  if (winners != 2) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected 2 winners for 2 distinct keys, observed %ld", winners);
    Fail("A", detail);
  }

  if (HASHTABLE_ENTRIES(hash) != 2) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected 2 entries, observed %ld", HASHTABLE_ENTRIES(hash));
    Fail("B", detail);
  }

  for (int k = 0; k < 2; ++k) {
    char key[KEY_LEN];
    snprintf(key, KEY_LEN, "contended-%d", k);

    void *holder = LookupKey(hash, key);
    if (!holder) {
      Fail("B", "claimed key is not findable");
      continue;
    }

    bool known = false;
    for (int i = 0; i < slots; ++i) {
      if (holder == &items[i]) known = true;
    }
    if (!known) Fail("B", "lookup returned a container that was never offered");
  }

  free(tids);
  free(args);
  free(items);
  HashTableDestruct(hash, false);
}


/* ── K: the comparator must read the extracted item, not the container ──────
 *
 * The key seam: a table whose callers hold containers (an extractor maps
 * container -> item). Every claimant puts a non-key string at the container
 * offset the key field occupies, so a comparator that reads the container
 * rather than the extracted item compares against that string, sees a
 * different key, and admits a second entry for a key already held.
 *
 * Oracle: exactly one winner per key and exactly one entry per key. The
 * counts are the signal - a wrong-object read is not a crash and leaves the
 * table structurally valid, so only the admission tally distinguishes it.
 */

typedef struct CdtHolder
{
  const char *shadow; /* occupies offsetof(CdtItem, key) inside the container */
  CdtItem     item;
} CdtHolder;

static void *
ExtractCdtHolder(ItemContainer *container)
{
  return &((CdtHolder*)container)->item;
}

static HashTable *
MakeHolderTable(const char *name, HashTable *storage)
{
  HashTable *hash = HashTableLockingInstantiate(storage, (int)offsetof(CdtItem, key), KEY_SIZE_ZERO,
                                                HASH_ITEM_IS_PTR_TYPE, name, ExtractCdtHolder);
  if (hash) HashTableSetUniqueKeys(hash, true);
  return hash;
}

typedef struct HolderClaimArg
{
  HashTable *hash;
  int        index;
  CdtHolder *holders;
  int        won;
  int        attempts;
} HolderClaimArg;

static void *
HolderClaimWorker(void *arg)
{
  HolderClaimArg *claim = (HolderClaimArg*)arg;

  for (int round = 0; round < g_rounds; ++round) {
    CdtHolder *mine = &claim->holders[round];
    CdtItem *  item = &mine->item;

    mine->shadow = "not-a-key";
    snprintf(item->keybuf, KEY_LEN, "contended-%d", claim->index);
    item->key   = item->keybuf;
    item->owner = claim->index;
    item->refs  = 0;

    if (AddToHash(claim->hash, mine) == mine) claim->won++;
    claim->attempts++;
  }

  return NULL;
}

static void
InvariantContainerKeySeam(HashTable *storage)
{
  HashTable *hash = MakeHolderTable("cdt_container_seam", storage);
  if (!hash) {
    Fail("K", "table could not be instantiated");
    return;
  }

  const int slots = g_threads * g_rounds;

  CdtHolder *     holders = (CdtHolder*)calloc((size_t)slots, sizeof(CdtHolder));
  HolderClaimArg *args    = (HolderClaimArg*)calloc((size_t)g_threads, sizeof(HolderClaimArg));
  pthread_t *     tids    = (pthread_t*)calloc((size_t)g_threads, sizeof(pthread_t));

  if (!holders || !args || !tids) {
    Fail("K", "allocation failure");
    free(holders);
    free(args);
    free(tids);
    return;
  }

  for (int i = 0; i < g_threads; ++i) {
    args[i].hash     = hash;
    args[i].index    = i % 2; /* every thread targets one of two keys */
    args[i].holders  = &holders[i * g_rounds];
    args[i].won      = 0;
    args[i].attempts = 0;
    pthread_create(&tids[i], NULL, HolderClaimWorker, &args[i]);
  }
  for (int i = 0; i < g_threads; ++i) pthread_join(tids[i], NULL);

  long winners = 0;
  for (int i = 0; i < g_threads; ++i) winners += args[i].won;

  if (winners != 2) {
    char detail[160];
    snprintf(detail, sizeof detail, "expected 2 winners for 2 distinct keys, observed %ld of %d attempts", winners,
             g_threads * g_rounds);
    Fail("K", detail);
  }

  if (HASHTABLE_ENTRIES(hash) != 2) {
    char detail[160];
    snprintf(detail, sizeof detail, "unique-key table holds %ld entries for 2 keys: the comparator read the container",
             (long)HASHTABLE_ENTRIES(hash));
    Fail("K", detail);
  }

  for (int k = 0; k < 2; ++k) {
    char key[KEY_LEN];
    snprintf(key, KEY_LEN, "contended-%d", k);

    void *resident = LookupKey(hash, key);
    if (!resident) {
      Fail("K", "claimed key is not findable");
      continue;
    }

    bool known = false;
    for (int i = 0; i < slots; ++i) {
      if (resident == &holders[i]) known = true;
    }
    if (!known) Fail("K", "lookup returned a container that was never offered");
  }

  free(tids);
  free(args);
  free(holders);
  HashTableDestruct(hash, false);
}

/* ── C/D: no lost keys, no lost containers, across growth ───────────────── */

typedef struct FillArg
{
  HashTable *hash;
  CdtItem *  items;
  int        count;
  int        failed;
} FillArg;

static void *
FillWorker(void *arg)
{
  FillArg *fill = (FillArg*)arg;

  for (int i = 0; i < fill->count; ++i) {
    if (AddToHash(fill->hash, &fill->items[i]) != &fill->items[i]) fill->failed++;
  }

  return NULL;
}

static void
InvariantNoLostKeys(HashTable *storage, int threads, int keys_per_thread)
{
  HashTable *hash = MakeTable("cdt_no_lost_keys", false, storage);
  if (!hash) {
    Fail("C", "table could not be instantiated");
    return;
  }

  CdtItem *  items = (CdtItem*)calloc((size_t)threads * (size_t)keys_per_thread, sizeof(CdtItem));
  FillArg *  args  = (FillArg*)calloc((size_t)threads, sizeof(FillArg));
  pthread_t *tids  = (pthread_t*)calloc((size_t)threads, sizeof(pthread_t));

  if (!items || !args || !tids) {
    Fail("C", "allocation failure");
    return;
  }

  for (int t = 0; t < threads; ++t) {
    args[t].hash   = hash;
    args[t].items  = &items[t * keys_per_thread];
    args[t].count  = keys_per_thread;
    args[t].failed = 0;

    for (int i = 0; i < keys_per_thread; ++i) {
      CdtItem *item = &args[t].items[i];
      snprintf(item->keybuf, KEY_LEN, "lost-%d-%d", t, i);
      item->key   = item->keybuf;
      item->owner = t;
      item->refs  = 0;
    }

    pthread_create(&tids[t], NULL, FillWorker, &args[t]);
  }
  for (int t = 0; t < threads; ++t) pthread_join(tids[t], NULL);

  long inserts  = (long)threads * keys_per_thread;
  long rejected = 0;
  for (int t = 0; t < threads; ++t) rejected += args[t].failed;

  if (rejected != 0) {
    char detail[128];
    snprintf(detail, sizeof detail, "%ld distinct inserts were rejected", rejected);
    Fail("C", detail);
  }

  if (HASHTABLE_ENTRIES(hash) != inserts) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected %ld entries, observed %ld", inserts, HASHTABLE_ENTRIES(hash));
    Fail("C", detail);
  }

  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < keys_per_thread; ++i) {
      CdtItem *item = &args[t].items[i];
      if (LookupKey(hash, item->key) != item) {
        Fail("D", "an inserted key was lost across growth");
        t = threads;
        break;
      }
    }
  }

  free(tids);
  free(args);
  free(items);
  HashTableDestruct(hash, false);
}

/* ── E: reference accounting ────────────────────────────────────────────── */

typedef struct RefArg
{
  HashTable *hash;
  CdtItem *  items;
  int        count;
} RefArg;

static void *
RefWorker(void *arg)
{
  RefArg *ref = (RefArg*)arg;

  for (int i = 0; i < ref->count; ++i) {
    AddToHashWithReference(ref->hash, &ref->items[i], IncrementRefs);
  }

  return NULL;
}

static void
InvariantReferenceAccounting(HashTable *storage, int threads, int keys_per_thread)
{
  HashTable *hash = MakeTable("cdt_references", true, storage);
  if (!hash) {
    Fail("E", "table could not be instantiated");
    return;
  }

  CdtItem *  items = (CdtItem*)calloc((size_t)threads * (size_t)keys_per_thread, sizeof(CdtItem));
  RefArg *   args  = (RefArg*)calloc((size_t)threads, sizeof(RefArg));
  pthread_t *tids  = (pthread_t*)calloc((size_t)threads, sizeof(pthread_t));

  if (!items || !args || !tids) {
    Fail("E", "allocation failure");
    return;
  }

  /* every thread offers the same key set, so exactly one offer per key may succeed */
  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < keys_per_thread; ++i) {
      CdtItem *item = &items[t * keys_per_thread + i];
      snprintf(item->keybuf, KEY_LEN, "ref-%d", i);
      item->key   = item->keybuf;
      item->owner = t;
      item->refs  = 0;
    }

    args[t].hash  = hash;
    args[t].items = &items[t * keys_per_thread];
    args[t].count = keys_per_thread;
    pthread_create(&tids[t], NULL, RefWorker, &args[t]);
  }
  for (int t = 0; t < threads; ++t) pthread_join(tids[t], NULL);

  for (int i = 0; i < keys_per_thread; ++i) {
    char key[KEY_LEN];
    snprintf(key, KEY_LEN, "ref-%d", i);

    CdtItem *holder = (CdtItem*)LookupKey(hash, key);
    if (!holder) {
      Fail("E", "claimed key is not findable");
      continue;
    }

    if (holder->refs != 1) {
      char detail[128];
      snprintf(detail, sizeof detail, "key %s referenced %d times, expected 1", key, holder->refs);
      Fail("E", detail);
    }
  }

  if (HASHTABLE_ENTRIES(hash) != keys_per_thread) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected %d entries, observed %ld", keys_per_thread, HASHTABLE_ENTRIES(hash));
    Fail("E", detail);
  }

  free(tids);
  free(args);
  free(items);
  HashTableDestruct(hash, false);
}

/* ── F: churn ───────────────────────────────────────────────────────────── */

typedef struct ChurnArg
{
  HashTable *hash;
  CdtItem *  items;
  int        count;
  int        removals;
  int        reinserts;
} ChurnArg;

static void *
ChurnWorker(void *arg)
{
  ChurnArg *churn = (ChurnArg*)arg;

  for (int i = 0; i < churn->count; ++i) {
    if (AddToHash(churn->hash, &churn->items[i]) == &churn->items[i]) {
    }
    (void)LookupKey(churn->hash, churn->items[i].key);
  }

  for (int i = 0; i < churn->count; i += 2) {
    if (RemoveFromHash(churn->hash, &churn->items[i]) == &churn->items[i]) churn->removals++;
  }

  for (int i = 0; i < churn->count; i += 2) {
    if (AddToHash(churn->hash, &churn->items[i]) == &churn->items[i]) churn->reinserts++;
  }

  return NULL;
}

static void
InvariantChurn(HashTable *storage, int threads, int keys_per_thread)
{
  HashTable *hash = MakeTable("cdt_churn", false, storage);
  if (!hash) {
    Fail("F", "table could not be instantiated");
    return;
  }

  CdtItem *  items = (CdtItem*)calloc((size_t)threads * (size_t)keys_per_thread, sizeof(CdtItem));
  ChurnArg * args  = (ChurnArg*)calloc((size_t)threads, sizeof(ChurnArg));
  pthread_t *tids  = (pthread_t*)calloc((size_t)threads, sizeof(pthread_t));

  if (!items || !args || !tids) {
    Fail("F", "allocation failure");
    return;
  }

  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < keys_per_thread; ++i) {
      CdtItem *item = &items[t * keys_per_thread + i];
      snprintf(item->keybuf, KEY_LEN, "churn-%d-%d", t, i);
      item->key   = item->keybuf;
      item->owner = t;
      item->refs  = 0;
    }
  }

  for (int t = 0; t < threads; ++t) {
    args[t].hash      = hash;
    args[t].items     = &items[t * keys_per_thread];
    args[t].count     = keys_per_thread;
    args[t].removals  = 0;
    args[t].reinserts = 0;
    pthread_create(&tids[t], NULL, ChurnWorker, &args[t]);
  }
  for (int t = 0; t < threads; ++t) pthread_join(tids[t], NULL);

  /* every item was inserted once, half removed, half reinserted: all must be present */
  long expected = (long)threads * keys_per_thread;

  if (HASHTABLE_ENTRIES(hash) != expected) {
    char detail[160];
    snprintf(detail, sizeof detail, "entries %ld does not match successful reinserts %ld", HASHTABLE_ENTRIES(hash),
             expected);
    Fail("F", detail);
  }

  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < keys_per_thread; ++i) {
      CdtItem *item = &items[t * keys_per_thread + i];
      if (LookupKey(hash, item->key) != item) {
        Fail("F", "key missing after churn");
        t = threads;
        break;
      }
    }
  }

  free(tids);
  free(args);
  free(items);
  HashTableDestruct(hash, false);
}


/* ── G/H/I: merge behaviour, each run in a guarded child ────────────────── */

typedef int (*GuardedBody)(void);

/* child: 0 = invariant held, non-zero = child detected a violation */
static int
RunGuarded(GuardedBody body, unsigned seconds, const char *name)
{
  pid_t pid = fork();
  if (pid < 0) {
    Fail(name, "could not fork");
    return -1;
  }

  if (pid == 0) {
    alarm(seconds);
    _exit(body());
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    Fail(name, "waitpid failed");
    return -1;
  }

  if (WIFSIGNALED(status)) {
    char detail[160];
    snprintf(detail, sizeof detail, "did not terminate within %us (signal %d)", seconds, WTERMSIG(status));
    Fail(name, detail);
    return -1;
  }

  if (WEXITSTATUS(status) != 0) {
    char detail[160];
    snprintf(detail, sizeof detail, "child exited %d", WEXITSTATUS(status));
    Fail(name, detail);
    return -1;
  }

  return 0;
}

static int
ChildSelfMerge(void)
{
  HashTable storage;
  memset(&storage, 0, sizeof storage);

  HashTable *hash = MakeTable("harness_self_merge", false, &storage);
  if (!hash) return 2;

  CdtItem items[5];
  for (int i = 0; i < 5; ++i) {
    snprintf(items[i].keybuf, KEY_LEN, "self-%d", i);
    items[i].key   = items[i].keybuf;
    items[i].owner = i;
    items[i].refs  = 0;
    if (AddToHash(hash, &items[i]) != &items[i]) return 3;
  }

  MergeHashEntries(hash, hash);

  return (HASHTABLE_ENTRIES(hash) == 5) ? 0 : 4;
}

static int
ChildMergeIntoFullDestination(void)
{
  HashTable dest_storage;
  HashTable src_storage;
  memset(&dest_storage, 0, sizeof dest_storage);
  memset(&src_storage, 0, sizeof src_storage);

  HashTable *dest = MakeTable("harness_merge_full_dest", false, &dest_storage);
  HashTable *src  = MakeTable("harness_merge_full_src", false, &src_storage);
  if (!dest || !src) return 2;

  dest->flag_resizable = 0;
  dest->max_size       = 2;

  CdtItem residents[2];
  CdtItem incoming[2];

  for (int i = 0; i < 2; ++i) {
    snprintf(residents[i].keybuf, KEY_LEN, "dest-%d", i);
    residents[i].key  = residents[i].keybuf;
    residents[i].refs = 0;
    if (AddToHash(dest, &residents[i]) != &residents[i]) return 3;

    snprintf(incoming[i].keybuf, KEY_LEN, "src-%d", i);
    incoming[i].key  = incoming[i].keybuf;
    incoming[i].refs = 0;
    if (AddToHash(src, &incoming[i]) != &incoming[i]) return 3;
  }

  long merged = MergeHashEntries(dest, src);
  if (merged != HASHTABLE_MERGE_REFUSED) return 5;          /* must refuse, and say so */
  if (HASHTABLE_ENTRIES(dest) != 2)       return 6;          /* destination unchanged  */
  if (HASHTABLE_ENTRIES(src) != 2)        return 7;          /* source untouched       */

  for (int i = 0; i < 2; ++i) {
    if (LookupKey(dest, residents[i].key) != &residents[i]) return 8;
    if (LookupKey(dest, incoming[i].key)  != NULL)          return 9;   /* no partial merge */
  }

  return 0;
}

typedef struct AbbaArg
{
  HashTable *a;
  HashTable *b;
} AbbaArg;

static void *
AbbaOne(void *arg)
{
  AbbaArg *abba = (AbbaArg*)arg;
  MergeHashEntries(abba->b, abba->a); /* rdlock a, then wrlock b */
  return NULL;
}

static void *
AbbaTwo(void *arg)
{
  AbbaArg *abba = (AbbaArg*)arg;
  MergeHashEntries(abba->a, abba->b); /* rdlock b, then wrlock a */
  return NULL;
}

static int
ChildOppositeDirectionMerges(void)
{
  HashTable storage_a;
  HashTable storage_b;
  memset(&storage_a, 0, sizeof storage_a);
  memset(&storage_b, 0, sizeof storage_b);

  HashTable *a = MakeTable("harness_abba_a", false, &storage_a);
  HashTable *b = MakeTable("harness_abba_b", false, &storage_b);
  if (!a || !b) return 2;

  CdtItem in_a;
  CdtItem in_b;
  snprintf(in_a.keybuf, KEY_LEN, "abba-a");
  in_a.key  = in_a.keybuf;
  in_a.refs = 0;
  snprintf(in_b.keybuf, KEY_LEN, "abba-b");
  in_b.key  = in_b.keybuf;
  in_b.refs = 0;

  if (AddToHash(a, &in_a) != &in_a) return 3;
  if (AddToHash(b, &in_b) != &in_b) return 3;

  AbbaArg   arg = {a, b};
  pthread_t one, two;

  if (pthread_create(&one, NULL, AbbaOne, &arg) != 0) return 6;
  if (pthread_create(&two, NULL, AbbaTwo, &arg) != 0) return 6;

  pthread_join(one, NULL);
  pthread_join(two, NULL);

  return 0;
}

/* ── J: AddToHashUniquely claim reporting ──────────────────────────────── */

typedef struct UniqueClaimArg
{
  HashTable *hash;
  int        key_index;
  CdtItem *  items;
  int        inserted;
  int        refused;
  int        bad;
} UniqueClaimArg;

static void *
UniqueClaimWorker(void *arg)
{
  UniqueClaimArg *claim = (UniqueClaimArg*)arg;

  for (int round = 0; round < g_rounds; ++round) {
    CdtItem *mine = &claim->items[round];
    snprintf(mine->keybuf, KEY_LEN, "uniquely-%d", claim->key_index);
    mine->key   = mine->keybuf;
    mine->owner = claim->key_index;
    mine->refs  = 0;

    bool  was_inserted = false;
    void *resident     = NULL;
    void *result       = AddToHashUniquely(claim->hash, mine, &was_inserted, &resident);

    if (!result || resident != result) {
      __sync_add_and_fetch(&claim->bad, 1);
      continue;
    }

    if (was_inserted) {
      if (result != mine) {
        __sync_add_and_fetch(&claim->bad, 1);
        continue;
      }
      __sync_add_and_fetch(&claim->inserted, 1);
    }
    else {
      if (resident == mine) {
        __sync_add_and_fetch(&claim->bad, 1);
        continue;
      }
      __sync_add_and_fetch(&claim->refused, 1);
    }
  }

  return NULL;
}

static void
InvariantUniqueInsert(HashTable *storage)
{
  /* a table without unique-key semantics must refuse the contract outright */
  HashTable plain_storage;
  memset(&plain_storage, 0, sizeof plain_storage);

  HashTable *plain = MakeTable("cdt_unique_plain", false, &plain_storage);
  if (!plain) {
    Fail("J", "plain table could not be instantiated");
    return;
  }

  CdtItem strays[2];
  for (int i = 0; i < 2; ++i) {
    snprintf(strays[i].keybuf, KEY_LEN, "plain-%d", i);
    strays[i].key   = strays[i].keybuf;
    strays[i].owner = i;
    strays[i].refs  = 0;
  }

  bool  was_inserted = true;
  void *resident     = (void*)&plain_storage;
  void *result       = AddToHashUniquely(plain, &strays[0], &was_inserted, &resident);

  if (result || was_inserted || resident) Fail("J", "plain table accepted the unique-key contract");
  if (HASHTABLE_ENTRIES(plain) != 0) Fail("J", "the refused insert landed in the plain table");
  if (LookupKey(plain, strays[0].key)) Fail("J", "the refused insert is findable in the plain table");
  HashTableDestruct(plain, false);

  HashTable *hash = MakeTable("cdt_unique_insert", true, storage);
  if (!hash) {
    Fail("J", "unique table could not be instantiated");
    return;
  }

  if (g_threads < 2) {
    Fail("J", "precondition: at least two threads, so both contended keys are offered");
    HashTableDestruct(hash, false);

    return;
  }

  const int threads = g_threads;
  const int slots   = threads * g_rounds;

  CdtItem *       items = (CdtItem*)calloc((size_t)slots, sizeof(CdtItem));
  UniqueClaimArg *args  = (UniqueClaimArg*)calloc((size_t)threads, sizeof(UniqueClaimArg));
  pthread_t *     tids  = (pthread_t*)calloc((size_t)threads, sizeof(pthread_t));

  if (!items || !args || !tids) {
    Fail("J", "allocation failure");
    return;
  }

  for (int t = 0; t < threads; ++t) {
    args[t].hash      = hash;
    args[t].key_index = t % 2;
    args[t].items     = &items[t * g_rounds];
    args[t].inserted  = 0;
    args[t].refused   = 0;
    args[t].bad       = 0;
    pthread_create(&tids[t], NULL, UniqueClaimWorker, &args[t]);
  }
  for (int t = 0; t < threads; ++t) pthread_join(tids[t], NULL);

  int inserted = 0;
  int refused  = 0;
  int bad      = 0;
  for (int t = 0; t < threads; ++t) {
    inserted += args[t].inserted;
    refused  += args[t].refused;
    bad      += args[t].bad;
  }

  if (bad) Fail("J", "a claim report contradicted the resident container it returned");
  if (inserted != 2) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected 2 winners over 2 keys, observed %d", inserted);
    Fail("J", detail);
  }
  if (refused != slots - inserted) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected %d refusals, observed %d", slots - inserted, refused);
    Fail("J", detail);
  }

  if (HASHTABLE_ENTRIES(hash) != 2) {
    char detail[128];
    snprintf(detail, sizeof detail, "expected 2 entries, observed %ld", HASHTABLE_ENTRIES(hash));
    Fail("J", detail);
  }

  for (int k = 0; k < 2; ++k) {
    char key[KEY_LEN];
    snprintf(key, KEY_LEN, "uniquely-%d", k);

    CdtItem *holder = (CdtItem*)LookupKey(hash, key);
    if (!holder) {
      Fail("J", "claimed key is not findable");
      continue;
    }

    int holders = 0;
    for (int i = 0; i < slots; ++i) if (holder == &items[i]) holders++;

    if (holders != 1) {
      Fail("J", "the stored container is not one of the offered instances");
      continue;
    }
    if (holder->owner != k) Fail("J", "the resident container does not carry the key it is filed under");

    /* re-offering the resident changes nothing */
    bool  again          = true;
    void *again_resident = NULL;
    void *again_result   = AddToHashUniquely(hash, holder, &again, &again_resident);

    if (again_result != holder || again_resident != holder || again) {
      Fail("J", "re-offering the resident container reported an insert");
    }
    if (HASHTABLE_ENTRIES(hash) != 2) Fail("J", "re-offering the resident changed the entry count");

    /* releasing the key must let a fresh instance claim it: no dependence on
     * how many offers this key happened to receive */
    if (RemoveFromHash(hash, holder) != holder) {
      Fail("J", "could not release the claimed key");
      continue;
    }

    CdtItem claimant;
    memset(&claimant, 0, sizeof claimant);
    snprintf(claimant.keybuf, KEY_LEN, "uniquely-%d", k);
    claimant.key   = claimant.keybuf;
    claimant.owner = k;

    bool  reclaimed        = false;
    void *reclaim_resident = NULL;
    void *reclaim_result   = AddToHashUniquely(hash, &claimant, &reclaimed, &reclaim_resident);

    if (reclaim_result != &claimant || reclaim_resident != &claimant || !reclaimed) {
      Fail("J", "a released key was not claimable by a fresh instance");
    }

    if (HASHTABLE_ENTRIES(hash) != 2) Fail("J", "the released key did not return to the table");
  }

  free(tids);
  free(args);
  free(items);
  HashTableDestruct(hash, false);
}

int
main(int argc, char **argv)
{
  if (argc > 1) g_threads = atoi(argv[1]);
  if (argc > 2) g_keys = atoi(argv[2]);
  if (argc > 3) g_rounds = atoi(argv[3]);

  if (g_threads < 2) g_threads = 2;
  if (g_threads > 32) g_threads = 32;
  if (g_keys < 1) g_keys = 1;
  if (g_rounds < 1) g_rounds = 1;

  printf("hashtable_cdt_stress: threads=%d keys_per_thread=%d rounds=%d\n", g_threads, g_keys, g_rounds);

  HashTable storage;

  fprintf(stderr, "  [A] unique claim under contention\n");
  fflush(stderr);
  memset(&storage, 0, sizeof storage);
  InvariantUniqueClaim(&storage);

  fprintf(stderr, "  [C/D] concurrent fill and growth\n");
  fflush(stderr);
  memset(&storage, 0, sizeof storage);
  InvariantNoLostKeys(&storage, g_threads, g_keys);

  fprintf(stderr, "  [E] reference accounting\n");
  fflush(stderr);
  memset(&storage, 0, sizeof storage);
  InvariantReferenceAccounting(&storage, g_threads, g_keys);

  fprintf(stderr, "  [F] insert/remove churn\n");
  fflush(stderr);
  memset(&storage, 0, sizeof storage);
  InvariantChurn(&storage, g_threads, g_keys);

  fprintf(stderr, "  [G] self-merge terminates\n");
  fflush(stderr);
  RunGuarded(ChildSelfMerge, 5, "G");

  fprintf(stderr, "  [H] merge into a full fixed destination\n");
  fflush(stderr);
  RunGuarded(ChildMergeIntoFullDestination, 5, "H");

  fprintf(stderr, "  [I] opposite-direction merges\n");
  fflush(stderr);
  RunGuarded(ChildOppositeDirectionMerges, 8, "I");

  fprintf(stderr, "  [J] unique-key claim reporting\n");
  fflush(stderr);
  memset(&storage, 0, sizeof storage);
  InvariantUniqueInsert(&storage);

  fprintf(stderr, "  [K] container/key seam\n");
  fflush(stderr);
  memset(&storage, 0, sizeof storage);
  InvariantContainerKeySeam(&storage);

  if (g_failures) {
    printf("RESULT: FAIL (%d invariant violations)\n", g_failures);
    return 1;
  }

  printf("RESULT: PASS (all invariants held)\n");
  return 0;
}
