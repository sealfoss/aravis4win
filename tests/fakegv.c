/* SPDX-License-Identifier:Unlicense */

#include <glib.h>
#include <arv.h>

static ArvGvFakeCamera *simulator = NULL;
static ArvCamera *camera = NULL;

/* The fake camera listens on 127.0.0.1, but it also answers the discovery broadcasts sent through the other network
 * interfaces of the host. Whichever answer comes first, or last, it must be reached through the loopback interface,
 * which owns its subnet, as its traffic goes through it anyway. */

static void
assert_reached_through_loopback (ArvCamera *gv_camera)
{
        ArvDevice *device;
        GSocketAddress *interface_address;

        device = arv_camera_get_device (gv_camera);
        g_assert (ARV_IS_GV_DEVICE (device));

        interface_address = arv_gv_device_get_interface_address (ARV_GV_DEVICE (device));
        g_assert (G_IS_INET_SOCKET_ADDRESS (interface_address));
        g_assert_true (g_inet_address_get_is_loopback
                       (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (interface_address))));
}

static void
discovery_test (void)
{
        ArvCamera *test_camera;
        char *discovery_iface;
        int n_devices;
        int i;

        /* Opened in main() by name, through a discovery returning the first answer */
        assert_reached_through_loopback (camera);

        arv_gv_interface_set_discovery_interface_name("not-an-iface");

        arv_update_device_list();

        discovery_iface = arv_gv_interface_dup_discovery_interface_name();
        g_assert (discovery_iface != NULL);
        g_assert_cmpstr("not-an-iface", ==, discovery_iface);
        g_free (discovery_iface);

        test_camera = arv_camera_new ("Aravis-GVTest", NULL);
        g_assert (test_camera == NULL);

        arv_gv_interface_set_discovery_interface_name(NULL);

        arv_update_device_list();

        discovery_iface = arv_gv_interface_dup_discovery_interface_name();
        g_assert (discovery_iface == NULL);

        test_camera = arv_camera_new ("Aravis-GVTest", NULL);
        g_assert (ARV_IS_CAMERA(test_camera));
        /* Opened from the device list, filled with the last answer */
        assert_reached_through_loopback (test_camera);
        g_clear_object (&test_camera);

        n_devices = arv_get_n_devices ();

        for (i = 0; i < n_devices; i++) {
                if (g_strcmp0 (arv_get_device_vendor (i), "Aravis") == 0 &&
                    g_strcmp0 (arv_get_device_model (i), "Fake") == 0 &&
                    g_strcmp0 (arv_get_device_serial_nbr (i), "GVTest") == 0 &&
                    g_strcmp0 (arv_get_device_manufacturer_info (i), "none") == 0 &&
                    g_strcmp0 (arv_get_device_id (i), "Aravis-Fake-GVTest") == 0 &&
                    g_strcmp0 (arv_get_device_physical_id (i), "00:00:00:00:00:00") == 0)
                        return;
        }

        g_assert_not_reached ();
}

