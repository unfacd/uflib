/* Test stub: the subset of hiredis used by the sample and its mock backend. */
#ifndef TEST_HIREDIS_H
#define TEST_HIREDIS_H
#include <stddef.h>
#define REDIS_REPLY_STRING 1
#define REDIS_REPLY_ARRAY  2
typedef struct redisReply {
  int type;
  long long integer;
  size_t len;
  char *str;
  size_t elements;
  struct redisReply **element;
} redisReply;
void freeReplyObject(void *reply);
#endif
