/* SPDX-License-Identifier:Unlicense */

/* WinUSB RAW_IO and the USB transfer size, against the fake libusb, which emulates RAW_IO
 * for its Windows preset with libusb 1.0.30 headers: once RAW_IO is on, WinUSB rejects any read that is not a whole
 * number of packets, or is larger than the RAW_IO maximum transfer size.
 *
 * Frames that are not a whole number of packets stream with RAW_IO, with the same bytes as without. The leader and
 * trailer reads are rounded up to whole packets while the camera registers keep their values, and the end of the
 * payload goes through a bounce buffer, copied before any GenDC parsing. RAW_IO is off unless ARV_UV_RAW_IO turns it
 * on, which these cases do: it is used in async mode only, never changed with transfers pending, and falls back when it
 * cannot be turned on, or when the libusb in use lacks its functions (an older libusb-1.0.dll on Windows). Reads out of
 * step with the camera transfers, which whole packet reads cannot recover from by themselves, are resynchronized,
 * frames of whole packets included, whose trailer shares a read with their payload. A read RAW_IO rejects, the device
 * being there, is not taken for a removal, while a removal still is: the acquisition goes on without RAW_IO, without
 * spinning. Other read failures with RAW_IO on are retried, and a pipe that a failure leaves halted, which WinUSB does
 * not reset with RAW_IO on, is reset. The transfer size is a multiple of the packet size and of the camera alignment,
 * capped at the RAW_IO maximum, which does not reduce the bytes in flight, and
 * arv_camera_uv_set_maximum_transfer_size () refuses sizes out of range.
 *
 * Built with older libusb headers (no RAW_IO), the same cases check that nothing changes without it. */

#include "fakeuvtest.h"
#include <arvbufferprivate.h>
#include <arvuvcpprivate.h>
#include <arvuvdeviceprivate.h>
#include <arvuvspprivate.h>
#include <string.h>

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
#define RAW_IO_AVAILABLE	TRUE
#else
#define RAW_IO_AVAILABLE	FALSE
#endif

#define N_BUFFERS		4
#define N_FRAMES		10
#define TIMEOUT_MS		FAKEUV_TIMEOUT_MS
/* A loss of control reported at once, before any one second delay of the device checks could have passed */
#define FAST_LOSS_TIMEOUT_MS	700

#define PACKET_SIZE		FAKE_USB_DEFAULT_MAX_PACKET_SIZE
/* As written to the camera, aligned to the 8 bytes of the fake's default alignment */
#define LEADER_SIZE		((sizeof (ArvUvspLeader) + 7) / 8 * 8)
#define TRAILER_SIZE		((sizeof (ArvUvspTrailer) + 7) / 8 * 8)

#define MAXIMUM_TRANSFER_SIZE_MAX	(64 * 1024 * 1024)

/* Every byte depends on its position only: all frames are the same, with RAW_IO or without */

static guint8
_pixel (gsize i)
{
	return (guint8) ((i * 7 + i / 1021) & 0xff);
}

static void
_fixed_pattern (ArvBuffer *buffer, void *fill_pattern_data, guint32 exposure_time_us, guint32 gain,
		ArvPixelFormat pixel_format)
{
	gsize size = (gsize) buffer->priv->parts[0].width * buffer->priv->parts[0].height;
	gsize i;

	size = MIN (size, buffer->priv->allocated_size);
	for (i = 0; i < size; i++)
		buffer->priv->data[i] = _pixel (i);
	buffer->priv->received_size = size;
}

static gboolean
_matches_pattern (const guint8 *data, gsize size)
{
	gsize i;

	for (i = 0; i < size; i++)
		if (data[i] != _pixel (i))
			return FALSE;

	return TRUE;
}

static FakeUsbDevice *
_add_camera (FakeUsbPlatform platform, guint32 width, guint32 height)
{
	FakeUsbDevice *device;

	/* Off by default: on for these cases */
	g_setenv ("ARV_UV_RAW_IO", "1", TRUE);

	fake_usb_set_platform (platform);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);

	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (device, width, height, ARV_PIXEL_FORMAT_MONO_8);
	arv_fake_camera_set_fill_pattern (fake_usb_camera_get_fake_camera (device), _fixed_pattern, NULL, NULL);

	return device;
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
_create_stream (ArvCamera *camera, FakeUsbDevice *device, ArvUvUsbMode usb_mode, ArvStreamCallback callback,
		void *callback_data)
{
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	guint i;

	/* The SIRM payload size, which counts a GenDC descriptor, unlike the GenICam PayloadSize */
	payload = fake_usb_camera_get_sirm_register (device, ARV_SIRM_REQ_PAYLOAD_SIZE);

	arv_camera_uv_set_usb_mode (camera, usb_mode);
	stream = arv_camera_create_stream (camera, callback, callback_data, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));

	for (i = 0; i < N_BUFFERS; i++) {
		ArvBuffer *buffer = arv_buffer_new (payload, NULL);

		memset (buffer->priv->data, 0, payload);
		arv_stream_push_buffer (stream, buffer);
	}

	return stream;
}

static guint32
_sirm (FakeUsbDevice *device, guint32 offset)
{
	return fake_usb_camera_get_sirm_register (device, offset);
}

typedef struct {
	gboolean raw_io_seen;		/* RAW_IO on, right after the acquisition start */
	guint64 n_set_raw_io;		/* libusb_endpoint_set_raw_io () calls, start to unref */
	guint64 n_unaligned_reads;	/* reads that are not a whole number of packets */
} Acquisition;

/* Acquires @n_frames frames of the fixed pattern, checks every one of them, and that RAW_IO is off afterwards */

static void
_acquire (ArvCamera *camera, FakeUsbDevice *device, ArvUvUsbMode usb_mode, guint n_frames, Acquisition *result)
{
	ArvStream *stream;
	GError *error = NULL;
	guint64 n_set_raw_io = fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO);
	guint64 n_unaligned_reads = fake_usb_device_get_stat (device, FAKE_USB_STAT_UNALIGNED_STREAM_READS);
	guint64 n_rejected = fake_usb_device_get_stat (device, FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS);
	gint x, y, width, height;
	guint i;

	arv_camera_get_region (camera, &x, &y, &width, &height, &error);
	g_assert_no_error (error);

	/* In sync mode, one frame per trigger: free running, the camera goes on while every buffer waits in the output
	 * queue, when this thread is slow to push them back, and the stream thread drops these frames, their bytes
	 * counted as ignored */
	if (usb_mode == ARV_UV_USB_MODE_SYNC) {
		arv_camera_set_trigger (camera, "Software", &error);
		g_assert_no_error (error);
	}

	stream = _create_stream (camera, device, usb_mode, NULL, NULL);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	/* Turned on before the stream thread starts */
	result->raw_io_seen = fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);

	for (i = 0; i < n_frames; i++) {
		ArvBuffer *buffer;
		const guint8 *data;
		size_t size = 0;

		if (usb_mode == ARV_UV_USB_MODE_SYNC) {
			arv_camera_software_trigger (camera, &error);
			g_assert_no_error (error);
		}

		buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);
		g_assert_nonnull (buffer);
		g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SUCCESS);

		/* What the camera sent for this frame, and the same bytes in every mode */
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		data = arv_buffer_get_image_data (buffer, &size);
		g_assert_cmpuint (size, ==, (gsize) width * height);
		g_assert_true (_matches_pattern (data, size));

		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	/* Turned off again once the stream thread is gone, never with transfers pending */
	g_assert_false (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_RAW_IO_CHANGES_WHILE_PENDING), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS), ==, n_rejected);

	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_completed_buffers"), >=, n_frames);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);
	/* Never out of step, never resynchronized */
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_ignored_bytes"), ==, 0);

	g_object_unref (stream);

	if (usb_mode == ARV_UV_USB_MODE_SYNC) {
		arv_camera_clear_triggers (camera, &error);
		g_assert_no_error (error);
	}

	result->n_set_raw_io = fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO) -
		n_set_raw_io;
	result->n_unaligned_reads = fake_usb_device_get_stat (device, FAKE_USB_STAT_UNALIGNED_STREAM_READS) -
		n_unaligned_reads;
}

