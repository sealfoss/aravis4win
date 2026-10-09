/* SPDX-License-Identifier:Unlicense */

/* USB3 Vision stream defects, against the fake libusb:
 * - buffers pushed back while the trailer callback that completed them still runs (async mode);
 * - the buffer counts of arv_stream_get_n_owned_buffers () while threads wait on the stream queues;
 * - in async mode, the transfer contexts of released buffers: used again for a new buffer at the address of a released
 *   one, or for a buffer of another size in the memory of a released one, piling up when the application replaces its
 *   buffers, and the context of a buffer being filled used for another buffer of the same memory;
 * - a stream that stops after another stream of its camera has started;
 * - stream interface registers that the acquisition start cannot read;
 * - in sync mode: the leader of the next frame parsed from a buffer the application owns already, reads shorter than a
 *   packet header, frames cut short, with or without their trailer, and the padding of the final transfer. */

#include "fakeuvtest.h"
#include <arvbufferprivate.h>
#include <arvuvcpprivate.h>
#include <string.h>

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
#define RAW_IO_AVAILABLE	TRUE
#else
#define RAW_IO_AVAILABLE	FALSE
#endif

#define N_BUFFERS	3
#define TIMEOUT_MS	FAKEUV_TIMEOUT_MS

/* Pops a buffer, waiting for it at most TIMEOUT_MS (scaled) */

static ArvBuffer *
_pop_buffer (ArvStream *stream)
{
	return arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);
}

/* Pops @n_frames frames, each one good, and pushes them back */

static void
_receive_good_frames (ArvStream *stream, FakeUsbDevice *device, guint n_frames)
{
	guint i;

	for (i = 0; i < n_frames; i++) {
		ArvBuffer *buffer = _pop_buffer (stream);

		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		arv_stream_push_buffer (stream, buffer);
	}
}

typedef struct {
	FakeUsbDevice *device;
	guint64 n_frames;
} FramesStarted;

static gboolean
_frame_started (gpointer user_data)
{
	FramesStarted *started = user_data;

	return fake_usb_device_get_stat (started->device, FAKE_USB_STAT_FRAMES_STARTED) > started->n_frames;
}

/* A camera in software trigger mode: triggers a frame, and waits until the camera starts sending it. A trigger that
 * comes before the camera has started the frame of the previous one is lost. */

static void
_trigger_frame (ArvCamera *camera, FakeUsbDevice *device)
{
	FramesStarted started = { device, fake_usb_device_get_stat (device, FAKE_USB_STAT_FRAMES_STARTED) };
	GError *error = NULL;

	arv_camera_software_trigger (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fakeuv_wait_for (_frame_started, &started, TIMEOUT_MS));
}

static ArvStream *
_start_stream (ArvCamera *camera, ArvUvUsbMode usb_mode, ArvStreamCallback callback, void *callback_data,
	       guint n_buffers)
{
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i;

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, usb_mode);
	stream = arv_camera_create_stream (camera, callback, callback_data, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));

	for (i = 0; i < n_buffers; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	return stream;
}

static void
_assert_no_buffer_lost (ArvStream *stream, guint n_buffers)
{
	gint n_input = -1, n_output = -1, n_filling = -1;

	arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, &n_filling);
	g_assert_cmpint (n_filling, ==, 0);
	g_assert_cmpint (n_input + n_output, ==, n_buffers);
}

/* Trailer callback race. The trailer callback, on libusb's event thread, completes a buffer.
 * The application may pop it and push it back at once (aravissrc does), and the stream thread may attach it to its
 * transfer context again, before the callback has returned. The callback used to push the buffer to the output queue
 * first and to detach it from the context afterwards, losing it when the stream thread had attached it in between.
 * Here the BUFFER_DONE callback, which runs within the trailer callback, holds the event thread until the stream
 * thread has started the next fill of the same buffer and submitted its first transfer: the old order lost the buffer
 * every time. */

typedef struct {
	FakeUsbDevice *device;
	GThread *main_thread;
	GMutex mutex;
	GCond cond;
	guint n_starts;
	guint n_dones;
	guint64 n_submits_at_last_start;
	gint armed;
	gint n_rounds;
	gint n_raced;
	gint n_timeouts;
} RaceState;

/* With a single buffer, fills alternate: START_BUFFER 1, BUFFER_DONE 1, START_BUFFER 2... START_BUFFER n + 1 may come
 * before or after BUFFER_DONE n is entered, as the buffer is in the output queue by then. Transfer callbacks run on
 * libusb's event thread, or on a thread doing synchronous I/O, which the main thread does while it starts the
 * acquisition: only the event thread is held. */

