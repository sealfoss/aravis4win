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
 * ArvDebug:
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arvdebugprivate.h>
#include <arvenumtypesprivate.h>
#include <arvmiscprivate.h>

#ifdef _MSC_VER
#define STDERR_FILENO _fileno(stderr)
#else
#include <unistd.h>
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#include <locale.h>
#endif

ArvDebugCategoryInfos arv_debug_category_infos[] = {
	{ .name = "interface", 		.description = "Device lookup for each supported protocol" },
	{ .name = "device", 		.description = "Device control" },
	{ .name = "stream", 		.description = "Video stream management" },
	{ .name = "stream-thread", 	.description = "Video stream thread (likely high volume output)" },
	{ .name = "cp", 		.description = "Control protocol packets" },
	{ .name = "sp", 		.description = "Stream protocol packets (likely high volume output)" },
	{ .name = "genicam", 		.description = "Genicam specialized DOM elements" },
	{ .name = "policies", 		.description = "Genicam runtime configurable policies",
					.level = ARV_DEBUG_LEVEL_INFO },
	{ .name = "chunk", 		.description = "Chunk data code" },
	{ .name = "dom", 		.description = "Genicam DOM document" },
	{ .name = "evaluator", 		.description = "Expression evaluator" },
	{ .name = "viewer", 		.description = "Simple viewer application" },
	{ .name = "misc", 		.description = "Miscellaneous code" },
	{ .name = "gentl", 		.description = "GenTL interface" },
};

typedef struct {
	const char *color;
	const char *symbol;
	const char *ascii_symbol;
} ArvDebugLevelInfos;

/* The ASCII symbols replace the glyphs where they would be garbled (see arv_debug_use_glyphs()). They have the same
 * display width, which the indentation of continuation lines in arv_debug_with_level() relies on. */

ArvDebugLevelInfos arv_debug_level_infos[] = {
	{ .color = "",			.symbol = "",		.ascii_symbol = ""},
	{ .color = "\033[1;31m",	.symbol = "🆆 ",	.ascii_symbol = "W "},
	{ .color = "\033[1;32m",	.symbol = "🅸 ",	.ascii_symbol = "I "},
	{ .color = "\033[1;34m",	.symbol = "🅳 ",	.ascii_symbol = "D "},
	{ .color = "\033[0m",		.symbol = "🆃 ",	.ascii_symbol = "T "}
};

static gboolean
arv_debug_initialize (const char *debug_var)
{
	gboolean success = TRUE;
	char **categories;
	int i;

	if (debug_var == NULL)
		return TRUE;

	categories = g_strsplit (debug_var, ",", -1);
	for (i = 0; categories[i] != NULL; i++) {
		char **infos;
		unsigned int j;

		infos = g_strsplit (categories[i], ":", -1);
		if (infos[0] != NULL) {
			gboolean found = FALSE;
			for (j = 0; j < G_N_ELEMENTS (arv_debug_category_infos); j++) {
				if (g_strcmp0 (arv_debug_category_infos[j].name, infos[0]) == 0 ||
				    g_strcmp0 ("all", infos[0]) == 0) {
					if (infos[1] != NULL)
						arv_debug_category_infos[j].level = atoi (infos[1]);
					else
						arv_debug_category_infos[j].level = ARV_DEBUG_LEVEL_INFO;
					found = TRUE;
				}
			}
			if (!found)
				success = FALSE;
		}
		g_strfreev (infos);
	}
	g_strfreev (categories);

	return success;
}

static gboolean
stderr_has_color_support (void)
{
#if GLIB_CHECK_VERSION(2,50,0)
	static int has_color_support = -1;

	if (has_color_support >= 0)
		return has_color_support > 0;

	has_color_support = g_log_writer_supports_color (STDERR_FILENO) ? 1 : 0;

	return has_color_support;
#else
	return FALSE;
#endif
}

