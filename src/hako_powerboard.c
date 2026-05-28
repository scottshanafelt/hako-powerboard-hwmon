// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hwmon driver for the HakoForge Hako-Core Powerboard.
 *
 * The powerboard is a USB CDC ACM device that drives three rows of 4-pin PWM
 * fans and exposes tach feedback plus four ADC shunt channels for 12V rail
 * power monitoring. Communication is a newline-delimited text protocol;
 * see PLAN.md for the wire format.
 *
 * v0.2 scope: fan PWM (RW), fan tach (R), 12V rail wattage (R), device metadata.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>

#define HAKO_DRV_NAME		"hako_powerboard"

#define HAKO_VID		0x2341
#define HAKO_PID		0x8036

#define HAKO_RX_BUF_SZ		64
#define HAKO_TX_BUF_SZ		64
#define HAKO_LINE_BUF_SZ	128
#define HAKO_RESP_BUF_SZ	128
#define HAKO_QUERY_TMO_MS	2000

/* fw_quirks bits, set at probe based on V: response */
#define HAKO_Q_FW22_U_INVERT	BIT(0)	/* FW 2.2: U: payload is 100 - row */
#define HAKO_Q_FW23_P_INVERT	BIT(1)	/* FW 2.3: P: response is 255 - x  */
/*
 * Pre-2.5 hardware reports J: with inverted polarity vs HW 2.5+. The
 * sysfs attribute canonicalizes to "1 = firmware/powerboard active" so
 * userspace doesn't have to know the hw rev. Quirk fires when we need
 * to invert, i.e. on pre-2.5 hardware.
 */
#define HAKO_Q_J_INVERT_PRE25	BIT(2)

/* CDC SET_CONTROL_LINE_STATE bits */
#define HAKO_CTRL_DTR		0x01
#define HAKO_CTRL_RTS		0x02

#define HAKO_NUM_CHAN		3
#define HAKO_POWER_NUM_CHAN	4
#define HAKO_CACHE_TTL_DEFAULT	HZ		/* 1 second */
#define HAKO_CACHE_TTL_MIN_MS	100		/* 10 reads per second max */
#define HAKO_CACHE_TTL_MAX_MS	60000		/* one read per minute */

struct hako_data {
	struct usb_device *udev;
	struct usb_interface *intf;

	u8 ep_in;
	u8 ep_out;

	struct urb *rx_urb;
	u8 *rx_buf;

	/*
	 * DMA-safe TX scratch buffer; usb_bulk_msg requires non-stack memory.
	 * Access serialized by xfer_lock.
	 */
	char *tx_buf;

	/* line assembly state, written from rx complete (atomic ctx) */
	char line_buf[HAKO_LINE_BUF_SZ];
	size_t line_len;

	/* response handoff to query waiter */
	struct mutex xfer_lock;
	struct completion resp_done;
	char resp_buf[HAKO_RESP_BUF_SZ];
	size_t resp_len;

	/* CDC Communications interface number, for SET_CONTROL_LINE_STATE */
	u8 ctrl_ifnum;

	/* parsed metadata */
	u8 hw_major, hw_minor;
	u8 fw_major, fw_minor;
	u32 location;
	u32 fw_quirks;

	/*
	 * Cached canonical jumper state, sampled at probe. true means firmware
	 * owns PWM (sysfs writes affect fans); false means motherboard owns
	 * the 4-pin signal and our U: writes never reach the fan output.
	 * Refreshed opportunistically on jumper_mode sysfs reads. Stale if
	 * the user toggles the physical jumper without re-reading jumper_mode
	 * or reloading the driver.
	 */
	bool jumper_fw_active;

	/* Tunable via update_interval sysfs attr. */
	unsigned long cache_ttl;

	/* hwmon registration */
	struct device *hwmon_dev;

	/* per-class jiffies-based cache, row order */
	struct {
		long val[HAKO_NUM_CHAN];
		unsigned long last_jiffies;
		bool valid;
	} pwm_cache, fan_cache;

	/* power readings cached in microwatts, shunt 1-4 order */
	struct {
		long val[HAKO_POWER_NUM_CHAN];
		unsigned long last_jiffies;
		bool valid;
	} power_cache;
};

/*
 * Per-hardware-rev linear calibration for the W: ADC readings, scaled by 1000
 * to keep integer math exact. Foundry's formula per channel:
 *   if raw <= 0: current = 0
 *   else:        current_A = (raw - intercept) / slope
 *   power_W      = current_A * 12
 *
 * HW 2.2 and 2.3 use multivariate regression instead — see hako_calc_mv22_w.
 */
struct hako_power_cal {
	s32 intercept_x1000;
	u32 slope_x1000;
};

static int hako_find_ctrl_ifnum(struct usb_device *udev)
{
	int i;

	if (!udev->actconfig)
		return -ENODEV;

	for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
		struct usb_interface *intf = udev->actconfig->interface[i];
		struct usb_interface_descriptor *desc;

		if (!intf || !intf->cur_altsetting)
			continue;
		desc = &intf->cur_altsetting->desc;
		if (desc->bInterfaceClass == USB_CLASS_COMM)
			return desc->bInterfaceNumber;
	}
	return -ENODEV;
}

/*
 * Arduino Leonardo's CDC firmware ignores incoming serial bytes until the
 * host asserts DTR (so the application code knows a host is present). cdc-acm
 * does this via SET_CONTROL_LINE_STATE on tty open; this driver replaces
 * cdc-acm so we have to do it ourselves before any V:/P:/T:/U: query.
 */
static int hako_set_control_line(struct hako_data *data, u16 state)
{
	return usb_control_msg(data->udev, usb_sndctrlpipe(data->udev, 0),
			       USB_CDC_REQ_SET_CONTROL_LINE_STATE,
			       USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			       state, data->ctrl_ifnum,
			       NULL, 0,
			       msecs_to_jiffies(1000));
}

