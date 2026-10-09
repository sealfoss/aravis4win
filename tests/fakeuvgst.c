/* SPDX-License-Identifier:Unlicense */

/* aravissrc against the fake libusb. The element and the device provider are compiled into this program from gst/,
 * registered statically, and driven with GstHarness: no GStreamer plugin is loaded (GST_PLUGIN_SYSTEM_PATH_1_0 is
 * empty in the test environment). Built when the GStreamer plugin is, if gstreamer-check-1.0 is available.
 *
 * The cases run aravissrc over USB3Vision without a camera: both USB modes, the USB settings whatever their order
 * relative to camera-name, frames lent downstream across a stop and a renegotiation, a renegotiation while the
 * application holds the previous stream, a camera unplugged while streaming, and the device provider while the device
 * list changes, while a camera streams, or when the list gives an identifier several times. */

#include "fakeuvtest.h"
#include <gst/gst.h>
#include <gst/check/gstharness.h>
#include <string.h>

GST_PLUGIN_STATIC_DECLARE (aravis);

#define WIDTH			640
#define HEIGHT			480
/* At 100 frames per second, to keep the cases short */
#define CAPS			"video/x-raw, format=(string)GRAY8, width=(int)640, height=(int)480," \
				" framerate=(fraction)100/1"
#define SMALL_WIDTH		320
#define SMALL_HEIGHT		240
#define SMALL_CAPS		"video/x-raw, format=(string)GRAY8, width=(int)320, height=(int)240," \
				" framerate=(fraction)100/1"
/* How long to wait for something that should happen, scaled by fakeuv_timeout_ms () */
#define TIMEOUT_MS		5000

/* An aravissrc with a fake camera, and a bus of its own for its error messages, as it has no pipeline. The harness
 * plays it. */

typedef struct {
	FakeUsbDevice *device;
	char *device_id;
	GstElement *element;
	GstBus *bus;
	GstHarness *harness;
} Rig;

static void
rig_init (Rig *rig)
{
	rig->device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (rig->device, WIDTH, HEIGHT, ARV_PIXEL_FORMAT_MONO_8);
	rig->device_id = fake_usb_camera_dup_device_id (rig->device);

	rig->element = gst_element_factory_make ("aravissrc", NULL);
	g_assert_nonnull (rig->element);
	rig->bus = gst_bus_new ();
	gst_element_set_bus (rig->element, rig->bus);
	rig->harness = NULL;
}

static void
rig_play (Rig *rig, const char *caps)
{
	rig->harness = gst_harness_new_with_element (rig->element, NULL, "src");
	/* aravissrc is live and syncs on the clock: the default GstTestClock would never let it go */
	gst_harness_use_systemclock (rig->harness);
	/* Without sink caps, bayer would be fixated */
	gst_harness_set_sink_caps_str (rig->harness, caps);
	gst_harness_play (rig->harness);
}

static void
rig_assert_no_error (Rig *rig)
{
	GstMessage *message = gst_bus_pop_filtered (rig->bus, GST_MESSAGE_ERROR);

	if (message != NULL) {
		GError *error = NULL;

		gst_message_parse_error (message, &error, NULL);
		g_assert_no_error (error);
	}
}

/* Pulls the next buffer whose size is size, dropping the others; fails on timeout or error message */

static GstBuffer *
rig_pull (Rig *rig, gsize size)
{
	gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	gsize last_size = 0;
	guint n_dropped = 0;

	for (;;) {
		GstBuffer *buffer = gst_harness_try_pull (rig->harness);

		if (buffer != NULL) {
			if (gst_buffer_get_size (buffer) == size)
				return buffer;
			last_size = gst_buffer_get_size (buffer);
			n_dropped++;
			gst_buffer_unref (buffer);
		} else
			g_usleep (500);

		rig_assert_no_error (rig);
		if (g_get_monotonic_time () >= end_time)
			g_error ("No buffer of %" G_GSIZE_FORMAT " bytes within %u ms, %u others dropped, the last one of %"
				 G_GSIZE_FORMAT " bytes", size, fakeuv_timeout_ms (TIMEOUT_MS), n_dropped, last_size);
	}
}

static void
rig_pull_frames (Rig *rig, gsize size, guint n_frames)
{
	guint i;

	for (i = 0; i < n_frames; i++)
		gst_buffer_unref (rig_pull (rig, size));
}

/* Stops and frees the element: everything it took from libusb is released, held frames or not */

static void
rig_teardown (Rig *rig)
{
	if (rig->harness != NULL)
		gst_harness_teardown (rig->harness);
	gst_element_set_bus (rig->element, NULL);
	gst_object_unref (rig->element);
	gst_object_unref (rig->bus);
	g_free (rig->device_id);

	g_assert_cmpuint (fake_usb_device_get_n_open_handles (rig->device), ==, 0);
	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);
}

/* A frame downstream, and a copy of its data at that time */

typedef struct {
	GstBuffer *buffer;
	GBytes *snapshot;
} Frame;

/* Takes the buffer */

static Frame *
frame_new (GstBuffer *buffer)
{
	Frame *frame = g_new0 (Frame, 1);
	gpointer data = NULL;
	gsize size = 0;

	frame->buffer = buffer;
	gst_buffer_extract_dup (buffer, 0, gst_buffer_get_size (buffer), &data, &size);
	frame->snapshot = g_bytes_new_take (data, size);

	return frame;
}

/* The frame still has the data it had when received, the default pattern of ArvFakeCamera: (x + y + n) % 255 */

static void
frame_assert_intact (Frame *frame, guint width, guint height)
{
	GstMapInfo map;
	guint x, y;

	g_assert_true (gst_buffer_map (frame->buffer, &map, GST_MAP_READ));
	g_assert_cmpuint (map.size, ==, (gsize) width * height);
	g_assert_cmpmem (map.data, map.size, g_bytes_get_data (frame->snapshot, NULL), g_bytes_get_size (frame->snapshot));
	for (y = 0; y < height; y += 7)
		for (x = 0; x < width; x += 5)
			g_assert_cmpuint (map.data[y * width + x], ==, (x + y + map.data[0]) % 255);
	gst_buffer_unmap (frame->buffer, &map);
}

