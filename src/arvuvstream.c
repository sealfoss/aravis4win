/* Aravis - Digital camera library
 *
 * Copyright © 2009-2025 Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Emmanuel Pacaud <emmanuel.pacaud@free.fr>
 */

/**
 * SECTION: arvuvstream
 * @short_description: USB3Vision video stream
 */

#include <arvenumtypes.h>
#include <arvuvstreamprivate.h>
#include <arvstreamprivate.h>
#include <arvbufferprivate.h>
#include <arvuvspprivate.h>
#include <arvuvcpprivate.h>
#include <arvdebug.h>
#include <arvmisc.h>
#include <libusb.h>
#include <string.h>

#define ARV_UV_STREAM_POP_INPUT_BUFFER_TIMEOUT_MS       10
#define ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS          10

/* Device loss detection, see arv_uv_stream_thread_check_device () */
#define ARV_UV_STREAM_CHECK_INTERVAL_MS                 100     /* minimum interval between two device checks */
#define ARV_UV_STREAM_CHECK_IDLE_TIMEOUT_MS             1000    /* no hotplug: check when transfers are pending
                                                                 * but none came back for this long */
#define ARV_UV_STREAM_SUBMIT_RETRY_CHECK_INTERVAL_MS    1000    /* check and warn this often while a submission
                                                                 * keeps failing */

/* arv_uv_stream_thread_recover () and arv_uv_stream_thread_resync () */
#define ARV_UV_STREAM_RECOVERY_INTERVAL_MS              100     /* minimum interval between two recoveries */
#define ARV_UV_STREAM_RESYNC_READ_TIMEOUT_MS            100     /* short, so that a stop is not delayed */
#define ARV_UV_STREAM_RESYNC_N_FRAMES_MAX               2       /* data read at most without finding the end of a
                                                                 * frame, in frames */

/* arv_uv_stream_thread_drop_unused_contexts () */
#define ARV_UV_STREAM_N_SPARE_CONTEXTS                  16

enum {
       ARV_UV_STREAM_PROPERTY_0,
       ARV_UV_STREAM_PROPERTY_USB_MODE,
       ARV_UV_STREAM_PROPERTY_MAXIMUM_TRANSFER_SIZE,
} ArvUvStreamProperties;

/* Acquisition thread */

typedef struct {
        guint64 n_completed_buffers;
        guint64 n_failures;
        guint64 n_underruns;
        guint64 n_aborted;

        guint64 n_transferred_bytes;
        guint64 n_ignored_bytes;
} ArvStreamStatistics;

typedef struct {
        ArvUvDevice *uv_device;
        void *data;
        size_t allocated_size;
} ArvUvStreamBufferData;

typedef struct {
	ArvStream *stream;

        gboolean thread_started;
        GMutex thread_started_mutex;
        GCond thread_started_cond;

	ArvUvDevice *uv_device;
	ArvStreamCallback callback;
	void *callback_data;

        size_t expected_size;
	size_t leader_size;
	size_t payload_size;
        guint32 payload_count;
        size_t transfer1_size;
	size_t trailer_size;
        guint64 maximum_transfer_size;

        /* WinUSB RAW_IO, see arv_uv_stream_start_acquisition (). When read_packet_size is not 0, every read is a
         * whole number of packets of that size: leader_size and trailer_size are rounded up, and a payload read that
         * would not be one goes through a bounce buffer. raw_io: the policy is on (atomic: the transfer callbacks read
         * it). resync_needed and pipe_reset_needed: set by the transfer callbacks, see arv_uv_stream_thread_recover ().
         * zero_length_packet: see arv_uv_stream_buffer_context_zero_length_packet (). */
        guint32 read_packet_size;
        guint32 raw_io_maximum;
        gint raw_io;
        gint resync_needed;
        gint pipe_reset_needed;
        gint zero_length_packet;

        /* Async mode: bytes of reads in flight, at most */
        guint64 maximum_submit_total;

	gboolean cancel;

	/* Notification for completed transfers and cancellation */
	GMutex stream_mtx;
	GCond stream_event;

	/* Statistics */
	ArvStreamStatistics statistics;

        gint n_buffer_in_use;

        /* Bytes of the async transfers in flight */
        gint total_submitted_bytes;

        /* Device loss detection and recovery. Set by the transfer callbacks: */
        gint transfer_failed;
        gint n_completed_transfers;
        gint n_failed_transfers;
        /* Used by the stream thread only: */
        gboolean has_hotplug;
        gboolean check_pending;
        gint64 last_check_time;
        gint last_n_completed_transfers;
        gint64 last_activity_time;
        gint last_n_failed_transfers;
        gboolean raw_io_off_needed;
        gint64 last_recovery_time;
        gboolean resync_limit_warned;
} ArvUvStreamThreadData;

typedef struct {
	GThread *thread;
	ArvUvStreamThreadData *thread_data;
	ArvUvUsbMode usb_mode;
        guint64 maximum_transfer_size;

        guint64 sirm_address;
        /* The setup of the stream interface by the last start, see arv_uv_device_take_stream_interface () */
        guint stream_interface_owner;
} ArvUvStreamPrivate;

struct _ArvUvStream {
	ArvStream	stream;
};

struct _ArvUvStreamClass {
	ArvStreamClass parent_class;
};

/* A payload read that is not a whole number of packets, with RAW_IO: the transfer reads into a buffer of the next
 * whole number of packets, and arv_uv_stream_payload_cb () copies what fits to its place in the ArvBuffer */

typedef struct {
        struct libusb_transfer *transfer;
        guint8 *data;
        size_t offset;
        size_t size;
} ArvUvStreamBounceBuffer;

typedef struct {
	ArvBuffer *buffer;
	ArvStream *stream;
        ArvUvStreamThreadData *thread_data;

        /* The size of the buffer the context was made for. The stream thread finds a context by the memory its
         * payload transfers read into, that of the buffer: it serves any buffer of the same memory and size, see
         * arv_uv_stream_thread_async (). */
        size_t allocated_size;
        /* The buffer attached last, to know when it is gone, see arv_uv_stream_thread_drop_unused_contexts () */
        GWeakRef last_buffer;

        ArvStreamCallback callback;
        gpointer callback_data;

	GMutex* transfer_completed_mtx;
	GCond* transfer_completed_event;

	size_t total_payload_transferred;
        size_t expected_size;

        guint64 maximum_submit_total;

	guint8 *leader_buffer, *trailer_buffer;

	int num_payload_transfers;
	struct libusb_transfer *leader_transfer, *trailer_transfer, **payload_transfers;

        /* Normally one at most, for the last payload read */
        ArvUvStreamBounceBuffer *bounce_buffers;
        guint n_bounce_buffers;

	guint num_submitted;

	gint *total_submitted_bytes;

        /* The stream thread takes the transfers back, see arv_uv_stream_buffer_context_cancel (). It may set it while a
         * transfer callback runs on the libusb event thread: atomic, and read once by each callback, on entry. */
        gint is_aborting;
        /* Taken back by arv_uv_stream_thread_recover (): the frame is lost, a failure rather than an abort. Set before
         * is_aborting, so that a callback that sees is_aborting set sees it too. */
        gint is_resyncing;

	ArvStreamStatistics *statistics;

        gint *n_buffer_in_use;
} ArvUvStreamBufferContext;

G_DEFINE_TYPE_WITH_CODE (ArvUvStream, arv_uv_stream, ARV_TYPE_STREAM, G_ADD_PRIVATE (ArvUvStream))

static void
arv_uv_stream_buffer_context_wait_transfer_completed (ArvUvStreamBufferContext* ctx, gint64 timeout_ms)
{
	g_mutex_lock( ctx->transfer_completed_mtx );

        if (timeout_ms > 0) {
                gint64 end_time;

                end_time = g_get_monotonic_time() + timeout_ms * G_TIME_SPAN_MILLISECOND;
                g_cond_wait_until (ctx->transfer_completed_event, ctx->transfer_completed_mtx, end_time);
        } else {
                g_cond_wait( ctx->transfer_completed_event, ctx->transfer_completed_mtx );
        }

	g_mutex_unlock( ctx->transfer_completed_mtx );
}

/* Waits until no transfer of the context is in flight. num_submitted is read under the mutex the transfer callbacks
 * decrement it with, so that the context can be freed once this returns. */

static void
arv_uv_stream_buffer_context_wait_idle (ArvUvStreamBufferContext* ctx)
{
	g_mutex_lock (ctx->transfer_completed_mtx);

        while (g_atomic_int_get (&ctx->num_submitted) > 0) {
                gint64 end_time;

                end_time = g_get_monotonic_time () + ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;
                g_cond_wait_until (ctx->transfer_completed_event, ctx->transfer_completed_mtx, end_time);
        }

	g_mutex_unlock (ctx->transfer_completed_mtx);
}

/* Last thing a transfer callback does: once num_submitted drops, the stream thread may free the context. @aborting:
 * is_aborting, as the callback read it on entry. */

static void
arv_uv_stream_buffer_context_transfer_done (ArvUvStreamBufferContext* ctx, struct libusb_transfer *transfer,
                                            gboolean aborting)
{
        ArvUvStreamThreadData *thread_data = ctx->thread_data;
        GMutex *mutex = ctx->transfer_completed_mtx;
        GCond *cond = ctx->transfer_completed_event;

        /* Anything but a completion, or a cancellation the stream thread asked for: the device may be gone. No
         * synchronous I/O in a libusb callback, the stream thread checks. */
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
            !(transfer->status == LIBUSB_TRANSFER_CANCELLED && aborting)) {
                g_atomic_int_set (&thread_data->transfer_failed, TRUE);
                g_atomic_int_inc (&thread_data->n_failed_transfers);

                /* With RAW_IO, WinUSB bypasses its error handling, which otherwise resets the pipe after a failed read
                 * (AUTO_CLEAR_STALL): every later read would fail. Not after a cancellation, or when the device is
                 * gone. */
                if (transfer->status != LIBUSB_TRANSFER_CANCELLED &&
                    transfer->status != LIBUSB_TRANSFER_NO_DEVICE &&
                    g_atomic_int_get (&thread_data->raw_io))
                        g_atomic_int_set (&thread_data->pipe_reset_needed, TRUE);
        }

	g_atomic_int_add (ctx->total_submitted_bytes, -transfer->length);
        /* What came, not what was asked for: RAW_IO rounds reads up to whole packets */
	ctx->statistics->n_transferred_bytes += transfer->actual_length;
        g_atomic_int_inc (&thread_data->n_completed_transfers);

	g_mutex_lock (mutex);
	g_atomic_int_dec_and_test (&ctx->num_submitted);
	g_cond_broadcast (cond);
	g_mutex_unlock (mutex);
}

/* Size of the image part of a GenDC container, as its descriptor gives it, within the received data */

static void
_gendc_clamp_part_size (ArvBuffer *buffer, size_t received_size)
{
        ArvBufferPartInfos *part = &buffer->priv->parts[0];

        if (part->data_offset < 0 || (size_t) part->data_offset > received_size) {
                part->data_offset = 0;
                part->size = 0;
        } else {
                part->size = MIN (part->size, received_size - (size_t) part->data_offset);
        }
}

/* A leader or a trailer, parsed from a copy of what was received: a read may get the start of a packet only, and a
 * camera may send a shorter one. The fields beyond the data received read as zeros, rather than as bytes past the end
 * of the read, or as what an earlier read left in its buffer. */

typedef union {
        ArvUvspLeader leader;
        ArvUvspTrailer trailer;
} ArvUvStreamPacketCopy;

static ArvUvspPacket *
_copy_packet (ArvUvStreamPacketCopy *copy, const void *data, size_t size)
{
        memset (copy, 0, sizeof (*copy));
        memcpy (copy, data, MIN (size, sizeof (*copy)));

        return (ArvUvspPacket *) copy;
}

/* The description of a frame its leader gives, in both USB modes */

static void
_set_buffer_leader (ArvBuffer *buffer, ArvUvspPacket *leader)
{
        buffer->priv->payload_type = arv_uvsp_packet_get_buffer_payload_type (leader, &buffer->priv->has_chunks);
        buffer->priv->chunk_endianness = G_LITTLE_ENDIAN;
        if (buffer->priv->payload_type == ARV_BUFFER_PAYLOAD_TYPE_IMAGE ||
            buffer->priv->payload_type == ARV_BUFFER_PAYLOAD_TYPE_EXTENDED_CHUNK_DATA ||
            buffer->priv->payload_type == ARV_BUFFER_PAYLOAD_TYPE_GENDC_CONTAINER) {
                arv_buffer_set_n_parts (buffer, 1);
                buffer->priv->parts[0].data_offset = 0;
                buffer->priv->parts[0].component_id = 0;
                buffer->priv->parts[0].data_type = ARV_BUFFER_PART_DATA_TYPE_2D_IMAGE;
                buffer->priv->parts[0].pixel_format = arv_uvsp_packet_get_pixel_format (leader);
                arv_uvsp_packet_get_region (leader,
                                            &buffer->priv->parts[0].width,
                                            &buffer->priv->parts[0].height,
                                            &buffer->priv->parts[0].x_offset,
                                            &buffer->priv->parts[0].y_offset,
                                            &buffer->priv->parts[0].x_padding,
                                            &buffer->priv->parts[0].y_padding);
        }
        buffer->priv->frame_id = arv_uvsp_packet_get_frame_id (leader);
        buffer->priv->timestamp_ns = arv_uvsp_packet_get_timestamp (leader);
}

