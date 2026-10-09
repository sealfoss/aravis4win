/* SPDX-License-Identifier:Unlicense */

/*
 * aravissrc tests, meson suite 'gst'.
 *
 * They run the GStreamer plugin of this build against Aravis' in-process fake camera, and the mock GenTL producer of
 * tests/gentlmock, without camera or network.
 * The meson test environment sets GST_PLUGIN_PATH to the build's gst/ directory and GST_REGISTRY to a registry file
 * of the build directory. This program links the libaravis the plugin uses, so the arv_* () calls made here, like
 * arv_select_interface (), apply to the plugin. The 'harness' test case checks both.
 *
 * Writing a test:
 *  - describe the pipeline in gst-launch syntax, name the aravissrc "src" and end it with TEST_SINK, an appsink
 *    named "sink"; open the fake camera with camera-name=TEST_CAMERA, and use TEST_CAPS to keep frames small;
 *  - test_pipeline_new () builds it, test_pipeline_play () sets it to PLAYING and returns its first sample,
 *    test_pipeline_pull_sample () and test_pipeline_pop_message () wait with a timeout,
 *    test_pipeline_expect_error () checks the next error message, test_pipeline_check_no_error () that none waits,
 *    and test_pipeline_free () stops and frees it; test_pipeline_new_from_element () builds a pipeline around an
 *    aravissrc created by the test;
 *  - setup () runs before each test: only the "Fake" interface is enabled for device discovery; teardown () fails the
 *    test on the GLib warnings and criticals of the plugin it did not expect with test_aravis_log_expect_warnings ();
 *  - to check frames and their memory: test_count_arv_buffers () counts the live ArvBuffers, test_frame_new () copies
 *    a frame of the fake camera for test_frame_check () to compare later, a TestCollector holds or counts what a
 *    fakesink receives, test_fake_streams_reset () records how many fake streams live at most when one is created,
 *    test_fake_stream_count_starts () counts the acquisition starts of a fake stream, a TestStreamHolder holds the
 *    stream of an aravissrc, as an application following its stream property would, until the next stream comes or
 *    to the end, and a TestFollower follows its stream or camera property as gst/pipelines.md says, releasing the
 *    object held at each notification;
 *  - to act as an application using the Aravis device list on another thread: test_lister_start () and
 *    test_lister_stop (), which checks what the lister read;
 *  - to disturb a stream: test_element_starve () makes the fake camera stop delivering frames until the acquisition
 *    stops, test_element_fail_frames () makes all its frames fail, and a TestEmitter emits control-lost on the device
 *    of the camera from another thread;
 *  - to list devices: test_provider_probe () lists the devices of the device provider, test_monitor_start () starts a
 *    GstDeviceMonitor which only sees this provider, until test_monitor_stop (), or teardown_device_provider () after
 *    a failed check, and test_count_fake_device_openings () counts the openings of the fake camera.
 *
 * The checks run in a single process (CK_FORK=no, as on Windows); GST_CHECKS=<test name> runs a single test. The
 * 'gentl' test case loads the mock GenTL producer whose path meson gives in ARV_TEST_GENTL_MOCK, and its tests do
 * nothing without it. The 'network' test case, with the in-process fake GigEVision camera on 127.0.0.1, only exists
 * with GST_ARAVIS_TEST_NETWORK=1, which the test of the 'network' suite sets, as it binds the fixed GigEVision ports.
 */

#include <gst/check/gstcheck.h>
#include <gst/app/gstappsink.h>
#include <glib/gstdio.h>
#include <arv.h>
#include <string.h>

#include "arvgentlmock.h"

/* How long to wait for something that should happen; a frame period of the fake camera is 40 ms by default */
#define TEST_TIMEOUT		(10 * GST_SECOND)

#define TEST_CAMERA		"Fake_1"
/* Small frames, with a row stride aravissrc does not need to pad */
#define TEST_CAPS		"video/x-raw,format=GRAY8,width=64,height=48"
#define TEST_SINK		"appsink name=sink sync=false max-buffers=1 drop=true"

/* Helpers */

/* The pipelines not freed yet. A failed check leaves its test at once, and teardown () stops what it left running:
 * otherwise the next test would wait forever for the streaming threads in GST_END_TEST. */
static GList *test_pipelines = NULL;

static GstElement *test_pipeline_new (const char *format, ...) G_GNUC_PRINTF (1, 2);

/* Builds a pipeline from a gst-launch description */

static GstElement *
test_pipeline_new (const char *format, ...)
{
	GstElement *pipeline;
	GError *error = NULL;
	char *description;
	va_list args;

	va_start (args, format);
	description = g_strdup_vprintf (format, args);
	va_end (args);

	GST_INFO ("Pipeline: %s", description);

	pipeline = gst_parse_launch_full (description, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &error);
	fail_unless (error == NULL, "Can't build '%s': %s", description, error != NULL ? error->message : "");
	fail_unless (GST_IS_PIPELINE (pipeline), "'%s' is not a pipeline", description);

	g_free (description);

	test_pipelines = g_list_prepend (test_pipelines, pipeline);

	return pipeline;
}

/* Returns the element of the pipeline with the given name (transfer full) */

static GstElement *
test_pipeline_get_element (GstElement *pipeline, const char *name)
{
	GstElement *element;

	element = gst_bin_get_by_name (GST_BIN (pipeline), name);
	fail_unless (element != NULL, "No element named '%s'", name);

	return element;
}

/* Changes the state and waits for the change to complete. Returns the result, GST_STATE_CHANGE_ASYNC if the change
 * did not complete within the timeout. */

static GstStateChangeReturn
test_pipeline_set_state (GstElement *pipeline, GstState state, GstClockTime timeout)
{
	GstStateChangeReturn result;

	result = gst_element_set_state (pipeline, state);
	if (result == GST_STATE_CHANGE_ASYNC)
		result = gst_element_get_state (pipeline, NULL, NULL, timeout);

	return result;
}

/* Waits for a message of one of the given types (transfer full); NULL on timeout */

static GstMessage *
test_pipeline_pop_message (GstElement *pipeline, GstMessageType types, GstClockTime timeout)
{
	GstMessage *message;
	GstBus *bus;

	bus = gst_element_get_bus (pipeline);
	message = gst_bus_timed_pop_filtered (bus, timeout, types);
	gst_object_unref (bus);

	return message;
}

/* Returns the text of the error message waiting on the bus, if any, for failure reports */

static char *
test_pipeline_dup_error_text (GstElement *pipeline)
{
	GstMessage *message;
	GError *error = NULL;
	char *debug = NULL;
	char *text;

	message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, 0);
	if (message == NULL)
		return g_strdup ("no error message");

	gst_message_parse_error (message, &error, &debug);
	text = g_strdup_printf ("%s: %s (%s)", GST_MESSAGE_SRC_NAME (message), error->message,
				debug != NULL ? debug : "no details");
	g_error_free (error);
	g_free (debug);
	gst_message_unref (message);

	return text;
}

/* Pulls a sample from the appsink named "sink" (transfer full); NULL on timeout or end of stream */

static GstSample *
test_pipeline_pull_sample (GstElement *pipeline, GstClockTime timeout)
{
	GstElement *sink;
	GstSample *sample;

	sink = test_pipeline_get_element (pipeline, "sink");
	sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), timeout);
	gst_object_unref (sink);

	return sample;
}

/* Sets the pipeline to PLAYING and returns its first sample (transfer full) */

static GstSample *
test_pipeline_play (GstElement *pipeline)
{
	GstStateChangeReturn result;
	GstSample *sample;

	result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	fail_unless (result == GST_STATE_CHANGE_SUCCESS || result == GST_STATE_CHANGE_NO_PREROLL,
		     "Can't start the pipeline (%s): %s", gst_element_state_change_return_get_name (result),
		     test_pipeline_dup_error_text (pipeline));

	sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
	fail_unless (sample != NULL, "No sample within %" GST_TIME_FORMAT ": %s", GST_TIME_ARGS (TEST_TIMEOUT),
		     test_pipeline_dup_error_text (pipeline));

	return sample;
}

/* Sets a pipeline that may not end in an appsink to PLAYING */

static void
test_pipeline_start (GstElement *pipeline)
{
	GstStateChangeReturn result;

	result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	fail_unless (result == GST_STATE_CHANGE_SUCCESS || result == GST_STATE_CHANGE_NO_PREROLL,
		     "Can't start the pipeline (%s): %s", gst_element_state_change_return_get_name (result),
		     test_pipeline_dup_error_text (pipeline));
}

/* Waits for an error message, and checks that it comes from the element named source_name, with the given domain
 * and code, and that its text contains text (if not NULL) */

static void
test_pipeline_expect_error (GstElement *pipeline, const char *source_name, GQuark domain, gint code, const char *text)
{
	GstMessage *message;
	GError *error = NULL;
	char *debug = NULL;

	message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, TEST_TIMEOUT);
	fail_unless (message != NULL, "No error message within %" GST_TIME_FORMAT, GST_TIME_ARGS (TEST_TIMEOUT));

	gst_message_parse_error (message, &error, &debug);
	GST_INFO ("Error from %s: %s (%s)", GST_MESSAGE_SRC_NAME (message), error->message, debug);

	fail_unless_equals_string (GST_MESSAGE_SRC_NAME (message), source_name);
	fail_unless (g_error_matches (error, domain, code), "Unexpected error %s %d: %s",
		     g_quark_to_string (error->domain), error->code, error->message);
	if (text != NULL)
		fail_unless (strstr (error->message, text) != NULL, "'%s' not in '%s'", text, error->message);

	g_error_free (error);
	g_free (debug);
	gst_message_unref (message);
}

/* Stops and frees the pipeline */

static void
test_pipeline_free (GstElement *pipeline)
{
	GstStateChangeReturn result;

	test_pipelines = g_list_remove (test_pipelines, pipeline);

	result = test_pipeline_set_state (pipeline, GST_STATE_NULL, TEST_TIMEOUT);
	gst_object_unref (pipeline);

	fail_unless (result == GST_STATE_CHANGE_SUCCESS, "Can't stop the pipeline: %s",
		     gst_element_state_change_return_get_name (result));
}

/* Builds a pipeline from an aravissrc named "src", whose bus gets the messages of the element from then on, with
 * TEST_CAPS and TEST_SINK */

static GstElement *
test_pipeline_new_from_element (GstElement *src)
{
	GstElement *pipeline;
	GstElement *filter;

	pipeline = test_pipeline_new ("capsfilter name=filter caps=\"%s\" ! %s", TEST_CAPS, TEST_SINK);

	fail_unless (gst_bin_add (GST_BIN (pipeline), src));
	filter = test_pipeline_get_element (pipeline, "filter");
	fail_unless (gst_element_link (src, filter));
	gst_object_unref (filter);

	return pipeline;
}

/* Fails if an error message waits on the bus */

static void
test_pipeline_check_no_error (GstElement *pipeline, const char *when)
{
	GstMessage *message;

	message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, 0);
	if (message != NULL) {
		GError *error = NULL;

		gst_message_parse_error (message, &error, NULL);
		fail ("Error from %s %s: %s", GST_MESSAGE_SRC_NAME (message), when, error->message);
	}
}

/* Returns the camera opened by an aravissrc (transfer full) */

static ArvCamera *
test_element_dup_camera (GstElement *src)
{
	ArvCamera *camera = NULL;

	g_object_get (src, "camera", &camera, NULL);
	fail_unless (ARV_IS_CAMERA (camera), "No camera opened");

	return camera;
}

/* Runs a latency query on the source pad of an element; FALSE if unanswered */

static gboolean
test_element_query_latency (GstElement *element, gboolean *live, GstClockTime *min_latency, GstClockTime *max_latency)
{
	GstQuery *query;
	GstPad *pad;
	gboolean answered;

	pad = gst_element_get_static_pad (element, "src");
	fail_unless (pad != NULL);

	query = gst_query_new_latency ();
	answered = gst_pad_query (pad, query);
	if (answered)
		gst_query_parse_latency (query, live, min_latency, max_latency);

	gst_query_unref (query);
	gst_object_unref (pad);

	return answered;
}

static gboolean
test_is_same_file (const char *filename_a, const char *filename_b)
{
	char *a;
	char *b;
	gboolean same;

	a = g_canonicalize_filename (filename_a, NULL);
	b = g_canonicalize_filename (filename_b, NULL);

#ifdef G_OS_WIN32
	g_strdelimit (a, "/", '\\');
	g_strdelimit (b, "/", '\\');
	same = g_ascii_strcasecmp (a, b) == 0;
#else
	same = g_strcmp0 (a, b) == 0;
	if (!same) {
		GStatBuf stat_a, stat_b;

		/* The same file through a symbolic link */
		same = g_stat (a, &stat_a) == 0 && g_stat (b, &stat_b) == 0 &&
			stat_a.st_dev == stat_b.st_dev && stat_a.st_ino == stat_b.st_ino;
	}
#endif

	g_free (a);
	g_free (b);

	return same;
}

/* Lists the devices with the Aravis API from another thread, as an application may while an aravissrc opens a camera
 * or the device provider probes: until test_lister_stop (), it updates the device list and reads the strings of its
 * first device, which must be the fake camera, the only one, letting other threads update the list in between. No
 * check on that thread, which only counts the bad listings. A single lister, which teardown () stops after a failed
 * check. */

typedef struct {
	GThread *thread;
	gint stop;
	gint n_listings;
	gint n_bad_listings;
} TestLister;

static TestLister test_lister = {NULL, FALSE, 0, 0};

static gpointer
test_lister_run (gpointer data)
{
	TestLister *lister = data;

	while (!g_atomic_int_get (&lister->stop)) {
		const char *device_id;
		const char *vendor;
		const char *model;
		gboolean is_good;

		arv_update_device_list ();
		device_id = arv_get_device_id (0);
		vendor = arv_get_device_vendor (0);
		model = arv_get_device_model (0);

		g_thread_yield ();

		is_good = g_strcmp0 (device_id, TEST_CAMERA) == 0 &&
			g_strcmp0 (vendor, "Aravis") == 0 &&
			g_strcmp0 (model, "Fake") == 0;
		if (!is_good)
			g_atomic_int_inc (&lister->n_bad_listings);
		g_atomic_int_inc (&lister->n_listings);
	}

	return NULL;
}

static void
test_lister_start (void)
{
	fail_unless (test_lister.thread == NULL);

	test_lister.stop = FALSE;
	test_lister.n_listings = 0;
	test_lister.n_bad_listings = 0;
	test_lister.thread = g_thread_new ("lister", test_lister_run, &test_lister);
}

static int
test_lister_get_n_listings (void)
{
	return g_atomic_int_get (&test_lister.n_listings);
}

/* Stops the lister, and checks that it always found the fake camera */

static void
test_lister_stop (void)
{
	if (test_lister.thread == NULL)
		return;

	g_atomic_int_set (&test_lister.stop, TRUE);
	g_thread_join (g_steal_pointer (&test_lister.thread));

	GST_INFO ("%d listings, %d bad ones", test_lister.n_listings, test_lister.n_bad_listings);

	fail_unless (test_lister.n_listings > 0, "No listing");
	fail_unless (test_lister.n_bad_listings == 0, "%d bad listings out of %d", test_lister.n_bad_listings,
		     test_lister.n_listings);
}

/* The GLib warnings and criticals of the plugin, whose log domain is "Aravis": gst-check only fails a test on those of
 * the default domain, which the library uses, of GLib and of GStreamer. A handler counts them, on any thread, and
 * teardown () fails the test on those it did not expect with test_aravis_log_expect_warnings (). */

typedef struct {
	GMutex mutex;
	guint n_criticals;
	guint n_warnings;
	guint n_expected_warnings;
	char *last_message;
} TestAravisLog;

static TestAravisLog test_aravis_log;

static void
test_aravis_log_handler (const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
	gboolean is_critical = (log_level & G_LOG_LEVEL_CRITICAL) != 0;

	g_printerr ("%s-%s **: %s\n", log_domain, is_critical ? "CRITICAL" : "WARNING", message);

	g_mutex_lock (&test_aravis_log.mutex);
	if (is_critical)
		test_aravis_log.n_criticals++;
	else
		test_aravis_log.n_warnings++;
	g_free (test_aravis_log.last_message);
	test_aravis_log.last_message = g_strdup (message);
	g_mutex_unlock (&test_aravis_log.mutex);
}

static void
test_aravis_log_reset (void)
{
	g_mutex_lock (&test_aravis_log.mutex);
	test_aravis_log.n_criticals = 0;
	test_aravis_log.n_warnings = 0;
	test_aravis_log.n_expected_warnings = 0;
	g_clear_pointer (&test_aravis_log.last_message, g_free);
	g_mutex_unlock (&test_aravis_log.mutex);
}

