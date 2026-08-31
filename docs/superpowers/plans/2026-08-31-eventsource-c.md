# eventsource-c Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the allocation-free C99 SSE/EventSource client library described in PROPOSAL.md: a byte-machine parser, a poll-driven reconnecting client, a libcurl POSIX port, and an ESP-IDF component with registry packaging.

**Architecture:** Three layers. `sse_parser` (src/sse_parser.c) is a pure byte-oriented state machine with zero I/O and zero allocation. `sse_client` (src/sse_client.c) is a connection state machine driven by `sse_client_poll()`, talking to the network only through an `sse_transport_t` vtable and a `now_ms()` clock function. Ports (libcurl for POSIX, esp_http_client for ESP-IDF) implement the vtable.

**Tech Stack:** C99, CMake (dual-mode: host build or `idf_component_register`), libcurl (host port only), ESP-IDF >= 5.0 (device port only), libFuzzer + ASan/UBSan for fuzzing, plain-C test executables run by CTest.

**Spec:** `PROPOSAL.md` at the repo root. The plan argues from the spec; executors read both.

## Global Constraints

- Language: C99 (`-std=c99`), compiled with `-Wall -Wextra -Werror`. Test builds add `-fsanitize=address,undefined`.
- Core (`src/`, `include/`) never calls malloc/free and never includes OS or network headers. `<string.h>`, `<stdio.h>` (snprintf only), `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` are allowed. Ports may allocate.
- All public identifiers use the `sse_` prefix; macros and enum values use `SSE_`.
- Config structs must be zero-init friendly: `0` in any numeric field means "use the documented default" (`default_retry_ms` 0 -> 3000, `read_timeout_ms` 0 -> 500, `max_retry_ms` 0 -> no backoff/flat interval, `jitter_pct` 0 -> no jitter, `idle_timeout_ms` 0 -> disabled). `skip_content_type_check` false -> strict.
- A data buffer of size N holds event payloads up to N-1 bytes (the parser reuses the trailing separator slot for the NUL terminator).
- The client caps persisted ids at `SSE_CLIENT_ID_MAX` (default 128, compile-time overridable).
- Reconnect delay: `Retry-After` (delta-seconds) wins if present; else `min(base << attempts, max_retry_ms)` when `max_retry_ms > 0`, else flat `base`; then add up-only jitter `delay * (rand % (jitter_pct+1)) / 100`. `base` is the last server-sent `retry:` value, else `default_retry_ms`.
- Default reconnect policy: retry on transport error/EOF/idle-timeout/bad-content-type/5xx/429/any-4xx-with-Retry-After; stop on 204 and all other non-200. The `reconnect_policy` hook, when set, gets the default decision in `err->will_retry` and its return value is final.
- License: MIT (matches the sibling JS repos).
- Commits use conventional-commit prefixes (`feat:`, `test:`, `build:`, `docs:`, `ci:`).
- Callback ordering contract (parity with `../parser` and `../eventsource`): within a block, `on_id` fires before `on_event`; an overflowed block fires neither; `lastEventId` is updated before `on_message` sees it.

## Known, deliberate refinements vs PROPOSAL.md

These are edge-case precision decisions the spec left loose; they are the plan's contract:

1. An invalid `id`/`event` field (NUL byte or buffer overflow) resets that buffer to "absent" for the block rather than restoring a previous same-block value. Avoids double-buffering. The JS parser would keep the earlier value; nobody streams two id fields per block where the second is invalid.
2. `sse_error_t` with reason `SSE_ERR_MESSAGE_TOO_LARGE` is informational: the connection stays open (parser resyncs). `will_retry` is set true and `retry_in_ms` 0 in that callback.
3. Callback order on connection failure: `reconnect_policy` hook first (it receives the default decision), then a single `on_error` carrying the final `will_retry` and delay.
4. `sse_client_poll()` returns `UINT32_MAX` once CLOSED ("stop calling me"), `0` for "call again immediately", otherwise a max-sleep hint in ms.

---

### Task 1: Repo scaffold + parser API surface + init/reset

**Files:**
- Create: `.gitignore`, `LICENSE`, `README.md`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/t.h`
- Create: `include/eventsource/sse_parser.h`, `src/sse_parser.c` (init/reset/feed skeleton only)
- Test: `tests/test_parser_init.c`

**Interfaces:**
- Produces: `sse_parser_t` (full struct in header, fields private by convention), `sse_parser_init(sse_parser_t *p, const sse_parser_callbacks_t *cb, const sse_parser_buffers_t *buf)`, `sse_parser_feed(sse_parser_t *p, const void *chunk, size_t len)`, `sse_parser_reset(sse_parser_t *p)`. Callback struct field order: `userdata, on_event, on_id, on_retry, on_error`. Buffer struct field order: `data_buf, data_buf_len, id_buf, id_buf_len, event_buf, event_buf_len`.
- Produces: test macros `OK(cond)`, `OK_STR(a,b)`, `OK_INT(a,b)` and counter `t_failures` from `tests/t.h`; CMake helper `sse_add_test(<name>)`.

- [ ] **Step 1: Initialize the repository**

```bash
cd /Users/espenh/webdev/eventsource/c
git init
```

Write `.gitignore`:

```
build/
*.o
.DS_Store
managed_components/
dependencies.lock
sdkconfig
sdkconfig.old
```

Write `LICENSE` with the MIT license text, copyright line: `Copyright (c) 2026 Espen Hovlandsdal <espen@hovlandsdal.com>`.

Write `README.md` stub (Task 13 replaces it):

```markdown
# eventsource-c

Server-Sent Events (EventSource) client for embedded C. Work in progress; see PROPOSAL.md.
```

- [ ] **Step 2: Write the public parser header**

`include/eventsource/sse_parser.h`:

```c
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
  SSE_PARSE_ERR_UNKNOWN_FIELD       /* informational; line skipped */
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
```

- [ ] **Step 3: Write the test harness header**

`tests/t.h`:

```c
#ifndef T_H
#define T_H
#include <stdio.h>
#include <string.h>

static int t_failures = 0;

#define OK(cond) do { if (!(cond)) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define OK_STR(a, b) do { const char *t_a = (a), *t_b = (b); \
  if (t_a == NULL || strcmp(t_a, t_b) != 0) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
          t_a ? t_a : "(null)", t_b); } } while (0)

#define OK_INT(a, b) do { long t_a = (long)(a), t_b = (long)(b); \
  if (t_a != t_b) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: got %ld want %ld\n", __FILE__, __LINE__, t_a, t_b); } } while (0)

#define T_END() do { if (t_failures) { \
  fprintf(stderr, "%d failure(s)\n", t_failures); return 1; } \
  printf("ok\n"); return 0; } while (0)
#endif
```

- [ ] **Step 4: Write the failing init/reset test**

`tests/test_parser_init.c`:

```c
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
```

- [ ] **Step 5: Write the build files**

`CMakeLists.txt` (root):

```cmake
cmake_minimum_required(VERSION 3.16)

if(ESP_PLATFORM)
  # ESP-IDF component build. Sources grow in Task 12.
  idf_component_register(
    SRCS src/sse_parser.c
    INCLUDE_DIRS include
  )
  return()
endif()

