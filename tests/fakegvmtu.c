/* SPDX-License-Identifier:Unlicense */

/* GigE Vision packet size versus MTU (issue #1047), end to end, with the fake GigE camera on the loopback interface.
 *
 * Each test starts its own fake camera, which binds the fixed GVCP port: never run these tests concurrently with
 * another fake GigE camera.
 *
 * The loopback MTU is unknown on Linux and Windows. The tests needing a known interface MTU set the loopback one with a
 * test hook. The fake camera emulates the MTU of the network path (gvsp-mtu property), by dropping larger packets. */

#include <glib.h>
#include <arv.h>
#include "../src/arvnetworkprivate.h"

#define SERIAL_NUMBER		"GVMTUTest"
#define LARGE_PACKET_SIZE	20000
#define ISSUE_PACKET_SIZE	8228
#define ISSUE_MTU		1500
#define N_BUFFERS		5

/* The fake camera sends each frame at once, at the speed of the loopback interface, which drops the datagrams the
 * stream socket can't hold. The default socket buffer, the size of a frame plus a little, then overflows whenever the
 * stream thread doesn't read the frame as fast, as in a sanitizer build: the frames of a lossless network fail. The
 * stream sockets of the tests hold several frames. */
#define SOCKET_BUFFER_SIZE	(4 * 1024 * 1024)
/* The stream applies its socket buffer size when the first frame arrives, which the default socket buffer of the
 * system may not hold: the acquisitions which need a complete frame pop at most this many buffers to get one */
#define MAX_BUFFERS		50

/* With a packet size of 8228 bytes, data packets carry 8192 bytes: a 512x512 Mono8 frame fills 32 of them, a 512x513
 * one needs a 33rd, of 512 bytes, which fits in a 1500 bytes MTU. Same for 1920x1080 Mono8 frames: 253 data packets
 * of 8192 bytes, and one of 1024 bytes. */
#define WIDTH			512
#define EXACT_HEIGHT		512
#define INEXACT_HEIGHT		513

typedef struct {
	ArvGvFakeCamera *simulator;
	ArvCamera *camera;
} Fixture;

static void
fixture_start (Fixture *fixture)
{
	GInetAddress *loopback;
	ArvDevice *device;
	GError *error = NULL;

	arv_set_fake_camera_genicam_filename (GENICAM_FILENAME);

	fixture->simulator = arv_gv_fake_camera_new ("127.0.0.1", SERIAL_NUMBER);
	g_assert_true (ARV_IS_GV_FAKE_CAMERA (fixture->simulator));
	g_assert_true (arv_gv_fake_camera_is_running (fixture->simulator));

	/* Connect through the loopback interface, without discovery */
	loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
	device = arv_gv_device_new (loopback, loopback, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_GV_DEVICE (device));
	g_object_unref (loopback);

	fixture->camera = arv_camera_new_with_device (device, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (fixture->camera));
	g_object_unref (device);
}

static void
fixture_stop (Fixture *fixture)
{
	g_clear_object (&fixture->camera);
	g_clear_object (&fixture->simulator);

	arv_network_set_loopback_mtu_override (0);
}

static guint
loopback_mtu (void)
{
	GInetAddress *loopback;
	guint mtu;

	loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
	mtu = arv_network_get_interface_mtu_by_address (loopback);
	g_object_unref (loopback);

	g_test_message ("Loopback interface MTU = %u%s", mtu, mtu == 0 ? " (unknown)" : "");

	return mtu;
}

static void
set_packet_size (ArvCamera *camera, guint packet_size)
{
	GError *error = NULL;

	arv_camera_gv_set_packet_size (camera, packet_size, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_camera_gv_get_packet_size (camera, NULL), ==, packet_size);
}

static void
set_height (ArvCamera *camera, guint height)
{
	GError *error = NULL;

	arv_camera_set_integer (camera, "Width", WIDTH, &error);
	g_assert_no_error (error);
	arv_camera_set_integer (camera, "Height", height, &error);
	g_assert_no_error (error);
	g_assert_cmpint (arv_camera_get_payload (camera, NULL), ==, WIDTH * height);
}

