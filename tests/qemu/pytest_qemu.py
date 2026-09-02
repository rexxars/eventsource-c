"""QEMU on-target integration test for the ESP-IDF port.

Starts the fixture server on the host, boots tests/qemu in
qemu-system-xtensa (pytest-embedded), and asserts on the app's
"QEMU-TEST ..." serial markers. Run from tests/qemu:

    pytest pytest_qemu.py --embedded-services idf,qemu --app-path . \
        --qemu-extra-args "-nic user,model=open_eth"
"""
import os
import re
import subprocess
import sys
import time

import pytest

FIXTURE_PORT = 8085  # must match FIXTURE_BASE in main/main.c
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# A prompt EOF arrives within the read timeout; a misreported one only when
# the app's 15 s idle timeout fires. Anything under this bound is "prompt".
EOF_LATENCY_LIMIT_MS = 5000


@pytest.fixture(autouse=True)
def fixture_server():
    proc = subprocess.Popen(
        [
            sys.executable,
            os.path.join(REPO_ROOT, "tools", "sse_fixture_server.py"),
            str(FIXTURE_PORT),
        ]
    )
    time.sleep(1)
    assert proc.poll() is None, "fixture server failed to start"
    yield
    proc.terminate()
    proc.wait(timeout=5)


def expect_eof_scenario(dut, name):
    for i in (1, 2, 3):
        dut.expect(f"QEMU-TEST msg name={name} id={i} data=lim {i}", timeout=30)
    m = dut.expect(
        re.compile(
            (
                "QEMU-TEST err name=%s reason=0 status=0 retry=1 latency_ms=(\\d+)"
                % name
            ).encode()
        ),
        timeout=30,
    )
    latency = int(m.group(1))
    assert latency < EOF_LATENCY_LIMIT_MS, (
        f"{name}: EOF not detected promptly (latency {latency} ms; the idle "
        "timeout, not the read path, noticed the closed stream)"
    )
    dut.expect(f"QEMU-TEST open name={name} count=1", timeout=30)
    for i in (4, 5, 6):  # Last-Event-ID resume after the reconnect
        dut.expect(f"QEMU-TEST msg name={name} id={i} data=lim {i}", timeout=30)
    dut.expect(f"QEMU-TEST done name={name}", timeout=30)


def test_sse_transport_qemu(dut):
    dut.expect("QEMU-TEST net up", timeout=120)

    # Quiet stream: headers complete, 3 s of silence, then the event. The
    # mid-stream read timeouts in between must not become transport errors
    # (regression: -ESP_ERR_HTTP_EAGAIN was mapped to SSE_READ_ERROR). The
    # single open(0) plus done proves no reconnect happened.
    dut.expect("QEMU-TEST open name=silent count=0", timeout=30)
    dut.expect("QEMU-TEST msg name=silent id=1 data=late 1", timeout=30)
    dut.expect("QEMU-TEST done name=silent", timeout=30)

    expect_eof_scenario(dut, "eof")         # close-delimited stream
    expect_eof_scenario(dut, "eofchunked")  # chunked stream

    # Same-origin redirect with the request header surviving the reopen.
    dut.expect("QEMU-TEST open name=redirect count=0", timeout=30)
    dut.expect("QEMU-TEST msg name=redirect id=1 data=hdr=qemu-marker", timeout=30)
    dut.expect("QEMU-TEST done name=redirect", timeout=30)

    # Cross-origin redirect refused: surfaces the 302, never opens.
    dut.expect(
        re.compile(rb"QEMU-TEST err name=xorigin reason=1 status=302 retry=0"),
        timeout=30,
    )
    dut.expect("QEMU-TEST closed name=xorigin", timeout=30)
    dut.expect("QEMU-TEST all done", timeout=30)
