/*
 *  ACPI-WMI mapping driver (acer_acpi port)
 *
 *  Copyright (C) 2007 Carlos Corbacho <carlos@strangeworlds.co.uk>
 *
 *  GUID parsing code from ldm.c is:
 *   Copyright (C) 2001,2002 Richard Russon <ldm@flatcap.org>
 *   Copyright (c) 2001-2007 Anton Altaparmakov
 *   Copyright (C) 2001,2002 Jakob Kemi <jakob.kemi@telia.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/list.h>

#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>

#include "wmi-acer.h"

#define ACPI_WMI_CLASS			"wmi-acer"

#undef PREFIX
#define PREFIX "ACPI: WMI-Acer: "
#define WMI_ERR KERN_ERR PREFIX
#define WMI_NOTICE KERN_NOTICE PREFIX
#define WMI_INFO KERN_INFO PREFIX

#define DEBUG(level, message...) { \
	if (debug >= level) \
		printk(KERN_DEBUG PREFIX message);\
}

MODULE_AUTHOR("Carlos Corbacho");
MODULE_DESCRIPTION("ACPI-WMI Mapping Driver - acer_acpi port");
MODULE_LICENSE("GPL");

static DEFINE_MUTEX(wmi_data_lock);

struct guid_block
{
	char guid[16];
	union
	{
		char object_id[2];
		struct
		{
			unsigned char notify_id;
			unsigned char reserved;
		};
	};
	u8 instance_count;
	u8 flags;
};

struct wmi_block
{
	struct list_head list;
	struct guid_block gblock;
	acpi_handle handle;
};

static struct wmi_block wmi_blocks;

static wmi_notify_handler wmi_external_handler;
static void *wmi_external_data;

static int debug;

module_param(debug, int, 0664);
MODULE_PARM_DESC(debug, "Debugging verbosity level (0=least 2=most)");

/*
 * If the GUID data block is marked as expensive, we must enable and
 * explicitily disable data collection.
 */
#define ACPI_WMI_EXPENSIVE   0x1
#define ACPI_WMI_METHOD      0x2	/* GUID is a method */
#define ACPI_WMI_STRING      0x4	/* GUID takes & returns a string */
#define ACPI_WMI_EVENT       0x8	/* GUID is an event */

static int acpi_wmi_remove(struct acpi_device *device, int type);
static int acpi_wmi_add(struct acpi_device *device);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)) || \
(defined(CONFIG_SUSE_KERNEL) && LINUX_VERSION_CODE == KERNEL_VERSION(2,6,22))
const static struct acpi_device_id wmi_device_ids[] = {
	{"PNP0C14", 0},
	{"pnp0c14", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, wmi_device_ids);
#endif

static struct acpi_driver acpi_wmi_driver = {
	.name = "wmi-acer",
	.class = ACPI_WMI_CLASS,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)) || \
(defined(CONFIG_SUSE_KERNEL) && LINUX_VERSION_CODE == KERNEL_VERSION(2,6,22))
	.ids = wmi_device_ids,
#else
	.ids = "PNP0C14,pnp0c14",
#endif
	.ops = {
		.add = acpi_wmi_add,
		.remove = acpi_wmi_remove,
		},
};

/*
 * GUID parsing functions
 */

/**
 * wmi_parse_hexbyte - Convert a ASCII hex number to a byte
 * @src:  Pointer to at least 2 characters to convert.
 *
 * Convert a two character ASCII hex string to a number.
 *
 * Return:  0-255  Success, the byte was parsed correctly
 *          -1     Error, an invalid character was supplied
 */
static int wmi_parse_hexbyte(const u8 *src)
{
	unsigned int x; /* For correct wrapping */
	int h;

	/* high part */
	x = src[0];
	if (x - '0' <= '9' - '0') {
		h = x - '0';
	} else if (x - 'a' <= 'f' - 'a') {
		h = x - 'a' + 10;
	} else if (x - 'A' <= 'F' - 'A') {
		h = x - 'A' + 10;
	} else {
		return -1;
	}
	h <<= 4;

	/* low part */
	x = src[1];
	if (x - '0' <= '9' - '0')
		return h | (x - '0');
	if (x - 'a' <= 'f' - 'a')
		return h | (x - 'a' + 10);
	if (x - 'A' <= 'F' - 'A')
		return h | (x - 'A' + 10);
	return -1;
}

