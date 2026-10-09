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

/**
 * SECTION:provider-aravisdeviceprovider
 *
 * Lists the cameras of the Aravis interfaces enabled in the process for #GstDeviceMonitor, as "Video/Source"
 * devices whose element is an aravissrc. Fake and GenTL are disabled by default: GenTL cameras are only listed once
 * the GenTL interface is enabled, by arv_enable_interface () or the environment variable ARV_GENTL_ENABLE=1.
 *
 * Each probe runs arv_update_device_list (), so it costs what the discovery of the enabled interfaces costs: at least
 * one second for GigEVision, which broadcasts a discovery request on every network interface and waits for the
 * answers, and for USB3Vision, an enumeration of the USB devices. The provider has no start () and does not watch
 * the cameras: a started #GstDeviceMonitor lists the cameras found when it started. The environment variable
 * GST_ARAVIS_DEVICE_PROVIDER=0 (or off, false, no) disables the probes: no discovery, no camera listed. A probe lists
 * the cameras of one state of the device list, even if another thread updates the list meanwhile: updates that find
 * the same cameras don't disturb it, and a list that changes during each of 10 reads in a row gives no camera, until
 * the next probe.
 *
 * On Windows, the USB3Vision discovery opens each camera, while WinUSB lets a single handle open a device at a time: a
 * camera that the process is streaming, through an aravissrc or the Aravis API, is not listed meanwhile. Its stream is
 * not disturbed.
 *
 * Device properties: device.api (aravis), device.vendor.name, device.product.name and device.serial, then what
 * Aravis knows of the camera: aravis.device-id (the camera-name of the element), aravis.physical-id, aravis.address,
 * aravis.protocol and aravis.interface (the interface of the element). The fields Aravis does not know are left out.
 */

#include <gstaravisdeviceprovider.h>
#include <gstaravis.h>

GST_DEBUG_CATEGORY_STATIC (gst_aravis_device_provider_debug);
#define GST_CAT_DEFAULT gst_aravis_device_provider_debug

/* The caps of aravissrc depend on the pixel formats of the camera, and are only known once it is open. Without
 * opening it, these are the media types it may produce. */
#define GST_ARAVIS_DEVICE_CAPS		"video/x-raw; video/x-bayer"
#define GST_ARAVIS_DEVICE_CLASS		"Video/Source"

/* GstDeviceMonitor uses the provider in every application, which pays its discovery cost. This environment variable
 * disables it without changing the application. */
#define GST_ARAVIS_DEVICE_PROVIDER_ENV	"GST_ARAVIS_DEVICE_PROVIDER"

/* Device */

G_DEFINE_TYPE (GstAravisDevice, gst_aravis_device, GST_TYPE_DEVICE);

static GstElement *
gst_aravis_device_create_element (GstDevice *device, const gchar *name)
{
	GstAravisDevice *aravis_device = GST_ARAVIS_DEVICE (device);
	GstElement *element;

	element = gst_element_factory_make ("aravissrc", name);
	if (element == NULL)
		return NULL;

	/* Without opening the camera now, as the camera-name setter would: it opens when the element starts */
	gst_aravis_select_camera (GST_ARAVIS (element), aravis_device->interface_id, aravis_device->device_id);

	return element;
}

static void
gst_aravis_device_init (GstAravisDevice *aravis_device)
{
	aravis_device->device_id = NULL;
	aravis_device->interface_id = NULL;
}

static void
gst_aravis_device_finalize (GObject *object)
{
	GstAravisDevice *aravis_device = GST_ARAVIS_DEVICE (object);

	g_clear_pointer (&aravis_device->device_id, g_free);
	g_clear_pointer (&aravis_device->interface_id, g_free);

	G_OBJECT_CLASS (gst_aravis_device_parent_class)->finalize (object);
}

static void
gst_aravis_device_class_init (GstAravisDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GstDeviceClass *device_class = GST_DEVICE_CLASS (klass);

	object_class->finalize = gst_aravis_device_finalize;

	device_class->create_element = GST_DEBUG_FUNCPTR (gst_aravis_device_create_element);
}

/* Device provider */

G_DEFINE_TYPE (GstAravisDeviceProvider, gst_aravis_device_provider, GST_TYPE_DEVICE_PROVIDER);

/* Whether GST_ARAVIS_DEVICE_PROVIDER disables the probes: 0, off, false or no */

static gboolean
gst_aravis_device_provider_is_disabled (void)
{
	const char *value = g_getenv (GST_ARAVIS_DEVICE_PROVIDER_ENV);

	return value != NULL &&
		(g_strcmp0 (value, "0") == 0 ||
		 g_ascii_strcasecmp (value, "off") == 0 ||
		 g_ascii_strcasecmp (value, "false") == 0 ||
		 g_ascii_strcasecmp (value, "no") == 0);
}