static void hako_rx_complete(struct urb *urb)
{
	struct hako_data *data = urb->context;
	int status = urb->status;
	int i;

	switch (status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* device gone or unbinding; do not resubmit */
		return;
	default:
		dev_dbg(&data->intf->dev, "rx urb status %d\n", status);
		goto resubmit;
	}

	for (i = 0; i < urb->actual_length; i++) {
		u8 c = data->rx_buf[i];

		if (c == '\r')
			continue;

		if (c == '\n') {
			size_t n = min_t(size_t, data->line_len,
					 HAKO_RESP_BUF_SZ - 1);

			memcpy(data->resp_buf, data->line_buf, n);
			data->resp_buf[n] = '\0';
			data->resp_len = n;
			data->line_len = 0;
			complete(&data->resp_done);
			continue;
		}

		if (data->line_len < HAKO_LINE_BUF_SZ - 1) {
			data->line_buf[data->line_len++] = c;
		} else {
			/* overrun — drop the line, keep scanning for '\n' */
			data->line_len = 0;
			dev_warn_ratelimited(&data->intf->dev,
					     "rx line overrun, discarding\n");
		}
	}

resubmit:
	if (usb_submit_urb(urb, GFP_ATOMIC))
		dev_err(&data->intf->dev, "rx urb resubmit failed\n");
}

/*
 * usb_bulk_msg requires the transfer buffer to be DMA-safe; passing a
 * stack-allocated string trips a kernel WARN ("transfer buffer is on stack")
 * and may corrupt the TX. Copy into the per-device tx_buf, which is allocated
 * via devm_kzalloc and guarded by xfer_lock.
 */
static int hako_send_cmd(struct hako_data *data, const char *cmd)
{
	size_t len = strlen(cmd);
	int actual = 0;
	int ret;

	if (len >= HAKO_TX_BUF_SZ)
		return -EINVAL;

	memcpy(data->tx_buf, cmd, len);

	ret = usb_bulk_msg(data->udev,
			   usb_sndbulkpipe(data->udev, data->ep_out),
			   data->tx_buf, len, &actual,
			   msecs_to_jiffies(HAKO_QUERY_TMO_MS));
	if (ret) {
		dev_dbg(&data->intf->dev, "tx '%s' failed: %d\n", cmd, ret);
		return ret;
	}
	if (actual < 0 || (size_t)actual != len)
		return -EIO;
	return 0;
}

/*
 * Caller must hold xfer_lock. timeout_ms governs how long to wait for the
 * response. Pass resp=NULL to discard the body (set commands like U:).
 */
static int hako_query_locked(struct hako_data *data, const char *cmd,
			     char *resp, size_t resp_sz, int timeout_ms)
{
	unsigned long left;
	int ret;

	/*
	 * Discard a completion left over from a previously timed-out query
	 * whose response arrived late during the idle gap between commands, so
	 * it can't satisfy this wait with the wrong command's reply. Draining
	 * via try_wait_for_completion() (which takes the completion's wait.lock)
	 * also avoids racing rx_complete's complete() against the unlocked
	 * store in reinit_completion(). Note: a reply that arrives *during* the
	 * wait below still can't be distinguished — the text protocol carries no
	 * sequence tag — but xfer_lock serialization plus this drain make that
	 * window very narrow.
	 */
	while (try_wait_for_completion(&data->resp_done))
		;
	reinit_completion(&data->resp_done);
	/* drop any stale partial line from a previous failed/aborted xfer */
	data->line_len = 0;

	ret = hako_send_cmd(data, cmd);
	if (ret)
		return ret;

	left = wait_for_completion_timeout(&data->resp_done,
					   msecs_to_jiffies(timeout_ms));
	if (!left)
		return -ETIMEDOUT;

	if (!resp)
		return 0;

	if (data->resp_len >= resp_sz)
		return -EOVERFLOW;
	memcpy(resp, data->resp_buf, data->resp_len);
	resp[data->resp_len] = '\0';
	return data->resp_len;
}

static int hako_query(struct hako_data *data, const char *cmd,
		      char *resp, size_t resp_sz)
{
	int ret;

	mutex_lock(&data->xfer_lock);
	ret = hako_query_locked(data, cmd, resp, resp_sz, HAKO_QUERY_TMO_MS);
	mutex_unlock(&data->xfer_lock);
	return ret;
}

static int hako_parse_v(struct hako_data *data, const char *resp)
{
	unsigned int hwm, hwn, fwm, fwn, loc;
	int n;

	/* expect "<hw_major>.<hw_minor>,<fw_major>.<fw_minor>,<location>" */
	n = sscanf(resp, "%u.%u,%u.%u,%u", &hwm, &hwn, &fwm, &fwn, &loc);
	if (n != 5)
		return -ENODEV;
	if (hwm > 9 || hwn > 9 || fwm > 9 || fwn > 9)
		return -ENODEV;

	data->hw_major = hwm;
	data->hw_minor = hwn;
	data->fw_major = fwm;
	data->fw_minor = fwn;
	data->location = loc;

	data->fw_quirks = 0;
	if (fwm == 2 && fwn == 2)
		data->fw_quirks |= HAKO_Q_FW22_U_INVERT;
	if (fwm == 2 && fwn == 3)
		data->fw_quirks |= HAKO_Q_FW23_P_INVERT;
	/* HW 2.5+ wire is canonical (1 = firmware); only pre-2.5 needs invert */
	if (hwm < 2 || (hwm == 2 && hwn < 5))
		data->fw_quirks |= HAKO_Q_J_INVERT_PRE25;

	return 0;
}

static int hako_parse_3ints(const char *resp, long *out)
{
	int n = sscanf(resp, "%ld,%ld,%ld", &out[0], &out[1], &out[2]);

	return n == 3 ? 0 : -EINVAL;
}

/*
 * Read EEPROM-stored PWM via P:, in row order, 0-255. Response is in pin
 * order; per HakoFoundry powerboard.py the pin->row mapping is
 * (pin1,pin2,pin3) -> (row1,row2,row3) = (pin3,pin1,pin2). FW 2.3 inverts
 * the per-channel value (board returns 255 - x). Caller must hold xfer_lock.
 */
