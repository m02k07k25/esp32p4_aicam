# Server host tests

`run_host_tests.py` compiles `main/image_reassembly.c` directly with host GCC.
No parallel model of the receiver is used.

```powershell
python firmware/server/tests/run_host_tests.py
```

The tests validate the shared C1-C9 wire layout and little-endian helpers,
boundary sizes, 374-byte chunk reassembly, bitmap NACKs, duplicate and restart
semantics, the 60-second COMPLETE cache, 30-second inactivity timeout, CRC32,
SOI/EOI, and a 224 x 224 SOF marker. A source-level guard also checks persisted
provisioner-node restoration, safe AppKey conflict handling, and that model
publication remains the final configuration step. Radio provisioning and the
optional HTTP adapter still require ESP-IDF builds and real-device tests.

The optional adapter can be compile-checked on ESP32 with the non-flashable
placeholder overlay `tests/sdkconfig.http.defaults` in `SDKCONFIG_DEFAULTS`.

The same runner also executes `test_receive_images.py` without importing
pyserial. It feeds fragmented in-memory records mixed with normal console
logs and verifies exact log forwarding, split-magic resynchronization,
header/JPEG CRC rejection, oversize and truncated-record timeout recovery,
SOI/EOI checks, time-source/sequence validation, sequence gaps, and atomic
JPEG/JSON output.
