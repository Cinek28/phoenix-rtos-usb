/*
 * Phoenix-RTOS
 *
 * cdc - USB Communication Device Class
 *
 * Copyright 2019 Phoenix Systems
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <errno.h>
#include <stdio.h>

#include <sys/list.h>

#include <usbclient.h>

#include "cdc_client.h"

struct {
	usb_desc_list_t *descList;

	usb_desc_list_t dev;
	usb_desc_list_t conf;

	usb_desc_list_t comIface;
	usb_desc_list_t header;
	usb_desc_list_t call;
	usb_desc_list_t acm;
	usb_desc_list_t unio;
	usb_desc_list_t comEp;

	usb_desc_list_t dataIface;
	usb_desc_list_t dataEpOUT;
	usb_desc_list_t dataEpIN;

	usb_desc_list_t str0;
	usb_desc_list_t strman;
	usb_desc_list_t strprod;
} cdc_common;


/* Device descriptor */
static const usb_device_desc_t ddev = { .bLength = sizeof(usb_device_desc_t), .bDescriptorType = USB_DESC_DEVICE, .bcdUSB = 0x0002,
	.bDeviceClass = 0x0, .bDeviceSubClass = 0, .bDeviceProtocol = 0, .bMaxPacketSize0 = 64,
	.idVendor = 0x16f9, .idProduct = 0x0003, .bcdDevice = 0x0200,
	.iManufacturer = 1, .iProduct = 2, .iSerialNumber = 0,
	.bNumConfigurations = 1 };


/* Configuration descriptor */
static const usb_configuration_desc_t dconfig = { .bLength = 9, .bDescriptorType = USB_DESC_CONFIG,
	.wTotalLength = sizeof(usb_configuration_desc_t) + sizeof(usb_interface_desc_t) + sizeof(usb_desc_cdc_header_t) + sizeof(usb_desc_cdc_call_t)
	+ sizeof(usb_desc_cdc_acm_t) + sizeof(usb_desc_cdc_union_t) + sizeof(usb_endpoint_desc_t) + sizeof(usb_interface_desc_t) + sizeof(usb_endpoint_desc_t) + sizeof(usb_endpoint_desc_t),
	.bNumInterfaces = 2, .bConfigurationValue = 1, .iConfiguration = 0, .bmAttributes = 0xc0, .bMaxPower = 5 };


/* Communication Interface Descriptor */
static const usb_interface_desc_t dComIntf =  { .bLength = 9, .bDescriptorType = USB_DESC_INTERFACE, .bInterfaceNumber = 0, .bAlternateSetting = 0,
	.bNumEndpoints = 1, .bInterfaceClass = 0x02, .bInterfaceSubClass = 0x02, .bInterfaceProtocol = 0x00, .iInterface = 4 };


static const usb_desc_cdc_header_t dHeader = { .bLength = 5, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0, .bcdCDC = 0x0110 };


static const usb_desc_cdc_call_t dCall = { .bLength = 5, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0x01, .bmCapabilities = 0x01, .bDataInterface = 0x1 };


static const usb_desc_cdc_acm_t dAcm = { .bLength = 4, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0x02, .bmCapabilities = 0x03 };


static const usb_desc_cdc_union_t dUnion = { .bLength = 5, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0x06, .bControlInterface = 0x0, .bSubordinateInterface = 0x1};


/* Communication Interrupt Endpoint IN */
static const usb_endpoint_desc_t dComEp = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x81	, /* direction IN */
	.bmAttributes = 0x03, .wMaxPacketSize = 0x20, .bInterval = 0x08
};


/* CDC Data Interface Descriptor */
static const usb_interface_desc_t dDataIntf = { .bLength = 9, .bDescriptorType = USB_DESC_INTERFACE, .bInterfaceNumber = 1, .bAlternateSetting = 0,
	 .bNumEndpoints = 2, .bInterfaceClass = 0x0a, .bInterfaceSubClass = 0x00, .bInterfaceProtocol = 0x00, .iInterface = 0
};


/* Data Bulk Endpoint OUT */
static const usb_endpoint_desc_t depOUT = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x02, /* direction OUT */
	.bmAttributes = 0x02, .wMaxPacketSize = 0x0200, .bInterval = 0
};


