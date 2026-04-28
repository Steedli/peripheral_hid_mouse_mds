/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>
#include <assert.h>

#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/bluetooth/services/bas.h>
#include <bluetooth/services/hids.h>
#if defined(CONFIG_BT_MDS)
#include <bluetooth/services/mds.h>
#endif
#include <zephyr/bluetooth/services/dis.h>
#include <dk_buttons_and_leds.h>

#if defined(CONFIG_MEMFAULT)
#include <memfault/core/trace_event.h>
#include <memfault/metrics/metrics.h>
#endif

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION   0x0101

/* Number of pixels by which the cursor is moved when a button is pushed. */
#define MOVEMENT_SPEED              5
/* Number of input reports in this application. */
#define INPUT_REPORT_COUNT          3
/* Length of Mouse Input Report containing button data. */
#define INPUT_REP_BUTTONS_LEN       3
/* Length of Mouse Input Report containing movement data. */
#define INPUT_REP_MOVEMENT_LEN      3
/* Length of Mouse Input Report containing media player data. */
#define INPUT_REP_MEDIA_PLAYER_LEN  1
/* Index of Mouse Input Report containing button data. */
#define INPUT_REP_BUTTONS_INDEX     0
/* Index of Mouse Input Report containing movement data. */
#define INPUT_REP_MOVEMENT_INDEX    1
/* Index of Mouse Input Report containing media player data. */
#define INPUT_REP_MPLAYER_INDEX     2
/* Id of reference to Mouse Input Report containing button data. */
#define INPUT_REP_REF_BUTTONS_ID    1
/* Id of reference to Mouse Input Report containing movement data. */
#define INPUT_REP_REF_MOVEMENT_ID   2
/* Id of reference to Mouse Input Report containing media player data. */
#define INPUT_REP_REF_MPLAYER_ID    3

/* HIDs queue size. */
#define HIDS_QUEUE_SIZE 10

/* Key used to move cursor left */
#define KEY_LEFT_MASK   DK_BTN1_MSK
/* Key used to move cursor up */
#define KEY_UP_MASK     DK_BTN2_MSK
/* Key used to move cursor right */
#define KEY_RIGHT_MASK  DK_BTN3_MSK
/* Key used to move cursor down */
#define KEY_DOWN_MASK   DK_BTN4_MSK

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

/* Key used to erase all bonds (long press 5 seconds) */
#define KEY_BOND_ERASE_MASK DK_BTN1_MSK

/* HIDS instance. */
BT_HIDS_DEF(hids_obj,
	    INPUT_REP_BUTTONS_LEN,
	    INPUT_REP_MOVEMENT_LEN,
	    INPUT_REP_MEDIA_PLAYER_LEN);

static struct k_work hids_work;
struct mouse_pos {
	int16_t x_val;
	int16_t y_val;
};

/* Mouse movement queue. */
K_MSGQ_DEFINE(hids_queue,
	      sizeof(struct mouse_pos),
	      HIDS_QUEUE_SIZE,
	      4);

#if CONFIG_BT_DIRECTED_ADVERTISING
/* Bonded address queue. */
K_MSGQ_DEFINE(bonds_queue,
	      sizeof(bt_addr_le_t),
	      CONFIG_BT_MAX_PAIRED,
	      4);

/* Directed advertising retry tracking */
static bt_addr_le_t current_dir_adv_addr;
static uint8_t dir_adv_retry_count;
#define DIR_ADV_MAX_RETRIES 3
#endif

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
					  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
#if defined(CONFIG_BT_MDS)
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MDS_VAL),
#endif
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static struct conn_mode {
	struct bt_conn *conn;
	bool in_boot_mode;
} conn_mode[CONFIG_BT_HIDS_MAX_CLIENT_COUNT];

static volatile bool is_adv_running;
static bool pending_repairing;
/* Active BT identity used for advertising; always ID 1 (ID 0 cannot be reset) */
static uint8_t current_adv_id = 1U;
#if defined(CONFIG_BT_MDS)
static struct bt_conn *mds_conn;
#endif

static struct k_work adv_work;

static struct k_work pairing_work;
static struct k_work_delayable bond_erase_work;
static struct k_work_delayable post_erase_adv_work;