/**
 * wmi_swap_bytes - Rearrange GUID bytes to match GUID binary
 * @src:   Memory block holding binary GUID (16 bytes)
 * @dest:  Memory block to hold byte swapped binary GUID (16 bytes)
 *
 * Byte swap a binary GUID to match it's real GUID value
 */
static void wmi_swap_bytes(u8 *src, u8 *dest)
{
	int i;

	for (i = 0; i <= 3; i++)
		memcpy(dest + i, src + (3 - i), 1);

	for (i = 0; i <= 1; i++)
		memcpy(dest + 4 + i, src + (5 - i), 1);

	for (i = 0; i <= 1; i++)
		memcpy(dest + 6 + i, src + (7 - i), 1);

	memcpy(dest + 8, src + 8, 8);
}

/**
 * wmi_parse_guid - Convert GUID from ASCII to binary
 * @src:   36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @dest:  Memory block to hold binary GUID (16 bytes)
 *
 * N.B. The GUID need not be NULL terminated.
 *
 * Return:  'true'   @dest contains binary GUID
 *          'false'  @dest contents are undefined
 */
static bool wmi_parse_guid(const u8 *src, u8 *dest)
{
	static const int size[] = { 4, 2, 2, 2, 6 };
	int i, j, v;

	if (src[8]  != '-' || src[13] != '-' ||
		src[18] != '-' || src[23] != '-')
		return false;

	for (j = 0; j < 5; j++, src++) {
		for (i = 0; i < size[j]; i++, src += 2, *dest++ = v) {
			v = wmi_parse_hexbyte(src);
			if (v < 0)
				return false;
		}
	}

	return true;
}

static bool find_guid(const char *guid_string, struct wmi_block **out)
{
	char tmp[16], guid_input[16];
	struct wmi_block *wblock;
	struct guid_block *block;
	struct list_head *p;

	DEBUG(2, "find_guid called\n");
	DEBUG(2, "Passing GUID to parser\n");
	wmi_parse_guid(guid_string, tmp);
	wmi_swap_bytes(tmp, guid_input);

	list_for_each(p, &wmi_blocks.list) {
		wblock = list_entry(p, struct wmi_block, list);
		block = &wblock->gblock;

		if (memcmp(block->guid, guid_input, 16) == 0) {
			DEBUG(2, "GUID found - returning block\n");
			if (out)
				*out = wblock;
			return 1;
		}
	}
	return 0;
}

/*
 * Exported WMI functions
 */
/**
 * wmi_acer_evaluate_method - Evaluate a WMI method
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @instance: Instance index
 * @method_id: Method ID to call
 * &in: Buffer containing input for the method call
 * &out: Empty buffer to return the method results
 *
 * Convert a WMI method call to an ACPI one, and return the results
 */
acpi_status wmi_acer_evaluate_method(const char *guid_string, u8 instance,
u32 method_id, const struct acpi_buffer *in, struct acpi_buffer *out)
{
	struct guid_block *block = NULL;
	struct wmi_block *wblock = NULL;
	acpi_handle handle;
	acpi_status status;
	struct acpi_object_list input;
	union acpi_object params[3];
	char method[4] = "WM";

	if (!find_guid(guid_string, &wblock))
		return AE_BAD_ADDRESS;

	block = &wblock->gblock;
	handle = wblock->handle;

	if (!block->flags & ACPI_WMI_METHOD) {
		DEBUG(2, "GUID is not a method\n");
		return AE_BAD_DATA;
	}

	if (block->instance_count < instance)
		return AE_BAD_PARAMETER;

	input.count = 2;
	input.pointer = params;
	params[0].type = ACPI_TYPE_INTEGER;
	params[0].integer.value = instance;
	params[1].type = ACPI_TYPE_INTEGER;
	params[1].integer.value = method_id;

	if (in != NULL) {
		input.count = 3;

		if (block->flags & ACPI_WMI_STRING) {
			params[2].type = ACPI_TYPE_STRING;
		} else {
			params[2].type = ACPI_TYPE_BUFFER;
		}
		params[2].buffer.length = in->length;
		params[2].buffer.pointer = in->pointer;
	}

	strncat(method, block->object_id, 2);
	DEBUG(2, "Object to call is %s\n", method);

	status = acpi_evaluate_object(handle, method, &input, out);

	return status;
}
EXPORT_SYMBOL_GPL(wmi_acer_evaluate_method);

