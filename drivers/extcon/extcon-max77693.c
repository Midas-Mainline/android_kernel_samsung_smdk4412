// SPDX-License-Identifier: GPL-2.0+
//
// extcon-max77693.c - MAX77693 extcon driver to support MAX77693 MUIC
//
// Copyright (C) 2012 Samsung Electrnoics
// Chanwoo Choi <cw00.choi@samsung.com>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/mfd/max77693.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77693-private.h>
#include <linux/extcon-provider.h>
#include <linux/regmap.h>
#include <linux/irqdomain.h>
#include <linux/delay.h>

#define	DEV_NAME			"max77693-muic"
#define	DELAY_MS_DEFAULT		20000		/* unit: millisecond */

struct max77693_muic_info *gInfo;

extern int midas_enable_host_mode(int new_state);

/*
 * Default value of MAX77693 register to bring up MUIC device.
 * If user don't set some initial value for MUIC device through platform data,
 * extcon-max77693 driver use 'default_init_data' to bring up base operation
 * of MAX77693 MUIC device.
 */
static struct max77693_reg_data default_init_data[] = {
	{
		/* STATUS2 - [3]ChgDetRun */
		.addr = MAX77693_MUIC_REG_STATUS2,
		.data = MAX77693_STATUS2_CHGDETRUN_MASK,
	}, {
		/* INTMASK1 - Unmask [3]ADC1KM,[0]ADCM */
		.addr = MAX77693_MUIC_REG_INTMASK1,
		.data = INTMASK1_ADC1K_MASK
			| INTMASK1_ADC_MASK,
	}, {
		/* INTMASK2 - Unmask [0]ChgTypM */
		.addr = MAX77693_MUIC_REG_INTMASK2,
		.data = INTMASK2_CHGTYP_MASK,
	}, {
		/* INTMASK3 - Mask all of interrupts */
		.addr = MAX77693_MUIC_REG_INTMASK3,
		.data = 0x0,
	}, {
		/* CDETCTRL2 */
		.addr = MAX77693_MUIC_REG_CDETCTRL2,
		.data = CDETCTRL2_VIDRMEN_MASK
			| CDETCTRL2_DXOVPEN_MASK,
	},
};

enum max77693_muic_adc_debounce_time {
	ADC_DEBOUNCE_TIME_5MS = 0,
	ADC_DEBOUNCE_TIME_10MS,
	ADC_DEBOUNCE_TIME_25MS,
	ADC_DEBOUNCE_TIME_38_62MS,
};

struct max77693_muic_info {
	struct device *dev;
	struct max77693_dev *max77693;
	struct extcon_dev *edev;
	int prev_cable_type;
	int prev_cable_type_gnd;
	int prev_chg_type;
	int prev_button_type;
	u8 status[2];

	int irq;
	struct work_struct irq_work;
	struct mutex mutex;

	/*
	 * Use delayed workqueue to detect cable state and then
	 * notify cable state to notifiee/platform through uevent.
	 * After completing the booting of platform, the extcon provider
	 * driver should notify cable state to upper layer.
	 */
	struct delayed_work wq_detcable;

	/* Button of dock device */
	struct input_dev *dock;

	/*
	 * Default usb/uart path whether UART/USB or AUX_UART/AUX_USB
	 * h/w path of COMP2/COMN1 on CONTROL1 register.
	 */
	int path_usb;
	int path_uart;
};

enum max77693_muic_cable_group {
	MAX77693_CABLE_GROUP_ADC = 0,
	MAX77693_CABLE_GROUP_ADC_GND,
	MAX77693_CABLE_GROUP_CHG,
	MAX77693_CABLE_GROUP_VBVOLT,
};

enum max77693_muic_charger_type {
	MAX77693_CHARGER_TYPE_NONE = 0,
	MAX77693_CHARGER_TYPE_USB,
	MAX77693_CHARGER_TYPE_DOWNSTREAM_PORT,
	MAX77693_CHARGER_TYPE_DEDICATED_CHG,
	MAX77693_CHARGER_TYPE_APPLE_500MA,
	MAX77693_CHARGER_TYPE_APPLE_1A_2A,
	MAX77693_CHARGER_TYPE_DEAD_BATTERY = 7,
};

/**
 * struct max77693_muic_irq
 * @irq: the index of irq list of MUIC device.
 * @name: the name of irq.
 * @virq: the virtual irq to use irq domain
 */
struct max77693_muic_irq {
	unsigned int irq;
	const char *name;
	unsigned int virq;
};

static struct max77693_muic_irq muic_irqs[] = {
	{ MAX77693_MUIC_IRQ_INT1_ADC,		"muic-ADC" },
	{ MAX77693_MUIC_IRQ_INT1_ADC_LOW,	"muic-ADCLOW" },
	{ MAX77693_MUIC_IRQ_INT1_ADC_ERR,	"muic-ADCError" },
	{ MAX77693_MUIC_IRQ_INT1_ADC1K,		"muic-ADC1K" },
	{ MAX77693_MUIC_IRQ_INT2_CHGTYP,	"muic-CHGTYP" },
	{ MAX77693_MUIC_IRQ_INT2_CHGDETREUN,	"muic-CHGDETREUN" },
	{ MAX77693_MUIC_IRQ_INT2_DCDTMR,	"muic-DCDTMR" },
	{ MAX77693_MUIC_IRQ_INT2_DXOVP,		"muic-DXOVP" },
	{ MAX77693_MUIC_IRQ_INT2_VBVOLT,	"muic-VBVOLT" },
	{ MAX77693_MUIC_IRQ_INT2_VIDRM,		"muic-VIDRM" },
	{ MAX77693_MUIC_IRQ_INT3_EOC,		"muic-EOC" },
	{ MAX77693_MUIC_IRQ_INT3_CGMBC,		"muic-CGMBC" },
	{ MAX77693_MUIC_IRQ_INT3_OVP,		"muic-OVP" },
	{ MAX77693_MUIC_IRQ_INT3_MBCCHG_ERR,	"muic-MBCCHG_ERR" },
	{ MAX77693_MUIC_IRQ_INT3_CHG_ENABLED,	"muic-CHG_ENABLED" },
	{ MAX77693_MUIC_IRQ_INT3_BAT_DET,	"muic-BAT_DET" },
};

/* Define supported accessory type */
enum max77693_muic_acc_type {
	MAX77693_MUIC_ADC_GROUND = 0x0,
	MAX77693_MUIC_ADC_MHL,
	MAX77693_MUIC_ADC_REMOTE_S1_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S2_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S3_BUTTON, // ADC_DOCK_PREV_KEY
	MAX77693_MUIC_ADC_REMOTE_S4_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S5_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S6_BUTTON, // ADC_DOCK_NEXT_KEY
	MAX77693_MUIC_ADC_REMOTE_S7_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S8_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S9_BUTTON, // ADC_DOCK_VOL_DN 0x01010 14.46K ohm
	MAX77693_MUIC_ADC_REMOTE_S10_BUTTON, // ADC_DOCK_VOL_UP 0x01011 17.26K ohm
	MAX77693_MUIC_ADC_REMOTE_S11_BUTTON,
	MAX77693_MUIC_ADC_REMOTE_S12_BUTTON, // ADC_DOCK_PLAY_PAUSE_KEY 0xd
	MAX77693_MUIC_ADC_RESERVED_ACC_1, // 0xe
	MAX77693_MUIC_ADC_RESERVED_ACC_2, // 0xf
	MAX77693_MUIC_ADC_SMARTDOCK, /* SmartDock 0x10000 40.2K ohm */ // 0x10
	MAX77693_MUIC_ADC_RESERVED_ACC_4, // 0x11
	MAX77693_MUIC_ADC_AUDIODOCK, /* AudioDock 0x10010 64.9K ohm */ // 0x12
	MAX77693_MUIC_ADC_CEA936_AUDIO, // 0x13
	MAX77693_MUIC_ADC_PHONE_POWERED_DEV, // 0x14
	MAX77693_MUIC_ADC_TTY_CONVERTER, // 0x15
	MAX77693_MUIC_ADC_UART_CABLE, // 0x16
	MAX77693_MUIC_ADC_CEA936A_TYPE1_CHG, // 0x17
	MAX77693_MUIC_ADC_FACTORY_MODE_USB_OFF, // 0x18
	MAX77693_MUIC_ADC_FACTORY_MODE_USB_ON, // 0x19
	MAX77693_MUIC_ADC_AV_CABLE_NOLOAD, // 0x1a
	MAX77693_MUIC_ADC_CEA936A_TYPE2_CHG, // 0x1b
	MAX77693_MUIC_ADC_FACTORY_MODE_UART_OFF, // 0x1c
	MAX77693_MUIC_ADC_FACTORY_MODE_UART_ON, // 0x1d
	MAX77693_MUIC_ADC_AUDIO_MODE_REMOTE, // 0x1e
	MAX77693_MUIC_ADC_OPEN, // 0x1f

