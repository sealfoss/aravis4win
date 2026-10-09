/* SPDX-License-Identifier:Unlicense */

#include "fakeuvtest.h"
#include <arvuvinterfaceprivate.h>
#include <stdlib.h>

typedef struct {
	GTestFunc func;
} FakeuvTest;

static guint fakeuv_time_scale = 1;
static guint64 fakeuv_expected_misuse = 0;

void
fakeuv_test_init (int *argc, char ***argv)
{
	const char *time_scale;

	g_test_init (argc, argv, NULL);

#ifdef GENICAM_FILENAME
	arv_set_fake_camera_genicam_filename (GENICAM_FILENAME);
#endif

	/* No network, no GenTL producer: only the USB3Vision interface, on top of the fake libusb */
	arv_select_interface ("USB3Vision");

	time_scale = g_getenv ("FAKEUV_TIME_SCALE");
	if (time_scale != NULL && atoi (time_scale) > 1)
		fakeuv_time_scale = atoi (time_scale);
	fake_usb_set_time_scale (fakeuv_time_scale);

	fake_usb_reset ();
}

static void
_check_misuse (void)
{
	guint64 n_misuse = fake_usb_get_total_stat (FAKE_USB_STAT_API_MISUSE);

	if (n_misuse != fakeuv_expected_misuse) {
		char *last = fake_usb_dup_last_misuse ();

		g_printerr ("libusb API misuse: %" G_GUINT64_FORMAT " time(s), %" G_GUINT64_FORMAT " expected; "
			    "last: %s (FAKE_USB_DEBUG=1 logs each one)\n",
			    n_misuse, fakeuv_expected_misuse, last != NULL ? last : "-");
		g_free (last);
	}

	g_assert_cmpuint (n_misuse, ==, fakeuv_expected_misuse);
}

static void
_run_test (gconstpointer data)
{
	const FakeuvTest *test = data;

	fakeuv_test_reset ();

	test->func ();

	_check_misuse ();
	fake_usb_reset_counters ();
	fakeuv_expected_misuse = 0;
}

void
fakeuv_test_add (const char *path, GTestFunc test_func)
{
	FakeuvTest *test = g_new0 (FakeuvTest, 1);

	/* Lives as long as the test suite */
	test->func = test_func;

	g_test_add_data_func (path, test, _run_test);
}

int
fakeuv_test_run (void)
{
	int result;

	result = g_test_run ();

	_check_misuse ();

	arv_shutdown ();

	/* Everything Aravis got from libusb must have been released */
	if (fake_usb_get_n_contexts () != 0 || fake_usb_get_n_open_handles () != 0 || fake_usb_get_n_transfers () != 0) {
		g_printerr ("Leaked libusb objects after arv_shutdown: %u context(s), %u handle(s), %u transfer(s)\n",
			    fake_usb_get_n_contexts (), fake_usb_get_n_open_handles (), fake_usb_get_n_transfers ());
		result = 1;
	}

	fake_usb_reset ();

	return result;
}

void
fakeuv_test_reset (void)
{
	_check_misuse ();

	fake_usb_reset ();
	fakeuv_expected_misuse = 0;

	/* Aravis's defaults: no RAW_IO, nothing known of where a camera was found, as the fake gives its new devices the
	 * addresses of the old ones */
	g_unsetenv ("ARV_UV_RAW_IO");
	arv_uv_interface_forget_device_locations ();
}

void
fakeuv_test_expect_misuse (guint64 n_misuse)
{
	fakeuv_expected_misuse = n_misuse;
}

guint
fakeuv_timeout_ms (guint timeout_ms)
{
	return timeout_ms * fakeuv_time_scale;
}

FakeUsbDevice *
fakeuv_add_camera (const char *serial_number)
{
	FakeUsbDevice *camera;

	camera = fake_usb_camera_new (serial_number);
	fake_usb_camera_set_frame_period (camera, FAKEUV_FRAME_PERIOD_US);

	return camera;
}

