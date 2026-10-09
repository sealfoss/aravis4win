/* SPDX-License-Identifier:Unlicense */

/* Debug output: the level symbols, glyphs or the ASCII symbols where a Windows console would not show the glyphs, and
 * the C runtime that writes the messages */

/* freopen(), without the deprecation warning of MSVC */
#define _CRT_SECURE_NO_WARNINGS

#include <glib.h>
#include <glib/gstdio.h>
#include <arv.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/arvdebugprivate.h"

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

/* Decision */

static unsigned int n_charset_queries;
static unsigned int n_terminal_queries;
static gboolean charset_is_utf8;
static gboolean has_terminal;

static gboolean
fake_console_charset_is_utf8 (void)
{
	n_charset_queries++;

	return charset_is_utf8;
}

static gboolean
fake_console_has_terminal (void)
{
	n_terminal_queries++;

	return has_terminal;
}

static const struct {
	const char *description;
	guint console_output_cp;
	gboolean stderr_is_console;
	/* Code page of the C runtime locale, 0 for "C" */
	guint crt_code_page;
	gboolean console_charset_is_utf8;
	/* Whether a terminal emulator, rather than a console window, displays the console */
	gboolean console_has_terminal;
	/* Expected on Windows. Elsewhere, the glyphs are always used and nothing is queried. */
	gboolean use_glyphs;
	gboolean charset_queried;
	gboolean terminal_queried;
} symbol_cases[] = {
	{ "No console",					0,	FALSE,	0,	FALSE,	FALSE,	TRUE,	FALSE,	FALSE },
	{ "No console, UTF-8 charset",			0,	FALSE,	0,	TRUE,	FALSE,	TRUE,	FALSE,	FALSE },
	{ "No console, inconsistent console flag",	0,	TRUE,	0,	FALSE,	FALSE,	TRUE,	FALSE,	FALSE },
	{ "No console, CP1252 locale",			0,	FALSE,	1252,	FALSE,	FALSE,	TRUE,	FALSE,	FALSE },
	{ "UTF-8 console window",			65001,	TRUE,	0,	FALSE,	FALSE,	FALSE,	FALSE,	TRUE },
	{ "UTF-8 console in a terminal",		65001,	TRUE,	0,	FALSE,	TRUE,	TRUE,	FALSE,	TRUE },
	{ "UTF-8 console window, UTF-8 charset",	65001,	TRUE,	0,	TRUE,	FALSE,	FALSE,	FALSE,	TRUE },
	{ "UTF-8 console window, UTF-8 locale",		65001,	TRUE,	65001,	FALSE,	FALSE,	FALSE,	FALSE,	TRUE },
	{ "UTF-8 console in a terminal, UTF-8 locale",	65001,	TRUE,	65001,	FALSE,	TRUE,	TRUE,	FALSE,	TRUE },
	{ "UTF-8 console window, CP1252 locale",	65001,	TRUE,	1252,	FALSE,	FALSE,	FALSE,	FALSE,	FALSE },
	{ "UTF-8 console in a terminal, CP1252 locale",	65001,	TRUE,	1252,	FALSE,	TRUE,	FALSE,	FALSE,	FALSE },
	{ "UTF-8 console in a terminal, CP1252 locale, UTF-8 charset",
							65001,	TRUE,	1252,	TRUE,	TRUE,	FALSE,	FALSE,	FALSE },
	{ "UTF-8 console in a terminal, CP932 locale",	65001,	TRUE,	932,	FALSE,	TRUE,	FALSE,	FALSE,	FALSE },
	{ "UTF-8 console, stderr redirected",		65001,	FALSE,	0,	FALSE,	FALSE,	TRUE,	FALSE,	FALSE },
	{ "UTF-8 console, stderr redirected, CP1252 locale",
							65001,	FALSE,	1252,	FALSE,	FALSE,	TRUE,	FALSE,	FALSE },
	{ "CP437 console",				437,	TRUE,	0,	FALSE,	FALSE,	FALSE,	FALSE,	FALSE },
	{ "CP437 console in a terminal",		437,	TRUE,	0,	FALSE,	TRUE,	FALSE,	FALSE,	FALSE },
	{ "CP437 console, UTF-8 LANG as set by MSYS2",	437,	TRUE,	0,	TRUE,	TRUE,	FALSE,	FALSE,	FALSE },
	{ "CP437 console, UTF-8 locale",		437,	TRUE,	65001,	FALSE,	TRUE,	FALSE,	FALSE,	FALSE },
	{ "CP850 console",				850,	TRUE,	0,	FALSE,	FALSE,	FALSE,	FALSE,	FALSE },
	{ "CP1252 console, UTF-8 charset",		1252,	TRUE,	0,	TRUE,	FALSE,	FALSE,	FALSE,	FALSE },
	{ "CP1252 console, CP1252 locale",		1252,	TRUE,	1252,	FALSE,	FALSE,	FALSE,	FALSE,	FALSE },
	{ "CP437 console, stderr redirected, UTF-8 charset",
							437,	FALSE,	0,	TRUE,	FALSE,	TRUE,	TRUE,	FALSE },
	{ "CP437 console, stderr redirected, legacy charset",
							437,	FALSE,	0,	FALSE,	FALSE,	FALSE,	TRUE,	FALSE },
	{ "CP437 console in a terminal, stderr redirected, legacy charset",
							437,	FALSE,	0,	FALSE,	TRUE,	FALSE,	TRUE,	FALSE },
	{ "CP437 console, stderr redirected, UTF-8 charset, CP1252 locale",
							437,	FALSE,	1252,	TRUE,	FALSE,	TRUE,	TRUE,	FALSE },
	{ "CP437 console, stderr redirected, legacy charset, UTF-8 locale",
							437,	FALSE,	65001,	FALSE,	FALSE,	FALSE,	TRUE,	FALSE },
	{ "CP850 console, stderr redirected, UTF-8 charset",
							850,	FALSE,	0,	TRUE,	FALSE,	TRUE,	TRUE,	FALSE },
	{ "CP850 console, stderr redirected, legacy charset",
							850,	FALSE,	0,	FALSE,	FALSE,	FALSE,	TRUE,	FALSE },
};

