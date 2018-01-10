// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for XMM6262 as found on Samsung GT-I9300, GT-N7100.
 *
 * Copyright (C) 2018 Simon Shields <simon@lineageos.org>
 */
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>

#ifdef CONFIG_USB_DYNAMIC_MINORS
#define USB_XMM6262_MINOR_BASE 0
#else
#define USB_XMM6262_MINOR_BASE 191 /* TODO */
#endif

struct xmm6262 {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct usb_interface *data_intf;

	struct urb *bulk_in_urb;
	unsigned char *bulk_in_buf;
	size_t bulk_in_size;
	u8 bulk_in_addr;
	u8 bulk_out_addr;

	struct sk_buff_head in_queue;

	/*
	 * should be held whenever
	 * bulk_in_buf and bulk_in_urb
	 * are in use
	 */
	struct mutex rx_mutex;
	/*
	 * should be held while
	 * doing any file op
	 */
	struct mutex io_mutex;
};

static struct usb_driver xmm6262_boot_driver;
static int xmm6262_boot_start_rx(struct xmm6262 *dev);

static void xmm6262_boot_rx_callback(struct urb *urb)
{
	struct xmm6262 *dev = urb->context;
	struct sk_buff *skb;
	int ret = 0;

	if (urb->status) {
		if (urb->status != -ENOENT &&
				urb->status != -ECONNRESET &&
				urb->status != -ESHUTDOWN)
			dev_err(&dev->intf->dev, "nonzero bulk read status: %d\n", urb->status);
		ret = urb->status;
		goto error;
	} else if (urb->actual_length) {
		dev_info(&dev->intf->dev, "queued %d bytes of data\n", urb->actual_length);
		skb = alloc_skb(urb->actual_length, GFP_ATOMIC);
		if (!skb) {
			dev_err(&dev->intf->dev, "failed to allocate skb\n");
			ret = -ENOMEM;
			goto error;
		}
		skb_put_data(skb, dev->bulk_in_buf, urb->actual_length);
		skb_queue_tail(&dev->in_queue, skb);
	} /* skip 0-length URBs */

	mutex_unlock(&dev->rx_mutex);
	xmm6262_boot_start_rx(dev);
	return;
error:
	mutex_unlock(&dev->rx_mutex);
	// TODO: do something with ret

}

static int xmm6262_boot_start_rx(struct xmm6262 *dev)
{
	struct urb *urb = dev->bulk_in_urb;

	int ret = mutex_trylock(&dev->rx_mutex);
	if (!ret) {
		dev_warn(&dev->intf->dev, "already got pending RX job!\n");
		return -EBUSY;
	}

	if (!dev->intf)
		return -ENODEV;

	urb->transfer_flags = 0;
	usb_fill_bulk_urb(urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_addr),
			dev->bulk_in_buf,
			dev->bulk_in_size,
			xmm6262_boot_rx_callback,
			dev);

	ret = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "failed to submit rx urb: %d\n", ret);
		ret = (ret == -ENOMEM) ? ret : -EIO;
		mutex_unlock(&dev->rx_mutex);
		/* TODO: we need a way to recover from this */
	}

	return ret;
}

static int xmm6262_boot_open(struct inode *inode, struct file *file)
{
	struct xmm6262 *dev;
	struct usb_interface *intf;
	int subminor, ret = 0;

	subminor = iminor(inode);

	intf = usb_find_interface(&xmm6262_boot_driver, subminor);
	if (!intf) {
		pr_err("%s: can't find device for minor %d\n",
				__func__, subminor);
		return -ENODEV;
	}

	dev = usb_get_intfdata(intf);
	if (!dev)
		return -ENODEV;

	/* begin receiving data */
	xmm6262_boot_start_rx(dev);

	file->private_data = dev;
	return 0;
}

static int xmm6262_boot_release(struct inode *inode, struct file *file)
{
	/* TODO stop RX */
	return 0;
}