static void
frame_free (Frame *frame)
{
	gst_buffer_unref (frame->buffer);
	g_bytes_unref (frame->snapshot);
	g_free (frame);
}

/* Asynchronous USB mode, the default: frames flow, with increasing timestamps */

static void
streaming_test (void)
{
	GstClockTime last_pts = GST_CLOCK_TIME_NONE;
	GstStructure *structure;
	GstCaps *caps;
	Rig rig;
	gint width = 0, height = 0;
	guint i;

	rig_init (&rig);
	g_object_set (rig.element, "camera-name", rig.device_id, NULL);
	rig_play (&rig, "video/x-raw, format=(string)GRAY8");

	for (i = 0; i < 20; i++) {
		GstBuffer *buffer = rig_pull (&rig, WIDTH * HEIGHT);

		g_assert_true (GST_BUFFER_PTS_IS_VALID (buffer));
		if (GST_CLOCK_TIME_IS_VALID (last_pts))
			g_assert_cmpuint (GST_BUFFER_PTS (buffer), >, last_pts);
		last_pts = GST_BUFFER_PTS (buffer);
		gst_buffer_unref (buffer);
	}

	caps = gst_pad_get_current_caps (rig.harness->sinkpad);
	g_assert_nonnull (caps);
	structure = gst_caps_get_structure (caps, 0);
	g_assert_cmpstr (gst_structure_get_name (structure), ==, "video/x-raw");
	g_assert_cmpstr (gst_structure_get_string (structure, "format"), ==, "GRAY8");
	g_assert_true (gst_structure_get_int (structure, "width", &width));
	g_assert_true (gst_structure_get_int (structure, "height", &height));
	g_assert_cmpint (width, ==, WIDTH);
	g_assert_cmpint (height, ==, HEIGHT);
	gst_caps_unref (caps);

	rig_assert_no_error (&rig);
	rig_teardown (&rig);

	g_assert_cmpuint (fake_usb_device_get_call_count (rig.device, FAKE_USB_CALL_SUBMIT_TRANSFER), >, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (rig.device, FAKE_USB_STAT_FRAMES_SENT), >=, 20);
}

/* usb-mode, before or after camera-name, which opens the camera: synchronous transfers only */

static void
usb_mode_test (void)
{
	guint order;

	for (order = 0; order < 2; order++) {
		Rig rig;

		fakeuv_test_reset ();

		rig_init (&rig);
		if (order == 0) {
			gst_util_set_object_arg (G_OBJECT (rig.element), "usb-mode", "sync");
			g_object_set (rig.element, "camera-name", rig.device_id, NULL);
		} else {
			g_object_set (rig.element, "camera-name", rig.device_id, NULL);
			gst_util_set_object_arg (G_OBJECT (rig.element), "usb-mode", "sync");
		}
		rig_play (&rig, CAPS);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 10);
		rig_teardown (&rig);

		g_assert_cmpuint (fake_usb_device_get_call_count (rig.device, FAKE_USB_CALL_SUBMIT_TRANSFER), ==, 0);
		g_assert_cmpuint (fake_usb_device_get_stat (rig.device, FAKE_USB_STAT_FRAMES_SENT), >=, 10);
	}
}

/* usb-maximum-transfer-size, before or after camera-name: no stream read is larger. The library takes the largest size
 * the property accepts. */

static void
transfer_size_test (void)
{
	static const struct {
		guint size;
		gboolean after_camera_name;
	} cases[] = {
		{65536, FALSE},
		{65536, TRUE},
		{64 * 1024 * 1024, TRUE},
		{0, TRUE}
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS (cases); i++) {
		guint64 max_read;
		guint size = 1;
		Rig rig;

		fakeuv_test_reset ();

		rig_init (&rig);
		if (!cases[i].after_camera_name)
			g_object_set (rig.element, "usb-maximum-transfer-size", cases[i].size, NULL);
		g_object_set (rig.element, "camera-name", rig.device_id, NULL);
		if (cases[i].after_camera_name)
			g_object_set (rig.element, "usb-maximum-transfer-size", cases[i].size, NULL);
		rig_play (&rig, CAPS);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 5);
		g_object_get (rig.element, "usb-maximum-transfer-size", &size, NULL);
		rig_teardown (&rig);

		max_read = fake_usb_device_get_stat (rig.device, FAKE_USB_STAT_MAX_STREAM_READ);
		g_test_message ("usb-maximum-transfer-size=%u %s camera-name: largest read %" G_GUINT64_FORMAT,
				cases[i].size, cases[i].after_camera_name ? "after" : "before", max_read);

		g_assert_cmpuint (size, ==, cases[i].size);
		if (cases[i].size == 0 || cases[i].size >= WIDTH * HEIGHT)
			/* The default size, 1 MiB, or more: whole payloads */
			g_assert_cmpuint (max_read, >=, WIDTH * HEIGHT);
		else
			g_assert_cmpuint (max_read, <=, cases[i].size);
	}
}

/* Frames lent downstream by the asynchronous stream stay intact while it goes on, and after it stops */

