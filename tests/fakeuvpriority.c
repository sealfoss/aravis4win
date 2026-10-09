/* SPDX-License-Identifier:Unlicense */

/* Thread priorities, against the fake libusb, which records the priority of the threads calling
 * libusb_handle_events*() on Windows (fake_usb_device_get_event_thread_priority ()).
 *
 * An application raises the priority of the stream thread from the stream callback, at ARV_STREAM_CALLBACK_TYPE_INIT,
 * as arv-camera-test --realtime and --high-priority do. On Windows, the libusb event thread of the device, which runs
 * the transfer callbacks (in async mode, they complete the buffers), takes that priority during the acquisition, and
 * goes back to its own afterwards. Without a raise, nothing changes, and the event thread never goes below its own
 * priority. Each device has its own event thread, and takes one priority request at a time.
 *
 * arv_make_thread_realtime () reports success only when Windows granted the realtime class, which needs a privilege:
 * without it, SetPriorityClass (REALTIME_PRIORITY_CLASS) succeeds, but the process gets HIGH_PRIORITY_CLASS. The
 * decision is checked on made-up answers, and the real call on this machine, with or without the privilege; the
 * priority class of the process is restored at once. */

#include "fakeuvtest.h"
#include <arvuvdeviceprivate.h>
#include <arvrealtimeprivate.h>
#include <string.h>

#define N_BUFFERS		4
#define N_FRAMES		10
#define TIMEOUT_MS		FAKEUV_TIMEOUT_MS

#ifdef G_OS_WIN32

/* The INIT callback leaves the priority of the stream thread as it is */
#define KEEP_PRIORITY		THREAD_PRIORITY_ERROR_RETURN

typedef struct {
	FakeuvStreamEvents events;

	/* At INIT: arv_make_thread_high_priority (), or SetThreadPriority (init_priority) */
	gboolean high_priority;
	int init_priority;
	gint init_failed;

	/* Async mode: the priority of the thread that completed the last buffer, the event thread */
	gint done_priority;
} StreamData;

static void
_stream_callback (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer)
{
	StreamData *data = user_data;

	switch (type) {
		case ARV_STREAM_CALLBACK_TYPE_INIT:
			if (data->high_priority) {
				if (!arv_make_thread_high_priority (-10))
					g_atomic_int_set (&data->init_failed, TRUE);
			} else if (data->init_priority != KEEP_PRIORITY) {
				if (!SetThreadPriority (GetCurrentThread (), data->init_priority))
					g_atomic_int_set (&data->init_failed, TRUE);
			}
			break;
		case ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE:
			g_atomic_int_set (&data->done_priority, GetThreadPriority (GetCurrentThread ()));
			break;
		default:
			break;
	}

	fakeuv_stream_callback (&data->events, type, buffer);
}

static void
_stream_data_init (StreamData *data, gboolean high_priority, int init_priority)
{
	memset (data, 0, sizeof (StreamData));
	data->high_priority = high_priority;
	data->init_priority = init_priority;
	data->done_priority = THREAD_PRIORITY_ERROR_RETURN;	/* no buffer yet */
}

static FakeUsbDevice *
_add_camera (const char *serial_number)
{
	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);

	return fakeuv_add_camera (serial_number);
}

static ArvCamera *
_open_camera (FakeUsbDevice *device)
{
	ArvCamera *camera;
	GError *error = NULL;

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));

	return camera;
}

static ArvStream *
_start_acquisition (ArvCamera *camera, ArvUvUsbMode usb_mode, StreamData *data)
{
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i;

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, usb_mode);
	stream = arv_camera_create_stream (camera, _stream_callback, data, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));

	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	return stream;
}

static void
_receive_frames (ArvStream *stream, FakeUsbDevice *device, guint n_frames)
{
	guint i;

	for (i = 0; i < n_frames; i++) {
		ArvBuffer *buffer;

		buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);
		g_assert_true (ARV_IS_BUFFER (buffer));
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		arv_stream_push_buffer (stream, buffer);
	}
}

static void
_stop_acquisition (ArvCamera *camera, ArvStream *stream, StreamData *data)
{
	GError *error = NULL;

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);
	g_object_unref (stream);

	g_assert_cmpint (g_atomic_int_get (&data->events.n_init), ==, 1);
	g_assert_cmpint (g_atomic_int_get (&data->events.n_exit), ==, 1);
	g_assert_false (g_atomic_int_get (&data->init_failed));
}

typedef struct {
	FakeUsbDevice *device;
	int priority;
	guint64 n_calls;
} EventThreadWait;

