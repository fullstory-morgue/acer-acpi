/*
 *  Acer Laptop ACPI Extras
 *
 *  Copyright (C) 2005-2007	E.M. Smith
 *  Copyright (C) 2007		Carlos Corbacho <cathectic@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  The devolpment page for this driver is located at
 *  http://code.google.com/p/aceracpi
 *
 *  Credits:
 *
 *  John Belmonte - the Toshiba ACPI driver originally adapted for this module.
 *  Julien Lerouge & Karol Kozimor - ASUS Acpi driver authors.
 *  Olaf Tauber - developer of acerhk, the inspiration to solve the 64-bit
 *                driver problem for my Aspire 5024.
 *  Mathieu Segaud - solved the ACPI problem that needed a double-modprobe
 *                   in version 0.2 and below.
 *  Jim Ramsay - Figured out and added support for WMID interface
 */

#define ACER_ACPI_VERSION	"0.11.1"

/*
 * Comment the following line out to remove /proc support
 */
#define CONFIG_PROC

#ifdef CONFIG_PROC
#define PROC_ACER		"acer"
#include <linux/proc_fs.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,17)
#include <asm/uaccess.h>
#else
#include <linux/uaccess.h>
#endif

#include <linux/io.h>
#include <linux/dmi.h>
#include <linux/backlight.h>
#include <linux/leds.h>
#include <linux/platform_device.h>

#include <acpi/acpi_drivers.h>

#include "wmi-acer.h"

/* Workaround needed for older kernels */
#ifndef bool
#define bool int
#endif

MODULE_AUTHOR("Mark Smith, Carlos Corbacho");
MODULE_DESCRIPTION("Acer Laptop ACPI Extras Driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS("dmi:*:*Acer*:*:");

#define ACER_LOGPREFIX "acer_acpi: "
#define ACER_ERR KERN_ERR ACER_LOGPREFIX
#define ACER_NOTICE KERN_NOTICE ACER_LOGPREFIX
#define ACER_INFO KERN_INFO ACER_LOGPREFIX

#define DEBUG(level, message...) { \
	if (debug >= level) \
		printk(KERN_DEBUG ACER_LOGPREFIX message);\
}

/*
 * The maximum temperature one can set for fan control override.
 * Doesn't propably make much sense if over 80 degrees celsius though...
 */
#define ACER_MAX_TEMPERATURE_OVERRIDE 150

/*
 * The following defines quirks to get some specific functions to work
 * which are known to not be supported over ACPI (such as the mail LED
 * on WMID based Acer's)
 */
struct acer_quirks {
	const char *vendor;
	const char *model;
	u16 quirks;
};

/*
 * Keyboard controller ports
 */
#define ACER_KBD_STATUS_REG	0x64	/* Status register (R) */
#define ACER_KBD_CNTL_REG	0x64	/* Controller command register (W) */
#define ACER_KBD_DATA_REG	0x60	/* Keyboard data register (R/W) */

/*
 * Magic Number
 * Meaning is unknown - this number is required for writing to ACPI for AMW0
 * (it's also used in acerhk when directly accessing the EC)
 */
#define ACER_AMW0_WRITE	0x9610

/*
 * Bit masks for the old AMW0 interface
 */
#define ACER_AMW0_WIRELESS_MASK  0x35
#define ACER_AMW0_BLUETOOTH_MASK 0x34
#define ACER_AMW0_MAILLED_MASK   0x31

/*
 * Method IDs for new WMID interface
 */
#define ACER_WMID_GET_WIRELESS_METHODID		1
#define ACER_WMID_GET_BLUETOOTH_METHODID	2
#define ACER_WMID_GET_BRIGHTNESS_METHODID	3
#define ACER_WMID_SET_WIRELESS_METHODID		4
#define ACER_WMID_SET_BLUETOOTH_METHODID	5
#define ACER_WMID_SET_BRIGHTNESS_METHODID	6
#define ACER_WMID_GET_THREEG_METHODID		10
#define ACER_WMID_SET_THREEG_METHODID		11

/*
 * Acer ACPI method GUIDs
 */
#define AMW0_GUID1		"67C3371D-95A3-4C37-BB61-DD47B491DAAB"
#define WMID_GUID1		"6AF4F258-B401-42fd-BE91-3D4AC2D7C0D3"
#define WMID_GUID2		"95764E09-FB56-4e83-B31A-37761F60994A"

/*
 * Interface capability flags
 */
#define ACER_CAP_MAILLED		(1<<0)
#define ACER_CAP_WIRELESS		(1<<1)
#define ACER_CAP_BLUETOOTH		(1<<2)
#define ACER_CAP_BRIGHTNESS		(1<<3)
#define ACER_CAP_THREEG			(1<<4)
#define ACER_CAP_TOUCHPAD_READ		(1<<5)
#define ACER_CAP_TEMPERATURE_OVERRIDE	(1<<6)
#define ACER_CAP_ANY			(0xFFFFFFFF)

/*
 * Interface type flags
 */
enum interface_flags {
	ACER_AMW0,
	ACER_AMW0_V2,
	ACER_WMID,
};

