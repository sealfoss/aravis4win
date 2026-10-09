/* SPDX-License-Identifier:Unlicense */

/*
 * GenTL consumer tests, against the mock GenTL producers of tests/gentlmock: opt-in interface, producer selection,
 * producer loading and its error reports, optional functions, missing strings in discovery, the release of a producer
 * still used by a device after arv_shutdown(), devices closed during a discovery, the stream: stop while the stream
 * thread waits, buffers pushed during the acquisition or refused by the producer, error reports, buffer status, and the
 * device events of -Devent=true builds. And Aravis's own GenTL producer, src/gentl, when built
 * (-Dgentl-producer=true).
 *
 * meson gives the paths of the mock producers and of the utilities in the ARV_TEST_xxx environment variables. The other
 * interfaces are disabled: no network discovery, no USB enumeration. Each test ends with arv_shutdown(), which
 * releases the producers, so that the next test loads them again, reading the mock options from the environment.
 */

#include <arv.h>
#include <arvgentlprivate.h>
#include <arvgvcpprivate.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <string.h>

#include "arvgentlmock.h"

#define MOCK_ID_SUFFIX		"-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL "-" ARV_GENTL_MOCK_SERIAL "0"
#define MOCK_DEVICE_ID		ARV_GENTL_MOCK_TL_VENDOR MOCK_ID_SUFFIX
#define MOCK_MINIMAL_DEVICE_ID	ARV_GENTL_MOCK_TL_VENDOR "Minimal" MOCK_ID_SUFFIX
#define MOCK_DEP_DEVICE_ID	ARV_GENTL_MOCK_TL_VENDOR "Dep" MOCK_ID_SUFFIX

/* Device id built with the producer file name, the GenTL interface id and the GenTL device id when the producer tells
 * no serial number */
#define MOCK_NO_SERIAL_ID_SUFFIX(interface_index) \
	"-" ARV_GENTL_MOCK_INTERFACE_ID_PREFIX interface_index "-" ARV_GENTL_MOCK_GENTL_DEVICE_ID "0"
#define MOCK_NO_SERIAL_DEVICE_ID	ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL \
					"-ArvGenTLMock" MOCK_NO_SERIAL_ID_SUFFIX ("0")
#define MOCK_NO_SERIAL_DEVICE_ID_IF1	ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL \
					"-ArvGenTLMock" MOCK_NO_SERIAL_ID_SUFFIX ("1")
/* Device id built from the producer file name, the GenTL interface id and the GenTL device id, when the producer
 * tells no string at all */
#define MOCK_NO_STRING_DEVICE_ID	"ArvGenTLMock---ArvGenTLMock" MOCK_NO_SERIAL_ID_SUFFIX ("0")
#define MOCK_NO_SERIAL_ID_PREFIX	ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL \
					"-ArvGenTLMock-"

#define MOCK_FRAME_PERIOD_US		"10000"
#define FRAME_TIMEOUT_US		(10 * G_USEC_PER_SEC)
#define SUBPROCESS_TIMEOUT_US		(60 * G_USEC_PER_SEC)

/* Discovery limited to a network interface which doesn't exist: no network traffic */
#define NO_GV_DISCOVERY_INTERFACE	"arv-test-no-such-interface"
#define NO_GV_DISCOVERY_OPTION		"--gv-discovery-interface=" NO_GV_DISCOVERY_INTERFACE

#define GET_TEST_PATH_OR_SKIP(path,variable) \
	G_STMT_START { \
		path = g_getenv (variable); \
		if (path == NULL) { \
			g_test_skip (variable " is not set, the test must be run by meson"); \
			return; \
		} \
	} G_STMT_END

/* Aravis's own GenTL producer, src/gentl, is only built with -Dgentl-producer=true: meson sets
 * ARV_TEST_GENTL_PRODUCER_NOT_BUILT instead of its path otherwise */
#define GET_PRODUCER_PATH_OR_SKIP(path) \
	G_STMT_START { \
		if (g_getenv ("ARV_TEST_GENTL_PRODUCER_NOT_BUILT") != NULL) { \
			g_test_skip ("Aravis's GenTL producer is not built: configure with -Dgentl-producer=true"); \
			return; \
		} \
		GET_TEST_PATH_OR_SKIP (path, "ARV_TEST_GENTL_PRODUCER"); \
	} G_STMT_END

static const char *mock_options[] = {
	"ARV_GENTL_MOCK_N_DEVICES",
	"ARV_GENTL_MOCK_N_INTERFACES",
	"ARV_GENTL_MOCK_MISSING_INFO",
	"ARV_GENTL_MOCK_EMPTY_INFO",
	"ARV_GENTL_MOCK_FAIL",
	"ARV_GENTL_MOCK_EVENT_KILL",
	"ARV_GENTL_MOCK_INCOMPLETE_EVERY",
	"ARV_GENTL_MOCK_FRAME_PERIOD_US",
	"ARV_GENTL_MOCK_BUF_ANNOUNCE_MIN",
	"ARV_GENTL_MOCK_N_PARTS",
	"ARV_GENTL_MOCK_CLOSE_DELAY_US",
	"ARV_GENTL_MOCK_DEVICE_EVENT_ID",
	"ARV_GENTL_MOCK_SIZE_EXCESS",
	"ARV_GENTL_MOCK_IMAGE_OFFSET"
};

static const char *
get_gentl_path_variable (void)
{
	return sizeof (void *) == 8 ? "GENICAM_GENTL64_PATH" : "GENICAM_GENTL32_PATH";
}

/* The producer path and the default mock options, without any interface selection */

static void
setup_mock_environment (const char *gentl_path)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (mock_options); i++)
		g_unsetenv (mock_options[i]);
	g_unsetenv ("ARV_GENTL_PRODUCERS");

	g_setenv ("ARV_GENTL_MOCK_FRAME_PERIOD_US", MOCK_FRAME_PERIOD_US, TRUE);

	if (gentl_path != NULL)
		g_setenv (get_gentl_path_variable (), gentl_path, TRUE);
	else
		g_unsetenv (get_gentl_path_variable ());
}

static void
setup_environment (const char *gentl_path)
{
	setup_mock_environment (gentl_path);

	/* GenTL devices only: no network discovery, no USB enumeration. The GenTL interface state is left to the
	 * test. */
	arv_disable_interface ("Fake");
	arv_disable_interface ("GigEVision");
#if ARAVIS_HAS_USB
	arv_disable_interface ("USB3Vision");
#endif
#if ARAVIS_HAS_V4L2
	arv_disable_interface ("V4L2");
#endif
}

static void
teardown_environment (void)
{
	arv_disable_interface ("GenTL");

	/* Releases the producers, closed unless a device still uses them */
	arv_shutdown ();
}

static int
find_device (const char *device_id)
{
	unsigned int n_devices = arv_get_n_devices ();
	unsigned int i;

	for (i = 0; i < n_devices; i++)
		if (g_strcmp0 (arv_get_device_id (i), device_id) == 0)
			return i;

	return -1;
}

static unsigned int
count_mock_devices (void)
{
	unsigned int n_devices = arv_get_n_devices ();
	unsigned int n_mock_devices = 0;
	unsigned int i;

	for (i = 0; i < n_devices; i++)
		if (g_str_has_prefix (arv_get_device_id (i), ARV_GENTL_MOCK_TL_VENDOR))
			n_mock_devices++;

	return n_mock_devices;
}

/* Control of a mock producer, reached through GModule: the library is shared with the GenTL consumer */

typedef struct {
	GModule *module;
	ArvGenTLMockGetCounter get_counter;
	ArvGenTLMockGetLastViolation get_last_violation;
	ArvGenTLMockSetOption set_option;
	ArvGenTLMockSetIds set_ids;
	ArvGenTLMockResetCounters reset_counters;
} MockControl;

static MockControl *
mock_control_new (const char *path)
{
	MockControl *control = g_new0 (MockControl, 1);

	control->module = g_module_open (path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
	if (control->module == NULL)
		g_error ("Failed to load '%s': %s", path, g_module_error ());

	g_assert_true (g_module_symbol (control->module, ARV_GENTL_MOCK_GET_COUNTER,
					(gpointer *) &control->get_counter));
	g_assert_true (g_module_symbol (control->module, ARV_GENTL_MOCK_GET_LAST_VIOLATION,
					(gpointer *) &control->get_last_violation));
	g_assert_true (g_module_symbol (control->module, ARV_GENTL_MOCK_SET_OPTION,
					(gpointer *) &control->set_option));
	g_assert_true (g_module_symbol (control->module, ARV_GENTL_MOCK_SET_IDS,
					(gpointer *) &control->set_ids));
	g_assert_true (g_module_symbol (control->module, ARV_GENTL_MOCK_RESET_COUNTERS,
					(gpointer *) &control->reset_counters));

	control->reset_counters ();

	return control;
}

static void
mock_control_free (MockControl *control)
{
	g_module_close (control->module);
	g_free (control);
}

static void
assert_no_violation (MockControl *control)
{
	/* Shows the last violation on failure */
	g_assert_cmpstr (control->get_counter ("violations") > 0 ? control->get_last_violation () : NULL, ==, NULL);
}

static void
check_image (ArvBuffer *buffer)
{
	const guint8 *data;
	size_t size = 0;
	size_t i;
	gboolean is_blank = TRUE;

	g_assert_true (ARV_IS_BUFFER (buffer));
	g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SUCCESS);
	g_assert_cmpint (arv_buffer_get_payload_type (buffer), ==, ARV_BUFFER_PAYLOAD_TYPE_IMAGE);
	g_assert_cmpint (arv_buffer_get_image_width (buffer), ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_cmpint (arv_buffer_get_image_height (buffer), ==, ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	g_assert_cmpint (arv_buffer_get_image_pixel_format (buffer), ==, ARV_PIXEL_FORMAT_MONO_8);

	/* Fake camera diagonal ramp */
	data = arv_buffer_get_image_data (buffer, &size);
	g_assert_nonnull (data);
	g_assert_cmpuint (size, ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	for (i = 0; i < size && is_blank; i++)
		is_blank = data[i] == 0;
	g_assert_false (is_blank);
}

static void
acquire_frames (ArvCamera *camera, guint n_frames, gboolean native_buffers)
{
	ArvStream *stream;
	GError *error = NULL;
	guint64 frame_id = 0;
	gint payload;
	guint i;

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_STREAM (stream));

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (payload, ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);

	if (native_buffers) {
		arv_stream_create_buffers (stream, 3, NULL, NULL, &error);
		g_assert_no_error (error);
	} else {
		for (i = 0; i < 3; i++)
			arv_stream_push_buffer (stream, arv_buffer_new_allocate (payload));
	}

	arv_camera_set_acquisition_mode (camera, ARV_ACQUISITION_MODE_CONTINUOUS, &error);
	g_assert_no_error (error);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	for (i = 0; i < n_frames; i++) {
		ArvBuffer *buffer;

		buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
		check_image (buffer);

		g_assert_cmpuint (arv_buffer_get_frame_id (buffer), >, frame_id);
		frame_id = arv_buffer_get_frame_id (buffer);

		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_object_unref (stream);
}

/* GenTL is not used by the global discovery unless enabled */

static void
opt_in_test (void)
{
	MockControl *control;
	const char *mock_path;
	int index;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);

	arv_update_device_list ();
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), <, 0);
	/* The producer is not even initialized */
	g_assert_cmpint (control->get_counter ("GCInitLib"), ==, 0);

	arv_enable_interface ("GenTL");
	arv_update_device_list ();

	index = find_device (MOCK_DEVICE_ID);
	g_assert_cmpint (index, >=, 0);
	g_assert_cmpstr (arv_get_device_vendor (index), ==, ARV_GENTL_MOCK_VENDOR);
	g_assert_cmpstr (arv_get_device_model (index), ==, ARV_GENTL_MOCK_MODEL);
	g_assert_cmpstr (arv_get_device_serial_nbr (index), ==, ARV_GENTL_MOCK_SERIAL "0");
	g_assert_cmpstr (arv_get_device_address (index), ==, ARV_GENTL_MOCK_TL_TYPE);
	g_assert_cmpstr (arv_get_device_protocol (index), ==, "Custom");
	g_assert_cmpint (control->get_counter ("GCInitLib"), ==, 1);

	teardown_environment ();

	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	g_assert_cmpint (control->get_counter ("initialized"), ==, 0);
	assert_no_violation (control);

	mock_control_free (control);
}

/* ARV_GENTL_ENABLE is read once, before the first change of the interface selection: these tests run in a
 * subprocess. */

static void
check_environment_flag (const char *value, gboolean is_enabled)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		/* Before any interface selection */
		g_setenv ("ARV_GENTL_ENABLE", value, TRUE);

		setup_environment (mock_path);
		arv_update_device_list ();

		if (is_enabled)
			g_assert_cmpint (find_device (MOCK_DEVICE_ID), >=, 0);
		else
			g_assert_cmpint (find_device (MOCK_DEVICE_ID), <, 0);

		arv_shutdown ();
		return;
	}

	/* No flag: G_TEST_SUBPROCESS_DEFAULT needs GLib 2.74 */
	g_test_trap_subprocess (NULL, SUBPROCESS_TIMEOUT_US, (GTestSubprocessFlags) 0);
	g_test_trap_assert_passed ();
}

static void
environment_enabled_test (void)
{
	check_environment_flag ("1", TRUE);
}

static void
environment_disabled_test (void)
{
	check_environment_flag ("off", FALSE);
}

/* arv_get_device_interface_id() applies ARV_GENTL_ENABLE like the other device list functions, even as the first of
 * them: for the same index, it tells the interface of the device arv_get_device_id() gives. The GenTL device list is
 * filled through the interface itself, which reads no interface selection. The device lists of the other interfaces
 * are never updated: no network discovery, no USB enumeration. */

static void
device_interface_id_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		ArvInterface *interface;

		/* Before any interface selection */
		g_setenv ("ARV_GENTL_ENABLE", "1", TRUE);
		setup_mock_environment (mock_path);

		interface = arv_get_interface_by_id ("GenTL");
		arv_interface_update_device_list (interface);
		g_assert_cmpuint (arv_interface_get_n_devices (interface), ==, 1);

		g_assert_cmpstr (arv_get_device_interface_id (0), ==, "GenTL");
		g_assert_cmpstr (arv_get_device_id (0), ==, MOCK_DEVICE_ID);
		g_assert_null (arv_get_device_interface_id (1));

		arv_shutdown ();
		return;
	}

	g_test_trap_subprocess (NULL, SUBPROCESS_TIMEOUT_US, (GTestSubprocessFlags) 0);
	g_test_trap_assert_passed ();
}

/* A disabled interface is still usable through arv_get_interface_by_id() */

