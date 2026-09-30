#include "ufcommand_priv.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>


size_t UfCommandHashParts(char *const *tokens, size_t count)
{
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < count; ++i) {
    if (i) {
      h ^= (unsigned char)' ';
      h *= UINT64_C(1099511628211);
    }
    for (const unsigned char *p = (const unsigned char*)tokens[i]; *p; ++p) {
      h ^= *p;
      h *= UINT64_C(1099511628211);
    }
  }
  return (size_t)(h ^ (h >> 32));
}

char *UfCommandStrDup(const char *s)
{
  if (!s) return NULL;
  size_t n = strlen(s);
  if (n == SIZE_MAX) return NULL;
  char *p = malloc(n + 1);
  if (p) memcpy(p, s, n + 1);
  return p;
}

char *UfCommandStrNDup(const char *s, size_t n)
{
  if (!s || n == SIZE_MAX) return NULL;
  char *p = malloc(n + 1);
  if (p) {
    memcpy(p, s, n);
    p[n] = '\0';
  }
  return p;
}





