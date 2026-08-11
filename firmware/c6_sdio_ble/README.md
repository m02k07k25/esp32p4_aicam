# ESP32-C6 SDIO + BLE Mesh image source

`c6_sdio_ble` is the ESP32-C6 companion for `../p4_inference`. It receives one
224x224 JPEG over SDIO, validates it, then sends it to the Gateway configured
as the Vendor Model publication destination. It also keeps the ESP-BLE-Mesh
relay feature enabled; it does not contain Gateway JPEG reassembly code.

The project is independent of `../c6_hosted`. Its Hosted 2.12.3 SDIO slave
sources are local to this project, while the role-neutral radio ABI is shared
with `../server` through `../components/ble_mesh_image_protocol`.

## Data and recovery flow

1. P4 SDIO protocol v3 carries `frame_id`, JPEG length/CRC, and the absolute
   `detected_at_ms` recorded at frame capture, before inference. C6 reassembles
   into one static 30,720-byte buffer and validates order, CRC32, and JPEG
   SOI/EOI.
2. C6 sends `OPEN` containing BLE frame ID, detection time, image length and
   CRC. No image bytes are sent until the Gateway answers `ACCEPT`.
3. C6 sends variable-length `DATA` packets. The prefix is only a 16-bit frame
   ID and 8-bit chunk index; a full packet carries 374 JPEG bytes and exactly
   fills the ESP-IDF 377-byte Vendor Access payload limit.
4. `END` asks the Gateway to scan its receive bitmap. `NACK` carries a base
   chunk index plus a 1-5 byte missing bitmap, and C6 retransmits only those
   chunks before sending `END` again.
5. Only `COMPLETE`, sent after Gateway length/CRC/JPEG validation, succeeds the
   transfer. C6 then releases the JPEG and reports SDIO `SERVER_ACKED` to P4.

`BUSY` retries the same `OPEN` with full-jitter caps of 1/2/4/8/10 seconds.
`RESTART` restarts the same frame at `OPEN` and resends the full JPEG.
`REJECT` is terminal. OPEN and repair stages allow five attempts, Gateway
response timeout is five seconds, and the overall frame deadline is 300
seconds. A local Mesh send completion timeout is ten seconds; it marks the
transport not ready and restarts after two seconds while preserving Mesh NVS.

Only one SDIO/BLE frame is active. A new frame receives `BUSY` until the
Gateway completes or the current frame fails. SDIO `ACCEPTED` means only that
C6 owns the buffer; `SERVER_ACKED` is the end-to-end success indication. BUSY
echoes the requesting P4 frame ID and places the conflicting active P4 frame
ID in `detail`. C6 caches the last terminal `SERVER_ACKED` or `FAILED` control
and reproduces it for a matching QUERY, so a lost control packet cannot turn
an idle READY state into a false result.

## Build and flash

Use ESP-IDF 5.5.x:

```text
cd firmware/c6_sdio_ble
idf.py fullclean
idf.py build
idf.py -p COMx flash monitor
```

The project forces target `esp32c6` and produces `build/c6_sdio_ble.bin`.
Wi-Fi, HTTP, network split, and Hosted BT/HCI sharing are disabled. Bluetooth,
NimBLE, BLE Mesh, PB-ADV/PB-GATT, persistent settings, and Mesh relay support
remain local to C6. Disabled Hosted power-save, light-sleep, network-split,
GPIO-expander, and external-coexistence implementation files are not compiled;
the application has no direct `esp_wifi` component dependency.

## SDIO wiring

| Signal | C6 GPIO | P4 default GPIO |
| --- | ---: | ---: |
| CLK | 19 | 18 |
| CMD | 18 | 19 |
| D0 | 20 | 14 |
| D1 | 21 | 15 |
| D2 | 22 | 16 |
| D3 | 23 | 17 |

Use common ground and 3.3 V logic. Separate boards need external pull-ups on
CMD and DAT0-DAT3. Connect the configured P4 reset output to C6 EN when
automatic coprocessor reset is required.

## Mesh provisioning and destination

C6 reports `READY` only after all of these are true:

- it is provisioned;
- AppKey is bound to Vendor Model company `0x02E5`, model `0x0002`;
- that model has a unicast publication address using the same AppKey.

The Gateway address is therefore not compiled into C6. BLE frame IDs start
from a nonzero random per-boot seed to avoid deterministic completed-cache
collisions without per-frame NVS writes. The Provisioner must
set the model publication address to the actual `server` node address (usually
`0x0001`). Source C6 nodes receive different unicast addresses. The monitor
prints the configured publication destination and the READY transition.

The source defaults Network Transmit and Relay Retransmit to one radio
transmission (`count=0`) because segmented unicast already performs selective
Lower Transport recovery and this protocol adds frame-level NACK recovery.
Relay stays enabled, and default TTL is 3. A Provisioner may tune Relay,
retransmit, and TTL for the physical topology.

## BLE test

Monitoring C6 alone can confirm provisioning, binding, publication and local
send errors, but it cannot prove image delivery. A working `firmware/server`
node is required for the positive end-to-end path:

```text
C6: OPEN -> ACCEPT -> DATA... -> END
server: NACK(bitmap) -> C6 retransmits requested DATA -> END
server: COMPLETE
P4: ACCEPTED -> SERVER_ACKED -> READY
```

For a repair test, instrument a server test build to discard a DATA chunk once,
or inject an equivalent receive loss. Verify that its NACK bitmap names that
index, C6 logs exactly that chunk as a retransmission, and COMPLETE occurs only
after server CRC/JPEG validation. A server `COMPLETE` received for a duplicate
OPEN is also accepted as idempotent success.

## Host tests

From the repository root:

```text
python firmware/c6_sdio_ble/tests/run_host_tests.py
```

The tests compare the P4/C6 SDIO v3 headers byte-for-byte, exercise the
production SDIO receiver, and drive the production Mesh source through
BUSY/ACCEPT, variable 374-byte DATA packetization, bitmap NACK repair,
COMPLETE, full RESTART, duplicate-OPEN idempotency, terminal-control loss
recovery, BUSY collision semantics, zero-copy ownership, READY gating, and
transport-timeout restart recovery.
