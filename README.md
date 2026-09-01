# Peripheral HID Mouse MDS Sample

This repository contains a Nordic nRF Connect SDK Bluetooth LE HID mouse sample with an optional Memfault Diagnostic Service (MDS) build.

The base application behaves as a BLE HID mouse. The MDS build adds Memfault diagnostics over Bluetooth, including custom metrics, trace events, logging, reboot reason collection, and RRAM-backed coredump storage.

## SDK Version

This example is prepared for nRF Connect SDK v3.4.0.

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

The sample derives its device ID from hardware so that every board reports a
distinct identity out of the box:

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID=y
CONFIG_HW_ID_LIBRARY_SOURCE_DEVICE_ID=y
```

See [Device Identity for Fleet Deployment](#device-identity-for-fleet-deployment)
for the alternatives and their trade-offs.

## Device Identity for Fleet Deployment

Memfault keys everything -- coredumps, traces, metrics, reboots -- off the device
serial. Rolling out more than one board therefore requires each unit to report a
**unique and stable** serial. If two units report the same serial they merge into
a single device in the cloud and their data is interleaved; if a unit's serial
changes it appears as a brand new device and loses its history.

`CONFIG_MEMFAULT_NCS_DEVICE_ID_*` symbols are members of a single Kconfig
`choice` in `nrf/modules/memfault-firmware-sdk/Kconfig`, so **exactly one may be
set**. Setting two is not a build error -- Kconfig keeps the last assignment and
silently drops the earlier one, which is easy to miss in a `prj.conf` diff.

| Option                           | Serial comes from                      | App code needed | Suitable for a fleet                         |
| -------------------------------- | -------------------------------------- | --------------- | -------------------------------------------- |
| `MEMFAULT_NCS_DEVICE_ID_STATIC`  | `CONFIG_MEMFAULT_NCS_DEVICE_ID` string | No              | **No** -- every unit reports the same serial |
| `MEMFAULT_NCS_DEVICE_ID_HW_ID`   | `hw_id` library, read from hardware    | No              | Yes                                          |
| `MEMFAULT_NCS_DEVICE_ID_RUNTIME` | `memfault_ncs_device_id_set()`         | **Yes**         | Yes                                          |

`STATIC` is only for a single bench board. The two options below are the two ways
to deploy a fleet.

### Option 1: HW_ID -- hardware-derived, no application code

This is the sample's default, selected as block (a) in `prj_mds.conf`. `hw_id`
reads an identifier out of the silicon during `SYS_INIT()`, so the serial exists
before `main()` and no application code is involved:

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID=y
CONFIG_HW_ID_LIBRARY_SOURCE_DEVICE_ID=y
```

On an nRF54LM20 DK this yields a 16 hex-digit serial such as `6F1C10AF57328697`.

`CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID` selects `HW_ID_LIBRARY`, and the source is
a nested choice that determines the serial length:

| `HW_ID_LIBRARY_SOURCE_*` | Length | Notes                                                        |
| ------------------------ | ------ | ------------------------------------------------------------ |
| `DEVICE_ID`              | 16     | Default, requires `CONFIG_HWINFO=y`. Available on any SoC    |
| `BT_DEVICE_ADDRESS`      | 12     | Not ready at `SYS_INIT()` time -- see below                  |
| `NET_MAC`                | 12     | Requires networking                                          |
| `IMEI`                   | 15     | Modem only                                                   |
| `UUID`                   | 36     | Modem only, and too long for MDS -- see the URI budget below |

Choose `DEVICE_ID` for a Bluetooth-only product. `BT_DEVICE_ADDRESS` works, but
the Bluetooth address is not available when `device_info_init()` runs, so the
serial is temporarily `"Unknown"` and the integration re-reads it on the first
non-ISR call to `memfault_platform_get_device_info()`
(`nrf/modules/memfault-firmware-sdk/memfault_integration.c`). That extra state is
avoidable by using `DEVICE_ID`.

Trade-off: the serial is not human readable and you cannot map it to your own
manufacturing records without a lookup table.

### Option 2: RUNTIME -- application supplies the serial

Use this when the serial must come from somewhere the SDK cannot reach on its
own: a provisioned value in settings, a factory-programmed record, a serial
number printed on the enclosure, or a readable prefix such as `mouse-6F1C10AF`.

