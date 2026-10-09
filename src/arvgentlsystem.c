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
 * SECTION: arvgentlsystem
 * @short_description: GenTL system module.
 *
 * #ArvGenTLSystem represents a GenTL module.
 */

#include <gmodule.h>
#include <string.h>

#include <arvgentlsystemprivate.h>
#include <arvgentlinterfaceprivate.h>
#include <arvmiscprivate.h>
#include <arvdebugprivate.h>
#include <arvfeatures.h>

#ifdef G_OS_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

enum
{
	PROP_0,
	PROP_GENTL_SYSTEM_FILENAME
};

typedef struct {
	char *id;
	char *name;
	IF_HANDLE handle;
	guint ref_count;
	ArvGenTLSystem *system;
} Interface;

typedef struct {
	char *filename;
	gboolean is_valid;
	/* In arv_gentl_live_systems */
	gboolean is_registered;
	ArvGenTLModule gentl;
	GModule *module;

	/* The transport layer and interface handles, opened on demand and shared by the discovery and the devices. A
	 * device may be destroyed by any thread: the handles, their reference counts and the interface table are
	 * protected by this mutex, held across the producer calls opening or closing them. */
	GMutex mutex;
	TL_HANDLE system_handle;
	guint system_handle_ref_count;
	gboolean is_tl_open_failure_reported;
	GHashTable *interfaces;
} ArvGenTLSystemPrivate;

struct _ArvGenTLSystem {
	GObject object;
};

struct _ArvGenTLSystemClass {
	GObjectClass parent_class;
};


G_DEFINE_TYPE_WITH_CODE (ArvGenTLSystem, arv_gentl_system, G_TYPE_OBJECT, G_ADD_PRIVATE (ArvGenTLSystem))

static char *
_gentl_get_info_str (GC_ERROR(*func)(void*, const char *, int32_t, int32_t*, void*, size_t*), void *handle, const char
                     *id, BUFFER_INFO_CMD info_cmd)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = 0;
	char *value = NULL;

	error = func(handle, id, info_cmd, &type, NULL, &size);
	if (error == GC_ERR_SUCCESS && size > 0) {
		/* One more byte, for producers not counting the string terminator */
		value = g_malloc0(size + 1);
		error = func(handle, id, info_cmd, &type, value, &size);
		if (error != GC_ERR_SUCCESS)
			g_clear_pointer (&value, g_free);
	}
	return value;
}

/* The functions below opening or closing handles are called with the mutex of the system held, or by its
 * finalization. */

static TL_HANDLE
_open_system_handle (ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);

	if (priv->system_handle == NULL) {
		TL_HANDLE handle = NULL;
		GC_ERROR error;

		arv_info_interface("TLOpen: %s", priv->filename);
		error = priv->gentl.TLOpen(&handle);
		if (error == GC_ERR_SUCCESS && handle == NULL)
			error = GC_ERR_INVALID_HANDLE;
		if (error != GC_ERR_SUCCESS) {
			/* Reported once by loading of the producer, which is then skipped by each discovery */
			if (!priv->is_tl_open_failure_reported)
				g_warning ("GenTL producer '%s' not usable: TLOpen failed (%s)", priv->filename,
					   arv_gentl_gc_error_to_string (error));
			else
				arv_warning_interface ("TLOpen: %s: %s", priv->filename,
						       arv_gentl_gc_error_to_string (error));
			priv->is_tl_open_failure_reported = TRUE;

			return NULL;
		}

		priv->system_handle = handle;
	}

	priv->system_handle_ref_count++;

	return priv->system_handle;
}

static void
_close_system_handle (ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	GC_ERROR error;

	g_return_if_fail (priv->system_handle_ref_count > 0);

	priv->system_handle_ref_count--;
	if (priv->system_handle_ref_count == 0 && priv->system_handle != NULL) {
		arv_info_interface("TLClose: %s", priv->filename);
		error = priv->gentl.TLClose(priv->system_handle);
		if (error != GC_ERR_SUCCESS)
			arv_warning_interface("TLClose: %d", error);
		priv->system_handle = NULL;
	}
}