/*
 * Presumed start states -
 * On some AMW0 laptops, we do not yet know how to get the device status from
 * the EC, so we must store this ourselves.
 *
 * Plus, we can't tell which features are enabled or disabled on a specific
 * model - e.g. The 5020 series can _support_ bluetooth; but the 5021 has no
 * bluetooth, whilst the 5024 does.  However, the BIOS identifies both laptops
 * as 5020, and you can add bluetooth later.
 *
 * Basically the code works like this:
 *   - On init, any values specified on the commandline are set.
 *   - For interfaces where the current values cannot be detected and which
 *     have not been set on the commandline, we set them to some sane default
 *     (disabled)
 *
 * See AMW0_init and acer_commandline_init
 */

#define ACER_DEFAULT_WIRELESS  0
#define ACER_DEFAULT_BLUETOOTH 0
#define ACER_DEFAULT_MAILLED   0
#define ACER_DEFAULT_THREEG    0

static int max_brightness = 0xF;

static int wireless = -1;
static int bluetooth = -1;
static int mailled = -1;
static int brightness = -1;
static int threeg = -1;
static int fan_temperature_override = -1;
static int debug;
static int force_series;

module_param(mailled, int, 0444);
module_param(wireless, int, 0444);
module_param(bluetooth, int, 0444);
module_param(brightness, int, 0444);
module_param(threeg, int, 0444);
module_param(force_series, int, 0444);
module_param(fan_temperature_override, int, 0444);
module_param(debug, int, 0664);
MODULE_PARM_DESC(wireless, "Set initial state of Wireless hardware");
MODULE_PARM_DESC(bluetooth, "Set initial state of Bluetooth hardware");
MODULE_PARM_DESC(mailled, "Set initial state of Mail LED");
MODULE_PARM_DESC(brightness, "Set initial LCD backlight brightness");
MODULE_PARM_DESC(threeg, "Set initial state of 3G hardware");
MODULE_PARM_DESC(fan_temperature_override, "Set initial state of the 'FAN temperature-override'");
MODULE_PARM_DESC(debug, "Debugging verbosity level (0=least 2=most)");
MODULE_PARM_DESC(force_series, "Force a different laptop series for extra features (5020, 5720 or 2490)");

#ifdef CONFIG_PROC
struct ProcItem {
	const char *name;
	char *(*read_func) (char *, u32);
	unsigned long (*write_func) (const char *, unsigned long, u32);
	unsigned int capability;
};

static struct proc_dir_entry *acer_proc_dir;
#endif

/*
 * Wait for the keyboard controller to become ready
 */
static int wait_kbd_write(void)
{
	int i = 0;
	while ((inb(ACER_KBD_STATUS_REG) & 0x02) && (i < 10000)) {
		udelay(50);
		i++;
	}
	return -(i == 10000);
}

static void send_kbd_cmd(u8 cmd, u8 val)
{
	preempt_disable();
	if (!wait_kbd_write())
		outb(cmd, ACER_KBD_CNTL_REG);
	if (!wait_kbd_write())
		outb(val, ACER_KBD_DATA_REG);
	preempt_enable_no_resched();
}

static void set_keyboard_quirk(void)
{
	send_kbd_cmd(0x59, 0x90);
}

struct acer_data {
	int mailled;
	int wireless;
	int bluetooth;
	int threeg;
	int brightness;
};

/* Each low-level interface must define at least some of the following */
struct Interface {
	u32 type;		/* WMI device type */
	u32 capability;		/* The capabilities this interface provides */
	struct acer_data data;	/* Private data for interface */
};

/* The static interface pointer, points to the currently detected interface */
static struct Interface *interface;

/*
 * Embedded Controller quirks
 * Some laptops require us to directly access the EC to either enable or query
 * features that are not available through ACPI.
 */

struct quirk_entry {
	u8 wireless;
	u8 mailled;
	u8 brightness;
	u8 touchpad;
	u8 temperature_override;
	u8 mmkeys;
	u8 bluetooth;
};

static struct quirk_entry *quirks;

static void set_quirks(void)
{
	if (quirks->mailled != 0) {
		interface->capability |= ACER_CAP_MAILLED;
		DEBUG(1, "Using EC direct-access quirk for mail LED\n");
	}

	if (quirks->touchpad != 0) {
		interface->capability |= ACER_CAP_TOUCHPAD_READ;
		DEBUG(1, "Using EC direct-access quirk for reading touchpad status\n");
	}

	if (quirks->temperature_override != 0) {
		interface->capability |= ACER_CAP_TEMPERATURE_OVERRIDE;
		DEBUG(1, "Using EC direct-access quirk for temperature override setting (fan)\n");
	}

	if (quirks->brightness != 0) {
		interface->capability |= ACER_CAP_BRIGHTNESS;
		DEBUG(1, "Using EC direct-access quirk for backlight brightness\n");
	}

	if (quirks->mmkeys != 0) {
		set_keyboard_quirk();
		printk(ACER_INFO "Setting keyboard quirk to enable multimedia keys\n");
	}

	if (quirks->bluetooth != 0) {
		interface->capability |= ACER_CAP_BLUETOOTH;
		DEBUG(1, "Using EC direct-access quirk for bluetooth\n");
	}

	if (quirks->wireless != 0) {
		interface->capability |= ACER_CAP_WIRELESS;
		DEBUG(1, "Using EC direct-access quirk for wireless\n");
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
static int dmi_matched(const struct dmi_system_id *dmi)
#else
static int dmi_matched(struct dmi_system_id *dmi)
#endif
{
	quirks = dmi->driver_data;
	return 0;
}

static struct quirk_entry quirk_unknown = {
};

/* Same Mail LED quirk as TM2490, but does not require keyboard quirk */
static struct quirk_entry quirk_acer_aspire_5100 = {
	.mailled = 1,
};

static struct quirk_entry quirk_acer_travelmate_2490 = {
	.mmkeys = 1,
	.mailled = 1,
	.temperature_override = 1,
	.touchpad = 1,
};

static struct quirk_entry quirk_acer_travelmate_5720 = {
	.touchpad = 2,
};

static struct quirk_entry quirk_medion_md_98300 = {
	.wireless = 1,
};

static struct dmi_system_id acer_quirks[] = {
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 3100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3100"),
		},
		.driver_data = &quirk_acer_aspire_5100,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5100"),
		},
		.driver_data = &quirk_acer_aspire_5100,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5630",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5630"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5650",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5650"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5680",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5680"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 9110",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 9110"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Extensa 5220",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Extensa 5220"),
		},
		.driver_data = &quirk_acer_travelmate_5720,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer TravelMate 2490",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 2490"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer TravelMate 5720",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 5720"),
		},
		.driver_data = &quirk_acer_travelmate_5720,
	},
	{
		.callback = dmi_matched,
		.ident = "Medion MD 98300",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEDION"),
			DMI_MATCH(DMI_PRODUCT_NAME, "WAM2030"),
		},
		.driver_data = &quirk_medion_md_98300,
	},
	{}
};

