#if defined(CONFIG_MEMFAULT_FIXED_PARTITION_COREDUMP)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nrfx_rramc.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#include <memfault/components.h>
#include <memfault/ports/buffered_coredump_storage.h>

#define RRAM_WRITE_BUFFER_SIZE 1
#define RRAM_WRITE_LINE_SIZE 16
#define FLASH_NODE DT_CHOSEN(zephyr_flash)
#define PARTITION_OFFSET FIXED_PARTITION_OFFSET(memfault_coredump_partition)
#define PARTITION_SIZE FIXED_PARTITION_SIZE(memfault_coredump_partition)
#define PARTITION_ADDR (DT_REG_ADDR(FLASH_NODE) + PARTITION_OFFSET)

BUILD_ASSERT(FIXED_PARTITION_EXISTS(memfault_coredump_partition),
	     "memfault_coredump_partition fixed partition is required");
BUILD_ASSERT((PARTITION_SIZE % MEMFAULT_COREDUMP_STORAGE_WRITE_SIZE) == 0,
	     "memfault_coredump_partition size must be aligned to write size");
BUILD_ASSERT((MEMFAULT_COREDUMP_STORAGE_WRITE_SIZE % sizeof(uint32_t)) == 0,
	     "Memfault coredump write size must be word aligned");
BUILD_ASSERT((MEMFAULT_COREDUMP_STORAGE_WRITE_SIZE % RRAM_WRITE_LINE_SIZE) == 0,
	     "Memfault coredump write size must be aligned to RRAM write line");

static bool coredump_cleared;

static bool op_within_partition(uint32_t offset, size_t len)
{
	return (offset <= PARTITION_SIZE) && (len <= (PARTITION_SIZE - offset));
}

static void rram_write(uint32_t offset, const void *data, size_t len)
{
	nrfx_rramc_write_enable_set(true, RRAM_WRITE_BUFFER_SIZE);

	if (data != NULL) {
		memcpy((void *)(PARTITION_ADDR + offset), data, len);
	} else {
		memset((void *)(PARTITION_ADDR + offset), 0xff, len);
	}

	barrier_dmem_fence_full();
	nrfx_rramc_write_enable_set(false, RRAM_WRITE_BUFFER_SIZE);
}

void memfault_platform_coredump_storage_get_info(sMfltCoredumpStorageInfo *info)
{
	*info = (sMfltCoredumpStorageInfo) {
		.size = PARTITION_SIZE,
	};
}

bool memfault_platform_coredump_storage_read(uint32_t offset, void *data, size_t read_len)
{
	if (!op_within_partition(offset, read_len)) {
		return false;
	}

	memcpy(data, (const void *)(PARTITION_ADDR + offset), read_len);
	return true;
}

bool memfault_platform_coredump_storage_erase(uint32_t offset, size_t erase_size)
{
	if (!op_within_partition(offset, erase_size)) {
		return false;
	}

	rram_write(offset, NULL, erase_size);
	return true;
}

bool memfault_platform_coredump_storage_buffered_write(sCoredumpWorkingBuffer *blk)
{
	if (!op_within_partition(blk->write_offset, MEMFAULT_COREDUMP_STORAGE_WRITE_SIZE)) {
		return false;
	}

	rram_write(blk->write_offset, blk->data, MEMFAULT_COREDUMP_STORAGE_WRITE_SIZE);
	return true;
}

bool memfault_coredump_read(uint32_t offset, void *data, size_t read_len)
{
	if (coredump_cleared) {
		memset(data, 0, read_len);
		return true;
	}

	return memfault_platform_coredump_storage_read(offset, data, read_len);
}

void memfault_platform_coredump_storage_clear(void)
{
	uint8_t clear_data[RRAM_WRITE_LINE_SIZE] = { 0 };

	if (op_within_partition(0, sizeof(clear_data))) {
		rram_write(0, clear_data, sizeof(clear_data));
		coredump_cleared = true;
	}
}

#endif