static void
interface_by_id_test (void)
{
	ArvInterface *interface;
	ArvDevice *device;
	GError *error = NULL;
	const char *mock_path;
	gboolean is_found = FALSE;
	unsigned int i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	setup_environment (mock_path);

	device = arv_open_device (MOCK_DEVICE_ID, &error);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_null (device);
	g_clear_error (&error);

	interface = arv_get_interface_by_id ("GenTL");
	g_assert_true (ARV_IS_GENTL_INTERFACE (interface));

	arv_interface_update_device_list (interface);
	for (i = 0; i < arv_interface_get_n_devices (interface); i++)
		if (g_strcmp0 (arv_interface_get_device_id (interface, i), MOCK_DEVICE_ID) == 0)
			is_found = TRUE;
	g_assert_true (is_found);

	device = arv_interface_open_device (interface, MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_DEVICE (device));

	/* A fake camera register, read through the producer */
	g_assert_cmpint (arv_device_get_integer_feature_value (device, "Width", &error), ==,
			 ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_no_error (error);

	g_object_unref (device);

	teardown_environment ();
}

/* A producer failing the serial number query crashed the discovery */

static void
missing_serial_number_test (void)
{
	ArvDevice *device;
	GError *error = NULL;
	const char *mock_path;
	int index;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_MISSING_INFO", "DEVICE_INFO_SERIAL_NUMBER", TRUE);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();

	index = find_device (MOCK_NO_SERIAL_DEVICE_ID);
	g_assert_cmpint (index, >=, 0);
	g_assert_cmpstr (arv_get_device_serial_nbr (index), ==, "");
	g_assert_cmpstr (arv_get_device_vendor (index), ==, ARV_GENTL_MOCK_VENDOR);

	device = arv_open_device (MOCK_NO_SERIAL_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_DEVICE (device));
	g_object_unref (device);

	teardown_environment ();
}

static void
check_missing_strings (const char *option, const char *infos)
{
	ArvDevice *device;
	GError *error = NULL;
	const char *mock_path;
	int index;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	setup_environment (mock_path);
	g_setenv (option, infos, TRUE);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();

	index = find_device (MOCK_NO_STRING_DEVICE_ID);
	g_assert_cmpint (index, >=, 0);
	g_assert_cmpstr (arv_get_device_vendor (index), ==, "");
	g_assert_cmpstr (arv_get_device_model (index), ==, "");
	g_assert_cmpstr (arv_get_device_serial_nbr (index), ==, "");
	g_assert_cmpstr (arv_get_device_address (index), ==, "Custom");
	g_assert_cmpstr (arv_get_device_protocol (index), ==, "Custom");

	device = arv_open_device (MOCK_NO_STRING_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_DEVICE (device));
	g_assert_cmpint (arv_device_get_integer_feature_value (device, "Width", &error), ==,
			 ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_no_error (error);
	g_object_unref (device);

	teardown_environment ();
}

static void
missing_strings_test (void)
{
	check_missing_strings ("ARV_GENTL_MOCK_MISSING_INFO",
			       "TL_INFO_VENDOR,TL_INFO_TLTYPE,INTERFACE_INFO_DISPLAYNAME,INTERFACE_INFO_TLTYPE,"
			       "DEVICE_INFO_ID,DEVICE_INFO_VENDOR,DEVICE_INFO_MODEL,DEVICE_INFO_TLTYPE,"
			       "DEVICE_INFO_DISPLAYNAME,DEVICE_INFO_SERIAL_NUMBER,DEVICE_INFO_TIMESTAMP_FREQUENCY");
}

static void
empty_strings_test (void)
{
	check_missing_strings ("ARV_GENTL_MOCK_EMPTY_INFO",
			       "TL_INFO_VENDOR,TL_INFO_TLTYPE,INTERFACE_INFO_DISPLAYNAME,INTERFACE_INFO_TLTYPE,"
			       "DEVICE_INFO_ID,DEVICE_INFO_VENDOR,DEVICE_INFO_MODEL,DEVICE_INFO_TLTYPE,"
			       "DEVICE_INFO_DISPLAYNAME,DEVICE_INFO_SERIAL_NUMBER");
}

/* Serial number of the fake camera behind a device, read from its registers: it tells which mock camera the device
 * is, whether the producer tells the serial numbers or not */

static char *
dup_camera_serial_number (ArvDevice *device)
{
	char serial_number[ARV_GVBS_SERIAL_NUMBER_SIZE + 1] = {0};
	GError *error = NULL;

	arv_device_read_memory (device, ARV_GVBS_SERIAL_NUMBER_OFFSET, ARV_GVBS_SERIAL_NUMBER_SIZE, serial_number,
				&error);
	g_assert_no_error (error);

	return g_strdup (serial_number);
}

/* Ids of the listed mock devices, in the order of the device list */

static GPtrArray *
dup_mock_device_ids (void)
{
	GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
	unsigned int n_devices = arv_get_n_devices ();
	unsigned int i;

	for (i = 0; i < n_devices; i++)
		if (g_str_has_prefix (arv_get_device_id (i), ARV_GENTL_MOCK_TL_VENDOR))
			g_ptr_array_add (ids, g_strdup (arv_get_device_id (i)));

	return ids;
}

/* Opens each id in turn, and returns the serial numbers of the cameras reached */

static GPtrArray *
dup_reached_cameras (GPtrArray *ids)
{
	GPtrArray *serial_numbers = g_ptr_array_new_with_free_func (g_free);
	guint i;

	for (i = 0; i < ids->len; i++) {
		ArvDevice *device;
		GError *error = NULL;

		device = arv_open_device (g_ptr_array_index (ids, i), &error);
		g_assert_no_error (error);
		g_assert_true (ARV_IS_GENTL_DEVICE (device));
		g_ptr_array_add (serial_numbers, dup_camera_serial_number (device));
		g_object_unref (device);
	}

	return serial_numbers;
}

/* Two interfaces of a producer expose the same GenTL device id, with the same vendor and model: each listed device
 * must have its own id, which opens its own camera. The cameras of interfaces 0 and 1 are MOCK0 and MOCK0-IF1. */

static void
check_interfaces_with_same_device_id (gboolean has_serial_numbers)
{
	GPtrArray *ids;
	GPtrArray *cameras;
	const char *mock_path;
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_N_INTERFACES", "2", TRUE);
	if (!has_serial_numbers)
		g_setenv ("ARV_GENTL_MOCK_MISSING_INFO", "DEVICE_INFO_SERIAL_NUMBER", TRUE);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();

	ids = dup_mock_device_ids ();
	cameras = dup_reached_cameras (ids);
	for (i = 0; i < ids->len; i++)
		g_test_message ("Listed device %u: '%s' opens camera '%s'", i,
				(const char *) g_ptr_array_index (ids, i),
				(const char *) g_ptr_array_index (cameras, i));

	g_assert_cmpuint (ids->len, ==, 2);
	g_assert_cmpstr (g_ptr_array_index (ids, 0), !=, g_ptr_array_index (ids, 1));
	/* Listed interface by interface */
	g_assert_cmpstr (g_ptr_array_index (cameras, 0), ==, ARV_GENTL_MOCK_SERIAL "0");
	g_assert_cmpstr (g_ptr_array_index (cameras, 1), ==, ARV_GENTL_MOCK_SERIAL "0-IF1");

	if (!has_serial_numbers) {
		g_assert_cmpstr (g_ptr_array_index (ids, 0), ==, MOCK_NO_SERIAL_DEVICE_ID);
		g_assert_cmpstr (g_ptr_array_index (ids, 1), ==, MOCK_NO_SERIAL_DEVICE_ID_IF1);
		for (i = 0; i < ids->len; i++)
			g_assert_cmpstr (arv_get_device_serial_nbr (find_device (g_ptr_array_index (ids, i))), ==, "");
	}

	g_ptr_array_unref (cameras);
	g_ptr_array_unref (ids);

	teardown_environment ();
}

static void
interfaces_same_device_id_test (void)
{
	check_interfaces_with_same_device_id (TRUE);
}

static void
missing_serial_number_interfaces_test (void)
{
	check_interfaces_with_same_device_id (FALSE);
}

/* Two producers with the same transport layer vendor expose the same GenTL device id, with the same vendor and model:
 * a copy of the mock producer, under another name, which is another library. Each listed device must have its own
 * id, which opens a camera of its own producer. The fake cameras of both producers are alike: the producer of the
 * camera reached is told by their open device counter. */

static char *
copy_producer (const char *source_path, const char *directory, const char *basename)
{
	GFile *source;
	GFile *destination;
	GError *error = NULL;
	char *destination_path;

	destination_path = g_build_filename (directory, basename, NULL);

	source = g_file_new_for_path (source_path);
	destination = g_file_new_for_path (destination_path);
	g_file_copy (source, destination, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
	g_assert_no_error (error);

	g_object_unref (source);
	g_object_unref (destination);

	return destination_path;
}

static void
check_missing_serial_number_producers (const char *const basenames[2], const char *const interface_ids[2],
				      const char *const expected_ids[2])
{
	MockControl *controls[2];
	GPtrArray *ids;
	GError *error = NULL;
	const char *mock_path;
	char *directory;
	char *paths[2];
	char *gentl_path;
	guint producers[2] = {0, 0};
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	directory = g_dir_make_tmp ("arv-gentl-test-XXXXXX", &error);
	g_assert_no_error (error);
	for (i = 0; i < 2; i++) {
		paths[i] = basenames[i] != NULL ? copy_producer (mock_path, directory, basenames[i]) :
						g_strdup (mock_path);
		controls[i] = mock_control_new (paths[i]);
	}
	gentl_path = g_strjoin (G_SEARCHPATH_SEPARATOR_S, paths[0], paths[1], NULL);

	setup_environment (gentl_path);
	g_setenv ("ARV_GENTL_MOCK_MISSING_INFO", "DEVICE_INFO_SERIAL_NUMBER", TRUE);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();
	g_assert_cmpint (controls[0]->get_counter ("GCInitLib"), ==, 1);
	g_assert_cmpint (controls[1]->get_counter ("GCInitLib"), ==, 1);
	if (interface_ids[0] != NULL || interface_ids[1] != NULL) {
		for (i = 0; i < 2; i++)
			if (interface_ids[i] != NULL)
				g_assert_true (controls[i]->set_ids (0, interface_ids[i], NULL));
		arv_update_device_list ();
	}

	/* The producers are listed in an arbitrary order */
	ids = dup_mock_device_ids ();
	g_assert_cmpuint (ids->len, ==, 2);
	g_assert_cmpstr (g_ptr_array_index (ids, 0), !=, g_ptr_array_index (ids, 1));
	for (i = 0; i < ids->len; i++) {
		ArvDevice *device;

		device = arv_open_device (g_ptr_array_index (ids, i), &error);
		g_assert_no_error (error);
		g_assert_true (ARV_IS_GENTL_DEVICE (device));

		producers[i] = controls[0]->get_counter ("open-devices") == 1 ? 0 : 1;
		g_assert_cmpint (controls[producers[i]]->get_counter ("open-devices"), ==, 1);
		g_assert_cmpint (controls[1 - producers[i]]->get_counter ("open-devices"), ==, 0);
		g_assert_cmpstr (g_ptr_array_index (ids, i), ==, expected_ids[producers[i]]);

		g_test_message ("Listed device %u: '%s' opens the camera of producer %u", i,
				(const char *) g_ptr_array_index (ids, i), producers[i]);

		g_object_unref (device);
	}

	g_assert_cmpuint (producers[0], !=, producers[1]);

	g_ptr_array_unref (ids);

	teardown_environment ();

	for (i = 0; i < 2; i++) {
		assert_no_violation (controls[i]);
		mock_control_free (controls[i]);
		/* Best effort, the library may still be mapped */
		if (basenames[i] != NULL)
			g_unlink (paths[i]);
		g_free (paths[i]);
	}
	g_rmdir (directory);
	g_free (gentl_path);
	g_free (directory);
}

static void
missing_serial_number_producers_test (void)
{
	static const char *basenames[] = {NULL, "ArvGenTLMockCopy.cti"};
	static const char *interface_ids[] = {NULL, NULL};
	static const char *expected_ids[] = {
		MOCK_NO_SERIAL_DEVICE_ID,
		ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL
			"-ArvGenTLMockCopy" MOCK_NO_SERIAL_ID_SUFFIX ("0")
	};

	check_missing_serial_number_producers (basenames, interface_ids, expected_ids);
}

/* Joining Arv-Q/A0 and Arv/Q-A0 without escaping gave both cameras the same id. */
static void
missing_serial_number_producer_separators_test (void)
{
	static const char *basenames[] = {"Arv-Q.cti", "Arv.cti"};
	static const char *interface_ids[] = {"A0", "Q-A0"};
	static const char *expected_ids[] = {
		ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL
			"-Arv%2DQ-A0-MockDevice0",
		ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL
			"-Arv-Q%2DA0-MockDevice0"
	};

	check_missing_serial_number_producers (basenames, interface_ids, expected_ids);
}

typedef struct {
	const char *interface_ids[2];
	const char *device_ids[2];
	const char *expected_suffixes[2];
} MockIdentityPair;

static const MockIdentityPair encoded_identity_pairs[] = {
	/* Moving the separator between the interface and device used to preserve the canonical id. */
	{{"A-B", "A"}, {"C", "B-C"}, {"A%2DB-C", "A-B%2DC"}},
	/* A literal escape sequence must differ from the character it would encode. */
	{{"A%2DB", "A-B"}, {"Device%0", "Device%0"}, {"A%252DB-Device%250", "A%2DB-Device%250"}},
	/* The later control-character stripping must not merge distinct opaque ids. */
	{{"A\001B", "AB"}, {"D\037X", "D\037X"}, {"A%01B-D%1FX", "AB-D%1FX"}},
	/* Encode UTF-8 bytes, spaces and slashes; leave the safe ASCII punctuation unchanged. */
	{{"A_\303\251.~", "A_%C3%A9.~"}, {"D /\342\202\254", "D%20%2F%E2%82%AC"},
	 {"A_%C3%A9.~-D%20%2F%E2%82%AC", "A_%25C3%25A9.~-D%2520%252F%25E2%2582%25AC"}}
};

static void
missing_serial_number_encoded_ids_test (gconstpointer data)
{
	const MockIdentityPair *pair = data;
	MockControl *control;
	GPtrArray *ids;
	GPtrArray *cameras;
	const char *mock_path;
	guint i, j;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_N_INTERFACES", "2", TRUE);
	g_setenv ("ARV_GENTL_MOCK_MISSING_INFO", "DEVICE_INFO_SERIAL_NUMBER", TRUE);
	arv_enable_interface ("GenTL");
	arv_update_device_list ();
	for (i = 0; i < 2; i++)
		g_assert_true (control->set_ids (i, pair->interface_ids[i], pair->device_ids[i]));
	arv_update_device_list ();

	ids = dup_mock_device_ids ();
	g_assert_cmpuint (ids->len, ==, 2);
	g_assert_cmpstr (g_ptr_array_index (ids, 0), !=, g_ptr_array_index (ids, 1));
	cameras = dup_reached_cameras (ids);
	for (i = 0; i < 2; i++) {
		char *expected = g_strconcat (MOCK_NO_SERIAL_ID_PREFIX, pair->expected_suffixes[i], NULL);

		/* Aravis sorts the list by id, which can change the order of the interfaces after encoding. */
		for (j = 0; j < ids->len; j++)
			if (g_strcmp0 (g_ptr_array_index (ids, j), expected) == 0)
				break;
		g_assert_cmpuint (j, <, ids->len);
		g_assert_cmpstr (g_ptr_array_index (cameras, j), ==,
				i == 0 ? ARV_GENTL_MOCK_SERIAL "0" : ARV_GENTL_MOCK_SERIAL "0-IF1");
		g_assert_cmpstr (arv_get_device_serial_nbr (find_device (expected)), ==, "");
		g_test_message ("Listed '%s' opens camera '%s'", expected,
				(const char *) g_ptr_array_index (cameras, j));
		g_free (expected);
	}
	g_ptr_array_unref (cameras);
	g_ptr_array_unref (ids);
	teardown_environment ();
	assert_no_violation (control);
	mock_control_free (control);
}

/* A producer without the functions the consumer never calls, or can do without, is loaded and streams */

static void
optional_functions_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK_MINIMAL");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	arv_enable_interface ("GenTL");

	camera = arv_camera_new (MOCK_MINIMAL_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));

	acquire_frames (camera, 2, TRUE);

	g_object_unref (camera);

	teardown_environment ();

	g_assert_cmpint (control->get_counter ("frames"), >=, 2);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	assert_no_violation (control);

	mock_control_free (control);
}

/* Load errors are reported as warnings, naming the producer */

static void
missing_function_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK_BROKEN");

	setup_environment (mock_path);
	arv_enable_interface ("GenTL");

	g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*GenTL producer*ArvGenTLMockBroken.cti*not loaded*TLOpen*");
	arv_update_device_list ();
	g_test_assert_expected_messages ();

	g_assert_cmpuint (count_mock_devices (), ==, 0);

	teardown_environment ();
}

