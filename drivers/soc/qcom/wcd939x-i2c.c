// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/usb/typec.h>
#include <linux/usb/ucsi_glink.h>
#include <linux/soc/qcom/wcd939x-i2c.h>
#include <linux/qti-regmap-debugfs.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/pm_runtime.h>
#include <linux/nvmem-consumer.h>
//#ifdef OPLUS_ARCH_EXTENDS
/* Checking whether the surge occurs */
#include <linux/ktime.h>
//#endif /* OPLUS_ARCH_EXTENDS */
#include "wcd-usbss-priv.h"
#include "wcd-usbss-registers.h"
#include "wcd-usbss-reg-masks.h"
#include "wcd-usbss-reg-shifts.h"
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
#include <soc/oplus/system/oplus_mm_kevent_fb.h>
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
//#ifdef OPLUS_ARCH_EXTENDS
/* Add for avoiding ADSP notify wcd to switch to standy mode in the ftm mode */
#include <soc/oplus/system/boot_mode.h>
#include <soc/oplus/device_info.h>
#include <soc/oplus/system/oplus_project.h>

static int boot_mode = 0;
//#endif /* OPLUS_ARCH_EXTENDS */
#define WCD_USBSS_I2C_NAME	"wcd-usbss-i2c-driver"

#define DEFAULT_SURGE_TIMER_PERIOD_MS 15000
#define SEC_TO_MS 1000
#define NUM_RCO_MISC2_READ 10
#define MIN_SURGE_TIMER_PERIOD_SEC 3
#define MAX_SURGE_TIMER_PERIOD_SEC 20

enum {
	WCD_USBSS_AUDIO_MANUAL,
	WCD_USBSS_AUDIO_FSM,
};

enum {
	WCD_USBSS_1_X,
	WCD_USBSS_2_0,
};

enum {
	WCD_USBSS_LPD_USB_MODE_CLEAR = 0,
	WCD_USBSS_LPD_MODE_SET,
	WCD_USBSS_USB_MODE_SET,
	WCD_USBSS_LPD_USB_MODE_SET,
	WCD_USBSS_SDAM_MODE_MAX = 7, /* Values 4 to 7 are reserved */
	WCD_USBSS_AUDIO_MODE_SET = WCD_USBSS_SDAM_MODE_MAX + 1,
};

struct wcd_usbss_reg_mask_val {
	u16 reg;
	u8 mask;
	u8 val;
};

/* regulator power supply names */
static const char * const supply_names[] = {
	"vdd-usb-cp",
};

static int audio_fsm_mode = WCD_USBSS_AUDIO_MANUAL;

#ifdef OPLUS_FEATURE_CHG_BASIC
unsigned char wcd_usbss_equalizer1 = 0;
#endif

/* Linearlizer coefficients for 32ohm load */
static const struct wcd_usbss_reg_mask_val coeff_init[] = {
	{WCD_USBSS_AUD_COEF_L_K5_0,       0xFF, 0x39},
	{WCD_USBSS_AUD_COEF_R_K5_0,       0xFF, 0x39},
	{WCD_USBSS_GND_COEF_L_K2_0,       0xFF, 0xE8},
	{WCD_USBSS_GND_COEF_L_K4_0,       0xFF, 0x73},
	{WCD_USBSS_GND_COEF_R_K2_0,       0xFF, 0xE8},
	{WCD_USBSS_GND_COEF_R_K4_0,       0xFF, 0x73},
	{WCD_USBSS_RATIO_SPKR_REXT_L_LSB, 0xFF, 0x00},
	{WCD_USBSS_RATIO_SPKR_REXT_L_MSB, 0x7F, 0x04},
	{WCD_USBSS_RATIO_SPKR_REXT_R_LSB, 0xFF, 0x00},
	{WCD_USBSS_RATIO_SPKR_REXT_R_MSB, 0x7F, 0x04},
};

static struct wcd_usbss_ctxt *wcd_usbss_ctxt_;

/* Required for kobj_attributes */
static ssize_t wcd_usbss_surge_enable_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static ssize_t wcd_usbss_surge_period_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static ssize_t wcd_usbss_standby_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static struct kobj_attribute wcd_usbss_surge_enable_attribute =
	__ATTR(surge_enable, 0220, NULL, wcd_usbss_surge_enable_store);

static struct kobj_attribute wcd_usbss_surge_period_attribute =
	__ATTR(surge_period, 0220, NULL, wcd_usbss_surge_period_store);

static struct kobj_attribute wcd_usbss_standby_enable_attribute =
	__ATTR(standby_mode, 0220, NULL, wcd_usbss_standby_store);

#if 1 //OPLUS_BUG_COMPATIBILITY
#define WCD_USBSS_PM_WAIT_RETRY_TIME_MAX 100
static int wcd_usbss_pm_runtime_resume_and_wait(struct device *dev)
{
	int ret = 0;
	int retry = 0;

	for (retry = 0; retry < WCD_USBSS_PM_WAIT_RETRY_TIME_MAX; retry++) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret == 0) {
			break;
		}
		if (retry == 0) {
			dev_info(dev, "%s: pm_runtime_resume_and_get return %d, retry\n",
					__func__, ret);
		}
		usleep_range(2000, 2020);
	}

//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
	if (retry > 5) {
		dev_info(dev, "%s: ret %d, retry %d\n", __func__, ret, retry);
		mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
			"pm_runtime_resume_and_get failed retry %d, ret %d", retry, ret);
	}
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/

	return ret;
}

static bool wcd_usbss_wait_pm_runtime_enabled(struct device *dev)
{
	bool ret = false;
	int retry = 0;

	if (!dev) {
		pr_err("%s: dev is null\n", __func__);
		return ret;
	}

	for (retry = 0; retry < WCD_USBSS_PM_WAIT_RETRY_TIME_MAX; retry++) {
		ret = pm_runtime_enabled(dev);
		if (ret) {
			break;
		}
		if (retry == 0) {
			dev_info(dev, "%s: pm_runtime_enabled return %d, retry\n", __func__, ret);
		}
		usleep_range(1000, 1020);
	}

//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
	if (retry > 0) {
		dev_info(dev, "%s: ret %d, retry %d\n", __func__, ret, retry);
		mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
			"wait i2c bus pm runtime enabled, retry %d, ret %d", retry, ret);
	}
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/

	return ret;
}
#endif /* OPLUS_BUG_COMPATIBILITY */
/**
 * wcd_usbss_sbu_switch_orientation() - Determine SBU switch orientation based on switch settings.
 *
 * This function is used to determine SBU switch orientation of the WCD USBSS. INVALID_ORIENTATION
 * in enum wcd_usbss_sbu_switch_orientation represents an error state where none of the defined
 * orientations can be inferred by the switch settings.
 *
 * Return: Returns an enum wcd_usbss_sbu_switch_orientation to client. INVALID_ORIENTATION is
 *	   returned if the driver is not probed or if undefined switch settings are discovered.
 */
enum wcd_usbss_sbu_switch_orientation wcd_usbss_get_sbu_switch_orientation(void)
{
	unsigned int read_val = 0;
	int ret = 0;

	/* check if driver is probed and private context is init'ed */
	if (wcd_usbss_ctxt_ == NULL)
		return INVALID_ORIENTATION;

	if (!wcd_usbss_ctxt_->regmap)
		return INVALID_ORIENTATION;
#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
			__func__, ret);
		return ret;
	}

	regmap_read(wcd_usbss_ctxt_->regmap, WCD_USBSS_SWITCH_SELECT0, &read_val);
	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);
	if ((read_val & 0x3) == 0x1)
		return GND_SBU1_ORIENTATION_B;
	if ((read_val & 0x3) == 0x2)
		return GND_SBU2_ORIENTATION_A;
	return INVALID_ORIENTATION;
}
EXPORT_SYMBOL(wcd_usbss_get_sbu_switch_orientation);

/*
 * wcd_usbss_set_switch_settings_enable() - Configure a specified WCD USBSS switch.
 * @switch_type: Switch to be enabled/disabled.
 * @switch_setting: Enable or disable.
 *
 * This function will set or reset a specific bit in the WCD_USBSS_SWITCH_SETTINGS_ENABLE register.
 * There is a check that switch_type represents a bit in this register. Update the definition of
 * enum wcd_usbss_switch_type switch_type if the bits in WCD_USBSS_SWITCH_SETTINGS_ENABLE change.
 *
 * Return : Returns int on whether the switch configuration happened or not. -ENODEV is returned if
 *	    the driver is not probed.
 */
int wcd_usbss_set_switch_settings_enable(enum wcd_usbss_switch_type switch_type,
					 enum wcd_usbss_switch_state switch_state)
{
	int ret = 0;
	/* check if driver is probed and private context is initialized */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if ((!wcd_usbss_ctxt_->regmap) || (switch_type < MIN_SWITCH_TYPE_NUM) ||
	    (switch_type > MAX_SWITCH_TYPE_NUM) ||
	    (switch_state != USBSS_SWITCH_DISABLE && switch_state != USBSS_SWITCH_ENABLE))
		return -EINVAL;

#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
					__func__, ret);
		return ret;
	}
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
			   1 << switch_type, switch_state << switch_type);

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return ret;
}
EXPORT_SYMBOL(wcd_usbss_set_switch_settings_enable);

/*
 * wcd_usbss_linearizer_rdac_cal_code_select() - Configure the linearizer calibration codes source.
 *
 * @source: HW (hardware) or SW (software).
 *
 * This function configures the linearizer to use SW or HW as the sources for the calibration codes.
 *
 * Return: Returns int on whether the switch configuration happened or not. -ENODEV is returned if
 *	   the driver is not probed.
 */
int wcd_usbss_linearizer_rdac_cal_code_select(enum linearizer_rdac_cal_code_select source)
{
	int ret = 0;
	/* check if driver is probed and private context is initialized */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if ((!wcd_usbss_ctxt_->regmap) || (source != LINEARIZER_SOURCE_HW &&
					   source != LINEARIZER_SOURCE_SW))
		return -EINVAL;

#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
					__func__, ret);
		return ret;
	}

	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_FUNCTION_ENABLE, 0x4, source << 2);

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return 0;
}
EXPORT_SYMBOL(wcd_usbss_linearizer_rdac_cal_code_select);

/*
 * wcd_usbss_set_linearizer_sw_tap() - Configure linearizer audio and ground software tap values.
 *
 * @aud_tap: 10-bit tap code for the L and R audio software tap registers.
 * @gnd_tap: 10-bit tap code for the L and R ground software tap registers.
 *
 * This function writes tap values to the left and right tap registers for the audio and ground
 * FETs. Note that the tap values are 10 bits and cannot exceed 0x3FF, but they can be 0.
 *
 * Return: Returns int on whether the switch configuration happened or not. -ENODEV is returned if
 *	   the driver is not probed.
 */