/* Forward declaration */
static void identity_refresh(void);

struct pairing_data_mitm {
	struct bt_conn *conn;
	unsigned int passkey;
};

K_MSGQ_DEFINE(mitm_queue,
	      sizeof(struct pairing_data_mitm),
	      CONFIG_BT_HIDS_MAX_CLIENT_COUNT,
	      4);

#if CONFIG_BT_DIRECTED_ADVERTISING
static void bond_find(const struct bt_bond_info *info, void *user_data)
{
	int err;

	/* Filter already connected peers. */
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn) {
			const bt_addr_le_t *dst =
				bt_conn_get_dst(conn_mode[i].conn);

			if (!bt_addr_le_cmp(&info->addr, dst)) {
				return;
			}
		}
	}

	err = k_msgq_put(&bonds_queue, (void *) &info->addr, K_NO_WAIT);
	if (err) {
		printk("No space in the queue for the bond.\n");
	}
}
#endif

static void advertising_continue(void)
{
	struct bt_le_adv_param adv_param;

#if CONFIG_BT_DIRECTED_ADVERTISING
	bt_addr_le_t addr;
	bool has_addr = false;

	/* Check if we have a retry pending */
	if (dir_adv_retry_count > 0 && dir_adv_retry_count < DIR_ADV_MAX_RETRIES) {
		addr = current_dir_adv_addr;
		has_addr = true;
		dir_adv_retry_count++;
		printk("Retrying directed advertising (%u/%u)\n", 
		       dir_adv_retry_count, DIR_ADV_MAX_RETRIES);
	} else if (!k_msgq_get(&bonds_queue, &addr, K_NO_WAIT)) {
		/* New bonded device to try */
		has_addr = true;
		current_dir_adv_addr = addr;
		dir_adv_retry_count = 1;
	} else if (dir_adv_retry_count >= DIR_ADV_MAX_RETRIES) {
		/* Max retries reached, reset counter */
		dir_adv_retry_count = 0;
	}

	if (has_addr) {
		char addr_buf[BT_ADDR_LE_STR_LEN];
		int err;

		if (is_adv_running) {
			err = bt_le_adv_stop();
			if (err) {
				printk("Advertising failed to stop (err %d)\n", err);
				return;
			}
			is_adv_running = false;
		}

		/* Use high duty cycle for all attempts - it has automatic timeout */
		adv_param = *BT_LE_ADV_CONN_DIR(&addr);
		// adv_param.options |= BT_LE_ADV_OPT_DIR_ADDR_RPA;
		adv_param.options |= BT_LE_ADV_OPT_USE_IDENTITY;
		adv_param.id = current_adv_id;

		err = bt_le_adv_start(&adv_param, NULL, 0, NULL, 0);

		if (err) {
			printk("Directed advertising failed to start (err %d)\n", err);
			return;
		}

		bt_addr_le_to_str(&addr, addr_buf, BT_ADDR_LE_STR_LEN);
		printk("Direct advertising to %s started\n", addr_buf);
	} else
#endif
	{
		int err;

		if (is_adv_running) {
			return;
		}
		/* Use whitelist/filter policy for scanning requests */
		/* Only bonded devices in the filter accept list can discover this device */
		// adv_param = *BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN | 
		// 				    BT_LE_ADV_OPT_FILTER_CONN,
		// 				    BT_GAP_ADV_FAST_INT_MIN_2,
		// 				    BT_GAP_ADV_FAST_INT_MAX_2,
		// 				    NULL);
		adv_param = *BT_LE_ADV_CONN_FAST_2;
		adv_param.id = current_adv_id;
		err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
		if (err) {
			printk("Advertising failed to start (err %d)\n", err);
			return;
		}

		printk("Regular advertising started\n");
	}

	is_adv_running = true;
}

static void advertising_start(void)
{
#if CONFIG_BT_DIRECTED_ADVERTISING
	k_msgq_purge(&bonds_queue);
	bt_foreach_bond(current_adv_id, bond_find, NULL);
	dir_adv_retry_count = 0; /* Reset retry counter */
#endif

	k_work_submit(&adv_work);
}

static void advertising_process(struct k_work *work)
{
	advertising_continue();
}

