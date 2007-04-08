/*
 *  acer_acpi.c - Acer Laptop ACPI Extras
 *
 *
 *  Copyright (C) 2005      E.M. Smith
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
 *  John Belmonte - the Toshiba ACPI driver I've adapted for this module.
 *  Julien Lerouge & Karol Kozimor - ASUS Acpi driver authors.
 *  Olaf Tauber - developer of acerhk, the inspiration to solve the 64-bit
 *                driver problem for my Aspire 5024.
 *  Mathieu Segaud - solved the ACPI problem that needed a double-modprobe
 *                   in version 0.2 and below.
 *
 */

#define ACER_ACPI_VERSION	"0.4"
#define PROC_INTERFACE_VERSION	1
#define PROC_ACER		"acer"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/suspend.h>
#include <asm/uaccess.h>

#include <acpi/acpi_drivers.h>

MODULE_AUTHOR("Mark Smith");
MODULE_DESCRIPTION("Acer Laptop ACPI Extras Driver");
MODULE_LICENSE("GPL");

#define MY_LOGPREFIX "acer_acpi: "
#define MY_ERR KERN_ERR MY_LOGPREFIX
#define MY_NOTICE KERN_NOTICE MY_LOGPREFIX
#define MY_INFO KERN_INFO MY_LOGPREFIX

/*
 * Capabilities - eventually we'll use these to determine which laptop
 * model requires the various proc entries creating. 
 */
#define ACER_WIRELESS	0x01
#define ACER_BLUETOOTH  0x02
#define ACER_MAILLED	0x04
#define ACER_ALLBITS	0xFFFFFFFF

/*
 * Acer ACPI method paths 
 */
#define WMI_METHOD		"\\_SB_.AMW0.WMAB"
#define WMI_GETDATA		"\\_SB_.AMW0._WED"

typedef struct _WMAB_args {
		u32 eax;
		u32 ebx;
		u32 ecx;
		u32 edx;
} WMAB_args;

typedef struct _ProcItem {
		const char *name;
		char *(*read_func) (char *);
		unsigned long (*write_func) (const char *, unsigned long);
		unsigned int capability;
} ProcItem;

struct acer_hotk {
		struct acpi_device *device;
		acpi_handle handle;
};

static struct proc_dir_entry *acer_proc_dir;

static int
is_valid_acpi_path(const char *methodName)
{
		acpi_handle handle;
		acpi_status status;

		status = acpi_get_handle(NULL, (char *) methodName, &handle);
		return !ACPI_FAILURE(status);
}

static acpi_status
WMAB_execute(WMAB_args * regbuf, struct acpi_buffer *result)
{
		struct acpi_object_list input;
		union acpi_object params[3];

		acpi_status status = AE_OK;
		input.count = 3;
		input.pointer = params;

		params[0].type = ACPI_TYPE_INTEGER;
		params[0].integer.value = 0x01;	/* Only one instance of this object */
		params[1].type = ACPI_TYPE_INTEGER;
		params[1].integer.value = 0x01;	/* Technically this should be method
																		 * ID */
		params[2].type = ACPI_TYPE_BUFFER;
		params[2].buffer.length = sizeof(WMAB_args);
		params[2].buffer.pointer = (u8 *) regbuf;

		status = acpi_evaluate_object(NULL, WMI_METHOD, &input, result);
		return status;
}

/*
 * proc file handlers 
 */

static int
dispatch_read(char *page, char **start, off_t off, int count, int *eof, ProcItem * item)
{
		char *p = page;
		int len;

		if (off == 0)
				p = item->read_func(p);

		/*
		 * ISSUE: I don't understand this code 
		 */
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
dispatch_write(struct file *file, const char __user * buffer, unsigned long count, ProcItem * item)
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
				result = item->write_func(tmp_buffer, count);
		}
		kfree(tmp_buffer);
		return result;
}

/*
 * Mail LED 
 */