/* Gives the buffer of a context back to the application. The context lets go of it first: once in the output queue,
 * the buffer may be pushed back at once, and the stream thread attach it to this same context again. */

static void
arv_uv_stream_buffer_context_complete (ArvUvStreamBufferContext* ctx)
{
        ArvBuffer *buffer = ctx->buffer;

        /* Whatever the status: the descriptor of a failed buffer may be incomplete, or someone else's data, and
         * arv_buffer_get_image_data () does not check the part against the buffer */
        if (buffer->priv->has_gendc)
                _gendc_clamp_part_size (buffer, ctx->total_payload_transferred);

        ctx->buffer = NULL;
        g_atomic_int_dec_and_test (ctx->n_buffer_in_use);

        arv_stream_push_output_buffer (ctx->stream, buffer);
        if (ctx->callback != NULL)
                ctx->callback (ctx->callback_data,
                               ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE,
                               buffer);
}

/* A leader or a trailer read got something else: the reads are out of step with the transfers of the camera. With
 * RAW_IO, they would stay so, see arv_uv_stream_thread_resync (). */

static void
arv_uv_stream_buffer_context_out_of_step (ArvUvStreamBufferContext* ctx)
{
        if (ctx->thread_data->read_packet_size > 0)
                g_atomic_int_set (&ctx->thread_data->resync_needed, TRUE);
}

/* RAW_IO reads: a read got nothing but a zero length packet. The camera ends its transfers of whole packets with one,
 * which the next read gets, whichever it is, so that the reads of every frame get out of step. RAW_IO needs such
 * transfers: the device goes without it from its next acquisition, which then reads a frame smaller than the transfer
 * size in a single transfer, ending with a short packet, as before. */

static void
arv_uv_stream_buffer_context_zero_length_packet (ArvUvStreamBufferContext *ctx)
{
        if (g_atomic_int_compare_and_exchange (&ctx->thread_data->zero_length_packet, FALSE, TRUE)) {
                arv_warning_stream_thread ("The camera ends its transfers of whole packets with a zero length packet, "
                                           "which RAW_IO cannot follow: RAW_IO stays off for this device from the next "
                                           "acquisition (leave ARV_UV_RAW_IO unset for this camera)");
                arv_uv_device_disable_raw_io (ctx->thread_data->uv_device);
        }
}

/* A completed leader or trailer read: whether it got a packet of @type, copied into @copy. A read that got less than a
 * packet header is not parsed: its buffer still holds an earlier packet, or nothing yet. */

static gboolean
arv_uv_stream_buffer_context_check_packet (ArvUvStreamBufferContext *ctx, struct libusb_transfer *transfer,
                                           ArvUvspPacketType type, ArvUvStreamPacketCopy *copy)
{
        const char *expected = type == ARV_UVSP_PACKET_TYPE_LEADER ? "leader" : "trailer";
        ArvUvspPacket *packet = _copy_packet (copy, transfer->buffer, MAX (transfer->actual_length, 0));

        if (transfer->actual_length >= (int) sizeof (ArvUvspHeader)) {
                arv_uvsp_packet_debug (packet, ARV_DEBUG_LEVEL_DEBUG);

                if (arv_uvsp_packet_get_packet_type (packet) == type)
                        return TRUE;

                arv_warning_stream_thread ("Unexpected packet type (was expecting %s packet)", expected);
        } else {
                if (transfer->actual_length == 0 && ctx->thread_data->read_packet_size > 0)
                        arv_uv_stream_buffer_context_zero_length_packet (ctx);

                arv_warning_stream_thread ("Read of %d byte(s) (was expecting %s packet)", transfer->actual_length,
                                           expected);
        }

        ctx->buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
        arv_uv_stream_buffer_context_out_of_step (ctx);

        return FALSE;
}

static
void LIBUSB_CALL arv_uv_stream_leader_cb (struct libusb_transfer *transfer)
{
	ArvUvStreamBufferContext *ctx = transfer->user_data;
        gboolean aborting = g_atomic_int_get (&ctx->is_aborting);
        ArvUvStreamPacketCopy leader;

        if (ctx->buffer != NULL) {
                if (aborting) {
                        ctx->buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
                } else {
                        switch (transfer->status) {
                                case LIBUSB_TRANSFER_COMPLETED:
                                if (!arv_uv_stream_buffer_context_check_packet (ctx, transfer,
                                                                                ARV_UVSP_PACKET_TYPE_LEADER, &leader))
                                        break;

                                ctx->buffer->priv->system_timestamp_ns = g_get_real_time () * 1000LL;
                                _set_buffer_leader (ctx->buffer, (ArvUvspPacket *) &leader);
                                break;
                        default:
                                arv_warning_stream_thread ("Leader transfer failed (%s)",
                                                           libusb_error_name (transfer->status));
                                ctx->buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                                break;
                        }
                }
        }

        arv_uv_stream_buffer_context_transfer_done (ctx, transfer, aborting);
}

static void
_gendc_payload (ArvUvStreamBufferContext *ctx)
{
	int component_count;

        if(!arv_uvsp_packet_is_gendc (ctx->buffer->priv->data)){
                arv_warning_sp ("Invalid GenDC Container: Signature shows %.4s "
                                "which is supposed to be GNDC",
                                ctx->buffer->priv->data);
        } else {
                ctx->buffer->priv->has_gendc = TRUE;
                ctx->buffer->priv->gendc_data_offset =
                        arv_uvsp_packet_get_gendc_dataoffset(ctx->buffer->priv->data);
                ctx->buffer->priv->gendc_descriptor_size =
                        arv_uvsp_packet_get_gendc_descriptorsize(ctx->buffer->priv->data);
                ctx->buffer->priv->gendc_data_size =
                        arv_uvsp_packet_get_gendc_datasize(ctx->buffer->priv->data);
                component_count =
                        (int) arv_uvsp_packet_get_gendc_componentcount (ctx->buffer->priv->data);

                for(int ith_component = 0; ith_component < component_count; ++ith_component){
                        int64_t ith_component_offset =
                                arv_uvsp_packet_get_gendc_componentoffset(ctx->buffer->priv->data, ith_component);

                        if (arv_uvsp_packet_get_gendc_iscomponentvalid (ctx->buffer->priv->data + ith_component_offset)
                            && arv_uvsp_packet_get_gendc_componenttypeid (ctx->buffer->priv->data + ith_component_offset) == 0x1 ) {
                                guint64 partoffset = arv_uvsp_packet_get_gendc_partoffset
                                        (ctx->buffer->priv->data + ith_component_offset, 0);

                                ctx->buffer->priv->parts[0].data_offset =
                                        arv_uvsp_packet_get_gendc_partdatapffset(ctx->buffer->priv->data + partoffset);
                                ctx->buffer->priv->parts[0].size =
                                        arv_uvsp_packet_get_gendc_partdatasize(ctx->buffer->priv->data + partoffset);
                                ctx->buffer->priv->parts[0].component_id =
                                        ith_component;
                                ctx->buffer->priv->parts[0].data_type =
                                        ARV_BUFFER_PART_DATA_TYPE_2D_IMAGE;
                                ctx->buffer->priv->parts[0].pixel_format =
                                        arv_uvsp_packet_get_gendc_componentpixelformat
                                        (ctx->buffer->priv->data + ith_component_offset);
                                ctx->buffer->priv->parts[0].width =
                                        arv_uvsp_packet_get_gendc_partdimension_x(ctx->buffer->priv->data + partoffset);
                                ctx->buffer->priv->parts[0].height =
                                        arv_uvsp_packet_get_gendc_partdimension_y(ctx->buffer->priv->data + partoffset);
                                ctx->buffer->priv->parts[0].x_offset = 0;
                                ctx->buffer->priv->parts[0].y_offset = 0;
                                ctx->buffer->priv->parts[0].x_padding =
                                        arv_uvsp_packet_get_gendc_partpadding_x(ctx->buffer->priv->data + partoffset);
                                ctx->buffer->priv->parts[0].y_padding =
                                        arv_uvsp_packet_get_gendc_partpadding_y(ctx->buffer->priv->data + partoffset);

                                break;
                        }
                }
        }
}

/* Bytes a completed payload transfer put in the buffer. A read through a bounce buffer (RAW_IO) is copied to its
 * place in the buffer first, as much as fits there. */

static size_t
arv_uv_stream_buffer_context_payload_received (ArvUvStreamBufferContext *ctx, struct libusb_transfer *transfer)
{
        guint i;

        for (i = 0; i < ctx->n_bounce_buffers; i++) {
                ArvUvStreamBounceBuffer *bounce = &ctx->bounce_buffers[i];

                if (bounce->transfer == transfer) {
                        size_t size = MIN ((size_t) transfer->actual_length, bounce->size);

                        memcpy (ctx->buffer->priv->data + bounce->offset, bounce->data, size);

                        return size;
                }
        }

        return transfer->actual_length;
}

static void LIBUSB_CALL
arv_uv_stream_payload_cb (struct libusb_transfer *transfer)
{
	ArvUvStreamBufferContext *ctx = transfer->user_data;
        gboolean aborting = g_atomic_int_get (&ctx->is_aborting);

        if (ctx->buffer != NULL) {
                if (aborting) {
                        ctx->buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
                } else {
                        if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
                                if (transfer->actual_length == 0 && ctx->thread_data->read_packet_size > 0)
                                        arv_uv_stream_buffer_context_zero_length_packet (ctx);

                                /* Before any parsing of the buffer */
                                ctx->total_payload_transferred +=
                                        arv_uv_stream_buffer_context_payload_received (ctx, transfer);
                                if (ctx->buffer->priv->payload_type == ARV_BUFFER_PAYLOAD_TYPE_GENDC_CONTAINER){
                                        _gendc_payload(ctx);
                                }
                        } else {
                                arv_warning_stream_thread ("Payload transfer failed (%s)",
                                                           libusb_error_name (transfer->status));
                                ctx->buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                        }
                }
        }

        arv_uv_stream_buffer_context_transfer_done (ctx, transfer, aborting);
}

static
void LIBUSB_CALL arv_uv_stream_trailer_cb (struct libusb_transfer *transfer)
{
	ArvUvStreamBufferContext *ctx = transfer->user_data;
        /* Read once, on entry: the stream thread may take the transfers back for a resynchronization while this runs.
         * A second read of is_aborting could find it set after a first one found neither flag set, and give the lost
         * frame back aborted rather than failed. */
        gboolean aborting = g_atomic_int_get (&ctx->is_aborting);
        gboolean resyncing = aborting && g_atomic_int_get (&ctx->is_resyncing);
        ArvUvStreamPacketCopy trailer;

        if (ctx->buffer != NULL) {
                if (resyncing) {
                        ctx->buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                        ctx->statistics->n_failures += 1;
                } else if (aborting) {
                        ctx->buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
                        ctx->statistics->n_aborted += 1;
                } else {
                        switch (transfer->status) {
                                case LIBUSB_TRANSFER_COMPLETED:
                                        if (!arv_uv_stream_buffer_context_check_packet (ctx, transfer,
                                                                                        ARV_UVSP_PACKET_TYPE_TRAILER,
                                                                                        &trailer))
                                                break;

                                        arv_debug_stream_thread ("Total payload: %zu bytes", ctx->total_payload_transferred);
                                        if (ctx->total_payload_transferred != ctx->expected_size) {
                                                arv_warning_stream_thread ("Unexpected total payload size (received %"
                                                                           G_GSIZE_FORMAT " - expected %" G_GSIZE_FORMAT")",
                                                                           ctx->total_payload_transferred, ctx->expected_size);
                                                ctx->buffer->priv->status = ARV_BUFFER_STATUS_SIZE_MISMATCH;
                                                break;
                                        }

                                        break;
                                default:
                                        arv_warning_stream_thread ("Trailer transfer failed (%s)",
                                                                   libusb_error_name(transfer->status));
                                        ctx->buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                                        break;
                        }

                        switch (ctx->buffer->priv->status) {
                                case ARV_BUFFER_STATUS_FILLING:
                                        ctx->buffer->priv->status = ARV_BUFFER_STATUS_SUCCESS;
                                        ctx->buffer->priv->received_size = ctx->total_payload_transferred;
                                        /* A GenDC part keeps the size of its descriptor, see
                                         * arv_uv_stream_buffer_context_complete () */
                                        if (!ctx->buffer->priv->has_gendc)
                                                ctx->buffer->priv->parts[0].size = ctx->total_payload_transferred;
                                        ctx->statistics->n_completed_buffers += 1;
                                        break;
                                default:
                                        ctx->statistics->n_failures += 1;
                                        break;
                        }
                }

                arv_uv_stream_buffer_context_complete (ctx);
        }

        arv_uv_stream_buffer_context_transfer_done (ctx, transfer, aborting);
}