/* Frames that are not a whole number of packets, as 720x540 and 1440x1080 Mono8: the same bytes with RAW_IO as with
 * ARV_UV_RAW_IO=0, no failed buffer */

static void
frames (guint32 width, guint32 height)
{
	Acquisition with_raw_io = {0}, without_raw_io = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;

	g_assert_cmpuint ((gsize) width * height % PACKET_SIZE, !=, 0);

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, width, height);
	camera = _open_camera (device);

	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &with_raw_io);
	g_assert_cmpint (with_raw_io.raw_io_seen, ==, RAW_IO_AVAILABLE);
	if (RAW_IO_AVAILABLE) {
		/* On at the start, off at the stop, every read a whole number of packets */
		g_assert_cmpuint (with_raw_io.n_set_raw_io, ==, 2);
		g_assert_cmpuint (with_raw_io.n_unaligned_reads, ==, 0);
	}

	g_setenv ("ARV_UV_RAW_IO", "0", TRUE);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &without_raw_io);
	g_setenv ("ARV_UV_RAW_IO", "1", TRUE);

	/* As before: no RAW_IO call at all, and the whole frame in a single read */
	g_assert_false (without_raw_io.raw_io_seen);
	g_assert_cmpuint (without_raw_io.n_set_raw_io, ==, 0);
	g_assert_cmpuint (without_raw_io.n_unaligned_reads, >, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO), ==,
			  RAW_IO_AVAILABLE ? 1 : 0);

	g_clear_object (&camera);
}

static void
frames_720x540_test (void)
{
	frames (720, 540);
}

static void
frames_1440x1080_test (void)
{
	frames (1440, 1080);
}

/* Read lengths on the data endpoint, through the fake's transfer hook */

typedef struct {
	GMutex mutex;
	GHashTable *lengths;
} Reads;

static void
_transfer_hook (FakeUsbDevice *device, guint8 endpoint, int length, gboolean is_synchronous, gpointer user_data)
{
	Reads *reads = user_data;

	if (endpoint != FAKE_USB_CAMERA_DATA_ENDPOINT_IN)
		return;

	g_mutex_lock (&reads->mutex);
	g_hash_table_add (reads->lengths, GINT_TO_POINTER (length));
	g_mutex_unlock (&reads->mutex);
}

static gboolean
_reads_are (Reads *reads, guint n_lengths, ...)
{
	gboolean result;
	va_list args;
	guint i;

	g_mutex_lock (&reads->mutex);

	result = g_hash_table_size (reads->lengths) == n_lengths;

	va_start (args, n_lengths);
	for (i = 0; i < n_lengths; i++) {
		int length = va_arg (args, int);

		if (!g_hash_table_contains (reads->lengths, GINT_TO_POINTER (length))) {
			g_printerr ("No read of %d bytes\n", length);
			result = FALSE;
		}
	}
	va_end (args);

	if (!result) {
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init (&iter, reads->lengths);
		while (g_hash_table_iter_next (&iter, &key, NULL))
			g_printerr ("Read of %d bytes\n", GPOINTER_TO_INT (key));
	}

	g_hash_table_remove_all (reads->lengths);

	g_mutex_unlock (&reads->mutex);

	return result;
}

/* With RAW_IO, the leader and trailer reads are rounded up to a packet, and the end of the payload is read on its
 * own, through a bounce buffer; the camera is given the same leader and trailer sizes as without RAW_IO */

static void
read_sizes_test (void)
{
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	Reads reads;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);

	g_mutex_init (&reads.mutex);
	reads.lengths = g_hash_table_new (g_direct_hash, g_direct_equal);
	fake_usb_set_transfer_hook (_transfer_hook, &reads);

	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);

	g_assert_cmpuint (_sirm (device, ARV_SIRM_MAX_LEADER_SIZE), ==, LEADER_SIZE);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_MAX_TRAILER_SIZE), ==, TRAILER_SIZE);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_TRANSFER2_SIZE), ==, 0);

	if (RAW_IO_AVAILABLE) {
		/* 720 x 540 = 379 packets + 704 bytes */
		g_assert_true (_reads_are (&reads, 2, PACKET_SIZE, 379 * PACKET_SIZE));
		g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 379 * PACKET_SIZE);
		g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_COUNT), ==, 1);
		g_assert_cmpuint (_sirm (device, ARV_SIRM_TRANSFER1_SIZE), ==, 704);
	}

	/* Without RAW_IO, as before */
	g_setenv ("ARV_UV_RAW_IO", "off", TRUE);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);
	g_setenv ("ARV_UV_RAW_IO", "1", TRUE);

	g_assert_true (_reads_are (&reads, 3, (int) LEADER_SIZE, 720 * 540, (int) TRAILER_SIZE));
	g_assert_cmpuint (_sirm (device, ARV_SIRM_MAX_LEADER_SIZE), ==, LEADER_SIZE);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_MAX_TRAILER_SIZE), ==, TRAILER_SIZE);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_COUNT), ==, 1);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_TRANSFER1_SIZE), ==, 0);

	fake_usb_set_transfer_hook (NULL, NULL);
	g_hash_table_unref (reads.lengths);
	g_mutex_clear (&reads.mutex);

	g_clear_object (&camera);
}

/* Sync mode has a single read in flight: no RAW_IO, nothing changes */

static void
sync_mode_test (void)
{
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);

	_acquire (camera, device, ARV_UV_USB_MODE_SYNC, N_FRAMES, &acquisition);

	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_GET_MAX_RAW_IO_TRANSFER_SIZE), ==, 0);
	g_assert_cmpuint (acquisition.n_unaligned_reads, >, 0);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);

	g_clear_object (&camera);
}

/* RAW_IO cannot be turned on, or is not there: streaming goes on without it */

static void
fallback_test (void)
{
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);

	/* Turning it on fails: no RAW_IO, nothing to turn off at the stop. The reads stay whole packets, which needs no
	 * RAW_IO. */
	fake_usb_device_inject_error (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
				      LIBUSB_ERROR_OTHER, 1);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	if (RAW_IO_AVAILABLE) {
		g_assert_cmpuint (acquisition.n_set_raw_io, ==, 1);
		g_assert_cmpuint (acquisition.n_unaligned_reads, ==, 0);
		g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 379 * PACKET_SIZE);
	}

	/* Then fine again */
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);
	g_assert_cmpint (acquisition.raw_io_seen, ==, RAW_IO_AVAILABLE);

	/* The support query fails, the maximum transfer size query fails, the driver has no RAW_IO: as before */
	fake_usb_device_inject_error (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
				      LIBUSB_ERROR_NOT_FOUND, 1);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);

	fake_usb_device_inject_error (device, FAKE_USB_CALL_GET_MAX_RAW_IO_TRANSFER_SIZE,
				      FAKE_USB_CAMERA_DATA_ENDPOINT_IN, LIBUSB_ERROR_OTHER, 1);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);

	/* The packet size is unknown: no RAW_IO either, and a transfer size that is a multiple of the alignment only */
	fake_usb_device_inject_error (device, FAKE_USB_CALL_GET_MAX_PACKET_SIZE, -1, LIBUSB_ERROR_OTHER, 1);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);

	fake_usb_device_set_raw_io_supported (device, FALSE);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);

	g_clear_object (&camera);
}