static void pairing_process(struct k_work *work)
{
	int err;
	struct pairing_data_mitm pairing_data;

	char addr[BT_ADDR_LE_STR_LEN];

	err = k_msgq_peek(&mitm_queue, &pairing_data);
	if (err) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(pairing_data.conn),
			  addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, pairing_data.passkey);

	if (IS_ENABLED(CONFIG_SOC_SERIES_NRF54HX) || IS_ENABLED(CONFIG_SOC_SERIES_NRF54LX)) {
		printk("Press Button 0 to confirm, Button 1 to reject.\n");
	} else {
		printk("Press Button 1 to confirm, Button 2 to reject.\n");
	}
}


static void insert_conn_object(struct bt_conn *conn)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (!conn_mode[i].conn) {
			conn_mode[i].conn = conn;
			conn_mode[i].in_boot_mode = false;

			return;
		}
	}

	printk("Connection object could not be inserted %p\n", conn);
}


static bool is_conn_slot_free(void)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (!conn_mode[i].conn) {
			return true;
		}
	}

	return false;
}


static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	is_adv_running = false;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		if (err == BT_HCI_ERR_ADV_TIMEOUT) {
			printk("Direct advertising to %s timed out\n", addr);
			k_work_submit(&adv_work);
		} else {
			printk("Failed to connect to %s 0x%02x %s\n", addr, err,
			       bt_hci_err_to_str(err));
		}
		return;
	}

	printk("Connected %s\n", addr);

	struct bt_conn_info info;
	err = bt_conn_get_info(conn, &info);
	if (!err) {
		uint32_t interval_us = BT_CONN_INTERVAL_TO_US(info.le.interval);
		uint32_t interval_ms = interval_us / 1000;
		uint32_t interval_frac = (interval_us % 1000) / 10;
		printk("Connection interval: %u.%02u ms\n",
		       interval_ms, interval_frac);
		printk("Latency: %u, Timeout: %u ms\n",
		       info.le.latency,
		       info.le.timeout * 10);
	}

	err = bt_hids_connected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about connection\n");
		return;
	}

	/* Request connection parameter update for low latency */
	struct bt_le_conn_param param = {
		.interval_min = 6,   /* 7.5 ms */
		.interval_max = 6,   /* 7.5 ms */
		.latency = 0,       /* Can skip 0 events */
		.timeout = 400,      /* 4000 ms */
	};
	err = bt_conn_le_param_update(conn, &param);
	if (err) {
		printk("Failed to request conn param update (err %d)\n", err);
	}

	insert_conn_object(conn);

	if (is_conn_slot_free()) {
		advertising_start();
	}
}


static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

	err = bt_hids_disconnected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about disconnection\n");
	}

	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			conn_mode[i].conn = NULL;
			break;
		}
	}

#if defined(CONFIG_BT_MDS)
	if (conn == mds_conn) {
		mds_conn = NULL;
	}
#endif

	if (pending_repairing) {
		/* Bond erase in progress; check if all connections are gone.
		 * Only safe to call bt_id_reset() once no connections are active.
		 */
		bool all_disconnected = true;

		for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
			if (conn_mode[i].conn) {
				all_disconnected = false;
				break;
			}
		}
		if (all_disconnected) {
			identity_refresh();
		}
	} else {
		advertising_start();
	}
}


static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
		/* Pairing or re-pairing succeeded; return to normal operation */
		pending_repairing = false;
#if defined(CONFIG_BT_MDS)
		if ((level >= BT_SECURITY_L2) && !mds_conn) {
			mds_conn = conn;
		}
#endif
	} else {
		printk("Security failed: %s level %u err %d %s\n", addr, level, err,
		       bt_security_err_to_str(err));

		if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
			/* Remote still holds the old LTK; our bond is already gone.
			 * The user pressed BTN4 to start this advertising session,
			 * so request fresh SMP pairing on the current connection.
			 */
			int ret = bt_conn_set_security(conn,
						       BT_SECURITY_L2 |
						       BT_SECURITY_FORCE_PAIR);
			if (ret) {
				printk("Re-pairing request failed (err %d)\n", ret);
			} else {
				printk("Re-pairing requested for %s\n", addr);
			}
		}
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

