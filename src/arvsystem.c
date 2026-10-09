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

#include <arvsystemprivate.h>
#include <arvgvinterfaceprivate.h>
#include <arvfeatures.h>
#if ARAVIS_HAS_USB
#include <arvuvinterfaceprivate.h>
#endif
#if ARAVIS_HAS_V4L2
#include <arvv4l2interfaceprivate.h>
#endif
#include <arvfakeinterfaceprivate.h>
#include <arvgentlinterfaceprivate.h>
#include <arvdevice.h>
#include <arvdebugprivate.h>
#include <string.h>
#include <arvmisc.h>
#include <arvdomimplementation.h>

static GMutex arv_system_mutex;

/* Increased under arv_system_mutex when an interface joins or leaves the device list, and by arv_shutdown (). Added to
 * the count of the changes of the device lists of the interfaces, it makes the generation of the device list, see
 * arv_get_device_list_generation (). */
static guint64 arv_system_device_list_changes = 0;

/**
 * SECTION: arv
 * @short_description: Device discovery and instantiation
 *
 * This module contans a set of APIs that allows to list and enable/disable the available interfaces,
 * and list and instantiate devices.
 */

typedef struct {
	const char *interface_id;
        const char *protocol;
	gboolean is_available;
	ArvInterface * 	(*get_interface_instance) 	(void);
	void 		(*destroy_interface_instance) 	(void);
} ArvInterfaceInfos;

ArvInterfaceInfos interfaces[] = {
	{
		.interface_id = "Fake",
                .protocol = "Fake",
		.is_available = FALSE,
		.get_interface_instance = arv_fake_interface_get_instance,
		.destroy_interface_instance =  arv_fake_interface_destroy_instance
	},
#if ARAVIS_HAS_USB
	{	.interface_id = "USB3Vision",
                .protocol = "USB3Vision",
		.is_available = TRUE,
		.get_interface_instance = arv_uv_interface_get_instance,
		.destroy_interface_instance = arv_uv_interface_destroy_instance
	},
#endif
#if ARAVIS_HAS_V4L2
	{	.interface_id = "V4L2",
		.is_available = TRUE,
		.get_interface_instance = arv_v4l2_interface_get_instance,
		.destroy_interface_instance = arv_v4l2_interface_destroy_instance
	},
#endif
	{	.interface_id = "GigEVision",
                .protocol = "GigEVision",
		.is_available = TRUE,
		.get_interface_instance = arv_gv_interface_get_instance,
		.destroy_interface_instance = arv_gv_interface_destroy_instance
	},
	{	.interface_id = "GenTL",
                .protocol = "Mixed",
		/* Opt-in, as every GenTL producer found in the GenTL path is loaded on discovery */
		.is_available = FALSE,
		.get_interface_instance = arv_gentl_interface_get_instance,
		.destroy_interface_instance = arv_gentl_interface_destroy_instance
	}
};

static gboolean
_is_environment_flag_set (const char *name)
{
	const char *value = g_getenv (name);

	return value != NULL &&
		(g_strcmp0 (value, "1") == 0 ||
		 g_ascii_strcasecmp (value, "on") == 0 ||
		 g_ascii_strcasecmp (value, "true") == 0 ||
		 g_ascii_strcasecmp (value, "yes") == 0);
}

/* Default interface selection from the environment, applied once, before the first change of the selection by the
 * application. */

static void
arv_system_init_interfaces (void)
{
	static gsize is_initialized = 0;

	if (g_once_init_enter (&is_initialized)) {
		guint i;

		if (_is_environment_flag_set ("ARV_GENTL_ENABLE")) {
			for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
				if (g_strcmp0 (interfaces[i].interface_id, "GenTL") == 0)
					interfaces[i].is_available = TRUE;
			arv_info_interface ("[Arv::init_interfaces] GenTL enabled by ARV_GENTL_ENABLE");
		}

		g_once_init_leave (&is_initialized, 1);
	}
}

/* Enables or disables an interface for the device list, with arv_system_mutex held */

