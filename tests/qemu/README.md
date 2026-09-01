# QEMU on-target integration test

Runs the ESP-IDF port inside Espressif's QEMU (emulated ESP32 with the OpenCores ethernet MAC) against `tools/sse_fixture_server.py` on the host. Covers what the host suites cannot: the `esp_http_client` EOF-vs-timeout mapping (close-delimited and chunked streams), `Last-Event-ID` resume across a reconnect, same-origin redirect following with request headers persisting across the reopen, and the cross-origin redirect refusal.

Requires ESP-IDF (>= 5.2) with the QEMU tool installed (`python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`) and `pip install pytest pytest-embedded-idf pytest-embedded-qemu`.

```sh
cd tests/qemu
idf.py set-target esp32
idf.py build
pytest pytest_qemu.py --embedded-services idf,qemu --app-path . \
    --qemu-extra-args "-nic user,model=open_eth"
```

The pytest fixture starts the server itself. For interactive runs instead: start `python3 ../../tools/sse_fixture_server.py 8085` and use `idf.py qemu monitor` (the app reaches the host at 10.0.2.2).

Wi-Fi is not emulated; the one behavior this suite cannot cover is real-radio disconnect/reconnect interplay, which still needs a devkit (or Wokwi).
