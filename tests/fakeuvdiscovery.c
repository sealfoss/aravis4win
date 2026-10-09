/* SPDX-License-Identifier:Unlicense */

/* USB3 Vision devices that are found but cannot be opened, against the fake libusb:
 *
 *  - the device not found errors of arv_open_device () and arv_camera_new () name every USB3 Vision device libusb
 *    could not open, with its ids, its location, the libusb error and a hint at the cause, with no debug output
 *    enabled; arv-tool prints the same text;
 *  - their device strings are only read to build these errors, not by each discovery;
 *  - the USB3Vision interface itself reports no error for them, so that devices of the other interfaces are still
 *    found;
 *  - opening a device opens, and sends requests to, no device that fails discovery's USB3 Vision descriptor test, nor,
 *    once discovery has found where the camera is, any other USB3 Vision device; a camera plugged in again, at another
 *    address, or one whose place another camera took, is still found;
 *  - with a libusb that lacks libusb_get_device_string () at run time (an older libusb-1.0.dll on Windows), the errors
 *    go without the device strings.
 *
 * The tests that check Aravis's log run a scenario registered under /fakeuvdiscovery/subprocess/, which GLib only
 * runs through g_test_trap_subprocess (), and match the stderr of that subprocess. */

#include "fakeuvtest.h"
#include <arvdebugprivate.h>
#include <arvuvdeviceprivate.h>
#include <arvuvinterfaceprivate.h>
#include <arvsystemprivate.h>
#include <stdio.h>
#include <string.h>

/* The location of the cameras that fail to open, to look for in the messages */
#define CAMERA_BUS		2
#define CAMERA_ADDRESS		7

#define WEBCAM_PRODUCT_ID	0x0003

/* What libusb_open () returns on Windows for a camera without a WinUSB driver on its USB3 Vision function
 * (NOT_FOUND), with an unsupported driver (NOT_SUPPORTED), already open (ACCESS) or for any other failure (IO); and
 * for a camera unplugged meanwhile (NO_DEVICE) */
static const int open_errors[] = {
	LIBUSB_ERROR_NOT_FOUND,
	LIBUSB_ERROR_NOT_SUPPORTED,
	LIBUSB_ERROR_ACCESS,
	LIBUSB_ERROR_IO,
	LIBUSB_ERROR_NO_DEVICE
};

static FakeUsbDevice *
add_failing_camera (const char *serial_number, int open_error)
{
	FakeUsbDevice *camera;

	camera = fakeuv_add_camera (serial_number);
	fake_usb_device_set_location (camera, CAMERA_BUS, CAMERA_ADDRESS, 1);
	fake_usb_device_set_open_error (camera, open_error);

	return camera;
}

/* A device of the USB3 Vision device class (Miscellaneous, interface association) without the USB3 Vision
 * interfaces: a webcam */

static FakeUsbDevice *
add_webcam (void)
{
	FakeUsbDevice *device;
	guint8 interface_number;

	device = fake_usb_device_new (FAKEUV_OTHER_DEVICE_VENDOR_ID, WEBCAM_PRODUCT_ID, ARV_UV_INTERFACE_DEVICE_CLASS,
				      ARV_UV_INTERFACE_DEVICE_SUBCLASS, ARV_UV_INTERFACE_DEVICE_PROTOCOL);
	fake_usb_device_set_descriptor_strings (device,
						fake_usb_device_add_string (device, "Aravis"),
						fake_usb_device_add_string (device, "Fake webcam"),
						fake_usb_device_add_string (device, "UV1"));
	/* Video control and video streaming */
	interface_number = fake_usb_device_add_interface (device, 0x0e, 0x01, 0x00, NULL, 0);
	fake_usb_device_add_endpoint (device, interface_number, 0x81, LIBUSB_TRANSFER_TYPE_INTERRUPT, 16);
	interface_number = fake_usb_device_add_interface (device, 0x0e, 0x02, 0x00, NULL, 0);
	fake_usb_device_add_endpoint (device, interface_number, 0x82, LIBUSB_TRANSFER_TYPE_BULK, 1024);

	return device;
}