**The sample implements this path**, so switching schemes is a one-line change:
comment out block (a) in `prj_mds.conf` and uncomment block (b).

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME=y
CONFIG_HW_ID_LIBRARY_SOURCE_DEVICE_ID=y
```

Selecting `RUNTIME` is sufficient -- the application `Kconfig` reacts to it:

| Symbol                           | Effect                                                                                                                                                                                            |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `APP_MEMFAULT_RUNTIME_DEVICE_ID` | Hidden helper that follows `MEMFAULT_NCS_DEVICE_ID_RUNTIME`. Selects `HW_ID_LIBRARY`, which `HW_ID` would otherwise have selected for you, and compiles `memfault_device_id_init()` into `main.c` |
| `APP_MEMFAULT_DEVICE_ID_PREFIX`  | Prefix for the generated ID, default `"mouse"`                                                                                                                                                    |

The result is `<prefix>-<first 8 hex digits of the hardware ID>`, for example
`mouse-B7261A5F`. The implementation is `memfault_device_id_init()` in
`src/main.c`, called at the top of `main()`:

```c
#include <memfault_ncs.h>
#include <hw_id.h>

char hw_id_buf[HW_ID_LEN];
char device_id[23];

if (hw_id_get(hw_id_buf, sizeof(hw_id_buf)) == 0) {
	snprintk(device_id, sizeof(device_id), "%s-%.8s",
		 CONFIG_APP_MEMFAULT_DEVICE_ID_PREFIX, hw_id_buf);
	memfault_ncs_device_id_set(device_id, strlen(device_id));
}
```

The setter is declared in `nrf/include/memfault_ncs.h`, so the include is
`<memfault_ncs.h>`.

To source the ID from somewhere other than the hardware ID -- settings, a factory
record, an enclosure serial number -- replace the body of that function. The only
requirement is that `memfault_ncs_device_id_set()` runs before anything reports
Memfault data.

Under `HW_ID` none of this is built: the function and its includes are guarded by
`#if defined(CONFIG_APP_MEMFAULT_RUNTIME_DEVICE_ID)`. Enabling `RUNTIME` costs
roughly 240 bytes of flash.

Four things to watch out for with `RUNTIME`:

- **The serial is an empty string until the setter runs.** `device_serial` is a
  zero-initialized static buffer, so selecting `RUNTIME` and never calling the
  setter leaves it empty. Nothing fails loudly: the build succeeds, the device
  boots, and the only symptom is an empty `S/N` in `mflt get_device_info`. Over
  MDS the effect is that the Data URI is built without a device ID, the gateway
  app reports **`NETWORK UNAVAILABLE`**, and the cloud never registers the
  device.
- **Timing.** `device_info_init()` runs from
  `SYS_INIT(init, APPLICATION, CONFIG_MEMFAULT_NCS_INIT_PRIORITY)`, which is
  before `main()`. Anything Memfault captures ahead of your setter call -- the
  boot-time reboot reason, for instance -- records the empty serial.
- **Length.** `CONFIG_MEMFAULT_NCS_DEVICE_ID_MAX_LEN` defaults to 30, but MDS
  allows fewer (see below). An over-long ID is truncated with a warning rather
  than rejected.
- **`memfault_ncs_device_id_set()` returns `-ENOTSUP`** if `RUNTIME` is not the
  selected option, so it cannot be combined with `HW_ID`.

### MDS URI budget

MDS builds the chunk upload URI by appending the device serial to a fixed base,
so the serial length is bounded on this transport:

| Item                                         | Length |
| -------------------------------------------- | ------ |
| `https://chunks.memfault.com/api/v0/chunks/` | 42     |
| `CONFIG_BT_MDS_MAX_URI_LENGTH` (default)     | 64     |
| **Available for the serial**                 | **22** |

A 16-character `DEVICE_ID` serial totals 58 and fits. A 36-character `UUID`
serial totals 78 and trips the `Too long URI` path in
`nrf/subsys/bluetooth/services/mds.c`, which fails the characteristic read
instead of uploading. Raise `CONFIG_BT_MDS_MAX_URI_LENGTH` if a longer serial is
unavoidable.

### Verifying and switching schemes

Check what the device actually reports over the shell:

```
mflt get_device_info
  S/N: 6F1C10AF57328697
  SW type: app
  SW version: 0.0.1+a88615
  HW version: nrf54lm20dk
```

An empty `S/N` means the chosen scheme never produced a serial. Confirm which
option survived the Kconfig choice by grepping the generated config:

