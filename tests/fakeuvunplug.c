/* SPDX-License-Identifier:Unlicense */

/* A USB3 Vision camera unplugged while streaming, against the fake libusb. Without hotplug, as with
 * a default libusb build on Windows, for each way the fake can report the removal (transfers in flight failing with
 * LIBUSB_TRANSFER_NO_DEVICE, _ERROR, _STALL or _CANCELLED, or never completing; submissions failing with
 * LIBUSB_ERROR_IO or _NO_DEVICE; synchronous transfers failing), in both USB modes: control-lost within about a
 * second, every buffer comes back exactly once, the stream thread exits, nothing spins. The same with hotplug, with
 * the Windows and Linux timings, and when libusb has hotplug support but the device could not register its callback.
 * A transfer failure without unplug is not taken for a loss, and streaming goes on, with frames of whole packets too.
 * Neither is a submission refused for lack of usbfs memory on Linux. The camera, plugged back, streams again in the
 * same process.
 *
 * A removal that the Windows backend of libusb reports as LIBUSB_ERROR_IO, or as a transfer that fails otherwise than
 * with LIBUSB_TRANSFER_NO_DEVICE, is a loss of control with the Windows builds of Aravis only (FAKEUV_IO_ERROR_IS_LOSS):
 * elsewhere, libusb reports a removal as LIBUSB_ERROR_NO_DEVICE, and these cases are skipped.
 *
 * This program runs the cases in async USB mode, without RAW_IO, the default. tests/fakeuvunplugsync.c builds this
 * file again for sync mode, and tests/fakeuvunplugrawio.c for async mode with WinUSB's RAW_IO, as programs that run in
 * parallel. */

#include "fakeuvtest.h"
#include <arvuvcpprivate.h>

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
#define RAW_IO_AVAILABLE	TRUE
#else
#define RAW_IO_AVAILABLE	FALSE
#endif

#if defined (FAKEUV_UNPLUG_SYNC)
#define USB_MODE		ARV_UV_USB_MODE_SYNC
#define WITH_RAW_IO		FALSE
#define TEST_PATH(name)		"/fakeuvunplugsync/" name
#elif defined (FAKEUV_UNPLUG_RAW_IO)
#define USB_MODE		ARV_UV_USB_MODE_ASYNC
#define WITH_RAW_IO		TRUE
#define TEST_PATH(name)		"/fakeuvunplugrawio/" name
#else
#define USB_MODE		ARV_UV_USB_MODE_ASYNC
#define WITH_RAW_IO		FALSE
#define TEST_PATH(name)		"/fakeuvunplug/" name
#endif

#define N_BUFFERS		5
#define N_FRAMES		3
#define TIMEOUT_MS		FAKEUV_TIMEOUT_MS
/* "control-lost fires within about a second". When transfers never complete, a check after one second reports it (no
 * transfer back for that long, or a submission failing for that long): LOSS_TIMEOUT_MS gives a loaded machine half a
 * second more, and stays below the two seconds that a check twice as slow would take. Otherwise much faster:
 * FAST_LOSS_TIMEOUT_MS, before any one second delay could have passed. */
#define LOSS_TIMEOUT_MS		1500
#define FAST_LOSS_TIMEOUT_MS	700
/* libusb calls of the stream thread from the unplug until it exits, at most: a few, plus retries of a failed
 * submission every 10 ms; a thread spinning makes thousands */
#define MAX_CALLS_AFTER_UNPLUG(elapsed_ms)	(40 + 2 * (elapsed_ms) / 10)

typedef enum {
	UNPLUG_WHILE_STREAMING,	/* transfers in flight */
	UNPLUG_WHILE_WAITING,	/* triggered camera, waiting: every buffer in flight, no frame coming */
	UNPLUG_WHILE_HELD	/* the application holds every buffer, then pushes them back once the device is gone */
} UnplugMoment;

typedef struct {
	FakeUsbPlatform platform;
	gboolean hotplug;
	enum libusb_transfer_status in_flight_status;
	int submit_error;
	int synchronous_error;
	gboolean hang;
	UnplugMoment moment;
	int hotplug_register_error;	/* libusb_hotplug_register_callback () fails, the capability being there */
} UnplugScenario;

/* Buffer bookkeeping: each buffer pushed must come back once, through the output queue, or stay in the input
 * queue */

typedef struct {
	GHashTable *with_stream;
	guint n_success;
	guint n_aborted;
	guint n_other;
} Ledger;

static void
_ledger_init (Ledger *ledger)
{
	ledger->with_stream = g_hash_table_new (g_direct_hash, g_direct_equal);
	ledger->n_success = 0;
	ledger->n_aborted = 0;
	ledger->n_other = 0;
}