project(eventsource C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(SSE_BUILD_TESTS "Build host tests" ON)
option(SSE_SANITIZE "Build with ASan/UBSan" ON)

if(SSE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(eventsource src/sse_parser.c)
target_include_directories(eventsource PUBLIC include)
target_compile_options(eventsource PRIVATE -Wall -Wextra -Werror)

if(SSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

`tests/CMakeLists.txt`:

```cmake
function(sse_add_test name)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE eventsource)
  target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

sse_add_test(parser_init)
```

- [ ] **Step 6: Run the test to verify it fails**

Run: `cmake -B build && cmake --build build`
Expected: FAIL to link, `undefined reference`/`symbol not found` for `sse_parser_init` (src/sse_parser.c does not exist yet).

- [ ] **Step 7: Write the parser skeleton**

`src/sse_parser.c`:

```c
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
```

Note: `-Werror` + unused enum constants is fine (enums are not flagged by `-Wunused`); the `(void)` casts keep `process_byte` warning-clean until Task 2 fills it in.

- [ ] **Step 8: Run test to verify it passes**

Run: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `parser_init` PASS.

- [ ] **Step 9: Commit**

```bash
git add .gitignore LICENSE README.md CMakeLists.txt tests/ include/ src/ PROPOSAL.md docs/
git commit -m "feat: repo scaffold, sse_parser API surface, init/reset"
```

---

### Task 2: Parser core state machine (LF-only: data, event, comments, unknown fields)

**Files:**
- Modify: `src/sse_parser.c` (replace `process_byte` stub; add helpers)
- Create: `tests/parser_rec.h` (callback recorder shared by all parser tests)
- Test: `tests/test_parser_basic.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(parser_basic)`)

**Interfaces:**
- Consumes: Task 1's header and skeleton (`sse_parser_init/feed/reset`, struct fields, `PS_*`/`F_*` enums).
- Produces: internal statics in sse_parser.c used by later tasks: `process_byte(sse_parser_t*, uint8_t)`, `line_end(sse_parser_t*)`, `dispatch_block(sse_parser_t*)`, `resolve_field_name(sse_parser_t*)`, `start_value(sse_parser_t*)`, `value_byte(sse_parser_t*, uint8_t)`, `commit_value(sse_parser_t*)`, `emit_perr(sse_parser_t*, sse_parse_error_t)`.
- Produces: `tests/parser_rec.h` recorder: `rec_t` with `char log[8192]`, `rec_reset()`, `rec_cb()` returning a filled `sse_parser_callbacks_t`, log line formats `event(<type|->,<id|->,<len>,<data>)`, `id(<id>,<len>)`, `retry(<ms>)`, `err(<int>)`.

- [ ] **Step 1: Write the recorder header**

`tests/parser_rec.h`:

```c
#ifndef PARSER_REC_H
#define PARSER_REC_H
#include "eventsource/sse_parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char log[8192];
  size_t len;
} rec_t;

static void rec_add(rec_t *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(r->log + r->len, sizeof(r->log) - r->len, fmt, ap);
  va_end(ap);
  if (n > 0 && (size_t)n < sizeof(r->log) - r->len) r->len += (size_t)n;
}

static void rec_on_event(void *ud, const sse_parser_event_t *ev) {
  rec_add(ud, "event(%s,%s,%zu,%s)\n", ev->event ? ev->event : "-",
          ev->id ? ev->id : "-", ev->data_len, ev->data);
}
static void rec_on_id(void *ud, const char *id, size_t len) {
  rec_add(ud, "id(%s,%zu)\n", id, len);
}
static void rec_on_retry(void *ud, uint32_t ms) {
  rec_add(ud, "retry(%u)\n", (unsigned)ms);
}
static void rec_on_error(void *ud, sse_parse_error_t e) {
  rec_add(ud, "err(%d)\n", (int)e);
}

static void rec_reset(rec_t *r) { memset(r, 0, sizeof *r); }

static sse_parser_callbacks_t rec_cb(rec_t *r) {
  sse_parser_callbacks_t cb = {r, rec_on_event, rec_on_id, rec_on_retry, rec_on_error};
  return cb;
}
#endif
```

- [ ] **Step 2: Write the failing tests**

`tests/test_parser_basic.c`:

```c
#include "eventsource/sse_parser.h"
#include "parser_rec.h"
#include "t.h"

static rec_t r;
static char db[256], ib[64], eb[32];

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

  /* single data line, no space variant */
  fresh(&p);
  feed_str(&p, "data:hello\n\n");
  OK_STR(r.log, "event(-,-,5,hello)\n");

  /* single data line, one leading space is stripped, second kept */
  fresh(&p);
  feed_str(&p, "data:  two spaces\n\n");
  OK_STR(r.log, "event(-,-,11, two spaces)\n");

  /* multi-line data joined with \n */
  fresh(&p);
  feed_str(&p, "data: foo\ndata: bar\n\n");
  OK_STR(r.log, "event(-,-,7,foo\nbar)\n");

  /* event type field */
  fresh(&p);
  feed_str(&p, "event: add\ndata: x\n\n");
  OK_STR(r.log, "event(add,-,1,x)\n");

  /* empty event value resets type to none */
  fresh(&p);
  feed_str(&p, "event: add\nevent:\ndata: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* event type does not leak into the next block */
  fresh(&p);
  feed_str(&p, "event: add\ndata: x\n\ndata: y\n\n");
  OK_STR(r.log, "event(add,-,1,x)\nevent(-,-,1,y)\n");

  /* "data" with no colon dispatches an empty-string event */
  fresh(&p);
  feed_str(&p, "data\n\n");
  OK_STR(r.log, "event(-,-,0,)\n");

  /* "data:" with empty value also dispatches empty string */
  fresh(&p);
  feed_str(&p, "data:\n\n");
  OK_STR(r.log, "event(-,-,0,)\n");

  /* blank lines alone dispatch nothing */
  fresh(&p);
  feed_str(&p, "\n\n\n");
  OK_STR(r.log, "");

  /* event without data dispatches nothing */
  fresh(&p);
  feed_str(&p, "event: add\n\n");
  OK_STR(r.log, "");

  /* comment lines are skipped silently */
  fresh(&p);
  feed_str(&p, ": keepalive\ndata: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* unknown field is reported once and skipped */
  fresh(&p);
  feed_str(&p, "foo: bar\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n"); /* 4 == SSE_PARSE_ERR_UNKNOWN_FIELD */

  /* long unknown field name (>5 chars, no colon yet) is detected early */
  fresh(&p);
  feed_str(&p, "dataxx: y\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n");

  /* no trailing blank line: nothing dispatched */
  fresh(&p);
  feed_str(&p, "data: dangling\n");
  OK_STR(r.log, "");

  T_END();
}
```

Add to `tests/CMakeLists.txt`: `sse_add_test(parser_basic)`

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake -B build && cmake --build build && ctest --test-dir build -R parser_basic --output-on-failure`
Expected: FAIL with `OK_STR` mismatches (stub parses nothing, logs are empty).

- [ ] **Step 4: Implement the state machine**

Replace the `process_byte` stub in `src/sse_parser.c` with the following (all functions are `static`, placed above `sse_parser_init`). Task 3 fills the `F_ID`/`F_RETRY` cases, Task 4 the BOM/CR handling, Task 5 the overflow checks; the switch arms below already have the shape they extend.

```c
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

static void process_byte(sse_parser_t *p, uint8_t b) {
  /* Task 4 adds BOM handling and \r / swallow_lf handling here. */
  if (b == '\n') {
    line_end(p);
    return;
  }
  if (p->state == PS_BOM) p->state = PS_LINE_START; /* placeholder until Task 4 */

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
```

Note the fall-throughs: with `-Werror` and GCC 7+/-Wimplicit-fallthrough enabled by `-Wextra`, mark them with `/* fall through */` exactly as shown (both GCC and Clang honor the comment form).

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `parser_init` and `parser_basic` PASS.

- [ ] **Step 6: Commit**

```bash
git add src/sse_parser.c tests/parser_rec.h tests/test_parser_basic.c tests/CMakeLists.txt
git commit -m "feat: parser state machine for data/event fields, comments, unknown fields"
```

---

### Task 3: Parser id and retry semantics

**Files:**
- Modify: `src/sse_parser.c` (the `F_ID`/`F_RETRY` code already landed in Task 2; this task only adds tests that pin its semantics — if any test fails, fix the implementation)
- Test: `tests/test_parser_fields.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(parser_fields)`)

**Interfaces:**
- Consumes: recorder from `tests/parser_rec.h` (log formats `id(<id>,<len>)`, `retry(<ms>)`, `err(<int>)`), parser API from Task 1.
- Produces: pinned semantics later tasks rely on: `on_id` before `on_event`; `on_id` fires on id-only blocks; NUL-in-id drops the field.

- [ ] **Step 1: Write the failing/pinning tests**

`tests/test_parser_fields.c`:

```c
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
  sse_parser_feed(&p, "id: 4\x00" "2\ndata: x\n\n", 16);
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
```

Add to `tests/CMakeLists.txt`: `sse_add_test(parser_fields)`

- [ ] **Step 2: Run test**

Run: `cmake --build build && ctest --test-dir build -R parser_fields --output-on-failure`
Expected: PASS if Task 2's implementation is correct; if any assertion fails, fix `value_byte`/`commit_value`/`dispatch_block` in `src/sse_parser.c` until green. Do not adjust the expectations: they encode the spec.

- [ ] **Step 3: Commit**

```bash
git add tests/test_parser_fields.c tests/CMakeLists.txt src/sse_parser.c
git commit -m "test: pin parser id and retry semantics"
```

---

### Task 4: CR/CRLF terminators, BOM, chunk-boundary invariance

**Files:**
- Modify: `src/sse_parser.c` (`process_byte`: BOM + CR handling)
- Test: `tests/test_parser_boundaries.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(parser_boundaries)`)

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: the boundary-determinism guarantee (same stream, any chunking, identical callback log) that the fuzzer (Task 6) and client tests rely on.

- [ ] **Step 1: Write the failing tests**

`tests/test_parser_boundaries.c`:

```c
#include "eventsource/sse_parser.h"
#include "parser_rec.h"
#include "t.h"

static rec_t r;
static char db[512], ib[64], eb[32];

static void fresh(sse_parser_t *p) {
  rec_reset(&r);
  sse_parser_buffers_t bufs = {db, sizeof db, ib, sizeof ib, eb, sizeof eb};
  sse_parser_callbacks_t cb = rec_cb(&r);
  sse_parser_init(p, &cb, &bufs);
}

static void feed_str(sse_parser_t *p, const char *s) {
  sse_parser_feed(p, s, strlen(s));
}

/* Feed `stream` in chunks of `chunk` bytes and return the log. */
static const char *run_chunked(const char *stream, size_t chunk) {
  sse_parser_t p;
  fresh(&p);
  size_t n = strlen(stream);
  for (size_t i = 0; i < n; i += chunk) {
    size_t k = n - i < chunk ? n - i : chunk;
    sse_parser_feed(&p, stream + i, k);
  }
  return r.log;
}

int main(void) {
  sse_parser_t p;

  /* CRLF terminators */
  fresh(&p);
  feed_str(&p, "data: x\r\n\r\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* bare CR terminators */
  fresh(&p);
  feed_str(&p, "data: x\r\r");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* mixed: CR line, LF line */
  fresh(&p);
  feed_str(&p, "event: a\rdata: x\n\r");
  OK_STR(r.log, "event(a,-,1,x)\n");

  /* CRLF split across two feeds must not create a phantom blank line */
  fresh(&p);
  feed_str(&p, "data: x\r");
  feed_str(&p, "\ndata: y\n\n");
  OK_STR(r.log, "event(-,-,3,x\ny)\n");

  /* CR at end of one feed, next feed starts with a data line (bare CR case) */
  fresh(&p);
  feed_str(&p, "data: x\r");
  feed_str(&p, "\r");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* UTF-8 BOM at stream start is stripped */
  fresh(&p);
  feed_str(&p, "\xEF\xBB\xBF" "data: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* BOM split across feeds */
  fresh(&p);
  feed_str(&p, "\xEF");
  feed_str(&p, "\xBB\xBF" "data: x\n\n");
  OK_STR(r.log, "event(-,-,1,x)\n");

  /* Partial BOM prefix that is not a BOM gets replayed as line bytes
   * (becomes an unknown-field line, reported and skipped) */
  fresh(&p);
  feed_str(&p, "\xEF\xBB" "oops\ndata: x\n\n");
  OK_STR(r.log, "err(4)\nevent(-,-,1,x)\n");

  /* BOM only valid at stream start: after reset it is stripped again */
  fresh(&p);
  feed_str(&p, "\xEF\xBB\xBF" "data: x\n\n");
  sse_parser_reset(&p);
  rec_reset(&r);
  feed_str(&p, "\xEF\xBB\xBF" "data: y\n\n");
  OK_STR(r.log, "event(-,-,1,y)\n");

  /* Chunk-boundary invariance: a gnarly fixture must produce an identical
   * log fed whole or in chunks of size 1..7. */
  static const char fixture[] =
      "\xEF\xBB\xBF"
      ": warm up\r\n"
      "retry: 2500\n"
      "id: 100\r"
      "event: tick\r\n"
      "data: first\n"
      "data: second\r\n"
      "\r\n"
      "unknown-field: zzz\n"
      "id: 4\x31\n" /* "41" spelled awkwardly to keep bytes obvious */
      "data:no-space\r"
      "\r"
      "data: dangling";
  char expected[sizeof(r.log)];
  {
    const char *whole = run_chunked(fixture, sizeof fixture); /* one chunk */
    strcpy(expected, whole);
    OK(strlen(expected) > 0);
  }
  for (size_t chunk = 1; chunk <= 7; chunk++) {
    const char *got = run_chunked(fixture, chunk);
    if (strcmp(got, expected) != 0) {
      t_failures++;
      fprintf(stderr, "FAIL chunk=%zu:\n--- got ---\n%s--- want ---\n%s", chunk, got, expected);
    }
  }

  T_END();
}
```

Add to `tests/CMakeLists.txt`: `sse_add_test(parser_boundaries)`

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build -R parser_boundaries --output-on-failure`
Expected: FAIL on every CR/BOM case (Task 2's `process_byte` treats `\r` as a value byte and skips BOM handling).

- [ ] **Step 3: Implement BOM + CR handling**

In `src/sse_parser.c`, replace the top of `process_byte` (everything above the `switch`) with:

```c
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
  /* ... unchanged from Task 2 (remove the old `if (p->state == PS_BOM)`
     placeholder line) ... */
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all four parser suites PASS.

- [ ] **Step 5: Commit**

```bash
git add src/sse_parser.c tests/test_parser_boundaries.c tests/CMakeLists.txt
git commit -m "feat: CR/CRLF/split-CRLF terminators, BOM stripping, boundary invariance test"
```

---

### Task 5: Parser buffer limits and overflow policy

**Files:**
- Modify: `src/sse_parser.c` (only if a test exposes a defect; the overflow code shipped in Task 2)
- Test: `tests/test_parser_limits.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(parser_limits)`)

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: pinned overflow contract for the client (Task 7+): oversized block fires `err(0)` once, delivers nothing, does not commit id, parser resyncs at next blank line.

- [ ] **Step 1: Write the tests**

`tests/test_parser_limits.c`:

```c
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
```

Add to `tests/CMakeLists.txt`: `sse_add_test(parser_limits)`

- [ ] **Step 2: Run test**

Run: `cmake --build build && ctest --test-dir build -R parser_limits --output-on-failure`
Expected: PASS (Task 2 shipped the overflow logic). If the exact-capacity case fails, re-check the `data_len == data_buf_len` conditions in `value_byte`/`commit_value` against the "capacity = N-1 payload + trailing separator" rule and fix.

- [ ] **Step 3: Commit**

```bash
git add tests/test_parser_limits.c tests/CMakeLists.txt src/sse_parser.c
git commit -m "test: pin parser overflow policy and buffer limits"
```

---

### Task 6: Parser fuzz harness

**Files:**
- Create: `fuzz/fuzz_parser.c`, `fuzz/README.md`
- Modify: `CMakeLists.txt` (add `SSE_BUILD_FUZZERS` option)

**Interfaces:**
- Consumes: parser API (Task 1).
- Produces: `fuzz/fuzz_parser.c` with `LLVMFuzzerTestOneInput`; build via `-DSSE_BUILD_FUZZERS=ON` (requires clang).

- [ ] **Step 1: Write the harness**

`fuzz/fuzz_parser.c`:

```c
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
```

`fuzz/README.md`:

```markdown
# Fuzzing

Requires clang.

    cmake -B build-fuzz -DSSE_BUILD_FUZZERS=ON -DSSE_BUILD_TESTS=OFF -DCMAKE_C_COMPILER=clang
    cmake --build build-fuzz
    ./build-fuzz/fuzz_parser -max_total_time=60

Any crash or sanitizer report is a bug in the parser; minimize with
`./build-fuzz/fuzz_parser <crash-file> -minimize_crash=1`.
```

- [ ] **Step 2: Add the CMake option**

Append to root `CMakeLists.txt` (host section, after the tests block):

```cmake
option(SSE_BUILD_FUZZERS "Build libFuzzer harnesses (needs clang)" OFF)
if(SSE_BUILD_FUZZERS)
  add_executable(fuzz_parser fuzz/fuzz_parser.c src/sse_parser.c)
  target_include_directories(fuzz_parser PRIVATE include)
  target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
```

- [ ] **Step 3: Build and run a short fuzz as verification**

Run:

```bash
cmake -B build-fuzz -DSSE_BUILD_FUZZERS=ON -DSSE_BUILD_TESTS=OFF -DSSE_SANITIZE=OFF -DCMAKE_C_COMPILER=clang
cmake --build build-fuzz
./build-fuzz/fuzz_parser -max_total_time=30
```

Expected: runs to the time limit with `Done` and no crash. If it crashes, fix the parser bug it found before committing (the crash input is written to the working directory; reproduce with `./build-fuzz/fuzz_parser <file>`).

- [ ] **Step 4: Commit**

```bash
git add fuzz/ CMakeLists.txt
git commit -m "test: libFuzzer harness for the parser"
```

---

### Task 7: Transport + client headers, mock transport, connect happy path

**Files:**
- Create: `include/eventsource/sse_transport.h`, `include/eventsource/sse_client.h`, `src/sse_client.c`
- Create: `tests/mock_transport.h`
- Test: `tests/test_client_basic.c`
- Modify: root `CMakeLists.txt` (add `src/sse_client.c` to both the host library and the `idf_component_register` SRCS), `tests/CMakeLists.txt` (add `sse_add_test(client_basic)`)

**Interfaces:**
- Consumes: parser API and pinned semantics (Tasks 1-5).
- Produces (public, used by every later task):
  - `sse_transport_t { void *ctx; int (*open)(void *ctx, const sse_request_t *req, sse_response_info_t *out); int (*read)(void *ctx, void *buf, size_t len, uint32_t timeout_ms); void (*close)(void *ctx); }`
  - `sse_request_t { const char *url; const char *const *headers; }`
  - `sse_response_info_t { int status_code; char content_type[64]; int32_t retry_after_s; }` (raw Content-Type value; the client matches it case-insensitively and ignores parameters)
  - `SSE_READ_TIMEOUT (0)`, `SSE_READ_EOF (-1)`, `SSE_READ_ERROR (-2)`
  - `int sse_client_init(sse_client_t *c, const sse_client_config_t *cfg)` (0 ok, -1 invalid config)
  - `uint32_t sse_client_poll(sse_client_t *c)`; `void sse_client_close(sse_client_t *c)`; `void sse_client_request_stop(sse_client_t *c)`; `sse_client_state_t sse_client_state(const sse_client_t *c)`; `const char *sse_client_last_event_id(const sse_client_t *c)`
  - `sse_client_state_t`: `SSE_STATE_IDLE=0, SSE_STATE_CONNECTING, SSE_STATE_OPEN, SSE_STATE_WAITING_RETRY, SSE_STATE_CLOSED`
  - `sse_error_reason_t`: `SSE_ERR_TRANSPORT=0, SSE_ERR_HTTP_STATUS, SSE_ERR_BAD_CONTENT_TYPE, SSE_ERR_SERVER_STOP, SSE_ERR_IDLE_TIMEOUT, SSE_ERR_MESSAGE_TOO_LARGE`
- Produces (test-side): `tests/mock_transport.h` with `mock_state_t`, `mock_reset()`, `mock_transport()`, scripted connections, captured request headers in `mock.captured_headers`, and the fake clock `g_now` / `fake_now()`.

- [ ] **Step 1: Write `include/eventsource/sse_transport.h`**

```c
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
```

- [ ] **Step 2: Write `include/eventsource/sse_client.h`**

```c
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

  sse_parser_buffers_t buffers; /* caller-provided parser buffers */
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
  volatile bool stop_requested;
  bool transport_open;
  unsigned attempts; /* failures since last delivered message */
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
```

- [ ] **Step 3: Write the mock transport**

`tests/mock_transport.h`:

```c
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
```

- [ ] **Step 4: Write the failing happy-path tests**

`tests/test_client_basic.c`:

```c
#include "eventsource/sse_client.h"
#include "mock_transport.h"
#include "t.h"
#include <stdarg.h>

static char clog[4096];
static size_t clog_len;
static void cadd(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(clog + clog_len, sizeof clog - clog_len, fmt, ap);
  va_end(ap);
  if (n > 0) clog_len += (size_t)n;
}
static void c_open(void *ud, unsigned rc) { (void)ud; cadd("open(%u)\n", rc); }
static void c_msg(void *ud, const sse_message_t *m) {
  (void)ud;
  cadd("msg(%s,%s,%s)\n", m->event, m->last_event_id, m->data);
}
static void c_err(void *ud, const sse_error_t *e) {
  (void)ud;
  cadd("err(%d,%d,%d,%u)\n", (int)e->reason, e->http_status, (int)e->will_retry,
       (unsigned)e->retry_in_ms);
}
static void c_closed(void *ud) { (void)ud; cadd("closed\n"); }

static char db[1024], ib[128], eb[64];
static uint8_t rx[256];
static sse_client_t cl;

static sse_client_config_t base_cfg(void) {
  sse_client_config_t cfg = {0};
  cfg.url = "http://test.local/stream";
  cfg.buffers.data_buf = db; cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;   cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx; cfg.rx_buf_len = sizeof rx;
  cfg.transport = mock_transport();
  cfg.now_ms = fake_now;
  cfg.callbacks.on_open = c_open;
  cfg.callbacks.on_message = c_msg;
  cfg.callbacks.on_error = c_err;
  cfg.callbacks.on_closed = c_closed;
  return cfg;
}

static void fresh(void) {
  clog[0] = 0; clog_len = 0;
  mock_reset();
}

/* Poll until the client would sleep or is closed, at most `n` times. */
static void pump(int n) {
  while (n-- > 0) {
    uint32_t s = sse_client_poll(&cl);
    if (s != 0) break;
  }
}

int main(void) {
  /* invalid configs are rejected */
  {
    sse_client_config_t cfg = {0};
    OK_INT(sse_client_init(&cl, &cfg), -1);
    cfg = base_cfg();
    cfg.url = NULL;
    OK_INT(sse_client_init(&cl, &cfg), -1);
  }

  /* happy path: connect, open, one message, default event type */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].status = 200;
  mock.conns[0].content_type = "text/event-stream";
  mock.conns[0].retry_after_s = -1;
  mock.conns[0].chunks[0] = "data: hello\n\n";
  mock.conns[0].tail = SSE_READ_TIMEOUT;
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    OK_INT(sse_client_state(&cl), SSE_STATE_IDLE);
    pump(10);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
    OK_STR(clog, "open(0)\nmsg(message,,hello)\n");
    /* request headers were composed; no Last-Event-ID on first connect */
    OK(strstr(mock.captured_headers, "Accept: text/event-stream|") != NULL);
    OK(strstr(mock.captured_headers, "Cache-Control: no-store|") != NULL);
    OK(strstr(mock.captured_headers, "Last-Event-ID") == NULL);
  }

  /* named events, id tracking, extra headers, content-type with params */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].status = 200;
  mock.conns[0].content_type = "Text/Event-Stream; charset=utf-8";
  mock.conns[0].retry_after_s = -1;
  mock.conns[0].chunks[0] = "id: 41\nevent: tick\ndata: a\n\n";
  mock.conns[0].chunks[1] = "data: b\n\n";
  mock.conns[0].tail = SSE_READ_TIMEOUT;
  {
    static const char *extra[] = {"Authorization: Bearer xyz", NULL};
    sse_client_config_t cfg = base_cfg();
    cfg.extra_headers = extra;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    /* lastEventId persists into the second message even without an id field */
    OK_STR(clog, "open(0)\nmsg(tick,41,a)\nmsg(message,41,b)\n");
    OK_STR(sse_client_last_event_id(&cl), "41");
    OK(strstr(mock.captured_headers, "Authorization: Bearer xyz|") != NULL);
  }

  /* close(): transport closed, on_closed fired once, poll returns UINT32_MAX */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].status = 200;
  mock.conns[0].content_type = "text/event-stream";
  mock.conns[0].retry_after_s = -1;
  mock.conns[0].tail = SSE_READ_TIMEOUT;
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(3);
    sse_client_close(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
    OK_INT(mock.close_calls >= 1, 1);
    OK(sse_client_poll(&cl) == UINT32_MAX);
    sse_client_close(&cl); /* idempotent */
    OK(strstr(clog, "closed\n") != NULL);
    OK(strstr(clog, "closed\nclosed") == NULL);
  }

  T_END();
}
```

Add to `tests/CMakeLists.txt`: `sse_add_test(client_basic)`
Add `src/sse_client.c` to the host library line in root `CMakeLists.txt`: `add_library(eventsource src/sse_parser.c src/sse_client.c)` and to `idf_component_register(SRCS ...)`.

- [ ] **Step 5: Run test to verify it fails**

Run: `cmake -B build && cmake --build build`
Expected: FAIL to compile/link (`sse_client.c` missing).

- [ ] **Step 6: Implement the client (connection lifecycle subset)**

`src/sse_client.c`:

```c
#include "eventsource/sse_client.h"
#include <stdio.h>
#include <string.h>

/* ---- parser callback bridge ---- */

static void bridge_on_id(void *ud, const char *id, size_t len) {
  sse_client_t *c = ud;
  if (len <= SSE_CLIENT_ID_MAX) {
    memcpy(c->last_event_id, id, len + 1); /* parser NUL-terminates */
    c->last_event_id_len = len;
  }
}

static void bridge_on_retry(void *ud, uint32_t ms) {
  ((sse_client_t *)ud)->base_retry_ms = ms;
}

static void bridge_on_event(void *ud, const sse_parser_event_t *ev) {
  sse_client_t *c = ud;
  c->attempts = 0; /* healthy connection: reset flap counter */
  if (!c->cfg.callbacks.on_message) return;
  sse_message_t m;
  m.event = ev->event ? ev->event : "message";
  m.data = ev->data;
  m.data_len = ev->data_len;
  m.last_event_id = c->last_event_id;
  c->cfg.callbacks.on_message(c->cfg.callbacks.userdata, &m);
}

static void bridge_on_perr(void *ud, sse_parse_error_t err) {
  sse_client_t *c = ud;
  if (err != SSE_PARSE_ERR_DATA_TOO_LARGE) return;
  if (!c->cfg.callbacks.on_error) return;
  sse_error_t e = {SSE_ERR_MESSAGE_TOO_LARGE, 0, true, 0}; /* stream continues */
  c->cfg.callbacks.on_error(c->cfg.callbacks.userdata, &e);
}

/* ---- helpers ---- */

static uint32_t prng_next(sse_client_t *c) {
  uint32_t x = c->prng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  c->prng = x ? x : 0x9e3779b9u;
  return c->prng;
}

static void close_transport(sse_client_t *c) {
  if (c->transport_open) {
    c->cfg.transport->close(c->cfg.transport->ctx);
    c->transport_open = false;
  }
}

static void to_closed(sse_client_t *c) {
  close_transport(c);
  if (c->state == SSE_STATE_CLOSED) return;
  c->state = SSE_STATE_CLOSED;
  if (c->cfg.callbacks.on_closed) c->cfg.callbacks.on_closed(c->cfg.callbacks.userdata);
}

static bool ct_is_event_stream(const char *ct) {
  static const char want[] = "text/event-stream";
  size_t i = 0;
  for (; want[i]; i++) {
    char a = ct[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (a != want[i]) return false;
  }
  return ct[i] == '\0' || ct[i] == ';' || ct[i] == ' ';
}

static void build_headers(sse_client_t *c) {
  size_t n = 0;
  c->headers[n++] = "Accept: text/event-stream";
  c->headers[n++] = "Cache-Control: no-store";
  if (c->last_event_id_len > 0) {
    snprintf(c->id_header, sizeof c->id_header, "Last-Event-ID: %s", c->last_event_id);
    c->headers[n++] = c->id_header;
  }
  if (c->cfg.extra_headers) {
    for (const char *const *h = c->cfg.extra_headers; *h; h++) c->headers[n++] = *h;
  }
  c->headers[n] = NULL;
}

static bool default_should_retry(sse_error_reason_t reason, int status, int32_t retry_after_s) {
  switch (reason) {
    case SSE_ERR_TRANSPORT:
    case SSE_ERR_IDLE_TIMEOUT:
    case SSE_ERR_BAD_CONTENT_TYPE:
      return true;
    case SSE_ERR_SERVER_STOP:
      return false;
    case SSE_ERR_HTTP_STATUS:
      if (status >= 500) return true;
      if (status == 429) return true;
      return retry_after_s >= 0;
    default:
      return false;
  }
}

static void fail_conn(sse_client_t *c, sse_error_reason_t reason, int status,
                      int32_t retry_after_s) {
  close_transport(c);
  sse_error_t e;
  e.reason = reason;
  e.http_status = status;
  e.retry_in_ms = 0;
  bool retry = default_should_retry(reason, status, retry_after_s);
  if (c->cfg.reconnect_policy) {
    e.will_retry = retry;
    retry = c->cfg.reconnect_policy(c->cfg.callbacks.userdata, &e);
  }
  if (!retry) {
    e.will_retry = false;
    if (c->cfg.callbacks.on_error) c->cfg.callbacks.on_error(c->cfg.callbacks.userdata, &e);
    to_closed(c);
    return;
  }
  uint32_t delay;
  if (retry_after_s >= 0) {
    delay = (uint32_t)retry_after_s * 1000u;
  } else if (c->cfg.max_retry_ms == 0) {
    delay = c->base_retry_ms;
  } else {
    unsigned sh = c->attempts > 15 ? 15 : c->attempts;
    uint64_t d = (uint64_t)c->base_retry_ms << sh;
    delay = d > c->cfg.max_retry_ms ? c->cfg.max_retry_ms : (uint32_t)d;
  }
  if (c->cfg.jitter_pct) {
    delay += (uint32_t)(((uint64_t)delay * (prng_next(c) % (c->cfg.jitter_pct + 1u))) / 100u);
  }
  c->attempts++;
  c->retry_deadline = c->cfg.now_ms() + delay;
  c->state = SSE_STATE_WAITING_RETRY;
  e.will_retry = true;
  e.retry_in_ms = delay;
  if (c->cfg.callbacks.on_error) c->cfg.callbacks.on_error(c->cfg.callbacks.userdata, &e);
}

static void do_connect(sse_client_t *c) {
  c->state = SSE_STATE_CONNECTING;
  build_headers(c);
  sse_request_t req;
  req.url = c->cfg.url;
  req.headers = c->headers;
  sse_response_info_t info;
  info.status_code = 0;
  info.content_type[0] = '\0';
  info.retry_after_s = -1;
  if (c->cfg.transport->open(c->cfg.transport->ctx, &req, &info) != 0) {
    fail_conn(c, SSE_ERR_TRANSPORT, 0, -1);
    return;
  }
  c->transport_open = true;
  if (info.status_code == 204) {
    fail_conn(c, SSE_ERR_SERVER_STOP, 204, -1);
    return;
  }
  if (info.status_code != 200) {
    fail_conn(c, SSE_ERR_HTTP_STATUS, info.status_code, info.retry_after_s);
    return;
  }
  if (!c->cfg.skip_content_type_check && !ct_is_event_stream(info.content_type)) {
    fail_conn(c, SSE_ERR_BAD_CONTENT_TYPE, info.status_code, -1);
    return;
  }
  sse_parser_reset(&c->parser);
  c->last_rx_ms = c->cfg.now_ms();
  c->state = SSE_STATE_OPEN;
  if (c->cfg.callbacks.on_open) {
    c->cfg.callbacks.on_open(c->cfg.callbacks.userdata, c->attempts);
  }
}

/* ---- public API ---- */

int sse_client_init(sse_client_t *c, const sse_client_config_t *cfg) {
  if (!c || !cfg || !cfg->url || !cfg->transport || !cfg->now_ms) return -1;
  if (!cfg->transport->open || !cfg->transport->read || !cfg->transport->close) return -1;
  if (!cfg->buffers.data_buf || cfg->buffers.data_buf_len < 2) return -1;
  if (!cfg->buffers.id_buf || cfg->buffers.id_buf_len < 2) return -1;
  if (!cfg->buffers.event_buf || cfg->buffers.event_buf_len < 2) return -1;
  if (!cfg->rx_buf || cfg->rx_buf_len == 0) return -1;
  size_t extra = 0;
  if (cfg->extra_headers) {
    for (const char *const *h = cfg->extra_headers; *h; h++) extra++;
  }
  if (3 + extra > SSE_CLIENT_MAX_HEADERS) return -1;

  memset(c, 0, sizeof *c);
  c->cfg = *cfg;
  if (c->cfg.default_retry_ms == 0) c->cfg.default_retry_ms = 3000;
  if (c->cfg.read_timeout_ms == 0) c->cfg.read_timeout_ms = 500;
  c->base_retry_ms = c->cfg.default_retry_ms;
  c->prng = c->cfg.now_ms() ^ 0xA5A5A5A5u;
  if (!c->prng) c->prng = 1;

  sse_parser_callbacks_t pcb = {c, bridge_on_event, bridge_on_id, bridge_on_retry,
                                bridge_on_perr};
  sse_parser_init(&c->parser, &pcb, &c->cfg.buffers);
  c->state = SSE_STATE_IDLE;
  return 0;
}

uint32_t sse_client_poll(sse_client_t *c) {
  if (c->stop_requested && c->state != SSE_STATE_CLOSED) to_closed(c);
  switch ((sse_client_state_t)c->state) {
    case SSE_STATE_CLOSED:
      return UINT32_MAX;
    case SSE_STATE_IDLE:
    case SSE_STATE_CONNECTING:
      do_connect(c);
      return 0;
    case SSE_STATE_WAITING_RETRY: {
      int32_t remain = (int32_t)(c->retry_deadline - c->cfg.now_ms());
      if (remain <= 0) {
        c->state = SSE_STATE_CONNECTING;
        return 0;
      }
      return (uint32_t)remain;
    }
    case SSE_STATE_OPEN: {
      int r = c->cfg.transport->read(c->cfg.transport->ctx, c->cfg.rx_buf,
                                     c->cfg.rx_buf_len, c->cfg.read_timeout_ms);
      if (c->stop_requested) {
        to_closed(c);
        return UINT32_MAX;
      }
      if (r > 0) {
        c->last_rx_ms = c->cfg.now_ms();
        sse_parser_feed(&c->parser, c->cfg.rx_buf, (size_t)r);
        return 0;
      }
      if (r == SSE_READ_TIMEOUT) {
        if (c->cfg.idle_timeout_ms != 0 &&
            (uint32_t)(c->cfg.now_ms() - c->last_rx_ms) >= c->cfg.idle_timeout_ms) {
          fail_conn(c, SSE_ERR_IDLE_TIMEOUT, 0, -1);
        }
        return c->cfg.read_timeout_ms;
      }
      fail_conn(c, SSE_ERR_TRANSPORT, 0, -1); /* EOF or error */
      return 0;
    }
  }
  return 0;
}

void sse_client_close(sse_client_t *c) { to_closed(c); }

void sse_client_request_stop(sse_client_t *c) { c->stop_requested = true; }

sse_client_state_t sse_client_state(const sse_client_t *c) {
  return (sse_client_state_t)c->state;
}

const char *sse_client_last_event_id(const sse_client_t *c) {
  return c->last_event_id;
}
```

Note on the `pump()` helper in tests: after feeding a chunk, poll returns 0, so `pump` keeps going; when the mock returns `SSE_READ_TIMEOUT`, poll returns `read_timeout_ms` (non-zero) and `pump` stops. That is also why the OPEN/timeout branch returns `read_timeout_ms` rather than 0.

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all suites PASS, including `client_basic`.

- [ ] **Step 8: Commit**

```bash
git add include/eventsource/sse_transport.h include/eventsource/sse_client.h src/sse_client.c \
        tests/mock_transport.h tests/test_client_basic.c tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: sse_client core with transport vtable, happy path connect/dispatch"
```

---

### Task 8: Reconnect engine (EOF, delays, backoff, Retry-After, Last-Event-ID resume)

**Files:**
- Modify: `src/sse_client.c` (only if tests expose defects; the engine shipped in Task 7)
- Test: `tests/test_client_reconnect.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(client_reconnect)`)

**Interfaces:**
- Consumes: client API + mock transport (Task 7). Mock scripting: consecutive `mock.conns[i]` entries are served across reconnects; `mock.captured_headers` holds the latest open's headers.
- Produces: pinned reconnect behavior for Tasks 9-12.

- [ ] **Step 1: Write the tests**

`tests/test_client_reconnect.c` (same `cadd`/callback scaffolding as `tests/test_client_basic.c`; copy the static helpers verbatim, they are file-local by design):

```c
#include "eventsource/sse_client.h"
#include "mock_transport.h"
#include "t.h"
#include <stdarg.h>

static char clog[4096];
static size_t clog_len;
static void cadd(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(clog + clog_len, sizeof clog - clog_len, fmt, ap);
  va_end(ap);
  if (n > 0) clog_len += (size_t)n;
}
static void c_open(void *ud, unsigned rc) { (void)ud; cadd("open(%u)\n", rc); }
static void c_msg(void *ud, const sse_message_t *m) {
  (void)ud;
  cadd("msg(%s,%s,%s)\n", m->event, m->last_event_id, m->data);
}
static void c_err(void *ud, const sse_error_t *e) {
  (void)ud;
  cadd("err(%d,%d,%d,%u)\n", (int)e->reason, e->http_status, (int)e->will_retry,
       (unsigned)e->retry_in_ms);
}
static void c_closed(void *ud) { (void)ud; cadd("closed\n"); }

static char db[1024], ib[128], eb[64];
static uint8_t rx[256];
static sse_client_t cl;

static sse_client_config_t base_cfg(void) {
  sse_client_config_t cfg = {0};
  cfg.url = "http://test.local/stream";
  cfg.buffers.data_buf = db; cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;   cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx; cfg.rx_buf_len = sizeof rx;
  cfg.transport = mock_transport();
  cfg.now_ms = fake_now;
  cfg.callbacks.on_open = c_open;
  cfg.callbacks.on_message = c_msg;
  cfg.callbacks.on_error = c_err;
  cfg.callbacks.on_closed = c_closed;
  return cfg;
}

static void fresh(void) { clog[0] = 0; clog_len = 0; mock_reset(); }

static void pump(int n) {
  while (n-- > 0) {
    if (sse_client_poll(&cl) != 0) break;
  }
}

static void sse_conn(int i, int status, const char *ct, const char *body, int tail) {
  mock.conns[i].status = status;
  mock.conns[i].content_type = ct;
  mock.conns[i].retry_after_s = -1;
  mock.conns[i].chunks[0] = body;
  mock.conns[i].tail = tail;
}

int main(void) {
  /* EOF -> WAITING_RETRY with flat default delay (3000), then reconnect;
   * reconnect_count 1; Last-Event-ID header sent; parser reset between
   * connections (partial event must not leak). */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/event-stream", "id: 42\ndata: a\n\ndata: par", SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: tial\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg(); /* max_retry_ms 0 -> flat 3000 */
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK_INT(sse_client_state(&cl), SSE_STATE_WAITING_RETRY);
    OK_STR(clog, "open(0)\nmsg(message,42,a)\nerr(0,0,1,3000)\n");
    /* not yet due */
    OK(sse_client_poll(&cl) > 0);
    OK_INT(mock.open_calls, 1);
    /* due: advance the fake clock past the deadline */
    g_now += 3001;
    pump(10);
    OK_INT(mock.open_calls, 2);
    OK(strstr(mock.captured_headers, "Last-Event-ID: 42|") != NULL);
    /* partial "par" from conn 0 must NOT prefix "tial" */
    OK(strstr(clog, "open(1)\nmsg(message,42,tial)\n") != NULL);
    OK_STR(sse_client_last_event_id(&cl), "42");
  }

  /* server-sent retry: overrides the base delay */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/event-stream", "retry: 10\ndata: x\n\n", SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: y\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK(strstr(clog, "err(0,0,1,10)\n") != NULL);
  }

  /* backoff: two failed attempts double the delay; message resets counter.
   * conn0 connect-error, conn1 connect-error, conn2 delivers. base=100. */
  fresh();
  mock.n_conns = 3;
  mock.conns[0].open_result = -1;
  mock.conns[1].open_result = -1;
  sse_conn(2, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 100;
    cfg.max_retry_ms = 30000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5); /* attempt 0 fails: delay 100 << 0 = 100 */
    OK(strstr(clog, "err(0,0,1,100)\n") != NULL);
    g_now += 101;
    pump(5); /* attempt 1 fails: delay 100 << 1 = 200 */
    OK(strstr(clog, "err(0,0,1,200)\n") != NULL);
    g_now += 201;
    pump(10);
    OK(strstr(clog, "open(2)\nmsg(message,,x)\n") != NULL);
  }

  /* backoff cap: max_retry_ms bounds the doubled delay */
  fresh();
  mock.n_conns = 2;
  mock.conns[0].open_result = -1;
  mock.conns[1].open_result = -1;
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 100;
    cfg.max_retry_ms = 150;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    g_now += 101;
    pump(5); /* second attempt: min(200, 150) = 150 */
    OK(strstr(clog, "err(0,0,1,150)\n") != NULL);
  }

  /* Retry-After overrides computed delay */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 503, "", NULL, SSE_READ_EOF);
  mock.conns[0].retry_after_s = 7;
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,503,1,7000)\n") != NULL);
    g_now += 7001;
    pump(10);
    OK(strstr(clog, "open(1)\n") != NULL);
  }

  /* jitter: delay lands in [base, base + base*pct/100] */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].open_result = -1;
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 1000;
    cfg.jitter_pct = 10;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    unsigned d = 0;
    OK_INT(sscanf(clog, "err(0,0,1,%u)", &d), 1);
    OK(d >= 1000 && d <= 1100);
  }

  T_END();
}
```

Add to `tests/CMakeLists.txt`: `sse_add_test(client_reconnect)`

- [ ] **Step 2: Run test**

Run: `cmake --build build && ctest --test-dir build -R client_reconnect --output-on-failure`
Expected: PASS against Task 7's implementation. If a delay assertion fails, check `fail_conn` against the Global Constraints delay formula and fix the implementation, not the test.

- [ ] **Step 3: Commit**

```bash
git add tests/test_client_reconnect.c tests/CMakeLists.txt src/sse_client.c
git commit -m "test: pin reconnect engine (backoff, retry field, Retry-After, resume)"
```

---

### Task 9: Reconnect policy matrix

**Files:**
- Modify: `src/sse_client.c` (only on test failure)
- Test: `tests/test_client_policy.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(client_policy)`)

**Interfaces:**
- Consumes: Tasks 7-8.
- Produces: pinned policy contract: 204 stops; plain 4xx stops; 4xx+Retry-After retries; 429 retries; 5xx retries; bad content type retries; `skip_content_type_check` bypasses; `reconnect_policy` hook overrides in both directions.

- [ ] **Step 1: Write the tests**

`tests/test_client_policy.c` (reuse the identical static scaffolding block from `tests/test_client_reconnect.c`: `cadd`, callbacks, `base_cfg`, `fresh`, `pump`, `sse_conn`):

```c
/* ... scaffolding identical to test_client_reconnect.c ... */