static void
invalid_producer_test (void)
{
	GError *error = NULL;
	char *directory;
	char *path;

	directory = g_dir_make_tmp ("arv-gentl-test-XXXXXX", &error);
	g_assert_no_error (error);
	path = g_build_filename (directory, "invalid.cti", NULL);
	g_file_set_contents (path, "Not a GenTL producer", -1, &error);
	g_assert_no_error (error);

	setup_environment (directory);
	arv_enable_interface ("GenTL");

	g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*GenTL producer*invalid.cti*not loaded*");
	arv_update_device_list ();
	g_test_assert_expected_messages ();

	teardown_environment ();

	g_unlink (path);
	g_rmdir (directory);
	g_free (path);
	g_free (directory);
}

#ifdef G_OS_WIN32

/* A producer is loaded with its own folder searched for its dependencies */

static void
copy_file (const char *source_path, const char *directory)
{
	GFile *source;
	GFile *destination;
	GError *error = NULL;
	char *basename;
	char *destination_path;

	basename = g_path_get_basename (source_path);
	destination_path = g_build_filename (directory, basename, NULL);

	source = g_file_new_for_path (source_path);
	destination = g_file_new_for_path (destination_path);
	g_file_copy (source, destination, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
	g_assert_no_error (error);

	g_object_unref (source);
	g_object_unref (destination);
	g_free (destination_path);
	g_free (basename);
}

static void
delete_file (const char *source_path, const char *directory)
{
	char *basename = g_path_get_basename (source_path);
	char *path = g_build_filename (directory, basename, NULL);

	/* Best effort, the library may still be mapped */
	g_unlink (path);

	g_free (path);
	g_free (basename);
}

static void
dependency_test (void)
{
	GPtrArray *path_entries;
	GError *error = NULL;
	const char *mock_path;
	const char *dependency_path;
	char *dependency_name;
	char *directory;
	char *current_directory;
	char *dependency_in_current_directory;
	char *old_path;
	char **entries;
	char *new_path;
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK_DEP");
	GET_TEST_PATH_OR_SKIP (dependency_path, "ARV_TEST_GENTL_MOCK_DEP_LIBRARY");

	dependency_name = g_path_get_basename (dependency_path);

	/* The current directory is in the default search path */
	current_directory = g_get_current_dir ();
	dependency_in_current_directory = g_build_filename (current_directory, dependency_name, NULL);
	if (g_file_test (dependency_in_current_directory, G_FILE_TEST_EXISTS)) {
		g_test_skip ("The producer dependency is in the current directory");
		g_free (dependency_in_current_directory);
		g_free (current_directory);
		g_free (dependency_name);
		return;
	}
	g_free (dependency_in_current_directory);
	g_free (current_directory);

	directory = g_dir_make_tmp ("arv-gentl-test-XXXXXX", &error);
	g_assert_no_error (error);
	copy_file (mock_path, directory);
	copy_file (dependency_path, directory);

	/* No directory of PATH must provide the dependency */
	old_path = g_strdup (g_getenv ("PATH"));
	path_entries = g_ptr_array_new_with_free_func (g_free);
	entries = g_strsplit (old_path != NULL ? old_path : "", G_SEARCHPATH_SEPARATOR_S, -1);
	for (i = 0; entries[i] != NULL; i++) {
		char *candidate = g_build_filename (entries[i], dependency_name, NULL);

		if (!g_file_test (candidate, G_FILE_TEST_EXISTS))
			g_ptr_array_add (path_entries, g_strdup (entries[i]));
		g_free (candidate);
	}
	g_ptr_array_add (path_entries, NULL);
	new_path = g_strjoinv (G_SEARCHPATH_SEPARATOR_S, (char **) path_entries->pdata);
	g_setenv ("PATH", new_path, TRUE);

	setup_environment (directory);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();
	g_assert_cmpint (find_device (MOCK_DEP_DEVICE_ID), >=, 0);

	teardown_environment ();

	if (old_path != NULL)
		g_setenv ("PATH", old_path, TRUE);

	delete_file (mock_path, directory);
	delete_file (dependency_path, directory);
	g_rmdir (directory);

	g_strfreev (entries);
	g_ptr_array_unref (path_entries);
	g_free (new_path);
	g_free (old_path);
	g_free (directory);
	g_free (dependency_name);
}

#endif

/* ARV_GENTL_PRODUCERS restricts the loaded producers */

static void
producer_selection_test (void)
{
	const char *mock_path;
	char *directory;
	char *selection;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	/* The directory of every mock producer, including the broken one */
	directory = g_path_get_dirname (mock_path);

	setup_environment (directory);
	arv_enable_interface ("GenTL");

	g_setenv ("ARV_GENTL_PRODUCERS", "ArvGenTLMockMinimal.cti", TRUE);
	arv_update_device_list ();
	g_assert_cmpuint (count_mock_devices (), ==, 1);
	g_assert_cmpint (find_device (MOCK_MINIMAL_DEVICE_ID), >=, 0);

	/* Producers loaded again by the next discovery */
	arv_shutdown ();

	/* Case insensitive match, wildcards */
	selection = g_strjoin (G_SEARCHPATH_SEPARATOR_S, "arvgentlmock.cti", "ArvGenTLMock?inimal.CTI", NULL);
	g_setenv ("ARV_GENTL_PRODUCERS", selection, TRUE);
	arv_update_device_list ();
	g_assert_cmpuint (count_mock_devices (), ==, 2);
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), >=, 0);
	g_assert_cmpint (find_device (MOCK_MINIMAL_DEVICE_ID), >=, 0);

	teardown_environment ();

	g_free (selection);
	g_free (directory);
}

/* Failures to open the device or the interface don't leave GenTL handles open */

static void
open_failure_test (void)
{
	MockControl *control;
	ArvDevice *device;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);

	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FAIL", "IFOpenDevice", TRUE);
	arv_enable_interface ("GenTL");

	device = arv_open_device (MOCK_DEVICE_ID, &error);
	g_assert_null (device);
	g_assert_nonnull (error);
	g_clear_error (&error);

	g_assert_cmpint (control->get_counter ("open-interfaces"), ==, 0);
	g_assert_cmpint (control->get_counter ("open-tl"), ==, 0);
	assert_no_violation (control);

	teardown_environment ();
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	assert_no_violation (control);

	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FAIL", "TLOpenInterface", TRUE);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), <, 0);
	g_assert_cmpint (control->get_counter ("open-tl"), ==, 0);

	teardown_environment ();
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 2);
	assert_no_violation (control);

	mock_control_free (control);
}

/* arv_shutdown() with a live GenTL camera crashed. The producer must stay loaded until the camera is destroyed, then be
 * closed in order. */

static void
shutdown_live_camera_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	arv_enable_interface ("GenTL");

	camera = arv_camera_new (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));

	arv_camera_set_integer (camera, "Width", 256, &error);
	g_assert_no_error (error);

	arv_disable_interface ("GenTL");
	arv_shutdown ();

	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 0);
	g_assert_cmpint (control->get_counter ("initialized"), ==, 1);

	/* Still usable */
	g_assert_cmpint (arv_camera_get_integer (camera, "Width", &error), ==, 256);
	g_assert_no_error (error);

	g_clear_object (&camera);

	g_assert_cmpint (control->get_counter ("DevClose"), ==, 1);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	g_assert_cmpint (control->get_counter ("initialized"), ==, 0);
	assert_no_violation (control);

	mock_control_free (control);
}

static void
shutdown_live_stream_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	arv_enable_interface ("GenTL");

	camera = arv_camera_new (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	arv_stream_create_buffers (stream, 3, NULL, NULL, &error);
	g_assert_no_error (error);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
	check_image (buffer);

	arv_disable_interface ("GenTL");
	arv_shutdown ();

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_clear_object (&stream);
	g_clear_object (&camera);

	/* The popped buffer keeps its data stream, its device and the producer */
	g_assert_cmpint (control->get_counter ("open-streams"), ==, 1);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 0);

	g_clear_object (&buffer);

	g_assert_cmpint (control->get_counter ("DSClose"), ==, 1);
	g_assert_cmpint (control->get_counter ("DevClose"), ==, 1);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	assert_no_violation (control);

	mock_control_free (control);
}

/* Happy path, with buffers allocated by the producer or by Aravis */

static void
acquisition_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	arv_enable_interface ("GenTL");

	camera = arv_camera_new (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (arv_camera_is_gentl_device (camera));

	acquire_frames (camera, 3, TRUE);
	g_assert_cmpint (control->get_counter ("DSAllocAndAnnounceBuffer"), ==, 3);

	acquire_frames (camera, 3, FALSE);
	g_assert_cmpint (control->get_counter ("DSAnnounceBuffer"), ==, 3);

	g_assert_cmpint (control->get_counter ("frames"), >=, 6);

	g_object_unref (camera);

	teardown_environment ();

	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	assert_no_violation (control);

	mock_control_free (control);
}

/* The stream: stops, buffers pushed during the acquisition, error reports, buffer status, and the buffers flushed by a
 * stop. The tests which hang with the former stream code run in a subprocess, with a timeout. */

#define STREAM_SUBPROCESS_TIMEOUT_US	(30 * G_USEC_PER_SEC)
/* Far longer than the wait timeout of the stream thread */
#define STOP_DURATION_MAX_US		(2 * G_USEC_PER_SEC)
/* No frame during a test */
#define MOCK_FRAME_PERIOD_NEVER_US	"100000000"
#define N_STREAM_BUFFERS		3

static void
wait_for_counter (MockControl *control, const char *name, int value)
{
	gint64 end_time = g_get_monotonic_time () + FRAME_TIMEOUT_US;

	while (control->get_counter (name) < value && g_get_monotonic_time () < end_time)
		g_usleep (1000);

	g_assert_cmpint (control->get_counter (name), >=, value);
}

static ArvCamera *
open_mock_camera (void)
{
	ArvCamera *camera;
	GError *error = NULL;

	arv_enable_interface ("GenTL");

	camera = arv_camera_new (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));

	arv_camera_set_acquisition_mode (camera, ARV_ACQUISITION_MODE_CONTINUOUS, &error);
	g_assert_no_error (error);

	return camera;
}

static ArvStream *
create_mock_stream (ArvCamera *camera, guint n_native_buffers)
{
	ArvStream *stream;
	GError *error = NULL;

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_STREAM (stream));

	if (n_native_buffers > 0) {
		g_assert_true (arv_stream_create_buffers (stream, n_native_buffers, NULL, NULL, &error));
		g_assert_no_error (error);
	}

	return stream;
}

static void
start_acquisition (ArvCamera *camera)
{
	GError *error = NULL;

	g_assert_true (arv_camera_start_acquisition (camera, &error));
	g_assert_no_error (error);
}

static gint64
stop_acquisition (ArvCamera *camera)
{
	GError *error = NULL;
	gint64 start_time = g_get_monotonic_time ();

	g_assert_true (arv_camera_stop_acquisition (camera, &error));
	g_assert_no_error (error);

	return g_get_monotonic_time () - start_time;
}

/* Pops a complete frame, and pushes the buffer back */

static void
cycle_frame (ArvStream *stream)
{
	ArvBuffer *buffer;

	buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
	check_image (buffer);
	arv_stream_push_buffer (stream, buffer);
}

/* Pops buffers until @wanted, pushing back the others, which must be complete frames */

static void
pop_until (ArvStream *stream, ArvBuffer *wanted)
{
	ArvBuffer *buffer = NULL;
	guint i;

	for (i = 0; i < 100 && buffer != wanted; i++) {
		buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
		g_assert_true (ARV_IS_BUFFER (buffer));

		if (buffer != wanted) {
			check_image (buffer);
			arv_stream_push_buffer (stream, buffer);
		}
	}

	g_assert_true (buffer == wanted);
}

/* Pops buffers until a complete frame, pushing back the others */

static void
pop_complete_frame (ArvStream *stream)
{
	ArvBuffer *buffer = NULL;
	guint i;

	for (i = 0; i < 100; i++) {
		buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
		g_assert_true (ARV_IS_BUFFER (buffer));
		if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS)
			break;
		arv_stream_push_buffer (stream, buffer);
	}

	check_image (buffer);
	arv_stream_push_buffer (stream, buffer);
}