static guint64
get_statistic (ArvStream *stream, const char *name)
{
	return arv_stream_get_info_uint64_by_name (stream, name);
}

static ArvStream *
create_stream (ArvCamera *camera)
{
	ArvStream *stream;
	GError *error = NULL;
	size_t payload;
	unsigned int i;

	stream = arv_camera_create_stream (camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));

	g_object_set (stream,
		      "socket-buffer", ARV_GV_STREAM_SOCKET_BUFFER_FIXED,
		      "socket-buffer-size", SOCKET_BUFFER_SIZE,
		      NULL);

	payload = arv_camera_get_payload (camera, &error);
	g_assert_no_error (error);

	for (i = 0; i < N_BUFFERS; i++)
		arv_stream_push_buffer (stream, arv_buffer_new (payload, NULL));

	return stream;
}

static void
stop_acquisition (ArvCamera *camera, ArvStream *stream)
{
	GError *error = NULL;

	/* Also stops the stream thread: the statistics are final */
	arv_camera_stop_acquisition (camera, &error);
	g_assert_no_error (error);

	g_test_message ("%" G_GUINT64_FORMAT " completed, %" G_GUINT64_FORMAT " failed (%" G_GUINT64_FORMAT
			" timeouts), %" G_GUINT64_FORMAT " with only leader and trailer",
			get_statistic (stream, "n_completed_buffers"), get_statistic (stream, "n_failures"),
			get_statistic (stream, "n_timeouts"), get_statistic (stream, "n_leader_trailer_only_failures"));
}

/* Streams until @n_buffers buffers came out, @n_completed of them complete frames, or for at most @timeout_ms. At most
 * MAX_BUFFERS buffers come out. */

static ArvStream *
acquire (ArvCamera *camera, guint n_buffers, guint n_completed, guint timeout_ms)
{
	ArvStream *stream;
	GError *error = NULL;
	gint64 end_time;
	guint n_popped = 0;
	guint n_popped_completed = 0;

	stream = create_stream (camera);

	arv_camera_start_acquisition (camera, &error);
	g_assert_no_error (error);

	end_time = g_get_monotonic_time () + timeout_ms * 1000LL;
	while ((n_popped < n_buffers || n_popped_completed < n_completed) && n_popped < MAX_BUFFERS &&
	       g_get_monotonic_time () < end_time) {
		ArvBuffer *buffer;

		buffer = arv_stream_timeout_pop_buffer (stream, 100000);
		if (buffer != NULL) {
			n_popped++;
			if (arv_buffer_get_status (buffer) == ARV_BUFFER_STATUS_SUCCESS)
				n_popped_completed++;
			arv_stream_push_buffer (stream, buffer);
		}
	}

	g_test_message ("%u buffer(s) out", n_popped);

	stop_acquisition (camera, stream);

	return stream;
}

/* The fake camera can't fire test packets: the automatic packet size adjustment can only lower a packet size larger
 * than the MTU of the interface the device is reached through. The loopback MTU is unknown on Linux and Windows, so
 * the packet size is unchanged there; it is 16384 bytes on macOS. No streaming here, macOS can't send UDP datagrams
 * that large by default. */

static void
check_packet_size (guint packet_size, guint mtu)
{
	if (mtu > 0 && mtu < LARGE_PACKET_SIZE)
		g_assert_cmpuint (packet_size, <=, mtu);
	else
		g_assert_cmpuint (packet_size, ==, LARGE_PACKET_SIZE);
}