static void
use_glyphs_test (void)
{
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (symbol_cases); i++) {
		gboolean use_glyphs;

		g_test_message ("%s", symbol_cases[i].description);

		n_charset_queries = 0;
		n_terminal_queries = 0;
		charset_is_utf8 = symbol_cases[i].console_charset_is_utf8;
		has_terminal = symbol_cases[i].console_has_terminal;

		use_glyphs = arv_debug_use_glyphs (symbol_cases[i].console_output_cp,
						   symbol_cases[i].stderr_is_console,
						   symbol_cases[i].crt_code_page,
						   fake_console_charset_is_utf8,
						   fake_console_has_terminal);

#ifdef G_OS_WIN32
		g_assert_cmpint (use_glyphs, ==, symbol_cases[i].use_glyphs);
		g_assert_cmpuint (n_charset_queries, ==, symbol_cases[i].charset_queried ? 1 : 0);
		g_assert_cmpuint (n_terminal_queries, ==, symbol_cases[i].terminal_queried ? 1 : 0);
#else
		g_assert_true (use_glyphs);
		g_assert_cmpuint (n_charset_queries, ==, 0);
		g_assert_cmpuint (n_terminal_queries, ==, 0);
#endif
	}
}

static void
use_glyphs_without_queries_test (void)
{
#ifdef G_OS_WIN32
	/* Without a way to query the charset, a redirected stderr gets the ASCII symbols from a legacy console */
	g_assert_false (arv_debug_use_glyphs (437, FALSE, 0, NULL, NULL));
	g_assert_false (arv_debug_use_glyphs (437, TRUE, 0, NULL, NULL));
	g_assert_true (arv_debug_use_glyphs (65001, FALSE, 0, NULL, NULL));
	/* Without a way to find a terminal, so does a UTF-8 console */
	g_assert_false (arv_debug_use_glyphs (65001, TRUE, 0, NULL, NULL));
	g_assert_true (arv_debug_use_glyphs (0, FALSE, 0, NULL, NULL));
#else
	g_assert_true (arv_debug_use_glyphs (437, FALSE, 0, NULL, NULL));
	g_assert_true (arv_debug_use_glyphs (437, TRUE, 0, NULL, NULL));
	g_assert_true (arv_debug_use_glyphs (65001, TRUE, 0, NULL, NULL));
	g_assert_true (arv_debug_use_glyphs (65001, TRUE, 1252, NULL, NULL));
	g_assert_true (arv_debug_use_glyphs (0, FALSE, 0, NULL, NULL));
#endif
}

#define WT_SESSION_VALUE	"0f6a9c2e-4c1d-4b5e-9a7f-3d2c1b0a9e8d"

static void
is_terminal_test (void)
{
	/* A pseudo console, which a terminal emulator displays, whatever the environment */
	g_assert_true (arv_debug_is_terminal ("PseudoConsoleWindow", NULL, NULL));
	g_assert_true (arv_debug_is_terminal ("PseudoConsoleWindow", "", ""));

	/* A console window, whatever the environment, which the process may have inherited from a terminal */
	g_assert_false (arv_debug_is_terminal ("ConsoleWindowClass", NULL, NULL));
	g_assert_false (arv_debug_is_terminal ("ConsoleWindowClass", WT_SESSION_VALUE, NULL));
	g_assert_false (arv_debug_is_terminal ("ConsoleWindowClass", NULL, "vscode"));

	/* Without either window, that is without any window or with one of another class, the environment tells:
	 * Windows Terminal, or another terminal emulator */
	g_assert_false (arv_debug_is_terminal (NULL, NULL, NULL));
	g_assert_false (arv_debug_is_terminal (NULL, "", ""));
	g_assert_true (arv_debug_is_terminal (NULL, WT_SESSION_VALUE, NULL));
	g_assert_true (arv_debug_is_terminal (NULL, NULL, "vscode"));
	g_assert_false (arv_debug_is_terminal ("SomeOtherWindowClass", NULL, NULL));
	g_assert_false (arv_debug_is_terminal ("SomeOtherWindowClass", "", ""));
	g_assert_true (arv_debug_is_terminal ("SomeOtherWindowClass", WT_SESSION_VALUE, NULL));
	g_assert_true (arv_debug_is_terminal ("SomeOtherWindowClass", NULL, "mintty"));
}

/* Output */

#if !defined (G_OS_WIN32) || GLIB_CHECK_VERSION(2,62,0)

#define INDENTATION		23	/* "[hh:mm:ss.mmm] ", 2 columns of level symbol, "misc> " */

static void
emit_debug_output (void)
{
	arv_warning_misc ("warning, first line\nsecond line");
	arv_info_misc ("info");
	arv_debug_misc ("debug");
	arv_trace (ARV_DEBUG_CATEGORY_MISC, "trace");
}

#endif

#ifdef G_OS_WIN32

#if GLIB_CHECK_VERSION(2,62,0)

/* What emit_debug_output() prints, after the time stamp */
static const struct {
	const char *glyph;
	const char *ascii;
	const char *text;
} expected_lines[] = {
	{ "\360\237\206\206 ",	"W ",	"warning, first line" },
	{ NULL,			NULL,	"second line" },
	{ "\360\237\205\270 ",	"I ",	"info" },
	{ "\360\237\205\263 ",	"D ",	"debug" },
	{ "\360\237\206\203 ",	"T ",	"trace" }
};

#define TIME_STAMP_LENGTH	15	/* "[hh:mm:ss.mmm] " */
#define CATEGORY_TAG		"misc> "

typedef enum {
	EXPECT_ASCII,
	EXPECT_GLYPHS,
	/* The glyphs, as read back from a console, which may hold a replacement character for each */
	EXPECT_CONSOLE_GLYPHS
} Expect;

/* Keeps the test log in printable ASCII: the test runner may print it to a console that cannot display the glyphs, and
 * the output of a pseudo console holds escape sequences */

static void
log_text (const char *title, const char *text)
{
	GString *escaped = g_string_new (NULL);
	const char *c;

	for (c = text; *c != '\0'; c++) {
		if (*c == '\r')
			continue;
		if ((guchar) *c >= 0x80 || ((guchar) *c < 0x20 && *c != '\n' && *c != '\t') || *c == 0x7f)
			g_string_append_printf (escaped, "\\x%02x", (guchar) *c);
		else
			g_string_append_c (escaped, *c);
	}

	g_test_message ("%s:\n%s", title, escaped->str);
	g_string_free (escaped, TRUE);
}