/* ARV_UV_RAW_IO: "1", "on", "true", "yes" or "auto" uses RAW_IO where it is available, whatever the case. Off by
 * default: unset, empty or unknown. "0", "off", "false" or "no" turn it off too, without even asking libusb whether
 * the endpoint supports it, which the default does, for the log. */

static void
environment_test (void)
{
	static const char *default_values[] = { NULL, "", "unknown" };
	static const char *off_values[] = { "0", "off", "OFF", "false", "No" };
	static const char *on_values[] = { "1", "on", "auto", "Auto", "true", "yes" };
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	guint i;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);

	for (i = 0; i < G_N_ELEMENTS (default_values); i++) {
		if (default_values[i] != NULL)
			g_setenv ("ARV_UV_RAW_IO", default_values[i], TRUE);
		else
			g_unsetenv ("ARV_UV_RAW_IO");
		_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 2, &acquisition);
		g_assert_false (acquisition.raw_io_seen);
		g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
		/* As without RAW_IO: the whole 720x540 frame in a single read */
		g_assert_cmpuint (acquisition.n_unaligned_reads, >, 0);
	}

	for (i = 0; i < G_N_ELEMENTS (off_values); i++) {
		guint64 n_supports = fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO);

		g_setenv ("ARV_UV_RAW_IO", off_values[i], TRUE);
		_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 2, &acquisition);
		g_assert_false (acquisition.raw_io_seen);
		g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
		g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO), ==,
				  n_supports);
	}

	for (i = 0; i < G_N_ELEMENTS (on_values); i++) {
		g_setenv ("ARV_UV_RAW_IO", on_values[i], TRUE);
		_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 2, &acquisition);
		g_assert_cmpint (acquisition.raw_io_seen, ==, RAW_IO_AVAILABLE);
	}

	g_clear_object (&camera);
}

/* The transfer size: a multiple of the packet size and of the camera alignment, not larger than the maximum transfer
 * size, nor than the RAW_IO maximum when RAW_IO is on. Written to SIRM_PAYLOAD_SIZE when the payload is larger. */

typedef struct {
	FakeUsbPlatform platform;
	guint32 width;
	guint32 height;
	guint64 maximum_transfer_size;	/* 0: the default, 1 MiB */
	int raw_io_maximum;		/* 0: the fake's default, 2 MiB */
	guint alignment_shift;		/* 0: the fake's default, 8 bytes */
	guint16 packet_size;		/* 0: the fake's default, 1024 bytes */
	guint32 payload_size;		/* expected SIRM_PAYLOAD_SIZE, with RAW_IO where it is available */
	guint32 payload_size_without_raw_io;
} TransferSizeCase;

/* The RAW_IO maximum, below the maximum transfer size, caps the transfers: 300 packets */
static const TransferSizeCase raw_io_cap_case = {
	FAKE_USB_PLATFORM_WINDOWS, 1440, 1080, 0, 300 * 1024, 0, 0, 300 * 1024, 1024 * 1024
};
/* ... rounded down to whole packets: 976 */
static const TransferSizeCase raw_io_cap_packets_case = {
	FAKE_USB_PLATFORM_WINDOWS, 1440, 1080, 0, 1000000, 0, 0, 976 * 1024, 1024 * 1024
};
/* Without RAW_IO, whole packets too: 977 (1001000 before) */
static const TransferSizeCase packets_case = {
	FAKE_USB_PLATFORM_LINUX, 1440, 1080, 1001000, 0, 0, 0, 977 * 1024, 977 * 1024
};
/* A camera alignment larger than a packet: 488 x 2048 bytes */
static const TransferSizeCase alignment_case = {
	FAKE_USB_PLATFORM_LINUX, 1440, 1080, 1001000, 0, 11, 0, 488 * 2048, 488 * 2048
};
/* USB 2 packets of 512 bytes: 1955 */
static const TransferSizeCase usb2_case = {
	FAKE_USB_PLATFORM_LINUX, 1440, 1080, 1001000, 0, 0, 512, 1955 * 512, 1955 * 512
};
/* ... with RAW_IO: a payload of 759 packets of 512 bytes, then the remaining 192 bytes */
static const TransferSizeCase usb2_raw_io_case = {
	FAKE_USB_PLATFORM_WINDOWS, 720, 540, 0, 0, 0, 512, 759 * 512, 720 * 540
};

static void
transfer_size_test (gconstpointer data)
{
	const TransferSizeCase *size_case = data;
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	guint32 payload_size;
	guint32 alignment;
	guint32 packet_size;
	guint64 payload;

	fakeuv_test_reset ();

	device = _add_camera (size_case->platform, size_case->width, size_case->height);
	if (size_case->raw_io_maximum > 0)
		fake_usb_device_set_max_raw_io_transfer_size (device, size_case->raw_io_maximum);
	if (size_case->alignment_shift > 0)
		fake_usb_camera_set_alignment_shift (device, size_case->alignment_shift);
	if (size_case->packet_size > 0)
		fake_usb_device_set_max_packet_size (device, 0, size_case->packet_size);

	alignment = 1 << (size_case->alignment_shift > 0 ? size_case->alignment_shift :
			  FAKE_USB_CAMERA_DEFAULT_ALIGNMENT_SHIFT);
	packet_size = size_case->packet_size > 0 ? size_case->packet_size : PACKET_SIZE;
	payload = (guint64) size_case->width * size_case->height;

	camera = _open_camera (device);
	if (size_case->maximum_transfer_size > 0)
		arv_camera_uv_set_maximum_transfer_size (camera, size_case->maximum_transfer_size);

	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);

	/* RAW_IO wherever the fake offers it */
	g_assert_cmpint (acquisition.raw_io_seen, ==,
			 RAW_IO_AVAILABLE && size_case->platform == FAKE_USB_PLATFORM_WINDOWS);

	payload_size = _sirm (device, ARV_SIRM_PAYLOAD_SIZE);
	g_assert_cmpuint (payload_size, ==, acquisition.raw_io_seen ? size_case->payload_size :
			  size_case->payload_size_without_raw_io);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_COUNT), ==, payload / payload_size);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_TRANSFER1_SIZE), ==,
			  (payload % payload_size + alignment - 1) / alignment * alignment);

	if (acquisition.raw_io_seen) {
		/* Whole packets and whole alignment units, no larger than the RAW_IO maximum */
		g_assert_cmpuint (payload_size % packet_size, ==, 0);
		g_assert_cmpuint (payload_size % alignment, ==, 0);
		g_assert_cmpuint (acquisition.n_unaligned_reads, ==, 0);
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_MAX_STREAM_READ), <=,
				  size_case->raw_io_maximum > 0 ? (guint) size_case->raw_io_maximum :
				  FAKE_USB_DEFAULT_MAX_RAW_IO_TRANSFER_SIZE);
	} else if (payload > payload_size) {
		/* The transfer size itself */
		g_assert_cmpuint (payload_size % packet_size, ==, 0);
		g_assert_cmpuint (payload_size % alignment, ==, 0);
	}
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_MAX_STREAM_READ), <=,
			  size_case->maximum_transfer_size > 0 ? size_case->maximum_transfer_size : 1024 * 1024);

	g_clear_object (&camera);

	fakeuv_test_reset ();
}