/**
 * wmi_query_block - Return contents of a WMI block
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @instance: Instance index
 * &out: Empty buffer to return the contents of the data block to
 *
 * Return the contents of a data block to a buffer
 */
acpi_status wmi_acer_query_block(const char *guid_string, u8 instance,
struct acpi_buffer *out)
{
	struct guid_block *block = NULL;
	struct wmi_block *wblock = NULL;
	acpi_handle handle;
	acpi_status status, wc_status = AE_ERROR;
	struct acpi_object_list input, wc_input;
	union acpi_object wc_params[1], wq_params[1];
	char method[4];
	char wc_method[4] = "WC";

	if (guid_string == NULL || out == NULL)
		return AE_BAD_PARAMETER;

	if (!find_guid(guid_string, &wblock))
		return AE_BAD_ADDRESS;

	block = &wblock->gblock;
	handle = wblock->handle;

	if (block->instance_count < instance)
		return AE_BAD_PARAMETER;

	/* Check GUID is a data block */
	if (block->flags & (ACPI_WMI_EVENT | ACPI_WMI_METHOD))
		return AE_BAD_ADDRESS;

	input.count = 1;
	input.pointer = wq_params;
	wq_params[0].type = ACPI_TYPE_INTEGER;
	wq_params[0].integer.value = instance;

	/*
	 * If ACPI_WMI_EXPENSIVE, call the relevant WCxx method first to
	 * enable collection
	 */
	if (block->flags & ACPI_WMI_EXPENSIVE) {
		wc_input.count = 1;
		wc_input.pointer = wc_params;
		wc_params[0].type = ACPI_TYPE_INTEGER;
		wc_params[0].integer.value = 1;

		strncat(wc_method, block->object_id, 2);

		/*
		 * Some GUIDs break the specification by declaring themselves
		 * expensive, but have no corresponding WCxx method. So we
		 * should not fail if this happens.
		 */
		wc_status = acpi_evaluate_object(handle, wc_method,
			&wc_input, NULL);
	}

	strcpy(method, "WQ");
	strncat(method, block->object_id, 2);
	DEBUG(2, "Object to call is %s\n", method);

	status = acpi_evaluate_object(handle, method, NULL, out);

	/*
	 * If ACPI_WMI_EXPENSIVE, call the relevant WCxx method, even if
	 * the WQxx method failed - we should disable collection anyway
	 */
	if ((block->flags & ACPI_WMI_EXPENSIVE) && ACPI_SUCCESS(wc_status)) {
		wc_params[0].integer.value = 0;
		status = acpi_evaluate_object(handle,
		wc_method, &wc_input, NULL);
	}

	return status;
}
EXPORT_SYMBOL_GPL(wmi_acer_query_block);

/**
 * wmi_acer_set_block - Write to a WMI block
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @instance: Instance index
 * &in: Buffer containing new values for the data block
 *
 * Write the contents of the input buffer to ACPI
 */
