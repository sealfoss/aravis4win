/* SPDX-License-Identifier:Unlicense */

/* USB3 Vision camera emulation for the fake libusb, backed by ArvFakeCamera.
 *
 * Per the protocol-code rule of Aravis's README, this is written from Aravis's own code only: the register map comes
 * from src/arvuvcpprivate.h, the packet layouts from the structures of src/arvuvcpprivate.h and
 * src/arvuvspprivate.h, the descriptor layout from src/arvuvinterfaceprivate.h, and the behaviour from the way
 * src/arvuvdevice.c and src/arvuvstream.c read them. Values Aravis does not read are left 0 and say so.
 *
 * Address map, as seen through the control channel:
 *
 *  - ABRM registers Aravis reads, at their offsets from arvuvcpprivate.h: [0x0000, 0x0044) and [0x01c4, 0x01f0),
 *    read-only (Aravis never writes them);
 *  - SBRM, SIRM and manifest table blocks at FAKE_USB_CAMERA_SBRM/SIRM/MANIFEST_ADDRESS;
 *  - everything else below ARV_FAKE_CAMERA_MEMORY_SIZE is ArvFakeCamera's register map (the features of
 *    arv-fake-camera.xml live there, none of them overlaps the ABRM ranges above);
 *  - ArvFakeCamera's GenICam XML follows at ARV_FAKE_CAMERA_MEMORY_SIZE, and the manifest entry points to it. */

#include "fakeusbprivate.h"
#include <arvuvcpprivate.h>
#include <arvuvspprivate.h>
#include <arvuvinterfaceprivate.h>
#include <arvbufferprivate.h>
#include <string.h>

#define FAKE_USB_CAMERA_ABRM_SIZE		ARV_ABRM_RESERVED
#define FAKE_USB_CAMERA_SBRM_SIZE		ARV_SBRM_RESERVED
#define FAKE_USB_CAMERA_SIRM_SIZE		(ARV_SIRM_MAX_TRAILER_SIZE + 4)
#define FAKE_USB_CAMERA_MANIFEST_SIZE		(8 + sizeof (ArvUvcpManifestEntry))
#define FAKE_USB_CAMERA_N_FRAME_RECORDS		4096

/* The device info block that follows the control interface descriptor: Aravis reads string indices in it, up to
 * ARV_UV_INTERFACE_SERIAL_NUMBER_INDEX_OFFSET (arvuvinterfaceprivate.h) */
#define FAKE_USB_CAMERA_INFO_SIZE		(ARV_UV_INTERFACE_SERIAL_NUMBER_INDEX_OFFSET + 1)

/* GenDC containers, as Aravis parses them (arvuvspprivate.h, arvuvstream.c): a container header followed by the
 * offsets of its components, each component header followed by the offsets of its parts. The accessors
 * arv_uvsp_packet_get_gendc_componentoffset () and arv_uvsp_packet_get_gendc_partoffset () read these tables at
 * fixed offsets, which are the sizes of the headers. */
#define FAKE_USB_GENDC_COMPONENT_OFFSETS	sizeof (ArvUvspGenDCContainerHeader)
#define FAKE_USB_GENDC_COMPONENT		(FAKE_USB_GENDC_COMPONENT_OFFSETS + sizeof (guint64))
#define FAKE_USB_GENDC_PART_OFFSETS		(FAKE_USB_GENDC_COMPONENT + sizeof (ArvUvspGenDCComponentHeader))
#define FAKE_USB_GENDC_PART			(FAKE_USB_GENDC_PART_OFFSETS + sizeof (guint64))
#define FAKE_USB_GENDC_DESCRIPTOR_SIZE		(FAKE_USB_GENDC_PART + sizeof (ArvUvspGenDCPartHeader))
#define FAKE_USB_GENDC_SIGNATURE		0x43444E47	/* arv_uvsp_packet_is_gendc () */
#define FAKE_USB_GENDC_INTENSITY_TYPE_ID	0x1		/* the component type arvuvstream.c looks for */

G_STATIC_ASSERT (sizeof (ArvUvspGenDCContainerHeader) == 56);	/* arv_uvsp_packet_get_gendc_componentoffset () */
G_STATIC_ASSERT (sizeof (ArvUvspGenDCComponentHeader) == 48);	/* arv_uvsp_packet_get_gendc_partoffset () */

typedef enum {
	FAKE_USB_REGION_NONE,
	FAKE_USB_REGION_ABRM,
	FAKE_USB_REGION_SBRM,
	FAKE_USB_REGION_SIRM,
	FAKE_USB_REGION_MANIFEST,
	FAKE_USB_REGION_REGISTERS,
	FAKE_USB_REGION_GENICAM
} FakeUsbRegion;

typedef struct {
	GBytes *ack;
	gint64 due;
} FakeUsbDelayedAck;

typedef struct {
	gboolean valid;
	guint64 frame_id;
	guint32 checksum;
	gsize size;
} FakeUsbFrameRecord;

struct _FakeUsbCamera {
	ArvFakeCamera *camera;
	char *serial_number;
	char *genicam_filename;
	char *guid;

	gboolean geometry_set;
	guint32 width;
	guint32 height;
	ArvPixelFormat pixel_format;

	guint32 response_time_ms;
	guint32 max_cmd_transfer;
	guint32 max_ack_transfer;
	guint alignment_shift;
	gboolean zlp;
	gboolean ack_zlp;
	gboolean pad_final_transfer;
	guint16 payload_type;
	guint64 frame_period_us;
	guint64 next_frame_id;

	guint8 abrm[FAKE_USB_CAMERA_ABRM_SIZE];
	guint8 sbrm[FAKE_USB_CAMERA_SBRM_SIZE];
	guint8 sirm[FAKE_USB_CAMERA_SIRM_SIZE];
	guint8 manifest[FAKE_USB_CAMERA_MANIFEST_SIZE];

	guint n_pending_acks;
	guint16 pending_ack_timeout_ms;
	guint pending_ack_delay_ms;
	guint n_dropped_acks;
	guint n_truncated_acks;
	gsize truncated_ack_bytes;
	guint n_status_overrides;
	guint16 status_override;
	GQueue delayed_acks;

	gboolean was_acquiring;
	gint64 next_frame_time;
	ArvBuffer *frame_buffer;
	guint n_truncated_payloads;
	gsize truncated_bytes;

	guint n_dropped_trailers;

	FakeUsbFrameJob *job;

	FakeUsbFrameRecord frames[FAKE_USB_CAMERA_N_FRAME_RECORDS];
};

/* Little endian accessors: U3V bootstrap registers are little endian (Aravis reads them without conversion) */

static void
_set_u32 (guint8 *p, guint32 value)
{
	value = GUINT32_TO_LE (value);
	memcpy (p, &value, sizeof (value));
}

static void
_set_u64 (guint8 *p, guint64 value)
{
	value = GUINT64_TO_LE (value);
	memcpy (p, &value, sizeof (value));
}

static guint32
_get_u32 (const guint8 *p)
{
	guint32 value;

	memcpy (&value, p, sizeof (value));

	return GUINT32_FROM_LE (value);
}