static void
auto_packet_size_test (void)
{
	Fixture fixture;
	ArvStream *stream;
	GError *error = NULL;
	guint mtu;
	guint packet_size;

	fixture_start (&fixture);
	mtu = loopback_mtu ();

	set_packet_size (fixture.camera, LARGE_PACKET_SIZE);

	packet_size = arv_camera_gv_auto_packet_size (fixture.camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_camera_gv_get_packet_size (fixture.camera, NULL), ==, packet_size);
	check_packet_size (packet_size, mtu);

	/* Same at stream creation, with the default adjustment mode */
	set_packet_size (fixture.camera, LARGE_PACKET_SIZE);

	stream = arv_camera_create_stream (fixture.camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));
	check_packet_size (arv_camera_gv_get_packet_size (fixture.camera, NULL), mtu);
	g_clear_object (&stream);

	fixture_stop (&fixture);
}

/* Issue #1047, with a known MTU of the interface the device is reached through: a larger packet size is lowered to
 * it, by the automatic adjustment and at stream creation, although the device can't fire test packets. */

static void
known_mtu_test (void)
{
	Fixture fixture;
	ArvStream *stream;
	GError *error = NULL;
	gint64 increment;
	guint packet_size;

	fixture_start (&fixture);
	arv_network_set_loopback_mtu_override (ISSUE_MTU);
	g_assert_cmpuint (loopback_mtu (), ==, ISSUE_MTU);

	increment = arv_camera_get_integer_increment (fixture.camera, "GevSCPSPacketSize", &error);
	g_assert_no_error (error);
	g_assert_cmpint (increment, >, 0);

	set_packet_size (fixture.camera, LARGE_PACKET_SIZE);

	/* The largest size of the packet size grid fitting in the MTU */
	packet_size = arv_camera_gv_auto_packet_size (fixture.camera, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (packet_size, <=, ISSUE_MTU);
	g_assert_cmpuint (packet_size, >, ISSUE_MTU - increment);
	g_assert_cmpuint (arv_camera_gv_get_packet_size (fixture.camera, NULL), ==, packet_size);

	/* A packet size fitting in the MTU is kept */
	set_packet_size (fixture.camera, 1400);
	g_assert_cmpuint (arv_camera_gv_auto_packet_size (fixture.camera, &error), ==, 1400);
	g_assert_no_error (error);
	g_assert_cmpuint (arv_camera_gv_get_packet_size (fixture.camera, NULL), ==, 1400);

	/* Same at stream creation, with the default adjustment mode */
	set_packet_size (fixture.camera, ISSUE_PACKET_SIZE);

	stream = arv_camera_create_stream (fixture.camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));
	g_assert_cmpuint (arv_camera_gv_get_packet_size (fixture.camera, NULL), ==, packet_size);
	g_clear_object (&stream);

	fixture_stop (&fixture);
}

/* The "done when" of issue #1047: a camera stored at a packet size of 8228 bytes, reached through an interface whose
 * MTU is 1500 bytes, on a network dropping larger packets, streams complete frames with the default settings. */

static void
issue_1047_test (void)
{
	Fixture fixture;
	ArvStream *stream;

	fixture_start (&fixture);
	arv_network_set_loopback_mtu_override (ISSUE_MTU);

	g_object_set (fixture.simulator, "gvsp-mtu", (guint) ISSUE_MTU, NULL);
	set_packet_size (fixture.camera, ISSUE_PACKET_SIZE);

	stream = acquire (fixture.camera, N_BUFFERS, 1, 10000);

	g_assert_cmpuint (arv_camera_gv_get_packet_size (fixture.camera, NULL), <=, ISSUE_MTU);
	g_assert_cmpuint (get_statistic (stream, "n_completed_buffers"), >, 0);
	g_assert_cmpuint (get_statistic (stream, "n_leader_trailer_only_failures"), ==, 0);
	g_clear_object (&stream);

	fixture_stop (&fixture);
}

/* ARV_GV_PACKET_SIZE_ADJUSTMENT_NEVER keeps any packet size */