static void
check_debug_output (const char *output, Expect expect)
{
	GPtrArray *lines;
	char **all_lines;
	unsigned int i;

	log_text ("Debug output", output);

	all_lines = g_strsplit (output, "\n", -1);
	lines = g_ptr_array_new ();
	for (i = 0; all_lines[i] != NULL; i++) {
		/* Removes the carriage returns, and the padding of console rows */
		g_strchomp (all_lines[i]);
		if (all_lines[i][0] != '\0')
			g_ptr_array_add (lines, all_lines[i]);
	}

	g_assert_cmpuint (lines->len, ==, G_N_ELEMENTS (expected_lines));

	for (i = 0; i < G_N_ELEMENTS (expected_lines); i++) {
		const char *line = g_ptr_array_index (lines, i);
		const char *symbol;
		const char *tag;
		gsize symbol_length;
		char *expected;

		if (expected_lines[i].glyph == NULL) {
			/* Continuation line, aligned with the text of the first line */
			expected = g_strdup_printf ("%*s%s", INDENTATION, "", expected_lines[i].text);
			g_assert_cmpstr (line, ==, expected);
			g_free (expected);
			continue;
		}

		g_assert_true (g_regex_match_simple ("^\\[[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}\\] ", line, 0, 0));
		symbol = line + TIME_STAMP_LENGTH;
		tag = strstr (symbol, CATEGORY_TAG);
		g_assert_nonnull (tag);
		g_assert_cmpstr (tag + strlen (CATEGORY_TAG), ==, expected_lines[i].text);
		symbol_length = tag - symbol;

		switch (expect) {
			case EXPECT_ASCII:
				g_assert_cmpmem (symbol, symbol_length,
						 expected_lines[i].ascii, strlen (expected_lines[i].ascii));
				/* As wide as a glyph: the text lines up with the continuation lines */
				g_assert_cmpint (TIME_STAMP_LENGTH + symbol_length + strlen (CATEGORY_TAG), ==, INDENTATION);
				break;
			case EXPECT_GLYPHS:
				g_assert_cmpmem (symbol, symbol_length,
						 expected_lines[i].glyph, strlen (expected_lines[i].glyph));
				break;
			case EXPECT_CONSOLE_GLYPHS:
				/* One character and a space, possibly a replacement character, but neither an ASCII
				 * symbol nor the four characters a legacy code page makes of the glyph bytes */
				expected = g_strndup (symbol, symbol_length);
				g_assert_false (g_str_is_ascii (expected));
				g_assert_true (g_str_has_suffix (expected, " "));
				g_assert_cmpint (g_utf8_strlen (expected, -1), ==, 2);
				g_free (expected);
				break;
		}
	}

	g_ptr_array_unref (lines);
	g_strfreev (all_lines);
}

/* The test program runs itself as a child, with ARV_DEBUG set, and with stderr redirected to a pipe, to a console of
 * its own or to a pseudo console. The child reports to the parent through a file. */

#define CHILD_ARGUMENT		"--debug-output-child"

/* What the child does before its first message */
#define SETUP_NONE		"-"
/* Followed by a locale name: sets the locale of the C runtime, as gst-launch-1.0 and gtk_init() do */
#define SETUP_SETLOCALE		"setlocale:"
/* Reopens stderr on the console, for writing only, as GUI programs do after AttachConsole() or AllocConsole() */
#define SETUP_REOPEN_CONSOLE	"reopen-console"

static char *
console_cells_to_utf8 (const WCHAR *cells, DWORD n_cells)
{
	int size;
	char *text;

	/* Unlike g_utf16_to_utf8(), replaces the unpaired surrogates a console may hold */
	size = WideCharToMultiByte (CP_UTF8, 0, cells, (int) n_cells, NULL, 0, NULL, NULL);
	text = g_malloc0 (size + 1);
	WideCharToMultiByte (CP_UTF8, 0, cells, (int) n_cells, text, size, NULL, NULL);

	return text;
}

static int
debug_output_child (const char *code_page, const char *setup, const char *result_path)
{
	GString *result = g_string_new (NULL);
	CONSOLE_SCREEN_BUFFER_INFO console_info;
	HANDLE console = INVALID_HANDLE_VALUE;
	HANDLE handle;
	HWND console_window;
	DWORD mode;
	UINT console_output_cp;
	gboolean stderr_is_console;

	memset (&console_info, 0, sizeof (console_info));

	if (g_strcmp0 (code_page, "-") != 0) {
		DWORD process_ids[2];

		/* Changes the code page only of a console that belongs to this process alone */
		if (GetConsoleProcessList (process_ids, (DWORD) G_N_ELEMENTS (process_ids)) != 1) {
			g_string_append (result, "skip=No console of its own\n");
			goto out;
		}
		if (!SetConsoleOutputCP (atoi (code_page))) {
			g_string_append_printf (result, "skip=Code page %s not available\n", code_page);
			goto out;
		}
	}

	if (g_str_has_prefix (setup, SETUP_SETLOCALE)) {
		const char *locale = setup + strlen (SETUP_SETLOCALE);

		if (setlocale (LC_ALL, locale) == NULL) {
			g_string_append_printf (result, "skip=Locale %s not available\n", locale);
			goto out;
		}
		g_string_append_printf (result, "crt-code-page=%u\n", ___lc_codepage_func ());
	} else if (g_strcmp0 (setup, SETUP_REOPEN_CONSOLE) == 0) {
		if (freopen ("CONOUT$", "w", stderr) == NULL) {
			g_string_append (result, "skip=Cannot reopen stderr on the console\n");
			goto out;
		}
	}

	handle = (HANDLE) _get_osfhandle (_fileno (stderr));
	/* With read access, which stderr may lack, to read the console back */
	console = CreateFileW (L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			       OPEN_EXISTING, 0, NULL);
	stderr_is_console = console != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo (console, &console_info) &&
		GetFileType (handle) == FILE_TYPE_CHAR;
	console_output_cp = GetConsoleOutputCP ();

	g_string_append_printf (result, "cp=%u\n", console_output_cp);
	g_string_append_printf (result, "stderr-is-console=%d\n", stderr_is_console ? 1 : 0);
	if (GetConsoleMode (handle, &mode))
		g_string_append (result, "stderr-console-mode=available\n");
	else
		g_string_append_printf (result, "stderr-console-mode=error %lu\n", GetLastError ());
	/* Not queried without a console, as the library does not query it either */
	if (console_output_cp != 0)
		g_string_append_printf (result, "console-charset-is-utf8=%d\n",
					g_get_console_charset (NULL) ? 1 : 0);
	/* The window of a console window, or the hidden one of a pseudo console, if any */
	console_window = GetConsoleWindow ();
	if (console_window != NULL) {
		WCHAR class_name[64];
		int length = GetClassNameW (console_window, class_name, (int) G_N_ELEMENTS (class_name));
		char *text = console_cells_to_utf8 (class_name, length > 0 ? (DWORD) length : 0);

		g_string_append_printf (result, "console-window-class=%s\n", text);
		g_string_append_printf (result, "console-window-visible=%d\n", IsWindowVisible (console_window) ? 1 : 0);
		g_free (text);
	} else {
		g_string_append (result, "console-window-class=\n");
	}

	emit_debug_output ();

	if (stderr_is_console) {
		WCHAR cells[256];
		DWORD width = MIN ((DWORD) console_info.dwSize.X, (DWORD) G_N_ELEMENTS (cells));
		SHORT row;

		/* Reads back what the console displays */
		for (row = console_info.dwCursorPosition.Y;
		     row < console_info.dwCursorPosition.Y + 8 && row < console_info.dwSize.Y;
		     row++) {
			COORD coord;
			DWORD n_cells = 0;
			char *text;

			coord.X = 0;
			coord.Y = row;
			if (!ReadConsoleOutputCharacterW (console, cells, width, coord, &n_cells))
				break;

			text = console_cells_to_utf8 (cells, n_cells);
			g_string_append_printf (result, "row=%s\n", g_strchomp (text));
			g_free (text);
		}
	}

out:
	if (console != INVALID_HANDLE_VALUE)
		CloseHandle (console);
	g_file_set_contents (result_path, result->str, -1, NULL);
	g_string_free (result, TRUE);

	return 0;
}

