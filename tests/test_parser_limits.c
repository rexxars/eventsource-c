#include "eventsource/sse_parser.h"
#include "parser_rec.h"
#include "t.h"

static rec_t r;

static void feed_str(sse_parser_t *p, const char *s) {
  sse_parser_feed(p, s, strlen(s));
}

int main(void) {
  sse_parser_t p;
  char db[16], ib[8], eb[8]; /* data capacity 15, id 7, event 7 */
  sse_parser_buffers_t bufs = {db, sizeof db, ib, sizeof ib, eb, sizeof eb};
  sse_parser_callbacks_t cb;

  /* data fits exactly at capacity (15 bytes) */
  rec_reset(&r);
  cb = rec_cb(&r);
  sse_parser_init(&p, &cb, &bufs);
  feed_str(&p, "data: 123456789012345\n\n");
  OK_STR(r.log, "event(-,-,15,123456789012345)\n");

  /* one byte over: block discarded, err(0) once, id NOT committed,
   * following block parses fine */
  rec_reset(&r);
  cb = rec_cb(&r);
  sse_parser_init(&p, &cb, &bufs);
  feed_str(&p, "id: 9\ndata: 1234567890123456\n\ndata: ok\n\n");
  OK_STR(r.log, "err(0)\nevent(-,-,2,ok)\n"); /* 0 == SSE_PARSE_ERR_DATA_TOO_LARGE */

  /* overflow across multiple data lines (joined length exceeds capacity) */
  rec_reset(&r);
  cb = rec_cb(&r);
  sse_parser_init(&p, &cb, &bufs);
  feed_str(&p, "data: 12345678\ndata: 12345678\n\ndata: ok\n\n");
  OK_STR(r.log, "err(0)\nevent(-,-,2,ok)\n");

  /* retry inside an overflowed block is still honored */
  rec_reset(&r);
  cb = rec_cb(&r);
  sse_parser_init(&p, &cb, &bufs);
  feed_str(&p, "data: 1234567890123456\nretry: 500\n\n");
  OK_STR(r.log, "err(0)\nretry(500)\n");

  /* id longer than id buffer (7 chars max): dropped with err(1) */
  rec_reset(&r);
  cb = rec_cb(&r);
  sse_parser_init(&p, &cb, &bufs);
  feed_str(&p, "id: 12345678\ndata: x\n\n");
  OK_STR(r.log, "err(1)\nevent(-,-,1,x)\n");

  /* event type longer than event buffer: dropped with err(2), event
   * dispatches with the default type */
  rec_reset(&r);
  cb = rec_cb(&r);
  sse_parser_init(&p, &cb, &bufs);
  feed_str(&p, "event: 12345678\ndata: x\n\n");
  OK_STR(r.log, "err(2)\nevent(-,-,1,x)\n"); /* 2 == SSE_PARSE_ERR_EVENT_TYPE_TOO_LARGE */

  T_END();
}
