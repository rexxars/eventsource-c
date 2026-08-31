#include "eventsource/sse_parser.h"
#include <string.h>

enum { PS_BOM = 0, PS_LINE_START, PS_FIELD_NAME, PS_AFTER_COLON, PS_VALUE, PS_SKIP_LINE };
enum { F_NONE = 0, F_DATA, F_EVENT, F_ID, F_RETRY };

/* Tasks 2-5 replace this with the real state machine. */
static void process_byte(sse_parser_t *p, uint8_t b) {
  (void)p;
  (void)b;
}

void sse_parser_init(sse_parser_t *p, const sse_parser_callbacks_t *cb,
                     const sse_parser_buffers_t *buf) {
  memset(p, 0, sizeof *p);
  p->cb = *cb;
  p->buf = *buf;
  p->state = PS_BOM;
}

void sse_parser_feed(sse_parser_t *p, const void *chunk, size_t len) {
  const uint8_t *bytes = chunk;
  for (size_t i = 0; i < len; i++) {
    process_byte(p, bytes[i]);
  }
}

void sse_parser_reset(sse_parser_t *p) {
  sse_parser_callbacks_t cb = p->cb;
  sse_parser_buffers_t buf = p->buf;
  sse_parser_init(p, &cb, &buf);
}
