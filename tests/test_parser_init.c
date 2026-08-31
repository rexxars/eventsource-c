#include "eventsource/sse_parser.h"
#include "t.h"

int main(void) {
  char db[64], ib[16], eb[16];
  sse_parser_buffers_t bufs = {db, sizeof db, ib, sizeof ib, eb, sizeof eb};
  sse_parser_callbacks_t cb = {0}; /* all callbacks NULL must be safe */
  sse_parser_t p;

  sse_parser_init(&p, &cb, &bufs);
  /* Feeding with no callbacks set must not crash. */
  sse_parser_feed(&p, "data: hello\n\n", 13);
  sse_parser_feed(&p, "", 0);
  sse_parser_reset(&p);
  sse_parser_feed(&p, "data: again\n\n", 13);
  OK(1);
  T_END();
}
