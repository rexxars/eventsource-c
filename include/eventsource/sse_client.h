#ifndef SSE_CLIENT_H
#define SSE_CLIENT_H

#include "sse_parser.h"
#include "sse_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max persisted lastEventId length; override with -DSSE_CLIENT_ID_MAX=n */
#ifndef SSE_CLIENT_ID_MAX
#define SSE_CLIENT_ID_MAX 128
#endif

/* Max total header lines (3 composed + user extras). */
#ifndef SSE_CLIENT_MAX_HEADERS
#define SSE_CLIENT_MAX_HEADERS 16
#endif

typedef enum {
  SSE_STATE_IDLE = 0,
  SSE_STATE_CONNECTING,
  SSE_STATE_OPEN,
  SSE_STATE_WAITING_RETRY,
  SSE_STATE_CLOSED
} sse_client_state_t;

typedef struct {
  const char *event;         /* "message" when the server sent no event field */
  const char *data;          /* NUL-terminated; may contain embedded NULs */
  size_t data_len;
  const char *last_event_id; /* current lastEventId, "" when none */
} sse_message_t;

typedef enum {
  SSE_ERR_TRANSPORT = 0,    /* connect/read failure or EOF */
  SSE_ERR_HTTP_STATUS,      /* non-200 (status in .http_status) */
  SSE_ERR_BAD_CONTENT_TYPE,
  SSE_ERR_SERVER_STOP,      /* HTTP 204 */
  SSE_ERR_IDLE_TIMEOUT,
  SSE_ERR_MESSAGE_TOO_LARGE /* informational: stream continues */
} sse_error_reason_t;

typedef struct {
  sse_error_reason_t reason;
  int http_status;      /* 0 when not applicable */
  bool will_retry;      /* final decision (or default, inside reconnect_policy) */
  uint32_t retry_in_ms; /* valid when will_retry; 0 for MESSAGE_TOO_LARGE */
} sse_error_t;

typedef struct {
  void *userdata;
  void (*on_open)(void *ud, unsigned reconnect_count); /* 0 = first connect */
  void (*on_message)(void *ud, const sse_message_t *msg);
  void (*on_error)(void *ud, const sse_error_t *err); /* also the reconnect signal */
  void (*on_closed)(void *ud);                        /* terminal, fires once */
} sse_client_callbacks_t;

typedef struct {
  const char *url;                  /* borrowed; must outlive the client */
  const char *const *extra_headers; /* NULL-terminated "Name: value", or NULL */

  uint32_t default_retry_ms; /* 0 -> 3000 */
  uint32_t max_retry_ms;     /* backoff cap; 0 -> flat interval */
  uint8_t jitter_pct;        /* up-only jitter, 0 -> none */
  uint32_t idle_timeout_ms;  /* 0 -> disabled */
  uint32_t read_timeout_ms;  /* max block per poll, 0 -> 500 */
  bool skip_content_type_check; /* false -> require text/event-stream */

  /* Caller-provided parser buffers. id_buf_len must be at most
   * SSE_CLIENT_ID_MAX + 1 so every accepted id can be persisted as the
   * lastEventId (init fails otherwise). */
  sse_parser_buffers_t buffers;
  uint8_t *rx_buf;
  size_t rx_buf_len;

  sse_transport_t *transport;
  uint32_t (*now_ms)(void); /* monotonic ms */
  sse_client_callbacks_t callbacks;

  /* Optional. Receives the default decision in err->will_retry; the return
   * value is final. NULL -> default policy. */
  bool (*reconnect_policy)(void *ud, const sse_error_t *err);
} sse_client_config_t;

/* Struct declared here for static allocation; all fields private. */
typedef struct sse_client {
  sse_client_config_t cfg;
  sse_parser_t parser;
  uint8_t state;
  /* Cross-task stop flag. Accessed via atomic builtins on GCC/Clang
   * toolchains (see sse_client.c); volatile is the storage-class fallback
   * for other compilers, where request_stop's cross-task guarantee weakens
   * to "platforms with atomic aligned single-byte stores". */
  volatile unsigned char stop_requested;
  bool transport_open;
  unsigned attempts; /* failures since last delivered message */
  size_t extra_header_count; /* validated at init; bounds every rebuild */
  uint32_t base_retry_ms;
  uint32_t retry_deadline;
  uint32_t last_rx_ms;
  uint32_t prng;
  char last_event_id[SSE_CLIENT_ID_MAX + 1];
  size_t last_event_id_len;
  char id_header[SSE_CLIENT_ID_MAX + 32];
  const char *headers[SSE_CLIENT_MAX_HEADERS + 1];
} sse_client_t;

int sse_client_init(sse_client_t *c, const sse_client_config_t *cfg);
/* Drives everything. Returns a max-sleep hint in ms: 0 = call again now,
 * UINT32_MAX = client is CLOSED, stop calling. */
uint32_t sse_client_poll(sse_client_t *c);
void sse_client_close(sse_client_t *c);        /* from the polling task */
void sse_client_request_stop(sse_client_t *c); /* safe from other tasks */
sse_client_state_t sse_client_state(const sse_client_t *c);
const char *sse_client_last_event_id(const sse_client_t *c);

#ifdef __cplusplus
}
#endif
#endif /* SSE_CLIENT_H */