/*
 * arv_debug_use_glyphs:
 * @console_output_cp: the console output code page (GetConsoleOutputCP()), 0 when the process has no console
 * @stderr_is_console: whether stderr writes to the console itself, rather than to a file or a pipe
 * @crt_code_page: the code page of the C runtime locale (___lc_codepage_func()), 0 for the "C" locale
 * @console_charset_is_utf8: (nullable): returns whether g_get_console_charset() reports UTF-8; called only when stderr
 * is redirected while the process has a console that does not use UTF-8
 * @console_has_terminal: (nullable): returns whether a terminal emulator, rather than a console window, displays the
 * console; called only when stderr is a console that would receive the glyphs intact
 *
 * Chooses between the level glyphs and the ASCII symbols. Only Windows is concerned: a console decodes the output with
 * its code page, and a legacy one such as 437 or 850 garbles the four-byte UTF-8 glyphs. Elsewhere the glyphs are
 * always used.
 *
 * Outside the "C" locale, the C runtime also converts what it writes to a console from the code page of its locale
 * to the console code page. A program that calls setlocale (LC_ALL, ""), as gst-launch-1.0 and gtk_init() do, would
 * thus garble the glyphs on a UTF-8 console too, unless its locale uses UTF-8.
 *
 * Even when it receives them intact, a console window can't show the glyphs, which lie outside the Basic Multilingual
 * Plane: it holds a replacement character for each. Only a terminal emulator that displays the console, such as
 * Windows Terminal, shows them (see arv_debug_is_terminal()).
 *
 * g_get_console_charset() reads `LANG` first, which MSYS2 sets to a UTF-8 locale whatever the console code page, so it
 * is only followed for a redirected stderr, whose reader may be a UTF-8 terminal such as mintty.
 *
 * Returns: %TRUE for the glyphs, %FALSE for the ASCII symbols
 */

gboolean
arv_debug_use_glyphs (guint console_output_cp, gboolean stderr_is_console, guint crt_code_page,
		      ArvDebugQueryFunc console_charset_is_utf8, ArvDebugQueryFunc console_has_terminal)
{
#ifdef G_OS_WIN32
	if (console_output_cp == 0)
		return TRUE;

	if (stderr_is_console)
		return console_output_cp == CP_UTF8 && (crt_code_page == 0 || crt_code_page == CP_UTF8) &&
			console_has_terminal != NULL && console_has_terminal ();

	if (console_output_cp == CP_UTF8)
		return TRUE;

	return console_charset_is_utf8 != NULL && console_charset_is_utf8 ();
#else
	return TRUE;
#endif
}

/*
 * arv_debug_is_terminal:
 * @console_window_class: (nullable): the class of the console window (GetConsoleWindow()), %NULL without one
 * @wt_session: (nullable): the value of the `WT_SESSION` environment variable
 * @term_program: (nullable): the value of the `TERM_PROGRAM` environment variable
 *
 * Tells whether a terminal emulator, which can show any character, displays the console.
 *
 * A pseudo console (ConPTY), through which Windows Terminal, the terminal of Visual Studio Code and the other terminal
 * emulators of Windows display a console, has a hidden window of the `PseudoConsoleWindow` class. A console window,
 * of the `ConsoleWindowClass` class, can't show the glyphs, nor can a terminal emulator that reads its contents. The
 * environment variables, which a process inherits from the terminal of its parent even in a console window of its
 * own, only count when the console has neither window: no window at all, or one of another class. Windows Terminal
 * sets `WT_SESSION`, and several terminal emulators `TERM_PROGRAM`.
 *
 * Returns: %TRUE when a terminal emulator displays the console
 */

gboolean
arv_debug_is_terminal (const char *console_window_class, const char *wt_session, const char *term_program)
{
	if (g_strcmp0 (console_window_class, "PseudoConsoleWindow") == 0)
		return TRUE;

	if (g_strcmp0 (console_window_class, "ConsoleWindowClass") == 0)
		return FALSE;

	return (wt_session != NULL && wt_session[0] != '\0') || (term_program != NULL && term_program[0] != '\0');
}

#ifdef G_OS_WIN32

