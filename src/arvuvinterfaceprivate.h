/* Aravis - Digital camera library
 *
 * Copyright © 2009-2025 Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 */

#ifndef ARV_UV_INTERFACE_PRIVATE_H
#define ARV_UV_INTERFACE_PRIVATE_H

#if !defined (ARV_H_INSIDE) && !defined (ARAVIS_COMPILATION)
#error "Only <arv.h> can be included directly."
#endif

#include <arvuvinterface.h>
#include <arvinterfaceprivate.h>
#include <libusb.h>

#define ARV_UV_INTERFACE_DEVICE_CLASS			0xef	/* Miscellaneous device */
#define ARV_UV_INTERFACE_DEVICE_SUBCLASS		0x02
#define ARV_UV_INTERFACE_DEVICE_PROTOCOL		0x01
#define ARV_UV_INTERFACE_INTERFACE_CLASS		0xef
#define ARV_UV_INTERFACE_INTERFACE_SUBCLASS		0x05
#define ARV_UV_INTERFACE_CONTROL_PROTOCOL		0x00
#define ARV_UV_INTERFACE_EVENT_PROTOCOL			0x01
#define ARV_UV_INTERFACE_DATA_PROTOCOL			0x02
#define ARV_UV_INTERFACE_GUID_INDEX_OFFSET		11
#define ARV_UV_INTERFACE_VENDOR_NAME_INDEX_OFFSET	12
#define ARV_UV_INTERFACE_MODEL_NAME_INDEX_OFFSET	13
#define ARV_UV_INTERFACE_FAMILY_NAME_INDEX_OFFSET	14
#define ARV_UV_INTERFACE_SERIAL_NUMBER_INDEX_OFFSET	17

G_BEGIN_DECLS

/* String descriptor indices read from the device info block of a USB3 Vision control interface, -1 when absent */

typedef struct {
	int guid;
	int vendor_name;
	int model_name;
	int serial_number;
} ArvUvInterfaceStringIndices;

/* Where a USB device is: its bus number, its address on that bus, and the port numbers from the root hub to it. libusb
 * gives the same values to every context of a process, and a device plugged in again gets another address. */

#define ARV_UV_INTERFACE_PORT_NUMBERS_MAX		7

typedef struct {
	guint8 bus_number;
	guint8 device_address;
	int n_port_numbers;
	guint8 port_numbers[ARV_UV_INTERFACE_PORT_NUMBERS_MAX];
} ArvUvInterfaceDeviceLocation;

void 			arv_uv_interface_destroy_instance 	(void);

gboolean		arv_uv_interface_match_descriptors	(libusb_device *device,
								 struct libusb_device_descriptor *device_descriptor,
								 ArvUvInterfaceStringIndices *string_indices);

const char *		arv_uv_interface_get_open_error_hint	(int libusb_error);
char *			arv_uv_interface_describe_open_error	(libusb_device *device, int libusb_error,
								 gboolean with_device_strings);
char *			arv_uv_interface_dup_open_errors	(ArvUvInterface *uv_interface);

void			arv_uv_interface_get_device_location	(libusb_device *device,
								 ArvUvInterfaceDeviceLocation *location);
gboolean		arv_uv_interface_is_device_at_location	(libusb_device *device,
								 const ArvUvInterfaceDeviceLocation *location);
void			arv_uv_interface_remember_device_location	(const char *guid, libusb_device *device);
gboolean		arv_uv_interface_lookup_device_location	(const char *guid,
								 ArvUvInterfaceDeviceLocation *location);
void			arv_uv_interface_forget_device_locations	(void);

G_END_DECLS

#endif