/* The variable that tells the child that a terminal emulator displays its console, if any: none, as for a program run
 * in a console window, that of Windows Terminal, or that of the terminal of Visual Studio Code */
#define TERMINAL_NONE		NULL
#define TERMINAL_WT_SESSION	"WT_SESSION=" WT_SESSION_VALUE
#define TERMINAL_TERM_PROGRAM	"TERM_PROGRAM=vscode"

static WCHAR *
new_child_environment (const char *lang, const char *terminal_variable)
{
	GArray *block = g_array_new (FALSE, FALSE, sizeof (gunichar2));
	gunichar2 terminator = 0;
	char **environment;
	unsigned int i;

	environment = g_get_environ ();
	environment = g_environ_setenv (environment, "ARV_DEBUG", "misc:4", TRUE);
	/* GLib's override of the charset */
	environment = g_environ_unsetenv (environment, "CHARSET");
	if (lang != NULL)
		environment = g_environ_setenv (environment, "LANG", lang, TRUE);
	else
		environment = g_environ_unsetenv (environment, "LANG");
	/* Those the test runs in, which would otherwise tell the child that a terminal displays its console */
	environment = g_environ_unsetenv (environment, "WT_SESSION");
	environment = g_environ_unsetenv (environment, "TERM_PROGRAM");
	if (terminal_variable != NULL) {
		char **name_and_value = g_strsplit (terminal_variable, "=", 2);

		environment = g_environ_setenv (environment, name_and_value[0], name_and_value[1], TRUE);
		g_strfreev (name_and_value);
	}

	for (i = 0; environment[i] != NULL; i++) {
		gunichar2 *variable;
		glong length;

		variable = g_utf8_to_utf16 (environment[i], -1, NULL, &length, NULL);
		if (variable != NULL)
			g_array_append_vals (block, variable, length + 1);
		g_free (variable);
	}
	g_array_append_val (block, terminator);

	g_strfreev (environment);

	return (WCHAR *) g_array_free (block, FALSE);
}

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE	0x00020016
#endif

/*
 * run_child:
 * @code_page: code page the child sets on its console, "-" to keep it
 * @setup: what the child does before its first message, SETUP_NONE for nothing
 * @creation_flags: CREATE_NO_WINDOW for a console of its own, DETACHED_PROCESS for no console, 0 with @pseudo_console
 * @redirect_stderr: whether stderr is redirected to a pipe, or is the console
 * @lang: value of LANG in the child, %NULL to unset it
 * @terminal_variable: (nullable): TERMINAL_NONE, or the variable of a terminal emulator to set in the child
 * @pseudo_console: (nullable): pseudo console (HPCON) the child is attached to
 * @child_stderr: (out): what the child wrote to the pipe
 *
 * Returns: the report of the child
 */

static char *
run_child (const char *code_page, const char *setup, DWORD creation_flags, gboolean redirect_stderr,
	   const char *lang, const char *terminal_variable, gpointer pseudo_console, char **child_stderr)
{
	SECURITY_ATTRIBUTES pipe_attributes;
	STARTUPINFOEXW startup_info;
	PROCESS_INFORMATION process_info;
	HANDLE stderr_read = NULL;
	HANDLE stderr_write = NULL;
	WCHAR executable[MAX_PATH];
	WCHAR *environment;
	gunichar2 *command_line;
	GString *output;
	char *executable_utf8;
	char *result_path = NULL;
	char *command;
	char *result = NULL;
	DWORD creation_error = 0;
	DWORD wait_status = WAIT_FAILED;
	DWORD exit_code = 1;
	int fd;

	g_assert_cmpuint (GetModuleFileNameW (NULL, executable, MAX_PATH), <, MAX_PATH);
	executable_utf8 = g_utf16_to_utf8 ((gunichar2 *) executable, -1, NULL, NULL, NULL);
	g_assert_nonnull (executable_utf8);

	fd = g_file_open_tmp ("arv-debug-test-XXXXXX", &result_path, NULL);
	g_assert_cmpint (fd, >=, 0);
	g_close (fd, NULL);

	command = g_strdup_printf ("\"%s\" " CHILD_ARGUMENT " %s \"%s\" \"%s\"",
				   executable_utf8, code_page, setup, result_path);
	command_line = g_utf8_to_utf16 (command, -1, NULL, NULL, NULL);
	g_assert_nonnull (command_line);
	environment = new_child_environment (lang, terminal_variable);

	memset (&startup_info, 0, sizeof (startup_info));
	startup_info.StartupInfo.cb = sizeof (startup_info.StartupInfo);
	if (redirect_stderr) {
		pipe_attributes.nLength = sizeof (pipe_attributes);
		pipe_attributes.lpSecurityDescriptor = NULL;
		pipe_attributes.bInheritHandle = TRUE;
		/* Larger than the output of the child, which is read once it exits */
		g_assert_true (CreatePipe (&stderr_read, &stderr_write, &pipe_attributes, 65536));
		g_assert_true (SetHandleInformation (stderr_read, HANDLE_FLAG_INHERIT, 0));

		startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup_info.StartupInfo.hStdInput = NULL;
		startup_info.StartupInfo.hStdOutput = NULL;
		startup_info.StartupInfo.hStdError = stderr_write;
	} else if (pseudo_console != NULL) {
		SIZE_T size = 0;

		startup_info.StartupInfo.cb = sizeof (startup_info);
		/* No standard handles: the child gets those of the pseudo console, not those of the test, which the test
		 * runner usually redirects */
		startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		InitializeProcThreadAttributeList (NULL, 1, 0, &size);
		startup_info.lpAttributeList = g_malloc (size);
		g_assert_true (InitializeProcThreadAttributeList (startup_info.lpAttributeList, 1, 0, &size));
		g_assert_true (UpdateProcThreadAttribute (startup_info.lpAttributeList, 0,
							  PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
							  pseudo_console, sizeof (pseudo_console), NULL, NULL));
		creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
	}

	log_text ("Running", command);

	if (CreateProcessW (executable, (WCHAR *) command_line, NULL, NULL, redirect_stderr,
			    creation_flags | CREATE_UNICODE_ENVIRONMENT, environment, NULL,
			    &startup_info.StartupInfo, &process_info)) {
		wait_status = WaitForSingleObject (process_info.hProcess, 30000);
		if (wait_status != WAIT_OBJECT_0)
			TerminateProcess (process_info.hProcess, 1);
		else if (!GetExitCodeProcess (process_info.hProcess, &exit_code))
			exit_code = 1;
		CloseHandle (process_info.hProcess);
		CloseHandle (process_info.hThread);
	} else {
		creation_error = GetLastError ();
	}

	output = g_string_new (NULL);
	if (stderr_read != NULL) {
		char buffer[4096];
		DWORD n_read;

		/* Read once the child is gone, which closes the last write end */
		CloseHandle (stderr_write);
		while (ReadFile (stderr_read, buffer, sizeof (buffer), &n_read, NULL) && n_read > 0)
			g_string_append_len (output, buffer, n_read);
		CloseHandle (stderr_read);
	}
	*child_stderr = g_string_free (output, FALSE);

	if (startup_info.lpAttributeList != NULL) {
		DeleteProcThreadAttributeList (startup_info.lpAttributeList);
		g_free (startup_info.lpAttributeList);
	}

	if (!g_file_get_contents (result_path, &result, NULL, NULL))
		result = g_strdup ("");
	g_remove (result_path);

	log_text ("Child report", result);

	g_assert_cmpuint (creation_error, ==, 0);
	g_assert_cmpuint (wait_status, ==, WAIT_OBJECT_0);
	g_assert_cmpuint (exit_code, ==, 0);

	g_free (environment);
	g_free (command_line);
	g_free (command);
	g_free (result_path);
	g_free (executable_utf8);

	return result;
}