static Interface *
_interface_new (ArvGenTLSystem *system, const char *id)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	TL_HANDLE system_handle;
	IF_HANDLE handle = NULL;
	Interface *interface = NULL;
	GC_ERROR error;

	g_return_val_if_fail (system != NULL, NULL);
	g_return_val_if_fail (id != NULL, NULL);

	system_handle = _open_system_handle (system);
	if (system_handle == NULL)
		return NULL;

	arv_info_interface("TLOpenInterface '%s'", id);

	error = priv->gentl.TLOpenInterface(system_handle, id, &handle);
	if (error != GC_ERR_SUCCESS || handle == NULL) {
		arv_warning_interface("TLOpenInterface('%s'): %d", id, error);
		_close_system_handle (system);
		return NULL;
	}

	interface = g_new (Interface, 1);
	interface->id = g_strdup(id);
	interface->name = _gentl_get_info_str(priv->gentl.TLGetInterfaceInfo, system_handle, id,
					      INTERFACE_INFO_DISPLAYNAME);
	interface->handle = handle;
	interface->system = system;
	interface->ref_count = 1;

	return interface;
}

/* Destroy function of the interface table: closes an interface whose last reference is released */

static void
_interface_free (Interface *interface)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (interface->system);
	GC_ERROR error;

	arv_info_interface("IFClose '%s'", interface->id);
	error = priv->gentl.IFClose(interface->handle);
	if (error != GC_ERR_SUCCESS)
		arv_warning_interface("IFClose: %d", error);
	_close_system_handle (interface->system);

	g_free (interface->id);
	g_free (interface->name);
	g_free (interface);
}

static IF_HANDLE
_open_interface_handle (ArvGenTLSystem *system, const char *interface_id)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	Interface *interface;

	interface = g_hash_table_lookup (priv->interfaces, interface_id);
	if (interface == NULL) {
		interface = _interface_new (system, interface_id);
		if (interface == NULL)
			return NULL;
		g_hash_table_replace (priv->interfaces, g_strdup (interface_id), interface);
	} else {
		interface->ref_count++;
	}

	return interface->handle;
}

static void
_close_interface_handle (ArvGenTLSystem *system, const char *interface_id)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	Interface *interface;

	interface = g_hash_table_lookup (priv->interfaces, interface_id);
	if (interface == NULL) {
		arv_warning_interface ("Interface '%s' of '%s' is not open", interface_id, priv->filename);
		return;
	}

	/* The last reference closes the interface, through the destroy function of the table */
	interface->ref_count--;
	if (interface->ref_count == 0)
		g_hash_table_remove (priv->interfaces, interface_id);
}

static GFile *
_read_link(GFile* file)
{
#ifdef G_OS_UNIX
	char *resolved_path = realpath(g_file_peek_path(file), NULL);
	GFile *resolved_file = NULL;
	if (resolved_path) {
		resolved_file = g_file_new_for_path(resolved_path);
		free(resolved_path);
	}
	return resolved_file;
#else
	return g_object_ref(file);
#endif
}

/* ARV_GENTL_PRODUCERS restricts the loaded producers to a list of file names, separated like the paths of
 * GENICAM_GENTL64_PATH. The match is case insensitive, and the wildcards '*' and '?' are allowed. */

static char **
_get_producer_patterns (void)
{
	const char *selection = g_getenv ("ARV_GENTL_PRODUCERS");
	GPtrArray *patterns;
	char **entries;
	guint i;

	if (selection == NULL)
		return NULL;

	patterns = g_ptr_array_new ();
	entries = g_strsplit (selection, G_SEARCHPATH_SEPARATOR_S, -1);
	for (i = 0; entries[i] != NULL; i++) {
		char *entry = g_strstrip (entries[i]);

		if (entry[0] != '\0') {
			char *basename = g_path_get_basename (entry);

			g_ptr_array_add (patterns, g_ascii_strdown (basename, -1));
			g_free (basename);
		}
	}
	g_strfreev (entries);

	if (patterns->len == 0) {
		g_ptr_array_free (patterns, TRUE);
		return NULL;
	}

	arv_info_interface ("ARV_GENTL_PRODUCERS=%s", selection);

	g_ptr_array_add (patterns, NULL);

	return (char **) g_ptr_array_free (patterns, FALSE);
}

