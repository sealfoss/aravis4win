/* SPDX-License-Identifier:Unlicense */

/* GigE Vision packet size versus the MTU of the host interface (issues #1047 and #1065).
 *
 * Unit tests of the packet size negotiation of arv_gv_device_auto_packet_size(), with simulated test packets, of the MTU
 * reported by the network interface enumeration, and of the loopback MTU test hook. They need neither a camera nor any
 * network traffic. */

#include <glib.h>
#include <arv.h>
#include "../src/arvgvdeviceprivate.h"
#include "../src/arvnetworkprivate.h"

/* Default GevSCPSPacketSize bounds, used when the device Genicam data does not define them (see
 * arv_gc_set_default_gv_features) */
#define MIN_SIZE	220
#define MAX_SIZE	16404
#define INC		4

typedef struct {
	guint limit;		/* Largest packet size whose test packet reaches the host, 0 for no limit */
	gboolean fail_all;	/* No test packet reaches the host */
	gboolean error;		/* Fail with an error */
	guint n_tests;
	guint first_size;
	guint largest_size;
} TestPacketData;

static gboolean
test_packet (guint *packet_size, void *user_data, GError **error)
{
	TestPacketData *data = user_data;

	if (data->error) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT, "Simulated error");
		return FALSE;
	}

	if (data->n_tests == 0)
		data->first_size = *packet_size;
	data->largest_size = MAX (data->largest_size, *packet_size);
	data->n_tests++;

	if (data->fail_all)
		return FALSE;

	return data->limit == 0 || *packet_size <= data->limit;
}

static void
reset_counters (TestPacketData *data)
{
	data->n_tests = 0;
	data->first_size = 0;
	data->largest_size = 0;
}

static guint
search (guint packet_size, guint mtu, gboolean exit_early, TestPacketData *data)
{
	GError *error = NULL;
	guint result;

	if (data != NULL)
		reset_counters (data);

	result = arv_gv_device_search_packet_size (packet_size, MIN_SIZE, MAX_SIZE, INC, mtu, exit_early,
						   data != NULL ? test_packet : NULL, data, &error);
	g_assert_no_error (error);

	return result;
}

/* Issue #1065: the host reassembles fragmented test packets, so every test packet comes through */

static void
mtu_reassembly_test (void)
{
	TestPacketData data = {0};

	/* Without MTU, as before: the search climbs to the top of its grid */
	g_assert_cmpuint (search (1500, 0, FALSE, &data), ==, 16400);

	/* The MTU caps the search, which starts from the cap */
	g_assert_cmpuint (search (1500, 9000, FALSE, &data), ==, 9000);
	g_assert_cmpuint (data.first_size, ==, 9000);
	g_assert_cmpuint (data.n_tests, ==, 1);

	/* A stored packet size above the MTU is lowered, even though its own test packet would come through */
	g_assert_cmpuint (search (16400, 9000, TRUE, &data), ==, 9000);
	g_assert_cmpuint (data.largest_size, ==, 9000);
	g_assert_cmpuint (search (16400, 9000, FALSE, &data), ==, 9000);
	g_assert_cmpuint (data.largest_size, ==, 9000);

	/* A working packet size below the MTU is kept when only checking it */
	g_assert_cmpuint (search (8228, 9000, TRUE, &data), ==, 8228);
	g_assert_cmpuint (data.n_tests, ==, 1);
}

/* Issue #1047: packets larger than 1500 bytes never reach the host */

static void
mtu_no_test_packet_test (void)
{
	/* Without test packet, a stored packet size above the MTU is lowered */
	g_assert_cmpuint (search (8228, 1500, TRUE, NULL), ==, 1500);
	g_assert_cmpuint (search (8228, 1500, FALSE, NULL), ==, 1500);

	/* Without MTU, as before: unchanged */
	g_assert_cmpuint (search (8228, 0, TRUE, NULL), ==, 8228);

	/* A packet size fitting in the MTU is unchanged */
	g_assert_cmpuint (search (1500, 1500, TRUE, NULL), ==, 1500);
	g_assert_cmpuint (search (1400, 1500, TRUE, NULL), ==, 1400);
}

