# Server host tests

`run_host_tests.py` compiles `main/image_reassembly.c` directly with host GCC.
No parallel model of the receiver is used.

```powershell
python firmware/server/tests/run_host_tests.py
```

The tests validate the shared C1-CB wire layout and little-endian helpers,
boundary sizes, 374-byte chunk reassembly, bitmap NACKs, duplicate and restart
semantics, the 60-second COMPLETE cache, 30-second inactivity timeout, CRC32,
SOI/EOI, a 224 x 224 SOF marker, and the exact TIME_REQUEST/TIME_STATUS codec.
A source-level guard also checks persisted provisioner-node restoration, safe
AppKey conflict handling, model publication ordering, managed-source time
requests, explicit unavailable status, the 28-byte laptop-time UART packet,
five-minute clock expiry, image-I/O update skipping, and the optional persistent SNTP path. Radio
provisioning and UART full-duplex behavior still require ESP-IDF builds and
real-device tests.

The optional HTTP exporter can be compile-checked on ESP32 with the
non-flashable placeholder overlay `tests/sdkconfig.http.defaults`. Simultaneous
serial export plus Wi-Fi/SNTP uses `tests/sdkconfig.serial_sntp.defaults`.
Append either file to `SDKCONFIG_DEFAULTS`; never flash its placeholder SSID.

The same runner also executes `test_receive_images.py` without importing
pyserial. It verifies laptop Unix-ms packet encoding/CRC and sequence wrap,
then feeds fragmented image records mixed with normal console logs to check
exact log forwarding, split-magic resynchronization, header/JPEG CRC rejection,
oversize and truncated-record timeout recovery, SOI/EOI checks,
time-source/sequence validation, sequence gaps, and atomic JPEG/JSON output.