	/*
	 * The below accessories have same ADC value so ADCLow and
	 * ADC1K bit is used to separate specific accessory.
	 */
						/* ADC|VBVolot|ADCLow|ADC1K| */
	MAX77693_MUIC_GND_USB_HOST = 0x100,	/* 0x0|      0|     0|    0| */
	MAX77693_MUIC_GND_USB_HOST_VB = 0x104,	/* 0x0|      1|     0|    0| */
	MAX77693_MUIC_GND_AV_CABLE_LOAD = 0x102,/* 0x0|      0|     1|    0| */
	MAX77693_MUIC_GND_MHL = 0x103,		/* 0x0|      0|     1|    1| */
	MAX77693_MUIC_GND_MHL_VB = 0x107,	/* 0x0|      1|     1|    1| */
};

/*
 * MAX77693 MUIC device support below list of accessories(external connector)
 */
static const unsigned int max77693_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_FAST,
	EXTCON_CHG_USB_SLOW,
	EXTCON_CHG_USB_CDP,
	EXTCON_DISP_MHL,
	EXTCON_JIG,
	EXTCON_DOCK,
	EXTCON_NONE,
};

/*
 * max77693_muic_set_debounce_time - Set the debounce time of ADC
 * @info: the instance including private data of max77693 MUIC
 * @time: the debounce time of ADC
 */
static int max77693_muic_set_debounce_time(struct max77693_muic_info *info,
		enum max77693_muic_adc_debounce_time time)
{
	int ret;

	switch (time) {
	case ADC_DEBOUNCE_TIME_5MS:
	case ADC_DEBOUNCE_TIME_10MS:
	case ADC_DEBOUNCE_TIME_25MS:
	case ADC_DEBOUNCE_TIME_38_62MS:
		/*
		 * Don't touch BTLDset, JIGset when you want to change adc
		 * debounce time. If it writes other than 0 to BTLDset, JIGset
		 * muic device will be reset and loose current state.
		 */
		ret = regmap_write(info->max77693->regmap_muic,
				  MAX77693_MUIC_REG_CTRL3,
				  time << MAX77693_CONTROL3_ADCDBSET_SHIFT);
		if (ret) {
			dev_err(info->dev, "failed to set ADC debounce time\n");
			return ret;
		}
		break;
	default:
		dev_err(info->dev, "invalid ADC debounce time\n");
		return -EINVAL;
	}

	return 0;
};

/*
 * max77693_muic_set_path - Set hardware line according to attached cable
 * @info: the instance including private data of max77693 MUIC
 * @value: the path according to attached cable
 * @attached: the state of cable (true:attached, false:detached)
 *
 * The max77693 MUIC device share outside H/W line among a varity of cables
 * so, this function set internal path of H/W line according to the type of
 * attached cable.
 */
static int max77693_muic_set_path(struct max77693_muic_info *info,
		u8 val, bool attached)
{
	int ret;
	unsigned int ctrl1, ctrl2 = 0;

	if (attached)
		ctrl1 = val;
	else
		ctrl1 = MAX77693_CONTROL1_SW_OPEN;

	ret = regmap_update_bits(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_CTRL1, COMP_SW_MASK, ctrl1);
	if (ret < 0) {
		dev_err(info->dev, "failed to update MUIC register\n");
		return ret;
	}

	if (attached)
		ctrl2 |= MAX77693_CONTROL2_CPEN_MASK;	/* LowPwr=0, CPEn=1 */
	else
		ctrl2 |= MAX77693_CONTROL2_LOWPWR_MASK;	/* LowPwr=1, CPEn=0 */

	ret = regmap_update_bits(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_CTRL2,
			MAX77693_CONTROL2_LOWPWR_MASK | MAX77693_CONTROL2_CPEN_MASK,
			ctrl2);
	if (ret < 0) {
		dev_err(info->dev, "failed to update MUIC register\n");
		return ret;
	}

	dev_info(info->dev,
		"CONTROL1 : 0x%02x, CONTROL2 : 0x%02x, state : %s\n",
		ctrl1, ctrl2, attached ? "attached" : "detached");

	return 0;
}

/*
 * max77693_muic_get_cable_type - Return cable type and check cable state
 * @info: the instance including private data of max77693 MUIC
 * @group: the path according to attached cable
 * @attached: store cable state and return
 *
 * This function check the cable state either attached or detached,
 * and then divide precise type of cable according to cable group.
 *	- MAX77693_CABLE_GROUP_ADC
 *	- MAX77693_CABLE_GROUP_ADC_GND
 *	- MAX77693_CABLE_GROUP_CHG
 *	- MAX77693_CABLE_GROUP_VBVOLT
 */
static int max77693_muic_get_cable_type(struct max77693_muic_info *info,
		enum max77693_muic_cable_group group, bool *attached)
{
	int cable_type = 0;
	int adc;
	int adc1k;
	int adclow;
	int vbvolt;
	int chg_type;

