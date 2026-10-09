/* SPDX-License-Identifier:Unlicense */

/* USB3 Vision through Aravis's public API, against the fake libusb: discovery, open, GenICam features, acquisition
 * in both USB modes, restart, unplug, 64 bit frame ids and GenDC containers. */

#include "fakeuvtest.h"
#include <arvuvcpprivate.h>
#include <arvuvspprivate.h>

#define N_FRAMES	10
#define N_BUFFERS	5
#define TIMEOUT_MS	FAKEUV_TIMEOUT_MS

static void
discovery_test (void)
{
	FakeUsbDevice *other;
	FakeUsbDevice *camera;
	char *device_id;

	fakeuv_test_reset ();

	other = fakeuv_add_other_device ();
	camera = fakeuv_add_camera ("UV1");

	arv_update_device_list ();

	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	device_id = fake_usb_camera_dup_device_id (camera);
	g_assert_cmpstr (device_id, ==, "Aravis-Fake-UV1");
	g_assert_cmpstr (arv_get_device_id (0), ==, device_id);
	g_free (device_id);

	g_assert_cmpstr (arv_get_device_vendor (0), ==, "Aravis");
	g_assert_cmpstr (arv_get_device_model (0), ==, "Fake");
	g_assert_cmpstr (arv_get_device_serial_nbr (0), ==, "UV1");
	g_assert_cmpstr (arv_get_device_physical_id (0), ==, fake_usb_camera_get_guid (camera));
	g_assert_cmpstr (arv_get_device_protocol (0), ==, "USB3Vision");

	/* The camera was opened to read its strings, then closed; the other device has another class and is never
	 * opened */
	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_OPEN), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_CLOSE), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (camera), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (other, FAKE_USB_CALL_OPEN), ==, 0);

	/* Unplugged, the camera leaves the list; plugged again, it is back */
	fake_usb_device_unplug (camera);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 0);

	fake_usb_device_plug (camera);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	fakeuv_add_camera ("UV2");
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 2);
}