static void
mtu_dropped_test (void)
{
	TestPacketData data = {0};
	guint n_tests_without_mtu;

	data.limit = 1500;

	/* Without MTU, the binary search finds 1500 bytes after many test packets */
	g_assert_cmpuint (search (8228, 0, TRUE, &data), ==, 1500);
	n_tests_without_mtu = data.n_tests;

	/* With the MTU, the first test is the right one */
	g_assert_cmpuint (search (8228, 1500, TRUE, &data), ==, 1500);
	g_assert_cmpuint (data.first_size, ==, 1500);
	g_assert_cmpuint (data.largest_size, ==, 1500);
	g_assert_cmpuint (data.n_tests, ==, 1);
	g_assert_cmpuint (data.n_tests, <, n_tests_without_mtu);

	/* No test packet comes through: the stored size is lowered to the MTU anyway */
	data.fail_all = TRUE;
	g_assert_cmpuint (search (8228, 1500, TRUE, &data), ==, 1500);
	g_assert_cmpuint (data.largest_size, ==, 1500);

	/* Without MTU, as before: unchanged */
	g_assert_cmpuint (search (8228, 0, TRUE, &data), ==, 8228);
}

/* Unknown MTU (0, as reported for the Windows and Linux loopback interfaces), or MTU not smaller than the largest
 * packet size: the search is unchanged */

static void
mtu_unknown_test (void)
{
	static const guint mtus[] = {0, MAX_SIZE, 17000, 65535, 65536, 0xffffffff};
	TestPacketData data = {0};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (mtus); i++) {
		g_assert_cmpuint (search (1500, mtus[i], FALSE, &data), ==, 16400);
		g_assert_cmpuint (data.first_size, ==, 1500);
		g_assert_cmpuint (search (16400, mtus[i], TRUE, &data), ==, 16400);
		g_assert_cmpuint (data.n_tests, ==, 1);
	}

	/* A packet size above the largest one, but fitting in the MTU, is not changed either */
	g_assert_cmpuint (search (20000, 0, TRUE, NULL), ==, 20000);
	g_assert_cmpuint (search (20000, 65535, TRUE, NULL), ==, 20000);
	g_assert_cmpuint (search (20000, 0xffffffff, TRUE, NULL), ==, 20000);

	/* But a packet size above a known MTU is lowered, whatever the MTU */
	g_assert_cmpuint (search (20000, 17000, TRUE, NULL), ==, MAX_SIZE);
	g_assert_cmpuint (search (20000, 17000, FALSE, &data), ==, MAX_SIZE);
	g_assert_cmpuint (data.largest_size, ==, MAX_SIZE);
}

static void
mtu_alignment_test (void)
{
	/* The cap is the largest size of the search grid fitting in the MTU */
	g_assert_cmpuint (search (8228, 1441, TRUE, NULL), ==, 1440);
	g_assert_cmpuint (arv_gv_device_search_packet_size (8228, 576, 9000, 8, 1500, TRUE, NULL, NULL, NULL), ==, 1496);
	g_assert_cmpuint (arv_gv_device_search_packet_size (8228, 576, 9000, 0, 1500, TRUE, NULL, NULL, NULL), ==, 1500);

	/* An MTU below the smallest packet size gives the smallest packet size */
	g_assert_cmpuint (search (8228, 200, TRUE, NULL), ==, MIN_SIZE);
}

/* An MTU below the smallest packet size: no packet size fits in it. Every search gives the smallest packet size, which
 * is larger than the MTU, and a warning of the device debug category says so, instead of an info message claiming
 * that the packet size fits. The debug output goes to stderr, which GLib only matches for a subprocess. */

