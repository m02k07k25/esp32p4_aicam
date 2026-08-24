# ESP32 BLE Mesh relay

`esp32_ble` is a standalone ESP-IDF 5.5 project for the original ESP32. It is
a pure BLE Mesh network relay: it does not receive P4 data over SDIO and it
does not parse or reassemble JPEG chunks.

The project is compatible with the current `../server` auto-provisioner:

- Device UUID prefix is `32 10`.
- UUID bytes 2-7 contain the ESP32 Bluetooth address.
- UUID bytes 8-9 contain the installation ID.
- The node exposes vendor model `0x02E5:0x0002`, allowing the Gateway to bind
  its AppKey and finish the normal Relay/TTL/publication configuration chain.
- There is exactly one Mesh element, and the Mesh Relay feature starts enabled.

The Gateway currently calls every managed `32 10` node a `C6` in its logs.
Messages such as `C6 image model ready id=100` are therefore expected for this
ESP32 relay and do not mean the wrong firmware was flashed.

## Device identity

Before building each physical relay, edit `main/device_identity.h` and give it
a unique `ESP32_BLE_DEVICE_ID` in the range 1 through 32766. The default is
100, which requests Mesh unicast address 101 (`0x0065`). Do not reuse an ID
owned by a C6 source or another ESP32 relay.

Changing the ID after provisioning does not update the address stored in Mesh
NVS. Keep it stable. If the ID changes or either side loses Mesh NVS, follow
the full reset procedure documented by `../server` and reprovision all affected
nodes.

## Build and flash

From the repository root in an initialized ESP-IDF CMD:

```cmd
idf.py -C firmware\esp32_ble build
idf.py -C firmware\esp32_ble -p COM8 flash monitor
```

The project forces target `esp32`, so a C6 target cannot be selected by
accident. Replace `COM8` with the port that reports `Chip is ESP32`.

To confirm the physical chip before flashing:

```cmd
esptool.py --port COM8 chip_id
```

For a deliberate full reprovisioning reset of this relay only:

```cmd
idf.py -C firmware\esp32_ble -p COM8 erase-flash
```

Erasing only this relay while the Gateway retains its old node record can
create an identity conflict. Reset the Mesh network consistently when doing a
full reprovisioning recovery.