static void
assert_owned_buffers (ArvStream *stream, gint n_input, gint n_output, gint n_filling)
{
	gint n_input_buffers, n_output_buffers, n_buffer_filling;

	arv_stream_get_n_owned_buffers (stream, &n_input_buffers, &n_output_buffers, &n_buffer_filling);
	g_assert_cmpint (n_input_buffers, ==, n_input);
	g_assert_cmpint (n_output_buffers, ==, n_output);
	g_assert_cmpint (n_buffer_filling, ==, n_filling);
}

static void
run_stream_subprocess (void)
{
	g_test_trap_subprocess (NULL, STREAM_SUBPROCESS_TIMEOUT_US, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed ();
}

/* A stop while the stream thread waited in EventGetData hung, the wait having no timeout. The mock ignores EventKill
 * and no frame comes: only a finite wait ends the wait. */

static void
stop_blocked_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ArvCamera *camera;
		ArvStream *stream;
		gint64 max_duration = 0;
		guint i;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);
		g_setenv ("ARV_GENTL_MOCK_EVENT_KILL", "ignore", TRUE);
		g_setenv ("ARV_GENTL_MOCK_FRAME_PERIOD_US", MOCK_FRAME_PERIOD_NEVER_US, TRUE);

		camera = open_mock_camera ();
		stream = create_mock_stream (camera, N_STREAM_BUFFERS);

		for (i = 0; i < 50; i++) {
			gint64 duration;

			start_acquisition (camera);

			/* The stream thread waits for a frame */
			wait_for_counter (control, "new-buffer-waiters", 1);

			duration = stop_acquisition (camera);
			max_duration = MAX (max_duration, duration);
		}

		g_test_message ("Longest stop: %" G_GINT64_FORMAT " us", max_duration);
		g_assert_cmpint (max_duration, <, STOP_DURATION_MAX_US);

		g_assert_cmpint (control->get_counter ("frames"), ==, 0);
		g_assert_cmpint (control->get_counter ("new-buffer-waiters"), ==, 0);
		g_assert_cmpint (control->get_counter ("new-buffer-events"), ==, 0);
		g_assert_cmpint (control->get_counter ("kills-unregistered"), ==, 0);

		g_object_unref (stream);
		g_object_unref (camera);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
}

/* The same, racing: an EventKill sent while the stream thread doesn't wait is lost, or reached an unregistered event */

static void
stop_race_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ArvCamera *camera;
		gint64 max_duration = 0;
		guint i;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);
		g_setenv ("ARV_GENTL_MOCK_EVENT_KILL", "waiters", TRUE);
		g_setenv ("ARV_GENTL_MOCK_FRAME_PERIOD_US", "1000", TRUE);

		camera = open_mock_camera ();

		for (i = 0; i < 50; i++) {
			ArvStream *stream = create_mock_stream (camera, N_STREAM_BUFFERS);
			gint64 duration;

			start_acquisition (camera);
			cycle_frame (stream);
			duration = stop_acquisition (camera);
			max_duration = MAX (max_duration, duration);

			g_object_unref (stream);
		}

		g_test_message ("Longest stop: %" G_GINT64_FORMAT " us", max_duration);
		g_assert_cmpint (max_duration, <, STOP_DURATION_MAX_US);

		g_assert_cmpint (control->get_counter ("new-buffer-events"), ==, 0);
		g_assert_cmpint (control->get_counter ("kills-unregistered"), ==, 0);
		g_assert_cmpint (control->get_counter ("open-streams"), ==, 0);
		g_assert_cmpint (control->get_counter ("announced-buffers"), ==, 0);

		g_object_unref (camera);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
}

/* A buffer pushed after the acquisition start was never announced. The stream thread pushed it back to the input
 * queue in a busy loop, which also prevented the stop. */

static void
late_buffer_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ArvCamera *camera;
		ArvStream *stream;
		ArvBuffer *late_buffer;
		ArvBuffer *held_buffers[N_STREAM_BUFFERS];
		GError *error = NULL;
		gint64 end_time;
		gint payload;
		gint n_input, n_output, n_filling;
		int n_frames, n_waits, n_queued;
		guint i;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);

		camera = open_mock_camera ();
		payload = arv_camera_get_payload (camera, &error);
		g_assert_no_error (error);

		stream = create_mock_stream (camera, N_STREAM_BUFFERS - 1);
		start_acquisition (camera);
		cycle_frame (stream);

		/* Announced by the stream thread, then filled */
		late_buffer = arv_buffer_new_allocate (payload);
		arv_stream_push_buffer (stream, late_buffer);
		pop_until (stream, late_buffer);
		check_image (late_buffer);
		arv_stream_push_buffer (stream, late_buffer);
		g_assert_cmpint (control->get_counter ("DSAnnounceBuffer"), ==, 1);

		/* While streaming, every wait or queueing of the stream thread is due to a frame, or to a wait timeout */
		n_frames = control->get_counter ("frames");
		n_waits = control->get_counter ("new-buffer-waits");
		n_queued = control->get_counter ("DSQueueBuffer");
		end_time = g_get_monotonic_time () + G_USEC_PER_SEC / 2;
		while (g_get_monotonic_time () < end_time)
			cycle_frame (stream);
		n_frames = control->get_counter ("frames") - n_frames;
		n_waits = control->get_counter ("new-buffer-waits") - n_waits;
		n_queued = control->get_counter ("DSQueueBuffer") - n_queued;
		g_test_message ("Streaming 500 ms: %d frames, %d waits, %d queued buffers", n_frames, n_waits, n_queued);
		g_assert_cmpint (n_frames, >, 0);
		g_assert_cmpint (n_waits, <=, 2 * n_frames + 20);
		g_assert_cmpint (n_queued, <=, n_frames + 2 * N_STREAM_BUFFERS);
		g_assert_cmpint (control->get_counter ("DSAnnounceBuffer"), ==, 1);

		/* With every buffer held by the application, the stream thread has nothing to queue, and waits for one */
		for (i = 0; i < N_STREAM_BUFFERS; i++) {
			held_buffers[i] = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
			check_image (held_buffers[i]);
		}
		n_waits = control->get_counter ("new-buffer-waits");
		n_queued = control->get_counter ("DSQueueBuffer");
		g_usleep (G_USEC_PER_SEC / 2);
		n_waits = control->get_counter ("new-buffer-waits") - n_waits;
		n_queued = control->get_counter ("DSQueueBuffer") - n_queued;
		g_test_message ("Holding the buffers 500 ms: %d waits, %d queued buffers", n_waits, n_queued);
		g_assert_cmpint (n_queued, ==, 0);
		g_assert_cmpint (n_waits, <=, 25);
		/* Every buffer is held by the application: none in the queues, none filling, while the stream thread
		 * waits on the input queue */
		arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, &n_filling);
		g_assert_cmpint (n_input, ==, 0);
		g_assert_cmpint (n_output, ==, 0);
		g_assert_cmpint (n_filling, ==, 0);

		/* Back to the stream, the buffers are filled again */
		for (i = 0; i < N_STREAM_BUFFERS; i++)
			arv_stream_push_buffer (stream, held_buffers[i]);
		cycle_frame (stream);

		g_assert_cmpint (stop_acquisition (camera), <, STOP_DURATION_MAX_US);

		g_object_unref (stream);
		g_object_unref (camera);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
}

/* No busy loop either when EventGetData fails */

static void
wait_error_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	const char *mock_path;
	int n_waits;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);

	camera = open_mock_camera ();
	stream = create_mock_stream (camera, N_STREAM_BUFFERS);
	start_acquisition (camera);
	cycle_frame (stream);

	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "EventGetData"));
	n_waits = control->get_counter ("new-buffer-waits");
	g_usleep (G_USEC_PER_SEC / 2);
	n_waits = control->get_counter ("new-buffer-waits") - n_waits;
	g_test_message ("EventGetData failing 500 ms: %d waits", n_waits);
	g_assert_cmpint (n_waits, >, 0);
	g_assert_cmpint (n_waits, <=, 25);

	/* Frames again once EventGetData works */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
	cycle_frame (stream);

	g_assert_cmpint (stop_acquisition (camera), <, STOP_DURATION_MAX_US);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

/* Failures to open the data stream, to announce a buffer or to start the acquisition were swallowed: no error, TRUE
 * returned by the acquisition start, and no frame */

static void
stream_open_failure_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FAIL", "DevOpenDataStream", TRUE);

	camera = open_mock_camera ();

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR);
	g_assert_nonnull (strstr (error->message, "DevOpenDataStream"));
	g_assert_null (stream);
	g_clear_error (&error);

	/* No data stream function called with an invalid handle */
	assert_no_violation (control);
	g_assert_cmpint (control->get_counter ("open-streams"), ==, 0);

	/* Nothing to start */
	g_assert_false (arv_camera_start_acquisition (camera, &error));
	g_assert_nonnull (error);
	g_clear_error (&error);

	/* Once the producer accepts */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
	stream = create_mock_stream (camera, N_STREAM_BUFFERS);
	start_acquisition (camera);
	cycle_frame (stream);
	stop_acquisition (camera);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

static void
announce_failure_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *late_buffer;
	GError *error = NULL;
	const char *mock_path;
	gint payload;
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FAIL", "DSAnnounceBuffer", TRUE);

	camera = open_mock_camera ();
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	/* Buffers announced by the acquisition start */
	stream = create_mock_stream (camera, 0);
	for (i = 0; i < N_STREAM_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new_allocate (payload));

	g_assert_false (arv_camera_start_acquisition (camera, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR);
	g_assert_nonnull (strstr (error->message, "DSAnnounceBuffer"));
	g_clear_error (&error);

	/* Not started, the buffers back in the input queue */
	g_assert_cmpint (control->get_counter ("DSStartAcquisition"), ==, 0);
	g_assert_cmpint (control->get_counter ("new-buffer-events"), ==, 0);
	assert_owned_buffers (stream, N_STREAM_BUFFERS, 0, 0);

	/* Nothing to stop */
	stop_acquisition (camera);

	/* Buffers allocated by the producer */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "DSAllocAndAnnounceBuffer"));
	g_assert_false (arv_stream_create_buffers (stream, 2, NULL, NULL, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR);
	g_assert_nonnull (strstr (error->message, "DSAllocAndAnnounceBuffer"));
	g_clear_error (&error);

	/* A buffer pushed after the acquisition start and refused by the producer comes back aborted, instead of being
	 * pushed back to the input queue */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
	arv_stream_delete_buffers (stream);
	g_assert_true (arv_stream_create_buffers (stream, N_STREAM_BUFFERS, NULL, NULL, &error));
	g_assert_no_error (error);
	start_acquisition (camera);
	cycle_frame (stream);

	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "DSAnnounceBuffer"));
	late_buffer = arv_buffer_new_allocate (payload);
	arv_stream_push_buffer (stream, late_buffer);
	pop_until (stream, late_buffer);
	g_assert_cmpint (arv_buffer_get_status (late_buffer), ==, ARV_BUFFER_STATUS_ABORTED);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 1);
	g_object_unref (late_buffer);

	/* The acquisition goes on */
	cycle_frame (stream);
	stop_acquisition (camera);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

static void
start_failure_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FAIL", "DSStartAcquisition", TRUE);

	camera = open_mock_camera ();
	stream = create_mock_stream (camera, N_STREAM_BUFFERS);

	g_assert_false (arv_camera_start_acquisition (camera, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR);
	g_assert_nonnull (strstr (error->message, "DSStartAcquisition"));
	g_clear_error (&error);

	/* Left as before the start */
	g_assert_cmpint (control->get_counter ("new-buffer-events"), ==, 0);
	assert_owned_buffers (stream, N_STREAM_BUFFERS, 0, 0);
	stop_acquisition (camera);

	/* The registration of the new buffer event */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "GCRegisterEvent"));
	g_assert_false (arv_stream_start_acquisition (stream, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR);
	g_assert_nonnull (strstr (error->message, "GCRegisterEvent"));
	g_clear_error (&error);
	assert_owned_buffers (stream, N_STREAM_BUFFERS, 0, 0);

	/* Started once the producer accepts */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
	start_acquisition (camera);
	cycle_frame (stream);
	stop_acquisition (camera);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

/* An incomplete frame was reported as a success */

static void
incomplete_frame_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *small_buffer;
	GError *error = NULL;
	const char *mock_path;
	gint payload;
	guint n_complete = 0;
	guint n_incomplete = 0;
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_INCOMPLETE_EVERY", "2", TRUE);

	camera = open_mock_camera ();
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	stream = create_mock_stream (camera, N_STREAM_BUFFERS);
	start_acquisition (camera);

	/* Every second frame is half filled */
	for (i = 0; i < 6; i++) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);

		g_assert_true (ARV_IS_BUFFER (buffer));
		if (arv_buffer_get_frame_id (buffer) % 2 == 0) {
			g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_MISSING_PACKETS);
			n_incomplete++;
		} else {
			check_image (buffer);
			n_complete++;
		}
		arv_stream_push_buffer (stream, buffer);
	}
	g_assert_cmpuint (n_incomplete, ==, 3);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), >=, n_incomplete);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_completed_buffers"), >=, n_complete);

	/* A frame larger than the buffer */
	small_buffer = arv_buffer_new_allocate (payload / 2);
	arv_stream_push_buffer (stream, small_buffer);
	for (i = 0; i < 100; i++) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);

		g_assert_true (ARV_IS_BUFFER (buffer));
		if (buffer == small_buffer)
			break;
		arv_stream_push_buffer (stream, buffer);
	}
	g_assert_cmpuint (i, <, 100);
	g_assert_cmpint (arv_buffer_get_status (small_buffer), ==, ARV_BUFFER_STATUS_SIZE_MISMATCH);
	g_object_unref (small_buffer);

	stop_acquisition (camera);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

/* A producer telling sizes larger than the buffer, without the flag of the data larger than the buffer, made a
 * successful buffer whose data, as arv_buffer_get_data() and arv_buffer_get_part_data() gave it, and as aravissrc
 * lends it, extended past the buffer memory. With single part buffers, and with the multi-part functions. */

#define MOCK_SIZE_EXCESS	"4096"

static void
check_oversized_buffer (ArvBuffer *buffer, size_t buffer_size)
{
	const guint8 *data;
	const guint8 *part_data;
	size_t size = 0;
	size_t part_size = 0;

	g_assert_true (ARV_IS_BUFFER (buffer));
	g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SIZE_MISMATCH);

	data = arv_buffer_get_data (buffer, &size);
	g_assert_nonnull (data);
	g_assert_cmpuint (size, ==, buffer_size);

	g_assert_cmpuint (arv_buffer_get_n_parts (buffer), ==, 1);
	part_data = arv_buffer_get_part_data (buffer, 0, &part_size);
	g_assert_true (part_data >= data && part_data <= data + buffer_size);
	g_assert_cmpuint (part_size, <=, buffer_size - (size_t) (part_data - data));
}

