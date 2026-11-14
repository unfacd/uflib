/**
 * @file resource_router_stress.c
 * @brief Standalone multi-threaded stress and benchmark for ResourceRouter.
 *
 * Usage:
 *   resource_router_stress [--threads N] [--duration S] [--routes N] [--depth D]
 *                          [--bench N] [--quiet]
 *
 * Defaults: 4 threads, 5 seconds, 4096 routes, depth 3.
 *
 * Concurrency contract: registration happens once, before any worker starts, and
 * the registry is frozen for the run. Concurrent matching on a frozen registry is
 * the supported read path — the registry holds no lock, so registration racing a
 * match is a caller error and is deliberately not exercised here.
 *
 * Every match is checked against the route table: a registered path must MATCH
 * with the expected remainder, an unsupported method must return
 * METHOD_NOT_ALLOWED, an unknown path must return NO_MATCH. Any deviation is a
 * correctness error and forces a non-zero exit.
 *
 * --bench N runs a single-threaded latency measurement of N matches on one
 * route and prints ns/op.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include <uflib/resource_router/resource_router.h>

#define DEFAULT_THREADS  4
#define DEFAULT_DURATION 5
#define DEFAULT_ROUTES   4096
#define DEFAULT_DEPTH    3

#define PATH_CAP 128

typedef struct RouteSpec
{
  char     path[PATH_CAP];
  unsigned methods;
  bool     stemmed;
} RouteSpec;

typedef struct WorkerArgs
{
  const ResourceRouteRegistry *registry;
  const RouteSpec             *routes;
  size_t                       n_routes;
  unsigned                     seed;
  int                          id;
  _Atomic bool                *stop;
  _Atomic unsigned long long  *ops;
  _Atomic unsigned long long  *errors;
  _Atomic unsigned long long  *matches;
  _Atomic unsigned long long  *no_matches;
  _Atomic unsigned long long  *method_denied;
} WorkerArgs;

static BufferDescriptor *
sOnGet(ResourceHandlerContext *ctx)
{
  (void)ctx;
  return NULL;
}

static unsigned
sNextRandom(unsigned *state)
{
  unsigned x = *state;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static void
sBuildRoute(RouteSpec *spec, size_t index, int depth)
{
  char   *p   = spec->path;
  size_t  len = (size_t)PATH_CAP;

  int written = snprintf(p, len, "cmd%zu", index);
  if (written < 0 || (size_t)written >= len) {
    spec->path[0] = '\0';
    return;
  }
  p   += written;
  len -= (size_t)written;

  for (int level = 1; level < depth; level++) {
    written = snprintf(p, len, "/lvl%d", level);
    if (written < 0 || (size_t)written >= len) {
      return;
    }
    p   += written;
    len -= (size_t)written;
  }

  switch (index % 3) {
  case 0:  spec->methods = RESOURCE_ROUTE_METHOD_GET; break;
  case 1:  spec->methods = RESOURCE_ROUTE_METHOD_GET | RESOURCE_ROUTE_METHOD_DEL; break;
  default: spec->methods = RESOURCE_ROUTE_METHOD_GET | RESOURCE_ROUTE_METHOD_DEL | RESOURCE_ROUTE_METHOD_PUT; break;
  }
  spec->stemmed = (index % 7 == 0);
}

static void
sRegisterRoutes(ResourceRouteRegistry *registry, const RouteSpec *routes, size_t n_routes)
{
  ResourceRouteHandler handler = { .on_get = sOnGet, .methods = RESOURCE_ROUTE_METHOD_GET };

  for (size_t i = 0; i < n_routes; i++) {
    handler.methods = routes[i].methods;
    if (ResourceRouteRegister(registry, routes[i].path, "stress route", &handler,
                              routes[i].stemmed ? RESOURCE_ROUTE_STEMMED : 0) != 0) {
      fprintf(stderr, "register failed for '%s'\n", routes[i].path);
      exit(2);
    }
  }
}

static void *
sWorker(void *arg)
{
  WorkerArgs *w = (WorkerArgs *)arg;
  unsigned    seed = w->seed;
  unsigned long long ops = 0, errors = 0, matches = 0, no_matches = 0, denied = 0;

  while (!atomic_load_explicit(w->stop, memory_order_relaxed)) {
    for (int burst = 0; burst < 256; burst++) {
      size_t               index  = sNextRandom(&seed) % w->n_routes;
      const RouteSpec     *spec   = &w->routes[index];
      unsigned             method = (sNextRandom(&seed) & 1) ? RESOURCE_ROUTE_METHOD_GET : RESOURCE_ROUTE_METHOD_PUT;
      char                 path[PATH_CAP];
      const char          *remainder = NULL;
      const ResourceRouteHandler *handler = NULL;
      ResourceRouteMatchResult    result;

      if ((sNextRandom(&seed) & 3) == 0 && spec->stemmed) {
        snprintf(path, sizeof(path), "%s/arg%u", spec->path, sNextRandom(&seed) % 97);
      } else {
        snprintf(path, sizeof(path), "%s", spec->path);
      }

      result = ResourceRouteMatch((ResourceRouteRegistry *)w->registry, method, path, &handler, &remainder);

      if (result == RESOURCE_ROUTE_MATCH) {
        matches++;
        if (!(spec->methods & method)) {
          errors++;
        }
        if (spec->stemmed) {
          if (remainder == NULL) {
            errors++;
          }
        } else if (remainder != NULL) {
          errors++;
        }
      } else if (result == RESOURCE_ROUTE_METHOD_NOT_ALLOWED) {
        denied++;
        if (spec->methods & method) {
          errors++;
        }
      } else {
        no_matches++;
        errors++;
      }

      if (handler != NULL && result != RESOURCE_ROUTE_MATCH) {
        errors++;
      }

      ops++;
    }
  }

  atomic_fetch_add_explicit(w->ops, ops, memory_order_relaxed);
  atomic_fetch_add_explicit(w->errors, errors, memory_order_relaxed);
  atomic_fetch_add_explicit(w->matches, matches, memory_order_relaxed);
  atomic_fetch_add_explicit(w->no_matches, no_matches, memory_order_relaxed);
  atomic_fetch_add_explicit(w->method_denied, denied, memory_order_relaxed);

  return NULL;
}

static double
sNowSeconds(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
sBench(const ResourceRouteRegistry *registry, const RouteSpec *route, long iterations)
{
  const ResourceRouteHandler *handler = NULL;
  const char                 *remainder = NULL;
  double                      start;
  double                      elapsed;

  for (int i = 0; i < 1000; i++) {
    (void)ResourceRouteMatch((ResourceRouteRegistry *)registry, RESOURCE_ROUTE_METHOD_GET, route->path, &handler,
                             &remainder);
  }

  start = sNowSeconds();
  for (long i = 0; i < iterations; i++) {
    (void)ResourceRouteMatch((ResourceRouteRegistry *)registry, RESOURCE_ROUTE_METHOD_GET, route->path, &handler,
                             &remainder);
  }
  elapsed = sNowSeconds() - start;

  if (elapsed <= 0.0) {
    fprintf(stderr, "bench timer resolution too coarse\n");
    return -1;
  }

  printf("bench: %ld matches on '%s' in %.6fs -> %.1f ns/op, %.2f M ops/s\n",
         iterations, route->path, elapsed, elapsed * 1e9 / (double)iterations,
         (double)iterations / elapsed / 1e6);
  return 0;
}

int
main(int argc, char **argv)
{
  int    threads  = DEFAULT_THREADS;
  int    duration = DEFAULT_DURATION;
  size_t n_routes = DEFAULT_ROUTES;
  int    depth    = DEFAULT_DEPTH;
  long   bench    = 0;
  bool   quiet    = false;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
      threads = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
      duration = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--routes") && i + 1 < argc) {
      n_routes = (size_t)strtoul(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
      depth = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
      bench = strtol(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--quiet")) {
      quiet = true;
    } else {
      fprintf(stderr, "usage: %s [--threads N] [--duration S] [--routes N] [--depth D] [--bench N] [--quiet]\n",
              argv[0]);
      return 2;
    }
  }

  if (threads < 1 || threads > 1024 || duration < 1 || n_routes < 1 || depth < 1 || depth > 16) {
    fprintf(stderr, "invalid arguments\n");
    return 2;
  }

  ResourceRouteRegistry *registry = ResourceRouteRegistryCreate();
  if (!registry) {
    fprintf(stderr, "registry allocation failed\n");
    return 2;
  }

  RouteSpec *routes = calloc(n_routes, sizeof(*routes));
  if (!routes) {
    fprintf(stderr, "route array allocation failed\n");
    ResourceRouteRegistryDestroy(registry);
    return 2;
  }

  for (size_t i = 0; i < n_routes; i++) {
    sBuildRoute(&routes[i], i, depth);
    if (!routes[i].path[0]) {
      fprintf(stderr, "route path truncated\n");
      free(routes);
      ResourceRouteRegistryDestroy(registry);
      return 2;
    }
  }

  sRegisterRoutes(registry, routes, n_routes);

  if (bench > 0) {
    int rc = sBench(registry, &routes[n_routes / 2], bench);

    free(routes);
    ResourceRouteRegistryDestroy(registry);
    return rc == 0 ? 0 : 1;
  }

  _Atomic bool               stop        = false;
  _Atomic unsigned long long ops         = 0;
  _Atomic unsigned long long errors      = 0;
  _Atomic unsigned long long matches     = 0;
  _Atomic unsigned long long no_matches  = 0;
  _Atomic unsigned long long denied      = 0;
  WorkerArgs                *args        = calloc((size_t)threads, sizeof(*args));
  pthread_t                 *tids        = calloc((size_t)threads, sizeof(*tids));

  if (!args || !tids) {
    fprintf(stderr, "worker allocation failed\n");
    free(args);
    free(tids);
    free(routes);
    ResourceRouteRegistryDestroy(registry);
    return 2;
  }

  for (int t = 0; t < threads; t++) {
    args[t].registry = registry;
    args[t].routes   = routes;
    args[t].n_routes = n_routes;
    args[t].seed     = (unsigned)(t * 2654435761u + 12345u) | 1u;
    args[t].id       = t;
    args[t].stop     = &stop;
    args[t].ops      = &ops;
    args[t].errors   = &errors;
    args[t].matches  = &matches;
    args[t].no_matches = &no_matches;
    args[t].method_denied = &denied;

    if (pthread_create(&tids[t], NULL, sWorker, &args[t]) != 0) {
      fprintf(stderr, "pthread_create failed for worker %d\n", t);
      atomic_store(&stop, true);
      for (int j = 0; j < t; j++) {
        pthread_join(tids[j], NULL);
      }
      free(args);
      free(tids);
      free(routes);
      ResourceRouteRegistryDestroy(registry);
      return 2;
    }
  }

  double started = sNowSeconds();

  struct timespec sleep_for = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
  double deadline = started + (double)duration;

  while (sNowSeconds() < deadline) {
    nanosleep(&sleep_for, NULL);
  }

  atomic_store(&stop, true);

  for (int t = 0; t < threads; t++) {
    pthread_join(tids[t], NULL);
  }

  double elapsed = sNowSeconds() - started;
  double total   = (double)atomic_load(&ops);

  if (!quiet) {
    printf("routes=%zu depth=%d threads=%d duration=%.2fs\n", n_routes, depth, threads, elapsed);
    printf("ops=%llu matches=%llu no_match=%llu method_denied=%llu errors=%llu\n",
           atomic_load(&ops), atomic_load(&matches), atomic_load(&no_matches),
           atomic_load(&denied), atomic_load(&errors));
    printf("throughput: %.2f M ops/s (%.1f ns/op aggregate)\n",
           total / elapsed / 1e6, elapsed * 1e9 / (total > 0 ? total : 1));
  }

  int exit_code = atomic_load(&errors) == 0 ? 0 : 1;

  free(args);
  free(tids);
  free(routes);
  ResourceRouteRegistryDestroy(registry);

  return exit_code;
}
