/* Test stub: the thread-context fields the sample's source blocks touch. */
#ifndef TEST_THREAD_CONTEXT_TYPE_H
#define TEST_THREAD_CONTEXT_TYPE_H
typedef struct ThreadContext {
  int ok;
  void *userdata;
} ThreadContext;
extern __thread ThreadContext ufsrv_thread_context;
#define THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA (ufsrv_thread_context.ok)
#define THREAD_CONTEXT_UFSRV_RESULT_USERDATA (ufsrv_thread_context.userdata)
#endif
