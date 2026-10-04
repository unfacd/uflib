/**
 * @file lockless_treiber_stack.h
 * @brief Lock-free Treiber stack — multi-producer push, single-consumer drain.
 *
 * Producers push intrusively with a wait-free CAS; one consumer takes the whole
 * list in a single atomic operation. The per-node `is_claimed` flag makes
 * processing exactly-once, and the per-node `refcount` keeps a node alive across
 * the pusher/drainer hand-off. The stack handle is opaque.
 */

#ifndef LOCKLESS_TREIBER_STACK_H
#define LOCKLESS_TREIBER_STACK_H

#include <uflib/uflib_defs.h>
#include <uflib/standard_defs.h>
#include <stdbool.h>

#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack_type.h>
#include <uflib/logger/logger_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Create an empty stack.
 *
 * @return A new handle, or NULL on allocation failure. Release it with
 *         lockless_treiber_stack_destroy() once drained.
 */
PUBLIC_API LocklessTreiberStack *lockless_treiber_stack_create(void);

/*!
 * @brief Create an empty stack that reports its lifecycle through @p logger_ptr.
 *
 * Nodes are never logged: push and steal-all are not log sites.
 *
 * @param[in] logger_ptr  Borrowed, never owned: it must outlive the stack.
 *                        NULL reports nothing.
 * @return A new handle, or NULL on allocation failure.
 */
PUBLIC_API LocklessTreiberStack *lockless_treiber_stack_create_with_logger(UfLogger *logger_ptr);

/*!
 * @brief Release a stack handle.
 *
 * The stack owns no node memory: every pushed node must have been drained first.
 *
 * @param[in,out] stack  Stack to release (NULL is a no-op).
 */
PUBLIC_API void lockless_treiber_stack_destroy(LocklessTreiberStack *stack);

/*!
 * @brief Initialise a node before its first push.
 *
 * Sets `next` to NULL, `is_claimed` to false and `refcount` to 2 (pusher + stack).
 *
 * @param[in,out] node  Node to initialise (NULL is a no-op).
 */
PUBLIC_API void lockless_treiber_stack_node_init(struct LocklessTreiberStackNode *node);

/*!
 * @brief Add one reference to a node.
 *
 * @param[in,out] node  Node to retain (NULL is a no-op).
 */
PUBLIC_API void lockless_treiber_stack_node_retain(struct LocklessTreiberStackNode *node);

/*!
 * @brief Push a node (wait-free, multi-producer).
 *
 * @param[in,out] stack  Stack to push onto.
 * @param[in,out] node   Node to push; must have been node_init'd.
 * @return true if the node was published, false if an argument is NULL or the
 *         stack is closed — the node is then untouched and the caller still
 *         owns it.
 */
PUBLIC_API bool lockless_treiber_stack_push(LocklessTreiberStack *stack, struct LocklessTreiberStackNode *node);

/*!
 * @brief Drain the stack (single consumer).
 *
 * @param[in,out] stack  Stack to drain.
 * @return The drained chain, newest first, linked through `next`; NULL if the
 *         stack is empty, closed or NULL.
 */
PUBLIC_API struct LocklessTreiberStackNode *lockless_treiber_stack_steal_all(LocklessTreiberStack *stack);

/*!
 * @brief Drain the stack and close it against further pushes.
 *
 * @param[in,out] stack  Stack to drain and close.
 * @return The drained chain, newest first; NULL if the stack is empty or NULL,
 *         or if another caller closed it first. Once closed, push returns false
 *         and steal_all returns NULL.
 */
PUBLIC_API struct LocklessTreiberStackNode *lockless_treiber_stack_steal_all_and_close(LocklessTreiberStack *stack);

/*!
 * @brief Claim a node for exactly-once processing.
 *
 * @param[in,out] node  Node to claim.
 * @return true if this caller won the claim, false if it was already claimed.
 */
PUBLIC_API bool lockless_treiber_stack_claim(struct LocklessTreiberStackNode *node);

/*!
 * @brief Release one reference to a node.
 *
 * @param[in,out] node  Node to release.
 * @return true if that was the last reference: the caller then frees the
 *         enclosing struct (the node is embedded by value).
 */
PUBLIC_API bool lockless_treiber_stack_release(struct LocklessTreiberStackNode *node);

#ifdef __cplusplus
}
#endif

#endif /* LOCKLESS_TREIBER_STACK_H */