static void
oversized_frame_test (void)
{
	static const char *n_parts_options[] = {"none", "1"};
	const char *mock_path;
	guint i, j;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	for (i = 0; i < G_N_ELEMENTS (n_parts_options); i++) {
		MockControl *control;
		ArvCamera *camera;
		ArvStream *stream;
		GError *error = NULL;
		gint payload;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);
		g_setenv ("ARV_GENTL_MOCK_N_PARTS", n_parts_options[i], TRUE);
		g_setenv ("ARV_GENTL_MOCK_SIZE_EXCESS", MOCK_SIZE_EXCESS, TRUE);

		camera = open_mock_camera ();
		payload = arv_camera_get_payload (camera, &error);
		g_assert_no_error (error);

		/* Buffers of the payload size, allocated by the producer, and by Aravis */
		stream = create_mock_stream (camera, N_STREAM_BUFFERS);
		arv_stream_push_buffer (stream, arv_buffer_new_allocate (payload));
		start_acquisition (camera);

		for (j = 0; j < 2 * (N_STREAM_BUFFERS + 1); j++) {
			ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);

			check_oversized_buffer (buffer, payload);
			arv_stream_push_buffer (stream, buffer);
		}
		g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_completed_buffers"), ==, 0);
		g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), >=,
				  2 * (N_STREAM_BUFFERS + 1));

		/* Complete frames once the producer tells the actual sizes */
		g_assert_true (control->set_option ("ARV_GENTL_MOCK_SIZE_EXCESS", NULL));
		pop_complete_frame (stream);

		stop_acquisition (camera);

		g_object_unref (stream);
		g_object_unref (camera);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
	}
}

/* A producer telling an image offset, the data before the image being for example chunk data: the data sizes
 * (BUFFER_INFO_DATA_SIZE, BUFFER_INFO_SIZE_FILLED) count the data from the start of the buffer, these bytes included.
 * The part built from the buffer informations, without the multi-part functions, started at the offset but was as
 * large as the whole data: a complete frame filling its buffer came back as a SIZE_MISMATCH failure. The camera tells
 * a payload size counting the data before the image, as a camera whose payload starts with chunk data. With the zero
 * offset as well, and with the multi-part functions, which describe the image alone. */

#define MOCK_IMAGE_OFFSET		64
#define FAKE_PAYLOAD_SIZE_FORMULA	"WIDTH * HEIGHT * ((PIXELFORMAT>>16)&amp;0xFF) / 8"

/* Writes the GenICam description of the fake camera with a payload size larger by @excess bytes. Returns the path of
 * the file. */

static char *
write_genicam_file_with_payload_excess (guint excess)
{
	ArvFakeCamera *camera;
	const char *xml;
	const char *formula;
	char *contents;
	char *path = NULL;
	GError *error = NULL;
	size_t size = 0;
	int fd;

	camera = arv_fake_camera_new ("GENICAM0");
	xml = arv_fake_camera_get_genicam_xml (camera, &size);
	g_assert_nonnull (xml);

	formula = g_strstr_len (xml, size, FAKE_PAYLOAD_SIZE_FORMULA);
	g_assert_nonnull (formula);
	contents = g_strdup_printf ("%.*s" FAKE_PAYLOAD_SIZE_FORMULA " + %u%s", (int) (formula - xml), xml, excess,
				    formula + strlen (FAKE_PAYLOAD_SIZE_FORMULA));
	g_object_unref (camera);

	fd = g_file_open_tmp ("arv-gentl-test-XXXXXX.xml", &path, &error);
	g_assert_no_error (error);
	g_close (fd, NULL);
	g_assert_true (g_file_set_contents (path, contents, -1, &error));
	g_assert_no_error (error);
	g_free (contents);

	return path;
}

static void
check_offset_image (ArvBuffer *buffer, size_t image_offset)
{
	const guint8 *data;
	const guint8 *image;
	size_t size = 0;
	size_t image_size = 0;
	size_t i;
	guint x, y;

	g_assert_true (ARV_IS_BUFFER (buffer));
	g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SUCCESS);
	g_assert_cmpint (arv_buffer_get_payload_type (buffer), ==, ARV_BUFFER_PAYLOAD_TYPE_IMAGE);
	g_assert_cmpint (arv_buffer_get_image_width (buffer), ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_cmpint (arv_buffer_get_image_height (buffer), ==, ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	g_assert_cmpint (arv_buffer_get_image_pixel_format (buffer), ==, ARV_PIXEL_FORMAT_MONO_8);

	/* The data, from the start of the buffer */
	data = arv_buffer_get_data (buffer, &size);
	g_assert_nonnull (data);
	g_assert_cmpuint (size, ==, image_offset + ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	for (i = 0; i < image_offset; i++)
		g_assert_cmpuint (data[i], ==, ARV_GENTL_MOCK_IMAGE_OFFSET_FILL);

	/* The image alone, the diagonal ramp of the fake camera */
	g_assert_cmpuint (arv_buffer_get_n_parts (buffer), ==, 1);
	image = arv_buffer_get_image_data (buffer, &image_size);
	g_assert_true (image == data + image_offset);
	g_assert_cmpuint (image_size, ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	for (y = 0; y < ARV_FAKE_CAMERA_HEIGHT_DEFAULT; y++)
		for (x = 0; x < ARV_FAKE_CAMERA_WIDTH_DEFAULT; x++)
			if (image[y * ARV_FAKE_CAMERA_WIDTH_DEFAULT + x] != (x + y + image[0]) % 255)
				g_assert_cmpuint (image[y * ARV_FAKE_CAMERA_WIDTH_DEFAULT + x], ==,
						  (x + y + image[0]) % 255);
}

static void
image_offset_test (void)
{
	static const char *n_parts_options[] = {"none", "1"};
	static const guint image_offsets[] = {0, MOCK_IMAGE_OFFSET};
	/* Sizes of the buffers allocated by Aravis, beyond the payload size */
	static const guint buffer_excesses[] = {0, MOCK_IMAGE_OFFSET / 2, 4096};
	const char *mock_path;
	guint i, j, k;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	for (i = 0; i < G_N_ELEMENTS (image_offsets); i++) {
		char *image_offset = g_strdup_printf ("%u", image_offsets[i]);

		for (j = 0; j < G_N_ELEMENTS (n_parts_options); j++) {
			MockControl *control;
			ArvCamera *camera;
			ArvStream *stream;
			GError *error = NULL;
			char *genicam_path;
			gint payload;

			g_test_message ("Image offset %u, parts: %s", image_offsets[i], n_parts_options[j]);

			control = mock_control_new (mock_path);
			setup_environment (mock_path);
			g_setenv ("ARV_GENTL_MOCK_N_PARTS", n_parts_options[j], TRUE);
			g_setenv ("ARV_GENTL_MOCK_IMAGE_OFFSET", image_offset, TRUE);

			/* Read by the fake camera of the mock, created by IFOpenDevice */
			genicam_path = write_genicam_file_with_payload_excess (image_offsets[i]);
			arv_set_fake_camera_genicam_filename (genicam_path);
			camera = open_mock_camera ();
			arv_set_fake_camera_genicam_filename (NULL);
			g_remove (genicam_path);
			g_free (genicam_path);

			payload = arv_camera_get_payload (camera, &error);
			g_assert_no_error (error);
			g_assert_cmpint (payload, ==, image_offsets[i] +
					 ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);

			/* Buffers of the payload size, which the frames fill, allocated by the producer, and by Aravis.
			 * And larger ones: the image part must not take the bytes after the data. */
			stream = create_mock_stream (camera, N_STREAM_BUFFERS);
			for (k = 0; k < G_N_ELEMENTS (buffer_excesses); k++)
				arv_stream_push_buffer (stream, arv_buffer_new_allocate (payload + buffer_excesses[k]));
			start_acquisition (camera);

			for (k = 0; k < 2 * (N_STREAM_BUFFERS + G_N_ELEMENTS (buffer_excesses)); k++) {
				ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);

				check_offset_image (buffer, image_offsets[i]);
				arv_stream_push_buffer (stream, buffer);
			}
			g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

			stop_acquisition (camera);

			g_object_unref (stream);
			g_object_unref (camera);

			teardown_environment ();
			assert_no_violation (control);

			mock_control_free (control);
		}

		g_free (image_offset);
	}
}

/* The buffers flushed from the producer queues by a stop come back aborted */

static void
flushed_buffers_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffers[N_STREAM_BUFFERS];
	const char *mock_path;
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FRAME_PERIOD_US", MOCK_FRAME_PERIOD_NEVER_US, TRUE);

	camera = open_mock_camera ();
	stream = create_mock_stream (camera, N_STREAM_BUFFERS);

	start_acquisition (camera);
	wait_for_counter (control, "new-buffer-waiters", 1);
	g_assert_cmpint (control->get_counter ("DSQueueBuffer"), ==, N_STREAM_BUFFERS);
	stop_acquisition (camera);

	for (i = 0; i < N_STREAM_BUFFERS; i++) {
		buffers[i] = arv_stream_try_pop_buffer (stream);
		g_assert_true (ARV_IS_BUFFER (buffers[i]));
		g_assert_cmpint (arv_buffer_get_status (buffers[i]), ==, ARV_BUFFER_STATUS_ABORTED);
	}
	assert_owned_buffers (stream, 0, 0, 0);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_aborted"), ==, N_STREAM_BUFFERS);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_completed_buffers"), ==, 0);

	/* Filled by the next acquisition */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FRAME_PERIOD_US", MOCK_FRAME_PERIOD_US));
	for (i = 0; i < N_STREAM_BUFFERS; i++)
		arv_stream_push_buffer (stream, buffers[i]);
	start_acquisition (camera);
	cycle_frame (stream);
	stop_acquisition (camera);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

/* Aravis's own GenTL producer, src/gentl, built with -Dgentl-producer=true, whose path meson gives in
 * ARV_TEST_GENTL_PRODUCER. It exported 37 of the 57 functions the consumer loads, which refused it. Its transport layer
 * exposes the Aravis interfaces, the disabled ones included, but not GenTL, which would load the producer again: the
 * fake camera is found through its Fake interface. Loaded by the consumer, the producer shares the Aravis interfaces
 * of the process: these tests run in a subprocess, with the GigE Vision discovery limited to a network interface which
 * doesn't exist. */

/* The transport layer vendor of the producer, and the id of the fake camera found through it */
#define PRODUCER_TL_VENDOR		"Aravis-Vendor"
#define PRODUCER_FAKE_DEVICE_ID		PRODUCER_TL_VENDOR "-Aravis-Fake-1"

/* Every function of the GenTL API declared by src/arvgentlprivate.h */
static const char *gentl_functions[] = {
	"GCInitLib", "GCCloseLib", "GCGetInfo", "GCGetLastError",
	"GCReadPort", "GCWritePort", "GCReadPortStacked", "GCWritePortStacked", "GCGetPortURL", "GCGetPortInfo",
	"GCGetNumPortURLs", "GCGetPortURLInfo", "GCRegisterEvent", "GCUnregisterEvent",
	"TLOpen", "TLClose", "TLGetInfo", "TLGetNumInterfaces", "TLGetInterfaceID", "TLGetInterfaceInfo",
	"TLOpenInterface", "TLUpdateInterfaceList",
	"IFClose", "IFGetInfo", "IFGetNumDevices", "IFGetDeviceID", "IFUpdateDeviceList", "IFGetDeviceInfo",
	"IFOpenDevice", "IFGetParentTL",
	"DevGetPort", "DevGetNumDataStreams", "DevGetDataStreamID", "DevOpenDataStream", "DevGetInfo", "DevClose",
	"DevGetParentIF",
	"DSAnnounceBuffer", "DSAllocAndAnnounceBuffer", "DSFlushQueue", "DSStartAcquisition", "DSStopAcquisition",
	"DSGetInfo", "DSGetBufferID", "DSClose", "DSRevokeBuffer", "DSQueueBuffer", "DSGetBufferInfo",
	"DSGetBufferChunkData", "DSGetParentDev", "DSGetNumBufferParts", "DSGetBufferPartInfo",
	"DSAnnounceCompositeBuffer", "DSGetBufferInfoStacked", "DSGetBufferPartInfoStacked", "DSGetNumFlows",
	"DSGetFlowInfo", "DSGetNumBufferSegments", "DSGetBufferSegmentInfo",
	"EventGetData", "EventGetDataInfo", "EventGetInfo", "EventFlush", "EventKill"
};

typedef struct {
	GModule *module;
	PGCInitLib GCInitLib;
	PGCCloseLib GCCloseLib;
	PGCGetLastError GCGetLastError;
	PGCGetPortURLInfo GCGetPortURLInfo;
	PGCReadPort GCReadPort;
	PTLOpen TLOpen;
	PTLClose TLClose;
	PTLUpdateInterfaceList TLUpdateInterfaceList;
	PTLGetNumInterfaces TLGetNumInterfaces;
	PTLGetInterfaceID TLGetInterfaceID;
	PTLGetInterfaceInfo TLGetInterfaceInfo;
	PTLOpenInterface TLOpenInterface;
	PIFClose IFClose;
	PIFGetInfo IFGetInfo;
	PIFUpdateDeviceList IFUpdateDeviceList;
	PIFGetNumDevices IFGetNumDevices;
	PIFGetDeviceID IFGetDeviceID;
} ProducerApi;