static gboolean
_match_producer_name (const char *path, char **patterns)
{
	char *basename;
	char *name;
	gboolean is_matching = FALSE;
	guint i;

	basename = g_path_get_basename (path);
	name = g_ascii_strdown (basename, -1);

	for (i = 0; patterns[i] != NULL && !is_matching; i++)
		is_matching = g_pattern_match_simple (patterns[i], name);

	g_free (name);
	g_free (basename);

	return is_matching;
}

static void
_add_gentl_file (GHashTable *gentl_files, GFile *file, char **patterns)
{
	GFile *resolved_file;

	resolved_file = _read_link (file);
	if (resolved_file == NULL)
		return;

	/* Either the name found in the path, or the name of the actual file */
	if (patterns == NULL ||
	    _match_producer_name (g_file_peek_path (file), patterns) ||
	    _match_producer_name (g_file_peek_path (resolved_file), patterns)) {
		arv_info_interface ("Add file \"%s\"", g_file_peek_path (resolved_file));
		g_hash_table_replace (gentl_files, g_file_get_path (resolved_file), NULL);
	} else {
		arv_info_interface ("Skip file \"%s\" (not selected by ARV_GENTL_PRODUCERS)",
				    g_file_peek_path (resolved_file));
	}

	g_object_unref (resolved_file);
}

static GHashTable *
_get_gentl_files(void)
{
	GHashTable *gentl_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	const gchar *genicam_gentl_path_env = (sizeof(void *) == 8 ? "GENICAM_GENTL64_PATH" : "GENICAM_GENTL32_PATH");
	gchar **paths = NULL;
	char **patterns;
	const gchar *genicam_gentl_path;
	GFileEnumerator *file_enumerator;
	GFileInfo *file_info = NULL;

	genicam_gentl_path = g_getenv(genicam_gentl_path_env);
	if (!genicam_gentl_path) {
		arv_warning_interface("No \"%s\" defined", genicam_gentl_path_env);
		return gentl_files;
	}
	arv_info_interface("%s=%s", genicam_gentl_path_env, genicam_gentl_path);

	patterns = _get_producer_patterns ();

	paths = g_strsplit(genicam_gentl_path, G_SEARCHPATH_SEPARATOR_S, -1);

	for (guint i=0; i<g_strv_length(paths); i++) {
		GFile *path;
		GFileType file_type;

		if (paths[i][0] == '\0')
			continue;

		path = g_file_new_for_path(paths[i]);
		file_type = g_file_query_file_type(path, G_FILE_QUERY_INFO_NONE, NULL);

		switch (file_type) {
			case G_FILE_TYPE_REGULAR:
				/* If a file is given, assume it is a CTI file. */
				_add_gentl_file (gentl_files, path, patterns);
				break;
			case G_FILE_TYPE_DIRECTORY:
			{
				/* Search path for "*.cti" file. */
				file_enumerator = g_file_enumerate_children(path, "standard::*",
					G_FILE_QUERY_INFO_NONE, NULL, NULL);
				while (file_enumerator && (file_info = g_file_enumerator_next_file (file_enumerator,
                                                                                                    NULL, NULL))) {
					/* Only files with ".cti" suffix. */
					if ( g_file_info_get_file_type(file_info) == G_FILE_TYPE_REGULAR &&
					     g_str_has_suffix(g_file_info_get_name(file_info), ".cti")) {
						GFile *file = g_file_enumerator_get_child(file_enumerator, file_info);

						_add_gentl_file (gentl_files, file, patterns);
						g_object_unref(file);
					}
					g_object_unref(file_info);
				}
				if (file_enumerator)
					g_object_unref(file_enumerator);
			}
			break;
			default:
				arv_warning_interface("Skip invalid path \"%s\"", paths[i]);
			break;
			}
		g_object_unref(path);
	}
	g_strfreev(paths);
	g_strfreev(patterns);

	return gentl_files;
}

/* Producers found in the GenTL path, loaded once and kept until arv_gentl_clear_systems(), called on GenTL interface
 * destruction. Each device keeps a reference on its system, which is closed with the last device. */

static GList *arv_gentl_systems = NULL;
static gboolean arv_gentl_systems_loaded = FALSE;
static GMutex arv_gentl_systems_mutex;

/* Every valid system, by module, until it closes its producer, including the systems still used by a device after
 * arv_gentl_clear_systems(). A producer is initialized once: found again, after arv_shutdown() or by another path to
 * the same library, it is used through its live system. The values are weak references. Protected by
 * arv_gentl_systems_mutex, and signaled by arv_gentl_live_systems_cond when a system leaves the table. */

