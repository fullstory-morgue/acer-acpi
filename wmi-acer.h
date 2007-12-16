/*
 *  wmi.h
 *
 *  Copyright (C) 2007 Carlos Corbacho <cathectic@gmail.com>
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

/* Workaround needed for older kernels */
#ifndef bool
#define bool int
#endif

typedef void (*wmi_notify_handler) (u32 value, void *context);

extern acpi_status wmi_acer_evaluate_method(const char *guid, u8 instance,
					u32 method_id,
					const struct acpi_buffer *in,
					struct acpi_buffer *out);
extern acpi_status wmi_acer_query_block(const char *guid, u8 instance,
					struct acpi_buffer *out);
extern acpi_status wmi_acer_set_block(const char *guid, u8 instance,
					const struct acpi_buffer *in);
extern acpi_status wmi_acer_install_notify_handler(wmi_notify_handler handler,
					void *data);
extern acpi_status wmi_acer_remove_notify_handler(void);
extern acpi_status wmi_acer_get_event_data(u32 event, struct acpi_buffer *out);
extern bool wmi_acer_has_guid(const char *guid);