static gboolean
_event_thread_has_priority (gpointer user_data)
{
	EventThreadWait *wait = user_data;
	int last;

	return fake_usb_device_get_event_thread_priority (wait->device, &last, NULL) && last == wait->priority;
}

static gboolean
_event_thread_has_turned (gpointer user_data)
{
	EventThreadWait *wait = user_data;

	return fake_usb_device_get_call_count (wait->device, FAKE_USB_CALL_HANDLE_EVENTS) >= wait->n_calls;
}

/* Aravis's event thread is the only caller of libusb_handle_events*(), at least every 100 ms. The fake counts a call
 * before it records its priority: after two turns, the first one is recorded. */

static void
_wait_for_event_thread_turns (FakeUsbDevice *device, guint n_turns)
{
	EventThreadWait wait = { device, 0, 0 };

	wait.n_calls = fake_usb_device_get_call_count (device, FAKE_USB_CALL_HANDLE_EVENTS) + n_turns;
	g_assert_true (fakeuv_wait_for (_event_thread_has_turned, &wait, TIMEOUT_MS));
}

/* The event thread of @device runs at @priority, and stays there. It takes a requested priority at the start of its
 * next turn, and the fake records the priority at the start of each libusb_handle_events*() call. */

static void
_assert_event_thread_priority (FakeUsbDevice *device, int priority)
{
	EventThreadWait wait = { device, priority, 0 };
	int last = 0;
	int highest = 0;

	if (!fakeuv_wait_for (_event_thread_has_priority, &wait, TIMEOUT_MS)) {
		g_assert_true (fake_usb_device_get_event_thread_priority (device, &last, NULL));
		g_assert_cmpint (last, ==, priority);
	}

	fake_usb_device_reset_event_thread_priority (device);
	_wait_for_event_thread_turns (device, 2);

	g_assert_true (fake_usb_device_get_event_thread_priority (device, &last, &highest));
	g_assert_cmpint (last, ==, priority);
	g_assert_cmpint (highest, ==, priority);
}

/* The INIT callback raises the stream thread, with arv_make_thread_high_priority () as arv-camera-test --high-priority
 * does: the event thread takes THREAD_PRIORITY_HIGHEST during the acquisition, the buffers complete at that priority,
 * and it is back to normal once the acquisition stops. */

static void
async_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	StreamData data;

	device = _add_camera ("UV1");
	camera = _open_camera (device);

	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	_stream_data_init (&data, TRUE, KEEP_PRIORITY);
	stream = _start_acquisition (camera, ARV_UV_USB_MODE_ASYNC, &data);

	_receive_frames (stream, device, N_FRAMES);
	_assert_event_thread_priority (device, THREAD_PRIORITY_HIGHEST);

	/* Completed on the event thread, at the raised priority */
	_receive_frames (stream, device, N_FRAMES);
	g_assert_cmpint (g_atomic_int_get (&data.done_priority), ==, THREAD_PRIORITY_HIGHEST);

	_stop_acquisition (camera, stream, &data);

	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	g_object_unref (camera);
}

/* Sync mode: the event thread usually handles the completion of the synchronous transfers of the stream thread, and
 * takes its priority too, whatever it is */

static void
sync_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	StreamData data;

	device = _add_camera ("UV1");
	camera = _open_camera (device);

	_stream_data_init (&data, FALSE, THREAD_PRIORITY_ABOVE_NORMAL);
	stream = _start_acquisition (camera, ARV_UV_USB_MODE_SYNC, &data);

	_receive_frames (stream, device, N_FRAMES);
	_assert_event_thread_priority (device, THREAD_PRIORITY_ABOVE_NORMAL);
	_receive_frames (stream, device, N_FRAMES);

	_stop_acquisition (camera, stream, &data);

	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	g_object_unref (camera);
}

/* Without a raise, the event thread stays at normal priority from start to end */

static void
unchanged_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	StreamData data;
	int last = 0;
	int highest = 0;

	device = _add_camera ("UV1");
	camera = _open_camera (device);

	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	_stream_data_init (&data, FALSE, KEEP_PRIORITY);
	stream = _start_acquisition (camera, ARV_UV_USB_MODE_ASYNC, &data);
	_receive_frames (stream, device, 2 * N_FRAMES);
	g_assert_cmpint (g_atomic_int_get (&data.done_priority), ==, THREAD_PRIORITY_NORMAL);
	_stop_acquisition (camera, stream, &data);

	g_assert_true (fake_usb_device_get_event_thread_priority (device, &last, &highest));
	g_assert_cmpint (last, ==, THREAD_PRIORITY_NORMAL);
	g_assert_cmpint (highest, ==, THREAD_PRIORITY_NORMAL);

	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	g_object_unref (camera);
}

