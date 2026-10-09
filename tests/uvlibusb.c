/* SPDX-License-Identifier:Unlicense */

/* The functions libusb 1.0.30 added, as Aravis calls them (arv_uv_libusb_*() of arvuvdevice.c), against the libusb of
 * the build rather than the fake one: on Windows, looked up at run time in the libusb DLL, or called directly when
 * libusb is linked statically (ARV_UV_LIBUSB_STATIC, see src/meson.build), as they are elsewhere. A libusb linked
 * statically exports nothing: a lookup there finds nothing, and Aravis goes without RAW_IO and the device strings.
 *
 * libusb_get_device_string () refuses a NULL device with LIBUSB_ERROR_INVALID_PARAM, which shows that Aravis reached
 * it. Aravis gives LIBUSB_ERROR_NOT_SUPPORTED when it did not find it, which only a libusb older than 1.0.30, loaded
 * at run time instead of the one of the build, may justify. */

#include <arvuvdeviceprivate.h>

static void
device_string_test (void)
{
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	const struct libusb_version *version = libusb_get_version ();
	char string[LIBUSB_DEVICE_STRING_BYTES_MAX];
	gboolean older;

	older = version->major == 1 && version->minor == 0 && version->micro < 30;

	if (g_test_verbose ())
		g_printerr ("libusb %u.%u.%u\n", version->major, version->minor, version->micro);

	g_assert_cmpint (arv_uv_libusb_get_device_string (NULL, LIBUSB_DEVICE_STRING_PRODUCT, string, sizeof (string)),
			 ==, older ? LIBUSB_ERROR_NOT_SUPPORTED : LIBUSB_ERROR_INVALID_PARAM);
#else
	g_test_skip ("libusb 1.0.30 headers needed");
#endif
}

int
main (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/uvlibusb/device-string", device_string_test);

	return g_test_run ();
}