static guint
count_lines (const char *text)
{
	guint n_lines = 1;

	for (; *text != '\0'; text++)
		if (*text == '\n')
			n_lines++;

	return n_lines;
}

/* @text describes the failed open of a camera added by add_failing_camera () */

static void
assert_open_error (const char *text, int open_error)
{
	char *ids;
	char *location;

	ids = g_strdup_printf ("Failed to open USB3 Vision device %04x:%04x ",
			       FAKE_USB_CAMERA_VENDOR_ID, FAKE_USB_CAMERA_PRODUCT_ID);
	location = g_strdup_printf (" on bus %u, address %u: %s (%s)", CAMERA_BUS, CAMERA_ADDRESS,
				    libusb_error_name (open_error), arv_uv_interface_get_open_error_hint (open_error));

	if (strstr (text, ids) == NULL || strstr (text, location) == NULL)
		g_printerr ("Unexpected text:\n%s\n", text);
	g_assert_nonnull (strstr (text, ids));
	g_assert_nonnull (strstr (text, location));

	g_free (ids);
	g_free (location);
}

/* Device not found errors */

static void
not_found_error_test (void)
{
	guint i;

	/* The default debug level hides the warnings of discovery: the errors are the only report */
	if (g_getenv ("ARV_DEBUG") == NULL)
		g_assert_false (arv_debug_check (ARV_DEBUG_CATEGORY_INTERFACE, ARV_DEBUG_LEVEL_WARNING));

	for (i = 0; i < G_N_ELEMENTS (open_errors); i++) {
		FakeUsbDevice *other;
		FakeUsbDevice *camera;
		ArvCamera *arv_camera;
		ArvDevice *device;
		GError *error = NULL;

		fakeuv_test_reset ();

		other = fakeuv_add_other_device ();
		camera = add_failing_camera ("UV1", open_errors[i]);

		/* Also forgets the devices of the previous tests */
		arv_update_device_list ();
		g_assert_cmpuint (arv_get_n_devices (), ==, 0);

		/* The first available device */
		arv_camera = arv_camera_new (NULL, &error);
		g_assert_null (arv_camera);
		g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
		g_assert_true (g_str_has_prefix (error->message, "No supported device found\n"));
		g_assert_cmpuint (count_lines (error->message), ==, 2);
		assert_open_error (error->message, open_errors[i]);
		g_clear_error (&error);

		/* A device by name */
		device = arv_open_device ("Aravis-Fake-UV1", &error);
		g_assert_null (device);
		g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
		g_assert_true (g_str_has_prefix (error->message, "Device 'Aravis-Fake-UV1' not found\n"));
		g_assert_cmpuint (count_lines (error->message), ==, 2);
		assert_open_error (error->message, open_errors[i]);
		g_clear_error (&error);

		arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
		g_assert_null (arv_camera);
		g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
		assert_open_error (error->message, open_errors[i]);
		g_clear_error (&error);

		/* Discoveries only: the camera was never opened, the other device never looked at */
		g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_OPEN), >, 0);
		g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_CLAIM_INTERFACE), ==, 0);
		g_assert_cmpuint (fake_usb_device_get_call_count (other, FAKE_USB_CALL_OPEN), ==, 0);
		g_assert_cmpuint (fake_usb_device_get_call_count (other, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);
		g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	}
}

static void
two_cameras_test (void)
{
	ArvCamera *arv_camera;
	GError *error = NULL;
	char **lines;

	add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);
	add_failing_camera ("UV2", LIBUSB_ERROR_ACCESS);

	arv_update_device_list ();

	/* One line per camera, in the order of the USB device list */
	arv_camera = arv_camera_new (NULL, &error);
	g_assert_null (arv_camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	lines = g_strsplit (error->message, "\n", -1);
	g_assert_cmpuint (g_strv_length (lines), ==, 3);
	assert_open_error (lines[1], LIBUSB_ERROR_NOT_FOUND);
	assert_open_error (lines[2], LIBUSB_ERROR_ACCESS);
	g_strfreev (lines);
	g_clear_error (&error);
}