static void
open_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	fakeuv_test_reset ();

	fakeuv_add_other_device ();
	device = fakeuv_add_camera ("UV1");

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_assert_true (arv_camera_is_uv_device (camera));

	g_assert_cmpstr (arv_camera_get_vendor_name (camera, &error), ==, "Aravis");
	g_assert_no_error (error);
	g_assert_cmpstr (arv_camera_get_model_name (camera, &error), ==, "Fake");
	g_assert_no_error (error);
	g_assert_cmpstr (arv_camera_get_device_serial_number (camera, &error), ==, "UV1");
	g_assert_no_error (error);

	/* Control and data interfaces claimed, event interface left alone */
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 1);
	g_assert_true (fake_usb_device_is_interface_claimed (device, FAKE_USB_CAMERA_CONTROL_INTERFACE));
	g_assert_true (fake_usb_device_is_interface_claimed (device, FAKE_USB_CAMERA_DATA_INTERFACE));
	g_assert_false (fake_usb_device_is_interface_claimed (device, FAKE_USB_CAMERA_EVENT_INTERFACE));

	/* The GenICam data was read through the manifest, in chunks no larger than the camera allows */
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS), >,
			  20000 / FAKE_USB_CAMERA_DEFAULT_MAX_ACK_TRANSFER);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_INVALID_COMMANDS), ==, 0);

	g_clear_object (&camera);

	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);
	g_assert_false (fake_usb_device_is_interface_claimed (device, FAKE_USB_CAMERA_CONTROL_INTERFACE));
	g_assert_false (fake_usb_device_is_interface_claimed (device, FAKE_USB_CAMERA_DATA_INTERFACE));

	/* First available device */
	camera = arv_camera_new (NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_clear_object (&camera);

	camera = arv_camera_new ("Aravis-Fake-NotThere", &error);
	g_assert_null (camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_clear_error (&error);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
}

static void
features_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvDevice *arv_device;
	ArvFakeCamera *fake_camera;
	GError *error = NULL;
	guint32 value;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);
	fake_camera = fake_usb_camera_get_fake_camera (device);

	/* Defaults of ArvFakeCamera */
	g_assert_cmpint (arv_device_get_integer_feature_value (arv_device, "Width", &error), ==,
			 ARV_FAKE_CAMERA_WIDTH_DEFAULT);
	g_assert_no_error (error);
	g_assert_cmpint (arv_device_get_integer_feature_value (arv_device, "Height", &error), ==,
			 ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	g_assert_no_error (error);
	g_assert_cmpint (arv_camera_get_pixel_format (camera, &error), ==, ARV_PIXEL_FORMAT_MONO_8);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_camera_get_payload (camera, &error), ==,
			  ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	g_assert_no_error (error);
	g_assert_cmpfloat (arv_device_get_float_feature_value (arv_device, "AcquisitionFrameRate", &error), ==,
			   ARV_FAKE_CAMERA_ACQUISITION_FRAME_RATE_DEFAULT);
	g_assert_no_error (error);

	g_assert_cmpstr (arv_device_get_string_feature_value (arv_device, "DeviceVendorName", &error), ==, "Aravis");
	g_assert_no_error (error);

	/* Writes reach ArvFakeCamera's registers, and the stream interface follows the payload size */
	arv_camera_set_region (camera, 0, 0, 720, 540, &error);
	g_assert_no_error (error);
	arv_camera_set_pixel_format (camera, ARV_PIXEL_FORMAT_MONO_16, &error);
	g_assert_no_error (error);

	arv_fake_camera_read_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value);
	g_assert_cmpuint (value, ==, 720);
	arv_fake_camera_read_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_HEIGHT, &value);
	g_assert_cmpuint (value, ==, 540);
	arv_fake_camera_read_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_PIXEL_FORMAT, &value);
	g_assert_cmphex (value, ==, ARV_PIXEL_FORMAT_MONO_16);

	g_assert_cmpuint (arv_camera_get_payload (camera, &error), ==, 720 * 540 * 2);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_camera_get_sirm_register (device, ARV_SIRM_REQ_PAYLOAD_SIZE), ==, 720 * 540 * 2);

	arv_device_set_float_feature_value (arv_device, "AcquisitionFrameRate", 200.0, &error);
	g_assert_no_error (error);
	arv_fake_camera_read_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_ACQUISITION_FRAME_PERIOD_US, &value);
	g_assert_cmpuint (value, ==, 5000);

	arv_device_set_boolean_feature_value (arv_device, "TestBoolean", TRUE, &error);
	g_assert_no_error (error);
	g_assert_cmpint (arv_device_get_integer_feature_value (arv_device, "TestRegister", &error), ==, 321);
	g_assert_no_error (error);
	arv_device_set_boolean_feature_value (arv_device, "TestBoolean", FALSE, &error);
	g_assert_no_error (error);
	g_assert_cmpint (arv_device_get_integer_feature_value (arv_device, "TestRegister", &error), ==, 123);
	g_assert_no_error (error);

	/* Raw register access through the control channel */
	g_assert_cmpuint (arv_device_read_register (arv_device, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value, &error),
			  ==, TRUE);
	g_assert_no_error (error);
	g_assert_cmpuint (GUINT32_FROM_BE (value), ==, 720);

	/* Out of the address map: the camera answers with an error status */
	g_assert_false (arv_device_read_register (arv_device, 0x40000000, &value, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR_INVALID_ADDRESS);
	g_clear_error (&error);

	g_clear_object (&camera);
}

typedef struct {
	guint32 width;
	guint32 height;
	ArvPixelFormat pixel_format;
} Geometry;

static const Geometry geometries[] = {
	{ 512, 512, ARV_PIXEL_FORMAT_MONO_8 },		/* Payload a multiple of 1024 */
	{ 720, 540, ARV_PIXEL_FORMAT_MONO_8 },		/* Payload not a multiple of 1024 */
	{ 1440, 1080, ARV_PIXEL_FORMAT_MONO_8 },	/* Payload larger than the 1 MiB transfer size */
	{ 640, 480, ARV_PIXEL_FORMAT_MONO_16 }
};

