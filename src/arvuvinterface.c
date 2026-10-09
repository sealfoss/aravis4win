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

/**
 * SECTION: arvuvinterface
 * @short_description: USB3Vision interface
 */

#include <arvuvinterfaceprivate.h>
#include <arvinterfaceprivate.h>
#include <arvuvdeviceprivate.h>
#include <arvdebugprivate.h>
#include <arvmiscprivate.h>
#include <arvstr.h>
#include <libusb.h>
#include <stdio.h>
#include <string.h>

/* ArvUvInterface implementation */

typedef struct {
	char *guid;
	char *id;
	char *vendor_serial;
	char *vendor_alias_serial;
	char *manufacturer;
	char *product;
	char *serial_nbr;

	volatile gint ref_count;
} ArvUvInterfaceDeviceInfos;

static ArvUvInterfaceDeviceInfos *
arv_uv_interface_device_infos_new (const char *manufacturer,
				   const char *product,
				   const char *serial_nbr,
				   const char *guid)
{
	ArvUvInterfaceDeviceInfos *infos;

	g_return_val_if_fail (manufacturer != NULL, NULL);
	g_return_val_if_fail (product != NULL, NULL);
	g_return_val_if_fail (serial_nbr != NULL, NULL);
	g_return_val_if_fail (guid != NULL, NULL);

	infos = g_new (ArvUvInterfaceDeviceInfos, 1);
	infos->id = g_strdup_printf ("%s-%s-%s", manufacturer, product, serial_nbr);
	infos->manufacturer = g_strdup (manufacturer);
	infos->vendor_alias_serial = g_strdup_printf ("%s-%s", arv_vendor_alias_lookup (manufacturer), serial_nbr);
	infos->vendor_serial = g_strdup_printf ("%s-%s", manufacturer, serial_nbr);
	infos->product = g_strdup (product);
	infos->serial_nbr = g_strdup (serial_nbr);
	infos->guid = g_strdup (guid);
	infos->ref_count = 1;

	arv_str_strip (infos->id, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS, ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);
	arv_str_strip (infos->vendor_serial, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS, ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);
	arv_str_strip (infos->vendor_alias_serial, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS, ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);

	return infos;
}

static ArvUvInterfaceDeviceInfos *
arv_uv_interface_device_infos_ref (ArvUvInterfaceDeviceInfos *infos)
{
	g_return_val_if_fail (infos != NULL, NULL);
	g_return_val_if_fail (g_atomic_int_get (&infos->ref_count) > 0, NULL);

	g_atomic_int_inc (&infos->ref_count);

	return infos;
}

static void
arv_uv_interface_device_infos_unref (ArvUvInterfaceDeviceInfos *infos)
{
	g_return_if_fail (infos != NULL);
	g_return_if_fail (g_atomic_int_get (&infos->ref_count) > 0);

	if (g_atomic_int_dec_and_test (&infos->ref_count)) {
		g_clear_pointer (&infos->id, g_free);
		g_clear_pointer (&infos->vendor_serial, g_free);
		g_clear_pointer (&infos->vendor_alias_serial, g_free);
		g_clear_pointer (&infos->manufacturer, g_free);
		g_clear_pointer (&infos->product, g_free);
		g_clear_pointer (&infos->serial_nbr, g_free);
		g_clear_pointer (&infos->guid, g_free);
		g_clear_pointer (&infos, g_free);
	}
}

/* A USB3 Vision device that the last discovery could not open */

typedef struct {
	libusb_device *device;		/* Referenced */
	int error;
	char *description;		/* With the device strings, built by the first error message that needs it */
} ArvUvInterfaceOpenError;

static void
arv_uv_interface_open_error_free (ArvUvInterfaceOpenError *open_error)
{
	libusb_unref_device (open_error->device);
	g_free (open_error->description);
	g_free (open_error);
}

typedef struct {
	GHashTable *devices;
	GPtrArray *open_errors;		/* ArvUvInterfaceOpenError */
	libusb_context *usb;
} ArvUvInterfacePrivate;

struct _ArvUvInterface {
	ArvInterface	interface;

	ArvUvInterfacePrivate *priv;
};

struct _ArvUvInterfaceClass {
	ArvInterfaceClass parent_class;
};

