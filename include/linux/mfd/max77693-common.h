/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Common data shared between Maxim 77693 and 77843 drivers
 *
 * Copyright (C) 2015 Samsung Electronics
 */

#ifndef __LINUX_MFD_MAX77693_COMMON_H
#define __LINUX_MFD_MAX77693_COMMON_H

enum max77693_types {
	TYPE_MAX77693_UNKNOWN,
	TYPE_MAX77693,
	TYPE_MAX77843,

	TYPE_MAX77693_NUM,
};

/*
 * Shared also with max77843.
 */
struct max77693_dev {
	struct device *dev;
	struct i2c_client *i2c;		/* 0xCC , PMIC, Charger, Flash LED */
	struct i2c_client *i2c_muic;	/* 0x4A , MUIC */
	struct i2c_client *i2c_haptic;	/* MAX77693: 0x90 , Haptic */
	struct i2c_client *i2c_chg;	/* MAX77843: 0xD2, Charger */

	enum max77693_types type;

	struct regmap *regmap;
	struct regmap *regmap_muic;
	struct regmap *regmap_haptic;	/* Only MAX77693 */
	struct regmap *regmap_chg;	/* Only MAX77843 */

	struct regmap_irq_chip_data *irq_data_led;
	struct regmap_irq_chip_data *irq_data_topsys;
	struct regmap_irq_chip_data *irq_data_chg; /* Only MAX77693 */
	struct regmap_irq_chip_data *irq_data_muic;

	int irq;
};

struct extcon_dev;

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

#endif /*  __LINUX_MFD_MAX77693_COMMON_H */