	switch (group) {
	case MAX77693_CABLE_GROUP_ADC:
		/*
		 * Read ADC value to check cable type and decide cable state
		 * according to cable type
		 */
		adc = info->status[0] & MAX77693_STATUS1_ADC_MASK;
		adc >>= MAX77693_STATUS1_ADC_SHIFT;

		/*
		 * Check current cable state/cable type and store cable type
		 * (info->prev_cable_type) for handling cable when cable is
		 * detached.
		 */
		if (adc == MAX77693_MUIC_ADC_OPEN) {
			*attached = false;

			cable_type = info->prev_cable_type;
			info->prev_cable_type = MAX77693_MUIC_ADC_OPEN;
		} else {
			*attached = true;

			cable_type = info->prev_cable_type = adc;
		}
		break;
	case MAX77693_CABLE_GROUP_ADC_GND:
		/*
		 * Read ADC value to check cable type and decide cable state
		 * according to cable type
		 */
		adc = info->status[0] & MAX77693_STATUS1_ADC_MASK;
		adc >>= MAX77693_STATUS1_ADC_SHIFT;

		/*
		 * Check current cable state/cable type and store cable type
		 * (info->prev_cable_type/_gnd) for handling cable when cable
		 * is detached.
		 */
		if (adc == MAX77693_MUIC_ADC_OPEN) {
			*attached = false;

			cable_type = info->prev_cable_type_gnd;
			info->prev_cable_type_gnd = MAX77693_MUIC_ADC_OPEN;
		} else {
			*attached = true;

			adclow = info->status[0] & MAX77693_STATUS1_ADCLOW_MASK;
			adclow >>= MAX77693_STATUS1_ADCLOW_SHIFT;
			adc1k = info->status[0] & MAX77693_STATUS1_ADC1K_MASK;
			adc1k >>= MAX77693_STATUS1_ADC1K_SHIFT;

			vbvolt = info->status[1] & MAX77693_STATUS2_VBVOLT_MASK;
			vbvolt >>= MAX77693_STATUS2_VBVOLT_SHIFT;

			/**
			 * [0x1|VBVolt|ADCLow|ADC1K]
			 * [0x1|     0|     0|    0] USB_HOST
			 * [0x1|     1|     0|    0] USB_HSOT_VB
			 * [0x1|     0|     1|    0] Audio Video cable with load
			 * [0x1|     0|     1|    1] MHL without charging cable
			 * [0x1|     1|     1|    1] MHL with charging cable
			 */
			cable_type = ((0x1 << 8)
					| (vbvolt << 2)
					| (adclow << 1)
					| adc1k);

			info->prev_cable_type = adc;
			info->prev_cable_type_gnd = cable_type;
		}

		break;
	case MAX77693_CABLE_GROUP_CHG:
		/*
		 * Read charger type to check cable type and decide cable state
		 * according to type of charger cable.
		 */
		chg_type = info->status[1] & MAX77693_STATUS2_CHGTYP_MASK;
		chg_type >>= MAX77693_STATUS2_CHGTYP_SHIFT;

		if (chg_type == MAX77693_CHARGER_TYPE_NONE) {
			*attached = false;

			cable_type = info->prev_chg_type;
			info->prev_chg_type = MAX77693_CHARGER_TYPE_NONE;
		} else {
			*attached = true;

			/*
			 * Check current cable state/cable type and store cable
			 * type(info->prev_chg_type) for handling cable when
			 * charger cable is detached.
			 */
			cable_type = info->prev_chg_type = chg_type;
		}

		break;
	case MAX77693_CABLE_GROUP_VBVOLT:
		/*
		 * Read ADC value to check cable type and decide cable state
		 * according to cable type
		 */
		adc = info->status[0] & MAX77693_STATUS1_ADC_MASK;
		adc >>= MAX77693_STATUS1_ADC_SHIFT;
		chg_type = info->status[1] & MAX77693_STATUS2_CHGTYP_MASK;
		chg_type >>= MAX77693_STATUS2_CHGTYP_SHIFT;

		if (adc == MAX77693_MUIC_ADC_OPEN
				&& chg_type == MAX77693_CHARGER_TYPE_NONE)
			*attached = false;
		else
			*attached = true;

		/*
		 * Read vbvolt field, if vbvolt is 1,
		 * this cable is used for charging.
		 */
		vbvolt = info->status[1] & MAX77693_STATUS2_VBVOLT_MASK;
		vbvolt >>= MAX77693_STATUS2_VBVOLT_SHIFT;

		cable_type = vbvolt;
		break;
	default:
		dev_err(info->dev, "Unknown cable group (%d)\n", group);
		cable_type = -EINVAL;
		break;
	}

	return cable_type;
}

static int max77693_muic_dock_handler(struct max77693_muic_info *info,
		int cable_type, bool attached)
{
	int ret = 0;
	int vbvolt;
	bool cable_attached;
	unsigned int dock_id;

	dev_info(info->dev,
		"%s: external connector is %s (adc:0x%02x)\n",
		attached ? "attached" : "detached", __func__, cable_type);

	switch (cable_type) {
	case MAX77693_MUIC_ADC_SMARTDOCK:		/* Dock-Smart */
		/*
		 * Check power cable whether attached or detached state.
		 * The Dock-Smart device need surely external power supply.
		 * If power cable(USB/TA) isn't connected to Dock device,
		 * user can't use Dock-Smart for desktop mode.
		 */
		vbvolt = max77693_muic_get_cable_type(info,
				MAX77693_CABLE_GROUP_VBVOLT, &cable_attached);
		if (attached && !vbvolt) {
			dev_warn(info->dev,
				"Cannot detect external power supply\n");
			return 0;
		}

		/*
		 * Notify Dock/MHL state.
		 * - Dock device include three type of cable which
		 * are HDMI, USB for mouse/keyboard and micro-usb port
		 * for USB/TA cable. Dock device need always exteranl
		 * power supply(USB/TA cable through micro-usb cable). Dock
		 * device support screen output of target to separate
		 * monitor and mouse/keyboard for desktop mode.
		 *
		 * Features of 'USB/TA cable with Dock device'
		 * - Support MHL
		 * - Support external output feature of audio
		 * - Support charging through micro-usb port without data
		 *	     connection if TA cable is connected to target.
		 * - Support charging and data connection through micro-usb port
		 *           if USB cable is connected between target and host
		 *	     device.
		 * - Support OTG(On-The-Go) device (Ex: Mouse/Keyboard)
		 */
		ret = max77693_muic_set_path(info, info->path_usb, attached);
		if (ret < 0)
			return ret;

		extcon_set_state_sync(info->edev, EXTCON_DOCK, attached);
		extcon_set_state_sync(info->edev, EXTCON_DISP_MHL, attached);
		goto out;
	case MAX77693_MUIC_ADC_AUDIO_MODE_REMOTE:	/* Dock-Desk */
		dock_id = EXTCON_DOCK;
		break;
	case MAX77693_MUIC_ADC_AV_CABLE_NOLOAD:		/* Dock-Audio */
		dock_id = EXTCON_DOCK;
		if (!attached) {
			extcon_set_state_sync(info->edev, EXTCON_USB, false);
			extcon_set_state_sync(info->edev, EXTCON_CHG_USB_SDP,
						false);
		}
		break;
	default:
		dev_err(info->dev, "failed to detect %s dock device\n",
			attached ? "attached" : "detached");
		return -EINVAL;
	}

	/* Dock-Car/Desk/Audio, PATH:AUDIO */
	ret = max77693_muic_set_path(info, MAX77693_CONTROL1_SW_AUDIO,
					attached);
	if (ret < 0)
		return ret;
	extcon_set_state_sync(info->edev, dock_id, attached);

out:
	return 0;
}

static int max77693_muic_dock_button_handler(struct max77693_muic_info *info,
		int button_type, bool attached)
{
	struct input_dev *dock = info->dock;
	unsigned int code;

	switch (button_type) {
	case MAX77693_MUIC_ADC_REMOTE_S3_BUTTON-1
		... MAX77693_MUIC_ADC_REMOTE_S3_BUTTON+1:
		/* DOCK_KEY_PREV */
		code = KEY_PREVIOUSSONG;
		break;
	case MAX77693_MUIC_ADC_REMOTE_S7_BUTTON-1
		... MAX77693_MUIC_ADC_REMOTE_S7_BUTTON+1:
		/* DOCK_KEY_NEXT */
		code = KEY_NEXTSONG;
		break;
	case MAX77693_MUIC_ADC_REMOTE_S9_BUTTON:
		/* DOCK_VOL_DOWN */
		code = KEY_VOLUMEDOWN;
		break;
	case MAX77693_MUIC_ADC_REMOTE_S10_BUTTON:
		/* DOCK_VOL_UP */
		code = KEY_VOLUMEUP;
		break;
	case MAX77693_MUIC_ADC_REMOTE_S12_BUTTON-1
		... MAX77693_MUIC_ADC_REMOTE_S12_BUTTON+1:
		/* DOCK_KEY_PLAY_PAUSE */
		code = KEY_PLAYPAUSE;
		break;
	default:
		dev_err(info->dev,
			"failed to detect %s key (adc:0x%x)\n",
			attached ? "pressed" : "released", button_type);
		return -EINVAL;
	}

	input_event(dock, EV_KEY, code, attached);
	input_sync(dock);

	return 0;
}

unsigned int max77693_chg_int_mask = 0;
unsigned int max77693_cdetctrl1 = 0;
unsigned int max77693_chg_cnf00 = 0;

unsigned int max77693_ctrl1 = 0;
unsigned int max77693_ctrl2 = 0;

void max77693_read_regs(void)
{
	regmap_read(gInfo->max77693->regmap,
                        MAX77693_CHG_REG_CHG_INT_MASK, &max77693_chg_int_mask);
	regmap_read(gInfo->max77693->regmap_muic,
			MAX77693_MUIC_REG_CDETCTRL1, &max77693_cdetctrl1);
	regmap_read(gInfo->max77693->regmap,
			MAX77693_CHG_REG_CHG_CNFG_00, &max77693_chg_cnf00);

	regmap_read(gInfo->max77693->regmap_muic,
			MAX77693_MUIC_REG_CTRL1, &max77693_ctrl1);
	regmap_read(gInfo->max77693->regmap_muic,
			MAX77693_MUIC_REG_CTRL2, &max77693_ctrl2);

}

