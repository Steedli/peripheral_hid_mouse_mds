# 在 NCS 工程中接入 Memfault 指南

本文以 `D:\peripheral_hid_mouse_mds` 为例，说明如何把一个原本没有 Memfault 的 nRF Connect SDK 工程改造成支持 Memfault Diagnostic Service (MDS)、自定义 metrics、trace event、日志采集和 RRAM coredump 的工程。

示例工程基于 Bluetooth HIDS Mouse，当前使用 `prj_mds.conf` 作为 Memfault 版本的构建配置。

## 1. 注册账号并创建 Memfault 项目

在改设备端代码之前，先把 Memfault 云端项目准备好。设备端的 `CONFIG_MEMFAULT_NCS_PROJECT_KEY` 会决定数据上传到哪个项目；设备 ID / serial 会决定云端把数据归到哪个设备。

1. 打开 [Memfault / nRF Cloud 注册页面](https://app.memfault.com/register) 注册账号。
2. 注册完成后选择使用的 Nordic 芯片系列，系统会自动为该芯片创建一个项目。
3. 如果需要为当前产品单独建项目，在云端项目/组织切换入口中新建 Project，并选择对应的 Nordic 芯片或硬件类型；后续设备数据、符号文件和 OTA release 都应该放在这个项目下。
4. 如果已经有 nRF Cloud 账号但看不到 Memfault 功能，可以新建账号，或者联系 Memfault / Nordic 支持开通访问权限。
5. 进入项目后，打开 **Settings → General**，复制 **Project Key**。也可以使用 [Project Key 快捷入口](https://mflt.io/project-key)。
6. 把 Project Key 填入工程配置：

```conf
CONFIG_MEMFAULT_NCS_PROJECT_KEY="<YOUR_MEMFAULT_PROJECT_KEY>"
```

7. 确认设备 ID 策略。当前示例使用静态 ID：

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_STATIC=y
CONFIG_MEMFAULT_NCS_DEVICE_ID="nrf-hids-mouse"
```

这个配置适合单板调试。量产或多设备测试时，每台设备都必须使用唯一且稳定的 device id / serial，否则多个设备的数据会在云端合并到同一个设备页面。

8. 安装手机端 [nRF Connect Device Manager](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-Device-Manager)。BLE 外设通常没有直接联网能力，本工程通过 MDS 让手机 App 作为 gateway，把设备生成的 Memfault chunks 上传到云端。
9. 构建并烧录后，建议把 `zephyr.elf` 上传到云端 **Software → Symbol Files**。这样 crash/coredump 才能在 **Issues** 页面显示带符号的调用栈。常见路径示例：

```text
build_mds/zephyr/zephyr.elf
D:/b/hids_mds/zephyr/zephyr.elf
```

注意：Project Key 可以提交占位符，但不要把真实 key 提交到公共仓库。Device ID 可以提交示例值，但正式固件应从制造数据、芯片唯一 ID 或后端分配的 serial 派生。

## 2. Memfault 数据流概览

Memfault 在设备端主要产生以下几类数据：

### Metrics

用于记录数值型指标，例如计数器或状态值。

- 典型 API：`MEMFAULT_METRIC_ADD()`、`MEMFAULT_METRIC_SET_UNSIGNED()`
- 当前工程示例：`button_press_count`、`battery_soc_pct`
- 存储位置：RAM event storage
- 上传时机：heartbeat 触发后打包为 chunk

### Timer Metric

用于记录一段持续时间。

- 典型 API：`MEMFAULT_METRIC_TIMER_START()`、`MEMFAULT_METRIC_TIMER_STOP()`
- 当前工程示例：`button_elapsed_time_ms`
- 存储位置：RAM event storage
- 上传时机：timer stop 后触发 heartbeat

### Trace Event

用于记录一次离散事件，适合描述“发生了什么”。

- 典型 API：`MEMFAULT_TRACE_EVENT_WITH_LOG()`
- 当前工程示例：`button_state_changed`
- 存储位置：RAM event storage
- 上传时机：生成后等待 chunk 导出或上传

### Logs

用于采集设备日志。

- 典型来源：Zephyr log、Memfault logging
- 当前工程示例：`CONFIG_MEMFAULT_LOGGING_ENABLE=y`
- 存储位置：Memfault log buffer / event storage
- 上传时机：触发 log capture 或上传

### Coredump

用于保存 crash 时的寄存器、栈和指定内存区域。

- 典型来源：fault handler 自动保存
- 当前工程示例：BTN4 除零 crash
- 存储位置：RRAM `memfault_coredump_partition`
- 上传时机：重启后通过 MDS 或 CLI 导出上传

### Reboot Reason

用于记录设备上一次复位原因。

- 典型来源：SDK 启动时采集
- 当前工程示例：crash 后重启原因
- 存储位置：RAM event storage
- 上传时机：下次 heartbeat/chunk 上传

注意：`button_press_count metric increased` 只表示本地 metric 计数成功，不等于已经上传到 Memfault 云。metrics 通常需要触发 heartbeat 后才会被打包。

## 3. 添加 Memfault 配置文件

建议新增一个独立配置文件，例如本工程的 `prj_mds.conf`，不要直接污染原始 `prj.conf`。核心配置如下：

```conf
# Memfault Diagnostic Service over BLE
CONFIG_BT_MDS=y
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_CTLR_DATA_LENGTH_MAX=251

# Memfault SDK
CONFIG_LOG=y
CONFIG_LOG_PRINTK=n
CONFIG_LOG_DEFAULT_LEVEL=2
CONFIG_LOG_MODE_DEFERRED=y
CONFIG_LOG_MODE_OVERFLOW=y
CONFIG_LOG_BACKEND_RTT=n

CONFIG_SHELL=y
CONFIG_HEAP_MEM_POOL_SIZE=256

CONFIG_MEMFAULT=y
CONFIG_MEMFAULT_SHELL=y
CONFIG_MEMFAULT_LOGGING_ENABLE=y
CONFIG_MEMFAULT_LOG_LEVEL_INF=y
CONFIG_MEMFAULT_EVENT_STORAGE_SIZE=4096

# 替换成 Memfault 项目的 Project Key，不建议提交真实 key 到公共仓库
CONFIG_MEMFAULT_NCS_PROJECT_KEY="<YOUR_MEMFAULT_PROJECT_KEY>"
CONFIG_MEMFAULT_NCS_DEVICE_ID_STATIC=y
CONFIG_MEMFAULT_NCS_DEVICE_ID="nrf-hids-mouse"

# 采集 BLE 相关指标
CONFIG_MEMFAULT_NCS_BT_METRICS=y
```

`CONFIG_MEMFAULT_NCS_PROJECT_KEY` 使用第 1 节中复制的 Project Key。`CONFIG_MEMFAULT_NCS_DEVICE_ID` 是云端设备页面显示和聚合数据的关键字段，单板调试可以固定为 `nrf-hids-mouse`，多设备时必须改成唯一值。

### 使用动态 Device ID

NCS 3.2.3 中 Memfault device ID 有三种常用策略：

- `CONFIG_MEMFAULT_NCS_DEVICE_ID_STATIC=y`：编译期固定 ID，适合单板调试。
- `CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID=y`：使用 `hw_id` 库从芯片硬件 ID 生成唯一 ID，推荐用于多设备测试和没有生产序列号的场景。
- `CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME=y`：运行时由应用设置 ID，适合 ID 来自生产烧录、settings、UICR、外部 flash 或后端分配 serial 的场景。

如果只需要每块板自动生成不同 ID，推荐把静态配置：

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_STATIC=y
CONFIG_MEMFAULT_NCS_DEVICE_ID="nrf-hids-mouse"
```

改成：

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID=y
```

`CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID` 会让 NCS 在初始化时调用 `hw_id_get()` 填充 Memfault device serial。这个 ID 对同一颗芯片应保持稳定，且不同芯片不同。

如果需要使用制造阶段写入的序列号，改用运行时 ID：

```conf
CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME=y
CONFIG_MEMFAULT_NCS_DEVICE_ID_MAX_LEN=32
```

然后在应用启动早期调用 `memfault_ncs_device_id_set()`：

```c
#include <string.h>
#include <memfault_ncs.h>

static void set_memfault_device_id(void)
{
    const char *id = "your-runtime-serial";
    int err = memfault_ncs_device_id_set(id, strlen(id));

    if (err) {
        printk("Failed to set Memfault device ID: %d\n", err);
    }
}
```

`id` 应该替换为从 settings、UICR、生产数据或其他持久化位置读取到的序列号。这个函数需要在生成 heartbeat、trace、coredump 上传之前调用。无论使用哪种动态方式，同一台设备的 ID 都必须长期稳定，不能每次开机随机生成，否则云端会把同一台设备识别成多台设备。

如果要用手机 App 的 Device Manager / SMP Echo 页面，还需要启用 MCUmgr SMP over BLE：

```conf
CONFIG_ZCBOR=y
CONFIG_MCUMGR=y
CONFIG_MCUMGR_TRANSPORT_BT=y
CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY=y
CONFIG_MCUMGR_TRANSPORT_BT_CONN_PARAM_CONTROL=y
CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_ENCRYPT=y
CONFIG_MCUMGR_GRP_OS=y
CONFIG_MCUMGR_GRP_OS_ECHO=y
CONFIG_MCUMGR_GRP_OS_INFO=y
```

其中 `CONFIG_BT_SMP` 是 Bluetooth 配对安全，`CONFIG_MCUMGR_TRANSPORT_BT` 才是手机 App 提示的 SMP service。

## 4. 添加 memfault_config 目录

本工程通过 `CMakeLists.txt` 把 `memfault_config` 加入 include path：

```cmake
zephyr_include_directories(memfault_config)
```

目录中至少包含这些文件：

```text
memfault_config/
  memfault_platform_config.h
  memfault_metrics_heartbeat_config.def
  memfault_trace_reason_user_config.def
```

`memfault_platform_config.h` 可以先保持为空：

```c
/*
 * Platform overrides for default memfault-firmware-sdk settings can be added
 * here when this sample needs them.
 */
```

自定义 metrics 写在 `memfault_metrics_heartbeat_config.def`：

```c
MEMFAULT_METRICS_KEY_DEFINE(button_press_count, kMemfaultMetricType_Unsigned)
MEMFAULT_METRICS_KEY_DEFINE(button_elapsed_time_ms, kMemfaultMetricType_Timer)
MEMFAULT_METRICS_KEY_DEFINE(battery_soc_pct, kMemfaultMetricType_Unsigned)
```

自定义 trace reason 写在 `memfault_trace_reason_user_config.def`：

```c
MEMFAULT_TRACE_REASON_DEFINE(button_state_changed)
```

如果代码里引用了一个 metric，但这里没有定义，编译会出现类似错误：

```text
kMfltMetricsIndex_heartbeat__button_elapsed_time_ms undeclared
```

## 5. 在代码中启用 MDS

先添加头文件：

```c
#if defined(CONFIG_BT_MDS)
#include <bluetooth/services/mds.h>
#endif

#if defined(CONFIG_MEMFAULT)
#include <memfault/core/trace_event.h>
#include <memfault/metrics/metrics.h>
#endif
```

在广播数据中加入 MDS UUID：

```c
#if defined(CONFIG_BT_MDS)
BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MDS_VAL),
#endif
```

MDS 通常应该只允许已经加密/配对的连接访问。本工程保存当前满足安全等级的连接：

```c
#if defined(CONFIG_BT_MDS)
static struct bt_conn *mds_conn;
#endif
```

安全等级变化后记录 MDS 连接：

```c
#if defined(CONFIG_BT_MDS)
if ((level >= BT_SECURITY_L2) && !mds_conn) {
    mds_conn = conn;
}
#endif
```

注册 MDS access callback：

```c
#if defined(CONFIG_BT_MDS)
static bool mds_access_enable(struct bt_conn *conn)
{
    return mds_conn && (conn == mds_conn);
}

static const struct bt_mds_cb mds_cb = {
    .access_enable = mds_access_enable,
};
#endif
```

在 `main()` 中注册：

```c
#if defined(CONFIG_BT_MDS)
err = bt_mds_cb_register(&mds_cb);
if (err) {
    printk("Memfault Diagnostic service callback registration failed (err %d)\n", err);
    return 0;
}
#endif
```

## 6. 添加按键触发示例

本工程在 `CONFIG_MEMFAULT` 启用时，把 DK 的 4 个按钮改成 Memfault 示例行为：

| 按键 | 行为 | Memfault 数据类型 |
| --- | --- | --- |
| BTN1 | 第一次启动 timer，第二次停止 timer 并触发 heartbeat | Timer metric + heartbeat |
| BTN2 | 记录按钮状态变化 | Trace event |
| BTN3 | 增加按键计数 | Unsigned metric |
| BTN4 | 触发除零 crash | Coredump + reboot reason |

核心代码：

```c
if (buttons & DK_BTN1_MSK) {
    time_measure_start = !time_measure_start;

    if (time_measure_start) {
        err = MEMFAULT_METRIC_TIMER_START(button_elapsed_time_ms);
    } else {
        err = MEMFAULT_METRIC_TIMER_STOP(button_elapsed_time_ms);
        memfault_metrics_heartbeat_debug_trigger();
    }
}

if (has_changed & DK_BTN2_MSK) {
    bool button_state = (buttons & DK_BTN2_MSK) ? 1 : 0;

    MEMFAULT_TRACE_EVENT_WITH_LOG(button_state_changed,
                                  "Button state: %u",
                                  button_state);
}

if (buttons & DK_BTN3_MSK) {
    err = MEMFAULT_METRIC_ADD(button_press_count, 1);
}

if (buttons & DK_BTN4_MSK) {
    volatile uint32_t i;
    i = 1 / 0;
    ARG_UNUSED(i);
}
```

为什么 BTN3 后云端不一定马上看到？

`MEMFAULT_METRIC_ADD(button_press_count, 1)` 只是修改当前 heartbeat 的 metric 值。需要触发 heartbeat 后，metric 才会进入 event storage 并等待导出/上传。当前工程用 BTN1 第二次按下来调用：

```c
memfault_metrics_heartbeat_debug_trigger();
```

## 7. 配置 RRAM coredump

nRF54L 系列支持 RRAM-backed coredump。启用配置：

```conf
CONFIG_MEMFAULT_COREDUMP_STORAGE_RRAM=y
```

同时必须提供名为 `memfault_coredump_partition` 的固定分区。本工程通过 `app.overlay` 在 `cpuapp_rram` 中添加 4KB 分区：

```dts
&cpuapp_rram {
    partitions {
        storage_partition: partition@1dd000 {
            reg = <0x1dd000 DT_SIZE_K(28)>;
        };

        memfault_coredump_partition: partition@1e4000 {
            label = "memfault_coredump_partition";
            reg = <0x1e4000 DT_SIZE_K(4)>;
        };
    };
};
```

这里把默认 `storage_partition` 从 32KB 缩小为 28KB，把剩余 4KB 分给 Memfault coredump。

RRAM coredump 的管理方式：

- 这是一个固定大小的单槽 coredump 区域，不是循环日志。
- crash 时如果已有有效 coredump，默认不会覆盖，保留第一次 crash。
- 如果没有有效 coredump，会清除头部标记，顺序写入 coredump，最后写 header 标记有效。
- 如果 coredump 超过分区容量，会尽量截断；关键数据也写不下时保存失败。
- 上传、导出或手动清除后，下次 crash 才能重新使用这块区域。

可以在 shell 中查看容量需求：

```sh
mflt coredump_size
```

检查和清除 coredump：

```sh
mflt get_core
mflt clear_core
```

如果 `coredump size required` 大于当前容量，需要把 `memfault_coredump_partition` 增大，例如 8KB 或 16KB，并相应调整其他分区。

## 8. RAM event storage 和 RRAM coredump 的区别

`CONFIG_MEMFAULT_EVENT_STORAGE_SIZE=4096` 控制的是 RAM event storage，不是 RRAM coredump。

RAM event storage 用来暂存：

- metrics heartbeat
- trace events
- reboot reason event
- 部分日志/事件数据

它满了会出现：

```text
<err> mflt: Event storage full
```

满了以后新事件会被丢弃并统计 drop count，不会自动挤掉旧事件。解决方式：

- 增大 `CONFIG_MEMFAULT_EVENT_STORAGE_SIZE`
- 减少短时间内产生大量 trace/metrics
- 更及时通过 MDS/App/CLI 导出 chunks

RRAM coredump 用来保存 crash 时的寄存器、栈、选定内存区域和 trace reason。它能跨复位保存，断电后通常也能保留。

## 9. 上传和验证

### 云端准备检查

上传前先确认云端侧已经准备好：

1. 已注册并进入正确的 Memfault / nRF Cloud 项目。
2. `CONFIG_MEMFAULT_NCS_PROJECT_KEY` 已替换为该项目的 Project Key。
3. `CONFIG_MEMFAULT_NCS_DEVICE_ID` 当前是预期的设备 ID。当前示例为 `nrf-hids-mouse`。
4. 已构建并烧录 Memfault 配置固件。
5. 已上传本次固件对应的 `zephyr.elf` 到 **Software → Symbol Files**，用于 crash 符号化。

### 使用手机 App

1. 手机蓝牙设置里删除旧 bond，App 里也忘记旧设备，避免 GATT 缓存。
2. 打开 nRF Connect Device Manager，扫描并连接 `Nordic_HIDS_mouse`。
3. 完成配对。如果需要 PIN，通常可以在串口日志中看到。
4. 使用 App 的 Echo / SMP 页面发送一次 `Hello`，确认 SMP 连接正常。
5. 打开 App 的 **Logs and Stats** 或 **Diagnostics** 页面，确认能看到 MDS / Observability 状态，例如 `Awaiting New Chunks` 或 `Uploaded: ... chunk(s)`。
6. 触发 BTN2/BTN3/BTN4，或者在串口中执行 `mflt test heartbeat` / `mflt test assert`。
7. 使用 App 上传 Memfault chunks。MDS 数据由手机 App 转发到 Project Key 对应的云端项目。
8. 到云端 **Integration Hub → Processing Log** 查看最近收到的数据。
9. 到云端 **Fleet → Devices** 打开对应 device id，例如 `nrf-hids-mouse`，确认 timeline 中出现 reboot、metrics、trace 或 crash 事件。
10. 如果触发过 crash，进入 **Issues** 页面确认是否出现符号化后的异常。如果没有调用栈符号，优先检查 `zephyr.elf` 是否上传且版本匹配。

如果 App 提示：

```text
SMP service not found
```

说明没有启用 MCUmgr SMP over BLE，或手机缓存了旧 GATT 服务。确认 `CONFIG_MCUMGR_TRANSPORT_BT=y` 后，删除手机 bond 并重新连接。

### 使用串口 Shell

启用 `CONFIG_MEMFAULT_SHELL=y` 后，Zephyr shell 中会注册 `mflt` 命令。当前工程也启用了 `CONFIG_SHELL=y`，因此可以在串口终端中直接输入这些命令。

最常用的验证流程：

```sh
mflt get_device_info
mflt coredump_size
mflt test heartbeat
mflt export
```

`mflt export` 会把设备上的 Memfault chunks 以文本形式导出，可以手动复制到 Memfault chunk debugger 或上传工具中验证。

## 10. Memfault CLI 用法

本节说明 `CONFIG_MEMFAULT_SHELL=y` 后可用的主要 CLI。实际能看到哪些命令取决于 Kconfig，例如 HTTP/FOTA/Networking 未启用时，相关命令可能不存在或只打印 “not enabled”。

### 基础信息命令

`mflt get_device_info`

显示设备信息，包括 device serial、software type、software version、hardware version。排查云端看不到数据时，先用它确认设备 ID 是否和 Memfault 云端页面一致。

`mflt get_reboot_reason`

显示最近一次 reboot reason。触发 BTN4 crash 后，可以用它确认 Memfault 是否记录到了异常复位原因。

`mflt metrics_dump`

打印当前 heartbeat metrics。默认等价于打印 heartbeat metrics。

```sh
mflt metrics_dump
mflt metrics_dump heartbeat
mflt metrics_dump sessions
```

`heartbeat` 显示当前 heartbeat 中的 metrics，例如 `button_press_count`、`button_elapsed_time_ms`、`battery_soc_pct`。`sessions` 用于查看 session metrics；如果没有启用相关 session metrics，可能没有有效输出。

### Coredump 命令

`mflt coredump_size`

计算当前配置下 coredump 需要的空间，并显示 coredump storage 容量。当前工程使用 RRAM 分区：

```dts
memfault_coredump_partition: partition@1e4000 {
    label = "memfault_coredump_partition";
    reg = <0x1e4000 DT_SIZE_K(4)>;
};
```

如果 `coredump size required` 大于 `coredump storage capacity`，说明分区太小，coredump 可能被截断或保存失败。

`mflt get_core`

检查当前是否已有 coredump。BTN4 触发除零 crash 后，设备重启，再运行这个命令可以确认 RRAM 中是否存在 coredump。

`mflt clear_core`

清除已经保存的 coredump。Memfault 默认不会覆盖已有有效 coredump，因此如果你想反复测试 BTN4 crash，通常需要先上传/导出，或者手动执行：

```sh
mflt clear_core
```

### 数据导出和上传命令

`mflt export`

把设备端已经生成的 Memfault chunks 通过串口打印出来。适合没有网络上传能力的 BLE 外设工程。典型流程：

```sh
mflt test heartbeat
mflt export
```

然后把输出复制到 Memfault 的 chunk decoder/debugger 或上传工具中。

`mflt post_chunks`

直接通过设备网络把 chunks 上传到 Memfault 云。这个命令依赖 `CONFIG_NETWORKING` 和 Memfault HTTP 相关配置。当前 BLE HIDS mouse 工程没有 IP 网络链路，通常不会使用这个命令。

### 测试数据命令

所有测试命令都挂在 `mflt test` 下面：

```sh
mflt test heartbeat
mflt test trace
mflt test logs
mflt test log_capture
```

`mflt test heartbeat`

立即触发一次 heartbeat，把当前 metrics 打包进 event storage。当前工程里 BTN1 第二次按下也会调用同样的逻辑：

```c
memfault_metrics_heartbeat_debug_trigger();
```

如果你按了 BTN3 看到 `button_press_count metric increased`，但云端还看不到，可以先执行：

```sh
mflt test heartbeat
mflt export
```

`mflt test trace`

生成一个示例 trace event，用于验证 trace event 管道是否正常。

`mflt test logs`

写入一些测试日志到 Memfault log buffer。

`mflt test log_capture`

触发当前 log buffer 的采集，把日志作为 Memfault 数据打包。

### Crash 和 reboot 测试命令

这些命令会故意制造异常或重启，用于验证 coredump 和 reboot reason。执行前建议先连接串口并确认已经理解后果。

```sh
mflt test reboot
mflt test assert
mflt test hardfault
mflt test busfault
mflt test memmanage
mflt test usagefault
mflt test stack_overflow
mflt test badptr
mflt test double_free
mflt test hang
mflt test isr_badptr
mflt test isr_hang
```

`mflt test reboot`

主动标记一次 user reset 并重启，用于验证 reboot reason，不一定产生 coredump。

`mflt test assert`

触发 Memfault assert，通常会产生 coredump。

`mflt test hardfault`、`busfault`、`memmanage`、`usagefault`

触发不同 ARM fault 类型，用来验证 fault handler 和 coredump 捕获。

`mflt test stack_overflow`

触发栈溢出，用于验证栈相关 coredump 信息。

`mflt test badptr`、`double_free`

触发典型内存错误。

`mflt test hang`、`isr_hang`

制造死循环。如果没有 watchdog，设备可能会一直卡住，需要手动复位。

`mflt test isr_badptr`

在 ISR 上下文触发非法地址访问，用于验证中断场景下的异常捕获。

`mflt test loadaddr`

测试从指定地址读取 32-bit 数据。该命令更偏底层调试，误用可能触发 fault。

### FOTA 相关命令

SDK 还可能注册 `mflt_nrf` 命令：

```sh
mflt_nrf fota
mflt_nrf get_latest_url
```

这些命令依赖 Memfault HTTP/FOTA 配置。当前 BLE HIDS mouse 示例主要通过 MDS/CLI 导出数据，并没有配置完整 IP 网络和 FOTA 流程，因此通常不使用。

### 推荐调试顺序

第一次接入 Memfault 时，建议按下面顺序验证：

```sh
mflt get_device_info
mflt coredump_size
mflt metrics_dump
mflt test heartbeat
mflt export
```

验证 metrics：

```sh
# 按 BTN3 多次
mflt metrics_dump
mflt test heartbeat
mflt export
```

验证 coredump：

```sh
mflt clear_core
# 按 BTN4 触发 crash，等待重启
mflt get_core
mflt export
```

## 11. 构建命令

普通短路径工程可以直接构建：

```powershell
west build --build-dir build_mds . --pristine --board nrf54lm20dk/nrf54lm20a/cpuapp -- -DCONF_FILE="prj_mds.conf"
```

如果 Windows 路径过长，建议把构建目录放到短路径，例如：

```powershell
west build --build-dir D:/b/hids_mds D:/peripheral_hid_mouse_mds --pristine --board nrf54lm20dk/nrf54lm20a/cpuapp -- -DCONF_FILE="prj_mds.conf"
```

路径过长时，单纯启用 Windows LongPaths 或修改 `CMAKE_OBJECT_PATH_MAX` 不一定够，因为工具链写 `.obj.d` 依赖文件时仍可能失败。短构建目录是最稳定的做法。

## 12. 接入检查清单

- Memfault / nRF Cloud 账号已注册
- 已创建或进入正确项目
- `CONFIG_MEMFAULT=y`
- `CONFIG_MEMFAULT_SHELL=y`
- `CONFIG_MEMFAULT_NCS_PROJECT_KEY` 已替换为真实 Project Key
- Device ID 已选择合适策略：单板调试可用 `CONFIG_MEMFAULT_NCS_DEVICE_ID_STATIC`，多设备推荐 `CONFIG_MEMFAULT_NCS_DEVICE_ID_HW_ID`，生产序列号可用 `CONFIG_MEMFAULT_NCS_DEVICE_ID_RUNTIME`
- Device ID 对每台设备唯一且长期稳定
- 已上传当前固件对应的 `zephyr.elf` 到 Symbol Files
- 已添加 `memfault_config` 目录
- 自定义 metric 已写入 `memfault_metrics_heartbeat_config.def`
- 自定义 trace reason 已写入 `memfault_trace_reason_user_config.def`
- BLE 传输使用 MDS 时已启用 `CONFIG_BT_MDS=y`
- 手机 Device Manager 需要 SMP 时已启用 `CONFIG_MCUMGR_TRANSPORT_BT=y`
- RRAM coredump 已启用 `CONFIG_MEMFAULT_COREDUMP_STORAGE_RRAM=y`
- Devicetree 或 Partition Manager 中存在 `memfault_coredump_partition`
- 能运行 `mflt coredump_size`
- 能通过 BTN1 或 `mflt test heartbeat` 触发 heartbeat
- 能通过 App 或 `mflt export` 导出 chunks