/* After the acquisition, the event thread goes back to the priority it started with, which is not always normal:
 * g_thread_new () gives it the priority of the thread that opens the device */

static void
own_priority_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	StreamData data;
	int own_priority = 0;

	device = _add_camera ("UV1");

	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_ABOVE_NORMAL));
	camera = _open_camera (device);
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL));

	fake_usb_device_reset_event_thread_priority (device);
	_wait_for_event_thread_turns (device, 2);
	g_assert_true (fake_usb_device_get_event_thread_priority (device, &own_priority, NULL));
	if (own_priority != THREAD_PRIORITY_ABOVE_NORMAL)
		g_test_message ("The event thread did not start with the priority of the thread that created it (%d)",
				own_priority);
	g_assert_cmpint (own_priority, !=, THREAD_PRIORITY_HIGHEST);
	_assert_event_thread_priority (device, own_priority);

	_stream_data_init (&data, FALSE, THREAD_PRIORITY_HIGHEST);
	stream = _start_acquisition (camera, ARV_UV_USB_MODE_ASYNC, &data);

	_receive_frames (stream, device, N_FRAMES);
	_assert_event_thread_priority (device, THREAD_PRIORITY_HIGHEST);
	_receive_frames (stream, device, N_FRAMES);
	g_assert_cmpint (g_atomic_int_get (&data.done_priority), ==, THREAD_PRIORITY_HIGHEST);

	_stop_acquisition (camera, stream, &data);

	_assert_event_thread_priority (device, own_priority);

	g_object_unref (camera);
}

/* One event thread per device: the raise of one stream does not reach the event thread of another device */

static void
two_cameras_test (void)
{
	FakeUsbDevice *devices[2];
	ArvCamera *cameras[2];
	ArvStream *streams[2];
	StreamData data[2];
	guint i;

	devices[0] = _add_camera ("UV1");
	devices[1] = fakeuv_add_camera ("UV2");

	for (i = 0; i < 2; i++)
		cameras[i] = _open_camera (devices[i]);

	_stream_data_init (&data[0], FALSE, THREAD_PRIORITY_HIGHEST);
	_stream_data_init (&data[1], FALSE, KEEP_PRIORITY);
	for (i = 0; i < 2; i++)
		streams[i] = _start_acquisition (cameras[i], ARV_UV_USB_MODE_ASYNC, &data[i]);

	for (i = 0; i < 2; i++)
		_receive_frames (streams[i], devices[i], N_FRAMES);

	_assert_event_thread_priority (devices[0], THREAD_PRIORITY_HIGHEST);
	_assert_event_thread_priority (devices[1], THREAD_PRIORITY_NORMAL);

	for (i = 0; i < 2; i++) {
		_receive_frames (streams[i], devices[i], N_FRAMES);
		_stop_acquisition (cameras[i], streams[i], &data[i]);
	}

	g_assert_cmpint (g_atomic_int_get (&data[0].done_priority), ==, THREAD_PRIORITY_HIGHEST);
	g_assert_cmpint (g_atomic_int_get (&data[1].done_priority), ==, THREAD_PRIORITY_NORMAL);

	for (i = 0; i < 2; i++) {
		_assert_event_thread_priority (devices[i], THREAD_PRIORITY_NORMAL);
		g_object_unref (cameras[i]);
	}
}

/* The private API the stream threads use: the priority of the calling thread, read at the request, one request at a
 * time per device */

static void
request_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvUvDevice *uv_device;

	device = _add_camera ("UV1");
	camera = _open_camera (device);
	uv_device = ARV_UV_DEVICE (arv_camera_get_device (camera));

	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_ABOVE_NORMAL));
	g_assert_true (arv_uv_device_request_event_thread_priority (uv_device));
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL));
	_assert_event_thread_priority (device, THREAD_PRIORITY_ABOVE_NORMAL);

	/* Taken already */
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_HIGHEST));
	g_assert_false (arv_uv_device_request_event_thread_priority (uv_device));
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL));
	_assert_event_thread_priority (device, THREAD_PRIORITY_ABOVE_NORMAL);

	arv_uv_device_release_event_thread_priority (uv_device);
	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	/* Free again; a lower priority is taken as a request, but does not lower the event thread */
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_LOWEST));
	g_assert_true (arv_uv_device_request_event_thread_priority (uv_device));
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL));
	_wait_for_event_thread_turns (device, 2);
	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	arv_uv_device_release_event_thread_priority (uv_device);
	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	g_object_unref (camera);
}