static void
acquisition (ArvUvUsbMode usb_mode)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;
	guint i;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	for (i = 0; i < G_N_ELEMENTS (geometries); i++) {
		guint64 frames_sent;

		arv_camera_set_region (camera, 0, 0, geometries[i].width, geometries[i].height, &error);
		g_assert_no_error (error);
		arv_camera_set_pixel_format (camera, geometries[i].pixel_format, &error);
		g_assert_no_error (error);

		frames_sent = fake_usb_device_get_stat (device, FAKE_USB_STAT_FRAMES_SENT);

		g_assert_cmpuint (fakeuv_acquire_frames (camera, device, usb_mode, N_BUFFERS, N_FRAMES, TIMEOUT_MS),
				  ==, N_FRAMES);

		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_FRAMES_SENT) - frames_sent, >=,
				  N_FRAMES);
		g_assert_false (fake_usb_camera_is_streaming (device));
		g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN),
				  ==, 0);
	}

	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_OVERFLOWS), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_API_MISUSE), ==, 0);

	g_clear_object (&camera);
}

static void
acquisition_async_test (void)
{
	acquisition (ARV_UV_USB_MODE_ASYNC);
}

static void
acquisition_sync_test (void)
{
	acquisition (ARV_UV_USB_MODE_SYNC);
}

static void
single_frame_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvBuffer *buffer;
	GError *error = NULL;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	buffer = arv_camera_acquisition (camera, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000, &error);
	g_assert_no_error (error);
	fakeuv_assert_frame (device, buffer, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
	g_clear_object (&buffer);

	g_clear_object (&camera);
}

static void
restart_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	/* Stop, then start again with another geometry and another mode, on the same device */
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_ASYNC, N_BUFFERS, N_FRAMES,
						 TIMEOUT_MS), ==, N_FRAMES);
	arv_camera_set_region (camera, 0, 0, 720, 540, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_SYNC, N_BUFFERS, N_FRAMES,
						 TIMEOUT_MS), ==, N_FRAMES);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_ASYNC, 1, 3, TIMEOUT_MS), ==, 3);

	g_clear_object (&camera);
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);

	/* Reopen: the camera keeps its registers while plugged */
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_camera_get_payload (camera, &error), ==, 720 * 540);
	g_assert_no_error (error);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_ASYNC, N_BUFFERS, N_FRAMES,
						 TIMEOUT_MS), ==, N_FRAMES);
	g_clear_object (&camera);

	/* Replug: a power cycled camera, back to its defaults */
	fake_usb_device_unplug (device);
	fake_usb_device_plug (device);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_camera_get_payload (camera, &error), ==,
			  ARV_FAKE_CAMERA_WIDTH_DEFAULT * ARV_FAKE_CAMERA_HEIGHT_DEFAULT);
	g_assert_no_error (error);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_SYNC, N_BUFFERS, N_FRAMES,
						 TIMEOUT_MS), ==, N_FRAMES);
	g_clear_object (&camera);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);
}

static void
software_trigger_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	size_t payload;
	guint i;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	/* No frame without a trigger, one frame per trigger */
	g_usleep (fakeuv_timeout_ms (50) * 1000);
	g_assert_null (arv_stream_try_pop_buffer (stream));
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_FRAMES_STARTED), ==, 0);

	for (i = 0; i < 3; i++) {
		arv_camera_software_trigger (camera, &error);
		g_assert_no_error (error);

		buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);
		fakeuv_assert_frame (device, buffer, ARV_FAKE_CAMERA_WIDTH_DEFAULT, ARV_FAKE_CAMERA_HEIGHT_DEFAULT,
				     ARV_PIXEL_FORMAT_MONO_8);
		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_FRAMES_STARTED), ==, 3);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
control_lost_cb (ArvDevice *device, gint *n_control_lost)
{
	g_atomic_int_inc (n_control_lost);
}

typedef struct {
	gint *n_control_lost;
	FakeuvStreamEvents *events;
} UnplugState;

static gboolean
unplug_done (gpointer user_data)
{
	UnplugState *state = user_data;

	return g_atomic_int_get (state->n_control_lost) > 0 && g_atomic_int_get (&state->events->n_exit) > 0;
}

/* Unplug with hotplug available, completions as on Linux. The platform preset sets the hotplug timing: on Linux the
 * DEVICE_LEFT event comes at once and before the failed transfers of its round, with the Windows hotplug build
 * 10 ms later and after them. */