/* Returns the number of Aravis warnings since the start of the test, which it then expects */

static guint
test_aravis_log_expect_warnings (void)
{
	guint n_warnings;

	g_mutex_lock (&test_aravis_log.mutex);
	n_warnings = test_aravis_log.n_warnings;
	test_aravis_log.n_expected_warnings = n_warnings;
	g_mutex_unlock (&test_aravis_log.mutex);

	return n_warnings;
}

static void
test_aravis_log_check (void)
{
	guint n_criticals, n_warnings, n_expected_warnings;
	char *last_message;

	g_mutex_lock (&test_aravis_log.mutex);
	n_criticals = test_aravis_log.n_criticals;
	n_warnings = test_aravis_log.n_warnings;
	n_expected_warnings = test_aravis_log.n_expected_warnings;
	last_message = g_strdup (test_aravis_log.last_message);
	g_mutex_unlock (&test_aravis_log.mutex);

	test_aravis_log_reset ();

	fail_unless (n_criticals == 0 && n_warnings == n_expected_warnings,
		     "%u Aravis criticals and %u unexpected warnings, the last one: %s", n_criticals,
		     n_warnings - n_expected_warnings, last_message);

	g_free (last_message);
}

static void
setup (void)
{
	/* The in-process fake camera only: no USB enumeration, no network discovery */
	arv_select_interface ("Fake");

	test_aravis_log_reset ();
}

/* Stops what a failed check left running */

static void
teardown_leftovers (void)
{
	if (test_lister.thread != NULL) {
		GST_WARNING ("Stopping a lister left by a failed check");
		g_atomic_int_set (&test_lister.stop, TRUE);
		g_thread_join (g_steal_pointer (&test_lister.thread));
	}

	while (test_pipelines != NULL) {
		GstElement *pipeline = test_pipelines->data;

		test_pipelines = g_list_delete_link (test_pipelines, test_pipelines);
		GST_WARNING ("Stopping a pipeline left by a failed check");
		gst_element_set_state (pipeline, GST_STATE_NULL);
		gst_object_unref (pipeline);
	}
}

static void
teardown (void)
{
	teardown_leftovers ();
	test_aravis_log_check ();
}

/* Harness */

GST_START_TEST (test_harness_plugin)
{
	GstPlugin *plugin;
	GstElement *src;
	GParamSpec *pspec;
	ArvCamera *camera;

	/* The plugin of this build, not an installed one */
	plugin = gst_registry_find_plugin (gst_registry_get (), "aravis");
	fail_unless (plugin != NULL, "No aravis plugin in the registry");
	fail_unless (test_is_same_file (gst_plugin_get_filename (plugin), GST_ARAVIS_PLUGIN_FILENAME),
		     "Loaded '%s' instead of '%s'", gst_plugin_get_filename (plugin), GST_ARAVIS_PLUGIN_FILENAME);
	gst_object_unref (plugin);

	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);

	/* The plugin and this program share libaravis: one ArvCamera type */
	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (src), "camera");
	fail_unless (pspec != NULL);
	fail_unless (G_PARAM_SPEC_VALUE_TYPE (pspec) == ARV_TYPE_CAMERA);

	/* and one interface list: the Fake interface, disabled by default, was enabled by setup (). The camera-name
	 * setter opens the camera. */
	g_object_set (src, "camera-name", TEST_CAMERA, NULL);
	camera = test_element_dup_camera (src);
	fail_unless (G_OBJECT_TYPE (camera) == ARV_TYPE_CAMERA);
	fail_unless (ARV_IS_FAKE_DEVICE (arv_camera_get_device (camera)));

	g_object_unref (camera);
	gst_object_unref (src);
}
GST_END_TEST;

GST_START_TEST (test_harness_stream)
{
	GstElement *pipeline;
	GstStructure *structure;
	GstSample *sample;
	int width = 0, height = 0;
	int i;

	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s ! %s", TEST_CAMERA, TEST_CAPS, TEST_SINK);
	sample = test_pipeline_play (pipeline);

	structure = gst_caps_get_structure (gst_sample_get_caps (sample), 0);
	fail_unless_equals_string (gst_structure_get_name (structure), "video/x-raw");
	fail_unless_equals_string (gst_structure_get_string (structure, "format"), "GRAY8");
	fail_unless (gst_structure_get_int (structure, "width", &width));
	fail_unless (gst_structure_get_int (structure, "height", &height));
	fail_unless_equals_int (width, 64);
	fail_unless_equals_int (height, 48);
	fail_unless_equals_uint64 (gst_buffer_get_size (gst_sample_get_buffer (sample)), 64 * 48);
	gst_sample_unref (sample);

	/* The stream goes on */
	for (i = 0; i < 3; i++) {
		sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
		fail_unless (sample != NULL);
		gst_sample_unref (sample);
	}

	test_pipeline_free (pipeline);
}
GST_END_TEST;

/* Latency: the minimum latency is one frame period */

