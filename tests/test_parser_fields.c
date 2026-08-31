#include "eventsource/sse_parser.h"
#include "parser_rec.h"
#include "t.h"

static rec_t r;
static char db[256], ib[16], eb[16]; /* small id buf: capacity 15 chars */

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

  /* id: on_id fires before on_event, event carries the id */
  fresh(&p);
  feed_str(&p, "id: 42\ndata: x\n\n");
  OK_STR(r.log, "id(42,2)\nevent(-,42,1,x)\n");

  /* id-only block still commits the id (no event) */
  fresh(&p);
  feed_str(&p, "id: 7\n\n");
  OK_STR(r.log, "id(7,1)\n");

  /* empty id is valid and reported as "" */
  fresh(&p);
  feed_str(&p, "id\n\n");
  OK_STR(r.log, "id(,0)\n");

  /* id containing NUL is dropped with an error; block has no id */
  fresh(&p);
  sse_parser_feed(&p, "id: 4\x00" "2\ndata: x\n\n", 17);
  OK_STR(r.log, "err(1)\nevent(-,-,1,x)\n"); /* 1 == SSE_PARSE_ERR_ID_INVALID */

  /* id does not persist into the next block (client owns persistence) */
  fresh(&p);
  feed_str(&p, "id: 1\ndata: a\n\ndata: b\n\n");
  OK_STR(r.log, "id(1,1)\nevent(-,1,1,a)\nevent(-,-,1,b)\n");

  /* last id field in a block wins */
  fresh(&p);
  feed_str(&p, "id: 1\nid: 2\ndata: x\n\n");
  OK_STR(r.log, "id(2,1)\nevent(-,2,1,x)\n");

  /* retry with digits fires immediately at line end */
  fresh(&p);
  feed_str(&p, "retry: 10000\n");
  OK_STR(r.log, "retry(10000)\n");

  /* retry with non-digits is an error, not a callback */
  fresh(&p);
  feed_str(&p, "retry: 3s\n");
  OK_STR(r.log, "err(3)\n"); /* 3 == SSE_PARSE_ERR_INVALID_RETRY */

  /* empty retry is invalid */
  fresh(&p);
  feed_str(&p, "retry\n");
  OK_STR(r.log, "err(3)\n");

  /* absurdly large retry clamps to UINT32_MAX */
  fresh(&p);
  feed_str(&p, "retry: 99999999999999999999\n");
  OK_STR(r.log, "retry(4294967295)\n");

  /* field names are case sensitive: "ID" is unknown */
  fresh(&p);
  feed_str(&p, "ID: 9\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n");

  T_END();
}