static GHashTable *arv_gentl_live_systems = NULL;
static GCond arv_gentl_live_systems_cond;

static GModule *_open_module (const char *filename, char **error_message);

static void
_weak_ref_free (GWeakRef *weak_ref)
{
	g_weak_ref_clear (weak_ref);
	g_free (weak_ref);
}

static ArvGenTLSystem *
arv_gentl_system_new (const char *gentl_filename)
{
	return g_object_new (ARV_TYPE_GENTL_SYSTEM,
				"filename", gentl_filename,
				NULL);
}

/* Called with arv_gentl_systems_mutex held */

static ArvGenTLSystem *
_dup_live_system (GModule *module)
{
	GWeakRef *weak_ref;

	/* A system being destroyed leaves the table once it has closed its producer */
	while (arv_gentl_live_systems != NULL &&
	       (weak_ref = g_hash_table_lookup (arv_gentl_live_systems, module)) != NULL) {
		ArvGenTLSystem *system = g_weak_ref_get (weak_ref);

		if (system != NULL)
			return system;

		g_cond_wait (&arv_gentl_live_systems_cond, &arv_gentl_systems_mutex);
	}

	return NULL;
}

/* Called with arv_gentl_systems_mutex held. Returns a new reference on the system of a producer file, NULL if the
 * producer can't be used. */

static ArvGenTLSystem *
_dup_system (const char *filename)
{
	ArvGenTLSystem *system;
	GModule *module;
	char *error_message = NULL;

	/* The module identifies the producer, whatever the path it is found by */
	module = _open_module (filename, &error_message);
	if (module == NULL) {
		g_warning ("GenTL producer '%s' not loaded: %s", filename,
			   error_message != NULL ? error_message : "unknown error");
		g_free (error_message);
		return NULL;
	}

	system = _dup_live_system (module);
	if (system != NULL) {
		arv_info_interface ("[GenTLSystem::dup_system] '%s' already loaded as '%s'", filename,
				    arv_gentl_system_get_filename (system));
	} else {
		ArvGenTLSystemPrivate *priv;

		system = arv_gentl_system_new (filename);
		priv = arv_gentl_system_get_instance_private (system);

		if (priv->is_valid) {
			GWeakRef *weak_ref = g_new0 (GWeakRef, 1);

			g_weak_ref_init (weak_ref, system);
			if (arv_gentl_live_systems == NULL)
				arv_gentl_live_systems = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
										(GDestroyNotify) _weak_ref_free);
			g_hash_table_replace (arv_gentl_live_systems, priv->module, weak_ref);
			priv->is_registered = TRUE;
		} else {
			/* Not registered: its finalization doesn't take arv_gentl_systems_mutex */
			g_clear_object (&system);
		}
	}

	g_module_close (module);

	return system;
}

/**
 * arv_gentl_dup_systems: (skip)
 *
 * Loads the GenTL producers found in the GenTL path, on first call after arv_gentl_clear_systems().
 *
 * Returns: (transfer full): the list of valid systems, to be freed with g_list_free_full (list, g_object_unref).
 */

GList *
arv_gentl_dup_systems(void)
{
	GList *systems;
	GList *unused_systems = NULL;

	g_mutex_lock (&arv_gentl_systems_mutex);

	if (!arv_gentl_systems_loaded) {
		GHashTable *gentl_files;
		GHashTableIter iter;
		GList *loaded_systems = NULL;
		gpointer key;

		/* Built apart: the mutex is released while a system being destroyed closes its producer */
		gentl_files = _get_gentl_files();
		g_hash_table_iter_init(&iter, gentl_files);
		while (g_hash_table_iter_next(&iter, &key, NULL)) {
			ArvGenTLSystem *system = _dup_system ((const char *) key);

			if (system == NULL)
				continue;

			/* Found twice, by two paths to the same library. The list keeps a reference: this one is not
			 * the last. */
			if (g_list_find (loaded_systems, system) != NULL) {
				arv_info_interface ("[GenTLSystem::dup_systems] Skip '%s', already loaded as '%s'",
						    (const char *) key, arv_gentl_system_get_filename (system));
				g_object_unref (system);
				continue;
			}

			loaded_systems = g_list_prepend (loaded_systems, system);
		}
		g_hash_table_unref(gentl_files);

		/* Unless loaded meanwhile by another thread. The list is then released without the mutex, which the
		 * finalization of a system takes. */
		if (arv_gentl_systems_loaded) {
			unused_systems = loaded_systems;
		} else {
			arv_gentl_systems = loaded_systems;
			arv_gentl_systems_loaded = TRUE;
		}
	}

	systems = g_list_copy_deep (arv_gentl_systems, (GCopyFunc) g_object_ref, NULL);

	g_mutex_unlock (&arv_gentl_systems_mutex);

	g_list_free_full (unused_systems, g_object_unref);

	return systems;
}