static void
_race_callback (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer)
{
	RaceState *state = user_data;

	switch (type) {
		case ARV_STREAM_CALLBACK_TYPE_START_BUFFER:
			/* Stream thread, right before the buffer is attached to its context and its transfers
			 * submitted */
			g_mutex_lock (&state->mutex);
			state->n_starts++;
			state->n_submits_at_last_start = fake_usb_device_get_call_count (state->device,
											 FAKE_USB_CALL_SUBMIT_TRANSFER);
			g_cond_broadcast (&state->cond);
			g_mutex_unlock (&state->mutex);
			break;
		case ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE: {
			gint64 end_time;
			guint64 n_submits = 0;
			gboolean restarted;

			g_mutex_lock (&state->mutex);
			state->n_dones++;
			g_mutex_unlock (&state->mutex);

			/* After a round that timed out, the test fails: the event thread is not held any more */
			if (!g_atomic_int_get (&state->armed) || g_thread_self () == state->main_thread ||
			    g_atomic_int_get (&state->n_rounds) <= 0 || g_atomic_int_get (&state->n_timeouts) > 0)
				break;

			/* libusb event thread, inside the trailer callback: wait for the next fill of the buffer... */
			end_time = g_get_monotonic_time () +
				(gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;

			g_mutex_lock (&state->mutex);
			while (state->n_starts <= state->n_dones &&
			       g_cond_wait_until (&state->cond, &state->mutex, end_time))
				;
			restarted = state->n_starts > state->n_dones;
			if (restarted)
				n_submits = state->n_submits_at_last_start;
			g_mutex_unlock (&state->mutex);

			/* ...and for its first transfer, submitted once the stream thread has attached the buffer to
			 * the transfer context again */
			while (restarted &&
			       fake_usb_device_get_call_count (state->device, FAKE_USB_CALL_SUBMIT_TRANSFER) <= n_submits &&
			       g_get_monotonic_time () < end_time)
				g_usleep (200);

			if (restarted &&
			    fake_usb_device_get_call_count (state->device, FAKE_USB_CALL_SUBMIT_TRANSFER) > n_submits)
				g_atomic_int_inc (&state->n_raced);
			else
				g_atomic_int_inc (&state->n_timeouts);

			g_atomic_int_add (&state->n_rounds, -1);
			break;
		}
		default:
			break;
	}
}

static void
trailer_race_test (void)
{
	RaceState state = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	guint n_frames = 0;

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	g_mutex_init (&state.mutex);
	g_cond_init (&state.cond);
	state.device = device;
	state.main_thread = g_thread_self ();
	state.n_rounds = 5;

	/* One buffer: each frame needs the buffer of the previous one back */
	stream = _start_stream (camera, ARV_UV_USB_MODE_ASYNC, _race_callback, &state, 1);

	/* No more libusb calls from this thread */
	g_atomic_int_set (&state.armed, TRUE);

	/* The frame after the last round can only come once that round is over */
	while (g_atomic_int_get (&state.n_rounds) > 0 && n_frames < 20) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);

		g_assert_nonnull (buffer);
		fakeuv_assert_frame (device, buffer, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
		n_frames++;

		/* At once, while the BUFFER_DONE callback still waits */
		arv_stream_push_buffer (stream, buffer);
	}

	/* Every round really pushed the buffer back while the trailer callback was running */
	g_assert_cmpint (g_atomic_int_get (&state.n_raced), ==, 5);
	g_assert_cmpint (g_atomic_int_get (&state.n_timeouts), ==, 0);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	_assert_no_buffer_lost (stream, 1);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

	g_object_unref (stream);
	g_clear_object (&camera);

	g_mutex_clear (&state.mutex);
	g_cond_clear (&state.cond);
}

/* What Aravis's documentation shows for the stream callback: pop the buffer and push it back from BUFFER_DONE, which
 * in async USB mode runs within the trailer callback */

typedef struct {
	ArvStream *stream;
	FakeUsbDevice *device;
	gint n_done;
	gint n_bad;
} RecycleState;

static void
_recycle_callback (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer)
{
	RecycleState *state = user_data;
	ArvStream *stream;
	ArvBuffer *popped;

	if (type != ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE)
		return;

	stream = g_atomic_pointer_get (&state->stream);
	if (stream == NULL)
		return;

	popped = arv_stream_try_pop_buffer (stream);
	if (popped == NULL)
		return;

	if (!fakeuv_buffer_matches_frame (state->device, popped))
		g_atomic_int_inc (&state->n_bad);
	g_atomic_int_inc (&state->n_done);

	arv_stream_push_buffer (stream, popped);
}

/* Waits until @count reaches @target, failing only when it does not go up for @stall_ms (scaled): a test program that
 * shares a CPU with others gets there late, but gets there */

static gboolean
_wait_for_count (gint *count, gint target, guint stall_ms)
{
	gint64 stall_us = (gint64) fakeuv_timeout_ms (stall_ms) * G_TIME_SPAN_MILLISECOND;
	gint64 stall_end_time = g_get_monotonic_time () + stall_us;
	gint last = g_atomic_int_get (count);

	while (last < target) {
		gint current;

		g_usleep (1000);

		current = g_atomic_int_get (count);
		if (current != last) {
			last = current;
			stall_end_time = g_get_monotonic_time () + stall_us;
		} else if (g_get_monotonic_time () >= stall_end_time) {
			return FALSE;
		}
	}

	return TRUE;
}

static void
recycle_in_callback_test (void)
{
	RecycleState state = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i;

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 512, 512, ARV_PIXEL_FORMAT_MONO_8);
	state.device = device;

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, _recycle_callback, &state, NULL, &error);
	g_assert_no_error (error);
	g_atomic_pointer_set (&state.stream, stream);

	for (i = 0; i < 2; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	/* 50 frames, at 5 ms each when the CPU is free */
	g_assert_true (_wait_for_count (&state.n_done, 50, TIMEOUT_MS));

	/* The buffers aborted by the stop stay in the output queue */
	g_atomic_pointer_set (&state.stream, NULL);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_assert_cmpint (g_atomic_int_get (&state.n_bad), ==, 0);
	_assert_no_buffer_lost (stream, 2);

	g_object_unref (stream);
	g_clear_object (&camera);
}