static char *
read_mled(char *p)
{
		p += sprintf(p, "Sorry, reading status not yet implemented!\n");
		return p;
}

static unsigned long
write_mled(const char *buffer, unsigned long count)
{
		int value;
		WMAB_args args;

		if (sscanf(buffer, " enabled : %i", &value) == 1
				&& (value == 0 || value == 1)) {
				memset(&args, 0, sizeof(WMAB_args));
				args.eax = 0x9610;
				args.ebx = (value << 8) | 0x31;
				WMAB_execute(&args, NULL);
		} else {
				return -EINVAL;
		}
		return count;
}

/*
 * Bluetooth module 
 */
static char *
read_bt(char *p)
{
		p += sprintf(p, "Sorry, reading status not yet implemented!\n");
		return p;
}

static unsigned long
write_bt(const char *buffer, unsigned long count)
{
		int value;
		WMAB_args args;

		if (sscanf(buffer, " enabled : %i", &value) == 1
				&& (value == 0 || value == 1)) {
				memset(&args, 0, sizeof(WMAB_args));
				args.eax = 0x9610;
				args.ebx = (value << 8) | 0x34;
				WMAB_execute(&args, NULL);
		} else {
				return -EINVAL;
		}
		return count;
}

/*
 * Wireless LAN 
 */
static char *
read_wlan(char *p)
{
		p += sprintf(p, "Sorry, reading status not yet implemented!\n");
		return p;
}

static unsigned long
write_wlan(const char *buffer, unsigned long count)
{
		int value;
		WMAB_args args;

		if (sscanf(buffer, " enabled : %i", &value) == 1
				&& (value == 0 || value == 1)) {
				memset(&args, 0, sizeof(WMAB_args));
				args.eax = 0x9610;
				args.ebx = (value << 8) | 0x35;
				WMAB_execute(&args, NULL);
				printk(MY_INFO "Wireless value %i\n",value);
		} else {
				return -EINVAL;
		}
		return count;
}

static char *
read_version(char *p)
{
		p += sprintf(p, "driver:                  %s\n", ACER_ACPI_VERSION);
		p += sprintf(p, "proc_interface:          %d\n", PROC_INTERFACE_VERSION);
		return p;
}

ProcItem proc_items[] = {
		{"mailled", read_mled, write_mled, ACER_MAILLED} ,
		{"bluetooth", read_bt, write_bt, ACER_BLUETOOTH} ,
		{"wireless", read_wlan, write_wlan, ACER_WIRELESS} ,
		{"version", read_version, NULL, ACER_ALLBITS} ,
		{NULL}
};

static acpi_status __init
add_proc_entries(void)
{
		struct proc_dir_entry *proc;
		ProcItem *item;

		for (item = proc_items; item->name; ++item) {
				proc = create_proc_read_entry(item->name,
																			S_IFREG | S_IRUGO | S_IWUSR,
																			acer_proc_dir,
																			(read_proc_t *) dispatch_read, item);
				if (proc)
						proc->owner = THIS_MODULE;
				if (proc && item->write_func)
						proc->write_proc = (write_proc_t *) dispatch_write;
		}

		return AE_OK;
}

static acpi_status __exit
remove_proc_entries(void)
{
		ProcItem *item;

		for (item = proc_items; item->name; ++item)
				remove_proc_entry(item->name, acer_proc_dir);
		return AE_OK;
}

/*
 * TODO: make this actually do useful stuff, if we ever see events 
 */
static void
acer_acerkeys_notify(acpi_handle handle, u32 event, void *data)
{
		struct acer_hotk *hotk = (struct acer_hotk *) data;

		if (!hotk)
				return;
		printk(MY_ERR "Got an event!! %X", event);

		return;
}