#if 0
static void
printdev (libusb_device *device)
{
	struct libusb_device_descriptor desc;
	struct libusb_config_descriptor *config;
	const struct libusb_interface *inter;
	const struct libusb_interface_descriptor *interdesc;
	int r, i, j;

	r = libusb_get_device_descriptor (device, &desc);

	if (r < 0) {
		printf ("Failed to get device descriptor\n");
		return;
	}

	printf ("VendorID:          0x%04x\n", desc.idVendor);
	printf ("ProductID:         0x%04x\n", desc.idProduct);
	printf ("Device Class:      0x%02x\n", (int) desc.bDeviceClass);
	printf ("Device SubClass:   0x%02x\n", (int) desc.bDeviceSubClass);
	printf ("Protocol:          0x%02x\n", (int) desc.bDeviceProtocol);

	libusb_get_config_descriptor (device, 0, &config);

	printf ("Nbr of Interfaces: %d\n", (int)config->bNumInterfaces);
	for (i = 0; i< (int) config->bNumInterfaces; i++) {
		inter = &config->interface[i];
		for (j = 0; j < inter->num_altsetting; j++) {
			interdesc = &inter->altsetting[j];
			printf ("  Interface Class:    0x%02x\n", (int) interdesc->bInterfaceClass);
			printf ("  Interface SubClass: 0x%02x\n", (int) interdesc->bInterfaceSubClass);
			printf ("  Interface Protocol: 0x%02x\n", (int) interdesc->bInterfaceProtocol);
		}
	}
	libusb_free_config_descriptor (config);
}
#endif

static void
_read_string_index (const struct libusb_interface_descriptor *interdesc, int offset, int *index)
{
	if (interdesc->extra != NULL && interdesc->extra_length > offset)
		*index = (int) interdesc->extra[offset];
}

/* The USB3 Vision descriptor test, shared by discovery and by arv_uv_device_new*(), which opens no other device:
 * device class EF/02/01, a control interface EF/05/00 and a data interface EF/05/02. On success, fills
 * @device_descriptor and, if not NULL, @string_indices with the string indices of the device info block that follows
 * the control interface descriptor. */

gboolean
arv_uv_interface_match_descriptors (libusb_device *device, struct libusb_device_descriptor *device_descriptor,
				    ArvUvInterfaceStringIndices *string_indices)
{
	ArvUvInterfaceStringIndices indices = { -1, -1, -1, -1 };
	struct libusb_config_descriptor *config;
	gboolean control_protocol_found = FALSE;
	gboolean data_protocol_found = FALSE;
	int result, i, j;

	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (device_descriptor != NULL, FALSE);

	result = libusb_get_device_descriptor (device, device_descriptor);
	if (result < 0) {
		arv_warning_interface ("Failed to get device descriptor: %s",
				       libusb_error_name (result));
		return FALSE;
	}

	if (device_descriptor->bDeviceClass != ARV_UV_INTERFACE_DEVICE_CLASS ||
	    device_descriptor->bDeviceSubClass != ARV_UV_INTERFACE_DEVICE_SUBCLASS ||
	    device_descriptor->bDeviceProtocol != ARV_UV_INTERFACE_DEVICE_PROTOCOL)
		return FALSE;

	result = libusb_get_config_descriptor (device, 0, &config);
	if (result < 0) {
		arv_warning_interface ("Failed to get configuration descriptor of USB device %04x:%04x: %s",
				       device_descriptor->idVendor, device_descriptor->idProduct,
				       libusb_error_name (result));
		return FALSE;
	}

	for (i = 0; i < (int) config->bNumInterfaces; i++) {
		const struct libusb_interface *inter = &config->interface[i];

		for (j = 0; j < inter->num_altsetting; j++) {
			const struct libusb_interface_descriptor *interdesc = &inter->altsetting[j];

			if (interdesc->bInterfaceClass != ARV_UV_INTERFACE_INTERFACE_CLASS ||
			    interdesc->bInterfaceSubClass != ARV_UV_INTERFACE_INTERFACE_SUBCLASS)
				continue;

			if (interdesc->bInterfaceProtocol == ARV_UV_INTERFACE_CONTROL_PROTOCOL) {
				control_protocol_found = TRUE;
				_read_string_index (interdesc, ARV_UV_INTERFACE_GUID_INDEX_OFFSET, &indices.guid);
				_read_string_index (interdesc, ARV_UV_INTERFACE_VENDOR_NAME_INDEX_OFFSET,
						    &indices.vendor_name);
				_read_string_index (interdesc, ARV_UV_INTERFACE_MODEL_NAME_INDEX_OFFSET,
						    &indices.model_name);
				_read_string_index (interdesc, ARV_UV_INTERFACE_SERIAL_NUMBER_INDEX_OFFSET,
						    &indices.serial_number);
			}
			if (interdesc->bInterfaceProtocol == ARV_UV_INTERFACE_DATA_PROTOCOL)
				data_protocol_found = TRUE;
		}
	}
	libusb_free_config_descriptor (config);

	if (!control_protocol_found || !data_protocol_found)
		return FALSE;

	if (string_indices != NULL)
		*string_indices = indices;

	return TRUE;
}