static int hako_query_eeprom_pwm_locked(struct hako_data *data, long *vals)
{
	char resp[HAKO_RESP_BUF_SZ];
	long raw[HAKO_NUM_CHAN];
	int ret;

	ret = hako_query_locked(data, "P:\n", resp, sizeof(resp),
				HAKO_QUERY_TMO_MS);
	if (ret < 0)
		return ret;
	if (hako_parse_3ints(resp, raw))
		return -EINVAL;

	if (data->fw_quirks & HAKO_Q_FW23_P_INVERT) {
		raw[0] = 255 - raw[0];
		raw[1] = 255 - raw[1];
		raw[2] = 255 - raw[2];
	}

	vals[0] = clamp_t(long, raw[2], 0, 255); /* row1 = pin3 */
	vals[1] = clamp_t(long, raw[0], 0, 255); /* row2 = pin1 */
	vals[2] = clamp_t(long, raw[1], 0, 255); /* row3 = pin2 */
	return 0;
}

/*
 * Seed the live PWM cache from the EEPROM at first read. P: returns the
 * EEPROM value, not the live U:-set value, so the cache becomes authoritative
 * after our first U: write — we never re-query P: for the live read path.
 * Caller must hold xfer_lock.
 */
static int hako_refresh_pwm_locked(struct hako_data *data)
{
	int ret;

	if (data->pwm_cache.valid)
		return 0;

	ret = hako_query_eeprom_pwm_locked(data, data->pwm_cache.val);
	if (ret)
		return ret;

	data->pwm_cache.last_jiffies = jiffies;
	data->pwm_cache.valid = true;
	return 0;
}

static int hako_refresh_pwm(struct hako_data *data)
{
	int ret;

	mutex_lock(&data->xfer_lock);
	ret = hako_refresh_pwm_locked(data);
	mutex_unlock(&data->xfer_lock);
	return ret;
}

/*
 * Build and send a U: or F: command with the given row values (0-255 each)
 * under xfer_lock. Caller must hold the lock. FW 2.2's `100 - row` inversion
 * applies only to U: (per HakoFoundry's update_fan_speed; set_fan_speed sends
 * raw). Drain any optional echo with a short timeout; the response format is
 * undocumented and we don't validate it.
 */
static int hako_send_pwm_cmd_locked(struct hako_data *data, char cmd_letter,
				    const long row[HAKO_NUM_CHAN])
{
	char cmd[40];
	int pct[HAKO_NUM_CHAN];
	int i, ret;

	for (i = 0; i < HAKO_NUM_CHAN; i++)
		pct[i] = DIV_ROUND_CLOSEST((int)row[i] * 100, 255);

	if (cmd_letter == 'U' && (data->fw_quirks & HAKO_Q_FW22_U_INVERT))
		for (i = 0; i < HAKO_NUM_CHAN; i++)
			pct[i] = 100 - pct[i];

	/* wire order is row2,row3,row1 */
	scnprintf(cmd, sizeof(cmd), "%c:%d,%d,%d\n",
		  cmd_letter, pct[1], pct[2], pct[0]);

	ret = hako_query_locked(data, cmd, NULL, 0, 500);
	return (ret == -ETIMEDOUT) ? 0 : ret;
}

/*
 * Write live PWM via U: (volatile, no EEPROM wear). Refreshes the cache
 * to fill the OTHER rows in the wire payload, substitutes the channel
 * being written, sends, and updates the cache to reflect the new live state.
 */
static int hako_set_pwm(struct hako_data *data, int channel, long val)
{
	long row[HAKO_NUM_CHAN];
	int i, ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	if (!data->jumper_fw_active)
		dev_warn_ratelimited(&data->intf->dev,
				     "pwm write while motherboard owns the jumper — writes won't reach the fans\n");

	mutex_lock(&data->xfer_lock);

	ret = hako_refresh_pwm_locked(data);
	if (ret)
		goto out;

	for (i = 0; i < HAKO_NUM_CHAN; i++)
		row[i] = data->pwm_cache.val[i];
	row[channel] = val;

	ret = hako_send_pwm_cmd_locked(data, 'U', row);
	if (ret)
		goto out;

	data->pwm_cache.val[channel] = val;
	data->pwm_cache.last_jiffies = jiffies;
	data->pwm_cache.valid = true;

out:
	mutex_unlock(&data->xfer_lock);
	return ret;
}

/*
 * Write the EEPROM-stored boot-time PWM via F:.
 *
 * The board's F: command writes EEPROM AND side-effects the live PWM (fan
 * jumps to the new default value immediately). To preserve the "writing a
 * default doesn't disturb the running fan" sysfs contract, we snapshot the
 * live PWM before F:, then replay it via U: afterward. The fan briefly dips
 * during the F:->U: window (~300ms typical); that's the cost of clean
 * separation between default_pwm[N] and pwm[N] semantics.
 *
 * The OTHER rows of the F: command come from a fresh P: query (current
 * EEPROM), not the live cache, so a single-channel default_pwm write doesn't
 * accidentally write live U: values into the EEPROM defaults.
 */
static int hako_set_default_pwm(struct hako_data *data, int channel, long val)
{
	long eeprom[HAKO_NUM_CHAN];
	long live[HAKO_NUM_CHAN];
	int i, ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&data->xfer_lock);

	ret = hako_refresh_pwm_locked(data);
	if (ret)
		goto out;
	for (i = 0; i < HAKO_NUM_CHAN; i++)
		live[i] = data->pwm_cache.val[i];

	ret = hako_query_eeprom_pwm_locked(data, eeprom);
	if (ret)
		goto out;
	eeprom[channel] = val;

	ret = hako_send_pwm_cmd_locked(data, 'F', eeprom);
	if (ret)
		goto out;

	/* F: side-effected the live PWM; restore via U: with the snapshot. */
	ret = hako_send_pwm_cmd_locked(data, 'U', live);

out:
	mutex_unlock(&data->xfer_lock);
	return ret;
}

static int hako_get_default_pwm(struct hako_data *data, int channel, long *out)
{
	long vals[HAKO_NUM_CHAN];
	int ret;

	mutex_lock(&data->xfer_lock);
	ret = hako_query_eeprom_pwm_locked(data, vals);
	mutex_unlock(&data->xfer_lock);
	if (ret)
		return ret;
	*out = vals[channel];
	return 0;
}

