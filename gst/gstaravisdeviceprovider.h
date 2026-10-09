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
 * Author: William Reed Seal Foss <sealfoss@gmail.com>
 */

#ifndef GST_ARAVIS_DEVICE_PROVIDER_H
#define GST_ARAVIS_DEVICE_PROVIDER_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_ARAVIS_DEVICE_PROVIDER		(gst_aravis_device_provider_get_type ())
#define GST_ARAVIS_DEVICE_PROVIDER(obj)		(G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_ARAVIS_DEVICE_PROVIDER, \
									     GstAravisDeviceProvider))
#define GST_IS_ARAVIS_DEVICE_PROVIDER(obj)	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_ARAVIS_DEVICE_PROVIDER))

typedef struct _GstAravisDeviceProvider GstAravisDeviceProvider;
typedef struct _GstAravisDeviceProviderClass GstAravisDeviceProviderClass;

struct _GstAravisDeviceProvider {
	GstDeviceProvider parent;
};

struct _GstAravisDeviceProviderClass {
	GstDeviceProviderClass parent_class;
};

GType gst_aravis_device_provider_get_type (void);

#define GST_TYPE_ARAVIS_DEVICE			(gst_aravis_device_get_type ())
#define GST_ARAVIS_DEVICE(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_ARAVIS_DEVICE, GstAravisDevice))
#define GST_IS_ARAVIS_DEVICE(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_ARAVIS_DEVICE))

typedef struct _GstAravisDevice GstAravisDevice;
typedef struct _GstAravisDeviceClass GstAravisDeviceClass;

struct _GstAravisDevice {
	GstDevice parent;

	/* The camera-name and interface properties of the elements of the device. interface_id is NULL if no interface
	 * was found to list the device. */
	char *device_id;
	char *interface_id;
};

struct _GstAravisDeviceClass {
	GstDeviceClass parent_class;
};

GType gst_aravis_device_get_type (void);

G_END_DECLS

#endif