static ssize_t xmm6262_boot_read(struct file *file, char *buf, size_t count,
		loff_t *ppos)
{
	struct xmm6262 *dev = file->private_data;

	struct sk_buff *skb = NULL;
	int to_read = count;
	int read = 0;
	int ret, copy_len;

	if (file->f_flags & O_NONBLOCK) {
		ret = mutex_trylock(&dev->io_mutex);
		if (!ret) {
			dev_info(&dev->intf->dev, "already got another reader!\n");
			return -EAGAIN;
		}
	} else {
		ret = mutex_lock_interruptible(&dev->io_mutex);
		if (ret < 0)
			return ret;
	}

	ret = 0;

	while (to_read) {
		dev_info(&dev->intf->dev, "%d bytes left to read\n", to_read);
		skb = skb_dequeue(&dev->in_queue);
		if (!skb) {
			/* no data waiting:
			 * start rx again in case it failed for some reason */
			xmm6262_boot_start_rx(dev);
			break;
		}

		if (skb->len > to_read) {
			if (copy_to_user(buf + read, skb->data, to_read)) {
				dev_kfree_skb_any(skb);
				ret = -EFAULT;
				goto end;
			}

			read += to_read;
			to_read = 0;
			skb_pull(skb, to_read);
			dev_info(&dev->intf->dev, "new skb length = %d\n", skb->len);
			skb_queue_head(&dev->in_queue, skb);
		} else {
			/* copy just skb->len */
			if (copy_to_user(buf + read, skb->data, skb->len)) {
				dev_kfree_skb_any(skb);
				ret = -EFAULT;
				goto end;
			}
			read += skb->len;
			to_read -= skb->len;
			dev_kfree_skb_any(skb);
		}
	}
	dev_info(&dev->intf->dev, "read %d bytes!\n", read);
	ret = read;
end:
	mutex_unlock(&dev->io_mutex);
	return ret;
}

void xmm6262_boot_write_callback(struct urb *urb)
{
	struct xmm6262 *dev = urb->context;

	if (urb->status) {
		if (urb->status != -ENOENT &&
				urb->status != -ECONNRESET &&
				urb->status != -ESHUTDOWN)
			dev_err(&dev->intf->dev, "nonzero write bulk status received: %d\n", urb->status);
	}

	dev_info(&dev->intf->dev, "wrote %d bytes of data\n", urb->actual_length);
	kfree(urb->transfer_buffer);
	usb_free_urb(urb);
}

static ssize_t xmm6262_boot_write(struct file *file, const char *user_buf, size_t count,
		loff_t *ppos)
{
	struct xmm6262 *dev = file->private_data;
	struct urb *urb = NULL;
	char *buf = NULL;
	int ret;

	if (unlikely(count == 0))
		return count;

	if (file->f_flags & O_NONBLOCK) {
		ret = mutex_trylock(&dev->io_mutex);
		if (!ret) {
			dev_info(&dev->intf->dev, "already got another writer/reader!\n");
			return -EAGAIN;
		}
	} else {
		ret = mutex_lock_interruptible(&dev->io_mutex);
		if (ret < 0)
			return ret;
	}

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		ret = -ENOMEM;
		goto error;
	}

	buf = kzalloc(count, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	if (copy_from_user(buf, user_buf, count)) {
		ret = -EFAULT;
		goto err_copy;
	}

	urb->transfer_flags = URB_ZERO_PACKET;
	usb_fill_bulk_urb(urb, dev->udev,
			usb_sndbulkpipe(dev->udev,
				dev->bulk_out_addr),
			buf, count,
			xmm6262_boot_write_callback, dev);

	ret = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (ret) {
		dev_err(&dev->intf->dev, "failed submitting write urb: %d\n", ret);
		return ret;
	}

	return count;

err_copy:
	kfree(buf);
err_alloc:
	usb_free_urb(urb);
error:
	mutex_unlock(&dev->io_mutex);
	return ret;
}

static unsigned int xmm6262_boot_poll(struct file *file, struct poll_table_struct *p)
{
	struct xmm6262 *dev = file->private_data;

	if (!skb_queue_empty(&dev->in_queue)) {
		dev_info(&dev->intf->dev, "skb queue len: %u - head len %u\n", skb_queue_len(&dev->in_queue),
			skb_peek(&dev->in_queue)->len);
		return POLLIN;
	} else {
		dev_err(&dev->intf->dev, "skb queue len: %u\n", skb_queue_len(&dev->in_queue));
	}

	return 0;
}

static const struct file_operations xmm6262_boot_fops = {
	.owner = THIS_MODULE,
	.read = xmm6262_boot_read,
	.write = xmm6262_boot_write,
	.poll = xmm6262_boot_poll,
	.open = xmm6262_boot_open,
	.release = xmm6262_boot_release,
};

