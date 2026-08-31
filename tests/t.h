#ifndef T_H
#define T_H
#include <stdio.h>
#include <string.h>

static int t_failures = 0;

#define OK(cond) do { if (!(cond)) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define OK_STR(a, b) do { const char *t_a = (a), *t_b = (b); \
  if (t_a == NULL || strcmp(t_a, t_b) != 0) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
          t_a ? t_a : "(null)", t_b); } } while (0)

#define OK_INT(a, b) do { long t_a = (long)(a), t_b = (long)(b); \
  if (t_a != t_b) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: got %ld want %ld\n", __FILE__, __LINE__, t_a, t_b); } } while (0)

#define T_END() do { if (t_failures) { \
  fprintf(stderr, "%d failure(s)\n", t_failures); return 1; } \
  printf("ok\n"); return 0; } while (0)
#endif