static void
never_adjust_test (void)
{
	Fixture fixture;
	ArvStream *stream;
	GError *error = NULL;

	fixture_start (&fixture);
	arv_network_set_loopback_mtu_override (ISSUE_MTU);

	set_packet_size (fixture.camera, LARGE_PACKET_SIZE);
	arv_camera_gv_set_packet_size_adjustment (fixture.camera, ARV_GV_PACKET_SIZE_ADJUSTMENT_NEVER);

	stream = arv_camera_create_stream (fixture.camera, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_STREAM (stream));
	g_assert_cmpuint (arv_camera_gv_get_packet_size (fixture.camera, NULL), ==, LARGE_PACKET_SIZE);
	g_clear_object (&stream);

	fixture_stop (&fixture);
}

/* Issue #1047, before any adjustment: the network drops the data packets, larger than its MTU, and only the small
 * leader and trailer packets of each frame reach the host. The stream counts these failures, and warns once in a
 * while, naming the packet size and the interface MTU. The warnings of the stream category go to stderr. */

static void
setup_leader_trailer_only (Fixture *fixture, guint height)
{
	g_assert_true (arv_debug_enable ("stream:1"));

	fixture_start (fixture);
	arv_network_set_loopback_mtu_override (ISSUE_MTU);

	set_height (fixture->camera, height);
	g_object_set (fixture->simulator, "gvsp-mtu", (guint) ISSUE_MTU, NULL);
	set_packet_size (fixture->camera, ISSUE_PACKET_SIZE);
	arv_camera_gv_set_packet_size_adjustment (fixture->camera, ARV_GV_PACKET_SIZE_ADJUSTMENT_NEVER);
}

static void
check_leader_trailer_only_failures (ArvStream *stream)
{
	guint64 n_leader_trailer_only;

	n_leader_trailer_only = get_statistic (stream, "n_leader_trailer_only_failures");
	g_assert_cmpuint (n_leader_trailer_only, >, 0);
	g_assert_cmpuint (n_leader_trailer_only, <=, get_statistic (stream, "n_failures"));
	g_assert_cmpuint (get_statistic (stream, "n_completed_buffers"), ==, 0);
}

static void
leader_trailer_only_test (void)
{
	if (g_test_subprocess ()) {
		Fixture fixture;
		ArvStream *stream;

		setup_leader_trailer_only (&fixture, EXACT_HEIGHT);

		stream = acquire (fixture.camera, N_BUFFERS, 0, 10000);
		check_leader_trailer_only_failures (stream);
		g_clear_object (&stream);

		fixture_stop (&fixture);
		return;
	}

	g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed ();
	g_test_trap_assert_stderr ("*Only the leader and trailer of frame*were received, 32 of its 32 data packets were"
				   " lost*Packet size is 8228 bytes, interface MTU is 1500 bytes*");
	/* Rate limited: a single warning during the few seconds of the test */
	g_test_trap_assert_stderr_unmatched ("*Only the leader*Only the leader*");
}

/* The last data packet of each frame is small enough to get through: it doesn't hide the failure pattern */

static void
leader_trailer_only_last_packet_test (void)
{
	if (g_test_subprocess ()) {
		Fixture fixture;
		ArvStream *stream;

		setup_leader_trailer_only (&fixture, INEXACT_HEIGHT);

		stream = acquire (fixture.camera, N_BUFFERS, 0, 10000);
		check_leader_trailer_only_failures (stream);
		g_clear_object (&stream);

		fixture_stop (&fixture);
		return;
	}

	g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed ();
	g_test_trap_assert_stderr ("*Only the leader, the trailer and 1 smaller data packet(s) of frame*were received,"
				   " 32 of its 33 data packets were lost*Packet size is 8228 bytes*");
}

/* A single frame, like a snapshot or the last frame of an acquisition: no next frame comes to close it. Once its
 * trailer is received, it times out, is output, and counted. */