static void
arv_system_set_interface_available_locked (ArvInterfaceInfos *infos, gboolean is_available)
{
	if ((infos->is_available != FALSE) == (is_available != FALSE))
		return;

	infos->is_available = is_available;

	/* Its devices join or leave the device list */
	arv_system_device_list_changes++;
}

/**
 * arv_get_n_interfaces:
 *
 * Gets the number of available interfaces, including the disabled ones.
 *
 * Returns: The number of available interfaces.
 */

unsigned int
arv_get_n_interfaces (void)
{
	return G_N_ELEMENTS (interfaces);
}


/**
 * arv_get_interface:
 *
 * Gets an interface by index.
 *
 * Return value: (transfer none): An #ArvInterface instance.
 */
ArvInterface*
arv_get_interface (unsigned int index)
{
	if (index >= G_N_ELEMENTS (interfaces))
		return NULL;

	return interfaces[index].get_interface_instance ();
}

/**
 * arv_get_interface_by_id:
 *
 * Gets an interface by id, whether it is enabled or not.
 *
 * The methods of the interface, like arv_interface_update_device_list() and arv_interface_open_device(), don't take
 * the lock of the device list functions: they must not be used while another thread, for example the Aravis
 * GStreamer plugin, may use the device list. To open a device through a disabled interface, for example a GenTL
 * device without enabling the 'GenTL' interface for global discovery, arv_open_device_on_interface() is safe to use
 * from any thread.
 *
 * Return value: (transfer none): An #ArvInterface instance.
 */
ArvInterface*
arv_get_interface_by_id (const char* interface_id)
{
	guint i;

	g_return_val_if_fail (interface_id != NULL, NULL);

	for (i = 0; i < G_N_ELEMENTS (interfaces) ; i++)
		if (strcmp (interface_id, interfaces[i].interface_id) == 0) {
			return interfaces[i].get_interface_instance ();
		}

	return NULL;
}

/**
 * arv_get_interface_id:
 * @index: interface index
 *
 * Retrieves the interface identifier. Possible values are 'Fake', 'USB3Vision',
 * 'GigEVision' and 'GenTL'.
 *
 * Returns: The interface identifier string.
 */

const char *
arv_get_interface_id (unsigned int index)
{
	if (index >= G_N_ELEMENTS (interfaces))
		return NULL;

	return interfaces[index].interface_id;
}

/**
 * arv_get_interface_protocol:
 * @index: interface index
 *
 * Retrieves the interface protocol. Possible values are 'Fake', 'USB3Vision',
 * 'GigEVision' and 'Mixed'.
 *
 * Returns: The interface protocol string.
 *
 * Since: 0.10.0
 */

const char *
arv_get_interface_protocol (unsigned int index)
{
	if (index >= G_N_ELEMENTS (interfaces))
		return NULL;

	return interfaces[index].protocol;
}

/**
 * arv_enable_interface:
 * @interface_id: name of the interface
 *
 * Enable an interface by name. By default, all interfaces are enabled, except 'Fake' and 'GenTL'.
 *
 * Only the enabled interfaces are used by arv_update_device_list() and arv_open_device().
 *
 * The 'GenTL' interface loads every GenTL producer found in the paths listed by the `GENICAM_GENTL64_PATH`
 * environment variable (`GENICAM_GENTL32_PATH` for 32-bit builds). Besides this function, it can be enabled by
 * setting the `ARV_GENTL_ENABLE` environment variable to `1`, which is read before the first change of the
 * interface selection. The `ARV_GENTL_PRODUCERS` environment variable restricts the loaded producers to a list
 * of file names. A device can still be opened through a disabled interface by arv_open_device_on_interface().
 */

void
arv_enable_interface (const char *interface_id)
{
	guint i;

	g_return_if_fail (interface_id != NULL);

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces) ; i++)
		if (strcmp (interface_id, interfaces[i].interface_id) == 0) {
			arv_system_set_interface_available_locked (&interfaces[i], TRUE);
			g_mutex_unlock (&arv_system_mutex);
			return;
		}

	g_mutex_unlock (&arv_system_mutex);

	g_warning ("[Arv::enable_interface] Unknown interface '%s'", interface_id);
}