static void
_ledger_push (Ledger *ledger, ArvStream *stream, ArvBuffer *buffer)
{
	g_assert_false (g_hash_table_contains (ledger->with_stream, buffer));
	g_hash_table_add (ledger->with_stream, buffer);
	arv_stream_push_buffer (stream, buffer);
}

static void
_ledger_record (Ledger *ledger, ArvBuffer *buffer)
{
	/* Pushed to the output queue once per push to the input queue */
	g_assert_true (g_hash_table_remove (ledger->with_stream, buffer));

	switch (arv_buffer_get_status (buffer)) {
		case ARV_BUFFER_STATUS_SUCCESS:
			ledger->n_success++;
			break;
		case ARV_BUFFER_STATUS_ABORTED:
			ledger->n_aborted++;
			break;
		default:
			ledger->n_other++;
			break;
	}
}

static ArvBuffer *
_ledger_pop (Ledger *ledger, ArvStream *stream, guint timeout_ms)
{
	ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) timeout_ms * 1000);

	if (buffer != NULL)
		_ledger_record (ledger, buffer);

	return buffer;
}

/* Once the stream thread is gone: nothing left inside it */

static void
_ledger_check (Ledger *ledger, ArvStream *stream, GPtrArray *buffers)
{
	ArvBuffer *buffer;
	gint n_input = -1, n_output = -1, n_filling = -1;

	while ((buffer = arv_stream_try_pop_buffer (stream)) != NULL) {
		_ledger_record (ledger, buffer);
		g_ptr_array_add (buffers, buffer);
	}

	arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, &n_filling);
	g_assert_cmpint (n_output, ==, 0);
	g_assert_cmpint (n_filling, ==, 0);
	g_assert_cmpint (n_input, ==, g_hash_table_size (ledger->with_stream));
}

static void
_ledger_clear (Ledger *ledger)
{
	g_clear_pointer (&ledger->with_stream, g_hash_table_unref);
}

typedef struct {
	gint n_control_lost;
	gint64 control_lost_time;
} LossState;

static void
_control_lost_cb (ArvDevice *device, LossState *state)
{
	if (g_atomic_int_get (&state->n_control_lost) == 0)
		state->control_lost_time = g_get_monotonic_time ();
	g_atomic_int_inc (&state->n_control_lost);
}

static guint64
_n_stream_calls (FakeUsbDevice *device)
{
	return fake_usb_device_get_call_count (device, FAKE_USB_CALL_SUBMIT_TRANSFER) +
		fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER) +
		fake_usb_device_get_call_count (device, FAKE_USB_CALL_CONTROL_TRANSFER) +
		fake_usb_device_get_call_count (device, FAKE_USB_CALL_CANCEL_TRANSFER);
}

typedef struct {
	FakeUsbDevice *device;
	guint n_reads;
} PendingReads;

static gboolean
_reads_pending (gpointer user_data)
{
	PendingReads *pending = user_data;

	return fake_usb_device_get_n_pending_transfers (pending->device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN) ==
		pending->n_reads;
}

/* The camera plugged back is a new device, which streams in the same process */

static void
_replug_and_stream (FakeUsbDevice *device, ArvUvUsbMode usb_mode)
{
	ArvCamera *camera;
	GError *error = NULL;

	fake_usb_device_plug (device);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, usb_mode, N_BUFFERS, N_FRAMES, TIMEOUT_MS), ==,
			  N_FRAMES);
	g_clear_object (&camera);
}