void max77693_otg_control(struct max77693_muic_info *info, int enable)
{
	int chg_int_state;
	int int_mask, cdetctrl1, chg_cnfg_00;
	dev_dbg(info->dev, "%s: enable(%d)\n", __func__, enable);

	if (enable) {
		/* disable charger interrupt */
		regmap_read(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_INT_MASK, &int_mask);
		chg_int_state = int_mask;
		int_mask |= (1 << 4);	/* disable chgin intr */
		int_mask |= (1 << 6);	/* disable chg */
		int_mask &= ~(1 << 0);	/* enable byp intr */
		regmap_write(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_INT_MASK, int_mask);

		/* disable charger detection */
		regmap_read(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_CDETCTRL1, &cdetctrl1);
		cdetctrl1 &= ~(1 << 0);
		regmap_write(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_CDETCTRL1, cdetctrl1);

		/* OTG on, boost on, DIS_MUIC_CTRL=1 */
		regmap_read(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);
		chg_cnfg_00 &= ~(CHG_CNFG_00_CHG_MASK
				| CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BUCK_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK);
		chg_cnfg_00 |= (CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK);
		regmap_write(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_CNFG_00, chg_cnfg_00);
	} else {
		/* OTG off, boost off, (buck on),
		   DIS_MUIC_CTRL = 0 unless CHG_ENA = 1 */
		regmap_read(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);
		chg_cnfg_00 &= ~(CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK);
		chg_cnfg_00 |= CHG_CNFG_00_BUCK_MASK;
		regmap_write(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_CNFG_00, chg_cnfg_00);

		msleep(50);

		/* enable charger detection */
		regmap_read(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_CDETCTRL1, &cdetctrl1);
		cdetctrl1 |= (1 << 0);
		regmap_write(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_CDETCTRL1, cdetctrl1);

		/* enable charger interrupt */
		regmap_write(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_INT_MASK, chg_int_state);
		regmap_read(info->max77693->regmap,
			MAX77693_CHG_REG_CHG_INT_MASK, &int_mask);
	}

	dev_dbg(info->dev, "%s: INT_MASK(0x%x), CDETCTRL1(0x%x), CHG_CNFG_00(0x%x)\n",
				__func__, int_mask, cdetctrl1, chg_cnfg_00);
}

#if 0

static int max77693_muic_set_usb_path(struct max77693_muic_info *info, int path)
{
	int ret;
	int gpio_val;
	u8 cntl1_val, cntl1_msk;
	int val;
	pr_err("%s path:%d\n", __func__, path);
	/*if (mdata->set_safeout) {
		ret = mdata->set_safeout(path);
		if (ret) {
			dev_err(info->dev, "%s: fail to set safout!\n",
				__func__);
			return ret;
		}
	}*/
	switch (path) {
	case AP_USB_MODE:
		dev_info(info->dev, "%s: AP_USB_MODE\n", __func__);
		gpio_val = 0;
		val = MAX77693_MUIC_CTRL1_BIN_1_001;
		cntl1_val = (val << COMN1SW_SHIFT) | (val << COMP2SW_SHIFT);
		cntl1_msk = COMN1SW_MASK | COMP2SW_MASK;
		break;
#if 0
	case CP_USB_MODE:
		dev_info(info->dev, "%s: CP_USB_MODE\n", __func__);
		gpio_val = 1;
		if (info->max77693->pmic_rev >= MAX77693_REV_PASS2)
			val = MAX77693_MUIC_CTRL1_BIN_4_100;
		else
			val = MAX77693_MUIC_CTRL1_BIN_3_011;
		cntl1_val = (val << COMN1SW_SHIFT) | (val << COMP2SW_SHIFT);
		cntl1_msk = COMN1SW_MASK | COMP2SW_MASK;
		break;
	case AUDIO_MODE:
		dev_info(info->dev, "%s: AUDIO_MODE\n", __func__);
		gpio_val = 0;
		/* SL1, SR2 */
		cntl1_val = (MAX77693_MUIC_CTRL1_BIN_2_010 << COMN1SW_SHIFT)
			| (MAX77693_MUIC_CTRL1_BIN_2_010 << COMP2SW_SHIFT) |
			(0 << MICEN_SHIFT);
		cntl1_msk = COMN1SW_MASK | COMP2SW_MASK | MICEN_MASK;
		break;
	case OPEN_USB_MODE:
		dev_info(info->dev, "%s: OPEN_USB_MODE\n", __func__);
		gpio_val = 0;
		val = MAX77693_MUIC_CTRL1_BIN_0_000;
		cntl1_val = (val << COMN1SW_SHIFT) | (val << COMP2SW_SHIFT);
		cntl1_msk = COMN1SW_MASK | COMP2SW_MASK;
		break;
#endif
	default:
		dev_warn(info->dev, "%s: invalid path(%d)\n", __func__, path);
		return -EINVAL;
	}

#if 0
	if (info->max77693->pmic_rev < MAX77693_REV_PASS2) {
		pr_err("%s: set gpio_usb_sel: %d\n", __func__, gpio_val);
		if (gpio_is_valid(info->muic_data->gpio_usb_sel))
			gpio_direction_output(mdata->gpio_usb_sel, gpio_val);
	}
#endif

	pr_err("%s: Set manual path\n", __func__);
#if 0
	if (info->cable_type != CABLE_TYPE_CARDOCK_MUIC
		&& info->cable_type != CABLE_TYPE_DESKDOCK_MUIC)
#endif

	/*max77693_update_reg(client, MAX77693_MUIC_REG_CTRL1, cntl1_val,
				cntl1_msk);*/

	regmap_write(info->max77693->regmap_muic,
				MAX77693_MUIC_REG_CTRL2,
				CTRL2_CPEn1_LOWPWD0,
				CTRL2_CPEn_MASK | CTRL2_LOWPWD_MASK);

	cntl1_val = MAX77693_MUIC_CTRL1_BIN_0_000;
	regmap_read(info->max77693->regmap_muic, MAX77693_MUIC_REG_CTRL1, &cntl1_val);
	pr_err("%s: CNTL1(0x%02x)\n", __func__, cntl1_val);

	cntl1_val = MAX77693_MUIC_CTRL1_BIN_0_000;
	regmap_read(info->max77693->regmap_muic, MAX77693_MUIC_REG_CTRL2, &cntl1_val);
	pr_err("%s: CNTL2(0x%02x)\n", __func__, cntl1_val);

	//sysfs_notify(&switch_dev->kobj, NULL, "usb_sel");
	return 0;
}

#endif

void otg_control(int enable)
{
	if (gInfo)
		max77693_otg_control(gInfo, enable);
}

void set_usb_path(/*u8 val, */int attached)
{
	if (gInfo)
		max77693_muic_set_path(gInfo, MAX77693_CONTROL1_SW_USB /*val*/, attached);
}