static void
_delayed_ack_free (gpointer data)
{
	FakeUsbDelayedAck *delayed_ack = data;

	g_bytes_unref (delayed_ack->ack);
	g_free (delayed_ack);
}

static FakeUsbCamera *
_get_camera (FakeUsbDevice *device)
{
	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (device->camera != NULL, NULL);

	return device->camera;
}

static guint32
_scaled_ms_locked (guint32 ms)
{
	guint64 scaled = (guint64) ms * fake_usb_get_time_scale_locked ();

	return (guint32) MIN (scaled, G_MAXUINT32);
}

/* Register blocks */

static gsize
_payload_size (FakeUsbCamera *camera)
{
	gsize payload = arv_fake_camera_get_payload (camera->camera);

	if (camera->payload_type == ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER)
		payload += FAKE_USB_GENDC_DESCRIPTOR_SIZE;

	return payload;
}

static void
_refresh_sirm (FakeUsbCamera *camera)
{
	_set_u32 (camera->sirm + ARV_SIRM_INFO, (guint32) camera->alignment_shift << ARV_SIRM_INFO_ALIGNMENT_SHIFT);
	_set_u64 (camera->sirm + ARV_SIRM_REQ_PAYLOAD_SIZE, _payload_size (camera));
	_set_u32 (camera->sirm + ARV_SIRM_REQ_LEADER_SIZE, sizeof (ArvUvspLeader));
	_set_u32 (camera->sirm + ARV_SIRM_REQ_TRAILER_SIZE, sizeof (ArvUvspTrailer));
}

static void
_update_bootstrap_registers (FakeUsbCamera *camera)
{
	_set_u32 (camera->abrm + ARV_ABRM_MAX_DEVICE_RESPONSE_TIME, _scaled_ms_locked (camera->response_time_ms));
	_set_u32 (camera->sbrm + ARV_SBRM_MAX_CMD_TRANSFER, camera->max_cmd_transfer);
	_set_u32 (camera->sbrm + ARV_SBRM_MAX_ACK_TRANSFER, camera->max_ack_transfer);
	_refresh_sirm (camera);
}

static void
_init_registers (FakeUsbCamera *camera)
{
	ArvUvcpManifestEntry entry;
	size_t genicam_size = 0;

	memset (camera->abrm, 0, sizeof (camera->abrm));
	memset (camera->sbrm, 0, sizeof (camera->sbrm));
	memset (camera->sirm, 0, sizeof (camera->sirm));
	memset (camera->manifest, 0, sizeof (camera->manifest));

	memcpy (camera->abrm + ARV_ABRM_MANUFACTURER_NAME, "Aravis", strlen ("Aravis"));
	_set_u64 (camera->abrm + ARV_ABRM_MANIFEST_TABLE_ADDRESS, FAKE_USB_CAMERA_MANIFEST_ADDRESS);
	_set_u64 (camera->abrm + ARV_ABRM_SBRM_ADDRESS, FAKE_USB_CAMERA_SBRM_ADDRESS);

	_set_u32 (camera->sbrm + ARV_SBRM_NUM_STREAM_CHANNELS, 1);
	_set_u64 (camera->sbrm + ARV_SBRM_SIRM_ADDRESS, FAKE_USB_CAMERA_SIRM_ADDRESS);
	_set_u32 (camera->sbrm + ARV_SBRM_SIRM_LENGTH, FAKE_USB_CAMERA_SIRM_SIZE);

	arv_fake_camera_get_genicam_xml (camera->camera, &genicam_size);

	memset (&entry, 0, sizeof (entry));
	entry.file_version_major = 1;
	entry.schema = GUINT32_TO_LE (ARV_UVCP_SCHEMA_RAW << 10);
	entry.address = GUINT64_TO_LE (ARV_FAKE_CAMERA_MEMORY_SIZE);
	entry.size = GUINT64_TO_LE (genicam_size);

	_set_u64 (camera->manifest, 1);
	memcpy (camera->manifest + 8, &entry, sizeof (entry));

	_update_bootstrap_registers (camera);
}

static FakeUsbRegion
_lookup_region (FakeUsbCamera *camera, guint64 address, guint64 *offset, guint64 *available)
{
	size_t genicam_size = 0;

	*offset = address;
	*available = 0;

	if (address < ARV_ABRM_MODEL_NAME) {
		*available = ARV_ABRM_MODEL_NAME - address;
		return FAKE_USB_REGION_ABRM;
	}
	if (address >= ARV_ABRM_DEVICE_CAPABILITY && address < ARV_ABRM_TIMESTAMP) {
		*available = ARV_ABRM_TIMESTAMP - address;
		return FAKE_USB_REGION_ABRM;
	}
	if (address >= FAKE_USB_CAMERA_SBRM_ADDRESS && address < FAKE_USB_CAMERA_SBRM_ADDRESS + FAKE_USB_CAMERA_SBRM_SIZE) {
		*offset = address - FAKE_USB_CAMERA_SBRM_ADDRESS;
		*available = FAKE_USB_CAMERA_SBRM_SIZE - *offset;
		return FAKE_USB_REGION_SBRM;
	}
	if (address >= FAKE_USB_CAMERA_SIRM_ADDRESS && address < FAKE_USB_CAMERA_SIRM_ADDRESS + FAKE_USB_CAMERA_SIRM_SIZE) {
		*offset = address - FAKE_USB_CAMERA_SIRM_ADDRESS;
		*available = FAKE_USB_CAMERA_SIRM_SIZE - *offset;
		return FAKE_USB_REGION_SIRM;
	}
	if (address >= FAKE_USB_CAMERA_MANIFEST_ADDRESS &&
	    address < FAKE_USB_CAMERA_MANIFEST_ADDRESS + FAKE_USB_CAMERA_MANIFEST_SIZE) {
		*offset = address - FAKE_USB_CAMERA_MANIFEST_ADDRESS;
		*available = FAKE_USB_CAMERA_MANIFEST_SIZE - *offset;
		return FAKE_USB_REGION_MANIFEST;
	}
	if (address < ARV_FAKE_CAMERA_MEMORY_SIZE) {
		*available = (address < ARV_ABRM_DEVICE_CAPABILITY ? ARV_ABRM_DEVICE_CAPABILITY :
			      ARV_FAKE_CAMERA_MEMORY_SIZE) - address;
		return FAKE_USB_REGION_REGISTERS;
	}

	arv_fake_camera_get_genicam_xml (camera->camera, &genicam_size);
	if (address < ARV_FAKE_CAMERA_MEMORY_SIZE + genicam_size) {
		*available = ARV_FAKE_CAMERA_MEMORY_SIZE + genicam_size - address;
		return FAKE_USB_REGION_GENICAM;
	}

	return FAKE_USB_REGION_NONE;
}

/* What the host may write: the SIRM registers Aravis writes in arv_uv_stream_start_acquisition () and
 * arv_uv_stream_stop_acquisition () (control, then the sizes from MAX_LEADER_SIZE to MAX_TRAILER_SIZE), and
 * ArvFakeCamera's registers. The ABRM, SBRM, manifest and GenICam data are read-only. */