/* arv_stream_get_n_owned_buffers () gave g_async_queue_length (), which subtracts the threads waiting on a queue: -1
 * input buffers while the stream thread waits for one, -1 output buffers while the application waits for one. Every
 * buffer is in one place: the input queue, the stream thread, or the output queue. */

static gpointer
_pop_thread (gpointer data)
{
	ArvStream *stream = data;

	return arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (300) * 1000);
}

static void
owned_buffers (ArvUvUsbMode usb_mode)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GThread *thread;
	GError *error = NULL;
	size_t payload;
	guint n_frames = 0;
	guint i;

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 512, 512, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	/* No buffer: in async mode, the stream thread waits on the input queue */
	stream = _start_stream (camera, usb_mode, NULL, NULL, 0);

	for (i = 0; i < 10; i++) {
		gint n_input = -1, n_output = -1, n_filling = -1;

		arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, &n_filling);
		g_assert_cmpint (n_input, ==, 0);
		g_assert_cmpint (n_output, ==, 0);
		g_assert_cmpint (n_filling, ==, 0);
		g_usleep (1000);
	}

	/* An application thread waits on the output queue */
	thread = g_thread_new ("pop", _pop_thread, stream);
	for (i = 0; i < 10; i++) {
		gint n_input = -1, n_output = -1, n_filling = -1;

		arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, &n_filling);
		g_assert_cmpint (n_input, ==, 0);
		g_assert_cmpint (n_output, ==, 0);
		g_assert_cmpint (n_filling, ==, 0);
		g_usleep (1000);
	}
	g_assert_null (g_thread_join (thread));

	/* Streaming, buffers pushed back at once: every buffer is accounted for at any time */
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	while (n_frames < 30) {
		gint n_input = -1, n_output = -1, n_filling = -1;
		ArvBuffer *buffer;

		arv_stream_get_n_owned_buffers (stream, &n_input, &n_output, &n_filling);
		g_assert_cmpint (n_input, >=, 0);
		g_assert_cmpint (n_output, >=, 0);
		g_assert_cmpint (n_filling, >=, 0);
		g_assert_cmpint (n_input + n_output + n_filling, ==, N_BUFFERS);

		buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);
		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		n_frames++;

		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	_assert_no_buffer_lost (stream, N_BUFFERS);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
owned_buffers_async_test (void)
{
	owned_buffers (ARV_UV_USB_MODE_ASYNC);
}

static void
owned_buffers_sync_test (void)
{
	owned_buffers (ARV_UV_USB_MODE_SYNC);
}

/* An application that replaces each buffer it gets back with a new one, during the acquisition, in async mode. The
 * transfers of a context, made once, read into the memory of the buffer the context was made for. Contexts were
 * found by ArvBuffer, so that a new buffer that got the address of a released one, which allocators do readily, used
 * its context: the camera wrote into freed memory, or into the memory of another buffer, while the new buffer came
 * back successful with whatever it held. And the contexts of the released buffers piled up, with their transfers. Each
 * frame must be in the buffer it comes back in, and the libusb transfers alive stay few. */

typedef enum {
	REPLACE_FRESH,		/* a new buffer, while another allocation gets the memory of the released one */
	REPLACE_WRAPPED		/* a new buffer around the next block of memory of a pool that outlives the buffers */
} ReplaceMode;

typedef struct {
	FakeUsbPlatform platform;
	gboolean raw_io;
	guint32 width;
	guint32 height;
	ReplaceMode mode;
} ReplaceCase;

static const ReplaceCase replace_fresh_case = { FAKE_USB_PLATFORM_LINUX, FALSE, 512, 512, REPLACE_FRESH };
static const ReplaceCase replace_wrapped_case = { FAKE_USB_PLATFORM_LINUX, FALSE, 512, 512, REPLACE_WRAPPED };
/* With RAW_IO, the end of a 720x540 payload goes through a bounce buffer, copied to its offset in the buffer */
static const ReplaceCase replace_fresh_raw_io_case = { FAKE_USB_PLATFORM_WINDOWS, TRUE, 720, 540, REPLACE_FRESH };
static const ReplaceCase replace_wrapped_raw_io_case = { FAKE_USB_PLATFORM_WINDOWS, TRUE, 720, 540, REPLACE_WRAPPED };

#define N_REPLACED_FRAMES	100
#define N_SPARE_ALLOCATIONS	3
/* Contexts alive at once, at most: about twice as many as the buffers in use, plus some to spare; one per frame, or
 * one per block of the pool, if those of the released buffers are kept */
#define MAX_CONTEXTS		40
#define N_BLOCKS		64

static ArvBuffer *
_new_replacement_buffer (const ReplaceCase *replace_case, size_t payload, guint8 **blocks, guint *next_block)
{
	guint8 *block;

	if (replace_case->mode == REPLACE_FRESH)
		return arv_buffer_new (payload, NULL);

	/* Nothing of an earlier frame in it */
	block = blocks[*next_block];
	*next_block = (*next_block + 1) % N_BLOCKS;
	memset (block, 0, payload);

	return arv_buffer_new (payload, block);
}