#if defined(CONFIG_BT_MDS)
static bool mds_access_enable(struct bt_conn *conn)
{
	return mds_conn && (conn == mds_conn);
}

static const struct bt_mds_cb mds_cb = {
	.access_enable = mds_access_enable,
};
#endif


static void hids_pm_evt_handler(enum bt_hids_pm_evt evt,
				struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	size_t i;

	for (i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			break;
		}
	}

	if (i >= CONFIG_BT_HIDS_MAX_CLIENT_COUNT) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	switch (evt) {
	case BT_HIDS_PM_EVT_BOOT_MODE_ENTERED:
		printk("Boot mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = true;
		break;

	case BT_HIDS_PM_EVT_REPORT_MODE_ENTERED:
		printk("Report mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = false;
		break;

	default:
		break;
	}
}


static void hid_init(void)
{
	int err;
	struct bt_hids_init_param hids_init_param = { 0 };
	struct bt_hids_inp_rep *hids_inp_rep;
	static const uint8_t mouse_movement_mask[DIV_ROUND_UP(INPUT_REP_MOVEMENT_LEN, 8)] = {0};

	static const uint8_t report_map[] = {
		0x05, 0x01,     /* Usage Page (Generic Desktop) */
		0x09, 0x02,     /* Usage (Mouse) */

		0xA1, 0x01,     /* Collection (Application) */

		/* Report ID 1: Mouse buttons + scroll/pan */
		0x85, 0x01,       /* Report Id 1 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x95, 0x05,       /* Report Count (3) */
		0x75, 0x01,       /* Report Size (1) */
		0x05, 0x09,       /* Usage Page (Buttons) */
		0x19, 0x01,       /* Usage Minimum (01) */
		0x29, 0x05,       /* Usage Maximum (05) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x01,       /* Logical Maximum (1) */
		0x81, 0x02,       /* Input (Data, Variable, Absolute) */
		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x03,       /* Report Size (3) */
		0x81, 0x01,       /* Input (Constant) for padding */
		0x75, 0x08,       /* Report Size (8) */
		0x95, 0x01,       /* Report Count (1) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x38,       /* Usage (Wheel) */
		0x15, 0x81,       /* Logical Minimum (-127) */
		0x25, 0x7F,       /* Logical Maximum (127) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0x05, 0x0C,       /* Usage Page (Consumer) */
		0x0A, 0x38, 0x02, /* Usage (AC Pan) */
		0x95, 0x01,       /* Report Count (1) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0xC0,             /* End Collection (Physical) */

		/* Report ID 2: Mouse motion */
		0x85, 0x02,       /* Report Id 2 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x75, 0x0C,       /* Report Size (12) */
		0x95, 0x02,       /* Report Count (2) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x30,       /* Usage (X) */
		0x09, 0x31,       /* Usage (Y) */
		0x16, 0x01, 0xF8, /* Logical maximum (2047) */
		0x26, 0xFF, 0x07, /* Logical minimum (-2047) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0xC0,             /* End Collection (Physical) */
		0xC0,             /* End Collection (Application) */

		/* Report ID 3: Advanced buttons */
		0x05, 0x0C,       /* Usage Page (Consumer) */
		0x09, 0x01,       /* Usage (Consumer Control) */
		0xA1, 0x01,       /* Collection (Application) */
		0x85, 0x03,       /* Report Id (3) */
		0x15, 0x00,       /* Logical minimum (0) */
		0x25, 0x01,       /* Logical maximum (1) */
		0x75, 0x01,       /* Report Size (1) */
		0x95, 0x01,       /* Report Count (1) */

		0x09, 0xCD,       /* Usage (Play/Pause) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x0A, 0x83, 0x01, /* Usage (Consumer Control Configuration) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x09, 0xB5,       /* Usage (Scan Next Track) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x09, 0xB6,       /* Usage (Scan Previous Track) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */

		0x09, 0xEA,       /* Usage (Volume Down) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x09, 0xE9,       /* Usage (Volume Up) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x0A, 0x25, 0x02, /* Usage (AC Forward) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x0A, 0x24, 0x02, /* Usage (AC Back) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0xC0              /* End Collection */
	};

	hids_init_param.rep_map.data = report_map;
	hids_init_param.rep_map.size = sizeof(report_map);

	hids_init_param.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	hids_init_param.info.b_country_code = 0x00;
	hids_init_param.info.flags = (BT_HIDS_REMOTE_WAKE |
				      BT_HIDS_NORMALLY_CONNECTABLE);

	hids_inp_rep = &hids_init_param.inp_rep_group_init.reports[0];
	hids_inp_rep->size = INPUT_REP_BUTTONS_LEN;
	hids_inp_rep->id = INPUT_REP_REF_BUTTONS_ID;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_inp_rep++;
	hids_inp_rep->size = INPUT_REP_MOVEMENT_LEN;
	hids_inp_rep->id = INPUT_REP_REF_MOVEMENT_ID;
	hids_inp_rep->rep_mask = mouse_movement_mask;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_inp_rep++;
	hids_inp_rep->size = INPUT_REP_MEDIA_PLAYER_LEN;
	hids_inp_rep->id = INPUT_REP_REF_MPLAYER_ID;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_init_param.is_mouse = true;
	hids_init_param.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init_param);
	__ASSERT(err == 0, "HIDS initialization failed\n");
}