static struct usb_class_driver xmm6262_boot_class = {
	.name = "xmm6262_boot%d",
	.fops = &xmm6262_boot_fops,
	.minor_base = USB_XMM6262_MINOR_BASE,
};

static int xmm6262_probe(struct usb_interface *intf,
		const struct usb_device_id *id)
{
	int ret, buflen;
	struct xmm6262 *dev;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	struct usb_cdc_union_desc *union_header = NULL;
	struct usb_cdc_parsed_header h;
	struct usb_interface *data_intf;
	struct usb_driver *usbdrv = to_usb_driver(intf->dev.driver);
	unsigned char *buf;

	buflen = intf->altsetting->extralen;
	buf = intf->altsetting->endpoint->extra;

	if (!buflen) {
		if (intf->cur_altsetting->endpoint &&
				intf->cur_altsetting->endpoint->extralen &&
				intf->cur_altsetting->endpoint->extra) {
			buflen = intf->cur_altsetting->endpoint->extralen;
			buf = intf->cur_altsetting->endpoint->extra;
		} else {
			dev_err(&intf->dev, "No descriptor reference\n");
			return -EINVAL;
		}
	}

	cdc_parse_cdc_header(&h, intf, buf, buflen);
	union_header = h.usb_cdc_union_desc;

	dev = devm_kzalloc(&intf->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = usb_get_dev(interface_to_usbdev(intf));
	dev->intf = intf;
	data_intf = usb_ifnum_to_if(dev->udev,
			union_header->bSlaveInterface0);
	if (!data_intf || !data_intf->altsetting) {
		dev_err(&intf->dev, "couldn't find data interface!\n");
		return -ENODEV;
	}
	skb_queue_head_init(&dev->in_queue);

	dev->data_intf = data_intf;
	usb_driver_claim_interface(usbdrv, data_intf, dev);
	usb_set_intfdata(intf, dev);

	ret = usb_find_common_endpoints(data_intf->altsetting,
			&bulk_in, &bulk_out, NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "Couldn't find bulk-in and bulk-out EPs\n");
		return -EINVAL;
	}

	ret = usb_register_dev(intf, &xmm6262_boot_class);
	if (ret) {
		return ret;
	}

	dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
	dev->bulk_in_addr = bulk_in->bEndpointAddress;
	dev->bulk_in_buf = devm_kzalloc(&intf->dev, dev->bulk_in_size, GFP_KERNEL);
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb)
		return -ENOMEM;

	dev->bulk_out_addr = bulk_out->bEndpointAddress;

	dev_info(&intf->dev, "Loaded XMM6262 boot serial device!\n");

	mutex_init(&dev->rx_mutex);
	mutex_init(&dev->io_mutex);

	return 0;
}

static void xmm6262_disconnect(struct usb_interface *intf)
{
	struct xmm6262 *dev = usb_get_intfdata(intf);
	struct usb_driver *usbdrv = to_usb_driver(intf->dev.driver);
	
	mutex_lock(&dev->rx_mutex);
	/* time to stop! */
	dev->intf = NULL;
	mutex_unlock(&dev->rx_mutex);

	usb_driver_release_interface(usbdrv, dev->data_intf);
	usb_deregister_dev(intf, &xmm6262_boot_class);

}

static struct usb_device_id xmm6262_idtable[] = {
	/* Infineon XMM626x */
	/* Initially appears as a CDC device that needs firmware uploaded before boot will continue */
	{USB_DEVICE_AND_INTERFACE_INFO(0x058b, 0x0041,
			USB_CLASS_COMM, USB_CDC_SUBCLASS_ACM, USB_CDC_PROTO_NONE)},
	{}
};

static struct usb_driver xmm6262_boot_driver = {
	.name = "xmm6262",
	.probe = xmm6262_probe,
	.disconnect = xmm6262_disconnect,
	//.suspend = xmm6262_suspend,
	//.resume = xmm6262_resume,
	//.reset_resume = xmm6262_reset_resume,
	.id_table = xmm6262_idtable,
	.supports_autosuspend = 0,
};

module_usb_driver(xmm6262_boot_driver);

MODULE_DEVICE_TABLE(usb, xmm6262_idtable);

MODULE_LICENSE("GPL");