/* Returns: (transfer full): the value of @key in the child report, or the rows read back from the console for "row" */

static char *
get_report_value (const char *report, const char *key)
{
	GString *value = NULL;
	char **lines;
	char *prefix;
	unsigned int i;

	prefix = g_strconcat (key, "=", NULL);
	lines = g_strsplit (report, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		if (g_str_has_prefix (lines[i], prefix)) {
			if (value == NULL)
				value = g_string_new (NULL);
			else
				g_string_append_c (value, '\n');
			g_string_append (value, lines[i] + strlen (prefix));
		}
	}
	g_strfreev (lines);
	g_free (prefix);

	return value != NULL ? g_string_free (value, FALSE) : NULL;
}

static gboolean
skip_if_requested (const char *report)
{
	char *reason = get_report_value (report, "skip");

	if (reason == NULL)
		return FALSE;

	g_test_skip (reason);
	g_free (reason);

	return TRUE;
}

static void
check_redirected_stderr (const char *code_page, const char *setup, DWORD creation_flags, const char *lang,
			 gboolean use_glyphs)
{
	char *child_stderr = NULL;
	char *report;
	char *value;
	guint console_output_cp;
	gboolean predicted_glyphs;

	report = run_child (code_page, setup, creation_flags, TRUE, lang, TERMINAL_NONE, NULL, &child_stderr);
	if (skip_if_requested (report)) {
		g_free (report);
		g_free (child_stderr);
		return;
	}

	value = get_report_value (report, "stderr-is-console");
	g_assert_cmpstr (value, ==, "0");
	g_free (value);

	value = get_report_value (report, "cp");
	g_assert_nonnull (value);
	console_output_cp = atoi (value);
	g_free (value);

	/* A redirected stderr follows g_get_console_charset(), unless there is no console or a UTF-8 one. The locale of
	 * the C runtime does not matter, as the C runtime converts only what it writes to a console. */
	if (console_output_cp == 0 || console_output_cp == CP_UTF8) {
		predicted_glyphs = TRUE;
	} else {
		value = get_report_value (report, "console-charset-is-utf8");
		g_assert_nonnull (value);
		predicted_glyphs = g_strcmp0 (value, "1") == 0;
		g_free (value);
	}
	g_assert_cmpint (predicted_glyphs, ==, use_glyphs);

	/* All the child wrote to stderr, which would include what the debug C runtime of MSVC reports there at exit,
	 * such as a heap block it can't free (see disable_error_dialogs()) */
	check_debug_output (child_stderr, use_glyphs ? EXPECT_GLYPHS : EXPECT_ASCII);

	g_free (report);
	g_free (child_stderr);
}

static void
redirected_legacy_code_page_test (void)
{
	/* g_get_console_charset() answers with the code page of the console */
	check_redirected_stderr ("437", SETUP_NONE, CREATE_NO_WINDOW, NULL, FALSE);
}

static void
redirected_utf8_lang_test (void)
{
	/* As in a mintty terminal of MSYS2, whose processes write to pipes */
	check_redirected_stderr ("437", SETUP_NONE, CREATE_NO_WINDOW, "en_US.UTF-8", TRUE);
}

static void
redirected_utf8_lang_legacy_locale_test (void)
{
	check_redirected_stderr ("437", SETUP_SETLOCALE ".1252", CREATE_NO_WINDOW, "en_US.UTF-8", TRUE);
}

static void
redirected_legacy_lang_test (void)
{
	check_redirected_stderr ("437", SETUP_NONE, CREATE_NO_WINDOW, "en_US.ISO-8859-1", FALSE);
}

static void
redirected_utf8_code_page_test (void)
{
	/* The code page wins over LANG */
	check_redirected_stderr ("65001", SETUP_NONE, CREATE_NO_WINDOW, "en_US.ISO-8859-1", TRUE);
}

static void
redirected_no_console_test (void)
{
	check_redirected_stderr ("-", SETUP_NONE, DETACHED_PROCESS, "en_US.ISO-8859-1", TRUE);
}

/* The child runs in a console of its own, which has no window (CREATE_NO_WINDOW): unless @terminal_variable says that a
 * terminal emulator displays it, it stands for a console window. Read back, it holds a replacement character for each
 * glyph. */