static int max77693_muic_adc_ground_handler(struct max77693_muic_info *info)
{
	int cable_type_gnd;
	int ret = 0;
	bool attached;

	cable_type_gnd = max77693_muic_get_cable_type(info,
				MAX77693_CABLE_GROUP_ADC_GND, &attached);

	pr_err("%s: cable_type_gnd = %d", __func__, cable_type_gnd);

	switch (cable_type_gnd) {
	case MAX77693_MUIC_GND_USB_HOST:
	case MAX77693_MUIC_GND_USB_HOST_VB:
		/* USB_HOST, PATH: AP_USB */
		//midas_enable_host_mode(attached);
		if (ret < 0)
			return ret;
		/*if (attached)
			extcon_set_state_sync(info->edev, EXTCON_USB_HOST, attached);
		else
			extcon_set_state_sync(info->edev, EXTCON_CHG_USB_SDP,
						!attached);*/
			//max77693_otg_control(info, attached);
		break;
	case MAX77693_MUIC_GND_AV_CABLE_LOAD:
		/* Audio Video Cable with load, PATH:AUDIO */
		ret = max77693_muic_set_path(info, MAX77693_CONTROL1_SW_AUDIO,
						attached);
		if (ret < 0)
			return ret;
		extcon_set_state_sync(info->edev, EXTCON_USB, attached);
		extcon_set_state_sync(info->edev, EXTCON_CHG_USB_SDP,
					attached);
		break;
	case MAX77693_MUIC_GND_MHL:
	case MAX77693_MUIC_GND_MHL_VB:
		/* MHL or MHL with USB/TA cable */
		extcon_set_state_sync(info->edev, EXTCON_DISP_MHL, attached);
		break;
	default:
		dev_err(info->dev, "failed to detect %s cable of gnd type\n",
			attached ? "attached" : "detached");
		return -EINVAL;
	}

	return 0;
}

static int max77693_muic_jig_handler(struct max77693_muic_info *info,
		int cable_type, bool attached)
{
	int ret = 0;
	u8 path = MAX77693_CONTROL1_SW_OPEN;

	dev_info(info->dev,
		"external connector is %s (adc:0x%02x)\n",
		attached ? "attached" : "detached", cable_type);

	switch (cable_type) {
	case MAX77693_MUIC_ADC_FACTORY_MODE_USB_OFF:	/* ADC_JIG_USB_OFF */
	case MAX77693_MUIC_ADC_FACTORY_MODE_USB_ON:	/* ADC_JIG_USB_ON */
		/* PATH:AP_USB */
		path = MAX77693_CONTROL1_SW_USB;
		break;
	case MAX77693_MUIC_ADC_FACTORY_MODE_UART_OFF:	/* ADC_JIG_UART_OFF */
	case MAX77693_MUIC_ADC_FACTORY_MODE_UART_ON:	/* ADC_JIG_UART_ON */
		/* PATH:AP_UART */
		path = MAX77693_CONTROL1_SW_UART;
		break;
	default:
		dev_err(info->dev, "failed to detect %s jig cable\n",
			attached ? "attached" : "detached");
		return -EINVAL;
	}

	ret = max77693_muic_set_path(info, path, attached);
	if (ret < 0)
		return ret;

	extcon_set_state_sync(info->edev, EXTCON_JIG, attached);

	return 0;
}

static bool max77693_muic_filter_dev(struct max77693_muic_info *info,
					u8 status1, u8 status2)
{
	u8 adc, adclow, adcerr, adc1k, chgtyp, vbvolt, dxovp;
	//int intr = INT_ATTACH;
	bool attached = true;

	adc = status1 & MAX77693_STATUS1_ADC_MASK;
	adclow = status1 & MAX77693_STATUS1_ADCLOW_MASK;
	adcerr = status1 & MAX77693_STATUS1_ADCERR_MASK;
	adc1k = status1 & MAX77693_STATUS1_ADC1K_MASK;
	chgtyp = status2 & MAX77693_STATUS2_CHGTYP_MASK;
	vbvolt = status2 & MAX77693_STATUS2_VBVOLT_MASK;
	dxovp = status2 & MAX77693_STATUS2_DXOVP_MASK;

	dev_info(info->dev, "adc:%x adcerr:%x chgtyp:%x vb:%x dxovp:%x\n",
		adc, adcerr, chgtyp, vbvolt, dxovp);

	if (adclow && adc1k) {
		pr_err("%s: MHL cable connected\n", __func__);
		return attached;
	}

	switch (adc) {
	case MAX77693_MUIC_ADC_GROUND :
		if (!adclow) {
			pr_err("%s ADC_GND & !adclow = OTG\n",
					__func__);
			break;
		}
		pr_err("%s ADC_GND & adclow != OTG\n", __func__);
	case MAX77693_MUIC_ADC_MHL ... (MAX77693_MUIC_ADC_SMARTDOCK - 1):
	case (MAX77693_MUIC_ADC_OPEN - 1):
	case (MAX77693_MUIC_ADC_SMARTDOCK + 1):
	case (MAX77693_MUIC_ADC_AUDIODOCK + 1) ... (MAX77693_MUIC_ADC_CEA936A_TYPE1_CHG - 1):
		dev_warn(info->dev, "%s: unsupported ADC(0x%02x)\n",
				__func__, adc);
		//intr = INT_DETACH;
		attached = false;
		break;
	case MAX77693_MUIC_ADC_OPEN:
		//intr = INT_DETACH;
		attached = false;

		if (!adcerr) {
			if (chgtyp == MAX77693_CHARGER_TYPE_NONE) {
				if (dxovp)
					break;
				else
					attached = false;
			} else if (chgtyp == MAX77693_CHARGER_TYPE_USB ||
				 chgtyp == MAX77693_CHARGER_TYPE_DOWNSTREAM_PORT ||
				 chgtyp == MAX77693_CHARGER_TYPE_DEDICATED_CHG ||
				 chgtyp == MAX77693_CHARGER_TYPE_APPLE_500MA ||
				 chgtyp == MAX77693_CHARGER_TYPE_APPLE_1A_2A) {
					 attached = false;
				/*switch (info->cable_type) {
				case CABLE_TYPE_OTG_MUIC:
				case CABLE_TYPE_DESKDOCK_MUIC:
				case CABLE_TYPE_CARDOCK_MUIC:
				case CABLE_TYPE_SMARTDOCK_MUIC:
				case CABLE_TYPE_SMARTDOCK_TA_MUIC:
				case CABLE_TYPE_SMARTDOCK_USB_MUIC:
				case CABLE_TYPE_AUDIODOCK_MUIC:
					attached = false;
					break;
				default:
					break;
				}*/
			}
		}

		break;
	default:
		break;
	}

	return attached;
}