static void
device_strings_test (void)
{
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	FakeUsbDevice *other;
	FakeUsbDevice *working;
	FakeUsbDevice *failing;
	ArvCamera *arv_camera;
	GError *error = NULL;

	other = fakeuv_add_other_device ();
	working = fakeuv_add_camera ("UV2");
	failing = add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	/* libusb >= 1.0.30 reads the manufacturer, product and serial number strings of a device it cannot open */
	arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
	g_assert_null (arv_camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	assert_open_error (error->message, LIBUSB_ERROR_NOT_FOUND);
	g_assert_nonnull (strstr (error->message, " 1209:0001 (Aravis Fake S/N UV1) on bus "));
	g_clear_error (&error);

	/* Only for the devices that failed to open: on Windows each string is a request through the hub */
	g_assert_cmpuint (fake_usb_device_get_call_count (failing, FAKE_USB_CALL_GET_DEVICE_STRING), >, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (working, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (other, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);

	/* Strings the device does not have, or that cannot be read, are left out. New devices each time: libusb
	 * caches the strings in its device objects, which a context keeps when it has hotplug. */
	fakeuv_test_reset ();
	failing = add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);
	fake_usb_device_set_descriptor_strings (failing, FAKE_USB_CAMERA_STRING_MANUFACTURER,
						FAKE_USB_CAMERA_STRING_PRODUCT, 0);
	arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
	g_assert_null (arv_camera);
	assert_open_error (error->message, LIBUSB_ERROR_NOT_FOUND);
	g_assert_nonnull (strstr (error->message, " 1209:0001 (Aravis Fake) on bus "));
	g_clear_error (&error);

	/* As on Windows for a suspended device */
	fakeuv_test_reset ();
	failing = add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);
	fake_usb_device_inject_error (failing, FAKE_USB_CALL_GET_DEVICE_STRING, -1, LIBUSB_ERROR_IO, -1);
	arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
	g_assert_null (arv_camera);
	assert_open_error (error->message, LIBUSB_ERROR_NOT_FOUND);
	g_assert_nonnull (strstr (error->message, " 1209:0001 on bus "));
	g_clear_error (&error);

	/* A string that is not valid UTF-8, such as libusb's Windows backend makes of a descriptor with an unpaired
	 * UTF-16 surrogate, is repaired */
	fakeuv_test_reset ();
	failing = add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);
	fake_usb_device_set_string (failing, FAKE_USB_CAMERA_STRING_MANUFACTURER, "Ara\xed\xa0\x80vis");
	arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
	g_assert_null (arv_camera);
	assert_open_error (error->message, LIBUSB_ERROR_NOT_FOUND);
	g_assert_true (g_utf8_validate (error->message, -1, NULL));
	g_assert_nonnull (strstr (error->message, " 1209:0001 (Ara\xef\xbf\xbd"));
	g_assert_nonnull (strstr (error->message, "\xef\xbf\xbdvis Fake S/N UV1) on bus "));
	g_clear_error (&error);
#else
	g_test_skip ("libusb_get_device_string () needs libusb >= 1.0.30 headers");
#endif
}

/* Device strings are read for error messages only: on Windows each one is a request through the hub to a device that
 * may be in use by another process, and fails while the device is suspended */

