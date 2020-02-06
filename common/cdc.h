/*
 * Phoenix-RTOS
 *
 * USB Communications Device Class definitions
 *
 * Copyright 2018, 2020 Phoenix Systems
 * Author: Hubert Buczynski, Kamil Amanowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _USB_CDC_H_
#define _USB_CDC_H_


/* device class code */
#define USB_CLASS_CDC 0x2


/* interface class code */
#define USB_INTERFACE_COMMUNICATIONS 0x2
#define USB_INTERFACE_DATA 0xA


/* communications class subclass codes */
#define USB_SUBCLASS_DLCM 0x1			/* direct line control model */
#define USB_SUBCLASS_ACM 0x2			/* abstract control model */
#define USB_SUBCLASS_TCM 0x3			/* telephone control model */
#define USB_SUBCLASS_MCCM 0x4			/* multi-channel control model */
#define USB_SUBCLASS_CAPI 0x5			/* CAPI control model */
#define USB_SUBCLASS_ECM 0x6			/* ethernet networking control model */
#define USB_SUBCLASS_ATM 0x7			/* ATM networking control model */
#define USB_SUBCLASS_WIRELESS 0x8		/* wireless handset control model */
#define USB_SUBCLASS_DEV_MGMT 0x9		/* device management */
#define USB_SUBCLASS_MDLM 0xA			/* mobile direct line model */
#define USB_SUBCLASS_OBEX 0xB			/* OBEX */
#define USB_SUBCLASS_EEM 0xC			/* ethernet emulation model */
#define USB_SUBCLASS_NCM 0xD			/* network control model */


/* descriptors subclassses for communications class (CC)*/
#define USB_DESC_SUBTYPE_CC_HEADER 0x0
#define USB_DESC_SUBTYPE_CC_CALL_MGMT 0x1
#define USB_DESC_SUBTYPE_CC_ACM 0x2


/* CDC Header functional descriptor  */
typedef struct _usb_desc_cdc_header {
	uint8_t bLength;
	uint8_t bType;
	uint8_t bSubType;
	uint16_t bcdCDC;
} __attribute__((packed)) usb_desc_cdc_header_t;


/* CDC ACM functional descriptor  */
typedef struct _usb_desc_cdc_acm {
	uint8_t bLength;
	uint8_t bType;
	uint8_t bSubType;
	uint8_t bmCapabilities;
} __attribute__((packed)) usb_desc_cdc_acm_t;


/* CDC Union functional descriptor  */
typedef struct _usb_desc_cdc_union {
	uint8_t bLength;
	uint8_t bType;
	uint8_t bSubType;
	uint8_t bControlInterface;
	uint8_t bSubordinateInterface;
} __attribute__((packed)) usb_desc_cdc_union_t;


/* CDC Call management functional descriptor  */
typedef struct _usb_desc_cdc_call {
	uint8_t bLength;
	uint8_t bType;
	uint8_t bSubType;
	uint8_t bmCapabilities;
	uint8_t bDataInterface;
} __attribute__((packed)) usb_desc_cdc_call_t;


/* CDC Line Coding Request */
typedef struct _usb_cdc_line_coding {
	uint32_t dwDTERate;
	uint8_t bCharFormat;
	uint8_t bParityType;
	uint8_t bDataBits;
} __attribute__((packed)) usb_cdc_line_coding_t;


#endif