/* @value rounded up to a multiple of @unit, which is not necessarily a power of two */

static guint64
_round_up (guint64 value, guint64 unit)
{
        return (value + unit - 1) / unit * unit;
}

static ArvUvStreamBufferContext*
arv_uv_stream_buffer_context_new (ArvBuffer *buffer, ArvUvStreamThreadData *thread_data)
{
	ArvUvStreamBufferContext* ctx;
	int i;
	size_t offset = 0;

        ctx = g_new0 (ArvUvStreamBufferContext, 1);

	ctx->buffer = NULL;
	ctx->stream = thread_data->stream;
        ctx->thread_data = thread_data;
        ctx->allocated_size = buffer->priv->allocated_size;
        g_weak_ref_init (&ctx->last_buffer, buffer);
        ctx->callback = thread_data->callback;
        ctx->callback_data = thread_data->callback_data;
	ctx->transfer_completed_mtx = &thread_data->stream_mtx;
	ctx->transfer_completed_event = &thread_data->stream_event;
        ctx->n_buffer_in_use = &thread_data->n_buffer_in_use;
        ctx->maximum_submit_total = thread_data->maximum_submit_total;

	ctx->leader_buffer = g_malloc (thread_data->leader_size);
	ctx->leader_transfer = libusb_alloc_transfer (0);
	arv_uv_device_fill_bulk_transfer (ctx->leader_transfer, thread_data->uv_device,
		ARV_UV_ENDPOINT_DATA, LIBUSB_ENDPOINT_IN,
		ctx->leader_buffer, thread_data->leader_size,
		arv_uv_stream_leader_cb, ctx,
		0);

	ctx->num_payload_transfers = (buffer->priv->allocated_size - 1) / thread_data->payload_size + 1;
	ctx->payload_transfers = g_malloc (ctx->num_payload_transfers * sizeof(struct libusb_transfer*));
        if (thread_data->read_packet_size > 0)
                ctx->bounce_buffers = g_new0 (ArvUvStreamBounceBuffer, ctx->num_payload_transfers);

	for (i = 0; i < ctx->num_payload_transfers; ++i) {
		size_t size = MIN (thread_data->payload_size, buffer->priv->allocated_size - offset);
                guint8 *data = buffer->priv->data + offset;
                size_t length = size;

		ctx->payload_transfers[i] = libusb_alloc_transfer(0);

                /* RAW_IO: payload_size is a whole number of packets, so normally only the read of the end of the
                 * buffer is not one */
                if (thread_data->read_packet_size > 0 && size % thread_data->read_packet_size != 0) {
                        ArvUvStreamBounceBuffer *bounce = &ctx->bounce_buffers[ctx->n_bounce_buffers++];

                        length = _round_up (size, thread_data->read_packet_size);
                        bounce->transfer = ctx->payload_transfers[i];
                        bounce->data = g_malloc (length);
                        bounce->offset = offset;
                        bounce->size = size;
                        data = bounce->data;
                }

		arv_uv_device_fill_bulk_transfer (ctx->payload_transfers[i], thread_data->uv_device,
			ARV_UV_ENDPOINT_DATA, LIBUSB_ENDPOINT_IN,
			data, length,
			arv_uv_stream_payload_cb, ctx,
			0);

		offset += size;
	}

	ctx->trailer_buffer = g_malloc (thread_data->trailer_size);
	ctx->trailer_transfer = libusb_alloc_transfer (0);
	arv_uv_device_fill_bulk_transfer (ctx->trailer_transfer, thread_data->uv_device,
		ARV_UV_ENDPOINT_DATA, LIBUSB_ENDPOINT_IN,
		ctx->trailer_buffer, thread_data->trailer_size,
		arv_uv_stream_trailer_cb, ctx,
		0);

	ctx->num_submitted = 0;
	ctx->total_submitted_bytes = &thread_data->total_submitted_bytes;
	ctx->statistics = &thread_data->statistics;

	return ctx;
}

static void
arv_uv_stream_buffer_context_free (gpointer data)
{
	ArvUvStreamBufferContext* ctx = data;
	int i;

	g_return_if_fail (ctx->num_submitted == 0);

	libusb_free_transfer (ctx->leader_transfer);
	for (i = 0; i < ctx->num_payload_transfers; ++i) {
		libusb_free_transfer (ctx->payload_transfers[i]);
	}
	libusb_free_transfer (ctx->trailer_transfer );

	g_free (ctx->leader_buffer);
        g_free (ctx->payload_transfers);
	g_free (ctx->trailer_buffer);
        for (i = 0; i < (int) ctx->n_bounce_buffers; i++)
                g_free (ctx->bounce_buffers[i].data);
        g_free (ctx->bounce_buffers);

        if (ctx->buffer != NULL) {
                ctx->buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
                ctx->statistics->n_aborted += 1;
                arv_uv_stream_buffer_context_complete (ctx);
        }

        g_weak_ref_clear (&ctx->last_buffer);

	g_free (ctx);
}

/* Waits for @timeout_us at most, less if acquisition stops or a transfer comes back */

static void
arv_uv_stream_thread_wait (ArvUvStreamThreadData *thread_data, gint64 timeout_us)
{
        gint64 end_time = g_get_monotonic_time () + timeout_us;

        g_mutex_lock (&thread_data->stream_mtx);
        if (!g_atomic_int_get (&thread_data->cancel))
                g_cond_wait_until (&thread_data->stream_event, &thread_data->stream_mtx, end_time);
        g_mutex_unlock (&thread_data->stream_mtx);
}

/* Waits until @end_time, less if acquisition stops, whatever transfers come back: a back-off */

static void
arv_uv_stream_thread_wait_until (ArvUvStreamThreadData *thread_data, gint64 end_time)
{
        g_mutex_lock (&thread_data->stream_mtx);
        while (!g_atomic_int_get (&thread_data->cancel) &&
               g_cond_wait_until (&thread_data->stream_event, &thread_data->stream_mtx, end_time))
                ;
        g_mutex_unlock (&thread_data->stream_mtx);
}

/* Stream thread only: checks the device now, which counts as the last regular check */

static gboolean
arv_uv_stream_thread_check_device_now (ArvUvStreamThreadData *thread_data)
{
        thread_data->check_pending = FALSE;
        thread_data->last_check_time = g_get_monotonic_time ();

        return arv_uv_device_check_connection (thread_data->uv_device);
}

/* Stream thread only. Hotplug is the fast way to learn that the device is gone, but a default libusb build on Windows
 * has none, and there a removed device does not always give LIBUSB_ERROR_NO_DEVICE / LIBUSB_TRANSFER_NO_DEVICE either.
 * So the connection is checked (arv_uv_device_check_connection ()) when a transfer failed or the stream thread asked
 * for it (check_pending), and, without hotplug, when transfers are pending but none came back for
 * ARV_UV_STREAM_CHECK_IDLE_TIMEOUT_MS, as the transfers of a removed device may never complete. At most one check per
 * ARV_UV_STREAM_CHECK_INTERVAL_MS. Returns FALSE once the device is known to be gone. */

static gboolean
arv_uv_stream_thread_check_device (ArvUvStreamThreadData *thread_data, gboolean transfers_pending)
{
        gint64 now = g_get_monotonic_time ();
        gint n_completed_transfers = g_atomic_int_get (&thread_data->n_completed_transfers);

        if (g_atomic_int_compare_and_exchange (&thread_data->transfer_failed, TRUE, FALSE))
                thread_data->check_pending = TRUE;

        if (!transfers_pending || n_completed_transfers != thread_data->last_n_completed_transfers) {
                thread_data->last_n_completed_transfers = n_completed_transfers;
                thread_data->last_activity_time = now;
        } else if (!thread_data->has_hotplug &&
                   now - thread_data->last_activity_time >=
                   ARV_UV_STREAM_CHECK_IDLE_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND) {
                arv_debug_stream_thread ("No transfer came back for %d ms", ARV_UV_STREAM_CHECK_IDLE_TIMEOUT_MS);
                thread_data->check_pending = TRUE;
                thread_data->last_activity_time = now;
        }

        if (thread_data->check_pending &&
            now - thread_data->last_check_time >= ARV_UV_STREAM_CHECK_INTERVAL_MS * G_TIME_SPAN_MILLISECOND)
                arv_uv_stream_thread_check_device_now (thread_data);

        return arv_uv_device_is_connected (thread_data->uv_device);
}

/* Stream thread only: a submission failed with LIBUSB_ERROR_IO while RAW_IO is on, @n_retries times before, for
 * @retry_time_us. WinUSB rejects a read that breaks the RAW_IO rules (a whole number of packets, no larger than the
 * RAW_IO maximum transfer size) with the error a removed device gives too, so the device is checked first. A read
 * that breaks the rules, which the RAW_IO maximum changing would explain, will never be accepted: the acquisition goes
 * on without RAW_IO (arv_uv_stream_thread_recover ()), and so do the next ones of the device
 * (arv_uv_device_disable_raw_io ()). Another failure may be transient, RAW_IO passing every read on to the USB stack
 * at once: it is retried, as without RAW_IO, and the acquisition goes on without RAW_IO if it lasts
 * ARV_UV_STREAM_SUBMIT_RETRY_CHECK_INTERVAL_MS. Returns TRUE to retry, FALSE to give the submission up. */

static gboolean
arv_uv_stream_thread_raw_io_failure (ArvUvStreamThreadData *thread_data, struct libusb_transfer *transfer,
                                     guint n_retries, gint64 retry_time_us)
{
        guint maximum;

        if (thread_data->raw_io_off_needed)
                return FALSE;

        if (n_retries == 0) {
                if (!arv_uv_stream_thread_check_device_now (thread_data))
                        return FALSE;

                maximum = arv_uv_device_get_raw_io_maximum_transfer_size (thread_data->uv_device);
                if ((guint) transfer->length % thread_data->read_packet_size == 0 &&
                    (guint) transfer->length <= maximum)
                        return TRUE;

                arv_warning_stream_thread ("RAW_IO rejected a read of %d bytes (packet size %u bytes, RAW_IO maximum %u "
                                           "bytes, %u at the start): going on without RAW_IO, which stays off for this "
                                           "device (without ARV_UV_RAW_IO=1, it is off from the start)",
                                           transfer->length, thread_data->read_packet_size, maximum,
                                           thread_data->raw_io_maximum);
                arv_uv_device_disable_raw_io (thread_data->uv_device);
                thread_data->raw_io_off_needed = TRUE;

                return FALSE;
        }

        if (retry_time_us < ARV_UV_STREAM_SUBMIT_RETRY_CHECK_INTERVAL_MS * G_TIME_SPAN_MILLISECOND)
                return TRUE;

        arv_warning_stream_thread ("Reads keep failing with RAW_IO on (%s) while the camera is there: going on "
                                   "without RAW_IO for this acquisition", libusb_error_name (LIBUSB_ERROR_IO));
        thread_data->raw_io_off_needed = TRUE;

        return FALSE;
}

/* Submits one transfer of the buffer attached to @ctx. Returns FALSE if it could not be submitted: acquisition
 * stopped, device gone, or an error that retrying will not fix. */