/* Starts and stops an acquisition (sync mode, so that the payload transfer is the transfer size) */

static void
_start_stop (ArvCamera *camera, FakeUsbDevice *device)
{
	ArvStream *stream;
	GError *error = NULL;

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_SYNC, NULL, NULL);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_object_unref (stream);
}

/* arv_camera_uv_set_maximum_transfer_size (): 0 selects the default, sizes out of range are refused with a warning */

static void
setter_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;

	device = _add_camera (FAKE_USB_PLATFORM_LINUX, 1440, 1080);
	camera = _open_camera (device);

	_start_stop (camera, device);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 1024 * 1024);

	arv_camera_uv_set_maximum_transfer_size (camera, 256 * 1024);
	_start_stop (camera, device);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 256 * 1024);

	/* Refused, the previous size kept */
	g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*Invalid USB maximum transfer size 1023*");
	arv_camera_uv_set_maximum_transfer_size (camera, 1023);
	g_test_assert_expected_messages ();
	g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*Invalid USB maximum transfer size 67108865*");
	arv_camera_uv_set_maximum_transfer_size (camera, MAXIMUM_TRANSFER_SIZE_MAX + 1);
	g_test_assert_expected_messages ();
	g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*Invalid USB maximum transfer size*");
	arv_camera_uv_set_maximum_transfer_size (camera, G_MAXUINT64);
	g_test_assert_expected_messages ();

	_start_stop (camera, device);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 256 * 1024);

	/* 0: the default */
	arv_camera_uv_set_maximum_transfer_size (camera, 0);
	_start_stop (camera, device);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 1024 * 1024);

	/* The bounds are accepted */
	arv_camera_uv_set_maximum_transfer_size (camera, 1024);
	_start_stop (camera, device);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 1024);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_COUNT), ==, 1440 * 1080 / 1024);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_TRANSFER1_SIZE), ==, 1440 * 1080 % 1024);

	arv_camera_uv_set_maximum_transfer_size (camera, MAXIMUM_TRANSFER_SIZE_MAX);
	_start_stop (camera, device);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 1440 * 1080);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_COUNT), ==, 1);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_TRANSFER1_SIZE), ==, 0);

	g_clear_object (&camera);
}

/* GenDC containers with RAW_IO: the bounced end of the payload is in the buffer before the descriptor is parsed. The
 * buffers are cleared before each fill, so that a descriptor parsed too early is never the previous frame's. */

static void
gendc (guint32 width, guint32 height)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gsize descriptor_size = fake_usb_camera_get_gendc_descriptor_size ();
	gsize image_size = (gsize) width * height;
	gsize payload;
	guint i;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, width, height);
	fake_usb_camera_set_payload_type (device, ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER);
	camera = _open_camera (device);

	payload = fake_usb_camera_get_sirm_register (device, ARV_SIRM_REQ_PAYLOAD_SIZE);
	g_assert_cmpuint (payload, ==, image_size + descriptor_size);
	g_assert_cmpuint (payload % PACKET_SIZE, !=, 0);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, NULL, NULL);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, RAW_IO_AVAILABLE);

	for (i = 0; i < 2 * N_BUFFERS; i++) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) *
								    1000);
		const guint8 *data;
		const guint8 *image;
		size_t size = 0;

		g_assert_nonnull (buffer);
		g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_SUCCESS);
		g_assert_cmpint (arv_buffer_get_payload_type (buffer), ==, ARV_BUFFER_PAYLOAD_TYPE_GENDC_CONTAINER);
		g_assert_true (arv_buffer_has_gendc (buffer));

		data = arv_buffer_get_gendc_descriptor (buffer, &size);
		g_assert_nonnull (data);
		g_assert_cmpuint (size, ==, descriptor_size);

		image = arv_buffer_get_image_data (buffer, &size);
		g_assert_true (image == data + descriptor_size);
		g_assert_cmpuint (size, ==, image_size);
		g_assert_cmpint (arv_buffer_get_image_width (buffer), ==, width);
		g_assert_cmpint (arv_buffer_get_image_height (buffer), ==, height);
		g_assert_true (_matches_pattern (image, size));
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));

		memset (buffer->priv->data, 0, buffer->priv->allocated_size);
		arv_stream_push_buffer (stream, buffer);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);
	g_object_unref (stream);

	if (RAW_IO_AVAILABLE)
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_UNALIGNED_STREAM_READS), ==, 0);

	g_clear_object (&camera);
}

/* A container smaller than a packet: a single read, through the bounce buffer, descriptor included */

static void
gendc_small_test (void)
{
	gendc (16, 8);
}

/* A larger one: the descriptor comes in the first read, the end of the image through the bounce buffer */

static void
gendc_test (void)
{
	gendc (100, 100);
}

static void
_control_lost_cb (ArvDevice *device, gint *n_control_lost)
{
	g_atomic_int_inc (n_control_lost);
}

static guint64
_n_stream_calls (FakeUsbDevice *device)
{
	return fake_usb_device_get_call_count (device, FAKE_USB_CALL_SUBMIT_TRANSFER) +
		fake_usb_device_get_call_count (device, FAKE_USB_CALL_BULK_TRANSFER) +
		fake_usb_device_get_call_count (device, FAKE_USB_CALL_CONTROL_TRANSFER) +
		fake_usb_device_get_call_count (device, FAKE_USB_CALL_CANCEL_TRANSFER);
}

/* RAW_IO rejects the reads, the device being there (the RAW_IO maximum shrinks under Aravis's feet). On Windows, a
 * removed device gives the same LIBUSB_ERROR_IO at submission, which Aravis used to retry forever: the device check
 * tells the two apart. No loss of control: the buffer whose read was rejected comes back aborted, those in flight
 * failed, and the acquisition goes on without RAW_IO, which is turned off with nothing pending. The stream thread
 * neither hangs nor spins, and the next acquisition goes without RAW_IO too. */

static void
rejected_read_test (void)
{
	FakeuvStreamEvents events = {0};
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint n_control_lost = 0;
	guint64 n_control_requests, n_calls, n_supports;
	gint64 start_time, end_time, elapsed_ms;
	guint n_aborted = 0, n_other = 0, n_good_after = 0;
	guint i;

	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return;
	}

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb),
			  &n_control_lost);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, fakeuv_stream_callback, &events);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	for (i = 0; i < 3; i++) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) *
								    1000);

		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		arv_stream_push_buffer (stream, buffer);
	}

	n_control_requests = fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS);
	fake_usb_device_set_max_raw_io_transfer_size (device, 4 * PACKET_SIZE);

	/* Then good frames again, the application pushing the buffers back at once */
	start_time = g_get_monotonic_time ();
	end_time = start_time + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	n_calls = _n_stream_calls (device);
	while (n_good_after < 2 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_ABORTED)
			n_aborted++;
		else if (!fakeuv_buffer_matches_frame (device, buffer))
			n_other++;
		else if (n_aborted > 0)
			n_good_after++;
		arv_stream_push_buffer (stream, buffer);
	}
	elapsed_ms = (g_get_monotonic_time () - start_time) / G_TIME_SPAN_MILLISECOND;

	if (g_test_verbose ())
		g_printerr ("%u aborted, %u other, %u good after, %" G_GUINT64_FORMAT " libusb calls in %"
			    G_GINT64_FORMAT " ms\n", n_aborted, n_other, n_good_after, _n_stream_calls (device) - n_calls,
			    elapsed_ms);

	/* One read rejected, its buffer aborted, then frames without RAW_IO */
	g_assert_cmpuint (n_aborted, ==, 1);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS), ==, 1);
	g_assert_cmpuint (n_good_after, ==, 2 * N_BUFFERS);
	g_assert_false (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	/* A few calls per buffer: a spinning thread makes thousands */
	g_assert_cmpuint (_n_stream_calls (device) - n_calls, <=, 40 + 10 * (2 * N_BUFFERS + n_other));

	/* The device was checked, found, and kept */
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS), >, n_control_requests);
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_false (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_aborted"), >=, n_aborted);
	g_object_unref (stream);

	/* RAW_IO is not used again on this device */
	n_supports = fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO), ==,
			  n_supports);
	g_clear_object (&camera);

	/* A camera opened again tries again */
	fake_usb_device_set_max_raw_io_transfer_size (device, FAKE_USB_DEFAULT_MAX_RAW_IO_TRANSFER_SIZE);
	camera = _open_camera (device);
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);
	g_assert_true (acquisition.raw_io_seen);
	g_clear_object (&camera);

	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 0);
}

