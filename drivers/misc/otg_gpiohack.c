// SPDX-License-Identifier: GPL-2.0+
//
// Copyright (C) 2021 Victor Shilin <chrono.monochrome@gmail.com>
//
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>

struct gpiohack {
	struct device *dev;
	struct gpio_desc *otg_en;
	struct gpio_desc *usb_sel;

	int state;
};

static ssize_t gpiohack_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpiohack *hack = platform_get_drvdata(pdev);
	int new_state;

	if (kstrtoint(buf, 0, &new_state) < 0)
		return -EINVAL;

	dev_info(dev, "new state: %d\n", new_state);
	if (!new_state && hack->state) {
		/* currently on, power off */
		hack->state = 0;
		if (hack->otg_en)
			gpiod_set_value_cansleep(hack->otg_en, 0);
		//gpiohack_modem_off(hack);
	} else if (new_state && !hack->state) {
		/* currently off, power on */
		hack->state = 1;
		if (hack->otg_en)
			gpiod_set_value_cansleep(hack->otg_en, 1);
		//gpiohack_modem_on(hack);
	}

	return len;
}

static ssize_t gpiohack_sysfs_show(struct device *dev,
				   struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpiohack *hack = platform_get_drvdata(pdev);

	return scnprintf(buf, PAGE_SIZE, "%s\n", hack->state ? "on" : "off");
}

static DEVICE_ATTR(votg_power, 0644, gpiohack_sysfs_show, gpiohack_sysfs_store);

static int gpiohack_probe(struct platform_device *pdev) {
	struct gpiohack *dev;
	int ret;

	pr_err("%s: init\n", __func__);

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;

	dev->otg_en = devm_gpiod_get(dev->dev, "otg_en", GPIOD_OUT_LOW);
	if (IS_ERR(dev->otg_en)) {
		/*dev_err(dev->dev, "ernk reset-req: %ld\n",
			PTR_ERR(dev->reset_req));*/
		pr_err("%s: ernk otg_en: %ld\n", __func__, PTR_ERR(dev->otg_en));
		return PTR_ERR(dev->otg_en);
	}

	dev->usb_sel = devm_gpiod_get(dev->dev, "usb_sel", GPIOD_OUT_LOW);
	if (IS_ERR(dev->usb_sel)) {
		/*dev_err(dev->dev, "ernk reset-req: %ld\n",
			PTR_ERR(dev->reset_req));*/
		pr_err("%s: ernk usb_sel: %ld\n", __func__, PTR_ERR(dev->usb_sel));
		return PTR_ERR(dev->usb_sel);
	}

	dev_err(dev->dev, "Loaded all GPIOs\n");

	device_create_file(dev->dev, &dev_attr_votg_power);
	dev->state = 0;
	platform_set_drvdata(pdev, dev);

	pr_err("%s: exit\n", __func__);

	return 0;
}

static int gpiohack_remove(struct platform_device *pdev) {
	struct gpiohack *dev = platform_get_drvdata(pdev);
	dev_err(&pdev->dev, "bye\n");
	return 0;
}

static const struct of_device_id samsung_votg_ctl_of_ids[] = {
	{ .compatible = "samsung,votg-ctl", },
	{},
};

MODULE_DEVICE_TABLE(of, ids);

static struct platform_driver votgctl_driver = {
	.driver = {
		.name = "otg_gpiohack",
		.of_match_table = of_match_ptr(samsung_votg_ctl_of_ids),
	},
	.probe = gpiohack_probe,
	.remove = gpiohack_remove,
};

module_platform_driver(votgctl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Victor Shilin <chrono.monochrome@gmail.com>");
