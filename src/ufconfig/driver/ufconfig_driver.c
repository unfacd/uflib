/**
 * @file ufconfig_driver.c
 * @brief The driver registry: how a backing store is named, declared and found.
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "ufconfig_defs_priv.h"
#include <string.h>

/* The two opt-in drivers are registered only when their capability is on, and
   the guards below are not decoration: it is taking a driver's address that
   pulls its object out of the archive, so registering one that was never built
   drags its client library onto every consumer's link line.  Both macros come
   from config_uflib.h, and an undefined macro is 0 in an #if, so a translation
   unit that does not include that header registers neither — which is the safe
   direction, since a driver built but not registered is inert while one
   registered but not built is a link error. */
#if UFLIB_CAPABILITY_HIREDIS
extern const UfConfigDriver ufconfig_driver_redis;
#endif

static const UfConfigDriver *g_drv[PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_DRIVERS];
static int g_ndrv;

static void sEnsureBuiltin(void);

PUBLIC_API UfConfigStatus UfConfigRegisterDriver(const UfConfigDriver *drv) {
    if (!drv || !drv->caps.name) return UF_CONFIG_ERR_INVALID_ARG;
    if (drv->caps.api_version != UF_CONFIG_DRIVER_API_VERSION)
        return UF_CONFIG_ERR_UNIMPLEMENTED;
    sEnsureBuiltin();

    /* Re-registering the same driver is a no-op, not an error: a caller that
       registers at start-up and again after a reconfiguration should not fail
       for it. */
    for (int i = 0; i < g_ndrv; i++) {
        if (g_drv[i] == drv) return UF_CONFIG_OK;
    }

    /* A driver for a kind already served replaces it.  This is what lets a
       consumer put a real store in place of a built-in stand-in: without it the
       built-ins would be unreachable-around, and the interface would advertise
       an extensibility it did not have. */
    for (int i = 0; i < g_ndrv; i++) {
        if (g_drv[i]->caps.kind == drv->caps.kind) {
            g_drv[i] = drv;
            return UF_CONFIG_OK;
        }
    }

    if (g_ndrv >= PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_DRIVERS) return UF_CONFIG_ERR_LIMIT_EXCEEDED;
    g_drv[g_ndrv++] = drv;
    return UF_CONFIG_OK;
}

PUBLIC_API const UfConfigDriver *UfConfigDriverFindName(const char *name) {
    sEnsureBuiltin();
    if (!name) return NULL;
    for (int i = 0; i < g_ndrv; i++)
        if (g_drv[i]->caps.name && strcmp(g_drv[i]->caps.name, name) == 0)
            return g_drv[i];
    return NULL;
}

PUBLIC_API const UfConfigDriver *UfConfigDriverFindKind(UfConfigBackendKind kind) {
    sEnsureBuiltin();
    for (int i = 0; i < g_ndrv; i++) {
        if (g_drv[i]->caps.kind == kind) return g_drv[i];
    }
    return NULL;
}

extern const UfConfigDriver ufconfig_driver_file;
extern const UfConfigDriver ufconfig_driver_mem;
extern const UfConfigDriver ufconfig_driver_redis;
extern const UfConfigDriver ufconfig_driver_sql;
/* The declarations above are deliberately unguarded, and the guards below are
   not decoration: it is taking a driver's address that pulls its object out of
   the archive, so an opt-in driver whose *registration* is not guarded drags
   its client library onto every consumer's link line even when it was never
   built in.  An unused extern declaration costs nothing and emits nothing. */

static void sEnsureBuiltin(void) {
    static int once;
    if (once) return;
    once = 1;
    g_drv[g_ndrv++] = &ufconfig_driver_file;
    g_drv[g_ndrv++] = &ufconfig_driver_mem;
#if UFLIB_CAPABILITY_HIREDIS
    g_drv[g_ndrv++] = &ufconfig_driver_redis;
#endif
#if UFLIB_CAPABILITY_SQL
    g_drv[g_ndrv++] = &ufconfig_driver_sql;
#endif
}