static int
acpi_acerkeys_add(struct acpi_device *device)
{
		struct acer_hotk *hotk = NULL;
		acpi_status status = AE_OK;

		if (!device)
				return -EINVAL;

		hotk = (struct acer_hotk *) kmalloc(sizeof(struct acer_hotk), GFP_KERNEL);
		if (!hotk)
				return -ENOMEM;
		memset(hotk, 0, sizeof(struct acer_hotk));
		hotk->handle = device->handle;
		strcpy(acpi_device_name(device), "Acer Laptop ACPI Extras");
		strcpy(acpi_device_class(device), "hkey");
		acpi_driver_data(device) = hotk;
		hotk->device = device;

		status = acpi_install_notify_handler(hotk->handle, ACPI_SYSTEM_NOTIFY,
																				 acer_acerkeys_notify, hotk);
		if (ACPI_FAILURE(status))
				printk(MY_ERR "Error installing notify handler.\n");
		return 0;
}

static int
acpi_acerkeys_remove(struct acpi_device *device, int type)
{
		acpi_status status = 0;
		struct acer_hotk *hotk = NULL;

		if (!device || !acpi_driver_data(device))
				return -EINVAL;
		hotk = (struct acer_hotk *) acpi_driver_data(device);

		status = acpi_remove_notify_handler(hotk->handle, ACPI_SYSTEM_NOTIFY,
																				acer_acerkeys_notify);
		if (ACPI_FAILURE(status))
				printk(MY_ERR "Error removing notify handler.\n");
		kfree(hotk);

		return 0;
}

static struct acpi_driver acpi_acerkeys = {
		.name = "Acer Laptop ACPI Extras driver",
		.class = "hkey",
		.ids = "PNP0C14",
		.ops = {
						.add = acpi_acerkeys_add,
						.remove = acpi_acerkeys_remove,
						},
};

static int __init
acer_acpi_init(void)
{
		WMAB_args args;
		acpi_status status = AE_OK;

		printk(MY_INFO "Acer Laptop ACPI Extras version %s\n", ACER_ACPI_VERSION);
		if (acpi_disabled) {
				printk(MY_ERR "ACPI Disabled, unable to load.\n");
				return -ENODEV;
		}
		/*
		 * Check the WMI interface is there, if so, call it once so the BIOS
		 * knows it's to notify us of events via pnp0c14 
		 */
		if (is_valid_acpi_path(WMI_METHOD)) {
				memset(&args, 0, sizeof(WMAB_args));
				args.eax = 0x9610;
				args.ebx = 0;
				status = WMAB_execute(&args, NULL);
		} else {
				printk(MY_ERR "No WMI interface, unable to load.\n");
				return -ENODEV;
		}

		acer_proc_dir = proc_mkdir(PROC_ACER, acpi_root_dir);
		if (!acer_proc_dir) {
				status = AE_ERROR;
		} else {
				acer_proc_dir->owner = THIS_MODULE;
				status = add_proc_entries();
				if (ACPI_FAILURE(status))
						remove_proc_entry(PROC_ACER, acpi_root_dir);
		}

		if (ACPI_SUCCESS(status)) {
				status = acpi_bus_register_driver(&acpi_acerkeys);
				if (ACPI_FAILURE(status)) {
						remove_proc_entries();
						remove_proc_entry(PROC_ACER, acpi_root_dir);
						status = AE_ERROR;
						printk(MY_ERR "Unable to register driver, aborting.\n");
				}
		} else {
				printk(MY_ERR "Unable to create /proc entries, aborting.\n");
		}
		return (ACPI_SUCCESS(status)) ? 0 : -ENODEV;
}

static void __exit
acer_acpi_exit(void)
{
		acpi_bus_unregister_driver(&acpi_acerkeys);
		remove_proc_entries();
		if (acer_proc_dir)
				remove_proc_entry(PROC_ACER, acpi_root_dir);

		printk(MY_INFO "Acer Laptop ACPI Extras unloaded\n");
		return;
}

module_init(acer_acpi_init);
module_exit(acer_acpi_exit);