static void
frames_held_after_stop_test (void)
{
	GPtrArray *frames;
	Rig rig;
	guint i;

	rig_init (&rig);
	g_object_set (rig.element, "camera-name", rig.device_id, NULL);
	rig_play (&rig, CAPS);

	frames = g_ptr_array_new_with_free_func ((GDestroyNotify) frame_free);
	for (i = 0; i < 30; i++) {
		GstBuffer *buffer = rig_pull (&rig, WIDTH * HEIGHT);

		if (i % 10 == 0)
			g_ptr_array_add (frames, frame_new (buffer));
		else
			gst_buffer_unref (buffer);
	}

	/* More frames than the stream has buffers */
	rig_pull_frames (&rig, WIDTH * HEIGHT, 60);
	for (i = 0; i < frames->len; i++)
		frame_assert_intact (g_ptr_array_index (frames, i), WIDTH, HEIGHT);

	rig_assert_no_error (&rig);
	rig_teardown (&rig);

	for (i = 0; i < frames->len; i++)
		frame_assert_intact (g_ptr_array_index (frames, i), WIDTH, HEIGHT);
	g_ptr_array_unref (frames);

	g_assert_cmpuint (fake_usb_device_get_call_count (rig.device, FAKE_USB_CALL_SUBMIT_TRANSFER), >, 0);
}

/* The same across a renegotiation, which replaces the stream */

static void
frames_held_across_renegotiation_test (void)
{
	GPtrArray *frames;
	Rig rig;
	guint i;

	rig_init (&rig);
	g_object_set (rig.element, "camera-name", rig.device_id, NULL);
	rig_play (&rig, CAPS);

	frames = g_ptr_array_new_with_free_func ((GDestroyNotify) frame_free);
	for (i = 0; i < 3; i++)
		g_ptr_array_add (frames, frame_new (rig_pull (&rig, WIDTH * HEIGHT)));

	gst_harness_set_sink_caps_str (rig.harness, SMALL_CAPS);
	rig_pull_frames (&rig, SMALL_WIDTH * SMALL_HEIGHT, 30);

	for (i = 0; i < frames->len; i++)
		frame_assert_intact (g_ptr_array_index (frames, i), WIDTH, HEIGHT);

	/* Released while the next stream runs */
	g_ptr_array_unref (frames);
	rig_pull_frames (&rig, SMALL_WIDTH * SMALL_HEIGHT, 30);

	rig_assert_no_error (&rig);
	rig_teardown (&rig);
}

/* Counts the acquisition starts of each USB3Vision stream, in the data of the stream: the first call installs a hook on
 * the start_acquisition () of the ArvUvStream class, which then stays */

#define UV_STREAM_N_STARTS_KEY	"fakeuvgst-n-starts"

static gboolean (*uv_stream_parent_start_acquisition) (ArvStream *stream, GError **error) = NULL;

static gboolean
uv_stream_start_acquisition (ArvStream *stream, GError **error)
{
	gint n_starts = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (stream), UV_STREAM_N_STARTS_KEY));

	g_object_set_data (G_OBJECT (stream), UV_STREAM_N_STARTS_KEY, GINT_TO_POINTER (n_starts + 1));

	return uv_stream_parent_start_acquisition (stream, error);
}

static gint
uv_stream_count_starts (ArvStream *stream)
{
	ArvStreamClass *stream_class = g_type_class_ref (ARV_TYPE_UV_STREAM);

	if (stream_class->start_acquisition != uv_stream_start_acquisition) {
		/* Keeps the class reference */
		uv_stream_parent_start_acquisition = stream_class->start_acquisition;
		stream_class->start_acquisition = uv_stream_start_acquisition;
	} else
		g_type_class_unref (stream_class);

	return stream != NULL ? GPOINTER_TO_INT (g_object_get_data (G_OBJECT (stream), UV_STREAM_N_STARTS_KEY)) : 0;
}

/* An application following the stream property of aravissrc through notify::stream, as one reading its statistics
 * would, but ignoring the notifications of the element releasing its stream: it holds the stream of the element, and
 * releases it once the element has created the next one, or with keep_first, holds the first one until the end. It
 * counts the streams the element started more than once, from uv_stream_count_starts (). The notifications come from
 * the streaming thread. */

typedef struct {
	gboolean keep_first;
	ArvStream *stream;
	gint n_streams;
	gint n_restarted_streams;
} StreamHolder;

static void
stream_holder_check (StreamHolder *holder, ArvStream *stream)
{
	if (uv_stream_count_starts (stream) != 1)
		g_atomic_int_inc (&holder->n_restarted_streams);
}

static void
stream_holder_notify (GstElement *element, GParamSpec *pspec, StreamHolder *holder)
{
	ArvStream *stream = NULL;

	g_object_get (element, "stream", &stream, NULL);
	if (stream == NULL)
		return;

	g_atomic_int_inc (&holder->n_streams);
	if (holder->stream == NULL)
		holder->stream = g_steal_pointer (&stream);
	else if (!holder->keep_first) {
		/* The element has started the next stream: the previous one stays stopped */
		stream_holder_check (holder, holder->stream);
		g_object_unref (holder->stream);
		holder->stream = g_steal_pointer (&stream);
	}

	g_clear_object (&stream);
}

/* Renegotiations while the application holds a stream of aravissrc. The element stops and starts its own stream only,
 * so each of its streams is started once, the held ones included: arv_camera_start_acquisition () would restart every
 * stream of the device, and the old stream, restarted, would compete with the new one for the frames. The library
 * keeps the stream interface of the camera, and WinUSB RAW_IO, to the stream that set it up last when an older stream
 * stops or is released: /fakeuvstream/stale-stream and /fakeuvstream/stale-stream-released check that part. */