```powershell
Select-String -Path build_mds/peripheral_hid_mouse_mds/zephyr/.config -Pattern "MEMFAULT_NCS_DEVICE_ID|HW_ID_LIBRARY"
```

Changing the scheme changes the serial, so the fleet reappears in Memfault as new
devices. Data recorded under the previous serial stays under the old identity.

## Button Behavior

In the normal HID mouse build, buttons keep the HID mouse behavior.

In the MDS build, buttons are used for Memfault test data:

- BTN1 starts/stops a timer metric and triggers a heartbeat.
- BTN2 records a trace event.
- BTN3 increments a custom counter metric.
- BTN4 intentionally triggers a crash for coredump testing.

## Custom Metrics

Memfault metrics are not reported as they happen. They are aggregated over a
**heartbeat interval**, and at the end of each interval the SDK snapshots every
metric, serializes it into a chunk, and resets the values for the next interval.
This build does not override `CONFIG_MEMFAULT_METRICS_HEARTBEAT_INTERVAL_SECS`,
so it uses the SDK default of 3600 s. Each metric therefore shows up in the
Memfault UI as a time series sampled once per hour.

The three application metrics are declared in
`memfault_config/memfault_metrics_heartbeat_config.def`:

```c
MEMFAULT_METRICS_KEY_DEFINE(button_press_count, kMemfaultMetricType_Unsigned)
MEMFAULT_METRICS_KEY_DEFINE(button_elapsed_time_ms, kMemfaultMetricType_Timer)
MEMFAULT_METRICS_KEY_DEFINE(battery_soc_pct, kMemfaultMetricType_Unsigned)
```

That file is pulled into the SDK through
`-DMEMFAULT_METRICS_USER_HEARTBEAT_DEFS_FILE`, so it is not a normal header and
may only contain `MEMFAULT_METRICS_KEY_DEFINE()` entries. The declarations only
establish the key and its type; the values are written from `src/main.c`.

| Metric                   | Type       | Write macro                             | Meaning                                    | Driven by        |
| ------------------------ | ---------- | --------------------------------------- | ------------------------------------------ | ---------------- |
| `button_press_count`     | `Unsigned` | `MEMFAULT_METRIC_ADD`                   | Count accumulated over the interval        | BTN3             |
| `button_elapsed_time_ms` | `Timer`    | `MEMFAULT_METRIC_TIMER_START` / `_STOP` | Milliseconds accumulated over the interval | BTN1             |
| `battery_soc_pct`        | `Unsigned` | `MEMFAULT_METRIC_SET_UNSIGNED`          | Instantaneous value (gauge)                | BAS notification |

### `button_press_count`

A counter. BTN3 adds 1 per press, so the cloud shows how many presses happened
during the interval and the count restarts from zero afterwards.

### `button_elapsed_time_ms`

`kMemfaultMetricType_Timer` does not store a duration you compute yourself. The
SDK records a timestamp on `START` and, on `STOP`, adds the elapsed time to the
interval total. The unit is always milliseconds -- the `_ms` suffix is only a
naming convention. BTN1 toggles the timer: the first press starts it, the second
stops it.

The `STOP` branch also calls `memfault_metrics_heartbeat_debug_trigger()`, which
packages a heartbeat immediately instead of waiting out the hour. This is the
only path in the sample that forces metrics to be reported, which makes BTN1 the
quickest way to get metric data into the cloud.

Timers are typically used for duty-cycle style questions, such as how long a
radio or a subsystem was active during the interval.

### `battery_soc_pct`

SoC is state of charge, in percent. It is written from `bas_notify()` with `SET`
rather than `ADD`, so it is a gauge: the heartbeat records the last value written
during the interval, not a sum.

Note that this is **not a real battery reading**. `bas_notify()` takes the
current Battery Service level, decrements it, and wraps back to 100 when it
reaches zero, purely so that BAS and this metric have moving data to show.
Replace it with an ADC reading or an nPM-series fuel gauge for real hardware.

### Metrics vs trace events

BTN2 uses `MEMFAULT_TRACE_EVENT_WITH_LOG(button_state_changed, ...)`, which is a
**trace event**, not a metric. Trace reasons are declared separately in
`memfault_config/memfault_trace_reason_user_config.def` and travel through a
different data source, so they appear under Issues/Traces rather than in the
metric charts.

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
