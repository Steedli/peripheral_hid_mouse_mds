# Peripheral HID Mouse MDS Sample

This repository contains a Nordic nRF Connect SDK Bluetooth LE HID mouse sample with an optional Memfault Diagnostic Service (MDS) build.

The base application behaves as a BLE HID mouse. The MDS build adds Memfault diagnostics over Bluetooth, including custom metrics, trace events, logging, reboot reason collection, and RRAM-backed coredump storage.

## SDK Version

This example is prepared for nRF Connect SDK v3.2.3.

The commands below assume the NCS environment is already initialized and `west` is available in the shell.

## What This Sample Does

- Advertises as `Nordic_HIDS_mouse`.
- Provides Bluetooth HIDS, BAS, and DIS services.
- Keeps the original non-MDS HID mouse build available through `prj.conf`.
- Adds an MDS-enabled build through `prj_mds.conf`.
- Uses `memfault_config/` for application metrics and trace reason definitions.
- Adds an RRAM `memfault_coredump_partition` in `app.overlay`.
- Includes documentation in `doc/` for Memfault integration and BLE directed advertising/RPA behavior.

## MDS vs Non-MDS Builds

Use `prj.conf` for the normal HID mouse sample without Memfault:

```powershell
west build --build-dir build . --pristine --board nrf54lm20dk/nrf54lm20a/cpuapp
```

Use `prj_mds.conf` for the HID mouse sample with Memfault MDS enabled:

```powershell
west build --build-dir build_mds . --pristine --board nrf54lm20dk/nrf54lm20a/cpuapp -- -DCONF_FILE="prj_mds.conf"
```

If Windows path length becomes a problem, use a short build directory:

```powershell
west build --build-dir D:/b/hids_mds D:/peripheral_hid_mouse_mds --pristine --board nrf54lm20dk/nrf54lm20a/cpuapp -- -DCONF_FILE="prj_mds.conf"
```

Flash the selected build with:

```powershell
west flash --build-dir build_mds --erase
```

For the non-MDS build, replace `build_mds` with `build`.

## Memfault Setup

Before building `prj_mds.conf`, replace the placeholder project key:

```conf
CONFIG_MEMFAULT_NCS_PROJECT_KEY="<YOUR_MEMFAULT_PROJECT_KEY>"
```

The sample currently uses a static device ID for simple testing:

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_STATIC=y
CONFIG_MEMFAULT_NCS_DEVICE_ID="nrf-hids-mouse"
```

For multiple boards, use a unique and stable device ID. NCS v3.2.3 also supports `CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID=y` to derive the ID from hardware, or `CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME=y` to set it from application code.

## Button Behavior

In the normal HID mouse build, buttons keep the HID mouse behavior.

In the MDS build, buttons are used for Memfault test data:

- BTN1 starts/stops a timer metric and triggers a heartbeat.
- BTN2 records a trace event.
- BTN3 increments a custom counter metric.
- BTN4 intentionally triggers a crash for coredump testing.

## Documentation

Additional notes are in `doc/`:

- `doc/MEMFAULT_INTEGRATION_GUIDE.md`
- `doc/BLE_DirectedAdv_RPA_Analysis.md`

## Build Artifacts

Build output directories are ignored by `.gitignore`:

```gitignore
/build*/
```

Do not commit generated build directories or binaries.