GST_START_TEST (test_latency_frame_period)
{
	static const struct {
		const char *frame_rate;
		GstClockTime period;
	} rates[] = {
		{"25/1", 40 * GST_MSECOND},
		{"50/1", 20 * GST_MSECOND}
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (rates); i++) {
		GstElement *pipeline;
		GstElement *src;
		GstClockTime min_latency = 0, max_latency = 0;
		gboolean live = FALSE;

		pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s,framerate=%s ! %s",
					      TEST_CAMERA, TEST_CAPS, rates[i].frame_rate, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		fail_unless (test_element_query_latency (src, &live, &min_latency, &max_latency));
		fail_unless (live);
		fail_unless_equals_uint64 (min_latency, rates[i].period);
		/* Unbounded on purpose: a camera may stop delivering frames for a while */
		fail_unless_equals_uint64 (max_latency, GST_CLOCK_TIME_NONE);

		gst_object_unref (src);
		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

GST_START_TEST (test_latency_trigger_mode)
{
	GstElement *pipeline;
	GstElement *src;

	/* In trigger mode the frame rate is unknown, and the latency query stays unanswered. trigger is applied when
	 * the camera is opened, here by camera-name. */
	pipeline = test_pipeline_new ("aravissrc name=src trigger=Software camera-name=%s ! %s ! %s",
				      TEST_CAMERA, TEST_CAPS, TEST_SINK);
	src = test_pipeline_get_element (pipeline, "src");

	/* No stream yet */
	fail_if (test_element_query_latency (src, NULL, NULL, NULL));

	/* The fake camera triggers itself at 25 Hz */
	gst_sample_unref (test_pipeline_play (pipeline));
	fail_if (test_element_query_latency (src, NULL, NULL, NULL));

	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

/* USB settings: usb-maximum-transfer-size, and the USB settings whatever the property order */

/* The range of maximum transfer sizes the USB3Vision streams accept, besides 0 */
#define TEST_MIN_USB_TRANSFER_SIZE	1024
#define TEST_MAX_USB_TRANSFER_SIZE	(64 * 1024 * 1024)

GST_START_TEST (test_usb_maximum_transfer_size_property)
{
	static const guint sizes[] = {256 * 1024, 1024 * 1024, TEST_MAX_USB_TRANSFER_SIZE, 0};
	GParamSpecUInt *pspec_uint;
	GParamSpec *pspec;
	GstElement *src;
	guint size = 1;
	unsigned int i;

	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (src), "usb-maximum-transfer-size");
	fail_unless (pspec != NULL);
	fail_unless (G_IS_PARAM_SPEC_UINT (pspec));
	fail_unless ((pspec->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
	pspec_uint = G_PARAM_SPEC_UINT (pspec);
	fail_unless_equals_uint64 (pspec_uint->default_value, 0);
	fail_unless_equals_uint64 (pspec_uint->minimum, 0);
	/* The largest size the library accepts */
	fail_unless_equals_uint64 (pspec_uint->maximum, TEST_MAX_USB_TRANSFER_SIZE);

	g_object_get (src, "usb-maximum-transfer-size", &size, NULL);
	fail_unless_equals_uint64 (size, 0);

	for (i = 0; i < G_N_ELEMENTS (sizes); i++) {
		g_object_set (src, "usb-maximum-transfer-size", sizes[i], NULL);
		g_object_get (src, "usb-maximum-transfer-size", &size, NULL);
		fail_unless_equals_uint64 (size, sizes[i]);
	}

	gst_object_unref (src);
}
GST_END_TEST;

GST_START_TEST (test_usb_settings_non_usb_camera)
{
	/* The USB settings must not get in the way of another kind of camera, set before or after camera-name, which
	 * opens the camera. They only reach a USB3Vision device, which this test can't check. */
	static const char *descriptions[] = {
		"aravissrc name=src usb-mode=sync usb-maximum-transfer-size=262144 camera-name=" TEST_CAMERA,
		"aravissrc name=src camera-name=" TEST_CAMERA " usb-mode=sync usb-maximum-transfer-size=262144"
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (descriptions); i++) {
		GstElement *pipeline;
		GstElement *src;
		guint size = 0;
		gint usb_mode = ARV_UV_USB_MODE_DEFAULT;

		pipeline = test_pipeline_new ("%s ! %s ! %s", descriptions[i], TEST_CAPS, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		g_object_get (src, "usb-mode", &usb_mode, "usb-maximum-transfer-size", &size, NULL);
		fail_unless_equals_int (usb_mode, ARV_UV_USB_MODE_SYNC);
		fail_unless_equals_uint64 (size, 262144);
		gst_object_unref (src);

		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

GST_START_TEST (test_usb_maximum_transfer_size_invalid)
{
	static const guint small_sizes[] = {1, 512, TEST_MIN_USB_TRANSFER_SIZE - 1};
	static const guint large_sizes[] = {TEST_MAX_USB_TRANSFER_SIZE + 1, 1024 * 1024 * 1024};
	GParamSpec *pspec;
	GstElement *src;
	guint size = 0;
	unsigned int i;

	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);

	/* The USB3Vision streams take 0, for the library default, or 1024 bytes to 64 MiB, as gst-inspect-1.0 says */
	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (src), "usb-maximum-transfer-size");
	fail_unless (pspec != NULL);
	fail_unless (strstr (g_param_spec_get_blurb (pspec), "1024 to 67108864") != NULL, "'%s'",
		     g_param_spec_get_blurb (pspec));

	g_object_set (src, "usb-maximum-transfer-size", (guint) TEST_MIN_USB_TRANSFER_SIZE, NULL);
	g_object_get (src, "usb-maximum-transfer-size", &size, NULL);
	fail_unless_equals_uint64 (size, TEST_MIN_USB_TRANSFER_SIZE);

	/* GObject refuses the sizes above the range of the property, */
	for (i = 0; i < G_N_ELEMENTS (large_sizes); i++) {
		GValue value = G_VALUE_INIT;

		g_value_init (&value, G_TYPE_UINT);
		g_value_set_uint (&value, large_sizes[i]);
		fail_unless (g_param_value_validate (pspec, &value), "%u accepted", large_sizes[i]);
		g_value_unset (&value);
	}

	/* and the element the sizes below the minimum, which the range can't exclude */
	for (i = 0; i < G_N_ELEMENTS (small_sizes); i++) {
		g_object_set (src, "usb-maximum-transfer-size", small_sizes[i], NULL);
		g_object_get (src, "usb-maximum-transfer-size", &size, NULL);
		fail_unless_equals_uint64 (size, TEST_MIN_USB_TRANSFER_SIZE);
	}
	fail_unless_equals_int (test_aravis_log_expect_warnings (), G_N_ELEMENTS (small_sizes));

	g_object_set (src, "usb-maximum-transfer-size", (guint) 0, NULL);
	g_object_get (src, "usb-maximum-transfer-size", &size, NULL);
	fail_unless_equals_uint64 (size, 0);

	gst_object_unref (src);
}
GST_END_TEST;

/* The interface property */

GST_START_TEST (test_interface_property)
{
	GParamSpec *pspec;
	GstElement *src;
	char *interface_id = NULL;

	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (src), "interface");
	fail_unless (pspec != NULL);
	fail_unless (G_IS_PARAM_SPEC_STRING (pspec));
	fail_unless ((pspec->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
	fail_unless (G_PARAM_SPEC_STRING (pspec)->default_value == NULL);

	g_object_get (src, "interface", &interface_id, NULL);
	fail_unless (interface_id == NULL);

	g_object_set (src, "interface", "Fake", NULL);
	g_object_get (src, "interface", &interface_id, NULL);
	fail_unless_equals_string (interface_id, "Fake");
	g_free (interface_id);

	/* An empty string means all interfaces, as NULL */
	g_object_set (src, "interface", "", NULL);
	g_object_get (src, "interface", &interface_id, NULL);
	fail_unless (interface_id == NULL);

	gst_object_unref (src);
}
GST_END_TEST;

GST_START_TEST (test_interface_open)
{
	static const char *descriptions[] = {
		/* First device of the interface */
		"aravissrc name=src interface=Fake",
		"aravissrc name=src interface=Fake camera-name=" TEST_CAMERA,
		/* camera-name opens the camera, but can't find it on the enabled interfaces: the interface setter
		 * opens it again */
		"aravissrc name=src camera-name=" TEST_CAMERA " interface=Fake"
	};
	unsigned int i;

	/* The property reaches an interface disabled for device discovery, as GenTL is by default */
	arv_disable_interface ("Fake");

	for (i = 0; i < G_N_ELEMENTS (descriptions); i++) {
		GstElement *pipeline;
		GstElement *src;
		ArvCamera *camera;

		pipeline = test_pipeline_new ("%s ! %s ! %s", descriptions[i], TEST_CAPS, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		camera = test_element_dup_camera (src);
		fail_unless (ARV_IS_FAKE_DEVICE (arv_camera_get_device (camera)));
		g_object_unref (camera);
		gst_object_unref (src);

		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

GST_START_TEST (test_interface_unknown)
{
	static const char *descriptions[] = {
		"aravissrc name=src interface=Bogus",
		"aravissrc name=src interface=Bogus camera-name=" TEST_CAMERA,
		/* The interface also applies to a camera camera-name has already opened */
		"aravissrc name=src camera-name=" TEST_CAMERA " interface=Bogus"
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (descriptions); i++) {
		GstElement *pipeline;
		GstStateChangeReturn result;

		pipeline = test_pipeline_new ("%s ! %s ! %s", descriptions[i], TEST_CAPS, TEST_SINK);

		result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
		fail_unless_equals_int (result, GST_STATE_CHANGE_FAILURE);
		test_pipeline_expect_error (pipeline, "src", GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_SETTINGS,
					    "Unknown interface 'Bogus'");

		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

GST_START_TEST (test_interface_device_not_found)
{
	GstElement *pipeline;
	GstStateChangeReturn result;

	pipeline = test_pipeline_new ("aravissrc name=src interface=Fake camera-name=Nope ! %s ! %s",
				      TEST_CAPS, TEST_SINK);

	result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	fail_unless_equals_int (result, GST_STATE_CHANGE_FAILURE);
	test_pipeline_expect_error (pipeline, "src", GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND,
				    "'Nope' not found on interface 'Fake'");

	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_interface_order_in_bin)
{
	GstElement *pipeline;
	GstElement *src;
	ArvCamera *camera;

	/* An element already in a pipeline posts its error messages on the bus of the pipeline. Its camera is only on
	 * an interface disabled for device discovery, as GenTL is by default. */
	arv_disable_interface ("Fake");

	src = gst_element_factory_make ("aravissrc", "src");
	fail_unless (src != NULL);
	pipeline = test_pipeline_new_from_element (src);

	/* camera-name looks for the camera on the enabled interfaces, in vain, then interface opens it: no error, which
	 * would make an application stop the pipeline */
	g_object_set (src, "camera-name", TEST_CAMERA, "interface", "Fake", NULL);
	test_pipeline_check_no_error (pipeline, "before the start");

	camera = test_element_dup_camera (src);
	fail_unless (ARV_IS_FAKE_DEVICE (arv_camera_get_device (camera)));
	g_object_unref (camera);

	gst_sample_unref (test_pipeline_play (pipeline));
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_interface_open_error_at_start)
{
	GstElement *pipeline;
	GstElement *src;
	GstStateChangeReturn result;

	/* A camera that can't be opened, selected on an element in a pipeline: the error comes when the element starts,
	 * once all the properties are set, not when camera-name is set */
	src = gst_element_factory_make ("aravissrc", "src");
	fail_unless (src != NULL);
	pipeline = test_pipeline_new_from_element (src);

	g_object_set (src, "camera-name", "Nope", NULL);
	test_pipeline_check_no_error (pipeline, "before the start");

	result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	fail_unless_equals_int (result, GST_STATE_CHANGE_FAILURE);
	test_pipeline_expect_error (pipeline, "src", GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND,
				    "Could not find camera \"Nope\"");

	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_interface_error_camera_names)
{
	GstElement *pipeline;
	GstStateChangeReturn result;
	GstMessage *message;
	GError *error = NULL;
	ArvCamera *camera;
	char *debug = NULL;
	char *text;

	/* Without camera-name, the errors name the camera the element looks for, and without interface, the details
	 * point at the interfaces disabled for device discovery, */
	arv_disable_interface ("Fake");
	pipeline = test_pipeline_new ("aravissrc name=src ! %s ! %s", TEST_CAPS, TEST_SINK);
	result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	fail_unless_equals_int (result, GST_STATE_CHANGE_FAILURE);
	message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, TEST_TIMEOUT);
	fail_unless (message != NULL);
	gst_message_parse_error (message, &error, &debug);
	fail_unless (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND), "%s", error->message);
	fail_unless (g_str_has_prefix (error->message, "Could not find a camera: "), "%s", error->message);
	fail_unless (debug != NULL && strstr (debug, "the interface property") != NULL, "No hint in '%s'", debug);
	g_clear_error (&error);
	g_free (debug);
	gst_message_unref (message);
	test_pipeline_free (pipeline);
	arv_enable_interface ("Fake");

	/* and once it is open, the camera by its label, vendor-model-serial. The caps are set as the pipeline starts. */
	camera = arv_camera_new (TEST_CAMERA, NULL);
	fail_unless (ARV_IS_CAMERA (camera));
	text = g_strdup_printf ("Could not set caps on camera \"%s-%s-%s\": ", arv_camera_get_vendor_name (camera, NULL),
				arv_camera_get_model_name (camera, NULL),
				arv_camera_get_device_serial_number (camera, NULL));
	g_object_unref (camera);

	pipeline = test_pipeline_new ("aravissrc name=src interface=Fake features=NoSuchFeature=1 ! %s ! %s",
				      TEST_CAPS, TEST_SINK);
	test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	test_pipeline_expect_error (pipeline, "src", GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_WRITE, text);

	g_free (text);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

#define TEST_LISTING_N_OPENINGS	100

GST_START_TEST (test_interface_concurrent_listing)
{
	GstElement *src;
	unsigned int i;

	/* Without camera-name, the element updates the device list of the interface to open its first camera: while
	 * the application lists the devices with the Aravis API on another thread. Setting camera-name opens the camera
	 * again. */
	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);
	g_object_set (src, "interface", "Fake", NULL);

	test_lister_start ();

	for (i = 0; i < TEST_LISTING_N_OPENINGS || test_lister_get_n_listings () < TEST_LISTING_N_OPENINGS; i++) {
		ArvCamera *camera = NULL;

		g_object_set (src, "camera-name", NULL, NULL);
		g_object_get (src, "camera", &camera, NULL);
		fail_unless (camera != NULL, "Opening %u failed", i);
		g_object_unref (camera);
	}

	test_lister_stop ();
	gst_object_unref (src);
}
GST_END_TEST;

/* Frame memory: a GstBuffer owns the frame memory it shows */

/* Counts the live ArvBuffers, aravissrc's included, since the plugin and this program share the ArvBuffer class: a
 * hook on its constructed () puts a weak reference on every new ArvBuffer. */

static gint test_n_arv_buffers = 0;
static void (*test_arv_buffer_parent_constructed) (GObject *object) = NULL;

static void
test_arv_buffer_finalized (gpointer data, GObject *where_the_object_was)
{
	g_atomic_int_add (&test_n_arv_buffers, -1);
}

static void
test_arv_buffer_constructed (GObject *object)
{
	if (test_arv_buffer_parent_constructed != NULL)
		test_arv_buffer_parent_constructed (object);

	g_object_weak_ref (object, test_arv_buffer_finalized, NULL);
	g_atomic_int_inc (&test_n_arv_buffers);
}

/* Returns the number of ArvBuffers created since the first call and still alive. The first call, which installs the
 * hook, must happen while no stream runs. */

static int
test_count_arv_buffers (void)
{
	GObjectClass *object_class;

	object_class = g_type_class_ref (ARV_TYPE_BUFFER);
	if (object_class->constructed != test_arv_buffer_constructed) {
		ArvBuffer *buffer;

		/* Keeps the class reference */
		test_arv_buffer_parent_constructed = object_class->constructed;
		object_class->constructed = test_arv_buffer_constructed;

		/* The hook sees new buffers */
		buffer = arv_buffer_new (1, NULL);
		fail_unless_equals_int (g_atomic_int_get (&test_n_arv_buffers), 1);
		g_object_unref (buffer);
		fail_unless_equals_int (g_atomic_int_get (&test_n_arv_buffers), 0);
	} else
		g_type_class_unref (object_class);

	return g_atomic_int_get (&test_n_arv_buffers);
}

/* The fake camera fills Mono8 frames with a diagonal ramp, pixel (x, y) = (x + y + frame id) % 255 at the default gain
 * and exposure time, so the first pixel tells a frame from the 254 others around it. Returns that pixel, or -1 if the
 * data is not such a ramp. */

static int
test_frame_get_stamp (const guint8 *data, gsize size, int width, int height)
{
	int x, y;

	if (width <= 0 || height <= 0 || size != (gsize) width * height)
		return -1;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			if (data[y * width + x] != (x + y + data[0]) % 255)
				return -1;

	return data[0];
}

/* A frame received downstream, with a copy of its data at that time */

typedef struct {
	GstBuffer *buffer;
	GBytes *snapshot;
	int width;
	int height;
	int stamp;
} TestFrame;

/* Takes a reference to the buffer. No check here: this may run on a streaming thread. */

static TestFrame *
test_frame_new (GstBuffer *buffer, GstCaps *caps)
{
	TestFrame *frame;
	GstStructure *structure;
	gpointer data = NULL;
	gsize size = 0;

	frame = g_new0 (TestFrame, 1);
	frame->buffer = gst_buffer_ref (buffer);

	structure = gst_caps_get_structure (caps, 0);
	gst_structure_get_int (structure, "width", &frame->width);
	gst_structure_get_int (structure, "height", &frame->height);

	gst_buffer_extract_dup (buffer, 0, gst_buffer_get_size (buffer), &data, &size);
	frame->snapshot = g_bytes_new_take (data, size);
	frame->stamp = test_frame_get_stamp (data, size, frame->width, frame->height);

	return frame;
}

static TestFrame *
test_frame_new_from_sample (GstSample *sample)
{
	return test_frame_new (gst_sample_get_buffer (sample), gst_sample_get_caps (sample));
}

/* Checks that the frame came from the fake camera and still has the data it had when received; returns its stamp */

static int
test_frame_check (TestFrame *frame)
{
	GstMapInfo map;
	gboolean intact;

	fail_unless (frame->stamp >= 0, "Not a %dx%d frame of the fake camera", frame->width, frame->height);

	fail_unless (gst_buffer_map (frame->buffer, &map, GST_MAP_READ));
	intact = map.size == g_bytes_get_size (frame->snapshot) &&
		memcmp (map.data, g_bytes_get_data (frame->snapshot, NULL), map.size) == 0;
	gst_buffer_unmap (frame->buffer, &map);

	fail_unless (intact, "Frame %d changed after it was received", frame->stamp);

	return frame->stamp;
}

static void
test_frame_free (TestFrame *frame)
{
	gst_buffer_unref (frame->buffer);
	g_bytes_unref (frame->snapshot);
	g_free (frame);
}

/* Collects what a fakesink with signal-handoffs=true receives: holds the first n_frames_to_hold frames, and counts the
 * buffers of width count_width, or all of them if 0 */

typedef struct {
	GMutex mutex;
	GCond cond;
	guint n_frames_to_hold;
	GPtrArray *frames;
	int count_width;
	guint n_counted;
} TestCollector;

static void
test_collector_handoff (GstElement *sink, GstBuffer *buffer, GstPad *pad, TestCollector *collector)
{
	GstCaps *caps;
	int width = 0;

	caps = gst_pad_get_current_caps (pad);
	if (caps == NULL)
		return;
	gst_structure_get_int (gst_caps_get_structure (caps, 0), "width", &width);

	g_mutex_lock (&collector->mutex);
	if (collector->frames->len < collector->n_frames_to_hold)
		g_ptr_array_add (collector->frames, test_frame_new (buffer, caps));
	if (collector->count_width == 0 || collector->count_width == width)
		collector->n_counted++;
	g_cond_broadcast (&collector->cond);
	g_mutex_unlock (&collector->mutex);

	gst_caps_unref (caps);
}

static TestCollector *
test_collector_new (GstElement *pipeline, guint n_frames_to_hold)
{
	TestCollector *collector;
	GstElement *sink;

	collector = g_new0 (TestCollector, 1);
	g_mutex_init (&collector->mutex);
	g_cond_init (&collector->cond);
	collector->n_frames_to_hold = n_frames_to_hold;
	collector->frames = g_ptr_array_new_with_free_func ((GDestroyNotify) test_frame_free);

	sink = test_pipeline_get_element (pipeline, "sink");
	g_signal_connect (sink, "handoff", G_CALLBACK (test_collector_handoff), collector);
	gst_object_unref (sink);

	return collector;
}

static void
test_collector_set_count_width (TestCollector *collector, int width)
{
	g_mutex_lock (&collector->mutex);
	collector->count_width = width;
	collector->n_counted = 0;
	g_mutex_unlock (&collector->mutex);
}

/* Waits until the collector holds its frames and has counted n_counted buffers. Fails on timeout, or as soon as an
 * error message is posted. */

static void
test_collector_wait (TestCollector *collector, GstElement *pipeline, guint n_counted)
{
	gint64 end_time = g_get_monotonic_time () + TEST_TIMEOUT / GST_USECOND;

	for (;;) {
		GstMessage *message;
		guint n_frames, n;

		g_mutex_lock (&collector->mutex);
		if (collector->frames->len < collector->n_frames_to_hold || collector->n_counted < n_counted)
			g_cond_wait_until (&collector->cond, &collector->mutex,
					   g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND);
		n_frames = collector->frames->len;
		n = collector->n_counted;
		g_mutex_unlock (&collector->mutex);

		if (n_frames >= collector->n_frames_to_hold && n >= n_counted)
			return;

		message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, 0);
		if (message != NULL) {
			GError *error = NULL;

			gst_message_parse_error (message, &error, NULL);
			fail ("Error from %s after %u buffers: %s", GST_MESSAGE_SRC_NAME (message), n, error->message);
		}

		fail_unless (g_get_monotonic_time () < end_time, "Timeout: %u frames held, %u buffers counted",
			     n_frames, n);
	}
}

/* Releases the held frames, and holds no more */

static void
test_collector_release_frames (TestCollector *collector)
{
	GPtrArray *frames;

	g_mutex_lock (&collector->mutex);
	collector->n_frames_to_hold = 0;
	frames = g_steal_pointer (&collector->frames);
	collector->frames = g_ptr_array_new_with_free_func ((GDestroyNotify) test_frame_free);
	g_mutex_unlock (&collector->mutex);

	g_ptr_array_unref (frames);
}

/* Disconnect the collector from the pipeline, by stopping it, before freeing it */

static void
test_collector_free (TestCollector *collector)
{
	g_ptr_array_unref (collector->frames);
	g_mutex_clear (&collector->mutex);
	g_cond_clear (&collector->cond);
	g_free (collector);
}

GST_START_TEST (test_ownership_hold_after_stop)
{
	GstElement *pipeline;
	GstSample *sample;
	TestFrame *frame;
	int n_arv_buffers;

	n_arv_buffers = test_count_arv_buffers ();

	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s ! %s", TEST_CAMERA, TEST_CAPS, TEST_SINK);
	sample = test_pipeline_play (pipeline);
	frame = test_frame_new_from_sample (sample);
	gst_sample_unref (sample);
	test_frame_check (frame);

	/* The stream is destroyed */
	test_pipeline_free (pipeline);

	/* except for the buffer of the frame still held */
	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers + 1);
	test_frame_check (frame);

	test_frame_free (frame);
	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers);
}
GST_END_TEST;

#define TEST_CAPS_CHANGE_N_BUFFERS	10
#define TEST_CAPS_CHANGE_N_HELD		5

GST_START_TEST (test_ownership_caps_change)
{
	GstElement *pipeline;
	GstElement *filter;
	GstCaps *caps;
	TestCollector *collector;
	int n_arv_buffers;
	unsigned int i;

	n_arv_buffers = test_count_arv_buffers ();

	/* No application code between the source and the sink but the collector, which holds the first frames */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s num-arv-buffers=%d"
				      " ! capsfilter name=filter caps=\"video/x-raw,format=GRAY8,width=64,height=48\""
				      " ! fakesink name=sink sync=false signal-handoffs=true",
				      TEST_CAMERA, TEST_CAPS_CHANGE_N_BUFFERS);
	collector = test_collector_new (pipeline, TEST_CAPS_CHANGE_N_HELD);

	test_pipeline_start (pipeline);
	test_collector_wait (collector, pipeline, 0);

	/* The frame size stays the same, so that the new stream's buffers would reuse freed memory */
	filter = test_pipeline_get_element (pipeline, "filter");
	caps = gst_caps_from_string ("video/x-raw,format=GRAY8,width=48,height=64");
	test_collector_set_count_width (collector, 48);
	g_object_set (filter, "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (filter);

	/* Renegotiated: the new stream replaced the old one, which only left the buffers of the held frames */
	test_collector_wait (collector, pipeline, 3);
	fail_unless_equals_int (test_count_arv_buffers (),
				n_arv_buffers + TEST_CAPS_CHANGE_N_BUFFERS + TEST_CAPS_CHANGE_N_HELD);

	/* Without the lock, which a failed check would leave locked: the collector has its frames, and the array no
	 * longer changes */
	for (i = 0; i < collector->frames->len; i++) {
		TestFrame *frame = g_ptr_array_index (collector->frames, i);

		fail_unless_equals_int (frame->width, 64);
		test_frame_check (frame);
	}

	/* Released while the new stream runs, they are freed: they don't join that stream */
	test_collector_release_frames (collector);
	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers + TEST_CAPS_CHANGE_N_BUFFERS);

	test_pipeline_free (pipeline);
	test_collector_free (collector);

	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers);
}
GST_END_TEST;

#define TEST_HOLD_N_BUFFERS	4
#define TEST_HOLD_N_FRAMES	12

GST_START_TEST (test_ownership_hold_many_frames)
{
	GstElement *pipeline;
	GPtrArray *frames;
	unsigned int i, j;

	/* Only 4 buffers for the stream, while the application holds 12 frames, and the camera goes on */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s num-arv-buffers=%d ! %s,framerate=100/1"
				      " ! appsink name=sink sync=false max-buffers=0 drop=false",
				      TEST_CAMERA, TEST_HOLD_N_BUFFERS, TEST_CAPS);

	frames = g_ptr_array_new_with_free_func ((GDestroyNotify) test_frame_free);

	for (i = 0; i < TEST_HOLD_N_FRAMES; i++) {
		GstSample *sample;

		if (i == 0)
			sample = test_pipeline_play (pipeline);
		else
			sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
		fail_unless (sample != NULL, "No frame %u: %s", i, test_pipeline_dup_error_text (pipeline));

		g_ptr_array_add (frames, test_frame_new_from_sample (sample));
		gst_sample_unref (sample);
	}

	/* A few more frames, which would land in reused buffers */
	for (i = 0; i < TEST_HOLD_N_BUFFERS; i++) {
		GstSample *sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);

		fail_unless (sample != NULL, "No frame: %s", test_pipeline_dup_error_text (pipeline));
		gst_sample_unref (sample);
	}

	/* The held frames are intact and all different */
	for (i = 0; i < frames->len; i++) {
		int stamp = test_frame_check (g_ptr_array_index (frames, i));

		for (j = 0; j < i; j++)
			fail_unless (((TestFrame *) g_ptr_array_index (frames, j))->stamp != stamp,
				     "Frames %u and %u are the same", j, i);
	}

	test_pipeline_free (pipeline);
	g_ptr_array_unref (frames);
}
GST_END_TEST;

GST_START_TEST (test_ownership_queue_threshold)
{
	GstElement *pipeline;
	TestCollector *collector;

	/* The queue holds 10 buffers before letting one through, more than the stream has */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s num-arv-buffers=4 ! %s,framerate=100/1"
				      " ! queue min-threshold-buffers=10 ! fakesink name=sink sync=false signal-handoffs=true",
				      TEST_CAMERA, TEST_CAPS);
	collector = test_collector_new (pipeline, 0);

	test_pipeline_start (pipeline);

	/* Frames keep flowing, without timeout error */
	test_collector_wait (collector, pipeline, 50);

	test_pipeline_free (pipeline);
	test_collector_free (collector);
}
GST_END_TEST;

#define TEST_N_CYCLES	100

GST_START_TEST (test_ownership_start_stop_cycles)
{
	GstElement *pipeline;
	int n_arv_buffers;
	unsigned int i;

	n_arv_buffers = test_count_arv_buffers ();

	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s num-arv-buffers=4 ! %s,framerate=100/1 ! %s",
				      TEST_CAMERA, TEST_CAPS, TEST_SINK);

	for (i = 0; i < TEST_N_CYCLES; i++) {
		GstSample *sample;

		sample = test_pipeline_play (pipeline);

		/* A frame released while its stream runs goes back to it, one released after stop is freed */
		if (i % 2 == 0) {
			gst_sample_unref (sample);
			sample = NULL;
		}

		fail_unless_equals_int (test_pipeline_set_state (pipeline, GST_STATE_NULL, TEST_TIMEOUT),
					GST_STATE_CHANGE_SUCCESS);

		if (sample != NULL)
			gst_sample_unref (sample);

		fail_unless (test_count_arv_buffers () == n_arv_buffers, "%d ArvBuffers leaked after %u cycles",
			     test_count_arv_buffers () - n_arv_buffers, i + 1);
	}

	test_pipeline_free (pipeline);
}
GST_END_TEST;