static void
string_requests_test (void)
{
	FakeUsbDevice *busy;
	ArvDevice *device;
	GError *error = NULL;
	guint i;

	/* As with the default Windows libusb, a device object only lives as long as it is referenced */
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	/* A camera in use by another process: WinUSB opens a device for one handle at a time */
	fakeuv_add_camera ("UV2");
	busy = add_failing_camera ("UV1", LIBUSB_ERROR_ACCESS);

	/* An application that refreshes its device list sends it nothing */
	for (i = 0; i < 5; i++)
		arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_call_count (busy, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);

	/* Nor does the open of a device that is not there: its error lists the camera without its strings */
	device = arv_uv_device_new_from_guid ("FakeUV-UV9", &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	assert_open_error (error->message, LIBUSB_ERROR_ACCESS);
	g_assert_nonnull (strstr (error->message, " 1209:0001 on bus "));
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_device_get_call_count (busy, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	{
		char *open_errors;

		/* The device not found error of arv_open_device () has them */
		device = arv_open_device ("Aravis-Fake-UV9", &error);
		g_assert_null (device);
		g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
		g_assert_nonnull (strstr (error->message, " 1209:0001 (Aravis Fake S/N UV1) on bus "));
		g_clear_error (&error);
		g_assert_cmpuint (fake_usb_device_get_call_count (busy, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 3);

		/* Read once per discovery */
		open_errors = arv_dup_device_open_errors ();
		g_assert_nonnull (strstr (open_errors, " 1209:0001 (Aravis Fake S/N UV1) on bus "));
		g_free (open_errors);
		g_assert_cmpuint (fake_usb_device_get_call_count (busy, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 3);

		/* And from the device once while it keeps failing: libusb caches them in its device object, which the
		 * USB3Vision interface keeps from one discovery to the next */
		arv_update_device_list ();
		open_errors = arv_dup_device_open_errors ();
		g_assert_nonnull (strstr (open_errors, " 1209:0001 (Aravis Fake S/N UV1) on bus "));
		g_free (open_errors);
		g_assert_cmpuint (fake_usb_device_get_stat (busy, FAKE_USB_STAT_DEVICE_STRING_FETCHES), ==, 3);
	}
#endif
}

/* A discovery that cannot list the USB devices does not report the open errors of an older one */

static void
device_list_error_test (void)
{
	ArvDevice *device;
	GError *error = NULL;
	char *open_errors;

	add_failing_camera ("UV1", LIBUSB_ERROR_ACCESS);
	arv_update_device_list ();
	open_errors = arv_dup_device_open_errors ();
	g_assert_nonnull (open_errors);
	g_free (open_errors);

	fake_usb_inject_error (FAKE_USB_CALL_GET_DEVICE_LIST, LIBUSB_ERROR_NO_MEM, 1);
	arv_update_device_list ();
	g_assert_null (arv_dup_device_open_errors ());

	arv_update_device_list ();
	open_errors = arv_dup_device_open_errors ();
	g_assert_nonnull (open_errors);
	assert_open_error (open_errors, LIBUSB_ERROR_ACCESS);
	g_free (open_errors);

	/* The discovery of arv_open_device () */
	fake_usb_inject_error (FAKE_USB_CALL_GET_DEVICE_LIST, LIBUSB_ERROR_NO_MEM, 1);
	device = arv_open_device ("Aravis-Fake-UV1", &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_cmpstr (error->message, ==, "Device 'Aravis-Fake-UV1' not found");
	g_clear_error (&error);
}

/* Other devices are still found */

static void
other_devices_test (void)
{
	FakeUsbDevice *working;
	ArvCamera *arv_camera;
	ArvDevice *device;
	GError *error = NULL;

	add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);
	working = fakeuv_add_camera ("UV2");

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);
	g_assert_cmpstr (arv_get_device_id (0), ==, "Aravis-Fake-UV2");

	/* The USB3Vision interface reports no error for a device it could not open: arv_open_device () stops at the
	 * first interface that reports one, and the devices of the next ones (GigEVision, GenTL) would be hidden */
	device = arv_interface_open_device (arv_get_interface_by_id ("USB3Vision"), "Aravis-Fake-UV1", &error);
	g_assert_no_error (error);
	g_assert_null (device);

	/* The working camera, as the first available device and by name */
	arv_camera = arv_camera_new (NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (arv_camera_get_device_serial_number (arv_camera, &error), ==, "UV2");
	g_assert_no_error (error);
	g_clear_object (&arv_camera);

	arv_camera = arv_camera_new ("Aravis-Fake-UV2", &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (arv_camera));
	g_clear_object (&arv_camera);
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (working), ==, 0);

	/* And a device of another interface */
	arv_enable_interface ("Fake");

	arv_camera = arv_camera_new ("Fake_1", &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (arv_camera));
	g_clear_object (&arv_camera);

	device = arv_open_device ("Aravis-Fake-UV1", &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	assert_open_error (error->message, LIBUSB_ERROR_NOT_FOUND);
	g_clear_error (&error);

	arv_select_interface ("USB3Vision");
}

/* Open errors of a known device */

static void
known_device_test (void)
{
	FakeUsbDevice *camera;
	ArvCamera *first;
	ArvCamera *second;
	ArvDevice *device;
	GError *error = NULL;

	/* WinUSB opens a device for one handle at a time */
	camera = fakeuv_add_camera ("UV1");
	fake_usb_device_set_location (camera, CAMERA_BUS, CAMERA_ADDRESS, 1);
	fake_usb_device_set_exclusive_open (camera, TRUE);

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	first = fakeuv_open_camera (camera, &error);
	g_assert_no_error (error);

	/* A second open of a camera the USB3Vision interface knows, in this process or another one, fails with the
	 * error of libusb_open () */
	second = fakeuv_open_camera (camera, &error);
	g_assert_null (second);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_true (g_str_has_prefix (error->message, "USB device with GUID FakeUV-UV1 not found\n"));
	g_assert_cmpuint (count_lines (error->message), ==, 2);
	assert_open_error (error->message, LIBUSB_ERROR_ACCESS);
	g_clear_error (&error);

	device = arv_uv_device_new ("Aravis", "Fake", "UV1", &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_true (g_str_has_prefix (error->message, "USB device 'Aravis:Fake:UV1' not found\n"));
	assert_open_error (error->message, LIBUSB_ERROR_ACCESS);
	g_clear_error (&error);

	/* Free again, it opens */
	g_clear_object (&first);
	second = fakeuv_open_camera (camera, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (second));
	g_clear_object (&second);
}

/* Opening a device probes only USB3 Vision devices */

static void
assert_untouched (FakeUsbDevice *device)
{
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_OPEN), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_STRING_REQUESTS), ==, 0);
}

static void
open_probes_test (void)
{
	FakeUsbDevice *hid;
	FakeUsbDevice *other_class;
	FakeUsbDevice *webcam;
	FakeUsbDevice *camera;
	ArvCamera *arv_camera;
	ArvDevice *device;
	GError *error = NULL;

	/* Listed before the camera, where an open that tries every device meets them first: a HID device, a camera
	 * with the USB3 Vision interfaces but another device class, and a device of the USB3 Vision device class
	 * without its interfaces */
	hid = fakeuv_add_other_device ();
	other_class = fakeuv_add_camera ("UV2");
	fake_usb_device_set_class (other_class, 0x00, 0x00, 0x00);
	webcam = add_webcam ();
	camera = fakeuv_add_camera ("UV1");

	/* Discovery lists the camera only */
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	/* By device id */
	arv_camera = fakeuv_open_camera (camera, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (arv_camera));
	g_clear_object (&arv_camera);

	/* By GUID, and by vendor, model and serial number */
	device = arv_uv_device_new_from_guid (fake_usb_camera_get_guid (camera), &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_UV_DEVICE (device));
	g_clear_object (&device);

	device = arv_uv_device_new ("Aravis", "Fake", "UV1", &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_UV_DEVICE (device));
	g_clear_object (&device);

	assert_untouched (hid);
	assert_untouched (other_class);
	assert_untouched (webcam);

	/* A device with the USB3 Vision interfaces but another device class is no longer opened by exact name, as
	 * discovery does not list it */
	device = arv_uv_device_new ("Aravis", "Fake", "UV2", &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_clear_error (&error);

	device = arv_uv_device_new_from_guid (fake_usb_camera_get_guid (other_class), &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_clear_error (&error);

	assert_untouched (other_class);
}

/* Opens the camera with the GUID of @camera, which must succeed */

static void
open_by_guid (FakeUsbDevice *camera)
{
	ArvDevice *device;
	GError *error = NULL;

	device = arv_uv_device_new_from_guid (fake_usb_camera_get_guid (camera), &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_UV_DEVICE (device));
	g_clear_object (&device);
}

/* Opened, asked for its strings, then closed, as when an open looks at the USB3 Vision devices in turn */

static void
assert_probed (FakeUsbDevice *device)
{
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_OPEN), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_STRING_REQUESTS), >, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLAIM_INTERFACE), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);
}

