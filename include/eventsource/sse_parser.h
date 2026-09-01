#ifndef SSE_PARSER_H
#define SSE_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A dispatched event. Pointers point into the parser's buffers and are only
 * valid for the duration of the callback. */
typedef struct {
  const char *event; /* event type, NULL when the block had none */
  const char *id;    /* id committed in this block, NULL when none */
  const char *data;  /* NUL-terminated; may contain embedded NULs, use data_len */
  size_t data_len;
} sse_parser_event_t;

typedef enum {
  SSE_PARSE_ERR_DATA_TOO_LARGE,     /* block exceeded data buffer; block discarded */
  SSE_PARSE_ERR_ID_INVALID,         /* id had a NUL byte or exceeded id buffer; field dropped */
  SSE_PARSE_ERR_EVENT_TYPE_TOO_LARGE, /* event type exceeded buffer; field dropped */
  SSE_PARSE_ERR_INVALID_RETRY,      /* retry value not all-ASCII-digits; field dropped */
  SSE_PARSE_ERR_UNKNOWN_FIELD,      /* informational; line skipped */
  SSE_PARSE_ERR_EVENT_TYPE_INVALID  /* event type contained a NUL byte; field dropped */
} sse_parse_error_t;

typedef struct {
  void *userdata;
  void (*on_event)(void *ud, const sse_parser_event_t *ev);
  void (*on_id)(void *ud, const char *id, size_t len); /* block end, before on_event */
  void (*on_retry)(void *ud, uint32_t retry_ms);
  void (*on_error)(void *ud, sse_parse_error_t err);
} sse_parser_callbacks_t;

typedef struct {
  char *data_buf;  size_t data_buf_len;  /* payload capacity = data_buf_len - 1 */
  char *id_buf;    size_t id_buf_len;
  char *event_buf; size_t event_buf_len;
} sse_parser_buffers_t;

/* Struct is declared here so it can live in static storage. All fields are
 * private: read or write nothing directly. */
typedef struct sse_parser {
  sse_parser_callbacks_t cb;
  sse_parser_buffers_t buf;
  uint8_t state;
  uint8_t field;
  bool swallow_lf;
  uint8_t bom_n;
  char name[8];
  uint8_t name_len;
  size_t data_len;
  size_t data_lines;
  bool data_overflow;
  size_t event_len;
  bool has_event;
  bool event_invalid;
  size_t id_len;
  bool has_id;
  bool id_invalid;
  uint32_t retry_acc;
  bool retry_seen_digit;
  bool retry_invalid;
} sse_parser_t;

void sse_parser_init(sse_parser_t *p, const sse_parser_callbacks_t *cb,
                     const sse_parser_buffers_t *buf);
void sse_parser_feed(sse_parser_t *p, const void *chunk, size_t len);
void sse_parser_reset(sse_parser_t *p); /* call between connections */

#ifdef __cplusplus
}
#endif
#endif /* SSE_PARSER_H */