#define TEST_LEND_N_BUFFERS	5
#define TEST_LEND_N_HELD	8
/* GST_ARAVIS_MIN_STREAM_BUFFERS stay with the stream */
#define TEST_LEND_N_LENT	(TEST_LEND_N_BUFFERS - 2)

GST_START_TEST (test_ownership_lending_limit)
{
	GstElement *pipeline;
	GPtrArray *samples;
	int n_arv_buffers;
	unsigned int i;

	n_arv_buffers = test_count_arv_buffers ();

	/* The application holds every frame */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s num-arv-buffers=%d ! %s,framerate=100/1"
				      " ! appsink name=sink sync=false max-buffers=0 drop=false",
				      TEST_CAMERA, TEST_LEND_N_BUFFERS, TEST_CAPS);

	samples = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_sample_unref);
	for (i = 0; i < TEST_LEND_N_HELD; i++) {
		GstSample *sample;

		if (i == 0)
			sample = test_pipeline_play (pipeline);
		else
			sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
		fail_unless (sample != NULL, "No frame %u: %s", i, test_pipeline_dup_error_text (pipeline));
		g_ptr_array_add (samples, sample);
	}

	/* Frames are lent until the stream would keep fewer than 2 buffers, then copied: once the stream is destroyed,
	 * only the buffers of the frames lent remain */
	test_pipeline_free (pipeline);
	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers + TEST_LEND_N_LENT);

	g_ptr_array_unref (samples);
	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers);
}
GST_END_TEST;

/* Counts the live fake streams, like the ArvBuffers, and records how many were alive at most when one was created */

static gint test_n_fake_streams = 0;
static gint test_max_fake_streams = 0;
static void (*test_fake_stream_parent_constructed) (GObject *object) = NULL;

static void
test_fake_stream_finalized (gpointer data, GObject *where_the_object_was)
{
	g_atomic_int_add (&test_n_fake_streams, -1);
}

static void
test_fake_stream_constructed (GObject *object)
{
	gint n_fake_streams;

	if (test_fake_stream_parent_constructed != NULL)
		test_fake_stream_parent_constructed (object);

	g_object_weak_ref (object, test_fake_stream_finalized, NULL);
	n_fake_streams = g_atomic_int_add (&test_n_fake_streams, 1) + 1;
	if (n_fake_streams > g_atomic_int_get (&test_max_fake_streams))
		g_atomic_int_set (&test_max_fake_streams, n_fake_streams);
}

/* Starts recording the most fake streams alive when one is created, from the number alive now. The first call,
 * which installs the hook, must happen while no fake stream exists. */

static void
test_fake_streams_reset (void)
{
	GObjectClass *object_class;

	object_class = g_type_class_ref (ARV_TYPE_FAKE_STREAM);
	if (object_class->constructed != test_fake_stream_constructed) {
		/* Keeps the class reference */
		test_fake_stream_parent_constructed = object_class->constructed;
		object_class->constructed = test_fake_stream_constructed;
	} else
		g_type_class_unref (object_class);

	g_atomic_int_set (&test_max_fake_streams, g_atomic_int_get (&test_n_fake_streams));
}

GST_START_TEST (test_ownership_renegotiation_stream)
{
	GstElement *pipeline;
	GstElement *filter;
	GstCaps *caps;
	TestCollector *collector;

	test_fake_streams_reset ();
	fail_unless_equals_int (g_atomic_int_get (&test_n_fake_streams), 0);

	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s"
				      " ! capsfilter name=filter caps=\"%s,framerate=100/1\""
				      " ! fakesink name=sink sync=false signal-handoffs=true", TEST_CAMERA, TEST_CAPS);
	collector = test_collector_new (pipeline, 0);

	test_pipeline_start (pipeline);
	test_collector_wait (collector, pipeline, 2);

	/* The stream of the element goes before the next one comes: a GenTL producer may refuse a second data stream of
	 * a device */
	filter = test_pipeline_get_element (pipeline, "filter");
	caps = gst_caps_from_string ("video/x-raw,format=GRAY8,width=48,height=64,framerate=100/1");
	test_collector_set_count_width (collector, 48);
	g_object_set (filter, "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (filter);

	test_collector_wait (collector, pipeline, 2);
	fail_unless_equals_int (g_atomic_int_get (&test_max_fake_streams), 1);
	fail_unless_equals_int (g_atomic_int_get (&test_n_fake_streams), 1);

	test_pipeline_free (pipeline);
	test_collector_free (collector);

	fail_unless_equals_int (g_atomic_int_get (&test_n_fake_streams), 0);
}
GST_END_TEST;

/* Counts the acquisition starts of each fake stream: a hook on the start_acquisition () of the ArvFakeStream class
 * counts them in the data of the stream. The first call installs the hook, and must happen while no fake stream
 * exists. */

static gboolean (*test_fake_stream_parent_start_acquisition) (ArvStream *stream, GError **error) = NULL;

static gboolean
test_fake_stream_start_acquisition (ArvStream *stream, GError **error)
{
	gint n_starts = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (stream), "test-n-starts"));

	g_object_set_data (G_OBJECT (stream), "test-n-starts", GINT_TO_POINTER (n_starts + 1));

	return test_fake_stream_parent_start_acquisition (stream, error);
}

static int
test_fake_stream_count_starts (ArvStream *stream)
{
	ArvStreamClass *stream_class;

	stream_class = g_type_class_ref (ARV_TYPE_FAKE_STREAM);
	if (stream_class->start_acquisition != test_fake_stream_start_acquisition) {
		/* Keeps the class reference */
		test_fake_stream_parent_start_acquisition = stream_class->start_acquisition;
		stream_class->start_acquisition = test_fake_stream_start_acquisition;
	} else
		g_type_class_unref (stream_class);

	return stream != NULL ? GPOINTER_TO_INT (g_object_get_data (G_OBJECT (stream), "test-n-starts")) : 0;
}

/* An application following the stream property of an aravissrc through notify::stream, as one reading its statistics
 * would, but ignoring the notifications of the element releasing its stream: it holds the stream of the element, and
 * releases it once the element has created the next one, or with keep_first, holds the first one until the end. It
 * records the streams the element started more than once. The notifications come from the streaming thread. */

typedef struct {
	GMutex mutex;
	gboolean keep_first;
	ArvStream *stream;
	guint n_streams;
	guint n_restarted_streams;
} TestStreamHolder;

static void
test_stream_holder_check (TestStreamHolder *holder, ArvStream *stream)
{
	if (test_fake_stream_count_starts (stream) != 1)
		holder->n_restarted_streams++;
}

static void
test_stream_holder_notify (GstElement *src, GParamSpec *pspec, TestStreamHolder *holder)
{
	ArvStream *stream = NULL;

	g_object_get (src, "stream", &stream, NULL);
	if (stream == NULL)
		return;

	g_mutex_lock (&holder->mutex);
	holder->n_streams++;
	if (holder->stream == NULL)
		holder->stream = g_steal_pointer (&stream);
	else if (!holder->keep_first) {
		test_stream_holder_check (holder, holder->stream);
		g_object_unref (holder->stream);
		holder->stream = g_steal_pointer (&stream);
	}
	g_mutex_unlock (&holder->mutex);

	g_clear_object (&stream);
}

/* An application following a property of an aravissrc, "stream" or "camera", as gst/pipelines.md says: it holds the
 * object of the property, and releases it at each notification, including those of the element releasing it, when
 * the property is NULL. It counts the objects it got. Its signal handler owns it: it stays valid until
 * test_follower_free (), or the finalization of the element after a failed check. */

typedef struct {
	GMutex mutex;
	GObject *object;
	guint n_objects;
} TestFollower;

static void
test_follower_notify (GstElement *src, GParamSpec *pspec, TestFollower *follower)
{
	GObject *object = NULL;
	GObject *previous;

	g_object_get (src, g_param_spec_get_name (pspec), &object, NULL);

	g_mutex_lock (&follower->mutex);
	previous = follower->object;
	follower->object = object;
	if (object != NULL)
		follower->n_objects++;
	g_mutex_unlock (&follower->mutex);

	g_clear_object (&previous);
}

static void
test_follower_destroy (gpointer data, GClosure *closure)
{
	TestFollower *follower = data;

	g_clear_object (&follower->object);
	g_mutex_clear (&follower->mutex);
	g_free (follower);
}

static TestFollower *
test_follower_new (GstElement *src, const char *property_name)
{
	TestFollower *follower;
	char *signal_name;

	follower = g_new0 (TestFollower, 1);
	g_mutex_init (&follower->mutex);

	signal_name = g_strdup_printf ("notify::%s", property_name);
	g_signal_connect_data (src, signal_name, G_CALLBACK (test_follower_notify), follower, test_follower_destroy, 0);
	g_free (signal_name);

	return follower;
}

/* Whether the follower holds an object now */

static gboolean
test_follower_holds_object (TestFollower *follower)
{
	gboolean holds_object;

	g_mutex_lock (&follower->mutex);
	holds_object = follower->object != NULL;
	g_mutex_unlock (&follower->mutex);

	return holds_object;
}

static guint
test_follower_get_n_objects (TestFollower *follower)
{
	guint n_objects;

	g_mutex_lock (&follower->mutex);
	n_objects = follower->n_objects;
	g_mutex_unlock (&follower->mutex);

	return n_objects;
}

/* Stops following: releases the object held and frees the follower */

static void
test_follower_free (TestFollower *follower, GstElement *src)
{
	g_signal_handlers_disconnect_by_data (src, follower);
}

#define TEST_HELD_STREAM_N_CHANGES	3

GST_START_TEST (test_ownership_renegotiation_held_stream)
{
	static const gboolean keep_first_cases[] = {FALSE, TRUE};
	unsigned int i;

	/* The element restarts and stops its own stream only, not every stream of the camera: the old stream of the
	 * element, which the application still holds, stays stopped. On a USB3Vision camera, restarted, it would compete
	 * with the new stream for the frames, and stopped when the application releases it, it would disable the
	 * streaming of the camera. */
	test_fake_stream_count_starts (NULL);

	for (i = 0; i < G_N_ELEMENTS (keep_first_cases); i++) {
		TestStreamHolder holder = {0};
		GstElement *pipeline;
		GstElement *src;
		GstElement *filter;
		TestCollector *collector;
		unsigned int j;

		g_mutex_init (&holder.mutex);
		holder.keep_first = keep_first_cases[i];

		pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s"
					      " ! capsfilter name=filter caps=\"%s,framerate=100/1\""
					      " ! fakesink name=sink sync=false signal-handoffs=true", TEST_CAMERA,
					      TEST_CAPS);
		src = test_pipeline_get_element (pipeline, "src");
		g_signal_connect (src, "notify::stream", G_CALLBACK (test_stream_holder_notify), &holder);
		collector = test_collector_new (pipeline, 0);

		test_pipeline_start (pipeline);
		test_collector_wait (collector, pipeline, 2);

		/* Frames of each new size flow */
		filter = test_pipeline_get_element (pipeline, "filter");
		for (j = 0; j < TEST_HELD_STREAM_N_CHANGES; j++) {
			int width = j % 2 == 0 ? 48 : 64;
			int height = j % 2 == 0 ? 64 : 48;
			GstCaps *caps;

			caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "GRAY8",
						    "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
						    "framerate", GST_TYPE_FRACTION, 100, 1, NULL);
			test_collector_set_count_width (collector, width);
			g_object_set (filter, "caps", caps, NULL);
			gst_caps_unref (caps);

			test_collector_wait (collector, pipeline, 2);
		}
		gst_object_unref (filter);

		test_pipeline_free (pipeline);
		test_collector_free (collector);

		/* Every stream of the element was started once, the one still held included */
		g_signal_handlers_disconnect_by_data (src, &holder);
		gst_object_unref (src);
		test_stream_holder_check (&holder, holder.stream);
		fail_unless_equals_int (holder.n_streams, 1 + TEST_HELD_STREAM_N_CHANGES);
		fail_unless (holder.n_restarted_streams == 0, "%u streams restarted%s", holder.n_restarted_streams,
			     holder.keep_first ? ", holding the first one" : "");
		g_clear_object (&holder.stream);
		g_mutex_clear (&holder.mutex);
	}
}
GST_END_TEST;

/* Renegotiations through the stream channel of a GigEVision camera, which the finalization of a stream disables,
 * unless a newer stream has configured the channel since: the in-process fake GigEVision camera, on 127.0.0.1. It
 * binds the fixed GigEVision ports, so these checks only run in the test registered in the 'network' suite, with
 * GST_ARAVIS_TEST_NETWORK=1, alone. The element opens the camera by its address, which needs no discovery broadcast. */

#define TEST_GV_ADDRESS		"127.0.0.1"

static ArvGvFakeCamera *test_gv_fake_camera = NULL;

static void
setup_network (void)
{
	setup ();

	test_gv_fake_camera = arv_gv_fake_camera_new (TEST_GV_ADDRESS, "GstTest");
	fail_unless (ARV_IS_GV_FAKE_CAMERA (test_gv_fake_camera));
}

static void
teardown_network (void)
{
	teardown_leftovers ();
	g_clear_object (&test_gv_fake_camera);
	test_aravis_log_check ();
}

#define TEST_GV_N_FRAMES	3
#define TEST_GV_PIPELINE	"aravissrc name=src interface=GigEVision camera-name=" TEST_GV_ADDRESS \
				" ! capsfilter name=filter caps=\"" TEST_CAPS ",framerate=20/1\" ! " TEST_SINK
#define TEST_GV_NEW_CAPS	"video/x-raw,format=GRAY8,width=48,height=64,framerate=20/1"
#define TEST_GV_NEW_WIDTH	48

/* Changes the caps of the pipeline built from TEST_GV_PIPELINE to TEST_GV_NEW_CAPS */