static void
check_console_stderr (const char *code_page, const char *setup, const char *lang, const char *terminal_variable,
		      Expect expect)
{
	char *child_stderr = NULL;
	char *report;
	char *value;

	report = run_child (code_page, setup, CREATE_NO_WINDOW, FALSE, lang, terminal_variable, NULL, &child_stderr);
	if (skip_if_requested (report)) {
		g_free (report);
		g_free (child_stderr);
		return;
	}

	value = get_report_value (report, "stderr-is-console");
	if (g_strcmp0 (value, "1") != 0) {
		g_test_skip ("No console for stderr");
	} else {
		char *rows;
		char *window_class;

		/* Without a window, nothing but the environment tells a terminal */
		window_class = get_report_value (report, "console-window-class");
		g_assert_cmpstr (window_class, ==, "");
		g_free (window_class);

		rows = get_report_value (report, "row");
		g_assert_nonnull (rows);
		check_debug_output (rows, expect);
		g_free (rows);
	}

	g_free (value);
	g_free (report);
	g_free (child_stderr);
}

static void
console_legacy_code_page_test (void)
{
	/* A UTF-8 LANG, as MSYS2 sets it, does not change what the console displays */
	check_console_stderr ("437", SETUP_NONE, "en_US.UTF-8", TERMINAL_NONE, EXPECT_ASCII);
}

static void
console_legacy_code_page_terminal_test (void)
{
	/* Nor does a terminal, which receives what the code page of the console makes of the glyph bytes */
	check_console_stderr ("437", SETUP_NONE, NULL, TERMINAL_WT_SESSION, EXPECT_ASCII);
}

static void
console_legacy_code_page_reopened_test (void)
{
	/* Still the console, although GetConsoleMode() fails on a handle without read access */
	check_console_stderr ("437", SETUP_REOPEN_CONSOLE, "en_US.UTF-8", TERMINAL_NONE, EXPECT_ASCII);
}

static void
console_utf8_code_page_test (void)
{
	/* A console window receives the glyphs intact, but can't show them */
	check_console_stderr ("65001", SETUP_NONE, NULL, TERMINAL_NONE, EXPECT_ASCII);
}

static void
console_utf8_code_page_windows_terminal_test (void)
{
	check_console_stderr ("65001", SETUP_NONE, NULL, TERMINAL_WT_SESSION, EXPECT_CONSOLE_GLYPHS);
}

static void
console_utf8_code_page_term_program_test (void)
{
	check_console_stderr ("65001", SETUP_NONE, NULL, TERMINAL_TERM_PROGRAM, EXPECT_CONSOLE_GLYPHS);
}

static void
console_utf8_code_page_legacy_locale_test (void)
{
	/* The C runtime converts what it writes to the console from the code page of its locale, here 1252, which
	 * would turn each glyph into four characters, whatever displays them */
	check_console_stderr ("65001", SETUP_SETLOCALE ".1252", NULL, TERMINAL_WT_SESSION, EXPECT_ASCII);
}

static void
console_utf8_code_page_utf8_locale_test (void)
{
	check_console_stderr ("65001", SETUP_SETLOCALE ".UTF8", NULL, TERMINAL_NONE, EXPECT_ASCII);
}

static void
console_utf8_code_page_utf8_locale_terminal_test (void)
{
	check_console_stderr ("65001", SETUP_SETLOCALE ".UTF8", NULL, TERMINAL_WT_SESSION, EXPECT_CONSOLE_GLYPHS);
}

/* Pseudo console, as used by Windows Terminal. Unlike a console read back, which holds a replacement character for a
 * glyph, what it sends to its terminal tells the exact characters. */

/* Size of the pseudo console, and of the terminal that displays it */
#define TERMINAL_COLUMNS	120
#define TERMINAL_ROWS		25

/*
 * render_terminal_output:
 * @output: what a pseudo console sent to its terminal
 *
 * Interprets the escape sequences the console host sends for plain text output: cursor moves, erasures and line feeds.
 * The other ones, such as colors, window title and modes, are skipped. Each character takes one column.
 *
 * Returns: the rows the terminal displays, without trailing spaces
 */

static char *
render_terminal_output (const char *output)
{
	gunichar screen[TERMINAL_ROWS][TERMINAL_COLUMNS];
	GString *text = g_string_new (NULL);
	const char *c = output;
	int row = 0;
	int column = 0;
	int i, j;

	for (i = 0; i < TERMINAL_ROWS; i++)
		for (j = 0; j < TERMINAL_COLUMNS; j++)
			screen[i][j] = ' ';

	while (*c != '\0') {
		if (c[0] == '\033' && c[1] == '[') {
			int parameters[2] = { 0, 0 };
			unsigned int n_parameters = 0;
			int first, last;
			char command;

			/* Control sequence: parameter bytes, intermediate bytes, then the command */
			for (c += 2; *c >= 0x30 && *c <= 0x3f; c++) {
				if (*c == ';')
					n_parameters++;
				else if (g_ascii_isdigit (*c) && n_parameters < G_N_ELEMENTS (parameters))
					parameters[n_parameters] = parameters[n_parameters] * 10 + (*c - '0');
			}
			while (*c >= 0x20 && *c <= 0x2f)
				c++;
			if (*c == '\0')
				break;
			command = *c++;

			switch (command) {
				case 'A':
					row -= MAX (parameters[0], 1);
					break;
				case 'B':
					row += MAX (parameters[0], 1);
					break;
				case 'C':
					column += MAX (parameters[0], 1);
					break;
				case 'D':
					column -= MAX (parameters[0], 1);
					break;
				case 'G':
					column = MAX (parameters[0], 1) - 1;
					break;
				case 'H':
				case 'f':
					row = MAX (parameters[0], 1) - 1;
					column = MAX (parameters[1], 1) - 1;
					break;
				case 'J':
					/* Erases the display: after the cursor, before it, or all */
					first = parameters[0] == 0 ? row * TERMINAL_COLUMNS + column : 0;
					last = parameters[0] == 1 ?
						row * TERMINAL_COLUMNS + column : TERMINAL_ROWS * TERMINAL_COLUMNS - 1;
					for (i = first; i <= last && i < TERMINAL_ROWS * TERMINAL_COLUMNS; i++)
						screen[i / TERMINAL_COLUMNS][i % TERMINAL_COLUMNS] = ' ';
					break;
				case 'K':
					/* Erases the line: after the cursor, before it, or all */
					first = parameters[0] == 0 ? column : 0;
					last = parameters[0] == 1 ? column : TERMINAL_COLUMNS - 1;
					for (j = first; j <= last && j < TERMINAL_COLUMNS; j++)
						screen[row][j] = ' ';
					break;
				case 'X':
					/* Erases characters, without moving the cursor */
					for (j = column; j < column + MAX (parameters[0], 1) && j < TERMINAL_COLUMNS; j++)
						screen[row][j] = ' ';
					break;
				default:
					break;
			}
		} else if (c[0] == '\033' && c[1] == ']') {
			/* Operating system command, such as the window title, up to BEL or ST */
			c += 2;
			while (*c != '\0' && *c != '\a' && !(c[0] == '\033' && c[1] == '\\'))
				c++;
			if (*c == '\a')
				c++;
			else if (*c != '\0')
				c += 2;
		} else if (c[0] == '\033') {
			c += c[1] != '\0' ? 2 : 1;
		} else if (*c == '\r') {
			column = 0;
			c++;
		} else if (*c == '\n') {
			if (row == TERMINAL_ROWS - 1) {
				memmove (screen[0], screen[1], sizeof (screen[0]) * (TERMINAL_ROWS - 1));
				for (j = 0; j < TERMINAL_COLUMNS; j++)
					screen[row][j] = ' ';
			} else {
				row++;
			}
			c++;
		} else if ((guchar) *c < 0x20) {
			c++;
		} else {
			gunichar character = g_utf8_get_char_validated (c, -1);

			if (character == (gunichar) -1 || character == (gunichar) -2) {
				character = 0xfffd;
				c++;
			} else {
				c = g_utf8_next_char (c);
			}
			if (column < TERMINAL_COLUMNS)
				screen[row][column] = character;
			column++;
		}

		row = CLAMP (row, 0, TERMINAL_ROWS - 1);
		column = CLAMP (column, 0, TERMINAL_COLUMNS);
	}

	for (i = 0; i < TERMINAL_ROWS; i++) {
		int length = TERMINAL_COLUMNS;

		while (length > 0 && screen[i][length - 1] == ' ')
			length--;
		for (j = 0; j < length; j++)
			g_string_append_unichar (text, screen[i][j]);
		g_string_append_c (text, '\n');
	}

	return g_string_free (text, FALSE);
}