static bool policy_never(void *ud, const sse_error_t *e) {
  (void)ud; (void)e;
  return false;
}
static bool policy_always(void *ud, const sse_error_t *e) {
  (void)ud;
  cadd("hook(%d,%d,%d)\n", (int)e->reason, e->http_status, (int)e->will_retry);
  return true;
}

int main(void) {
  /* 204: SERVER_STOP, no retry, closed */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 204, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(3,204,0,0)\nclosed\n");
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
  }

  /* 404: stops permanently */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 404, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(1,404,0,0)\nclosed\n");
  }

  /* 401: stops permanently */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 401, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(1,401,0,0)\nclosed\n");
  }

  /* 404 + Retry-After: retries with the header's delay */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 404, "", NULL, SSE_READ_EOF);
  mock.conns[0].retry_after_s = 3;
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,404,1,3000)\n") != NULL);
  }

  /* 429 without Retry-After: still retries */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 429, "", NULL, SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,429,1,3000)\n") != NULL);
  }

  /* 500: retries */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 500, "", NULL, SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,500,1,3000)\n") != NULL);
  }

  /* wrong content type on 200: retries (captive-portal case) */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/html", "<html>portal</html>", SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(2,200,1,3000)\n") != NULL);
  }

  /* skip_content_type_check: text/html is accepted */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/html", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.skip_content_type_check = true;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK(strstr(clog, "msg(message,,x)\n") != NULL);
  }

  /* hook forces stop on a retryable error */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 500, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.reconnect_policy = policy_never;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(1,500,0,0)\nclosed\n");
  }

  /* hook forces retry on a non-retryable 401, sees the default decision */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 401, "", NULL, SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.reconnect_policy = policy_always;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "hook(1,401,0)\nerr(1,401,1,3000)\n") != NULL);
    g_now += 3001;
    pump(10);
    OK(strstr(clog, "open(1)\n") != NULL);
  }

  T_END();
}
```

Add to `tests/CMakeLists.txt`: `sse_add_test(client_policy)`

- [ ] **Step 2: Run test**

Run: `cmake --build build && ctest --test-dir build -R client_policy --output-on-failure`
Expected: PASS. On failure, fix `default_should_retry`/`fail_conn` per the Global Constraints policy table.

- [ ] **Step 3: Commit**

```bash
git add tests/test_client_policy.c tests/CMakeLists.txt src/sse_client.c
git commit -m "test: pin reconnect policy matrix (204/4xx/429/5xx/Retry-After/hook)"
```

---

### Task 10: Idle timeout, oversized-message forwarding, cross-task stop

**Files:**
- Modify: `src/sse_client.c` (only on test failure)
- Test: `tests/test_client_lifecycle.c`
- Modify: `tests/CMakeLists.txt` (add `sse_add_test(client_lifecycle)`)

**Interfaces:**
- Consumes: Tasks 7-9. Mock `tail = SSE_READ_TIMEOUT` simulates a silent-but-open socket.
- Produces: pinned liveness/lifecycle behavior used by the ports and examples.

- [ ] **Step 1: Write the tests**

`tests/test_client_lifecycle.c` (same scaffolding block as `tests/test_client_reconnect.c`):

```c
/* ... scaffolding identical to test_client_reconnect.c ... */