static void
test_network_gv_change_caps (GstElement *pipeline)
{
	GstElement *filter;
	GstCaps *caps;

	filter = test_pipeline_get_element (pipeline, "filter");
	caps = gst_caps_from_string (TEST_GV_NEW_CAPS);
	g_object_set (filter, "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (filter);
}

/* Frames of the new size come, TEST_GV_N_FRAMES of them, after those of the old size the sink may still hold */

static void
test_network_gv_pull_new_frames (GstElement *pipeline, const char *when)
{
	unsigned int n_frames = 0;
	unsigned int i;

	for (i = 0; i < 10 * TEST_GV_N_FRAMES && n_frames < TEST_GV_N_FRAMES; i++) {
		GstSample *sample;
		TestFrame *frame;

		sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
		fail_unless (sample != NULL, "No frame %s: %s", when, test_pipeline_dup_error_text (pipeline));
		frame = test_frame_new_from_sample (sample);
		gst_sample_unref (sample);

		if (frame->width == TEST_GV_NEW_WIDTH) {
			test_frame_check (frame);
			n_frames++;
		}
		test_frame_free (frame);
	}

	fail_unless_equals_int (n_frames, TEST_GV_N_FRAMES);
	test_pipeline_check_no_error (pipeline, when);
}

/* With follow_stream, an application follows the stream property: it releases the stream of the previous caps when
 * the element releases it, before the element creates the next one */

static void
test_network_gv_renegotiate (gboolean follow_stream)
{
	TestFollower *follower = NULL;
	GstElement *pipeline;
	GstElement *src = NULL;

	pipeline = test_pipeline_new ("%s", TEST_GV_PIPELINE);
	if (follow_stream) {
		src = test_pipeline_get_element (pipeline, "src");
		follower = test_follower_new (src, "stream");
	}
	gst_sample_unref (test_pipeline_play (pipeline));

	test_network_gv_change_caps (pipeline);
	test_network_gv_pull_new_frames (pipeline, "after the caps change");

	test_pipeline_free (pipeline);

	if (follow_stream) {
		/* The stream of each caps, released when the element stopped */
		fail_unless_equals_int (test_follower_get_n_objects (follower), 2);
		fail_if (test_follower_holds_object (follower));
		test_follower_free (follower, src);
		gst_object_unref (src);
	}
}

GST_START_TEST (test_network_gv_renegotiation)
{
	test_network_gv_renegotiate (FALSE);
}
GST_END_TEST;

GST_START_TEST (test_network_gv_renegotiation_followed_stream)
{
	test_network_gv_renegotiate (TRUE);
}
GST_END_TEST;

/* An application holds the first stream of the element across the caps change, and releases it once frames of the new
 * caps flow: the finalization of the first stream leaves the stream channel to the stream of the new caps. The fake
 * camera keeps sending to the destination it had when its acquisition started, so the frames don't show it: the port
 * register of the stream channel does. */

#define TEST_GV_STREAM_CHANNEL_0_PORT	0xd00

static guint32
test_network_gv_read_stream_channel_port (ArvCamera *camera)
{
	GError *error = NULL;
	guint32 value = 0;

	arv_device_read_register (arv_camera_get_device (camera), TEST_GV_STREAM_CHANNEL_0_PORT, &value, &error);
	fail_unless (error == NULL, "Failed to read the stream channel port: %s", error != NULL ? error->message : "");

	return value;
}

/* Whether the first stream is finalized. Static, as the stream may outlive a failed check. */
static gint test_network_gv_first_stream_finalized = FALSE;

/* Destroy notify of a data set on the stream. GObject destroys the data of an object at the end of its finalization, so
 * after arv_gv_stream_finalize () wrote the register, whichever thread dropped the last reference. A weak reference is
 * notified earlier, when the object is disposed. */

static void
test_network_gv_stream_finalized (gpointer data)
{
	g_atomic_int_set ((gint *) data, TRUE);
}

GST_START_TEST (test_network_gv_renegotiation_held_stream)
{
	TestStreamHolder holder = {0};
	GstElement *pipeline;
	GstElement *src;
	ArvCamera *camera;
	ArvStream *first_stream;
	gint64 end_time;
	guint32 port;

	g_mutex_init (&holder.mutex);
	holder.keep_first = TRUE;
	g_atomic_int_set (&test_network_gv_first_stream_finalized, FALSE);

	pipeline = test_pipeline_new ("%s", TEST_GV_PIPELINE);
	src = test_pipeline_get_element (pipeline, "src");
	g_signal_connect (src, "notify::stream", G_CALLBACK (test_stream_holder_notify), &holder);
	gst_sample_unref (test_pipeline_play (pipeline));

	test_network_gv_change_caps (pipeline);
	test_network_gv_pull_new_frames (pipeline, "after the caps change");

	/* The channel sends to the stream of the new caps */
	camera = test_element_dup_camera (src);
	port = test_network_gv_read_stream_channel_port (camera);
	fail_if (port == 0, "Stream channel disabled after the caps change");

	g_mutex_lock (&holder.mutex);
	first_stream = g_steal_pointer (&holder.stream);
	g_mutex_unlock (&holder.mutex);
	fail_unless (ARV_IS_GV_STREAM (first_stream));
	g_object_set_data_full (G_OBJECT (first_stream), "test-finalized", &test_network_gv_first_stream_finalized,
				test_network_gv_stream_finalized);
	g_object_unref (first_stream);

	/* A frame of the first stream released downstream meanwhile may hold it for a moment, and finalize it on
	 * another thread */
	end_time = g_get_monotonic_time () + GST_TIME_AS_USECONDS (TEST_TIMEOUT);
	while (!g_atomic_int_get (&test_network_gv_first_stream_finalized) && g_get_monotonic_time () < end_time)
		g_usleep (1000);
	fail_unless (g_atomic_int_get (&test_network_gv_first_stream_finalized), "The first stream is still alive");

	/* and it still does */
	fail_unless_equals_uint64 (test_network_gv_read_stream_channel_port (camera), port);
	g_object_unref (camera);

	test_network_gv_pull_new_frames (pipeline, "after the release of the first stream");

	test_pipeline_free (pipeline);

	/* The stream of each caps */
	g_signal_handlers_disconnect_by_data (src, &holder);
	gst_object_unref (src);
	fail_unless_equals_int (holder.n_streams, 2);
	g_mutex_clear (&holder.mutex);
}
GST_END_TEST;

/* Device loss and stalls: create () can be interrupted, and a lost camera, a stalled stream or a stream of failed
 * frames end it promptly with an error that says why */

/* The longest acceptable delay to stop or to report a lost camera: a wait slice of create (), 50 ms, and a frame
 * period of the fake camera at TEST_FAST_CAPS, 10 ms, which its stream thread may sleep before it stops, plus
 * scheduling */
#define TEST_PROMPT		(200 * GST_MSECOND)
#define TEST_FAST_CAPS		TEST_CAPS ",framerate=100/1"

static gint64
test_elapsed_ms (gint64 start_time)
{
	return (g_get_monotonic_time () - start_time) / 1000;
}

/* Returns the fake camera behind the camera of an aravissrc (transfer none: valid while the element keeps that
 * camera) */

static ArvFakeCamera *
test_element_get_fake_camera (GstElement *src)
{
	ArvCamera *camera;
	ArvDevice *device;

	camera = test_element_dup_camera (src);
	device = arv_camera_get_device (camera);
	fail_unless (ARV_IS_FAKE_DEVICE (device));
	g_object_unref (camera);

	return arv_fake_device_get_fake_camera (ARV_FAKE_DEVICE (device));
}

/* Holds every frame the fake camera starts to fill until the acquisition stops. The stream starves, as with a camera
 * waiting for a trigger, while the stream thread still stops quickly, unlike with a long frame period. */

static void
test_starving_fill_pattern (ArvBuffer *buffer, void *fill_pattern_data, guint32 exposure_time_us, guint32 gain,
			    ArvPixelFormat pixel_format)
{
	ArvFakeCamera *fake_camera = fill_pattern_data;

	while (arv_fake_camera_get_acquisition_status (fake_camera) != 0)
		g_usleep (1000);
}

/* Starves the stream of a playing aravissrc, and waits until create () surely waits for a frame. Each new camera, so
 * each start of the element, comes with the default fill pattern. */

static void
test_element_starve (GstElement *src)
{
	ArvFakeCamera *fake_camera = test_element_get_fake_camera (src);

	arv_fake_camera_set_fill_pattern (fake_camera, test_starving_fill_pattern, fake_camera, NULL);

	/* create () pushes the frames already filled, then waits */
	g_usleep (200 * G_TIME_SPAN_MILLISECOND);
}

/* Makes every following frame of the fake camera fail: they no longer fit in the buffers of the stream, and the camera
 * reports a size mismatch */

static void
test_element_fail_frames (GstElement *src)
{
	ArvFakeCamera *fake_camera = test_element_get_fake_camera (src);
	guint32 width = 0;

	fail_unless (arv_fake_camera_read_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, &width));
	fail_unless (arv_fake_camera_write_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, 2 * width));
}

/* Waits for an error message (transfer full); fails on timeout */

static GstMessage *
test_pipeline_pop_error (GstElement *pipeline, GError **error, char **debug)
{
	GstMessage *message;

	message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, TEST_TIMEOUT);
	fail_unless (message != NULL, "No error message within %" GST_TIME_FORMAT, GST_TIME_ARGS (TEST_TIMEOUT));

	gst_message_parse_error (message, error, debug);
	GST_INFO ("Error from %s: %s (%s)", GST_MESSAGE_SRC_NAME (message), (*error)->message,
		  *debug != NULL ? *debug : "no details");

	return message;
}