FakeUsbDevice *
fakeuv_add_other_device (void)
{
	FakeUsbDevice *device;
	guint8 interface_number;

	/* A HID device: per interface class, one interrupt endpoint */
	device = fake_usb_device_new (FAKEUV_OTHER_DEVICE_VENDOR_ID, FAKEUV_OTHER_DEVICE_PRODUCT_ID, 0x00, 0x00, 0x00);
	fake_usb_device_set_bcd_usb (device, 0x0200);
	fake_usb_device_set_descriptor_strings (device,
						fake_usb_device_add_string (device, "Aravis"),
						fake_usb_device_add_string (device, "Fake keyboard"),
						0);
	interface_number = fake_usb_device_add_interface (device, 0x03, 0x01, 0x01, NULL, 0);
	fake_usb_device_add_endpoint (device, interface_number, 0x81, LIBUSB_TRANSFER_TYPE_INTERRUPT, 8);

	return device;
}

ArvCamera *
fakeuv_open_camera (FakeUsbDevice *camera, GError **error)
{
	ArvCamera *arv_camera;
	char *device_id;

	device_id = fake_usb_camera_dup_device_id (camera);
	arv_camera = arv_camera_new (device_id, error);
	g_free (device_id);

	return arv_camera;
}

gboolean
fakeuv_wait_for (FakeuvCondition condition, gpointer user_data, guint timeout_ms)
{
	gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (timeout_ms) * G_TIME_SPAN_MILLISECOND;

	while (!condition (user_data)) {
		if (g_get_monotonic_time () >= end_time)
			return condition (user_data);
		g_usleep (1000);
	}

	return TRUE;
}

void
fakeuv_stream_callback (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer)
{
	FakeuvStreamEvents *events = user_data;

	switch (type) {
		case ARV_STREAM_CALLBACK_TYPE_INIT:
			g_atomic_int_inc (&events->n_init);
			break;
		case ARV_STREAM_CALLBACK_TYPE_EXIT:
			g_atomic_int_inc (&events->n_exit);
			break;
		case ARV_STREAM_CALLBACK_TYPE_START_BUFFER:
			g_atomic_int_inc (&events->n_start_buffer);
			break;
		case ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE:
			g_atomic_int_inc (&events->n_buffer_done);
			break;
		default:
			break;
	}
}

void
fakeuv_recycle_buffer (FakeUsbDevice *camera, ArvStream *stream, ArvBuffer *buffer)
{
	guint64 n_rounds = fake_usb_device_get_call_count (camera, FAKE_USB_CALL_HANDLE_EVENTS);
	gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS) *
		G_TIME_SPAN_MILLISECOND;

	/* The event thread of the device is the only caller of libusb_handle_events*() on its context: the next call
	 * starts after the one that dispatched the trailer callback of this buffer has returned */
	while (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_HANDLE_EVENTS) == n_rounds &&
	       g_get_monotonic_time () < end_time)
		g_usleep (500);

	arv_stream_push_buffer (stream, buffer);
}

gboolean
fakeuv_buffer_matches_frame (FakeUsbDevice *camera, ArvBuffer *buffer)
{
	const void *data;
	size_t size = 0;
	guint32 checksum;
	gsize sent_size;

	if (!ARV_IS_BUFFER (buffer) || arv_buffer_get_status (buffer) != ARV_BUFFER_STATUS_SUCCESS)
		return FALSE;

	if (!fake_usb_camera_get_frame_checksum (camera, arv_buffer_get_frame_id (buffer), &checksum, &sent_size)) {
		if (g_getenv ("FAKEUV_DEBUG") != NULL)
			g_printerr ("frame %" G_GUINT64_FORMAT " unknown\n", arv_buffer_get_frame_id (buffer));
		return FALSE;
	}

	data = arv_buffer_get_image_data (buffer, &size);

	if (g_getenv ("FAKEUV_DEBUG") != NULL)
		g_printerr ("frame %" G_GUINT64_FORMAT ": size %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT
			    " checksum %08x/%08x\n", arv_buffer_get_frame_id (buffer), (gsize) size, sent_size,
			    data != NULL ? fake_usb_checksum (data, size) : 0, checksum);

	return data != NULL && size == sent_size && fake_usb_checksum (data, size) == checksum;
}