static void
leader_trailer_only_single_frame_test (void)
{
	if (g_test_subprocess ()) {
		Fixture fixture;
		ArvStream *stream;
		ArvBuffer *buffer;
		GError *error = NULL;

		setup_leader_trailer_only (&fixture, EXACT_HEIGHT);

		arv_camera_set_trigger (fixture.camera, "Software", &error);
		g_assert_no_error (error);

		stream = create_stream (fixture.camera);

		arv_camera_start_acquisition (fixture.camera, &error);
		g_assert_no_error (error);

		arv_camera_software_trigger (fixture.camera, &error);
		g_assert_no_error (error);

		buffer = arv_stream_timeout_pop_buffer (stream, 5 * G_USEC_PER_SEC);
		g_assert_nonnull (buffer);
		g_assert_cmpint (arv_buffer_get_status (buffer), ==, ARV_BUFFER_STATUS_TIMEOUT);
		arv_stream_push_buffer (stream, buffer);

		stop_acquisition (fixture.camera, stream);

		g_assert_cmpuint (get_statistic (stream, "n_leader_trailer_only_failures"), ==, 1);
		g_assert_cmpuint (get_statistic (stream, "n_timeouts"), ==, 1);
		g_assert_cmpuint (get_statistic (stream, "n_aborted"), ==, 0);
		g_clear_object (&stream);

		fixture_stop (&fixture);
		return;
	}

	g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed ();
	g_test_trap_assert_stderr ("*Only the leader and trailer of frame*were received, 32 of its 32 data packets were"
				   " lost*");
}

/* Frames failing for other reasons are not counted as leader and trailer only failures */

static void
no_false_positive_test (void)
{
	static const guint heights[] = {EXACT_HEIGHT, INEXACT_HEIGHT};
	Fixture fixture;
	ArvStream *stream;
	unsigned int i;

	fixture_start (&fixture);
	arv_network_set_loopback_mtu_override (ISSUE_MTU);

	set_packet_size (fixture.camera, ISSUE_PACKET_SIZE);
	arv_camera_gv_set_packet_size_adjustment (fixture.camera, ARV_GV_PACKET_SIZE_ADJUSTMENT_NEVER);

	for (i = 0; i < G_N_ELEMENTS (heights); i++) {
		set_height (fixture.camera, heights[i]);

		/* Large packets go through the emulated network */
		g_object_set (fixture.simulator, "gvsp-mtu", (guint) 9000, "gvsp-lost-ratio", 0.0, NULL);
		stream = acquire (fixture.camera, N_BUFFERS, 1, 10000);
		g_assert_cmpuint (get_statistic (stream, "n_completed_buffers"), >, 0);
		g_assert_cmpuint (get_statistic (stream, "n_leader_trailer_only_failures"), ==, 0);
		g_clear_object (&stream);

		/* Random packet loss */
		g_object_set (fixture.simulator, "gvsp-mtu", (guint) 0, "gvsp-lost-ratio", 0.3, NULL);
		stream = acquire (fixture.camera, N_BUFFERS, 0, 10000);
		g_assert_cmpuint (get_statistic (stream, "n_leader_trailer_only_failures"), ==, 0);
		g_clear_object (&stream);
	}

	fixture_stop (&fixture);
}

int
main (int argc, char *argv[])
{
	int result;

	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/fakegvmtu/auto-packet-size", auto_packet_size_test);
	g_test_add_func ("/fakegvmtu/known-mtu", known_mtu_test);
	g_test_add_func ("/fakegvmtu/issue-1047", issue_1047_test);
	g_test_add_func ("/fakegvmtu/never-adjust", never_adjust_test);
	g_test_add_func ("/fakegvmtu/leader-trailer-only", leader_trailer_only_test);
	g_test_add_func ("/fakegvmtu/leader-trailer-only-last-packet", leader_trailer_only_last_packet_test);
	g_test_add_func ("/fakegvmtu/leader-trailer-only-single-frame", leader_trailer_only_single_frame_test);
	g_test_add_func ("/fakegvmtu/no-false-positive", no_false_positive_test);

	result = g_test_run ();

	arv_shutdown ();

	return result;
}