GST_START_TEST (test_unlock_buffer_timeout_property)
{
	static const gint timeouts[] = {6000, 0, -1};
	GParamSpecInt *pspec_int;
	GParamSpec *pspec;
	GstElement *src;
	gint timeout = 1;
	unsigned int i;

	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);

	/* In ms; -1, the default, keeps the automatic timeout, 0 waits forever */
	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (src), "buffer-timeout");
	fail_unless (pspec != NULL);
	fail_unless (G_IS_PARAM_SPEC_INT (pspec));
	fail_unless ((pspec->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
	pspec_int = G_PARAM_SPEC_INT (pspec);
	fail_unless_equals_int (pspec_int->default_value, -1);
	fail_unless_equals_int (pspec_int->minimum, -1);
	fail_unless_equals_int (pspec_int->maximum, G_MAXINT);

	g_object_get (src, "buffer-timeout", &timeout, NULL);
	fail_unless_equals_int (timeout, -1);

	for (i = 0; i < G_N_ELEMENTS (timeouts); i++) {
		g_object_set (src, "buffer-timeout", timeouts[i], NULL);
		g_object_get (src, "buffer-timeout", &timeout, NULL);
		fail_unless_equals_int (timeout, timeouts[i]);
	}

	gst_object_unref (src);
}
GST_END_TEST;

GST_START_TEST (test_unlock_stop_while_waiting)
{
	static const struct {
		const char *timeout;
		gboolean fail_frames;
	} cases[] = {
		/* The automatic timeout is 2 s */
		{"", FALSE},
		{"buffer-timeout=6000", FALSE},
		{"buffer-timeout=0", FALSE},
		/* Failed frames, which restart the wait until there are 10 of them */
		{"buffer-timeout=6000", TRUE}
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (cases); i++) {
		GstElement *pipeline;
		GstElement *src;
		GstStateChangeReturn result;
		gint64 start_time;
		gint64 elapsed_ms;

		pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s %s ! %s ! %s",
					      TEST_CAMERA, cases[i].timeout, TEST_FAST_CAPS, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		if (cases[i].fail_frames) {
			test_element_fail_frames (src);
			g_usleep (200 * G_TIME_SPAN_MILLISECOND);
		} else
			test_element_starve (src);

		start_time = g_get_monotonic_time ();
		result = test_pipeline_set_state (pipeline, GST_STATE_NULL, TEST_TIMEOUT);
		elapsed_ms = test_elapsed_ms (start_time);
		GST_INFO ("Stopped in %" G_GINT64_FORMAT " ms with '%s'%s", elapsed_ms, cases[i].timeout,
			  cases[i].fail_frames ? " and failed frames" : "");

		fail_unless_equals_int (result, GST_STATE_CHANGE_SUCCESS);
		fail_unless (elapsed_ms <= (gint64) (TEST_PROMPT / GST_MSECOND),
			     "Stopping took %" G_GINT64_FORMAT " ms with '%s'%s", elapsed_ms, cases[i].timeout,
			     cases[i].fail_frames ? " and failed frames" : "");

		gst_object_unref (src);
		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

#define TEST_PAUSE_N_CYCLES	6

GST_START_TEST (test_unlock_pause_resume)
{
	GstElement *pipeline;
	GstElement *src;
	gint64 start_time;
	gint64 elapsed_ms;
	int i;

	/* From GStreamer 1.24 on, pausing a live source unlocks create (). Resumed at once, before create () notices,
	 * frames must flow again. At 5 Hz, create () mostly waits without frame, and notices the unlock at the end of
	 * a wait slice, after the resume. */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s,framerate=5/1 ! %s", TEST_CAMERA,
				      TEST_CAPS, TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	for (i = 0; i < TEST_PAUSE_N_CYCLES; i++) {
		GstStateChangeReturn result;
		GstSample *sample;

		fail_unless_equals_int (test_pipeline_set_state (pipeline, GST_STATE_PAUSED, TEST_TIMEOUT),
					GST_STATE_CHANGE_NO_PREROLL);
		/* And a longer pause at times */
		if (i % 3 == 2)
			g_usleep (100 * G_TIME_SPAN_MILLISECOND);

		result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
		fail_unless (result == GST_STATE_CHANGE_SUCCESS || result == GST_STATE_CHANGE_NO_PREROLL,
			     "Can't resume (%s) after %d pauses: %s", gst_element_state_change_return_get_name (result),
			     i + 1, test_pipeline_dup_error_text (pipeline));

		sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
		fail_unless (sample != NULL, "No frame after %d pauses: %s", i + 1, test_pipeline_dup_error_text (pipeline));
		gst_sample_unref (sample);
	}

	test_pipeline_free (pipeline);

	/* Stopping a paused element unblocks create (), even waiting for a frame */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s ! %s", TEST_CAMERA, TEST_FAST_CAPS,
				      TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	src = test_pipeline_get_element (pipeline, "src");
	test_element_starve (src);
	fail_unless_equals_int (test_pipeline_set_state (pipeline, GST_STATE_PAUSED, TEST_TIMEOUT),
				GST_STATE_CHANGE_NO_PREROLL);
	g_usleep (100 * G_TIME_SPAN_MILLISECOND);

	start_time = g_get_monotonic_time ();
	fail_unless_equals_int (test_pipeline_set_state (pipeline, GST_STATE_NULL, TEST_TIMEOUT),
				GST_STATE_CHANGE_SUCCESS);
	elapsed_ms = test_elapsed_ms (start_time);
	fail_unless (elapsed_ms <= (gint64) (TEST_PROMPT / GST_MSECOND),
		     "Stopping took %" G_GINT64_FORMAT " ms", elapsed_ms);

	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_unlock_access_while_waiting)
{
	GstElement *pipeline;
	GstElement *src;
	ArvStream *stream = NULL;
	ArvCamera *camera = NULL;
	char *camera_name = NULL;
	GstClockTime min_latency = 0;
	gint64 start_time;
	gint64 elapsed_ms;

	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s ! %s", TEST_CAMERA, TEST_FAST_CAPS,
				      TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	src = test_pipeline_get_element (pipeline, "src");
	test_element_starve (src);

	/* create () no longer holds the object lock while it waits */
	start_time = g_get_monotonic_time ();
	g_object_get (src, "stream", &stream, "camera", &camera, "camera-name", &camera_name, NULL);
	g_object_set (src, "gain", 1.0, NULL);
	fail_unless (test_element_query_latency (src, NULL, &min_latency, NULL));
	elapsed_ms = test_elapsed_ms (start_time);
	GST_INFO ("Accessed in %" G_GINT64_FORMAT " ms", elapsed_ms);

	fail_unless (elapsed_ms <= 100, "Property accesses and query took %" G_GINT64_FORMAT " ms", elapsed_ms);
	fail_unless (ARV_IS_STREAM (stream));
	fail_unless (ARV_IS_CAMERA (camera));
	fail_unless_equals_string (camera_name, TEST_CAMERA);
	fail_unless_equals_uint64 (min_latency, 10 * GST_MSECOND);

	g_object_unref (stream);
	g_object_unref (camera);
	g_free (camera_name);
	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_unlock_control_lost)
{
	static const struct {
		const char *description;
		gboolean starve;
	} cases[] = {
		/* While frames flow */
		{"aravissrc name=src camera-name=" TEST_CAMERA, FALSE},
		/* While create () waits for a frame */
		{"aravissrc name=src camera-name=" TEST_CAMERA, TRUE},
		/* The first camera found, without camera-name */
		{"aravissrc name=src interface=Fake", TRUE}
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (cases); i++) {
		GstElement *pipeline;
		GstElement *src;
		GstMessage *message;
		ArvCamera *camera;
		GError *error = NULL;
		char *debug = NULL;
		char *label;
		gint64 start_time;
		gint64 elapsed_ms;

		pipeline = test_pipeline_new ("%s ! %s ! %s", cases[i].description, TEST_FAST_CAPS, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		if (cases[i].starve)
			test_element_starve (src);

		/* Named as camera-name names it, or as the interfaces identify devices: vendor-model-serial */
		camera = test_element_dup_camera (src);
		if (strstr (cases[i].description, "camera-name=") != NULL)
			label = g_strdup (TEST_CAMERA);
		else
			label = g_strdup_printf ("%s-%s-%s", arv_camera_get_vendor_name (camera, NULL),
						 arv_camera_get_model_name (camera, NULL),
						 arv_camera_get_device_serial_number (camera, NULL));

		start_time = g_get_monotonic_time ();
		g_signal_emit_by_name (arv_camera_get_device (camera), "control-lost");

		/* The first error says what happened, before the generic one of GstBaseSrc */
		message = test_pipeline_pop_error (pipeline, &error, &debug);
		elapsed_ms = test_elapsed_ms (start_time);
		GST_INFO ("Error after %" G_GINT64_FORMAT " ms", elapsed_ms);

		fail_unless_equals_string (GST_MESSAGE_SRC_NAME (message), "src");
		fail_unless (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ),
			     "Unexpected error %s", error->message);
		fail_unless (strstr (error->message, label) != NULL, "'%s' not in '%s'", label, error->message);
		fail_unless (strstr (error->message, "Lost control") != NULL, "Unexpected error %s", error->message);
		fail_unless (debug != NULL && strstr (debug, "control-lost") != NULL);
		fail_unless (elapsed_ms <= (gint64) (TEST_PROMPT / GST_MSECOND),
			     "The error came after %" G_GINT64_FORMAT " ms", elapsed_ms);

		test_pipeline_expect_error (pipeline, "src", GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, NULL);

		g_error_free (error);
		g_free (debug);
		g_free (label);
		gst_message_unref (message);
		g_object_unref (camera);
		gst_object_unref (src);
		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

GST_START_TEST (test_unlock_timeout)
{
	GstElement *pipeline;
	GstElement *src;
	GstMessage *message;
	GError *error = NULL;
	char *debug = NULL;

	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s buffer-timeout=300 ! %s ! %s",
				      TEST_CAMERA, TEST_FAST_CAPS, TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	src = test_pipeline_get_element (pipeline, "src");
	test_element_starve (src);

	/* The first error says what happened, with the stream statistics in the details */
	message = test_pipeline_pop_error (pipeline, &error, &debug);
	fail_unless_equals_string (GST_MESSAGE_SRC_NAME (message), "src");
	fail_unless (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ),
		     "Unexpected error %s", error->message);
	fail_unless_equals_string (error->message, "No frame received from camera \"" TEST_CAMERA "\" within 300 ms");
	fail_unless (debug != NULL && strstr (debug, "n_completed_buffers") != NULL, "No statistics in '%s'", debug);

	/* GstBaseSrc's generic error comes next */
	test_pipeline_expect_error (pipeline, "src", GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, NULL);

	g_error_free (error);
	g_free (debug);
	gst_message_unref (message);
	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_unlock_wait_forever)
{
	GstElement *pipeline;
	GstElement *src;
	GstMessage *message;

	/* Longer than the automatic timeout, 2 s, and still waiting */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s buffer-timeout=0 ! %s ! %s",
				      TEST_CAMERA, TEST_FAST_CAPS, TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	src = test_pipeline_get_element (pipeline, "src");
	test_element_starve (src);

	message = test_pipeline_pop_message (pipeline, GST_MESSAGE_ERROR, 2500 * GST_MSECOND);
	if (message != NULL) {
		GError *error = NULL;

		gst_message_parse_error (message, &error, NULL);
		fail ("Error while waiting forever: %s", error->message);
	}

	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_unlock_failed_frames)
{
	static const struct {
		const char *timeout;
		const char *text;
		gint64 max_ms;
	} cases[] = {
		/* A stream of failed frames ends at the automatic timeout: from the 10th failed frame on, they no longer
		 * restart it */
		{"", "No complete frame received from camera \"" TEST_CAMERA "\" within 2000 ms", 2000},
		/* Without timeout, failed frames in a row end the wait: 10 frames at 100 Hz */
		{"buffer-timeout=0", "Camera \"" TEST_CAMERA "\" sent 10 failed frames in a row", 1000}
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (cases); i++) {
		GstElement *pipeline;
		GstElement *src;
		GstMessage *message;
		GError *error = NULL;
		char *debug = NULL;
		gint64 start_time;
		gint64 elapsed_ms;

		pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s %s ! %s ! %s",
					      TEST_CAMERA, cases[i].timeout, TEST_FAST_CAPS, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		start_time = g_get_monotonic_time ();
		test_element_fail_frames (src);

		message = test_pipeline_pop_error (pipeline, &error, &debug);
		elapsed_ms = test_elapsed_ms (start_time);
		GST_INFO ("Error after %" G_GINT64_FORMAT " ms", elapsed_ms);

		fail_unless_equals_string (GST_MESSAGE_SRC_NAME (message), "src");
		fail_unless (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ),
			     "Unexpected error %s", error->message);
		fail_unless (strstr (error->message, cases[i].text) != NULL, "'%s' not in '%s'",
			     cases[i].text, error->message);
		fail_unless (strstr (error->message, "(last status: size mismatch)") != NULL,
			     "No status in '%s'", error->message);
		fail_unless (debug != NULL && strstr (debug, "n_failures") != NULL, "No statistics in '%s'", debug);
		fail_unless (elapsed_ms <= cases[i].max_ms + (gint64) (TEST_PROMPT / GST_MSECOND),
			     "The error came after %" G_GINT64_FORMAT " ms", elapsed_ms);

		test_pipeline_expect_error (pipeline, "src", GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, NULL);

		g_error_free (error);
		g_free (debug);
		gst_message_unref (message);
		gst_object_unref (src);
		test_pipeline_free (pipeline);
	}
}
GST_END_TEST;

GST_START_TEST (test_unlock_failed_frames_control_lost)
{
	GstElement *pipeline;
	GstElement *src;
	GstMessage *message;
	ArvStream *stream = NULL;
	ArvCamera *camera;
	GError *error = NULL;
	char *debug = NULL;
	guint64 n_failures = 0;
	gint64 end_time;

	/* Without timeout, 10 failed frames in a row end the wait. A camera that is unplugged or switched off may fail
	 * as many before its device notices the loss: the error then says that the camera was lost, if the device emits
	 * control-lost within 250 ms. */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s buffer-timeout=0 ! %s ! %s",
				      TEST_CAMERA, TEST_FAST_CAPS, TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	src = test_pipeline_get_element (pipeline, "src");
	g_object_get (src, "stream", &stream, NULL);
	fail_unless (ARV_IS_STREAM (stream));
	camera = test_element_dup_camera (src);

	test_element_fail_frames (src);

	/* Once create () has its 10 failed frames, and a frame period later */
	end_time = g_get_monotonic_time () + TEST_TIMEOUT / GST_USECOND;
	do {
		g_usleep (2 * G_TIME_SPAN_MILLISECOND);
		arv_stream_get_statistics (stream, NULL, &n_failures, NULL);
	} while (n_failures < 11 && g_get_monotonic_time () < end_time);
	fail_unless (n_failures >= 11, "%" G_GUINT64_FORMAT " failed frames", n_failures);
	g_usleep (20 * G_TIME_SPAN_MILLISECOND);

	g_signal_emit_by_name (arv_camera_get_device (camera), "control-lost");

	message = test_pipeline_pop_error (pipeline, &error, &debug);
	fail_unless_equals_string (GST_MESSAGE_SRC_NAME (message), "src");
	fail_unless (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ),
		     "Unexpected error %s", error->message);
	fail_unless_equals_string (error->message, "Lost control of camera \"" TEST_CAMERA "\"");

	test_pipeline_expect_error (pipeline, "src", GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, NULL);

	g_error_free (error);
	g_free (debug);
	gst_message_unref (message);
	g_object_unref (camera);
	g_object_unref (stream);
	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_unlock_failed_frame_restarts_wait)
{
	GstElement *pipeline;
	GstElement *src;
	GstElement *sink;
	GstSample *sample;
	ArvStream *stream = NULL;
	ArvFakeCamera *fake_camera;
	TestFrame *frame;
	guint64 n_failures = 0;
	guint32 width = 0;
	gint64 end_time;

	/* A frame every 400 ms, as from a triggered camera, and a timeout of 600 ms. The complete frame after a failed
	 * one comes 800 ms after the previous complete one, but 400 ms after the failed one, which restarts the wait. */
	pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s buffer-timeout=600 ! %s,framerate=5/2 ! %s",
				      TEST_CAMERA, TEST_CAPS, TEST_SINK);
	gst_sample_unref (test_pipeline_play (pipeline));

	src = test_pipeline_get_element (pipeline, "src");
	sink = test_pipeline_get_element (pipeline, "sink");
	g_object_get (src, "stream", &stream, NULL);
	fail_unless (ARV_IS_STREAM (stream));
	fake_camera = test_element_get_fake_camera (src);

	/* The next frame fails: it no longer fits in the buffers of the stream */
	fail_unless (arv_fake_camera_read_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, &width));
	fail_unless (arv_fake_camera_write_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, 2 * width));

	end_time = g_get_monotonic_time () + TEST_TIMEOUT / GST_USECOND;
	do {
		g_usleep (2 * G_TIME_SPAN_MILLISECOND);
		arv_stream_get_statistics (stream, NULL, &n_failures, NULL);
	} while (n_failures == 0 && g_get_monotonic_time () < end_time);
	fail_unless (n_failures > 0, "No failed frame");

	/* Drop the frame the sink may hold, from before the failed one, as the next ones fail. Those after this one are
	 * complete again. */
	sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), 0);
	if (sample != NULL)
		gst_sample_unref (sample);
	fail_unless (arv_fake_camera_write_register (fake_camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, width));

	/* No timeout error: the stream goes on */
	sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
	fail_unless (sample != NULL, "No frame after a failed one: %s", test_pipeline_dup_error_text (pipeline));
	frame = test_frame_new_from_sample (sample);
	gst_sample_unref (sample);
	test_frame_check (frame);
	test_frame_free (frame);
	test_pipeline_check_no_error (pipeline, "after a failed frame");

	arv_stream_get_statistics (stream, NULL, &n_failures, NULL);
	GST_INFO ("%" G_GUINT64_FORMAT " failed frames", n_failures);

	g_object_unref (stream);
	gst_object_unref (sink);
	gst_object_unref (src);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

/* Emits control-lost on a device until told to stop */

typedef struct {
	ArvDevice *device;
	gint stop;
	gint n_emissions;
} TestEmitter;

static gpointer
test_emitter_run (gpointer data)
{
	TestEmitter *emitter = data;

	while (!g_atomic_int_get (&emitter->stop)) {
		g_signal_emit_by_name (emitter->device, "control-lost");
		g_atomic_int_inc (&emitter->n_emissions);
	}

	return NULL;
}

/* Starts emitting control-lost on the device of the camera of an aravissrc, from another thread, and waits for the
 * first emissions */

static GThread *
test_emitter_start (TestEmitter *emitter, GstElement *src)
{
	ArvCamera *camera;
	GThread *thread;

	camera = test_element_dup_camera (src);
	emitter->device = g_object_ref (arv_camera_get_device (camera));
	emitter->stop = FALSE;
	emitter->n_emissions = 0;
	g_object_unref (camera);

	thread = g_thread_new ("control-lost", test_emitter_run, emitter);
	while (g_atomic_int_get (&emitter->n_emissions) < 10)
		g_thread_yield ();

	return thread;
}

static void
test_emitter_stop (TestEmitter *emitter, GThread *thread)
{
	g_atomic_int_set (&emitter->stop, TRUE);
	g_thread_join (thread);
	g_clear_object (&emitter->device);
}

#define TEST_EMISSION_N_ELEMENTS	100
#define TEST_EMISSION_N_PIPELINES	10

GST_START_TEST (test_unlock_control_lost_dispose)
{
	gpointer device_class;
	guint signal_id;
	unsigned int i;

	/* The class registers its signals */
	device_class = g_type_class_ref (ARV_TYPE_DEVICE);
	signal_id = g_signal_lookup ("control-lost", ARV_TYPE_DEVICE);
	g_type_class_unref (device_class);
	fail_unless (signal_id != 0);

	/* Elements finalized while control-lost is emitted on their camera's device, which outlives them. The handler
	 * must not touch the element: under AddressSanitizer, a flag in the element is a use after free here. */
	for (i = 0; i < TEST_EMISSION_N_ELEMENTS; i++) {
		TestEmitter emitter;
		GstElement *src;
		GThread *thread;

		src = gst_element_factory_make ("aravissrc", NULL);
		fail_unless (src != NULL);
		gst_object_ref_sink (src);
		g_object_set (src, "camera-name", TEST_CAMERA, NULL);

		thread = test_emitter_start (&emitter, src);

		/* The element watches the device of its camera from the opening on, */
		fail_unless (g_signal_has_handler_pending (emitter.device, signal_id, 0, FALSE));

		gst_object_unref (src);

		/* until it releases the camera */
		fail_if (g_signal_has_handler_pending (emitter.device, signal_id, 0, FALSE));

		test_emitter_stop (&emitter, thread);
	}

	/* The same when stopping a stream */
	for (i = 0; i < TEST_EMISSION_N_PIPELINES; i++) {
		TestEmitter emitter;
		GstElement *pipeline;
		GstElement *src;
		GThread *thread;

		pipeline = test_pipeline_new ("aravissrc name=src camera-name=%s ! %s ! %s", TEST_CAMERA,
					      TEST_FAST_CAPS, TEST_SINK);
		gst_sample_unref (test_pipeline_play (pipeline));

		src = test_pipeline_get_element (pipeline, "src");
		thread = test_emitter_start (&emitter, src);
		gst_object_unref (src);

		test_pipeline_free (pipeline);
		fail_if (g_signal_has_handler_pending (emitter.device, signal_id, 0, FALSE));

		test_emitter_stop (&emitter, thread);
	}
}
GST_END_TEST;

/* Device provider: it lists the cameras for GstDeviceMonitor */

#define TEST_PROVIDER		"aravisdeviceprovider"
/* The fake camera as the provider shows it: vendor, model and serial number */
#define TEST_DEVICE_NAME	"Aravis Fake (1)"

/* Counts the openings of the fake camera, like the ArvBuffers: a hook on the constructed () of the ArvFakeDevice
 * class counts the new devices. The first call installs the hook. */

static gint test_n_fake_device_openings = 0;
static void (*test_fake_device_parent_constructed) (GObject *object) = NULL;

static void
test_fake_device_constructed (GObject *object)
{
	if (test_fake_device_parent_constructed != NULL)
		test_fake_device_parent_constructed (object);

	g_atomic_int_inc (&test_n_fake_device_openings);
}

static int
test_count_fake_device_openings (void)
{
	GObjectClass *object_class;

	object_class = g_type_class_ref (ARV_TYPE_FAKE_DEVICE);
	if (object_class->constructed != test_fake_device_constructed) {
		/* Keeps the class reference */
		test_fake_device_parent_constructed = object_class->constructed;
		object_class->constructed = test_fake_device_constructed;
	} else
		g_type_class_unref (object_class);

	return g_atomic_int_get (&test_n_fake_device_openings);
}

/* Returns the provider (transfer full) */

static GstDeviceProvider *
test_provider_get (void)
{
	GstDeviceProvider *provider;

	provider = gst_device_provider_factory_get_by_name (TEST_PROVIDER);
	fail_unless (provider != NULL, "No device provider '%s'", TEST_PROVIDER);

	return provider;
}

/* Lists the devices of the provider, which probes them, unless a monitor started it (transfer full) */

static GList *
test_provider_probe (void)
{
	GstDeviceProvider *provider;
	GList *devices;

	provider = test_provider_get ();
	devices = gst_device_provider_get_devices (provider);
	gst_object_unref (provider);

	return devices;
}

static char *
test_device_dup_property (GstDevice *device, const char *name)
{
	GstStructure *properties;
	char *value;

	properties = gst_device_get_properties (device);
	fail_unless (properties != NULL);
	value = g_strdup (gst_structure_get_string (properties, name));
	gst_structure_free (properties);

	return value;
}

/* Returns the only device of this provider among devices (transfer none) */

static GstDevice *
test_devices_get_aravis_device (GList *devices)
{
	GstDevice *aravis_device = NULL;
	guint n_aravis_devices = 0;
	GList *iter;

	for (iter = devices; iter != NULL; iter = iter->next) {
		char *api = test_device_dup_property (iter->data, "device.api");

		if (g_strcmp0 (api, "aravis") == 0) {
			aravis_device = iter->data;
			n_aravis_devices++;
		}
		g_free (api);
	}

	fail_unless_equals_int (n_aravis_devices, 1);

	return aravis_device;
}