/**
 * arv_gentl_clear_systems: (skip)
 *
 * Releases the producer list. A producer still in use by a device is closed when the last device is destroyed.
 */

void
arv_gentl_clear_systems (void)
{
	GList *systems;

	g_mutex_lock (&arv_gentl_systems_mutex);

	systems = arv_gentl_systems;
	arv_gentl_systems = NULL;
	arv_gentl_systems_loaded = FALSE;

	g_mutex_unlock (&arv_gentl_systems_mutex);

	g_list_free_full (systems, g_object_unref);
}

ArvGenTLModule *
arv_gentl_system_get_gentl(ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	return &priv->gentl;
}

const char *
arv_gentl_system_get_filename (ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);

	return priv->filename;
}

/* Each successful open of a handle must be balanced by a close. The transport layer handle is NULL if the producer
 * fails to open it: the system is then unusable. */

TL_HANDLE
arv_gentl_system_open_system_handle(ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	TL_HANDLE system_handle;

	g_mutex_lock (&priv->mutex);
	system_handle = _open_system_handle (system);
	g_mutex_unlock (&priv->mutex);

	return system_handle;
}

void
arv_gentl_system_close_system_handle (ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);

	g_mutex_lock (&priv->mutex);
	_close_system_handle (system);
	g_mutex_unlock (&priv->mutex);
}

IF_HANDLE
arv_gentl_system_open_interface_handle (ArvGenTLSystem *system, const char *interface_id)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	IF_HANDLE interface_handle;

	g_mutex_lock (&priv->mutex);
	interface_handle = _open_interface_handle (system, interface_id);
	g_mutex_unlock (&priv->mutex);

	return interface_handle;
}

void
arv_gentl_system_close_interface_handle(ArvGenTLSystem *system, const char *interface_id)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);

	g_mutex_lock (&priv->mutex);
	_close_interface_handle (system, interface_id);
	g_mutex_unlock (&priv->mutex);
}

DEV_HANDLE
arv_gentl_system_open_device_handle(ArvGenTLSystem *system, const char *interface_id, const char *device_id)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	ArvGenTLModule *gentl = &priv->gentl;
	IF_HANDLE interface_handle = NULL;
	DEV_HANDLE device_handle = NULL;
	GC_ERROR error;

	g_mutex_lock (&priv->mutex);

	/* Get interface handle */
	interface_handle = _open_interface_handle (system, interface_id);
	if (interface_handle != NULL) {
		arv_info_interface("IFOpenDevice: '%s'", device_id);
		error = gentl->IFOpenDevice(interface_handle, device_id, DEVICE_ACCESS_CONTROL, &device_handle);
		if (error != GC_ERR_SUCCESS || device_handle == NULL) {
			arv_warning_interface("IFOpenDevice: error %d", error);
			device_handle = NULL;
			_close_interface_handle (system, interface_id);
		}
	}

	g_mutex_unlock (&priv->mutex);

	return device_handle;
}

void
arv_gentl_system_close_device_handle(ArvGenTLSystem *system, const char *interface_id, DEV_HANDLE device_handle)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	ArvGenTLModule *gentl = &priv->gentl;
	GC_ERROR error;

	g_mutex_lock (&priv->mutex);

	error = gentl->DevClose(device_handle);
	if (error != GC_ERR_SUCCESS)
		arv_warning_interface("DevClose error %d", error);

	_close_interface_handle (system, interface_id);

	g_mutex_unlock (&priv->mutex);
}