static gboolean
_is_writable (FakeUsbRegion region, guint64 offset, guint64 size)
{
	switch (region) {
		case FAKE_USB_REGION_SIRM:
			return (offset >= ARV_SIRM_CONTROL && offset + size <= ARV_SIRM_CONTROL + 4) ||
				(offset >= ARV_SIRM_MAX_LEADER_SIZE && offset + size <= FAKE_USB_CAMERA_SIRM_SIZE);
		case FAKE_USB_REGION_REGISTERS:
			return TRUE;
		default:
			return FALSE;
	}
}

static void
_stream_control_changed_locked (FakeUsbDevice *device, guint32 previous, gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	guint32 control = _get_u32 (camera->sirm + ARV_SIRM_CONTROL);

	if ((previous & ARV_SIRM_CONTROL_STREAM_ENABLE) != 0 && (control & ARV_SIRM_CONTROL_STREAM_ENABLE) == 0) {
		/* Stream disabled: what was not read yet is lost */
		fake_usb_device_flush_endpoint_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);
	} else if ((previous & ARV_SIRM_CONTROL_STREAM_ENABLE) == 0 &&
		   (control & ARV_SIRM_CONTROL_STREAM_ENABLE) != 0) {
		camera->next_frame_time = now;
	}
}

static ArvUvcpStatus
_access_memory_locked (FakeUsbDevice *device, guint64 address, guint32 size, guint8 *buffer, gboolean write,
		       gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	guint32 previous_control;
	guint64 current;
	guint32 left;

	/* Check the whole range first, so that a failed access has no effect */
	for (current = address, left = size; left > 0; ) {
		FakeUsbRegion region;
		guint64 offset, available, n;

		region = _lookup_region (camera, current, &offset, &available);
		if (region == FAKE_USB_REGION_NONE)
			return ARV_UVCP_STATUS_INVALID_ADDRESS;

		n = MIN (left, available);
		if (write && !_is_writable (region, offset, n))
			return ARV_UVCP_STATUS_WRITE_PROTECT;

		current += n;
		left -= n;
	}

	_refresh_sirm (camera);
	previous_control = _get_u32 (camera->sirm + ARV_SIRM_CONTROL);

	for (current = address, left = size; left > 0; ) {
		FakeUsbRegion region;
		guint64 offset, available, n;
		guint8 *block = NULL;

		region = _lookup_region (camera, current, &offset, &available);
		n = MIN (left, available);
		if (region == FAKE_USB_REGION_NONE || n == 0)
			break;

		switch (region) {
			case FAKE_USB_REGION_ABRM:
				block = camera->abrm;
				break;
			case FAKE_USB_REGION_SBRM:
				block = camera->sbrm;
				break;
			case FAKE_USB_REGION_SIRM:
				block = camera->sirm;
				break;
			case FAKE_USB_REGION_MANIFEST:
				block = camera->manifest;
				break;
			default:
				break;
		}

		if (block != NULL) {
			if (write)
				memcpy (block + offset, buffer, n);
			else
				memcpy (buffer, block + offset, n);
		} else if (write) {
			arv_fake_camera_write_memory (camera->camera, offset, n, buffer);
		} else {
			arv_fake_camera_read_memory (camera->camera, offset, n, buffer);
		}

		current += n;
		left -= n;
		buffer += n;
	}

	if (write && _get_u32 (camera->sirm + ARV_SIRM_CONTROL) != previous_control)
		_stream_control_changed_locked (device, previous_control, now);

	return ARV_UVCP_STATUS_SUCCESS;
}

/* Control channel */

static GBytes *
_build_ack (guint16 command, guint16 status, guint16 packet_id, const void *payload, gsize payload_size)
{
	ArvUvcpHeader header;
	guint8 *ack;

	header.magic = GUINT32_TO_LE (ARV_UVCP_MAGIC);
	header.status = GUINT16_TO_LE (status);
	header.command = GUINT16_TO_LE (command);
	header.size = GUINT16_TO_LE (payload_size);
	header.id = GUINT16_TO_LE (packet_id);

	ack = g_malloc (sizeof (header) + payload_size);
	memcpy (ack, &header, sizeof (header));
	if (payload_size > 0)
		memcpy (ack + sizeof (header), payload, payload_size);

	return g_bytes_new_take (ack, sizeof (header) + payload_size);
}

/* An acknowledge on its way to the host, cut short by fake_usb_camera_truncate_acks () */

static void
_push_ack_locked (FakeUsbDevice *device, GBytes *ack)
{
	FakeUsbCamera *camera = device->camera;

	if (camera->n_truncated_acks > 0) {
		gsize size = g_bytes_get_size (ack);
		GBytes *truncated;

		camera->n_truncated_acks--;
		truncated = g_bytes_new_from_bytes (ack, 0, size - MIN (camera->truncated_ack_bytes, size));
		g_bytes_unref (ack);
		ack = truncated;
	}

	fake_usb_device_push_segment_locked (device, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack, camera->ack_zlp,
					     FAKE_USB_SEGMENT_ACK);
}

static void
_send_ack_locked (FakeUsbDevice *device, GBytes *ack, guint16 packet_id, gint64 now)
{
	FakeUsbCamera *camera = device->camera;

	if (camera->n_dropped_acks > 0) {
		camera->n_dropped_acks--;
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_DROPPED_ACKS, 1);
		g_bytes_unref (ack);
		return;
	}

	if (camera->n_pending_acks > 0) {
		ArvUvcpPendingAckInfos infos;

		camera->n_pending_acks--;

		infos.unknown = 0;
		infos.timeout = GUINT16_TO_LE (MIN (_scaled_ms_locked (camera->pending_ack_timeout_ms), G_MAXUINT16));
		_push_ack_locked (device, _build_ack (ARV_UVCP_COMMAND_PENDING_ACK, ARV_UVCP_STATUS_SUCCESS, packet_id,
						      &infos, sizeof (infos)));
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_PENDING_ACKS, 1);

		if (camera->pending_ack_delay_ms > 0) {
			FakeUsbDelayedAck *delayed_ack;
			GList *iter;

			delayed_ack = g_new0 (FakeUsbDelayedAck, 1);
			delayed_ack->ack = ack;
			delayed_ack->due = now + fake_usb_scaled_us_locked ((guint64) camera->pending_ack_delay_ms * 1000);

			for (iter = camera->delayed_acks.head; iter != NULL; iter = iter->next)
				if (((FakeUsbDelayedAck *) iter->data)->due > delayed_ack->due)
					break;
			if (iter != NULL)
				g_queue_insert_before (&camera->delayed_acks, iter, delayed_ack);
			else
				g_queue_push_tail (&camera->delayed_acks, delayed_ack);
			return;
		}
	}

	_push_ack_locked (device, ack);
}