/* Find which quirks are needed for a particular vendor/ model pair */
static void find_quirks(void)
{
	DEBUG(1, "Looking for quirks\n");
	if (!force_series) {
		dmi_check_system(acer_quirks);
	} else if (force_series == 2490) {
		DEBUG(0, "Forcing Acer TravelMate 2490\n");
		quirks = &quirk_acer_travelmate_2490;
	}

	if (quirks == NULL) {
		DEBUG(1, "No quirks known for this laptop\n");
		quirks = &quirk_unknown;
	}
	set_quirks();
}

/*
 * General interface convenience methods
 */

static bool has_cap(u32 cap)
{
	if ((interface->capability & cap) != 0)
		return 1;
	return 0;
}

/*
 * Old interface (now known as the AMW0 interface)
 */
struct WMAB_args {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
};

struct WMAB_ret {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
	u32 eex;
};

static acpi_status WMAB_execute(struct WMAB_args *regbuf, struct acpi_buffer *result)
{
	struct acpi_buffer input;
	acpi_status status;
	input.length = sizeof(struct WMAB_args);
	input.pointer = (u8 *) regbuf;

	status = wmi_acer_evaluate_method(AMW0_GUID1, 1, 1, &input, result);
	DEBUG(2, "  Args: 0x%08x 0x%08x 0x%08x 0x%08x\n", regbuf->eax, regbuf->ebx, regbuf->ecx, regbuf->edx );

	return status;
}

static acpi_status AMW0_get_u32(u32 *value, u32 cap, struct Interface *iface)
{
	u8 result;

	DEBUG(2, "  AMW0_get_u32: cap=%d\n", cap);
	switch (cap) {
	case ACER_CAP_MAILLED:
		switch (quirks->mailled) {
		default:
			ec_read(0x0A, &result);
			*value = (result >> 7) & 0x01;
			return 0;
		}
		break;
	case ACER_CAP_WIRELESS:
		switch (quirks->wireless) {
		case 1:
			ec_read(0x7B, &result);
			*value = result & 0x01;
			return 0;
		default:
			ec_read(0x0A, &result);
			*value = (result >> 2) & 0x01;
			return 0;
		}
		break;
	case ACER_CAP_BLUETOOTH:
		switch (quirks->bluetooth) {
		default:
			ec_read(0x0A, &result);
			*value = (result >> 4) & 0x01;
			return 0;
		}
		break;
	case ACER_CAP_BRIGHTNESS:
		switch (quirks->brightness) {
		default:
			ec_read(0x83, &result);
			*value = result;
			return 0;
		}
		break;
	default:
		return AE_BAD_ADDRESS;
	}
	return AE_OK;
}

static acpi_status AMW0_set_u32(u32 value, u32 cap, struct Interface *iface)
{
	struct WMAB_args args;

	args.eax = ACER_AMW0_WRITE;
	args.ebx = value ? (1<<8) : 0;
	args.ecx = args.edx = 0;

	switch (cap) {
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_MAILLED_MASK;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_WIRELESS_MASK;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_BLUETOOTH_MASK;
		break;
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		switch (quirks->brightness) {
		default:
			return ec_write(0x83, value);
			break;
		}
	default:
		return AE_BAD_ADDRESS;
	}

	/* Actually do the set */
	return WMAB_execute(&args, NULL);
}

static acpi_status AMW0_find_mailled(void)
{
	struct WMAB_args args;
	struct WMAB_ret ret;
	acpi_status status = AE_OK;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	args.eax = 0x0086;
	args.ebx = args.ecx = args.edx = 0;

	status = WMAB_execute(&args, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER &&
	obj->buffer.length == sizeof(struct WMAB_ret)) {
		ret = *((struct WMAB_ret *) obj->buffer.pointer);
	} else {
		return AE_ERROR;
	}

	if (ret.eex & 0x1) {
		interface->capability |= ACER_CAP_MAILLED;
		DEBUG(1, "Mail LED available - enabling\n");
	}

	return AE_OK;
}