int wcd_usbss_set_linearizer_sw_tap(uint32_t aud_tap, uint32_t gnd_tap)
{
	int ret = 0;
	uint32_t lsb_mask = 0xFF, msb_shift = 8;

	/* check if driver is probed and private context is initialized */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if ((!wcd_usbss_ctxt_->regmap) || aud_tap > 0x3FF || gnd_tap > 0x3FF)
		return -EINVAL;

#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
					__func__, ret);
		return ret;
	}

	/* Audio left */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_AUD_L_LSB, 0xFF,
			   aud_tap & lsb_mask);
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_AUD_L_MSB, 0x3,
			   aud_tap >> msb_shift);
	/* Audio right */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_AUD_R_LSB, 0xFF,
			   aud_tap & lsb_mask);
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_AUD_R_MSB, 0x3,
			   aud_tap >> msb_shift);
	/* Ground left */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_GND_L_LSB, 0xFF,
			   gnd_tap & lsb_mask);
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_GND_L_MSB, 0x3,
			   gnd_tap >> msb_shift);
	/* Ground right */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_GND_R_LSB, 0xFF,
			   gnd_tap & lsb_mask);
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_TAP_GND_R_MSB, 0x3,
			   gnd_tap >> msb_shift);

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return ret;
}
EXPORT_SYMBOL(wcd_usbss_set_linearizer_sw_tap);

/*
 * wcd_usbss_register_update() - Write or read multiple USB-SS registers.
 *
 * @reg_arr: Array of {register address, register value} pairs.
 * @write: Bool selecting whether to write values from reg_arr or read values to store in reg_arr.
 * @arr_size: Number of {register address, register value} pairs in reg_arr.
 *
 * This function writes or reads arr_size number of register values, specified in reg_arr. If write
 * is true, this function will write all the values specified in reg_arr to corresponding USB-SS
 * registers. If write is false, this function will read the USB-SS registers specified in reg_arr
 * and write those values to the corresponding register values in reg_arr. If any register write or
 * read fails, this function prints an error message and exits.
 *
 * Return: Returns int on whether the register writes/reads were successful. -ENODEV is
 *	   returned if the driver is not probed.
 */
int wcd_usbss_register_update(uint32_t reg_arr[][2], bool write, size_t arr_size)
{
	size_t i;
	int rc = 0;
	uint32_t reg_mask = 0xFF;

	/* check if driver is probed and private context is initialized */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if (!wcd_usbss_ctxt_->regmap)
		return -EINVAL;

#if 0 //OPLUS_BUG_COMPATIBILITY
	rc = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	rc = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (rc < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
					__func__, rc);
		return rc;
	}

	for (i = 0; i < arr_size; i++) {
		if (write) {
			rc = regmap_write(wcd_usbss_ctxt_->regmap, reg_arr[i][0],
					  reg_arr[i][1] & reg_mask);
			if (rc != 0) {
				dev_err(wcd_usbss_ctxt_->dev,
					"%s: USB-SS register 0x%x (value of 0x%x) write failed\n",
					__func__, reg_arr[i][0], reg_arr[i][1]);
				goto err;
			}
		} else {
			rc = regmap_read(wcd_usbss_ctxt_->regmap, reg_arr[i][0], &reg_arr[i][1]);
			if (rc != 0) {
				dev_err(wcd_usbss_ctxt_->dev,
					"%s: USB-SS register 0x%x read failed\n", __func__,
					reg_arr[i][0]);
				goto err;
			}
		}
	}
err:
	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return 0;
}
EXPORT_SYMBOL(wcd_usbss_register_update);

/*
 * wcd_usbss_is_in_reset_state() - Check whether a negative surge ESD event has occurred.
 *
 * This function has a series of three checks to determine whether a negative surge ESD event has
 * occurred. If any of the three check conditions is met, it is concluded that a negative surge
 * ESD event has occurred. The checks include the following:
 * 1. Register WCD_USBSS_CPLDO_CTL2 reads 0xFF
 * 2. Register WCD_USBSS_RCO_MISC2 Bit<1> reads 0 at least once in NUM_RCO_MISC2_READ reads
 * 3. Register 0x06 Bit<0> reads 1 after toggling register WCD_USBSS_PMP_MISC1 Bit<0> from
 *    0 --> 1 --> 0
 *
 * Return: Returns true if any check(s) fail, false otherwise.
 */
static bool wcd_usbss_is_in_reset_state(void)
{
	bool ret = false;
	int i = 0;
	int rc = 0;
	unsigned int read_val = 0;
	struct device *i2c_bus_dev = wcd_usbss_ctxt_->client->adapter->dev.parent;
#if 0 //OPLUS_BUG_COMPATIBILITY
	bool disable_rpm = false;
#endif /* OPLUS_BUG_COMPATIBILITY */

	rc = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
	if (rc < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, rc);
		return ret;
	}

	if (!pm_runtime_enabled(i2c_bus_dev)) {
#if 0 //OPLUS_BUG_COMPATIBILITY
		pm_runtime_enable(i2c_bus_dev);
		disable_rpm = true;
#else /* OPLUS_BUG_COMPATIBILITY */
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
		mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
			"pm_runtime_enabled i2c_bus failed");
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
		goto done;
#endif /* OPLUS_BUG_COMPATIBILITY */
	}

	/* Check 1: Read WCD_USBSS_CPLDO_CTL2 */
	rc = regmap_read(wcd_usbss_ctxt_->regmap, WCD_USBSS_CPLDO_CTL2, &read_val);
	if (rc != 0)
		goto done;

	if (read_val != 0xFF) {
//#ifdef OPLUS_ARCH_EXTENDS
		dev_err(wcd_usbss_ctxt_->dev, "%s: Surge check #1 failed, cable_status = %d\n", __func__, wcd_usbss_ctxt_->cable_status);
//#endif /* OPLUS_ARCH_EXTENDS */
		ret = true;
		goto done;
	}

	/* Check 2: Read WCD_USBSS_RCO_MISC2 */
	for (i = 0; i < NUM_RCO_MISC2_READ; i++) {
		rc = regmap_read(wcd_usbss_ctxt_->regmap, WCD_USBSS_RCO_MISC2, &read_val);
		if (rc != 0)
			goto done;

		if ((read_val & 0x2) == 0)
			break;
		if (i == (NUM_RCO_MISC2_READ - 1)) {
//#ifdef OPLUS_ARCH_EXTENDS
			dev_err(wcd_usbss_ctxt_->dev, "%s: Surge check #2 failed, cable_status = %d\n", __func__, wcd_usbss_ctxt_->cable_status);
//#endif /* OPLUS_ARCH_EXTENDS */
			ret = true;
			goto done;
		}
	}

#if 1 //OPLUS_BUG_COMPATIBILITY
	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);
	if (!wcd_usbss_ctxt_->is_in_standby) {
		/* Toggle WCD_USBSS_PMP_MISC1 bit<0>: 0 --> 1 --> 0 */
		rc = rc | regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_MISC1, 0x1, 0x0);
		rc = rc | regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_MISC1, 0x1, 0x1);
		rc = rc | regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_MISC1, 0x1, 0x0);

		/* Check 3: Read WCD_USBSS_PMP_MISC2 */
		rc = rc | regmap_read(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_MISC2, &read_val);

		if (rc != 0) {
			dev_err(wcd_usbss_ctxt_->dev, "%s: Surge check #3 read/write reg failed\n", __func__);
			mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
			goto done;
		}

		if ((read_val & 0x1) == 0) {
//#ifdef OPLUS_ARCH_EXTENDS
			dev_err(wcd_usbss_ctxt_->dev, "%s: Surge check #3 failed, cable_status = %d\n", __func__, wcd_usbss_ctxt_->cable_status);
//#endif /* OPLUS_ARCH_EXTENDS */
			ret = true;
		}
	}
	mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
#endif /* OPLUS_BUG_COMPATIBILITY */

done:
#if 0 //OPLUS_BUG_COMPATIBILITY
	if (disable_rpm)
		pm_runtime_disable(i2c_bus_dev);
#endif /* OPLUS_BUG_COMPATIBILITY */

	/* All checks passed, so a negative surge ESD event has not occurred */
	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

//#ifdef OPLUS_ARCH_EXTENDS
	pr_info("%s: Exit ret = %d, cable_status = %d\n", __func__, ret, wcd_usbss_ctxt_->cable_status);
//#endif /* OPLUS_ARCH_EXTENDS */
	return ret;
}

/*
 * wcd_usbss_reset_routine - Uses cached state to restore USB-SS registers after a negative surge.
 *
 * Return: Returns int return value from wcd_usbss_switch_update()
 */
static int wcd_usbss_reset_routine(void)
{
	int ret = 0;
	struct device *i2c_bus_dev = wcd_usbss_ctxt_->client->adapter->dev.parent;
#if 0 //OPLUS_BUG_COMPATIBILITY
	bool disable_rpm = false;
#endif /* OPLUS_BUG_COMPATIBILITY */

//#ifdef OPLUS_ARCH_EXTENDS
	pr_info("%s: Enter, cable_status = %d\n", __func__, wcd_usbss_ctxt_->cable_status);
//#endif /* OPLUS_ARCH_EXTENDS */

#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		return ret;
	}

	if (!pm_runtime_enabled(i2c_bus_dev)) {
#if 0 //OPLUS_BUG_COMPATIBILITY
		pm_runtime_enable(i2c_bus_dev);
		disable_rpm = true;
#else /* OPLUS_BUG_COMPATIBILITY */
		pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
		pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
		mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
			"pm_runtime_enabled i2c_bus failed");
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
		return 0;
#endif /* OPLUS_BUG_COMPATIBILITY */
	}

	/* Mark the cache as dirty to force a flush */
	regcache_mark_dirty(wcd_usbss_ctxt_->regmap);
	regcache_sync(wcd_usbss_ctxt_->regmap);
	/* Write 0xFF to WCD_USBSS_CPLDO_CTL2 */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_CPLDO_CTL2, 0xFF, 0xFF);

	/* Set RCO_EN: WCD_USBSS_USB_SS_CNTL Bit<3> --> 0x0 --> 0x1 */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_USB_SS_CNTL, 0x8, 0x0);
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_USB_SS_CNTL, 0x8, 0x8);

	/* If in audio mode reset codec registers */
	if ((wcd_usbss_ctxt_->cable_status & (BIT(WCD_USBSS_AATC) |
					       BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
					       BIT(WCD_USBSS_HSJ_CONNECT) |
					       BIT(WCD_USBSS_GND_MIC_SWAP_HSJ))))
		blocking_notifier_call_chain(&wcd_usbss_ctxt_->wcd_usbss_notifier,
				WCD_USBSS_SURGE_RESET_EVENT, NULL);

