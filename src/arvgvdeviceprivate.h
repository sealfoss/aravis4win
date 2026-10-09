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

#ifndef ARV_GV_DEVICE_PRIVATE_H
#define ARV_GV_DEVICE_PRIVATE_H

#if !defined (ARV_H_INSIDE) && !defined (ARAVIS_COMPILATION)
#error "Only <arv.h> can be included directly."
#endif

#include <arvgvdevice.h>

G_BEGIN_DECLS

#if ARAVIS_HAS_FAST_HEARTBEAT
    #define ARV_GV_DEVICE_GVCP_N_RETRIES_DEFAULT    3
    #define ARV_GV_DEVICE_GVCP_TIMEOUT_MS_DEFAULT   25
    #define ARV_GV_DEVICE_HEARTBEAT_PERIOD_US       50000
    #define ARV_GV_DEVICE_HEARTBEAT_RETRY_DELAY_US  1000
    #define ARV_GV_DEVICE_HEARTBEAT_RETRY_TIMEOUT_S 0.25
#else
    #define ARV_GV_DEVICE_GVCP_N_RETRIES_DEFAULT    5
    #define ARV_GV_DEVICE_GVCP_TIMEOUT_MS_DEFAULT   500
    #define ARV_GV_DEVICE_HEARTBEAT_PERIOD_US       1000000
    #define ARV_GV_DEVICE_HEARTBEAT_RETRY_DELAY_US  10000
    #define ARV_GV_DEVICE_HEARTBEAT_RETRY_TIMEOUT_S 5.0		/* FIXME */
#endif

#define ARV_GV_DEVICE_GVSP_PACKET_SIZE_DEFAULT	1500

#define ARV_GV_DEVICE_BUFFER_SIZE	1024

GRegex * 		arv_gv_device_get_url_regex 			(void);
void                    arv_gc_set_default_gv_features                  (ArvGc *genicam);

/* The stream which configured a stream channel last owns it, and disables it when destroyed. A stream configures the
 * channel and takes it, or releases it and disables it, with the stream channel lock held. */
void			arv_gv_device_lock_stream_channels		(ArvGvDevice *gv_device);
void			arv_gv_device_unlock_stream_channels		(ArvGvDevice *gv_device);
void			arv_gv_device_take_stream_channel		(ArvGvDevice *gv_device, guint channel,
									 gconstpointer stream);
gboolean		arv_gv_device_release_stream_channel		(ArvGvDevice *gv_device, guint channel,
									 gconstpointer stream);

/* Packet size negotiation logic of arv_gv_device_auto_packet_size(). The test function sets the packet size, updates
 * it with the value the device actually uses, and returns whether a test packet of that size reached the host. */

typedef gboolean	(*ArvGvDevicePacketSizeTest)			(guint *packet_size, void *user_data, GError **error);

/* private, but used by tests */
ARV_API guint		arv_gv_device_search_packet_size		(guint packet_size,
									 guint min_size, guint max_size, guint inc,
									 guint mtu, gboolean exit_early,
									 ArvGvDevicePacketSizeTest test_func, void *test_data,
									 GError **error);

G_END_DECLS

#endif