#define LOAD_PRODUCER_FUNCTION(api,function) \
	g_assert_true (g_module_symbol ((api)->module, #function, (gpointer *) &(api)->function))

#define ASSERT_NOT_IMPLEMENTED(api,function,...) \
	G_STMT_START { \
		P##function function_pointer = NULL; \
		g_assert_true (g_module_symbol ((api)->module, #function, (gpointer *) &function_pointer)); \
		g_assert_cmpint (function_pointer (__VA_ARGS__), ==, GC_ERR_NOT_IMPLEMENTED); \
	} G_STMT_END

/* GenTL transport layer type of an Aravis interface */

static const char *
get_interface_tl_type (const char *interface_id)
{
	const char *protocol = NULL;
	guint i;

	for (i = 0; i < arv_get_n_interfaces (); i++)
		if (g_strcmp0 (arv_get_interface_id (i), interface_id) == 0)
			protocol = arv_get_interface_protocol (i);

	if (g_strcmp0 (protocol, "GigEVision") == 0)
		return "GEV";
	if (g_strcmp0 (protocol, "USB3Vision") == 0)
		return "U3V";

	return "Custom";
}

/* The producer API, used directly: every function exported, the interfaces, the stubs */

static void
producer_api_test (void)
{
	ProducerApi api = {NULL};
	TL_HANDLE tl = NULL;
	IF_HANDLE interface = NULL;
	GC_ERROR last_error = GC_ERR_SUCCESS;
	const char *producer_path;
	uint32_t n_interfaces = 0;
	uint32_t n_devices = 0;
	bool8_t is_changed = 1;
	guint64 url_size;
	char text[256];
	char *xml;
	size_t size;
	guint i;

	GET_PRODUCER_PATH_OR_SKIP (producer_path);

	api.module = g_module_open (producer_path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
	if (api.module == NULL)
		g_error ("Failed to load '%s': %s", producer_path, g_module_error ());

	for (i = 0; i < G_N_ELEMENTS (gentl_functions); i++) {
		gpointer function = NULL;

		if (!g_module_symbol (api.module, gentl_functions[i], &function))
			function = NULL;
		/* Names the missing function on failure */
		g_assert_cmpstr (function != NULL ? NULL : gentl_functions[i], ==, NULL);
	}

	LOAD_PRODUCER_FUNCTION (&api, GCInitLib);
	LOAD_PRODUCER_FUNCTION (&api, GCCloseLib);
	LOAD_PRODUCER_FUNCTION (&api, GCGetLastError);
	LOAD_PRODUCER_FUNCTION (&api, GCGetPortURLInfo);
	LOAD_PRODUCER_FUNCTION (&api, GCReadPort);
	LOAD_PRODUCER_FUNCTION (&api, TLOpen);
	LOAD_PRODUCER_FUNCTION (&api, TLClose);
	LOAD_PRODUCER_FUNCTION (&api, TLUpdateInterfaceList);
	LOAD_PRODUCER_FUNCTION (&api, TLGetNumInterfaces);
	LOAD_PRODUCER_FUNCTION (&api, TLGetInterfaceID);
	LOAD_PRODUCER_FUNCTION (&api, TLGetInterfaceInfo);
	LOAD_PRODUCER_FUNCTION (&api, TLOpenInterface);
	LOAD_PRODUCER_FUNCTION (&api, IFClose);
	LOAD_PRODUCER_FUNCTION (&api, IFGetInfo);
	LOAD_PRODUCER_FUNCTION (&api, IFUpdateDeviceList);
	LOAD_PRODUCER_FUNCTION (&api, IFGetNumDevices);
	LOAD_PRODUCER_FUNCTION (&api, IFGetDeviceID);

	g_assert_cmpint (api.GCInitLib (), ==, GC_ERR_SUCCESS);
	g_assert_cmpint (api.TLOpen (&tl), ==, GC_ERR_SUCCESS);
	g_assert_nonnull (tl);

	g_assert_cmpint (api.TLUpdateInterfaceList (tl, &is_changed, 100), ==, GC_ERR_SUCCESS);
	g_assert_cmpint (is_changed, ==, 0);

	/* The description of the transport layer, read through its port. The size in a local URL is hexadecimal. */
	size = sizeof (text);
	g_assert_cmpint (api.GCGetPortURLInfo (tl, 0, URL_INFO_URL, NULL, text, &size), ==, GC_ERR_SUCCESS);
	g_assert_true (g_str_has_prefix (text, "Local:"));
	g_assert_nonnull (strrchr (text, ';'));
	url_size = g_ascii_strtoull (strrchr (text, ';') + 1, NULL, 16);
	g_assert_cmpuint (url_size, >, 0);
	g_assert_cmpuint (url_size, <, 65536);
	xml = g_malloc0 ((gsize) url_size);
	size = (size_t) url_size;
	g_assert_cmpint (api.GCReadPort (tl, 0, xml, &size), ==, GC_ERR_SUCCESS);
	g_assert_true (g_str_has_prefix (xml, "<RegisterDescription"));
	/* The data read, the string terminator included, give or take one byte */
	g_assert_cmpuint (url_size, <=, size + 1);
	g_free (xml);

	/* Every Aravis interface but GenTL, described before being opened */
	g_assert_cmpint (api.TLGetNumInterfaces (tl, &n_interfaces), ==, GC_ERR_SUCCESS);
	g_assert_cmpuint (n_interfaces, ==, arv_get_n_interfaces () - 1);
	for (i = 0; i < n_interfaces; i++) {
		char id[64];

		size = sizeof (id);
		g_assert_cmpint (api.TLGetInterfaceID (tl, i, id, &size), ==, GC_ERR_SUCCESS);
		g_assert_cmpstr (id, !=, "GenTL");

		size = sizeof (text);
		g_assert_cmpint (api.TLGetInterfaceInfo (tl, id, INTERFACE_INFO_ID, NULL, text, &size), ==,
				 GC_ERR_SUCCESS);
		g_assert_cmpstr (text, ==, id);
		size = sizeof (text);
		g_assert_cmpint (api.TLGetInterfaceInfo (tl, id, INTERFACE_INFO_DISPLAYNAME, NULL, text, &size), ==,
				 GC_ERR_SUCCESS);
		g_assert_cmpstr (text, ==, id);
		size = sizeof (text);
		g_assert_cmpint (api.TLGetInterfaceInfo (tl, id, INTERFACE_INFO_TLTYPE, NULL, text, &size), ==,
				 GC_ERR_SUCCESS);
		g_assert_cmpstr (text, ==, get_interface_tl_type (id));
	}
	size = sizeof (text);
	g_assert_cmpint (api.TLGetInterfaceID (tl, n_interfaces, text, &size), ==, GC_ERR_INVALID_INDEX);

	size = sizeof (text);
	g_assert_cmpint (api.TLGetInterfaceInfo (tl, "GenTL", INTERFACE_INFO_ID, NULL, text, &size), ==,
			 GC_ERR_INVALID_ID);
	g_assert_cmpint (api.TLOpenInterface (tl, "GenTL", &interface), ==, GC_ERR_INVALID_ID);
	g_assert_null (interface);

	/* The fake camera */
	g_assert_cmpint (api.TLOpenInterface (tl, "Fake", &interface), ==, GC_ERR_SUCCESS);
	g_assert_nonnull (interface);
	size = sizeof (text);
	g_assert_cmpint (api.IFGetInfo (interface, INTERFACE_INFO_ID, NULL, text, &size), ==, GC_ERR_SUCCESS);
	g_assert_cmpstr (text, ==, "Fake");
	is_changed = 1;
	g_assert_cmpint (api.IFUpdateDeviceList (interface, &is_changed, 100), ==, GC_ERR_SUCCESS);
	g_assert_cmpint (is_changed, ==, 0);
	g_assert_cmpint (api.IFGetNumDevices (interface, &n_devices), ==, GC_ERR_SUCCESS);
	g_assert_cmpuint (n_devices, ==, 1);
	size = sizeof (text);
	g_assert_cmpint (api.IFGetDeviceID (interface, 0, text, &size), ==, GC_ERR_SUCCESS);
	g_assert_cmpstr (text, ==, "Fake_1");
	size = sizeof (text);
	g_assert_cmpint (api.IFGetDeviceID (interface, 1, text, &size), ==, GC_ERR_INVALID_INDEX);
	g_assert_cmpint (api.IFClose (interface), ==, GC_ERR_SUCCESS);

	/* No data stream, no event */
	ASSERT_NOT_IMPLEMENTED (&api, DSAnnounceBuffer, NULL, NULL, 0, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSAllocAndAnnounceBuffer, NULL, 0, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSFlushQueue, NULL, ACQ_QUEUE_ALL_DISCARD);
	ASSERT_NOT_IMPLEMENTED (&api, DSStartAcquisition, NULL, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
	ASSERT_NOT_IMPLEMENTED (&api, DSStopAcquisition, NULL, ACQ_STOP_FLAGS_DEFAULT);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetInfo, NULL, STREAM_INFO_ID, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferID, NULL, 0, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSClose, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSRevokeBuffer, NULL, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSQueueBuffer, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferInfo, NULL, NULL, BUFFER_INFO_BASE, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferChunkData, NULL, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetParentDev, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetNumBufferParts, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferPartInfo, NULL, NULL, 0, BUFFER_PART_INFO_BASE, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSAnnounceCompositeBuffer, NULL, 0, NULL, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferInfoStacked, NULL, NULL, NULL, 0);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferPartInfoStacked, NULL, NULL, NULL, 0);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetNumFlows, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetFlowInfo, NULL, 0, FLOW_INFO_SIZE, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetNumBufferSegments, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, DSGetBufferSegmentInfo, NULL, NULL, 0, SEGMENT_INFO_BASE, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, EventGetData, NULL, NULL, NULL, 0);
	ASSERT_NOT_IMPLEMENTED (&api, EventGetDataInfo, NULL, NULL, 0, EVENT_DATA_ID, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, EventGetInfo, NULL, EVENT_EVENT_TYPE, NULL, NULL, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, EventFlush, NULL);
	ASSERT_NOT_IMPLEMENTED (&api, EventKill, NULL);

	size = sizeof (text);
	g_assert_cmpint (api.GCGetLastError (&last_error, text, &size), ==, GC_ERR_SUCCESS);
	g_assert_cmpint (last_error, ==, GC_ERR_NOT_IMPLEMENTED);
	g_assert_nonnull (strstr (text, "EventKill"));

	g_assert_cmpint (api.TLClose (tl), ==, GC_ERR_SUCCESS);
	g_assert_cmpint (api.GCCloseLib (), ==, GC_ERR_SUCCESS);

	g_module_close (api.module);

	/* The Fake interface opened by the producer */
	arv_shutdown ();
}

static void
setup_producer_environment (const char *producer_path)
{
	setup_environment (producer_path);

	/* The producer exposes the GigE Vision interface, disabled or not */
	arv_gv_interface_set_discovery_interface_name (NO_GV_DISCOVERY_INTERFACE);

	arv_enable_interface ("GenTL");
}

/* The fake camera, found and opened through the producer */

static ArvCamera *
open_producer_camera (void)
{
	ArvCamera *camera;
	GError *error = NULL;
	int index;

	arv_update_device_list ();

	index = find_device (PRODUCER_FAKE_DEVICE_ID);
	g_assert_cmpint (index, >=, 0);
	g_assert_cmpstr (arv_get_device_vendor (index), ==, "Aravis");
	g_assert_cmpstr (arv_get_device_model (index), ==, "Fake");
	g_assert_cmpstr (arv_get_device_serial_nbr (index), ==, "1");
	/* The transport layer type of the Fake interface */
	g_assert_cmpstr (arv_get_device_address (index), ==, "Custom");
	g_assert_cmpstr (arv_get_device_protocol (index), ==, "Custom");

	camera = arv_camera_new (PRODUCER_FAKE_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (arv_camera_is_gentl_device (camera));

	/* A register of the fake camera, read through the producer */
	g_assert_cmpint (arv_camera_get_integer (camera, "Width", &error), ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_no_error (error);

	return camera;
}

static void
producer_discovery_test (void)
{
	const char *producer_path;

	GET_PRODUCER_PATH_OR_SKIP (producer_path);

	if (g_test_subprocess ()) {
		ArvCamera *camera;
		ArvStream *stream;
		GError *error = NULL;

		setup_producer_environment (producer_path);

		camera = open_producer_camera ();

		/* Not implemented by the producer: register writes, data streams */
		arv_camera_set_integer (camera, "Width", ARV_FAKE_CAMERA_WIDTH_DEFAULT / 2, &error);
		g_assert_nonnull (error);
		g_assert_nonnull (strstr (error->message, "not implemented"));
		g_clear_error (&error);

		stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
		g_assert_null (stream);
		g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR);
		g_assert_nonnull (strstr (error->message, "DevOpenDataStream"));
		g_assert_nonnull (strstr (error->message, "not implemented"));
		g_clear_error (&error);

		g_object_unref (camera);

		/* Still enabled: the producer leaves the interface selection as it is */
		arv_update_device_list ();
		g_assert_cmpint (find_device (PRODUCER_FAKE_DEVICE_ID), >=, 0);

		teardown_environment ();
		return;
	}

	run_stream_subprocess ();
}

/* The producer, unloaded by arv_shutdown(), is loaded again by the next discovery. It registered types and an error
 * quark with names of its own memory, which GLib kept after the unload. */

static void
producer_reload_test (void)
{
	const char *producer_path;

	GET_PRODUCER_PATH_OR_SKIP (producer_path);

	if (g_test_subprocess ()) {
		guint i;

		for (i = 0; i < 3; i++) {
			ArvCamera *camera;
			GError *error = NULL;

			/* Again after arv_shutdown(), which forgets the GigE Vision discovery interface */
			setup_producer_environment (producer_path);

			camera = open_producer_camera ();

			/* A producer error */
			arv_camera_set_integer (camera, "Width", ARV_FAKE_CAMERA_WIDTH_DEFAULT / 2, &error);
			g_assert_nonnull (error);
			g_clear_error (&error);

			g_object_unref (camera);

			/* Unloads the producer */
			teardown_environment ();
		}

		return;
	}

	run_stream_subprocess ();
}

/* The device event thread of -Devent=true builds, with a producer which can't register the device event, like the
 * Aravis producer, or fails to wait for it. It used an uninitialized event handle and event size, and looped without
 * waiting on the errors. */

static void
device_event_errors_test (void)
{
#if ARAVIS_HAS_EVENT
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ArvCamera *camera;
		int n_waits;

		control = mock_control_new (mock_path);

		/* No device event */
		setup_environment (mock_path);
		g_setenv ("ARV_GENTL_MOCK_FAIL", "GCRegisterEvent", TRUE);

		camera = open_mock_camera ();
		wait_for_counter (control, "GCRegisterEvent", 1);
		g_usleep (200000);
		g_object_unref (camera);

		g_assert_cmpint (control->get_counter ("EventGetInfo"), ==, 0);
		g_assert_cmpint (control->get_counter ("EventGetData"), ==, 0);

		teardown_environment ();
		assert_no_violation (control);

		/* Failing waits */
		control->reset_counters ();
		setup_environment (mock_path);
		g_setenv ("ARV_GENTL_MOCK_FAIL", "EventGetData", TRUE);

		camera = open_mock_camera ();
		wait_for_counter (control, "EventGetData", 1);
		g_usleep (500000);
		n_waits = control->get_counter ("EventGetData");
		g_object_unref (camera);

		g_test_message ("EventGetData failing 500 ms: %d device event waits", n_waits);
		g_assert_cmpint (n_waits, <, 50);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
#else
	g_test_skip ("No event support (-Devent=false)");
#endif
}

/* The device event thread of -Devent=true builds, with a producer delivering remote device events. The event id, which
 * the mock gives without the zero ending a string, was read past its end, and leaked with every event. An application
 * reading the event value, as the GenICam event ports do, could read past its end, or while the next event of the same
 * id freed it. */

#if ARAVIS_HAS_EVENT

#define MOCK_DEVICE_EVENT_ID		0x9001
#define MOCK_DEVICE_EVENT_ID_STRING	"9001"
#define N_DEVICE_EVENTS			5

typedef struct {
	gint n_events;
	gint n_other_events;
} DeviceEventCount;

static void
device_event_cb (ArvDevice *device, int event_id, gpointer user_data)
{
	DeviceEventCount *count = user_data;

	if (event_id == MOCK_DEVICE_EVENT_ID)
		g_atomic_int_inc (&count->n_events);
	else
		g_atomic_int_inc (&count->n_other_events);
}

#endif

static void
device_events_test (void)
{
#if ARAVIS_HAS_EVENT
	MockControl *control;
	ArvCamera *camera;
	ArvDevice *device;
	DeviceEventCount count = {0, 0};
	GError *error = NULL;
	const char *mock_path;
	guint64 value = 0;
	gint64 end_time;
	guint i;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_DEVICE_EVENT_ID", MOCK_DEVICE_EVENT_ID_STRING, TRUE);

	camera = open_mock_camera ();
	device = arv_camera_get_device (camera);
	g_signal_connect (device, "device-event", G_CALLBACK (device_event_cb), &count);

	end_time = g_get_monotonic_time () + FRAME_TIMEOUT_US;
	while (g_atomic_int_get (&count.n_events) < N_DEVICE_EVENTS && g_get_monotonic_time () < end_time)
		g_usleep (1000);
	g_assert_cmpint (g_atomic_int_get (&count.n_events), >=, N_DEVICE_EVENTS);
	g_assert_cmpint (g_atomic_int_get (&count.n_other_events), ==, 0);

	/* The value of the last event, its number, while the next events replace it */
	for (i = 0; i < 1000; i++) {
		g_assert_true (arv_device_read_event_data (device, MOCK_DEVICE_EVENT_ID, 0, sizeof (value), &value,
							   &error));
		g_assert_no_error (error);
		g_assert_cmpuint (value, >=, N_DEVICE_EVENTS);
	}

	/* Nothing past its end */
	g_assert_false (arv_device_read_event_data (device, MOCK_DEVICE_EVENT_ID, 4, sizeof (value), &value, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR_INVALID_ADDRESS);
	g_clear_error (&error);
	g_assert_false (arv_device_read_event_data (device, MOCK_DEVICE_EVENT_ID, G_MAXUINT64, 1, &value, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR_INVALID_ADDRESS);
	g_clear_error (&error);

	/* No event of another id */
	g_assert_false (arv_device_read_event_data (device, MOCK_DEVICE_EVENT_ID + 1, 0, 1, &value, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_UNKNOWN);
	g_clear_error (&error);

	g_signal_handlers_disconnect_by_data (device, &count);
	/* Ends the event thread */
	g_object_unref (camera);

	g_assert_cmpint (control->get_counter ("device-events"), >=, N_DEVICE_EVENTS);
	g_assert_cmpint (control->get_counter ("registered-events"), ==, 0);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
#else
	g_test_skip ("No event support (-Devent=false)");
#endif
}

/* Refused buffers, undescribed buffers, fast frames, producer loading and concurrent closes. The cases which crash or
 * hang with the former code run in a subprocess. */

/* Stream: a buffer refused by the producer came back aborted, and an application pushing it back at once made the
 * stream thread and the application pass it back and forth at full speed. Pushed back by a "new-buffer" signal
 * handler, which runs in the stream thread, it kept the thread from checking for a stop, which never returned. */

#define REFUSED_BUFFERS_MAX_QUEUED	100

static void
push_back_new_buffer_cb (ArvStream *stream, gpointer user_data)
{
	ArvBuffer *buffer = arv_stream_try_pop_buffer (stream);
	gint *n_aborted = user_data;

	if (buffer != NULL) {
		if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_ABORTED)
			g_atomic_int_inc (n_aborted);
		arv_stream_push_buffer (stream, buffer);
	}
}

static void
refused_buffers_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ArvCamera *camera;
		ArvStream *stream;
		gint64 end_time;
		gint n_aborted = 0;
		int n_queued;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);

		camera = open_mock_camera ();

		/* The usual application loop, which pushes back every buffer it pops */
		stream = create_mock_stream (camera, N_STREAM_BUFFERS);
		start_acquisition (camera);
		cycle_frame (stream);

		g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "DSQueueBuffer"));
		n_queued = control->get_counter ("DSQueueBuffer");
		end_time = g_get_monotonic_time () + G_USEC_PER_SEC / 2;
		while (g_get_monotonic_time () < end_time) {
			ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, G_USEC_PER_SEC / 10);

			if (buffer != NULL) {
				if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_ABORTED)
					n_aborted++;
				arv_stream_push_buffer (stream, buffer);
			}
		}
		n_queued = control->get_counter ("DSQueueBuffer") - n_queued;
		g_test_message ("DSQueueBuffer failing 500 ms, buffers pushed back by a loop: %d queued, %d aborted",
				n_queued, n_aborted);
		g_assert_cmpint (n_aborted, >, 0);
		g_assert_cmpint (n_queued, <=, REFUSED_BUFFERS_MAX_QUEUED);
		g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), >=, (guint64) n_aborted);
		g_assert_cmpint (stop_acquisition (camera), <, STOP_DURATION_MAX_US);

		/* Started again once the producer accepts */
		g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
		start_acquisition (camera);
		cycle_frame (stream);
		stop_acquisition (camera);
		g_object_unref (stream);

		/* Pushed back by a signal handler, in the stream thread */
		n_aborted = 0;
		stream = create_mock_stream (camera, N_STREAM_BUFFERS);
		g_signal_connect (stream, "new-buffer", G_CALLBACK (push_back_new_buffer_cb), &n_aborted);
		arv_stream_set_emit_signals (stream, TRUE);
		start_acquisition (camera);
		wait_for_counter (control, "frames", control->get_counter ("frames") + 2);

		g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "DSQueueBuffer"));
		n_queued = control->get_counter ("DSQueueBuffer");
		g_usleep (G_USEC_PER_SEC / 2);
		n_queued = control->get_counter ("DSQueueBuffer") - n_queued;
		g_test_message ("DSQueueBuffer failing 500 ms, buffers pushed back by a signal handler: %d queued, "
				"%d aborted", n_queued, g_atomic_int_get (&n_aborted));
		g_assert_cmpint (g_atomic_int_get (&n_aborted), >, 0);
		g_assert_cmpint (n_queued, <=, REFUSED_BUFFERS_MAX_QUEUED);
		g_assert_cmpint (stop_acquisition (camera), <, STOP_DURATION_MAX_US);

		arv_stream_set_emit_signals (stream, FALSE);
		g_object_unref (stream);
		g_object_unref (camera);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
}

