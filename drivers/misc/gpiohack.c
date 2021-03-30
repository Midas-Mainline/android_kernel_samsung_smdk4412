// SPDX-License-Identifier: GPL-2.0+
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
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
	struct gpio_desc *reset_req;
	struct gpio_desc *cp_on;
	struct gpio_desc *cp_reset;
	struct gpio_desc *phone_active;
	struct gpio_desc *cp_dump;
	struct gpio_desc *pda_active;
	struct gpio_desc *link_active;
	struct gpio_desc *link_hostwake;
	struct gpio_desc *link_slavewake;
	struct gpio_desc *suspend_req;

	struct clk *clk;

	int irq_phone_active;
	int irq_hostwake;

	int state;
};

static void reset_modem_control(struct gpiohack *dev) {
	gpiod_direction_output(dev->pda_active, 0);
	gpiod_direction_output(dev->cp_dump, 0);
	gpiod_direction_output(dev->link_active, 0);
	gpiod_direction_output(dev->link_slavewake, 0);

	gpiod_direction_output(dev->reset_req, 0);
	gpiod_direction_output(dev->cp_on, 0);
	gpiod_direction_output(dev->cp_reset, 0);

	msleep(20);
}

static void gpiohack_modem_on(struct gpiohack *dev) {
	reset_modem_control(dev);

	gpiod_set_value(dev->reset_req, 0);
	gpiod_set_value(dev->cp_on, 0);
	gpiod_set_value(dev->cp_reset, 0);
	msleep(100);

	gpiod_set_value(dev->cp_reset, 1);
	msleep(50);
	gpiod_set_value(dev->reset_req, 1);

	gpiod_set_value(dev->suspend_req, 1);

	gpiod_set_value(dev->cp_on, 1);
	udelay(60);
	gpiod_set_value(dev->cp_on, 0);
	msleep(20);

	gpiod_direction_input(dev->cp_dump);

	gpiod_set_value(dev->pda_active, 1);
}

static void gpiohack_modem_off(struct gpiohack *dev) {
	gpiod_set_value(dev->cp_on, 0);
	gpiod_set_value(dev->cp_reset, 0);
	reset_modem_control(dev);
}

static ssize_t gpiohack_hostwake_show(struct device *_dev, struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(_dev);
	struct gpiohack *dev = platform_get_drvdata(pdev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", gpiod_get_value(dev->link_hostwake));
}

static irqreturn_t phone_active_irq_handler(int irq, void *_dev)
{
	struct gpiohack *dev = _dev;
	int pa, cpr, cpd;

	disable_irq_nosync(dev->irq_phone_active);

	pa = gpiod_get_value(dev->phone_active);
	cpr = gpiod_get_value(dev->cp_reset);
	cpd = gpiod_get_value(dev->cp_dump);

	dev_info(dev->dev, "phone_active: cp_reset=%d, phone_active=%d, cp_dump=%d\n",
			cpr, pa, cpd);

	if (cpr && pa)
		dev_info(dev->dev, "BOOTING\n");
	else if (cpr && !pa) {
		if (cpd)
			dev_info(dev->dev, "CRASH EXIT\n");
		else
			dev_info(dev->dev, "CRASH RESET\n");
	} else
		dev_info(dev->dev, "OFFLINE\n");

	if (pa)
		irq_set_irq_type(dev->irq_phone_active, IRQ_TYPE_LEVEL_LOW);
	else
		irq_set_irq_type(dev->irq_phone_active, IRQ_TYPE_LEVEL_HIGH);

	enable_irq(dev->irq_phone_active);

	return IRQ_HANDLED;
}

static irqreturn_t hostwake_irq_handler(int irq, void *_dev)
{
	struct gpiohack *dev = _dev;
	int hwk;

	hwk = gpiod_get_value(dev->link_hostwake);

	dev_info(dev->dev, "hostwake: %d\n", hwk);

	return IRQ_HANDLED;
}

static ssize_t gpiohack_phone_active_show(struct device *_dev, struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(_dev);
	struct gpiohack *dev = platform_get_drvdata(pdev);
	int pa, cpr, cpd;
	pa = gpiod_get_value(dev->phone_active);
	cpr = gpiod_get_value(dev->cp_reset);
	cpd = gpiod_get_value(dev->cp_dump);

	if (cpr && pa) {
		return scnprintf(buf, PAGE_SIZE, "booting pa=%d cpr=%d cpd=%d\n", pa, cpr, cpd);
	} else if (cpr && !pa) {
		if (cpd)
			return scnprintf(buf, PAGE_SIZE, "crash exit pa=%d cpr=%d cpd=%d\n", pa, cpr, cpd);
		else
			return scnprintf(buf, PAGE_SIZE, "crash reset pa=%d cpr=%d cpd=%d\n", pa, cpr, cpd);
	} else {
		return scnprintf(buf, PAGE_SIZE, "offline pa=%d cpr=%d cpd=%d\n", pa, cpr, cpd);
	}
}


static ssize_t gpiohack_link_active_store(struct device *_dev, struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct platform_device *pdev = to_platform_device(_dev);
	struct gpiohack *dev = platform_get_drvdata(pdev);
	int state;

	if (kstrtoint(buf, 0, &state) < 0)
		return -EINVAL;

	state = !!state;
	dev_info(_dev, "link active? %d\n", state);
	gpiod_set_value(dev->link_active, state);
	return len;
}

static ssize_t gpiohack_slavewake_store(struct device *_dev, struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct platform_device *pdev = to_platform_device(_dev);
	struct gpiohack *dev = platform_get_drvdata(pdev);
	int state;

	if (kstrtoint(buf, 0, &state) < 0)
		return -EINVAL;

	state = !!state;
	dev_info(_dev, "slave wake => %d\n", state);
	gpiod_set_value(dev->link_slavewake, state);
	return len;
}

static ssize_t gpiohack_sysfs_store(struct device *dev, struct device_attribute *attr,
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
		gpiohack_modem_off(hack);
	} else if (new_state && !hack->state) {
		/* currently off, power on */
		hack->state = 1;
		gpiohack_modem_on(hack);
	}

	return len;
}

