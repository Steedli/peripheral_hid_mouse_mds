/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Platform overrides for default memfault-firmware-sdk settings can be added
 * here when this sample needs them.
 */

#ifdef CONFIG_MEMFAULT_COREDUMP_STORAGE_NRF_RRAM
#include <zephyr/storage/flash_map.h>

/*
 * The RRAM coredump backend
 * (ports/zephyr/common/coredump_storage/memfault_nrf_rram_backed_coredump.c)
 * resolves its partition only through DT_HAS_FIXED_PARTITION_LABEL(), which the
 * device tree generator emits exclusively for children of a "fixed-partitions"
 * node. The nRF54L/nRF54LM partition layouts use "zephyr,mapped-partition"
 * instead, so that macro is 0 and the backend ends up with neither
 * MEMFAULT_COREDUMP_PARTITION_ADDRESS nor _SIZE defined, which fails to compile.
 *
 * Define them here (this header is pulled in via memfault/components.h before
 * the backend's own #if chain runs). PARTITION_ADDRESS()/PARTITION_SIZE() handle
 * both partition schemes, so this stays correct if the layout is ever switched
 * back to "fixed-partitions".
 */
#define MEMFAULT_COREDUMP_PARTITION_ADDRESS PARTITION_ADDRESS(memfault_coredump_partition)
#define MEMFAULT_COREDUMP_PARTITION_SIZE    PARTITION_SIZE(memfault_coredump_partition)
#endif /* CONFIG_MEMFAULT_COREDUMP_STORAGE_NRF_RRAM */