static gboolean
_submit_transfer (ArvUvStreamBufferContext* ctx, struct libusb_transfer* transfer)
{
        ArvUvStreamThreadData *thread_data = ctx->thread_data;
        gint64 retry_start_time = 0;
        gint64 next_retry_check_time = 0;
        guint n_retries = 0;

        /* At most maximum_submit_total bytes in flight */
	while ((g_atomic_int_get (ctx->total_submitted_bytes) + transfer->length) > ctx->maximum_submit_total) {
                if (g_atomic_int_get (&thread_data->cancel) ||
                    !arv_uv_stream_thread_check_device (thread_data, TRUE))
                        return FALSE;
		arv_uv_stream_buffer_context_wait_transfer_completed (ctx, ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS);
	}

	for (;;) {
                gboolean nothing_in_flight;
                gboolean retrying_for_long;
                gint64 now;
                int status;

                if (g_atomic_int_get (&thread_data->cancel) ||
                    !arv_uv_stream_thread_check_device (thread_data, g_atomic_int_get (ctx->total_submitted_bytes) > 0))
                        return FALSE;

		status = libusb_submit_transfer (transfer);
                if (status == LIBUSB_SUCCESS) {
			g_atomic_int_inc (&ctx->num_submitted);
			g_atomic_int_add (ctx->total_submitted_bytes, transfer->length);
			return TRUE;
		}

                if (status != LIBUSB_ERROR_IO && status != LIBUSB_ERROR_NO_MEM) {
                        /* LIBUSB_ERROR_NO_DEVICE included: the check confirms */
			arv_warning_stream_thread ("libusb_submit_transfer failed (%s)", libusb_error_name (status));
                        thread_data->check_pending = TRUE;
			return FALSE;
		}

                now = g_get_monotonic_time ();
                if (retry_start_time == 0)
                        retry_start_time = now;

                if (status == LIBUSB_ERROR_IO && g_atomic_int_get (&thread_data->raw_io) &&
                    !arv_uv_stream_thread_raw_io_failure (thread_data, transfer, n_retries++, now - retry_start_time))
                        return FALSE;

                /* LIBUSB_ERROR_NO_MEM: the usbfs memory limit of Linux is reached, and memory comes back as transfers
                 * complete. To allow more memory for submitted transfers, increase usbfs_memory_mb (16 MB by default):
                 * sudo modprobe usbcore usbfs_memory_mb=1000
                 * Never a removal, which Linux reports as LIBUSB_ERROR_NO_DEVICE: the device is not checked. The check
                 * would likely be refused for lack of memory too, and libusb reports that as LIBUSB_ERROR_IO.
                 *
                 * LIBUSB_ERROR_IO is retried as well, but on Windows every failed submission gives it, a removed device
                 * included. So check the device when nothing of this stream is in flight (waiting frees nothing), and
                 * every second while the retries last. */
                nothing_in_flight = g_atomic_int_get (ctx->total_submitted_bytes) == 0;
                retrying_for_long = now - retry_start_time >=
                        ARV_UV_STREAM_SUBMIT_RETRY_CHECK_INTERVAL_MS * G_TIME_SPAN_MILLISECOND;
                if ((retrying_for_long || (nothing_in_flight && status == LIBUSB_ERROR_IO)) &&
                    now >= next_retry_check_time) {
                        next_retry_check_time = now + ARV_UV_STREAM_SUBMIT_RETRY_CHECK_INTERVAL_MS * G_TIME_SPAN_MILLISECOND;
                        if (status == LIBUSB_ERROR_IO)
                                thread_data->check_pending = TRUE;
                        if (retrying_for_long)
                                arv_warning_stream_thread ("libusb_submit_transfer failed (%s) for %" G_GINT64_FORMAT
                                                           " ms, retrying", libusb_error_name (status),
                                                           (now - retry_start_time) / G_TIME_SPAN_MILLISECOND);
                }

		arv_uv_stream_buffer_context_wait_transfer_completed (ctx, ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS);
	}
}

/* Takes back the transfers of @ctx in flight, and waits until they are all back */

static void
arv_uv_stream_buffer_context_cancel (ArvUvStreamBufferContext* ctx)
{
	int i;

        g_atomic_int_set (&ctx->is_aborting, TRUE);

	libusb_cancel_transfer (ctx->leader_transfer );

	for (i = 0; i < ctx->num_payload_transfers; ++i) {
		libusb_cancel_transfer (ctx->payload_transfers[i]);
	}

	libusb_cancel_transfer (ctx->trailer_transfer);

        arv_uv_stream_buffer_context_wait_idle (ctx);
}

static void
_cancel_buffer_context (gpointer key, gpointer value, gpointer user_data)
{
        arv_uv_stream_buffer_context_cancel (value);
}

static void
_resync_buffer_context (gpointer key, gpointer value, gpointer user_data)
{
        ArvUvStreamBufferContext *ctx = value;

        /* Before is_aborting: the trailer callback of a transfer back already may be running, and must not see
         * is_aborting set without is_resyncing */
        g_atomic_int_set (&ctx->is_resyncing, TRUE);
        arv_uv_stream_buffer_context_cancel (ctx);
}

/* Attaches @buffer to @ctx and submits its transfers. When one cannot be submitted, the trailer callback will not
 * complete the buffer: take the others back, then give the buffer back, aborted, exactly once. */

static gboolean
arv_uv_stream_buffer_context_submit (ArvUvStreamBufferContext* ctx, ArvBuffer *buffer, ArvUvStreamThreadData *thread_data)
{
        gboolean success;
	int i;

        if (ctx->callback != NULL)
                ctx->callback (ctx->callback_data,
                               ARV_STREAM_CALLBACK_TYPE_START_BUFFER,
                               buffer);

        ctx->buffer = buffer;
        g_weak_ref_set (&ctx->last_buffer, buffer);
        ctx->total_payload_transferred = 0;
        g_atomic_int_set (&ctx->is_aborting, FALSE);
        g_atomic_int_set (&ctx->is_resyncing, FALSE);
        buffer->priv->status = ARV_BUFFER_STATUS_FILLING;
        buffer->priv->has_gendc = FALSE;

        ctx->expected_size = thread_data->expected_size;

        success = _submit_transfer (ctx, ctx->leader_transfer);

        for (i = 0; success && i < ctx->num_payload_transfers; ++i)
                success = _submit_transfer (ctx, ctx->payload_transfers[i]);

        if (success)
                success = _submit_transfer (ctx, ctx->trailer_transfer);

        if (!success) {
                arv_uv_stream_buffer_context_cancel (ctx);

                ctx->buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
                thread_data->statistics.n_aborted += 1;
                arv_uv_stream_buffer_context_complete (ctx);
        }

        return success;
}

/* A read of whole packets of @packet_size ends when full, or at the short packet, or zero length packet, that ends a
 * transfer of the camera. Whether that short packet, the last one of the read, is a trailer: the end of a frame. The
 * trailer may follow payload packets in the same read, when the payload transfers are whole packets themselves. */

static gboolean
_read_ends_with_trailer (const guint8 *data, size_t transferred, guint32 packet_size)
{
        size_t last_packet = transferred - transferred % packet_size;

        return transferred - last_packet >= sizeof (ArvUvspHeader) &&
                arv_uvsp_packet_get_packet_type ((const ArvUvspPacket *) (data + last_packet)) ==
                ARV_UVSP_PACKET_TYPE_TRAILER;
}

/* Stream thread only, reads of whole packets (read_packet_size), nothing in flight. Each read ends when full, or at
 * the short packet that ends a transfer of the camera. Reads can get out of step with these transfers, after a
 * transfer lost in the middle of a frame for example. Without whole packets, the leader and trailer reads, shorter
 * than a packet, bring them back into step. With whole packets, which RAW_IO needs, so are the payload transfers,
 * which nothing ends: the reads of each frame would end at the same wrong places, frame after frame. So the stream is
 * read synchronously, in whole packets, up to the end of a frame: the trailer that ends a read, after which the next
 * leader read starts. Not further than ARV_UV_STREAM_RESYNC_N_FRAMES_MAX frames' worth of data, for a camera whose
 * frames end otherwise: streaming goes on, and resynchronizes again if needed. A read is pending all along, so the
 * idle check of arv_uv_stream_thread_check_device () applies, and only data counts as activity. */

static void
arv_uv_stream_thread_resync (ArvUvStreamThreadData *thread_data)
{
        guint8 *data;
        guint64 n_bytes = 0;
        guint64 maximum_bytes;
        guint n_reads = 0;

        arv_info_stream_thread ("Reads out of step with the camera transfers, resynchronizing");

        maximum_bytes = ARV_UV_STREAM_RESYNC_N_FRAMES_MAX *
                (thread_data->leader_size + thread_data->expected_size + thread_data->trailer_size) +
                thread_data->maximum_transfer_size;

        data = g_malloc (thread_data->maximum_transfer_size);

        while (!g_atomic_int_get (&thread_data->cancel) &&
               arv_uv_stream_thread_check_device (thread_data, TRUE)) {
                GError *error = NULL;
                size_t transferred = 0;

                if (!arv_uv_device_bulk_transfer (thread_data->uv_device, ARV_UV_ENDPOINT_DATA, LIBUSB_ENDPOINT_IN,
                                                  data, thread_data->maximum_transfer_size, &transferred,
                                                  ARV_UV_STREAM_RESYNC_READ_TIMEOUT_MS, &error)) {
                        gboolean timeout = g_error_matches (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT);

                        g_clear_error (&error);
                        if (!timeout) {
                                /* Not a timeout: the device may be gone, the regular check tells. With RAW_IO, the
                                 * pipe may need a reset too, see arv_uv_stream_buffer_context_transfer_done (). */
                                g_atomic_int_set (&thread_data->transfer_failed, TRUE);
                                if (g_atomic_int_get (&thread_data->raw_io))
                                        g_atomic_int_set (&thread_data->pipe_reset_needed, TRUE);
                                arv_info_stream_thread ("Resynchronization failed");
                                break;
                        }

                        /* A read that times out keeps what it got, which libusb reports: whole packets, without the
                         * short packet that would have ended the read. They count, so that a camera sending less than
                         * a read per timeout still reaches the limit below. */
                        if (transferred == 0)
                                continue;
                }

                g_atomic_int_inc (&thread_data->n_completed_transfers);
                n_reads++;
                n_bytes += transferred;
                thread_data->statistics.n_ignored_bytes += transferred;

                if (_read_ends_with_trailer (data, transferred, thread_data->read_packet_size)) {
                        arv_info_stream_thread ("Resynchronized after %u read(s), %" G_GUINT64_FORMAT " bytes",
                                                n_reads, n_bytes);
                        break;
                }

                if (n_bytes > maximum_bytes) {
                        if (!thread_data->resync_limit_warned)
                                arv_warning_stream_thread ("No end of frame found in %" G_GUINT64_FORMAT " bytes, "
                                                           "going on unsynchronized", n_bytes);
                        else
                                arv_info_stream_thread ("No end of frame found in %" G_GUINT64_FORMAT " bytes, "
                                                        "going on unsynchronized", n_bytes);
                        thread_data->resync_limit_warned = TRUE;
                        break;
                }
        }

        g_free (data);
}

/* Stream thread only. Takes every transfer back: their frames are lost, the buffers come back with
 * ARV_BUFFER_STATUS_MISSING_PACKETS. Then, with nothing in flight:
 * - raw_io_off_needed: a read failed with RAW_IO on, see arv_uv_stream_thread_raw_io_failure (). RAW_IO is turned
 *   off for the rest of the acquisition, the reads keeping their layout, which needs no RAW_IO.
 * - pipe_reset_needed: a read failed with RAW_IO on. WinUSB then bypasses its error handling, which otherwise resets
 *   the pipe after a failed read (AUTO_CLEAR_STALL), so that every later read would fail: once the device is known to
 *   be there, its pipe is reset (arv_uv_device_clear_stream_halt ()).
 * - With reads of whole packets, the stream is resynchronized (arv_uv_stream_thread_resync ()): the transfers taken
 *   back leave the reads out of step with the camera, if they were not already.
 * At most one recovery per ARV_UV_STREAM_RECOVERY_INTERVAL_MS, so that a pipe that keeps failing does not make the
 * thread spin. */

static void
arv_uv_stream_thread_recover (ArvUvStreamThreadData *thread_data, GHashTable *ctx_lookup)
{
        gboolean pipe_reset;

        arv_uv_stream_thread_wait_until (thread_data, thread_data->last_recovery_time +
                                         ARV_UV_STREAM_RECOVERY_INTERVAL_MS * G_TIME_SPAN_MILLISECOND);
        if (g_atomic_int_get (&thread_data->cancel))
                return;
        thread_data->last_recovery_time = g_get_monotonic_time ();

        g_hash_table_foreach (ctx_lookup, _resync_buffer_context, NULL);

        /* What the transfers taken back reported belongs to the same incident */
        g_atomic_int_set (&thread_data->resync_needed, FALSE);
        pipe_reset = g_atomic_int_compare_and_exchange (&thread_data->pipe_reset_needed, TRUE, FALSE);

        if (thread_data->raw_io_off_needed) {
                thread_data->raw_io_off_needed = FALSE;
                if (arv_uv_device_set_raw_io (thread_data->uv_device, FALSE)) {
                        g_atomic_int_set (&thread_data->raw_io, FALSE);
                        arv_info_stream_thread ("RAW_IO off for the rest of the acquisition");
                }
        }

        if (pipe_reset) {
                if (!arv_uv_stream_thread_check_device_now (thread_data))
                        return;
                arv_uv_device_clear_stream_halt (thread_data->uv_device);
        }

        if (thread_data->read_packet_size > 0)
                arv_uv_stream_thread_resync (thread_data);
}

/* Stream thread only: whether a context can be dropped, see arv_uv_stream_thread_drop_unused_contexts () */