static void
renegotiation_held_stream_test (void)
{
	static const struct {
		const char *usb_mode;
		gboolean keep_first;
	} cases[] = {
		{"async", FALSE},
		{"sync", FALSE},
		{"async", TRUE},
		{"sync", TRUE}
	};
	static const struct {
		const char *caps;
		gsize size;
	} changes[] = {
		{SMALL_CAPS, SMALL_WIDTH * SMALL_HEIGHT},
		{CAPS, WIDTH * HEIGHT},
		{SMALL_CAPS, SMALL_WIDTH * SMALL_HEIGHT}
	};
	guint i, j;

	/* Before the first stream of these cases */
	uv_stream_count_starts (NULL);

	for (i = 0; i < G_N_ELEMENTS (cases); i++) {
		StreamHolder holder = {cases[i].keep_first, NULL, 0, 0};
		Rig rig;

		fakeuv_test_reset ();
		fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);

		rig_init (&rig);
		gst_util_set_object_arg (G_OBJECT (rig.element), "usb-mode", cases[i].usb_mode);
		g_object_set (rig.element, "camera-name", rig.device_id, NULL);
		g_signal_connect (rig.element, "notify::stream", G_CALLBACK (stream_holder_notify), &holder);
		rig_play (&rig, CAPS);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 5);

		for (j = 0; j < G_N_ELEMENTS (changes); j++) {
			g_test_message ("usb-mode=%s%s, caps change %u", cases[i].usb_mode,
					cases[i].keep_first ? ", first stream held" : "", j + 1);
			gst_harness_set_sink_caps_str (rig.harness, changes[j].caps);
			rig_pull_frames (&rig, changes[j].size, 20);
		}

		rig_assert_no_error (&rig);

		/* Every stream of the element was started once, the one still held included */
		g_signal_handlers_disconnect_by_data (rig.element, &holder);
		stream_holder_check (&holder, holder.stream);
		g_assert_cmpint (holder.n_streams, ==, 1 + G_N_ELEMENTS (changes));
		g_assert_cmpint (holder.n_restarted_streams, ==, 0);

		/* The held stream keeps the device open */
		g_clear_object (&holder.stream);

		rig_teardown (&rig);
	}
}

/* An application following the stream or the camera property of aravissrc, as gst/pipelines.md says: it holds the
 * object of the property, and releases it at each notification, including those of the element releasing it, when
 * the property is NULL */

typedef struct {
	GObject *object;
	gint n_objects;
} Follower;

static void
follower_notify (GstElement *element, GParamSpec *pspec, Follower *follower)
{
	GObject *object = NULL;

	g_object_get (element, g_param_spec_get_name (pspec), &object, NULL);
	if (object != NULL)
		follower->n_objects++;

	g_clear_object (&follower->object);
	follower->object = object;
}

/* The stream and the camera keep the device open, which WinUSB lets a single handle open: the application releases them
 * when aravissrc stops, so that the camera opens again at the next start */

static void
restart_followed_objects_test (void)
{
	static const char *property_names[] = {"stream", "camera"};
	guint i;

	for (i = 0; i < G_N_ELEMENTS (property_names); i++) {
		Follower follower = {NULL, 0};
		char *signal_name;
		Rig rig;

		fakeuv_test_reset ();
		fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);

		rig_init (&rig);
		fake_usb_device_set_exclusive_open (rig.device, TRUE);
		signal_name = g_strdup_printf ("notify::%s", property_names[i]);
		g_signal_connect (rig.element, signal_name, G_CALLBACK (follower_notify), &follower);
		g_free (signal_name);
		g_object_set (rig.element, "camera-name", rig.device_id, NULL);
		rig_play (&rig, CAPS);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 5);
		g_assert_nonnull (follower.object);

		g_test_message ("Restart, following the %s", property_names[i]);
		g_assert_cmpint (gst_element_set_state (rig.element, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
		gst_harness_play (rig.harness);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 5);
		rig_assert_no_error (&rig);

		/* One per start */
		g_assert_cmpint (follower.n_objects, ==, 2);
		g_signal_handlers_disconnect_by_data (rig.element, &follower);
		g_clear_object (&follower.object);

		rig_teardown (&rig);
	}
}

/* A camera unplugged while it streams: aravissrc posts an error naming it, which says it was lost, within about a
 * second, whether libusb reports the removal (hotplug) or the stream finds out. With buffer-timeout=0, the failed
 * frames of the unplug don't hide the cause.
 *
 * Without hotplug, the Windows preset reports the removal as LIBUSB_ERROR_IO and failing transfers, which the Windows
 * builds of Aravis only take for a loss (FAKEUV_IO_ERROR_IS_LOSS): elsewhere, libusb reports a removal as
 * LIBUSB_ERROR_NO_DEVICE. With the other builds, these cases end as gst/pipelines.md says a stream that stops does: no
 * complete frame within buffer-timeout, or with buffer-timeout=0, failed frames in a row. */

typedef struct {
	const char *name;
	FakeUsbPlatform platform;
	gboolean hotplug;
	const char *usb_mode;
	gboolean hang;
	const char *raw_io;
	gint buffer_timeout_ms;		/* -1: UNPLUG_BUFFER_TIMEOUT_MS */
	guint max_delay_ms;
} UnplugCase;

/* The buffer-timeout of the cases that don't set one: the default of aravissrc at 100 frames per second, scaled as the
 * delays of the fake are. With hanging transfers, a synchronous read of the stream only ends after the response time
 * the camera advertises, which the fake scales, and the stream checks the device then. */
#define UNPLUG_BUFFER_TIMEOUT_MS	2000

/* What aravissrc takes for a stream of failed frames with buffer-timeout=0 */
#define UNPLUG_MAX_FAILED_FRAMES	10

static const UnplugCase unplug_cases[] = {
	{"windows-async",		FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"async",	FALSE,	NULL,	-1,	1000},
	{"windows-async-raw-io",	FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"async",	FALSE,	"1",	-1,	1000},
	{"windows-async-no-raw-io",	FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"async",	FALSE,	"0",	-1,	1000},
	{"windows-sync",		FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"sync",		FALSE,	NULL,	-1,	1000},
	{"windows-hotplug-async",	FAKE_USB_PLATFORM_WINDOWS,	TRUE,	"async",	FALSE,	NULL,	-1,	1000},
	{"linux-hotplug-async",		FAKE_USB_PLATFORM_LINUX,	TRUE,	"async",	FALSE,	NULL,	-1,	1000},
	{"linux-hotplug-sync",		FAKE_USB_PLATFORM_LINUX,	TRUE,	"sync",		FALSE,	NULL,	-1,	1000},
	/* Transfers that never end after the unplug, without hotplug: the stream checks the device after a second */
	{"windows-async-hang",		FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"async",	TRUE,	NULL,	-1,	2000},
	{"windows-sync-hang",		FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"sync",		TRUE,	NULL,	-1,	2000},
	/* The transfers in flight fail at once, before the stream checks the device */
	{"windows-async-wait-forever",	FAKE_USB_PLATFORM_WINDOWS,	FALSE,	"async",	FALSE,	NULL,	0,	1000},
};

