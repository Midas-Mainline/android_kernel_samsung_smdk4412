// SPDX-License-Identifier: GPL-2.0+
//
// HSIC interface for modems speaking
// Samsung's IPC v4.x protocol
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
//
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/sipc.h>
#include <linux/skbuff.h>

#define USB_EP_FMT 0
#define USB_EP_RAW 1
#define USB_EP_RFS 2
#define USB_EP_CMD 3

struct sipc_usb_ep {
	int ep;
	struct sk_buff_head tx_q;

	struct urb *in_urb;
	unsigned char *in_buf;
	size_t in_buf_size;

	u8 bulk_in_addr;
	u8 bulk_out_addr;

	struct sipc_link link_ops;
	struct sipc_link_callback *cb;
	struct usb_device *udev;
	struct usb_interface *data_intf;
};
#define linkops_to_ep(ops) container_of(ops, struct sipc_usb_ep, link_ops)

static int sipc_start_rx(struct sipc_usb_ep *ep);
static int sipc_link_transmit(struct sipc_link *link, struct sk_buff *skb);

static unsigned int usb_to_sipc_format(int ep) {
	switch (ep) {
	case USB_EP_FMT:
		return SAMSUNG_IPC_FORMAT_FMT;
	case USB_EP_RAW:
		return SAMSUNG_IPC_FORMAT_RAW;
	case USB_EP_RFS:
		return SAMSUNG_IPC_FORMAT_RFS;
	case USB_EP_CMD:
		return SAMSUNG_IPC_FORMAT_CMD;
	}

	return -1;
}

static void sipc_rx_complete(struct urb *urb)
{
	struct sipc_usb_ep *ep = urb->context;
	int format = usb_to_sipc_format(ep->ep);
	if (format == SAMSUNG_IPC_FORMAT_RAW)
		format = SAMSUNG_IPC_FORMAT_MULTI_RAW;

	if (!urb->status) {
		ep->cb->receive(ep->cb, urb->transfer_buffer,
				urb->actual_length, format);
		sipc_start_rx(ep);
	}
}

static int sipc_start_rx(struct sipc_usb_ep *ep)
{
	struct urb *urb;
	int ret;

	urb = ep->in_urb;
	urb->transfer_flags = 0;
	usb_fill_bulk_urb(urb, ep->udev, ep->bulk_in_addr,
			ep->in_buf, ep->in_buf_size, sipc_rx_complete,
			ep);

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret)
		dev_err(&ep->udev->dev, "Failed to submit rx urb: %d\n", ret);

	return ret;
}

static void sipc_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	dev_kfree_skb_any(skb);
	usb_free_urb(urb);
}

static int sipc_link_transmit(struct sipc_link *link, struct sk_buff *skb)
{
	struct sipc_usb_ep *ep = linkops_to_ep(link);
	struct urb *urb;
	int ret;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	urb->transfer_flags = URB_ZERO_PACKET;
	usb_fill_bulk_urb(urb, ep->udev, ep->bulk_out_addr, skb->data, skb->len,
			sipc_tx_complete, skb);

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&ep->udev->dev, "Failed to submit URB: %d\n", ret);
		usb_free_urb(urb);
		return ret;
	}

	return 0;
}

static int sipc_link_open(struct sipc_link *link, int channel, int format)
{
	struct sipc_usb_ep *ep = linkops_to_ep(link);
	struct sk_buff *skb;
	char data = 'a';
	int ret = 0;

	/* TODO: runtime PM support */
	if (channel == 0x1) {

		skb = alloc_skb(16, GFP_KERNEL);
		if (unlikely(!skb))
			return -ENOMEM;

		memcpy(skb_put(skb, sizeof(data)), &data, sizeof(data));

		ret = sipc_link_transmit(&ep->link_ops, skb);
		if (ret < 0) {
			dev_kfree_skb_any(skb);
		}
	}
	return ret;
}

static void sipc_set_callbacks(struct sipc_link *link,
		struct sipc_link_callback *cb)
{
	struct sipc_usb_ep *ep = linkops_to_ep(link);
	ep->cb = cb;
}