#if 0 //OPLUS_BUG_COMPATIBILITY
	if (disable_rpm)
		pm_runtime_disable(i2c_bus_dev);
#endif /* OPLUS_BUG_COMPATIBILITY */

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return 0;
}
/* Called with switch_update_lock mutex locked */
static void wcd_usbss_standby_control_locked(bool enter_standby)
{
	if (wcd_usbss_ctxt_->is_in_standby == enter_standby)
		return;

	if (enter_standby) {
		dev_dbg(wcd_usbss_ctxt_->dev, "%s: Enabling standby mode\n",
			__func__);
		regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_USB_SS_CNTL, 0x10, 0x10);
		wcd_usbss_ctxt_->is_in_standby = true;
	} else {
		dev_dbg(wcd_usbss_ctxt_->dev, "%s: Disabling standby mode\n",
			__func__);
		regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_USB_SS_CNTL, 0x10, 0x00);
		/* 10ms wait recommended to get WCD USBSS out of standby */
		usleep_range(10000, 10100);
		wcd_usbss_ctxt_->is_in_standby = false;
	}
}

static int wcd_usbss_standby_control(bool enter_standby)
{
	struct device *i2c_bus_dev = wcd_usbss_ctxt_->client->adapter->dev.parent;
#if 0 //OPLUS_BUG_COMPATIBILITY
	bool disable_rpm = false;
#endif /* OPLUS_BUG_COMPATIBILITY */
	int ret = 0;

	if (!wcd_usbss_ctxt_->standby_enable)
		return 0;

#if 0 //OPLUS_BUG_COMPATIBILITY
	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);

	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
		return ret;
	}

	if (!pm_runtime_enabled(i2c_bus_dev)) {
		pm_runtime_enable(i2c_bus_dev);
		disable_rpm = true;
	}
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		return ret;
	}

	if (!wcd_usbss_wait_pm_runtime_enabled(i2c_bus_dev)) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: i2c bus in suspend\n", __func__);
		pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
		pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);
		return -EACCES;
	}

	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);
#endif /* OPLUS_BUG_COMPATIBILITY */

	wcd_usbss_standby_control_locked(enter_standby);

#if 0 //OPLUS_BUG_COMPATIBILITY
	if (disable_rpm)
		pm_runtime_disable(i2c_bus_dev);
#endif /* OPLUS_BUG_COMPATIBILITY */

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);

	return 0;
}

static ssize_t wcd_usbss_surge_enable_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int enable = 0;

	if (kstrtouint(buf, 10, &enable) < 0)
		return -EINVAL;

	/* Return if period is 0ms */
	if (!wcd_usbss_ctxt_->surge_timer_period_ms)
		wcd_usbss_ctxt_->surge_timer_period_ms = DEFAULT_SURGE_TIMER_PERIOD_MS;

	wcd_usbss_ctxt_->surge_enable = enable;

	return count;
}

static ssize_t wcd_usbss_surge_period_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int period_sec = 0;

	if (kstrtouint(buf, 10, &period_sec) < 0)
		return -EINVAL;

	/* Constrain period */
	if (period_sec >= MIN_SURGE_TIMER_PERIOD_SEC && period_sec <= MAX_SURGE_TIMER_PERIOD_SEC)
		wcd_usbss_ctxt_->surge_timer_period_ms = SEC_TO_MS * period_sec;

	if (!wcd_usbss_ctxt_->surge_thread)
		return count;

	/* Wake up thread if usb is connected and surge is enabled */
	if (wcd_usbss_ctxt_->cable_status && wcd_usbss_ctxt_->surge_enable)
		wake_up_process(wcd_usbss_ctxt_->surge_thread);

	return count;
}

static ssize_t wcd_usbss_standby_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int enable = 0;

	if (kstrtouint(buf, 10, &enable) < 0)
		return -EINVAL;

	/* temporarily enabling standby to force proper state update */
	wcd_usbss_ctxt_->standby_enable = true;

	if (enable) {
		if (!wcd_usbss_ctxt_->cable_status)
			wcd_usbss_standby_control(true);
		else
			wcd_usbss_standby_control(false);
	} else {
		wcd_usbss_standby_control(false);
	}

	wcd_usbss_ctxt_->standby_enable = enable;

	return count;
}
/*
 * wcd_usbss_surge_kthread_fn - checks for a negative surge reset at a given period interval
 *
 * Returns 0
 */
static int wcd_usbss_surge_kthread_fn(void *p)
{
	while (!kthread_should_stop()) {
		if (wcd_usbss_ctxt_->cable_status &&
			wcd_usbss_ctxt_->surge_enable &&
#if 0 //OPLUS_BUG_COMPATIBILITY
			wcd_usbss_is_in_reset_state())
			wcd_usbss_reset_routine();
#else /* OPLUS_BUG_COMPATIBILITY */
			!wcd_usbss_ctxt_->suspended) {
			pm_stay_awake(wcd_usbss_ctxt_->dev);
			if (wcd_usbss_is_in_reset_state()) {
				/* Checking whether the surge occurs */
				if (wcd_usbss_ctxt_->check_surge_workqueue) {
					cancel_delayed_work_sync(&wcd_usbss_ctxt_->check_surge_delaywork);
				}
				wcd_usbss_reset_routine();
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
				if (!(wcd_usbss_ctxt_->cable_status & (BIT(WCD_USBSS_USB)))) {
					mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
					"payload@@negative surge occurs, cable_status = %d", wcd_usbss_ctxt_->cable_status);
				}
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
			}
			pm_relax(wcd_usbss_ctxt_->dev);
		}
#endif /* OPLUS_BUG_COMPATIBILITY */
		msleep_interruptible(wcd_usbss_ctxt_->surge_timer_period_ms);
	}

	return 0;
}

/*
 * wcd_usbss_enable_surge_kthread - routine for creating and deploying a kthread to handle surge
 *								   protection.
 */
static void wcd_usbss_enable_surge_kthread(void)
{

	if (!wcd_usbss_ctxt_->surge_enable)
		return;

	if (!wcd_usbss_ctxt_->surge_thread)
		wcd_usbss_ctxt_->surge_thread = kthread_run(wcd_usbss_surge_kthread_fn,
						NULL, "Surge kthread");

	if (!wcd_usbss_ctxt_->surge_thread)
		pr_err("%s, Unable to create WCD USBSS surge kthread.\n", __func__);
}

/*
 * wcd_usbss_disable_surge_kthread - routine for stopping a kthread that handles surge
 *								    protection.
 */
static void wcd_usbss_disable_surge_kthread(void)
{
	if (!wcd_usbss_ctxt_->surge_enable)
		return;

	if (!wcd_usbss_ctxt_->surge_thread)
		return;

	kthread_stop(wcd_usbss_ctxt_->surge_thread);
	wcd_usbss_ctxt_->surge_thread = NULL;
}

//#ifdef OPLUS_ARCH_EXTENDS
/* Checking whether the surge occurs */
static void wcd_usbss_check_surge_work_fn(struct work_struct *work)
{
	struct wcd_usbss_ctxt *priv =
		container_of(work, struct wcd_usbss_ctxt, check_surge_delaywork.work);

	if (!priv) {
		pr_err("%s: wcd usbss container invalid\n", __func__);
		return;
	}

	if (priv->cable_status) {
		pm_stay_awake(wcd_usbss_ctxt_->dev);
		if(wcd_usbss_is_in_reset_state()) {
			pr_err("%s: the surge event occurs, reset usbss\n", __func__);
			wcd_usbss_reset_routine();
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
			mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
				"payload@@usbss surge occurs, cable_status = %d", priv->cable_status);
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
		}
		pm_relax(wcd_usbss_ctxt_->dev);
	}
}
//#endif /* OPLUS_ARCH_EXTENDS */

static int wcd_usbss_sysfs_init(struct wcd_usbss_ctxt *priv)
{
	int rc = 0;

	priv->surge_kobject = kobject_create_and_add("wcd_usbss", kernel_kobj);

	if (!(priv->surge_kobject)) {
		dev_err(priv->dev, "%s: sysfs failed, surge kobj not created\n", __func__);
		return -ENOMEM;
	}

	rc = sysfs_create_file(priv->surge_kobject, &wcd_usbss_surge_enable_attribute.attr);
	if (rc < 0) {
		dev_err(priv->dev,
			"%s: sysfs failed, unable to register surge enable attribute. rc: %d\n",
			__func__, rc);
		return rc;
	}

	rc = sysfs_create_file(priv->surge_kobject, &wcd_usbss_surge_period_attribute.attr);
	if (rc < 0) {
		dev_err(priv->dev,
			"%s: sysfs failed, unable to register surge period attribute. rc: %d\n",
			__func__, rc);
		return rc;
	}

	rc = sysfs_create_file(priv->surge_kobject, &wcd_usbss_standby_enable_attribute.attr);
	if (rc < 0) {
		dev_err(priv->dev,
			"%s: sysfs failed, unable to register standby enable attribute. rc: %d\n",
			__func__, rc);
		return rc;
	}

	return 0;
}

static int wcd_usbss_usbc_event_changed(struct notifier_block *nb,
				      unsigned long evt, void *ptr)
{
	struct wcd_usbss_ctxt *priv =
			container_of(nb, struct wcd_usbss_ctxt, ucsi_nb);
	struct device *dev;
	enum typec_accessory acc = ((struct ucsi_glink_constat_info *)ptr)->acc;

	if (!priv)
		return -EINVAL;

	dev = priv->dev;
	if (!dev)
		return -EINVAL;

//#ifdef OPLUS_ARCH_EXTENDS
	dev_info(dev, "%s: USB change event received, supply mode %d, usbc mode %ld, expected %d\n",
			__func__, acc, priv->usbc_mode.counter,
			TYPEC_ACCESSORY_AUDIO);
//#endif /* OPLUS_ARCH_EXTENDS */

	switch (acc) {
	case TYPEC_ACCESSORY_AUDIO:
	case TYPEC_ACCESSORY_NONE:
		if (atomic_read(&(priv->usbc_mode)) == acc)
			break; /* filter notifications received before */
		atomic_set(&(priv->usbc_mode), acc);

		dev_dbg(dev, "%s: queueing usbc_analog_work\n",
			__func__);
		pm_stay_awake(priv->dev);
		queue_work(system_freezable_wq, &priv->usbc_analog_work);
		break;
	default:
		break;
	}

	return 0;
}

