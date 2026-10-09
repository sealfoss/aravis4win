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
 * Author: Xiaoqiang Wang <xiaoqiang.wang@psi.ch>
 */

/**
 * SECTION: arvgentlinterface
 * @short_description: GenTL interface
 */
#include <arvgentlprivate.h>
#include <arvgentlsystemprivate.h>
#include <arvgentlinterfaceprivate.h>
#include <arvgentldeviceprivate.h>
#include <arvinterfaceprivate.h>
#include <arvdebugprivate.h>
#include <arvmiscprivate.h>
#include <arvstr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ArvGenTLnterface implementation */

typedef struct {
	ArvGenTLSystem *system;
	char *interface;

	char *guid;
	char *id;
	char *vendor_serial;
	char *vendor_alias_serial;
	char *vendor;
	char *model;
	char *serial_nbr;

	volatile gint ref_count;
} ArvGenTLInterfaceDeviceInfos;

static ArvGenTLInterfaceDeviceInfos *
arv_gentl_interface_device_infos_new (ArvGenTLSystem *system,
                                      const char *tl_vendor,
                                      const char *interface,
                                      const char *guid,
                                      const char *vendor,
                                      const char *model,
                                      const char *serial_nbr)
{
        ArvGenTLInterfaceDeviceInfos *infos;

	g_return_val_if_fail (system != NULL, NULL);
	g_return_val_if_fail (tl_vendor != NULL, NULL);
	g_return_val_if_fail (interface != NULL, NULL);
	g_return_val_if_fail (guid != NULL, NULL);
	g_return_val_if_fail (vendor != NULL, NULL);
	g_return_val_if_fail (model != NULL, NULL);
	g_return_val_if_fail (serial_nbr != NULL, NULL);

	infos = g_new (ArvGenTLInterfaceDeviceInfos, 1);
	infos->system  = g_object_ref (system);
	infos->interface = g_strdup(interface);
        infos->guid = g_strdup (guid);
	infos->id = g_strdup_printf ("%s-%s-%s-%s", tl_vendor, vendor, model, serial_nbr);
	infos->vendor_alias_serial = g_strdup_printf ("%s-%s-%s", tl_vendor, arv_vendor_alias_lookup (vendor), serial_nbr);
	infos->vendor_serial = g_strdup_printf ("%s-%s-%s", tl_vendor, vendor, serial_nbr);
	infos->vendor = g_strdup (vendor);
	infos->model = g_strdup (model);
	infos->serial_nbr = g_strdup (serial_nbr);
	infos->ref_count = 1;

	arv_str_strip (infos->id, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS, ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);
	arv_str_strip (infos->vendor_serial, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS, ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);
	arv_str_strip (infos->vendor_alias_serial, ARV_DEVICE_NAME_ILLEGAL_CHARACTERS, ARV_DEVICE_NAME_REPLACEMENT_CHARACTER);

	return infos;
}

static ArvGenTLInterfaceDeviceInfos *
arv_gentl_interface_device_infos_ref (ArvGenTLInterfaceDeviceInfos *infos)
{
	g_return_val_if_fail (infos != NULL, NULL);
	g_return_val_if_fail (g_atomic_int_get (&infos->ref_count) > 0, NULL);

	g_atomic_int_inc (&infos->ref_count);

	return infos;
}

static void
arv_gentl_interface_device_infos_unref (ArvGenTLInterfaceDeviceInfos *infos)
{
	g_return_if_fail (infos != NULL);
	g_return_if_fail (g_atomic_int_get (&infos->ref_count) > 0);

	if (g_atomic_int_dec_and_test (&infos->ref_count)) {
		g_clear_object (&infos->system);
		g_clear_pointer (&infos->interface, g_free);
		g_clear_pointer (&infos->guid, g_free);
		g_clear_pointer (&infos->id, g_free);
		g_clear_pointer (&infos->vendor_serial, g_free);
		g_clear_pointer (&infos->vendor_alias_serial, g_free);
		g_clear_pointer (&infos->vendor, g_free);
		g_clear_pointer (&infos->model, g_free);
		g_clear_pointer (&infos->serial_nbr, g_free);
		g_clear_pointer (&infos, g_free);
	}
}