static int max77693_muic_adc_handler(struct max77693_muic_info *info)
{
	int cable_type;
	int button_type;
	bool attached;
	int ret = 0;

	/* Check accessory state which is either detached or attached */
	cable_type = max77693_muic_get_cable_type(info,
				MAX77693_CABLE_GROUP_ADC, &attached);

	dev_info(info->dev,
		"%s: external connector is %s (adc:0x%02x, prev_adc:0x%x)\n",
		__func__, attached ? "attached" : "detached", cable_type,
		info->prev_cable_type);

	if (!attached) {
		pr_err("%s: disable host mode\n", __func__);
		//midas_enable_host_mode(0);
	}

	switch (cable_type) {
	case MAX77693_MUIC_ADC_GROUND:
		/* USB_HOST/MHL/Audio */
		max77693_muic_adc_ground_handler(info);
		break;
	case MAX77693_MUIC_ADC_FACTORY_MODE_USB_OFF:
	case MAX77693_MUIC_ADC_FACTORY_MODE_USB_ON:
	case MAX77693_MUIC_ADC_FACTORY_MODE_UART_OFF:
	case MAX77693_MUIC_ADC_FACTORY_MODE_UART_ON:
		/* JIG */
		ret = max77693_muic_jig_handler(info, cable_type, attached);
		if (ret < 0)
			return ret;
		break;
	case MAX77693_MUIC_ADC_SMARTDOCK:		/* Dock-Smart */
	case MAX77693_MUIC_ADC_AUDIO_MODE_REMOTE:	/* Dock-Desk */
	case MAX77693_MUIC_ADC_AV_CABLE_NOLOAD:		/* Dock-Audio */
		/*
		 * DOCK device
		 *
		 * The MAX77693 MUIC device can detect total 34 cable type
		 * except of charger cable and MUIC device didn't define
		 * specfic role of cable in the range of from 0x01 to 0x12
		 * of ADC value. So, can use/define cable with no role according
		 * to schema of hardware board.
		 */
		ret = max77693_muic_dock_handler(info, cable_type, attached);
		if (ret < 0)
			return ret;
		break;
	case MAX77693_MUIC_ADC_REMOTE_S3_BUTTON:      /* DOCK_KEY_PREV */
	case MAX77693_MUIC_ADC_REMOTE_S7_BUTTON:      /* DOCK_KEY_NEXT */
	case MAX77693_MUIC_ADC_REMOTE_S9_BUTTON:      /* DOCK_VOL_DOWN */
	case MAX77693_MUIC_ADC_REMOTE_S10_BUTTON:     /* DOCK_VOL_UP */
	case MAX77693_MUIC_ADC_REMOTE_S12_BUTTON:     /* DOCK_KEY_PLAY_PAUSE */
		/*
		 * Button of DOCK device
		 * - the Prev/Next/Volume Up/Volume Down/Play-Pause button
		 *
		 * The MAX77693 MUIC device can detect total 34 cable type
		 * except of charger cable and MUIC device didn't define
		 * specfic role of cable in the range of from 0x01 to 0x12
		 * of ADC value. So, can use/define cable with no role according
		 * to schema of hardware board.
		 */
		if (attached)
			button_type = info->prev_button_type = cable_type;
		else
			button_type = info->prev_button_type;

		ret = max77693_muic_dock_button_handler(info, button_type,
							attached);
		if (ret < 0)
			return ret;
		break;
	case MAX77693_MUIC_ADC_MHL:
	case MAX77693_MUIC_ADC_REMOTE_S1_BUTTON:
	case MAX77693_MUIC_ADC_REMOTE_S2_BUTTON:
	case MAX77693_MUIC_ADC_REMOTE_S4_BUTTON:
	case MAX77693_MUIC_ADC_REMOTE_S5_BUTTON:
	case MAX77693_MUIC_ADC_REMOTE_S6_BUTTON:
	case MAX77693_MUIC_ADC_REMOTE_S8_BUTTON:
	case MAX77693_MUIC_ADC_REMOTE_S11_BUTTON:
	case MAX77693_MUIC_ADC_RESERVED_ACC_1:
	case MAX77693_MUIC_ADC_RESERVED_ACC_2:
	case MAX77693_MUIC_ADC_RESERVED_ACC_4:
	case MAX77693_MUIC_ADC_AUDIODOCK:
	case MAX77693_MUIC_ADC_CEA936_AUDIO:
	case MAX77693_MUIC_ADC_PHONE_POWERED_DEV:
	case MAX77693_MUIC_ADC_TTY_CONVERTER:
	case MAX77693_MUIC_ADC_UART_CABLE:
	case MAX77693_MUIC_ADC_CEA936A_TYPE1_CHG:
	case MAX77693_MUIC_ADC_CEA936A_TYPE2_CHG:
		/*
		 * This accessory isn't used in general case if it is specially
		 * needed to detect additional accessory, should implement
		 * proper operation when this accessory is attached/detached.
		 */
		dev_info(info->dev,
			"accessory is %s but it isn't used (adc:0x%x)\n",
			attached ? "attached" : "detached", cable_type);
		return -EAGAIN;
	default:
		dev_err(info->dev,
			"failed to detect %s accessory (adc:0x%x)\n",
			attached ? "attached" : "detached", cable_type);
		return -EINVAL;
	}

	return 0;
}

static int max77693_muic_chg_handler(struct max77693_muic_info *info)
{
	int chg_type;
	int cable_type_gnd;
	int cable_type;
	bool attached;
	bool cable_attached;
	int ret = 0;

	chg_type = max77693_muic_get_cable_type(info,
				MAX77693_CABLE_GROUP_CHG, &attached);

	dev_info(info->dev,
		"%s: external connector is %s(chg_type:0x%x, prev_chg_type:0x%x)\n",
			__func__, attached ? "attached" : "detached",
			chg_type, info->prev_chg_type);

	switch (chg_type) {
	case MAX77693_CHARGER_TYPE_USB:
	case MAX77693_CHARGER_TYPE_DEDICATED_CHG:
	case MAX77693_CHARGER_TYPE_NONE:
		/* Check MAX77693_CABLE_GROUP_ADC_GND type */
		cable_type_gnd = max77693_muic_get_cable_type(info,
					MAX77693_CABLE_GROUP_ADC_GND,
					&cable_attached);
		switch (cable_type_gnd) {
		case MAX77693_MUIC_GND_MHL:
		case MAX77693_MUIC_GND_MHL_VB:
			/*
			 * MHL cable with USB/TA cable
			 * - MHL cable include two port(HDMI line and separate
			 * micro-usb port. When the target connect MHL cable,
			 * extcon driver check whether USB/TA cable is
			 * connected. If USB/TA cable is connected, extcon
			 * driver notify state to notifiee for charging battery.
			 *
			 * Features of 'USB/TA with MHL cable'
			 * - Support MHL
			 * - Support charging through micro-usb port without
			 *   data connection
			 */
			extcon_set_state_sync(info->edev, EXTCON_CHG_USB_DCP,
						attached);
			extcon_set_state_sync(info->edev, EXTCON_DISP_MHL,
						cable_attached);
			break;
		}

		/* Check MAX77693_CABLE_GROUP_ADC type */
		cable_type = max77693_muic_get_cable_type(info,
					MAX77693_CABLE_GROUP_ADC,
					&cable_attached);
		switch (cable_type) {
		case MAX77693_MUIC_ADC_AV_CABLE_NOLOAD:		/* Dock-Audio */
			/*
			 * Dock-Audio device with USB/TA cable
			 * - Dock device include two port(Dock-Audio and micro-
			 * usb port). When the target connect Dock-Audio device,
			 * extcon driver check whether USB/TA cable is connected
			 * or not. If USB/TA cable is connected, extcon driver
			 * notify state to notifiee for charging battery.
			 *
			 * Features of 'USB/TA cable with Dock-Audio device'
			 * - Support external output feature of audio.
			 * - Support charging through micro-usb port without
			 *   data connection.
			 */
			extcon_set_state_sync(info->edev, EXTCON_USB,
						attached);
			extcon_set_state_sync(info->edev, EXTCON_CHG_USB_SDP,
						attached);

			if (!cable_attached)
				extcon_set_state_sync(info->edev, EXTCON_DOCK,
							cable_attached);
			break;
		case MAX77693_MUIC_ADC_SMARTDOCK:		/* Dock-Smart */
			/*
			 * Dock-Smart device with USB/TA cable
			 * - Dock-Desk device include three type of cable which
			 * are HDMI, USB for mouse/keyboard and micro-usb port
			 * for USB/TA cable. Dock-Smart device need always
			 * exteranl power supply(USB/TA cable through micro-usb
			 * cable). Dock-Smart device support screen output of
			 * target to separate monitor and mouse/keyboard for
			 * desktop mode.
			 *
			 * Features of 'USB/TA cable with Dock-Smart device'
			 * - Support MHL
			 * - Support external output feature of audio
			 * - Support charging through micro-usb port without
			 *   data connection if TA cable is connected to target.
			 * - Support charging and data connection through micro-
			 *   usb port if USB cable is connected between target
			 *   and host device
			 * - Support OTG(On-The-Go) device (Ex: Mouse/Keyboard)
			 */
			ret = max77693_muic_set_path(info, info->path_usb,
						    attached);
			if (ret < 0)
				return ret;

			extcon_set_state_sync(info->edev, EXTCON_DOCK,
						attached);
			extcon_set_state_sync(info->edev, EXTCON_DISP_MHL,
						attached);
			break;
		}

		/* Check MAX77693_CABLE_GROUP_CHG type */
		switch (chg_type) {
		case MAX77693_CHARGER_TYPE_NONE:
			/*
			 * When MHL(with USB/TA cable) or Dock-Audio with USB/TA
			 * cable is attached, muic device happen below two irq.
			 * - 'MAX77693_MUIC_IRQ_INT1_ADC' for detecting
			 *    MHL/Dock-Audio.
			 * - 'MAX77693_MUIC_IRQ_INT2_CHGTYP' for detecting
			 *    USB/TA cable connected to MHL or Dock-Audio.
			 * Always, happen eariler MAX77693_MUIC_IRQ_INT1_ADC
			 * irq than MAX77693_MUIC_IRQ_INT2_CHGTYP irq.
			 *
			 * If user attach MHL (with USB/TA cable and immediately
			 * detach MHL with USB/TA cable before MAX77693_MUIC_IRQ
			 * _INT2_CHGTYP irq is happened, USB/TA cable remain
			 * connected state to target. But USB/TA cable isn't
			 * connected to target. The user be face with unusual
			 * action. So, driver should check this situation in
			 * spite of, that previous charger type is N/A.
			 */
			break;
		case MAX77693_CHARGER_TYPE_USB:
			/* Only USB cable, PATH:AP_USB */
			ret = max77693_muic_set_path(info, info->path_usb,
						    attached);
			if (ret < 0)
				return ret;

			extcon_set_state_sync(info->edev, EXTCON_USB,
						attached);
			extcon_set_state_sync(info->edev, EXTCON_CHG_USB_SDP,
						attached);
			break;
		case MAX77693_CHARGER_TYPE_DEDICATED_CHG:
			/* Only TA cable */
			extcon_set_state_sync(info->edev, EXTCON_CHG_USB_DCP,
						attached);
			break;
		}
		break;
	case MAX77693_CHARGER_TYPE_DOWNSTREAM_PORT:
		extcon_set_state_sync(info->edev, EXTCON_CHG_USB_CDP,
					attached);
		break;
	case MAX77693_CHARGER_TYPE_APPLE_500MA:
		extcon_set_state_sync(info->edev, EXTCON_CHG_USB_SLOW,
					attached);
		break;
	case MAX77693_CHARGER_TYPE_APPLE_1A_2A:
		extcon_set_state_sync(info->edev, EXTCON_CHG_USB_FAST,
					attached);
		break;
	case MAX77693_CHARGER_TYPE_DEAD_BATTERY:
		break;
	default:
		dev_err(info->dev,
			"failed to detect %s accessory (chg_type:0x%x)\n",
			attached ? "attached" : "detached", chg_type);
		return -EINVAL;
	}

	return 0;
}