typedef HRESULT (WINAPI *CreatePseudoConsoleFunc) (COORD size, HANDLE input, HANDLE output, DWORD flags,
						   gpointer *pseudo_console);
typedef void (WINAPI *ClosePseudoConsoleFunc) (gpointer pseudo_console);

typedef struct {
	HANDLE pipe;
	GString *data;
} PipeReader;

static DWORD WINAPI
read_pipe (LPVOID user_data)
{
	PipeReader *reader = user_data;
	char buffer[4096];
	DWORD n_read;

	/* Until the console host exits, which closes the other end */
	while (ReadFile (reader->pipe, buffer, sizeof (buffer), &n_read, NULL) && n_read > 0)
		g_string_append_len (reader->data, buffer, n_read);

	return 0;
}

/*
 * run_child_in_pseudo_console:
 * @code_page: code page the child sets on its console
 * @setup: what the child does before its first message
 * @terminal_output: (out): what the pseudo console sent to its terminal
 *
 * Returns: the report of the child, %NULL when there are no pseudo consoles
 */

static char *
run_child_in_pseudo_console (const char *code_page, const char *setup, char **terminal_output)
{
	CreatePseudoConsoleFunc create_pseudo_console;
	ClosePseudoConsoleFunc close_pseudo_console;
	HMODULE kernel32 = GetModuleHandleW (L"kernel32.dll");
	HANDLE input_read, input_write;
	HANDLE output_read, output_write;
	HANDLE thread;
	PipeReader reader;
	gpointer pseudo_console = NULL;
	COORD size;
	HRESULT status;
	char *child_stderr = NULL;
	char *report;

	/* Windows 10 1809 and later */
	create_pseudo_console = (CreatePseudoConsoleFunc) (void (*) (void))
		GetProcAddress (kernel32, "CreatePseudoConsole");
	close_pseudo_console = (ClosePseudoConsoleFunc) (void (*) (void))
		GetProcAddress (kernel32, "ClosePseudoConsole");
	if (create_pseudo_console == NULL || close_pseudo_console == NULL)
		return NULL;

	g_assert_true (CreatePipe (&input_read, &input_write, NULL, 0));
	g_assert_true (CreatePipe (&output_read, &output_write, NULL, 0));

	size.X = TERMINAL_COLUMNS;
	size.Y = TERMINAL_ROWS;
	status = create_pseudo_console (size, input_read, output_write, 0, &pseudo_console);
	/* The console host has its own copies */
	CloseHandle (input_read);
	CloseHandle (output_write);
	if (FAILED (status)) {
		g_test_message ("CreatePseudoConsole: error 0x%08lx", (unsigned long) status);
		CloseHandle (input_write);
		CloseHandle (output_read);
		return NULL;
	}

	/* Read while the child runs, as the console host waits when the pipe is full */
	reader.pipe = output_read;
	reader.data = g_string_new (NULL);
	thread = CreateThread (NULL, 0, read_pipe, &reader, 0, NULL);
	g_assert_nonnull (thread);

	/* Nothing in the environment tells the child that a terminal displays its console: only the pseudo console does */
	report = run_child (code_page, setup, 0, FALSE, NULL, TERMINAL_NONE, pseudo_console, &child_stderr);

	/* Sends the last frame, then ends the console host, which closes the pipe */
	close_pseudo_console (pseudo_console);
	if (WaitForSingleObject (thread, 10000) != WAIT_OBJECT_0) {
		g_test_message ("The output of the pseudo console did not end");
		do
			CancelSynchronousIo (thread);
		while (WaitForSingleObject (thread, 1000) == WAIT_TIMEOUT);
	}

	CloseHandle (thread);
	CloseHandle (output_read);
	CloseHandle (input_write);
	g_free (child_stderr);

	*terminal_output = g_string_free (reader.data, FALSE);

	return report;
}

static void
check_pseudo_console_stderr (const char *code_page, const char *setup, Expect expect)
{
	char *terminal_output = NULL;
	char *report;
	char *value;

	report = run_child_in_pseudo_console (code_page, setup, &terminal_output);
	if (report == NULL) {
		g_test_skip ("No pseudo console, which needs Windows 10 1809 or later");
		return;
	}
	if (skip_if_requested (report)) {
		g_free (report);
		g_free (terminal_output);
		return;
	}

	value = get_report_value (report, "stderr-is-console");
	if (g_strcmp0 (value, "1") != 0) {
		g_test_skip ("No pseudo console for stderr");
	} else {
		char *window_class;

		/* The library tells a pseudo console by the class of its hidden window, the one Windows 11 gives it. Without
		 * that window, only a variable of the environment, unset here, would tell a terminal. */
		window_class = get_report_value (report, "console-window-class");
		if (expect == EXPECT_GLYPHS && g_strcmp0 (window_class, "PseudoConsoleWindow") != 0) {
			g_test_skip ("The pseudo console has no window of the PseudoConsoleWindow class");
		} else {
			char *rows;

			log_text ("Terminal output", terminal_output);
			rows = render_terminal_output (terminal_output);
			check_debug_output (rows, expect);
			g_free (rows);
		}
		g_free (window_class);
	}

	g_free (value);
	g_free (report);
	g_free (terminal_output);
}