static void
replaced_buffers_test (gconstpointer data)
{
	const ReplaceCase *replace_case = data;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gpointer spare_allocations[N_SPARE_ALLOCATIONS] = { NULL };
	guint8 *blocks[N_BLOCKS] = { NULL };
	guint next_block = 0;
	guint n_transfers_max = 0;
	guint transfers_per_context;
	size_t payload;
	guint i;

	fakeuv_test_reset ();

	fake_usb_set_platform (replace_case->platform);
	g_setenv ("ARV_UV_RAW_IO", replace_case->raw_io ? "1" : "0", TRUE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, replace_case->width, replace_case->height, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	if (replace_case->mode == REPLACE_WRAPPED)
		for (i = 0; i < N_BLOCKS; i++)
			blocks[i] = g_malloc (payload);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, _new_replacement_buffer (replace_case, payload, blocks, &next_block));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
			 replace_case->raw_io && RAW_IO_AVAILABLE);

	/* The leader, payload and trailer transfers of a context */
	transfers_per_context = 3 + (payload - 1) / fake_usb_camera_get_sirm_register (device, ARV_SIRM_PAYLOAD_SIZE);

	for (i = 0; i < N_REPLACED_FRAMES; i++) {
		ArvBuffer *buffer = _pop_buffer (stream);

		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		g_object_unref (buffer);

		if (replace_case->mode == REPLACE_FRESH) {
			g_free (spare_allocations[i % N_SPARE_ALLOCATIONS]);
			spare_allocations[i % N_SPARE_ALLOCATIONS] = g_malloc (payload);
		}
		arv_stream_push_buffer (stream, _new_replacement_buffer (replace_case, payload, blocks, &next_block));

		n_transfers_max = MAX (n_transfers_max, fake_usb_get_n_transfers ());
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

	if (g_test_verbose ())
		g_printerr ("%u libusb transfers alive at most, %u per context\n", n_transfers_max,
			    transfers_per_context);
	g_assert_cmpuint (n_transfers_max, <=, MAX_CONTEXTS * transfers_per_context);

	g_object_unref (stream);
	g_clear_object (&camera);

	for (i = 0; i < N_BLOCKS; i++)
		g_free (blocks[i]);
	for (i = 0; i < N_SPARE_ALLOCATIONS; i++)
		g_free (spare_allocations[i]);

	g_unsetenv ("ARV_UV_RAW_IO");

	fakeuv_test_reset ();
}

/* The memory of a released buffer in a new buffer of another size, in async mode: the transfers of the context made
 * for the first buffer would read past the end of the second one. A 1440x1080 frame takes two payload transfers, and
 * the second buffer is 4 KiB short of the payload: its frame fails, and nothing is written past its end. */

#define SHORT_BYTES	4096
#define GUARD_BYTES	8192
#define GUARD_VALUE	0xa5

static void
resized_buffer (FakeUsbPlatform platform, gboolean raw_io)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	guint8 *block;
	size_t payload;
	gsize i;

	fake_usb_set_platform (platform);
	g_setenv ("ARV_UV_RAW_IO", raw_io ? "1" : "0", TRUE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 1440, 1080, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	block = g_malloc (payload + GUARD_BYTES);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
			 raw_io && RAW_IO_AVAILABLE);
	g_assert_cmpuint (fake_usb_camera_get_sirm_register (device, ARV_SIRM_PAYLOAD_COUNT), ==, 1);

	arv_stream_push_buffer (stream, arv_buffer_new (payload, block));
	buffer = _pop_buffer (stream);
	g_assert_nonnull (buffer);
	g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
	g_object_unref (buffer);

	memset (block + payload - SHORT_BYTES, GUARD_VALUE, SHORT_BYTES + GUARD_BYTES);

	arv_stream_push_buffer (stream, arv_buffer_new (payload - SHORT_BYTES, block));
	buffer = _pop_buffer (stream);
	g_assert_nonnull (buffer);
	g_assert_cmpint (arv_buffer_get_status (buffer), !=, ARV_BUFFER_STATUS_SUCCESS);
	g_object_unref (buffer);

	for (i = payload - SHORT_BYTES; i < payload + GUARD_BYTES; i++)
		if (block[i] != GUARD_VALUE)
			g_error ("Byte %" G_GSIZE_FORMAT " written past the end of a buffer of %" G_GSIZE_FORMAT
				 " bytes", i, payload - SHORT_BYTES);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);
	g_clear_object (&camera);

	g_free (block);

	g_unsetenv ("ARV_UV_RAW_IO");
}

static void
resized_buffer_test (void)
{
	resized_buffer (FAKE_USB_PLATFORM_LINUX, FALSE);
}

static void
resized_buffer_raw_io_test (void)
{
	resized_buffer (FAKE_USB_PLATFORM_WINDOWS, TRUE);
}

/* Two buffers sharing their memory, pushed one after the other, in async mode: the second one would get the context of
 * that memory, while the first one is filled through it. It comes back aborted at once instead, and the first one gets
 * its frame. The second one is filled later, once the memory is free. */