acpi_status wmi_acer_set_block(const char *guid_string, u8 instance,
const struct acpi_buffer *in)
{
	struct guid_block *block = NULL;
	struct wmi_block *wblock = NULL;
	acpi_handle handle;
	struct acpi_object_list input;
	union acpi_object params[2];
	char method[4] = "WS";

	if (guid_string == NULL || in == NULL)
		return AE_BAD_DATA;

	if (!find_guid(guid_string, &wblock))
		return AE_BAD_ADDRESS;

	block = &wblock->gblock;
	handle = wblock->handle;

	if (block->instance_count < instance)
		return AE_BAD_PARAMETER;

	/* Check GUID is a data block */
	if (block->flags & (ACPI_WMI_EVENT | ACPI_WMI_METHOD))
		return AE_BAD_ADDRESS;

	input.count = 2;
	input.pointer = params;
	params[0].type = ACPI_TYPE_INTEGER;
	params[0].integer.value = instance;

	if (block->flags & ACPI_WMI_STRING) {
		params[1].type = ACPI_TYPE_STRING;
	} else {
		params[1].type = ACPI_TYPE_BUFFER;
	}
	params[1].buffer.length = in->length;
	params[1].buffer.pointer = in->pointer;

	strncat(method, block->object_id, 2);
	DEBUG(2, "Object to call is %s\n", method);

	return acpi_evaluate_object(handle, method, &input, NULL);
}
EXPORT_SYMBOL_GPL(wmi_acer_set_block);

/**
 * wmi_install_notify_handler - Register handler for WMI events
 * @handler: Function to handle notifications
 * @data: Data to be returned to handler when event is fired
 *
 * Register a handler for events sent to the WMI-ACPI mapper device.
 */
acpi_status wmi_acer_install_notify_handler(wmi_notify_handler handler, void *data)
{
	if (!handler)
		return AE_BAD_PARAMETER;

	if (!wmi_external_handler)
		return AE_ALREADY_ACQUIRED;

	wmi_external_handler = handler;
	wmi_external_data = data;

	return AE_OK;
}
EXPORT_SYMBOL_GPL(wmi_acer_install_notify_handler);

/**
 * wmi_acer_uninstall_notify_handler - Unregister handler for WMI events
 *
 * Unregister handler for events sent to the WMI-ACPI mapper device.
 */
acpi_status wmi_acer_remove_notify_handler(void)
{
	DEBUG(1, "Notifier triggered\n");

	if (wmi_external_handler) {
		wmi_external_handler = NULL;
		wmi_external_data = NULL;
		return AE_OK;
	}
	return AE_ERROR;
}
EXPORT_SYMBOL_GPL(wmi_acer_remove_notify_handler);

/**
 * wmi_acer_get_event_data - Get WMI data associated with an event
 *
 * @event - Event to find
 * &out - Buffer to hold event data
 *
 * Returns extra data associated with an event in WMI.
 */
acpi_status wmi_acer_get_event_data(u32 event, struct acpi_buffer *out)
{
	struct acpi_object_list input;
	union acpi_object params[1];
	struct guid_block *gblock;
	struct wmi_block *wblock;
	struct list_head *p;

	input.count = 1;
	input.pointer = params;
	params[0].type = ACPI_TYPE_INTEGER;
	params[0].integer.value = event;

	list_for_each(p, &wmi_blocks.list) {
		wblock = list_entry(p, struct wmi_block, list);
		gblock = &wblock->gblock;

		if ((gblock->flags & ACPI_WMI_EVENT) &&
			(gblock->notify_id == event))
			return acpi_evaluate_object(wblock->handle, "_WED",
				&input, out);
	}

	return AE_NOT_FOUND;
}
EXPORT_SYMBOL(wmi_acer_get_event_data);

/**
 * wmi_acer_has_guid - Check if a GUID is available
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 *
 * Check if a given GUID is defined by _WDG
 */
bool wmi_acer_has_guid(const char *guid_string)
{
	return find_guid(guid_string, NULL);
}
EXPORT_SYMBOL_GPL(wmi_acer_has_guid);

/**
 * parse_wdg - Parse the _WDG method for the GUID data blocks
 */
static __init acpi_status parse_wdg(acpi_handle handle)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	struct guid_block *gblock;
	struct wmi_block *wblock;
	acpi_status status;
	u32 i, total;

	status = acpi_evaluate_object(handle, "_WDG", NULL, &out);

	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) out.pointer;

	if (obj->type != ACPI_TYPE_BUFFER)
		return AE_ERROR;

	total = obj->buffer.length / sizeof(struct guid_block);

	gblock = kzalloc(obj->buffer.length, GFP_KERNEL);
	if (!gblock)
		return AE_NO_MEMORY;

	memcpy(gblock, obj->buffer.pointer, obj->buffer.length);

	for (i = 0; i < total; i++) {
		wblock = kzalloc(sizeof(struct wmi_block), GFP_KERNEL);
		if (!wblock)
			return AE_NO_MEMORY;

		wblock->gblock = gblock[i];
		wblock->handle = handle;
		list_add_tail(&wblock->list, &wmi_blocks.list);
	}

	kfree(out.pointer);
	kfree(gblock);

	return status;
}