static void mouse_movement_send(int16_t x_delta, int16_t y_delta)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {

		if (!conn_mode[i].conn) {
			continue;
		}

		if (conn_mode[i].in_boot_mode) {
			x_delta = MAX(MIN(x_delta, SCHAR_MAX), SCHAR_MIN);
			y_delta = MAX(MIN(y_delta, SCHAR_MAX), SCHAR_MIN);

			bt_hids_boot_mouse_inp_rep_send(&hids_obj,
							     conn_mode[i].conn,
							     NULL,
							     (int8_t) x_delta,
							     (int8_t) y_delta,
							     NULL);
		} else {
			uint8_t x_buff[2];
			uint8_t y_buff[2];
			uint8_t buffer[INPUT_REP_MOVEMENT_LEN];

			int16_t x = MAX(MIN(x_delta, 0x07ff), -0x07ff);
			int16_t y = MAX(MIN(y_delta, 0x07ff), -0x07ff);

			/* Convert to little-endian. */
			sys_put_le16(x, x_buff);
			sys_put_le16(y, y_buff);

			/* Encode report. */
			BUILD_ASSERT(sizeof(buffer) == 3,
					 "Only 2 axis, 12-bit each, are supported");

			buffer[0] = x_buff[0];
			buffer[1] = (y_buff[0] << 4) | (x_buff[1] & 0x0f);
			buffer[2] = (y_buff[1] << 4) | (y_buff[0] >> 4);


			bt_hids_inp_rep_send(&hids_obj, conn_mode[i].conn,
						  INPUT_REP_MOVEMENT_INDEX,
						  buffer, sizeof(buffer), NULL);
		}
	}
}


static void mouse_handler(struct k_work *work)
{
	struct mouse_pos pos;

	while (!k_msgq_get(&hids_queue, &pos, K_NO_WAIT)) {
		mouse_movement_send(pos.x_val, pos.y_val);
	}
}

#if defined(CONFIG_BT_HIDS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}


static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	int err;

	struct pairing_data_mitm pairing_data;

	pairing_data.conn    = bt_conn_ref(conn);
	pairing_data.passkey = passkey;

	err = k_msgq_put(&mitm_queue, &pairing_data, K_NO_WAIT);
	if (err) {
		printk("Pairing queue is full. Purge previous data.\n");
	}

	/* In the case of multiple pairing requests, trigger
	 * pairing confirmation which needed user interaction only
	 * once to avoid display information about all devices at
	 * the same time. Passkey confirmation for next devices will
	 * be proccess from queue after handling the earlier ones.
	 */
	if (k_msgq_num_used_get(&mitm_queue) == 1) {
		k_work_submit(&pairing_work);
	}
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct pairing_data_mitm pairing_data;

	if (k_msgq_peek(&mitm_queue, &pairing_data) != 0) {
		return;
	}

	if (pairing_data.conn == conn) {
		bt_conn_unref(pairing_data.conn);
		k_msgq_get(&mitm_queue, &pairing_data, K_NO_WAIT);
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d %s\n", addr, reason,
	       bt_security_err_to_str(reason));
}

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif /* defined(CONFIG_BT_HIDS_SECURITY_ENABLED) */