/*
 * T: response is already in row order; raw * 30 = RPM. The staleness check
 * and the cache update both run under xfer_lock so two concurrent readers
 * that race past a stale deadline don't each issue a redundant USB round-trip.
 */
static int hako_refresh_fan(struct hako_data *data)
{
	char resp[HAKO_RESP_BUF_SZ];
	long raw[HAKO_NUM_CHAN];
	int ret = 0, i;

	mutex_lock(&data->xfer_lock);

	if (data->fan_cache.valid &&
	    time_before(jiffies, data->fan_cache.last_jiffies + data->cache_ttl))
		goto out;

	ret = hako_query_locked(data, "T:\n", resp, sizeof(resp),
				HAKO_QUERY_TMO_MS);
	if (ret < 0)
		goto out;
	if (hako_parse_3ints(resp, raw)) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < HAKO_NUM_CHAN; i++)
		data->fan_cache.val[i] = max_t(long, raw[i], 0) * 30;
	data->fan_cache.last_jiffies = jiffies;
	data->fan_cache.valid = true;
	ret = 0;
out:
	mutex_unlock(&data->xfer_lock);
	return ret;
}

static int hako_parse_4sints(const char *resp, long *out)
{
	int n = sscanf(resp, "%ld,%ld,%ld,%ld",
		       &out[0], &out[1], &out[2], &out[3]);

	return n == 4 ? 0 : -EINVAL;
}

static bool hako_uses_multivariate(u8 hw_major, u8 hw_minor)
{
	return hw_major == 2 && (hw_minor == 2 || hw_minor == 3);
}

/*
 * Linear-calibration lookup for hw revs that don't use the multivariate
 * regression. Caller must check hako_uses_multivariate first; for HW 2.2/2.3
 * this returns the modern cal but the value is meaningless.
 */
static const struct hako_power_cal *hako_get_linear_cal(u8 hw_major, u8 hw_minor)
{
	static const struct hako_power_cal cal_2_0 = { -1375, 3574 };
	static const struct hako_power_cal cal_2_1 = { -1069, 3284 };
	static const struct hako_power_cal cal_modern = { 0, 925250 };

	if (hw_major == 2) {
		if (hw_minor == 0)
			return &cal_2_0;
		if (hw_minor == 1)
			return &cal_2_1;
	}
	return &cal_modern; /* 2.4, 2.5, 2.6+, and any non-HW-2 fallback */
}

/*
 * HW 2.2 / 2.3 multivariate regression. Per-shunt coefficients are
 * Foundry's float values * 12 V * 1e12, so each c[j] * features[j] yields
 * a contribution to power in picowatts. Verified against Foundry's
 * _calculate_wattage_22 on a battery of test vectors — output matches
 * exactly across raw inputs from -100 to 3000.
 */
struct hako_mv22_row {
	s64 c[11];
};

static const struct hako_mv22_row hako_mv22_coefs[HAKO_POWER_NUM_CHAN] = {
	{ { 253200000000LL, 12720000000LL, -17160000LL, -15720000LL, -14520000LL,
	    -15600LL, -17040LL, -19320LL, -1332LL, -1476LL, -1740LL } },
	{ { -267600000000LL, -14520000LL, 12720000000LL, -13320000LL, -12120000LL,
	    -14520LL, -15720LL, -17160LL, -1452LL, -1596LL, -1860LL } },
	{ { -294000000000LL, -12120000LL, -15720000LL, 12720000000LL, -10920000LL,
	    -13320LL, -14520LL, -15720LL, -1572LL, -1716LL, -1980LL } },
	{ { -337200000000LL, -9720000LL, -13320000LL, -14520000LL, 12720000000LL,
	    -12120LL, -13320LL, -14520LL, -1692LL, -1836LL, -2100LL } },
};

/*
 * Foundry's MANUAL_OFFSETS table: post-correction by argmax-shunt and
 * rounded-to-12W key. Empirically derived calibration fudge that nets
 * ~1% accuracy improvement on the multivariate output.
 */
struct hako_mv22_off_entry {
	int wattage;             /* expected wattage key (multiple of 12); 0 = sentinel */
	int offset[HAKO_POWER_NUM_CHAN];
};

static const struct hako_mv22_off_entry hako_mv22_off_s1[] = {
	{ 108, { 1, 0, 0, 0 } }, { 120, { 1, 0, 0, 0 } }, { 132, { 1, 0, 0, 0 } },
	{ 144, { 13, 0, 0, 0 } }, { 156, { 13, 0, 0, 0 } }, { 168, { 1, 0, 0, 0 } },
	{ 0, { 0, 0, 0, 0 } },
};

static const struct hako_mv22_off_entry hako_mv22_off_s2[] = {
	{ 24, { 0, 1, 0, 0 } }, { 36, { 0, 1, 0, 0 } }, { 48, { 0, 1, 0, 0 } },
	{ 60, { 0, 1, 0, 0 } }, { 72, { 0, 1, -1, 0 } }, { 84, { 0, 1, -1, 0 } },
	{ 96, { 0, 1, -1, 0 } }, { 108, { 0, 1, -1, 0 } }, { 120, { 0, 1, -2, 0 } },
	{ 132, { 0, 1, -2, 0 } }, { 144, { 0, 1, -2, 0 } }, { 156, { 0, 1, -2, 0 } },
	{ 168, { 0, 1, -2, 0 } },
	{ 0, { 0, 0, 0, 0 } },
};

static const struct hako_mv22_off_entry hako_mv22_off_s3[] = {
	{ 24, { 0, 0, 1, 0 } }, { 36, { 0, 0, 1, 0 } }, { 48, { 0, 0, 1, 0 } },
	{ 60, { 0, -1, 1, 0 } }, { 72, { 0, -1, 1, 0 } }, { 84, { 0, -1, 1, 0 } },
	{ 96, { 0, -1, 1, 0 } }, { 108, { 0, -1, 1, 0 } }, { 120, { 0, -2, 1, 0 } },
	{ 132, { 0, -2, 1, 0 } }, { 144, { 0, -2, -11, 0 } }, { 156, { 0, -2, 1, 0 } },
	{ 168, { 0, -3, 1, 0 } },
	{ 0, { 0, 0, 0, 0 } },
};