static gboolean
_is_unused_context (gpointer key, gpointer value, gpointer user_data)
{
        ArvUvStreamBufferContext *ctx = value;
        ArvBuffer *buffer;
        gboolean idle;

        /* num_submitted drops under this mutex, the last thing a transfer callback does with the context */
        g_mutex_lock (ctx->transfer_completed_mtx);
        idle = g_atomic_int_get (&ctx->num_submitted) == 0;
        g_mutex_unlock (ctx->transfer_completed_mtx);

        if (!idle || ctx->buffer != NULL)
                return FALSE;

        buffer = g_weak_ref_get (&ctx->last_buffer);
        if (buffer == NULL)
                return TRUE;

        g_object_unref (buffer);

        return FALSE;
}

/* Stream thread only. The contexts stay in @ctx_lookup from one fill to the next: applications usually push the same
 * buffers back. One whose buffer is gone stays too, as another buffer may get its memory. But an application that
 * replaces its buffers would pile them up, with their transfers: once the table holds twice as many contexts as after
 * the last time, plus ARV_UV_STREAM_N_SPARE_CONTEXTS, those whose last buffer is gone and with nothing in flight are
 * dropped. Every buffer alive keeps its context, even one the application holds for long. */

static void
arv_uv_stream_thread_drop_unused_contexts (GHashTable *ctx_lookup, guint *n_kept)
{
        guint n_dropped;

        if (g_hash_table_size (ctx_lookup) <= 2 * *n_kept + ARV_UV_STREAM_N_SPARE_CONTEXTS)
                return;

        n_dropped = g_hash_table_foreach_remove (ctx_lookup, _is_unused_context, NULL);
        *n_kept = g_hash_table_size (ctx_lookup);

        arv_debug_stream_thread ("%u unused stream buffer context(s) dropped, %u kept", n_dropped, *n_kept);
}

/* Stream thread only: gives back a buffer that cannot be filled, aborted */

static void
arv_uv_stream_thread_abort_buffer (ArvUvStreamThreadData *thread_data, ArvBuffer *buffer)
{
        if (thread_data->callback != NULL)
                thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_START_BUFFER, buffer);

        buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
        thread_data->statistics.n_aborted += 1;
        g_atomic_int_dec_and_test (&thread_data->n_buffer_in_use);

        arv_stream_push_output_buffer (thread_data->stream, buffer);
        if (thread_data->callback != NULL)
                thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE, buffer);
}

static void *
arv_uv_stream_thread_async (void *data)
{
	ArvUvStreamThreadData *thread_data = data;
	ArvBuffer *buffer = NULL;
	GHashTable *ctx_lookup;
        guint n_kept_contexts = 0;
        gboolean event_thread_priority;

	arv_info_stream_thread ("Start async USB3Vision stream thread");

	arv_debug_stream_thread ("leader_size = %zu", thread_data->leader_size );
	arv_debug_stream_thread ("payload_size = %zu", thread_data->payload_size );
	arv_debug_stream_thread ("trailer_size = %zu", thread_data->trailer_size );

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_INIT, NULL);

        /* The transfer callbacks, which complete the buffers, run on the libusb event thread of the device. On
         * Windows, it takes the priority the INIT callback may have given to this thread. */
        event_thread_priority = arv_uv_device_request_event_thread_priority (thread_data->uv_device);

	ctx_lookup = g_hash_table_new_full( g_direct_hash, g_direct_equal, NULL, arv_uv_stream_buffer_context_free );

        g_mutex_lock (&thread_data->thread_started_mutex);
        thread_data->thread_started = TRUE;
        g_cond_signal (&thread_data->thread_started_cond);
        g_mutex_unlock (&thread_data->thread_started_mutex);

	while (!g_atomic_int_get (&thread_data->cancel) &&
               arv_uv_stream_thread_check_device (thread_data,
                                                  g_atomic_int_get (&thread_data->total_submitted_bytes) > 0)) {
		ArvUvStreamBufferContext* ctx;
                gint n_failed_transfers;

                if (thread_data->raw_io_off_needed ||
                    g_atomic_int_get (&thread_data->resync_needed) ||
                    g_atomic_int_get (&thread_data->pipe_reset_needed)) {
                        arv_uv_stream_thread_recover (thread_data, ctx_lookup);
                        continue;
                }

                /* Transfers failed since the last turn: do not spin through the buffers, which a pipe whose reads all
                 * fail at once would make it do */
                n_failed_transfers = g_atomic_int_get (&thread_data->n_failed_transfers);
                if (n_failed_transfers != thread_data->last_n_failed_transfers) {
                        thread_data->last_n_failed_transfers = n_failed_transfers;
                        arv_uv_stream_thread_wait_until (thread_data, g_get_monotonic_time () +
                                                         ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS *
                                                         G_TIME_SPAN_MILLISECOND);
                        continue;
                }

                buffer = arv_stream_timeout_pop_input_buffer (thread_data->stream,
                                                              ARV_UV_STREAM_POP_INPUT_BUFFER_TIMEOUT_MS * 1000);

		if( buffer == NULL ) {
                        if (thread_data->n_buffer_in_use == 0)
                                thread_data->statistics.n_underruns += 1;
                        /* NOTE: n_ignored_bytes is not accumulated because it doesn't submit next USB transfer if
                         * buffer is shortage. It means back pressure might be hanlded by USB slave side. */
			continue;
		} else {
                        g_atomic_int_inc(&thread_data->n_buffer_in_use);
                }

                /* By the memory the transfers read into, not by the ArvBuffer: a new buffer may get the address of a
                 * buffer that is gone, while its memory is elsewhere, and the transfers of that buffer's context would
                 * then read into freed memory */
		ctx = g_hash_table_lookup (ctx_lookup, buffer->priv->data);
                if (ctx != NULL && ctx->buffer != NULL) {
                        /* The memory of a buffer being filled: the same buffer pushed twice, or two buffers sharing
                         * their memory */
                        arv_warning_stream_thread ("Buffer %p shares its memory with a buffer being filled, aborted",
                                                   buffer);
                        arv_uv_stream_thread_abort_buffer (thread_data, buffer);
                        continue;
                }
                if (ctx != NULL && ctx->allocated_size != buffer->priv->allocated_size) {
                        arv_debug_stream_thread ("Buffer %p of another size than the last one of its memory, "
                                                 "renewing the stream buffer context", buffer);
                        /* The trailer callback that gave the last buffer back may still be returning */
                        arv_uv_stream_buffer_context_wait_idle (ctx);
                        g_hash_table_remove (ctx_lookup, buffer->priv->data);
                        ctx = NULL;
                }
		if (ctx == NULL) {
			arv_debug_stream_thread ("Stream buffer context not found for buffer %p, creating...", buffer);

			ctx = arv_uv_stream_buffer_context_new (buffer, thread_data);

			g_hash_table_insert (ctx_lookup, buffer->priv->data, ctx);

                        arv_uv_stream_thread_drop_unused_contexts (ctx_lookup, &n_kept_contexts);
		}

                /* After an error that retrying would not fix, do not spin through the buffers */
                if (!arv_uv_stream_buffer_context_submit (ctx, buffer, thread_data))
                        arv_uv_stream_thread_wait_until (thread_data, g_get_monotonic_time () +
                                                         ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS *
                                                         G_TIME_SPAN_MILLISECOND);
	}

	g_hash_table_foreach (ctx_lookup, _cancel_buffer_context, NULL);

	g_hash_table_destroy (ctx_lookup);

        if (event_thread_priority)
                arv_uv_device_release_event_thread_priority (thread_data->uv_device);

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_EXIT, NULL);

	arv_info_stream_thread ("Stop USB3Vision async stream thread");

	return NULL;
}

static void
_gendc_packet (ArvBuffer *buffer)
{
	int component_count;

        if(!arv_uvsp_packet_is_gendc (buffer->priv->data)){
                arv_warning_sp ("Invalid GenDC Container: Signature shows %.4s which is supposed to be GNDC", buffer->priv->data);
        } else {
                buffer->priv->has_gendc = TRUE;
                buffer->priv->gendc_data_offset = arv_uvsp_packet_get_gendc_dataoffset(buffer->priv->data);
                buffer->priv->gendc_descriptor_size = arv_uvsp_packet_get_gendc_descriptorsize(buffer->priv->data);
                buffer->priv->gendc_data_size = arv_uvsp_packet_get_gendc_datasize(buffer->priv->data);

                component_count = (int) arv_uvsp_packet_get_gendc_componentcount(buffer->priv->data);
                for(int ith_component = 0; ith_component < component_count; ++ith_component){
                        int64_t ith_component_offset = arv_uvsp_packet_get_gendc_componentoffset(buffer->priv->data, ith_component);

                        /* only if the component is valid and have an image data (GDC_INTENSITY from SFNC) */
                        if (arv_uvsp_packet_get_gendc_iscomponentvalid(buffer->priv->data + ith_component_offset)
                            && arv_uvsp_packet_get_gendc_componenttypeid(buffer->priv->data + ith_component_offset) == 0x1 ){
                                guint64 partoffset = arv_uvsp_packet_get_gendc_partoffset
                                        (buffer->priv->data + ith_component_offset, 0);
                                buffer->priv->parts[0].data_offset =
                                        arv_uvsp_packet_get_gendc_partdatapffset(buffer->priv->data + partoffset);
                                buffer->priv->parts[0].size =
                                        arv_uvsp_packet_get_gendc_partdatasize(buffer->priv->data + partoffset);
                                buffer->priv->parts[0].component_id = ith_component;
                                buffer->priv->parts[0].data_type = ARV_BUFFER_PART_DATA_TYPE_2D_IMAGE;
                                buffer->priv->parts[0].pixel_format =
                                        arv_uvsp_packet_get_gendc_componentpixelformat(buffer->priv->data + ith_component_offset);
                                buffer->priv->parts[0].width =
                                        arv_uvsp_packet_get_gendc_partdimension_x(buffer->priv->data + partoffset);
                                buffer->priv->parts[0].height =
                                        arv_uvsp_packet_get_gendc_partdimension_y(buffer->priv->data + partoffset);
                                buffer->priv->parts[0].x_offset = 0;
                                buffer->priv->parts[0].y_offset = 0;
                                buffer->priv->parts[0].x_padding =
                                        arv_uvsp_packet_get_gendc_partpadding_x(buffer->priv->data + partoffset);
                                buffer->priv->parts[0].y_padding =
                                        arv_uvsp_packet_get_gendc_partpadding_y(buffer->priv->data + partoffset);
                                break;
                        }
                }
        }
}