typedef struct {
	GHashTable *devices;
} ArvGenTLInterfacePrivate;

struct _ArvGenTLInterface {
	ArvInterface	interface;

	ArvGenTLInterfacePrivate *priv;
};

struct _ArvGenTLInterfaceClass {
	ArvInterfaceClass parent_class;
};


G_DEFINE_TYPE_WITH_CODE (ArvGenTLInterface, arv_gentl_interface, ARV_TYPE_INTERFACE, G_ADD_PRIVATE (ArvGenTLInterface))

/* The string getters return NULL if the producer can't provide the information. One byte is added to the allocated
 * size, for producers not counting the string terminator. */

static char *
_gentl_get_tl_info_str (ArvGenTLModule *gentl, TL_HANDLE handle, TL_INFO_CMD info_cmd)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = 0;
	char *value = NULL;

	error = gentl->TLGetInfo (handle, info_cmd, &type, NULL, &size);
	if (error == GC_ERR_SUCCESS && size > 0) {
		value = g_malloc0(size + 1);
		error = gentl->TLGetInfo (handle, info_cmd, &type, value, &size);
		if (error != GC_ERR_SUCCESS)
			g_clear_pointer (&value, g_free);
	}

	if (value == NULL)
		arv_info_interface("_gentl_get_tl_info_str: no info %d (%s)", info_cmd,
				   arv_gentl_gc_error_to_string (error));

	return value;
}

static char *
_gentl_get_info_str(GC_ERROR(*func)(void*, const char *, int32_t, int32_t*, void*, size_t*),
                    void *interface, const char *device, BUFFER_INFO_CMD info_cmd)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = 0;
	char *value = NULL;

	error = func(interface, device, info_cmd, &type, NULL, &size);
	if (error == GC_ERR_SUCCESS && size > 0) {
		value = g_malloc0(size + 1);
		error = func(interface, device, info_cmd, &type, value, &size);
		if (error != GC_ERR_SUCCESS)
			g_clear_pointer (&value, g_free);
	}

	if (value == NULL)
		arv_info_interface("_gentl_get_info_str: no info %d for '%s' (%s)", info_cmd,
				   device != NULL ? device : "", arv_gentl_gc_error_to_string (error));

	return value;
}

static char *
_gentl_get_id(GC_ERROR(*func)(void*, uint32_t, char*, size_t*), void *handle, uint32_t index)
{
	GC_ERROR error;
	size_t size = 0;
	char *value = NULL;

	error = func(handle, index, NULL, &size);
	if (error == GC_ERR_SUCCESS && size > 0) {
		value = g_malloc0(size + 1);
		error = func(handle, index, value, &size);
		if (error != GC_ERR_SUCCESS || value[0] == '\0')
			g_clear_pointer (&value, g_free);
	}

	return value;
}

/* Missing or empty strings are replaced by a fallback. */

static char *
_take_string_or_fallback (char *value, const char *fallback)
{
	if (value != NULL && value[0] != '\0')
		return value;

	g_free (value);

	return g_strdup (fallback);
}

/* Name of the producer file, without extension: used as transport layer vendor if the producer doesn't tell it */

static char *
_get_producer_name (ArvGenTLSystem *gentl_system)
{
	char *basename;
	char *extension;

	basename = g_path_get_basename (arv_gentl_system_get_filename (gentl_system));
	extension = strrchr (basename, '.');
	if (extension != NULL && extension != basename)
		*extension = '\0';

	return basename;
}

/* Keep the three fallback components distinct even when they contain the separator, an escape sequence or bytes
 * that arv_str_strip() would remove from the resulting device name. Escape bytes, not Unicode code points, so the
 * producer's opaque identifiers are preserved. The raw identifiers used to open the interface/device are unchanged. */