/* The launch line gst-device-monitor-1.0 prints for a device: the factory of the element the device creates, and
 * its read-write properties whose values differ from those of a new element of the factory, in the order the class
 * lists them */

static char *
test_device_dup_launch_line (GstDevice *device)
{
	static const char *ignored_properties[] = {"name", "parent", "direction", "template", "caps"};
	GstElementFactory *factory;
	GstElement *element;
	GstElement *new_element;
	GParamSpec **pspecs;
	GString *line;
	guint n_pspecs = 0;
	guint i, j;

	element = gst_device_create_element (device, NULL);
	fail_unless (element != NULL);
	gst_object_ref_sink (element);

	factory = gst_element_get_factory (element);
	fail_unless (factory != NULL);
	new_element = gst_element_factory_create (factory, NULL);
	fail_unless (new_element != NULL);
	gst_object_ref_sink (new_element);

	line = g_string_new (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));

	pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (element), &n_pspecs);
	for (i = 0; i < n_pspecs; i++) {
		GValue value = G_VALUE_INIT;
		GValue new_value = G_VALUE_INIT;
		gboolean ignored = FALSE;

		if ((pspecs[i]->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE)
			continue;
		for (j = 0; j < G_N_ELEMENTS (ignored_properties); j++)
			ignored = ignored || g_strcmp0 (pspecs[i]->name, ignored_properties[j]) == 0;
		if (ignored)
			continue;

		g_value_init (&value, pspecs[i]->value_type);
		g_value_init (&new_value, pspecs[i]->value_type);
		g_object_get_property (G_OBJECT (element), pspecs[i]->name, &value);
		g_object_get_property (G_OBJECT (new_element), pspecs[i]->name, &new_value);
		if (gst_value_compare (&value, &new_value) != GST_VALUE_EQUAL) {
			char *text = gst_value_serialize (&value);

			g_string_append_printf (line, " %s=%s", pspecs[i]->name, text != NULL ? text : "(unserializable)");
			g_free (text);
		}
		g_value_unset (&value);
		g_value_unset (&new_value);
	}
	g_free (pspecs);

	gst_object_unref (element);
	gst_object_unref (new_element);

	return g_string_free (line, FALSE);
}

/* A device monitor started by test_monitor_start (), and the providers it does not see. teardown () stops it after a
 * failed check: the provider is a singleton, and would stay started. */

typedef struct {
	GstPluginFeature *feature;
	guint rank;
} TestDemotedFeature;

static GstDeviceMonitor *test_monitor = NULL;
static GList *test_demoted_features = NULL;

/* Starts a device monitor for the classes, which only sees this provider: the other providers of the registry, which
 * would list the webcams of the host for example, are none of these tests' business, and are set to rank none until
 * test_monitor_stop () */

static GstDeviceMonitor *
test_monitor_start (const char *classes)
{
	GList *factories;
	GList *iter;

	fail_unless (test_monitor == NULL);

	factories = gst_device_provider_factory_list_get_device_providers (GST_RANK_NONE + 1);
	for (iter = factories; iter != NULL; iter = iter->next) {
		GstPluginFeature *feature = iter->data;
		TestDemotedFeature *demoted;

		if (g_strcmp0 (gst_plugin_feature_get_name (feature), TEST_PROVIDER) == 0)
			continue;

		demoted = g_new (TestDemotedFeature, 1);
		demoted->feature = gst_object_ref (feature);
		demoted->rank = gst_plugin_feature_get_rank (feature);
		test_demoted_features = g_list_prepend (test_demoted_features, demoted);
		gst_plugin_feature_set_rank (feature, GST_RANK_NONE);
	}
	gst_plugin_feature_list_free (factories);

	test_monitor = gst_device_monitor_new ();
	/* 0 if no provider has the classes */
	fail_unless (gst_device_monitor_add_filter (test_monitor, classes, NULL) != 0, "No provider for %s", classes);
	fail_unless (gst_device_monitor_start (test_monitor));

	return test_monitor;
}

static void
test_monitor_stop (void)
{
	if (test_monitor != NULL) {
		gst_device_monitor_stop (test_monitor);
		g_clear_object (&test_monitor);
	}

	while (test_demoted_features != NULL) {
		TestDemotedFeature *demoted = test_demoted_features->data;

		test_demoted_features = g_list_delete_link (test_demoted_features, test_demoted_features);
		gst_plugin_feature_set_rank (demoted->feature, demoted->rank);
		gst_object_unref (demoted->feature);
		g_free (demoted);
	}
}

/* Plays a pipeline, and checks that its first frames come from the fake camera */

static void
test_pipeline_check_frames (GstElement *pipeline, unsigned int n_frames)
{
	unsigned int i;

	for (i = 0; i < n_frames; i++) {
		GstSample *sample;
		TestFrame *frame;

		if (i == 0)
			sample = test_pipeline_play (pipeline);
		else
			sample = test_pipeline_pull_sample (pipeline, TEST_TIMEOUT);
		fail_unless (sample != NULL, "No frame %u: %s", i, test_pipeline_dup_error_text (pipeline));

		frame = test_frame_new_from_sample (sample);
		gst_sample_unref (sample);
		test_frame_check (frame);
		test_frame_free (frame);
	}
}

GST_START_TEST (test_device_provider_factory)
{
	GstDeviceProviderFactory *factory;

	factory = gst_device_provider_factory_find (TEST_PROVIDER);
	fail_unless (factory != NULL, "No device provider '%s'", TEST_PROVIDER);

	/* In the plugin of aravissrc */
	fail_unless_equals_string (gst_plugin_feature_get_plugin_name (GST_PLUGIN_FEATURE (factory)), "aravis");
	/* GstDeviceMonitor only uses the providers of rank 1 or above */
	fail_unless_equals_int (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), GST_RANK_MARGINAL);
	/* and of the classes it was asked for */
	fail_unless (gst_device_provider_factory_has_classes (factory, "Video/Source"));
	fail_if (gst_device_provider_factory_has_classes (factory, "Audio/Source"));

	gst_object_unref (factory);
}
GST_END_TEST;

GST_START_TEST (test_device_provider_probe)
{
	static const struct {
		const char *name;
		const char *value;
	} expected_properties[] = {
		{"device.api",		"aravis"},
		{"device.vendor.name",	"Aravis"},
		{"device.product.name",	"Fake"},
		{"device.serial",	"1"},
		{"aravis.device-id",	TEST_CAMERA},
		{"aravis.physical-id",	TEST_CAMERA},
		{"aravis.address",	"0.0.0.0"},
		{"aravis.protocol",	"Fake"},
		{"aravis.interface",	"Fake"}
	};
	GstDevice *device;
	GstCaps *caps;
	GstCaps *expected_caps;
	GList *devices;
	char *text;
	int n_openings;
	unsigned int i;

	n_openings = test_count_fake_device_openings ();

	/* setup () enables the Fake interface only: one camera */
	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);
	device = devices->data;

	text = gst_device_get_display_name (device);
	fail_unless_equals_string (text, TEST_DEVICE_NAME);
	g_free (text);

	text = gst_device_get_device_class (device);
	fail_unless_equals_string (text, "Video/Source");
	g_free (text);

	/* What aravissrc may produce, as the pixel formats of the camera are unknown until it is open */
	caps = gst_device_get_caps (device);
	expected_caps = gst_caps_from_string ("video/x-raw; video/x-bayer");
	fail_unless (gst_caps_is_equal (caps, expected_caps), "Unexpected caps %" GST_PTR_FORMAT, caps);
	gst_caps_unref (caps);
	gst_caps_unref (expected_caps);

	for (i = 0; i < G_N_ELEMENTS (expected_properties); i++) {
		text = test_device_dup_property (device, expected_properties[i].name);
		fail_unless (text != NULL, "No %s property", expected_properties[i].name);
		fail_unless_equals_string (text, expected_properties[i].value);
		g_free (text);
	}

	/* Without opening the camera */
	fail_unless_equals_int (test_count_fake_device_openings (), n_openings);

	g_list_free_full (devices, gst_object_unref);
}
GST_END_TEST;

GST_START_TEST (test_device_provider_enabled_interfaces)
{
	GList *devices;

	/* The provider lists the cameras of the interfaces enabled in the process, here none, */
	arv_disable_interface ("Fake");
	devices = test_provider_probe ();
	fail_unless (devices == NULL, "%u devices without enabled interface", g_list_length (devices));

	/* and each probe updates the list */
	arv_enable_interface ("Fake");
	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);
	g_list_free_full (devices, gst_object_unref);
}
GST_END_TEST;

GST_START_TEST (test_device_provider_create_element)
{
	GstElement *pipeline;
	GstElement *src;
	GList *devices;
	ArvCamera *camera = NULL;
	char *camera_name = NULL;
	char *interface_id = NULL;
	int n_openings;

	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);

	n_openings = test_count_fake_device_openings ();

	src = gst_device_create_element (devices->data, "src");
	fail_unless (src != NULL);
	g_list_free_full (devices, gst_object_unref);
	fail_unless_equals_string (GST_OBJECT_NAME (gst_element_get_factory (src)), "aravissrc");
	fail_unless_equals_string (GST_OBJECT_NAME (src), "src");

	g_object_get (src, "camera-name", &camera_name, "interface", &interface_id, "camera", &camera, NULL);
	fail_unless_equals_string (camera_name, TEST_CAMERA);
	fail_unless_equals_string (interface_id, "Fake");
	g_free (camera_name);
	g_free (interface_id);

	/* The camera opens when the element starts */
	fail_unless (camera == NULL);
	fail_unless_equals_int (test_count_fake_device_openings (), n_openings);

	/* through the interface of the device, which needs no longer be enabled for device discovery */
	arv_disable_interface ("Fake");

	pipeline = test_pipeline_new_from_element (src);
	test_pipeline_check_frames (pipeline, 4);

	camera = test_element_dup_camera (src);
	fail_unless (ARV_IS_FAKE_DEVICE (arv_camera_get_device (camera)));
	g_object_unref (camera);
	fail_unless_equals_int (test_count_fake_device_openings (), n_openings + 1);

	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_device_provider_launch_line)
{
	GList *devices;
	char *line;

	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);

	/* Only the properties of the device: the element of the device does not open the camera, whose settings would
	 * otherwise show, like h-binning. And interface before camera-name, which then opens the camera through that
	 * interface only. */
	line = test_device_dup_launch_line (devices->data);
	fail_unless_equals_string (line, "aravissrc interface=Fake camera-name=" TEST_CAMERA);

	g_free (line);
	g_list_free_full (devices, gst_object_unref);
}
GST_END_TEST;

GST_START_TEST (test_device_provider_monitor)
{
	GstDeviceMonitor *monitor;
	GstElement *pipeline;
	GstElement *src;
	GstDevice *device;
	GList *devices;
	char *name;

	monitor = test_monitor_start ("Video/Source");

	devices = gst_device_monitor_get_devices (monitor);
	device = test_devices_get_aravis_device (devices);

	name = gst_device_get_display_name (device);
	fail_unless_equals_string (name, TEST_DEVICE_NAME);
	g_free (name);

	/* The listed device plays */
	src = gst_device_create_element (device, "src");
	fail_unless (src != NULL);
	g_list_free_full (devices, gst_object_unref);
	pipeline = test_pipeline_new_from_element (src);
	test_pipeline_check_frames (pipeline, 2);
	test_pipeline_free (pipeline);

	test_monitor_stop ();
}
GST_END_TEST;

/* Probes on another thread, without checks, which must stay on the thread of the test */

typedef struct {
	GstDeviceProvider *provider;
	gint stop;
	gint n_probes;
	gint n_bad_probes;
} TestProber;

static gpointer
test_prober_run (gpointer data)
{
	TestProber *prober = data;

	while (!g_atomic_int_get (&prober->stop)) {
		GList *devices = gst_device_provider_get_devices (prober->provider);
		gboolean is_good = FALSE;

		if (g_list_length (devices) == 1) {
			GstStructure *properties = gst_device_get_properties (devices->data);

			is_good = properties != NULL &&
				g_strcmp0 (gst_structure_get_string (properties, "aravis.device-id"), TEST_CAMERA) == 0 &&
				g_strcmp0 (gst_structure_get_string (properties, "aravis.interface"), "Fake") == 0;
			if (properties != NULL)
				gst_structure_free (properties);
		}

		if (!is_good)
			g_atomic_int_inc (&prober->n_bad_probes);
		g_atomic_int_inc (&prober->n_probes);

		g_list_free_full (devices, gst_object_unref);
	}

	return NULL;
}

#define TEST_CONCURRENT_N_OPENINGS	50

GST_START_TEST (test_device_provider_concurrent_open)
{
	TestProber prober = {NULL, FALSE, 0, 0};
	GstElement *src;
	GThread *thread;
	unsigned int i;

	/* The probes update and read the device lists of the interfaces, which aravissrc updates too when it opens the
	 * first camera of an interface. Without camera-name, setting camera-name again does it. */
	src = gst_element_factory_make ("aravissrc", NULL);
	fail_unless (src != NULL);
	gst_object_ref_sink (src);
	g_object_set (src, "interface", "Fake", NULL);

	prober.provider = test_provider_get ();
	thread = g_thread_new ("prober", test_prober_run, &prober);

	for (i = 0; i < TEST_CONCURRENT_N_OPENINGS; i++) {
		ArvCamera *camera = NULL;

		g_object_set (src, "camera-name", NULL, NULL);
		g_object_get (src, "camera", &camera, NULL);
		if (camera == NULL)
			break;
		g_object_unref (camera);
	}

	g_atomic_int_set (&prober.stop, TRUE);
	g_thread_join (thread);
	gst_object_unref (prober.provider);
	gst_object_unref (src);

	GST_INFO ("%d probes during %u openings", prober.n_probes, i);

	fail_unless_equals_int (i, TEST_CONCURRENT_N_OPENINGS);
	fail_unless (prober.n_probes > 0);
	fail_unless (prober.n_bad_probes == 0, "%d bad probes out of %d", prober.n_bad_probes, prober.n_probes);
}
GST_END_TEST;

#define TEST_LISTING_N_PROBES	200

GST_START_TEST (test_device_provider_concurrent_listing)
{
	GstDeviceProvider *provider;
	unsigned int i;

	/* The probes update the device list of Aravis, and read it, while the application does the same on another
	 * thread */
	provider = test_provider_get ();
	test_lister_start ();

	for (i = 0; i < TEST_LISTING_N_PROBES || test_lister_get_n_listings () < TEST_LISTING_N_PROBES; i++) {
		GList *devices = gst_device_provider_get_devices (provider);
		char *device_id;
		char *interface_id;

		fail_unless_equals_int (g_list_length (devices), 1);
		device_id = test_device_dup_property (devices->data, "aravis.device-id");
		interface_id = test_device_dup_property (devices->data, "aravis.interface");
		fail_unless_equals_string (device_id, TEST_CAMERA);
		fail_unless_equals_string (interface_id, "Fake");

		g_free (device_id);
		g_free (interface_id);
		g_list_free_full (devices, gst_object_unref);
	}

	test_lister_stop ();
	gst_object_unref (provider);
}
GST_END_TEST;

#define TEST_PROVIDER_ENV	"GST_ARAVIS_DEVICE_PROVIDER"

GST_START_TEST (test_device_provider_disabled)
{
	static const char *disabling_values[] = {"0", "off", "false", "no", "OFF", "False"};
	GstDeviceMonitor *monitor;
	GList *devices;
	unsigned int i;

	/* For every application using GstDeviceMonitor, without changing it: no probe, no camera */
	for (i = 0; i < G_N_ELEMENTS (disabling_values); i++) {
		g_setenv (TEST_PROVIDER_ENV, disabling_values[i], TRUE);
		devices = test_provider_probe ();
		fail_unless (devices == NULL, "%u devices with %s=%s", g_list_length (devices), TEST_PROVIDER_ENV,
			     disabling_values[i]);
	}

	monitor = test_monitor_start ("Video/Source");
	devices = gst_device_monitor_get_devices (monitor);
	fail_unless (devices == NULL, "The monitor lists %u devices", g_list_length (devices));
	test_monitor_stop ();

	/* Other values keep the provider */
	g_setenv (TEST_PROVIDER_ENV, "1", TRUE);
	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);
	g_list_free_full (devices, gst_object_unref);

	g_unsetenv (TEST_PROVIDER_ENV);
	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);
	g_list_free_full (devices, gst_object_unref);
}
GST_END_TEST;

static void
teardown_device_provider (void)
{
	teardown_leftovers ();
	test_monitor_stop ();
	g_unsetenv (TEST_PROVIDER_ENV);
	test_aravis_log_check ();
}

