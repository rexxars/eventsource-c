#ifndef SSE_TRANSPORT_H
#define SSE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *url;
  const char *const *headers; /* NULL-terminated "Name: value" strings */
} sse_request_t;

typedef struct {
  int status_code;
  char content_type[64]; /* raw Content-Type value; may include parameters */
  int32_t retry_after_s; /* Retry-After delta-seconds, -1 when absent */
} sse_response_info_t;

/* read() return values (0 and negatives; positive = byte count) */
#define SSE_READ_TIMEOUT 0
#define SSE_READ_EOF (-1)
#define SSE_READ_ERROR (-2)

typedef struct sse_transport {
  void *ctx;
  /* Blocks until response headers are available. 0 = ok, <0 = failure. */
  int (*open)(void *ctx, const sse_request_t *req, sse_response_info_t *out);
  /* Blocks at most timeout_ms. Returns bytes read, or SSE_READ_*. */
  int (*read)(void *ctx, void *buf, size_t len, uint32_t timeout_ms);
  void (*close)(void *ctx);
} sse_transport_t;

#ifdef __cplusplus
}
#endif
#endif /* SSE_TRANSPORT_H */
