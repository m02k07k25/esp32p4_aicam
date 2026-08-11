# Generic BLE Mesh image server

`firmware/server` is an ESP-IDF 5.5 Provisioner and JPEG Gateway. It is not
tied to an ESP32-WROOM module and never forces `IDF_TARGET`. The validated
target family is the one advertised by the IDF 5.5 BLE Mesh Provisioner
example: ESP32, ESP32-C3, ESP32-C6, ESP32-C61, ESP32-H2, and ESP32-S3.

The core has no Wi-Fi dependency. By default every completed JPEG is exported
over the same USB console stream as the normal event logs, so no Wi-Fi radio
time is needed to retrieve images. A Wi-Fi/SNTP/HTTP adapter remains an
alternative on targets with `SOC_WIFI_SUPPORTED`; it is not compiled for
ESP32-H2.

## Mesh ownership and provisioning

The server is the Mesh Provisioner and owns primary unicast address `0x0001`.
It creates and persists the Mesh keys, filters unprovisioned advertisements by
the C6 UUID prefix `32 10`, and allocates C6 addresses from `0x0002`. A phone
provisioner is not needed.

The default capacity is 10 C6 nodes. To raise it, increase both
`CONFIG_SERVER_MAX_NODES` and ESP-BLE-Mesh's
`CONFIG_BLE_MESH_MAX_PROV_NODES` in `menuconfig`; the smaller value is the
effective limit.

For each new C6 it runs this configuration chain with three retries per step:

1. AppKey Add
2. bind AppKey to vendor model CID `0x02E5`, model `0x0002`
3. Relay Set: enabled, one network transmission
4. Network Transmit Set: one transmission
5. Default TTL Set: 3
6. Model Publication Set: server `0x0001`, AppKey 0, TTL 3

Publication is deliberately last: the C6 cannot report READY and start an
image while the earlier configuration traffic is still active. Server control
messages use TTL 3 and three network transmissions. The C6 publication/data
path uses one network transmission to minimize image airtime.

Mesh settings and the randomly generated AppKey are stored in NVS. Erasing
the server's NVS creates a new network; already provisioned C6 nodes must then
be node-reset or have their NVS erased before they can join again.
On a normal server reboot, persisted C6 node-table entries are restored and
the idempotent configuration chain is resumed. An AppKey-index conflict is
treated as fatal instead of declaring a false READY state; reset/reprovision
that node after confirming which device lost its Mesh NVS.

## Minimal image protocol

The common ABI is
`firmware/components/ble_mesh_image_protocol`. All integers are little-endian
and callback buffers are decoded with unaligned-safe helpers, not packed
struct casts. Vendor model `0x0002` identifies this protocol generation, so
there is no repeated magic or version field.

| Opcode | Direction | Payload |
| --- | --- | --- |
| C1 OPEN | C6 -> server | frame u16, detected Unix ms u64, JPEG length u16, CRC32 u32 (16 B) |
| C2 DATA | C6 -> server | frame u16, chunk index u8, JPEG bytes 1..374 |
| C3 END | C6 -> server | frame u16 |
| C4 ACCEPT | server -> C6 | frame u16 |
| C5 BUSY | server -> C6 | frame u16 |
| C6 COMPLETE | server -> C6 | frame u16 |
| C7 NACK | server -> C6 | frame u16, base index u8, missing bitmap 1..5 B |
| C8 RESTART | server -> C6 | frame u16 |
| C9 REJECT | server -> C6 | frame u16, reason u8 |

DATA has only a three-byte application header. A 30,720-byte JPEG needs at
most 83 application chunks. The server accepts chunks out of order, stores a
received bitmap, and after END requests only missing bits in up to three NACK
messages. It does not ACK each DATA packet.

One 30 KiB static reassembly slot is used. A different OPEN receives BUSY
while it is occupied. A valid duplicate OPEN receives ACCEPT, and a 16-entry
60-second completion cache answers duplicate END/OPEN with COMPLETE if the
original response was lost. Thirty seconds without a valid packet rejects and
discards the active frame.

Before COMPLETE, the server verifies the declared length, CRC32, JPEG SOI and
EOI, and a JPEG SOF declaring exactly 224 x 224. The public callback receives
only the Mesh source, event time, time provenance, and immutable JPEG view:

```c
static void received(const server_image_t *image, void *ctx)
{
    // image->jpeg is valid only until this callback returns.
    consume(image->source_addr, image->event_time_ms,
            image->time_source, image->jpeg, image->jpeg_len);
}

mesh_image_gateway_register_image_callback(received, NULL);
```

`SERVER_TIME_P4_DETECTED` means the nonzero time came from P4's direct SNTP
clock. If P4 sends zero, a registered `server_time_provider_t` may supply the
wall clock sampled at the first accepted OPEN as a receive-time estimate
(`SERVER_TIME_RX_ESTIMATE`); OPEN retries and the later JPEG transfer do not
move it. Otherwise the time is zero and `SERVER_TIME_UNKNOWN`. No Mesh
time-sync messages exist.

## Build and flash

