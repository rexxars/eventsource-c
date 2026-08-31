#ifndef PARSER_REC_H
#define PARSER_REC_H
#include "eventsource/sse_parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char log[8192];
  size_t len;
} rec_t;

static void rec_add(rec_t *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(r->log + r->len, sizeof(r->log) - r->len, fmt, ap);
  va_end(ap);
  if (n > 0 && (size_t)n < sizeof(r->log) - r->len) r->len += (size_t)n;
}

static void rec_on_event(void *ud, const sse_parser_event_t *ev) {
  rec_add(ud, "event(%s,%s,%zu,%s)\n", ev->event ? ev->event : "-",
          ev->id ? ev->id : "-", ev->data_len, ev->data);
}
static void rec_on_id(void *ud, const char *id, size_t len) {
  rec_add(ud, "id(%s,%zu)\n", id, len);
}
static void rec_on_retry(void *ud, uint32_t ms) {
  rec_add(ud, "retry(%u)\n", (unsigned)ms);
}
static void rec_on_error(void *ud, sse_parse_error_t e) {
  rec_add(ud, "err(%d)\n", (int)e);
}

static void rec_reset(rec_t *r) { memset(r, 0, sizeof *r); }

static sse_parser_callbacks_t rec_cb(rec_t *r) {
  sse_parser_callbacks_t cb = {r, rec_on_event, rec_on_id, rec_on_retry, rec_on_error};
  return cb;
}
#endif