static void max77693_muic_irq_work(struct work_struct *work)
{
	struct max77693_muic_info *info = container_of(work,
			struct max77693_muic_info, irq_work);
	int irq_type = -1;
	int i, ret, cable_type = 0;
	u8 adc, adclow, adcerr, adc1k, chgtyp, vbvolt, dxovp;
	bool attached;

	if (!info->edev)
		return;

	mutex_lock(&info->mutex);

	for (i = 0; i < ARRAY_SIZE(muic_irqs); i++)
		if (info->irq == muic_irqs[i].virq)
			irq_type = muic_irqs[i].irq;

	ret = regmap_bulk_read(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_STATUS1, info->status, 2);
	if (ret) {
		dev_err(info->dev, "failed to read MUIC register\n");
		mutex_unlock(&info->mutex);
		return;
	}

	adc = info->status[0] & MAX77693_STATUS1_ADC_MASK;
	adclow = info->status[0] & MAX77693_STATUS1_ADCLOW_MASK;
	adcerr = info->status[0] & MAX77693_STATUS1_ADCERR_MASK;
	adc1k = info->status[0] & MAX77693_STATUS1_ADC1K_MASK;
	chgtyp = info->status[1] & MAX77693_STATUS2_CHGTYP_MASK;
	vbvolt = info->status[1] & MAX77693_STATUS2_VBVOLT_MASK;
	dxovp = info->status[1] & MAX77693_STATUS2_DXOVP_MASK;

	cable_type = max77693_muic_get_cable_type(info,
				MAX77693_CABLE_GROUP_ADC_GND, &attached);

	dev_info(info->dev, "%s: adc:%x adcerr:%x chgtyp:%x vb:%x dxovp:%x cable_type: %d\n",
		__func__, adc, adcerr, chgtyp, vbvolt, dxovp, cable_type);

	pr_err("%s: irq_type = %d\n", __func__, irq_type);

	if (cable_type == MAX77693_MUIC_GND_USB_HOST || cable_type == MAX77693_MUIC_GND_USB_HOST_VB) {
		pr_err("%s: goto adc_handler\n", __func__);
		midas_enable_host_mode(attached);
		goto out;
		//goto adc_handler;
	}

	//if (adc == (MAX77693_MUIC_ADC_GROUND || MAX77693_MUIC_ADC_OPEN) && chgtyp == MAX77693_CHARGER_TYPE_NONE)
	//	goto adc_handler;

	switch (irq_type) {
	case MAX77693_MUIC_IRQ_INT1_ADC:
	case MAX77693_MUIC_IRQ_INT1_ADC_LOW:
	case MAX77693_MUIC_IRQ_INT1_ADC_ERR:
	case MAX77693_MUIC_IRQ_INT1_ADC1K:
adc_handler:
		/*
		 * Handle all of accessory except for
		 * type of charger accessory.
		 */
		ret = max77693_muic_adc_handler(info);
		break;
	case MAX77693_MUIC_IRQ_INT2_CHGTYP:
	case MAX77693_MUIC_IRQ_INT2_CHGDETREUN:
	case MAX77693_MUIC_IRQ_INT2_DCDTMR:
	case MAX77693_MUIC_IRQ_INT2_DXOVP:
	case MAX77693_MUIC_IRQ_INT2_VBVOLT:
	case MAX77693_MUIC_IRQ_INT2_VIDRM:
		/* Handle charger accessory */
		ret = max77693_muic_chg_handler(info);
		break;
	case MAX77693_MUIC_IRQ_INT3_EOC:
	case MAX77693_MUIC_IRQ_INT3_CGMBC:
	case MAX77693_MUIC_IRQ_INT3_OVP:
	case MAX77693_MUIC_IRQ_INT3_MBCCHG_ERR:
	case MAX77693_MUIC_IRQ_INT3_CHG_ENABLED:
	case MAX77693_MUIC_IRQ_INT3_BAT_DET:
		break;
	default:
		dev_err(info->dev, "muic interrupt: irq %d occurred\n",
				irq_type);
		mutex_unlock(&info->mutex);
		return;
	}
out:

	if (ret < 0)
		dev_err(info->dev, "failed to handle MUIC interrupt\n");

	mutex_unlock(&info->mutex);
}

static irqreturn_t max77693_muic_irq_handler(int irq, void *data)
{
	struct max77693_muic_info *info = data;

	info->irq = irq;
	schedule_work(&info->irq_work);

	return IRQ_HANDLED;
}

static const struct regmap_config max77693_muic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int max77693_muic_detect_accessory(struct max77693_muic_info *info)
{
	int ret = 0;
	int adc;
	int chg_type;
	bool attached;

	mutex_lock(&info->mutex);

	/* Read STATUSx register to detect accessory */
	ret = regmap_bulk_read(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_STATUS1, info->status, 2);

	pr_err("%s: MAX77693_MUIC_REG_STATUS1 %02x %02x\n", __func__, info->status[0], info->status[1]);

	if (ret) {
		dev_err(info->dev, "failed to read MUIC register\n");
		mutex_unlock(&info->mutex);
		return ret;
	}

	adc = max77693_muic_get_cable_type(info, MAX77693_CABLE_GROUP_ADC,
					&attached);

	pr_err("%s: MAX77693_CABLE_GROUP_ADC attached=%d, adc=%d\n", __func__, attached, adc);

	if (attached && adc != MAX77693_MUIC_ADC_OPEN) {
		ret = max77693_muic_adc_handler(info);
		if (ret < 0) {
			dev_err(info->dev, "Cannot detect accessory\n");
			mutex_unlock(&info->mutex);
			return ret;
		}
	}

	chg_type = max77693_muic_get_cable_type(info, MAX77693_CABLE_GROUP_CHG,
					&attached);
	if (attached && chg_type != MAX77693_CHARGER_TYPE_NONE) {
		ret = max77693_muic_chg_handler(info);
		if (ret < 0) {
			dev_err(info->dev, "Cannot detect charger accessory\n");
			mutex_unlock(&info->mutex);
			return ret;
		}
	}

	mutex_unlock(&info->mutex);

	return 0;
}

