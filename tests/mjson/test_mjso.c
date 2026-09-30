/*
*# assuming original mjson sources are in ./src/
gcc -I. -Isrc -o test_mjson_ex \
    test_mjson_ex.c mjson_ex.c src/mjson.c -lm

./test_mjson_ex

test_basic OK
test_wildcard OK
test_recursive OK (found 3 prices)
test_filter OK
test_combined OK

All tests passed

 **/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <uflib/mjson/mjson_ex.h>

static int g_count;
static char g_buf[32][128];

static void capture(const char *tok, int toklen, int type, void *ud) {
  (void)type; (void)ud;
  if (g_count < 32) {
    snprintf(g_buf[g_count], sizeof(g_buf[0]), "%.*s", toklen, tok);
    g_count++;
  }
}

static void test_basic(void) {
  const char *j = "{\"a\":1,\"b\":[true,false],\"c\":{\"d\":3.14}}";
  int len = strlen(j);
  const char *tok;
  int toklen;
  double num;
  int b;
  char str[64];

  assert(mjson_find_ex(j, len, "$.a", &tok, &toklen) == MJSON_TOK_NUMBER);
  assert(mjson_get_number_ex(j, len, "$.a", &num) && num == 1.0);

  assert(mjson_get_bool_ex(j, len, "$.b[0]", &b) && b == 1);
  assert(mjson_get_bool_ex(j, len, "$.b[1]", &b) && b == 0);

  assert(mjson_get_number_ex(j, len, "$.c.d", &num) && num > 3.1 && num < 3.2);
  printf("test_basic OK\n");
}

static void test_wildcard(void) {
  const char *j = "{\"arr\":[10,20,30]}";
  g_count = 0;
  int n = mjson_find_all_ex(j, strlen(j), "$.arr[*]", capture, NULL);
  assert(n == 3);
  assert(strcmp(g_buf[0], "10") == 0);
  assert(strcmp(g_buf[1], "20") == 0);
  assert(strcmp(g_buf[2], "30") == 0);
  printf("test_wildcard OK\n");
}

static void test_recursive(void) {
  const char *j =
      "{\"store\":{\"book\":[{\"price\":8.95},{\"price\":12.99}],"
      "\"bicycle\":{\"price\":19.95}}}";
  g_count = 0;
  int n = mjson_find_all_ex(j, strlen(j), "$..price", capture, NULL);
  assert(n == 3);
  printf("test_recursive OK (found %d prices)\n", n);
}

static void test_filter(void) {
  const char *j =
      "{\"books\":["
      "{\"title\":\"A\",\"price\":8.95,\"cat\":\"fiction\"},"
      "{\"title\":\"B\",\"price\":12.99,\"cat\":\"ref\"},"
      "{\"title\":\"C\",\"price\":22.5,\"cat\":\"fiction\"}"
      "]}";

  g_count = 0;
  int n = mjson_find_all_ex(j, strlen(j),
                            "$.books[?(@.price>10)]", capture, NULL);
  assert(n == 2);

  g_count = 0;
  n = mjson_find_all_ex(j, strlen(j),
                        "$.books[?(@.cat=='fiction')]", capture, NULL);
  assert(n == 2);

  double price;
  assert(mjson_get_number_ex(j, strlen(j),
                             "$.books[?(@.title=='A')].price", &price));
  assert(price == 8.95);

  printf("test_filter OK\n");
}

static void test_combined(void) {
  const char *j =
      "{\"store\":{\"book\":["
      "{\"title\":\"A\",\"price\":8.95},"
      "{\"title\":\"B\",\"price\":12.99}"
      "]}}";

  char title[64];
  assert(mjson_get_string_ex(j, strlen(j),
                             "$..book[?(@.price<10)].title",
                             title, sizeof(title)) > 0);
  assert(strcmp(title, "A") == 0);
  printf("test_combined OK\n");
}

int main(void) {
  test_basic();
  test_wildcard();
  test_recursive();
  test_filter();
  test_combined();
  printf("\nAll tests passed\n");
  return 0;
}