/* The usual cause of a libusb_open () error, and what to do about it (libusb 1.0.30 sources: os/windows_winusb.c,
 * os/linux_usbfs.c). One error code has several causes: this is a hint, not a diagnosis. */

const char *
arv_uv_interface_get_open_error_hint (int libusb_error)
{
	switch (libusb_error) {
#if defined (G_OS_WIN32)
		case LIBUSB_ERROR_NOT_FOUND:
			/* Composite device, and no interface of it set up with a WinUSB-type driver. libusb finds the
			 * driver of a function only through the device interface GUIDs of its DeviceInterfaceGUIDs or
			 * DeviceInterfaceGUID registry value, which a WinUSB binding made with Device Manager lacks, and
			 * it skips a driver whose installation state is not "installed". */
			return "libusb finds no usable WinUSB driver on its USB3 Vision function: no driver, the vendor's "
				"driver, WinUSB without a DeviceInterfaceGUIDs registry value (as Device Manager binds it), "
				"or an unfinished driver installation; bind WinUSB to the function, see Windows in the USB "
				"documentation of Aravis";
		case LIBUSB_ERROR_NOT_SUPPORTED:
			/* The driver of the device node is not one libusb supports */
			return "a driver that libusb does not support, such as a vendor driver, is bound to the whole "
				"device; bind WinUSB instead, see Windows in the USB documentation of Aravis";
		case LIBUSB_ERROR_ACCESS:
			/* CreateFile () denied, or the WinUSB DLL missing */
			return "it is already open in this or another process, or the WinUSB DLL is missing";
#elif defined (__linux__)
		case LIBUSB_ERROR_ACCESS:
			return "no permission to open its device node: install the udev rules of Aravis (aravis.rules)";
#else
		case LIBUSB_ERROR_ACCESS:
			return "no permission to open it, or another program has it open";
#endif
		case LIBUSB_ERROR_NO_DEVICE:
			return "it was unplugged";
		default:
			return "LIBUSB_DEBUG=4 shows the log of libusb";
	}
}

/* One line on a USB3 Vision device that libusb could not open, for the device not found errors and the log: its
 * vendor and product ids, with @with_device_strings and libusb >= 1.0.30 (at build and run time) its manufacturer,
 * product and serial number strings, which libusb reads without opening it, its location, the libusb error and a hint
 * at its usual cause.
 *
 * On Windows, libusb reads each string with a descriptor request through the parent hub (unless its device object has
 * it already), which fails while the device is suspended: ask for the strings only to build an error message, not to
 * log. */

char *
arv_uv_interface_describe_open_error (libusb_device *device, int libusb_error, gboolean with_device_strings)
{
	struct libusb_device_descriptor desc;
	GString *description;

	g_return_val_if_fail (device != NULL, NULL);

	description = g_string_new ("Failed to open USB3 Vision device");

	if (libusb_get_device_descriptor (device, &desc) == LIBUSB_SUCCESS)
		g_string_append_printf (description, " %04x:%04x", desc.idVendor, desc.idProduct);

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	if (with_device_strings) {
		static const struct {
			enum libusb_device_string_type type;
			const char *prefix;
		} device_strings[] = {
			{ LIBUSB_DEVICE_STRING_MANUFACTURER, "" },
			{ LIBUSB_DEVICE_STRING_PRODUCT, "" },
			{ LIBUSB_DEVICE_STRING_SERIAL_NUMBER, "S/N " }
		};
		char string[LIBUSB_DEVICE_STRING_BYTES_MAX];
		gsize length = description->len;
		guint i;

		for (i = 0; i < G_N_ELEMENTS (device_strings); i++) {
			char *valid_string;

			if (arv_uv_libusb_get_device_string (device, device_strings[i].type, string,
							     (int) sizeof (string)) < 0)
				continue;

			/* The Windows backend converts the UTF-16 of the descriptor, unpaired surrogates included */
			valid_string = g_utf8_make_valid (string, -1);
			arv_str_strip (valid_string, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS,
				       ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);
			if (valid_string[0] != '\0')
				g_string_append_printf (description, " %s%s%s", description->len == length ? "(" : "",
							device_strings[i].prefix, valid_string);
			g_free (valid_string);
		}
		if (description->len > length)
			g_string_append_c (description, ')');
	}
#endif

	g_string_append_printf (description, " on bus %u, address %u: %s (%s)",
				(guint) libusb_get_bus_number (device), (guint) libusb_get_device_address (device),
				libusb_error_name (libusb_error), arv_uv_interface_get_open_error_hint (libusb_error));

	return g_string_free (description, FALSE);
}