static char *
_make_fallback_serial (const char *producer, const char *interface_id, const char *device_id)
{
	const char *components[] = {producer, interface_id, device_id};
	GString *serial = g_string_new (NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS (components); i++) {
		const guchar *p = (const guchar *) components[i];

		if (i > 0)
			g_string_append_c (serial, '-');
		for (; *p != '\0'; p++) {
			if (g_ascii_isalnum (*p) || *p == '_' || *p == '.' || *p == '~')
				g_string_append_c (serial, *p);
			else
				g_string_append_printf (serial, "%%%02X", (unsigned int) *p);
		}
	}

	return g_string_free (serial, FALSE);
}

static uint32_t
_gentl_refresh(GC_ERROR(*update)(void*, bool8_t *, uint64_t), GC_ERROR(*get)(void*, uint32_t*), void *handle)
{
	GC_ERROR error;
	uint32_t num = 0;

	error = update(handle, NULL, 100);
	if (error != GC_ERR_SUCCESS) {
		arv_warning_interface("_gentl_refresh: update %d", error);
		return 0;
	}

	error = get(handle, &num);
	if (error != GC_ERR_SUCCESS) {
		arv_warning_interface("_gentl_refresh: get %d", error);
		return 0;
	}

	return num;
}

static void
_discover (ArvGenTLInterface *gentl_interface, GArray *device_ids)
{
	ArvGenTLInterfacePrivate *priv = arv_gentl_interface_get_instance_private(gentl_interface);
	GList *gentl_systems = arv_gentl_dup_systems();
	GList *gentl_systems_iter;

	g_hash_table_remove_all (priv->devices);

	/* Iterate over all discovered GenTL systems */
	for ( gentl_systems_iter = gentl_systems; gentl_systems_iter; gentl_systems_iter = g_list_next(gentl_systems_iter) ) {
		ArvGenTLSystem *gentl_system = gentl_systems_iter->data;
		ArvGenTLModule * gentl = arv_gentl_system_get_gentl(gentl_system);
		TL_HANDLE system_handle;
		uint32_t num_interfaces;
		char *producer_name, *tl_vendor, *tl_type;

		/* No transport layer handle: the producer failed to open it, and is skipped */
		system_handle = arv_gentl_system_open_system_handle(gentl_system);
		if (system_handle == NULL)
			continue;

		num_interfaces = _gentl_refresh(gentl->TLUpdateInterfaceList, gentl->TLGetNumInterfaces, system_handle);

		/* The transport layer vendor is part of the device id. The producer file name is used if the producer
		 * doesn't tell its vendor, which keeps the ids of a device seen by several producers distinct. */
		producer_name = _get_producer_name (gentl_system);
		tl_vendor = _take_string_or_fallback (_gentl_get_tl_info_str(gentl, system_handle, TL_INFO_VENDOR),
						      producer_name);
		tl_type = _take_string_or_fallback (_gentl_get_tl_info_str(gentl, system_handle, TL_INFO_TLTYPE),
						    "Custom");

		/* Iterate over all interfaces of a GenTL system */
		for (uint32_t i=0; i<num_interfaces; i++) {
			char *interface_id, *interface_type;
			uint32_t num_devices = 0;
			IF_HANDLE interface_handle;

			interface_id = _gentl_get_id(gentl->TLGetInterfaceID, system_handle, i);
			if (interface_id == NULL) {
				arv_warning_interface ("[GenTLInterface::discovery] No id for interface %u of '%s'",
						       i, arv_gentl_system_get_filename (gentl_system));
				continue;
			}

			interface_type = _take_string_or_fallback
				(_gentl_get_info_str(gentl->TLGetInterfaceInfo, system_handle, interface_id,
						     INTERFACE_INFO_TLTYPE),
				 tl_type);

			interface_handle = arv_gentl_system_open_interface_handle(gentl_system, interface_id);
			if (interface_handle == NULL) {
				g_clear_pointer(&interface_id, g_free);
				g_clear_pointer(&interface_type, g_free);
				continue;
			}

			num_devices = _gentl_refresh(gentl->IFUpdateDeviceList, gentl->IFGetNumDevices, interface_handle);

			/* Iterate over all devices of a GenTL interface */
			for (uint32_t j=0; j<num_devices; j++) {
				char *device_id, *id, *model, *vendor, *serial_nbr, *id_serial_nbr;
				ArvGenTLInterfaceDeviceInfos *device_infos;
				ArvInterfaceDeviceIds *ids;

				device_id = _gentl_get_id(gentl->IFGetDeviceID, interface_handle, j);
				if (device_id == NULL) {
					arv_warning_interface ("[GenTLInterface::discovery] No id for device %u of "
							       "interface '%s'", j, interface_id);
					continue;
				}

				/* The producer may not provide every string: the device id replaces a missing
				 * DEVICE_INFO_ID. A missing serial number is replaced in the Aravis device ids by the
				 * producer file name, the interface id and the device id: a GenTL device id is only
				 * unique in its interface, two interfaces may each have a "Device0", and a vendor may
				 * ship several producers. */
				id = _take_string_or_fallback (_gentl_get_info_str(gentl->IFGetDeviceInfo,
										  interface_handle, device_id,
										  DEVICE_INFO_ID),
							       device_id);
				model = _take_string_or_fallback (_gentl_get_info_str(gentl->IFGetDeviceInfo,
										     interface_handle, device_id,
										     DEVICE_INFO_MODEL),
								  "");
				vendor = _take_string_or_fallback (_gentl_get_info_str(gentl->IFGetDeviceInfo,
										      interface_handle, device_id,
										      DEVICE_INFO_VENDOR),
								   "");
				serial_nbr = _take_string_or_fallback (_gentl_get_info_str(gentl->IFGetDeviceInfo,
											  interface_handle, device_id,
											  DEVICE_INFO_SERIAL_NUMBER),
								       "");

				if (serial_nbr[0] != '\0')
					id_serial_nbr = g_strdup (serial_nbr);
				else
					id_serial_nbr = _make_fallback_serial (producer_name, interface_id, device_id);

				device_infos =  arv_gentl_interface_device_infos_new(gentl_system, tl_vendor, interface_id, id,
                                                                                    vendor, model, id_serial_nbr);
				g_free (id_serial_nbr);
                                arv_info_interface ("[GentTLInterface::discovery] Device '%s' found",
                                                    device_infos->id);

                                g_hash_table_replace(priv->devices, device_infos->id,
                                                     arv_gentl_interface_device_infos_ref(device_infos));
                                arv_info_interface ("  %s", device_infos->id);
                                g_hash_table_replace (priv->devices, device_infos->vendor_serial,
                                                      arv_gentl_interface_device_infos_ref (device_infos));
                                arv_info_interface ("  %s", device_infos->vendor_serial);
                                g_hash_table_replace (priv->devices, device_infos->vendor_alias_serial,
                                                      arv_gentl_interface_device_infos_ref (device_infos));
                                arv_info_interface ("  %s", device_infos->vendor_alias_serial);
                                g_hash_table_replace (priv->devices, device_infos->guid,
                                                      arv_gentl_interface_device_infos_ref (device_infos));
                                arv_info_interface ("  %s", device_infos->guid);

				if (device_ids) {
					ids = g_new0 (ArvInterfaceDeviceIds, 1);
					ids->device = g_strdup (device_infos->id);
					ids->model = model;
					ids->vendor = vendor;
					ids->serial_nbr = serial_nbr;
					ids->address = g_strdup(interface_type);
                                        ids->protocol = arv_protocol_from_transport_layer_type (interface_type);
					g_array_append_val (device_ids, ids);
                                        g_free (id);
				} else {
					g_free(id);
					g_free(model);
					g_free(vendor);
					g_free(serial_nbr);
				}
				g_clear_pointer(&device_id, g_free);

				arv_gentl_interface_device_infos_unref(device_infos);
			}
			arv_gentl_system_close_interface_handle(gentl_system, interface_id);

			g_clear_pointer(&interface_id, g_free);
			g_clear_pointer(&interface_type, g_free);
		}
		arv_gentl_system_close_system_handle(gentl_system);

		g_free (producer_name);
		g_free (tl_vendor);
		g_free (tl_type);
	}

	g_list_free_full (gentl_systems, g_object_unref);
}