static void
arv_gentl_system_init (ArvGenTLSystem *system)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);

	g_mutex_init (&priv->mutex);
	priv->system_handle_ref_count = 0;
	priv->system_handle = NULL;

	priv->filename = NULL;
	priv->module = NULL;
	priv->is_valid = FALSE;
	priv->interfaces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) _interface_free);
	memset(&priv->gentl, 0, sizeof(ArvGenTLModule));
}

/* On Windows, a producer is loaded with its own folder searched first for the libraries it depends on, as vendors
 * usually install a producer along with its dependencies, a folder which is not in the default search path. */

static GModule *
_open_module (const char *filename, char **error_message)
{
	GModule *module;
#ifdef G_OS_WIN32
	char *native_filename;
	wchar_t *wfilename;
	HMODULE handle;
	DWORD last_error;
	DWORD old_mode = 0;
	BOOL is_error_mode_set;

	/* LOAD_WITH_ALTERED_SEARCH_PATH needs an absolute path, with backslashes */
	native_filename = g_canonicalize_filename (filename, NULL);
	g_strdelimit (native_filename, "/", '\\');

	wfilename = g_utf8_to_utf16 (native_filename, -1, NULL, NULL, NULL);
	if (wfilename == NULL) {
		*error_message = g_strdup ("invalid file name");
		g_free (native_filename);
		return NULL;
	}

	/* No error dialog box */
	is_error_mode_set = SetThreadErrorMode (SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
	handle = LoadLibraryExW (wfilename, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	last_error = GetLastError ();
	if (is_error_mode_set)
		SetThreadErrorMode (old_mode, NULL);

	g_free (wfilename);

	if (handle == NULL) {
		char *message = g_win32_error_message (last_error);

		*error_message = g_strdup_printf ("%s (error %lu)", message, (unsigned long) last_error);
		g_free (message);
		g_free (native_filename);

		return NULL;
	}

	/* The library is already loaded, and its dependencies resolved: GModule only takes a new reference */
	module = g_module_open (native_filename, G_MODULE_BIND_LOCAL | G_MODULE_BIND_LAZY);
	if (module == NULL)
		*error_message = g_strdup (g_module_error ());

	FreeLibrary (handle);
	g_free (native_filename);
#else
	module = g_module_open (filename, G_MODULE_BIND_LOCAL | G_MODULE_BIND_LAZY);
	if (module == NULL)
		*error_message = g_strdup (g_module_error ());
#endif

	return module;
}

typedef struct {
	const char *name;
	gsize offset;
	gboolean is_optional;
} ArvGenTLSymbol;

#define ARV_GENTL_SYMBOL(symbol,is_optional) { #symbol, G_STRUCT_OFFSET (ArvGenTLModule, symbol), is_optional }

/* A producer must export the functions the consumer calls. The optional ones are never called, or for the multi-part
 * functions of GenTL 1.5, have a fallback. EventGetInfo and EventGetDataInfo are only used by the experimental
 * event support. */

static const ArvGenTLSymbol arv_gentl_symbols[] = {
	ARV_GENTL_SYMBOL (GCInitLib,			FALSE),
	ARV_GENTL_SYMBOL (GCCloseLib,			FALSE),
	ARV_GENTL_SYMBOL (GCGetInfo,			TRUE),
	ARV_GENTL_SYMBOL (GCGetLastError,		TRUE),
	ARV_GENTL_SYMBOL (GCReadPort,			FALSE),
	ARV_GENTL_SYMBOL (GCWritePort,			FALSE),
	ARV_GENTL_SYMBOL (GCReadPortStacked,		TRUE),
	ARV_GENTL_SYMBOL (GCWritePortStacked,		TRUE),
	ARV_GENTL_SYMBOL (GCGetPortURL,			TRUE),
	ARV_GENTL_SYMBOL (GCGetPortInfo,		TRUE),
	ARV_GENTL_SYMBOL (GCGetNumPortURLs,		TRUE),
	ARV_GENTL_SYMBOL (GCGetPortURLInfo,		FALSE),
	ARV_GENTL_SYMBOL (GCRegisterEvent,		FALSE),
	ARV_GENTL_SYMBOL (GCUnregisterEvent,		FALSE),

	ARV_GENTL_SYMBOL (TLOpen,			FALSE),
	ARV_GENTL_SYMBOL (TLClose,			FALSE),
	ARV_GENTL_SYMBOL (TLGetInfo,			FALSE),
	ARV_GENTL_SYMBOL (TLGetNumInterfaces,		FALSE),
	ARV_GENTL_SYMBOL (TLGetInterfaceID,		FALSE),
	ARV_GENTL_SYMBOL (TLGetInterfaceInfo,		FALSE),
	ARV_GENTL_SYMBOL (TLOpenInterface,		FALSE),
	ARV_GENTL_SYMBOL (TLUpdateInterfaceList,	FALSE),

	ARV_GENTL_SYMBOL (IFClose,			FALSE),
	ARV_GENTL_SYMBOL (IFGetInfo,			TRUE),
	ARV_GENTL_SYMBOL (IFGetNumDevices,		FALSE),
	ARV_GENTL_SYMBOL (IFGetDeviceID,		FALSE),
	ARV_GENTL_SYMBOL (IFUpdateDeviceList,		FALSE),
	ARV_GENTL_SYMBOL (IFGetDeviceInfo,		FALSE),
	ARV_GENTL_SYMBOL (IFOpenDevice,			FALSE),
	ARV_GENTL_SYMBOL (IFGetParentTL,		TRUE),

	ARV_GENTL_SYMBOL (DevGetPort,			FALSE),
	ARV_GENTL_SYMBOL (DevGetNumDataStreams,		FALSE),
	ARV_GENTL_SYMBOL (DevGetDataStreamID,		FALSE),
	ARV_GENTL_SYMBOL (DevOpenDataStream,		FALSE),
	ARV_GENTL_SYMBOL (DevGetInfo,			FALSE),
	ARV_GENTL_SYMBOL (DevClose,			FALSE),
	ARV_GENTL_SYMBOL (DevGetParentIF,		TRUE),

	ARV_GENTL_SYMBOL (DSAnnounceBuffer,		FALSE),
	ARV_GENTL_SYMBOL (DSAllocAndAnnounceBuffer,	FALSE),
	ARV_GENTL_SYMBOL (DSFlushQueue,			FALSE),
	ARV_GENTL_SYMBOL (DSStartAcquisition,		FALSE),
	ARV_GENTL_SYMBOL (DSStopAcquisition,		FALSE),
	ARV_GENTL_SYMBOL (DSGetInfo,			TRUE),
	ARV_GENTL_SYMBOL (DSGetBufferID,		TRUE),
	ARV_GENTL_SYMBOL (DSClose,			FALSE),
	ARV_GENTL_SYMBOL (DSRevokeBuffer,		FALSE),
	ARV_GENTL_SYMBOL (DSQueueBuffer,		FALSE),
	ARV_GENTL_SYMBOL (DSGetBufferInfo,		FALSE),
	ARV_GENTL_SYMBOL (DSGetBufferChunkData,		TRUE),
	ARV_GENTL_SYMBOL (DSGetParentDev,		TRUE),
	ARV_GENTL_SYMBOL (DSGetNumBufferParts,		TRUE),
	ARV_GENTL_SYMBOL (DSGetBufferPartInfo,		TRUE),

	ARV_GENTL_SYMBOL (EventGetData,			FALSE),
	ARV_GENTL_SYMBOL (EventGetDataInfo,		!ARAVIS_HAS_EVENT),
	ARV_GENTL_SYMBOL (EventGetInfo,			!ARAVIS_HAS_EVENT),
	ARV_GENTL_SYMBOL (EventFlush,			TRUE),
	ARV_GENTL_SYMBOL (EventKill,			FALSE),
};

static gboolean
_load_symbols (ArvGenTLSystemPrivate *priv, GModule *module)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (arv_gentl_symbols); i++) {
		const ArvGenTLSymbol *symbol = &arv_gentl_symbols[i];
		gpointer *function = G_STRUCT_MEMBER_P (&priv->gentl, symbol->offset);

		if (!g_module_symbol (module, symbol->name, function) || *function == NULL) {
			*function = NULL;

			if (!symbol->is_optional) {
				g_warning ("GenTL producer '%s' not loaded: missing function %s",
					   priv->filename, symbol->name);
				return FALSE;
			}

			arv_info_interface ("[GenTLSystem::load] %s: no optional function %s",
					    priv->filename, symbol->name);
		}
	}

	return TRUE;
}