static void
register_test (void)
{
	ArvDevice *device;
	int int_value;
	double dbl_value;
	double boolean_value;

	device = arv_camera_get_device (camera);
	g_assert (ARV_IS_GV_DEVICE (device));

	/* Check default */
	int_value = arv_device_get_integer_feature_value (device, "Width", NULL);
	g_assert_cmpint (int_value, ==, ARV_FAKE_CAMERA_WIDTH_DEFAULT);

	arv_device_set_integer_feature_value (device, "Width", 1024, NULL);
	int_value = arv_device_get_integer_feature_value (device, "Width", NULL);
	g_assert_cmpint (int_value, ==, 1024);

	/* Check default */
	int_value = arv_device_get_integer_feature_value (device, "Height", NULL);
	g_assert_cmpint (int_value, ==, ARV_FAKE_CAMERA_HEIGHT_DEFAULT);

	arv_device_set_integer_feature_value (device, "Height", 1024, NULL);
	int_value = arv_device_get_integer_feature_value (device, "Height", NULL);
	g_assert_cmpint (int_value, ==, 1024);

	int_value = arv_device_get_integer_feature_value (device, "BinningHorizontal", NULL);
	g_assert_cmpint (int_value, ==, ARV_FAKE_CAMERA_BINNING_HORIZONTAL_DEFAULT);
	int_value = arv_device_get_integer_feature_value (device, "BinningVertical", NULL);
	g_assert_cmpint (int_value, ==, ARV_FAKE_CAMERA_BINNING_VERTICAL_DEFAULT);
	int_value = arv_device_get_integer_feature_value (device, "PixelFormat", NULL);
	g_assert_cmpint (int_value, ==, ARV_FAKE_CAMERA_PIXEL_FORMAT_DEFAULT);

	dbl_value = arv_device_get_float_feature_value (device, "AcquisitionFrameRate", NULL);
	g_assert_cmpfloat (dbl_value, ==, ARV_FAKE_CAMERA_ACQUISITION_FRAME_RATE_DEFAULT);
	dbl_value = arv_device_get_float_feature_value (device,  "ExposureTimeAbs", NULL);
	g_assert_cmpfloat (dbl_value, ==, ARV_FAKE_CAMERA_EXPOSURE_TIME_US_DEFAULT);

	int_value = arv_device_get_integer_feature_value (device, "GainRaw", NULL);
	g_assert_cmpint (int_value, ==, 0);
	int_value = arv_device_get_integer_feature_value (device, "GainAuto", NULL);
	g_assert_cmpint (int_value, ==, 1);

	int_value = arv_device_get_integer_feature_value (device, "PayloadSize", NULL);
	g_assert_cmpint (int_value, ==, 1024 * 1024);

	arv_device_set_boolean_feature_value (device, "TestBoolean", FALSE, NULL);
	boolean_value = arv_device_get_boolean_feature_value (device, "TestBoolean", NULL);
	g_assert_cmpint (boolean_value, ==, FALSE);
	int_value = arv_device_get_integer_feature_value (device, "TestRegister", NULL);
	g_assert_cmpint (int_value, ==, 123);

	arv_device_set_boolean_feature_value (device, "TestBoolean", TRUE, NULL);
	boolean_value = arv_device_get_boolean_feature_value (device, "TestBoolean", NULL);
	g_assert_cmpint (boolean_value, ==, TRUE);
	int_value = arv_device_get_integer_feature_value (device, "TestRegister", NULL);
	g_assert_cmpint (int_value, ==, 321);
}

static void
acquisition_test (void)
{
	GError *error = NULL;
	ArvBuffer *buffer;
	gint x, y, width, height;
        const char *ignore_buffer;

        ignore_buffer = g_getenv("ARV_TEST_IGNORE_BUFFER");

	buffer = arv_camera_acquisition (camera, 0, &error);
	g_assert (error == NULL);
	g_assert (ARV_IS_BUFFER (buffer));

        if (ignore_buffer == NULL && arv_buffer_get_status(buffer) == ARV_BUFFER_STATUS_SUCCESS) {
                arv_buffer_get_image_region (buffer, &x, &y, &width, &height);

                g_assert_cmpint (x, ==, 0);
                g_assert_cmpint (y, ==, 0);
                g_assert_cmpint (width, ==, 1024);
                g_assert_cmpint (height, ==, 1024);

                g_assert_cmpint (arv_buffer_get_image_x (buffer), ==, 0);
                g_assert_cmpint (arv_buffer_get_image_y (buffer), ==, 0);
                g_assert_cmpint (arv_buffer_get_image_width (buffer), ==, 1024);
                g_assert_cmpint (arv_buffer_get_image_height (buffer), ==, 1024);

                g_assert (arv_buffer_get_image_pixel_format (buffer) == ARV_PIXEL_FORMAT_MONO_8);
        }

	g_clear_object (&buffer);
}

static void
new_buffer_cb (ArvStream *stream, unsigned *buffer_count)
{
	ArvBuffer *buffer;

	buffer = arv_stream_try_pop_buffer (stream);
	if (buffer != NULL) {
		(*buffer_count)++;
		if (*buffer_count == 10) {
			/* Sleep after the last buffer was received, in order
			 * to keep a reference to stream while the main loop
			 * ends. If the main is able to unref stream while
			 * this signal callback is still waiting, stream will
			 * be finalized in its stream thread contex (because
			 * g_signal_emit holds a reference to stream), leading
			 * to a deadlock. */
			g_usleep (1000000);
		}
		arv_stream_push_buffer (stream, buffer);
	}
}

