/**
 * @file lockless_treiber_stack.h
 * @brief Lock-free Treiber stack — multi-producer push, steal-all drain.
 *
 * A Treiber stack is a lock-free LIFO: producers push intrusively (wait-free CAS),
 * and a single consumer drains by atomically exchanging the head to NULL. The
 * per-node `is_claimed` flag makes processing idempotent (exactly-once), and the
 * per-node `refcount` keeps a node alive across the pusher/drainer hand-off.
 *
 * The stack object itself is an opaque handle — its internals are private.
 *
 * @code{.c}
 * LocklessTreiberStack *stack = lockless_treiber_stack_create();
 * if (stack == NULL) { return NULL; }   // handle allocation failure
 *
 * lockless_treiber_stack_push(stack, &node_a);
 * lockless_treiber_stack_push(stack, &node_b);
 *
 * struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all(stack);
 * while (head != NULL) {
 *     struct LocklessTreiberStackNode *next = head->next;
 *     if (lockless_treiber_stack_claim(head)) { process(head); }   // exactly once
 *     if (lockless_treiber_stack_release(head)) { free(head); }
 *     head = next;
 * }
 *
 * lockless_treiber_stack_destroy(stack);
 * @endcode
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
 * Allocates and initialises an opaque stack handle. The caller must release it
 * with lockless_treiber_stack_destroy() after draining any remaining nodes.
 *
 * @return A new stack handle, or NULL on allocation failure.
 */
PUBLIC_API LocklessTreiberStack *lockless_treiber_stack_create(void);

/*!
 * @brief Create an empty stack that reports its lifecycle through @p logger_ptr.
 *
 * Behaves exactly as lockless_treiber_stack_create(), and additionally reports
 * creation, allocation failure and release through the supplied logger.  Nodes
 * are never logged: push and steal-all stay exactly as they were, so a stack
 * built this way has the same concurrency behaviour as one built without a
 * logger.
 *
 * The logger is **borrowed, not owned**.  The stack stores the pointer and never
 * destroys it, so the logger must outlive the stack: release the stack before
 * releasing the logger.  A stack released after its logger would report through
 * a dangling handle.
 *
 * @param[in] logger_ptr  Logger to report through, or NULL to report nothing.
 *                        NULL gives exactly lockless_treiber_stack_create().
 * @return A new stack handle, or NULL on allocation failure.
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 * if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK) { return NULL; }
 *
 * LocklessTreiberStack *stack = lockless_treiber_stack_create_with_logger(log_ptr);
 * if (stack == NULL) { UfLoggerDestroy(log_ptr); return NULL; }
 *
 * lockless_treiber_stack_destroy(stack);   // the stack first —
 * UfLoggerDestroy(log_ptr);                // then the logger it borrowed
 * @endcode
 */
PUBLIC_API LocklessTreiberStack *lockless_treiber_stack_create_with_logger(UfLogger *logger_ptr);

/*!
 * @brief Release a stack handle.
 *
 * Frees the opaque handle. The stack owns no node memory, so the caller must have
 * drained (or otherwise released) all pushed nodes first.
 *
 * @param[in] stack  Stack to release (may be NULL, a no-op).
 */
PUBLIC_API void lockless_treiber_stack_destroy(LocklessTreiberStack *stack);

/*!
 * @brief Initialise a node for use with the stack.
 *
 * Sets the link to NULL, `is_claimed` to false, and `refcount` to 2 (pusher +
 * stack). Call before the first push.
 *
 * @param[in,out] node  Node to initialise (may be NULL, a no-op).
 */
PUBLIC_API void lockless_treiber_stack_node_init(struct LocklessTreiberStackNode *node);

/*!
 * @brief Push a node (wait-free, multi-producer).
 *
 * @param[in]     stack  Stack to push onto (may be NULL, a no-op).
 * @param[in]     node   Node to push (must have been node_init'd).
 */
PUBLIC_API void lockless_treiber_stack_push(LocklessTreiberStack *stack, struct LocklessTreiberStackNode *node);

/*!
 * @brief Atomically drain the stack.
 *
 * Exchanges the head to NULL and returns the previous top. The returned nodes are
 * linked via their `next` field (LIFO order). Single-consumer — only one thread
 * may drain at a time.
 *
 * @param[in,out] stack  Stack to drain.
 * @return The previous top of the stack, or NULL if empty or stack is NULL.
 */
PUBLIC_API struct LocklessTreiberStackNode *lockless_treiber_stack_steal_all(LocklessTreiberStack *stack);

/*!
 * @brief Claim a node for exactly-once processing.
 *
 * Atomically marks the node claimed and reports whether this caller won (and is
 * therefore responsible for processing it).
 *
 * @param[in,out] node  Node to claim.
 * @return true if this caller won the claim, false if already processed.
 */
PUBLIC_API bool lockless_treiber_stack_claim(struct LocklessTreiberStackNode *node);

/*!
 * @brief Release one reference to a node.
 *
 * Decrements the node's refcount and reports whether this was the last reference;
 * the caller then frees the enclosing struct (the node is embedded by value).
 *
 * @param[in,out] node  Node to release.
 * @return true if this was the last reference (caller should free the node).
 */
PUBLIC_API bool lockless_treiber_stack_release(struct LocklessTreiberStackNode *node);

#ifdef __cplusplus
}
#endif

#endif /* LOCKLESS_TREIBER_STACK_H */
