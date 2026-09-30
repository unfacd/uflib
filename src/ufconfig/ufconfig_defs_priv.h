/**
 * @file ufconfig_defs_priv.h
 * @brief The ufconfig module's private compile-time constants.
 *
 * Not installed, and not reachable from a consumer: this file lives under
 * @c src/, which is on the library's PRIVATE include path only, and the install
 * rule copies @c include/ and nothing else.
 *
 * Everything here bounds the implementation rather than the format.  None of it
 * is a decision a caller is entitled to make, which is why it is not in the
 * public @ref ufconfig_defs.h — a caller that could raise the parse depth or
 * the file ceiling would be reasoning about this parser rather than about
 * configuration.
 *
 * The one constant a caller does need, the path length, is public: it is part
 * of the document format, so a caller writing a document has to know it.
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

#ifndef UFLIB_UFCONFIG_UFCONFIG_DEFS_PRIV_H
#define UFLIB_UFCONFIG_UFCONFIG_DEFS_PRIV_H

/*!
 * Deepest table nesting the parser accepts.
 *
 * Bounded because the parse of a nested table is recursive.  A document deeper
 * than this is refused outright rather than parsed to the ceiling and
 * truncated, so a truncated tree is never mistaken for a complete one.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_DEPTH  32

/*!
 * Largest document the loader will read, in bytes.
 *
 * A configuration is read into memory whole, so this is what bounds that
 * allocation.  Sixteen mebibytes is far above any configuration this fleet
 * carries and well below anything that would exhaust a server.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_FILE_BYTES  (16u * 1024u * 1024u)

/*!
 * Most elements a single array field may hold.
 *
 * Checked on parse and again on append, so a document cannot grow an array past
 * this and neither can a caller.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_ARRAY_ELEMS  65536

/*!
 * Children a scope is canonicalised out of stack storage, before the collector
 * allocates.
 *
 * The value matches the fixed array this replaced, so a scope of ordinary width
 * canonicalises exactly as it did.  What changed is the ceiling: a scope wider
 * than this now has its children allocated instead of the surplus being dropped
 * from the JSON form and from the digest.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_CANON_STACK_KIDS 256

/*!
 * Longest string value, in bytes.
 *
 * Symmetric with the file ceiling: a value is bounded separately because a
 * single field can be produced by a transform rather than being present in the
 * document, and so is not covered by the file bound.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_STRING_BYTES  (1024u * 1024u)

/*!
 * Deepest alias chain that resolves.
 *
 * Bounded so that a cyclic document fails with a diagnostic rather than
 * recursing until the stack is exhausted.  A cycle is detected explicitly as
 * well; this ceiling is the backstop for a chain that is merely very long.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_ALIAS_DEPTH  16

/*!
 * Most drivers the registry holds.
 *
 * The in-tree drivers are registered automatically, so this leaves ample room
 * for a consumer's own without the registry growing.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_DRIVERS  8

/*!
 * Longest probe sequence the Redis driver will follow before giving up.
 *
 * A bound rather than a retry-forever loop: a pathological store must not be
 * able to stall a reader indefinitely.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_MEM_PROBE_MAX  32

/*!
 * Size of the fixed scratch buffer used to hold an error message.
 *
 * Matches the public @ref UfConfigError member, which is part of the ABI.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_ERROR_MSG_BYTES  512

/*!
 * Secrets file consulted when the descriptor names none.
 *
 * Relative, so it resolves against the process's working directory.  A
 * deployment that cares where its keys come from names a path explicitly; this
 * exists so a developer running a binary out of a build directory does not have
 * to.  A descriptor that names a path which then cannot be read is an error, and
 * does *not* arrive here — see UfConfigDescriptor.config_file_secrets.
 */
#define PRIV_CONFIG_DEFAULT_UFCONFIG_SECRETS_FILE  "ufconfig.secrets"

#endif /* UFLIB_UFCONFIG_UFCONFIG_DEFS_PRIV_H */