static const struct hako_mv22_off_entry hako_mv22_off_s4[] = {
	{ 12, { 0, 0, 0, 1 } }, { 24, { 0, 0, 0, 1 } }, { 36, { 0, 0, 0, 1 } },
	{ 48, { 0, 0, 0, 1 } }, { 60, { 0, 0, 0, 1 } }, { 72, { 0, 0, 0, 1 } },
	{ 84, { 0, 0, 0, 1 } }, { 96, { 0, 0, 0, 1 } },
	{ 0, { 0, 0, 0, 0 } },
};

static const struct hako_mv22_off_entry * const
hako_mv22_off_tables[HAKO_POWER_NUM_CHAN] = {
	hako_mv22_off_s1, hako_mv22_off_s2, hako_mv22_off_s3, hako_mv22_off_s4,
};

static void hako_calc_mv22_w(const long raw[HAKO_POWER_NUM_CHAN],
			     int wattages[HAKO_POWER_NUM_CHAN])
{
	s64 features[11];
	s64 sum;
	int i, j;
	int active_idx;
	int active_w;
	int expected_w;
	const struct hako_mv22_off_entry *table;

	features[0] = 1;
	features[1] = raw[0];
	features[2] = raw[1];
	features[3] = raw[2];
	features[4] = raw[3];
	features[5] = (s64)raw[0] * raw[1];
	features[6] = (s64)raw[0] * raw[2];
	features[7] = (s64)raw[0] * raw[3];
	features[8] = (s64)raw[1] * raw[2];
	features[9] = (s64)raw[1] * raw[3];
	features[10] = (s64)raw[2] * raw[3];

	for (i = 0; i < HAKO_POWER_NUM_CHAN; i++) {
		sum = 0;
		for (j = 0; j < 11; j++)
			sum += hako_mv22_coefs[i].c[j] * features[j];
		/* sum is in pW; round to nearest W, clamp negatives to 0 */
		if (sum <= 0)
			wattages[i] = 0;
		else
			wattages[i] = (int)div64_u64((u64)sum + 500000000000ULL,
						     1000000000000ULL);
	}

	/* find argmax */
	active_idx = 0;
	active_w = wattages[0];
	for (i = 1; i < HAKO_POWER_NUM_CHAN; i++) {
		if (wattages[i] > active_w) {
			active_w = wattages[i];
			active_idx = i;
		}
	}

	/* round to nearest multiple of 12 */
	expected_w = ((active_w + 6) / 12) * 12;

	/* lookup and apply manual offsets */
	table = hako_mv22_off_tables[active_idx];
	for (i = 0; table[i].wattage != 0; i++) {
		if (table[i].wattage == expected_w) {
			for (j = 0; j < HAKO_POWER_NUM_CHAN; j++)
				wattages[j] += table[i].offset[j];
			break;
		}
	}

	/* offsets can push values negative; reclamp */
	for (i = 0; i < HAKO_POWER_NUM_CHAN; i++)
		if (wattages[i] < 0)
			wattages[i] = 0;
}

/*
 * Convert a raw W: reading to microwatts using the per-rev linear calibration.
 * Foundry's formula:
 *     if raw <= 0: current_A = 0
 *     else:        current_A = (raw - intercept) / slope
 *     power_W   = current_A * 12
 *
 * Integer math: scale slope and intercept by 1000 so we can do exact
 * arithmetic. power_uW = (raw*1000 - intercept_x1000) * 12_000_000 / slope_x1000.
 * Use u64 intermediate to avoid overflow (raw up to a few thousand, times
 * 12_000_000, times 1000 lands around 10^13).
 */
static u64 hako_raw_to_uw(long raw, const struct hako_power_cal *cal)
{
	s64 sub;
	u64 num;

	if (raw <= 0)
		return 0;

	sub = (s64)raw * 1000 - cal->intercept_x1000;
	if (sub <= 0)
		return 0;
	num = (u64)sub * 12000000ULL;
	return div_u64(num, cal->slope_x1000);
}

/*
 * W: response is 4 signed ADC ints; we cache microwatts in shunt order.
 * Staleness check and cache update run under xfer_lock (see hako_refresh_fan).
 */
static int hako_refresh_power(struct hako_data *data)
{
	char resp[HAKO_RESP_BUF_SZ];
	long raw[HAKO_POWER_NUM_CHAN];
	int ret = 0, i;

	mutex_lock(&data->xfer_lock);

	if (data->power_cache.valid &&
	    time_before(jiffies, data->power_cache.last_jiffies + data->cache_ttl))
		goto out;

	ret = hako_query_locked(data, "W:\n", resp, sizeof(resp),
				HAKO_QUERY_TMO_MS);
	if (ret < 0)
		goto out;
	if (hako_parse_4sints(resp, raw)) {
		ret = -EINVAL;
		goto out;
	}

	if (hako_uses_multivariate(data->hw_major, data->hw_minor)) {
		int w[HAKO_POWER_NUM_CHAN];

		hako_calc_mv22_w(raw, w);
		for (i = 0; i < HAKO_POWER_NUM_CHAN; i++)
			data->power_cache.val[i] = (long)w[i] * 1000000L;
	} else {
		const struct hako_power_cal *cal =
			hako_get_linear_cal(data->hw_major, data->hw_minor);

		for (i = 0; i < HAKO_POWER_NUM_CHAN; i++)
			data->power_cache.val[i] =
				(long)hako_raw_to_uw(raw[i], cal);
	}
	data->power_cache.last_jiffies = jiffies;
	data->power_cache.valid = true;
	ret = 0;
out:
	mutex_unlock(&data->xfer_lock);
	return ret;
}

static const char * const hako_row_labels[HAKO_NUM_CHAN] = {
	"Row 1", "Row 2", "Row 3"
};