/* Where discovery, or an open, last found each USB3 Vision device, by GUID. arv_uv_device_new*() tries the device at
 * that location first: opening a camera then opens no other USB3 Vision device, which may be in use in this process
 * or another one (WinUSB lets one handle at a time open a device). A location is a hint only: the device found there is
 * checked as any other, and when it is not there any more, a camera plugged in again getting another address, or when
 * it is another one, every USB3 Vision device is tried. Process wide, forgotten at arv_shutdown (). */

static GMutex arv_uv_interface_locations_mutex;
static GHashTable *arv_uv_interface_locations = NULL;	/* GUID -> ArvUvInterfaceDeviceLocation */

void
arv_uv_interface_get_device_location (libusb_device *device, ArvUvInterfaceDeviceLocation *location)
{
	int n_port_numbers;

	g_return_if_fail (device != NULL);
	g_return_if_fail (location != NULL);

	memset (location, 0, sizeof (ArvUvInterfaceDeviceLocation));

	location->bus_number = libusb_get_bus_number (device);
	location->device_address = libusb_get_device_address (device);
	n_port_numbers = libusb_get_port_numbers (device, location->port_numbers, ARV_UV_INTERFACE_PORT_NUMBERS_MAX);
	location->n_port_numbers = CLAMP (n_port_numbers, 0, ARV_UV_INTERFACE_PORT_NUMBERS_MAX);
}

static gboolean
_device_locations_equal (const ArvUvInterfaceDeviceLocation *a, const ArvUvInterfaceDeviceLocation *b)
{
	return a->bus_number == b->bus_number &&
		a->device_address == b->device_address &&
		a->n_port_numbers == b->n_port_numbers &&
		memcmp (a->port_numbers, b->port_numbers, (size_t) a->n_port_numbers) == 0;
}

gboolean
arv_uv_interface_is_device_at_location (libusb_device *device, const ArvUvInterfaceDeviceLocation *location)
{
	ArvUvInterfaceDeviceLocation device_location;

	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (location != NULL, FALSE);

	arv_uv_interface_get_device_location (device, &device_location);

	return _device_locations_equal (&device_location, location);
}

void
arv_uv_interface_remember_device_location (const char *guid, libusb_device *device)
{
	ArvUvInterfaceDeviceLocation *location;
	GHashTableIter iter;
	gpointer key, value;

	g_return_if_fail (device != NULL);

	/* Nothing to look a device without a GUID up by */
	if (guid == NULL || guid[0] == '\0')
		return;

	location = g_new (ArvUvInterfaceDeviceLocation, 1);
	arv_uv_interface_get_device_location (device, location);

	g_mutex_lock (&arv_uv_interface_locations_mutex);

	if (arv_uv_interface_locations == NULL)
		arv_uv_interface_locations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	/* One device at a location: another GUID found there before has gone */
	g_hash_table_iter_init (&iter, arv_uv_interface_locations);
	while (g_hash_table_iter_next (&iter, &key, &value))
		if (strcmp (key, guid) != 0 && _device_locations_equal (value, location))
			g_hash_table_iter_remove (&iter);

	g_hash_table_replace (arv_uv_interface_locations, g_strdup (guid), location);

	g_mutex_unlock (&arv_uv_interface_locations_mutex);
}

