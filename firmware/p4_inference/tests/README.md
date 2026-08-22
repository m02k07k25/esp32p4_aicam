# P4 host tests

Run from the repository root:

```powershell
python firmware/p4_inference/tests/run_host_tests.py
```

The test compiles the production `sdio_time_clock.c` on the host and checks
the four-timestamp midpoint calculation, measured path delay, mapping expiry,
explicit unavailable status, stale request/echo rejection, inconsistent server
timestamps, excessive round-trip age, and transport-style invalidation.