/* A frame shorter than announced, as when the camera drops the end of a frame: the following reads are out of step
 * with the camera transfers. With RAW_IO, they would stay so, the payload transfers being whole packets that nothing
 * ends, so that every frame failed from then on: the stream thread resynchronizes, and frames are good again after a
 * few failed ones. Without RAW_IO, the reads fall back into step by themselves.
 *
 * Frames of whole packets, as 640x480 and 1920x1080 Mono8, have no short payload transfer: the trailer, a short
 * packet, ends the read that gets the end of the payload, and no read starts with it, unless the payload is a whole
 * number of reads, as 1024x1024 */

static void
resync (gboolean raw_io, guint32 width, guint32 height)
{
	FakeuvStreamEvents events = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint64 end_time;
	guint n_good_before = 0, n_good_after = 0, n_bad = 0;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, width, height);
	camera = _open_camera (device);

	g_setenv ("ARV_UV_RAW_IO", raw_io ? "1" : "0", TRUE);
	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, fakeuv_stream_callback, &events);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
			 raw_io && RAW_IO_AVAILABLE);

	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (n_good_after < 2 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		if (!fakeuv_buffer_matches_frame (device, buffer)) {
			/* Not the frame the camera sent: never reported complete */
			g_assert_cmpint (arv_buffer_get_status (buffer), !=, ARV_BUFFER_STATUS_SUCCESS);
			n_bad++;
		} else if (n_bad > 0) {
			n_good_after++;
		} else if (++n_good_before == 3) {
			/* The next frame the camera sends lacks one packet */
			fake_usb_camera_truncate_payloads (device, 1, PACKET_SIZE);
		}

		arv_stream_push_buffer (stream, buffer);
	}

	if (g_test_verbose ())
		g_printerr ("%u bad frame(s), %" G_GUINT64_FORMAT " bytes ignored\n", n_bad,
			    arv_stream_get_info_uint64_by_name (stream, "n_ignored_bytes"));

	g_assert_cmpuint (n_bad, >=, 1);
	g_assert_cmpuint (n_good_after, ==, 2 * N_BUFFERS);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);
	/* Frames lost, not aborted */
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_aborted"), ==, 0);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), >=, n_bad);
	/* What a resynchronization reads is a few frames at most */
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_ignored_bytes"), <=,
			  (guint64) (2 * N_BUFFERS) * (width * height + 2 * PACKET_SIZE));

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	g_clear_object (&camera);
}

static void
resync_test (void)
{
	resync (TRUE, 720, 540);
}

static void
resync_640x480_test (void)
{
	resync (TRUE, 640, 480);
}

static void
resync_1920x1080_test (void)
{
	resync (TRUE, 1920, 1080);
}

static void
resync_1024x1024_test (void)
{
	resync (TRUE, 1024, 1024);
}

static void
resync_without_raw_io_test (void)
{
	resync (FALSE, 720, 540);
}

static void
resync_without_raw_io_640x480_test (void)
{
	resync (FALSE, 640, 480);
}

/* Frames whose end the resynchronization cannot find: with 4 byte packets on the data endpoint, the leader and the
 * trailer the fake camera sends are whole packets too, so that no read ever ends with a short packet. The stream
 * thread gives up after a couple of frames' worth of data, and streaming goes on: buffers keep coming back, failed,
 * instead of none at all. With a camera slower than one read per read timeout, the reads time out with the data they
 * got, which counts too: the limit used to be reached only by reads that completed. */

static void
resync_limit (guint64 frame_period_us)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint64 end_time;
	guint n_returned = 0;

	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return;
	}

	g_assert_cmpuint (sizeof (ArvUvspLeader) % 4, ==, 0);
	g_assert_cmpuint (sizeof (ArvUvspTrailer) % 4, ==, 0);

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	fake_usb_device_set_max_packet_size (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 4);
	fake_usb_camera_set_frame_period (device, frame_period_us);
	camera = _open_camera (device);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, NULL, NULL);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	/* Out of step from the first frame: each resynchronization takes the buffers in flight back */
	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (n_returned < 3 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		n_returned++;
		arv_stream_push_buffer (stream, buffer);
	}

	g_assert_cmpuint (n_returned, ==, 3 * N_BUFFERS);
	g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_ignored_bytes"), >, 0);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	g_clear_object (&camera);
}

static void
resync_limit_test (void)
{
	resync_limit (FAKEUV_FRAME_PERIOD_US);
}

/* A 720x540 frame every 50 ms: less than one 1 MiB read per 100 ms read timeout */

static void
resync_limit_slow_test (void)
{
	resync_limit (50000);
}

/* A camera that ends its transfers of whole packets with a zero length packet, which the next read gets: with RAW_IO,
 * the reads of every frame get out of step. The buffers keep coming back, and the next acquisition goes without RAW_IO,
 * which reads the 720x540 payload in a single transfer ending with a short packet, as before. With frames of whole
 * packets, as 640x480, the trailer read gets the zero length packet: nothing to parse, its buffer holds an earlier
 * packet, or nothing yet. Whole packet frames fail without RAW_IO too with such a camera, as before. */

static void
zero_length_packets (guint32 width, guint32 height)
{
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint64 end_time;
	guint n_returned = 0;
	guint n_success = 0;

	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return;
	}

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, width, height);
	fake_usb_camera_set_zero_length_packets (device, TRUE);
	camera = _open_camera (device);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, NULL, NULL);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (n_returned < 2 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		n_returned++;
		if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS) {
			/* Never with a trailer that was not read */
			g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
			n_success++;
		}
		arv_stream_push_buffer (stream, buffer);
	}

	if (g_test_verbose ())
		g_printerr ("%u buffers back, %u successful\n", n_returned, n_success);

	g_assert_cmpuint (n_returned, ==, 2 * N_BUFFERS);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_ZERO_LENGTH_PACKETS), >, 0);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	if ((gsize) width * height % PACKET_SIZE != 0) {
		_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
		g_assert_false (acquisition.raw_io_seen);
		g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	} else {
		guint64 n_set_raw_io = fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO);

		stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, NULL, NULL);
		arv_camera_start_acquisition (camera, &error);
		g_assert_no_error (error);
		g_assert_false (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
		arv_camera_stop_acquisition (camera, &error);
		g_assert_no_error (error);
		g_object_unref (stream);
		g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO), ==,
				  n_set_raw_io);
	}

	g_clear_object (&camera);
}

