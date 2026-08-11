# C6 SDIO and BLE Mesh host tests

The runner compiles production headers and source directly with host mocks:

- P4 and C6 SDIO v3 headers are byte-for-byte identical, 44-byte frame header
  offsets and the 24-byte control ABI are asserted.
- `sdio_frame_receiver.c` covers the 30,720/30,721-byte boundary, chunk order,
  duplicate/metadata rejection, CRC/JPEG validation, refreshed five-second
  assembly timeout, READY/BUSY states, timestamp/CRC pass-through, and
  `SERVER_ACKED` completion. It also verifies exact terminal replay after a
  lost `SERVER_ACKED`/`FAILED`, terminal preservation when the status queue is
  full, terminal mesh-not-ready rejection, and BUSY echo of the requesting P4
  frame ID with the conflicting active frame ID in `detail`.
- `ble_mesh_image_source.c` covers publication routing, OPEN BUSY backoff then
  ACCEPT, actual-length 374-byte DATA packets, END, duplicate bitmap NACK
  de-duplication, selective retransmission, Gateway COMPLETE, zero-copy JPEG
  lifetime, full-frame RESTART, duplicate-OPEN COMPLETE, the provision/bind/
  publication READY gate, early DATA-phase RESTART interruption, and the
  delayed transport-fault restart boundary.

Run from the repository root:

```powershell
python firmware/c6_sdio_ble/tests/run_host_tests.py
```

A host `gcc` is required. Generated executables remain under the ignored
`tests/.build` directory.
