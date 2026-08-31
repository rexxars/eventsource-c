#include "eventsource/sse_parser.h"
#include <string.h>

enum { PS_BOM = 0, PS_LINE_START, PS_FIELD_NAME, PS_AFTER_COLON, PS_VALUE, PS_SKIP_LINE };
enum { F_NONE = 0, F_DATA, F_EVENT, F_ID, F_RETRY };

static void emit_perr(sse_parser_t *p, sse_parse_error_t err) {
  if (p->cb.on_error) p->cb.on_error(p->cb.userdata, err);
}

static void dispatch_block(sse_parser_t *p) {
  if (!p->data_overflow) {
    if (p->has_id) {
      p->buf.id_buf[p->id_len] = '\0';
      if (p->cb.on_id) p->cb.on_id(p->cb.userdata, p->buf.id_buf, p->id_len);
    }
    if (p->data_lines > 0 && p->cb.on_event) {
      size_t n = p->data_len - 1; /* strip trailing '\n' */
      p->buf.data_buf[n] = '\0';
      sse_parser_event_t ev;
      ev.data = p->buf.data_buf;
      ev.data_len = n;
      if (p->has_event) {
        p->buf.event_buf[p->event_len] = '\0';
        ev.event = p->buf.event_buf;
      } else {
        ev.event = NULL;
      }
      ev.id = p->has_id ? p->buf.id_buf : NULL;
      p->cb.on_event(p->cb.userdata, &ev);
    }
  }
  p->data_len = 0;
  p->data_lines = 0;
  p->data_overflow = false;
  p->event_len = 0;
  p->has_event = false;
  p->event_invalid = false;
  p->id_len = 0;
  p->has_id = false;
  p->id_invalid = false;
}

static void start_value(sse_parser_t *p) {
  switch (p->field) {
    case F_EVENT:
      p->event_len = 0;
      p->event_invalid = false;
      break;
    case F_ID:
      p->id_len = 0;
      p->id_invalid = false;
      break;
    case F_RETRY:
      p->retry_acc = 0;
      p->retry_seen_digit = false;
      p->retry_invalid = false;
      break;
    default:
      break; /* F_DATA appends; separator handled at commit */
  }
}

static void overflow_block(sse_parser_t *p) {
  if (!p->data_overflow) {
    p->data_overflow = true;
    emit_perr(p, SSE_PARSE_ERR_DATA_TOO_LARGE);
  }
}

static void value_byte(sse_parser_t *p, uint8_t b) {
  switch (p->field) {
    case F_DATA:
      if (p->data_overflow) return;
      if (p->data_len == p->buf.data_buf_len) {
        overflow_block(p);
        return;
      }
      p->buf.data_buf[p->data_len++] = (char)b;
      return;
    case F_EVENT:
      if (p->event_invalid) return;
      if (p->event_len + 1 >= p->buf.event_buf_len) {
        p->event_invalid = true;
        emit_perr(p, SSE_PARSE_ERR_EVENT_TYPE_TOO_LARGE);
        return;
      }
      p->buf.event_buf[p->event_len++] = (char)b;
      return;
    case F_ID:
      if (p->id_invalid) return;
      if (b == 0 || p->id_len + 1 >= p->buf.id_buf_len) {
        p->id_invalid = true;
        emit_perr(p, SSE_PARSE_ERR_ID_INVALID);
        return;
      }
      p->buf.id_buf[p->id_len++] = (char)b;
      return;
    case F_RETRY:
      if (p->retry_invalid) return;
      if (b < '0' || b > '9') {
        p->retry_invalid = true;
        return; /* error is reported at commit, once per line */
      }
      p->retry_seen_digit = true;
      if (p->retry_acc > (UINT32_MAX - (uint32_t)(b - '0')) / 10u) {
        p->retry_acc = UINT32_MAX; /* clamp */
      } else {
        p->retry_acc = p->retry_acc * 10u + (uint32_t)(b - '0');
      }
      return;
    default:
      return; /* F_NONE: discard */
  }
}