int main(void) {
  /* idle timeout: silent socket for > idle_timeout_ms forces reconnect */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  sse_conn(1, 200, "text/event-stream", "data: y\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.idle_timeout_ms = 60000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK(strstr(clog, "msg(message,,x)\n") != NULL);
    /* below threshold: still open */
    g_now += 59999;
    sse_client_poll(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
    /* cross threshold */
    g_now += 2;
    sse_client_poll(&cl);
    OK(strstr(clog, "err(4,0,1,") != NULL); /* 4 == SSE_ERR_IDLE_TIMEOUT */
    OK_INT(sse_client_state(&cl), SSE_STATE_WAITING_RETRY);
    g_now += 3001;
    pump(10);
    OK(strstr(clog, "msg(message,,y)\n") != NULL);
  }

  /* idle timeout disabled (0): a silent socket stays open forever */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    g_now += 100000000u;
    sse_client_poll(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
  }

  /* received bytes reset the idle timer (heartbeat comments count) */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  mock.conns[0].chunks[1] = ": keepalive\n\n";
  {
    sse_client_config_t cfg = base_cfg();
    cfg.idle_timeout_ms = 60000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(2); /* consume chunk 0 only: poll(connect)=0, poll(read chunk0)=0 */
    g_now += 59000;
    pump(1); /* delivers the heartbeat chunk, resetting the timer */
    g_now += 59000; /* 118000 total > 60000, but only 59000 since heartbeat */
    sse_client_poll(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
  }

  /* oversized message: informational error, stream continues */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", NULL, SSE_READ_TIMEOUT);
  {
    static char big[2048];
    memset(big, 'a', sizeof big);
    memcpy(big, "data: ", 6);
    memcpy(big + sizeof big - 12, "\n\ndata: k\n\n", 12); /* includes NUL */
    mock.conns[0].chunks[0] = big;
    sse_client_config_t cfg = base_cfg(); /* data_buf is 1024: overflow */
    cfg.rx_buf_len = 256; /* mock caps chunk to rx len; multiple reads */
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(20);
    OK(strstr(clog, "err(5,0,1,0)\n") != NULL); /* 5 == SSE_ERR_MESSAGE_TOO_LARGE */
    OK(strstr(clog, "msg(message,,k)\n") != NULL); /* resynced */
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
  }

  /* request_stop from "another task": next poll closes cleanly */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    sse_client_request_stop(&cl);
    OK(sse_client_poll(&cl) == UINT32_MAX);
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
    OK(strstr(clog, "closed\n") != NULL);
    OK_INT(mock.close_calls >= 1, 1);
  }

  /* WAITING_RETRY poll returns the remaining delay as a sleep hint */
  fresh();
  mock.n_conns = 2;
  mock.conns[0].open_result = -1;
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 2000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    uint32_t hint = sse_client_poll(&cl);
    OK(hint > 0 && hint <= 2000);
    g_now += 500;
    uint32_t hint2 = sse_client_poll(&cl);
    OK(hint2 > 0 && hint2 <= 1500);
  }

  T_END();
}
```

Note on the oversized-message fixture: `memcpy(big + sizeof big - 12, "\n\ndata: k\n\n", 12)` copies the string plus its NUL; the mock uses `strlen`, so the trailing NUL just terminates the chunk. The payload is ~2030 'a' bytes against a 1024-byte data buffer.

Add to `tests/CMakeLists.txt`: `sse_add_test(client_lifecycle)`

- [ ] **Step 2: Run test**

Run: `cmake --build build && ctest --test-dir build -R client_lifecycle --output-on-failure`
Expected: PASS; on failure fix `sse_client_poll`'s OPEN branch (idle check uses wrap-safe `uint32_t` subtraction) or `bridge_on_perr`.

- [ ] **Step 3: Run the whole suite plus a fuzz smoke, then commit**

Run: `ctest --test-dir build --output-on-failure && ./build-fuzz/fuzz_parser -max_total_time=30`
Expected: all PASS, no fuzz crash.

```bash
git add tests/test_client_lifecycle.c tests/CMakeLists.txt src/sse_client.c
git commit -m "test: pin idle timeout, oversized-message forwarding, stop semantics"
```

---

### Task 11: POSIX libcurl transport, fixture server, example

**Files:**
- Create: `ports/posix/sse_transport_curl.h`, `ports/posix/sse_transport_curl.c`, `ports/posix/sse_clock_posix.h`, `ports/posix/sse_clock_posix.c`
- Create: `tools/sse_fixture_server.py`
- Create: `examples/posix/main.c`
- Modify: root `CMakeLists.txt`

**Interfaces:**
- Consumes: `sse_transport_t` contract (Task 7).
- Produces:
  - `sse_transport_t *sse_transport_curl_new(void)` / `void sse_transport_curl_free(sse_transport_t *t)` (ports may allocate)
  - `uint32_t sse_now_ms_posix(void)`

- [ ] **Step 1: Write the clock port**

`ports/posix/sse_clock_posix.h`:

```c
#ifndef SSE_CLOCK_POSIX_H
#define SSE_CLOCK_POSIX_H
#include <stdint.h>
uint32_t sse_now_ms_posix(void);
#endif
```

`ports/posix/sse_clock_posix.c`:

```c
#include "sse_clock_posix.h"
#include <time.h>

