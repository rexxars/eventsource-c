#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H
#include "eventsource/sse_transport.h"
#include <stdio.h>
#include <string.h>

/* Fake clock shared by client tests. */
static uint32_t g_now = 1000;
static uint32_t fake_now(void) { return g_now; }

typedef struct {
  int open_result;         /* 0 = respond, <0 = connect error */
  int status;              /* HTTP status */
  const char *content_type;
  int32_t retry_after_s;   /* -1 = absent */
  const char *chunks[16];  /* NUL-terminated body chunks, one per read() */
  int tail;                /* read() result after chunks: SSE_READ_EOF etc. */
} mock_conn_t;

typedef struct {
  mock_conn_t conns[8];
  int n_conns;
  int conn;         /* index of current/next connection */
  int chunk;        /* next chunk within current connection */
  size_t chunk_off; /* read offset within the current chunk */
  int open_calls;
  int close_calls;
  bool is_open;
  char captured_headers[512]; /* headers of the most recent open, joined by | */
} mock_state_t;

static mock_state_t mock;

static int mock_open(void *ctx, const sse_request_t *req, sse_response_info_t *out) {
  (void)ctx;
  mock.captured_headers[0] = 0;
  for (const char *const *h = req->headers; *h; h++) {
    strncat(mock.captured_headers, *h,
            sizeof(mock.captured_headers) - strlen(mock.captured_headers) - 2);
    strcat(mock.captured_headers, "|");
  }
  mock_conn_t *cn = &mock.conns[mock.conn < mock.n_conns ? mock.conn : mock.n_conns - 1];
  mock.open_calls++;
  if (cn->open_result < 0) {
    mock.conn++;
    return cn->open_result;
  }
  mock.is_open = true;
  mock.chunk = 0;
  mock.chunk_off = 0;
  out->status_code = cn->status;
  snprintf(out->content_type, sizeof out->content_type, "%s",
           cn->content_type ? cn->content_type : "");
  out->retry_after_s = cn->retry_after_s;
  return 0;
}

static int mock_read(void *ctx, void *buf, size_t len, uint32_t timeout_ms) {
  (void)ctx;
  (void)timeout_ms;
  mock_conn_t *cn = &mock.conns[mock.conn < mock.n_conns ? mock.conn : mock.n_conns - 1];
  if (mock.chunk < 16 && cn->chunks[mock.chunk]) {
    /* Chunks larger than the caller's buffer are delivered across reads. */
    const char *c = cn->chunks[mock.chunk];
    size_t clen = strlen(c);
    size_t rem = clen - mock.chunk_off;
    size_t n = rem < len ? rem : len;
    memcpy(buf, c + mock.chunk_off, n);
    mock.chunk_off += n;
    if (mock.chunk_off >= clen) {
      mock.chunk++;
      mock.chunk_off = 0;
    }
    return (int)n;
  }
  return cn->tail;
}

static void mock_close(void *ctx) {
  (void)ctx;
  mock.close_calls++;
  if (mock.is_open) {
    mock.is_open = false;
    mock.conn++; /* next open() serves the next scripted connection */
  }
}

static sse_transport_t mock_vtable = {NULL, mock_open, mock_read, mock_close};

static void mock_reset(void) {
  memset(&mock, 0, sizeof mock);
  g_now = 1000;
}
static sse_transport_t *mock_transport(void) { return &mock_vtable; }
#endif