/**
 * arv_select_interface:
 * @interface_id: name of the interface
 *
 * Enable an interface by name and disable all the others.
 *
 * Since: 0.8.35
 */

void
arv_select_interface (const char *interface_id)
{
	guint i;

	g_return_if_fail (interface_id != NULL);

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces) ; i++)
		arv_system_set_interface_available_locked (&interfaces[i],
							   g_strcmp0 (interface_id, interfaces[i].interface_id) == 0);

	g_mutex_unlock (&arv_system_mutex);
}

/**
 * arv_disable_interface:
 * @interface_id: name of the interface
 *
 * Disable an interface by name. By default, all interfaces are enabled, except 'Fake' and 'GenTL'.
 */

void
arv_disable_interface (const char *interface_id)
{
	guint i;

	g_return_if_fail (interface_id != NULL);

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
		if (strcmp (interface_id, interfaces[i].interface_id) == 0) {
			arv_system_set_interface_available_locked (&interfaces[i], FALSE);
			g_mutex_unlock (&arv_system_mutex);
			return;
		}

	g_mutex_unlock (&arv_system_mutex);

	g_warning ("[Arv::enable_interface] Unknown interface '%s'", interface_id);
}

/**
 * arv_set_interface_flags:
 * @interface_id: name of the interface
 * @flags: interface flags
 *
 * Set the device specific flags.
 *
 * Since: 0.8.23
 */

void
arv_set_interface_flags(const char *interface_id, int flags)
{
	guint i;

	g_return_if_fail (interface_id != NULL);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
		if (strcmp (interface_id, interfaces[i].interface_id) == 0) {
                        ArvInterface *iface;

			iface = interfaces[i].get_interface_instance ();
			arv_interface_set_flags (iface, flags);
			return;
		}

	g_warning ("[Arv::enable_interface] Unknown interface '%s'", interface_id);
}

/**
 * arv_update_device_list:
 *
 * Updates the list of currently online devices.
 *
 * The strings returned by arv_get_device_id() and the other functions querying the device list remain valid after
 * the following updates of the list, which another thread may run at any time: they are kept until the process
 * exits, arv_shutdown() included. The memory they take is bounded by the number of distinct strings these functions
 * have returned, not by the number of updates or queries. It only grows when devices with new identifiers, addresses
 * or names are queried, for example GigE Vision devices given new addresses by DHCP, or devices answering the
 * discovery with changing strings.
 *
 * An update that finds other devices, or other identifiers, names or addresses for them, increases the generation of
 * the device list, while one that finds the same devices leaves it as it is, see arv_get_device_list_generation().
 **/

void
arv_update_device_list (void)
{
	unsigned int i;

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;

		if (interfaces[i].is_available) {
			interface = interfaces[i].get_interface_instance ();
			arv_interface_update_device_list (interface);
		}
	}

	g_mutex_unlock (&arv_system_mutex);
}

/**
 * arv_get_device_list_generation:
 *
 * Gets the generation of the device list: a number that increases whenever the list changes.
 *
 * The list changes when an update of the device list of an interface finds other devices, or other identifiers, names
 * or addresses for them. These updates are made by arv_update_device_list(), by arv_open_device() and
 * arv_open_device_on_interface(), which may update the list of an interface to find a device, and by
 * arv_interface_update_device_list(). The list also changes when an interface joins or leaves it, through
 * arv_enable_interface(), arv_disable_interface() or arv_select_interface(), and at arv_shutdown(). An update that
 * finds the same devices, as most do, leaves the generation as it is: a thread that updates the list again and again
 * doesn't keep the others from reading it. The generation may also increase while the list stays the same, for example
 * when the device list of an interface disabled for device discovery changes.
 *
 * Each function querying the device list, like arv_get_n_devices() and arv_get_device_id(), takes the lock of the list
 * on its own, so another thread may change the list between two queries. Queries made between two reads of the same
 * generation describe one state of the list. To read several devices, or several fields of a device, from one state
 * of the list, read the generation before and after the queries, and query again if it changed. The strings the
 * queries return remain valid whatever the changes, see arv_update_device_list().
 *
 * Returns: the generation of the device list
 *
 * Since: 0.10.0
 */