uint32_t sse_now_ms_posix(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
```

- [ ] **Step 2: Write the curl transport**

`ports/posix/sse_transport_curl.h`:

```c
#ifndef SSE_TRANSPORT_CURL_H
#define SSE_TRANSPORT_CURL_H
#include "eventsource/sse_transport.h"
sse_transport_t *sse_transport_curl_new(void);
void sse_transport_curl_free(sse_transport_t *t);
#endif
```

`ports/posix/sse_transport_curl.c`:

```c
#include "sse_transport_curl.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

#define RING_CAP (32 * 1024)
#define OPEN_TIMEOUT_MS 30000

typedef struct {
  CURLM *multi;
  CURL *easy;
  struct curl_slist *hdrs;
  char ring[RING_CAP];
  size_t r_head, r_len;
  int body_started;      /* first body byte seen: headers are final */
  int transfer_done;
  CURLcode transfer_result;
  int paused;
  int32_t retry_after_s;
} curl_ctx_t;

static size_t on_body(char *ptr, size_t sz, size_t nm, void *ud) {
  curl_ctx_t *t = ud;
  size_t n = sz * nm;
  t->body_started = 1;
  if (n > RING_CAP - t->r_len) {
    t->paused = 1;
    return CURL_WRITEFUNC_PAUSE;
  }
  for (size_t i = 0; i < n; i++) {
    t->ring[(t->r_head + t->r_len + i) % RING_CAP] = ptr[i];
  }
  t->r_len += n;
  return n;
}

static size_t on_header(char *ptr, size_t sz, size_t nm, void *ud) {
  curl_ctx_t *t = ud;
  size_t n = sz * nm;
  if (n > 5 && strncmp(ptr, "HTTP/", 5) == 0) {
    t->retry_after_s = -1; /* new response block (redirect chain) */
  } else if (n > 12 && strncasecmp(ptr, "Retry-After:", 12) == 0) {
    long v = strtol(ptr + 12, NULL, 10);
    if (v >= 0) t->retry_after_s = (int32_t)v; /* delta-seconds only */
  }
  return n;
}

/* Run curl for up to wait_ms; harvest completion. Returns 0 or -1. */
static int pump(curl_ctx_t *t, int wait_ms) {
  int running = 0;
  if (t->paused && t->r_len < RING_CAP / 2) {
    t->paused = 0;
    curl_easy_pause(t->easy, CURLPAUSE_CONT);
  }
  if (curl_multi_wait(t->multi, NULL, 0, wait_ms, NULL) != CURLM_OK) return -1;
  if (curl_multi_perform(t->multi, &running) != CURLM_OK) return -1;
  CURLMsg *msg;
  int left;
  while ((msg = curl_multi_info_read(t->multi, &left)) != NULL) {
    if (msg->msg == CURLMSG_DONE) {
      t->transfer_done = 1;
      t->transfer_result = msg->data.result;
    }
  }
  return 0;
}

static void teardown(curl_ctx_t *t) {
  if (t->easy && t->multi) curl_multi_remove_handle(t->multi, t->easy);
  if (t->easy) curl_easy_cleanup(t->easy);
  if (t->multi) curl_multi_cleanup(t->multi);
  if (t->hdrs) curl_slist_free_all(t->hdrs);
  memset(t, 0, sizeof *t);
  t->retry_after_s = -1;
}

static int curl_open_fn(void *vctx, const sse_request_t *req, sse_response_info_t *out) {
  curl_ctx_t *t = vctx;
  teardown(t);
  t->easy = curl_easy_init();
  t->multi = curl_multi_init();
  if (!t->easy || !t->multi) {
    teardown(t);
    return -1;
  }
  for (const char *const *h = req->headers; h && *h; h++) {
    t->hdrs = curl_slist_append(t->hdrs, *h);
  }
  curl_easy_setopt(t->easy, CURLOPT_URL, req->url);
  curl_easy_setopt(t->easy, CURLOPT_HTTPHEADER, t->hdrs);
  curl_easy_setopt(t->easy, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(t->easy, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(t->easy, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(t->easy, CURLOPT_HEADERDATA, t);
  curl_easy_setopt(t->easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(t->easy, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(t->easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_multi_add_handle(t->multi, t->easy);

  int waited = 0;
  while (!t->body_started && !t->transfer_done && waited < OPEN_TIMEOUT_MS) {
    if (pump(t, 100) < 0) {
      teardown(t);
      return -1;
    }
    waited += 100;
  }
  if (!t->body_started && !t->transfer_done) { /* header stall */
    teardown(t);
    return -1;
  }
  if (t->transfer_done && t->transfer_result != CURLE_OK && !t->body_started) {
    long code = 0;
    curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &code);
    if (code == 0) { /* no HTTP response at all: transport-level failure */
      teardown(t);
      return -1;
    }
  }
  long status = 0;
  curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &status);
  out->status_code = (int)status;
  const char *ct = NULL;
  curl_easy_getinfo(t->easy, CURLINFO_CONTENT_TYPE, &ct);
  snprintf(out->content_type, sizeof out->content_type, "%s", ct ? ct : "");
  out->retry_after_s = t->retry_after_s;
  return 0;
}

static int curl_read_fn(void *vctx, void *buf, size_t len, uint32_t timeout_ms) {
  curl_ctx_t *t = vctx;
  if (t->r_len == 0) {
    if (t->transfer_done) {
      return t->transfer_result == CURLE_OK ? SSE_READ_EOF : SSE_READ_ERROR;
    }
    if (pump(t, (int)timeout_ms) < 0) return SSE_READ_ERROR;
    if (t->r_len == 0) {
      if (t->transfer_done) {
        return t->transfer_result == CURLE_OK ? SSE_READ_EOF : SSE_READ_ERROR;
      }
      return SSE_READ_TIMEOUT;
    }
  }
  size_t n = t->r_len < len ? t->r_len : len;
  for (size_t i = 0; i < n; i++) {
    ((char *)buf)[i] = t->ring[(t->r_head + i) % RING_CAP];
  }
  t->r_head = (t->r_head + n) % RING_CAP;
  t->r_len -= n;
  return (int)n;
}

static void curl_close_fn(void *vctx) { teardown(vctx); }

sse_transport_t *sse_transport_curl_new(void) {
  static int global_done = 0;
  if (!global_done) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    global_done = 1;
  }
  sse_transport_t *tr = calloc(1, sizeof *tr);
  curl_ctx_t *ctx = calloc(1, sizeof *ctx);
  if (!tr || !ctx) {
    free(tr);
    free(ctx);
    return NULL;
  }
  ctx->retry_after_s = -1;
  tr->ctx = ctx;
  tr->open = curl_open_fn;
  tr->read = curl_read_fn;
  tr->close = curl_close_fn;
  return tr;
}

void sse_transport_curl_free(sse_transport_t *t) {
  if (!t) return;
  teardown(t->ctx);
  free(t->ctx);
  free(t);
}
```

- [ ] **Step 3: Write the fixture server**

`tools/sse_fixture_server.py`:

```python
#!/usr/bin/env python3
"""Minimal SSE fixture server. Usage: sse_fixture_server.py [port]"""
import http.server
import sys
import time


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # no chunked framing needed

    def do_GET(self):
        if self.path != "/stream":
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        last = self.headers.get("Last-Event-ID", "")
        n = int(last) if last.isdigit() else 0
        try:
            while True:
                n += 1
                self.wfile.write(f"id: {n}\ndata: tick {n}\n\n".encode())
                self.wfile.flush()
                if n % 5 == 0:
                    self.wfile.write(b": keepalive\n")
                    self.wfile.flush()
                time.sleep(1)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *a):
        pass


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
```

- [ ] **Step 4: Write the POSIX example**

`examples/posix/main.c`:

```c
#include "eventsource/sse_client.h"
#include "sse_clock_posix.h"
#include "sse_transport_curl.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static char db[8192], ib[128], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

static void on_open(void *ud, unsigned rc) {
  (void)ud;
  printf("* open (reconnect_count=%u)\n", rc);
}
static void on_message(void *ud, const sse_message_t *m) {
  (void)ud;
  printf("[%s] (id=%s) %.*s\n", m->event, m->last_event_id, (int)m->data_len, m->data);
}
static void on_error(void *ud, const sse_error_t *e) {
  (void)ud;
  printf("* error reason=%d status=%d will_retry=%d retry_in=%ums\n", (int)e->reason,
         e->http_status, (int)e->will_retry, (unsigned)e->retry_in_ms);
}
static void on_closed(void *ud) {
  (void)ud;
  printf("* closed\n");
}
static void on_sigint(int sig) {
  (void)sig;
  sse_client_request_stop(&client);
}

int main(int argc, char **argv) {
  const char *url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/stream";
  sse_transport_t *tr = sse_transport_curl_new();
  if (!tr) return 1;

  sse_client_config_t cfg = {0};
  cfg.url = url;
  cfg.buffers.data_buf = db; cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;   cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx; cfg.rx_buf_len = sizeof rx;
  cfg.max_retry_ms = 30000;
  cfg.jitter_pct = 10;
  cfg.idle_timeout_ms = 60000;
  cfg.transport = tr;
  cfg.now_ms = sse_now_ms_posix;
  cfg.callbacks.on_open = on_open;
  cfg.callbacks.on_message = on_message;
  cfg.callbacks.on_error = on_error;
  cfg.callbacks.on_closed = on_closed;

  if (sse_client_init(&client, &cfg) != 0) {
    fprintf(stderr, "invalid config\n");
    return 1;
  }
  signal(SIGINT, on_sigint);

  for (;;) {
    uint32_t s = sse_client_poll(&client);
    if (s == UINT32_MAX) break;
    if (s > 0) usleep((s > 250 ? 250 : s) * 1000u); /* stay responsive to ^C */
  }
  sse_transport_curl_free(tr);
  return 0;
}
```

- [ ] **Step 5: Wire into CMake**

Append to root `CMakeLists.txt` (host section):

```cmake
option(SSE_BUILD_POSIX_PORT "Build the libcurl POSIX transport + example" ON)
if(SSE_BUILD_POSIX_PORT)
  find_package(CURL REQUIRED)
  add_library(eventsource_posix ports/posix/sse_transport_curl.c ports/posix/sse_clock_posix.c)
  target_include_directories(eventsource_posix PUBLIC ports/posix)
  target_link_libraries(eventsource_posix PUBLIC eventsource CURL::libcurl)
  target_compile_options(eventsource_posix PRIVATE -Wall -Wextra -Werror)

  add_executable(example_posix examples/posix/main.c)
  target_link_libraries(example_posix PRIVATE eventsource_posix)
endif()
```

- [ ] **Step 6: Build and smoke test manually**

Run:

```bash
cmake -B build && cmake --build build
python3 tools/sse_fixture_server.py 8080 &
FIXTURE_PID=$!
./build/example_posix http://127.0.0.1:8080/stream &
EXAMPLE_PID=$!
sleep 5
kill $EXAMPLE_PID
# restart-resume check: kill and restart the server while the example runs
kill $FIXTURE_PID
```

Expected: the example prints `* open (reconnect_count=0)` then `[message] (id=1) tick 1`, `tick 2`, ... Additionally run the full smoke by hand: start server, start example, kill the server, watch `* error ... will_retry=1`, restart the server, watch `* open (reconnect_count=1)` and ticks resuming from the last id + 1 (the fixture honors `Last-Event-ID`).

- [ ] **Step 7: Run the unit suite (nothing may regress), commit**

Run: `ctest --test-dir build --output-on-failure`

```bash
git add ports/posix/ tools/sse_fixture_server.py examples/posix/ CMakeLists.txt
git commit -m "feat: libcurl POSIX transport, fixture server, posix example"
```

---

### Task 12: ESP-IDF component: manifest, transport, task wrapper, example

**Files:**
- Create: `idf_component.yml`, `ports/esp-idf/sse_transport_esp.h`, `ports/esp-idf/sse_transport_esp.c`, `ports/esp-idf/sse_client_task.h`, `ports/esp-idf/sse_client_task.c`
- Create: `examples/esp32/` (project: `CMakeLists.txt`, `main/CMakeLists.txt`, `main/main.c`, `main/Kconfig.projbuild`, `main/idf_component.yml`)
- Modify: root `CMakeLists.txt` (ESP branch)

**Interfaces:**
- Consumes: `sse_transport_t` contract (Task 7), client API (Task 7).
- Produces:
  - `sse_transport_t *sse_transport_esp_http_client(void)` / `void sse_transport_esp_http_client_free(sse_transport_t *t)`
  - `int sse_client_start_task(sse_client_t *c, const char *name, uint32_t stack_bytes, unsigned priority)` (0 ok, -1 fail; task self-deletes when the client reaches CLOSED)

- [ ] **Step 1: Write the component manifest**

`idf_component.yml` (repo root):

```yaml
version: "0.1.0"
description: "Server-Sent Events (EventSource) client: spec-accurate SSE parser, auto-reconnect, Last-Event-ID resume, zero heap in the core"
url: "https://github.com/rexxars/eventsource-c"
license: "MIT"
dependencies:
  idf: ">=5.0"
files:
  exclude:
    - "tests/**"
    - "fuzz/**"
    - "tools/**"
    - "ports/posix/**"
    - "examples/posix/**"
    - "docs/**"
    - "build*/**"
    - ".github/**"
```

(Adjust `url` if the repo lands elsewhere; it is metadata only.)

- [ ] **Step 2: Update the ESP branch of the root CMakeLists**

Replace the `if(ESP_PLATFORM)` block:

```cmake
if(ESP_PLATFORM)
  idf_component_register(
    SRCS
      src/sse_parser.c
      src/sse_client.c
      ports/esp-idf/sse_transport_esp.c
      ports/esp-idf/sse_client_task.c
    INCLUDE_DIRS include ports/esp-idf
    PRIV_REQUIRES esp_http_client mbedtls
  )
  return()
endif()
```

- [ ] **Step 3: Write the ESP transport**

`ports/esp-idf/sse_transport_esp.h`:

```c
#ifndef SSE_TRANSPORT_ESP_H
#define SSE_TRANSPORT_ESP_H
#include "eventsource/sse_transport.h"
sse_transport_t *sse_transport_esp_http_client(void);
void sse_transport_esp_http_client_free(sse_transport_t *t);
#endif
```

`ports/esp-idf/sse_transport_esp.c`:

```c
#include "sse_transport_esp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

typedef struct {
  esp_http_client_handle_t hc;
  char content_type[64];
  int32_t retry_after_s;
  uint32_t configured_timeout_ms;
} esp_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  esp_ctx_t *t = evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (strcasecmp(evt->header_key, "content-type") == 0) {
      snprintf(t->content_type, sizeof t->content_type, "%s", evt->header_value);
    } else if (strcasecmp(evt->header_key, "retry-after") == 0) {
      long v = strtol(evt->header_value, NULL, 10);
      if (v >= 0) t->retry_after_s = (int32_t)v;
    }
  }
  return ESP_OK;
}

static void esp_teardown(esp_ctx_t *t) {
  if (t->hc) {
    esp_http_client_close(t->hc);
    esp_http_client_cleanup(t->hc);
    t->hc = NULL;
  }
}

static int esp_open_fn(void *vctx, const sse_request_t *req, sse_response_info_t *out) {
  esp_ctx_t *t = vctx;
  esp_teardown(t);
  t->content_type[0] = '\0';
  t->retry_after_s = -1;

  esp_http_client_config_t cfg = {
      .url = req->url,
      .method = HTTP_METHOD_GET,
      .event_handler = on_http_event,
      .user_data = t,
      .timeout_ms = 10000, /* connect + header timeout; reads adjust below */
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 1024,
  };
  t->hc = esp_http_client_init(&cfg);
  if (!t->hc) return -1;

  for (const char *const *h = req->headers; h && *h; h++) {
    const char *colon = strchr(*h, ':');
    if (!colon) continue;
    char name[64];
    size_t nl = (size_t)(colon - *h);
    if (nl >= sizeof name) continue;
    memcpy(name, *h, nl);
    name[nl] = '\0';
    const char *val = colon + 1;
    while (*val == ' ') val++;
    esp_http_client_set_header(t->hc, name, val);
  }

  if (esp_http_client_open(t->hc, 0) != ESP_OK) {
    esp_teardown(t);
    return -1;
  }
  if (esp_http_client_fetch_headers(t->hc) < 0) {
    esp_teardown(t);
    return -1;
  }
  out->status_code = esp_http_client_get_status_code(t->hc);
  snprintf(out->content_type, sizeof out->content_type, "%s", t->content_type);
  out->retry_after_s = t->retry_after_s;
  t->configured_timeout_ms = 0; /* force set_timeout on first read */
  return 0;
}

static int esp_read_fn(void *vctx, void *buf, size_t len, uint32_t timeout_ms) {
  esp_ctx_t *t = vctx;
  if (!t->hc) return SSE_READ_ERROR;
  if (timeout_ms != t->configured_timeout_ms) {
    esp_http_client_set_timeout_ms(t->hc, (int)timeout_ms);
    t->configured_timeout_ms = timeout_ms;
  }
  errno = 0;
  int r = esp_http_client_read(t->hc, buf, (int)len);
  if (r > 0) return r;
  if (r == 0) {
    /* 0 can mean EOF or (for some transports) a poll timeout. Chunked SSE
     * streams report completion explicitly; trust that first. */
    return esp_http_client_is_complete_data_received(t->hc) ? SSE_READ_EOF
                                                            : SSE_READ_TIMEOUT;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
    return SSE_READ_TIMEOUT;
  }
  return SSE_READ_ERROR;
}

static void esp_close_fn(void *vctx) { esp_teardown(vctx); }

sse_transport_t *sse_transport_esp_http_client(void) {
  sse_transport_t *tr = calloc(1, sizeof *tr);
  esp_ctx_t *ctx = calloc(1, sizeof *ctx);
  if (!tr || !ctx) {
    free(tr);
    free(ctx);
    return NULL;
  }
  ctx->retry_after_s = -1;
  tr->ctx = ctx;
  tr->open = esp_open_fn;
  tr->read = esp_read_fn;
  tr->close = esp_close_fn;
  return tr;
}

void sse_transport_esp_http_client_free(sse_transport_t *t) {
  if (!t) return;
  esp_teardown(t->ctx);
  free(t->ctx);
  free(t);
}
```

Known risk to validate on hardware (this mapping is the one part of the plan not verifiable on the host): the `esp_http_client_read` return-0-vs-timeout disambiguation. If a real device shows EOF misreported as timeout on `Connection: close` streams (no chunking), extend the `r == 0` branch to also treat `!esp_http_client_is_chunked_response(t->hc) && content-length consumed` as EOF.

- [ ] **Step 4: Write the task wrapper**

`ports/esp-idf/sse_client_task.h`:

```c
#ifndef SSE_CLIENT_TASK_H
#define SSE_CLIENT_TASK_H
#include "eventsource/sse_client.h"
/* Runs sse_client_poll() in a FreeRTOS task. The task self-deletes when the
 * client reaches SSE_STATE_CLOSED (via sse_client_request_stop() or a
 * non-retryable failure). Returns 0 on success. */
int sse_client_start_task(sse_client_t *c, const char *name, uint32_t stack_bytes,
                          unsigned priority);
#endif
```

`ports/esp-idf/sse_client_task.c`:

```c
#include "sse_client_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void sse_task_main(void *arg) {
  sse_client_t *c = arg;
  while (sse_client_state(c) != SSE_STATE_CLOSED) {
    uint32_t s = sse_client_poll(c);
    if (s == UINT32_MAX) break;
    if (s > 0) {
      if (s > 250) s = 250; /* stay responsive to sse_client_request_stop() */
      vTaskDelay(pdMS_TO_TICKS(s));
    }
  }
  vTaskDelete(NULL);
}