static gboolean
stderr_writes_to_console (void)
{
	int fd = _fileno (stderr);
	HANDLE handle;
	DWORD mode;
	DWORD n_written;

	/* _isatty() first, as _get_osfhandle() calls the invalid parameter handler on a closed file descriptor */
	if (fd < 0 || !_isatty (fd))
		return FALSE;

	handle = (HANDLE) _get_osfhandle (fd);

	/* GetConsoleMode() needs read access, which stderr lacks once reopened with freopen ("CONOUT$", "w", stderr), as
	 * GUI programs do after AttachConsole() or AllocConsole(). Writing nothing succeeds on any console output handle,
	 * and fails on the other character devices, such as NUL. */
	return GetConsoleMode (handle, &mode) || WriteConsoleW (handle, L"", 0, &n_written, NULL);
}

static gboolean
query_console_charset_is_utf8 (void)
{
#if GLIB_CHECK_VERSION(2,62,0)
	return g_get_console_charset (NULL);
#else
	/* The ASCII symbols are readable whatever the charset */
	return FALSE;
#endif
}

static gboolean
query_console_has_terminal (void)
{
	HWND window = GetConsoleWindow ();
	char *class_name = NULL;
	gboolean is_terminal;

	if (window != NULL) {
		WCHAR name[64];
		int length = GetClassNameW (window, name, (int) G_N_ELEMENTS (name));

		if (length > 0)
			class_name = g_utf16_to_utf8 ((const gunichar2 *) name, length, NULL, NULL, NULL);
	}

	is_terminal = arv_debug_is_terminal (class_name, g_getenv ("WT_SESSION"), g_getenv ("TERM_PROGRAM"));

	g_free (class_name);

	return is_terminal;
}

#endif

static const char *
arv_debug_get_level_symbol (ArvDebugLevel level)
{
	/* 0 until the choice is made, then 1 for the ASCII symbols, 2 for the glyphs */
	static gsize symbol_set = 0;

	if (g_once_init_enter (&symbol_set)) {
		gboolean use_glyphs;

#ifdef G_OS_WIN32
		/* The locale of the C runtime is read on the first message too, as programs set it when they start. It
		 * is the one of the C runtime this library is linked with, which writes the messages (see
		 * arv_debug_with_level()). */
		use_glyphs = arv_debug_use_glyphs (GetConsoleOutputCP (), stderr_writes_to_console (),
						   ___lc_codepage_func (), query_console_charset_is_utf8,
						   query_console_has_terminal);
#else
		use_glyphs = arv_debug_use_glyphs (0, FALSE, 0, NULL, NULL);
#endif

		g_once_init_leave (&symbol_set, use_glyphs ? 2 : 1);
	}

	return symbol_set == 2 ? arv_debug_level_infos[level].symbol : arv_debug_level_infos[level].ascii_symbol;
}

gboolean
arv_debug_check	(ArvDebugCategory category, ArvDebugLevel level)
{
	if (category < 0 || category >= ARV_DEBUG_CATEGORY_N_ELEMENTS)
		return FALSE;

	if (level <= 0 || level >= ARV_DEBUG_LEVEL_N_ELEMENTS)
		return FALSE;

	if ((int) level <= (int) arv_debug_category_infos[category].level)
		return TRUE;

	return FALSE;
}

static void arv_debug_with_level (ArvDebugCategory category,
				  ArvDebugLevel level,
				  const char *format,
				  va_list args) G_GNUC_PRINTF(3,0);