guint64
arv_get_device_list_generation (void)
{
	guint64 generation;

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	/* Both counts only increase: their sum changes whenever one of them does. The interfaces count the changes of
	 * their own device lists, whoever updates them. */
	generation = arv_system_device_list_changes + arv_interface_get_device_list_changes ();

	g_mutex_unlock (&arv_system_mutex);

	return generation;
}

/**
 * arv_get_n_devices:
 *
 * Retrieves the number of currently online devices. This value is valid until
 * the next call to arv_update_device_list().
 *
 * Returns: The number of currently online devices.
 **/

unsigned int
arv_get_n_devices (void)
{
	unsigned int n_devices = 0;
	unsigned int i;

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;

		if (interfaces[i].is_available) {
			interface = interfaces[i].get_interface_instance ();
			n_devices += arv_interface_get_n_devices (interface);
		}
	}

	g_mutex_unlock (&arv_system_mutex);

	return n_devices;
}

static const char *
arv_get_info (unsigned int index, const char *get_info (ArvInterface *, guint))
{
	unsigned int offset = 0;
	unsigned int i;
	const char *info;

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;
		unsigned int n_devices;

		if (interfaces[i].is_available) {
			interface = interfaces[i].get_interface_instance ();
			n_devices = arv_interface_get_n_devices (interface);

			if (index - offset < n_devices) {
				/* Interned: the next update of the device list, possibly by another thread as soon
				 * as the lock is released, frees the string of the interface */
				info = g_intern_string (get_info (interface, index - offset));

				g_mutex_unlock (&arv_system_mutex);

				return info;
			}

			offset += n_devices;
		}
	}

	g_mutex_unlock (&arv_system_mutex);

	return NULL;
}

/**
 * arv_get_device_id:
 * @index: device index
 *
 * Queries the unique device id corresponding to index.  Prior to this
 * call the arv_update_device_list() function must be called.
 *
 * Like the strings of the other functions querying the device list, the returned string is kept until the process
 * exits: see arv_update_device_list().
 *
 * Returns: a unique device id
 *
 * Since: 0.2.0
 */

const char *
arv_get_device_id (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_id);
}

/**
 * arv_get_device_physical_id:
 * @index: device index
 *
 * Queries the physical device id corresponding to index such
 * as the MAC address for Ethernet based devices, bus id for PCI,
 * USB or Firewire based devices.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: a physical device id
 *
 * Since: 0.2.0
 */

const char *
arv_get_device_physical_id (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_physical_id);
}

/**
 * arv_get_device_vendor:
 * @index: device index
 *
 * Queries the device vendor.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device vendor, NULL on error
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_vendor	(unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_vendor);
}

/**
 * arv_get_device_manufacturer_info:
 * @index: device index
 *
 * Queries the device manufacturer info.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device manufacturer info, NULL on error
 *
 * Since: 0.8.20
 */

const char *
arv_get_device_manufacturer_info (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_manufacturer_info);
}

/**
 * arv_get_device_model:
 * @index: device index
 *
 * Queries the device model.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device model, NULL on error
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_model (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_model);
}

/**
 * arv_get_device_serial_nbr:
 * @index: device index
 *
 * Queries the device serial.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device serial, NULL on error
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_serial_nbr (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_serial_nbr);
}

/**
 * arv_get_device_address:
 * @index: device index
 *
 * The index of a device may change after a call to arv_update_device_list().
 *
 * Returns: The address of the device corresponding to @index as a string.
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_address (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_address);
}

/**
 * arv_get_device_protocol:
 * @index: device index
 *
 * The index of a device may change after a call to arv_update_device_list().
 *
 * Returns: The device protocol as a string.
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_protocol	(unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_protocol);
}

/* Why devices the last discovery of @interface found could not be opened, one per line, or NULL. Only the USB3Vision
 * interface keeps track of them: a camera without a usable driver is found, but libusb cannot open it. */