int sse_client_start_task(sse_client_t *c, const char *name, uint32_t stack_bytes,
                          unsigned priority) {
  BaseType_t ok = xTaskCreate(sse_task_main, name, stack_bytes / sizeof(StackType_t), c,
                              (UBaseType_t)priority, NULL);
  return ok == pdPASS ? 0 : -1;
}
```

- [ ] **Step 5: Write the ESP32 example**

`examples/esp32/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sse_example)
```

`examples/esp32/main/idf_component.yml`:

```yaml
dependencies:
  eventsource:
    path: ../../..
```

`examples/esp32/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS main.c PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif)
```

`examples/esp32/main/Kconfig.projbuild`:

```
menu "SSE example configuration"
config SSE_EXAMPLE_WIFI_SSID
    string "WiFi SSID"
    default "myssid"
config SSE_EXAMPLE_WIFI_PASSWORD
    string "WiFi password"
    default "mypassword"
config SSE_EXAMPLE_URL
    string "SSE stream URL"
    default "https://example.com/stream"
endmenu
```

`examples/esp32/main/main.c`:

```c
#include "eventsource/sse_client.h"
#include "sse_client_task.h"
#include "sse_transport_esp.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "sse_example";

static char db[8192], ib[128], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void on_open(void *ud, unsigned rc) {
  (void)ud;
  ESP_LOGI(TAG, "open (reconnect_count=%u)", rc);
}
static void on_message(void *ud, const sse_message_t *m) {
  (void)ud;
  ESP_LOGI(TAG, "[%s] (id=%s) %.*s", m->event, m->last_event_id, (int)m->data_len, m->data);
}
static void on_error(void *ud, const sse_error_t *e) {
  (void)ud;
  ESP_LOGW(TAG, "error reason=%d status=%d will_retry=%d retry_in=%ums", (int)e->reason,
           e->http_status, (int)e->will_retry, (unsigned)e->retry_in_ms);
}
static void on_closed(void *ud) {
  (void)ud;
  ESP_LOGI(TAG, "closed");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
  }
}