static void
unplug_hotplug (FakeUsbPlatform platform)
{
	FakeuvStreamEvents events = {0};
	UnplugState state;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	gint n_control_lost = 0;
	gint n_input = 0, n_output = 0;
	guint n_success = 0, n_other = 0;
	size_t payload;
	guint i;

	fakeuv_test_reset ();

	/* Hotplug available, transfers failing as on Linux, cable pulled after the 3rd frame */
	fake_usb_set_platform (platform);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, TRUE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_device_set_unplug_errors (device, LIBUSB_TRANSFER_NO_DEVICE,
					   LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE);
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK), ==, 1);

	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (control_lost_cb),
			  &n_control_lost);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	fake_usb_device_unplug_after_frames (device, 3, 0);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	/* The hotplug callback reports the loss of control, and the stream thread ends by itself */
	state.n_control_lost = &n_control_lost;
	state.events = &events;
	g_assert_true (fakeuv_wait_for (unplug_done, &state, TIMEOUT_MS));
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 1);
	g_assert_false (fake_usb_device_is_plugged (device));

	/* Every buffer comes back: the frames sent before the unplug, the others failed or aborted */
	while ((buffer = arv_stream_try_pop_buffer (stream)) != NULL) {
		if (fakeuv_buffer_matches_frame (device, buffer))
			n_success++;
		else
			n_other++;
		g_object_unref (buffer);
	}
	/* (g_async_queue_length() is negative while a thread waits on the queue, the stream thread is gone here) */
	arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, NULL);
	g_assert_cmpint (n_input, >=, 0);
	g_assert_cmpuint (n_success, ==, 3);
	g_assert_cmpuint (n_success + n_other + n_input, ==, N_BUFFERS);

	/* The device is gone: stopping reports an error, and nothing hangs */
	arv_camera_stop_acquisition (camera, &error);
	g_clear_error (&error);

	g_object_unref (stream);
	g_clear_object (&camera);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);
}

static void
unplug_hotplug_linux_test (void)
{
	unplug_hotplug (FAKE_USB_PLATFORM_LINUX);
}

static void
unplug_hotplug_windows_test (void)
{
	unplug_hotplug (FAKE_USB_PLATFORM_WINDOWS);
}

static void
pending_ack_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");

	/* The camera asks for more time on the first commands, then answers */
	fake_usb_camera_set_pending_acks (device, 10, 100, 5);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_PENDING_ACKS), ==, 10);

	g_assert_cmpstr (arv_camera_get_vendor_name (camera, &error), ==, "Aravis");
	g_assert_no_error (error);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_ASYNC, N_BUFFERS, 3, TIMEOUT_MS),
			  ==, 3);

	g_clear_object (&camera);
}

/* Frame ids are 64 bit on USB3 Vision: no wrap at 65535 as with GigE Vision's 16 bit ids */

static void
frame_ids_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	guint64 first_id = G_GUINT64_CONSTANT (0x10000000000) - 2;
	size_t payload;
	guint i, mode;

	fakeuv_test_reset ();

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	for (mode = 0; mode < 2; mode++) {
		ArvUvUsbMode usb_mode = mode == 0 ? ARV_UV_USB_MODE_ASYNC : ARV_UV_USB_MODE_SYNC;

		fake_usb_camera_set_next_frame_id (device, first_id);

		arv_camera_uv_set_usb_mode (camera, usb_mode);
		stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
		g_assert_no_error (error);
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

		arv_camera_start_acquisition (camera, &error);
		g_assert_no_error (error);

		/* One buffer: consecutive frames */
		for (i = 0; i < 4; i++) {
			ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms
									    (TIMEOUT_MS) * 1000);

			fakeuv_assert_frame (device, buffer, ARV_FAKE_CAMERA_WIDTH_DEFAULT,
					     ARV_FAKE_CAMERA_HEIGHT_DEFAULT, ARV_PIXEL_FORMAT_MONO_8);
			if (i == 0)
				g_assert_cmpuint (arv_buffer_get_frame_id (buffer), >=, first_id);
			g_assert_cmpuint (arv_buffer_get_frame_id (buffer), >, G_MAXUINT32);

			arv_stream_push_buffer (stream, buffer);
		}

		arv_camera_stop_acquisition (camera, &error);
		g_assert_no_error (error);
		g_object_unref (stream);
	}

	g_clear_object (&camera);
}

/* GenDC containers, built by the camera from Aravis's ArvUvspGenDC* structures */