static const char * const hako_shunt_labels[HAKO_POWER_NUM_CHAN] = {
	"Shunt 1", "Shunt 2", "Shunt 3", "Shunt 4"
};

static umode_t hako_is_visible(const void *drvdata,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_update_interval)
			return 0644;
		return 0;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		}
		return 0;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
		case hwmon_pwm_mode:
			return 0444;
		}
		return 0;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
		case hwmon_power_label:
			return 0444;
		}
		return 0;
	default:
		return 0;
	}
}

static int hako_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *val)
{
	struct hako_data *data = dev_get_drvdata(dev);
	int ret;

	if (type == hwmon_chip) {
		if (attr != hwmon_chip_update_interval)
			return -EOPNOTSUPP;
		*val = jiffies_to_msecs(data->cache_ttl);
		return 0;
	}

	if (channel < 0)
		return -EINVAL;

	switch (type) {
	case hwmon_fan:
		if (channel >= HAKO_NUM_CHAN)
			return -EINVAL;
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		ret = hako_refresh_fan(data);
		if (ret)
			return ret;
		*val = data->fan_cache.val[channel];
		return 0;
	case hwmon_pwm:
		if (channel >= HAKO_NUM_CHAN)
			return -EINVAL;
		switch (attr) {
		case hwmon_pwm_input:
			ret = hako_refresh_pwm(data);
			if (ret)
				return ret;
			*val = data->pwm_cache.val[channel];
			return 0;
		case hwmon_pwm_enable:
			*val = 1; /* manual; firmware has no closed-loop */
			return 0;
		case hwmon_pwm_mode:
			*val = 1; /* PWM (vs DC) */
			return 0;
		}
		return -EOPNOTSUPP;
	case hwmon_power:
		if (channel >= HAKO_POWER_NUM_CHAN)
			return -EINVAL;
		if (attr != hwmon_power_input)
			return -EOPNOTSUPP;
		ret = hako_refresh_power(data);
		if (ret)
			return ret;
		*val = data->power_cache.val[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int hako_read_string(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, const char **str)
{
	if (channel < 0)
		return -EINVAL;

	if (type == hwmon_fan && attr == hwmon_fan_label) {
		if (channel >= HAKO_NUM_CHAN)
			return -EINVAL;
		*str = hako_row_labels[channel];
		return 0;
	}
	if (type == hwmon_power && attr == hwmon_power_label) {
		if (channel >= HAKO_POWER_NUM_CHAN)
			return -EINVAL;
		*str = hako_shunt_labels[channel];
		return 0;
	}
	return -EOPNOTSUPP;
}

static int hako_write(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long val)
{
	struct hako_data *data = dev_get_drvdata(dev);

	if (type == hwmon_chip) {
		if (attr != hwmon_chip_update_interval)
			return -EOPNOTSUPP;
		if (val < HAKO_CACHE_TTL_MIN_MS || val > HAKO_CACHE_TTL_MAX_MS)
			return -EINVAL;
		data->cache_ttl = msecs_to_jiffies(val);
		return 0;
	}

	if (channel < 0 || channel >= HAKO_NUM_CHAN)
		return -EINVAL;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		return hako_set_pwm(data, channel, val);
	case hwmon_pwm_enable:
		/* firmware has no closed-loop; only manual mode is meaningful */
		return val == 1 ? 0 : -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops hako_hwmon_ops = {
	.is_visible	= hako_is_visible,
	.read		= hako_read,
	.read_string	= hako_read_string,
	.write		= hako_write,
};

static const struct hwmon_channel_info * const hako_channel_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_chip_info hako_chip_info = {
	.ops	= &hako_hwmon_ops,
	.info	= hako_channel_info,
};

/*
 * Per-device sysfs attributes under the USB interface. These are not part
 * of the hwmon class because they describe the board itself rather than
 * sensor channels (and hwmon's attribute schema doesn't have slots for
 * "hardware revision" / "location").
 */

static ssize_t hardware_revision_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct hako_data *data = usb_get_intfdata(intf);

	if (!data)
		return -ENODEV;
	return sysfs_emit(buf, "%u.%u\n", data->hw_major, data->hw_minor);
}
static DEVICE_ATTR_RO(hardware_revision);

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct hako_data *data = usb_get_intfdata(intf);

	if (!data)
		return -ENODEV;
	return sysfs_emit(buf, "%u.%u\n", data->fw_major, data->fw_minor);
}
static DEVICE_ATTR_RO(firmware_version);

static ssize_t location_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct hako_data *data = usb_get_intfdata(intf);

	if (!data)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", data->location);
}
static DEVICE_ATTR_RO(location);

/*
 * Read J: and canonicalize to "true = firmware/powerboard owns PWM, false =
 * motherboard owns PWM" regardless of hw rev. Pre-2.5 wires the polarity
 * inverted; HAKO_Q_J_INVERT_PRE25 fires the !wire flip.
 */
static int hako_query_jumper(struct hako_data *data, bool *fw_active)
{
	char resp[HAKO_RESP_BUF_SZ];
	unsigned int wire;
	int ret;

	ret = hako_query(data, "J:\n", resp, sizeof(resp));
	if (ret < 0)
		return ret;
	if (kstrtouint(resp, 10, &wire) || wire > 1)
		return -EIO;

	if (data->fw_quirks & HAKO_Q_J_INVERT_PRE25)
		wire = !wire;

	*fw_active = !!wire;
	return 0;
}

/*
 * jumper_mode: 1 = firmware/powerboard owns PWM, 0 = motherboard owns PWM.
 * Read live so users see the current physical state. Side effect:
 * refreshes the cached jumper_fw_active that gates the pwm-write warning.
 */
static ssize_t jumper_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct hako_data *data = usb_get_intfdata(intf);
	bool fw_active;
	int ret;

	if (!data)
		return -ENODEV;

	ret = hako_query_jumper(data, &fw_active);
	if (ret)
		return ret;

	data->jumper_fw_active = fw_active;
	return sysfs_emit(buf, "%u\n", fw_active ? 1 : 0);
}
static DEVICE_ATTR_RO(jumper_mode);