/* The event thread never goes below its own priority, which g_thread_new () copies from the thread that opens the
 * device: not when the INIT callback lowers the stream thread, nor when the acquisition starts from a thread of lower
 * priority than the one that opened the device, with no INIT callback raising it */

static void
never_lower_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	StreamData data;
	int own_priority = 0;

	/* The INIT callback lowers the stream thread */
	device = _add_camera ("UV1");
	camera = _open_camera (device);
	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);

	_stream_data_init (&data, FALSE, THREAD_PRIORITY_LOWEST);
	stream = _start_acquisition (camera, ARV_UV_USB_MODE_ASYNC, &data);
	_receive_frames (stream, device, N_FRAMES);
	_wait_for_event_thread_turns (device, 2);
	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);
	_receive_frames (stream, device, N_FRAMES);
	g_assert_cmpint (g_atomic_int_get (&data.done_priority), ==, THREAD_PRIORITY_NORMAL);
	_stop_acquisition (camera, stream, &data);
	_assert_event_thread_priority (device, THREAD_PRIORITY_NORMAL);
	g_object_unref (camera);

	/* The device opened from a raised thread, the acquisition started from a normal one */
	fakeuv_test_reset ();
	device = _add_camera ("UV1");

	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_ABOVE_NORMAL));
	camera = _open_camera (device);
	g_assert_true (SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL));

	fake_usb_device_reset_event_thread_priority (device);
	_wait_for_event_thread_turns (device, 2);
	g_assert_true (fake_usb_device_get_event_thread_priority (device, &own_priority, NULL));
	if (own_priority == THREAD_PRIORITY_NORMAL) {
		g_test_message ("The event thread did not start with the priority of the thread that created it");
	} else {
		g_assert_cmpint (own_priority, ==, THREAD_PRIORITY_ABOVE_NORMAL);

		_stream_data_init (&data, FALSE, KEEP_PRIORITY);
		stream = _start_acquisition (camera, ARV_UV_USB_MODE_ASYNC, &data);
		_receive_frames (stream, device, N_FRAMES);
		_wait_for_event_thread_turns (device, 2);
		_assert_event_thread_priority (device, THREAD_PRIORITY_ABOVE_NORMAL);
		_receive_frames (stream, device, N_FRAMES);
		g_assert_cmpint (g_atomic_int_get (&data.done_priority), ==, THREAD_PRIORITY_ABOVE_NORMAL);
		_stop_acquisition (camera, stream, &data);
		_assert_event_thread_priority (device, THREAD_PRIORITY_ABOVE_NORMAL);
	}

	g_object_unref (camera);
}

/* arv_win32_check_priority_request (): what counts as granted */

static void
_set_request (ArvWin32PriorityRequest *request, DWORD priority_class, int priority)
{
	memset (request, 0, sizeof (ArvWin32PriorityRequest));
	request->requested_class = priority_class;
	request->class_set = priority_class != 0;
	request->granted_class = priority_class != 0 ? priority_class : NORMAL_PRIORITY_CLASS;
	request->requested_priority = priority;
	request->priority_set = TRUE;
	request->granted_priority = priority;
}

static void
priority_request_test (void)
{
	ArvWin32PriorityRequest request;

	/* arv_make_thread_realtime (), granted */
	_set_request (&request, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL);
	g_assert_true (arv_win32_check_priority_request (&request));

	/* Without the privilege to increase the scheduling priority: SetPriorityClass () succeeds, the process gets the
	 * high class */
	request.granted_class = HIGH_PRIORITY_CLASS;
	g_assert_false (arv_win32_check_priority_request (&request));

	/* GetPriorityClass () failed */
	request.granted_class = 0;
	g_assert_false (arv_win32_check_priority_request (&request));

	/* SetPriorityClass () failed */
	_set_request (&request, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL);
	request.class_set = FALSE;
	request.class_error = ERROR_ACCESS_DENIED;
	request.granted_class = NORMAL_PRIORITY_CLASS;
	g_assert_false (arv_win32_check_priority_request (&request));

	/* SetThreadPriority () failed */
	_set_request (&request, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL);
	request.priority_set = FALSE;
	request.priority_error = ERROR_INVALID_PARAMETER;
	request.granted_priority = THREAD_PRIORITY_NORMAL;
	g_assert_false (arv_win32_check_priority_request (&request));

	/* The thread has another priority than the one asked for, or GetThreadPriority () failed */
	_set_request (&request, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL);
	request.granted_priority = THREAD_PRIORITY_HIGHEST;
	g_assert_false (arv_win32_check_priority_request (&request));
	request.granted_priority = THREAD_PRIORITY_ERROR_RETURN;
	g_assert_false (arv_win32_check_priority_request (&request));

	/* arv_make_thread_high_priority (): the class of the process is not part of the request */
	_set_request (&request, 0, THREAD_PRIORITY_HIGHEST);
	g_assert_true (arv_win32_check_priority_request (&request));
	request.granted_class = HIGH_PRIORITY_CLASS;
	g_assert_true (arv_win32_check_priority_request (&request));
	request.granted_class = 0;
	g_assert_true (arv_win32_check_priority_request (&request));
	request.priority_set = FALSE;
	request.priority_error = ERROR_ACCESS_DENIED;
	request.granted_priority = THREAD_PRIORITY_NORMAL;
	g_assert_false (arv_win32_check_priority_request (&request));
}

