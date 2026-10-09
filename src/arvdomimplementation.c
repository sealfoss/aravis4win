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
 * Author:
 * 	Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 */

#include <arvdomimplementation.h>
#include <arvdebugprivate.h>
#include <arvgc.h>
#include <string.h>

/* Documents are created by the parsers of any thread, for example when several threads open cameras. The table is
 * created on demand, with the type of the GenICam descriptions, and freed by arv_dom_implementation_cleanup (), hence a
 * lock rather than a one-time initialization. */

static GMutex document_types_mutex;
static GHashTable *document_types = NULL;

static void
_insert_document_type_locked (const char *qualified_name, GType document_type)
{
	GType *document_type_ptr;

	document_type_ptr = g_new (GType, 1);
	*document_type_ptr = document_type;

	g_hash_table_insert (document_types, g_strdup (qualified_name), document_type_ptr);
}

static void
_ensure_document_types_locked (void)
{
	if (document_types != NULL)
		return;

	document_types = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	_insert_document_type_locked ("RegisterDescription", ARV_TYPE_GC);
}

void
arv_dom_implementation_add_document_type (const char *qualified_name,
					  GType document_type)
{
	g_return_if_fail (qualified_name != NULL);

	g_mutex_lock (&document_types_mutex);

	_ensure_document_types_locked ();
	_insert_document_type_locked (qualified_name, document_type);

	g_mutex_unlock (&document_types_mutex);
}

/**
 * arv_dom_implementation_create_document:
 * @namespace_uri: namespace URI
 * @qualified_name: qualified name
 *
 * Create a new DOM document. Currently, only @qualified_name is used.
 *
 * Returns: (transfer full): a new #ArvDomDocument, NULL on error:
 */

ArvDomDocument *
arv_dom_implementation_create_document (const char *namespace_uri,
					const char *qualified_name)
{
	GType *document_type_ptr;
	GType document_type = G_TYPE_INVALID;

	g_return_val_if_fail (qualified_name != NULL, NULL);

	g_mutex_lock (&document_types_mutex);

	_ensure_document_types_locked ();
	document_type_ptr = g_hash_table_lookup (document_types, qualified_name);
	if (document_type_ptr != NULL)
		document_type = *document_type_ptr;

	g_mutex_unlock (&document_types_mutex);

	if (document_type == G_TYPE_INVALID) {
		arv_info_dom ("[ArvDomImplementation::create_document] Unknown document type (%s)",
			       qualified_name);
		return NULL;
	}

	return g_object_new (document_type, NULL);
}

void
arv_dom_implementation_cleanup (void)
{
	g_mutex_lock (&document_types_mutex);

	g_clear_pointer (&document_types, g_hash_table_unref);

	g_mutex_unlock (&document_types_mutex);
}