static void *
arv_uv_stream_thread_sync (void *data)
{
	ArvUvStreamThreadData *thread_data = data;
	ArvUvspPacket *packet;
	ArvBuffer *buffer = NULL;
	void *incoming_buffer;
	guint64 offset;
        guint64 after_payload;
        size_t padding_max;
	size_t transferred;
        gboolean event_thread_priority;

	arv_info_stream_thread ("Start sync USB3Vision stream thread");

	incoming_buffer = g_malloc (thread_data->maximum_transfer_size);

        /* The bytes a camera may send after a complete payload, before the trailer: the padding of the final transfer
         * to the aligned size the camera was given (SIRM_TRANSFER1_SIZE). Bytes beyond that belong to another frame. */
        if (thread_data->transfer1_size > 0)
                padding_max = thread_data->transfer1_size - thread_data->expected_size % thread_data->payload_size;
        else
                padding_max = 0;

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_INIT, NULL);

        /* The libusb event thread of the device, which holds the event lock most of the time, usually handles the
         * completion of the synchronous transfers and wakes this thread up. On Windows, it takes the priority the INIT
         * callback may have given to this thread. */
        event_thread_priority = arv_uv_device_request_event_thread_priority (thread_data->uv_device);

	offset = 0;
        after_payload = 0;

        g_mutex_lock (&thread_data->thread_started_mutex);
        thread_data->thread_started = TRUE;
        g_cond_signal (&thread_data->thread_started_cond);
        g_mutex_unlock (&thread_data->thread_started_mutex);

	while (!g_atomic_int_get (&thread_data->cancel) &&
               arv_uv_stream_thread_check_device (thread_data, TRUE)) {
		GError *error = NULL;
		size_t size;
                size_t payload_end;
                gint64 start_time;
		transferred = 0;

                /* The payload goes in the buffer, as far as it fits, then comes the trailer. A buffer larger than the
                 * payload is not read further: its end would get the start of the trailer, in a read shorter than a
                 * packet header when the buffer is a few bytes larger than the payload. Without a buffer, or once its
                 * frame has failed, the reads are whole transfers of the camera, up to a trailer or the next leader,
                 * rather than small reads that a larger packet would overflow, one packet at a time. */
		if (buffer != NULL && buffer->priv->status == ARV_BUFFER_STATUS_FILLING) {
                        payload_end = MIN (buffer->priv->allocated_size, thread_data->expected_size);
			if (offset < payload_end)
				size = MIN (thread_data->payload_size, payload_end - offset);
			else
				size = thread_data->trailer_size;
		} else {
                        payload_end = 0;
			size = thread_data->maximum_transfer_size;
                }

		/* Avoid unnecessary memory copy by transferring data directly to the image buffer */
		if (buffer != NULL && offset < payload_end)
			packet = (ArvUvspPacket *) (buffer->priv->data + offset);
		else
			packet = incoming_buffer;

		arv_debug_sp ("Asking for %" G_GSIZE_FORMAT " bytes", size);
                start_time = g_get_monotonic_time ();
		arv_uv_device_bulk_transfer (thread_data->uv_device,  ARV_UV_ENDPOINT_DATA, LIBUSB_ENDPOINT_IN,
					     packet, size, &transferred, 0, &error);

		if (error != NULL) {
                        gint64 elapsed;

			arv_warning_sp ("USB transfer error: %s", error->message);

                        /* Not a mere timeout: the device may be gone, which is not always reported as
                         * LIBUSB_ERROR_NO_DEVICE (LIBUSB_ERROR_IO on Windows). And when the read got data before it
                         * failed, the frame lacks it: so does a read that a larger packet overflows (Linux), which a
                         * read of the same size would do again. */
                        if (!g_error_matches (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT)) {
                                g_atomic_int_set (&thread_data->transfer_failed, TRUE);
                                if (transferred > 0 &&
                                    buffer != NULL && buffer->priv->status == ARV_BUFFER_STATUS_FILLING)
                                        buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                        }
			g_clear_error (&error);

                        /* No spinning on transfers that fail at once */
                        elapsed = g_get_monotonic_time () - start_time;
                        if (elapsed < ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND)
                                arv_uv_stream_thread_wait (thread_data,
                                                           ARV_UV_STREAM_TRANSFER_WAIT_TIMEOUT_MS *
                                                           G_TIME_SPAN_MILLISECOND - elapsed);
                } else if (transferred < 1) {
                        g_atomic_int_inc (&thread_data->n_completed_transfers);
			arv_warning_sp ("No data transferred");
		} else {
			ArvUvspPacketType packet_type = ARV_UVSP_PACKET_TYPE_DATA;
                        ArvUvStreamPacketCopy copy;

                        g_atomic_int_inc (&thread_data->n_completed_transfers);

			arv_debug_sp ("Received %" G_GSIZE_FORMAT " bytes", transferred);

                        /* Leaders and trailers are parsed from a copy (_copy_packet ()). A read shorter than a packet
                         * header is data, or a piece of a packet the reads are out of step with: it is not parsed. */
                        _copy_packet (&copy, packet, transferred);
                        if (transferred >= sizeof (ArvUvspHeader)) {
                                packet_type = arv_uvsp_packet_get_packet_type ((ArvUvspPacket *) &copy);
                                arv_uvsp_packet_debug ((ArvUvspPacket *) &copy, ARV_DEBUG_LEVEL_DEBUG);
                        }

			switch (packet_type) {
				case ARV_UVSP_PACKET_TYPE_LEADER:
                                        /* After a lost trailer, the leader may be in the buffer still open, which
                                         * belongs to the application once pushed below: the copy is parsed */
					if (buffer != NULL) {
						arv_info_stream_thread ("New leader received while a buffer is still open");
						buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                                                if (buffer->priv->has_gendc)
                                                        _gendc_clamp_part_size (buffer, offset);
						arv_stream_push_output_buffer (thread_data->stream, buffer);
						if (thread_data->callback != NULL)
							thread_data->callback (thread_data->callback_data,
									       ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE,
									       buffer);
						thread_data->statistics.n_failures++;
                                                g_atomic_int_dec_and_test(&thread_data->n_buffer_in_use);
						buffer = NULL;
					}
					buffer = arv_stream_pop_input_buffer (thread_data->stream);
					if (buffer != NULL) {
                                                g_atomic_int_inc(&thread_data->n_buffer_in_use);
						buffer->priv->system_timestamp_ns = g_get_real_time () * 1000LL;
						buffer->priv->status = ARV_BUFFER_STATUS_FILLING;
                                                buffer->priv->received_size = 0;
                                                buffer->priv->has_gendc = FALSE;
                                                _set_buffer_leader (buffer, (ArvUvspPacket *) &copy);
						offset = 0;
                                                after_payload = 0;
                                                /* A leader that filled its read may go on in the next one
                                                 * (WinUSB partial reads): the reads of the frame would be out of
                                                 * step with it, the frame fails */
                                                if (transferred == size) {
                                                        arv_info_stream_thread ("Leader cut short by a read of %"
                                                                                G_GSIZE_FORMAT " bytes", size);
                                                        buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
                                                }
						if (thread_data->callback != NULL)
							thread_data->callback (thread_data->callback_data,
									       ARV_STREAM_CALLBACK_TYPE_START_BUFFER,
									       NULL);
                                                thread_data->statistics.n_transferred_bytes += transferred;
                                        } else {
                                                thread_data->statistics.n_underruns++;
                                                thread_data->statistics.n_ignored_bytes += transferred;
                                        }
                                        break;
				case ARV_UVSP_PACKET_TYPE_TRAILER:
					if (buffer != NULL) {
						arv_debug_stream_thread ("Received %" G_GUINT64_FORMAT " bytes",
								       offset);

                                                /* A frame with a failed read stays failed, whatever its size. The
                                                 * padding of the final transfer after a complete payload does not fail
                                                 * it: more bytes than that did already (see the data case below). */
                                                if (buffer->priv->status == ARV_BUFFER_STATUS_MISSING_PACKETS) {
                                                        arv_info_stream_thread ("Image with missing data dropped "
                                                                                "(received %" G_GUINT64_FORMAT
                                                                                " / expected %" G_GSIZE_FORMAT ")",
                                                                                offset, thread_data->expected_size);
                                                } else if (offset != thread_data->expected_size) {
                                                        arv_info_stream_thread ("Incomplete image received, dropping "
                                                                                "(received %" G_GUINT64_FORMAT
                                                                                " / expected %" G_GSIZE_FORMAT ")",
                                                                                offset, thread_data->expected_size);
                                                        buffer->priv->status = ARV_BUFFER_STATUS_SIZE_MISMATCH;
                                                }

                                                if (buffer->priv->status == ARV_BUFFER_STATUS_MISSING_PACKETS ||
                                                    offset != thread_data->expected_size) {
                                                       if (buffer->priv->has_gendc)
                                                               _gendc_clamp_part_size (buffer, offset);
                                                       arv_stream_push_output_buffer (thread_data->stream, buffer);
                                                       if (thread_data->callback != NULL)
                                                               thread_data->callback (thread_data->callback_data,
                                                                                      ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE,
                                                                                      buffer);
                                                       thread_data->statistics.n_failures++;
                                                       thread_data->statistics.n_ignored_bytes += transferred;
                                                        g_atomic_int_dec_and_test(&thread_data->n_buffer_in_use);
                                                       buffer = NULL;
                                                } else {
                                                        buffer->priv->status = ARV_BUFFER_STATUS_SUCCESS;
                                                        buffer->priv->received_size = offset;
                                                        if (buffer->priv->has_gendc)
                                                                _gendc_clamp_part_size (buffer, offset);
                                                        else
                                                                buffer->priv->parts[0].size = offset;
                                                        arv_stream_push_output_buffer (thread_data->stream, buffer);
                                                        if (thread_data->callback != NULL)
                                                                thread_data->callback (thread_data->callback_data,
                                                                                       ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE,
                                                                                       buffer);
                                                        thread_data->statistics.n_completed_buffers++;
                                                        thread_data->statistics.n_transferred_bytes += transferred;
                                                        g_atomic_int_dec_and_test(&thread_data->n_buffer_in_use);
                                                        buffer = NULL;
                                                }
                                        }
                                        break;
                                case ARV_UVSP_PACKET_TYPE_DATA:
                                        if (buffer != NULL && buffer->priv->status == ARV_BUFFER_STATUS_FILLING &&
                                            offset < payload_end) {
                                                /* Read into the buffer, within the payload */
                                                offset += transferred;
                                                thread_data->statistics.n_transferred_bytes += transferred;

                                                if (buffer->priv->payload_type == ARV_BUFFER_PAYLOAD_TYPE_GENDC_CONTAINER){
                                                        _gendc_packet (buffer);
                                                }
                                        } else {
                                                /* With a buffer: the end of a payload larger than the buffer, or
                                                 * bytes after a complete payload, which do not go in the buffer. The
                                                 * final transfer may bring padding there. More bytes than that come
                                                 * from the next frames, the trailer lost, and the end of the payload
                                                 * may be the start of the next leader, which a read shorter than a
                                                 * packet header got after a payload cut short (WinUSB partial reads).
                                                 * The frame fails. */
                                                if (buffer != NULL && offset >= thread_data->expected_size) {
                                                        after_payload += transferred;
                                                        if (after_payload > padding_max &&
                                                            buffer->priv->status != ARV_BUFFER_STATUS_MISSING_PACKETS) {
                                                                arv_info_stream_thread ("%" G_GUINT64_FORMAT " bytes "
                                                                                        "after the payload, more than "
                                                                                        "the %" G_GSIZE_FORMAT " bytes "
                                                                                        "of padding: frame end lost",
                                                                                        after_payload, padding_max);
                                                                buffer->priv->status =
                                                                        ARV_BUFFER_STATUS_MISSING_PACKETS;
                                                        }
                                                }
                                                if (buffer != NULL && buffer->priv->status == ARV_BUFFER_STATUS_FILLING)
                                                        buffer->priv->status = ARV_BUFFER_STATUS_SIZE_MISMATCH;
                                                thread_data->statistics.n_ignored_bytes += transferred;
                                        }
                                        break;
                                default:
                                        arv_info_stream_thread ("Unknown packet type");
                                        break;
                        }
                }
        }

        if (buffer != NULL) {
		buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
                thread_data->statistics.n_aborted++;
                if (buffer->priv->has_gendc)
                        _gendc_clamp_part_size (buffer, offset);
		arv_stream_push_output_buffer (thread_data->stream, buffer);
		if (thread_data->callback != NULL)
			thread_data->callback (thread_data->callback_data,
					       ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE,
					       buffer);
                g_atomic_int_dec_and_test(&thread_data->n_buffer_in_use);
	}

        if (event_thread_priority)
                arv_uv_device_release_event_thread_priority (thread_data->uv_device);

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_EXIT, NULL);

	g_free (incoming_buffer);

	arv_info_stream_thread ("Stop USB3Vision sync stream thread");

	return NULL;
}

/* ArvUvStream implementation */

static guint32
align (guint32 val, guint32 alignment)
{
	/* Alignment must be a power of two, otherwise the used alignment algorithm does not work. */
	g_assert (alignment > 0 && (alignment & (alignment - 1)) == 0);

	return (val + (alignment - 1)) & ~(alignment - 1);
}

/* Least common multiple of the camera alignment and of the packet size */

static guint64
_lcm (guint64 a, guint64 b)
{
        guint64 x = a, y = b;

        while (y != 0) {
                guint64 r = x % y;

                x = y;
                y = r;
        }

        return a / x * b;
}

typedef enum {
        ARV_UV_STREAM_RAW_IO_DEFAULT,
        ARV_UV_STREAM_RAW_IO_OFF,
        ARV_UV_STREAM_RAW_IO_ON
} ArvUvStreamRawIoSetting;

/* The ARV_UV_RAW_IO environment variable, read at each acquisition start: "1", "on", "true", "yes" or "auto" uses
 * WinUSB's RAW_IO where it is available; "0", "off", "false" or "no" does not. Unset, empty or unknown, RAW_IO stays
 * off: no measurement shows yet what it brings, and its error handling was only checked against a model of WinUSB, not
 * with a camera (see the USB page of the documentation). */

static ArvUvStreamRawIoSetting
_get_raw_io_setting (void)
{
        static const char *off_values[] = { "0", "off", "false", "no" };
        static const char *on_values[] = { "1", "on", "true", "yes", "auto" };
        const char *value = g_getenv ("ARV_UV_RAW_IO");
        guint i;

        if (value == NULL || value[0] == '\0')
                return ARV_UV_STREAM_RAW_IO_DEFAULT;

        for (i = 0; i < G_N_ELEMENTS (off_values); i++)
                if (g_ascii_strcasecmp (value, off_values[i]) == 0)
                        return ARV_UV_STREAM_RAW_IO_OFF;

        for (i = 0; i < G_N_ELEMENTS (on_values); i++)
                if (g_ascii_strcasecmp (value, on_values[i]) == 0)
                        return ARV_UV_STREAM_RAW_IO_ON;

        arv_warning_stream ("Unknown ARV_UV_RAW_IO value '%s' (expected 1, on, auto, 0 or off), leaving RAW_IO off",
                            value);

        return ARV_UV_STREAM_RAW_IO_DEFAULT;
}

