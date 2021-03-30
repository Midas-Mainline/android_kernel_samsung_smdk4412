/*
 * Copyright (C) 2011 Samsung Electronics Co. Ltd.
 *  Hyuk Kang <hyuk78.kang@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/wakelock.h>
#include <linux/host_notify.h>

struct  host_notifier_info {
	struct host_notifier_platform_data *pdata;
	struct task_struct *th;
	struct wake_lock	wlock;
	struct delayed_work current_dwork;
	wait_queue_head_t	delay_wait;
	int	thread_remove;
	int currentlimit_irq;
};

static struct host_notifier_info ninfo;

static int start_usbhostd_thread(void)
{
	return 0;
}

static int stop_usbhostd_thread(void)
{
	return 0;
}

static int start_usbhostd_notify(void)
{
	return 0;
}

static int stop_usbhostd_notify(void)
{
	return 0;
}

static void host_notifier_booster(int enable)
{
	pr_info("host_notifier: booster %s\n", enable ? "ON" : "OFF");

	ninfo.pdata->booster(enable);
}

static int host_notifier_probe(struct platform_device *pdev)
{
	int ret = 0;

	if (pdev && pdev->dev.platform_data)
		ninfo.pdata = pdev->dev.platform_data;
	else {
		pr_err("host_notifier: platform_data is null.\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "notifier_prove\n");

	ninfo.pdata->ndev.set_booster = host_notifier_booster;
	ninfo.pdata->usbhostd_start = start_usbhostd_notify;
	ninfo.pdata->usbhostd_stop = stop_usbhostd_notify;

	ret = host_notify_dev_register(&ninfo.pdata->ndev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to host_notify_dev_register\n");
		return ret;
	}

	return 0;
}

static int host_notifier_remove(struct platform_device *pdev)
{
	host_notify_dev_unregister(&ninfo.pdata->ndev);
	return 0;
}

static struct platform_driver host_notifier_driver = {
	.probe		= host_notifier_probe,
	.remove		= host_notifier_remove,
	.driver		= {
		.name	= "host_notifier",
		.owner	= THIS_MODULE,
	},
};


static int __init host_notifier_init(void)
{
	return platform_driver_register(&host_notifier_driver);
}

static void __init host_notifier_exit(void)
{
	platform_driver_unregister(&host_notifier_driver);
}

module_init(host_notifier_init);
module_exit(host_notifier_exit);

MODULE_AUTHOR("Hyuk Kang <hyuk78.kang@samsung.com>");
MODULE_DESCRIPTION("USB Host notifier");
MODULE_LICENSE("GPL");