static void
unplug_test (void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (unplug_cases); i++) {
		const UnplugCase *unplug_case = &unplug_cases[i];
		GstMessage *message = NULL;
		GError *error = NULL;
		char *text;
		gint64 unplug_time, end_time;
		gint64 delay_ms;
		gboolean is_loss;
		guint buffer_timeout_ms;
		Rig rig;

		/* Without hotplug, the Windows preset reports the removal as LIBUSB_ERROR_IO */
		is_loss = FAKEUV_IO_ERROR_IS_LOSS || unplug_case->hotplug ||
			unplug_case->platform != FAKE_USB_PLATFORM_WINDOWS;
		buffer_timeout_ms = unplug_case->buffer_timeout_ms >= 0 ? (guint) unplug_case->buffer_timeout_ms :
			fakeuv_timeout_ms (UNPLUG_BUFFER_TIMEOUT_MS);

		fakeuv_test_reset ();
		fake_usb_set_platform (unplug_case->platform);
		fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, unplug_case->hotplug);
		if (unplug_case->raw_io != NULL)
			g_setenv ("ARV_UV_RAW_IO", unplug_case->raw_io, TRUE);

		rig_init (&rig);
		fake_usb_device_set_unplug_transfers_hang (rig.device, unplug_case->hang);
		gst_util_set_object_arg (G_OBJECT (rig.element), "usb-mode", unplug_case->usb_mode);
		g_object_set (rig.element, "buffer-timeout", (gint) buffer_timeout_ms, NULL);
		g_object_set (rig.element, "camera-name", rig.device_id, NULL);
		rig_play (&rig, CAPS);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 10);

		unplug_time = g_get_monotonic_time ();
		fake_usb_device_unplug (rig.device);

		/* The frames already received may still come */
		end_time = unplug_time + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
		while (message == NULL && g_get_monotonic_time () < end_time) {
			GstBuffer *buffer = gst_harness_try_pull (rig.harness);

			if (buffer != NULL)
				gst_buffer_unref (buffer);
			message = gst_bus_timed_pop_filtered (rig.bus, GST_MSECOND, GST_MESSAGE_ERROR);
		}
		delay_ms = (g_get_monotonic_time () - unplug_time) / 1000;
		g_assert_nonnull (message);

		gst_message_parse_error (message, &error, NULL);
		g_test_message ("%s: '%s' after %" G_GINT64_FORMAT " ms", unplug_case->name, error->message, delay_ms);

		g_assert_true (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ));
		if (is_loss) {
			text = g_strdup_printf ("Lost control of camera \"%s\"", rig.device_id);
			g_assert_cmpstr (error->message, ==, text);
			g_assert_cmpint (delay_ms, <=, fakeuv_timeout_ms (unplug_case->max_delay_ms));
		} else if (buffer_timeout_ms == 0) {
			text = g_strdup_printf ("Camera \"%s\" sent %u failed frames in a row (", rig.device_id,
						UNPLUG_MAX_FAILED_FRAMES);
			g_assert_true (g_str_has_prefix (error->message, text));
		} else {
			/* With failed frames or not */
			text = g_strdup_printf ("No frame received from camera \"%s\" within %u ms", rig.device_id,
						buffer_timeout_ms);
			if (g_strcmp0 (error->message, text) != 0) {
				g_free (text);
				text = g_strdup_printf ("No complete frame received from camera \"%s\" within %u ms: ",
							rig.device_id, buffer_timeout_ms);
				g_assert_true (g_str_has_prefix (error->message, text));
			}
		}

		g_free (text);
		g_error_free (error);
		gst_message_unref (message);

		rig_teardown (&rig);
		g_unsetenv ("ARV_UV_RAW_IO");
	}
}

/* The devices of a probe are the cameras given, in any order, once each, with their own properties */

static void
assert_listed_cameras (GList *devices, FakeUsbDevice **cameras, const char **serials, guint n_cameras)
{
	gboolean *listed = g_new0 (gboolean, n_cameras);
	GList *iter;
	guint i;

	g_assert_cmpuint (g_list_length (devices), ==, n_cameras);

	for (iter = devices; iter != NULL; iter = iter->next) {
		GstStructure *properties = gst_device_get_properties (iter->data);
		const char *device_id = gst_structure_get_string (properties, "aravis.device-id");
		char *id = NULL;

		g_assert_nonnull (device_id);
		for (i = 0; i < n_cameras; i++) {
			id = fake_usb_camera_dup_device_id (cameras[i]);
			if (g_strcmp0 (device_id, id) == 0)
				break;
			g_clear_pointer (&id, g_free);
		}
		g_assert_cmpuint (i, <, n_cameras);
		g_free (id);

		/* Once, with the properties of that camera */
		g_assert_false (listed[i]);
		listed[i] = TRUE;
		g_assert_cmpstr (gst_structure_get_string (properties, "device.serial"), ==, serials[i]);
		g_assert_cmpstr (gst_structure_get_string (properties, "aravis.physical-id"), ==,
				 fake_usb_camera_get_guid (cameras[i]));
		g_assert_cmpstr (gst_structure_get_string (properties, "aravis.interface"), ==, "USB3Vision");

		gst_structure_free (properties);
	}

	g_free (listed);
}

/* The device provider logs each device it reads from the device list, between the first query of the device and the
 * others: the cases below run a function of theirs there, in the thread of the probe, through a log function of the
 * GStreamer debug system, to change the device list while the provider reads it */