static char *
_dup_interface_open_errors_locked (ArvInterface *interface)
{
#if ARAVIS_HAS_USB
	if (ARV_IS_UV_INTERFACE (interface))
		return arv_uv_interface_dup_open_errors (ARV_UV_INTERFACE (interface));
#endif

	return NULL;
}

/* The same, for the enabled interfaces */

static char *
_dup_device_open_errors_locked (void)
{
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
		if (interfaces[i].is_available && g_strcmp0 (interfaces[i].interface_id, "USB3Vision") == 0)
			return _dup_interface_open_errors_locked (interfaces[i].get_interface_instance ());

	return NULL;
}

/* The error of the device opening functions when they find no device. The interfaces report no error for a device they
 * do not know, so that the next ones are searched too: @open_errors, the devices the last discovery found but could
 * not open, go in this error instead, one per line. @interface_id is NULL when every enabled interface was searched. */

static void
_set_device_not_found_error (GError **error, const char *device_id, const char *interface_id,
			     const char *open_errors)
{
	GString *message = g_string_new (NULL);

	if (device_id != NULL)
		g_string_append_printf (message, "Device '%s' not found", device_id);
	else if (interface_id != NULL)
		g_string_append (message, "No device found");
	else
		g_string_append (message, "No supported device found");

	if (interface_id != NULL)
		g_string_append_printf (message, " on interface '%s'", interface_id);

	if (open_errors != NULL)
		g_string_append_printf (message, "\n%s", open_errors);

	g_set_error_literal (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND, message->str);

	g_string_free (message, TRUE);
}

/* Private, exported for arv-tool, which reports a failed device discovery without calling arv_open_device () */

char *
arv_dup_device_open_errors (void)
{
	char *open_errors;

	g_mutex_lock (&arv_system_mutex);

	open_errors = _dup_device_open_errors_locked ();

	g_mutex_unlock (&arv_system_mutex);

	return open_errors;
}

/**
 * arv_open_device:
 * @device_id: (allow-none): a device identifier string
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Open a device corresponding to the given identifier. A %NULL string makes
 * this function return the first available device.
 *
 * When no device is found, the error message also lists the devices that were found but could not be opened, with
 * the probable cause, such as a USB3 Vision camera without a usable driver.
 *
 * Return value: (transfer full): A new #ArvDevice instance.
 *
 * Since: 0.8.0
 */

ArvDevice *
arv_open_device (const char *device_id, GError **error)
{
	char *open_errors;
	unsigned int i;

	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;
		ArvDevice *device;

		if (interfaces[i].is_available) {
			GError *local_error = NULL;

			interface = interfaces[i].get_interface_instance ();
			device = arv_interface_open_device (interface, device_id, &local_error);
			if (ARV_IS_DEVICE (device) || local_error != NULL) {
				if (local_error != NULL)
					g_propagate_error (error, local_error);
				g_mutex_unlock (&arv_system_mutex);
				return device;
			}
		}
	}

	open_errors = _dup_device_open_errors_locked ();

	g_mutex_unlock (&arv_system_mutex);

	_set_device_not_found_error (error, device_id, NULL, open_errors);

	g_free (open_errors);

	return NULL;
}