/* Stream: a buffer the producer couldn't describe at all, its informations failing, was a success */

static void
undescribed_buffer_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);

	camera = open_mock_camera ();
	/* Before the failures: the creation of the buffers reads their address */
	stream = create_mock_stream (camera, N_STREAM_BUFFERS);

	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", "DSGetBufferInfo"));
	start_acquisition (camera);

	buffer = arv_stream_timeout_pop_buffer (stream, FRAME_TIMEOUT_US);
	g_assert_true (ARV_IS_BUFFER (buffer));
	g_assert_cmpint (arv_buffer_get_status (buffer), !=, ARV_BUFFER_STATUS_SUCCESS);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), >=, 1);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_completed_buffers"), ==, 0);
	arv_stream_push_buffer (stream, buffer);

	/* Complete frames once the producer describes them */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
	pop_complete_frame (stream);

	stop_acquisition (camera);

	g_object_unref (stream);
	g_object_unref (camera);

	teardown_environment ();
	assert_no_violation (control);

	mock_control_free (control);
}

/* Mock: the frames were filled with the mock mutex held. With a short frame period, the stream thread held it most of
 * the time, and the calls of the other threads, like the acquisition stop, waited for seconds. */

typedef struct {
	ArvStream *stream;
	gint stop;
	gint n_frames;
} RecycleData;

static gpointer
recycle_thread (gpointer user_data)
{
	RecycleData *data = user_data;

	while (!g_atomic_int_get (&data->stop)) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (data->stream, 10000);

		if (buffer != NULL) {
			g_atomic_int_inc (&data->n_frames);
			arv_stream_push_buffer (data->stream, buffer);
		}
	}

	return NULL;
}

static void
fast_frames_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ArvCamera *camera;
		RecycleData data = {NULL, 0, 0};
		GThread *thread;
		GError *error = NULL;
		gint64 max_duration = 0;
		gint64 stop_duration;
		guint i;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);
		g_setenv ("ARV_GENTL_MOCK_FRAME_PERIOD_US", "1000", TRUE);

		camera = open_mock_camera ();
		data.stream = create_mock_stream (camera, 2 * N_STREAM_BUFFERS);
		start_acquisition (camera);
		thread = g_thread_new ("recycle", recycle_thread, &data);

		/* Register reads through the port of the device, while the frames are produced */
		for (i = 0; i < 20; i++) {
			gint64 start_time = g_get_monotonic_time ();
			gint64 duration;
			guint32 value = 0;

			g_assert_true (arv_device_read_register (arv_camera_get_device (camera),
								 ARV_FAKE_CAMERA_REGISTER_WIDTH, &value, &error));
			g_assert_no_error (error);
			duration = g_get_monotonic_time () - start_time;
			max_duration = MAX (max_duration, duration);
			g_usleep (10000);
		}
		stop_duration = stop_acquisition (camera);

		g_atomic_int_set (&data.stop, 1);
		g_thread_join (thread);

		g_test_message ("1 ms frame period: %d frames, longest register read %" G_GINT64_FORMAT " us, stop %"
				G_GINT64_FORMAT " us", g_atomic_int_get (&data.n_frames), max_duration, stop_duration);
		g_assert_cmpint (g_atomic_int_get (&data.n_frames), >, 0);
		g_assert_cmpint (max_duration, <, STOP_DURATION_MAX_US);
		g_assert_cmpint (stop_duration, <, STOP_DURATION_MAX_US);

		g_object_unref (data.stream);
		g_object_unref (camera);

		teardown_environment ();
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
}

/* Discovery: a producer failing TLOpen was still called with a NULL transport layer handle, and the failure only
 * reported by the debug output */

static void
tlopen_failure_test (void)
{
	MockControl *control;
	ArvDevice *device;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	g_setenv ("ARV_GENTL_MOCK_FAIL", "TLOpen", TRUE);
	arv_enable_interface ("GenTL");

	/* Reported once by loading of the producer */
	g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*GenTL producer*ArvGenTLMock.cti*not usable*TLOpen*");
	arv_update_device_list ();
	g_test_assert_expected_messages ();
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), <, 0);

	device = arv_open_device (MOCK_DEVICE_ID, &error);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_assert_null (device);
	g_clear_error (&error);

	g_assert_cmpint (control->get_counter ("TLOpen"), >=, 2);
	g_assert_cmpint (control->get_counter ("TLOpenInterface"), ==, 0);
	assert_no_violation (control);

	/* Usable once TLOpen succeeds */
	g_assert_true (control->set_option ("ARV_GENTL_MOCK_FAIL", NULL));
	arv_update_device_list ();
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), >=, 0);
	device = arv_open_device (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_DEVICE (device));
	g_object_unref (device);

	teardown_environment ();
	g_assert_cmpint (control->get_counter ("open-tl"), ==, 0);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	assert_no_violation (control);

	mock_control_free (control);
}

/* Discovery: a producer found by two paths to the same file, like two spellings of its folder on Windows, was
 * initialized twice, which a producer refuses */

static void
same_producer_test (void)
{
	MockControl *control;
	const char *mock_path;
	char *directory;
	char *alias;
	char *gentl_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	directory = g_path_get_dirname (mock_path);
#ifdef G_OS_WIN32
	/* Paths are case insensitive */
	alias = g_ascii_strup (directory, -1);
#else
	alias = g_build_filename (directory, ".", NULL);
#endif
	gentl_path = g_strjoin (G_SEARCHPATH_SEPARATOR_S, directory, alias, mock_path, NULL);

	control = mock_control_new (mock_path);
	setup_environment (gentl_path);
	g_setenv ("ARV_GENTL_PRODUCERS", "ArvGenTLMock.cti", TRUE);
	arv_enable_interface ("GenTL");

	arv_update_device_list ();
	g_assert_cmpuint (count_mock_devices (), ==, 1);
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), >=, 0);
	g_assert_cmpint (control->get_counter ("GCInitLib"), ==, 1);

	teardown_environment ();
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	assert_no_violation (control);

	mock_control_free (control);
	g_free (gentl_path);
	g_free (alias);
	g_free (directory);
}

/* Discovery: after arv_shutdown(), a discovery initialized again the producer of a live camera, which a producer
 * refuses: its devices were missing until the next arv_shutdown(). The producer is now shared, and closed once, by
 * the last of its users. */

static void
shutdown_rediscovery_test (void)
{
	MockControl *control;
	ArvCamera *camera;
	ArvDevice *device;
	GError *error = NULL;
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	control = mock_control_new (mock_path);
	setup_environment (mock_path);
	arv_enable_interface ("GenTL");

	camera = arv_camera_new (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));

	arv_disable_interface ("GenTL");
	arv_shutdown ();

	/* The camera is alive */
	arv_enable_interface ("GenTL");
	arv_update_device_list ();
	g_assert_cmpint (find_device (MOCK_DEVICE_ID), >=, 0);
	g_assert_cmpint (control->get_counter ("GCInitLib"), ==, 1);
	assert_no_violation (control);

	g_assert_cmpint (arv_camera_get_integer (camera, "Width", &error), ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_no_error (error);

	g_clear_object (&camera);

	/* The producer stays initialized for the new discovery */
	g_assert_cmpint (control->get_counter ("DevClose"), ==, 1);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 0);

	device = arv_open_device (MOCK_DEVICE_ID, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GENTL_DEVICE (device));
	g_object_unref (device);

	teardown_environment ();

	g_assert_cmpint (control->get_counter ("GCInitLib"), ==, 1);
	g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
	g_assert_cmpint (control->get_counter ("initialized"), ==, 0);
	assert_no_violation (control);

	mock_control_free (control);
}

/* Discovery: a device closed by a thread while another thread ran a discovery, or opened or closed another device of
 * the same interface, corrupted the reference counts and the interface table of the producer, each thread using
 * handles another one had just closed. Each device is opened and closed in a loop by its own thread, during
 * discoveries. The mock closes its modules slowly, which widens the race windows. */