#ifndef GST_DISABLE_GST_DEBUG

typedef void (*DeviceReadFunc) (const char *message, gpointer user_data);

typedef struct {
	DeviceReadFunc func;
	gpointer user_data;
	gboolean was_active;
	guint n_default_log_functions;
} DeviceReadHook;

static void
device_read_hook_log (GstDebugCategory *category, GstDebugLevel level, const gchar *file, const gchar *function,
		      gint line, GObject *object, GstDebugMessage *message, gpointer user_data)
{
	DeviceReadHook *hook = user_data;
	const char *text;

	if (level != GST_LEVEL_LOG || g_strcmp0 (gst_debug_category_get_name (category), "aravisdeviceprovider") != 0)
		return;

	/* "Reading device <index>: '<identifier>'" */
	text = gst_debug_message_get (message);
	if (text != NULL && g_str_has_prefix (text, "Reading device "))
		hook->func (text, hook->user_data);
}

static void
device_read_hook_start (DeviceReadHook *hook, DeviceReadFunc func, gpointer user_data)
{
	hook->func = func;
	hook->user_data = user_data;
	hook->was_active = gst_debug_is_active ();
	hook->n_default_log_functions = gst_debug_remove_log_function (gst_debug_log_default);
	gst_debug_add_log_function (device_read_hook_log, hook, NULL);
	gst_debug_set_threshold_for_name ("aravisdeviceprovider", GST_LEVEL_LOG);
}

static void
device_read_hook_stop (DeviceReadHook *hook)
{
	gst_debug_unset_threshold_for_name ("aravisdeviceprovider");
	gst_debug_remove_log_function (device_read_hook_log);
	if (hook->n_default_log_functions > 0)
		gst_debug_add_log_function (gst_debug_log_default, NULL, NULL);
	if (!hook->was_active)
		gst_debug_set_active (FALSE);
}

/* The device provider lists each camera with its own properties, once, while another thread updates the device list:
 * the application, or an aravissrc opening the first camera of an interface. The list alternates between two cameras
 * and one, which moves the second camera between indices. The thread updates the list at chosen device reads of a
 * probe, between the first query of a device and the others, once or several times in a row: a probe that did not read
 * the list again would mix two cameras in a device, or leave properties out. Each probe lists the cameras of the last
 * state of the list. Updates that find the same cameras, even at every device read, don't make the provider read the
 * list again, nor keep it from listing them. */

typedef struct {
	GMutex mutex;
	GCond cond;
	FakeUsbDevice *camera;
	gboolean hidden;
	gboolean changes_list;
	guint n_requested;
	guint n_done;
	gboolean stop;
} Updater;

static void
updater_set_hidden_locked (Updater *updater, gboolean hidden)
{
	updater->hidden = hidden;
	/* A camera discovery can't open is not listed */
	fake_usb_device_set_open_error (updater->camera, hidden ? LIBUSB_ERROR_ACCESS : 0);
}

static gpointer
updater_run (gpointer data)
{
	Updater *updater = data;

	g_mutex_lock (&updater->mutex);
	while (!updater->stop) {
		if (updater->n_done == updater->n_requested) {
			g_cond_wait (&updater->cond, &updater->mutex);
			continue;
		}

		if (updater->changes_list)
			updater_set_hidden_locked (updater, !updater->hidden);
		arv_update_device_list ();

		updater->n_done++;
		g_cond_broadcast (&updater->cond);
	}
	g_mutex_unlock (&updater->mutex);

	return NULL;
}

/* Has the thread of the updater update the list, and waits until it has */

static void
updater_update (Updater *updater)
{
	g_mutex_lock (&updater->mutex);
	updater->n_requested++;
	g_cond_broadcast (&updater->cond);
	while (updater->n_done != updater->n_requested)
		g_cond_wait (&updater->cond, &updater->mutex);
	g_mutex_unlock (&updater->mutex);
}

/* The updates during a probe: one at each device read from the first_update-th one on, n_updates in all */

typedef struct {
	Updater *updater;
	guint first_update;
	guint n_updates;
	guint n_reads;
	guint n_updated;
} ProbeUpdates;

static void
probe_updates_device_read (const char *message, gpointer user_data)
{
	ProbeUpdates *updates = user_data;

	updates->n_reads++;
	if (updates->n_reads >= updates->first_update && updates->n_updated < updates->n_updates) {
		updater_update (updates->updater);
		updates->n_updated++;
	}
}

static void
probe_updates_init (ProbeUpdates *updates, Updater *updater, guint first_update, guint n_updates)
{
	updates->updater = updater;
	updates->first_update = first_update;
	updates->n_updates = n_updates;
	updates->n_reads = 0;
	updates->n_updated = 0;
}

#endif

#define CONCURRENT_UPDATE_MAX_UPDATES	3