static void
_handle_command_locked (FakeUsbDevice *device, const guint8 *data, gsize size, gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	ArvUvcpHeader header;
	guint16 command, packet_id, status;
	GBytes *ack = NULL;

	if (size < sizeof (ArvUvcpHeader)) {
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);
		return;
	}

	memcpy (&header, data, sizeof (header));
	if (GUINT32_FROM_LE (header.magic) != ARV_UVCP_MAGIC) {
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);
		return;
	}

	command = GUINT16_FROM_LE (header.command);
	packet_id = GUINT16_FROM_LE (header.id);

	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_COMMANDS, 1);

	switch (command) {
		case ARV_UVCP_COMMAND_READ_MEMORY_CMD:
			{
				ArvUvcpReadMemoryCmd cmd;
				guint64 address;
				guint16 read_size;
				guint8 *payload;

				if (size < sizeof (cmd)) {
					fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);
					ack = _build_ack (ARV_UVCP_COMMAND_READ_MEMORY_ACK, ARV_UVCP_STATUS_INVALID_HEADER,
							  packet_id, NULL, 0);
					break;
				}

				memcpy (&cmd, data, sizeof (cmd));
				address = GUINT64_FROM_LE (cmd.infos.address);
				read_size = GUINT16_FROM_LE (cmd.infos.size);

				payload = g_malloc0 (MAX (read_size, 1));
				status = read_size > 0 ?
					_access_memory_locked (device, address, read_size, payload, FALSE, now) :
					ARV_UVCP_STATUS_INVALID_PARAMETER;
				if (camera->n_status_overrides > 0) {
					camera->n_status_overrides--;
					status = camera->status_override;
				}
				if (status != ARV_UVCP_STATUS_SUCCESS)
					fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);

				ack = _build_ack (ARV_UVCP_COMMAND_READ_MEMORY_ACK, status, packet_id, payload,
						  status == ARV_UVCP_STATUS_SUCCESS ? read_size : 0);
				g_free (payload);
			}
			break;
		case ARV_UVCP_COMMAND_WRITE_MEMORY_CMD:
			{
				ArvUvcpWriteMemoryCmd cmd;
				ArvUvcpWriteMemoryAckInfos infos;
				guint64 address;
				gsize write_size;
				guint8 *payload;

				if (size < sizeof (cmd) ||
				    GUINT16_FROM_LE (header.size) < sizeof (ArvUvcpWriteMemoryCmdInfos)) {
					fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);
					ack = _build_ack (ARV_UVCP_COMMAND_WRITE_MEMORY_ACK, ARV_UVCP_STATUS_INVALID_HEADER,
							  packet_id, NULL, 0);
					break;
				}

				memcpy (&cmd, data, sizeof (cmd));
				address = GUINT64_FROM_LE (cmd.infos.address);
				write_size = GUINT16_FROM_LE (header.size) - sizeof (ArvUvcpWriteMemoryCmdInfos);
				write_size = MIN (write_size, size - sizeof (cmd));

				payload = g_malloc0 (MAX (write_size, 1));
				memcpy (payload, data + sizeof (cmd), write_size);

				status = write_size > 0 ?
					_access_memory_locked (device, address, write_size, payload, TRUE, now) :
					ARV_UVCP_STATUS_INVALID_PARAMETER;
				if (camera->n_status_overrides > 0) {
					camera->n_status_overrides--;
					status = camera->status_override;
				}
				if (status != ARV_UVCP_STATUS_SUCCESS)
					fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);

				infos.unknown = 0;
				infos.bytes_written = GUINT16_TO_LE (status == ARV_UVCP_STATUS_SUCCESS ? write_size : 0);
				ack = _build_ack (ARV_UVCP_COMMAND_WRITE_MEMORY_ACK, status, packet_id,
						  &infos, sizeof (infos));
				g_free (payload);
			}
			break;
		default:
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_INVALID_COMMANDS, 1);
			ack = _build_ack (command | 0x0001, ARV_UVCP_STATUS_NOT_IMPLEMENTED, packet_id, NULL, 0);
			break;
	}

	_send_ack_locked (device, ack, packet_id, now);
}

void
fake_usb_camera_receive_locked (FakeUsbDevice *device, guint8 endpoint, const guint8 *data, gsize size, gint64 now)
{
	if (endpoint == FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT)
		_handle_command_locked (device, data, size, now);
}

/* Stream channel */

static gint64
_frame_period_locked (FakeUsbCamera *camera)
{
	guint32 period_us = 0;

	if (camera->frame_period_us > 0)
		return fake_usb_scaled_us_locked (camera->frame_period_us);

	arv_fake_camera_read_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_ACQUISITION_FRAME_PERIOD_US,
				       &period_us);

	return fake_usb_scaled_us_locked (period_us > 0 ? period_us : G_USEC_PER_SEC);
}

static gboolean
_is_free_running (FakeUsbCamera *camera)
{
	guint32 trigger_mode = 0;

	arv_fake_camera_read_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_TRIGGER_MODE, &trigger_mode);

	return trigger_mode == 0;
}

/* A new frame is produced only while the stream is enabled and acquisition started, when the host is reading and
 * the previous frame has been completely read */

static gboolean
_can_send_frame_locked (FakeUsbDevice *device)
{
	FakeUsbCamera *camera = device->camera;
	FakeUsbEndpointState *state;

	state = fake_usb_device_get_endpoint_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);

	return camera->job == NULL &&
		(_get_u32 (camera->sirm + ARV_SIRM_CONTROL) & ARV_SIRM_CONTROL_STREAM_ENABLE) != 0 &&
		arv_fake_camera_get_acquisition_status (camera->camera) != 0 &&
		!state->halted &&
		g_queue_is_empty (&state->segments) &&
		!g_queue_is_empty (&state->pending);
}

/* Frames are produced in three steps, so that the slow part (ArvFakeCamera's fill pattern, the copy and the checksum)
 * runs without the fake lock: otherwise a free running camera starves the other threads of the lock. */

struct _FakeUsbFrameJob {
	FakeUsbDevice *device;
	guint generation;
	ArvFakeCamera *fake_camera;
	ArvBuffer *buffer;
	guint64 frame_id;
	guint16 payload_type;
	gsize image_size;
	gsize missing_bytes;

	gboolean filled;
	GBytes *payload_bytes;		/* the whole payload as sent: a GenDC container, or the image */
	gsize image_offset;		/* where the image data starts in it */
	guint32 checksum;		/* of the image data sent */
	gsize image_sent;
};