static void
stream_test (void)
{
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	unsigned buffer_count = 0;
	unsigned i;

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert (ARV_IS_STREAM (stream));
	g_assert (error == NULL);

	payload = arv_camera_get_payload (camera, NULL);

	for (i = 0; i < 5; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	g_signal_connect (stream, "new-buffer", G_CALLBACK (new_buffer_cb), &buffer_count);
	arv_stream_set_emit_signals (stream, TRUE);

	arv_camera_start_acquisition (camera, NULL);

	while (buffer_count < 10)
		g_usleep (1000);

	arv_camera_stop_acquisition (camera, NULL);
	/* The following will block until the signal callback returns
	 * which avoids a race and possible deadlock.
	 */
	arv_stream_set_emit_signals (stream, FALSE);

	g_clear_object (&stream);

	/* For actually testing the deadlock condition (see comment in
	 * new_buffer_cb), one must wait a bit before leaving this test,
	 * because otherwise the stream thread will be killed while sleeping. */
	g_usleep (2000000);
}

#define N_BUFFERS	5

static struct {
	int width;
	int height;
} rois[] = { {100, 100}, {200, 200}, {300,300} };

static void
dynamic_roi_test (void)
{
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	unsigned buffer_count = 0;
	unsigned i, j;

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert (ARV_IS_STREAM (stream));
	g_assert (error == NULL);

	payload = arv_camera_get_payload (camera, NULL);

	g_signal_connect (stream, "new-buffer", G_CALLBACK (new_buffer_cb), &buffer_count);
	arv_stream_set_emit_signals (stream, TRUE);

	for (j = 0; j < G_N_ELEMENTS (rois); j++) {
		int height, width;

                if (j > 0)
                        g_assert (arv_stream_delete_buffers (stream) == N_BUFFERS);

		buffer_count = 0;

		arv_camera_set_region (camera, 0, 0, rois[j].width , rois[j].height, NULL);
		arv_camera_get_region (camera, NULL, NULL, &width, &height, NULL);

		g_assert (width == rois[j].width);
		g_assert (height == rois[j].height);

		payload = arv_camera_get_payload (camera, NULL);

		for (i = 0; i < N_BUFFERS; i++)
			arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

		arv_camera_start_acquisition (camera, NULL);

		while (buffer_count < 10) {
			g_usleep (10000);
		}

		arv_camera_stop_acquisition (camera, NULL);
	}

	arv_stream_set_emit_signals (stream, FALSE);

	g_clear_object (&stream);
}

/* A stream disables its stream channel when it is destroyed, unless a newer stream of the device configured the
 * channel since: the channel goes on sending to the newer stream. An application may destroy a stream after creating
 * the next one, for example one that still holds the former stream of aravissrc, read from its 'stream' property, when
 * the element creates a new stream at a caps change. */

static gint64
get_stream_channel_port (ArvDevice *device)
{
	GError *error = NULL;
	gint64 port;

	port = arv_device_get_integer_feature_value (device, "ArvGevSCPHostPort", &error);
	g_assert_no_error (error);

	return port;
}

static void
stream_channel_test (void)
{
	ArvDevice *device = arv_camera_get_device (camera);
	ArvStream *former_stream;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	gint64 former_port;
	gint64 port;
	size_t payload;
	unsigned i;

	former_stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GV_STREAM (former_stream));
	former_port = get_stream_channel_port (device);
	g_assert_cmpint (former_port, !=, 0);

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GV_STREAM (stream));
	port = get_stream_channel_port (device);
	g_assert_cmpint (port, !=, 0);
	g_assert_cmpint (port, !=, former_port);

	g_clear_object (&former_stream);
	g_assert_cmpint (get_stream_channel_port (device), ==, port);

	/* Frames reach the newer stream */
	payload = arv_camera_get_payload (camera, NULL);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	buffer = arv_stream_timeout_pop_buffer (stream, 5 * G_USEC_PER_SEC);
	g_assert_true (ARV_IS_BUFFER (buffer));
	g_object_unref (buffer);
	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	/* The last stream disables the channel */
	g_clear_object (&stream);
	g_assert_cmpint (get_stream_channel_port (device), ==, 0);
}