/* Once discovery has found where a camera is, opening it opens no other USB3 Vision device, nor asks one for its
 * strings: another one may be in use, in this process or another one, where WinUSB lets one handle at a time open a
 * device (another process opening it at that moment would get LIBUSB_ERROR_ACCESS) */

static void
open_other_camera_test (void)
{
	FakeUsbDevice *first;
	FakeUsbDevice *second;
	ArvCamera *camera;
	GError *error = NULL;

	first = fakeuv_add_camera ("UV1");
	second = fakeuv_add_camera ("UV2");

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 2);

	/* The one listed second, by GUID and by device id */
	fake_usb_reset_counters ();
	open_by_guid (second);
	camera = fakeuv_open_camera (second, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_clear_object (&camera);

	assert_untouched (first);
	g_assert_cmpuint (fake_usb_device_get_call_count (second, FAKE_USB_CALL_OPEN), ==, 2);

	/* And the first one */
	fake_usb_reset_counters ();
	open_by_guid (first);
	assert_untouched (second);

	/* While the first one is open, here or in another process: the second one opens, the first one untouched */
	camera = fakeuv_open_camera (first, &error);
	g_assert_no_error (error);
	fake_usb_reset_counters ();
	open_by_guid (second);
	assert_untouched (first);
	g_clear_object (&camera);
}