static acpi_status AMW0_set_capabilities(void) {
	struct WMAB_args args;
	struct WMAB_ret ret;
	acpi_status status = AE_OK;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	args.eax = ACER_AMW0_WRITE;
	args.ecx = args.edx = 0;

	args.ebx = 0xa2 << 8;
	args.ebx |= ACER_AMW0_WIRELESS_MASK;

	status = WMAB_execute(&args, &out);
	if (ACPI_FAILURE(status)) {
		printk(ACER_INFO "Problem with wireless path\n");
		return status;
	}

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER &&
	obj->buffer.length == sizeof(struct WMAB_ret)) {
		ret = *((struct WMAB_ret *) obj->buffer.pointer);
	} else {
		return AE_ERROR;
	}

	if (ret.eax & 0x1) {
		interface->capability |= ACER_CAP_WIRELESS;
		DEBUG(1, "Wireless hardware available - enabling\n");
	}

	args.ebx = 2 << 8;
	args.ebx |= ACER_AMW0_BLUETOOTH_MASK;

	status = WMAB_execute(&args, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER
	&& obj->buffer.length == sizeof(struct WMAB_ret)) {
		ret = *((struct WMAB_ret *) obj->buffer.pointer);
	} else {
		return AE_ERROR;
	}

	if (ret.eax & 0x1) {
		interface->capability |= ACER_CAP_BLUETOOTH;
		DEBUG(1, "Bluetooth hardware available - enabling\n");
	}

	/*
	 * This appears to be safe to enable, since all Wistron based laptops
	 * appear to use the same EC register for brightness, even if they
	 * differ for wireless, etc
	 */
	interface->capability |= ACER_CAP_BRIGHTNESS;

	return AE_OK;
}

static struct Interface AMW0_interface = {
	.type = ACER_AMW0,
};

static struct Interface AMW0_V2_interface = {
	.type = ACER_AMW0_V2,
	.capability = (
		ACER_CAP_BRIGHTNESS
	),
};

/*
 * New interface (The WMID interface)
 */
static acpi_status
WMI_execute_u32(u32 method_id, u32 in, u32 *out)
{
	struct acpi_buffer input = { (acpi_size) sizeof(u32), (void *)(&in) };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u32 tmp;
	acpi_status status;

	DEBUG(2, "  WMI_execute_u32:\n");
	status = wmi_acer_evaluate_method(WMID_GUID1, 1, method_id, &input, &result);
	DEBUG(2, "  In: 0x%08x\n", in);

	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) result.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER && obj->buffer.length == sizeof(u32)) {
		tmp = *((u32 *) obj->buffer.pointer);
		DEBUG(2, "  Out: 0x%08x\n", tmp);
	} else {
		tmp = 0;
		if (obj) {
			DEBUG(2, "  Got unexpected result of type %d\n", obj->type);
		} else {
			DEBUG(2, "  Got unexpected null result\n");
		}
	}

	if (out)
		*out = tmp;

	if (result.length > 0 && result.pointer)
		kfree(result.pointer);

	DEBUG(2, "  Returning from WMI_execute_u32:\n");
	return status;
}

static acpi_status WMID_get_u32(u32 *value, u32 cap, struct Interface *iface)
{
	acpi_status status;
	u8 tmp;
	u32 result, method_id = 0;

	DEBUG(2, "  WMID_get_u32: cap=%d\n", cap);
	switch (cap) {
	case ACER_CAP_WIRELESS:
		method_id = ACER_WMID_GET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		method_id = ACER_WMID_GET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_BRIGHTNESS:
		method_id = ACER_WMID_GET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_THREEG:
		method_id = ACER_WMID_GET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (quirks->mailled == 1) {
			ec_read(0x9f, &tmp);
			*value = tmp & 0x01;
			return 0;
		}
	case ACER_CAP_TOUCHPAD_READ:
		switch (quirks->touchpad) {
		case 1:
			ec_read(0x9e, &tmp);
			*value = 1 - ((tmp >> 3) & 0x01);
			return 0;
		case 2:
			ec_read(0x74, &tmp);
			*value = ((tmp >> 3) & 0x01);
			return 0;
		default:
			break;
		}
	case ACER_CAP_TEMPERATURE_OVERRIDE:
		if (quirks->temperature_override == 1) {
			ec_read(0xa9, &tmp);
			*value = tmp;
			return 0;
		}
	default:
		return AE_BAD_ADDRESS;
	}
	status = WMI_execute_u32(method_id, 0, &result);
	DEBUG(2, "   WMI_execute_u32 status=%d:\n", status);

	if (ACPI_SUCCESS(status))
		*value = (u8)result;

	DEBUG(2, "  Returning from WMID_get_u32:\n");
	return status;
}

static acpi_status WMID_set_u32(u32 value, u32 cap, struct Interface *iface)
{
	u32 method_id = 0;

	switch (cap) {
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_THREEG:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		if (quirks->mailled == 1) {
			send_kbd_cmd(0x59, value ? 0x92 : 0x93);
			return 0;
		}
		break;
	case ACER_CAP_TEMPERATURE_OVERRIDE:
		if (value > ACER_MAX_TEMPERATURE_OVERRIDE)
			return AE_BAD_PARAMETER;

		if (quirks->temperature_override == 1) {
			ec_write(0xa9, value);
			return 0;
		}
	default:
		return AE_BAD_ADDRESS;
	}
	return WMI_execute_u32(method_id, (u32)value, NULL);
}