typedef struct {
	gboolean high_priority;
	gboolean result;
	DWORD priority_class;
	int priority;
} PriorityCall;

static gpointer
_priority_call_thread (gpointer user_data)
{
	PriorityCall *call = user_data;

	call->result = call->high_priority ? arv_make_thread_high_priority (-10) : arv_make_thread_realtime (10);
	call->priority_class = GetPriorityClass (GetCurrentProcess ());
	call->priority = GetThreadPriority (GetCurrentThread ());

	return NULL;
}

/* The real calls, on a thread of their own, the process priority class restored at once. Whether this machine grants
 * the realtime class depends on the privileges of the test: the result must say what was granted. */

static void
realtime_test (void)
{
	PriorityCall call = {0};
	DWORD original_class;

	original_class = GetPriorityClass (GetCurrentProcess ());
	g_assert_cmpuint (original_class, !=, 0);

	call.high_priority = FALSE;
	g_thread_join (g_thread_new ("realtime", _priority_call_thread, &call));

	g_assert_true (SetPriorityClass (GetCurrentProcess (), original_class));
	g_assert_cmpuint (GetPriorityClass (GetCurrentProcess ()), ==, original_class);

	g_test_message ("arv_make_thread_realtime (): %s, priority class 0x%lx, thread priority %d",
			call.result ? "TRUE" : "FALSE", call.priority_class, call.priority);

	g_assert_cmpint (call.priority, ==, THREAD_PRIORITY_TIME_CRITICAL);
	g_assert_cmpint (call.result, ==, call.priority_class == REALTIME_PRIORITY_CLASS);
	if (!call.result)
		g_assert_cmpuint (call.priority_class, ==, HIGH_PRIORITY_CLASS);

	call.high_priority = TRUE;
	g_thread_join (g_thread_new ("high-priority", _priority_call_thread, &call));

	g_assert_true (SetPriorityClass (GetCurrentProcess (), original_class));

	g_assert_true (call.result);
	g_assert_cmpint (call.priority, ==, THREAD_PRIORITY_HIGHEST);
	g_assert_cmpuint (call.priority_class, ==, original_class);
}

#else

/* Elsewhere, the event thread keeps its priority, and the stream threads do not ask for any */

static void
other_platforms_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	g_assert_false (arv_uv_device_request_event_thread_priority (ARV_UV_DEVICE (arv_camera_get_device (camera))));
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_ASYNC, N_BUFFERS, N_FRAMES, TIMEOUT_MS),
			  ==, N_FRAMES);

	g_object_unref (camera);
}

#endif

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

#ifdef G_OS_WIN32
	fakeuv_test_add ("/fakeuvpriority/async", async_test);
	fakeuv_test_add ("/fakeuvpriority/sync", sync_test);
	fakeuv_test_add ("/fakeuvpriority/unchanged", unchanged_test);
	fakeuv_test_add ("/fakeuvpriority/own-priority", own_priority_test);
	fakeuv_test_add ("/fakeuvpriority/two-cameras", two_cameras_test);
	fakeuv_test_add ("/fakeuvpriority/request", request_test);
	fakeuv_test_add ("/fakeuvpriority/never-lower", never_lower_test);
	fakeuv_test_add ("/fakeuvpriority/priority-request", priority_request_test);
	fakeuv_test_add ("/fakeuvpriority/realtime", realtime_test);
#else
	fakeuv_test_add ("/fakeuvpriority/other-platforms", other_platforms_test);
#endif

	return fakeuv_test_run ();
}