static void
zero_length_packets_test (void)
{
	zero_length_packets (720, 540);
}

static void
zero_length_packets_640x480_test (void)
{
	zero_length_packets (640, 480);
}

typedef struct {
	gint *n_control_lost;
	FakeuvStreamEvents *events;
} LossState;

static gboolean
_lost_and_exited (gpointer user_data)
{
	LossState *state = user_data;

	return g_atomic_int_get (state->n_control_lost) > 0 && g_atomic_int_get (&state->events->n_exit) > 0;
}

/* The camera removed with RAW_IO on (Windows, no hotplug): submissions fail with LIBUSB_ERROR_IO, as a rejected read
 * does, but the device check fails too. Control lost at once, the stream thread gone, RAW_IO left alone. Windows builds
 * only: elsewhere, a check that fails with LIBUSB_ERROR_IO is not a removal. */

static void
unplug_test (void)
{
	FakeuvStreamEvents events = {0};
	LossState state;
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	ArvBuffer *buffers[N_BUFFERS];
	GError *error = NULL;
	gint n_control_lost = 0;
	gint64 unplug_time;
	guint64 n_set_raw_io;
	guint i;

	if (!FAKEUV_IO_ERROR_IS_LOSS) {
		g_test_skip ("A removal reported as LIBUSB_ERROR_IO is a loss of control for Windows builds only");
		return;
	}

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	fake_usb_device_set_unplug_errors (device, LIBUSB_TRANSFER_ERROR, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO);
	camera = _open_camera (device);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb),
			  &n_control_lost);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, fakeuv_stream_callback, &events);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, RAW_IO_AVAILABLE);

	/* The application holds every buffer: nothing in flight, the next submission is the first to fail */
	for (i = 0; i < N_BUFFERS; i++) {
		buffers[i] = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) * 1000);
		g_assert_nonnull (buffers[i]);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffers[i]));
	}

	n_set_raw_io = fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO);
	unplug_time = g_get_monotonic_time ();
	fake_usb_device_unplug (device);

	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, buffers[i]);

	state.n_control_lost = &n_control_lost;
	state.events = &events;
	g_assert_true (fakeuv_wait_for (_lost_and_exited, &state, TIMEOUT_MS));
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 1);
	g_assert_cmpint ((g_get_monotonic_time () - unplug_time) / G_TIME_SPAN_MILLISECOND, <=,
			 fakeuv_timeout_ms (FAST_LOSS_TIMEOUT_MS));

	g_assert_false (arv_camera_stop_acquisition (camera, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED);
	g_clear_error (&error);

	/* The stream stops, at the latest when released, without touching RAW_IO on a device that is gone */
	g_object_unref (stream);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO), ==, n_set_raw_io);

	g_clear_object (&camera);

	g_assert_cmpuint (fake_usb_get_n_open_handles (), ==, 0);
	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);
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

/* A triggered camera, never triggered: every buffer in flight when the acquisition stops. RAW_IO is turned off only
 * once they are all back: the fake counts a change with transfers pending as misuse, which fails the test. */

static void
pending_transfers_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;
	guint i;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);

	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);

	for (i = 0; i < 3; i++) {
		ArvStream *stream;
		PendingReads pending;
		guint32 payload_size;

		stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, NULL, NULL);
		arv_camera_start_acquisition (camera, &error);
		g_assert_no_error (error);
		g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
				 RAW_IO_AVAILABLE);

		/* Leader, payload and trailer reads of every buffer */
		payload_size = _sirm (device, ARV_SIRM_PAYLOAD_SIZE);
		pending.device = device;
		pending.n_reads = N_BUFFERS * (2 + (720 * 540 + payload_size - 1) / payload_size);
		g_assert_true (fakeuv_wait_for (_reads_pending, &pending, TIMEOUT_MS));

		arv_camera_stop_acquisition (camera, &error);
		g_assert_no_error (error);
		g_assert_false (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
		g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
				  0);

		g_object_unref (stream);
	}

	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_RAW_IO_CHANGES_WHILE_PENDING), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO), ==,
			  RAW_IO_AVAILABLE ? 6 : 0);

	g_clear_object (&camera);
}

/* Receives @n_frames good frames from a stream that delivers nothing else, pushing the buffers back */

static void
_receive_good_frames (ArvStream *stream, FakeUsbDevice *device, guint n_frames)
{
	guint i;

	for (i = 0; i < n_frames; i++) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, (guint64) fakeuv_timeout_ms (TIMEOUT_MS) *
								    1000);

		g_assert_nonnull (buffer);
		g_assert_true (fakeuv_buffer_matches_frame (device, buffer));
		arv_stream_push_buffer (stream, buffer);
	}
}

/* The camera stalls the data endpoint while streaming. Without RAW_IO, WinUSB's AUTO_CLEAR_STALL policy clears the
 * stall once the reads in flight have failed. RAW_IO bypasses that error handling, and every later read fails until
 * the pipe is reset: the stream thread takes its transfers back, checks the device, resets the pipe and
 * resynchronizes. Either way, frames come back, the device is not taken for lost, and nothing spins. */

static void
halted_pipe (gboolean raw_io)
{
	FakeuvStreamEvents events = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint n_control_lost = 0;
	guint64 n_clear_halt, n_calls = 0;
	gint64 end_time;
	guint n_good_before = 0, n_good_after = 0, n_bad = 0;
	gboolean with_raw_io = raw_io && RAW_IO_AVAILABLE;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb),
			  &n_control_lost);

	g_setenv ("ARV_UV_RAW_IO", raw_io ? "1" : "0", TRUE);
	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, fakeuv_stream_callback, &events);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, with_raw_io);

	n_clear_halt = fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLEAR_HALT);

	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (n_good_after < 2 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		if (!fakeuv_buffer_matches_frame (device, buffer)) {
			n_bad++;
		} else if (n_calls > 0) {
			n_good_after++;
		} else if (++n_good_before == 3) {
			n_calls = _n_stream_calls (device);
			fake_usb_device_halt_endpoint (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);
		}

		arv_stream_push_buffer (stream, buffer);
	}

	if (g_test_verbose ())
		g_printerr ("%u bad, %u good after, %" G_GUINT64_FORMAT " libusb calls, %" G_GUINT64_FORMAT
			    " pipe reset(s), %" G_GUINT64_FORMAT " stall(s) cleared by WinUSB\n", n_bad, n_good_after,
			    _n_stream_calls (device) - n_calls,
			    fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLEAR_HALT) - n_clear_halt,
			    fake_usb_device_get_stat (device, FAKE_USB_STAT_AUTO_CLEARED_STALLS));

	g_assert_cmpuint (n_bad, >=, 1);
	g_assert_cmpuint (n_good_after, ==, 2 * N_BUFFERS);
	/* A few calls per buffer: a spinning thread makes thousands */
	g_assert_cmpuint (_n_stream_calls (device) - n_calls, <=, 40 + 10 * (2 * N_BUFFERS + n_bad));
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);

	if (with_raw_io) {
		/* Reset by the stream thread, with RAW_IO kept */
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_AUTO_CLEARED_STALLS), ==, 0);
		g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLEAR_HALT), >, n_clear_halt);
		g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	} else {
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_AUTO_CLEARED_STALLS), >=, 1);
		g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLEAR_HALT), ==, n_clear_halt);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	g_clear_object (&camera);
}