/* The former stream may also be destroyed in another thread while the next one is being created. Here it is destroyed
 * as soon as the fake camera has the port of the next stream, before that stream took the channel over: the channel
 * must still send to the next stream once it is created. */

#define N_CONCURRENT_STREAMS	20

typedef struct {
	ArvFakeCamera *fake_camera;
	ArvStream *former_stream;
	guint16 former_port;
	gint stream_created;
} DestroyFormerStreamData;

/* Read from the memory of the fake camera, without a GVCP transaction */

static guint16
get_fake_camera_stream_port (ArvFakeCamera *fake_camera)
{
	GSocketAddress *address;
	guint16 port;

	address = arv_fake_camera_get_stream_address (fake_camera);
	port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (address));
	g_object_unref (address);

	return port;
}

static gpointer
destroy_former_stream (gpointer user_data)
{
	DestroyFormerStreamData *data = user_data;

	/* Busy wait, as the next stream takes the channel over as soon as the fake camera acknowledges its port write */
	while (get_fake_camera_stream_port (data->fake_camera) == data->former_port &&
	       !g_atomic_int_get (&data->stream_created))
		;

	g_object_unref (data->former_stream);

	return NULL;
}

static void
stream_channel_concurrent_test (void)
{
	ArvDevice *device = arv_camera_get_device (camera);
	ArvFakeCamera *fake_camera = arv_gv_fake_camera_get_fake_camera (simulator);
	ArvStream *former_stream;
	GError *error = NULL;
	unsigned i;

	former_stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GV_STREAM (former_stream));

	for (i = 0; i < N_CONCURRENT_STREAMS; i++) {
		DestroyFormerStreamData data;
		ArvStream *stream;
		GThread *thread;

		data.fake_camera = fake_camera;
		data.former_stream = former_stream;
		data.former_port = arv_gv_stream_get_port (ARV_GV_STREAM (former_stream));
		data.stream_created = FALSE;
		g_assert_cmpuint (get_fake_camera_stream_port (fake_camera), ==, data.former_port);

		thread = g_thread_new ("destroy-former-stream", destroy_former_stream, &data);
		stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
		g_atomic_int_set (&data.stream_created, TRUE);
		g_thread_join (thread);
		g_assert_no_error (error);
		g_assert_true (ARV_IS_GV_STREAM (stream));

		g_assert_cmpint (get_stream_channel_port (device), ==, arv_gv_stream_get_port (ARV_GV_STREAM (stream)));

		former_stream = stream;
	}

	/* The last stream disables the channel */
	g_clear_object (&former_stream);
	g_assert_cmpint (get_stream_channel_port (device), ==, 0);
}

int
main (int argc, char *argv[])
{
	int result;

	g_test_init (&argc, &argv, NULL);

	arv_set_fake_camera_genicam_filename (GENICAM_FILENAME);

	simulator = arv_gv_fake_camera_new ("127.0.0.1", "GVTest");
	g_assert (ARV_IS_GV_FAKE_CAMERA (simulator));

	camera = arv_camera_new ("Aravis-GVTest", NULL);
	g_assert (ARV_IS_CAMERA (camera));

	arv_update_device_list ();

	g_test_add_func ("/fakegv/discovery", discovery_test);
	g_test_add_func ("/fakegv/device_registers", register_test);
	g_test_add_func ("/fakegv/acquisition", acquisition_test);
	g_test_add_func ("/fakegv/stream", stream_test);
	g_test_add_func ("/fakegv/dynamic_roi", dynamic_roi_test);
	g_test_add_func ("/fakegv/stream-channel", stream_channel_test);
	g_test_add_func ("/fakegv/stream-channel-concurrent", stream_channel_concurrent_test);

	result = g_test_run();

	g_object_unref (camera);

	g_object_unref (simulator);

	arv_shutdown ();

	return result;
}