/* GenTL: the mock GenTL producer of tests/gentlmock, whose path meson gives in ARV_TEST_GENTL_MOCK, through
 * interface=GenTL. Its camera is an ArvFakeCamera. The producer is loaded at the first use of the GenTL interface,
 * reading the options of the mock from the environment, and released by teardown_gentl () with arv_shutdown (). */

#define TEST_GENTL_DEVICE_ID	ARV_GENTL_MOCK_TL_VENDOR "-" ARV_GENTL_MOCK_VENDOR "-" ARV_GENTL_MOCK_MODEL "-" \
				ARV_GENTL_MOCK_SERIAL "0"

static const char *test_gentl_variables[] = {
	"GENICAM_GENTL64_PATH",
	"GENICAM_GENTL32_PATH",
	"ARV_GENTL_PRODUCERS",
	"ARV_GENTL_MOCK_FRAME_PERIOD_US",
	"ARV_GENTL_MOCK_N_DEVICES"
};

static void
setup_gentl (void)
{
	const char *mock_path = g_getenv ("ARV_TEST_GENTL_MOCK");
	unsigned int i;

	setup ();

	for (i = 0; i < G_N_ELEMENTS (test_gentl_variables); i++)
		g_unsetenv (test_gentl_variables[i]);

	if (mock_path != NULL)
		g_setenv (sizeof (void *) == 8 ? "GENICAM_GENTL64_PATH" : "GENICAM_GENTL32_PATH", mock_path, TRUE);
	g_setenv ("ARV_GENTL_MOCK_FRAME_PERIOD_US", "10000", TRUE);
}

static void
teardown_gentl (void)
{
	unsigned int i;

	teardown_leftovers ();

	/* Releases the producer */
	arv_shutdown ();

	for (i = 0; i < G_N_ELEMENTS (test_gentl_variables); i++)
		g_unsetenv (test_gentl_variables[i]);

	test_aravis_log_check ();
}

/* Run by meson only, which builds the mock producer */

#define TEST_GENTL_SKIP_WITHOUT_MOCK() \
	G_STMT_START { \
		if (g_getenv ("ARV_TEST_GENTL_MOCK") == NULL) { \
			g_print ("Skipped: ARV_TEST_GENTL_MOCK is not set, the test must be run by meson\n"); \
			return; \
		} \
	} G_STMT_END

GST_START_TEST (test_gentl_renegotiation)
{
	GstElement *pipeline;
	GstElement *filter;
	GstElement *sink;
	GstCaps *caps;
	TestCollector *collector;
	gboolean keeps_last_sample = FALSE;

	TEST_GENTL_SKIP_WITHOUT_MOCK ();

	/* GStreamer's sinks keep the last frame by default: while the caps change, a frame of the old stream is held
	 * downstream. Lent, it would keep the GenTL data stream open, and the producer would refuse to open the next
	 * one. */
	pipeline = test_pipeline_new ("aravissrc name=src interface=GenTL num-arv-buffers=10"
				      " ! capsfilter name=filter caps=\"%s\" ! fakesink name=sink sync=false signal-handoffs=true",
				      TEST_CAPS);
	sink = test_pipeline_get_element (pipeline, "sink");
	g_object_get (sink, "enable-last-sample", &keeps_last_sample, NULL);
	fail_unless (keeps_last_sample);
	gst_object_unref (sink);
	collector = test_collector_new (pipeline, 0);

	test_pipeline_start (pipeline);
	test_collector_wait (collector, pipeline, 2);

	filter = test_pipeline_get_element (pipeline, "filter");
	caps = gst_caps_from_string ("video/x-raw,format=GRAY8,width=48,height=64");
	test_collector_set_count_width (collector, 48);
	g_object_set (filter, "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (filter);

	/* Frames of the new caps flow */
	test_collector_wait (collector, pipeline, 3);

	test_pipeline_free (pipeline);
	test_collector_free (collector);
}
GST_END_TEST;

GST_START_TEST (test_gentl_restart_holding_frame)
{
	GstElement *pipeline;
	GstSample *sample;
	TestFrame *frame;
	int n_arv_buffers;

	TEST_GENTL_SKIP_WITHOUT_MOCK ();

	n_arv_buffers = test_count_arv_buffers ();

	pipeline = test_pipeline_new ("aravissrc name=src interface=GenTL ! %s ! %s", TEST_CAPS, TEST_SINK);
	sample = test_pipeline_play (pipeline);
	frame = test_frame_new_from_sample (sample);
	gst_sample_unref (sample);
	test_frame_check (frame);

	/* The frames of a GenTL stream are copies: no buffer of the stream outlives it, nor keeps the data stream and
	 * the device open */
	fail_unless_equals_int (test_pipeline_set_state (pipeline, GST_STATE_NULL, TEST_TIMEOUT),
				GST_STATE_CHANGE_SUCCESS);
	fail_unless_equals_int (test_count_arv_buffers (), n_arv_buffers);
	test_frame_check (frame);

	/* So the camera opens again while the application holds the frame */
	gst_sample_unref (test_pipeline_play (pipeline));
	test_frame_check (frame);

	test_frame_free (frame);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_gentl_renegotiation_followed_stream)
{
	TestFollower *follower;
	GstElement *pipeline;
	GstElement *filter;
	GstElement *src;
	GstCaps *caps;
	TestCollector *collector;

	TEST_GENTL_SKIP_WITHOUT_MOCK ();

	/* An application following the stream property releases the stream of the previous caps when the element
	 * releases it, before the element creates the next one: the producer refuses to open a data stream of the
	 * device while another one is open */
	pipeline = test_pipeline_new ("aravissrc name=src interface=GenTL num-arv-buffers=10"
				      " ! capsfilter name=filter caps=\"%s\" ! fakesink name=sink sync=false signal-handoffs=true",
				      TEST_CAPS);
	src = test_pipeline_get_element (pipeline, "src");
	follower = test_follower_new (src, "stream");
	collector = test_collector_new (pipeline, 0);

	test_pipeline_start (pipeline);
	test_collector_wait (collector, pipeline, 2);

	filter = test_pipeline_get_element (pipeline, "filter");
	caps = gst_caps_from_string ("video/x-raw,format=GRAY8,width=48,height=64");
	test_collector_set_count_width (collector, 48);
	g_object_set (filter, "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (filter);

	/* Frames of the new caps flow */
	test_collector_wait (collector, pipeline, 3);

	test_pipeline_free (pipeline);
	test_collector_free (collector);

	/* The stream of each caps, released when the element stopped */
	fail_unless_equals_int (test_follower_get_n_objects (follower), 2);
	fail_if (test_follower_holds_object (follower));
	test_follower_free (follower, src);
	gst_object_unref (src);
}
GST_END_TEST;

GST_START_TEST (test_gentl_restart_followed_objects)
{
	static const char *property_names[] = {"stream", "camera"};
	unsigned int i;

	TEST_GENTL_SKIP_WITHOUT_MOCK ();

	/* The stream and the camera keep the device open: an application following either property releases them when
	 * the element stops, so that the camera opens again at the next start, while the producer refuses to open a
	 * device that is open */
	for (i = 0; i < G_N_ELEMENTS (property_names); i++) {
		TestFollower *follower;
		GstElement *pipeline;
		GstElement *src;

		pipeline = test_pipeline_new ("aravissrc name=src interface=GenTL ! %s ! %s", TEST_CAPS, TEST_SINK);
		src = test_pipeline_get_element (pipeline, "src");
		follower = test_follower_new (src, property_names[i]);

		gst_sample_unref (test_pipeline_play (pipeline));
		fail_unless (test_follower_holds_object (follower), "No %s to follow", property_names[i]);
		fail_unless_equals_int (test_pipeline_set_state (pipeline, GST_STATE_NULL, TEST_TIMEOUT),
					GST_STATE_CHANGE_SUCCESS);

		GST_INFO ("Restart, following the %s", property_names[i]);
		gst_sample_unref (test_pipeline_play (pipeline));

		test_pipeline_free (pipeline);

		/* One per start, released when the element stopped */
		fail_unless_equals_int (test_follower_get_n_objects (follower), 2);
		fail_if (test_follower_holds_object (follower));
		test_follower_free (follower, src);
		gst_object_unref (src);
	}
}
GST_END_TEST;

GST_START_TEST (test_gentl_error_camera_names)
{
	GstElement *pipeline;
	GstStateChangeReturn result;

	TEST_GENTL_SKIP_WITHOUT_MOCK ();

	/* Without camera-name, the error names the interface the element looked on */
	g_setenv ("ARV_GENTL_MOCK_N_DEVICES", "0", TRUE);
	pipeline = test_pipeline_new ("aravissrc name=src interface=GenTL ! %s ! %s", TEST_CAPS, TEST_SINK);
	result = test_pipeline_set_state (pipeline, GST_STATE_PLAYING, TEST_TIMEOUT);
	fail_unless_equals_int (result, GST_STATE_CHANGE_FAILURE);
	test_pipeline_expect_error (pipeline, "src", GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND,
				    "Could not find a camera on interface GenTL: ");
	test_pipeline_free (pipeline);
}
GST_END_TEST;

GST_START_TEST (test_gentl_device_provider)
{
	GstDevice *device = NULL;
	GstElement *pipeline;
	GstElement *src;
	GList *devices;
	GList *iter;
	char *text;

	TEST_GENTL_SKIP_WITHOUT_MOCK ();

	/* GenTL is disabled by default: the provider lists its cameras once it is enabled */
	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 1);
	g_list_free_full (devices, gst_object_unref);

	arv_enable_interface ("GenTL");
	devices = test_provider_probe ();
	fail_unless_equals_int (g_list_length (devices), 2);
	for (iter = devices; iter != NULL; iter = iter->next) {
		text = test_device_dup_property (iter->data, "aravis.interface");
		if (g_strcmp0 (text, "GenTL") == 0)
			device = iter->data;
		g_free (text);
	}
	fail_unless (device != NULL, "No GenTL device");

	text = test_device_dup_property (device, "aravis.device-id");
	fail_unless_equals_string (text, TEST_GENTL_DEVICE_ID);
	g_free (text);

	/* Its camera-name is a GenTL device identifier, which only the GenTL interface knows: interface comes first in
	 * the launch line gst-device-monitor-1.0 prints, */
	text = test_device_dup_launch_line (device);
	fail_unless_equals_string (text, "aravissrc interface=GenTL camera-name=" TEST_GENTL_DEVICE_ID);
	g_free (text);

	/* and the device plays, even with GenTL disabled again */
	src = gst_device_create_element (device, "src");
	fail_unless (src != NULL);
	g_list_free_full (devices, gst_object_unref);
	arv_disable_interface ("GenTL");

	pipeline = test_pipeline_new_from_element (src);
	test_pipeline_check_frames (pipeline, 2);
	test_pipeline_free (pipeline);
}
GST_END_TEST;

static Suite *
gst_aravis_suite (void)
{
	Suite *suite = suite_create ("aravissrc");
	TCase *tcase;

	tcase = tcase_create ("harness");
	tcase_add_checked_fixture (tcase, setup, teardown);
	tcase_add_test (tcase, test_harness_plugin);
	tcase_add_test (tcase, test_harness_stream);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("latency");
	tcase_add_checked_fixture (tcase, setup, teardown);
	tcase_add_test (tcase, test_latency_frame_period);
	tcase_add_test (tcase, test_latency_trigger_mode);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("usb");
	tcase_add_checked_fixture (tcase, setup, teardown);
	tcase_add_test (tcase, test_usb_maximum_transfer_size_property);
	tcase_add_test (tcase, test_usb_settings_non_usb_camera);
	tcase_add_test (tcase, test_usb_maximum_transfer_size_invalid);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("interface");
	tcase_add_checked_fixture (tcase, setup, teardown);
	tcase_add_test (tcase, test_interface_property);
	tcase_add_test (tcase, test_interface_open);
	tcase_add_test (tcase, test_interface_unknown);
	tcase_add_test (tcase, test_interface_device_not_found);
	tcase_add_test (tcase, test_interface_order_in_bin);
	tcase_add_test (tcase, test_interface_open_error_at_start);
	tcase_add_test (tcase, test_interface_error_camera_names);
	tcase_add_test (tcase, test_interface_concurrent_listing);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("ownership");
	tcase_add_checked_fixture (tcase, setup, teardown);
	tcase_add_test (tcase, test_ownership_hold_after_stop);
	tcase_add_test (tcase, test_ownership_caps_change);
	tcase_add_test (tcase, test_ownership_hold_many_frames);
	tcase_add_test (tcase, test_ownership_queue_threshold);
	tcase_add_test (tcase, test_ownership_start_stop_cycles);
	tcase_add_test (tcase, test_ownership_lending_limit);
	tcase_add_test (tcase, test_ownership_renegotiation_stream);
	tcase_add_test (tcase, test_ownership_renegotiation_held_stream);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("unlock");
	tcase_add_checked_fixture (tcase, setup, teardown);
	tcase_add_test (tcase, test_unlock_buffer_timeout_property);
	tcase_add_test (tcase, test_unlock_stop_while_waiting);
	tcase_add_test (tcase, test_unlock_pause_resume);
	tcase_add_test (tcase, test_unlock_access_while_waiting);
	tcase_add_test (tcase, test_unlock_control_lost);
	tcase_add_test (tcase, test_unlock_timeout);
	tcase_add_test (tcase, test_unlock_wait_forever);
	tcase_add_test (tcase, test_unlock_failed_frames);
	tcase_add_test (tcase, test_unlock_failed_frames_control_lost);
	tcase_add_test (tcase, test_unlock_failed_frame_restarts_wait);
	tcase_add_test (tcase, test_unlock_control_lost_dispose);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("device-provider");
	tcase_add_checked_fixture (tcase, setup, teardown_device_provider);
	tcase_add_test (tcase, test_device_provider_factory);
	tcase_add_test (tcase, test_device_provider_probe);
	tcase_add_test (tcase, test_device_provider_enabled_interfaces);
	tcase_add_test (tcase, test_device_provider_create_element);
	tcase_add_test (tcase, test_device_provider_launch_line);
	tcase_add_test (tcase, test_device_provider_monitor);
	tcase_add_test (tcase, test_device_provider_concurrent_open);
	tcase_add_test (tcase, test_device_provider_concurrent_listing);
	tcase_add_test (tcase, test_device_provider_disabled);
	suite_add_tcase (suite, tcase);

	tcase = tcase_create ("gentl");
	tcase_add_checked_fixture (tcase, setup_gentl, teardown_gentl);
	tcase_add_test (tcase, test_gentl_renegotiation);
	tcase_add_test (tcase, test_gentl_restart_holding_frame);
	tcase_add_test (tcase, test_gentl_renegotiation_followed_stream);
	tcase_add_test (tcase, test_gentl_restart_followed_objects);
	tcase_add_test (tcase, test_gentl_error_camera_names);
	tcase_add_test (tcase, test_gentl_device_provider);
	suite_add_tcase (suite, tcase);

	/* With the fake GigEVision camera on 127.0.0.1: only in the test of the 'network' suite */
	if (g_strcmp0 (g_getenv ("GST_ARAVIS_TEST_NETWORK"), "1") == 0) {
		tcase = tcase_create ("network");
		tcase_add_checked_fixture (tcase, setup_network, teardown_network);
		tcase_add_test (tcase, test_network_gv_renegotiation);
		tcase_add_test (tcase, test_network_gv_renegotiation_followed_stream);
		tcase_add_test (tcase, test_network_gv_renegotiation_held_stream);
		suite_add_tcase (suite, tcase);
	}

	return suite;
}

int
main (int argc, char **argv)
{
	/* GStreamer's own elements the tests use, from the coreelements and app plugins */
	static const char *required_elements[] = {"capsfilter", "appsink", "fakesink", "queue"};
	unsigned int i;
	int n_failures;

	gst_check_init (&argc, &argv);

	g_log_set_handler ("Aravis", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, test_aravis_log_handler, NULL);

	for (i = 0; i < G_N_ELEMENTS (required_elements); i++) {
		GstElementFactory *factory = gst_element_factory_find (required_elements[i]);

		if (factory == NULL) {
			g_print ("Skipped: GStreamer element '%s' not found\n", required_elements[i]);
			return 77;
		}
		gst_object_unref (factory);
	}

	arv_set_fake_camera_genicam_filename (GENICAM_FILENAME);

	n_failures = gst_check_run_suite (gst_aravis_suite (), "gstaravis", __FILE__);

	arv_shutdown ();

	return n_failures != 0 ? 1 : 0;
}