static void
unplug (const UnplugScenario *scenario, ArvUvUsbMode usb_mode)
{
	FakeuvStreamEvents events = {0};
	LossState state = {0};
	Ledger ledger;
	GPtrArray *held;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	gint64 unplug_time, end_time;
	guint64 n_calls_at_unplug, n_calls_at_exit;
	size_t payload;
	guint i;

	fake_usb_set_platform (scenario->platform);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, scenario->hotplug);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
	fake_usb_device_set_unplug_errors (device, scenario->in_flight_status, scenario->submit_error,
					   scenario->synchronous_error);
	fake_usb_device_set_unplug_transfers_hang (device, scenario->hang);

	if (scenario->hotplug_register_error != 0)
		fake_usb_inject_error (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK, scenario->hotplug_register_error, 1);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	if (scenario->hotplug_register_error != 0)
		g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK), >=, 1);

	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb), &state);

	if (scenario->moment == UNPLUG_WHILE_WAITING) {
		arv_camera_set_trigger (camera, "Software", &error);
		g_assert_no_error (error);
	}

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, usb_mode);
	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);

	_ledger_init (&ledger);
	held = g_ptr_array_new_with_free_func (g_object_unref);
	for (i = 0; i < N_BUFFERS; i++)
		_ledger_push (&ledger, stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, WITH_RAW_IO);

	/* Frames go through, buffers pushed back at once */
	for (i = 0; i < N_FRAMES; i++) {
		if (scenario->moment == UNPLUG_WHILE_WAITING) {
			arv_camera_software_trigger (camera, &error);
			g_assert_no_error (error);
		}

		buffer = _ledger_pop (&ledger, stream, fakeuv_timeout_ms (TIMEOUT_MS));
		g_assert_nonnull (buffer);
		fakeuv_assert_frame (device, buffer, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
		_ledger_push (&ledger, stream, buffer);
	}

	if (scenario->moment == UNPLUG_WHILE_WAITING) {
		/* The payload reads of a buffer, in transfers of SIRM_PAYLOAD_SIZE bytes (two with RAW_IO, which reads
		 * the end of the 720x540 payload separately, one without) */
		guint32 payload_size = fake_usb_camera_get_sirm_register (device, ARV_SIRM_PAYLOAD_SIZE);
		guint n_reads = 2 + (payload + payload_size - 1) / payload_size;
		PendingReads pending = { device, usb_mode == ARV_UV_USB_MODE_ASYNC ? n_reads * N_BUFFERS : 1 };

		/* No frame coming: every buffer in flight in async mode (leader, payload and trailer reads), a read
		 * waiting in sync mode */
		g_assert_true (fakeuv_wait_for (_reads_pending, &pending, TIMEOUT_MS));
	}

	if (scenario->moment == UNPLUG_WHILE_HELD) {
		/* The application holds every buffer: nothing of this stream in flight in async mode */
		while (held->len < N_BUFFERS) {
			buffer = _ledger_pop (&ledger, stream, fakeuv_timeout_ms (TIMEOUT_MS));
			g_assert_nonnull (buffer);
			g_ptr_array_add (held, buffer);
		}
	}

	n_calls_at_unplug = _n_stream_calls (device);
	unplug_time = g_get_monotonic_time ();
	fake_usb_device_unplug (device);

	/* Buffers given back to a stream whose device is gone */
	while (held->len > 0)
		_ledger_push (&ledger, stream, g_ptr_array_steal_index (held, held->len - 1));

	/* The application keeps pushing back what it gets, as aravissrc does */
	end_time = unplug_time + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while ((g_atomic_int_get (&state.n_control_lost) == 0 || g_atomic_int_get (&events.n_exit) == 0) &&
	       g_get_monotonic_time () < end_time) {
		buffer = _ledger_pop (&ledger, stream, 10);
		if (buffer != NULL)
			_ledger_push (&ledger, stream, buffer);
	}

	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 1);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 1);
	if (g_test_verbose ())
		g_printerr ("control-lost after %" G_GINT64_FORMAT " ms\n",
			    (state.control_lost_time - unplug_time) / G_TIME_SPAN_MILLISECOND);
	g_assert_cmpint ((state.control_lost_time - unplug_time) / G_TIME_SPAN_MILLISECOND, <=,
			 fakeuv_timeout_ms (scenario->hang && (!scenario->hotplug || scenario->hotplug_register_error != 0) ?
					    LOSS_TIMEOUT_MS : FAST_LOSS_TIMEOUT_MS));

	/* The stream thread is gone, and did not spin before */
	n_calls_at_exit = _n_stream_calls (device);
	if (g_test_verbose ())
		g_printerr ("%" G_GUINT64_FORMAT " libusb calls in %" G_GINT64_FORMAT " ms\n",
			    n_calls_at_exit - n_calls_at_unplug,
			    (g_get_monotonic_time () - unplug_time) / G_TIME_SPAN_MILLISECOND);
	g_assert_cmpuint (n_calls_at_exit - n_calls_at_unplug, <=,
			  MAX_CALLS_AFTER_UNPLUG ((g_get_monotonic_time () - unplug_time) / G_TIME_SPAN_MILLISECOND));
	g_usleep (fakeuv_timeout_ms (50) * 1000);
	g_assert_cmpuint (_n_stream_calls (device), ==, n_calls_at_exit);

	/* Every buffer came back once, or still waits in the input queue */
	_ledger_check (&ledger, stream, held);
	g_assert_cmpuint (ledger.n_success, >=, N_FRAMES);
	if (scenario->moment == UNPLUG_WHILE_HELD && usb_mode == ARV_UV_USB_MODE_ASYNC && !scenario->hotplug)
		/* A buffer was given back to a stream whose submissions failed */
		g_assert_cmpuint (ledger.n_aborted, >=, 1);

	/* Control I/O fails at once, with the reason */
	g_assert_false (arv_camera_stop_acquisition (camera, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&error);

	g_object_unref (stream);
	g_clear_object (&camera);
	g_ptr_array_unref (held);
	_ledger_clear (&ledger);

	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 1);
	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);

	_replug_and_stream (device, usb_mode);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);
}