static int wcd_usbss_usbc_analog_setup_switches(struct wcd_usbss_ctxt *priv)
{
	int rc = 0;
	int mode;
	struct device *dev;
	bool cable_status_cache = false;

	if (!priv)
		return -EINVAL;

	dev = priv->dev;
	if (!dev)
		return -EINVAL;

	mutex_lock(&priv->notification_lock);
	/* get latest mode again within locked context */
	mode = atomic_read(&(priv->usbc_mode));

//#ifdef OPLUS_ARCH_EXTENDS
	dev_info(dev, "%s: setting GPIOs active = %d cable_status = %d mode = %d\n",
		__func__, mode != TYPEC_ACCESSORY_NONE, priv->cable_status, mode);
//#endif /* OPLUS_ARCH_EXTENDS */

	switch (mode) {
	/* add all modes WCD USBSS should notify for in here */
	case TYPEC_ACCESSORY_AUDIO:
		/*
		 * If cable_type is already decided, update the cable_status to
		 * avoid reconfiguration of AATC switch settings again
		 */
		if (priv->cable_status & (BIT(WCD_USBSS_AATC) |
					  BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
					  BIT(WCD_USBSS_HSJ_CONNECT) |
					  BIT(WCD_USBSS_GND_MIC_SWAP_HSJ)))
			cable_status_cache = true;
		/* notify call chain on event */
		blocking_notifier_call_chain(&priv->wcd_usbss_notifier,
					     mode, &cable_status_cache);
		break;
	case TYPEC_ACCESSORY_NONE:
		/* notify call chain on event */
		blocking_notifier_call_chain(&priv->wcd_usbss_notifier,
				TYPEC_ACCESSORY_NONE, NULL);
		break;
	default:
		/* ignore other usb connection modes */
		break;
	}

	mutex_unlock(&priv->notification_lock);
	return rc;
}

static int wcd_usbss_validate_display_port_settings(struct wcd_usbss_ctxt *priv,
						enum wcd_usbss_cable_types ctype)
{
	unsigned int sts;
	int rc;

	rc = regmap_read(priv->regmap, WCD_USBSS_SWITCH_STATUS1, &sts);
	if (rc)
		return rc;

	sts &= 0xCC;
	pr_info("DPAUX switch status (MG1/2): %08x\n", sts);

	if (ctype == WCD_USBSS_DP_AUX_CC1 && sts == 0x48)
		return 0;

	if (ctype == WCD_USBSS_DP_AUX_CC2 && sts == 0x84)
		return 0;

	pr_err("Failed to update switch for display port\n");
	rc = -EINVAL;

	return rc;
}

static int wcd_usbss_switch_update_defaults(struct wcd_usbss_ctxt *priv)
{
	dev_dbg(priv->dev, "restoring defaults\n");
	/* Disable all switches */
	regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x00);
	/* Select MG1 for AGND_SWITCHES */
	regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SELECT1, 0x01, 0x00);
	/* Select GSBU1 and MG1 for MIC_SWITCHES */
	regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SELECT0, 0x03, 0x00);
	/* Enable OVP_MG1_BIAS PCOMP_DYN_BST_EN */
	regmap_update_bits(priv->regmap, WCD_USBSS_MG1_BIAS, 0x08, 0x08);
	/* Enable OVP_MG2_BIAS PCOMP_DYN_BST_EN */
	regmap_update_bits(priv->regmap, WCD_USBSS_MG2_BIAS, 0x08, 0x08);
	regmap_update_bits_base(priv->regmap, WCD_USBSS_AUDIO_FSM_START, 0x01,
			0x01, NULL, false, true);
	/* Select DN for DNL_SWITHCES and DP for DPR_SWITCHES */
	regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SELECT0, 0x3C, 0x14);
	regmap_update_bits(priv->regmap, WCD_USBSS_USB_SS_CNTL, 0x07, 0x05); /* Mode5: USB*/
	regmap_write(priv->regmap, WCD_USBSS_PMP_EN, 0x0);
	if (wcd_usbss_ctxt_->version == WCD_USBSS_2_0)
		regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_OUT1,
				0x40, 0x00);
	regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_SW_CTRL_1, 0x00);
	regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0x00);

	/* Once plug-out done, restore to MANUAL mode */
	audio_fsm_mode = WCD_USBSS_AUDIO_MANUAL;
	return 0;
}

static void wcd_usbss_update_reg_init(struct regmap *regmap)
{
	if (audio_fsm_mode == WCD_USBSS_AUDIO_FSM)
		regmap_update_bits(regmap, WCD_USBSS_FUNCTION_ENABLE, 0x03,
				0x02); /* AUDIO_FSM mode */
	else
		regmap_update_bits(regmap, WCD_USBSS_FUNCTION_ENABLE, 0x03,
				0x01); /* AUDIO_MANUAL mode */

	/* Enable dynamic boosting for DP and DN */
	regmap_update_bits(wcd_usbss_ctxt_->regmap,
						WCD_USBSS_DP_DN_MISC1, 0x09, 0x09);
	/* Enable dynamic boosting for MG1 OVP */
	regmap_update_bits(wcd_usbss_ctxt_->regmap,
						WCD_USBSS_MG1_MISC, 0x24, 0x24);
	/* Enable dynamic boosting for MG2 OVP */
	regmap_update_bits(wcd_usbss_ctxt_->regmap,
						WCD_USBSS_MG2_MISC, 0x24, 0x24);

	/* Disable Equalizer */
	regmap_update_bits(regmap, WCD_USBSS_EQUALIZER1,
			WCD_USBSS_EQUALIZER1_EQ_EN_MASK, 0x00);
	/* For surge reset routine: Write WCD_USBSS_CPLDO_CTL2 --> 0xFF */
	regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_CPLDO_CTL2, 0xFF, 0xFF);
}

#define AUXP_M_EN_MASK	(WCD_USBSS_SWITCH_SETTINGS_ENABLE_DP_AUXM_TO_MGX_SWITCHES_MASK |\
			WCD_USBSS_SWITCH_SETTINGS_ENABLE_DP_AUXP_TO_MGX_SWITCHES_MASK)

static int wcd_usbss_display_port_switch_update(struct wcd_usbss_ctxt *priv,
					enum wcd_usbss_cable_types ctype)
{
	pr_info("Configuring display port for ctype %d\n", ctype);

	/* Disable AUX switches */
	regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE, AUXP_M_EN_MASK, 0x00);

	/* Select MG1 for AUXP and MG2 for AUXM */
	if (ctype == WCD_USBSS_DP_AUX_CC1)
		regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SELECT0, 0xC0, 0x40);
	/* Select MG2 for AUXP and MG1 for AUXM */
	else
		regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SELECT0, 0xC0, 0x80);

	/* Enable DP_AUXP_TO_MGX and DP_AUXM_TO_MGX switches */
	regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE, AUXP_M_EN_MASK, 0x60);
	return wcd_usbss_validate_display_port_settings(priv, ctype);
}

static void wcd_usbss_dpdm_switch_connect(struct wcd_usbss_ctxt *priv, bool connect)
{
	if (connect)
		regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				0x18, 0x18);
	else
		regmap_update_bits(priv->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				0x18, 0x00);
}

static const char *status_to_str(int status)
{
	switch (status) {
	case WCD_USBSS_LPD_USB_MODE_CLEAR:
		return "STANDBY";
	case WCD_USBSS_LPD_MODE_SET:
		return "LPD";
	case WCD_USBSS_USB_MODE_SET:
		return "USB";
	case WCD_USBSS_LPD_USB_MODE_SET:
		return "LPD_USB";
	case WCD_USBSS_AUDIO_MODE_SET:
		return "AUDIO";
	default:
		return "UNDEFINED";
	}
}

/* to use with DPDM switch selection */
#define DPDM_SEL_MASK       (WCD_USBSS_SWITCH_SELECT0_DPR_SWITCHES_MASK |\
					WCD_USBSS_SWITCH_SELECT0_DNL_SWITCHES_MASK)
#define DPDM_SEL_ENABLE     ((0x1 << WCD_USBSS_SWITCH_SELECT0_DPR_SWITCHES_SHIFT) |\
					(0x1 << WCD_USBSS_SWITCH_SELECT0_DNL_SWITCHES_SHIFT))
#define DPDM_SEL_DISABLE    0x0

/* to use with DPDM switch enable/disable*/
#define DPDM_SW_EN_MASK     (WCD_USBSS_SWITCH_SETTINGS_ENABLE_DPR_SWITCHES_MASK |\
					WCD_USBSS_SWITCH_SETTINGS_ENABLE_DNL_SWITCHES_MASK)
#define DPDM_SW_ENABLE      ((0x1 << WCD_USBSS_SWITCH_SETTINGS_ENABLE_DNL_SWITCHES_SHIFT) |\
				(0x1 << WCD_USBSS_SWITCH_SETTINGS_ENABLE_DPR_SWITCHES_SHIFT))
#define DPDM_SW_DISABLE     0x0

/*
 * wcd_usbss_dpdm_switch_update - configure WCD USBSS DP/DM switch position
 *
 * @sw_en: enable or disable DP/DM switches.
 * @eq_en: enable or disable equalizer. Usually true in case of USB high-speed.
 *
 * Returns zero for success, a negative number on error.
 */
int wcd_usbss_dpdm_switch_update(bool sw_en, bool eq_en)
{
	int ret = 0;

	/* check if driver is probed and private context is initialized */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if (!wcd_usbss_ctxt_->regmap)
		return -EINVAL;

#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		return ret;
	}
	ret = regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				DPDM_SW_EN_MASK, (sw_en ? DPDM_SW_ENABLE : DPDM_SW_DISABLE));
	if (ret)
		pr_err("%s(): Failed to write dpdm_en_value ret:%d\n", __func__, ret);

	ret = regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_EQUALIZER1,
				WCD_USBSS_EQUALIZER1_EQ_EN_MASK,
				(eq_en ? WCD_USBSS_EQUALIZER1_EQ_EN_MASK : 0x0));
	if (ret)
		pr_err("%s(): Failed to write equalizer1_en ret:%d\n", __func__, ret);

#ifdef OPLUS_FEATURE_CHG_BASIC
	/* 8 is the default value. you can change as what you want to set like x.
	 * x << 3, the x is the decimal value you want to write.
	 */
	if (eq_en && wcd_usbss_equalizer1) {
		ret = regmap_update_bits(wcd_usbss_ctxt_->regmap, WCD_USBSS_EQUALIZER1,
				WCD_USBSS_EQUALIZER1_BW_SETTINGS_MASK, wcd_usbss_equalizer1 << 3);
		pr_err("%s(): write wcd_usbss_equalizer1:%d", __func__, wcd_usbss_equalizer1);
	}