static ssize_t gpiohack_sysfs_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpiohack *hack = platform_get_drvdata(pdev);

	return scnprintf(buf, PAGE_SIZE, "%s\n", hack->state ? "on" : "off");
}

static ssize_t gpiohack_pda_active_store(struct device *_dev, struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct platform_device *pdev = to_platform_device(_dev);
	struct gpiohack *dev = platform_get_drvdata(pdev);
	int state;

	if (kstrtoint(buf, 0, &state) < 0)
		return -EINVAL;

	state = !!state;
	dev_info(_dev, "pda active => %d\n", state);
	gpiod_set_value(dev->pda_active, state);
	return len;
}


static DEVICE_ATTR(modem_power, 0644, gpiohack_sysfs_show, gpiohack_sysfs_store);
static DEVICE_ATTR(hostwake, 0444, gpiohack_hostwake_show, NULL);
static DEVICE_ATTR(phone_active, 0444, gpiohack_phone_active_show, NULL);
static DEVICE_ATTR(slavewake, 0200, NULL, gpiohack_slavewake_store);
static DEVICE_ATTR(link_active, 0200, NULL, gpiohack_link_active_store);
static DEVICE_ATTR(pda_active, 0200, NULL, gpiohack_pda_active_store);

static int gpiohack_probe(struct platform_device *pdev) {
	struct gpiohack *dev;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;

	dev->reset_req = devm_gpiod_get(dev->dev, "reset-req", GPIOD_OUT_LOW);
	if (IS_ERR(dev->reset_req)) {
		dev_err(dev->dev, "ernk reset-req: %ld\n", PTR_ERR(dev->reset_req));
		return PTR_ERR(dev->reset_req);
	}

	dev->cp_on = devm_gpiod_get(dev->dev, "cp-on", GPIOD_OUT_LOW);
	if (IS_ERR(dev->cp_on)) {
		dev_err(dev->dev, "ernk cp_on: %ld\n", PTR_ERR(dev->cp_on));
		return PTR_ERR(dev->cp_on);
	}
	dev->cp_reset = devm_gpiod_get(dev->dev, "cp-reset", GPIOD_OUT_LOW);
	if (IS_ERR(dev->cp_reset)) {
		dev_err(dev->dev, "ernk cp-reset: %ld\n", PTR_ERR(dev->cp_reset));
		return PTR_ERR(dev->cp_reset);
	}

	dev->link_active = devm_gpiod_get(dev->dev, "link-active", GPIOD_OUT_LOW);
	if (IS_ERR(dev->link_active)) {
		dev_err(dev->dev, "ernk link_active: %ld\n", PTR_ERR(dev->link_active));
		return PTR_ERR(dev->link_active);
	}

	dev->phone_active = devm_gpiod_get(dev->dev, "phone-active", GPIOD_IN);
	if (IS_ERR(dev->phone_active)) {
		dev_err(dev->dev, "ernk phone_active: %ld\n", PTR_ERR(dev->phone_active));
		return PTR_ERR(dev->phone_active);
	}

	dev->cp_dump = devm_gpiod_get(dev->dev, "cp-dump", GPIOD_IN);
	if (IS_ERR(dev->cp_dump)) {
		dev_err(dev->dev, "ernk cp_dump: %ld\n", PTR_ERR(dev->cp_dump));
		return PTR_ERR(dev->cp_dump);
	}

	dev->link_hostwake = devm_gpiod_get(dev->dev, "link-hostwake", GPIOD_IN);
	if (IS_ERR(dev->link_hostwake)) {
		dev_err(dev->dev, "ernk link_hostwake: %ld\n", PTR_ERR(dev->link_hostwake));
		return PTR_ERR(dev->link_hostwake);
	}

	dev->suspend_req = devm_gpiod_get(dev->dev, "suspend-req", GPIOD_OUT_LOW);
	if (IS_ERR(dev->suspend_req)) {
		dev_err(dev->dev, "ernk suspend_req: %ld\n", PTR_ERR(dev->suspend_req));
		return PTR_ERR(dev->suspend_req);
	}

	dev->link_slavewake = devm_gpiod_get(dev->dev, "link-slavewake", GPIOD_OUT_LOW);
	if (IS_ERR(dev->link_slavewake)) {
		dev_err(dev->dev, "ernk link_slavewake: %ld\n", PTR_ERR(dev->link_slavewake));
		return PTR_ERR(dev->link_slavewake);
	}

	dev_err(dev->dev, "Loaded all GPIOs\n");

	dev->pda_active = devm_gpiod_get(dev->dev, "pda-active", GPIOD_OUT_LOW);
	if (IS_ERR(dev->pda_active)) {
		dev_err(dev->dev, "ernk pda-active: %ld\n", PTR_ERR(dev->pda_active));
		return PTR_ERR(dev->pda_active);
	}

	dev_err(dev->dev, "Loaded enable gpio\n");

	dev->irq_phone_active = gpiod_to_irq(dev->phone_active);
	ret = devm_request_irq(dev->dev, dev->irq_phone_active, phone_active_irq_handler,
			IRQF_NO_SUSPEND | IRQF_TRIGGER_HIGH,
			"phone_active", dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request phone_active irq: %d\n", ret);
		return ret;
	}

	dev->irq_hostwake = gpiod_to_irq(dev->link_hostwake);
	ret = devm_request_irq(dev->dev, dev->irq_hostwake, hostwake_irq_handler,
			IRQF_NO_SUSPEND | IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			"hostwake", dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request hostwake irq: %d\n", ret);
		return ret;
	}

	dev->clk = devm_clk_get(dev->dev, "cp_clk");
	if (IS_ERR_OR_NULL(dev->clk)) {
		dev_err(dev->dev, "Failed to get CP clock\n");
		return PTR_ERR(dev->clk);
	}

	clk_prepare_enable(dev->clk);

	device_create_file(dev->dev, &dev_attr_modem_power);
	device_create_file(dev->dev, &dev_attr_phone_active);
	device_create_file(dev->dev, &dev_attr_hostwake);
	device_create_file(dev->dev, &dev_attr_slavewake);
	device_create_file(dev->dev, &dev_attr_link_active);
	device_create_file(dev->dev, &dev_attr_pda_active);
	dev->state = 0;
	platform_set_drvdata(pdev, dev);
	return 0;
}

static int gpiohack_remove(struct platform_device *pdev) {
	struct gpiohack *dev = platform_get_drvdata(pdev);
	clk_disable_unprepare(dev->clk);
	dev_err(&pdev->dev, "bye\n");
	return 0;
}

static const struct of_device_id samsung_modem_ctl_of_ids[] = {
	{ .compatible = "samsung,modem-ctl", },
	{},
};

MODULE_DEVICE_TABLE(of, samsung_modem_ctl_of_ids);

static struct platform_driver modemctl_driver = {
	.driver = {
		.name = "gpiohack",
		.of_match_table = of_match_ptr(samsung_modem_ctl_of_ids),
	},
	.probe = gpiohack_probe,
	.remove = gpiohack_remove,
};

module_platform_driver(modemctl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Shields <simon@lineageos.org>");