FakeUsbFrameJob *
fake_usb_camera_start_frame_locked (FakeUsbDevice *device, gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	FakeUsbFrameJob *job;
	gsize image_size;

	if (!_can_send_frame_locked (device))
		return NULL;

	if (_is_free_running (camera)) {
		if (now < camera->next_frame_time)
			return NULL;
	} else if (!arv_fake_camera_is_in_software_trigger_mode (camera->camera) ||
		   !arv_fake_camera_check_and_acknowledge_software_trigger (camera->camera)) {
		return NULL;
	}

	camera->next_frame_time = now + _frame_period_locked (camera);

	image_size = arv_fake_camera_get_payload (camera->camera);
	if (image_size == 0)
		return NULL;

	if (camera->frame_buffer == NULL || camera->frame_buffer->priv->allocated_size != image_size) {
		g_clear_object (&camera->frame_buffer);
		camera->frame_buffer = arv_buffer_new_allocate (image_size);
	}

	job = g_new0 (FakeUsbFrameJob, 1);
	job->device = fake_usb_device_ref_locked (device);
	job->generation = device->generation;
	job->fake_camera = g_object_ref (camera->camera);
	job->buffer = g_object_ref (camera->frame_buffer);
	job->frame_id = camera->next_frame_id++;
	job->payload_type = camera->payload_type;
	job->image_size = image_size;

	if (camera->n_truncated_payloads > 0) {
		camera->n_truncated_payloads--;
		job->missing_bytes = camera->truncated_bytes;
	}

	camera->job = job;

	return job;
}

/* A GenDC container with one intensity component of one part, from Aravis's ArvUvspGenDC* structures. Fields
 * Aravis does not read (versions, header types, group, source, region and flow ids) are left 0. */

static void
_fill_gendc_descriptor (guint8 *container, FakeUsbFrameJob *job)
{
	ArvBuffer *buffer = job->buffer;
	ArvUvspGenDCContainerHeader container_header;
	ArvUvspGenDCComponentHeader component_header;
	ArvUvspGenDCPartHeader part_header;
	guint64 offset;

	memset (&container_header, 0, sizeof (container_header));
	container_header.signature = GUINT32_TO_LE (FAKE_USB_GENDC_SIGNATURE);
	container_header.header_size = GUINT32_TO_LE (FAKE_USB_GENDC_COMPONENT);
	container_header.id = GUINT64_TO_LE (job->frame_id);
	container_header.datasize = GUINT64_TO_LE (job->image_size);
	container_header.dataoffset = GUINT64_TO_LE (FAKE_USB_GENDC_DESCRIPTOR_SIZE);
	container_header.descriptorsize = GUINT32_TO_LE (FAKE_USB_GENDC_DESCRIPTOR_SIZE);
	container_header.component_count = GUINT32_TO_LE (1);
	memcpy (container, &container_header, sizeof (container_header));

	offset = GUINT64_TO_LE (FAKE_USB_GENDC_COMPONENT);
	memcpy (container + FAKE_USB_GENDC_COMPONENT_OFFSETS, &offset, sizeof (offset));

	memset (&component_header, 0, sizeof (component_header));
	component_header.flags = 0;	/* valid */
	component_header.header_size = GUINT32_TO_LE (FAKE_USB_GENDC_PART - FAKE_USB_GENDC_COMPONENT);
	component_header.timestamp = GUINT64_TO_LE (buffer->priv->timestamp_ns);
	component_header.type_id = GUINT64_TO_LE (FAKE_USB_GENDC_INTENSITY_TYPE_ID);
	component_header.format = GUINT32_TO_LE (buffer->priv->parts[0].pixel_format);
	component_header.part_count = GUINT16_TO_LE (1);
	memcpy (container + FAKE_USB_GENDC_COMPONENT, &component_header, sizeof (component_header));

	offset = GUINT64_TO_LE (FAKE_USB_GENDC_PART);
	memcpy (container + FAKE_USB_GENDC_PART_OFFSETS, &offset, sizeof (offset));

	memset (&part_header, 0, sizeof (part_header));
	part_header.header_size = GUINT32_TO_LE (sizeof (ArvUvspGenDCPartHeader));
	part_header.format = GUINT32_TO_LE (buffer->priv->parts[0].pixel_format);
	part_header.datasize = GUINT64_TO_LE (job->image_size);
	part_header.dataoffset = GUINT64_TO_LE (FAKE_USB_GENDC_DESCRIPTOR_SIZE);
	part_header.dimension_x = GUINT32_TO_LE (buffer->priv->parts[0].width);
	part_header.dimension_y = GUINT32_TO_LE (buffer->priv->parts[0].height);
	memcpy (container + FAKE_USB_GENDC_PART, &part_header, sizeof (part_header));
}

void
fake_usb_camera_run_frame (FakeUsbFrameJob *job)
{
	ArvBuffer *buffer = job->buffer;
	gsize payload_size, sent;
	guint8 *payload;

	memset (buffer->priv->data, 0, job->image_size);
	arv_fake_camera_fill_buffer (job->fake_camera, buffer, NULL);

	if (buffer->priv->status != ARV_BUFFER_STATUS_SUCCESS)
		return;

	job->image_offset = job->payload_type == ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER ?
		FAKE_USB_GENDC_DESCRIPTOR_SIZE : 0;
	payload_size = job->image_offset + job->image_size;
	sent = payload_size - MIN (job->missing_bytes, payload_size);

	payload = g_malloc0 (MAX (payload_size, 1));
	if (job->image_offset > 0)
		_fill_gendc_descriptor (payload, job);
	memcpy (payload + job->image_offset, buffer->priv->data, job->image_size);

	job->payload_bytes = g_bytes_new_take (payload, sent);
	job->image_sent = sent > job->image_offset ? sent - job->image_offset : 0;
	job->checksum = fake_usb_checksum (payload + job->image_offset, job->image_sent);
	job->filled = TRUE;
}