/* Without hotplug */

static const UnplugScenario scenario_no_device = {
	FAKE_USB_PLATFORM_LINUX, FALSE,
	LIBUSB_TRANSFER_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, FALSE, UNPLUG_WHILE_STREAMING
};
static const UnplugScenario scenario_error = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, FALSE, UNPLUG_WHILE_STREAMING
};
static const UnplugScenario scenario_stall = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_STALL, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, FALSE, UNPLUG_WHILE_STREAMING
};
static const UnplugScenario scenario_cancelled = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_CANCELLED, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, FALSE, UNPLUG_WHILE_STREAMING
};
/* Accepted, then failing: ERROR_IO_PENDING, then an error (the connection check included) */
static const UnplugScenario scenario_pending_error = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_ERROR, 0, 0, FALSE, UNPLUG_WHILE_STREAMING
};
/* Transfers that never complete: a triggered camera waiting, nothing else to notice */
static const UnplugScenario scenario_hang = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, TRUE, UNPLUG_WHILE_WAITING
};
/* The same while streaming: submissions may fail too */
static const UnplugScenario scenario_hang_streaming = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, TRUE, UNPLUG_WHILE_STREAMING
};
static const UnplugScenario scenario_submit_io = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, FALSE, UNPLUG_WHILE_HELD
};
static const UnplugScenario scenario_submit_no_device = {
	FAKE_USB_PLATFORM_WINDOWS, FALSE,
	LIBUSB_TRANSFER_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, FALSE, UNPLUG_WHILE_HELD
};

/* With hotplug: the platform presets (Windows: completions then hotplug, 10 ms later; Linux: hotplug first) */

static const UnplugScenario scenario_hotplug_windows = {
	FAKE_USB_PLATFORM_WINDOWS, TRUE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, FALSE, UNPLUG_WHILE_STREAMING
};
static const UnplugScenario scenario_hotplug_linux = {
	FAKE_USB_PLATFORM_LINUX, TRUE,
	LIBUSB_TRANSFER_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, FALSE, UNPLUG_WHILE_STREAMING
};
static const UnplugScenario scenario_hotplug_hang = {
	FAKE_USB_PLATFORM_WINDOWS, TRUE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, TRUE, UNPLUG_WHILE_WAITING
};
static const UnplugScenario scenario_hotplug_held = {
	FAKE_USB_PLATFORM_WINDOWS, TRUE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, FALSE, UNPLUG_WHILE_HELD
};
/* Hotplug support, but the device could not register its callback: no hotplug event comes, and transfers that never
 * complete must still be noticed */
static const UnplugScenario scenario_hotplug_unregistered = {
	FAKE_USB_PLATFORM_WINDOWS, TRUE,
	LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, TRUE, UNPLUG_WHILE_WAITING, LIBUSB_ERROR_NO_MEM
};

/* Test case setup, with RAW_IO for tests/fakeuvunplugrawio.c. Returns FALSE when the case is skipped: RAW_IO is a
 * WinUSB policy, which the Windows preset of the fake models only, with libusb 1.0.30 headers. */

static gboolean
_reset (FakeUsbPlatform platform)
{
	fakeuv_test_reset ();

#ifdef FAKEUV_UNPLUG_RAW_IO
	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return FALSE;
	}
	if (platform != FAKE_USB_PLATFORM_WINDOWS) {
		g_test_skip ("RAW_IO is a WinUSB policy");
		return FALSE;
	}
	g_setenv ("ARV_UV_RAW_IO", "1", TRUE);
#endif

	return TRUE;
}

static void
unplug_test (gconstpointer data)
{
	const UnplugScenario *scenario = data;

	/* The Windows backend of libusb reports this removal as LIBUSB_ERROR_IO, and failing transfers */
	if (!FAKEUV_IO_ERROR_IS_LOSS && scenario->platform == FAKE_USB_PLATFORM_WINDOWS &&
	    scenario->synchronous_error != LIBUSB_ERROR_NO_DEVICE) {
		g_test_skip ("A removal reported as LIBUSB_ERROR_IO is a loss of control for Windows builds only");
		return;
	}

	if (!_reset (scenario->platform))
		return;

	unplug (scenario, USB_MODE);

	/* Also checks that the libusb API was not misused */
	fakeuv_test_reset ();
}