static void
provider_concurrent_update_test (void)
{
#ifdef GST_DISABLE_GST_DEBUG
	g_test_skip ("GStreamer is built without its debug system, which this case uses");
#else
	static const char *serials[] = {"UV1", "UV2"};
	FakeUsbDevice *cameras[G_N_ELEMENTS (serials)];
	GstDeviceProvider *provider;
	DeviceReadHook hook;
	ProbeUpdates updates;
	Updater updater;
	GThread *thread;
	GList *devices;
	guint hidden;
	guint first_update;
	guint n_updates;
	guint i;

	for (i = 0; i < G_N_ELEMENTS (serials); i++)
		cameras[i] = fakeuv_add_camera (serials[i]);

	provider = gst_device_provider_factory_get_by_name ("aravisdeviceprovider");
	g_assert_nonnull (provider);

	g_mutex_init (&updater.mutex);
	g_cond_init (&updater.cond);
	updater.camera = cameras[0];
	updater.hidden = FALSE;
	updater.changes_list = TRUE;
	updater.n_requested = 0;
	updater.n_done = 0;
	updater.stop = FALSE;
	thread = g_thread_new ("updater", updater_run, &updater);

	device_read_hook_start (&hook, probe_updates_device_read, &updates);

	/* The probe starts with both cameras listed, or the second one only. The first update comes at a device read of
	 * the first reading of the list, the others at the next device reads. */
	for (hidden = 0; hidden < 2; hidden++) {
		for (first_update = 1; first_update <= (hidden ? 1 : G_N_ELEMENTS (serials)); first_update++) {
			for (n_updates = 1; n_updates <= CONCURRENT_UPDATE_MAX_UPDATES; n_updates++) {
				gboolean last_hidden = (hidden + n_updates) % 2 == 1;

				g_mutex_lock (&updater.mutex);
				updater_set_hidden_locked (&updater, hidden);
				g_mutex_unlock (&updater.mutex);
				probe_updates_init (&updates, &updater, first_update, n_updates);

				devices = gst_device_provider_get_devices (provider);

				g_test_message ("%s camera listed, %u update%s from device read %u: %u device reads, "
						"%u device%s listed", hidden ? "One" : "Each", n_updates,
						n_updates > 1 ? "s" : "", first_update, updates.n_reads,
						g_list_length (devices), g_list_length (devices) == 1 ? "" : "s");
				g_assert_cmpuint (updates.n_updated, ==, n_updates);
				if (last_hidden)
					assert_listed_cameras (devices, cameras + 1, serials + 1, 1);
				else
					assert_listed_cameras (devices, cameras, serials, G_N_ELEMENTS (serials));

				g_list_free_full (devices, gst_object_unref);
			}
		}
	}

	/* A list that changes at each device read: no device rather than a mixed one, nor a probe without end */
	probe_updates_init (&updates, &updater, 1, G_MAXUINT);
	devices = gst_device_provider_get_devices (provider);
	g_test_message ("Updates at every device read: %u device reads", updates.n_reads);
	g_assert_null (devices);
	g_assert_cmpuint (updates.n_updated, ==, updates.n_reads);

	/* Updates at each device read that find the same cameras: a single reading of the list, which lists them */
	g_mutex_lock (&updater.mutex);
	updater_set_hidden_locked (&updater, FALSE);
	updater.changes_list = FALSE;
	g_mutex_unlock (&updater.mutex);
	probe_updates_init (&updates, &updater, 1, G_MAXUINT);
	devices = gst_device_provider_get_devices (provider);
	g_test_message ("Updates finding the same cameras at every device read: %u device reads, %u device%s listed",
			updates.n_reads, g_list_length (devices), g_list_length (devices) == 1 ? "" : "s");
	g_assert_cmpuint (updates.n_reads, ==, G_N_ELEMENTS (serials));
	g_assert_cmpuint (updates.n_updated, ==, updates.n_reads);
	assert_listed_cameras (devices, cameras, serials, G_N_ELEMENTS (serials));
	g_list_free_full (devices, gst_object_unref);

	device_read_hook_stop (&hook);

	g_mutex_lock (&updater.mutex);
	updater.stop = TRUE;
	g_cond_broadcast (&updater.cond);
	g_mutex_unlock (&updater.mutex);
	g_thread_join (thread);

	g_cond_clear (&updater.cond);
	g_mutex_clear (&updater.mutex);
	fake_usb_device_set_open_error (cameras[0], 0);
	gst_object_unref (provider);
#endif
}

/* WinUSB lets a single handle open a device: the USB3Vision discovery can't open a camera this process streams, so
 * the device provider does not list it meanwhile, which does not disturb the stream */

static void
provider_while_streaming_test (void)
{
	GstDeviceProvider *provider;
	guint exclusive;

	provider = gst_device_provider_factory_get_by_name ("aravisdeviceprovider");
	g_assert_nonnull (provider);

	for (exclusive = 0; exclusive < 2; exclusive++) {
		GList *devices;
		Rig rig;
		guint i;

		fakeuv_test_reset ();
		fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);

		rig_init (&rig);
		fake_usb_device_set_exclusive_open (rig.device, exclusive);
		g_object_set (rig.element, "camera-name", rig.device_id, NULL);
		rig_play (&rig, CAPS);
		rig_pull_frames (&rig, WIDTH * HEIGHT, 5);

		for (i = 0; i < 3; i++) {
			devices = gst_device_provider_get_devices (provider);
			g_assert_cmpuint (g_list_length (devices), ==, exclusive ? 0 : 1);
			g_list_free_full (devices, gst_object_unref);

			rig_pull_frames (&rig, WIDTH * HEIGHT, 5);
		}

		rig_assert_no_error (&rig);
		rig_teardown (&rig);

		/* Listed again once the stream stops */
		devices = gst_device_provider_get_devices (provider);
		g_assert_cmpuint (g_list_length (devices), ==, 1);
		g_list_free_full (devices, gst_object_unref);
	}

	gst_object_unref (provider);
}

/* The device provider lists the cameras of the last state of the device list, once each, even when the list changes
 * twice during a probe, a camera coming in front of the one being read each time: the first reading of the list reads
 * that camera at three indices in a row, then the provider reads the list again. Each of the first two device reads of
 * the probe makes the discovery find another camera, at the front of the list. */

#define LIST_CHANGES_N_CAMERAS	5
#define LIST_CHANGES_N_CHANGES	2

#ifndef GST_DISABLE_GST_DEBUG

typedef struct {
	FakeUsbDevice *hidden[LIST_CHANGES_N_CHANGES];
	GPtrArray *devices_read;
} ListChanger;

static void
list_changer_device_read (const char *message, gpointer user_data)
{
	ListChanger *changer = user_data;

	/* In the thread of the probe, between two queries of the device list */
	if (changer->devices_read->len < LIST_CHANGES_N_CHANGES) {
		fake_usb_device_set_open_error (changer->hidden[changer->devices_read->len], 0);
		arv_update_device_list ();
	}
	g_ptr_array_add (changer->devices_read, g_strdup (message));
}

