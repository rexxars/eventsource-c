#include "eventsource/sse_parser.h"
#include <stddef.h>
#include <stdint.h>

/* Touch every byte the parser hands out so ASan sees any bad pointer. */
static void on_event(void *ud, const sse_parser_event_t *ev) {
  (void)ud;
  volatile unsigned sink = 0;
  for (size_t i = 0; i < ev->data_len; i++) sink += (unsigned char)ev->data[i];
  if (ev->event) for (const char *c = ev->event; *c; c++) sink += (unsigned char)*c;
  if (ev->id) for (const char *c = ev->id; *c; c++) sink += (unsigned char)*c;
  (void)sink;
}
static void on_id(void *ud, const char *id, size_t len) {
  (void)ud;
  volatile unsigned sink = 0;
  for (size_t i = 0; i < len; i++) sink += (unsigned char)id[i];
  (void)sink;
}
static void on_retry(void *ud, uint32_t ms) { (void)ud; (void)ms; }
static void on_error(void *ud, sse_parse_error_t e) { (void)ud; (void)e; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char db[300], ib[33], eb[17];
  sse_parser_buffers_t bufs = {db, sizeof db, ib, sizeof ib, eb, sizeof eb};
  sse_parser_callbacks_t cb = {NULL, on_event, on_id, on_retry, on_error};
  sse_parser_t p;
  sse_parser_init(&p, &cb, &bufs);

  if (size == 0) return 0;
  size_t step = (size_t)(data[0] % 7) + 1; /* first byte picks the chunking */
  for (size_t i = 1; i < size; i += step) {
    size_t k = size - i < step ? size - i : step;
    sse_parser_feed(&p, data + i, k);
  }
  sse_parser_reset(&p);
  sse_parser_feed(&p, data + 1, size - 1); /* whole-feed pass too */
  return 0;
}