/*
 * default_pwm[1-3]: the EEPROM-stored boot-time PWM (0-255). Read returns the
 * board's current EEPROM value via P:; write commits to EEPROM via F:. These
 * are the values the board drives at power-on, equivalent to HakoFoundry's
 * "Default Fan Speed" page. Not part of standard hwmon; this concept doesn't
 * exist in the hwmon ABI, so we expose it as a custom interface-device attr.
 *
 * EEPROM endurance on the ATmega32u4 is ~100k writes per cell. Don't drive
 * these from a polling loop; they're for one-time / occasional config.
 */
#define HAKO_DEFAULT_PWM_ATTR(N)					      \
static ssize_t default_pwm##N##_show(struct device *dev,		      \
				     struct device_attribute *attr,	      \
				     char *buf)				      \
{									      \
	struct hako_data *data = usb_get_intfdata(to_usb_interface(dev));     \
	long val;							      \
	int ret;							      \
									      \
	if (!data)							      \
		return -ENODEV;						      \
	ret = hako_get_default_pwm(data, (N) - 1, &val);		      \
	if (ret)							      \
		return ret;						      \
	return sysfs_emit(buf, "%ld\n", val);				      \
}									      \
static ssize_t default_pwm##N##_store(struct device *dev,		      \
				      struct device_attribute *attr,	      \
				      const char *buf, size_t count)	      \
{									      \
	struct hako_data *data = usb_get_intfdata(to_usb_interface(dev));     \
	long val;							      \
	int ret;							      \
									      \
	if (!data)							      \
		return -ENODEV;						      \
	ret = kstrtol(buf, 0, &val);					      \
	if (ret)							      \
		return ret;						      \
	ret = hako_set_default_pwm(data, (N) - 1, val);			      \
	return ret < 0 ? ret : count;					      \
}									      \
static DEVICE_ATTR_RW(default_pwm##N)

HAKO_DEFAULT_PWM_ATTR(1);
HAKO_DEFAULT_PWM_ATTR(2);
HAKO_DEFAULT_PWM_ATTR(3);

static struct attribute *hako_dev_attrs[] = {
	&dev_attr_hardware_revision.attr,
	&dev_attr_firmware_version.attr,
	&dev_attr_location.attr,
	&dev_attr_jumper_mode.attr,
	&dev_attr_default_pwm1.attr,
	&dev_attr_default_pwm2.attr,
	&dev_attr_default_pwm3.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hako_dev);

static int hako_probe(struct usb_interface *intf,
		      const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *ep_in, *ep_out;
	struct hako_data *data;
	char resp[HAKO_RESP_BUF_SZ];
	int ret;

	/*
	 * Bind only to the CDC Data interface. The static id_table already
	 * filters on bInterfaceClass via USB_DEVICE_AND_INTERFACE_INFO, but a
	 * dynamic id added via "echo VID PID > new_id" has only VID/PID match
	 * flags and would otherwise let probe fire on the CDC Comm interface.
	 */
	if (intf->cur_altsetting->desc.bInterfaceClass != USB_CLASS_CDC_DATA)
		return -ENODEV;

	data = devm_kzalloc(&intf->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->udev = usb_get_dev(udev);
	data->intf = intf;
	mutex_init(&data->xfer_lock);
	init_completion(&data->resp_done);
	data->cache_ttl = HAKO_CACHE_TTL_DEFAULT;

	ret = usb_find_common_endpoints(intf->cur_altsetting,
					&ep_in, &ep_out, NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "bulk endpoints not found: %d\n", ret);
		ret = -ENODEV;
		goto err_put;
	}
	data->ep_in = usb_endpoint_num(ep_in);
	data->ep_out = usb_endpoint_num(ep_out);

	data->rx_buf = devm_kzalloc(&intf->dev, HAKO_RX_BUF_SZ, GFP_KERNEL);
	if (!data->rx_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	data->tx_buf = devm_kzalloc(&intf->dev, HAKO_TX_BUF_SZ, GFP_KERNEL);
	if (!data->tx_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	data->rx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!data->rx_urb) {
		ret = -ENOMEM;
		goto err_put;
	}

	usb_fill_bulk_urb(data->rx_urb, udev,
			  usb_rcvbulkpipe(udev, data->ep_in),
			  data->rx_buf, HAKO_RX_BUF_SZ,
			  hako_rx_complete, data);

	ret = usb_submit_urb(data->rx_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&intf->dev, "rx urb submit failed: %d\n", ret);
		goto err_free_urb;
	}

	ret = hako_find_ctrl_ifnum(udev);
	if (ret < 0) {
		dev_dbg(&intf->dev, "CDC Comm interface not found\n");
		ret = -ENODEV;
		goto err_kill_urb;
	}
	data->ctrl_ifnum = ret;

	/*
	 * Assert DTR+RTS so the Arduino's CDC app loop accepts our bytes.
	 * Brief settle so the firmware sees the transition before V:.
	 */
	ret = hako_set_control_line(data, HAKO_CTRL_DTR | HAKO_CTRL_RTS);
	if (ret < 0) {
		dev_dbg(&intf->dev, "set_control_line failed: %d\n", ret);
		goto err_kill_urb;
	}
	msleep(100);

	/* probe-time discrimination: send V:, parse response */
	ret = hako_query(data, "V:\n", resp, sizeof(resp));
	if (ret < 0) {
		dev_dbg(&intf->dev,
			"V: query failed: %d (not a Hako board?)\n", ret);
		ret = -ENODEV;
		goto err_kill_urb;
	}

	ret = hako_parse_v(data, resp);
	if (ret) {
		dev_dbg(&intf->dev,
			"V: parse failed for '%s' (not a Hako board)\n", resp);
		goto err_kill_urb;
	}

	dev_info(&intf->dev,
		 "Hako-Core Powerboard hw %u.%u fw %u.%u location %u (quirks 0x%x)\n",
		 data->hw_major, data->hw_minor,
		 data->fw_major, data->fw_minor,
		 data->location, data->fw_quirks);

	/*
	 * Snapshot the jumper position so hako_set_pwm can warn when writes
	 * are about to be silently swallowed by motherboard PWM passthrough.
	 * If the read fails we default to "fw active" so we don't generate
	 * spurious warnings; users who care can read jumper_mode to refresh.
	 */
	if (hako_query_jumper(data, &data->jumper_fw_active))
		data->jumper_fw_active = true;

	data->hwmon_dev = hwmon_device_register_with_info(&intf->dev,
							  HAKO_DRV_NAME,
							  data,
							  &hako_chip_info,
							  NULL);
	if (IS_ERR(data->hwmon_dev)) {
		ret = PTR_ERR(data->hwmon_dev);
		dev_err(&intf->dev, "hwmon register failed: %d\n", ret);
		goto err_kill_urb;
	}

	usb_set_intfdata(intf, data);
	return 0;

err_kill_urb:
	usb_kill_urb(data->rx_urb);
err_free_urb:
	usb_free_urb(data->rx_urb);
err_put:
	usb_put_dev(udev);
	return ret;
}

static void hako_disconnect(struct usb_interface *intf)
{
	struct hako_data *data = usb_get_intfdata(intf);
	long defaults[HAKO_NUM_CHAN];

	if (!data)
		return;

	usb_set_intfdata(intf, NULL);
	/*
	 * Tear down hwmon first so no read callback can race with URB free.
	 * hwmon_device_unregister() is synchronous and waits for in-flight
	 * sysfs reads to finish before returning.
	 */
	hwmon_device_unregister(data->hwmon_dev);

	/*
	 * Before letting the device go, push the EEPROM-stored defaults via U:
	 * so the live PWM ends up at the user's configured safe values instead
	 * of whatever the last sysfs writer left it at. The board's firmware
	 * has no DTR-driven fallback (verified empirically — it holds the last
	 * U: indefinitely through DTR drop), so this is the only way to land
	 * fans at a known state on rmmod / hako-release / DKMS reinstall.
	 *
	 * Best-effort: if the device is already gone (unplug) the queries fail
	 * with -ENODEV and we just skip the safety push. The xfer_lock ordering
	 * is fine because hwmon_device_unregister has drained all sysfs reads.
	 */
	mutex_lock(&data->xfer_lock);
	if (hako_query_eeprom_pwm_locked(data, defaults) == 0)
		hako_send_pwm_cmd_locked(data, 'U', defaults);
	mutex_unlock(&data->xfer_lock);

	/* Deassert DTR/RTS so cdc-acm's next bind sees a clean transition. */
	hako_set_control_line(data, 0);
	usb_kill_urb(data->rx_urb);
	usb_free_urb(data->rx_urb);
	usb_put_dev(data->udev);

	dev_info(&intf->dev, "disconnected\n");
}

/*
 * Suspend/resume:
 *
 *   suspend  - kill the RX URB so the host stops polling the device while
 *              the system is asleep. Best-effort: any in-flight hako_query
 *              waiter ends up timing out at -ETIMEDOUT.
 *
 *   resume   - resubmit the RX URB, reassert DTR (the Arduino's CDC firmware
 *              treats USB suspend/resume as a DTR transition and drops
 *              incoming bytes until DTR is re-asserted), and invalidate
 *              the live caches. Userspace will reread T:/W:/P: on next
 *              hwmon poll.
 *
 *   reset_resume - same as resume; the firmware doesn't lose state across
 *                  port reset because Vbus stays applied.
 *
 * The pwm_cache is intentionally invalidated too. If the device power-
 * cycled during suspend (Vbus drop on some hosts), the firmware booted
 * back into its EEPROM defaults and our cached "last U:" is wrong. The
 * next sysfs read repopulates from P: which matches the EEPROM value =
 * current live state in that case.
 */
static int hako_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct hako_data *data = usb_get_intfdata(intf);

	if (!data)
		return 0;
	usb_kill_urb(data->rx_urb);
	return 0;
}