static acpi_status WMID_set_capabilities(void) {
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	acpi_status status;
	u32 devices;

	status = wmi_acer_query_block(WMID_GUID2, 1, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER && obj->buffer.length == sizeof(u32)) {
		devices = *((u32 *) obj->buffer.pointer);
	} else {
		return AE_ERROR;
	}

	/* Not sure on the meaning of the relevant bits yet */
	interface->capability |= ACER_CAP_WIRELESS;
	interface->capability |= ACER_CAP_THREEG;

	if (devices & 0x10) {
		DEBUG(1, "Bluetooth hardware available - enabling\n");
		interface->capability |= ACER_CAP_BLUETOOTH;
	}

	if (!(devices & 0x20)) {
		DEBUG(1, "Maximum brightness quirk detected - enabling\n");
		max_brightness = 0x9;
	}

	return status;
}


static struct Interface WMID_interface = {
	.type = ACER_WMID,
	.capability = ACER_CAP_BRIGHTNESS,
};

#ifdef CONFIG_PROC
/*
 * High-level Procfs file handlers
 */

static int
dispatch_read(char *page, char **start, off_t off, int count, int *eof,
	      struct ProcItem *item)
{
	char *p = page;
	int len;

	DEBUG(2, "  dispatch_read: \n");
	if (off == 0)
		p = item->read_func(p, item->capability);
	len = (p - page);
	if (len <= off + count)
		*eof = 1;
	*start = page + off;
	len -= off;
	if (len > count)
		len = count;
	if (len < 0)
		len = 0;
	return len;
}

static int
dispatch_write(struct file *file, const char __user *buffer,
	       unsigned long count, struct ProcItem *item)
{
	int result;
	char *tmp_buffer;

	/*
	 * Arg buffer points to userspace memory, which can't be accessed
	 * directly.  Since we're making a copy, zero-terminate the
	 * destination so that sscanf can be used on it safely.
	 */
	tmp_buffer = kmalloc(count + 1, GFP_KERNEL);
	if (copy_from_user(tmp_buffer, buffer, count)) {
		result = -EFAULT;
	} else {
		tmp_buffer[count] = 0;
		result = item->write_func(tmp_buffer, count, item->capability);
	}
	kfree(tmp_buffer);
	return result;
}
#endif

/*
 * Generic Device (interface-independent)
 */
static acpi_status get_u32(u32 *value, u32 cap)
{
	DEBUG(2, "  get_u32: cap=%u, value=%u\n", cap, *value);

	if (!(interface->capability & cap))
		return AE_BAD_PARAMETER;

	switch (interface->type) {
	case ACER_AMW0:
		return AMW0_get_u32(value, cap, interface);
	case ACER_AMW0_V2:
		if (cap == ACER_CAP_MAILLED)
			return AMW0_get_u32(value, cap, interface);
	case ACER_WMID:
		return WMID_get_u32(value, cap, interface);
	default:
		return AE_BAD_PARAMETER;
	}
}

static acpi_status set_u32(u32 value, u32 cap)
{
	acpi_status status;

	DEBUG(2, "  set_u32: cap=%u, value=%u\n", cap, value);

	if (!(interface->capability & cap))
		return AE_BAD_PARAMETER;

	switch (interface->type) {
	case ACER_AMW0:
		return AMW0_set_u32(value, cap, interface);
	case ACER_AMW0_V2:
		if (cap == ACER_CAP_MAILLED)
			return AMW0_set_u32(value, cap, interface);

		/*
		 * On some models, some WMID methods don't toggle properly.
		 * For those cases, we want to run the AMW0 method afterwards
		 * to be certain we've really toggled the device state.
		 */
		if (cap == ACER_CAP_WIRELESS || cap == ACER_CAP_BLUETOOTH) {
			status = WMID_set_u32(value, cap, interface);
			if (ACPI_FAILURE(status))
				return status;

			return AMW0_set_u32(value, cap, interface);
		}
	case ACER_WMID:
		return WMID_set_u32(value, cap, interface);
	default:
		return AE_BAD_PARAMETER;
	}
}

static void __init acer_commandline_init(void)
{
	DEBUG(1, "Commandline args: mailled(%d) wireless(%d) bluetooth(%d) brightness(%d)\n",
		mailled, wireless, bluetooth, brightness);

	/*
	 * These will all fail silently if the value given is invalid, or the
	 * capability isn't available on the given interface
	 */
	set_u32(mailled, ACER_CAP_MAILLED);
	set_u32(wireless, ACER_CAP_WIRELESS);
	set_u32(bluetooth, ACER_CAP_BLUETOOTH);
	set_u32(threeg, ACER_CAP_THREEG);
	set_u32(fan_temperature_override, ACER_CAP_TEMPERATURE_OVERRIDE);
	set_u32(brightness, ACER_CAP_BRIGHTNESS);
}

#ifdef CONFIG_PROC
/*
 * Procfs interface (deprecated)
 */
static char *read_u32(char *p, u32 cap)
{
	u32 result;
	acpi_status status;

	DEBUG(2, "  read_u32: cap=%d\n", cap);
	status = get_u32(&result, cap);
	if (ACPI_SUCCESS(status))
		p += sprintf(p, "%u\n", result);
	else
		p += sprintf(p, "Read error\n");
	return p;
}

static unsigned long write_u32(const char *buffer, unsigned long count, u32 cap)
{
	u32 value;

	if (sscanf(buffer, "%u", &value) == 1) {
		acpi_status status = set_u32(value, cap);
		if (ACPI_FAILURE(status))
			return -EINVAL;
	} else {
		return -EINVAL;
	}
	return count;
}