/* A transfer failure without unplug: the device is checked, found, and streaming goes on. The camera starts over at
 * its next frame (@flush), or goes on with the rest of the frame, which the next reads get: out of step, with RAW_IO
 * and frames of whole packets as 640x480 and 1920x1080, the trailer ends the read that gets the end of the payload. */

typedef struct {
	enum libusb_transfer_status status;
	guint32 width;
	guint32 height;
	gboolean flush;
} TransientScenario;

static const TransientScenario transient_stall = { LIBUSB_TRANSFER_STALL, 720, 540, TRUE };
static const TransientScenario transient_error = { LIBUSB_TRANSFER_ERROR, 720, 540, TRUE };
static const TransientScenario transient_error_no_flush = { LIBUSB_TRANSFER_ERROR, 720, 540, FALSE };
static const TransientScenario transient_error_640x480 = { LIBUSB_TRANSFER_ERROR, 640, 480, FALSE };
static const TransientScenario transient_error_1920x1080 = { LIBUSB_TRANSFER_ERROR, 1920, 1080, FALSE };
static const TransientScenario transient_stall_640x480 = { LIBUSB_TRANSFER_STALL, 640, 480, FALSE };

static void
transient_failure (ArvUvUsbMode usb_mode, const TransientScenario *scenario)
{
	FakeuvStreamEvents events = {0};
	LossState state = {0};
	Ledger ledger;
	GPtrArray *buffers;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	guint64 n_control_requests;
	gint64 end_time;
	guint n_bad = 0, n_good_after = 0;
	size_t payload;
	guint i;

	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, scenario->width, scenario->height, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb), &state);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, usb_mode);
	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);

	_ledger_init (&ledger);
	for (i = 0; i < N_BUFFERS; i++)
		_ledger_push (&ledger, stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, WITH_RAW_IO);

	for (i = 0; i < N_FRAMES; i++) {
		buffer = _ledger_pop (&ledger, stream, fakeuv_timeout_ms (TIMEOUT_MS));
		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		_ledger_push (&ledger, stream, buffer);
	}

	/* A frame on its way: the transfer receiving its payload fails, with every other one pending on the data
	 * endpoint */
	n_control_requests = fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS);
	fake_usb_device_fail_transfers (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, scenario->status, 100000,
					scenario->flush);

	/* Then frames again, more than the buffers that may have been complete before the failure */
	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (n_good_after < N_BUFFERS + 5 && g_get_monotonic_time () < end_time) {
		gboolean failed = fake_usb_device_get_stat (device, FAKE_USB_STAT_SCRIPTED_FAILURES) > 0;

		buffer = _ledger_pop (&ledger, stream, 10);
		if (buffer == NULL)
			continue;

		if (!fakeuv_buffer_matches_frame (device, buffer))
			n_bad++;
		else if (failed)
			n_good_after++;
		_ledger_push (&ledger, stream, buffer);
	}

	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_SCRIPTED_FAILURES), >=, 1);
	g_assert_cmpuint (n_good_after, ==, N_BUFFERS + 5);
	/* In sync mode, the read that fails may be the one of a leader, before any buffer is filled */
	if (usb_mode == ARV_UV_USB_MODE_ASYNC)
		g_assert_cmpuint (n_bad, >=, 1);

	/* The device was checked, and found */
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS), >, n_control_requests);
	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), >=, n_bad);

	buffers = g_ptr_array_new_with_free_func (g_object_unref);
	_ledger_check (&ledger, stream, buffers);
	g_ptr_array_unref (buffers);
	_ledger_clear (&ledger);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
transient_failure_test (gconstpointer data)
{
	if (!_reset (FAKE_USB_PLATFORM_WINDOWS))
		return;

	transient_failure (USB_MODE, data);

	fakeuv_test_reset ();
}

#if !defined (FAKEUV_UNPLUG_SYNC) && !defined (FAKEUV_UNPLUG_RAW_IO)

/* The control channel of an unplugged camera, not streaming: the error says why. Without hotplug, nothing tells the
 * removal but the transfers that fail: those failing otherwise than with LIBUSB_ERROR_NO_DEVICE make Aravis check the
 * device, which is lost from then on, with Windows builds (FAKEUV_IO_ERROR_IS_LOSS). */

typedef struct {
	ArvDevice *device;
	gint n_control_lost;
	GError *error;
} ReentrantState;

static void
_reentrant_control_lost_cb (ArvDevice *device, ReentrantState *state)
{
	guint32 value;

	g_atomic_int_inc (&state->n_control_lost);

	/* Control I/O from the handler, emitted by a failing control transfer on a thread that holds the device's
	 * transfer lock: an error at once, not a deadlock */
	g_clear_error (&state->error);
	arv_device_read_register (device, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value, &state->error);
}