/* Copies the location where the device with @guid was found last to @location, and returns TRUE, if it is known */

gboolean
arv_uv_interface_lookup_device_location (const char *guid, ArvUvInterfaceDeviceLocation *location)
{
	ArvUvInterfaceDeviceLocation *known = NULL;

	g_return_val_if_fail (location != NULL, FALSE);

	if (guid == NULL || guid[0] == '\0')
		return FALSE;

	g_mutex_lock (&arv_uv_interface_locations_mutex);
	if (arv_uv_interface_locations != NULL)
		known = g_hash_table_lookup (arv_uv_interface_locations, guid);
	if (known != NULL)
		*location = *known;
	g_mutex_unlock (&arv_uv_interface_locations_mutex);

	return known != NULL;
}

void
arv_uv_interface_forget_device_locations (void)
{
	g_mutex_lock (&arv_uv_interface_locations_mutex);
	g_clear_pointer (&arv_uv_interface_locations, g_hash_table_unref);
	g_mutex_unlock (&arv_uv_interface_locations_mutex);
}

static ArvInterfaceDeviceIds *
_usb_device_to_device_ids (ArvUvInterface *uv_interface, libusb_device *device)
{
	ArvInterfaceDeviceIds *device_ids = NULL;
	libusb_device_handle *device_handle;
	struct libusb_device_descriptor desc;
	ArvUvInterfaceStringIndices string_indices;
	int result;

	if (!arv_uv_interface_match_descriptors (device, &desc, &string_indices))
		return NULL;

        result = libusb_open (device, &device_handle);
	if (result == LIBUSB_SUCCESS) {
		ArvUvInterfaceDeviceInfos *device_infos;
		unsigned char *manufacturer;
		unsigned char *product;
		unsigned char *serial_nbr;
		unsigned char *guid;
		int index;

		device_ids = g_new0 (ArvInterfaceDeviceIds, 1);

		manufacturer = g_malloc0 (256);
		product = g_malloc0 (256);
		serial_nbr = g_malloc0 (256);
		guid = g_malloc0 (256);

		index = string_indices.guid;
		if (index > 0)
			libusb_get_string_descriptor_ascii (device_handle, index, guid, 256);
		index = string_indices.vendor_name;
		if (index > 0)
			libusb_get_string_descriptor_ascii (device_handle, index, manufacturer, 256);
                else {
                        index = desc.iManufacturer;
			libusb_get_string_descriptor_ascii (device_handle, index, manufacturer, 256);
                }
                index = string_indices.model_name;
                if (index > 0)
			libusb_get_string_descriptor_ascii (device_handle, index, product, 256);
                else {
                        index = desc.iProduct;
                        if (index > 0)
                                libusb_get_string_descriptor_ascii (device_handle, index, product, 256);
                }
                index = string_indices.serial_number;
                if (index > 0)
			libusb_get_string_descriptor_ascii (device_handle, index, serial_nbr, 256);
                else {
                        index = desc.iSerialNumber;
                        if (index > 0)
                                libusb_get_string_descriptor_ascii (device_handle, index, serial_nbr, 256);
                }

		device_infos = arv_uv_interface_device_infos_new ((char *) manufacturer, (char *) product,
                                                                  (char *) serial_nbr, (char *) guid);
                arv_info_interface ("[UvInterface::discovery] Device '%s' found",
                                    device_infos->id);
                g_hash_table_replace (uv_interface->priv->devices, device_infos->id,
                                      arv_uv_interface_device_infos_ref (device_infos));
                arv_info_interface ("  %s", device_infos->id);
                g_hash_table_replace (uv_interface->priv->devices, device_infos->vendor_serial,
                                      arv_uv_interface_device_infos_ref (device_infos));
                arv_info_interface ("  %s", device_infos->vendor_serial);
                g_hash_table_replace (uv_interface->priv->devices, device_infos->vendor_alias_serial,
                                      arv_uv_interface_device_infos_ref (device_infos));
                arv_info_interface ("  %s", device_infos->vendor_alias_serial);
                g_hash_table_replace (uv_interface->priv->devices, device_infos->guid,
                                      arv_uv_interface_device_infos_ref (device_infos));
                arv_info_interface ("  %s", device_infos->guid);
                arv_uv_interface_device_infos_unref (device_infos);

		device_ids->device = g_strdup (device_infos->id);
		device_ids->physical = g_strdup (device_infos->guid);
		device_ids->address = g_strdup ("USB3");	/* FIXME */
		device_ids->vendor = g_strdup (device_infos->manufacturer);
                device_ids->manufacturer_info = g_strdup ("none");
		device_ids->model = g_strdup (device_infos->product);
		device_ids->serial_nbr = g_strdup (device_infos->serial_nbr);
                device_ids->protocol = "USB3Vision";

		g_free (manufacturer);
		g_free (product);
		g_free (serial_nbr);
		g_free (guid);

		libusb_close (device_handle);
	} else {
		ArvUvInterfaceOpenError *open_error;
		char *description;

		description = arv_uv_interface_describe_open_error (device, result, FALSE);
		arv_warning_interface ("[UvInterface::discovery] %s", description);
		g_free (description);

		/* Kept for the device not found errors, as this warning is hidden at the default debug level. The
		 * reference also keeps the device object, and the strings libusb reads into it, through the next
		 * discoveries for as long as the device fails to open. */
		open_error = g_new0 (ArvUvInterfaceOpenError, 1);
		open_error->device = libusb_ref_device (device);
		open_error->error = result;
		g_ptr_array_add (uv_interface->priv->open_errors, open_error);
	}

	return device_ids;
}