#endif

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return ret;
}
EXPORT_SYMBOL(wcd_usbss_dpdm_switch_update);

/* wcd_usbss_audio_config - configure audio for power mode and Impedance calculations
 *
 * @enable: enable/disable switch settings for MIC and SENSE for impedance readings
 * @config_type: Config type to configure audio
 * @power_mode: power mode type to config
 *
 * Returns int on whether the config happened or not. -ENODEV is returned
 * in case if the driver is not probed.
 */

int wcd_usbss_audio_config(bool enable, enum wcd_usbss_config_type config_type,
			unsigned int power_mode)
{

	int rc = 0;
	struct device *i2c_bus_dev = wcd_usbss_ctxt_->client->adapter->dev.parent;
#if 0 //OPLUS_BUG_COMPATIBILITY
	bool disable_rpm = false;
#endif /* OPLUS_BUG_COMPATIBILITY */
	unsigned int current_power_mode;

	/* check if driver is probed and private context is init'ed */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if (!wcd_usbss_ctxt_->regmap)
		return -EINVAL;

	pr_info("%s: connect_status = 0x%x, power mode = %d\n",
		__func__, wcd_usbss_ctxt_->cable_status, power_mode);

	if (!(wcd_usbss_ctxt_->cable_status & (BIT(WCD_USBSS_AATC) |
					       BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
					       BIT(WCD_USBSS_HSJ_CONNECT) |
					       BIT(WCD_USBSS_GND_MIC_SWAP_HSJ))))
		return 0;

#if 0 //OPLUS_BUG_COMPATIBILITY
	rc = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	rc = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (rc < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, rc);
		return rc;
	}

#if 0 //OPLUS_BUG_COMPATIBILITY
	if (!pm_runtime_enabled(i2c_bus_dev)) {
		pm_runtime_enable(i2c_bus_dev);
		disable_rpm = true;
	}
#else /* OPLUS_BUG_COMPATIBILITY */
	if (!wcd_usbss_wait_pm_runtime_enabled(i2c_bus_dev)) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: i2c bus in suspend\n", __func__);
		pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
		pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);
		return -EACCES;
	}
#endif /* OPLUS_BUG_COMPATIBILITY */

	regmap_read(wcd_usbss_ctxt_->regmap, WCD_USBSS_USB_SS_CNTL, &current_power_mode);
	if ((current_power_mode & 0x07) == power_mode)
		goto exit;

	switch (config_type) {
	case WCD_USBSS_CONFIG_TYPE_POWER_MODE:
		/* switching to MBHC mode */
		if (power_mode == 0x1) {
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_SW_CTRL_1, 0x98);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_EN, 0xF);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0x82);
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_USB_SS_CNTL, 0x07, power_mode);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0x02);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_SW_CTRL_1, 0x9E);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_CLK, 0x10);
		} else { /* switching to ULP/HiFi/Std */
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0x82);
			if (power_mode == 0x2) /* ULP */
				regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_CLK, 0x1C);
			else
				regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_CLK, 0x10);

			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_USB_SS_CNTL, 0x07, power_mode);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_EN, 0x0);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0xB2);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_SW_CTRL_1, 0x90);
		}
		break;
	default:
		pr_err("%s Invalid config type %d\n", __func__, config_type);
		rc = -EINVAL;
	}

exit:
#if 0 //OPLUS_BUG_COMPATIBILITY
	if (disable_rpm)
		pm_runtime_disable(i2c_bus_dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return rc;
}
EXPORT_SYMBOL(wcd_usbss_audio_config);

/*
 * wcd_usbss_switch_update - configure WCD USBSS switch position based on
 *  cable type and status
 *
 * @ctype - cable type
 * @connect_status - cable connected/disconnected status
 *
 * Returns int on whether the switch happened or not. -ENODEV is returned
 *  in case if the driver is not probed
 */
int wcd_usbss_switch_update(enum wcd_usbss_cable_types ctype,
							enum wcd_usbss_cable_status connect_status)
{
	int i = 0, ret = 0;
	bool audio_switch = false;
#ifdef OPLUS_FEATURE_CHG_BASIC
	unsigned int debug_buf[9] = {0};
#endif

	/* check if driver is probed and private context is init'ed */
	if (wcd_usbss_ctxt_ == NULL)
		return -ENODEV;

	if (!wcd_usbss_ctxt_->regmap)
		return -EINVAL;

//#ifdef OPLUS_ARCH_EXTENDS
/* Checking whether the surge occurs */
	if (wcd_usbss_ctxt_->check_surge_workqueue) {
		cancel_delayed_work_sync(&wcd_usbss_ctxt_->check_surge_delaywork);
	}
//#endif /* OPLUS_ARCH_EXTENDS */
#if 0 //OPLUS_BUG_COMPATIBILITY
	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);

	pr_info("%s: ctype = %d, connect_status = %d\n",
		__func__, ctype, connect_status);

	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
		return ret;
	}
#else /* OPLUS_BUG_COMPATIBILITY */
	pr_info("%s: ctype = %d, connect_status = %d\n",
		__func__, ctype, connect_status);

	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		return ret;
	}

	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);
#endif /* OPLUS_BUG_COMPATIBILITY */


	if (connect_status == WCD_USBSS_CABLE_DISCONNECT) {
		wcd_usbss_ctxt_->cable_status &= ~BIT(ctype);

		switch (ctype) {
		case WCD_USBSS_USB:
			/* Keep DP/DM switch on but disable EQ */
			if (wcd_usbss_ctxt_->standby_enable && wcd_usbss_ctxt_->is_in_standby)
				wcd_usbss_dpdm_switch_update(false, false);
			else
				wcd_usbss_dpdm_switch_update(true, false);
			break;
		case WCD_USBSS_DP_AUX_CC1:
			fallthrough;
		case WCD_USBSS_DP_AUX_CC2:
			/* Disable AUX switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT0, 0xC0, 0x00);
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE,
					AUXP_M_EN_MASK, 0x00);
			wcd_usbss_ctxt_->cable_status &= ~BIT(WCD_USBSS_DP_AUX_CC1);
			wcd_usbss_ctxt_->cable_status &= ~BIT(WCD_USBSS_DP_AUX_CC2);
			break;
		case WCD_USBSS_AATC:
			wcd_usbss_ctxt_->cable_status &= ~BIT(WCD_USBSS_GND_MIC_SWAP_AATC);
			audio_switch = true;
			break;
		case WCD_USBSS_GND_MIC_SWAP_AATC:
			wcd_usbss_ctxt_->cable_status &= ~BIT(WCD_USBSS_AATC);
			audio_switch = true;
			break;
		case WCD_USBSS_HSJ_CONNECT:
			wcd_usbss_ctxt_->cable_status &= ~BIT(WCD_USBSS_GND_MIC_SWAP_HSJ);
			audio_switch = true;
			break;
		case WCD_USBSS_GND_MIC_SWAP_HSJ:
			wcd_usbss_ctxt_->cable_status &= ~BIT(WCD_USBSS_HSJ_CONNECT);
			audio_switch = true;
			break;
		default:
			break;
		}

		/* reset to defaults when all cable types are disconnected */
		if (!wcd_usbss_ctxt_->cable_status && audio_switch) {
			wcd_usbss_switch_update_defaults(wcd_usbss_ctxt_);
			if (wcd_usbss_ctxt_->standby_enable) {
				wcd_usbss_dpdm_switch_connect(wcd_usbss_ctxt_, false);
				wcd_usbss_standby_control_locked(true);
				wcd_usbss_ctxt_->wcd_standby_status = WCD_USBSS_LPD_USB_MODE_CLEAR;
				dev_dbg(wcd_usbss_ctxt_->dev, "wcd state transition to %s complete\n",
						status_to_str(wcd_usbss_ctxt_->wcd_standby_status));
			} else {
				wcd_usbss_dpdm_switch_connect(wcd_usbss_ctxt_, true);
				wcd_usbss_ctxt_->wcd_standby_status = WCD_USBSS_USB_MODE_SET;
				dev_dbg(wcd_usbss_ctxt_->dev, "wcd state transition to %s complete\n",
						status_to_str(wcd_usbss_ctxt_->wcd_standby_status));
			}
		}
	} else if (connect_status == WCD_USBSS_CABLE_CONNECT) {
		wcd_usbss_ctxt_->cable_status |= BIT(ctype);

		wcd_usbss_standby_control_locked(false);

		switch (ctype) {
		case WCD_USBSS_USB:
			wcd_usbss_dpdm_switch_update(true, true);
			break;
		case WCD_USBSS_AATC:
			/* Update power mode to mode 1 for AATC */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
				WCD_USBSS_USB_SS_CNTL, 0x07, 0x01);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_EN, 0xF);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_SW_CTRL_1, 0x9E);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0x02);
			if (wcd_usbss_ctxt_->version == WCD_USBSS_2_0)
				regmap_update_bits(wcd_usbss_ctxt_->regmap,
						WCD_USBSS_PMP_OUT1, 0x40, 0x40);
			/* for AATC plug-in, change mode to FSM */
			audio_fsm_mode = WCD_USBSS_AUDIO_FSM;
			/* Disable all switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
				WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x7F, 0x00);
			if (audio_fsm_mode == WCD_USBSS_AUDIO_FSM) {
				regmap_update_bits_base(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_AUDIO_FSM_START, 0x01, 0x01, NULL, false, true);
			}
			/* Select L, R, GSBU2, MG1 */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT0, 0x3F, 0x02);
			/* Disable OVP_MG2_BIAS PCOMP_DYN_BST_EN */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_MG2_BIAS, 0x08, 0x00);
			/* Enable SENSE, MIC switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x06, 0x06);
			/* Select MG2 for AGND_SWITCHES */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT1, 0x01, 0x01);
			/* Enable AGND switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x01, 0x01);
			/* Enable DPR, DNL */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x18);
			/* Set DELAY_L_SW to CYL_1K */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_DELAY_L_SW, 0xFF, 0x02);
			/* Set DELAY_R_SW to CYL_1K */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_DELAY_R_SW, 0xFF, 0x02);
			/* Set DELAY_MIC_SW to CYL_1K */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_DELAY_MIC_SW, 0xFF, 0x01);
			if (audio_fsm_mode == WCD_USBSS_AUDIO_FSM) {
				regmap_update_bits_base(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_AUDIO_FSM_START, 0x01, 0x01, NULL, false, true);
			}
			for (i = 0; i < ARRAY_SIZE(coeff_init); ++i)
				regmap_update_bits(wcd_usbss_ctxt_->regmap, coeff_init[i].reg,
						coeff_init[i].mask, coeff_init[i].val);
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_USB_SS_CNTL, 0x08, 0x00);
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_USB_SS_CNTL, 0x08, 0x08);
			usleep_range(10000, 10100);
			break;
		case WCD_USBSS_GND_MIC_SWAP_AATC:
			dev_info(wcd_usbss_ctxt_->dev,
					"%s: GND MIC Swap register updates..\n", __func__);
			/* Update power mode to mode 1 for AATC */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
				WCD_USBSS_USB_SS_CNTL, 0x07, 0x01);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_EN, 0xF);
			if (wcd_usbss_ctxt_->version == WCD_USBSS_2_0)
				regmap_update_bits(wcd_usbss_ctxt_->regmap,
						WCD_USBSS_PMP_OUT1, 0x40, 0x40);
			/* for GND MIC Swap, change mode to FSM */
			audio_fsm_mode = WCD_USBSS_AUDIO_FSM;
			/* Disable all switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x7F, 0x00);
			/* Select L, R, GSBU1, MG2 */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT0, 0x3F, 0x01);
			/* Disable OVP_MG1_BIAS PCOMP_DYN_BST_EN */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_MG1_BIAS, 0x08, 0x00);
			/* Enable SENSE, MIC switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x06, 0x06);
			/* Select MG1 for AGND_SWITCHES */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT1, 0x01, 0x00);
			/* Enable AGND switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x01, 0x01);
			/* Enable DPR, DNL */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x18);
			regmap_update_bits_base(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_AUDIO_FSM_START, 0x01, 0x01, NULL, false, true);
			break;
		case WCD_USBSS_HSJ_CONNECT:
			/* Update power mode to mode 1 for AATC */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
				WCD_USBSS_USB_SS_CNTL, 0x07, 0x01);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_PMP_EN, 0xF);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_SW_CTRL_1, 0x9E);
			regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_EXT_LIN_EN, 0x02);
			if (wcd_usbss_ctxt_->version == WCD_USBSS_2_0)
				regmap_update_bits(wcd_usbss_ctxt_->regmap,
						WCD_USBSS_PMP_OUT1, 0x40, 0x40);
			/* Select MG2, GSBU1 */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT0, 0x03, 0x1);
			/* Disable OVP_MG1_BIAS PCOMP_DYN_BST_EN */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_MG1_BIAS, 0x08, 0x00);
			/* Enable SENSE, MIC, AGND switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x07);
			break;
		case WCD_USBSS_GND_MIC_SWAP_HSJ:
			/* Disable SENSE, MIC, AGND switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x00);
			/* Select MG1, GSBU2 */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT0, 0x03, 0x2);
			/* Enable SENSE, MIC, AGND switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x07);
			break;
		case WCD_USBSS_CHARGER:
			/* Disable DN DP Switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x00);
			/* Select DN2 DP2 */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SELECT0, 0x3C, 0x28);
			/* Enable DN DP Switches */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x18);
			break;
		case WCD_USBSS_DP_AUX_CC1:
			fallthrough;
		case WCD_USBSS_DP_AUX_CC2:
			/* Update Leakage Canceller Coefficient for AUXP pins */
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_DISP_AUXP_CTL, 0x07, 0x01);
			regmap_update_bits(wcd_usbss_ctxt_->regmap,
					WCD_USBSS_DISP_AUXP_THRESH, 0xE0, 0xE0);
			ret = wcd_usbss_display_port_switch_update(wcd_usbss_ctxt_, ctype);
			if (ret) /* clear DP AUX bit if DP switch update fails */
				wcd_usbss_ctxt_->cable_status &= ~BIT(ctype);
			break;
		default:
			break;
		}
		if ((wcd_usbss_ctxt_->cable_status & (BIT(WCD_USBSS_AATC) |
						BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
						BIT(WCD_USBSS_HSJ_CONNECT) |
						BIT(WCD_USBSS_GND_MIC_SWAP_HSJ)))) {
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
			if (wcd_usbss_ctxt_->cable_status & (BIT(WCD_USBSS_USB) |
							BIT(WCD_USBSS_DP_AUX_CC1) |
							BIT(WCD_USBSS_DP_AUX_CC2) |
							BIT(WCD_USBSS_CHARGER))) {
				dev_err(wcd_usbss_ctxt_->dev, "error state 0x%x\n", wcd_usbss_ctxt_->cable_status);
				mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
					"payload@@wcd_usbss_switch_update error state 0x%x", wcd_usbss_ctxt_->cable_status);
			}
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
			wcd_usbss_ctxt_->wcd_standby_status = WCD_USBSS_AUDIO_MODE_SET;
			dev_dbg(wcd_usbss_ctxt_->dev, "wcd state transition to %s complete\n",
					status_to_str(wcd_usbss_ctxt_->wcd_standby_status));