static void
control_channel_test (void)
{
	ReentrantState state = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvDevice *arv_device;
	GError *error = NULL;
	guint64 n_bulk_transfers, n_control_transfers;
	guint32 value;

	/* Removal reported as LIBUSB_ERROR_IO (Windows, no hotplug): the device is checked. With a Windows build,
	 * control-lost, then "not connected", at once; elsewhere, a transfer error, not a timeout. */
	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);
	state.device = arv_device;
	g_signal_connect (arv_device, "control-lost", G_CALLBACK (_reentrant_control_lost_cb), &state);

	n_control_transfers = fake_usb_device_get_call_count (device, FAKE_USB_CALL_CONTROL_TRANSFER);
	fake_usb_device_unplug (device);

	g_assert_false (arv_device_read_register (arv_device, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value, &error));
	g_assert_error (error, ARV_DEVICE_ERROR,
			(FAKEUV_IO_ERROR_IS_LOSS ? ARV_DEVICE_ERROR_NOT_CONNECTED : ARV_DEVICE_ERROR_TRANSFER_ERROR));
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CONTROL_TRANSFER), >,
			  n_control_transfers);
	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, FAKEUV_IO_ERROR_IS_LOSS ? 1 : 0);
	if (FAKEUV_IO_ERROR_IS_LOSS)
		g_assert_error (state.error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&state.error);

	n_bulk_transfers = fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER);
	g_assert_false (arv_device_write_register (arv_device, ARV_FAKE_CAMERA_REGISTER_WIDTH, 512, &error));
	g_assert_error (error, ARV_DEVICE_ERROR,
			(FAKEUV_IO_ERROR_IS_LOSS ? ARV_DEVICE_ERROR_NOT_CONNECTED : ARV_DEVICE_ERROR_TRANSFER_ERROR));
	g_clear_error (&error);
	if (FAKEUV_IO_ERROR_IS_LOSS)
		g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER), ==,
				  n_bulk_transfers);
	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, FAKEUV_IO_ERROR_IS_LOSS ? 1 : 0);

	g_clear_object (&camera);
	fakeuv_test_reset ();

	/* Removal reported as LIBUSB_ERROR_NO_DEVICE (Linux, no hotplug): control-lost, then "not connected", at once */
	fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);
	g_atomic_int_set (&state.n_control_lost, 0);
	state.device = arv_device;
	g_signal_connect (arv_device, "control-lost", G_CALLBACK (_reentrant_control_lost_cb), &state);

	fake_usb_device_unplug (device);

	g_assert_false (arv_device_write_register (arv_device, ARV_FAKE_CAMERA_REGISTER_WIDTH, 512, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&error);

	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 1);
	g_assert_error (state.error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&state.error);

	n_bulk_transfers = fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER);
	g_assert_false (arv_device_read_register (arv_device, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER), ==, n_bulk_transfers);
	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 1);

	g_clear_object (&camera);
}

static void
_count_control_lost_cb (ArvDevice *device, gint *n_control_lost)
{
	g_atomic_int_inc (n_control_lost);
}

/* A camera unplugged before the acquisition starts (Windows, no hotplug): the start fails at the first stream interface
 * register it cannot read, with the reason, rather than computing the stream layout from registers it could not read
 * and writing it to the camera */

static void
start_after_unplug_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint n_control_lost = 0;
	guint64 n_bulk_transfers;
	size_t payload;
	guint i;

	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_count_control_lost_cb),
			  &n_control_lost);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, USB_MODE);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	fake_usb_device_unplug (device);

	n_bulk_transfers = fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER);
	g_assert_false (arv_stream_start_acquisition (stream, &error));
	g_assert_error (error, ARV_DEVICE_ERROR,
			(FAKEUV_IO_ERROR_IS_LOSS ? ARV_DEVICE_ERROR_NOT_CONNECTED : ARV_DEVICE_ERROR_TRANSFER_ERROR));
	g_clear_error (&error);

	/* The tries of the first read, nothing more */
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER) - n_bulk_transfers, <=,
			  5);
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, FAKEUV_IO_ERROR_IS_LOSS ? 1 : 0);

	g_object_unref (stream);
	g_clear_object (&camera);
}

/* Receives @n_frames good frames, pushing back what comes, failed buffers included */

static void
_receive_good_frames (Ledger *ledger, ArvStream *stream, FakeUsbDevice *device, guint n_frames)
{
	gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	guint n_good = 0;

	while (n_good < n_frames && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = _ledger_pop (ledger, stream, 10);

		if (buffer == NULL)
			continue;

		if (fakeuv_buffer_matches_frame (device, buffer))
			n_good++;
		_ledger_push (ledger, stream, buffer);
	}

	g_assert_cmpuint (n_good, ==, n_frames);
}