static void
_discover (ArvUvInterface *uv_interface,  GArray *device_ids)
{
	libusb_device **devices;
	GHashTable *located_guids;
	unsigned uv_count = 0;
	ssize_t result;
	unsigned i;

        if (uv_interface->priv->usb == NULL)
                return;

	result = libusb_get_device_list(uv_interface->priv->usb, &devices);
	if (result < 0) {
		arv_warning_interface ("Failed to get USB device list: %s",
				       libusb_error_name (result));
		/* Not the open errors of an older discovery as if they were current */
		g_ptr_array_set_size (uv_interface->priv->open_errors, 0);
		return;
	}

	g_hash_table_remove_all (uv_interface->priv->devices);
	/* Only now that the new list holds its own references: libusb has given it the device objects that the open
	 * errors referenced, with the device strings cached in them */
	g_ptr_array_set_size (uv_interface->priv->open_errors, 0);

	located_guids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < result; i++) {
		ArvInterfaceDeviceIds *ids;

		ids = _usb_device_to_device_ids (uv_interface, devices[i]);
		if (ids != NULL) {
		    /* Where to open it. Of several devices with one GUID, the first one, which an open by GUID found
		     * first before. */
		    if (g_hash_table_add (located_guids, g_strdup (ids->physical)))
			    arv_uv_interface_remember_device_location (ids->physical, devices[i]);

		    uv_count++;
		    if (device_ids != NULL)
			    g_array_append_val (device_ids, ids);
		    else {
			    g_free (ids->device);
			    g_free (ids->physical);
			    g_free (ids->address);
			    g_free (ids->vendor);
                            g_free (ids->manufacturer_info);
			    g_free (ids->model);
			    g_free (ids->serial_nbr);
			    g_free (ids);
		    }
		}
	}

	arv_info_interface ("Found %d USB3Vision device%s (among %" G_GSSIZE_FORMAT " USB device%s), "
			    "%u could not be opened",
			     uv_count , uv_count > 1 ? "s" : "",
			     result, result > 1 ? "s" : "",
			     uv_interface->priv->open_errors->len);

	g_hash_table_unref (located_guids);
	libusb_free_device_list (devices, 1);
}

/* The open errors of the USB3 Vision devices that the last discovery found, one per line, or NULL when there was
 * none. Their device strings are read here, for the first message only. Like the device table, the list relies on
 * its callers (arvsystem.c) for locking. */

char *
arv_uv_interface_dup_open_errors (ArvUvInterface *uv_interface)
{
	GPtrArray *open_errors;
	GString *string;
	guint i;

	g_return_val_if_fail (ARV_IS_UV_INTERFACE (uv_interface), NULL);

	open_errors = uv_interface->priv->open_errors;
	if (open_errors->len == 0)
		return NULL;

	string = g_string_new (NULL);
	for (i = 0; i < open_errors->len; i++) {
		ArvUvInterfaceOpenError *open_error = g_ptr_array_index (open_errors, i);

		if (open_error->description == NULL)
			open_error->description = arv_uv_interface_describe_open_error (open_error->device,
											open_error->error, TRUE);
		g_string_append_printf (string, "%s%s", i > 0 ? "\n" : "", open_error->description);
	}

	return g_string_free (string, FALSE);
}