static int hako_resume(struct usb_interface *intf)
{
	struct hako_data *data = usb_get_intfdata(intf);
	int ret;

	if (!data)
		return 0;

	ret = usb_submit_urb(data->rx_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&intf->dev, "rx urb resubmit on resume failed: %d\n",
			ret);
		return ret;
	}

	ret = hako_set_control_line(data, HAKO_CTRL_DTR | HAKO_CTRL_RTS);
	if (ret < 0)
		dev_dbg(&intf->dev, "set_control_line on resume failed: %d\n",
			ret);
	msleep(100);

	mutex_lock(&data->xfer_lock);
	data->fan_cache.valid = false;
	data->power_cache.valid = false;
	data->pwm_cache.valid = false;
	mutex_unlock(&data->xfer_lock);

	return 0;
}

static int hako_reset_resume(struct usb_interface *intf)
{
	return hako_resume(intf);
}

static const struct usb_device_id hako_id_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(HAKO_VID, HAKO_PID,
					USB_CLASS_CDC_DATA, 0, 0) },
	{ }
};
MODULE_DEVICE_TABLE(usb, hako_id_table);

static struct usb_driver hako_driver = {
	.name		= HAKO_DRV_NAME,
	.id_table	= hako_id_table,
	.probe		= hako_probe,
	.disconnect	= hako_disconnect,
	.suspend	= hako_suspend,
	.resume		= hako_resume,
	.reset_resume	= hako_reset_resume,
	.dev_groups	= hako_dev_groups,
	/*
	 * Tell the USB core not to terminate URBs / disable endpoints before
	 * disconnect() — we need to send a final U: with the EEPROM defaults
	 * so fans land in a known state on rmmod / hako-release / DKMS reinstall.
	 * If the device is genuinely disconnected (unplug), the core ignores
	 * this hint and terminates URBs anyway, so our disconnect's safety push
	 * fails fast with -ENODEV in that case.
	 */
	.soft_unbind	= 1,
};

module_usb_driver(hako_driver);

MODULE_AUTHOR("Scott Shanafelt <sgshanaf@gmail.com>");
MODULE_DESCRIPTION("hwmon driver for HakoForge Hako-Core Powerboard");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.3.0");