/* Linux, without hotplug: the usbfs memory limit is reached (other cameras, other processes), and submissions fail
 * with LIBUSB_ERROR_NO_MEM until memory comes back. Not a removal, which Linux reports as LIBUSB_ERROR_NO_DEVICE: no
 * device check. The kernel would likely refuse the control request of a check for lack of memory too, which libusb
 * reports as LIBUSB_ERROR_IO for a control transfer, and a working camera lost its control. */

static void
no_memory_test (void)
{
	FakeuvStreamEvents events = {0};
	LossState state = {0};
	Ledger ledger;
	GPtrArray *buffers;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	guint64 n_control_transfers;
	gint64 end_time;
	size_t payload;
	guint i;

	fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb), &state);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);

	_ledger_init (&ledger);
	for (i = 0; i < N_BUFFERS; i++)
		_ledger_push (&ledger, stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	_receive_good_frames (&ledger, stream, device, N_FRAMES);

	/* No memory for any submission, nor for a control request */
	n_control_transfers = fake_usb_device_get_call_count (device, FAKE_USB_CALL_CONTROL_TRANSFER);
	fake_usb_device_inject_error (device, FAKE_USB_CALL_CONTROL_TRANSFER, -1, LIBUSB_ERROR_IO, -1);
	fake_usb_set_submit_memory_limit (1, 0);

	/* Longer than the retries of a submission take to be reported, the application pushing back what it gets */
	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (1500) * G_TIME_SPAN_MILLISECOND;
	while (g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = _ledger_pop (&ledger, stream, 10);

		if (buffer != NULL)
			_ledger_push (&ledger, stream, buffer);
	}

	g_assert_cmpuint (fake_usb_get_total_stat (FAKE_USB_STAT_MEMORY_LIMIT_REJECTIONS), >, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CONTROL_TRANSFER), ==,
			  n_control_transfers);
	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);

	/* Memory back: frames again */
	fake_usb_set_submit_memory_limit (0, 0);
	fake_usb_device_clear_injections (device);
	_receive_good_frames (&ledger, stream, device, 2 * N_BUFFERS);
	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 0);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	buffers = g_ptr_array_new_with_free_func (g_object_unref);
	_ledger_check (&ledger, stream, buffers);
	g_ptr_array_unref (buffers);
	_ledger_clear (&ledger);

	g_object_unref (stream);
	g_clear_object (&camera);
}

typedef struct {
	FakeUsbDevice *device;
	guint64 n_clear_halt;
} PipeResetWait;

static gboolean
_pipe_was_reset (gpointer user_data)
{
	PipeResetWait *wait = user_data;

	return fake_usb_device_get_call_count (wait->device, FAKE_USB_CALL_CLEAR_HALT) > wait->n_clear_halt;
}

/* Windows, without hotplug, RAW_IO: the camera goes away while the stream thread resynchronizes, its data reads hanging
 * until their timeout while its control requests fail with LIBUSB_ERROR_IO. The idle check applies during a
 * resynchronization too: control lost within about a second. The fake cannot script a removed device whose control
 * requests fail while its data reads hang; this is a triggered camera that sends nothing, so that the synchronous reads
 * time out, with control requests failing as those of a removed device do: with LIBUSB_ERROR_IO on Windows,
 * LIBUSB_ERROR_NO_DEVICE elsewhere. */