Select the chip explicitly, then build normally. For an ESP32/WROOM-style
board, for example:

```powershell
cd firmware/server
idf.py set-target esp32
idf.py build
idf.py -p COMx flash
```

Use `esp32c3`, `esp32c6`, `esp32c61`, `esp32h2`, or `esp32s3` instead without
editing source code. The output is `build/mesh_image_server.bin`.

Expected provisioning logs end with:

```text
auto provisioning enabled for UUID prefix 32 10
C6 provisioned addr=0x0002 elements=1
C6 image model ready addr=0x0002 ttl=3
```

A complete frame logs its source address, event time provenance, and byte
count. Mesh DATA chunks are never hex-dumped.

## USB console logs and JPEG receiver

`CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y` is the default. The server emits each
completed image as one atomic record on its normal console UART, between the
ordinary text logs. The project defaults the console to 921600 baud and LF
line endings. Target defaults use primary UART0 with no secondary console.
There is no second data UART: on a development board, use the same built-in
USB-to-UART bridge and cable used for flashing and logs. A bare module needs
one ordinary USB-to-UART bridge wired to its primary UART0 as usual.

On chips that support native USB Serial/JTAG, it may instead be selected as
the **primary** console in ESP-IDF. In that case its COM port is used and the
UART baud argument is not physically relevant. Never send image records
through USB Serial/JTAG configured only as the nonblocking secondary console;
it may drop a 30 KiB record. Whichever primary console is selected, the PC
receiver and `idf.py monitor` still cannot share its single COM port.

The record is a 40-byte little-endian header followed immediately by the JPEG:

```text
<8sHHHBBQIIII
magic="BMJPEG01", version=1, header_size=40,
source_addr, time_source, reserved=0, event_time_ms,
jpeg_len, jpeg_crc32, sequence, header_crc32
```

The header CRC-32 covers its first 36 bytes. JPEG CRC-32 covers the exact
following `jpeg_len` bytes. Both use standard reflected CRC-32/IEEE. The PC
tool also validates version, length up to 30,720 bytes, time source, and JPEG
SOI/EOI before saving anything.

Install pyserial once, then run the receiver from the repository root. Replace
`COM_SERVER` with the same port used to flash the server:

```powershell
python -m pip install pyserial
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

Do **not** run `idf.py monitor` at the same time. A serial port can be opened by
only one program, and the regular monitor does not understand the binary JPEG
records. `receive_images.py` replaces it: it prints the ESP-IDF text logs in
real time while scanning through them for `BMJPEG01` records. Press the
board's reset/EN button after starting the tool if boot/provisioning logs are
also needed.

Every valid record creates an immutable timestamped `.jpg` and matching
`.json`; `latest.jpg` and `latest.json` are atomically replaced. The JSON
contains Mesh source address, P4/server time provenance, event time, byte
length, CRC, receive time, and export sequence. Header/JPEG corruption is
discarded and the scanner resynchronizes at the next magic. A global sequence
gap is printed as a warning.

## Optional Wi-Fi/SNTP/HTTP adapter

On a Wi-Fi-capable target, first disable
`CONFIG_SERVER_SERIAL_IMAGE_ENABLE`, then enable `CONFIG_SERVER_HTTP_ENABLE`
and set the SSID, password, SNTP server, and HTTP port in menuconfig. The two
export adapters are intentionally mutually exclusive. The HTTP adapter copies
the latest completed image and exposes:

- `GET /latest.jpg`
- `GET /latest.json`

`/latest.jpg` atomically reserves an idle Mesh window and returns `503` plus
`Retry-After: 1` if a frame or another bulk operation is active. New OPEN
messages receive BUSY until the response ends, so the 30 KiB HTTP snapshot
cannot race reassembly. JSON is small and remains available. SNTP uses the
same reservation for one initial synchronization, then deinitializes to
prevent periodic polls from colliding with image radio traffic. P4's
timestamp remains authoritative.

The HTTP adapter is disabled by default. ESP32-H2 can use the default console
exporter, while custom Ethernet or storage integrations can still register the
same callback API after disabling both built-in adapters.

## Recovery test and host tests

Set `CONFIG_SERVER_TEST_DROP_CHUNK_INDEX` to `0..82` to drop that DATA chunk
once after boot. The next END produces a bitmap NACK. UART prints the base and
all five bitmap bytes, for example, without printing JPEG bytes. Default `-1`
fully disables fault injection.

Run the production reassembly tests on the host:

```powershell
python firmware/server/tests/run_host_tests.py
```

They cover the 1/374/375/30,720/30,721 size boundaries, unaligned LE ABI,
out-of-order and duplicate data, conflicting duplicates, first/middle/last
missing bitmap recovery, CRC and JPEG marker/SOF validation, timeout,
server-restart RESTART, BUSY isolation by Mesh source, and completion-cache
expiry. The same command tests the PC stream parser using fragmented mixed
logs, split magic, bad header/JPEG CRC, truncated frames, invalid markers and
time source, resynchronization, sequence gaps, and atomic JPEG/JSON output;
pyserial and physical hardware are not needed for these host tests.