static void wifi_connect_blocking(void) {
  wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event_handler, NULL));
  wifi_config_t wc = {0};
  strncpy((char *)wc.sta.ssid, CONFIG_SSE_EXAMPLE_WIFI_SSID, sizeof wc.sta.ssid - 1);
  strncpy((char *)wc.sta.password, CONFIG_SSE_EXAMPLE_WIFI_PASSWORD,
          sizeof wc.sta.password - 1);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
  ESP_LOGI(TAG, "wifi connected");
}

void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  wifi_connect_blocking();

  sse_transport_t *tr = sse_transport_esp_http_client();
  sse_client_config_t cfg = {0};
  cfg.url = CONFIG_SSE_EXAMPLE_URL;
  cfg.buffers.data_buf = db;
  cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;
  cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb;
  cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx;
  cfg.rx_buf_len = sizeof rx;
  cfg.max_retry_ms = 30000;
  cfg.jitter_pct = 10;
  cfg.idle_timeout_ms = 60000;
  cfg.transport = tr;
  cfg.now_ms = now_ms;
  cfg.callbacks.on_open = on_open;
  cfg.callbacks.on_message = on_message;
  cfg.callbacks.on_error = on_error;
  cfg.callbacks.on_closed = on_closed;

  ESP_ERROR_CHECK(sse_client_init(&client, &cfg) == 0 ? ESP_OK : ESP_FAIL);
  ESP_ERROR_CHECK(sse_client_start_task(&client, "sse", 6144, 5) == 0 ? ESP_OK : ESP_FAIL);
}
```

- [ ] **Step 6: Build gate**

If ESP-IDF (>= 5.0) is available in the environment (`idf.py --version` succeeds after sourcing the export script):

```bash
cd examples/esp32
idf.py set-target esp32
idf.py build
```

Expected: build succeeds. If ESP-IDF is NOT available locally, this gate moves to CI (Task 13 adds a build job) and to a manual on-target validation pass; note that clearly in the commit message.

Also re-run the host suite (`ctest --test-dir build --output-on-failure`); the ESP files must not affect the host build.

- [ ] **Step 7: Commit**

```bash
git add idf_component.yml ports/esp-idf/ examples/esp32/ CMakeLists.txt
git commit -m "feat: ESP-IDF component, esp_http_client transport, task wrapper, esp32 example"
```

---

### Task 13: README, CI, and registry publishing workflow

**Files:**
- Modify: `README.md` (replace stub)
- Create: `.github/workflows/ci.yml`, `.github/workflows/publish.yml`

**Interfaces:**
- Consumes: everything.
- Produces: the registry-facing README and the automation that publishes tags.

- [ ] **Step 1: Write the README**

Replace `README.md` with (do not hard-wrap):

```markdown
# eventsource-c