static int sipc_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	int ret, buflen;
	struct sipc_usb_ep *ep;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	struct usb_cdc_union_desc *union_header;
	struct usb_cdc_parsed_header h;
	struct usb_driver *usbdrv = to_usb_driver(intf->dev.driver);
	struct usb_interface *data_intf;
	unsigned char *buf;
	unsigned int fmt;

	buflen = intf->altsetting->extralen;
	buf = intf->altsetting->endpoint->extra;

	if (!buflen) {
		if (intf->cur_altsetting->endpoint) {
			buflen = intf->cur_altsetting->endpoint->extralen;
			buf = intf->cur_altsetting->endpoint->extra;
		}
	}

	if (!buflen || !buf) {
		dev_err(&intf->dev, "No descriptor reference\n");
		return -EINVAL;
	}

	cdc_parse_cdc_header(&h, intf, buf, buflen);
	union_header = h.usb_cdc_union_desc;

	ep = devm_kzalloc(&intf->dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ep->udev = usb_get_dev(interface_to_usbdev(intf));

	data_intf = usb_ifnum_to_if(ep->udev, union_header->bSlaveInterface0);
	if (!data_intf || !data_intf->altsetting) {
		dev_err(&intf->dev, "Couldn't find data interface!\n");
		return -EINVAL;
	}

	ret = usb_find_common_endpoints(data_intf->altsetting,
			&bulk_in, &bulk_out, NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "Couldn't find bulk-in and bulk-out endpoints: %d\n", ret);
		return ret;
	}

	ep->data_intf = data_intf;
	ep->in_buf_size = usb_endpoint_maxp(bulk_in);
	ep->bulk_in_addr = bulk_in->bEndpointAddress;
	ep->in_buf = devm_kzalloc(&intf->dev, ep->in_buf_size, GFP_KERNEL);
	if (!ep->in_buf)
		return -ENOMEM;
	ep->in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ep->in_urb)
		return -ENOMEM;

	ep->bulk_out_addr = bulk_out->bEndpointAddress;
	/* divide by two because each interface has rx + tx endpoints */
	ep->ep = intf->altsetting->desc.bInterfaceNumber / 2;

	ret = usb_driver_claim_interface(usbdrv, data_intf, ep);
	if (ret) {
		dev_err(&intf->dev, "Failed to claim interface\n");
		goto urb_free;
	}

	fmt = usb_to_sipc_format(ep->ep);
	if (fmt < 0) {
		dev_warn(&intf->dev, "Unknown EP format %d\n", ep->ep);
		return 0;
	}

	ep->link_ops.transmit = sipc_link_transmit;
	ep->link_ops.open = sipc_link_open;
	ep->link_ops.set_callbacks = sipc_set_callbacks;

	ret = sipc_set_link(&ep->link_ops, fmt);
	if (ret < 0) {
		dev_err(&intf->dev, "Failed to set SIPC link for fmt %u: %d\n", fmt, ret);
		return ret;
	}

	if (fmt == SAMSUNG_IPC_FORMAT_RAW) {
		ret = sipc_set_link(&ep->link_ops, SAMSUNG_IPC_FORMAT_MULTI_RAW);
		if (ret < 0) {
			dev_err(&intf->dev, "Failed to set SIPC link for MULTI_RAW: %d\n", ret);
			return ret;
		}
	}

	sipc_start_rx(ep);
	return 0;
urb_free:
	usb_free_urb(ep->in_urb);
	return ret;
}

static void sipc_disconnect(struct usb_interface *intf)
{
	struct sipc_usb_ep *ep = usb_get_intfdata(intf);
	int fmt;

	fmt = usb_to_sipc_format(ep->ep);
	if (fmt < 0)
		goto invalid_format;

	sipc_clear_link(fmt);
	if (fmt == SAMSUNG_IPC_FORMAT_RAW)
		sipc_clear_link(SAMSUNG_IPC_FORMAT_MULTI_RAW);

invalid_format:
	usb_free_urb(ep->in_urb);
}

static struct usb_device_id sipc_idtable[] = {
	/* Infineon XMM626x */
	{USB_DEVICE_AND_INTERFACE_INFO(0x1519, 0x0020,
			USB_CLASS_COMM, USB_CDC_SUBCLASS_ACM, USB_CDC_ACM_PROTO_AT_V25TER)},
	{},
};

static struct usb_driver sipc_hsic_driver = {
	.name = "sipc_hsic",
	.probe = sipc_probe,
	.disconnect = sipc_disconnect,
	.id_table = sipc_idtable,
	.supports_autosuspend = 0,
};

module_usb_driver(sipc_hsic_driver);

MODULE_DEVICE_TABLE(usb, sipc_idtable);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Shields <simon@lineageos.org>");