static void
mtu_below_minimum_test (void)
{
	if (g_test_subprocess ()) {
		TestPacketData pass = {0};
		TestPacketData fail = {0};

		g_assert_true (arv_debug_enable ("device:2"));

		fail.fail_all = TRUE;

		/* An MTU fitting packet sizes: no warning */
		g_assert_cmpuint (search (8228, 1500, TRUE, NULL), ==, 1500);

		/* A larger packet size is lowered to the smallest one, with or without test packets */
		g_assert_cmpuint (search (8228, 200, TRUE, NULL), ==, MIN_SIZE);
		g_assert_cmpuint (search (8228, 200, TRUE, &pass), ==, MIN_SIZE);
		g_assert_cmpuint (search (8228, 200, FALSE, &pass), ==, MIN_SIZE);
		g_assert_cmpuint (search (8228, 200, TRUE, &fail), ==, MIN_SIZE);
		g_assert_cmpuint (search (8228, 200, FALSE, &fail), ==, MIN_SIZE);

		/* The smallest packet size is kept, and doesn't fit either */
		g_assert_cmpuint (search (MIN_SIZE, MIN_SIZE - 1, TRUE, NULL), ==, MIN_SIZE);
		return;
	}

	g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed ();
	g_test_trap_assert_stderr ("*Packet size lowered from 8228 to 1500 bytes, to fit in the interface MTU (1500 bytes)*");
	g_test_trap_assert_stderr ("*interface MTU (200 bytes) is smaller than the smallest packet size of the device"
				   " (220 bytes)*");
	g_test_trap_assert_stderr ("*interface MTU (219 bytes) is smaller than the smallest packet size of the device"
				   " (220 bytes)*");
	g_test_trap_assert_stderr_unmatched ("*to fit in the interface MTU (200 bytes)*");
	g_test_trap_assert_stderr_unmatched ("*MTU (1500 bytes) is smaller*");
}

static void
search_error_test (void)
{
	TestPacketData data = {0};
	GError *error = NULL;
	guint packet_size;

	data.error = TRUE;

	packet_size = arv_gv_device_search_packet_size (8228, MIN_SIZE, MAX_SIZE, INC, 1500, TRUE,
							test_packet, &data, &error);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT);
	g_assert_cmpuint (packet_size, ==, 1500);
	g_clear_error (&error);

	packet_size = arv_gv_device_search_packet_size (8228, MIN_SIZE, MAX_SIZE, INC, 0, FALSE,
							test_packet, &data, &error);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT);
	g_assert_cmpuint (packet_size, ==, 8228);
	g_clear_error (&error);
}

static GInetAddress *
interface_inet_address (ArvNetworkInterface *iface)
{
	GSocketAddress *socket_address;
	GInetAddress *inet_address;

	socket_address = g_socket_address_new_from_native (arv_network_interface_get_addr (iface),
							   sizeof (struct sockaddr));
	g_assert_true (G_IS_INET_SOCKET_ADDRESS (socket_address));

	inet_address = g_object_ref (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (socket_address)));
	g_object_unref (socket_address);

	return inet_address;
}

static void
interface_mtu_test (void)
{
	GList *ifaces;
	GList *iter;
	GInetAddress *address;
	gboolean has_loopback = FALSE;
	guint n_others = 0;
	guint n_others_with_mtu = 0;

	ifaces = arv_enumerate_network_interfaces ();
	if (ifaces == NULL) {
		g_test_skip ("No network interface");
		return;
	}

	for (iter = ifaces; iter != NULL; iter = iter->next) {
		ArvNetworkInterface *iface = iter->data;
		GList *first_iter;
		char *address_string;
		guint mtu;

		mtu = arv_network_interface_get_mtu (iface);
		address = interface_inet_address (iface);
		address_string = g_inet_address_to_string (address);

		g_test_message ("%s (%s): MTU = %u%s", arv_network_interface_get_name (iface), address_string, mtu,
				mtu == 0 ? " (unknown)" : "");

		/* A sane MTU, or 0 if unknown, never a value like the 0xffffffff of the Windows loopback interface */
		g_assert_true (mtu == 0 ||
			       (mtu >= ARV_NETWORK_MINIMUM_MTU && mtu <= ARV_NETWORK_MAXIMUM_MTU));

		/* The lookup by address returns the MTU of the first interface owning the address */
		for (first_iter = ifaces; first_iter != iter; first_iter = first_iter->next) {
			GInetAddress *first_address = interface_inet_address (first_iter->data);
			gboolean is_equal = g_inet_address_equal (first_address, address);

			g_object_unref (first_address);
			if (is_equal)
				break;
		}
		g_assert_cmpuint (arv_network_get_interface_mtu_by_address (address), ==,
				  arv_network_interface_get_mtu (first_iter->data));

		if (arv_network_interface_is_loopback (iface)) {
			has_loopback = TRUE;
		} else {
			n_others++;
			if (mtu > 0)
				n_others_with_mtu++;
		}

		g_free (address_string);
		g_object_unref (address);
	}

	g_list_free_full (ifaces, (GDestroyNotify) arv_network_interface_free);

	if (!has_loopback)
		g_test_message ("No loopback interface enumerated");

	/* Real interfaces report their MTU */
	if (n_others > 0)
		g_assert_cmpuint (n_others_with_mtu, >, 0);

	/* No interface owns this documentation address (RFC 5737) */
	address = g_inet_address_new_from_string ("192.0.2.1");
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (address), ==, 0);
	g_object_unref (address);
}

