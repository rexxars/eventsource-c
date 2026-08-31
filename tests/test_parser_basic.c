#include "eventsource/sse_parser.h"
#include "parser_rec.h"
#include "t.h"

static rec_t r;
static char db[256], ib[64], eb[32];

static void feed_str(sse_parser_t *p, const char *s) {
  sse_parser_feed(p, s, strlen(s));
}

static void fresh(sse_parser_t *p) {
  rec_reset(&r);
  sse_parser_buffers_t bufs = {db, sizeof db, ib, sizeof ib, eb, sizeof eb};
  sse_parser_callbacks_t cb = rec_cb(&r);
  sse_parser_init(p, &cb, &bufs);
}

int main(void) {
  sse_parser_t p;

  /* single data line, no space variant */
  fresh(&p);
  feed_str(&p, "data:hello\n\n");
  OK_STR(r.log, "event(-,-,5,hello)\n");

  /* single data line, one leading space is stripped, second kept */
  fresh(&p);
  feed_str(&p, "data:  two spaces\n\n");
  OK_STR(r.log, "event(-,-,11, two spaces)\n");

  /* multi-line data joined with \n */
  fresh(&p);
  feed_str(&p, "data: foo\ndata: bar\n\n");
  OK_STR(r.log, "event(-,-,7,foo\nbar)\n");

  /* event type field */
  fresh(&p);
  feed_str(&p, "event: add\ndata: x\n\n");
  OK_STR(r.log, "event(add,-,1,x)\n");

  /* empty event value resets type to none */
  fresh(&p);
  feed_str(&p, "event: add\nevent:\ndata: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* event type does not leak into the next block */
  fresh(&p);
  feed_str(&p, "event: add\ndata: x\n\ndata: y\n\n");
  OK_STR(r.log, "event(add,-,1,x)\nevent(-,-,1,y)\n");

  /* "data" with no colon dispatches an empty-string event */
  fresh(&p);
  feed_str(&p, "data\n\n");
  OK_STR(r.log, "event(-,-,0,)\n");

  /* "data:" with empty value also dispatches empty string */
  fresh(&p);
  feed_str(&p, "data:\n\n");
  OK_STR(r.log, "event(-,-,0,)\n");

  /* blank lines alone dispatch nothing */
  fresh(&p);
  feed_str(&p, "\n\n\n");
  OK_STR(r.log, "");

  /* event without data dispatches nothing */
  fresh(&p);
  feed_str(&p, "event: add\n\n");
  OK_STR(r.log, "");

  /* comment lines are skipped silently */
  fresh(&p);
  feed_str(&p, ": keepalive\ndata: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* unknown field is reported once and skipped */
  fresh(&p);
  feed_str(&p, "foo: bar\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n"); /* 4 == SSE_PARSE_ERR_UNKNOWN_FIELD */

  /* long unknown field name (>5 chars, no colon yet) is detected early */
  fresh(&p);
  feed_str(&p, "dataxx: y\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n");

  /* no trailing blank line: nothing dispatched */
  fresh(&p);
  feed_str(&p, "data: dangling\n");
  OK_STR(r.log, "");

  T_END();
}
