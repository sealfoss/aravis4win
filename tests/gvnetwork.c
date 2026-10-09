/* SPDX-License-Identifier:Unlicense */

/* Network helpers of the GigE Vision code:
 *
 * - subnet and host address checks, used to reach a discovered device through the interface owning its subnet, and to
 *   decide whether its stream can be captured by a packet socket;
 * - extraction of the UDP payload from a frame captured by a packet socket, which must reject IP fragments and
 *   truncated captures instead of returning a truncated GVSP packet, and tell them from unrelated traffic.
 *
 * Frames are built from IPv4, UDP and Ethernet header layouts. No camera, no network traffic. */

#include <glib.h>
#include <string.h>
#include <arv.h>
#include "../src/arvnetworkprivate.h"

static GInetAddress *
inet_address (const char *string)
{
	GInetAddress *address = g_inet_address_new_from_string (string);

	g_assert (G_IS_INET_ADDRESS (address));

	return address;
}

static gboolean
is_on_subnet (const char *address_string, const char *interface_string, const char *netmask_string)
{
	GInetAddress *address = inet_address (address_string);
	GInetAddress *interface_address = inet_address (interface_string);
	GInetAddress *netmask = netmask_string != NULL ? inet_address (netmask_string) : NULL;
	gboolean result;

	result = arv_network_address_is_on_subnet (address, interface_address, netmask);

	g_object_unref (address);
	g_object_unref (interface_address);
	g_clear_object (&netmask);

	return result;
}