static void
resync_loss_test (void)
{
	FakeuvStreamEvents events = {0};
	LossState state = {0};
	PipeResetWait wait;
	Ledger ledger;
	GPtrArray *buffers;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	gint64 loss_time, end_time;
	size_t payload;
	guint i;

	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return;
	}

	/* Off by default */
	g_setenv ("ARV_UV_RAW_IO", "1", TRUE);

	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb), &state);

	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);

	_ledger_init (&ledger);
	for (i = 0; i < N_BUFFERS; i++)
		_ledger_push (&ledger, stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	for (i = 0; i < N_FRAMES; i++) {
		arv_camera_software_trigger (camera, &error);
		g_assert_no_error (error);
		buffer = _ledger_pop (&ledger, stream, fakeuv_timeout_ms (TIMEOUT_MS));
		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		_ledger_push (&ledger, stream, buffer);
	}

	/* The camera stalls its endpoint: with RAW_IO, the stream thread resets the pipe, then resynchronizes, reading
	 * what the camera, waiting for a trigger, does not send */
	wait.device = device;
	wait.n_clear_halt = fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLEAR_HALT);
	fake_usb_device_halt_endpoint (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);
	g_assert_true (fakeuv_wait_for (_pipe_was_reset, &wait, TIMEOUT_MS));
	g_usleep (fakeuv_timeout_ms (200) * 1000);

	/* Gone */
	loss_time = g_get_monotonic_time ();
	fake_usb_device_inject_error (device, FAKE_USB_CALL_CONTROL_TRANSFER, -1,
				      FAKEUV_IO_ERROR_IS_LOSS ? LIBUSB_ERROR_IO : LIBUSB_ERROR_NO_DEVICE, -1);

	end_time = loss_time + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while ((g_atomic_int_get (&state.n_control_lost) == 0 || g_atomic_int_get (&events.n_exit) == 0) &&
	       g_get_monotonic_time () < end_time) {
		buffer = _ledger_pop (&ledger, stream, 10);
		if (buffer != NULL)
			_ledger_push (&ledger, stream, buffer);
	}

	if (g_test_verbose ())
		g_printerr ("control-lost after %" G_GINT64_FORMAT " ms\n",
			    (state.control_lost_time - loss_time) / G_TIME_SPAN_MILLISECOND);

	g_assert_cmpint (g_atomic_int_get (&state.n_control_lost), ==, 1);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 1);
	g_assert_cmpint ((state.control_lost_time - loss_time) / G_TIME_SPAN_MILLISECOND, <=,
			 fakeuv_timeout_ms (LOSS_TIMEOUT_MS));

	buffers = g_ptr_array_new_with_free_func (g_object_unref);
	_ledger_check (&ledger, stream, buffers);
	g_ptr_array_unref (buffers);
	_ledger_clear (&ledger);

	g_assert_false (arv_camera_stop_acquisition (camera, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&error);

	g_object_unref (stream);
	g_clear_object (&camera);
}

#endif

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	g_test_add_data_func (TEST_PATH ("no-device"), &scenario_no_device, unplug_test);
	g_test_add_data_func (TEST_PATH ("error"), &scenario_error, unplug_test);
	g_test_add_data_func (TEST_PATH ("stall"), &scenario_stall, unplug_test);
	g_test_add_data_func (TEST_PATH ("cancelled"), &scenario_cancelled, unplug_test);
	g_test_add_data_func (TEST_PATH ("pending-error"), &scenario_pending_error, unplug_test);
	g_test_add_data_func (TEST_PATH ("hang"), &scenario_hang, unplug_test);
	g_test_add_data_func (TEST_PATH ("hang-streaming"), &scenario_hang_streaming, unplug_test);
	g_test_add_data_func (TEST_PATH ("submit-io"), &scenario_submit_io, unplug_test);
	g_test_add_data_func (TEST_PATH ("submit-no-device"), &scenario_submit_no_device, unplug_test);
	g_test_add_data_func (TEST_PATH ("hotplug-windows"), &scenario_hotplug_windows, unplug_test);
	g_test_add_data_func (TEST_PATH ("hotplug-linux"), &scenario_hotplug_linux, unplug_test);
	g_test_add_data_func (TEST_PATH ("hotplug-hang"), &scenario_hotplug_hang, unplug_test);
	g_test_add_data_func (TEST_PATH ("hotplug-held"), &scenario_hotplug_held, unplug_test);
	g_test_add_data_func (TEST_PATH ("hotplug-unregistered"), &scenario_hotplug_unregistered, unplug_test);

	g_test_add_data_func (TEST_PATH ("transient-stall"), &transient_stall, transient_failure_test);
	g_test_add_data_func (TEST_PATH ("transient-error"), &transient_error, transient_failure_test);
	g_test_add_data_func (TEST_PATH ("transient-error-no-flush"), &transient_error_no_flush,
			      transient_failure_test);
	g_test_add_data_func (TEST_PATH ("transient-error-640x480"), &transient_error_640x480, transient_failure_test);
	g_test_add_data_func (TEST_PATH ("transient-error-1920x1080"), &transient_error_1920x1080,
			      transient_failure_test);
	g_test_add_data_func (TEST_PATH ("transient-stall-640x480"), &transient_stall_640x480, transient_failure_test);

#if !defined (FAKEUV_UNPLUG_SYNC) && !defined (FAKEUV_UNPLUG_RAW_IO)
	fakeuv_test_add (TEST_PATH ("control-channel"), control_channel_test);
	fakeuv_test_add (TEST_PATH ("start-after-unplug"), start_after_unplug_test);
	fakeuv_test_add (TEST_PATH ("no-memory"), no_memory_test);
	fakeuv_test_add (TEST_PATH ("resync-loss"), resync_loss_test);
#endif

	return fakeuv_test_run ();
}