static void
arv_debug_with_level (ArvDebugCategory category, ArvDebugLevel level, const char *format, va_list args)
{
        GString *message;
        char *text = NULL;
	char *time_str = NULL;
	const char *symbol;
        GDateTime *date = NULL;
        char **lines;
        int header_length;
        gint i;

	if (!arv_debug_check (category, level))
		return;

        date = g_date_time_new_now_local ();
        time_str = g_date_time_format (date, "%H:%M:%S");
	symbol = arv_debug_get_level_symbol (level);

        message = g_string_new (NULL);

	if (stderr_has_color_support ())
                g_string_append_printf (message, "[\033[34m%s.%03d\033[0m] %s%s%s\033[0m> ",
                                        time_str, g_date_time_get_microsecond (date) / 1000,
                                        arv_debug_level_infos[level].color,
                                        symbol,
                                        arv_debug_category_infos[category].name);
        else
                g_string_append_printf (message, "[%s.%03d] %s%s> ",
                                        time_str, g_date_time_get_microsecond (date) / 1000,
                                        symbol,
                                        arv_debug_category_infos[category].name);

        header_length = 19 + strlen (arv_debug_category_infos[category].name);

        text = g_strdup_vprintf (format, args);
        lines = g_strsplit (text, "\n", -1);

        for (i = 0; lines[i] != NULL; i++) {
                if (strlen (lines[i]) >0)
                        g_string_append_printf (message, "%*s%s\n", i > 0 ? header_length : 0, "", lines[i]);
        }

        g_strfreev (lines);

        /* The message is formatted by GLib, and written by the C runtime of this library, the one stderr belongs to,
         * never by GLib's: the two differ when an MSVC debug build of Aravis, which uses the debug C runtime, runs
         * with a release build of GLib, such as the one of the GStreamer MSVC package. The C runtime of GLib would
         * then attach a buffer from its own heap to this stderr, which the debug C runtime frees at exit with its own
         * heap: a "Debug Assertion Failed!" dialog, or heap corruption. One write per message also keeps the lines of
         * concurrent threads apart. */
        fwrite (message->str, 1, message->len, stderr);
        fflush (stderr);

        g_string_free (message, TRUE);
        g_free (text);
        g_free (time_str);
        g_date_time_unref (date);
}

void
arv_warning (ArvDebugCategory category, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	arv_debug_with_level (category, ARV_DEBUG_LEVEL_WARNING, format, args);
	va_end (args);
}

void
arv_info (ArvDebugCategory category, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	arv_debug_with_level (category, ARV_DEBUG_LEVEL_INFO, format, args);
	va_end (args);
}

void
arv_debug (ArvDebugCategory category, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	arv_debug_with_level (category, ARV_DEBUG_LEVEL_DEBUG, format, args);
	va_end (args);
}

void
arv_trace (ArvDebugCategory category, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	arv_debug_with_level (category, ARV_DEBUG_LEVEL_TRACE, format, args);
	va_end (args);
}

/**
 * arv_debug_enable:
 * @category_selection: debug category configuration string
 *
 * Configures the debug output using a configuration string consisting of a comma separated list of debug categories or
 * category/debug level pair.
 *
 * This function overwrites the configuration done by `ARV_DEBUG` environment variable.  For example, enabling debug level
 * 3 of the gvcp category and default debug level of category genicam is done using:
 *
 * ```C
 * arv_debug_enable ("gvcp:3,genicam");
 * ```
 *
 * Returns: %TRUE on success
 * Since: 0.8.8
 */

gboolean
arv_debug_enable (const char *category_selection)
{
	return arv_debug_initialize (category_selection);
}

static char *
arv_debug_dup_infos_as_string (void)
{
	GEnumClass *debug_level_class = g_type_class_ref (ARV_TYPE_DEBUG_LEVEL);
	GString *string = g_string_new ("");
	unsigned int i;

	g_string_append (string, "Debug categories:\n");
	for (i = 0; i < ARV_DEBUG_CATEGORY_N_ELEMENTS; i++) {
		g_string_append_printf (string, "%-15s: %s\n",
					arv_debug_category_infos[i].name,
					arv_debug_category_infos[i].description);
	}
	g_string_append (string, "all            : Everything\n");

	g_string_append (string, "\nDebug levels:\n");
	for (i = 0; i < ARV_DEBUG_LEVEL_N_ELEMENTS; i++) {
		GEnumValue *enum_value;

		enum_value = g_enum_get_value (g_type_class_ref (ARV_TYPE_DEBUG_LEVEL), i);
		if (enum_value != NULL)
			g_string_append_printf (string, "%d: %s\n", i, enum_value->value_nick);
	}

	g_type_class_unref (debug_level_class);

        return arv_g_string_free_and_steal(string);
}

void
arv_debug_print_infos (void)
{
	char *str;

	str = arv_debug_dup_infos_as_string ();
	printf ("%s", str);
	g_free (str);
}

ARV_DEFINE_CONSTRUCTOR (arv_initialize_debug)
static void
arv_initialize_debug (void) {
	arv_debug_initialize (g_getenv ("ARV_DEBUG"));
}

