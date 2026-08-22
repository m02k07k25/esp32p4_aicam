# C6 SDIO and BLE Mesh host tests

The runner compiles production headers and source directly with host mocks:

- P4 and C6 SDIO v3 headers are byte-for-byte identical; 44-byte frame-header,
  24-byte control, and 40-byte time-message offsets and wire bytes are
  asserted.
- `sdio_frame_receiver.c` covers the 30,720/30,721-byte boundary, chunk order,
  duplicate/metadata rejection, CRC/JPEG validation, refreshed five-second
  assembly timeout, READY/BUSY states, timestamp/CRC pass-through, and
  `SERVER_ACKED` completion. It also verifies exact terminal replay after a
  lost `SERVER_ACKED`/`FAILED`, terminal preservation when the status queue is
  full, terminal mesh-not-ready rejection, and BUSY echo of the requesting P4
  frame ID with the conflicting active frame ID in `detail`. The same tests
  cover QUERY-to-worker handoff, echoed monotonic request metadata, explicit
  `NOT_READY`/`BUSY`/`UNAVAILABLE`/`FAILED`, and READY restoration after a
  terminal time sample.
- `ble_mesh_image_source.c` covers publication routing, OPEN BUSY backoff then
  ACCEPT, actual-length 374-byte DATA packets, END, duplicate bitmap NACK
  de-duplication, selective retransmission, Gateway COMPLETE, zero-copy JPEG
  lifetime, full-frame RESTART, duplicate-OPEN COMPLETE, the provision/bind/
  publication READY gate, early DATA-phase RESTART interruption, and the
  delayed transport-fault restart boundary. It also verifies the exact Device
  UUID layout, little-endian compile-time installation ID, derived unicast
  address, wrong-address READY rejection, and reserved-byte validation. Clock
  tests assert the four-byte TIME_REQUEST, exact 24-byte TIME_STATUS parsing,
  source/NetKey/AppKey/publication/request validation, unavailable semantics,
  and strict serialization with the image job.

Run from the repository root:

```powershell
python firmware/c6_sdio_ble/tests/run_host_tests.py
```

A host `gcc` is required. Generated executables remain under the ignored
`tests/.build` directory.