Server-Sent Events (EventSource) client for embedded C. Spec-accurate SSE parsing, automatic reconnection with `retry`/`Retry-After` support, `Last-Event-ID` resume, optional idle timeout for dead-connection detection, and zero heap allocation in the core. Primary target: ESP32 under ESP-IDF; the core is portable C99 and also builds on POSIX.

## Install (ESP-IDF)

    idf.py add-dependency "rexxars/eventsource"

## Usage

    #include "eventsource/sse_client.h"
    #include "sse_client_task.h"
    #include "sse_transport_esp.h"

    static char db[8192], ib[128], eb[64];
    static uint8_t rx[1024];
    static sse_client_t client;

    static void on_message(void *ud, const sse_message_t *m) {
      printf("[%s] %.*s\n", m->event, (int)m->data_len, m->data);
    }

    void start_stream(void) {
      sse_client_config_t cfg = {0};
      cfg.url = "https://example.com/stream";
      cfg.buffers.data_buf = db;  cfg.buffers.data_buf_len = sizeof db;
      cfg.buffers.id_buf = ib;    cfg.buffers.id_buf_len = sizeof ib;
      cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
      cfg.rx_buf = rx;            cfg.rx_buf_len = sizeof rx;
      cfg.max_retry_ms = 30000;
      cfg.jitter_pct = 10;
      cfg.idle_timeout_ms = 60000;
      cfg.transport = sse_transport_esp_http_client();
      cfg.now_ms = /* esp_timer_get_time()/1000 wrapper */;
      cfg.callbacks.on_message = on_message;
      sse_client_init(&client, &cfg);
      sse_client_start_task(&client, "sse", 6144, 5);
    }

See `examples/esp32` for a complete application and `examples/posix` for the host variant (libcurl). Design details live in PROPOSAL.md.

## Buffer sizing

- `data_buf` caps the event payload: a buffer of N bytes holds events up to N-1 bytes. Oversized events are dropped with `SSE_ERR_MESSAGE_TOO_LARGE` and the stream continues.
- `id_buf`/`event_buf`: 128 and 64 bytes are good defaults.
- All buffers are caller-provided; the core never allocates.

## Reconnect behavior

Retries transport failures, HTTP 5xx, 429, and any 4xx carrying `Retry-After`. Stops permanently on HTTP 204 and other 4xx. Override per-failure with the `reconnect_policy` hook. Delays honor the server's `retry:` field, with optional exponential backoff (`max_retry_ms`) and jitter (`jitter_pct`).

## License

MIT
```

- [ ] **Step 2: Write the CI workflow**

`.github/workflows/ci.yml`:

```yaml
name: CI
on:
  push:
    branches: [main]
  pull_request:

jobs:
  host:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev
      - run: cmake -B build -DCMAKE_BUILD_TYPE=Debug
      - run: cmake --build build
      - run: ctest --test-dir build --output-on-failure

  fuzz-smoke:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: cmake -B build-fuzz -DSSE_BUILD_FUZZERS=ON -DSSE_BUILD_TESTS=OFF -DSSE_BUILD_POSIX_PORT=OFF -DSSE_SANITIZE=OFF -DCMAKE_C_COMPILER=clang
      - run: cmake --build build-fuzz
      - run: ./build-fuzz/fuzz_parser -max_total_time=60

  esp-idf-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.2
          target: esp32
          path: examples/esp32
```

- [ ] **Step 3: Write the publish workflow**

`.github/workflows/publish.yml`:

```yaml
name: Publish to ESP Component Registry
on:
  push:
    tags: ["v*"]

jobs:
  publish:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: espressif/upload-components-ci-action@v1
        with:
          name: eventsource
          namespace: rexxars
          api_token: ${{ secrets.IDF_COMPONENT_API_TOKEN }}
```

Note for the human operator (do not automate): publishing requires the `IDF_COMPONENT_API_TOKEN` repo secret and a version bump in `idf_component.yml` before tagging. Tags are pushed manually.

- [ ] **Step 4: Full verification pass**

Run:

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
./build-fuzz/fuzz_parser -max_total_time=30
```

Expected: everything green, no fuzz findings.

- [ ] **Step 5: Commit**

```bash
git add README.md .github/
git commit -m "docs: README; ci: host tests, fuzz smoke, esp-idf build, registry publish"
```

---

## Self-review notes (already applied)

- Spec coverage checked against PROPOSAL.md: parser field semantics (Tasks 2-3), terminators/BOM/boundary determinism (Task 4), overflow policy (Task 5), fuzzing (Task 6), client state machine/headers/lastEventId (Task 7), reconnect delays incl. `retry:`/backoff/jitter/`Retry-After` (Task 8), policy matrix incl. hook (Task 9), idle timeout/`MESSAGE_TOO_LARGE`/stop (Task 10), libcurl port + fixture (Task 11), ESP-IDF component + `sse_client_start_task` + example (Task 12), README/CI/registry publish (Task 13). The parser `memchr` fast path is explicitly deferred by the spec and has no task.
- The `strict_content_type` -> `skip_content_type_check` rename was applied to PROPOSAL.md before this plan was written.
- Type/name consistency: `sse_client_state()` (function) vs `sse_client_state_t` (type) is intentional and legal C; callback struct field orders are stated in Task 1 and Task 7 Interfaces blocks and used consistently in positional initializers.