static char *read_version(char *p, u32 cap)
{
	p += sprintf(p, "%s\n", ACER_ACPI_VERSION);
	return p;
}

static char *read_interface(char *p, u32 cap)
{
	switch (interface->type) {
	case ACER_AMW0:
		p += sprintf(p, "AMW0\n");
		break;
	case ACER_AMW0_V2:
		p += sprintf(p, "AMW0 v2\n");
		break;
	case ACER_WMID:
		p += sprintf(p, "WMID\n");
		break;
	default:
		p += sprintf(p, "Error!\n");
		break;
	}
	return p;
}

static struct ProcItem proc_items[] = {
	{"mailled", read_u32, write_u32, ACER_CAP_MAILLED},
	{"bluetooth", read_u32, write_u32, ACER_CAP_BLUETOOTH},
	{"wireless", read_u32, write_u32, ACER_CAP_WIRELESS},
	{"brightness", read_u32, write_u32, ACER_CAP_BRIGHTNESS},
	{"threeg", read_u32, write_u32, ACER_CAP_THREEG},
	{"touchpad", read_u32, NULL, ACER_CAP_TOUCHPAD_READ},
	{"fan_temperature_override", read_u32, write_u32, ACER_CAP_TEMPERATURE_OVERRIDE},
	{"version", read_version, NULL, ACER_CAP_ANY},
	{"interface", read_interface, NULL, ACER_CAP_ANY},
	{NULL}
};

static int __init add_proc_entries(void)
{
	struct proc_dir_entry *proc;
	struct ProcItem *item;

	for (item = proc_items; item->name; ++item) {
		/*
		 * Only add the proc file if the current interface actually
		 * supports it
		 */
		if (interface->capability & item->capability) {
			proc = create_proc_read_entry(item->name,
					S_IFREG | S_IRUGO | S_IWUSR,
					acer_proc_dir,
					(read_proc_t *) dispatch_read,
					item);
			if (proc)
				proc->owner = THIS_MODULE;
			if (proc && item->write_func)
				proc->write_proc = (write_proc_t *) dispatch_write;
		}
	}

	return 0;
}

static int __exit remove_proc_entries(void)
{
	struct ProcItem *item;

	for (item = proc_items; item->name; ++item)
		remove_proc_entry(item->name, acer_proc_dir);
	return 0;
}
#endif

/*
 * LED device (Mail LED only, no other LEDs known yet)
 */
static void mail_led_set(struct led_classdev *led_cdev, enum led_brightness value)
{
	u32 tmp = value;
	set_u32(tmp, ACER_CAP_MAILLED);
}

static struct led_classdev mail_led = {
	.name = "acer_acpi:mail",
	.brightness_set = mail_led_set,
};

static void acer_led_init(struct device *dev)
{
	led_classdev_register(dev, &mail_led);
}

static void acer_led_exit(void)
{
	led_classdev_unregister(&mail_led);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
/*
 * Backlight device
 */
static struct backlight_device *acer_backlight_device;

static int read_brightness(struct backlight_device *bd)
{
	u32 value;
	get_u32(&value, ACER_CAP_BRIGHTNESS);
	return value;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,20)
static int update_bl_status(struct backlight_device *bd)
{
	set_u32(bd->props->brightness, ACER_CAP_BRIGHTNESS);
	return 0;
}

static struct backlight_properties acer_backlight_properties = {
	.get_brightness = read_brightness,
	.update_status = update_bl_status,
};

static int __init acer_backlight_init(struct device *dev)
{
	struct backlight_device *bd;

	DEBUG(1, "Loading backlight driver\n");
	bd = backlight_device_register("acer_acpi", dev, NULL, &acer_backlight_properties);
	if (IS_ERR(bd)) {
		printk(ACER_ERR "Could not register Acer backlight device\n");
		acer_backlight_device = NULL;
		return PTR_ERR(bd);
	}

	acer_backlight_device = bd;

	bd->props->max_brightness = max_brightness;
	return 0;
}
#else
static int update_bl_status(struct backlight_device *bd)
{
	set_u32(bd->props.brightness, ACER_CAP_BRIGHTNESS);
	return 0;
}

static struct backlight_ops acer_backlight_ops = {
	.get_brightness = read_brightness,
	.update_status = update_bl_status,
};

static int __init acer_backlight_init(struct device *dev)
{
	struct backlight_device *bd;

	DEBUG(1, "Loading backlight driver\n");
	bd = backlight_device_register("acer_acpi", dev, NULL, &acer_backlight_ops);
	if (IS_ERR(bd)) {
		printk(ACER_ERR "Could not register Acer backlight device\n");
		acer_backlight_device = NULL;
		return PTR_ERR(bd);
	}

	acer_backlight_device = bd;

	bd->props.max_brightness = max_brightness;
	bd->props.brightness = read_brightness(NULL);
	backlight_update_status(bd);
	return 0;
}
#endif

static void __exit acer_backlight_exit(void)
{
	backlight_device_unregister(acer_backlight_device);
}
#endif

/*
 * Read/ write bool sysfs macro
 */
#define show_set_bool(value, cap) \
static ssize_t \
show_bool_##value(struct device *dev, struct device_attribute *attr, \
	char *buf) \
{ \
	u32 result; \
	acpi_status status = get_u32(&result, cap); \
	if (ACPI_SUCCESS(status)) \
		return sprintf(buf, "%u\n", result); \
	return sprintf(buf, "Read error\n"); \
} \
\
static ssize_t \
set_bool_##value(struct device *dev, struct device_attribute *attr, \
	const char *buf, size_t count) \
{ \
	u32 tmp = simple_strtoul(buf, NULL, 10); \
	acpi_status status = set_u32(tmp, cap); \
		if (ACPI_FAILURE(status)) \
			return -EINVAL; \
	return count; \
} \
static DEVICE_ATTR(value, S_IWUGO | S_IRUGO | S_IWUSR, \
	show_bool_##value, set_bool_##value);

show_set_bool(wireless, ACER_CAP_WIRELESS);
show_set_bool(bluetooth, ACER_CAP_BLUETOOTH);
show_set_bool(threeg, ACER_CAP_THREEG);
show_set_bool(fan_temperature_override, ACER_CAP_TEMPERATURE_OVERRIDE);

/*
 * Read-only bool sysfs macro
 */
#define show_bool(value, cap) \
static ssize_t \
show_bool_##value(struct device *dev, struct device_attribute *attr, \
	char *buf) \
{ \
	u32 result; \
	acpi_status status = get_u32(&result, cap); \
	if (ACPI_SUCCESS(status)) \
		return sprintf(buf, "%u\n", result); \
	return sprintf(buf, "Read error\n"); \
} \
static DEVICE_ATTR(value, S_IWUGO | S_IRUGO | S_IWUSR, \
	show_bool_##value, NULL);

show_bool(touchpad, ACER_CAP_TOUCHPAD_READ);

/*
 * Read interface sysfs macro
 */
static ssize_t show_interface(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	switch (interface->type) {
	case ACER_AMW0:
		return sprintf(buf, "AMW0\n");
	case ACER_AMW0_V2:
		return sprintf(buf, "AMW0 v2\n");
	case ACER_WMID:
		return sprintf(buf, "WMID\n");
	default:
		return sprintf(buf, "Error!\n");
	}
}

static DEVICE_ATTR(interface, S_IWUGO | S_IRUGO | S_IWUSR, show_interface, NULL);

static ssize_t show_devices(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	acpi_status status;
	u32 tmp;

	status = wmi_acer_query_block(WMID_GUID2, 1, &out);
	if (ACPI_FAILURE(status))
		return sprintf(buf, "Error\n");

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER && obj->buffer.length == sizeof(u32)) {
		tmp = *((u32 *) obj->buffer.pointer);
	} else {
		return sprintf(buf, "Error\n");
	}

	return sprintf(buf, "%u\n", tmp);
}

