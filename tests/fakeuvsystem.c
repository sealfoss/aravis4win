/* SPDX-License-Identifier:Unlicense */

/* The device opening functions of the device list (src/arvsystem.c) with the USB3Vision interface, against the fake
 * libusb, and the generation of the device list as cameras come and go.
 *
 * arv_open_device_on_interface (), which aravissrc uses when its 'interface' property is set, gives the same device
 * not found error as arv_open_device (): one line per USB3 Vision device that discovery found but libusb could not
 * open, with its ids, its location, the libusb error and a hint at the cause, such as a camera without a usable WinUSB
 * driver on Windows. Also when the USB3Vision interface is disabled for discovery, as it opens devices through a
 * disabled interface too. An error of the interface itself, about a device it found, is given as it is. */

#include "fakeuvtest.h"
#include <arvuvinterfaceprivate.h>
#include <string.h>

/* The location of the cameras that fail to open, to look for in the messages */
#define CAMERA_BUS		3
#define CAMERA_ADDRESS		9

#define CAMERA_ID		"Aravis-Fake-UV1"
#define OTHER_CAMERA_ID		"Aravis-Fake-UV2"

/* What libusb_open () returns on Windows for a camera without a WinUSB driver on its USB3 Vision function
 * (NOT_FOUND), with an unsupported driver (NOT_SUPPORTED), already open (ACCESS) or for any other failure (IO) */
static const int open_errors[] = {
	LIBUSB_ERROR_NOT_FOUND,
	LIBUSB_ERROR_NOT_SUPPORTED,
	LIBUSB_ERROR_ACCESS,
	LIBUSB_ERROR_IO
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

/* @error is a device not found error, whose first line is @first_line, followed by the description of the failed open
 * of a camera added by add_failing_camera () */

static void
assert_not_found_error (const GError *error, const char *first_line, int open_error)
{
	char **lines;
	char *ids;
	char *location;

	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);

	ids = g_strdup_printf ("Failed to open USB3 Vision device %04x:%04x ",
			       FAKE_USB_CAMERA_VENDOR_ID, FAKE_USB_CAMERA_PRODUCT_ID);
	location = g_strdup_printf (" on bus %u, address %u: %s (%s)", CAMERA_BUS, CAMERA_ADDRESS,
				    libusb_error_name (open_error), arv_uv_interface_get_open_error_hint (open_error));

	lines = g_strsplit (error->message, "\n", -1);
	if (g_strv_length (lines) != 2 || strstr (lines[1], ids) == NULL || strstr (lines[1], location) == NULL)
		g_printerr ("Unexpected message:\n%s\n", error->message);
	g_assert_cmpuint (g_strv_length (lines), ==, 2);
	g_assert_cmpstr (lines[0], ==, first_line);
	g_assert_true (g_str_has_prefix (lines[1], ids));
	g_assert_nonnull (strstr (lines[1], location));

	g_strfreev (lines);
	g_free (ids);
	g_free (location);
}

static void
not_found_error_test (void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (open_errors); i++) {
		ArvDevice *device;
		GError *error = NULL;

		fakeuv_test_reset ();

		add_failing_camera ("UV1", open_errors[i]);

		/* The first device of the interface, after a discovery of its own */
		device = arv_open_device_on_interface ("USB3Vision", NULL, &error);
		g_assert_null (device);
		assert_not_found_error (error, "No device found on interface 'USB3Vision'", open_errors[i]);
		g_clear_error (&error);

		/* A device by name */
		device = arv_open_device_on_interface ("USB3Vision", CAMERA_ID, &error);
		g_assert_null (device);
		assert_not_found_error (error, "Device '" CAMERA_ID "' not found on interface 'USB3Vision'",
					open_errors[i]);
		g_clear_error (&error);

		/* As arv_open_device () */
		device = arv_open_device (CAMERA_ID, &error);
		g_assert_null (device);
		assert_not_found_error (error, "Device '" CAMERA_ID "' not found", open_errors[i]);
		g_clear_error (&error);

		device = arv_open_device (NULL, &error);
		g_assert_null (device);
		assert_not_found_error (error, "No supported device found", open_errors[i]);
		g_clear_error (&error);

		g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	}
}

/* arv_open_device_on_interface () opens devices through a disabled interface: its discovery tells why it found no
 * device. arv_open_device () only asks the enabled interfaces. */