/* "Vendor Model (serial)", from what is known */

static char *
gst_aravis_device_provider_build_display_name (const char *device_id, const char *vendor, const char *model,
					       const char *serial_number)
{
	GString *name = g_string_new (NULL);

	if (vendor != NULL && vendor[0] != '\0')
		g_string_append (name, vendor);
	if (model != NULL && model[0] != '\0')
		g_string_append_printf (name, "%s%s", name->len > 0 ? " " : "", model);
	if (name->len == 0)
		g_string_append (name, device_id);
	if (serial_number != NULL && serial_number[0] != '\0')
		g_string_append_printf (name, " (%s)", serial_number);

	return g_string_free (name, FALSE);
}

static void
gst_aravis_device_provider_set_field (GstStructure *properties, const char *name, const char *value)
{
	if (value != NULL && value[0] != '\0')
		gst_structure_set (properties, name, G_TYPE_STRING, value, NULL);
}

/* What the device list of Aravis tells of the device at an index */

typedef struct {
	const char *device_id;
	const char *vendor;
	const char *model;
	const char *serial_number;
	const char *protocol;
	const char *physical_id;
	const char *address;
	const char *interface_id;
} GstAravisDeviceInfos;

/* The LOG message, between the first query and the others, is where tests/fakeuvgst.c updates the device list while
 * the provider reads it */

static void
gst_aravis_device_provider_read_infos (unsigned int index, GstAravisDeviceInfos *infos)
{
	infos->device_id = arv_get_device_id (index);
	GST_LOG ("Reading device %u: '%s'", index, infos->device_id != NULL ? infos->device_id : "(none)");
	infos->vendor = arv_get_device_vendor (index);
	infos->model = arv_get_device_model (index);
	infos->serial_number = arv_get_device_serial_nbr (index);
	infos->protocol = arv_get_device_protocol (index);
	infos->physical_id = arv_get_device_physical_id (index);
	infos->address = arv_get_device_address (index);
	infos->interface_id = arv_get_device_interface_id (index);
}

/* The devices of the device list of Aravis, after arv_update_device_list (), read from a single state of the list: an
 * array of GstAravisDeviceInfos.
 *
 * Each query of the list takes its lock on its own, so another thread may update the list between two queries: the
 * application, or an aravissrc opening the first camera of an interface. The list is read again until its generation
 * is the same before and after the queries, which then all describe the same state of the list: no device mixes two
 * cameras, and none is left out or read twice for moving to another index meanwhile. The strings stay valid whatever
 * the updates. The generation changes with the content of the list, not with each update: a thread that keeps
 * updating a list whose devices stay the same never makes the provider read it again. A list that still changes after
 * GST_ARAVIS_DEVICE_PROVIDER_MAX_READS reads gives no device: the next probe lists them. */

#define GST_ARAVIS_DEVICE_PROVIDER_MAX_READS	10

static GArray *
gst_aravis_device_provider_read_list (void)
{
	GArray *list = g_array_new (FALSE, TRUE, sizeof (GstAravisDeviceInfos));
	unsigned int n_reads;

	for (n_reads = 1; n_reads <= GST_ARAVIS_DEVICE_PROVIDER_MAX_READS; n_reads++) {
		guint64 generation = arv_get_device_list_generation ();
		unsigned int n_devices = arv_get_n_devices ();
		unsigned int i;

		g_array_set_size (list, n_devices);
		for (i = 0; i < n_devices; i++)
			gst_aravis_device_provider_read_infos (i, &g_array_index (list, GstAravisDeviceInfos, i));

		if (arv_get_device_list_generation () == generation)
			return list;

		GST_DEBUG ("The device list changed while it was read");
	}

	GST_INFO ("No device listed: the device list kept changing during %u reads",
		  GST_ARAVIS_DEVICE_PROVIDER_MAX_READS);
	g_array_set_size (list, 0);

	return list;
}

/* The device described by infos, or NULL if there is none.
 *
 * The interface of the device lets the element open it through that interface only, instead of asking every enabled
 * interface in turn: the protocol alone doesn't tell, as a camera found by a GenTL producer has the protocol of its
 * transport layer, USB3Vision or GigEVision for example. */