static void post_erase_adv_handler(struct k_work *work)
{
	pending_repairing = false;
	printk("Advertising with new identity (ID %u)\n", current_adv_id);
	advertising_start();
}

static void identity_refresh(void)
{
	int err;
	char addr_str[BT_ADDR_LE_STR_LEN];

	/* ID 1 is always used; bt_id_reset() resets its address and IRK.
	 * The PC holds the old IRK for ID 1 and cannot resolve RPAs from
	 * the new IRK, so it treats this as a brand-new device.
	 */
	err = bt_id_reset(current_adv_id, NULL, NULL);
	if (err < 0) {
		printk("bt_id_reset(ID %u) failed (err %d)\n", current_adv_id, err);
		return;
	}

	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = CONFIG_BT_ID_MAX;

	bt_id_get(addrs, &count);
	bt_addr_le_to_str(&addrs[current_adv_id], addr_str, sizeof(addr_str));
	printk("BT identity %u reset OK, new addr: %s\n", current_adv_id, addr_str);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_save();
	}

	/* Wait 3s so Windows abandons auto-reconnect before we appear */
	printk("Waiting 3s before advertising...\n");
	k_work_schedule(&post_erase_adv_work, K_SECONDS(3));
}


static void bond_erase_handler(struct k_work *work)
{
	bool has_conn = false;

	pending_repairing = true;

	if (is_adv_running) {
		bt_le_adv_stop();
		is_adv_running = false;
	}

	printk("Erasing all bonds\n");
	bt_unpair(current_adv_id, BT_ADDR_LE_ANY);

	/* identity_refresh() must be called with no active connections */
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn) {
			bt_conn_disconnect(conn_mode[i].conn,
					   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			has_conn = true;
		}
	}

	if (!has_conn) {
		identity_refresh();
	} else {
		printk("Disconnecting... identity refresh after connection drops\n");
	}
}


#if defined(CONFIG_MEMFAULT)
static void memfault_button_handler(uint32_t button_state, uint32_t has_changed)
{
	static bool time_measure_start;

	int err;
	uint32_t buttons = button_state & has_changed;

	if (buttons & DK_BTN1_MSK) {
		time_measure_start = !time_measure_start;

		if (time_measure_start) {
			err = MEMFAULT_METRIC_TIMER_START(button_elapsed_time_ms);
			if (err) {
				printk("Failed to start memfault metrics timer: %d\n", err);
			} else {
				printk("button_elapsed_time_ms timer started\n");
			}
		} else {
			err = MEMFAULT_METRIC_TIMER_STOP(button_elapsed_time_ms);
			if (err) {
				printk("Failed to stop memfault metrics: %d\n", err);
			} else {
				printk("button_elapsed_time_ms timer stopped\n");
			}

			/* Trigger collection of heartbeat data. */
			memfault_metrics_heartbeat_debug_trigger();
			printk("Memfault heartbeat metrics triggered\n");
		}
	}

	if (has_changed & DK_BTN2_MSK) {
		bool button_state = (buttons & DK_BTN2_MSK) ? 1 : 0;

		MEMFAULT_TRACE_EVENT_WITH_LOG(button_state_changed, "Button state: %u",
					      button_state);

		printk("button_state_changed event has been tracked, button state: %u\n",
		       button_state);
	}

	if (buttons & DK_BTN3_MSK) {
		err = MEMFAULT_METRIC_ADD(button_press_count, 1);
		if (err) {
			printk("Failed to increase button_press_count metric: %d\n", err);
		} else {
			printk("button_press_count metric increased\n");
		}
	}

	if (buttons & DK_BTN4_MSK) {
		volatile uint32_t i;

		printk("Division by zero will now be triggered\n");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiv-by-zero"
		i = 1 / 0;
#pragma GCC diagnostic pop
		ARG_UNUSED(i);
	}
}
#endif