static void
shared_memory_test (void)
{
	FakeuvStreamEvents events = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *first, *second, *buffer;
	GError *error = NULL;
	guint8 *block;
	size_t payload;

	fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 512, 512, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	block = g_malloc (payload);
	first = arv_buffer_new (payload, block);
	second = arv_buffer_new (payload, block);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, fakeuv_stream_callback, &events, NULL, &error);
	g_assert_no_error (error);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	arv_stream_push_buffer (stream, g_object_ref (first));
	arv_stream_push_buffer (stream, g_object_ref (second));

	buffer = _pop_buffer (stream);
	g_assert_true (buffer == second);
	g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_ABORTED);
	g_object_unref (buffer);

	_trigger_frame (camera, device);
	buffer = _pop_buffer (stream);
	g_assert_true (buffer == first);
	g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
	g_object_unref (buffer);

	arv_stream_push_buffer (stream, g_object_ref (second));
	_trigger_frame (camera, device);
	buffer = _pop_buffer (stream);
	g_assert_true (buffer == second);
	g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
	g_object_unref (buffer);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_aborted"), ==, 1);
	g_assert_cmpint (g_atomic_int_get (&events.n_start_buffer), ==, 3);
	g_assert_cmpint (g_atomic_int_get (&events.n_buffer_done), ==, 3);

	g_object_unref (stream);
	g_clear_object (&camera);

	g_object_unref (first);
	g_object_unref (second);
	g_free (block);
}

/* Two streams of a camera, in async mode, as with an application that keeps the stream it has while it creates a new
 * one: the old stream starts again, without buffers, then the new one starts, and sets the stream interface up last.
 * The old stream, stopped or released, leaves the interface to the new one: it disabled streaming, and turned RAW_IO
 * off with the transfers of the new stream pending. The new stream undoes its setup when it stops. */

typedef struct {
	FakeUsbPlatform platform;
	gboolean raw_io;
	gboolean release_only;		/* the old stream is released while it acquires: finalization stops it */
} StaleStreamCase;

static const StaleStreamCase stale_stream_case = { FAKE_USB_PLATFORM_WINDOWS, TRUE, FALSE };
static const StaleStreamCase stale_stream_released_case = { FAKE_USB_PLATFORM_WINDOWS, TRUE, TRUE };
static const StaleStreamCase stale_stream_without_raw_io_case = { FAKE_USB_PLATFORM_LINUX, FALSE, FALSE };

static void
stale_stream_test (gconstpointer data)
{
	const StaleStreamCase *stale_case = data;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *old_stream;
	ArvStream *stream;
	GError *error = NULL;
	gboolean raw_io;
	size_t payload;
	guint i;

	fakeuv_test_reset ();

	fake_usb_set_platform (stale_case->platform);
	g_setenv ("ARV_UV_RAW_IO", stale_case->raw_io ? "1" : "0", TRUE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);

	old_stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (arv_stream_start_acquisition (old_stream, &error));
	g_assert_no_error (error);

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));
	g_assert_true (arv_stream_start_acquisition (stream, &error));
	g_assert_no_error (error);
	arv_camera_execute_command (camera, "AcquisitionStart", &error);
	g_assert_no_error (error);

	raw_io = fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);
	g_assert_cmpint (raw_io, ==, stale_case->raw_io && RAW_IO_AVAILABLE);

	_receive_good_frames (stream, device, N_BUFFERS);

	if (!stale_case->release_only) {
		g_assert_true (arv_stream_stop_acquisition (old_stream, &error));
		g_assert_no_error (error);
	}
	g_object_unref (old_stream);

	g_assert_true (fake_usb_camera_is_streaming (device));
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, raw_io);
	_receive_good_frames (stream, device, 2 * N_BUFFERS);

	arv_camera_execute_command (camera, "AcquisitionStop", &error);
	g_assert_no_error (error);
	g_assert_true (arv_stream_stop_acquisition (stream, &error));
	g_assert_no_error (error);
	g_assert_false (fake_usb_camera_is_streaming (device));
	g_assert_false (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

	g_object_unref (stream);
	g_clear_object (&camera);

	g_unsetenv ("ARV_UV_RAW_IO");

	fakeuv_test_reset ();
}

/* The stream interface registers the acquisition start reads: one it cannot read makes it fail, with the reason, before
 * it writes the camera a stream layout made of a value it never got. The start works once the register can be read. */

typedef struct {
	gint n_commands;
	gint failed_command;
} FailedCommand;

static void
_fail_command_hook (FakeUsbDevice *device, guint8 endpoint, int length, gboolean is_synchronous, gpointer user_data)
{
	FailedCommand *failed = user_data;

	/* The tries of a command, each one sent again */
	if (endpoint == FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT &&
	    g_atomic_int_add (&failed->n_commands, 1) + 1 == failed->failed_command)
		fake_usb_device_inject_error (device, FAKE_USB_CALL_BULK_TRANSFER, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT,
					      LIBUSB_ERROR_IO, 5);
}

static void
start_read_error_test (void)
{
	FailedCommand failed = { 0, 4 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	guint64 n_commands;
	size_t payload;
	guint i;

	fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 512, 512, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_ASYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	/* The camera refuses the first read, of the SBRM address */
	n_commands = fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS);
	fake_usb_camera_set_command_status (device, 1, ARV_UVCP_STATUS_INVALID_ADDRESS);
	g_assert_false (arv_stream_start_acquisition (stream, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR_INVALID_ADDRESS);
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS), ==, n_commands + 1);

	/* The fourth one, of the payload size, is never sent */
	n_commands = fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS);
	fake_usb_set_transfer_hook (_fail_command_hook, &failed);
	g_assert_false (arv_stream_start_acquisition (stream, &error));
	fake_usb_set_transfer_hook (NULL, NULL);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TRANSFER_ERROR);
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS), ==, n_commands + 3);

	/* Nothing written: the stream interface is as at power on */
	g_assert_cmpuint (fake_usb_camera_get_sirm_register (device, ARV_SIRM_PAYLOAD_SIZE), ==, 0);
	g_assert_false (fake_usb_camera_is_streaming (device));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	_receive_good_frames (stream, device, N_BUFFERS);
	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_object_unref (stream);
	g_clear_object (&camera);
}

