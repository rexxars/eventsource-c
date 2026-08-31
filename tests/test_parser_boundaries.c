#include "eventsource/sse_parser.h"
#include "parser_rec.h"
#include "t.h"

static rec_t r;
static char db[512], ib[64], eb[32];

static void fresh(sse_parser_t *p) {
  rec_reset(&r);
  sse_parser_buffers_t bufs = {db, sizeof db, ib, sizeof ib, eb, sizeof eb};
  sse_parser_callbacks_t cb = rec_cb(&r);
  sse_parser_init(p, &cb, &bufs);
}

static void feed_str(sse_parser_t *p, const char *s) {
  sse_parser_feed(p, s, strlen(s));
}

/* Feed `stream` in chunks of `chunk` bytes and return the log. */
static const char *run_chunked(const char *stream, size_t chunk) {
  sse_parser_t p;
  fresh(&p);
  size_t n = strlen(stream);
  for (size_t i = 0; i < n; i += chunk) {
    size_t k = n - i < chunk ? n - i : chunk;
    sse_parser_feed(&p, stream + i, k);
  }
  return r.log;
}

int main(void) {
  sse_parser_t p;

  /* CRLF terminators */
  fresh(&p);
  feed_str(&p, "data: x\r\n\r\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* bare CR terminators */
  fresh(&p);
  feed_str(&p, "data: x\r\r");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* mixed: CR line, LF line */
  fresh(&p);
  feed_str(&p, "event: a\rdata: x\n\r");
  OK_STR(r.log, "event(a,-,1,x)\n");

  /* CRLF split across two feeds must not create a phantom blank line */
  fresh(&p);
  feed_str(&p, "data: x\r");
  feed_str(&p, "\ndata: y\n\n");
  OK_STR(r.log, "event(-,-,3,x\ny)\n");

  /* CR at end of one feed, next feed starts with a data line (bare CR case) */
  fresh(&p);
  feed_str(&p, "data: x\r");
  feed_str(&p, "\r");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* UTF-8 BOM at stream start is stripped */
  fresh(&p);
  feed_str(&p, "\xEF\xBB\xBF" "data: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* BOM split across feeds */
  fresh(&p);
  feed_str(&p, "\xEF");
  feed_str(&p, "\xBB\xBF" "data: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* Partial BOM prefix that is not a BOM gets replayed as line bytes
   * (becomes an unknown-field line, reported and skipped) */
  fresh(&p);
  feed_str(&p, "\xEF\xBB" "oops\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n");

  /* BOM only valid at stream start: after reset it is stripped again */
  fresh(&p);
  feed_str(&p, "\xEF\xBB\xBF" "data: x\n\n");
  sse_parser_reset(&p);
  rec_reset(&r);
  feed_str(&p, "\xEF\xBB\xBF" "data: y\n\n");
  OK_STR(r.log, "event(-,-,1,y)\n");

  /* Chunk-boundary invariance: a gnarly fixture must produce an identical
   * log fed whole or in chunks of size 1..7. */
  static const char fixture[] =
      "\xEF\xBB\xBF"
      ": warm up\r\n"
      "retry: 2500\n"
      "id: 100\r"
      "event: tick\r\n"
      "data: first\n"
      "data: second\r\n"
      "\r\n"
      "unknown-field: zzz\n"
      "id: 4\x31\n" /* "41" spelled awkwardly to keep bytes obvious */
      "data:no-space\r"
      "\r"
      "data: dangling";
  char expected[sizeof(r.log)];
  {
    const char *whole = run_chunked(fixture, sizeof fixture); /* one chunk */
    strcpy(expected, whole);
    OK(strlen(expected) > 0);
  }
  for (size_t chunk = 1; chunk <= 7; chunk++) {
    const char *got = run_chunked(fixture, chunk);
    if (strcmp(got, expected) != 0) {
      t_failures++;
      fprintf(stderr, "FAIL chunk=%zu:\n--- got ---\n%s--- want ---\n%s", chunk, got, expected);
    }
  }

  T_END();
}
