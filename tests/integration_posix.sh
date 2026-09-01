#!/usr/bin/env bash
# POSIX/libcurl transport integration suite. Runs the example client against
# the fixture server and asserts on its output per scenario.
#
# Usage: integration_posix.sh <example_posix binary> <fixture server .py> <python3>
set -u

EXAMPLE=$1
SERVER=$2
PYTHON=$3

PORT=$(( (RANDOM % 20000) + 20000 ))
BASE="http://127.0.0.1:${PORT}"
OUTDIR=$(mktemp -d)
FAILS=0

"$PYTHON" "$SERVER" "$PORT" &
SRV_PID=$!
trap 'kill "$SRV_PID" 2>/dev/null; rm -rf "$OUTDIR"' EXIT
sleep 1

run_case() { # name path seconds
  local name=$1 path=$2 secs=$3
  "$EXAMPLE" "$BASE$path" > "$OUTDIR/$name.txt" 2>&1 &
  local pid=$!
  sleep "$secs"
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

expect() { # name pattern
  if ! grep -q "$2" "$OUTDIR/$1.txt"; then
    echo "FAIL [$1]: missing '$2'. Output:"
    sed 's/^/    /' "$OUTDIR/$1.txt"
    FAILS=$((FAILS + 1))
  fi
}

expect_not() { # name pattern
  if grep -q "$2" "$OUTDIR/$1.txt"; then
    echo "FAIL [$1]: unexpected '$2'. Output:"
    sed 's/^/    /' "$OUTDIR/$1.txt"
    FAILS=$((FAILS + 1))
  fi
}

# Plain stream: open, messages, ids.
run_case stream /stream 2
expect stream "open (reconnect_count=0)"
expect stream "(id=2) tick${PORT} 2"

# Headers-then-silence: open() must complete on headers alone (the stream
# stays quiet for 3 s), and the late event must still arrive.
run_case silent /silent 5
expect silent "open (reconnect_count=0)"
expect silent "(id=1) late 1"

# 103 Early Hints before the 200: the 1xx block must not complete the open.
run_case early /early 2
expect early "open (reconnect_count=0)"
expect early "early 1"
expect_not early "status=103"

# Same-origin redirect: followed transparently.
run_case redirect /redirect 2
expect redirect "open (reconnect_count=0)"
expect redirect "tick${PORT} 1"

# Same-origin redirect with a 40 KiB body (exceeds the transport's 32 KiB
# ring): the body must be discarded and the redirect still followed.
run_case bigredirect /bigredirect 3
expect bigredirect "open (reconnect_count=0)"
expect bigredirect "tick${PORT} 1"
expect_not bigredirect "status=302"

# Slow-trickled ~100 KiB redirect body (many small writes over several
# seconds): the drain budget must be wall-clock time, not pump iterations,
# so the redirect is still followed on the FIRST attempt.
run_case slowredirect /slowredirect 8
expect slowredirect "open (reconnect_count=0)"
expect slowredirect "tick${PORT} 1"
expect_not slowredirect "will_retry"

# Cross-origin redirect: never followed; the 302 surfaces and stops.
run_case xorigin /xorigin 2
expect xorigin "status=302 will_retry=0"
expect xorigin "closed"
expect_not xorigin "open (reconnect_count"

# Redirect to a target that closes without responding: retryable transport
# error, not a stale non-retryable 302.
run_case badtarget /badtarget 2
expect badtarget "reason=0 status=0 will_retry=1"
expect_not badtarget "status=302"

# 204: server-requested permanent stop.
run_case stop204 /204 2
expect stop204 "status=204 will_retry=0"
expect stop204 "closed"

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS integration case(s) failed"
  exit 1
fi
echo "integration ok"