/* Data Bulk Endpoint IN */
static const usb_endpoint_desc_t depIN = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x82, /* direction IN */
	.bmAttributes = 0x02, .wMaxPacketSize = 0x0200, .bInterval = 1
};


/* String Data */
static const usb_string_desc_t dstrman = {
	.bLength = 2 * 18 + 2,
	.bDescriptorType = USB_DESC_STRING,
	.wData = {	'N', 0, 'X', 0, 'P', 0, ' ', 0, 'S', 0, 'E', 0, 'M', 0, 'I', 0, 'C', 0, 'O', 0, 'N', 0, 'D', 0, 'U', 0, 'C', 0, 'T', 0,
				'O', 0, 'R', 0, 'S', 0 }
};


static const usb_string_desc_t dstr0 = {
	.bLength = 4,
	.bDescriptorType = USB_DESC_STRING,
	.wData = {0x04, 0x09} /* English */
};


static const usb_string_desc_t dstrprod = {
	.bLength = 11 * 2 + 2,
	.bDescriptorType = USB_DESC_STRING,
	.wData = { 'M', 0, 'C', 0, 'U', 0, ' ', 0, 'V', 0, 'I', 0, 'R', 0, 'T', 0, 'U', 0, 'A', 0, 'L', 0 }
};


int cdc_recv(int endpt, char *data, unsigned int len)
{
	return usbclient_receive(endpt, data, len);
}


int cdc_send(int endpt, const char *data, unsigned int len)
{
	return usbclient_send(endpt, data, len);
}


void cdc_destroy(void)
{
	usbclient_destroy();
}


int cdc_init(void)
{
	int res = EOK;

	cdc_common.dev.descriptor = (usb_functional_desc_t *)&ddev;
	LIST_ADD(&cdc_common.descList, &cdc_common.dev);

	cdc_common.conf.descriptor = (usb_functional_desc_t *)&dconfig;
	LIST_ADD(&cdc_common.descList, &cdc_common.conf);

	cdc_common.comIface.descriptor = (usb_functional_desc_t *)&dComIntf;
	LIST_ADD(&cdc_common.descList, &cdc_common.comIface);

	cdc_common.header.descriptor = (usb_functional_desc_t *)&dHeader;
	LIST_ADD(&cdc_common.descList, &cdc_common.header);

	cdc_common.call.descriptor = (usb_functional_desc_t *)&dCall;
	LIST_ADD(&cdc_common.descList, &cdc_common.call);

	cdc_common.acm.descriptor = (usb_functional_desc_t *)&dAcm;
	LIST_ADD(&cdc_common.descList, &cdc_common.acm);

	cdc_common.unio.descriptor = (usb_functional_desc_t *)&dUnion;
	LIST_ADD(&cdc_common.descList, &cdc_common.unio);

	cdc_common.comEp.descriptor = (usb_functional_desc_t *)&dComEp;
	LIST_ADD(&cdc_common.descList, &cdc_common.comEp);

	cdc_common.dataIface.descriptor = (usb_functional_desc_t *)&dDataIntf;
	LIST_ADD(&cdc_common.descList, &cdc_common.dataIface);

	cdc_common.dataEpOUT.descriptor = (usb_functional_desc_t *)&depOUT;
	LIST_ADD(&cdc_common.descList, &cdc_common.dataEpOUT);

	cdc_common.dataEpIN.descriptor = (usb_functional_desc_t *)&depIN;
	LIST_ADD(&cdc_common.descList, &cdc_common.dataEpIN);

	cdc_common.str0.descriptor = (usb_functional_desc_t *)&dstr0;
	LIST_ADD(&cdc_common.descList, &cdc_common.str0);

	cdc_common.strman.descriptor = (usb_functional_desc_t *)&dstrman;
	LIST_ADD(&cdc_common.descList, &cdc_common.strman);

	cdc_common.strprod.descriptor = (usb_functional_desc_t *)&dstrprod;
	LIST_ADD(&cdc_common.descList, &cdc_common.strprod);

	cdc_common.strprod.next = NULL;

	if ((res = usbclient_init(cdc_common.descList)) != EOK)
		return res;

	return res;
}