void button_changed(uint32_t button_state, uint32_t has_changed)
{
#if defined(CONFIG_MEMFAULT)
	memfault_button_handler(button_state, has_changed);
#else
	bool data_to_send = false;
	struct mouse_pos pos;
	uint32_t buttons = button_state & has_changed;

	memset(&pos, 0, sizeof(struct mouse_pos));

	/* Long press bond erase detection: press and hold BTN1 for 5 seconds */
	if (has_changed & KEY_BOND_ERASE_MASK) {
		if (button_state & KEY_BOND_ERASE_MASK) {
			k_work_schedule(&bond_erase_work, K_SECONDS(5));
			printk("Bond erase armed, hold for 5s to erase\n");
		} else {
			if (k_work_cancel_delayable(&bond_erase_work) == 0) {
				printk("Bond erase cancelled\n");
			}
		}
	}

	if (buttons & KEY_LEFT_MASK) {
		pos.x_val -= MOVEMENT_SPEED;
		printk("%s(): left\n", __func__);
		data_to_send = true;
	}
	if (buttons & KEY_UP_MASK) {
		pos.y_val -= MOVEMENT_SPEED;
		printk("%s(): up\n", __func__);
		data_to_send = true;
	}
	if (buttons & KEY_RIGHT_MASK) {
		pos.x_val += MOVEMENT_SPEED;
		printk("%s(): right\n", __func__);
		data_to_send = true;
	}
	if (buttons & KEY_DOWN_MASK) {
		pos.y_val += MOVEMENT_SPEED;
		printk("%s(): down\n", __func__);
		data_to_send = true;
	}

	if (data_to_send) {
		int err;

		err = k_msgq_put(&hids_queue, &pos, K_NO_WAIT);
		if (err) {
			printk("No space in the queue for button pressed\n");
			return;
		}
		if (k_msgq_num_used_get(&hids_queue) == 1) {
			k_work_submit(&hids_work);
		}
	}
#endif
}


void configure_buttons(void)
{
	int err;

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err: %d)\n", err);
	}
}


static void bas_notify(void)
{
	uint8_t battery_level = bt_bas_get_battery_level();

	battery_level--;

	if (!battery_level) {
		battery_level = 100U;
	}

#if defined(CONFIG_MEMFAULT)
	int err = MEMFAULT_METRIC_SET_UNSIGNED(battery_soc_pct, battery_level);

	if (err) {
		printk("Failed to set battery_soc_pct metric (err %d)\n", err);
	}
#endif

	bt_bas_set_battery_level(battery_level);
}


int main(void)
{
	int err;
	printk("Starting Bluetooth Peripheral HIDS mouse sample\n");

	if (IS_ENABLED(CONFIG_BT_HIDS_SECURITY_ENABLED)) {

		err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
		if (err) {
			printk("Failed to register authorization info callbacks.\n");
			return 0;
		}
	}

	/* DIS initialized at system boot with SYS_INIT macro. */
	hid_init();

#if defined(CONFIG_BT_MDS)
	err = bt_mds_cb_register(&mds_cb);
	if (err) {
		printk("Memfault Diagnostic service callback registration failed (err %d)\n", err);
		return 0;
	}
#endif

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	k_work_init(&hids_work, mouse_handler);
	k_work_init(&adv_work, advertising_process);
	k_work_init_delayable(&bond_erase_work, bond_erase_handler);
	k_work_init_delayable(&post_erase_adv_work, post_erase_adv_handler);
	if (IS_ENABLED(CONFIG_BT_HIDS_SECURITY_ENABLED)) {
		k_work_init(&pairing_work, pairing_process);
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	/* Ensure ID 1 exists; it is the only identity we advertise with.
	 * Check via bt_id_get: if count < 2, ID 1 does not exist yet.
	 */
	{
		bt_addr_le_t id_addrs[CONFIG_BT_ID_MAX];
		size_t id_count = CONFIG_BT_ID_MAX;

		bt_id_get(id_addrs, &id_count);
		if (id_count < 2) {
			int id1 = bt_id_create(NULL, NULL);

			if (id1 < 0) {
				printk("Failed to create BT ID 1 (err %d)\n", id1);
			} else {
				printk("BT ID 1 created\n");
			}
		} else {
			printk("BT ID 1 already exists\n");
		}
	}

	advertising_start();

	configure_buttons();

	while (1) {
		k_sleep(K_SECONDS(1));
		/* Battery level simulation */
		bas_notify();
	}
}