static void commit_value(sse_parser_t *p) {
  switch (p->field) {
    case F_DATA:
      if (!p->data_overflow) {
        if (p->data_len == p->buf.data_buf_len) {
          overflow_block(p);
        } else {
          p->buf.data_buf[p->data_len++] = '\n';
          p->data_lines++;
        }
      }
      break;
    case F_EVENT:
      if (p->event_invalid) {
        p->event_len = 0;
        p->has_event = false;
      } else {
        p->has_event = p->event_len > 0; /* empty value resets to none */
      }
      break;
    case F_ID:
      if (p->id_invalid) {
        p->id_len = 0;
        p->has_id = false;
      } else {
        p->has_id = true; /* empty id is valid and means "" */
      }
      break;
    case F_RETRY:
      if (p->retry_invalid || !p->retry_seen_digit) {
        emit_perr(p, SSE_PARSE_ERR_INVALID_RETRY);
      } else if (p->cb.on_retry) {
        p->cb.on_retry(p->cb.userdata, p->retry_acc);
      }
      break;
    default:
      break;
  }
  p->field = F_NONE;
}

static void resolve_field_name(sse_parser_t *p) {
  size_t n = p->name_len;
  p->field = F_NONE;
  if (n == 4 && memcmp(p->name, "data", 4) == 0) p->field = F_DATA;
  else if (n == 5 && memcmp(p->name, "event", 5) == 0) p->field = F_EVENT;
  else if (n == 2 && memcmp(p->name, "id", 2) == 0) p->field = F_ID;
  else if (n == 5 && memcmp(p->name, "retry", 5) == 0) p->field = F_RETRY;

  if (p->field == F_NONE) {
    emit_perr(p, SSE_PARSE_ERR_UNKNOWN_FIELD);
    p->state = PS_SKIP_LINE;
    return;
  }
  start_value(p);
  p->state = PS_AFTER_COLON;
}

static void line_end(sse_parser_t *p) {
  switch (p->state) {
    case PS_LINE_START:
      dispatch_block(p); /* blank line */
      break;
    case PS_FIELD_NAME:
      /* Line without a colon: whole line is the field name, value is empty. */
      resolve_field_name(p);
      if (p->state == PS_AFTER_COLON) commit_value(p);
      break;
    case PS_AFTER_COLON:
    case PS_VALUE:
      commit_value(p);
      break;
    default:
      break; /* PS_SKIP_LINE: nothing to commit. PS_BOM cannot reach here. */
  }
  p->state = PS_LINE_START;
}

static const uint8_t SSE_BOM[3] = {0xEF, 0xBB, 0xBF};

static void process_byte(sse_parser_t *p, uint8_t b) {
  if (p->swallow_lf) {
    p->swallow_lf = false;
    if (b == '\n') return; /* second half of a CRLF pair */
  }

  if (p->state == PS_BOM) {
    if (p->bom_n < 3 && b == SSE_BOM[p->bom_n]) {
      p->bom_n++;
      if (p->bom_n == 3) {
        p->state = PS_LINE_START;
        p->bom_n = 0;
      }
      return;
    }
    /* Not a BOM after all: replay the matched prefix as ordinary bytes,
     * then fall through to process the current byte. Recursion depth <= 3
     * and replayed bytes are never terminators. */
    uint8_t n = p->bom_n;
    p->bom_n = 0;
    p->state = PS_LINE_START;
    for (uint8_t i = 0; i < n; i++) process_byte(p, SSE_BOM[i]);
  }

  if (b == '\r') {
    line_end(p);
    p->swallow_lf = true;
    return;
  }
  if (b == '\n') {
    line_end(p);
    return;
  }

  switch (p->state) {
    case PS_LINE_START:
      if (b == ':') { /* comment line */
        p->state = PS_SKIP_LINE;
        return;
      }
      p->state = PS_FIELD_NAME;
      p->name_len = 0;
      /* fall through */
    case PS_FIELD_NAME:
      if (b == ':') {
        resolve_field_name(p);
        return;
      }
      if (p->name_len < sizeof(p->name)) p->name[p->name_len++] = (char)b;
      if (p->name_len > 5) { /* longer than any known field name */
        emit_perr(p, SSE_PARSE_ERR_UNKNOWN_FIELD);
        p->state = PS_SKIP_LINE;
      }
      return;
    case PS_AFTER_COLON:
      p->state = PS_VALUE;
      if (b == ' ') return; /* single leading space is stripped */
      /* fall through */
    case PS_VALUE:
      value_byte(p, b);
      return;
    default:
      return; /* PS_SKIP_LINE */
  }
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