static void
halted_pipe_test (void)
{
	halted_pipe (TRUE);
}

static void
halted_pipe_without_raw_io_test (void)
{
	halted_pipe (FALSE);
}

/* Every read fails at once, the device being there, and resetting the pipe does not help: the stream thread neither
 * spins nor takes the device for lost, and streams again once the reads work again */

static void
failing_pipe (gboolean raw_io)
{
	FakeuvStreamEvents events = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint n_control_lost = 0;
	guint64 n_calls;
	gint64 start_time, end_time, elapsed_ms;
	guint n_returned = 0;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb),
			  &n_control_lost);

	g_setenv ("ARV_UV_RAW_IO", raw_io ? "1" : "0", TRUE);
	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, fakeuv_stream_callback, &events);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	_receive_good_frames (stream, device, 3);

	n_calls = _n_stream_calls (device);
	start_time = g_get_monotonic_time ();
	fake_usb_device_inject_transfer_status (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, LIBUSB_TRANSFER_STALL, -1);

	/* For a second, the application pushing back what it gets at once */
	end_time = start_time + (gint64) fakeuv_timeout_ms (1000) * G_TIME_SPAN_MILLISECOND;
	while (g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		n_returned++;
		arv_stream_push_buffer (stream, buffer);
	}
	elapsed_ms = (g_get_monotonic_time () - start_time) / G_TIME_SPAN_MILLISECOND;

	if (g_test_verbose ())
		g_printerr ("%u buffers back, %" G_GUINT64_FORMAT " libusb calls in %" G_GINT64_FORMAT " ms\n",
			    n_returned, _n_stream_calls (device) - n_calls, elapsed_ms);

	/* A spinning thread makes hundreds of thousands per second */
	g_assert_cmpuint (_n_stream_calls (device) - n_calls, <=, 100 + elapsed_ms);
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);

	/* The reads work again */
	fake_usb_device_inject_transfer_status (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, LIBUSB_TRANSFER_STALL, 0);
	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	n_returned = 0;
	while (n_returned < 2 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		if (fakeuv_buffer_matches_frame (device, buffer))
			n_returned++;
		arv_stream_push_buffer (stream, buffer);
	}
	g_assert_cmpuint (n_returned, ==, 2 * N_BUFFERS);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	g_clear_object (&camera);
}

static void
failing_pipe_test (void)
{
	failing_pipe (TRUE);
}

static void
failing_pipe_without_raw_io_test (void)
{
	failing_pipe (FALSE);
}

static gboolean
_raw_io_off (gpointer user_data)
{
	return !fake_usb_device_get_raw_io (user_data, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);
}

/* WinUSB fails reads that follow the RAW_IO rules, with LIBUSB_ERROR_IO, the device being there. Not a rejection: a
 * transient failure perhaps, RAW_IO passing reads on to the USB stack at once. Retried, as without RAW_IO, and nothing
 * is lost; failures that last a second make the acquisition go on without RAW_IO. Either way, the next acquisition
 * uses RAW_IO again. */

static void
io_failure (gboolean lasting)
{
	FakeuvStreamEvents events = {0};
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	GError *error = NULL;
	gint n_control_lost = 0;
	guint64 n_control_requests;
	gint64 end_time;
	guint n_good = 0, n_bad = 0;

	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return;
	}

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);
	g_signal_connect (arv_camera_get_device (camera), "control-lost", G_CALLBACK (_control_lost_cb),
			  &n_control_lost);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, fakeuv_stream_callback, &events);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	_receive_good_frames (stream, device, 3);

	n_control_requests = fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS);
	fake_usb_device_inject_error (device, FAKE_USB_CALL_SUBMIT_TRANSFER, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
				      LIBUSB_ERROR_IO, lasting ? -1 : 3);

	if (lasting) {
		/* RAW_IO turned off once the failures have lasted a second, with nothing pending (no misuse) */
		g_assert_true (fakeuv_wait_for (_raw_io_off, device, TIMEOUT_MS));
		fake_usb_device_clear_injections (device);
	}

	/* Good frames again; when RAW_IO was turned off, the frames in flight were lost */
	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (n_good < 2 * N_BUFFERS && g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

		if (buffer == NULL)
			continue;

		if (fakeuv_buffer_matches_frame (device, buffer))
			n_good++;
		else
			n_bad++;
		arv_stream_push_buffer (stream, buffer);
	}

	if (g_test_verbose ())
		g_printerr ("%u good, %u bad\n", n_good, n_bad);

	g_assert_cmpuint (n_good, ==, 2 * N_BUFFERS);
	if (!lasting)
		g_assert_cmpuint (n_bad, ==, 0);

	/* Checked, found */
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_CONTROL_REQUESTS), >, n_control_requests);
	g_assert_cmpint (g_atomic_int_get (&n_control_lost), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&events.n_exit), ==, 0);
	if (!lasting) {
		/* Retried, not given up */
		g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
		g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_aborted"), ==, 0);
		g_assert_cmpuint (arv_stream_get_info_uint64_by_name (stream, "n_failures"), ==, 0);
	}

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	/* Not a rejection: RAW_IO is not disabled for the device */
	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);
	g_assert_true (acquisition.raw_io_seen);

	g_clear_object (&camera);
}

static void
io_failure_test (void)
{
	io_failure (FALSE);
}

static void
lasting_io_failure_test (void)
{
	io_failure (TRUE);
}

/* The RAW_IO maximum caps the size of the reads, not the bytes in flight: 8 times the maximum transfer size, as
 * without RAW_IO. A triggered camera, never triggered: the reads of every buffer pending, as many as the budget
 * allows. 1440x1080 in reads of 300 KiB: 8 reads per buffer, 32 in all, about 6 MB. */

static void
in_flight_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvStream *stream;
	PendingReads pending;
	GError *error = NULL;

	if (!RAW_IO_AVAILABLE) {
		g_test_skip ("RAW_IO needs libusb 1.0.30 headers");
		return;
	}

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 1440, 1080);
	fake_usb_device_set_max_raw_io_transfer_size (device, 300 * 1024);
	camera = _open_camera (device);

	arv_camera_set_trigger (camera, "Software", &error);
	g_assert_no_error (error);

	stream = _create_stream (camera, device, ARV_UV_USB_MODE_ASYNC, NULL, NULL);
	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);
	g_assert_true (fake_usb_device_get_raw_io (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 300 * 1024);

	/* 2.4 MB in flight at most before: 11 reads */
	pending.device = device;
	pending.n_reads = N_BUFFERS * 8;
	g_assert_true (fakeuv_wait_for (_reads_pending, &pending, TIMEOUT_MS));
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_MAX_STREAM_READ), <=, 300 * 1024);

	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);
	g_object_unref (stream);

	g_clear_object (&camera);
}

/* On Windows, Aravis looks the RAW_IO functions of libusb 1.0.30 up at run time, so that it still loads with an older
 * libusb-1.0.dll: they are not there, as in kernel32.dll, and RAW_IO is not available, ARV_UV_RAW_IO=1 or not. Nothing
 * else changes. */