static void
arv_gentl_interface_update_device_list (ArvInterface *interface, GArray *device_ids)
{
	ArvGenTLInterface *gentl_interface = ARV_GENTL_INTERFACE (interface);

	g_assert (device_ids->len == 0);

	_discover(gentl_interface, device_ids);
}

static ArvDevice *
arv_gentl_interface_open_device (ArvInterface *interface, const char *key, GError **error)
{
	ArvGenTLInterfacePrivate *priv = arv_gentl_interface_get_instance_private(ARV_GENTL_INTERFACE (interface));
	ArvDevice *device = NULL;
	ArvGenTLInterfaceDeviceInfos *device_infos = NULL;

        if (key == NULL) {
		GList *device_list;

		device_list = g_hash_table_get_values (priv->devices);
		device_infos = device_list != NULL ? device_list->data : NULL;
		g_list_free (device_list);
	} else
		device_infos = g_hash_table_lookup (priv->devices, key);

	/* Refresh devices if the requested device is in the cache. */
	if (device_infos == NULL) {
		GList *device_list;

		_discover(ARV_GENTL_INTERFACE(interface), NULL);

                if (key == NULL) {
                        device_list = g_hash_table_get_values (priv->devices);
                        device_infos = device_list != NULL ? device_list->data : NULL;
                        g_list_free (device_list);
                } else
                        device_infos = g_hash_table_lookup (priv->devices, key);
        }

	if (device_infos)
		device = arv_gentl_device_new (device_infos->system, device_infos->interface, device_infos->guid, error);

	return device;
}