static void
pseudo_console_legacy_code_page_test (void)
{
	check_pseudo_console_stderr ("437", SETUP_NONE, EXPECT_ASCII);
}

static void
pseudo_console_utf8_code_page_test (void)
{
	check_pseudo_console_stderr ("65001", SETUP_NONE, EXPECT_GLYPHS);
}

static void
pseudo_console_utf8_code_page_legacy_locale_test (void)
{
	check_pseudo_console_stderr ("65001", SETUP_SETLOCALE ".1252", EXPECT_ASCII);
}

static void
pseudo_console_utf8_code_page_utf8_locale_test (void)
{
	check_pseudo_console_stderr ("65001", SETUP_SETLOCALE ".UTF8", EXPECT_GLYPHS);
}

#endif /* GLIB_CHECK_VERSION(2,62,0) */

#else /* G_OS_WIN32 */

static void
glyphs_test (void)
{
	char *pattern;

	if (g_test_subprocess ()) {
		/* No setlocale(): the charset would be ASCII, which must not matter outside Windows */
		arv_debug_enable ("misc:4");
		emit_debug_output ();
		return;
	}

	g_test_trap_subprocess (NULL, 0, 0);
	g_test_trap_assert_passed ();

	pattern = g_strdup_printf ("*] \360\237\206\206 misc> warning, first line\n"
				   "%*ssecond line\n"
				   "*] \360\237\205\270 misc> info\n"
				   "*] \360\237\205\263 misc> debug\n"
				   "*] \360\237\206\203 misc> trace\n*", INDENTATION, "");
	g_test_trap_assert_stderr (pattern);
	g_free (pattern);
}

#endif /* G_OS_WIN32 */

/*
 * disable_error_dialogs:
 *
 * Makes a failure of this program, or of its children, fail fast instead of waiting for someone to close a dialog: the
 * debug C runtime of MSVC reports its assertion failures and errors on stderr, rather than in a "Debug Assertion
 * Failed!" dialog such as the one it shows at exit for a stream buffer that another C runtime allocated; abort() skips
 * Windows Error Reporting; and a crash shows no dialog either.
 *
 * GLib's g_test_init() does the same for the C runtime GLib is linked with, which an MSVC debug build doesn't share:
 * it uses the debug C runtime, and the GLib of the GStreamer MSVC package the release one. The children don't call
 * g_test_init() at all.
 */

static void
disable_error_dialogs (void)
{
#ifdef G_OS_WIN32
	SetErrorMode (SetErrorMode (0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
#ifdef _MSC_VER
	_set_abort_behavior (0, _CALL_REPORTFAULT);
#endif
#if defined (_MSC_VER) && defined (_DEBUG)
	_CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode (_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile (_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

int
main (int argc, char *argv[])
{
#if defined (G_OS_WIN32) && GLIB_CHECK_VERSION(2,62,0)
	char **arguments;
#endif

	disable_error_dialogs ();

#if defined (G_OS_WIN32) && GLIB_CHECK_VERSION(2,62,0)
	arguments = g_win32_get_command_line ();

	/* Arguments in UTF-8, whatever the code page */
	if (g_strv_length (arguments) == 5 && g_strcmp0 (arguments[1], CHILD_ARGUMENT) == 0) {
		int status = debug_output_child (arguments[2], arguments[3], arguments[4]);

		g_strfreev (arguments);

		return status;
	}
	g_strfreev (arguments);
#endif

	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/debug/use-glyphs", use_glyphs_test);
	g_test_add_func ("/debug/use-glyphs-without-queries", use_glyphs_without_queries_test);
	g_test_add_func ("/debug/is-terminal", is_terminal_test);

#ifdef G_OS_WIN32
#if GLIB_CHECK_VERSION(2,62,0)
	g_test_add_func ("/debug/output/redirected/legacy-code-page", redirected_legacy_code_page_test);
	g_test_add_func ("/debug/output/redirected/utf8-lang", redirected_utf8_lang_test);
	g_test_add_func ("/debug/output/redirected/utf8-lang-legacy-locale", redirected_utf8_lang_legacy_locale_test);
	g_test_add_func ("/debug/output/redirected/legacy-lang", redirected_legacy_lang_test);
	g_test_add_func ("/debug/output/redirected/utf8-code-page", redirected_utf8_code_page_test);
	g_test_add_func ("/debug/output/redirected/no-console", redirected_no_console_test);
	g_test_add_func ("/debug/output/console/legacy-code-page", console_legacy_code_page_test);
	g_test_add_func ("/debug/output/console/legacy-code-page-terminal", console_legacy_code_page_terminal_test);
	g_test_add_func ("/debug/output/console/legacy-code-page-reopened", console_legacy_code_page_reopened_test);
	g_test_add_func ("/debug/output/console/utf8-code-page", console_utf8_code_page_test);
	g_test_add_func ("/debug/output/console/utf8-code-page-windows-terminal",
			 console_utf8_code_page_windows_terminal_test);
	g_test_add_func ("/debug/output/console/utf8-code-page-term-program", console_utf8_code_page_term_program_test);
	g_test_add_func ("/debug/output/console/utf8-code-page-legacy-locale",
			 console_utf8_code_page_legacy_locale_test);
	g_test_add_func ("/debug/output/console/utf8-code-page-utf8-locale", console_utf8_code_page_utf8_locale_test);
	g_test_add_func ("/debug/output/console/utf8-code-page-utf8-locale-terminal",
			 console_utf8_code_page_utf8_locale_terminal_test);
	g_test_add_func ("/debug/output/pseudo-console/legacy-code-page", pseudo_console_legacy_code_page_test);
	g_test_add_func ("/debug/output/pseudo-console/utf8-code-page", pseudo_console_utf8_code_page_test);
	g_test_add_func ("/debug/output/pseudo-console/utf8-code-page-legacy-locale",
			 pseudo_console_utf8_code_page_legacy_locale_test);
	g_test_add_func ("/debug/output/pseudo-console/utf8-code-page-utf8-locale",
			 pseudo_console_utf8_code_page_utf8_locale_test);
#endif
#else
	g_test_add_func ("/debug/output/glyphs", glyphs_test);
#endif

	return g_test_run ();
}
