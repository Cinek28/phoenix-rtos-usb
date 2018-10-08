/*
 * Phoenix-RTOS
 *
 * USB Host driver
 *
 * Copyright 2018 Phoenix Systems
 * Author: Jan Sikorski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>
#include <sys/interrupt.h>
#include <sys/threads.h>
#include <sys/platform.h>
#include <sys/list.h>
#include <sys/rb.h>
#include <sys/msg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "dma.h"
#include "ehci.h"
#include "usb.h"
#include "usbd.h"


typedef struct usb_request {
	struct usb_request *next, *prev;
	usb_urb_t urb;
} usb_request_t;


typedef struct {
	rbnode_t linkage;
	unsigned pid;
	unsigned port;
	usb_device_id_t filter;
	usb_request_t *requests;
} usb_driver_t;


typedef struct usb_endpoint {
	struct usb_endpoint *next, *prev;
	endpoint_desc_t *descriptor;

	int speed;
	// int transfer;
	int max_packet_len;
	int number;
	int token;
} usb_endpoint_t;


typedef struct usb_device {
	struct usb_device *next, *prev;

	device_desc_t *descriptor;
	char address;

	int num_endpoints;
	usb_endpoint_t *endpoints;
} usb_device_t;


typedef struct usb_queue {
	struct usb_queue *next, *prev;

	struct usb_transfer *transfers;
	struct usb_device *device;
	struct usb_endpoint *endpoint;

	struct qh *qh;
} usb_queue_t;


typedef struct usb_transfer {
	struct usb_transfer *next, *prev;

	struct qtd *qtd;
} usb_transfer_t;


static struct {
	usb_queue_t *queues;
	usb_queue_t *async_head;
	usb_device_t *devices;

	rbtree_t drivers;
	unsigned port;
} usbd_common;


usb_queue_t *usb_allocQueue(usb_device_t *dev, usb_endpoint_t *ep, int transfer)
{
	usb_queue_t *result = malloc(sizeof(usb_queue_t));

	if (result == NULL)
		return NULL;

	if ((result->qh = ehci_allocQh(dev->address, ep->number, transfer, ep->speed, ep->max_packet_len)) == NULL) {
		free(result);
		return NULL;
	}

	result->device = dev;
	result->endpoint = ep;
	result->next = result->prev = NULL;
	result->transfers = NULL;
	result->prev = result->next = NULL;
	return result;
}


usb_transfer_t *usb_appendTransfer(usb_queue_t *queue, int token, char *buffer, size_t *size, int datax)
{
	usb_transfer_t *transfer = malloc(sizeof(usb_transfer_t));

	if (transfer == NULL)
		return NULL;

	if ((transfer->qtd = ehci_allocQtd(token, buffer, size, datax)) == NULL) {
		free(transfer);
		return NULL;
	}

	LIST_ADD(&queue->transfers, transfer);
	return transfer;
}


void usb_linkTransfers(usb_queue_t *queue)
{
	usb_transfer_t *transfer = queue->transfers->prev;
	transfer->qtd->ioc = 1;

	do {
		ehci_consQtd(transfer->qtd, queue->qh);
		transfer = transfer->prev;
	} while (transfer != queue->transfers->prev);
}


void usb_deleteUnlinkedQueue(usb_queue_t *queue)
{
	usb_transfer_t *transfer;

	while ((transfer = queue->transfers) != NULL) {
		ehci_freeQtd(transfer->qtd);
		LIST_REMOVE(&queue->transfers, transfer);
		free(transfer);
	}

	ehci_freeQh(queue->qh);
	free(queue);
}


void usb_deleteQueue(usb_queue_t *queue)
{
	ehci_unlinkQh(queue->prev->qh, queue->qh, queue->next->qh);
	LIST_REMOVE(&usbd_common.queues, queue);
	usb_deleteUnlinkedQueue(queue);
}


void usb_linkAsync(usb_queue_t *queue)
{
	LIST_ADD(&usbd_common.queues, queue);

	if (queue == queue->next) {
		ehci_linkQh(queue->qh, queue->qh);
	}
	else {
		ehci_linkQh(queue->qh, queue->next->qh);
		ehci_linkQh(queue->prev->qh, queue->qh);
	}
}


int usb_control(usb_device_t *dev, usb_endpoint_t *ep, setup_packet_t *packet, void *buffer, int ssize_in)
{
	usb_queue_t *queue;
	int data_token, control_token;
	size_t size;
	int dt;

	data_token = ssize_in <= 0 ? out_token : in_token;
	control_token = data_token == out_token ? in_token : out_token;

	if ((queue = usb_allocQueue(dev, ep, transfer_control)) == NULL)
		return -ENOMEM;

	size = sizeof(setup_packet_t);

	if (usb_appendTransfer(queue, setup_token, (char *)packet, &size, 0) == NULL) {
		usb_deleteUnlinkedQueue(queue);
		return -ENOMEM;
	}

	dt = 1;
	size = abs(ssize_in);

	while (size) {
		if (usb_appendTransfer(queue, data_token, buffer, &size, dt) == NULL) {
			usb_deleteUnlinkedQueue(queue);
			return -ENOMEM;
		}

		dt = !dt;
	}

	if (usb_appendTransfer(queue, control_token, NULL, NULL, 1) == NULL) {
		usb_deleteUnlinkedQueue(queue);
		return -ENOMEM;
	}

	usb_linkTransfers(queue);
	usb_linkAsync(queue);
	return EOK;
}


int usb_setAddress(usb_device_t *dev, usb_endpoint_t *ep, unsigned char address)
{
	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE;
	setup->bRequest = SET_ADDRESS;
	setup->wValue = address;
	setup->wIndex = 0;
	setup->wLength = 0;

	retval = usb_control(dev, ep, setup, NULL, 0);
	ehci_await(USB_TIMEOUT);
	usb_deleteQueue(usbd_common.queues);
	dma_free64(setup);

	return retval;
}


int usb_getDescriptor(usb_device_t *dev, usb_endpoint_t *ep, int descriptor, int index, char *buffer, int size)
{
	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE;
	setup->bRequest = GET_DESCRIPTOR;
	setup->wValue = descriptor << 8 | index;
	setup->wIndex = 0;
	setup->wLength = size;

	retval = usb_control(dev, ep, setup, buffer, size);
	ehci_await(USB_TIMEOUT);
	usb_deleteQueue(usbd_common.queues);
	dma_free64(setup);

	return retval;
}


int usb_getConfigurationDescriptor(usb_device_t *dev, usb_endpoint_t *ep, configuration_desc_t *desc, int index, int length)
{
	return usb_getDescriptor(dev, ep, DESC_CONFIG, index, (char *)desc, length);
}


int usb_getInterfaceDescriptor(usb_device_t *dev, usb_endpoint_t *ep, interface_desc_t *desc, int index)
{
	return usb_getDescriptor(dev, ep, DESC_INTERFACE, index, (char *)desc, sizeof(*desc));
}


int usb_getStringDescriptor(usb_device_t *dev, usb_endpoint_t *ep, string_desc_t *desc, int index)
{
	return usb_getDescriptor(dev, ep, DESC_STRING, index, (char *)desc, sizeof(*desc));
}


int usb_getEndpointDescriptor(usb_device_t *dev, usb_endpoint_t *ep, endpoint_desc_t *desc, int index)
{
	return usb_getDescriptor(dev, ep, DESC_ENDPOINT, index, (char *)desc, sizeof(*desc));
}


int usb_getDeviceDescriptor(usb_device_t *dev, usb_endpoint_t *ep, device_desc_t *desc)
{
	return usb_getDescriptor(dev, ep, DESC_DEVICE, 0, (char *)desc, sizeof(*desc));
}


int usb_setConfiguration(usb_device_t *dev, usb_endpoint_t *ep, int value)
{
	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE;
	setup->bRequest = SET_CONFIGURATION;
	setup->wValue = value;
	setup->wIndex = 0;
	setup->wLength = 0;

	retval = usb_control(dev, ep, setup, NULL, 0);
	ehci_await(USB_TIMEOUT);
	usb_deleteQueue(usbd_common.queues);
	dma_free64(setup);

	return retval;
}


int usb_setInterface(usb_device_t *dev, usb_endpoint_t *ep, int alt, int index)
{
	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_INTERFACE;
	setup->bRequest = SET_INTERFACE;
	setup->wValue = alt;
	setup->wIndex = index;
	setup->wLength = 0;

	retval = usb_control(dev, ep, setup, NULL, 0);
	ehci_await(USB_TIMEOUT);
	usb_deleteQueue(usbd_common.queues);
	dma_free64(setup);

	return retval;
}


int usb_bulk(usb_device_t *dev, usb_endpoint_t *ep, int token, void *buffer, size_t size)
{
	usb_queue_t *queue;

	if ((queue = usb_allocQueue(dev, ep, transfer_bulk)) == NULL)
		return -ENOMEM;

	while (size) {
		if (usb_appendTransfer(queue, token, buffer, &size, 0) == NULL) {
			usb_deleteUnlinkedQueue(queue);
			return -ENOMEM;
		}
	}

	usb_linkTransfers(queue);
	usb_linkAsync(queue);

	ehci_await(USB_TIMEOUT);
	usb_deleteQueue(queue);
	return EOK;
}


void usb_dumpDeviceDescriptor(FILE *stream, device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: %d\n", descr->idVendor);
	fprintf(stream, "\tidProduct: %d\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


void usb_dumpConfigurationDescriptor(FILE *stream, configuration_desc_t *desc)
{
	fprintf(stream, "CONFIGURATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", desc->bDescriptorType);
	fprintf(stream, "\twTotalLength: %d\n", desc->wTotalLength);
	fprintf(stream, "\tbNumInterfaces: %d\n", desc->bNumInterfaces);
	fprintf(stream, "\tbConfigurationValue: %d\n", desc->bConfigurationValue);
	fprintf(stream, "\tiConfiguration: %d\n", desc->iConfiguration);
	fprintf(stream, "\tbmAttributes: %d\n", desc->bmAttributes);
	fprintf(stream, "\tbMaxPower: %d\n", desc->bMaxPower);
}


void usb_dumpInterfaceDescriptor(FILE *stream, interface_desc_t *desc)
{
	fprintf(stream, "INTERFACE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", desc->bDescriptorType);
	fprintf(stream, "\tbInterfaceNumber: %d\n", desc->bInterfaceNumber);
	fprintf(stream, "\tbAlternateSetting: %d\n", desc->bAlternateSetting);
	fprintf(stream, "\tbNumEndpoints: %d\n", desc->bNumEndpoints);
	fprintf(stream, "\tbInterfaceClass: %d\n", desc->bInterfaceClass);
	fprintf(stream, "\tbInterfaceSubClass: %d\n", desc->bInterfaceSubClass);
	fprintf(stream, "\tbInterfaceProtocol: %d\n", desc->bInterfaceProtocol);
	fprintf(stream, "\tiInterface: %d\n", desc->iInterface);
}


void usb_dumpEndpointDescriptor(FILE *stream, endpoint_desc_t *desc)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", desc->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", desc->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: %d\n", desc->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", desc->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", desc->bInterval);
}


void usb_dumpDescriptor(FILE *stream, struct desc_header *desc)
{
	switch (desc->bDescriptorType) {
	case DESC_CONFIG:
		usb_dumpConfigurationDescriptor(stream, (void *)desc);
		break;

	case DESC_DEVICE:
		usb_dumpDeviceDescriptor(stream, (void *)desc);
		break;

	case DESC_INTERFACE:
		usb_dumpInterfaceDescriptor(stream, (void *)desc);
		break;

	case DESC_ENDPOINT:
		usb_dumpEndpointDescriptor(stream, (void *)desc);
		break;

	default:
		printf("UNRECOGNIZED DESCRIPTOR (%d)\n", desc->bDescriptorType);
		break;
	}
}


void usb_dumpConfiguration(FILE *stream, configuration_desc_t *desc)
{
	int remaining_size = desc->wTotalLength;
	struct desc_header *header = (void *)desc;

	while (remaining_size > 0) {
		usb_dumpDescriptor(stream, header);

		if (!header->bLength)
			break;

		remaining_size -= header->bLength;
		header = (struct desc_header *)((char *)header + header->bLength);
	}
}


void usb_deviceAttach(void)
{
	usb_device_t *dev;
	usb_endpoint_t *ep;
	void *buf;
	size_t cfgsz;

	device_desc_t *ddesc = dma_alloc64();
	configuration_desc_t *cdesc = dma_alloc64();

	ehci_resetPort();
	ehci_await(USB_TIMEOUT);

	dev = usbd_common.devices = calloc(1, sizeof(usb_device_t));
	ep = dev->endpoints = calloc(1, sizeof(usb_endpoint_t));

	ep->number = 0;
	ep->speed = high_speed;
	ep->max_packet_len = 64;

	usb_getDeviceDescriptor(dev, ep, ddesc);
	ehci_resetPort();
	ehci_await(0);

	dev->descriptor = ddesc;
	ep->max_packet_len = ddesc->bMaxPacketSize0;

	usb_dumpDeviceDescriptor(stdout, ddesc);

	usb_setAddress(dev, ep, 1);
	dev->address = 1;

	usb_getConfigurationDescriptor(dev, ep, cdesc, 0, sizeof(configuration_desc_t));
	cfgsz = (cdesc->wTotalLength + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1);
	buf = mmap(NULL, cfgsz, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);
	usb_getConfigurationDescriptor(dev, ep, buf, 0, cdesc->wTotalLength);

	dma_free64(ddesc);
	dma_free64(cdesc);
	munmap(buf, cfgsz);
	return;
}


void usb_deviceDetach(void)
{
	printf("detach\n");
}


void usb_handleEvents(int port_change)
{
	if (port_change && ehci_deviceAttached()) {
		usb_deviceAttach();
		return;
	}
}


int usb_driver_cmp(rbnode_t *n1, rbnode_t *n2)
{
	usb_driver_t *d1 = lib_treeof(usb_driver_t, linkage, n1);
	usb_driver_t *d2 = lib_treeof(usb_driver_t, linkage, n2);

	if (d1->port > d2->port)
		return 1;
	else if (d1->port < d2->port)
		return -1;

	return 0;
}


int usb_connect(usb_connect_t *c, unsigned pid)
{
	usb_driver_t *driver = malloc(sizeof(*driver));

	if (driver == NULL)
		return -ENOMEM;

	driver->port = c->port;
	driver->filter = c->filter;
	driver->pid = pid;
	driver->requests = NULL;

	lib_rbInsert(&usbd_common.drivers, &driver->linkage);
	return EOK;
}


int usb_urb(usb_urb_t *u, msg_t *msg)
{
	usb_driver_t find, *driver;
	usb_device_t *device;
	usb_endpoint_t *endpoint;
	setup_packet_t *setup;
	int err;

	find.pid = msg->pid;
	if ((driver = lib_treeof(usb_driver_t, linkage, lib_rbFind(&usbd_common.drivers, &find.linkage))) == NULL)
		return -EINVAL;

	switch (u->type) {
	case usb_transfer_control:
		setup = dma_alloc64();
		*setup = u->setup;
		if (msg->i.size)
			usb_control(device, endpoint, setup, msg->i.data, -msg->i.size);
		else if (msg->o.size)
			usb_control(device, endpoint, setup, msg->o.data, msg->o.size);
		else
			usb_control(device, endpoint, setup, NULL, 0);
		err = ehci_await(USB_TIMEOUT);
		dma_free64(setup);
		break;

	case usb_transfer_bulk:
		if (msg->i.size)
			usb_bulk(device, endpoint, out_token, msg->i.data, msg->i.size);

		if (msg->o.size)
			usb_bulk(device, endpoint, in_token, msg->o.data, msg->o.size);

		err = EOK;
		break;

	case usb_transfer_interrupt:
	case usb_transfer_isochronous:
	default:
		err = -ENOSYS;
	}

	return err;
}


void msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned rid;
	msg_t msg;
	usb_msg_t *umsg;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;

		if (msg.type == mtDevCtl) {
			umsg = (void *)msg.i.raw;

			switch (umsg->type) {
			case usb_msg_connect:
				msg.o.io.err = usb_connect(&umsg->connect, msg.pid);
				break;
			case usb_msg_urb:
				msg.o.io.err = usb_urb(&umsg->urb, &msg);
				break;
			}
		}
		else {
			msg.o.io.err = -EINVAL;
		}

		msgRespond(port, &msg, rid);
	}
}


int main(int argc, char **argv)
{
	portCreate(&usbd_common.port);
	usbd_common.queues = NULL;
	usbd_common.devices = NULL;
	lib_rbInit(&usbd_common.drivers, usb_driver_cmp, NULL);

	ehci_init(usb_handleEvents);

	msgthr((void *)usbd_common.port);
	return 0;
}