static DEVICE_ATTR(devices, S_IWUGO | S_IRUGO | S_IWUSR, show_devices, NULL);

/*
 * Platform device
 */
static int __devinit acer_platform_probe(struct platform_device *device)
{
	if (has_cap(ACER_CAP_MAILLED))
		acer_led_init(&device->dev);
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
	if (has_cap(ACER_CAP_BRIGHTNESS))
		acer_backlight_init(&device->dev);
#endif
	return 0;
}

static int acer_platform_remove(struct platform_device *device)
{
	if (has_cap(ACER_CAP_MAILLED))
		acer_led_exit();
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
	if (has_cap(ACER_CAP_BRIGHTNESS))
		acer_backlight_exit();
#endif
	return 0;
}

static int acer_platform_suspend(struct platform_device *device, pm_message_t state)
{
	/*
	 * WMID fix for suspend-to-disk - save all current states now so we can
	 * restore them on resume
	 */
	u32 value;
	struct acer_data *data = &interface->data;

	#define save_device(device, cap) \
	if (has_cap(cap)) {\
		get_u32(&value, cap);\
		data->device = value;\
	}

	save_device(wireless, ACER_CAP_WIRELESS);
	save_device(bluetooth, ACER_CAP_BLUETOOTH);
	save_device(threeg, ACER_CAP_THREEG);
	save_device(mailled, ACER_CAP_MAILLED);
	save_device(brightness, ACER_CAP_BRIGHTNESS);

	return 0;
}

static int acer_platform_resume(struct platform_device *device)
{
	struct acer_data *data = &interface->data;

	#define restore_bool_device(device, cap) \
	if (has_cap(cap))\
		set_u32(data->device, cap);\

	restore_bool_device(wireless, ACER_CAP_WIRELESS);
	restore_bool_device(bluetooth, ACER_CAP_BLUETOOTH);
	restore_bool_device(threeg, ACER_CAP_THREEG);
	restore_bool_device(mailled, ACER_CAP_MAILLED);
	restore_bool_device(brightness, ACER_CAP_BRIGHTNESS);

	/* Check if this laptop requires the keyboard quirk */
	if (quirks->mmkeys != 0) {
		set_keyboard_quirk();
		printk(ACER_INFO "Setting keyboard quirk to enable multimedia keys\n");
	}

	return 0;
}

static struct platform_driver acer_platform_driver = {
	.driver = {
		.name = "acer_acpi",
		.owner = THIS_MODULE,
	},
	.probe = acer_platform_probe,
	.remove = acer_platform_remove,
	.suspend = acer_platform_suspend,
	.resume = acer_platform_resume,
};

static struct platform_device *acer_platform_device;