static gboolean
arv_uv_stream_start_acquisition (ArvStream *stream, GError **error)
{
	ArvUvStream *uv_stream = ARV_UV_STREAM (stream);
	ArvUvStreamPrivate *priv = arv_uv_stream_get_instance_private (uv_stream);
	ArvUvStreamThreadData *thread_data;
	ArvDevice *device;
        GError *local_error = NULL;
        guint64 sbrm_address;
        guint64 sirm_address;
	guint32 si_info;
	guint64 si_req_payload_size;
	guint32 si_req_leader_size;
	guint32 si_req_trailer_size;
	guint32 si_payload_size;
	guint32 si_payload_count;
	guint32 si_transfer1_size;
	guint32 si_transfer2_size;
	guint32 si_leader_size;
	guint32 si_trailer_size;
	guint32 si_control;
	guint32 alignment;
        guint32 packet_size;
        guint64 packet_unit = 0;
        guint64 maximum_transfer_size;
        guint64 transfer_size;
        ArvUvStreamRawIoSetting raw_io_setting;
        guint raw_io_maximum = 0;
        gboolean raw_io = FALSE;
        const char *raw_io_state;

	g_return_val_if_fail (priv->thread == NULL, FALSE);
	g_return_val_if_fail (priv->thread_data != NULL, FALSE);

	thread_data = priv->thread_data;

	device = ARV_DEVICE (thread_data->uv_device);

        /* A failed read leaves its value undefined: the layout below would be computed from it, and written to the
         * camera */
	if (!arv_device_read_memory (device, ARV_ABRM_SBRM_ADDRESS,
                                     sizeof (guint64), &sbrm_address, &local_error) ||
	    !arv_device_read_memory (device, sbrm_address + ARV_SBRM_SIRM_ADDRESS,
                                     sizeof (guint64), &sirm_address, &local_error) ||
	    !arv_device_read_memory (device, sirm_address + ARV_SIRM_INFO,
                                     sizeof (si_info), &si_info, &local_error) ||
	    !arv_device_read_memory (device, sirm_address + ARV_SIRM_REQ_PAYLOAD_SIZE,
                                     sizeof (si_req_payload_size), &si_req_payload_size, &local_error) ||
	    !arv_device_read_memory (device, sirm_address + ARV_SIRM_REQ_LEADER_SIZE,
                                     sizeof (si_req_leader_size), &si_req_leader_size, &local_error) ||
	    !arv_device_read_memory (device, sirm_address + ARV_SIRM_REQ_TRAILER_SIZE,
                                     sizeof (si_req_trailer_size), &si_req_trailer_size, &local_error)) {
                if (local_error == NULL)
                        local_error = g_error_new_literal (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
                                                           "Unknown error");
                g_propagate_prefixed_error (error, local_error, "Failed to read the stream interface registers: ");
                return FALSE;
        }

        priv->sirm_address = sirm_address;

	alignment = 1 << ((si_info & ARV_SIRM_INFO_ALIGNMENT_MASK) >> ARV_SIRM_INFO_ALIGNMENT_SHIFT);

	arv_info_stream ("SIRM_INFO             = 0x%08x", si_info);
	arv_info_stream ("SIRM_REQ_PAYLOAD_SIZE = 0x%016" G_GINT64_MODIFIER "x", si_req_payload_size);
	arv_info_stream ("SIRM_REQ_LEADER_SIZE  = 0x%08x", si_req_leader_size);
	arv_info_stream ("SIRM_REQ_TRAILER_SIZE = 0x%08x", si_req_trailer_size);

	arv_info_stream ("Required alignment    = %d", alignment);

	if (si_req_payload_size < 1) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_INVALID_PARAMETER,
		             "Device reported an invalid USB payload size (SIRM_REQ_PAYLOAD_SIZE)");
		return FALSE;
	}

        if (alignment == 0) {
                g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
                             "Device returned an invalid USB alignment");
                return FALSE;
        }

        maximum_transfer_size = MIN (priv->maximum_transfer_size, ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MAX);

        if (alignment > maximum_transfer_size) {
                g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_INVALID_PARAMETER,
                             "Invalid USB maximum transfer size (%" G_GINT64_MODIFIER "u)",
                             priv->maximum_transfer_size);
                return FALSE;
        }

        /* The transfer size: the largest multiple of both the camera alignment and the endpoint packet size that is not
         * larger than the maximum transfer size (a multiple of the alignment only, as before, when the packet size is
         * unknown or larger than the maximum) */
        packet_size = arv_uv_device_get_stream_packet_size (thread_data->uv_device);
        if (packet_size > 0)
                packet_unit = _lcm (alignment, packet_size);

        /* WinUSB RAW_IO (Windows, libusb 1.0.30 or later), in async mode only: sync mode has a single read in flight,
         * nothing to gain. With RAW_IO on, WinUSB passes the reads straight to the USB stack instead of queueing them
         * one at a time, but rejects any read that is not a whole number of packets, or larger than the RAW_IO maximum
         * transfer size. So:
         * - the payload transfers are a whole number of packets, no larger than that maximum, and the camera is told
         *   so (SIRM_PAYLOAD_SIZE);
         * - the leader and trailer reads are rounded up to whole packets, which the camera ends with a short packet,
         *   while the sizes written to the camera do not change;
         * - a payload read that is not a whole number of packets, normally the end of the buffer, goes through a
         *   bounce buffer (arv_uv_stream_payload_cb ()).
         * RAW_IO is turned on after the endpoint reset, before the stream thread starts, and off after the thread has
         * joined, as it must not change while transfers are pending. It stays off unless ARV_UV_RAW_IO turns it on, see
         * _get_raw_io_setting (): where it is available, the log says so. */
        raw_io_setting = _get_raw_io_setting ();
        if (priv->usb_mode != ARV_UV_USB_MODE_ASYNC) {
                raw_io_state = "off (sync mode)";
        } else if (raw_io_setting == ARV_UV_STREAM_RAW_IO_OFF) {
                raw_io_state = "off (ARV_UV_RAW_IO)";
        } else if (packet_unit == 0) {
                raw_io_state = "off (unknown packet size)";
        } else {
                raw_io_maximum = arv_uv_device_get_raw_io_maximum_transfer_size (thread_data->uv_device);

                if (raw_io_maximum == 0) {
                        raw_io_state = "off (not available)";
                } else if (raw_io_setting != ARV_UV_STREAM_RAW_IO_ON) {
                        raw_io_state = "off (default, ARV_UV_RAW_IO=1 turns it on)";
                } else if (packet_unit > MIN (maximum_transfer_size, raw_io_maximum) ||
                           _round_up (align (si_req_leader_size, alignment), packet_size) > raw_io_maximum ||
                           _round_up (align (si_req_trailer_size, alignment), packet_size) > raw_io_maximum) {
                        raw_io_state = "off (maximum transfer size too small)";
                } else {
                        raw_io = TRUE;
                        raw_io_state = "on";
                }
        }
        thread_data->raw_io_maximum = raw_io ? raw_io_maximum : 0;

        if (raw_io)
                transfer_size = MIN (maximum_transfer_size, thread_data->raw_io_maximum) / packet_unit * packet_unit;
        else if (packet_unit > 0 && packet_unit <= maximum_transfer_size)
                transfer_size = maximum_transfer_size / packet_unit * packet_unit;
        else
                transfer_size = maximum_transfer_size / alignment * alignment;

        /* keep a non mutated copy of the transfer size during the thread lifetime */
        thread_data->maximum_transfer_size = transfer_size;

	if (si_req_leader_size < 1) {
		arv_warning_stream ("Wrong SI_REQ_LEADER_SIZE value, using %" G_GUINT64_FORMAT " instead",
                                    transfer_size);
		si_leader_size = transfer_size;
	} else {
		si_leader_size = align (si_req_leader_size, alignment);
	}

	if (si_req_trailer_size < 1) {
		arv_warning_stream ("Wrong SI_REQ_TRAILER_SIZE value, using %" G_GUINT64_FORMAT " instead",
                                    transfer_size);
		si_trailer_size = transfer_size;
	} else {
		si_trailer_size = align (si_req_trailer_size, alignment);
	}

        if (raw_io) {
                /* Whole packets: the end of the payload goes in the final transfer */
                si_payload_size = MIN (si_req_payload_size / packet_unit * packet_unit, transfer_size);
                if (si_payload_size == 0)
                        /* Less than a packet: a single read, through a bounce buffer */
                        si_payload_size = si_req_payload_size;
        } else {
                si_payload_size = MIN (si_req_payload_size, transfer_size);
        }
	si_payload_count = si_req_payload_size / si_payload_size;
	si_transfer1_size = align(si_req_payload_size % si_payload_size, alignment);
	si_transfer2_size = 0;

	arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_MAX_LEADER_SIZE,
                                 sizeof (si_leader_size), &si_leader_size, NULL);
	arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_MAX_TRAILER_SIZE,
                                 sizeof (si_trailer_size), &si_trailer_size, NULL);
	arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_PAYLOAD_SIZE,
                                 sizeof (si_payload_size), &si_payload_size, NULL);
	arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_PAYLOAD_COUNT,
                                 sizeof (si_payload_count), &si_payload_count, NULL);
	arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_TRANSFER1_SIZE,
                                 sizeof (si_transfer1_size), &si_transfer1_size, NULL);
	arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_TRANSFER2_SIZE,
                                 sizeof (si_transfer2_size), &si_transfer2_size, NULL);

	arv_info_stream ("SIRM_PAYLOAD_SIZE     = 0x%08x", si_payload_size);
	arv_info_stream ("SIRM_PAYLOAD_COUNT    = 0x%08x", si_payload_count);
	arv_info_stream ("SIRM_TRANSFER1_SIZE   = 0x%08x", si_transfer1_size);
	arv_info_stream ("SIRM_TRANSFER2_SIZE   = 0x%08x", si_transfer2_size);
	arv_info_stream ("SIRM_MAX_LEADER_SIZE  = 0x%08x", si_leader_size);
	arv_info_stream ("SIRM_MAX_TRAILER_SIZE = 0x%08x", si_trailer_size);

        thread_data->expected_size = si_req_payload_size;
        thread_data->read_packet_size = raw_io ? packet_size : 0;
	thread_data->leader_size = raw_io ? _round_up (si_leader_size, packet_size) : si_leader_size;
	thread_data->payload_size = si_payload_size;
        thread_data->payload_count = si_payload_count;
        thread_data->transfer1_size = si_transfer1_size;
	thread_data->trailer_size = raw_io ? _round_up (si_trailer_size, packet_size) : si_trailer_size;
        thread_data->resync_needed = FALSE;
        thread_data->pipe_reset_needed = FALSE;
        thread_data->zero_length_packet = FALSE;
        thread_data->n_buffer_in_use = 0;
	thread_data->cancel = FALSE;

        /* Bytes in flight, from the maximum transfer size rather than from the transfer size, which the RAW_IO maximum
         * may cap: smaller reads, not fewer bytes in flight */
        thread_data->maximum_submit_total = ARV_UV_STREAM_N_MAXIMUM_SUBMITS * MAX (maximum_transfer_size, transfer_size);

        thread_data->total_submitted_bytes = 0;
        thread_data->transfer_failed = FALSE;
        thread_data->n_completed_transfers = 0;
        thread_data->n_failed_transfers = 0;
        thread_data->has_hotplug = arv_uv_device_has_hotplug (thread_data->uv_device);
        thread_data->check_pending = FALSE;
        thread_data->last_check_time = 0;
        thread_data->last_n_completed_transfers = 0;
        thread_data->last_activity_time = g_get_monotonic_time ();
        thread_data->last_n_failed_transfers = 0;
        thread_data->raw_io_off_needed = FALSE;
        thread_data->last_recovery_time = 0;
        thread_data->resync_limit_warned = FALSE;

        arv_uv_device_reset_stream_endpoint (thread_data->uv_device);

        si_control = ARV_SIRM_CONTROL_STREAM_ENABLE;
        arv_device_write_memory (device, priv->sirm_address + ARV_SIRM_CONTROL, sizeof (si_control), &si_control,
                                 &local_error);
        if (local_error != NULL) {
                arv_warning_stream ("Failed to enable stream (%s)",
                                    local_error->message);
                g_propagate_error (error, local_error);
                return FALSE;
        }

        /* Nothing in flight yet. If it fails, the reads stay whole packets, which needs no RAW_IO. */
        if (raw_io && !arv_uv_device_set_raw_io (thread_data->uv_device, TRUE)) {
                raw_io = FALSE;
                raw_io_state = "off (failed to turn it on)";
        }
        g_atomic_int_set (&thread_data->raw_io, raw_io);

        /* This stream owns the setup of the stream interface now, which arv_uv_stream_stop_acquisition () undoes */
        priv->stream_interface_owner = arv_uv_device_take_stream_interface (thread_data->uv_device);

        if (raw_io)
                arv_info_stream ("Transfer size %" G_GUINT64_FORMAT " bytes (maximum %" G_GUINT64_FORMAT "), "
                                 "packet size %u bytes, RAW_IO on (maximum %u bytes)",
                                 transfer_size, priv->maximum_transfer_size, packet_size, thread_data->raw_io_maximum);
        else
                arv_info_stream ("Transfer size %" G_GUINT64_FORMAT " bytes (maximum %" G_GUINT64_FORMAT "), "
                                 "packet size %u bytes, RAW_IO %s",
                                 transfer_size, priv->maximum_transfer_size, packet_size, raw_io_state);
        arv_info_stream ("Reads: leader %" G_GSIZE_FORMAT ", payload %u x %u + %u, trailer %" G_GSIZE_FORMAT " bytes",
                         thread_data->leader_size, si_payload_size, si_payload_count, si_transfer1_size,
                         thread_data->trailer_size);
        if (priv->usb_mode == ARV_UV_USB_MODE_ASYNC)
                arv_info_stream ("Reads in flight: up to %" G_GUINT64_FORMAT " bytes",
                                 thread_data->maximum_submit_total);

        switch (priv->usb_mode) {
                case ARV_UV_USB_MODE_SYNC:
                        priv->thread = g_thread_new ("arv_uv_stream", arv_uv_stream_thread_sync, priv->thread_data);
                        break;
                case ARV_UV_USB_MODE_ASYNC:
                        priv->thread = g_thread_new ("arv_uv_stream", arv_uv_stream_thread_async, priv->thread_data);
                        break;
                default:
                        g_assert_not_reached ();
        }

        g_mutex_lock (&thread_data->thread_started_mutex);
        while (!thread_data->thread_started)
                g_cond_wait (&thread_data->thread_started_cond,
                             &thread_data->thread_started_mutex);
        g_mutex_unlock (&thread_data->thread_started_mutex);

        return TRUE;
}