static void
_queue_frame_locked (FakeUsbDevice *device, FakeUsbFrameJob *job)
{
	FakeUsbCamera *camera = device->camera;
	FakeUsbFrameRecord *record;
	ArvBuffer *buffer = job->buffer;
	ArvUvspLeader leader;
	ArvUvspTrailer trailer;
	gsize sent, offset;
	guint32 chunk_size, chunk_count, transfer1_size, i;
	gsize max_packet_size;

	sent = g_bytes_get_size (job->payload_bytes);

	record = &camera->frames[job->frame_id % FAKE_USB_CAMERA_N_FRAME_RECORDS];
	record->valid = TRUE;
	record->frame_id = job->frame_id;
	record->checksum = job->checksum;
	record->size = job->image_sent;

	max_packet_size = fake_usb_device_get_max_packet_size_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);

	/* Leader, from ArvUvspLeader */
	memset (&leader, 0, sizeof (leader));
	leader.header.magic = GUINT32_TO_LE (ARV_UVSP_LEADER_MAGIC);
	leader.header.size = GUINT16_TO_LE (sizeof (ArvUvspLeader));
	leader.header.frame_id = GUINT64_TO_LE (job->frame_id);
	leader.infos.payload_type = GUINT16_TO_LE (job->payload_type);
	leader.infos.timestamp = GUINT64_TO_LE (buffer->priv->timestamp_ns);
	leader.infos.pixel_format = GUINT32_TO_LE (buffer->priv->parts[0].pixel_format);
	leader.infos.width = GUINT32_TO_LE (buffer->priv->parts[0].width);
	leader.infos.height = GUINT32_TO_LE (buffer->priv->parts[0].height);
	leader.infos.x_offset = GUINT32_TO_LE (buffer->priv->parts[0].x_offset);
	leader.infos.y_offset = GUINT32_TO_LE (buffer->priv->parts[0].y_offset);

	fake_usb_device_push_segment_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
					     g_bytes_new (&leader, sizeof (leader)),
					     camera->zlp && sizeof (leader) % max_packet_size == 0,
					     FAKE_USB_SEGMENT_LEADER);

	/* Payload, split in device side transfers as configured by the host in the SIRM registers: payload_count
	 * transfers of payload_size bytes, then the remainder, padded to TRANSFER1_SIZE on request */
	chunk_size = _get_u32 (camera->sirm + ARV_SIRM_PAYLOAD_SIZE);
	chunk_count = _get_u32 (camera->sirm + ARV_SIRM_PAYLOAD_COUNT);
	transfer1_size = _get_u32 (camera->sirm + ARV_SIRM_TRANSFER1_SIZE);

	offset = 0;
	for (i = 0; chunk_size > 0 && i < chunk_count && offset < sent; i++) {
		gsize length = MIN (chunk_size, sent - offset);

		fake_usb_device_push_segment_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
						     g_bytes_new_from_bytes (job->payload_bytes, offset, length),
						     camera->zlp && length % max_packet_size == 0,
						     FAKE_USB_SEGMENT_PAYLOAD);
		offset += length;
	}
	if (offset < sent) {
		gsize length = sent - offset;
		GBytes *bytes;

		if (camera->pad_final_transfer && transfer1_size > length) {
			guint8 *padded = g_malloc0 (transfer1_size);

			memcpy (padded, (const guint8 *) g_bytes_get_data (job->payload_bytes, NULL) + offset, length);
			bytes = g_bytes_new_take (padded, transfer1_size);
			length = transfer1_size;
		} else {
			bytes = g_bytes_new_from_bytes (job->payload_bytes, offset, length);
		}

		fake_usb_device_push_segment_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, bytes,
						     camera->zlp && length % max_packet_size == 0,
						     FAKE_USB_SEGMENT_PAYLOAD);
	}

	/* Trailer, from ArvUvspTrailer, unless fake_usb_camera_drop_trailers () asked for none */
	memset (&trailer, 0, sizeof (trailer));
	trailer.header.magic = GUINT32_TO_LE (ARV_UVSP_TRAILER_MAGIC);
	trailer.header.size = GUINT16_TO_LE (sizeof (ArvUvspTrailer));
	trailer.header.frame_id = GUINT64_TO_LE (job->frame_id);
	trailer.infos.payload_size = GUINT64_TO_LE (sent);

	if (camera->n_dropped_trailers > 0)
		camera->n_dropped_trailers--;
	else
		fake_usb_device_push_segment_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
						     g_bytes_new (&trailer, sizeof (trailer)),
						     camera->zlp && sizeof (trailer) % max_packet_size == 0,
						     FAKE_USB_SEGMENT_TRAILER);

	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_FRAMES_STARTED, 1);

	fake_usb_debug ("camera %s: frame %" G_GUINT64_FORMAT " queued, %" G_GSIZE_FORMAT " payload bytes",
			camera->serial_number, job->frame_id, sent);
}

void
fake_usb_camera_finish_frame_locked (FakeUsbFrameJob *job)
{
	FakeUsbDevice *device = job->device;
	FakeUsbCamera *camera = device->camera;

	if (camera->job == job)
		camera->job = NULL;

	/* Only if nothing changed meanwhile: still plugged, not power cycled, stream still enabled */
	if (job->filled &&
	    device->plugged &&
	    device->generation == job->generation &&
	    camera->camera == job->fake_camera &&
	    (_get_u32 (camera->sirm + ARV_SIRM_CONTROL) & ARV_SIRM_CONTROL_STREAM_ENABLE) != 0 &&
	    g_queue_is_empty (&fake_usb_device_get_endpoint_locked (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN)->segments))
		_queue_frame_locked (device, job);

	if (job->payload_bytes != NULL)
		g_bytes_unref (job->payload_bytes);
	g_object_unref (job->buffer);
	g_object_unref (job->fake_camera);
	fake_usb_device_unref_locked (device);
	g_free (job);

	fake_usb_signal_locked ();
}

static void
_release_delayed_acks_locked (FakeUsbDevice *device, gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	FakeUsbDelayedAck *delayed_ack;

	while ((delayed_ack = g_queue_peek_head (&camera->delayed_acks)) != NULL && delayed_ack->due <= now) {
		g_queue_pop_head (&camera->delayed_acks);
		_push_ack_locked (device, g_bytes_ref (delayed_ack->ack));
		_delayed_ack_free (delayed_ack);
	}
}

void
fake_usb_camera_tick_locked (FakeUsbDevice *device, gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	gboolean acquiring;

	_release_delayed_acks_locked (device, now);

	acquiring = arv_fake_camera_get_acquisition_status (camera->camera) != 0;
	if (acquiring && !camera->was_acquiring)
		camera->next_frame_time = now;
	camera->was_acquiring = acquiring;
}

gint64
fake_usb_camera_get_next_wakeup_locked (FakeUsbDevice *device, gint64 now)
{
	FakeUsbCamera *camera = device->camera;
	FakeUsbDelayedAck *delayed_ack;
	gint64 wakeup = G_MAXINT64;

	delayed_ack = g_queue_peek_head (&camera->delayed_acks);
	if (delayed_ack != NULL)
		wakeup = delayed_ack->due;

	if (_can_send_frame_locked (device) && _is_free_running (camera))
		wakeup = MIN (wakeup, MAX (camera->next_frame_time, now));

	return wakeup;
}

/* Life cycle */

void
fake_usb_camera_power_on_locked (FakeUsbDevice *device)
{
	FakeUsbCamera *camera = device->camera;

	g_clear_object (&camera->camera);
	g_clear_object (&camera->frame_buffer);

	camera->camera = arv_fake_camera_new_full (camera->serial_number, camera->genicam_filename);
	if (camera->geometry_set) {
		arv_fake_camera_write_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, camera->width);
		arv_fake_camera_write_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_HEIGHT, camera->height);
		arv_fake_camera_write_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_PIXEL_FORMAT,
						camera->pixel_format);
	}

	_init_registers (camera);

	fake_usb_queue_clear_full (&camera->delayed_acks, _delayed_ack_free);
	camera->was_acquiring = FALSE;
	camera->next_frame_time = 0;
	camera->next_frame_id = 1;
}

void
fake_usb_camera_unplugged_locked (FakeUsbDevice *device)
{
	FakeUsbCamera *camera = device->camera;

	fake_usb_queue_clear_full (&camera->delayed_acks, _delayed_ack_free);
	camera->was_acquiring = FALSE;
}

void
fake_usb_camera_free (FakeUsbCamera *camera)
{
	if (camera == NULL)
		return;

	fake_usb_queue_clear_full (&camera->delayed_acks, _delayed_ack_free);
	g_clear_object (&camera->camera);
	g_clear_object (&camera->frame_buffer);
	g_free (camera->serial_number);
	g_free (camera->genicam_filename);
	g_free (camera->guid);
	g_free (camera);
}