#define CONCURRENCY_DURATION_US		(3 * G_USEC_PER_SEC / 2)
#define CONCURRENCY_N_DEVICES		4
#define CONCURRENCY_CLOSE_DELAY_US	"1000"

typedef struct {
	gint stop;
	gint n_discoveries;
	gint n_incomplete_discoveries;
	gint n_opens;
	gint n_open_failures;
} ConcurrencyData;

typedef struct {
	ConcurrencyData *data;
	char *device_id;
} OpenCloseData;

static gpointer
discovery_thread (gpointer user_data)
{
	ConcurrencyData *data = user_data;

	while (!g_atomic_int_get (&data->stop)) {
		arv_update_device_list ();
		g_atomic_int_inc (&data->n_discoveries);
		/* The device list doesn't depend on the opened devices */
		if (count_mock_devices () != CONCURRENCY_N_DEVICES)
			g_atomic_int_inc (&data->n_incomplete_discoveries);
		g_usleep (50);
	}

	return NULL;
}

static gpointer
open_close_thread (gpointer user_data)
{
	OpenCloseData *open_close = user_data;
	ConcurrencyData *data = open_close->data;

	while (!g_atomic_int_get (&data->stop)) {
		GError *error = NULL;
		ArvDevice *device;

		device = arv_open_device (open_close->device_id, &error);
		if (device != NULL) {
			g_atomic_int_inc (&data->n_opens);
			g_usleep (g_random_int_range (0, 200));
			/* Closed without the lock of the discovery */
			g_object_unref (device);
		} else {
			g_atomic_int_inc (&data->n_open_failures);
			g_clear_error (&error);
		}
	}

	return NULL;
}

static void
concurrent_close_test (void)
{
	const char *mock_path;

	GET_TEST_PATH_OR_SKIP (mock_path, "ARV_TEST_GENTL_MOCK");

	if (g_test_subprocess ()) {
		MockControl *control;
		ConcurrencyData data = {0, 0, 0, 0, 0};
		OpenCloseData open_close[CONCURRENCY_N_DEVICES];
		GThread *open_close_threads[CONCURRENCY_N_DEVICES];
		GThread *discovery;
		char *n_devices;
		guint i;

		control = mock_control_new (mock_path);
		setup_environment (mock_path);
		n_devices = g_strdup_printf ("%d", CONCURRENCY_N_DEVICES);
		g_setenv ("ARV_GENTL_MOCK_N_DEVICES", n_devices, TRUE);
		g_free (n_devices);
		g_setenv ("ARV_GENTL_MOCK_CLOSE_DELAY_US", CONCURRENCY_CLOSE_DELAY_US, TRUE);
		arv_enable_interface ("GenTL");
		arv_update_device_list ();
		g_assert_cmpuint (count_mock_devices (), ==, CONCURRENCY_N_DEVICES);

		discovery = g_thread_new ("discovery", discovery_thread, &data);
		for (i = 0; i < CONCURRENCY_N_DEVICES; i++) {
			open_close[i].data = &data;
			open_close[i].device_id = g_strdup_printf ("%s-%s-%s-%s%u", ARV_GENTL_MOCK_TL_VENDOR,
								   ARV_GENTL_MOCK_VENDOR, ARV_GENTL_MOCK_MODEL,
								   ARV_GENTL_MOCK_SERIAL, i);
			open_close_threads[i] = g_thread_new ("open-close", open_close_thread, &open_close[i]);
		}

		g_usleep (CONCURRENCY_DURATION_US);

		g_atomic_int_set (&data.stop, 1);
		g_thread_join (discovery);
		for (i = 0; i < CONCURRENCY_N_DEVICES; i++) {
			g_thread_join (open_close_threads[i]);
			g_free (open_close[i].device_id);
		}

		g_test_message ("%d discoveries, %d incomplete, %d opens, %d failed opens", data.n_discoveries,
				data.n_incomplete_discoveries, data.n_opens, data.n_open_failures);
		g_assert_cmpint (data.n_discoveries, >, 0);
		g_assert_cmpint (data.n_opens, >, 0);
		assert_no_violation (control);
		g_assert_cmpint (data.n_incomplete_discoveries, ==, 0);
		g_assert_cmpint (data.n_open_failures, ==, 0);
		g_assert_cmpint (control->get_counter ("open-devices"), ==, 0);
		g_assert_cmpint (control->get_counter ("open-interfaces"), ==, 0);
		g_assert_cmpint (control->get_counter ("open-tl"), ==, 0);

		teardown_environment ();
		g_assert_cmpint (control->get_counter ("GCCloseLib"), ==, 1);
		assert_no_violation (control);

		mock_control_free (control);
		return;
	}

	run_stream_subprocess ();
}

/* Producer: its interface handles, the Aravis interfaces, were destroyed by arv_shutdown() while the consumer used
 * them: closing a camera opened through the producer crashed */

static void
producer_shutdown_test (void)
{
	const char *producer_path;

	GET_PRODUCER_PATH_OR_SKIP (producer_path);

	if (g_test_subprocess ()) {
		ArvCamera *camera;
		GError *error = NULL;

		setup_producer_environment (producer_path);
		camera = open_producer_camera ();

		arv_disable_interface ("GenTL");
		arv_shutdown ();

		g_assert_cmpint (arv_camera_get_integer (camera, "Width", &error), ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT);
		g_assert_no_error (error);

		/* Found again, through the producer of the camera */
		setup_producer_environment (producer_path);
		arv_update_device_list ();
		g_assert_cmpint (find_device (PRODUCER_FAKE_DEVICE_ID), >=, 0);

		g_object_unref (camera);

		teardown_environment ();
		return;
	}

	run_stream_subprocess ();
}

/* Utilities */

static char *
run_utility (const char *variable, const char * const *arguments)
{
	GPtrArray *argv;
	GError *error = NULL;
	const char *mock_path;
	const char *program;
	char **envp;
	char *output = NULL;
	gint wait_status = 0;
	guint i;

	mock_path = g_getenv ("ARV_TEST_GENTL_MOCK");
	program = g_getenv (variable);
	if (mock_path == NULL || program == NULL)
		return NULL;

	argv = g_ptr_array_new ();
	g_ptr_array_add (argv, (char *) program);
	for (i = 0; arguments[i] != NULL; i++)
		g_ptr_array_add (argv, (char *) arguments[i]);
	g_ptr_array_add (argv, NULL);

	envp = g_get_environ ();
	envp = g_environ_setenv (envp, get_gentl_path_variable (), mock_path, TRUE);
	envp = g_environ_setenv (envp, "ARV_GENTL_MOCK_FRAME_PERIOD_US", MOCK_FRAME_PERIOD_US, TRUE);
	envp = g_environ_unsetenv (envp, "ARV_GENTL_ENABLE");
	envp = g_environ_unsetenv (envp, "ARV_GENTL_PRODUCERS");
	envp = g_environ_unsetenv (envp, "ARV_DEBUG");

	g_spawn_sync (NULL, (char **) argv->pdata, envp, G_SPAWN_DEFAULT, NULL, NULL, &output, NULL,
		      &wait_status, &error);
	g_assert_no_error (error);

	/* Deprecated by GLib 2.70, which this test doesn't require */
	g_assert_true (g_spawn_check_exit_status (wait_status, NULL));

	g_strfreev (envp);
	g_ptr_array_unref (argv);

	return output;
}

static void
arv_tool_test (void)
{
	const char *list_arguments[] = {NO_GV_DISCOVERY_OPTION, NULL};
	const char *gentl_arguments[] = {"--gentl", NO_GV_DISCOVERY_OPTION, NULL};
	char *output;

	output = run_utility ("ARV_TEST_ARV_TOOL", list_arguments);
	if (output == NULL) {
		g_test_skip ("ARV_TEST_ARV_TOOL is not set, the test must be run by meson");
		return;
	}
	g_assert_null (strstr (output, MOCK_DEVICE_ID));
	g_free (output);

	output = run_utility ("ARV_TEST_ARV_TOOL", gentl_arguments);
	g_assert_nonnull (strstr (output, MOCK_DEVICE_ID));
	g_free (output);
}

static void
arv_camera_test_test (void)
{
	const char *arguments[] = {"--gentl", NO_GV_DISCOVERY_OPTION, "--name=" MOCK_DEVICE_ID, "--duration=1", NULL};
	const char *statistics;
	char *output;
	guint64 n_completed_buffers;

	output = run_utility ("ARV_TEST_ARV_CAMERA_TEST", arguments);
	if (output == NULL) {
		g_test_skip ("ARV_TEST_ARV_CAMERA_TEST is not set, the test must be run by meson");
		return;
	}

	statistics = strstr (output, "n_completed_buffers");
	if (statistics == NULL)
		g_test_message ("arv-camera-test output:\n%s", output);
	g_assert_nonnull (statistics);
	statistics = strchr (statistics, '=');
	g_assert_nonnull (statistics);
	n_completed_buffers = g_ascii_strtoull (statistics + 1, NULL, 10);
	g_assert_cmpuint (n_completed_buffers, >, 0);

	g_free (output);
}

/* arv-test had no option to enable GenTL. Only its option list is checked: arv-test can't restrict the GigE Vision
 * discovery, which would go out on the network. */

static void
arv_test_test (void)
{
	const char *arguments[] = {"--help", NULL};
	char *output;

	output = run_utility ("ARV_TEST_ARV_TEST", arguments);
	if (output == NULL) {
		g_test_skip ("ARV_TEST_ARV_TEST is not set, the test must be run by meson");
		return;
	}

	g_assert_nonnull (strstr (output, "--gentl"));

	g_free (output);
}

int
main (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

	/* The tests define the GenTL selection. No interface selection call here: ARV_GENTL_ENABLE is read by the
	 * first one. */
	g_unsetenv ("ARV_GENTL_ENABLE");
	g_unsetenv ("ARV_GENTL_PRODUCERS");

	g_test_add_func ("/gentl/opt-in/default", opt_in_test);
	g_test_add_func ("/gentl/opt-in/environment-enabled", environment_enabled_test);
	g_test_add_func ("/gentl/opt-in/environment-disabled", environment_disabled_test);
	g_test_add_func ("/gentl/opt-in/device-interface-id", device_interface_id_test);
	g_test_add_func ("/gentl/opt-in/interface-by-id", interface_by_id_test);
	g_test_add_func ("/gentl/discovery/missing-serial-number", missing_serial_number_test);
	g_test_add_func ("/gentl/discovery/missing-strings", missing_strings_test);
	g_test_add_func ("/gentl/discovery/empty-strings", empty_strings_test);
	g_test_add_func ("/gentl/discovery/interfaces-same-device-id", interfaces_same_device_id_test);
	g_test_add_func ("/gentl/discovery/missing-serial-number-interfaces", missing_serial_number_interfaces_test);
	g_test_add_func ("/gentl/discovery/missing-serial-number-producers", missing_serial_number_producers_test);
	g_test_add_func ("/gentl/discovery/missing-serial-number-producer-separators",
			 missing_serial_number_producer_separators_test);
	g_test_add_data_func ("/gentl/discovery/missing-serial-number-interface-device-separators",
			      &encoded_identity_pairs[0], missing_serial_number_encoded_ids_test);
	g_test_add_data_func ("/gentl/discovery/missing-serial-number-percent-identifiers",
			      &encoded_identity_pairs[1], missing_serial_number_encoded_ids_test);
	g_test_add_data_func ("/gentl/discovery/missing-serial-number-control-identifiers",
			      &encoded_identity_pairs[2], missing_serial_number_encoded_ids_test);
	g_test_add_data_func ("/gentl/discovery/missing-serial-number-utf8-identifiers",
			      &encoded_identity_pairs[3], missing_serial_number_encoded_ids_test);
	g_test_add_func ("/gentl/load/optional-functions", optional_functions_test);
	g_test_add_func ("/gentl/load/missing-function", missing_function_test);
	g_test_add_func ("/gentl/load/invalid-producer", invalid_producer_test);
#ifdef G_OS_WIN32
	g_test_add_func ("/gentl/load/dependency", dependency_test);
#endif
	g_test_add_func ("/gentl/load/producer-selection", producer_selection_test);
	g_test_add_func ("/gentl/open/failure", open_failure_test);
	g_test_add_func ("/gentl/shutdown/live-camera", shutdown_live_camera_test);
	g_test_add_func ("/gentl/shutdown/live-stream", shutdown_live_stream_test);
	g_test_add_func ("/gentl/acquisition/basic", acquisition_test);
	g_test_add_func ("/gentl/stream/stop-blocked", stop_blocked_test);
	g_test_add_func ("/gentl/stream/stop-race", stop_race_test);
	g_test_add_func ("/gentl/stream/late-buffer", late_buffer_test);
	g_test_add_func ("/gentl/stream/wait-error", wait_error_test);
	g_test_add_func ("/gentl/stream/open-failure", stream_open_failure_test);
	g_test_add_func ("/gentl/stream/announce-failure", announce_failure_test);
	g_test_add_func ("/gentl/stream/start-failure", start_failure_test);
	g_test_add_func ("/gentl/stream/incomplete-frame", incomplete_frame_test);
	g_test_add_func ("/gentl/stream/oversized-frame", oversized_frame_test);
	g_test_add_func ("/gentl/stream/image-offset", image_offset_test);
	g_test_add_func ("/gentl/stream/flushed-buffers", flushed_buffers_test);
	g_test_add_func ("/gentl/producer/api", producer_api_test);
	g_test_add_func ("/gentl/producer/discovery", producer_discovery_test);
	g_test_add_func ("/gentl/producer/reload", producer_reload_test);
	g_test_add_func ("/gentl/device/event-errors", device_event_errors_test);
	g_test_add_func ("/gentl/device/events", device_events_test);
	g_test_add_func ("/gentl/utilities/arv-tool", arv_tool_test);
	g_test_add_func ("/gentl/utilities/arv-camera-test", arv_camera_test_test);

	g_test_add_func ("/gentl/stream/refused-buffers", refused_buffers_test);
	g_test_add_func ("/gentl/stream/undescribed-buffer", undescribed_buffer_test);
	g_test_add_func ("/gentl/stream/fast-frames", fast_frames_test);
	g_test_add_func ("/gentl/discovery/tlopen-failure", tlopen_failure_test);
	g_test_add_func ("/gentl/discovery/same-producer", same_producer_test);
	g_test_add_func ("/gentl/discovery/concurrent-close", concurrent_close_test);
	g_test_add_func ("/gentl/shutdown/rediscovery", shutdown_rediscovery_test);
	g_test_add_func ("/gentl/producer/shutdown", producer_shutdown_test);
	g_test_add_func ("/gentl/utilities/arv-test", arv_test_test);

	return g_test_run ();
}