static gboolean
arv_uv_stream_stop_acquisition (ArvStream *stream, GError **error)
{
	ArvUvStream *uv_stream = ARV_UV_STREAM (stream);
	ArvUvStreamPrivate *priv = arv_uv_stream_get_instance_private (uv_stream);
	ArvUvStreamThreadData *thread_data;
	guint32 si_control;
        GError *local_error = NULL;
        gboolean raw_io;

	g_return_val_if_fail (priv->thread != NULL, FALSE);
	g_return_val_if_fail (priv->thread_data != NULL, FALSE);

	thread_data = priv->thread_data;

	g_atomic_int_set (&priv->thread_data->cancel, TRUE);
	g_cond_broadcast (&priv->thread_data->stream_event);
	g_thread_join (priv->thread);

	priv->thread = NULL;

        raw_io = g_atomic_int_get (&thread_data->raw_io);
        g_atomic_int_set (&thread_data->raw_io, FALSE);

        /* What the start set up for the whole device is undone by the stream that set it up last only. Another one,
         * started since, streams with it: its streaming must not be disabled, nor RAW_IO changed under its pending
         * transfers. */
        if (!arv_uv_device_release_stream_interface (thread_data->uv_device, priv->stream_interface_owner)) {
                arv_info_stream ("Stream interface set up by another stream since, left as it is");
                return TRUE;
        }

        /* The stream thread has taken every transfer back before returning: RAW_IO can change now. Unless the thread
         * turned it off already. */
        if (raw_io && arv_uv_device_is_connected (thread_data->uv_device))
                arv_uv_device_set_raw_io (thread_data->uv_device, FALSE);

	si_control = 0x0;
	arv_device_write_memory (ARV_DEVICE (thread_data->uv_device),
				 priv->sirm_address + ARV_SIRM_CONTROL, sizeof (si_control), &si_control, &local_error);
        if (local_error != NULL) {
                arv_warning_stream ("Failed to disable stream (%s)",
                                    local_error->message);
                g_propagate_error (error, local_error);
                return FALSE;
        }

        return TRUE;
}

static void
_buffer_data_destroy_func (gpointer data)
{
        ArvUvStreamBufferData *buffer_data = data;

        arv_uv_device_usb_mem_free (buffer_data->uv_device, buffer_data->data, buffer_data->allocated_size);
        g_object_unref (buffer_data->uv_device);
        g_free (buffer_data);
}

static gboolean
arv_uv_stream_create_buffers (ArvStream *stream, guint n_buffers, size_t size,
                                 void *user_data, GDestroyNotify user_data_destroy_func,
                                 GError **error)
{
	ArvUvStream *uv_stream = ARV_UV_STREAM (stream);
	ArvUvStreamPrivate *priv = arv_uv_stream_get_instance_private (uv_stream);
        ArvUvDevice *uv_device = priv->thread_data->uv_device;
        unsigned char *usb_buffer;
        guint i;

        for (i = 0; i < n_buffers; i++) {
                ArvBuffer *buffer;

                usb_buffer = arv_uv_device_usb_mem_alloc (uv_device, size);
                if (usb_buffer != NULL) {
                        ArvUvStreamBufferData *buffer_data;

                        buffer = arv_buffer_new_full (size, usb_buffer, user_data, user_data_destroy_func);

                        buffer_data = g_new0 (ArvUvStreamBufferData, 1);
                        buffer_data->uv_device = g_object_ref(uv_device);
                        buffer_data->data = buffer->priv->data;
                        buffer_data->allocated_size = buffer->priv->allocated_size;

                        g_object_set_data_full (G_OBJECT (buffer), "uv-buffer-data",
                                                buffer_data, _buffer_data_destroy_func);
                } else {
                        buffer = arv_buffer_new_full (size, NULL, user_data, user_data_destroy_func);
                }
                arv_stream_push_buffer (stream, buffer);
        }

        return TRUE;
}

/**
 * arv_uv_stream_new: (skip)
 * @uv_device: a #ArvUvDevice
 * @callback: (scope call): image processing callback
 * @callback_data: (closure): user data for @callback
 * @destroy: callback data destroy function
 * @usb_mode: USB mode selection
 * @maximum_transfer_size: maximum transfer size, in bytes
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Return Value: (transfer full): a new #ArvStream.
 */

ArvStream *
arv_uv_stream_new (ArvUvDevice *uv_device, ArvStreamCallback callback, void *callback_data, GDestroyNotify destroy,
                   ArvUvUsbMode usb_mode, guint64 maximum_transfer_size, GError **error)
{
	return g_initable_new (ARV_TYPE_UV_STREAM, NULL, error,
			       "device", uv_device,
			       "callback", callback,
			       "callback-data", callback_data,
                               "destroy-notify", destroy,
			       "usb-mode", usb_mode,
                               "maximum-transfer-size", maximum_transfer_size,
			       NULL);
}

static void
arv_uv_stream_constructed (GObject *object)
{
	ArvUvStream *uv_stream = ARV_UV_STREAM (object);
	ArvStream *stream = ARV_STREAM (uv_stream);
	ArvUvStreamPrivate *priv = arv_uv_stream_get_instance_private (uv_stream);
	ArvUvStreamThreadData *thread_data;

        G_OBJECT_CLASS (arv_uv_stream_parent_class)->constructed (object);

	thread_data = g_new0 (ArvUvStreamThreadData, 1);
	thread_data->stream = stream;

	g_cond_init( &thread_data->stream_event );
	g_mutex_init( &thread_data->stream_mtx );

	thread_data->statistics.n_completed_buffers = 0;
	thread_data->statistics.n_failures = 0;
	thread_data->statistics.n_underruns = 0;
        thread_data->statistics.n_aborted = 0;
	thread_data->statistics.n_transferred_bytes = 0;
	thread_data->statistics.n_ignored_bytes = 0;

	g_object_get (object,
		      "device", &thread_data->uv_device,
		      "callback", &thread_data->callback,
		      "callback-data", &thread_data->callback_data,
		      NULL);

	priv->thread_data = thread_data;

        arv_stream_declare_info (ARV_STREAM (uv_stream), "n_completed_buffers",
                                 G_TYPE_UINT64, &thread_data->statistics.n_completed_buffers);
        arv_stream_declare_info (ARV_STREAM (uv_stream), "n_failures",
                                 G_TYPE_UINT64, &thread_data->statistics.n_failures);
        arv_stream_declare_info (ARV_STREAM (uv_stream), "n_underruns",
                                 G_TYPE_UINT64, &thread_data->statistics.n_underruns);
        arv_stream_declare_info (ARV_STREAM (uv_stream), "n_aborted",
                                 G_TYPE_UINT64, &thread_data->statistics.n_aborted);
        arv_stream_declare_info (ARV_STREAM (uv_stream), "n_transferred_bytes",
                                 G_TYPE_UINT64, &thread_data->statistics.n_transferred_bytes);
        arv_stream_declare_info (ARV_STREAM (uv_stream), "n_ignored_bytes",
                                 G_TYPE_UINT64, &thread_data->statistics.n_ignored_bytes);
}

/* ArvStream implementation */

static void
arv_uv_stream_set_property (GObject * object, guint prop_id,
                            const GValue * value, GParamSpec * pspec)
{
       ArvUvStreamPrivate *priv = arv_uv_stream_get_instance_private (ARV_UV_STREAM (object));

       switch (prop_id) {
               case ARV_UV_STREAM_PROPERTY_USB_MODE:
                       priv->usb_mode = g_value_get_enum(value);
                       break;
               case ARV_UV_STREAM_PROPERTY_MAXIMUM_TRANSFER_SIZE:
                       priv->maximum_transfer_size = g_value_get_uint64(value);
                       break;
               default:
                       G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                       break;
       }
}

static void
arv_uv_stream_init (ArvUvStream *uv_stream)
{

}

static void
arv_uv_stream_finalize (GObject *object)
{
	ArvUvStream *uv_stream = ARV_UV_STREAM (object);
	ArvUvStreamPrivate *priv = arv_uv_stream_get_instance_private (uv_stream);

        if (priv->thread != NULL)
                arv_uv_stream_stop_acquisition (ARV_STREAM (uv_stream), NULL);

	if (priv->thread_data != NULL) {
		ArvUvStreamThreadData *thread_data;

		thread_data = priv->thread_data;

		arv_info_stream ("[UvStream::finalize] n_completed_buffers    = %" G_GUINT64_FORMAT,
				  thread_data->statistics.n_completed_buffers);
		arv_info_stream ("[UvStream::finalize] n_failures             = %" G_GUINT64_FORMAT,
				  thread_data->statistics.n_failures);
		arv_info_stream ("[UvStream::finalize] n_underruns            = %" G_GUINT64_FORMAT,
				  thread_data->statistics.n_underruns);
		arv_info_stream ("[UvStream::finalize] n_aborted              = %" G_GUINT64_FORMAT,
				  thread_data->statistics.n_aborted);

		arv_info_stream ("[UvStream::finalize] n_transferred_bytes    = %" G_GUINT64_FORMAT,
				  thread_data->statistics.n_transferred_bytes);
		arv_info_stream ("[UvStream::finalize] n_ignored_bytes        = %" G_GUINT64_FORMAT,
				  thread_data->statistics.n_ignored_bytes);

		g_mutex_clear (&thread_data->stream_mtx);
		g_cond_clear (&thread_data->stream_event);

		g_clear_object (&thread_data->uv_device);
		g_clear_pointer (&priv->thread_data, g_free);
	}

	G_OBJECT_CLASS (arv_uv_stream_parent_class)->finalize (object);
}

static void
arv_uv_stream_class_init (ArvUvStreamClass *uv_stream_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (uv_stream_class);
	ArvStreamClass *stream_class = ARV_STREAM_CLASS (uv_stream_class);

	object_class->constructed = arv_uv_stream_constructed;
	object_class->finalize = arv_uv_stream_finalize;
	object_class->set_property = arv_uv_stream_set_property;

	stream_class->start_acquisition = arv_uv_stream_start_acquisition;
	stream_class->stop_acquisition = arv_uv_stream_stop_acquisition;
        stream_class->create_buffers = arv_uv_stream_create_buffers;

         /**
          * ArvUvStream:usb-mode:
          *
          * USB device I/O mode.
          */
        g_object_class_install_property (
                object_class, ARV_UV_STREAM_PROPERTY_USB_MODE,
                g_param_spec_enum ("usb-mode", "USB mode",
                                   "USB device I/O mode",
                                   ARV_TYPE_UV_USB_MODE,
                                   ARV_UV_USB_MODE_DEFAULT,
				   G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)
		);

        /**
         * ArvUvStream:maximum-transfer-size:
         *
         * Maximum size when asking for a USB bulk transfer.
         */
        g_object_class_install_property (
                object_class, ARV_UV_STREAM_PROPERTY_MAXIMUM_TRANSFER_SIZE,
                g_param_spec_uint64 ("maximum-transfer-size", "Maximum transfer size",
                                     "USB maximum transfer size",
                                     1024, 1024 * 1024 * 1024,
                                     ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_DEFAULT,
                                     G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)
                );
}