/* Without a discovery before, nothing tells where a camera is: an open by GUID looks at the USB3 Vision devices in
 * turn, as it did before, and knows the place of the camera afterwards. An open by device id of an unknown device
 * discovers first. */

static void
open_without_discovery_test (void)
{
	FakeUsbDevice *first;
	FakeUsbDevice *second;
	ArvCamera *camera;
	GError *error = NULL;

	first = fakeuv_add_camera ("UV1");
	second = fakeuv_add_camera ("UV2");

	open_by_guid (second);
	assert_probed (first);

	fake_usb_reset_counters ();
	open_by_guid (second);
	assert_untouched (first);

	/* Nothing known after arv_shutdown (): the open by device id discovers, which probes every camera once, then
	 * goes straight to the camera */
	arv_shutdown ();
	fake_usb_reset_counters ();

	camera = fakeuv_open_camera (second, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_clear_object (&camera);

	assert_probed (first);
	g_assert_cmpuint (fake_usb_device_get_call_count (second, FAKE_USB_CALL_OPEN), ==, 2);
}

/* A camera plugged in again gets another address: the open does not find it where discovery saw it, looks at every
 * USB3 Vision device, and knows the new place afterwards */

static void
open_replugged_camera_test (void)
{
	FakeUsbDevice *first;
	FakeUsbDevice *second;
	guint8 address, new_address;

	first = fakeuv_add_camera ("UV1");
	second = fakeuv_add_camera ("UV2");

	arv_update_device_list ();
	fake_usb_device_get_location (second, NULL, &address, NULL);

	fake_usb_device_unplug (second);
	fake_usb_device_plug (second);
	fake_usb_device_get_location (second, NULL, &new_address, NULL);
	g_assert_cmpuint (new_address, !=, address);

	fake_usb_reset_counters ();
	open_by_guid (second);
	assert_probed (first);

	fake_usb_reset_counters ();
	open_by_guid (second);
	assert_untouched (first);
}

/* Another camera took the place where discovery saw this one: opened there, it is found to be another one, closed,
 * and every other USB3 Vision device is tried */

static void
open_moved_camera_test (void)
{
	FakeUsbDevice *first;
	FakeUsbDevice *second;
	guint8 bus, address, port;

	first = fakeuv_add_camera ("UV1");
	second = fakeuv_add_camera ("UV2");

	arv_update_device_list ();
	fake_usb_device_get_location (second, &bus, &address, &port);

	fake_usb_device_unplug (first);
	fake_usb_device_unplug (second);
	fake_usb_device_plug (first);
	fake_usb_device_set_location (first, bus, address, port);
	fake_usb_device_plug (second);

	fake_usb_reset_counters ();
	open_by_guid (second);

	/* Once, at the place of the second camera */
	assert_probed (first);

	/* Then the second one is where the open found it, and the first one where nothing knows of it */
	fake_usb_reset_counters ();
	open_by_guid (second);
	assert_untouched (first);
}

/* On Windows, Aravis looks libusb_get_device_string () up at run time, so that it still loads with an older
 * libusb-1.0.dll, which lacks it, as kernel32.dll does: the device not found errors then name the cameras without
 * their strings */

static void
older_libusb_dll_test (void)
{
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
	FakeUsbDevice *failing;
	ArvCamera *arv_camera;
	GError *error = NULL;

	failing = add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);

	g_assert_false (arv_uv_libusb_look_functions_up (GetModuleHandleW (L"kernel32.dll"), NULL));

	/* The error of arv_open_device (), which gives the device strings where it can, rather than the one of the open
	 * of a camera that an earlier discovery listed, which never does */
	arv_update_device_list ();
	arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
	g_assert_null (arv_camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_true (g_str_has_prefix (error->message, "Device 'Aravis-Fake-UV1' not found\n"));
	assert_open_error (error->message, LIBUSB_ERROR_NOT_FOUND);
	g_assert_nonnull (strstr (error->message, " 1209:0001 on bus "));
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_device_get_call_count (failing, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 0);

	/* The libusb of the program, the fake, has it */
	g_assert_true (arv_uv_libusb_look_functions_up (NULL, NULL));

	arv_update_device_list ();
	arv_camera = arv_camera_new ("Aravis-Fake-UV1", &error);
	g_assert_null (arv_camera);
	g_assert_true (g_str_has_prefix (error->message, "Device 'Aravis-Fake-UV1' not found\n"));
	g_assert_nonnull (strstr (error->message, " 1209:0001 (Aravis Fake S/N UV1) on bus "));
	g_clear_error (&error);
#else
	g_test_skip ("Looked up at run time on Windows only, with libusb 1.0.30 headers, for a libusb DLL");
#endif
}

/* arv-tool */

static void
arv_tool_test (void)
{
	FakeUsbDevice *failing;
	ArvDevice *device;
	GError *error = NULL;
	char *open_errors;
	char *message;

	/* Nothing to say while every camera opens */
	fakeuv_add_camera ("UV2");
	arv_update_device_list ();
	g_assert_null (arv_dup_device_open_errors ());

	failing = add_failing_camera ("UV1", LIBUSB_ERROR_ACCESS);
	arv_update_device_list ();
	open_errors = arv_dup_device_open_errors ();
	g_assert_nonnull (open_errors);
	g_assert_cmpuint (count_lines (open_errors), ==, 1);
	assert_open_error (open_errors, LIBUSB_ERROR_ACCESS);

	/* arv-tool prints what arv_open_device () appends to its error */
	device = arv_open_device ("Aravis-Fake-UV3", &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	message = g_strdup_printf ("Device 'Aravis-Fake-UV3' not found\n%s", open_errors);
	g_assert_cmpstr (error->message, ==, message);
	g_free (message);
	g_clear_error (&error);
	g_free (open_errors);

	/* Not when the USB3Vision interface is disabled */
	arv_disable_interface ("USB3Vision");
	g_assert_null (arv_dup_device_open_errors ());
	arv_enable_interface ("USB3Vision");

	/* Nor once the camera opens */
	fake_usb_device_set_open_error (failing, 0);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 2);
	g_assert_null (arv_dup_device_open_errors ());
}

/* Hints */

static void
hints_test (void)
{
	const char *hint;
	int libusb_error;

	/* libusb_open () on Windows (libusb 1.0.30, os/windows_winusb.c): no usable WinUSB-type driver on any interface
	 * of a composite device, which includes WinUSB bound without a DeviceInterfaceGUIDs registry value (by Device
	 * Manager), a driver libusb does not support, and CreateFile () denied or no WinUSB DLL. The first two say what
	 * to do. */
#if defined (G_OS_WIN32)
	hint = arv_uv_interface_get_open_error_hint (LIBUSB_ERROR_NOT_FOUND);
	g_assert_nonnull (strstr (hint, "WinUSB driver"));
	g_assert_nonnull (strstr (hint, "the vendor's driver"));
	g_assert_nonnull (strstr (hint, "without a DeviceInterfaceGUIDs registry value"));
	g_assert_nonnull (strstr (hint, "bind WinUSB to the function"));
	hint = arv_uv_interface_get_open_error_hint (LIBUSB_ERROR_NOT_SUPPORTED);
	g_assert_nonnull (strstr (hint, "does not support"));
	g_assert_nonnull (strstr (hint, "bind WinUSB instead"));
	g_assert_nonnull (strstr (arv_uv_interface_get_open_error_hint (LIBUSB_ERROR_ACCESS), "already open"));
#elif defined (__linux__)
	/* No write permission on the usbfs node (os/linux_usbfs.c) */
	g_assert_nonnull (strstr (arv_uv_interface_get_open_error_hint (LIBUSB_ERROR_ACCESS), "udev"));
#endif

	for (libusb_error = LIBUSB_ERROR_IO; libusb_error >= LIBUSB_ERROR_OTHER; libusb_error--) {
		hint = arv_uv_interface_get_open_error_hint (libusb_error);

		g_assert_nonnull (hint);
		g_assert_cmpstr (hint, !=, "");
		g_assert_null (strchr (hint, '\n'));
	}
}

/* Log checks */

static void
enable_debug_output (const char *categories)
{
	arv_debug_enable (categories);
}

static void
subprocess_summary (void)
{
	enable_debug_output ("interface:2");

	fakeuv_add_other_device ();
	add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);
	fakeuv_add_camera ("UV2");

	arv_update_device_list ();
}