static ArvInterface *arv_gentl_interface = NULL;
static GMutex arv_gentl_interface_mutex;

/**
 * arv_gentl_interface_get_instance:
 *
 * Gets the unique instance of the GenTL interface.
 *
 * Returns: (transfer none): a #ArvInterface singleton.
 */

ArvInterface *
arv_gentl_interface_get_instance (void)
{
	g_mutex_lock (&arv_gentl_interface_mutex);

	if (arv_gentl_interface == NULL)
		arv_gentl_interface = g_object_new (ARV_TYPE_GENTL_INTERFACE, NULL);

	g_mutex_unlock (&arv_gentl_interface_mutex);

	return ARV_INTERFACE (arv_gentl_interface);
}

void
arv_gentl_interface_destroy_instance (void)
{
	g_mutex_lock (&arv_gentl_interface_mutex);

	g_clear_object (&arv_gentl_interface);

	g_mutex_unlock (&arv_gentl_interface_mutex);
}

static void
arv_gentl_interface_init (ArvGenTLInterface *gentl_interface)
{
	ArvGenTLInterfacePrivate *priv = arv_gentl_interface_get_instance_private (gentl_interface);

        priv->devices = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                               (GDestroyNotify) arv_gentl_interface_device_infos_unref);
}

static void
arv_gentl_interface_finalize (GObject *object)
{
	ArvGenTLInterface *gentl_interface = ARV_GENTL_INTERFACE (object);
	ArvGenTLInterfacePrivate *priv = arv_gentl_interface_get_instance_private (gentl_interface);

	g_hash_table_unref (priv->devices);

	/* Producers still used by a device stay loaded until the destruction of their last device */
	arv_gentl_clear_systems ();

	G_OBJECT_CLASS (arv_gentl_interface_parent_class)->finalize (object);
}

static void
arv_gentl_interface_class_init (ArvGenTLInterfaceClass *gentl_interface_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (gentl_interface_class);
	ArvInterfaceClass *interface_class = ARV_INTERFACE_CLASS (gentl_interface_class);

	object_class->finalize = arv_gentl_interface_finalize;

	interface_class->update_device_list = arv_gentl_interface_update_device_list;
	interface_class->open_device = arv_gentl_interface_open_device;
}
