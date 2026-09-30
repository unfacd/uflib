/**
 * @file ufoptional_priv.h
 * @brief UfOptional representation — private to src/optional/.
 *
 * Never installed and never included by a consumer: the public header declares
 * `UfOptional` as an incomplete type precisely so that this layout stays free to
 * change without an ABI break.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_OPTIONAL_UFOPTIONAL_PRIV_H
#define UFLIB_OPTIONAL_UFOPTIONAL_PRIV_H

#include <uflib/optional/ufoptional_type.h>

/**
 * @brief One payload, or the absence of one.
 *
 * `destroy` and `context` describe the payload currently held and are cleared
 * together with it, so a container that has been taken from or filtered empty
 * cannot release a payload it no longer owns.
 */
struct UfOptional
{
  bool                      is_present;  ///< Whether value is a payload the container owns.
  void *                    value;       ///< The payload, or NULL when empty.
  UfOptionalDestroyCallback destroy;     ///< Releases value, or NULL for none.
  void *                    context;     ///< Handed to destroy alongside value.
  UfOptionalStatus          last_status; ///< Outcome of the last state-changing call.
};

#endif