static GstDevice *
gst_aravis_device_provider_new_device (const GstAravisDeviceInfos *infos)
{
	GstAravisDevice *device;
	GstStructure *properties;
	GstCaps *caps;
	char *display_name;

	if (infos->device_id == NULL)
		return NULL;

	properties = gst_structure_new ("aravis-proplist", "device.api", G_TYPE_STRING, "aravis", NULL);
	gst_aravis_device_provider_set_field (properties, "device.vendor.name", infos->vendor);
	gst_aravis_device_provider_set_field (properties, "device.product.name", infos->model);
	gst_aravis_device_provider_set_field (properties, "device.serial", infos->serial_number);
	gst_aravis_device_provider_set_field (properties, "aravis.device-id", infos->device_id);
	gst_aravis_device_provider_set_field (properties, "aravis.physical-id", infos->physical_id);
	gst_aravis_device_provider_set_field (properties, "aravis.address", infos->address);
	gst_aravis_device_provider_set_field (properties, "aravis.protocol", infos->protocol);
	gst_aravis_device_provider_set_field (properties, "aravis.interface", infos->interface_id);

	display_name = gst_aravis_device_provider_build_display_name (infos->device_id, infos->vendor, infos->model,
								      infos->serial_number);
	caps = gst_caps_from_string (GST_ARAVIS_DEVICE_CAPS);

	device = g_object_new (GST_TYPE_ARAVIS_DEVICE,
			       "display-name", display_name,
			       "device-class", GST_ARAVIS_DEVICE_CLASS,
			       "caps", caps,
			       "properties", properties,
			       NULL);
	device->device_id = g_strdup (infos->device_id);
	device->interface_id = g_strdup (infos->interface_id);

	GST_DEBUG ("Device '%s' on interface %s: %" GST_PTR_FORMAT, infos->device_id,
		   infos->interface_id != NULL ? infos->interface_id : "(unknown)", properties);

	g_free (display_name);
	gst_caps_unref (caps);
	gst_structure_free (properties);

	return GST_DEVICE (device);
}

/* Each identifier is listed once: the element opens a single camera by its identifier, while one state of the device
 * list may give an identifier twice, for two cameras with the same vendor, model and serial number for example */

static GList *
gst_aravis_device_provider_probe (GstDeviceProvider *provider)
{
	GHashTable *device_ids;
	GList *devices = NULL;
	GArray *list;
	unsigned int i;
	gint64 start_time;

	if (gst_aravis_device_provider_is_disabled ()) {
		GST_INFO_OBJECT (provider, "Disabled by %s=%s", GST_ARAVIS_DEVICE_PROVIDER_ENV,
				 g_getenv (GST_ARAVIS_DEVICE_PROVIDER_ENV));
		return NULL;
	}

	start_time = g_get_monotonic_time ();

	arv_update_device_list ();

	list = gst_aravis_device_provider_read_list ();
	device_ids = g_hash_table_new (g_str_hash, g_str_equal);

	for (i = 0; i < list->len; i++) {
		const GstAravisDeviceInfos *infos = &g_array_index (list, GstAravisDeviceInfos, i);
		GstDevice *device = gst_aravis_device_provider_new_device (infos);

		if (device == NULL)
			continue;

		/* The table holds the identifiers of the devices listed, which outlive it. Not g_hash_table_add ()
		 * alone: for an identifier already in the table, it would keep the identifier of the duplicate instead,
		 * freed below, which the next look-up of that identifier would read. */
		if (g_hash_table_contains (device_ids, GST_ARAVIS_DEVICE (device)->device_id)) {
			GST_INFO_OBJECT (provider, "Device '%s' found twice, listed once",
					 GST_ARAVIS_DEVICE (device)->device_id);
			gst_object_unref (gst_object_ref_sink (device));
			continue;
		}

		g_hash_table_add (device_ids, GST_ARAVIS_DEVICE (device)->device_id);
		devices = g_list_prepend (devices, device);
	}

	g_hash_table_unref (device_ids);
	g_array_unref (list);

	GST_INFO_OBJECT (provider, "Found %u camera%s in %" G_GINT64_FORMAT " ms", g_list_length (devices),
			 g_list_length (devices) == 1 ? "" : "s", (g_get_monotonic_time () - start_time) / 1000);

	return g_list_reverse (devices);
}

static void
gst_aravis_device_provider_init (GstAravisDeviceProvider *provider)
{
}

static void
gst_aravis_device_provider_class_init (GstAravisDeviceProviderClass *klass)
{
	GstDeviceProviderClass *provider_class = GST_DEVICE_PROVIDER_CLASS (klass);

	provider_class->probe = GST_DEBUG_FUNCPTR (gst_aravis_device_provider_probe);

	gst_device_provider_class_set_static_metadata (provider_class,
						       "Aravis Device Provider",
						       "Source/Video",
						       "Lists the cameras found by Aravis, unless the environment"
						       " variable " GST_ARAVIS_DEVICE_PROVIDER_ENV "=0",
						       "William Reed Seal Foss <sealfoss@gmail.com>");

	GST_DEBUG_CATEGORY_INIT (gst_aravis_device_provider_debug, "aravisdeviceprovider", 0,
				 "Aravis device provider");
}
