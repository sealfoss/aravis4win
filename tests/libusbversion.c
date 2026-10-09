/* SPDX-License-Identifier:Unlicense */

/*
 * Checks the libusb library loaded at run time by the programs linked like
 * Aravis.
 *
 * On Windows, meson.build refuses a libusb older than 1.0.27 at configure
 * time, but the library loaded at run time is the first libusb-1.0.dll in the
 * DLL search order, which can be another one. CI jobs also state which libusb
 * they expect to have built:
 *
 *   ARV_TEST_LIBUSB_MIN_VERSION=1.0.30  fails if the loaded libusb is older
 *   ARV_TEST_LIBUSB_HOTPLUG=1           fails unless libusb reports hotplug support
 *   ARV_TEST_LIBUSB_HOTPLUG=0           fails if libusb reports hotplug support
 */

#include <glib.h>
#include <libusb.h>

#define VERSION_NUMBER(major,minor,micro) ((((guint) (major)) << 16) | (((guint) (minor)) << 8) | ((guint) (micro)))

static gboolean
parse_version (const char *string, guint *number)
{
	guint64 parts[3] = {0, 0, 0};
	char **tokens;
	gboolean success;
	guint n_tokens;
	guint i;

	tokens = g_strsplit (string, ".", -1);
	n_tokens = g_strv_length (tokens);
	success = n_tokens >= 1 && n_tokens <= 3;
	for (i = 0; success && i < n_tokens; i++)
		success = g_ascii_string_to_unsigned (tokens[i], 10, 0, 255, &parts[i], NULL);
	g_strfreev (tokens);

	if (success)
		*number = VERSION_NUMBER (parts[0], parts[1], parts[2]);

	return success;
}

static void
check_minimum_version (guint minimum, const char *reason)
{
	const struct libusb_version *version = libusb_get_version ();

	if (VERSION_NUMBER (version->major, version->minor, version->micro) >= minimum)
		return;

	g_test_message ("Run-time libusb %u.%u.%u is older than %u.%u.%u (%s)",
			(guint) version->major, (guint) version->minor, (guint) version->micro,
			minimum >> 16, (minimum >> 8) & 0xff, minimum & 0xff, reason);
	g_test_fail ();
}

static void
version_test (void)
{
	const struct libusb_version *version = libusb_get_version ();
	const char *min_version = g_getenv ("ARV_TEST_LIBUSB_MIN_VERSION");

	g_test_message ("Run-time libusb: %u.%u.%u.%u%s",
			(guint) version->major, (guint) version->minor, (guint) version->micro, (guint) version->nano,
			version->rc != NULL ? version->rc : "");

#ifdef G_OS_WIN32
	check_minimum_version (VERSION_NUMBER (1, 0, 27), "version floor of meson.build on Windows");
#endif

	if (min_version != NULL && min_version[0] != '\0') {
		guint minimum = 0;

		g_assert_true (parse_version (min_version, &minimum));
		check_minimum_version (minimum, "ARV_TEST_LIBUSB_MIN_VERSION");
	}
}

static void
api_version_test (void)
{
	/* A run-time library older than the headers means that another libusb
	 * shadows the one Aravis was built against. */
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	check_minimum_version (VERSION_NUMBER (1, 0, 30), "headers of libusb 1.0.30");
#elif defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010B
	check_minimum_version (VERSION_NUMBER (1, 0, 29), "headers of libusb 1.0.29");
#elif defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010A
	check_minimum_version (VERSION_NUMBER (1, 0, 27), "headers of libusb 1.0.27");
#else
	g_test_skip ("libusb headers older than 1.0.27");
#endif
}

static void
hotplug_test (void)
{
	const char *expected = g_getenv ("ARV_TEST_LIBUSB_HOTPLUG");
	gboolean has_hotplug = libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG) != 0;

	g_test_message ("libusb hotplug support: %s", has_hotplug ? "yes" : "no");

	if (expected == NULL || expected[0] == '\0') {
		g_test_skip ("ARV_TEST_LIBUSB_HOTPLUG not set");
		return;
	}

	g_assert_true (g_strcmp0 (expected, "0") == 0 || g_strcmp0 (expected, "1") == 0);
	g_assert_cmpint (has_hotplug, ==, g_strcmp0 (expected, "1") == 0);
}

int
main (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/libusb/version", version_test);
	g_test_add_func ("/libusb/api-version", api_version_test);
	g_test_add_func ("/libusb/hotplug", hotplug_test);

	return g_test_run ();
}
