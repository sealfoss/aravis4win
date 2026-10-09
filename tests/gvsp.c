/* SPDX-License-Identifier:Unlicense */

/* GVSP packet accessors of the private GigE Vision headers.
 *
 * G_LIKELY() and G_UNLIKELY() return an integer truth value in optimized GCC and Clang builds, but expand to their bare
 * argument without optimization and with MSVC. A misuse like "G_LIKELY (leader) != NULL", which compares that truth
 * value with a pointer, was therefore only reported by optimized GCC and Clang builds, as a warning. The private GVSP
 * and GVCP headers are compiled here with stricter definitions, whose result is a truth value that can't be compared
 * with a pointer: any such misuse fails to build, with every compiler and every optimization level.
 *
 * The leader packets are built like arv_gvsp_packet_new_image_leader() does, from the structures of arvgvspprivate.h.
 * No camera, no network. */

#include <glib.h>
#include <string.h>
#include <arv.h>
#include "../src/arvdebugprivate.h"

#undef G_LIKELY
#undef G_UNLIKELY
#define G_LIKELY(expr)		(!!(expr) + 0.0)
#define G_UNLIKELY(expr)	(!!(expr) + 0.0)

#include "../src/arvgvspprivate.h"
#include "../src/arvgvcpprivate.h"

#define PACKET_BUFFER_SIZE	256

static size_t
build_packet (guint8 *buffer, ArvGvspContentType content_type, gboolean extended_ids, guint64 timestamp)
{
	ArvGvspPacket *packet = (ArvGvspPacket *) buffer;
	ArvGvspLeader *leader;
	size_t size;

	memset (buffer, 0, PACKET_BUFFER_SIZE);

	packet->status = g_htons (ARV_GVSP_PACKET_STATUS_SUCCESS);

	if (extended_ids) {
		ArvGvspExtendedHeader *header = (ArvGvspExtendedHeader *) &packet->header;

		header->packet_infos = g_htonl (ARV_GVSP_PACKET_EXTENDED_ID_MODE_MASK |
						((content_type << ARV_GVSP_PACKET_INFOS_CONTENT_TYPE_POS) &
						 ARV_GVSP_PACKET_INFOS_CONTENT_TYPE_MASK));
		header->frame_id = GUINT64_TO_BE (G_GUINT64_CONSTANT (0x100000002));
		header->packet_id = g_htonl (0);
		leader = (ArvGvspLeader *) &header->data;
		size = sizeof (ArvGvspPacket) + sizeof (ArvGvspExtendedHeader) + sizeof (ArvGvspImageLeader);
	} else {
		ArvGvspHeader *header = (ArvGvspHeader *) &packet->header;

		header->frame_id = g_htons (2);
		header->packet_infos = g_htonl ((content_type << ARV_GVSP_PACKET_INFOS_CONTENT_TYPE_POS) &
						ARV_GVSP_PACKET_INFOS_CONTENT_TYPE_MASK);
		leader = (ArvGvspLeader *) &header->data;
		size = sizeof (ArvGvspPacket) + sizeof (ArvGvspHeader) + sizeof (ArvGvspImageLeader);
	}

	g_assert_cmpuint (size, <=, PACKET_BUFFER_SIZE);

	leader->payload_type = g_htons (ARV_BUFFER_PAYLOAD_TYPE_IMAGE);
	leader->timestamp_high = g_htonl ((guint32) (timestamp >> 32));
	leader->timestamp_low = g_htonl ((guint32) (timestamp & 0xffffffff));

	return size;
}

static void
leader_timestamp_test (void)
{
	static const guint64 timestamps[] = {
		G_GUINT64_CONSTANT (0),
		G_GUINT64_CONSTANT (1),
		G_GUINT64_CONSTANT (0x0123456789abcdef),
		G_GUINT64_CONSTANT (0xfedcba9876543210)
	};
	guint8 buffer[PACKET_BUFFER_SIZE];
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS (timestamps); i++) {
		const ArvGvspPacket *packet = (const ArvGvspPacket *) buffer;
		size_t size;

		size = build_packet (buffer, ARV_GVSP_CONTENT_TYPE_LEADER, FALSE, timestamps[i]);
		g_assert_cmpint (arv_gvsp_packet_get_content_type (packet, size), ==, ARV_GVSP_CONTENT_TYPE_LEADER);
		g_assert_false (arv_gvsp_packet_has_extended_ids (packet, size));
		g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, size), ==, timestamps[i]);

		size = build_packet (buffer, ARV_GVSP_CONTENT_TYPE_LEADER, TRUE, timestamps[i]);
		g_assert_cmpint (arv_gvsp_packet_get_content_type (packet, size), ==, ARV_GVSP_CONTENT_TYPE_LEADER);
		g_assert_true (arv_gvsp_packet_has_extended_ids (packet, size));
		g_assert_cmpuint (arv_gvsp_packet_get_frame_id (packet, size), ==, G_GUINT64_CONSTANT (0x100000002));
		g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, size), ==, timestamps[i]);
	}
}

static void
leader_timestamp_invalid_test (void)
{
	guint64 timestamp = G_GUINT64_CONSTANT (0x0123456789abcdef);
	guint8 buffer[PACKET_BUFFER_SIZE];
	const ArvGvspPacket *packet = (const ArvGvspPacket *) buffer;
	size_t size;

	/* No packet */
	g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (NULL, 0), ==, 0);

	/* Packets too short for their header */
	build_packet (buffer, ARV_GVSP_CONTENT_TYPE_LEADER, FALSE, timestamp);
	g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, sizeof (ArvGvspPacket)), ==, 0);
	g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, sizeof (ArvGvspPacket) +
								sizeof (ArvGvspHeader) - 1), ==, 0);
	build_packet (buffer, ARV_GVSP_CONTENT_TYPE_LEADER, TRUE, timestamp);
	g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, sizeof (ArvGvspPacket) +
								sizeof (ArvGvspExtendedHeader) - 1), ==, 0);

	/* Other packet types */
	size = build_packet (buffer, ARV_GVSP_CONTENT_TYPE_PAYLOAD, FALSE, timestamp);
	g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, size), ==, 0);
	size = build_packet (buffer, ARV_GVSP_CONTENT_TYPE_TRAILER, TRUE, timestamp);
	g_assert_cmpuint (arv_gvsp_leader_packet_get_timestamp (packet, size), ==, 0);
}

int
main (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/gvsp/leader-timestamp", leader_timestamp_test);
	g_test_add_func ("/gvsp/leader-timestamp-invalid", leader_timestamp_invalid_test);

	return g_test_run ();
}
