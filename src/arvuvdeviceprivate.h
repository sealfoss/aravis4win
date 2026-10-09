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

#ifndef ARV_UV_DEVICE_PRIVATE_H
#define ARV_UV_DEVICE_PRIVATE_H

#if !defined (ARV_H_INSIDE) && !defined (ARAVIS_COMPILATION)
#error "Only <arv.h> can be included directly."
#endif

#include <arvuvdevice.h>
#include <arvdeviceprivate.h>
#include <libusb.h>

G_BEGIN_DECLS

typedef enum {
	ARV_UV_ENDPOINT_CONTROL,
	ARV_UV_ENDPOINT_DATA
} ArvUvEndpointType;

gboolean        arv_uv_device_bulk_transfer             (ArvUvDevice *uv_device,
							 ArvUvEndpointType endpoint_type, unsigned char endpoint_flags,
							 void *data, size_t size, size_t *transferred_size,
							 guint32 timeout_ms, GError **error);

void            arv_uv_device_fill_bulk_transfer        (struct libusb_transfer* transfer, ArvUvDevice *uv_device,
                                                         ArvUvEndpointType endpoint_type, unsigned char endpoint_flags,
                                                         void *data, size_t size,
                                                         libusb_transfer_cb_fn callback, void* callback_data,
                                                         unsigned int timeout);

void *          arv_uv_device_usb_mem_alloc             (ArvUvDevice *uv_device, size_t size);
void            arv_uv_device_usb_mem_free              (ArvUvDevice *uv_device, void *data, size_t size);

gboolean        arv_uv_device_is_connected              (ArvUvDevice *uv_device);
gboolean        arv_uv_device_check_connection          (ArvUvDevice *uv_device);

gboolean        arv_uv_device_reset_stream_endpoint     (ArvUvDevice *device);

guint           arv_uv_device_get_stream_packet_size    (ArvUvDevice *uv_device);
guint           arv_uv_device_get_raw_io_maximum_transfer_size  (ArvUvDevice *uv_device);
gboolean        arv_uv_device_set_raw_io                (ArvUvDevice *uv_device, gboolean enable);
void            arv_uv_device_disable_raw_io            (ArvUvDevice *uv_device);
gboolean        arv_uv_device_clear_stream_halt         (ArvUvDevice *uv_device);
gboolean        arv_uv_device_has_hotplug               (ArvUvDevice *uv_device);

guint           arv_uv_device_take_stream_interface     (ArvUvDevice *uv_device);
gboolean        arv_uv_device_release_stream_interface  (ArvUvDevice *uv_device, guint owner);

gboolean        arv_uv_device_request_event_thread_priority     (ArvUvDevice *uv_device);
void            arv_uv_device_release_event_thread_priority     (ArvUvDevice *uv_device);

/* The functions libusb 1.0.30 added, for WinUSB's RAW_IO and the device strings of a device that is not open. On
 * Windows, with libusb as a DLL, Aravis looks them up at run time, in the libusb it uses (see arvuvdevice.c): the ones
 * it lacks fail with LIBUSB_ERROR_NOT_SUPPORTED. With libusb linked statically, which the build tells with
 * ARV_UV_LIBUSB_STATIC (see src/meson.build), and elsewhere, these call libusb directly. */

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C

#if defined (G_OS_WIN32) && !defined (ARV_UV_LIBUSB_STATIC)
#define ARV_UV_LIBUSB_RUN_TIME_LOOKUP	1
#endif

int             arv_uv_libusb_endpoint_supports_raw_io          (libusb_device_handle *dev_handle, uint8_t endpoint);
int             arv_uv_libusb_endpoint_set_raw_io               (libusb_device_handle *dev_handle, uint8_t endpoint,
                                                                 int enable);
int             arv_uv_libusb_get_max_raw_io_transfer_size      (libusb_device_handle *dev_handle, uint8_t endpoint);
int             arv_uv_libusb_get_device_string                 (libusb_device *dev,
                                                                 enum libusb_device_string_type string_type,
                                                                 char *data, int length);

#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
gboolean        arv_uv_libusb_look_functions_up                 (gconstpointer libusb_address,
                                                                 gconstpointer aravis_address);
#endif

#endif

G_END_DECLS

#endif