static void
arv_gentl_system_constructed (GObject *object)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (ARV_GENTL_SYSTEM (object));
	char *error_message = NULL;
	GC_ERROR error;
	GModule *module;

	G_OBJECT_CLASS (arv_gentl_system_parent_class)->constructed (object);

	arv_info_interface("Open module %s", priv->filename);
	module = _open_module (priv->filename, &error_message);
	if (module == NULL) {
		g_warning ("GenTL producer '%s' not loaded: %s", priv->filename,
			   error_message != NULL ? error_message : "unknown error");
		g_free (error_message);
		return;
	}

	if (_load_symbols (priv, module)) {
		error = priv->gentl.GCInitLib();
		if (error == GC_ERR_SUCCESS) {
			priv->module = module;
			priv->is_valid = TRUE;

			arv_debug_interface("ArvGenTLSystem::constructed[%p]: %s", ARV_GENTL_SYSTEM (object),
					    priv->filename);
			return;
		}

		g_warning ("GenTL producer '%s' not loaded: GCInitLib failed (%s)", priv->filename,
			   arv_gentl_gc_error_to_string (error));
	}

	g_module_close(module);
	memset(&priv->gentl, 0, sizeof(ArvGenTLModule));
}

static void
arv_gentl_system_finalize (GObject *object)
{
	ArvGenTLSystem *system = ARV_GENTL_SYSTEM (object);
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (system);
	GC_ERROR error;

	arv_debug_interface("ArvGenTLSystem::finalize[%p]: %s", ARV_GENTL_SYSTEM (object), priv->filename);

	/* Interfaces still open, closing the system handle with the last one */
	g_hash_table_unref (priv->interfaces);

	if (priv->is_valid) {
		if (priv->system_handle != NULL) {
			arv_warning_interface("TLClose: %s still open on finalization", priv->filename);
			error = priv->gentl.TLClose(priv->system_handle);
			if (error != GC_ERR_SUCCESS)
				arv_warning_interface("TLClose: %d", error);
			priv->system_handle = NULL;
		}

		error = priv->gentl.GCCloseLib();
		if (error)
			arv_warning_interface("GCCloseLib %d\n", error);

		/* The producer can be initialized again, by a new system. Removed while the module is still open, which
		 * keeps the key from being reused by another module meanwhile. */
		if (priv->is_registered) {
			g_mutex_lock (&arv_gentl_systems_mutex);
			g_hash_table_remove (arv_gentl_live_systems, priv->module);
			if (g_hash_table_size (arv_gentl_live_systems) == 0)
				g_clear_pointer (&arv_gentl_live_systems, g_hash_table_unref);
			g_cond_broadcast (&arv_gentl_live_systems_cond);
			g_mutex_unlock (&arv_gentl_systems_mutex);
			priv->is_registered = FALSE;
		}

		priv->is_valid = FALSE;
		if (!g_module_close(priv->module))
			arv_warning_interface("Error in close modules \"%s\"", priv->filename);

	}
	g_free(priv->filename);
	g_mutex_clear (&priv->mutex);

	G_OBJECT_CLASS (arv_gentl_system_parent_class)->finalize (object);
}

static void
arv_gentl_system_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (ARV_GENTL_SYSTEM (object));

	switch (prop_id)
	{
		case PROP_GENTL_SYSTEM_FILENAME:
			priv->filename = g_value_dup_string(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
arv_gentl_system_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	ArvGenTLSystemPrivate *priv = arv_gentl_system_get_instance_private (ARV_GENTL_SYSTEM (object));

	switch (prop_id)
	{
		case PROP_GENTL_SYSTEM_FILENAME:
			g_value_set_string(value, priv->filename);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
arv_gentl_system_class_init (ArvGenTLSystemClass *system_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (system_class);

	object_class->constructed = arv_gentl_system_constructed;
	object_class->finalize = arv_gentl_system_finalize;
	object_class->get_property = arv_gentl_system_get_property;
	object_class->set_property = arv_gentl_system_set_property;

	g_object_class_install_property
		(object_class,
		 PROP_GENTL_SYSTEM_FILENAME,
		 g_param_spec_string ("filename",
				      "GenTL filename",
				      "GenTL filename",
				      NULL,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}