static void
subnet_test (void)
{
	g_assert_true (is_on_subnet ("127.0.0.1", "127.0.0.1", "255.0.0.0"));
	g_assert_true (is_on_subnet ("127.1.2.3", "127.0.0.1", "255.0.0.0"));
	g_assert_false (is_on_subnet ("128.0.0.1", "127.0.0.1", "255.0.0.0"));

	/* A device on the host loopback address, seen from another interface */
	g_assert_false (is_on_subnet ("127.0.0.1", "192.168.1.10", "255.255.255.0"));
	g_assert_false (is_on_subnet ("127.0.0.1", "172.17.0.1", "255.255.0.0"));

	g_assert_true (is_on_subnet ("192.168.1.50", "192.168.1.10", "255.255.255.0"));
	g_assert_true (is_on_subnet ("192.168.1.255", "192.168.1.10", "255.255.255.0"));
	g_assert_false (is_on_subnet ("192.168.2.50", "192.168.1.10", "255.255.255.0"));
	g_assert_true (is_on_subnet ("169.254.20.20", "169.254.10.10", "255.255.0.0"));
	g_assert_true (is_on_subnet ("10.1.2.3", "10.0.0.1", "255.0.0.0"));
	g_assert_true (is_on_subnet ("192.168.71.1", "192.168.68.58", "255.255.252.0"));
	g_assert_false (is_on_subnet ("192.168.72.1", "192.168.68.58", "255.255.252.0"));

	/* Point to point interface */
	g_assert_true (is_on_subnet ("10.8.0.6", "10.8.0.6", "255.255.255.255"));
	g_assert_false (is_on_subnet ("10.8.0.5", "10.8.0.6", "255.255.255.255"));

	/* Unknown netmask */
	g_assert_false (is_on_subnet ("192.168.1.50", "192.168.1.10", NULL));
	g_assert_false (is_on_subnet ("192.168.1.10", "192.168.1.10", "0.0.0.0"));

	/* IPv6 is not supported */
	g_assert_false (is_on_subnet ("::1", "127.0.0.1", "255.0.0.0"));
	g_assert_false (is_on_subnet ("::1", "::1", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
}

static GInetAddress *
ipv4_address_from_sockaddr (struct sockaddr *sockaddr)
{
	return g_inet_address_new_from_bytes ((const guint8 *) &((struct sockaddr_in *) sockaddr)->sin_addr,
					      G_SOCKET_FAMILY_IPV4);
}

static void
interfaces_test (void)
{
	GInetAddress *loopback;
	GInetAddress *test_net;
	gboolean test_net_is_host_address = FALSE;
	GList *ifaces;
	GList *iter;

	ifaces = arv_enumerate_network_interfaces ();

	for (iter = ifaces; iter != NULL; iter = iter->next) {
		struct sockaddr *addr = arv_network_interface_get_addr (iter->data);
		struct sockaddr *netmask = arv_network_interface_get_netmask (iter->data);
		GInetAddress *address;
		char *address_string;

		if (addr == NULL || addr->sa_family != AF_INET)
			continue;

		address = ipv4_address_from_sockaddr (addr);
		address_string = g_inet_address_to_string (address);

		g_test_message ("%s: %s", arv_network_interface_get_name (iter->data), address_string);

		/* Every interface address belongs to the host, and to the subnet of its interface */
		g_assert_true (arv_network_is_host_address (address));

		if (netmask != NULL) {
			GInetAddress *netmask_address = ipv4_address_from_sockaddr (netmask);

			if (!g_inet_address_get_is_any (netmask_address))
				g_assert_true (arv_network_address_is_on_subnet (address, address, netmask_address));

			g_object_unref (netmask_address);
		}

		if (g_strcmp0 (address_string, "192.0.2.1") == 0)
			test_net_is_host_address = TRUE;

		g_free (address_string);
		g_object_unref (address);
	}

	g_list_free_full (ifaces, (GDestroyNotify) arv_network_interface_free);

	loopback = inet_address ("127.0.0.1");
	g_assert_true (arv_network_is_host_address (loopback));
	g_object_unref (loopback);

	loopback = inet_address ("127.1.2.3");
	g_assert_true (arv_network_is_host_address (loopback));
	g_object_unref (loopback);

	/* TEST-NET-1 documentation address, not used on real networks */
	test_net = inet_address ("192.0.2.1");
	if (!test_net_is_host_address)
		g_assert_false (arv_network_is_host_address (test_net));
	g_object_unref (test_net);
}

/* Ethernet, IPv4 and UDP header sizes */
#define ETH_SIZE	14
#define IP_SIZE		20
#define UDP_SIZE	8

#define FRAME_BUFFER_SIZE	2048

#define IP_DONT_FRAGMENT	0x4000
#define IP_MORE_FRAGMENTS	0x2000

static void
set_be16 (guint8 *bytes, guint value)
{
	bytes[0] = (value >> 8) & 0xff;
	bytes[1] = value & 0xff;
}

/* Builds an Ethernet frame carrying an IPv4 packet whose payload starts with a UDP header, and returns its size.
 * @ip_header_size: 20 bytes, more with options
 * @ip_payload_size: size of the IPv4 payload in this packet, UDP header included
 * @udp_size: UDP length field, the size of the whole datagram, larger than @ip_payload_size for a first fragment
 * @fragment: flags and fragment offset field of the IPv4 header */

static size_t
build_frame (guint8 *frame, size_t ip_header_size, size_t ip_payload_size, size_t udp_size, guint fragment)
{
	guint8 *ip = frame + ETH_SIZE;
	guint8 *udp = ip + ip_header_size;
	size_t i;

	g_assert_cmpuint (ETH_SIZE + ip_header_size + ip_payload_size, <=, FRAME_BUFFER_SIZE);

	memset (frame, 0, FRAME_BUFFER_SIZE);

	/* Destination and source MAC addresses, EtherType */
	for (i = 0; i < 12; i++)
		frame[i] = (guint8) (0x10 + i);
	set_be16 (frame + 12, 0x0800);

	ip[0] = (guint8) (0x40 | (ip_header_size / 4));
	set_be16 (ip + 2, (guint) (ip_header_size + ip_payload_size));
	set_be16 (ip + 4, 0x1234);
	set_be16 (ip + 6, fragment);
	ip[8] = 64;
	ip[9] = 17;
	ip[12] = 192; ip[13] = 168; ip[14] = 1; ip[15] = 50;
	ip[16] = 192; ip[17] = 168; ip[18] = 1; ip[19] = 10;

	set_be16 (udp + 0, 20202);
	set_be16 (udp + 2, 50000);
	set_be16 (udp + 4, (guint) udp_size);

	for (i = UDP_SIZE; i < ip_payload_size; i++)
		udp[i] = (guint8) (i & 0xff);

	return ETH_SIZE + ip_header_size + ip_payload_size;
}

/* @frame carries a whole UDP datagram, whose payload starts at @payload_offset */

static void
assert_udp_datagram (const guint8 *frame, size_t frame_size, size_t payload_offset, size_t payload_size)
{
	const void *payload;
	gboolean is_incomplete = TRUE;
	size_t size;

	payload = arv_network_ethernet_frame_get_udp_payload (frame, frame_size, &size, &is_incomplete);
	g_assert_true (payload == frame + payload_offset);
	g_assert_cmpuint (size, ==, payload_size);
	g_assert_false (is_incomplete);

	/* is_incomplete is optional */
	payload = arv_network_ethernet_frame_get_udp_payload (frame, frame_size, &size, NULL);
	g_assert_true (payload == frame + payload_offset);
	g_assert_cmpuint (size, ==, payload_size);
}

/* @frame carries only a part of an IPv4 UDP datagram: an IP fragment, or a capture shorter than its packet. A packet
 * socket stream ignores it, and warns that the packet size may be larger than the MTU. */

static void
assert_incomplete_udp_datagram (const guint8 *frame, size_t frame_size)
{
	gboolean is_incomplete = FALSE;
	size_t size = 1;

	g_assert_null (arv_network_ethernet_frame_get_udp_payload (frame, frame_size, &size, &is_incomplete));
	g_assert_cmpuint (size, ==, 0);
	g_assert_true (is_incomplete);

	size = 1;
	g_assert_null (arv_network_ethernet_frame_get_udp_payload (frame, frame_size, &size, NULL));
	g_assert_cmpuint (size, ==, 0);
}

/* @frame carries neither a UDP datagram nor a part of one, or a malformed one: not a packet of the stream, that a
 * packet socket stream silently drops */

static void
assert_not_udp_datagram (const guint8 *frame, size_t frame_size)
{
	gboolean is_incomplete = TRUE;
	size_t size = 1;

	g_assert_null (arv_network_ethernet_frame_get_udp_payload (frame, frame_size, &size, &is_incomplete));
	g_assert_cmpuint (size, ==, 0);
	g_assert_false (is_incomplete);

	size = 1;
	g_assert_null (arv_network_ethernet_frame_get_udp_payload (frame, frame_size, &size, NULL));
	g_assert_cmpuint (size, ==, 0);
}

static void
ethernet_frame_test (void)
{
	guint8 frame[FRAME_BUFFER_SIZE];
	size_t frame_size;

	/* Whole datagram */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE + 1000, 0);
	assert_udp_datagram (frame, frame_size, ETH_SIZE + IP_SIZE + UDP_SIZE, 1000);

	/* Don't fragment flag, as set for GVSP packets, is not a fragment */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE + 1000, IP_DONT_FRAGMENT);
	assert_udp_datagram (frame, frame_size, ETH_SIZE + IP_SIZE + UDP_SIZE, 1000);

	/* IPv4 options */
	frame_size = build_frame (frame, IP_SIZE + 8, UDP_SIZE + 500, UDP_SIZE + 500, 0);
	assert_udp_datagram (frame, frame_size, ETH_SIZE + IP_SIZE + 8 + UDP_SIZE, 500);

	/* Empty datagram */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE, UDP_SIZE, 0);
	assert_udp_datagram (frame, frame_size, ETH_SIZE + IP_SIZE + UDP_SIZE, 0);

	/* Ethernet padding after a small datagram */
	build_frame (frame, IP_SIZE, UDP_SIZE + 4, UDP_SIZE + 4, 0);
	assert_udp_datagram (frame, 60, ETH_SIZE + IP_SIZE + UDP_SIZE, 4);

	/* First fragment of an 8192 bytes payload over a 1500 bytes MTU: it only carries the first 1472 bytes.
	 * Returning them as a whole GVSP packet would silently truncate it. */
	frame_size = build_frame (frame, IP_SIZE, 1480, UDP_SIZE + 8192, IP_MORE_FRAGMENTS);
	assert_incomplete_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, 1480, UDP_SIZE + 8192, IP_MORE_FRAGMENTS | IP_DONT_FRAGMENT);
	assert_incomplete_udp_datagram (frame, frame_size);

	/* Following fragments, offset in 8 bytes units, down to a last one smaller than a UDP header */
	frame_size = build_frame (frame, IP_SIZE, 1480, UDP_SIZE + 8192, IP_MORE_FRAGMENTS | (1480 / 8));
	assert_incomplete_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, 1000, UDP_SIZE + 8192, 5 * 1480 / 8);
	assert_incomplete_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, 4, UDP_SIZE + 8196, 1025);
	assert_incomplete_udp_datagram (frame, frame_size);

	/* Captures shorter than the IPv4 packet */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE + 1000, 0);
	assert_incomplete_udp_datagram (frame, frame_size - 1);
	assert_incomplete_udp_datagram (frame, ETH_SIZE + IP_SIZE + UDP_SIZE);
	assert_incomplete_udp_datagram (frame, ETH_SIZE + IP_SIZE);

	/* Captures shorter than an IPv4 header */
	assert_not_udp_datagram (frame, ETH_SIZE + IP_SIZE - 1);
	assert_not_udp_datagram (frame, 0);
	assert_not_udp_datagram (NULL, frame_size);

	/* UDP length larger than the IPv4 payload */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE + 1001, 0);
	assert_not_udp_datagram (frame, frame_size);

	/* UDP length smaller than the IPv4 payload, or than the UDP header */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE + 900, 0);
	assert_udp_datagram (frame, frame_size, ETH_SIZE + IP_SIZE + UDP_SIZE, 900);
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE - 1, 0);
	assert_not_udp_datagram (frame, frame_size);

	/* IPv4 packet length shorter than its headers */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 1000, UDP_SIZE + 1000, 0);
	set_be16 (frame + ETH_SIZE + 2, IP_SIZE + UDP_SIZE - 1);
	assert_not_udp_datagram (frame, frame_size);

	/* Other traffic, like the frames of any interface a packet socket created for a protocol captures before it is
	 * bound and filtered, is not mistaken for incomplete GVSP packets */
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 100, UDP_SIZE + 100, 0);
	set_be16 (frame + 12, 0x86dd);	/* IPv6 */
	assert_not_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 100, UDP_SIZE + 100, 0);
	set_be16 (frame + 12, 0x0806);	/* ARP */
	assert_not_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 100, UDP_SIZE + 100, 0);
	frame[ETH_SIZE] = 0x65;		/* IP version 6 */
	assert_not_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 100, UDP_SIZE + 100, 0);
	frame[ETH_SIZE] = 0x44;		/* IPv4 header shorter than 20 bytes */
	assert_not_udp_datagram (frame, frame_size);
	frame_size = build_frame (frame, IP_SIZE, UDP_SIZE + 100, UDP_SIZE + 100, 0);
	frame[ETH_SIZE + 9] = 6;	/* TCP */
	assert_not_udp_datagram (frame, frame_size);
	assert_not_udp_datagram (frame, frame_size - 1);
	frame_size = build_frame (frame, IP_SIZE, 1480, UDP_SIZE + 8192, IP_MORE_FRAGMENTS);
	frame[ETH_SIZE + 9] = 1;	/* ICMP fragment */
	assert_not_udp_datagram (frame, frame_size);
}

int
main (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/gvnetwork/subnet", subnet_test);
	g_test_add_func ("/gvnetwork/interfaces", interfaces_test);
	g_test_add_func ("/gvnetwork/ethernet-frame", ethernet_frame_test);

	return g_test_run ();
}