/* Test hook giving the loopback interface a known MTU, for the end to end tests with the fake GigE camera (fakegvmtu):
 * it must only affect loopback addresses, and be undone. */

static void
loopback_mtu_override_test (void)
{
	GInetAddress *loopback;
	GInetAddress *other_loopback;
	GInetAddress *test_net;
	GList *ifaces;
	GList *iter;
	guint loopback_mtu;
	guint other_loopback_mtu;

	loopback = g_inet_address_new_from_string ("127.0.0.1");
	other_loopback = g_inet_address_new_from_string ("127.1.2.3");
	test_net = g_inet_address_new_from_string ("192.0.2.1");

	loopback_mtu = arv_network_get_interface_mtu_by_address (loopback);
	other_loopback_mtu = arv_network_get_interface_mtu_by_address (other_loopback);
	g_test_message ("Loopback MTU = %u%s", loopback_mtu, loopback_mtu == 0 ? " (unknown)" : "");

	arv_network_set_loopback_mtu_override (1500);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (loopback), ==, 1500);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (other_loopback), ==, 1500);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (test_net), ==, 0);

	/* Other interfaces keep their MTU */
	ifaces = arv_enumerate_network_interfaces ();
	for (iter = ifaces; iter != NULL; iter = iter->next) {
		GInetAddress *address = interface_inet_address (iter->data);

		if (!g_inet_address_get_is_loopback (address)) {
			guint mtu;

			arv_network_set_loopback_mtu_override (0);
			mtu = arv_network_get_interface_mtu_by_address (address);
			arv_network_set_loopback_mtu_override (1500);
			g_assert_cmpuint (arv_network_get_interface_mtu_by_address (address), ==, mtu);
		}
		g_object_unref (address);
	}
	g_list_free_full (ifaces, (GDestroyNotify) arv_network_interface_free);

	/* Like interface MTUs, a value an IPv4 link can't have is ignored */
	arv_network_set_loopback_mtu_override (ARV_NETWORK_MAXIMUM_MTU + 1);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (loopback), ==, loopback_mtu);

	arv_network_set_loopback_mtu_override (9000);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (loopback), ==, 9000);

	arv_network_set_loopback_mtu_override (0);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (loopback), ==, loopback_mtu);
	g_assert_cmpuint (arv_network_get_interface_mtu_by_address (other_loopback), ==, other_loopback_mtu);

	g_object_unref (loopback);
	g_object_unref (other_loopback);
	g_object_unref (test_net);
}

int
main (int argc, char *argv[])
{
	int result;

	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/gvpacketsize/mtu-reassembly", mtu_reassembly_test);
	g_test_add_func ("/gvpacketsize/mtu-no-test-packet", mtu_no_test_packet_test);
	g_test_add_func ("/gvpacketsize/mtu-dropped", mtu_dropped_test);
	g_test_add_func ("/gvpacketsize/mtu-unknown", mtu_unknown_test);
	g_test_add_func ("/gvpacketsize/mtu-alignment", mtu_alignment_test);
	g_test_add_func ("/gvpacketsize/mtu-below-minimum", mtu_below_minimum_test);
	g_test_add_func ("/gvpacketsize/search-error", search_error_test);
	g_test_add_func ("/gvpacketsize/interface-mtu", interface_mtu_test);
	g_test_add_func ("/gvpacketsize/loopback-mtu-override", loopback_mtu_override_test);

	result = g_test_run ();

	arv_shutdown ();

	return result;
}