/* Sync mode, a frame whose end is lost, trailer included: the next leader is read into the buffer still open, which
 * the stream gives back failed before it starts the next frame. That leader used to be parsed after the buffer was
 * handed over, from memory the application had freed or reused: a "new-buffer" handler here pops each failed buffer,
 * then releases it and pushes a new one, or clears it and pushes it back. Each successful buffer must hold the frame
 * its leader announced. */

#define N_LOST_TRAILERS	5

typedef struct {
	FakeUsbDevice *device;
	size_t payload;
	gboolean replace;
	gint n_good;
	gint n_failed;
	gint n_bad;
} LostTrailerState;

static void
_lost_trailer_new_buffer_cb (ArvStream *stream, LostTrailerState *state)
{
	ArvBuffer *buffer = arv_stream_try_pop_buffer (stream);

	if (buffer == NULL)
		return;

	if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS) {
		if (fakeuv_buffer_matches_frame (state->device, buffer))
			g_atomic_int_inc (&state->n_good);
		else
			g_atomic_int_inc (&state->n_bad);
		arv_stream_push_buffer (stream, buffer);
		return;
	}

	g_atomic_int_inc (&state->n_failed);

	if (state->replace) {
		g_object_unref (buffer);
		arv_stream_push_buffer (stream, arv_buffer_new (state->payload, NULL));
	} else {
		memset (buffer->priv->data, 0xa5, buffer->priv->allocated_size);
		arv_stream_push_buffer (stream, buffer);
	}
}