FakeUsbDevice *
fake_usb_camera_new_full (const char *serial_number, const char *genicam_filename)
{
	FakeUsbDevice *device;
	FakeUsbCamera *camera;
	guint8 info[FAKE_USB_CAMERA_INFO_SIZE];
	guint8 control, event, data;

	g_return_val_if_fail (serial_number != NULL && *serial_number != '\0', NULL);

	device = fake_usb_device_new_unplugged (FAKE_USB_CAMERA_VENDOR_ID, FAKE_USB_CAMERA_PRODUCT_ID,
						ARV_UV_INTERFACE_DEVICE_CLASS, ARV_UV_INTERFACE_DEVICE_SUBCLASS,
						ARV_UV_INTERFACE_DEVICE_PROTOCOL);

	camera = g_new0 (FakeUsbCamera, 1);
	camera->serial_number = g_strdup (serial_number);
	camera->genicam_filename = g_strdup (genicam_filename);
	camera->guid = g_strdup_printf ("FakeUV-%s", serial_number);
	camera->response_time_ms = FAKE_USB_CAMERA_DEFAULT_RESPONSE_TIME_MS;
	camera->max_cmd_transfer = FAKE_USB_CAMERA_DEFAULT_MAX_CMD_TRANSFER;
	camera->max_ack_transfer = FAKE_USB_CAMERA_DEFAULT_MAX_ACK_TRANSFER;
	camera->alignment_shift = FAKE_USB_CAMERA_DEFAULT_ALIGNMENT_SHIFT;
	camera->payload_type = ARV_UVSP_PAYLOAD_TYPE_IMAGE;
	camera->next_frame_id = 1;
	g_queue_init (&camera->delayed_acks);

	fake_usb_device_set_string (device, FAKE_USB_CAMERA_STRING_MANUFACTURER, "Aravis");
	fake_usb_device_set_string (device, FAKE_USB_CAMERA_STRING_PRODUCT, "Fake");
	fake_usb_device_set_string (device, FAKE_USB_CAMERA_STRING_SERIAL_NUMBER, serial_number);
	fake_usb_device_set_string (device, FAKE_USB_CAMERA_STRING_GUID, camera->guid);
	fake_usb_device_set_string (device, FAKE_USB_CAMERA_STRING_FAMILY, "Aravis Fake USB3 Vision");
	fake_usb_device_set_descriptor_strings (device, FAKE_USB_CAMERA_STRING_MANUFACTURER,
						FAKE_USB_CAMERA_STRING_PRODUCT,
						FAKE_USB_CAMERA_STRING_SERIAL_NUMBER);

	/* Device info block following the control interface descriptor. Aravis only reads the string indices at the
	 * offsets defined in arvuvinterfaceprivate.h. Like every USB descriptor, it starts with its length; its type
	 * byte is not read by Aravis and left 0. */
	memset (info, 0, sizeof (info));
	info[0] = sizeof (info);
	info[ARV_UV_INTERFACE_GUID_INDEX_OFFSET] = FAKE_USB_CAMERA_STRING_GUID;
	info[ARV_UV_INTERFACE_VENDOR_NAME_INDEX_OFFSET] = FAKE_USB_CAMERA_STRING_MANUFACTURER;
	info[ARV_UV_INTERFACE_MODEL_NAME_INDEX_OFFSET] = FAKE_USB_CAMERA_STRING_PRODUCT;
	info[ARV_UV_INTERFACE_FAMILY_NAME_INDEX_OFFSET] = FAKE_USB_CAMERA_STRING_FAMILY;
	info[ARV_UV_INTERFACE_SERIAL_NUMBER_INDEX_OFFSET] = FAKE_USB_CAMERA_STRING_SERIAL_NUMBER;

	control = fake_usb_device_add_interface (device, ARV_UV_INTERFACE_INTERFACE_CLASS,
						 ARV_UV_INTERFACE_INTERFACE_SUBCLASS,
						 ARV_UV_INTERFACE_CONTROL_PROTOCOL, info, sizeof (info));
	fake_usb_device_add_endpoint (device, control, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT,
				      LIBUSB_TRANSFER_TYPE_BULK, FAKE_USB_DEFAULT_MAX_PACKET_SIZE);
	fake_usb_device_add_endpoint (device, control, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN,
				      LIBUSB_TRANSFER_TYPE_BULK, FAKE_USB_DEFAULT_MAX_PACKET_SIZE);

	event = fake_usb_device_add_interface (device, ARV_UV_INTERFACE_INTERFACE_CLASS,
					       ARV_UV_INTERFACE_INTERFACE_SUBCLASS,
					       ARV_UV_INTERFACE_EVENT_PROTOCOL, NULL, 0);
	fake_usb_device_add_endpoint (device, event, FAKE_USB_CAMERA_EVENT_ENDPOINT_IN,
				      LIBUSB_TRANSFER_TYPE_BULK, FAKE_USB_DEFAULT_MAX_PACKET_SIZE);

	data = fake_usb_device_add_interface (device, ARV_UV_INTERFACE_INTERFACE_CLASS,
					      ARV_UV_INTERFACE_INTERFACE_SUBCLASS,
					      ARV_UV_INTERFACE_DATA_PROTOCOL, NULL, 0);
	fake_usb_device_add_endpoint (device, data, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
				      LIBUSB_TRANSFER_TYPE_BULK, FAKE_USB_DEFAULT_MAX_PACKET_SIZE);

	g_assert (control == FAKE_USB_CAMERA_CONTROL_INTERFACE);
	g_assert (event == FAKE_USB_CAMERA_EVENT_INTERFACE);
	g_assert (data == FAKE_USB_CAMERA_DATA_INTERFACE);

	fake_usb_lock ();
	device->camera = camera;
	fake_usb_unlock ();

	fake_usb_device_plug (device);

	return device;
}

FakeUsbDevice *
fake_usb_camera_new (const char *serial_number)
{
	return fake_usb_camera_new_full (serial_number, NULL);
}

/* Scripting and inspection */

gboolean
fake_usb_device_is_camera (FakeUsbDevice *device)
{
	g_return_val_if_fail (device != NULL, FALSE);

	return device->camera != NULL;
}

ArvFakeCamera *
fake_usb_camera_get_fake_camera (FakeUsbDevice *device)
{
	FakeUsbCamera *camera = _get_camera (device);
	ArvFakeCamera *fake_camera;

	g_return_val_if_fail (camera != NULL, NULL);

	fake_usb_lock ();
	fake_camera = camera->camera;
	fake_usb_unlock ();

	return fake_camera;
}

const char *
fake_usb_camera_get_guid (FakeUsbDevice *device)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_val_if_fail (camera != NULL, NULL);

	return camera->guid;
}

char *
fake_usb_camera_dup_device_id (FakeUsbDevice *device)
{
	char *id;

	g_return_val_if_fail (device != NULL, NULL);

	fake_usb_lock ();
	id = g_strdup_printf ("%s-%s-%s",
			      (char *) g_ptr_array_index (device->strings, FAKE_USB_CAMERA_STRING_MANUFACTURER),
			      (char *) g_ptr_array_index (device->strings, FAKE_USB_CAMERA_STRING_PRODUCT),
			      (char *) g_ptr_array_index (device->strings, FAKE_USB_CAMERA_STRING_SERIAL_NUMBER));
	fake_usb_unlock ();

	return id;
}