static void
older_libusb_dll_test (void)
{
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
	Acquisition acquisition = {0};
	FakeUsbDevice *device;
	ArvCamera *camera;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 720, 540);
	camera = _open_camera (device);

	g_assert_false (arv_uv_libusb_look_functions_up (GetModuleHandleW (L"kernel32.dll"), NULL));

	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, N_FRAMES, &acquisition);
	g_assert_false (acquisition.raw_io_seen);
	g_assert_cmpuint (acquisition.n_set_raw_io, ==, 0);
	g_assert_cmpuint (acquisition.n_unaligned_reads, >, 0);
	g_assert_cmpuint (_sirm (device, ARV_SIRM_PAYLOAD_SIZE), ==, 720 * 540);
	/* Not called: Aravis did not find them */
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_GET_MAX_RAW_IO_TRANSFER_SIZE), ==, 0);

	/* The libusb of the program, the fake, has them */
	g_assert_true (arv_uv_libusb_look_functions_up (NULL, NULL));

	_acquire (camera, device, ARV_UV_USB_MODE_ASYNC, 3, &acquisition);
	g_assert_true (acquisition.raw_io_seen);

	g_clear_object (&camera);
#else
	g_test_skip ("Looked up at run time on Windows only, with libusb 1.0.30 headers, for a libusb DLL");
#endif
}

/* A GenDC buffer that fails, its frame cut short: the image part, whose size the descriptor gives, does not reach
 * past the data received, in both USB modes */

static void
gendc_failed_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	gsize descriptor_size = fake_usb_camera_get_gendc_descriptor_size ();
	gsize image_size = 100 * 100;
	guint mode;

	device = _add_camera (FAKE_USB_PLATFORM_WINDOWS, 100, 100);
	fake_usb_camera_set_payload_type (device, ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER);
	camera = _open_camera (device);

	for (mode = 0; mode < 2; mode++) {
		ArvUvUsbMode usb_mode = mode == 0 ? ARV_UV_USB_MODE_ASYNC : ARV_UV_USB_MODE_SYNC;
		ArvStream *stream;
		GError *error = NULL;
		gint64 end_time;
		guint n_good = 0, n_failed_gendc = 0;

		stream = _create_stream (camera, device, usb_mode, NULL, NULL);
		arv_camera_start_acquisition (camera, &error);
		g_assert_no_error (error);

		end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
		while ((n_failed_gendc == 0 || n_good < 3 + N_BUFFERS) && g_get_monotonic_time () < end_time) {
			ArvBuffer *buffer = arv_stream_timeout_pop_buffer (stream, 10000);

			if (buffer == NULL)
				continue;

			if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS) {
				if (++n_good == 3)
					/* The next frame lacks the last 1000 bytes of its image */
					fake_usb_camera_truncate_payloads (device, 1, 1000);
			} else if (buffer->priv->has_gendc) {
				const guint8 *data;
				size_t size = 0;

				data = arv_buffer_get_image_data (buffer, &size);
				g_assert_true (data >= buffer->priv->data);
				g_assert_cmpuint ((gsize) (data - buffer->priv->data) + size, <=,
						  buffer->priv->allocated_size);
				g_assert_cmpuint (size, <, image_size);
				n_failed_gendc++;
			}

			memset (buffer->priv->data, 0, buffer->priv->allocated_size);
			arv_stream_push_buffer (stream, buffer);
		}

		if (g_test_verbose ())
			g_printerr ("%s: %u good, %u failed GenDC buffers\n",
				    mode == 0 ? "async" : "sync", n_good, n_failed_gendc);

		g_assert_cmpuint (n_failed_gendc, >=, 1);
		g_assert_cmpuint (n_good, >=, 3 + N_BUFFERS);

		arv_camera_stop_acquisition (camera, &error);
		g_assert_no_error (error);
		g_object_unref (stream);
	}

	g_assert_cmpuint (descriptor_size + image_size, ==, fake_usb_camera_get_sirm_register (device,
												  ARV_SIRM_REQ_PAYLOAD_SIZE));

	g_clear_object (&camera);
}

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvrawio/frames-720x540", frames_720x540_test);
	fakeuv_test_add ("/fakeuvrawio/frames-1440x1080", frames_1440x1080_test);
	fakeuv_test_add ("/fakeuvrawio/read-sizes", read_sizes_test);
	fakeuv_test_add ("/fakeuvrawio/sync-mode", sync_mode_test);
	fakeuv_test_add ("/fakeuvrawio/fallback", fallback_test);
	fakeuv_test_add ("/fakeuvrawio/environment", environment_test);
	fakeuv_test_add ("/fakeuvrawio/older-libusb-dll", older_libusb_dll_test);
	g_test_add_data_func ("/fakeuvrawio/transfer-size/raw-io-cap", &raw_io_cap_case, transfer_size_test);
	g_test_add_data_func ("/fakeuvrawio/transfer-size/raw-io-cap-packets", &raw_io_cap_packets_case,
			      transfer_size_test);
	g_test_add_data_func ("/fakeuvrawio/transfer-size/packets", &packets_case, transfer_size_test);
	g_test_add_data_func ("/fakeuvrawio/transfer-size/alignment", &alignment_case, transfer_size_test);
	g_test_add_data_func ("/fakeuvrawio/transfer-size/usb2", &usb2_case, transfer_size_test);
	g_test_add_data_func ("/fakeuvrawio/transfer-size/usb2-raw-io", &usb2_raw_io_case, transfer_size_test);
	fakeuv_test_add ("/fakeuvrawio/setter", setter_test);
	fakeuv_test_add ("/fakeuvrawio/gendc-small", gendc_small_test);
	fakeuv_test_add ("/fakeuvrawio/gendc", gendc_test);
	fakeuv_test_add ("/fakeuvrawio/resync", resync_test);
	fakeuv_test_add ("/fakeuvrawio/resync-640x480", resync_640x480_test);
	fakeuv_test_add ("/fakeuvrawio/resync-1920x1080", resync_1920x1080_test);
	fakeuv_test_add ("/fakeuvrawio/resync-1024x1024", resync_1024x1024_test);
	fakeuv_test_add ("/fakeuvrawio/resync-without-raw-io", resync_without_raw_io_test);
	fakeuv_test_add ("/fakeuvrawio/resync-without-raw-io-640x480", resync_without_raw_io_640x480_test);
	fakeuv_test_add ("/fakeuvrawio/resync-limit", resync_limit_test);
	fakeuv_test_add ("/fakeuvrawio/resync-limit-slow", resync_limit_slow_test);
	fakeuv_test_add ("/fakeuvrawio/zero-length-packets", zero_length_packets_test);
	fakeuv_test_add ("/fakeuvrawio/zero-length-packets-640x480", zero_length_packets_640x480_test);
	fakeuv_test_add ("/fakeuvrawio/rejected-read", rejected_read_test);
	fakeuv_test_add ("/fakeuvrawio/io-failure", io_failure_test);
	fakeuv_test_add ("/fakeuvrawio/lasting-io-failure", lasting_io_failure_test);
	fakeuv_test_add ("/fakeuvrawio/halted-pipe", halted_pipe_test);
	fakeuv_test_add ("/fakeuvrawio/halted-pipe-without-raw-io", halted_pipe_without_raw_io_test);
	fakeuv_test_add ("/fakeuvrawio/failing-pipe", failing_pipe_test);
	fakeuv_test_add ("/fakeuvrawio/failing-pipe-without-raw-io", failing_pipe_without_raw_io_test);
	fakeuv_test_add ("/fakeuvrawio/in-flight", in_flight_test);
	fakeuv_test_add ("/fakeuvrawio/gendc-failed", gendc_failed_test);
	fakeuv_test_add ("/fakeuvrawio/unplug", unplug_test);
	fakeuv_test_add ("/fakeuvrawio/pending-transfers", pending_transfers_test);

	return fakeuv_test_run ();
}