static void
summary_log_test (void)
{
	char *pattern;

	g_test_trap_subprocess ("/fakeuvdiscovery/subprocess/summary",
				(guint64) fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS) * 1000, 0);
	g_test_trap_assert_passed ();

	/* A warning per device, and the count in the summary line */
	pattern = g_strdup_printf ("*Failed to open USB3 Vision device %04x:%04x *on bus %u, address %u: "
				   "LIBUSB_ERROR_NOT_FOUND*"
				   "Found 1 USB3Vision device (among 3 USB devices), 1 could not be opened*",
				   FAKE_USB_CAMERA_VENDOR_ID, FAKE_USB_CAMERA_PRODUCT_ID, CAMERA_BUS, CAMERA_ADDRESS);
	g_test_trap_assert_stderr (pattern);
	g_free (pattern);
}

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvdiscovery/not-found-error", not_found_error_test);
	fakeuv_test_add ("/fakeuvdiscovery/two-cameras", two_cameras_test);
	fakeuv_test_add ("/fakeuvdiscovery/device-strings", device_strings_test);
	fakeuv_test_add ("/fakeuvdiscovery/string-requests", string_requests_test);
	fakeuv_test_add ("/fakeuvdiscovery/device-list-error", device_list_error_test);
	fakeuv_test_add ("/fakeuvdiscovery/other-devices", other_devices_test);
	fakeuv_test_add ("/fakeuvdiscovery/known-device", known_device_test);
	fakeuv_test_add ("/fakeuvdiscovery/open-probes", open_probes_test);
	fakeuv_test_add ("/fakeuvdiscovery/open-other-camera", open_other_camera_test);
	fakeuv_test_add ("/fakeuvdiscovery/open-without-discovery", open_without_discovery_test);
	fakeuv_test_add ("/fakeuvdiscovery/open-replugged-camera", open_replugged_camera_test);
	fakeuv_test_add ("/fakeuvdiscovery/open-moved-camera", open_moved_camera_test);
	fakeuv_test_add ("/fakeuvdiscovery/older-libusb-dll", older_libusb_dll_test);
	fakeuv_test_add ("/fakeuvdiscovery/arv-tool", arv_tool_test);
	fakeuv_test_add ("/fakeuvdiscovery/hints", hints_test);
	fakeuv_test_add ("/fakeuvdiscovery/summary-log", summary_log_test);

	/* Only run by the log tests, through g_test_trap_subprocess () */
	fakeuv_test_add ("/fakeuvdiscovery/subprocess/summary", subprocess_summary);

	return fakeuv_test_run ();
}