static int remove_sysfs(struct platform_device *device)
{
	#define remove_device_file(value, cap) \
	if (has_cap(cap)) \
		device_remove_file(&device->dev, &dev_attr_##value);

	if (wmi_acer_has_guid(WMID_GUID2))
		device_remove_file(&device->dev, &dev_attr_devices);

	remove_device_file(wireless, ACER_CAP_WIRELESS);
	remove_device_file(bluetooth, ACER_CAP_BLUETOOTH);
	remove_device_file(threeg, ACER_CAP_THREEG);
	remove_device_file(interface, ACER_CAP_ANY);
	remove_device_file(fan_temperature_override, ACER_CAP_TEMPERATURE_OVERRIDE);
	remove_device_file(touchpad, ACER_CAP_TOUCHPAD_READ);
	return 0;
}

static int create_sysfs(void)
{
	int retval = -ENOMEM;

	#define add_device_file(value, cap) \
	if (has_cap(cap)) {\
		retval = device_create_file(&acer_platform_device->dev, &dev_attr_##value);\
		if (retval)\
			goto error;\
	}

	if (wmi_acer_has_guid(WMID_GUID2)) {
		retval = device_create_file(&acer_platform_device->dev, &dev_attr_devices);
		if (retval)
			goto error;
	}

	add_device_file(wireless, ACER_CAP_WIRELESS);
	add_device_file(bluetooth, ACER_CAP_BLUETOOTH);
	add_device_file(threeg, ACER_CAP_THREEG);
	add_device_file(interface, ACER_CAP_ANY);
	add_device_file(fan_temperature_override, ACER_CAP_TEMPERATURE_OVERRIDE);
	add_device_file(touchpad, ACER_CAP_TOUCHPAD_READ);

	return 0;

error:
	remove_sysfs(acer_platform_device);
	return retval;
}

static int __init acer_acpi_init(void)
{
	printk(ACER_INFO "Acer Laptop ACPI Extras version %s\n",
			ACER_ACPI_VERSION);

	/*
	 * Detect which WMI interface we're using.
	 */
	if (wmi_acer_has_guid(AMW0_GUID1) && wmi_acer_has_guid(WMID_GUID1)) {
		DEBUG(0, "Detected Acer AMW0 version 2 interface\n");
		interface = &AMW0_V2_interface;
	}

	if (!wmi_acer_has_guid(AMW0_GUID1) && wmi_acer_has_guid(WMID_GUID1)) {
		DEBUG(0, "Detected Acer WMID interface\n");
		interface = &WMID_interface;
	}

	if (wmi_acer_has_guid(WMID_GUID2) && interface) {
		if (ACPI_FAILURE(WMID_set_capabilities())) {
			printk(ACER_ERR "Unable to detect available devices\n");
			return -ENODEV;
		}
	} else if (!wmi_acer_has_guid(WMID_GUID2) && interface) {
		printk(ACER_ERR "Unable to detect available devices\n");
		return -ENODEV;
	}

	if (wmi_acer_has_guid(AMW0_GUID1) && !wmi_acer_has_guid(WMID_GUID1)) {
		DEBUG(0, "Detected Acer AMW0 version 2 interface\n");
		interface = &AMW0_interface;

		if (ACPI_FAILURE(AMW0_set_capabilities())) {
			printk(ACER_ERR "Unable to detect available devices\n");
			return -ENODEV;
		}
	}

	if (wmi_acer_has_guid(AMW0_GUID1)) {
		if (ACPI_FAILURE(AMW0_find_mailled())) {
			printk(ACER_ERR "Unable to detect mail LED\n");
		}
	}

	/* Find if this laptop requires any quirks */
	DEBUG(1, "Finding quirks\n");
	find_quirks();

	if (!interface) {
		printk(ACER_ERR "No or unsupported WMI interface, unable to load.\n");
		return -ENODEV;
	}

#ifdef CONFIG_PROC
	/* Create the proc entries */
	acer_proc_dir = proc_mkdir(PROC_ACER, acpi_root_dir);
	if (!acer_proc_dir) {
		printk(ACER_ERR "Unable to create /proc entries, aborting.\n");
		goto error_proc_mkdir;
	}

	acer_proc_dir->owner = THIS_MODULE;
	if (add_proc_entries()) {
		printk(ACER_ERR "Unable to create /proc entries, aborting.\n");
		goto error_proc_add;
	}
#endif

	/*
	 * Register the driver
	 */
	if (platform_driver_register(&acer_platform_driver)) {
		printk(ACER_ERR "Unable to register platform driver, aborting.\n");
		goto error_platform_register;
	}
	acer_platform_device = platform_device_alloc("acer_acpi", -1);
	platform_device_add(acer_platform_device);

	create_sysfs();

	DEBUG(1, "Driver registered\n");

	/* Override any initial settings with values from the commandline */
	acer_commandline_init();

	return 0;

error_platform_register:
#ifdef CONFIG_PROC
	remove_proc_entries();
error_proc_add:
	if (acer_proc_dir)
		remove_proc_entry(PROC_ACER, acpi_root_dir);
error_proc_mkdir:
#endif
	return -ENODEV;
}

static void __exit acer_acpi_exit(void)
{
	remove_sysfs(acer_platform_device);
	platform_device_del(acer_platform_device);
	platform_driver_unregister(&acer_platform_driver);

#ifdef CONFIG_PROC
	remove_proc_entries();

	if (acer_proc_dir)
		remove_proc_entry(PROC_ACER, acpi_root_dir);
#endif
	printk(ACER_INFO "Acer Laptop ACPI Extras unloaded\n");
	return;
}

module_init(acer_acpi_init);
module_exit(acer_acpi_exit);