static void
lost_trailer (gboolean replace)
{
	LostTrailerState state = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	guint i;

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 640, 480, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	/* One frame at a time, so that each one has a buffer */
	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);

	state.device = device;
	state.replace = replace;
	state.payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_SYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	arv_stream_set_emit_signals (stream, TRUE);
	g_signal_connect (stream, "new-buffer", G_CALLBACK (_lost_trailer_new_buffer_cb), &state);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (state.payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	_trigger_frame (camera, device);
	g_assert_true (_wait_for_count (&state.n_good, 1, TIMEOUT_MS));

	for (i = 0; i < N_LOST_TRAILERS; i++) {
		gint n_good = g_atomic_int_get (&state.n_good);

		/* The frame lacks the end of its payload, and its trailer */
		fake_usb_camera_truncate_payloads (device, 1, 100000 + i * 513);
		fake_usb_camera_drop_trailers (device, 1);
		_trigger_frame (camera, device);

		/* The next leader closes it, and starts a good frame */
		_trigger_frame (camera, device);
		g_assert_true (_wait_for_count (&state.n_failed, i + 1, TIMEOUT_MS));
		g_assert_true (_wait_for_count (&state.n_good, n_good + 1, TIMEOUT_MS));
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	arv_stream_set_emit_signals (stream, FALSE);

	if (g_test_verbose ())
		g_printerr ("%d good, %d failed, %d bad\n", state.n_good, state.n_failed, state.n_bad);

	g_assert_cmpint (g_atomic_int_get (&state.n_bad), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&state.n_failed), ==, N_LOST_TRAILERS);
	g_assert_cmpint (g_atomic_int_get (&state.n_good), ==, 1 + N_LOST_TRAILERS);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
lost_trailer_replace_test (void)
{
	lost_trailer (TRUE);
}

/* Sync mode, a frame whose trailer only is lost: the read of the trailer gets the next leader, which does not fit in
 * it. With WinUSB partial reads, the leader read was cut short: its end went to the payload of the next frame, all of
 * whose reads were out of step, and the frame came back successful, its data out of place. A frame that starts with a
 * leader that filled its read fails now; on Linux, the read overflows, and the frame whose leader it was is lost. The
 * four frames of a round have a buffer each: two fail, or one, then two are good. */

#define N_LOST_TRAILER_FRAMES	4

static void
lost_trailer_only (FakeUsbPlatform platform)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i, j;

	fake_usb_set_platform (platform);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_SYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_LOST_TRAILER_FRAMES; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	for (i = 0; i < N_LOST_TRAILERS; i++) {
		guint n_good = 0, n_failed = 0;

		/* The camera starts a frame once the stream has read the previous one */
		fake_usb_camera_drop_trailers (device, 1);
		for (j = 0; j < N_LOST_TRAILER_FRAMES; j++)
			_trigger_frame (camera, device);

		while (n_good < 2) {
			ArvBuffer *buffer;

			buffer = _pop_buffer (stream);
			g_assert_nonnull (buffer);

			if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS) {
				g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
				n_good++;
			} else {
				n_failed++;
			}
			arv_stream_push_buffer (stream, buffer);
		}

		g_assert_cmpuint (n_failed, >=, 1);
		g_assert_cmpuint (n_failed, <=, 2);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
lost_trailer_only_windows_test (void)
{
	lost_trailer_only (FAKE_USB_PLATFORM_WINDOWS);
}

static void
lost_trailer_only_linux_test (void)
{
	lost_trailer_only (FAKE_USB_PLATFORM_LINUX);
}

static void
lost_trailer_reuse_test (void)
{
	lost_trailer (FALSE);
}

/* Sync mode, buffers a few bytes larger than the payload: the end of the buffer was read on its own, and got the start
 * of the trailer, typed from bytes past the end of the read when they were less than a packet header (Windows partial
 * reads), or the read overflowed: every frame failed. The trailer is read after the payload, whatever the buffer size.
 * One buffer of each size, each one filled several times. */

static void
larger_buffers (FakeUsbPlatform platform)
{
	static const size_t extra_bytes[] = { 1, 2, 3, 15, 27, 28, 100 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i;

	fake_usb_set_platform (platform);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_SYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < G_N_ELEMENTS (extra_bytes); i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload + extra_bytes[i], NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	for (i = 0; i < 3 * G_N_ELEMENTS (extra_bytes); i++) {
		ArvBuffer *buffer = _pop_buffer (stream);

		g_assert_nonnull (buffer);
		fakeuv_assert_frame (device, buffer, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
larger_buffers_windows_test (void)
{
	larger_buffers (FAKE_USB_PLATFORM_WINDOWS);
}

static void
larger_buffers_linux_test (void)
{
	larger_buffers (FAKE_USB_PLATFORM_LINUX);
}

/* Sync mode, frames cut short by a few bytes: the read of the rest of the payload gets the start of the trailer, less
 * than a packet header, which was typed from bytes past the end of the buffer (Windows partial reads), or the read
 * overflows, one packet at a time (Linux). The frame fails, alone: no other one fails for it, the stream reading whole
 * transfers up to the next leader, nor comes back successful with its data out of place. */

static void
cut_frames (FakeUsbPlatform platform)
{
	static const gsize missing_bytes[] = { 1, 2, 3, 100 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffer;
	GError *error = NULL;
	size_t payload;
	guint i, j;

	fake_usb_set_platform (platform);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	/* One frame at a time, so that each one has a buffer */
	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_SYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	for (i = 0; i < G_N_ELEMENTS (missing_bytes); i++) {
		/* The cut frame, then a frame whose leader ends it, whatever reads it is waiting for */
		fake_usb_camera_truncate_payloads (device, 1, missing_bytes[i]);
		_trigger_frame (camera, device);
		_trigger_frame (camera, device);

		buffer = _pop_buffer (stream);
		g_assert_nonnull (buffer);
		g_assert_cmpint (arv_buffer_get_status (buffer), !=, ARV_BUFFER_STATUS_SUCCESS);
		arv_stream_push_buffer (stream, buffer);

		for (j = 0; j < N_BUFFERS; j++) {
			if (j > 0)
				_trigger_frame (camera, device);
			buffer = _pop_buffer (stream);
			g_assert_nonnull (buffer);
			fakeuv_assert_frame (device, buffer, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
			arv_stream_push_buffer (stream, buffer);
		}
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, G_N_ELEMENTS (missing_bytes));

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
cut_frames_windows_test (void)
{
	cut_frames (FAKE_USB_PLATFORM_WINDOWS);
}

static void
cut_frames_linux_test (void)
{
	cut_frames (FAKE_USB_PLATFORM_LINUX);
}

/* Sync mode, a frame cut short by less than a packet header that loses its trailer too: the read of the rest of its
 * payload gets the start of the next leader (Windows partial reads), taken for the end of the payload, and the rest of
 * that leader and the next frame followed, up to the next trailer, which closed the frame: it came back successful,
 * with leader bytes at its end, and the next frame was lost. More bytes after the payload than the final transfer pads
 * fail the frame. On Linux, the read overflows. Each round: the cut frame, the next one, whose leader it gets, and a
 * good one. */

static void
cut_frames_lost_trailer (FakeUsbPlatform platform)
{
	static const gsize missing_bytes[] = { 2, 8, 15 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	guint i, j;

	fake_usb_set_platform (platform);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 720, 540, ARV_PIXEL_FORMAT_MONO_8);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	/* One frame at a time, so that each one has a buffer */
	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);

	stream = _start_stream (camera, ARV_UV_USB_MODE_SYNC, NULL, NULL, N_BUFFERS);

	_trigger_frame (camera, device);
	_receive_good_frames (stream, device, 1);

	for (i = 0; i < G_N_ELEMENTS (missing_bytes); i++) {
		guint n_good = 0, n_failed = 0;

		fake_usb_camera_truncate_payloads (device, 1, missing_bytes[i]);
		fake_usb_camera_drop_trailers (device, 1);
		for (j = 0; j < 3; j++)
			_trigger_frame (camera, device);

		while (n_good < 1) {
			ArvBuffer *buffer = _pop_buffer (stream);

			g_assert_nonnull (buffer);
			if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS) {
				g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
				n_good++;
			} else {
				n_failed++;
			}
			arv_stream_push_buffer (stream, buffer);
		}

		g_assert_cmpuint (n_failed, ==, 1);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_object_unref (stream);
	g_clear_object (&camera);
}

static void
cut_frames_lost_trailer_windows_test (void)
{
	cut_frames_lost_trailer (FAKE_USB_PLATFORM_WINDOWS);
}

static void
cut_frames_lost_trailer_linux_test (void)
{
	cut_frames_lost_trailer (FAKE_USB_PLATFORM_LINUX);
}

/* Sync mode, a camera that pads the final transfer of each frame to the aligned size the stream gives it (SIRM
 * TRANSFER1_SIZE): the padding comes after the payload, before the trailer, and does not fail the frame, in a buffer of
 * the payload size, or a larger one, which the padding used to go into, failing the frame. The final transfer ends
 * with a short packet, which keeps the trailer out of the read of the padding. Windows partial reads: on Linux, the
 * read of the end of the payload, which is not a whole number of packets, overflows. */

static void
padded_final_transfer_test (void)
{
	static const size_t extra_bytes[] = { 0, 1000 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i;

	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, 721, 541, ARV_PIXEL_FORMAT_MONO_8);
	/* 5 transfers of 65536 bytes, then 62381 bytes padded to 62400 */
	fake_usb_camera_set_alignment_shift (device, 6);
	fake_usb_camera_set_pad_final_transfer (device, TRUE);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_camera_uv_set_maximum_transfer_size (camera, 65536);
	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	arv_camera_uv_set_usb_mode (camera, ARV_UV_USB_MODE_SYNC);
	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	for (i = 0; i < G_N_ELEMENTS (extra_bytes); i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload + extra_bytes[i], NULL));

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_camera_get_sirm_register (device, ARV_SIRM_PAYLOAD_SIZE), ==, 65536);
	g_assert_cmpuint (fake_usb_camera_get_sirm_register (device, ARV_SIRM_PAYLOAD_COUNT), ==, 5);
	g_assert_cmpuint (fake_usb_camera_get_sirm_register (device, ARV_SIRM_TRANSFER1_SIZE), ==, 62400);

	for (i = 0; i < 3 * G_N_ELEMENTS (extra_bytes); i++) {
		ArvBuffer *buffer = _pop_buffer (stream);

		g_assert_nonnull (buffer);
		fakeuv_assert_frame (device, buffer, 721, 541, ARV_PIXEL_FORMAT_MONO_8);
		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);

	g_object_unref (stream);
	g_clear_object (&camera);
}

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvstream/trailer-race", trailer_race_test);
	fakeuv_test_add ("/fakeuvstream/recycle-in-callback", recycle_in_callback_test);
	fakeuv_test_add ("/fakeuvstream/owned-buffers-async", owned_buffers_async_test);
	fakeuv_test_add ("/fakeuvstream/owned-buffers-sync", owned_buffers_sync_test);
	g_test_add_data_func ("/fakeuvstream/replaced-buffers", &replace_fresh_case, replaced_buffers_test);
	g_test_add_data_func ("/fakeuvstream/replaced-buffers-raw-io", &replace_fresh_raw_io_case,
			      replaced_buffers_test);
	g_test_add_data_func ("/fakeuvstream/wrapped-buffers", &replace_wrapped_case, replaced_buffers_test);
	g_test_add_data_func ("/fakeuvstream/wrapped-buffers-raw-io", &replace_wrapped_raw_io_case,
			      replaced_buffers_test);
	fakeuv_test_add ("/fakeuvstream/resized-buffer", resized_buffer_test);
	fakeuv_test_add ("/fakeuvstream/resized-buffer-raw-io", resized_buffer_raw_io_test);
	fakeuv_test_add ("/fakeuvstream/shared-memory", shared_memory_test);
	g_test_add_data_func ("/fakeuvstream/stale-stream", &stale_stream_case, stale_stream_test);
	g_test_add_data_func ("/fakeuvstream/stale-stream-released", &stale_stream_released_case, stale_stream_test);
	g_test_add_data_func ("/fakeuvstream/stale-stream-without-raw-io", &stale_stream_without_raw_io_case,
			      stale_stream_test);
	fakeuv_test_add ("/fakeuvstream/start-read-error", start_read_error_test);
	fakeuv_test_add ("/fakeuvstream/sync-lost-trailer-replace", lost_trailer_replace_test);
	fakeuv_test_add ("/fakeuvstream/sync-lost-trailer-reuse", lost_trailer_reuse_test);
	fakeuv_test_add ("/fakeuvstream/sync-lost-trailer-only-windows", lost_trailer_only_windows_test);
	fakeuv_test_add ("/fakeuvstream/sync-lost-trailer-only-linux", lost_trailer_only_linux_test);
	fakeuv_test_add ("/fakeuvstream/sync-larger-buffers-windows", larger_buffers_windows_test);
	fakeuv_test_add ("/fakeuvstream/sync-larger-buffers-linux", larger_buffers_linux_test);
	fakeuv_test_add ("/fakeuvstream/sync-cut-frames-windows", cut_frames_windows_test);
	fakeuv_test_add ("/fakeuvstream/sync-cut-frames-linux", cut_frames_linux_test);
	fakeuv_test_add ("/fakeuvstream/sync-cut-frames-lost-trailer-windows", cut_frames_lost_trailer_windows_test);
	fakeuv_test_add ("/fakeuvstream/sync-cut-frames-lost-trailer-linux", cut_frames_lost_trailer_linux_test);
	fakeuv_test_add ("/fakeuvstream/sync-padded-final-transfer", padded_final_transfer_test);

	return fakeuv_test_run ();
}