void
fake_usb_camera_set_geometry (FakeUsbDevice *device, guint32 width, guint32 height, ArvPixelFormat pixel_format)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->geometry_set = TRUE;
	camera->width = width;
	camera->height = height;
	camera->pixel_format = pixel_format;
	arv_fake_camera_write_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, width);
	arv_fake_camera_write_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_HEIGHT, height);
	arv_fake_camera_write_register (camera->camera, ARV_FAKE_CAMERA_REGISTER_PIXEL_FORMAT, pixel_format);
	_refresh_sirm (camera);
	fake_usb_unlock ();
}

void
fake_usb_camera_set_frame_period (FakeUsbDevice *device, guint64 period_us)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->frame_period_us = period_us;
	fake_usb_signal_locked ();
	fake_usb_unlock ();
}

void
fake_usb_camera_set_alignment_shift (FakeUsbDevice *device, guint alignment_shift)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);
	g_return_if_fail (alignment_shift < 31);

	fake_usb_lock ();
	camera->alignment_shift = alignment_shift;
	_refresh_sirm (camera);
	fake_usb_unlock ();
}

void
fake_usb_camera_set_max_transfers (FakeUsbDevice *device, guint32 max_cmd_transfer, guint32 max_ack_transfer)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->max_cmd_transfer = max_cmd_transfer;
	camera->max_ack_transfer = max_ack_transfer;
	_update_bootstrap_registers (camera);
	fake_usb_unlock ();
}

void
fake_usb_camera_set_response_time (FakeUsbDevice *device, guint32 response_time_ms)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->response_time_ms = response_time_ms;
	_update_bootstrap_registers (camera);
	fake_usb_unlock ();
}

void
fake_usb_camera_set_zero_length_packets (FakeUsbDevice *device, gboolean enable)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->zlp = enable;
	fake_usb_unlock ();
}

void
fake_usb_camera_set_ack_zero_length_packets (FakeUsbDevice *device, gboolean enable)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->ack_zlp = enable;
	fake_usb_unlock ();
}

void
fake_usb_camera_set_pad_final_transfer (FakeUsbDevice *device, gboolean pad)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->pad_final_transfer = pad;
	fake_usb_unlock ();
}

void
fake_usb_camera_set_next_frame_id (FakeUsbDevice *device, guint64 frame_id)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->next_frame_id = frame_id;
	fake_usb_unlock ();
}

void
fake_usb_camera_set_payload_type (FakeUsbDevice *device, guint16 payload_type)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);
	g_return_if_fail (payload_type == ARV_UVSP_PAYLOAD_TYPE_IMAGE ||
			  payload_type == ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER);

	fake_usb_lock ();
	camera->payload_type = payload_type;
	_refresh_sirm (camera);
	fake_usb_unlock ();
}

gsize
fake_usb_camera_get_gendc_descriptor_size (void)
{
	return FAKE_USB_GENDC_DESCRIPTOR_SIZE;
}

void
fake_usb_camera_set_pending_acks (FakeUsbDevice *device, guint n_commands, guint16 timeout_ms, guint delay_ms)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->n_pending_acks = n_commands;
	camera->pending_ack_timeout_ms = timeout_ms;
	camera->pending_ack_delay_ms = delay_ms;
	fake_usb_unlock ();
}

void
fake_usb_camera_drop_acks (FakeUsbDevice *device, guint n_commands)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->n_dropped_acks = n_commands;
	fake_usb_unlock ();
}

void
fake_usb_camera_truncate_acks (FakeUsbDevice *device, guint n_acks, gsize n_missing_bytes)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->n_truncated_acks = n_acks;
	camera->truncated_ack_bytes = n_missing_bytes;
	fake_usb_unlock ();
}

void
fake_usb_camera_set_command_status (FakeUsbDevice *device, guint n_commands, guint16 status)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->n_status_overrides = n_commands;
	camera->status_override = status;
	fake_usb_unlock ();
}

void
fake_usb_camera_truncate_payloads (FakeUsbDevice *device, guint n_frames, gsize n_missing_bytes)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->n_truncated_payloads = n_frames;
	camera->truncated_bytes = n_missing_bytes;
	fake_usb_unlock ();
}

void
fake_usb_camera_drop_trailers (FakeUsbDevice *device, guint n_frames)
{
	FakeUsbCamera *camera = _get_camera (device);

	g_return_if_fail (camera != NULL);

	fake_usb_lock ();
	camera->n_dropped_trailers = n_frames;
	fake_usb_unlock ();
}

gboolean
fake_usb_camera_read_memory (FakeUsbDevice *device, guint64 address, guint32 size, void *buffer)
{
	FakeUsbCamera *camera = _get_camera (device);
	ArvUvcpStatus status;

	g_return_val_if_fail (camera != NULL, FALSE);
	g_return_val_if_fail (buffer != NULL, FALSE);

	fake_usb_lock ();
	status = _access_memory_locked (device, address, size, buffer, FALSE, g_get_monotonic_time ());
	fake_usb_unlock ();

	return status == ARV_UVCP_STATUS_SUCCESS;
}

guint32
fake_usb_camera_get_sirm_register (FakeUsbDevice *device, guint32 offset)
{
	FakeUsbCamera *camera = _get_camera (device);
	guint32 value;

	g_return_val_if_fail (camera != NULL, 0);
	g_return_val_if_fail (offset + 4 <= FAKE_USB_CAMERA_SIRM_SIZE, 0);

	fake_usb_lock ();
	_refresh_sirm (camera);
	value = _get_u32 (camera->sirm + offset);
	fake_usb_unlock ();

	return value;
}

gboolean
fake_usb_camera_is_streaming (FakeUsbDevice *device)
{
	FakeUsbCamera *camera = _get_camera (device);
	gboolean streaming;

	g_return_val_if_fail (camera != NULL, FALSE);

	fake_usb_lock ();
	streaming = device->plugged &&
		(_get_u32 (camera->sirm + ARV_SIRM_CONTROL) & ARV_SIRM_CONTROL_STREAM_ENABLE) != 0;
	fake_usb_unlock ();

	return streaming;
}

gboolean
fake_usb_camera_get_frame_checksum (FakeUsbDevice *device, guint64 frame_id, guint32 *checksum, gsize *size)
{
	FakeUsbCamera *camera = _get_camera (device);
	FakeUsbFrameRecord *record;
	gboolean found;

	g_return_val_if_fail (camera != NULL, FALSE);

	fake_usb_lock ();
	record = &camera->frames[frame_id % FAKE_USB_CAMERA_N_FRAME_RECORDS];
	found = record->valid && record->frame_id == frame_id;
	if (found && checksum != NULL)
		*checksum = record->checksum;
	if (found && size != NULL)
		*size = record->size;
	fake_usb_unlock ();

	return found;
}