/*
 * WMI can have EmbeddedControl access regions. In which case, we just want to
 * hand these off to the EC driver.
 */
static acpi_status
acpi_wmi_ec_space_handler(u32 function, acpi_physical_address address,
		      u32 bits, acpi_integer *value,
		      void *handler_context, void *region_context)
{
	int result = 0, i = 0;
	u8 temp = 0;

	if ((address > 0xFF) || !value)
		return AE_BAD_PARAMETER;

	if (function != ACPI_READ && function != ACPI_WRITE)
		return AE_BAD_PARAMETER;

	if (bits != 8)
		return AE_BAD_PARAMETER;

	if (function == ACPI_READ) {
		result = ec_read(address, &temp);
		(*value) |= ((acpi_integer)temp) << i;
	} else {
		temp = 0xff & ((*value) >> i);
		result = ec_write(address, temp);
	}

	switch (result) {
	case -EINVAL:
		return AE_BAD_PARAMETER;
		break;
	case -ENODEV:
		return AE_NOT_FOUND;
		break;
	case -ETIME:
		return AE_TIME;
		break;
	default:
		return AE_OK;
	}
}

static void acpi_wmi_notify(acpi_handle handle, u32 event, void *data)
{
	struct guid_block *block;
	struct wmi_block *wblock;
	struct list_head *p;

	list_for_each(p, &wmi_blocks.list) {
		wblock = list_entry(p, struct wmi_block, list);
		block = &wblock->gblock;

		if ((block->flags & ACPI_WMI_EVENT) &&
		block->notify_id == event) {
			if (wmi_external_handler)
				wmi_external_handler(event, wmi_external_data);
			break;
		}
	}
}

static int __init acpi_wmi_add(struct acpi_device *device)
{
	acpi_status status;
	int result = 0;

	status = acpi_install_notify_handler(device->handle, ACPI_DEVICE_NOTIFY,
		acpi_wmi_notify, device);

	if (ACPI_FAILURE(status)) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR,
				  "Error installing notify handler\n"));
		return -ENODEV;
	}

	status = acpi_install_address_space_handler(device->handle,
						    ACPI_ADR_SPACE_EC,
						    &acpi_wmi_ec_space_handler,
						    NULL, NULL);
	if (ACPI_FAILURE(status)) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR,
				  "Error installing EC region handler\n"));
		return -ENODEV;
	}

	status = parse_wdg(device->handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	return result;
}

static int acpi_wmi_remove(struct acpi_device *device, int type)
{
	acpi_remove_address_space_handler(device->handle, ACPI_ADR_SPACE_EC,
						    &acpi_wmi_ec_space_handler);

	acpi_remove_notify_handler(device->handle, ACPI_DEVICE_NOTIFY,
		acpi_wmi_notify);

	return 0;
}

static int __init acpi_wmi_init(void)
{
	acpi_status result;

	if (acpi_disabled)
		return -ENODEV;

	INIT_LIST_HEAD(&wmi_blocks.list);

	result = acpi_bus_register_driver(&acpi_wmi_driver);

	if (result < 0) {
		printk(KERN_INFO PREFIX "Interface device not found\n");
		return result;
	}

	printk(KERN_INFO PREFIX "Mapper loaded\n");

	return result;
}

static void __exit acpi_wmi_exit(void)
{
	struct list_head *p, *tmp;
	struct wmi_block *wblock;

	acpi_bus_unregister_driver(&acpi_wmi_driver);

	list_for_each_safe(p, tmp, &wmi_blocks.list) {
		wblock = list_entry(p, struct wmi_block, list);

		list_del(p);
		kfree(wblock);
	}

	printk(KERN_INFO PREFIX "Mapper unloaded\n");
}

module_init(acpi_wmi_init);
module_exit(acpi_wmi_exit);