//#ifdef OPLUS_ARCH_EXTENDS
/* Checking whether the surge occurs */
			if (wcd_usbss_ctxt_->check_surge_workqueue) {
				dev_dbg(wcd_usbss_ctxt_->dev, "%s: queueing check_surge_workqueue\n",
					__func__);
				queue_delayed_work(wcd_usbss_ctxt_->check_surge_workqueue, &wcd_usbss_ctxt_->check_surge_delaywork, msecs_to_jiffies(200));
			}
//#endif /* OPLUS_ARCH_EXTENDS */
		}
	}

//#ifdef OPLUS_ARCH_EXTENDS
	pr_info("%s: Exit, cable_status = %d\n", __func__, wcd_usbss_ctxt_->cable_status);
//#endif /* OPLUS_ARCH_EXTENDS */
#ifdef OPLUS_FEATURE_CHG_BASIC
	for (i = 0; i < 8; ++i) {
		ret = regmap_read(wcd_usbss_ctxt_->regmap, 0x400 + i, &debug_buf[i]);
		if (ret != 0) {
			printk(KERN_ERR "0x%x: read error, ", 0x400 + i);
			debug_buf[i] = 0xffff;
		}
	}
	printk(KERN_ERR "WCD switch registers[0x400~0x407]:0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x", debug_buf[0],
		debug_buf[1],debug_buf[2], debug_buf[3], debug_buf[4], debug_buf[5], debug_buf[6], debug_buf[7]);
	for (i = 0; i < 8; ++i) {
		ret = regmap_read(wcd_usbss_ctxt_->regmap, 0x408 + i, &debug_buf[i]);
		if (ret != 0) {
			printk(KERN_ERR "0x%x: read error, ", 0x408 + i);
			debug_buf[i] = 0xffff;
		}
	}
	printk(KERN_ERR "WCD switch registers[0x408~0x40f]:0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x", debug_buf[0],
		debug_buf[1],debug_buf[2], debug_buf[3], debug_buf[4], debug_buf[5], debug_buf[6], debug_buf[7]);
	for (i = 0; i < 9; ++i) {
		ret = regmap_read(wcd_usbss_ctxt_->regmap, 0x410 + i, &debug_buf[i]);
		if (ret != 0) {
			printk(KERN_ERR "0x%x: read error, ", 0x410 + i);
			debug_buf[i] = 0xffff;
		}
	}
	printk(KERN_ERR "WCD switch registers[0x410~0x419]:0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x", debug_buf[0],
		debug_buf[1],debug_buf[2], debug_buf[3], debug_buf[4], debug_buf[5], debug_buf[6], debug_buf[7], debug_buf[8]);
#endif

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
	return ret;
}
EXPORT_SYMBOL(wcd_usbss_switch_update);

/*
 * wcd_usbss_reg_notifier - register notifier block with wcd usbss driver
 *
 * @nb - notifier block of wcd_usbss
 * @node - phandle node to wcd_usbss device
 *
 * Returns 0 on success, or error code
 */
int wcd_usbss_reg_notifier(struct notifier_block *nb,
			 struct device_node *node)
{
	int rc = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct wcd_usbss_ctxt *priv;

	if (!client)
		return -EINVAL;

	priv = (struct wcd_usbss_ctxt *)i2c_get_clientdata(client);
	if (!priv)
		return -EINVAL;

	rc = blocking_notifier_chain_register
				(&priv->wcd_usbss_notifier, nb);

	dev_dbg(priv->dev, "%s: registered notifier for %s\n",
		__func__, node->name);
	if (rc)
		return rc;

	/*
	 * as part of the init sequence check if there is a connected
	 * USB C analog adapter
	 */
	if (atomic_read(&(priv->usbc_mode)) == TYPEC_ACCESSORY_AUDIO) {
		dev_dbg(priv->dev, "%s: analog adapter already inserted\n",
			__func__);
		rc = wcd_usbss_usbc_analog_setup_switches(priv);
	}

	return rc;
}
EXPORT_SYMBOL(wcd_usbss_reg_notifier);

/*
 * wcd_usbss_unreg_notifier - unregister notifier block with wcd usbss driver
 *
 * @nb - notifier block of wcd_usbss
 * @node - phandle node to wcd_usbss device
 *
 * Returns 0 on pass, or error code
 */
int wcd_usbss_unreg_notifier(struct notifier_block *nb,
			     struct device_node *node)
{
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct wcd_usbss_ctxt *priv;

	if (!client)
		return -EINVAL;

	priv = (struct wcd_usbss_ctxt *)i2c_get_clientdata(client);
	if (!priv)
		return -EINVAL;

	return blocking_notifier_chain_unregister
					(&priv->wcd_usbss_notifier, nb);
}
EXPORT_SYMBOL(wcd_usbss_unreg_notifier);


/*
 * wcd_usbss_update_default_trim - update default trim for TP < 3
 *
 * Returns 0 on pass, or error code
 */
int wcd_usbss_update_default_trim(void)
{
	int ret = 0;
	if (!wcd_usbss_ctxt_)
		return -ENODEV;

	if (!wcd_usbss_ctxt_->regmap)
		return -EINVAL;

#if 0 //OPLUS_BUG_COMPATIBILITY
	ret = pm_runtime_resume_and_get(wcd_usbss_ctxt_->dev);
#else /* OPLUS_BUG_COMPATIBILITY */
	ret = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	if (ret < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, ret);
		return ret;
	}
	regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_SW_LIN_CTRL_1, 0x01);
	regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_DC_TRIMCODE_1, 0x00);
	regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_DC_TRIMCODE_2, 0x00);
	regmap_write(wcd_usbss_ctxt_->regmap, WCD_USBSS_DC_TRIMCODE_3, 0x00);

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);

	return ret;
}
EXPORT_SYMBOL(wcd_usbss_update_default_trim);