#endif

static void
provider_list_changes_twice_test (void)
{
#ifdef GST_DISABLE_GST_DEBUG
	g_test_skip ("GStreamer is built without its debug system, which this case uses");
#else
	static const char *serials[LIST_CHANGES_N_CAMERAS] = {"S1", "S2", "S3", "S4", "S5"};
	FakeUsbDevice *cameras[LIST_CHANGES_N_CAMERAS];
	GstDeviceProvider *provider;
	DeviceReadHook hook;
	ListChanger changer;
	GList *devices;
	char *device_id;
	guint i;

	for (i = 0; i < LIST_CHANGES_N_CAMERAS; i++)
		cameras[i] = fakeuv_add_camera (serials[i]);

	/* A camera the discovery can't open is not listed: the list starts with the third camera, and the first two
	 * come in front of it, one after the other */
	for (i = 0; i < LIST_CHANGES_N_CHANGES; i++) {
		changer.hidden[i] = cameras[i];
		fake_usb_device_set_open_error (cameras[i], LIBUSB_ERROR_ACCESS);
	}
	changer.devices_read = g_ptr_array_new_with_free_func (g_free);

	provider = gst_device_provider_factory_get_by_name ("aravisdeviceprovider");
	g_assert_nonnull (provider);

	device_read_hook_start (&hook, list_changer_device_read, &changer);
	devices = gst_device_provider_get_devices (provider);
	device_read_hook_stop (&hook);

	for (i = 0; i < changer.devices_read->len; i++)
		g_test_message ("%s", (char *) g_ptr_array_index (changer.devices_read, i));

	/* The first reading of the list, of the third camera at three indices in a row, then the second one */
	device_id = fake_usb_camera_dup_device_id (cameras[LIST_CHANGES_N_CHANGES]);
	g_assert_cmpuint (changer.devices_read->len, ==,
			  LIST_CHANGES_N_CAMERAS - LIST_CHANGES_N_CHANGES + LIST_CHANGES_N_CAMERAS);
	for (i = 0; i <= LIST_CHANGES_N_CHANGES; i++) {
		char *text = g_strdup_printf ("Reading device %u: '%s'", i, device_id);

		g_assert_cmpstr (g_ptr_array_index (changer.devices_read, i), ==, text);
		g_free (text);
	}

	/* What the second reading found */
	assert_listed_cameras (devices, cameras, serials, LIST_CHANGES_N_CAMERAS);

	g_list_free_full (devices, gst_object_unref);
	g_ptr_array_unref (changer.devices_read);
	g_free (device_id);
	gst_object_unref (provider);
#endif
}

/* One state of the device list may give an identifier several times, for cameras with the same vendor, model and
 * serial number: the provider lists it once, as the element opens a single camera by its identifier. The provider used
 * to keep a freed identifier in its table of the devices listed, which the third look-up of the identifier read: what
 * that look-up found depended on the allocator, hence several probes. */

#define DUPLICATE_IDS_N_CAMERAS		3
#define DUPLICATE_IDS_N_PROBES		10

static void
provider_duplicate_ids_test (void)
{
	static const char *serials[] = {"UV1", "UV2"};
	FakeUsbDevice *cameras[G_N_ELEMENTS (serials)];
	GstDeviceProvider *provider;
	char *device_id;
	guint probe;
	guint i;

	/* The cameras of the first serial number, then another one */
	cameras[0] = fakeuv_add_camera (serials[0]);
	for (i = 1; i < DUPLICATE_IDS_N_CAMERAS; i++)
		fakeuv_add_camera (serials[0]);
	cameras[1] = fakeuv_add_camera (serials[1]);

	/* The device list gives the identifier of the first serial number at as many indices */
	device_id = fake_usb_camera_dup_device_id (cameras[0]);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, DUPLICATE_IDS_N_CAMERAS + 1);
	for (i = 0; i < DUPLICATE_IDS_N_CAMERAS; i++)
		g_assert_cmpstr (arv_get_device_id (i), ==, device_id);

	provider = gst_device_provider_factory_get_by_name ("aravisdeviceprovider");
	g_assert_nonnull (provider);

	for (probe = 0; probe < DUPLICATE_IDS_N_PROBES; probe++) {
		GList *devices = gst_device_provider_get_devices (provider);

		assert_listed_cameras (devices, cameras, serials, G_N_ELEMENTS (serials));
		g_list_free_full (devices, gst_object_unref);
	}

	g_free (device_id);
	gst_object_unref (provider);
}

int
main (int argc, char *argv[])
{
	int result;

	gst_init (&argc, &argv);
	GST_PLUGIN_STATIC_REGISTER (aravis);

	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvgst/streaming", streaming_test);
	fakeuv_test_add ("/fakeuvgst/usb-mode", usb_mode_test);
	fakeuv_test_add ("/fakeuvgst/transfer-size", transfer_size_test);
	fakeuv_test_add ("/fakeuvgst/frames-held-after-stop", frames_held_after_stop_test);
	fakeuv_test_add ("/fakeuvgst/frames-held-across-renegotiation", frames_held_across_renegotiation_test);
	fakeuv_test_add ("/fakeuvgst/renegotiation-held-stream", renegotiation_held_stream_test);
	fakeuv_test_add ("/fakeuvgst/restart-followed-objects", restart_followed_objects_test);
	fakeuv_test_add ("/fakeuvgst/unplug", unplug_test);
	fakeuv_test_add ("/fakeuvgst/provider-concurrent-update", provider_concurrent_update_test);
	fakeuv_test_add ("/fakeuvgst/provider-while-streaming", provider_while_streaming_test);
	fakeuv_test_add ("/fakeuvgst/provider-list-changes-twice", provider_list_changes_twice_test);
	fakeuv_test_add ("/fakeuvgst/provider-duplicate-ids", provider_duplicate_ids_test);

	result = fakeuv_test_run ();

	gst_deinit ();

	return result;
}