static void max77693_muic_detect_cable_wq(struct work_struct *work)
{
	struct max77693_muic_info *info = container_of(to_delayed_work(work),
				struct max77693_muic_info, wq_detcable);

	max77693_muic_detect_accessory(info);
}

static int max77693_muic_probe(struct platform_device *pdev)
{
	struct max77693_dev *max77693 = dev_get_drvdata(pdev->dev.parent);
	struct max77693_platform_data *pdata = dev_get_platdata(max77693->dev);
	struct max77693_muic_info *info;
	struct max77693_reg_data *init_data;
	int num_init_data;
	int delay_jiffies;
	int cable_type;
	bool attached;
	int ret;
	int i;
	unsigned int id;

	info = devm_kzalloc(&pdev->dev, sizeof(struct max77693_muic_info),
				   GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &pdev->dev;
	info->max77693 = max77693;
	if (info->max77693->regmap_muic) {
		dev_dbg(&pdev->dev, "allocate register map\n");
	} else {
		info->max77693->regmap_muic = devm_regmap_init_i2c(
						info->max77693->i2c_muic,
						&max77693_muic_regmap_config);
		if (IS_ERR(info->max77693->regmap_muic)) {
			ret = PTR_ERR(info->max77693->regmap_muic);
			dev_err(max77693->dev,
				"failed to allocate register map: %d\n", ret);
			return ret;
		}
	}

	/* Register input device for button of dock device */
	info->dock = devm_input_allocate_device(&pdev->dev);
	if (!info->dock) {
		dev_err(&pdev->dev, "%s: failed to allocate input\n", __func__);
		return -ENOMEM;
	}
	info->dock->name = "max77693-muic/dock";
	info->dock->phys = "max77693-muic/extcon";
	info->dock->dev.parent = &pdev->dev;

	__set_bit(EV_REP, info->dock->evbit);

	input_set_capability(info->dock, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(info->dock, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(info->dock, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(info->dock, EV_KEY, KEY_PREVIOUSSONG);
	input_set_capability(info->dock, EV_KEY, KEY_NEXTSONG);

	ret = input_register_device(info->dock);
	if (ret < 0) {
		dev_err(&pdev->dev, "Cannot register input device error(%d)\n",
				ret);
		return ret;
	}

	platform_set_drvdata(pdev, info);
	mutex_init(&info->mutex);

	INIT_WORK(&info->irq_work, max77693_muic_irq_work);

	/* Support irq domain for MAX77693 MUIC device */
	for (i = 0; i < ARRAY_SIZE(muic_irqs); i++) {
		struct max77693_muic_irq *muic_irq = &muic_irqs[i];
		int virq;

		virq = regmap_irq_get_virq(max77693->irq_data_muic,
					muic_irq->irq);
		if (virq <= 0)
			return -EINVAL;
		muic_irq->virq = virq;

		ret = devm_request_threaded_irq(&pdev->dev, virq, NULL,
				max77693_muic_irq_handler,
				IRQF_NO_SUSPEND,
				muic_irq->name, info);
		if (ret) {
			dev_err(&pdev->dev,
				"failed: irq request (IRQ: %d, error :%d)\n",
				muic_irq->irq, ret);
			return ret;
		}
	}

	/* Initialize extcon device */
	info->edev = devm_extcon_dev_allocate(&pdev->dev,
					      max77693_extcon_cable);
	if (IS_ERR(info->edev)) {
		dev_err(&pdev->dev, "failed to allocate memory for extcon\n");
		return PTR_ERR(info->edev);
	}

	ret = devm_extcon_dev_register(&pdev->dev, info->edev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register extcon device\n");
		return ret;
	}

	/* Initialize MUIC register by using platform data or default data */
	if (pdata && pdata->muic_data) {
		init_data = pdata->muic_data->init_data;
		num_init_data = pdata->muic_data->num_init_data;
	} else {
		init_data = default_init_data;
		num_init_data = ARRAY_SIZE(default_init_data);
	}

	for (i = 0; i < num_init_data; i++) {
		regmap_write(info->max77693->regmap_muic,
				init_data[i].addr,
				init_data[i].data);
	}

	if (pdata && pdata->muic_data) {
		struct max77693_muic_platform_data *muic_pdata
						   = pdata->muic_data;

		/*
		 * Default usb/uart path whether UART/USB or AUX_UART/AUX_USB
		 * h/w path of COMP2/COMN1 on CONTROL1 register.
		 */
		if (muic_pdata->path_uart)
			info->path_uart = muic_pdata->path_uart;
		else
			info->path_uart = MAX77693_CONTROL1_SW_UART;

		if (muic_pdata->path_usb)
			info->path_usb = muic_pdata->path_usb;
		else
			info->path_usb = MAX77693_CONTROL1_SW_USB;

		/*
		 * Default delay time for detecting cable state
		 * after certain time.
		 */
		if (muic_pdata->detcable_delay_ms)
			delay_jiffies =
				msecs_to_jiffies(muic_pdata->detcable_delay_ms);
		else
			delay_jiffies = msecs_to_jiffies(DELAY_MS_DEFAULT);
	} else {
		info->path_usb = MAX77693_CONTROL1_SW_USB;
		info->path_uart = MAX77693_CONTROL1_SW_UART;
		delay_jiffies = msecs_to_jiffies(DELAY_MS_DEFAULT);
	}

	/* Set initial path for UART when JIG is connected to get serial logs */
	ret = regmap_bulk_read(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_STATUS1, info->status, 2);
	if (ret) {
		dev_err(info->dev, "failed to read MUIC register\n");
		return ret;
	}
	cable_type = max77693_muic_get_cable_type(info,
					   MAX77693_CABLE_GROUP_ADC, &attached);
	if (attached && (cable_type == MAX77693_MUIC_ADC_FACTORY_MODE_UART_ON ||
			 cable_type == MAX77693_MUIC_ADC_FACTORY_MODE_UART_OFF))
		max77693_muic_set_path(info, info->path_uart, true);

	/* Check revision number of MUIC device*/
	ret = regmap_read(info->max77693->regmap_muic,
			MAX77693_MUIC_REG_ID, &id);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to read revision number\n");
		return ret;
	}
	dev_info(info->dev, "device ID : 0x%x\n", id);

	/* Set ADC debounce time */
	max77693_muic_set_debounce_time(info, ADC_DEBOUNCE_TIME_25MS);

	/*
	 * Detect accessory after completing the initialization of platform
	 *
	 * - Use delayed workqueue to detect cable state and then
	 * notify cable state to notifiee/platform through uevent.
	 * After completing the booting of platform, the extcon provider
	 * driver should notify cable state to upper layer.
	 */
	INIT_DELAYED_WORK(&info->wq_detcable, max77693_muic_detect_cable_wq);
	queue_delayed_work(system_power_efficient_wq, &info->wq_detcable,
			delay_jiffies);

	gInfo = info;

	return ret;
}

static int max77693_muic_remove(struct platform_device *pdev)
{
	struct max77693_muic_info *info = platform_get_drvdata(pdev);

	cancel_work_sync(&info->irq_work);
	input_unregister_device(info->dock);

	return 0;
}

static struct platform_driver max77693_muic_driver = {
	.driver		= {
		.name	= DEV_NAME,
	},
	.probe		= max77693_muic_probe,
	.remove		= max77693_muic_remove,
};

module_platform_driver(max77693_muic_driver);

MODULE_DESCRIPTION("Maxim MAX77693 Extcon driver");
MODULE_AUTHOR("Chanwoo Choi <cw00.choi@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:max77693-muic");