static void wcd_usbss_usbc_analog_work_fn(struct work_struct *work)
{
	struct wcd_usbss_ctxt *priv =
		container_of(work, struct wcd_usbss_ctxt, usbc_analog_work);

	if (!priv) {
		pr_err("%s: wcd usbss container invalid\n", __func__);
		return;
	}
	wcd_usbss_usbc_analog_setup_switches(priv);
	pm_relax(priv->dev);
}

static int wcd_usbss_init_optional_reset_pins(struct wcd_usbss_ctxt *priv)
{
	priv->rst_pins = devm_pinctrl_get(priv->dev);
	if (IS_ERR_OR_NULL(priv->rst_pins)) {
		dev_dbg(priv->dev, "Cannot get wcd usbss reset pinctrl:%ld\n",
				PTR_ERR(priv->rst_pins));
		return PTR_ERR(priv->rst_pins);
	}

	priv->rst_pins_active = pinctrl_lookup_state(
			priv->rst_pins, "active");
	if (IS_ERR_OR_NULL(priv->rst_pins_active)) {
		dev_dbg(priv->dev, "Cannot get active pinctrl state:%ld\n",
				PTR_ERR(priv->rst_pins_active));
		return PTR_ERR(priv->rst_pins_active);
	}

	if (priv->rst_pins_active)
		return pinctrl_select_state(priv->rst_pins,
				priv->rst_pins_active);

	return 0;
}

/* called from switch_update_lock mutex locked */
static int wcd_usbss_sdam_handle_events_locked(int req_state)
{
	struct wcd_usbss_ctxt *priv = wcd_usbss_ctxt_;
	int rc = 0;

	switch (req_state) {
	case WCD_USBSS_LPD_USB_MODE_CLEAR:
		/* Enable D+/D- 1M & 400K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_BIAS_TOP, 0x20, 0x00);

		/* Enable DP/DN 2K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_DP_BIAS, 0x01, 0x01);
		regmap_update_bits(priv->regmap, WCD_USBSS_DN_BIAS, 0x01, 0x01);

		/* Enable SBU1/2 2K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_MG1_BIAS, 0x01, 0x01);
		regmap_update_bits(priv->regmap, WCD_USBSS_MG2_BIAS, 0x01, 0x01);
		/* Disconnect D+/D- switch */
		wcd_usbss_dpdm_switch_update(false, false);

		/* Enter standby */
		wcd_usbss_standby_control_locked(true);
		break;
	case WCD_USBSS_LPD_MODE_SET:
		fallthrough;
	case WCD_USBSS_LPD_USB_MODE_SET:
		/* Disable D+/D- 1M & 400K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_BIAS_TOP, 0x20, 0x20);
		/* Disable DP/DN 2K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_DP_BIAS, 0x01, 0x00);
		regmap_update_bits(priv->regmap, WCD_USBSS_DN_BIAS, 0x01, 0x00);

		/* Disable SBU1/2 2K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_MG1_BIAS, 0x01, 0x00);
		regmap_update_bits(priv->regmap, WCD_USBSS_MG2_BIAS, 0x01, 0x00);
		/* USB Mode : Connect D+/D- switch */
		wcd_usbss_dpdm_switch_connect(priv, true);

		/* Exit from standby */
		wcd_usbss_standby_control_locked(false);
		break;
	case WCD_USBSS_USB_MODE_SET:
		/* Enable D+/D- 1M & 400K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_BIAS_TOP, 0x20, 0x00);
		/* Enable DP/DN 2K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_DP_BIAS, 0x01, 0x01);
		regmap_update_bits(priv->regmap, WCD_USBSS_DN_BIAS, 0x01, 0x01);

		/* Enable SBU1/2 2K PLDN */
		regmap_update_bits(priv->regmap, WCD_USBSS_MG1_BIAS, 0x01, 0x01);
		regmap_update_bits(priv->regmap, WCD_USBSS_MG2_BIAS, 0x01, 0x01);

		/* Connect D+/D- switch */
		wcd_usbss_dpdm_switch_connect(priv, true);

		/* Exit from standby */
		wcd_usbss_standby_control_locked(false);
		break;
	default:
		dev_err(priv->dev, "unexpected state:%d\n", req_state);
		rc = -EINVAL;
		break;
	}

	return rc;
}


static irqreturn_t wcd_usbss_sdam_notifier_handler(int irq, void *data)
{
	struct wcd_usbss_ctxt *priv = data;
	u8 *buf;
	size_t len = 0;
	int rc = 0;
	struct device *i2c_bus_dev = priv->client->adapter->dev.parent;
#if 0 //OPLUS_BUG_COMPATIBILITY
	bool disable_rpm = false;
#endif /* OPLUS_BUG_COMPATIBILITY */

//#ifdef OPLUS_ARCH_EXTENDS
/* Add for avoiding ADSP notify wcd to switch to standy mode in the ftm mode */
	if (boot_mode == MSM_BOOT_MODE__FACTORY) {
		dev_err(priv->dev, "wcd_usbss_sdam_notifier_handler boot_mode:%d, force return\n", boot_mode);
		return 0;
	}
//#endif /* OPLUS_ARCH_EXTENDS */

	buf = nvmem_cell_read(priv->nvmem_cell, &len);
	if (IS_ERR(buf)) {
		rc = PTR_ERR(buf);
		dev_err(priv->dev, "nvmem cell read failed, rc:%d\n", rc);
		return rc;
	}
	buf[0] &= 0x3;
//#ifdef OPLUS_ARCH_EXTENDS
	dev_info(priv->dev, "sdam notifier request:%d\n", buf[0]);
//#endif /* OPLUS_ARCH_EXTENDS */

#if 1 //OPLUS_BUG_COMPATIBILITY
	pm_stay_awake(wcd_usbss_ctxt_->dev);
	rc = wcd_usbss_pm_runtime_resume_and_wait(wcd_usbss_ctxt_->dev);
	if (rc < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
			__func__, rc);
		pm_relax(wcd_usbss_ctxt_->dev);
		kfree(buf);
		return rc;
	}
#endif /* OPLUS_BUG_COMPATIBILITY */
	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);
	wcd_usbss_ctxt_->req_state = buf[0];
	if (buf[0] == priv->wcd_standby_status) {
		dev_info(priv->dev, "%s: wcd already in %s mode:\n", __func__,
				status_to_str(priv->wcd_standby_status));
		mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
		goto exit;
	}

	if (wcd_usbss_ctxt_->suspended) {
		wcd_usbss_ctxt_->defer_writes = true;
		dev_dbg(priv->dev, "i2c in suspend, deferring %s transition to resume\n",
				status_to_str(wcd_usbss_ctxt_->req_state));
		mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
		goto exit;
	}

#if 0 //OPLUS_BUG_COMPATIBILITY
	if (!pm_runtime_enabled(i2c_bus_dev)) {
		pm_runtime_enable(i2c_bus_dev);
		disable_rpm = true;
	}
#else /* OPLUS_BUG_COMPATIBILITY */
	if (!wcd_usbss_wait_pm_runtime_enabled(i2c_bus_dev)) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: i2c bus in suspend, req_state %s\n",
				__func__, status_to_str(wcd_usbss_ctxt_->req_state));
		mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
		goto exit;
	}
#endif /* OPLUS_BUG_COMPATIBILITY */

	dev_dbg(priv->dev, "executing wcd state transition from %s to %s\n",
			status_to_str(priv->wcd_standby_status), status_to_str(buf[0]));

	rc = wcd_usbss_sdam_handle_events_locked(buf[0]);
	if (rc == 0) {
		priv->wcd_standby_status = buf[0];
//#ifdef OPLUS_ARCH_EXTENDS
		dev_info(priv->dev, "wcd state transition to %s complete\n",
				status_to_str(priv->wcd_standby_status));
//#endif /* OPLUS_ARCH_EXTENDS */
	}

#if 0 //OPLUS_BUG_COMPATIBILITY
	if (disable_rpm)
		pm_runtime_disable(i2c_bus_dev);
#endif /* OPLUS_BUG_COMPATIBILITY */

	mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
exit:
#if 1 //OPLUS_BUG_COMPATIBILITY
	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);
	pm_relax(wcd_usbss_ctxt_->dev);
#endif /* OPLUS_BUG_COMPATIBILITY */
	kfree(buf);
	return IRQ_HANDLED;
}

static int wcd_usbss_sdam_registration(struct wcd_usbss_ctxt *priv)
{
	int rc = 0;

	if (!priv)
		return -EINVAL;

	priv->wcd_standby_status = WCD_USBSS_USB_MODE_SET;
	priv->nvmem_cell = devm_nvmem_cell_get(priv->dev, "usb_mode");
	if (IS_ERR(priv->nvmem_cell)) {
		rc = PTR_ERR(priv->nvmem_cell);
		if (rc != -EPROBE_DEFER)
			dev_err(priv->dev, "nvmem cell get failed, rc:%d\n", rc);
		goto exit;
	}
	/* client->irq = of_get_irq( ); not required i2c_client->irq is populated */
	rc = devm_request_threaded_irq(priv->dev, priv->client->irq, NULL,
			wcd_usbss_sdam_notifier_handler, IRQF_ONESHOT,
			"wcd-usbss-sdam", priv);
	if (rc) {
		dev_err(priv->dev, "sdam registration failed, standby not supported, rc:%d\n",
				rc);
	} else {
		enable_irq_wake(priv->client->irq);
	}

exit:
	if (rc == 0)
		dev_info(priv->dev, "sdam registration successful\n");

	return rc;
}