static void
arv_uv_interface_update_device_list (ArvInterface *interface, GArray *device_ids)
{
	ArvUvInterface *uv_interface = ARV_UV_INTERFACE (interface);

	g_assert (device_ids->len == 0);

	_discover (uv_interface, device_ids);
}

static ArvDevice *
_open_device (ArvInterface *interface, const char *key, GError **error)
{
	ArvUvInterface *uv_interface;
	ArvUvInterfaceDeviceInfos *device_infos;

	uv_interface = ARV_UV_INTERFACE (interface);

	if (key == NULL) {
		GList *device_list;

		device_list = g_hash_table_get_values (uv_interface->priv->devices);
		device_infos = device_list != NULL ? device_list->data : NULL;
		g_list_free (device_list);
	} else
		device_infos = g_hash_table_lookup (uv_interface->priv->devices, key);

	if (device_infos == NULL)
		return NULL;

	return arv_uv_device_new_from_guid (device_infos->guid, error);
}

static ArvDevice *
arv_uv_interface_open_device (ArvInterface *interface, const char *key, GError **error)
{
	ArvDevice *device;
	GError *local_error = NULL;

	device = _open_device (interface, key, &local_error);
	if (ARV_IS_DEVICE (device) || local_error != NULL) {
		if (local_error != NULL)
			g_propagate_error (error, local_error);
		return device;
	}

	_discover (ARV_UV_INTERFACE (interface), NULL);

	return _open_device (interface, key, error);
}

static ArvInterface *arv_uv_interface = NULL;
static GMutex arv_uv_interface_mutex;

/**
 * arv_uv_interface_get_instance:
 *
 * Gets the unique instance of the GV interface.
 *
 * Returns: (transfer none): a #ArvInterface singleton.
 */

ArvInterface *
arv_uv_interface_get_instance (void)
{
	g_mutex_lock (&arv_uv_interface_mutex);

	if (arv_uv_interface == NULL)
		arv_uv_interface = g_object_new (ARV_TYPE_UV_INTERFACE, NULL);

	g_mutex_unlock (&arv_uv_interface_mutex);

	return ARV_INTERFACE (arv_uv_interface);
}

void
arv_uv_interface_destroy_instance (void)
{
	g_mutex_lock (&arv_uv_interface_mutex);

	g_clear_object (&arv_uv_interface);

	g_mutex_unlock (&arv_uv_interface_mutex);

	arv_uv_interface_forget_device_locations ();
}

G_DEFINE_TYPE_WITH_CODE (ArvUvInterface, arv_uv_interface, ARV_TYPE_INTERFACE, G_ADD_PRIVATE (ArvUvInterface))

static void
arv_uv_interface_init (ArvUvInterface *uv_interface)
{
        int result;

	uv_interface->priv = arv_uv_interface_get_instance_private (uv_interface);

	result = libusb_init (&uv_interface->priv->usb);
        if (result != 0)
		arv_warning_interface ("Failed to initialize USB library: %s",
				       libusb_error_name (result));

	uv_interface->priv->devices = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
							     (GDestroyNotify) arv_uv_interface_device_infos_unref);
	uv_interface->priv->open_errors =
		g_ptr_array_new_with_free_func ((GDestroyNotify) arv_uv_interface_open_error_free);
}

static void
arv_uv_interface_finalize (GObject *object)
{
	ArvUvInterface *uv_interface = ARV_UV_INTERFACE (object);

	g_hash_table_unref (uv_interface->priv->devices);
	g_ptr_array_unref (uv_interface->priv->open_errors);

	G_OBJECT_CLASS (arv_uv_interface_parent_class)->finalize (object);

        if (uv_interface->priv->usb != NULL)
                libusb_exit (uv_interface->priv->usb);
}

static void
arv_uv_interface_class_init (ArvUvInterfaceClass *uv_interface_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (uv_interface_class);
	ArvInterfaceClass *interface_class = ARV_INTERFACE_CLASS (uv_interface_class);

	object_class->finalize = arv_uv_interface_finalize;

	interface_class->update_device_list = arv_uv_interface_update_device_list;
	interface_class->open_device = arv_uv_interface_open_device;
}
