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

#ifndef ARV_REALTIME_PRIVATE_H
#define ARV_REALTIME_PRIVATE_H

#include <arvrealtime.h>
#include <gio/gio.h>

#ifndef G_OS_WIN32

#include <unistd.h> /* for pid_t */

/* private, but used by tests */
ARV_API int		arv_rtkit_get_max_realtime_priority	(GDBusConnection *connection, GError **error);
ARV_API int		arv_rtkit_get_min_nice_level 		(GDBusConnection *connection, GError **error);
ARV_API gint64		arv_rtkit_get_rttime_usec_max 		(GDBusConnection *connection, GError **error);
ARV_API void		arv_rtkit_make_realtime 		(GDBusConnection *connection, pid_t thread, int priority, GError **error);
ARV_API void		arv_rtkit_make_high_priority 		(GDBusConnection *connection, pid_t thread, int nice_level, GError **error);

#else

#include <windows.h>

/* A priority request of arv_make_thread_realtime () or arv_make_thread_high_priority (), and what Windows answered */

typedef struct {
	DWORD requested_class;		/* SetPriorityClass () argument, 0 to keep the process priority class */
	gboolean class_set;		/* SetPriorityClass () result */
	DWORD class_error;		/* GetLastError () after a SetPriorityClass () failure */
	DWORD granted_class;		/* GetPriorityClass () after the request, 0 if it failed */
	int requested_priority;		/* SetThreadPriority () argument */
	gboolean priority_set;		/* SetThreadPriority () result */
	DWORD priority_error;		/* GetLastError () after a SetThreadPriority () failure */
	int granted_priority;		/* GetThreadPriority () after the request */
} ArvWin32PriorityRequest;

/* private, but used by tests */
gboolean		arv_win32_check_priority_request	(const ArvWin32PriorityRequest *request);

#endif
#endif