/**
 * arv_open_device_on_interface:
 * @interface_id: an interface identifier, as returned by arv_get_interface_id()
 * @device_id: (allow-none): a device identifier string
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Opens a device through a single interface, whether it is enabled or not. Unlike arv_open_device(), which asks every
 * enabled interface in turn, this does not wait for the device discovery of the other interfaces. A %NULL @device_id
 * opens the first device of the interface, after an update of its device list, as arv_update_device_list() does for
 * the enabled interfaces.
 *
 * Like arv_open_device() and arv_update_device_list(), and unlike the functions of the #ArvInterface instance
 * returned by arv_get_interface_by_id(), this function can be called while other threads use the device list.
 *
 * When no device is found, the error message also lists the devices of the interface that were found but could not
 * be opened, with the probable cause, as arv_open_device() does: for example a USB3 Vision camera without a usable
 * driver, when @interface_id is 'USB3Vision'.
 *
 * Return value: (transfer full): A new #ArvDevice instance, %NULL on error.
 *
 * Since: 0.10.0
 */

ArvDevice *
arv_open_device_on_interface (const char *interface_id, const char *device_id, GError **error)
{
	ArvInterface *iface;
	ArvDevice *device = NULL;
	GError *local_error = NULL;
	char *open_errors = NULL;

	g_return_val_if_fail (interface_id != NULL, NULL);

	iface = arv_get_interface_by_id (interface_id);
	if (iface == NULL) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_INVALID_PARAMETER,
			     "Unknown interface '%s'", interface_id);
		return NULL;
	}

	g_mutex_lock (&arv_system_mutex);

	if (device_id == NULL) {
		/* Not every interface opens a device for a NULL identifier, and those which do only consider the
		 * devices they already know */
		arv_interface_update_device_list (iface);
		if (arv_interface_get_n_devices (iface) > 0) {
			char *first_device_id = g_strdup (arv_interface_get_device_id (iface, 0));

			device = arv_interface_open_device (iface, first_device_id, &local_error);
			g_free (first_device_id);
		}
	} else
		device = arv_interface_open_device (iface, device_id, &local_error);

	/* From the discovery this call just ran, read from the interface itself, which may be disabled. An error of
	 * the interface already tells why its device could not be opened. */
	if (!ARV_IS_DEVICE (device) && local_error == NULL)
		open_errors = _dup_interface_open_errors_locked (iface);

	g_mutex_unlock (&arv_system_mutex);

	if (ARV_IS_DEVICE (device)) {
		g_clear_error (&local_error);
		return device;
	}

	if (local_error != NULL)
		g_propagate_error (error, local_error);
	else
		_set_device_not_found_error (error, device_id, interface_id, open_errors);

	g_free (open_errors);

	return NULL;
}

/**
 * arv_get_device_interface_id:
 * @index: device index
 *
 * Queries the identifier of the interface listing the device corresponding to index, which
 * arv_open_device_on_interface() accepts. A device found by a GenTL producer is listed by the 'GenTL' interface, whatever
 * its protocol. Prior to this call the arv_update_device_list() function must be called.
 *
 * Returns: (nullable): the interface identifier, %NULL if @index is out of range
 *
 * Since: 0.10.0
 */

const char *
arv_get_device_interface_id (unsigned int index)
{
	const char *interface_id = NULL;
	unsigned int offset = 0;
	unsigned int i;

	/* The same interfaces as arv_get_device_id () for the same index */
	arv_system_init_interfaces ();

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces) && interface_id == NULL; i++) {
		if (interfaces[i].is_available) {
			unsigned int n_devices;

			n_devices = arv_interface_get_n_devices (interfaces[i].get_interface_instance ());
			if (index - offset < n_devices)
				interface_id = interfaces[i].interface_id;

			offset += n_devices;
		}
	}

	g_mutex_unlock (&arv_system_mutex);

	return interface_id;
}

/**
 * arv_shutdown:
 *
 * Frees a number of ressources allocated by Aravis that would be otherwise
 * reported as memory leak by tools like Valgrind. The call to this function is
 * optional if you don't intend to check for memory leaks.
 */

void
arv_shutdown (void)
{
	unsigned int i;

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
		interfaces[i].destroy_interface_instance ();

	/* The next queries find the empty lists of new interface instances */
	arv_system_device_list_changes++;

	arv_dom_implementation_cleanup ();

	g_mutex_unlock (&arv_system_mutex);
}
