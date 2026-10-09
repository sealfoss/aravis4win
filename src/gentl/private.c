/* Aravis - Digital camera library
 *
 * Copyright © 2023 Václav Šmilauer
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
 * Authors: Václav Šmilauer <eu@doxos.eu>
 */

#include"private.h"

#include<arvsystem.h>

/* The producer can be unloaded and loaded again by the same process, while GLib keeps the types and the quarks it
 * registered, with the pointers given at their registration. The handle types, which need no class or instance
 * initialization, are registered without functions and with a copy of their name, and found by their name after a
 * reload. G_DEFINE_TYPE would register functions and a name of the unloaded library. */

static GType
_get_handle_type (const char *name, guint class_size, guint instance_size)
{
	GType type = g_type_from_name (name);

	if (type == G_TYPE_INVALID)
		type = g_type_register_static_simple (G_TYPE_OBJECT, name, class_size, NULL, instance_size, NULL, 0);

	return type;
}

GType
arv_gentl_event_get_type (void)
{
	static gsize type = 0;

	if (g_once_init_enter (&type))
		g_once_init_leave (&type, _get_handle_type ("ArvGentlEvent", sizeof (ArvGentlEventClass),
							    sizeof (ArvGentlEvent)));

	return type;
}

GType
arv_transport_layer_get_type (void)
{
	static gsize type = 0;

	if (g_once_init_enter (&type))
		g_once_init_leave (&type, _get_handle_type ("ArvTransportLayer", sizeof (ArvTransportLayerClass),
							    sizeof (ArvTransportLayer)));

	return type;
}

/* global variables */
int gentl_GCInitLib = 0;

/* singleton for the transport layer */
ArvTransportLayer* gentl_transport_layer = NULL;

/* error data must be thread-local as per GenTL spec */
GENTL_THREAD_LOCAL_STORAGE GError* gentl_err = NULL;

/* Map handle (pointer) to ArvGentlHandleEvents containing all events associated to this handle. This is not yet
 * implemented, someone knowledgeable of both Aravis and GenTL could propose a clean way of handling this. */
GHashTable* gentl_events=NULL;

GQuark
gentl_error_quark (void)
{
  /* Not a static string, which would be unloaded with the producer */
  return g_quark_from_string ("gentl-error-quark");
}

GC_ERROR
gentl_take_error (GC_ERROR code, GError *error)
{
	g_clear_error (&gentl_err);
	gentl_err = g_error_new_literal (GENTL_ERROR, code, error != NULL ? error->message : "Unknown error");
	g_clear_error (&error);

	return code;
}

GC_ERROR
gentl_init (void)
{
	if (gentl_GCInitLib != 0)
                return GC_ERR_RESOURCE_IN_USE;

	gentl_GCInitLib = 1;
	gentl_events = g_hash_table_new(g_direct_hash,g_direct_equal);

	return GC_ERR_SUCCESS;
}

GC_ERROR
gentl_fini (void)
{
	GENTL_ENSURE_INIT;
	gentl_GCInitLib = 0;

	g_hash_table_destroy(gentl_events);

	gentl_events = NULL;

	/* The last error of the calling thread, which would be lost with an unloaded producer */
	g_clear_error (&gentl_err);

	return GC_ERR_SUCCESS;
}

gboolean
gentl_is_initialized (void)
{
	return gentl_GCInitLib == 1;
}

/* The interfaces of the transport layer are the Aravis interfaces, the disabled ones included, except GenTL: its
 * device discovery loads the producers of the GenTL path, this one included, which would enumerate itself again. */

static gboolean
_is_exposed_interface (const char *interface_id)
{
	return interface_id != NULL && g_strcmp0 (interface_id, "GenTL") != 0;
}

guint
gentl_get_n_interfaces (void)
{
	guint n_interfaces = 0;
	guint i;

	for (i = 0; i < arv_get_n_interfaces (); i++)
		if (_is_exposed_interface (arv_get_interface_id (i)))
			n_interfaces++;

	return n_interfaces;
}

const char *
gentl_get_interface_id (guint index)
{
	guint i;

	for (i = 0; i < arv_get_n_interfaces (); i++) {
		const char *interface_id = arv_get_interface_id (i);

		if (_is_exposed_interface (interface_id)) {
			if (index == 0)
				return interface_id;
			index--;
		}
	}

	return NULL;
}

/* Index of an exposed interface in the Aravis interface list, -1 if there is none with this id */

static int
_get_interface_index (const char *interface_id)
{
	guint i;

	if (!_is_exposed_interface (interface_id))
		return -1;

	for (i = 0; i < arv_get_n_interfaces (); i++)
		if (g_strcmp0 (arv_get_interface_id (i), interface_id) == 0)
			return i;

	return -1;
}

const char *
gentl_get_tl_type (const char *protocol)
{
	if (g_strcmp0 (protocol, "GigEVision") == 0)
		return "GEV";
	if (g_strcmp0 (protocol, "USB3Vision") == 0)
		return "U3V";

	return "Custom";
}

GC_ERROR
gentl_get_interface_info (const char *interface_id, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType,
			  void *pBuffer, size_t *piSize)
{
	int index;

	index = _get_interface_index (interface_id);
	if (index < 0)
		return GC_ERR_INVALID_ID;

	switch (iInfoCmd) {
		case INTERFACE_INFO_ID:
		case INTERFACE_INFO_DISPLAYNAME:
			return gentl_to_buf (INFO_DATATYPE_STRING, pBuffer, arv_get_interface_id (index), piSize, piType);
		case INTERFACE_INFO_TLTYPE:
			return gentl_to_buf (INFO_DATATYPE_STRING, pBuffer,
					     gentl_get_tl_type (arv_get_interface_protocol (index)), piSize, piType);
		default:
			return GC_ERR_INVALID_PARAMETER;
	}
}

/* All events associated to one handle. Each handle can have at most one event of a given type as per GenTL spec. */
struct ArvGentlHandleEvents{
	/* EVENT_MODULE is the last one in EVENT_TYPE_LIST */
	ArvGentlEvent* ev[EVENT_MODULE+1];
};