void
fakeuv_assert_frame (FakeUsbDevice *camera, ArvBuffer *buffer, guint32 width, guint32 height,
		     ArvPixelFormat pixel_format)
{
	const guint8 *data;
	size_t size = 0;

	g_assert_true (ARV_IS_BUFFER (buffer));
	g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SUCCESS);
	g_assert_cmpint (arv_buffer_get_payload_type (buffer), ==, ARV_BUFFER_PAYLOAD_TYPE_IMAGE);
	g_assert_cmpint (arv_buffer_get_image_width (buffer), ==, width);
	g_assert_cmpint (arv_buffer_get_image_height (buffer), ==, height);
	g_assert_cmpint (arv_buffer_get_image_x (buffer), ==, 0);
	g_assert_cmpint (arv_buffer_get_image_y (buffer), ==, 0);
	g_assert_cmphex (arv_buffer_get_image_pixel_format (buffer), ==, pixel_format);
	g_assert_cmpuint (arv_buffer_get_frame_id (buffer), !=, 0);

	data = arv_buffer_get_image_data (buffer, &size);
	g_assert_nonnull (data);
	g_assert_cmpuint (size, ==, (gsize) width * height * ARV_PIXEL_FORMAT_BIT_PER_PIXEL (pixel_format) / 8);

	/* Same bytes as the fake camera sent for this frame id */
	g_assert_true (fakeuv_buffer_matches_frame (camera, buffer));

	/* And, independently, ArvFakeCamera's default pattern (no gain, default exposure): (x + y + n) % 255, where
	 * n depends on ArvFakeCamera's own frame counter, not on the frame id the camera sends */
	if (pixel_format == ARV_PIXEL_FORMAT_MONO_8) {
		guint offset = data[0];
		guint32 x, y;

		for (y = 0; y < height; y += 7)
			for (x = 0; x < width; x += 5)
				g_assert_cmpuint (data[y * width + x], ==, (x + y + offset) % 255);
	}
}

guint
fakeuv_acquire_frames (ArvCamera *camera, FakeUsbDevice *device, ArvUvUsbMode usb_mode,
		       guint n_buffers, guint n_frames, guint timeout_ms)
{
	FakeuvStreamEvents events = {0};
	ArvStream *stream;
	GError *error = NULL;
	gint64 end_time;
	guint32 width, height;
	ArvPixelFormat pixel_format;
	gint x, y, w, h;
	size_t payload;
	guint n_received = 0;
	guint i;

	arv_camera_get_region (camera, &x, &y, &w, &h, &error);
	g_assert_no_error (error);
	width = w;
	height = h;
	pixel_format = arv_camera_get_pixel_format (camera, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, usb_mode);

	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));

	for (i = 0; i < n_buffers; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (timeout_ms) * G_TIME_SPAN_MILLISECOND;

	while (n_received < n_frames) {
		ArvBuffer *buffer;
		gint64 now = g_get_monotonic_time ();

		if (now >= end_time)
			break;

		buffer = arv_stream_timeout_pop_buffer (stream, end_time - now);
		if (buffer == NULL)
			break;

		fakeuv_assert_frame (device, buffer, width, height, pixel_format);
		n_received++;

		/* At once, in both modes, as aravissrc does */
		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_completed_buffers"), >=, n_received);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

	g_object_unref (stream);

	g_assert_cmpint (g_atomic_int_get (&events.n_init), ==, 1);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 1);

	return n_received;
}