static void
gendc_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;
	gsize descriptor_size = fake_usb_camera_get_gendc_descriptor_size ();
	gsize image_size = 640 * 480;
	guint32 payload;
	guint i, mode;

	fakeuv_test_reset ();

	/* Not square, so that a width and a height swapped or confused show */
	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 640, 480, ARV_PIXEL_FORMAT_MONO_8);
	fake_usb_camera_set_payload_type (device, ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	/* The camera's GenICam PayloadSize does not count the GenDC descriptor, its SIRM does */
	payload = fake_usb_camera_get_sirm_register (device, ARV_SIRM_REQ_PAYLOAD_SIZE);
	g_assert_cmpuint (payload, ==, image_size + descriptor_size);
	g_assert_cmpuint (arv_camera_get_payload (camera, &error), ==, image_size);
	g_assert_no_error (error);

	for (mode = 0; mode < 2; mode++) {
		ArvUvUsbMode usb_mode = mode == 0 ? ARV_UV_USB_MODE_ASYNC : ARV_UV_USB_MODE_SYNC;
		ArvStream *stream;

		arv_camera_uv_set_usb_mode (camera, usb_mode);
		stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
		g_assert_no_error (error);
		for (i = 0; i < 3; i++)
			arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

		arv_camera_start_acquisition (camera, &error);
		g_assert_no_error (error);

		for (i = 0; i < 5; i++) {
			ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms
									    (TIMEOUT_MS) * 1000);
			const guint8 *data;
			const guint8 *image;
			size_t size = 0;
			guint32 checksum;
			gsize sent;

			g_assert_true (ARV_IS_BUFFER (buffer));
			g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SUCCESS);
			g_assert_cmpint (arv_buffer_get_payload_type (buffer), ==,
					 ARV_BUFFER_PAYLOAD_TYPE_GENDC_CONTAINER);
			g_assert_true (arv_buffer_has_gendc (buffer));

			data = arv_buffer_get_gendc_descriptor (buffer, &size);
			g_assert_nonnull (data);
			g_assert_cmpuint (size, ==, descriptor_size);
			g_assert_nonnull (arv_buffer_get_gendc_data (buffer, &size));
			g_assert_cmpuint (size, ==, image_size);

			/* The image is the part data, after the descriptor, as large as the part (both GenDC parsers of
			 * arvuvstream.c used to store the Y dimension into the width, and to give the size of the whole
			 * container) */
			image = arv_buffer_get_image_data (buffer, &size);
			g_assert_true (image == data + descriptor_size);
			g_assert_cmpuint (size, ==, image_size);
			g_assert_cmpint (arv_buffer_get_image_width (buffer), ==, 640);
			g_assert_cmpint (arv_buffer_get_image_height (buffer), ==, 480);
			g_assert_cmphex (arv_buffer_get_image_pixel_format (buffer), ==, ARV_PIXEL_FORMAT_MONO_8);
			g_assert_true (fake_usb_camera_get_frame_checksum (device, arv_buffer_get_frame_id (buffer),
									   &checksum, &sent));
			g_assert_cmpuint (sent, ==, image_size);
			g_assert_cmphex (fake_usb_checksum (image, image_size), ==, checksum);

			arv_stream_push_buffer (stream, buffer);
		}

		arv_camera_stop_acquisition (camera, &error);
		g_assert_no_error (error);
		g_object_unref (stream);
	}

	g_clear_object (&camera);
}

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuv/discovery", discovery_test);
	fakeuv_test_add ("/fakeuv/open", open_test);
	fakeuv_test_add ("/fakeuv/features", features_test);
	fakeuv_test_add ("/fakeuv/acquisition-async", acquisition_async_test);
	fakeuv_test_add ("/fakeuv/acquisition-sync", acquisition_sync_test);
	fakeuv_test_add ("/fakeuv/single-frame", single_frame_test);
	fakeuv_test_add ("/fakeuv/restart", restart_test);
	fakeuv_test_add ("/fakeuv/software-trigger", software_trigger_test);
	fakeuv_test_add ("/fakeuv/unplug-hotplug-linux", unplug_hotplug_linux_test);
	fakeuv_test_add ("/fakeuv/unplug-hotplug-windows", unplug_hotplug_windows_test);
	fakeuv_test_add ("/fakeuv/pending-ack", pending_ack_test);
	fakeuv_test_add ("/fakeuv/frame-ids", frame_ids_test);
	fakeuv_test_add ("/fakeuv/gendc", gendc_test);

	return fakeuv_test_run ();
}