static int wcd_usbss_probe(struct i2c_client *i2c)
{
	struct wcd_usbss_ctxt *priv;
	struct device *dev = &i2c->dev;
	int rc = 0, i;
	unsigned int ver = 0;

	priv = devm_kzalloc(&i2c->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &i2c->dev;
	priv->client = i2c;
	mutex_init(&priv->io_lock);
	mutex_init(&priv->switch_update_lock);
	i2c_set_clientdata(i2c, priv);
//#ifdef OPLUS_ARCH_EXTENDS
/* Add for avoiding ADSP notify wcd to switch to standy mode in the ftm mode */
	boot_mode = get_boot_mode();
	dev_err(priv->dev, "wcd_usbss_probe boot_mode: %d\n", boot_mode);
	if (boot_mode == MSM_BOOT_MODE__FACTORY)
		wcd_usbss_switch_update(WCD_USBSS_USB, WCD_USBSS_CABLE_CONNECT);
//#endif /* OPLUS_ARCH_EXTENDS */

	pm_runtime_enable(dev);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 600);
	rc = pm_runtime_resume_and_get(priv->dev);
	if (rc < 0) {
		dev_err(wcd_usbss_ctxt_->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, rc);
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
		mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
			"pm_runtime_resume_and_get failed: %i", rc);
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
		goto err_data;
	}

	if (ARRAY_SIZE(supply_names) >= WCD_USBSS_SUPPLY_MAX) {
		dev_err(priv->dev, "Unsupported number of supplies: %d\n",
				ARRAY_SIZE(supply_names));
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(supply_names); ++i)
		priv->supplies[i].supply = supply_names[i];

	rc = devm_regulator_bulk_get(priv->dev, ARRAY_SIZE(supply_names),
			priv->supplies);
	if (rc < 0) {
		dev_err(priv->dev, "Failed to get supplies: %d\n", rc);
		return rc;
	}

	rc = regulator_bulk_enable(ARRAY_SIZE(supply_names), priv->supplies);
	if (rc) {
		dev_err(priv->dev, "Failed to enable supplies: %d\n", rc);
		return rc;
	}

#ifdef OPLUS_FEATURE_CHG_BASIC
	device_property_read_u8(priv->dev, "qcom,wcd_usbss_equalizer1", &wcd_usbss_equalizer1);
	dev_err(priv->dev, "wcd_usbss_equalizer1 configuration is 0x%x\n", wcd_usbss_equalizer1);
#endif

	rc = wcd_usbss_init_optional_reset_pins(priv);
	if (rc) {
		dev_dbg(priv->dev, "%s: Optional reset pin reset failed\n",
				__func__);
		rc = 0;
	}
	priv->regmap = wcd_usbss_regmap_init(priv->dev, &wcd_usbss_regmap_config);
	if (IS_ERR_OR_NULL(priv->regmap)) {
		rc = PTR_ERR(priv->regmap);
		if (!priv->regmap)
			rc = -EINVAL;

		dev_err(priv->dev, "Failed to initialize regmap: %d\n", rc);
		goto err_data;
	}

	/* OVP-Fuse settings recommended from HW */
	regmap_update_bits(priv->regmap, WCD_USBSS_FSM_OVERRIDE, 0x77, 0x77);
	regmap_update_bits(priv->regmap, WCD_USBSS_DP_EN, 0x0E, 0x08);
	regmap_update_bits(priv->regmap, WCD_USBSS_DN_EN, 0x0E, 0x08);

	/* Display common mode and OVP 4V updates */
	regmap_update_bits(priv->regmap, WCD_USBSS_DISP_AUXP_CTL, 0x07, 0x01);
	regmap_update_bits(priv->regmap, WCD_USBSS_DISP_AUXP_THRESH, 0xE0, 0xE0);
	regmap_update_bits(priv->regmap, WCD_USBSS_DISP_AUXM_THRESH, 0xE0, 0xE0);
	regmap_update_bits(priv->regmap, WCD_USBSS_MG1_EN, 0x0C, 0x0C);
	regmap_update_bits(priv->regmap, WCD_USBSS_MG2_EN, 0x0C, 0x0C);

	regmap_read(priv->regmap, WCD_USBSS_CHIP_ID1, &ver);
	if (ver == 0x1) { /* Harmonium 2.0 */
		regmap_update_bits(priv->regmap, WCD_USBSS_MG1_EN, 0x2, 0x0);
		regmap_update_bits(priv->regmap, WCD_USBSS_MG2_EN, 0x2, 0x0);
	}
	priv->version = ver;

	devm_regmap_qti_debugfs_register(priv->dev, priv->regmap);

	wcd_usbss_ctxt_ = priv;

	i2c_set_clientdata(i2c, priv);

	rc = wcd_usbss_sdam_registration(priv);
	if (rc == 0)
		priv->standby_enable = true;
	else
		dev_info(priv->dev, "wcd standby feature not enabled\n");

	priv->ucsi_nb.notifier_call = wcd_usbss_usbc_event_changed;
	priv->ucsi_nb.priority = 0;
	rc = register_ucsi_glink_notifier(&priv->ucsi_nb);
	if (rc) {
		dev_err(priv->dev, "%s: ucsi glink notifier registration failed: %d\n",
			__func__, rc);
		goto err_data;
	}

	mutex_init(&priv->notification_lock);

	wcd_usbss_update_reg_init(priv->regmap);
	INIT_WORK(&priv->usbc_analog_work,
		  wcd_usbss_usbc_analog_work_fn);
	BLOCKING_INIT_NOTIFIER_HEAD(&priv->wcd_usbss_notifier);

	rc = wcd_usbss_sysfs_init(priv);
	if (rc == 0) {
		priv->surge_timer_period_ms = DEFAULT_SURGE_TIMER_PERIOD_MS;
		priv->surge_enable = true;
		wcd_usbss_enable_surge_kthread();
	}

//#ifdef OPLUS_ARCH_EXTENDS
/* Checking whether the surge occurs */
	priv->check_surge_workqueue = create_singlethread_workqueue("wcd_usbss_check_surge_work_fn");
	if (!priv->check_surge_workqueue) {
		dev_err(priv->dev, "Failed to create_singlethread_workqueue\n");
		goto err_data;
	}
	INIT_DELAYED_WORK(&priv->check_surge_delaywork, wcd_usbss_check_surge_work_fn);
//#endif /* OPLUS_ARCH_EXTENDS */
#if 1 //OPLUS_BUG_COMPATIBILITY
	device_init_wakeup(wcd_usbss_ctxt_->dev, true);
#endif /* OPLUS_BUG_COMPATIBILITY */

	pm_runtime_mark_last_busy(wcd_usbss_ctxt_->dev);
	pm_runtime_put_autosuspend(wcd_usbss_ctxt_->dev);
	dev_info(priv->dev, "Probe completed!\n");
	return 0;
err_data:
	pm_runtime_dont_use_autosuspend(wcd_usbss_ctxt_->dev);
	pm_runtime_disable(wcd_usbss_ctxt_->dev);
	return rc;
}

static void wcd_usbss_remove(struct i2c_client *i2c)
{
	int error;
	struct wcd_usbss_ctxt *priv =
			(struct wcd_usbss_ctxt *)i2c_get_clientdata(i2c);

	if (!priv)
		return;

	error = pm_runtime_resume_and_get(priv->dev);
	if (error < 0) {
		dev_err(priv->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
				__func__, error);
//#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
		mm_fb_audio_kevent_named(OPLUS_AUDIO_EVENTID_HEADSET_DET, MM_FB_KEY_RATELIMIT_5MIN, \
			"pm_runtime_resume_and_get failed: %i", error);
//#endif /*CONFIG_OPLUS_FEATURE_MM_FEEDBACK*/
	}

	wcd_usbss_disable_surge_kthread();
	unregister_ucsi_glink_notifier(&priv->ucsi_nb);
	cancel_work_sync(&priv->usbc_analog_work);
	pm_relax(priv->dev);
#if 1 //OPLUS_BUG_COMPATIBILITY
	device_init_wakeup(wcd_usbss_ctxt_->dev, false);
#endif /* OPLUS_BUG_COMPATIBILITY */
	mutex_destroy(&priv->notification_lock);
	mutex_destroy(&priv->io_lock);
	mutex_destroy(&priv->switch_update_lock);
	if (error >= 0)
		pm_runtime_put_sync(priv->dev);
	pm_runtime_dont_use_autosuspend(priv->dev);
	pm_runtime_disable(priv->dev);
	dev_set_drvdata(&i2c->dev, NULL);
	wcd_usbss_ctxt_ = NULL;

//#ifdef OPLUS_ARCH_EXTENDS
/* Checking whether the surge occurs */
	if (priv->check_surge_workqueue) {
		cancel_delayed_work_sync(&priv->check_surge_delaywork);
	}
//#endif /* OPLUS_ARCH_EXTENDS */
}

#ifdef CONFIG_PM_SLEEP
static int wcd_usbss_pm_suspend(struct device *dev)
{
	if (wcd_usbss_ctxt_)
		wcd_usbss_ctxt_->suspended = true;

	dev_dbg(wcd_usbss_ctxt_->dev, "wcd usbss pm suspended");
	return 0;
}

static int wcd_usbss_pm_resume(struct device *dev)
{
	int rc = 0;

	if (!wcd_usbss_ctxt_)
		return 0;

	mutex_lock(&wcd_usbss_ctxt_->switch_update_lock);
	if (wcd_usbss_ctxt_->defer_writes) {
		dev_dbg(wcd_usbss_ctxt_->dev, "wcd defer writes in progress");
		rc = wcd_usbss_sdam_handle_events_locked(wcd_usbss_ctxt_->req_state);
		wcd_usbss_ctxt_->defer_writes = false;
		if (rc == 0) {
			wcd_usbss_ctxt_->wcd_standby_status = wcd_usbss_ctxt_->req_state;
			dev_dbg(wcd_usbss_ctxt_->dev, "wcd state transition to %s complete\n",
					status_to_str(wcd_usbss_ctxt_->wcd_standby_status));
		}
	}
	mutex_unlock(&wcd_usbss_ctxt_->switch_update_lock);
	wcd_usbss_ctxt_->suspended = false;
	dev_dbg(wcd_usbss_ctxt_->dev, "wcd usbss pm resume completed");
	return 0;
}
#endif

static const struct of_device_id wcd_usbss_i2c_dt_match[] = {
	{
		.compatible = "qcom,wcd939x-i2c",
	},
	{}
};
MODULE_DEVICE_TABLE(of, wcd_usbss_i2c_dt_match);

static const struct i2c_device_id wcd_usbss_id_i2c[] = {
	{ "wcd939x", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, wcd_usbss_id_i2c);

static const struct dev_pm_ops wcd_usbss_pm_ops = {
	.suspend_late = wcd_usbss_pm_suspend,
	.resume_early = wcd_usbss_pm_resume,
};

static struct i2c_driver wcd_usbss_i2c_driver = {
	.driver = {
		.name = WCD_USBSS_I2C_NAME,
		.of_match_table = wcd_usbss_i2c_dt_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
#ifdef CONFIG_PM_SLEEP
		.pm = &wcd_usbss_pm_ops,
#endif
	},
	.id_table = wcd_usbss_id_i2c,
	.probe_new = wcd_usbss_probe,
	.remove = wcd_usbss_remove,
};
module_i2c_driver(wcd_usbss_i2c_driver);

MODULE_DESCRIPTION("WCD USBSS I2C driver");
MODULE_LICENSE("GPL");