static void
disabled_interface_test (void)
{
	ArvDevice *device;
	GError *error = NULL;

	add_failing_camera ("UV1", LIBUSB_ERROR_NOT_FOUND);

	arv_disable_interface ("USB3Vision");

	device = arv_open_device_on_interface ("USB3Vision", NULL, &error);
	g_assert_null (device);
	assert_not_found_error (error, "No device found on interface 'USB3Vision'", LIBUSB_ERROR_NOT_FOUND);
	g_clear_error (&error);

	device = arv_open_device_on_interface ("USB3Vision", CAMERA_ID, &error);
	g_assert_null (device);
	assert_not_found_error (error, "Device '" CAMERA_ID "' not found on interface 'USB3Vision'",
				LIBUSB_ERROR_NOT_FOUND);
	g_clear_error (&error);

	device = arv_open_device (CAMERA_ID, &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_cmpstr (error->message, ==, "Device '" CAMERA_ID "' not found");
	g_clear_error (&error);

	arv_enable_interface ("USB3Vision");
}

/* A camera that opens is opened, whatever the other ones. The error of a camera found by discovery but failing to
 * open, here claiming its data interface, comes from the device, without the list of the devices discovery could not
 * open. */

static void
found_camera_test (void)
{
	FakeUsbDevice *camera;
	ArvDevice *device;
	GError *error = NULL;
	guint8 data_interface = 2;

	add_failing_camera ("UV1", LIBUSB_ERROR_ACCESS);
	camera = fakeuv_add_camera ("UV2");

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);
	g_assert_cmpstr (arv_get_device_id (0), ==, OTHER_CAMERA_ID);

	device = arv_open_device_on_interface ("USB3Vision", NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_UV_DEVICE (device));
	g_object_unref (device);

	device = arv_open_device_on_interface ("USB3Vision", OTHER_CAMERA_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_UV_DEVICE (device));
	g_object_unref (device);

	fake_usb_device_inject_error (camera, FAKE_USB_CALL_CLAIM_INTERFACE, data_interface, LIBUSB_ERROR_BUSY, -1);

	device = arv_open_device_on_interface ("USB3Vision", OTHER_CAMERA_ID, &error);
	g_assert_null (device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR);
	g_assert_nonnull (strstr (error->message, "LIBUSB_ERROR_BUSY"));
	g_assert_null (strstr (error->message, "\n"));
	g_clear_error (&error);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
}

/* The generation of the device list, which a reader of the list compares before and after its queries, increases when
 * the cameras of the list change, whichever function updated the list: a camera comes or goes, or a camera at the same
 * index tells another physical identifier. Updates that find the same cameras, which another thread may run again and
 * again, leave it as it is. */

#define OTHER_GUID		"FakeUV-Other"

static guint64
assert_generation_increased (guint64 generation)
{
	guint64 new_generation = arv_get_device_list_generation ();

	g_assert_cmpuint (new_generation, >, generation);

	return new_generation;
}

static void
device_list_generation_test (void)
{
	FakeUsbDevice *camera;
	FakeUsbDevice *other_camera;
	ArvDevice *device;
	GError *error = NULL;
	guint64 generation;

	camera = fakeuv_add_camera ("UV1");

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);
	generation = arv_get_device_list_generation ();

	/* The same camera, also found by the update arv_open_device_on_interface () runs to open the first device */
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_device_list_generation (), ==, generation);

	device = arv_open_device_on_interface ("USB3Vision", NULL, &error);
	g_assert_no_error (error);
	g_object_unref (device);
	g_assert_cmpuint (arv_get_device_list_generation (), ==, generation);

	other_camera = fakeuv_add_camera ("UV2");
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 2);
	generation = assert_generation_increased (generation);

	arv_update_device_list ();
	g_assert_cmpuint (arv_get_device_list_generation (), ==, generation);

	fake_usb_device_set_string (camera, FAKE_USB_CAMERA_STRING_GUID, OTHER_GUID);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 2);
	g_assert_cmpstr (arv_get_device_id (0), ==, CAMERA_ID);
	g_assert_cmpstr (arv_get_device_physical_id (0), ==, OTHER_GUID);
	generation = assert_generation_increased (generation);

	fake_usb_device_unplug (other_camera);
	device = arv_open_device_on_interface ("USB3Vision", NULL, &error);
	g_assert_no_error (error);
	g_object_unref (device);
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);
	assert_generation_increased (generation);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
}

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvsystem/not-found-error", not_found_error_test);
	fakeuv_test_add ("/fakeuvsystem/disabled-interface", disabled_interface_test);
	fakeuv_test_add ("/fakeuvsystem/found-camera", found_camera_test);
	fakeuv_test_add ("/fakeuvsystem/device-list-generation", device_list_generation_test);

	return fakeuv_test_run ();
}
