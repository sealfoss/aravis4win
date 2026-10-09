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
 * Author: Xiaoqiang Wang <xiaoqiang.wang@psi.ch>
 */

/**
 * SECTION: arvgentlstream
 * @short_description: GenTL stream
 */

#include <arvdebugprivate.h>
#include <arvgentlsystemprivate.h>
#include <arvgentlstreamprivate.h>
#include <arvgentldeviceprivate.h>
#include <arvstreamprivate.h>
#include <arvbufferprivate.h>
#include <arvmiscprivate.h>
#include <arvdebug.h>
#include <stdio.h>

/* The stream thread waits for the new buffer events with a finite timeout, and checks for a stop request between two
 * waits: a producer may lose the EventKill of a stop requested while the thread doesn't wait, or ignore it. */
#define ARV_GENTL_STREAM_EVENT_TIMEOUT_MS		50
/* Period of the stop request checks while the producer holds no buffer, and the thread waits for the application to
 * push one */
#define ARV_GENTL_STREAM_POP_INPUT_BUFFER_TIMEOUT_MS	10

/* This structure allows to Revoke a GenTL buffer even after ArvStream object is finalized, which may happen if a buffer
 * is poped and never returned to the stream queues. We keep a DataStream handle and a reference to the parent device,
 * and attach a referenced pointer of this data to each buffer as "gentl-buffer-data", to be used in the buffer data
 * destroy callback. Once the stream and every buffer have been destroyed, the reference to the parent device will be
 * also released. */

typedef struct {
        gatomicrefcount ref_count;
	DS_HANDLE data_stream;
        ArvGenTLDevice *device;
} ArvGenTLDataStream;

static ArvGenTLDataStream *
arv_gentl_data_stream_new (ArvGenTLDevice *device, GError **error)
{
        ArvGenTLDataStream *gentl_data_stream;
        DS_HANDLE data_stream;

        g_return_val_if_fail(ARV_IS_GENTL_DEVICE(device), NULL);

        data_stream = arv_gentl_device_open_stream_handle (device, error);
        if (data_stream == NULL)
                return NULL;

        gentl_data_stream = g_new0 (ArvGenTLDataStream, 1);
        gentl_data_stream->data_stream = data_stream;
        gentl_data_stream->device = g_object_ref (device);
        g_atomic_ref_count_init (&gentl_data_stream->ref_count);

        return gentl_data_stream;
}

static ArvGenTLDataStream *
arv_gentl_data_stream_ref (ArvGenTLDataStream *gentl_data_stream)
{
        g_return_val_if_fail (gentl_data_stream != NULL, NULL);

        g_atomic_ref_count_inc (&gentl_data_stream->ref_count);

        return gentl_data_stream;
}

static void
arv_gentl_data_stream_unref (ArvGenTLDataStream *gentl_data_stream)
{
        g_return_if_fail(gentl_data_stream != NULL);

        if (g_atomic_ref_count_dec (&gentl_data_stream->ref_count)) {
                ArvGenTLSystem *gentl_system = arv_gentl_device_get_system(gentl_data_stream->device);
                ArvGenTLModule *gentl = arv_gentl_system_get_gentl(gentl_system);
                GC_ERROR error;

                arv_info_stream ("[GenTLDataStream::unref] close data stream");
                error = gentl->DSClose (gentl_data_stream->data_stream);
                if (error != GC_ERR_SUCCESS) {
                        arv_warning_stream ("[GenTLDataStream::unref] DSClose error (%s)",
                                            arv_gentl_gc_error_to_string(error));
                }

                g_clear_object (&gentl_data_stream->device);
                g_free (gentl_data_stream);
        }
}

/* Error of a GenTL function, with the description of the producer if it gives one */

static void
_set_gentl_error (GError **error, ArvGenTLModule *gentl, const char *function, GC_ERROR gc_error)
{
	char text[256];
	size_t size = sizeof (text);
	GC_ERROR last_error = GC_ERR_SUCCESS;

	text[0] = '\0';
	if (gentl->GCGetLastError == NULL ||
	    gentl->GCGetLastError (&last_error, text, &size) != GC_ERR_SUCCESS ||
	    last_error != gc_error)
		text[0] = '\0';
	text[sizeof (text) - 1] = '\0';

	if (text[0] != '\0')
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR, "%s error (%s: %s)",
			     function, arv_gentl_gc_error_to_string (gc_error), text);
	else
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR, "%s error (%s)",
			     function, arv_gentl_gc_error_to_string (gc_error));
}

enum
{
	PROP_0,
	PROP_STREAM_N_BUFFERS
};

typedef struct {
        ArvGenTLDataStream *gentl_data_stream;
        BUFFER_HANDLE gentl_buffer;
} ArvGenTLStreamBufferData;

static void
_buffer_data_destroy_func (gpointer data)
{
        ArvGenTLStreamBufferData *buffer_data = data;
        ArvGenTLSystem *gentl_system = arv_gentl_device_get_system(buffer_data->gentl_data_stream->device);
        ArvGenTLModule *gentl = arv_gentl_system_get_gentl(gentl_system);

        gentl->DSRevokeBuffer(buffer_data->gentl_data_stream->data_stream, buffer_data->gentl_buffer, NULL, NULL);

        arv_gentl_data_stream_unref(buffer_data->gentl_data_stream);

        g_free (buffer_data);
}

/* Announces the memory of a buffer which was not created by arv_stream_create_buffers() */

static ArvGenTLStreamBufferData *
_announce_buffer (ArvGenTLModule *gentl, ArvGenTLDataStream *gentl_data_stream, ArvBuffer *buffer, GError **error)
{
	ArvGenTLStreamBufferData *buffer_data;
	BUFFER_HANDLE gentl_buffer = NULL;
	GC_ERROR gc_error;

	gc_error = gentl->DSAnnounceBuffer (gentl_data_stream->data_stream,
					    buffer->priv->data, buffer->priv->allocated_size,
					    NULL, &gentl_buffer);
	if (gc_error != GC_ERR_SUCCESS) {
		_set_gentl_error (error, gentl, "DSAnnounceBuffer", gc_error);
		return NULL;
	}

	buffer_data = g_new0 (ArvGenTLStreamBufferData, 1);
	buffer_data->gentl_buffer = gentl_buffer;
	buffer_data->gentl_data_stream = arv_gentl_data_stream_ref (gentl_data_stream);

	g_object_set_data_full (G_OBJECT (buffer), "gentl-buffer-data", buffer_data, _buffer_data_destroy_func);

	return buffer_data;
}

typedef struct {
	ArvStream *stream;

	gboolean thread_started;
	GMutex thread_started_mutex;
	GCond thread_started_cond;

	ArvStreamCallback callback;
	void *callback_data;

	/* Set before the thread creation */
	ArvGenTLModule *gentl;
	ArvGenTLDataStream *gentl_data_stream;
	EVENT_HANDLE event_handle;
	uint64_t timestamp_tick_frequency;
	/* GenTL buffer handle -> ArvBuffer, for the buffers held by the producer. Owned by the thread once started. */
	GHashTable *buffers;

	/* Statistics */
	guint64 n_completed_buffers;
	guint64 n_failures;
	guint64 n_underruns;
	guint64 n_aborted;

	guint64 n_transferred_bytes;

	/* Stop request, and its notification */
	gint cancel;
	GMutex stream_mtx;
	GCond stream_event;
} ArvGenTLStreamThreadData;

typedef struct {
	ArvGenTLDevice *gentl_device;
        ArvGenTLDataStream *gentl_data_stream;
	uint64_t timestamp_tick_frequency;

	guint n_buffers;

	GThread *thread;
	ArvGenTLStreamThreadData *thread_data;
} ArvGenTLStreamPrivate;

struct _ArvGenTLStream {
	ArvStream stream;
};

struct _ArvGenTLStreamClass {
	ArvStreamClass parent_class;
};

G_DEFINE_TYPE_WITH_CODE (ArvGenTLStream, arv_gentl_stream, ARV_TYPE_STREAM, G_ADD_PRIVATE (ArvGenTLStream))

/* Acquisition thread */

static gboolean
_gentl_buffer_info_ptr(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer,
                       BUFFER_INFO_CMD info_cmd, void** value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(void*);
	*value = NULL;
	error = gentl->DSGetBufferInfo(datastream, buffer, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

static gboolean
_gentl_buffer_info_bool8(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer,
                         BUFFER_INFO_CMD info_cmd, bool8_t *value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(bool8_t);
	*value = 0;
	error = gentl->DSGetBufferInfo(datastream, buffer, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

static gboolean
_gentl_buffer_info_sizet(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer,
                         BUFFER_INFO_CMD info_cmd, size_t *value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(size_t);
	*value = 0;
	error = gentl->DSGetBufferInfo(datastream, buffer, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

static gboolean
_gentl_buffer_info_uint64(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer,
                          BUFFER_INFO_CMD info_cmd, uint64_t *value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(uint64_t);
	*value = 0;
	error = gentl->DSGetBufferInfo(datastream, buffer, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

static gboolean
_gentl_buffer_part_info_ptr(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer, uint32_t index,
                            BUFFER_INFO_CMD info_cmd, void** value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(void*);
	*value = NULL;
	error = gentl->DSGetBufferPartInfo(datastream, buffer, index, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

static gboolean
_gentl_buffer_part_info_sizet(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer, uint32_t index,
                              BUFFER_INFO_CMD info_cmd, size_t *value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(size_t);
	*value = 0;
	error = gentl->DSGetBufferPartInfo(datastream, buffer, index, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

static gboolean
_gentl_buffer_part_info_uint64(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE buffer, uint32_t index,
                               BUFFER_INFO_CMD info_cmd, uint64_t *value)
{
	GC_ERROR error;
	INFO_DATATYPE type;
	size_t size = sizeof(uint64_t);
	*value = 0;
	error = gentl->DSGetBufferPartInfo(datastream, buffer, index, info_cmd, &type, value, &size);
	return error == GC_ERR_SUCCESS;
}

/* Keeps a part of @arv_buffer in its memory, whatever the producer tells: the data accessors, and aravissrc which lends
 * the data as it is, must not reach past it. Returns TRUE if the part had to be cut. */

static gboolean
_clamp_part_to_buffer (ArvBuffer *arv_buffer, guint part_id)
{
	ArvBufferPartInfos *part = &arv_buffer->priv->parts[part_id];
	size_t allocated_size = arv_buffer->priv->allocated_size;

	if (part->data_offset < 0 || (size_t) part->data_offset > allocated_size) {
		part->data_offset = 0;
		part->size = 0;
		return TRUE;
	}

	if (part->size > allocated_size - (size_t) part->data_offset) {
		part->size = allocated_size - (size_t) part->data_offset;
		return TRUE;
	}

	return FALSE;
}

static void
_gentl_buffer_to_arv_buffer(ArvGenTLModule *gentl, DS_HANDLE datastream, BUFFER_HANDLE gentl_buffer,
                            ArvBuffer *arv_buffer, uint64_t timestamp_tick_frequency)
{
	size_t payload_type = PAYLOAD_TYPE_UNKNOWN;
	bool8_t has_chunks = FALSE;
	bool8_t is_incomplete = FALSE;
	bool8_t is_data_larger = FALSE;
	gboolean has_payload_type;
	gboolean has_size_filled;
	gboolean is_larger_than_buffer;
	uint32_t num_parts;
	size_t data_size, data_type, actual_size, image_offset, width, height, x_offset, y_offset, x_padding, y_padding;
	uint64_t source_id, pixel_format, frame_id, timestamp = 0;
	void *data;

	has_payload_type = _gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_PAYLOADTYPE,
						    &payload_type);
	switch(payload_type) {
		case PAYLOAD_TYPE_UNKNOWN:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_UNKNOWN;
			break;
		case PAYLOAD_TYPE_IMAGE:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_IMAGE;
			break;
		case PAYLOAD_TYPE_RAW_DATA:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_RAWDATA;
			break;
		case PAYLOAD_TYPE_FILE:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_FILE;
			break;
		case PAYLOAD_TYPE_CHUNK_DATA:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_EXTENDED_CHUNK_DATA;
			break;
		case PAYLOAD_TYPE_CHUNK_ONLY:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_CHUNK_DATA;
			break;
		case PAYLOAD_TYPE_JPEG:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_JPEG;
			break;
		case PAYLOAD_TYPE_JPEG2000:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_JPEG2000;
			break;
		case PAYLOAD_TYPE_H264:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_H264;
			break;
		case PAYLOAD_TYPE_MULTI_PART:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_MULTIPART;
			break;
		default:
			arv_buffer->priv->payload_type = ARV_BUFFER_PAYLOAD_TYPE_UNKNOWN;
			arv_buffer->priv->status = ARV_BUFFER_STATUS_PAYLOAD_NOT_SUPPORTED;
			return;
	}

	_gentl_buffer_info_bool8(gentl, datastream, gentl_buffer, BUFFER_INFO_CONTAINS_CHUNKDATA, &has_chunks);
	arv_buffer->priv->has_chunks =
                has_chunks ||
                payload_type == PAYLOAD_TYPE_CHUNK_DATA ||
                payload_type == PAYLOAD_TYPE_CHUNK_ONLY;

	_gentl_buffer_info_uint64(gentl, datastream, gentl_buffer, BUFFER_INFO_FRAMEID, &frame_id);
	arv_buffer->priv->frame_id = frame_id;

	if (!_gentl_buffer_info_uint64(gentl, datastream, gentl_buffer, BUFFER_INFO_TIMESTAMP_NS, &timestamp)) {
		uint64_t timestamp_ticks = 0;
		if (timestamp_tick_frequency &&
		    _gentl_buffer_info_uint64(gentl, datastream, gentl_buffer, BUFFER_INFO_TIMESTAMP, &timestamp_ticks)) {
			timestamp = timestamp_ticks / timestamp_tick_frequency * 1000000000
				  + ((timestamp_ticks % timestamp_tick_frequency) * 1000000000) / timestamp_tick_frequency;
		} else {
			timestamp = g_get_real_time() * 1000LL;
		}
	}
	arv_buffer->priv->timestamp_ns = timestamp;

	_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_DATA_SIZE, &data_size);
	has_size_filled = _gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_SIZE_FILLED,
						   &actual_size);

	/* Filled beyond the buffer memory, by the producer's account: arv_buffer_get_data() must not return more */
	is_larger_than_buffer = actual_size > arv_buffer->priv->allocated_size;
	arv_buffer->priv->received_size = MIN (actual_size, arv_buffer->priv->allocated_size);

	_gentl_buffer_info_ptr(gentl, datastream, gentl_buffer, BUFFER_INFO_BASE, &data);
        if (data != NULL && data != arv_buffer->priv->data)
                memcpy(arv_buffer->priv->data, data, MIN (data_size, arv_buffer->priv->allocated_size));

	if (payload_type == PAYLOAD_TYPE_CHUNK_ONLY) {
		arv_buffer_set_n_parts(arv_buffer, 0);
	} else if (gentl->DSGetNumBufferParts != NULL &&
		   gentl->DSGetBufferPartInfo != NULL &&
		   gentl->DSGetNumBufferParts(datastream, gentl_buffer, &num_parts) == GC_ERR_SUCCESS) {
		arv_buffer_set_n_parts(arv_buffer, num_parts);
		for (uint32_t i=0; i<num_parts; i++) {
			void *part_data;
			size_t part_data_size;

			_gentl_buffer_part_info_uint64(gentl, datastream, gentl_buffer, i,
                                                       BUFFER_PART_INFO_SOURCE_ID, &source_id);
			arv_buffer->priv->parts[i].component_id = source_id;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_DATA_TYPE, &data_type);
			arv_buffer->priv->parts[i].data_type = data_type;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_DATA_SIZE, &part_data_size);
			arv_buffer->priv->parts[i].size = part_data_size;

			_gentl_buffer_part_info_ptr(gentl, datastream, gentl_buffer, i,
                                                    BUFFER_PART_INFO_BASE, &part_data);
			arv_buffer->priv->parts[i].data_offset = (char*)part_data - (char*)data;

			_gentl_buffer_part_info_uint64(gentl, datastream, gentl_buffer, i,
                                                       BUFFER_PART_INFO_DATA_FORMAT, &pixel_format);
			arv_buffer->priv->parts[i].pixel_format = pixel_format;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_WIDTH, &width);
			arv_buffer->priv->parts[i].width = width;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_HEIGHT, &height);
			arv_buffer->priv->parts[i].height = height;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_XOFFSET, &x_offset);
			arv_buffer->priv->parts[i].x_offset = x_offset;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_YOFFSET, &y_offset);
			arv_buffer->priv->parts[i].y_offset = y_offset;

			_gentl_buffer_part_info_sizet(gentl, datastream, gentl_buffer, i,
                                                      BUFFER_PART_INFO_XPADDING, &x_padding);
			arv_buffer->priv->parts[i].x_padding = x_padding;

			if (_clamp_part_to_buffer (arv_buffer, i))
				is_larger_than_buffer = TRUE;
		}
	} else {
		arv_buffer_set_n_parts(arv_buffer, 1);

		arv_buffer->priv->parts[0].component_id = 0;
		arv_buffer->priv->parts[0].data_type = ARV_BUFFER_PART_DATA_TYPE_2D_IMAGE;

		/* The data size counts the data from the start of the buffer, and the image starts at the image offset in
		 * it, after chunk data or an alignment gap for example: the image part ends with the data */
		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_IMAGEOFFSET, &image_offset);
		arv_buffer->priv->parts[0].data_offset = image_offset;
		arv_buffer->priv->parts[0].size = data_size > image_offset ? data_size - image_offset : 0;

		_gentl_buffer_info_uint64(gentl, datastream, gentl_buffer, BUFFER_INFO_PIXELFORMAT, &pixel_format);
		arv_buffer->priv->parts[0].pixel_format = pixel_format;

		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_WIDTH, &width);
		arv_buffer->priv->parts[0].width = width;

		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_HEIGHT, &height);
		arv_buffer->priv->parts[0].height = height;

		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_XOFFSET, &x_offset);
		arv_buffer->priv->parts[0].x_offset = x_offset;

		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_YOFFSET, &y_offset);
		arv_buffer->priv->parts[0].y_offset = y_offset;

		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_XPADDING, &x_padding);
		arv_buffer->priv->parts[0].x_padding = x_padding;

		_gentl_buffer_info_sizet(gentl, datastream, gentl_buffer, BUFFER_INFO_YPADDING, &y_padding);
		arv_buffer->priv->parts[0].y_padding = y_padding;

		if (_clamp_part_to_buffer (arv_buffer, 0))
			is_larger_than_buffer = TRUE;
	}

	/* A buffer the producer can't describe, neither by its payload type nor by its filled size, for example after
	 * an error of the producer, is not a success. A frame truncated by the producer, or with lost data, neither.
	 * These two informations are optional. Neither is a buffer whose data, by the sizes the producer gives, doesn't
	 * fit in its memory, whether the producer says so or not. */
	if (!has_payload_type && !has_size_filled)
		arv_buffer->priv->status = ARV_BUFFER_STATUS_UNKNOWN;
	else if (is_larger_than_buffer ||
		 (_gentl_buffer_info_bool8(gentl, datastream, gentl_buffer, BUFFER_INFO_DATA_LARGER_THAN_BUFFER,
					   &is_data_larger) && is_data_larger))
		arv_buffer->priv->status = ARV_BUFFER_STATUS_SIZE_MISMATCH;
	else if (_gentl_buffer_info_bool8(gentl, datastream, gentl_buffer, BUFFER_INFO_IS_INCOMPLETE,
					  &is_incomplete) && is_incomplete)
		arv_buffer->priv->status = ARV_BUFFER_STATUS_MISSING_PACKETS;
	else
		arv_buffer->priv->status = ARV_BUFFER_STATUS_SUCCESS;
}

static gboolean
_is_cancelled (ArvGenTLStreamThreadData *thread_data)
{
	return g_atomic_int_get (&thread_data->cancel);
}

/* Waits for a stop request, at most for an event timeout: no busy loop on a persistent error */

static void
_wait_after_error (ArvGenTLStreamThreadData *thread_data)
{
	gint64 end_time = g_get_monotonic_time () + ARV_GENTL_STREAM_EVENT_TIMEOUT_MS * 1000;

	g_mutex_lock (&thread_data->stream_mtx);
	while (!_is_cancelled (thread_data) &&
	       g_cond_wait_until (&thread_data->stream_event, &thread_data->stream_mtx, end_time))
		;
	g_mutex_unlock (&thread_data->stream_mtx);
}

static void
_push_output_buffer (ArvGenTLStreamThreadData *thread_data, ArvBuffer *arv_buffer)
{
	arv_stream_push_output_buffer (thread_data->stream, arv_buffer);
	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_BUFFER_DONE, arv_buffer);
}

/* Queues a buffer pushed by the application in the producer input pool. A buffer pushed after the acquisition start is
 * announced first. Returns FALSE if the producer refuses the buffer. */

static gboolean
_queue_buffer (ArvGenTLStreamThreadData *thread_data, GHashTable *buffers, ArvBuffer *arv_buffer)
{
	ArvGenTLStreamBufferData *buffer_data;
	GError *error = NULL;

	buffer_data = g_object_get_data (G_OBJECT (arv_buffer), "gentl-buffer-data");
	if (buffer_data == NULL) {
		buffer_data = _announce_buffer (thread_data->gentl, thread_data->gentl_data_stream, arv_buffer, &error);
	} else if (buffer_data->gentl_data_stream != thread_data->gentl_data_stream) {
		g_set_error (&error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR, "Buffer of another stream");
		buffer_data = NULL;
	}

	if (buffer_data != NULL) {
		GC_ERROR gc_error;

		gc_error = thread_data->gentl->DSQueueBuffer (thread_data->gentl_data_stream->data_stream,
							      buffer_data->gentl_buffer);
		if (gc_error == GC_ERR_SUCCESS) {
			g_hash_table_replace (buffers, buffer_data->gentl_buffer, arv_buffer);
			return TRUE;
		}

		_set_gentl_error (&error, thread_data->gentl, "DSQueueBuffer", gc_error);
	}

	arv_warning_stream_thread ("[GenTLStream::queue_buffer] Buffer %p not queued: %s",
				   arv_buffer, error != NULL ? error->message : "unknown error");
	g_clear_error (&error);

	return FALSE;
}

/* Queues the buffers pushed by the application: @arv_buffer first if it is not NULL, then those of the input queue. A
 * buffer the producer refuses is given back to the application, as an aborted one, instead of being retried. The
 * refused buffers are only given back once the input queue is empty: an application pushing them back at once, for
 * example from its stream callback or "new-buffer" signal handler, can't keep the thread in this loop. Returns FALSE if
 * a buffer was refused. */

static gboolean
_queue_input_buffers (ArvGenTLStreamThreadData *thread_data, GHashTable *buffers, ArvBuffer *arv_buffer)
{
	GQueue refused_buffers = G_QUEUE_INIT;

	if (arv_buffer == NULL)
		arv_buffer = arv_stream_pop_input_buffer (thread_data->stream);

	while (arv_buffer != NULL) {
		if (!_queue_buffer (thread_data, buffers, arv_buffer))
			g_queue_push_tail (&refused_buffers, arv_buffer);

		/* A stop leaves the next buffers in the input queue */
		arv_buffer = _is_cancelled (thread_data) ? NULL : arv_stream_pop_input_buffer (thread_data->stream);
	}

	if (g_queue_is_empty (&refused_buffers))
		return TRUE;

	while ((arv_buffer = g_queue_pop_head (&refused_buffers)) != NULL) {
		arv_buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
		thread_data->n_failures += 1;
		_push_output_buffer (thread_data, arv_buffer);
	}

	return FALSE;
}

static void
_process_new_buffer (ArvGenTLStreamThreadData *thread_data, GHashTable *buffers, BUFFER_HANDLE gentl_buffer)
{
	ArvBuffer *arv_buffer;

	arv_buffer = g_hash_table_lookup (buffers, gentl_buffer);
	if (arv_buffer == NULL) {
		arv_warning_stream_thread ("[GenTLStream::loop] Unknown buffer %p", gentl_buffer);
		return;
	}

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data,
				       ARV_STREAM_CALLBACK_TYPE_START_BUFFER,
				       NULL);

	g_hash_table_remove (buffers, gentl_buffer);
	_gentl_buffer_to_arv_buffer(thread_data->gentl, thread_data->gentl_data_stream->data_stream, gentl_buffer,
				    arv_buffer, thread_data->timestamp_tick_frequency);

	if (arv_buffer->priv->status == ARV_BUFFER_STATUS_SUCCESS)
		thread_data->n_completed_buffers += 1;
	else
		thread_data->n_failures += 1;

	thread_data->n_transferred_bytes += arv_buffer->priv->allocated_size;

	_push_output_buffer (thread_data, arv_buffer);
}

static void
_loop (ArvGenTLStreamThreadData *thread_data)
{
	ArvGenTLModule *gentl = thread_data->gentl;
	GHashTable *buffers;
	GHashTableIter iter;
	gpointer value;
	GC_ERROR error;

	buffers = g_steal_pointer (&thread_data->buffers);

	g_mutex_lock (&thread_data->thread_started_mutex);
	thread_data->thread_started = TRUE;
	g_cond_signal (&thread_data->thread_started_cond);
	g_mutex_unlock (&thread_data->thread_started_mutex);

	while (!_is_cancelled (thread_data)) {
		EVENT_NEW_BUFFER_DATA NewImageEventData = {NULL, NULL};
		size_t size = sizeof(EVENT_NEW_BUFFER_DATA);
		gboolean is_refused;

		is_refused = !_queue_input_buffers (thread_data, buffers, NULL);

		if (g_hash_table_size (buffers) == 0) {
			ArvBuffer *arv_buffer = NULL;

			/* No frame can come before the application pushes a buffer the producer accepts. No busy loop
			 * either with refused buffers the application pushes back at once. */
			if (!is_refused)
				arv_buffer = arv_stream_timeout_pop_input_buffer
					(thread_data->stream, ARV_GENTL_STREAM_POP_INPUT_BUFFER_TIMEOUT_MS * 1000);
			if (arv_buffer != NULL)
				is_refused = !_queue_input_buffers (thread_data, buffers, arv_buffer);
			if (is_refused && g_hash_table_size (buffers) == 0)
				_wait_after_error (thread_data);
			continue;
		}

		/* With buffers held by the producer, the retries of the refused ones are bounded by the frame rate, or
		 * by the wait timeout */
		error = gentl->EventGetData(thread_data->event_handle, &NewImageEventData, &size,
					    ARV_GENTL_STREAM_EVENT_TIMEOUT_MS);
		if (error == GC_ERR_SUCCESS) {
			_process_new_buffer (thread_data, buffers, NewImageEventData.BufferHandle);
		} else if (error != GC_ERR_TIMEOUT && !_is_cancelled (thread_data)) {
			arv_warning_stream_thread ("[GenTLStream::loop] new buffer error (%s)",
						   arv_gentl_gc_error_to_string(error));
			_wait_after_error (thread_data);
		}
	}

        error = gentl->DSFlushQueue(thread_data->gentl_data_stream->data_stream, ACQ_QUEUE_ALL_DISCARD);
        if (error != GC_ERR_SUCCESS) {
		arv_warning_stream_thread("[GenTLStream::_loop] DSFlushQueue error (%s)",
					  arv_gentl_gc_error_to_string(error));
        }

	/* Buffers flushed from the producer queues */
        g_hash_table_iter_init (&iter, buffers);
        while (g_hash_table_iter_next (&iter, NULL, &value)) {
		ArvBuffer *arv_buffer = value;

		arv_buffer->priv->status = ARV_BUFFER_STATUS_ABORTED;
		thread_data->n_aborted += 1;
		_push_output_buffer (thread_data, arv_buffer);
        }

        g_hash_table_unref (buffers);
}

static void *
arv_gentl_stream_thread (void *data)
{
	ArvGenTLStreamThreadData *thread_data = data;

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_INIT, NULL);

	_loop (thread_data);

	if (thread_data->callback != NULL)
		thread_data->callback (thread_data->callback_data, ARV_STREAM_CALLBACK_TYPE_EXIT, NULL);

	return NULL;
}

/* ArvGenTLStream implementation */

/* Puts back at the end of the input queue buffers taken with arv_stream_pop_input_buffer(), through the output queue,
 * which keeps the count of the buffers owned by the stream thread balanced. The output queue must be empty. */

static void
_give_back_input_buffers (ArvStream *stream, GQueue *buffers)
{
	ArvBuffer *buffer;
	guint n_buffers = g_queue_get_length (buffers);
	guint i;

	while ((buffer = g_queue_pop_head (buffers)) != NULL)
		arv_stream_push_output_buffer (stream, buffer);

	for (i = 0; i < n_buffers; i++) {
		buffer = arv_stream_try_pop_buffer (stream);
		if (buffer == NULL)
			break;
		arv_stream_push_buffer (stream, buffer);
	}
}

static gboolean
arv_gentl_stream_start_acquisition (ArvStream *stream, GError **error)
{
	ArvGenTLStream *gentl_stream = ARV_GENTL_STREAM(stream);
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (gentl_stream);
	ArvGenTLStreamThreadData *thread_data;
	ArvGenTLSystem *gentl_system = arv_gentl_device_get_system(priv->gentl_device);
	ArvGenTLModule *gentl = arv_gentl_system_get_gentl(gentl_system);
	DS_HANDLE data_stream;
	GQueue pending = G_QUEUE_INIT;
	GHashTable *buffers = NULL;
	EVENT_HANDLE event_handle = NULL;
	gboolean is_event_registered = FALSE;
	GError *local_error = NULL;
	ArvBuffer *buffer;
	GList *iter;
	GC_ERROR gc_error;

	g_return_val_if_fail (priv->thread == NULL, FALSE);
	g_return_val_if_fail (priv->thread_data != NULL, FALSE);

	thread_data = priv->thread_data;
	data_stream = priv->gentl_data_stream->data_stream;

	/* The buffers left in the output queue by a previous acquisition are used again */
        do {
                buffer = arv_stream_try_pop_buffer (stream);
                if (buffer != NULL)
                        arv_stream_push_buffer (stream, buffer);
        } while (buffer != NULL);

	do {
		buffer = arv_stream_pop_input_buffer (stream);
		if (buffer != NULL)
			g_queue_push_tail (&pending, buffer);
	} while (buffer != NULL);

        /* Ensure every buffer has a gentl_buffer attached, for those not pushed using arv_stream_create_buffers() */
	for (iter = pending.head; iter != NULL && local_error == NULL; iter = iter->next) {
		if (g_object_get_data (G_OBJECT (iter->data), "gentl-buffer-data") == NULL)
			_announce_buffer (gentl, priv->gentl_data_stream, iter->data, &local_error);
	}

	/* Registered before the acquisition start, until the stream thread is stopped */
	if (local_error == NULL) {
		gc_error = gentl->GCRegisterEvent(data_stream, EVENT_NEW_BUFFER, &event_handle);
		if (gc_error == GC_ERR_SUCCESS)
			is_event_registered = TRUE;
		else
			_set_gentl_error (&local_error, gentl, "GCRegisterEvent", gc_error);
	}

	/* Queued before the acquisition start */
	if (local_error == NULL) {
		buffers = g_hash_table_new (g_direct_hash, g_direct_equal);

		for (iter = pending.head; iter != NULL && local_error == NULL; iter = iter->next) {
			ArvGenTLStreamBufferData *buffer_data;

			buffer_data = g_object_get_data (G_OBJECT (iter->data), "gentl-buffer-data");
			if (buffer_data->gentl_data_stream != priv->gentl_data_stream) {
				g_set_error (&local_error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR,
					     "Buffer of another stream");
				break;
			}

			gc_error = gentl->DSQueueBuffer(data_stream, buffer_data->gentl_buffer);
			if (gc_error == GC_ERR_SUCCESS)
				g_hash_table_replace (buffers, buffer_data->gentl_buffer, iter->data);
			else
				_set_gentl_error (&local_error, gentl, "DSQueueBuffer", gc_error);
		}
	}

	if (local_error == NULL) {
		gc_error = gentl->DSStartAcquisition(data_stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
		if (gc_error != GC_ERR_SUCCESS)
			_set_gentl_error (&local_error, gentl, "DSStartAcquisition", gc_error);
	}

	if (local_error != NULL) {
		arv_warning_stream ("[GenTLStream::start_acquisition] %s", local_error->message);

		if (buffers != NULL) {
			if (g_hash_table_size (buffers) > 0) {
				gc_error = gentl->DSFlushQueue(data_stream, ACQ_QUEUE_ALL_DISCARD);
				if (gc_error != GC_ERR_SUCCESS)
					arv_warning_stream ("[GenTLStream::start_acquisition] DSFlushQueue error (%s)",
							    arv_gentl_gc_error_to_string (gc_error));
			}
			g_hash_table_unref (buffers);
		}

		if (is_event_registered) {
			gc_error = gentl->GCUnregisterEvent(data_stream, EVENT_NEW_BUFFER);
			if (gc_error != GC_ERR_SUCCESS)
				arv_warning_stream ("[GenTLStream::start_acquisition] GCUnregisterEvent error (%s)",
						    arv_gentl_gc_error_to_string (gc_error));
		}

		/* The stream is left as before the start, with its buffers in the input queue */
		_give_back_input_buffers (stream, &pending);

		g_propagate_error (error, local_error);

		return FALSE;
	}

	g_queue_clear (&pending);

	thread_data->gentl = gentl;
	thread_data->gentl_data_stream = priv->gentl_data_stream;
	thread_data->event_handle = event_handle;
	thread_data->timestamp_tick_frequency = priv->timestamp_tick_frequency;
	thread_data->buffers = buffers;
	thread_data->thread_started = FALSE;
	g_atomic_int_set (&thread_data->cancel, FALSE);

	priv->thread = g_thread_new ("arv_gentl_stream", arv_gentl_stream_thread, priv->thread_data);

	g_mutex_lock (&thread_data->thread_started_mutex);
	while (!thread_data->thread_started)
		g_cond_wait (&thread_data->thread_started_cond,
				&thread_data->thread_started_mutex);
	g_mutex_unlock (&thread_data->thread_started_mutex);

	arv_info_stream("[GenTLStream::start_acquisition]");

        return TRUE;
}

static gboolean
arv_gentl_stream_stop_acquisition(ArvStream *stream, GError **error)
{
	ArvGenTLStream *gentl_stream = ARV_GENTL_STREAM(stream);
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (gentl_stream);
	ArvGenTLStreamThreadData *thread_data;
	ArvGenTLSystem *gentl_system = arv_gentl_device_get_system(priv->gentl_device);
	ArvGenTLModule *gentl = arv_gentl_system_get_gentl(gentl_system);
	GC_ERROR gc_error;

	g_return_val_if_fail (priv->thread_data != NULL, FALSE);

	/* Not started, or the start failed */
	if (priv->thread == NULL) {
		arv_info_stream("[GenTLStream::stop_acquisition] Not started");
		return TRUE;
	}

	arv_info_stream("[GenTLStream::stop_acquisition]");

	thread_data = priv->thread_data;

	gc_error = gentl->DSStopAcquisition(priv->gentl_data_stream->data_stream, ACQ_STOP_FLAGS_DEFAULT);
	if (gc_error != GC_ERR_SUCCESS && gc_error != GC_ERR_RESOURCE_IN_USE)
		arv_warning_stream("[GenTLStream::stop_acquisition] DSStopAcquisition error (%s)",
                                   arv_gentl_gc_error_to_string(gc_error));

	/* The thread checks the stop request between two waits. EventKill ends a wait in progress, but the wait timeout
	 * bounds the stop duration if the producer loses or ignores the kill. */
	g_mutex_lock (&thread_data->stream_mtx);
	g_atomic_int_set (&thread_data->cancel, TRUE);
	g_cond_broadcast (&thread_data->stream_event);
	g_mutex_unlock (&thread_data->stream_mtx);

	gc_error = gentl->EventKill(thread_data->event_handle);
	if (gc_error != GC_ERR_SUCCESS)
		arv_debug_stream("[GenTLStream::stop_acquisition] EventKill error (%s)",
				 arv_gentl_gc_error_to_string(gc_error));

	g_thread_join (priv->thread);
	priv->thread = NULL;

	/* Unregistered once the thread doesn't wait for it anymore */
	gc_error = gentl->GCUnregisterEvent(priv->gentl_data_stream->data_stream, EVENT_NEW_BUFFER);
	if (gc_error != GC_ERR_SUCCESS)
		arv_warning_stream("[GenTLStream::stop_acquisition] GCUnregisterEvent error (%s)",
				   arv_gentl_gc_error_to_string(gc_error));
	thread_data->event_handle = NULL;

        return TRUE;
}

static gboolean
arv_gentl_stream_create_buffers (ArvStream *stream, guint n_buffers, size_t size,
                                 void *user_data, GDestroyNotify user_data_destroy_func,
                                 GError **error)
{
	ArvGenTLStream *gentl_stream = ARV_GENTL_STREAM(stream);
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (gentl_stream);
	ArvGenTLSystem *gentl_system = arv_gentl_device_get_system(priv->gentl_device);
	ArvGenTLModule *gentl = arv_gentl_system_get_gentl(gentl_system);
        GC_ERROR gc_error = GC_ERR_SUCCESS;
        guint i;

        for (i = 0; i < n_buffers; i++) {
                ArvBuffer *buffer;
                ArvGenTLStreamBufferData *buffer_data;
                BUFFER_HANDLE gentl_buffer;
                void *data;

		gc_error = gentl->DSAllocAndAnnounceBuffer(priv->gentl_data_stream->data_stream,
                                                           size, NULL, &gentl_buffer);
		if (gc_error != GC_ERR_SUCCESS) {
			_set_gentl_error (error, gentl, "DSAllocAndAnnounceBuffer", gc_error);
			arv_warning_stream("[GenTLStream::create_buffers] DSAllocAndAnnounceBuffer (%s)",
                                           arv_gentl_gc_error_to_string(gc_error));
			break;
		}

                /* FIXME handler error */
                _gentl_buffer_info_ptr(gentl, priv->gentl_data_stream->data_stream,
                                       gentl_buffer, BUFFER_INFO_BASE, &data);

                buffer_data = g_new0 (ArvGenTLStreamBufferData, 1);
                buffer_data->gentl_buffer = gentl_buffer;
                buffer_data->gentl_data_stream = arv_gentl_data_stream_ref (priv->gentl_data_stream);

                buffer = arv_buffer_new_full(size, data, user_data, user_data_destroy_func);
                g_object_set_data_full (G_OBJECT (buffer), "gentl-buffer-data", buffer_data, _buffer_data_destroy_func);
                arv_stream_push_buffer (stream, buffer);
        }

        return gc_error == GC_ERR_SUCCESS;
}

/**
 * arv_gentl_stream_new: (skip)
 * @gentl_device: a #ArvGenTLDevice
 * @callback: (scope call): processing callback
 * @callback_data: (closure): user data for @callback
 *
 * Return value: (transfer full): a new #ArvStream.
 */

ArvStream *
arv_gentl_stream_new (ArvGenTLDevice *gentl_device,
                      ArvStreamCallback callback, void *callback_data, GDestroyNotify destroy,
                      GError **error)
{
	return g_initable_new (ARV_TYPE_GENTL_STREAM, NULL, error,
			"device",  gentl_device,
			"callback", callback,
			"callback-data", callback_data,
			"destroy-notify", destroy,
			NULL);
}

/* ArvStream implementation */

static void
arv_gentl_stream_init (ArvGenTLStream *gentl_stream)
{
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (gentl_stream);

	priv->thread_data = g_new0 (ArvGenTLStreamThreadData, 1);
	g_mutex_init (&priv->thread_data->thread_started_mutex);
	g_cond_init (&priv->thread_data->thread_started_cond);
	g_mutex_init (&priv->thread_data->stream_mtx);
	g_cond_init (&priv->thread_data->stream_event);
	priv->gentl_data_stream = NULL;
	priv->n_buffers = ARV_GENTL_STREAM_DEFAULT_N_BUFFERS;
}

static void
arv_gentl_stream_set_property (GObject *self, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (ARV_GENTL_STREAM (self));

	switch (prop_id)
	{
		case PROP_STREAM_N_BUFFERS:
			priv->n_buffers = g_value_get_uint(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
			break;
	}
}

static void
arv_gentl_stream_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (ARV_GENTL_STREAM (object));

	switch (prop_id)
	{
		case PROP_STREAM_N_BUFFERS:
			g_value_set_uint(value, priv->n_buffers);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
arv_gentl_stream_constructed (GObject *object)
{
	ArvStream *stream = ARV_STREAM (object);
	ArvGenTLStream *gentl_stream = ARV_GENTL_STREAM (stream);
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (gentl_stream);
	GError *error = NULL;

	G_OBJECT_CLASS (arv_gentl_stream_parent_class)->constructed (object);

        g_object_get (object,
                      "device",&priv->gentl_device,
                      "callback", &priv->thread_data->callback,
                      "callback-data", &priv->thread_data->callback_data,
                      NULL);

	priv->gentl_data_stream = arv_gentl_data_stream_new (priv->gentl_device, &error);
	if (priv->gentl_data_stream == NULL) {
		arv_warning_stream ("[GenTLStream::constructed] Failed to open the data stream: %s",
				    error != NULL ? error->message : "unknown error");
		arv_stream_take_init_error (stream,
					    error != NULL ? error :
					    g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_STREAM_ERROR,
							 "Failed to open the data stream"));
		return;
	}

	priv->timestamp_tick_frequency = arv_gentl_device_get_timestamp_tick_frequency(priv->gentl_device);
	priv->thread_data->stream = stream;

	arv_stream_declare_info (stream, "n_completed_buffers",
				 G_TYPE_UINT64, &priv->thread_data->n_completed_buffers);
	arv_stream_declare_info (stream, "n_failures",
				 G_TYPE_UINT64, &priv->thread_data->n_failures);
	arv_stream_declare_info (stream, "n_underruns",
				 G_TYPE_UINT64, &priv->thread_data->n_underruns);
	arv_stream_declare_info (stream, "n_aborted",
				 G_TYPE_UINT64, &priv->thread_data->n_aborted);
	arv_stream_declare_info (stream, "n_transferred_bytes",
				 G_TYPE_UINT64, &priv->thread_data->n_transferred_bytes);
}

static void
arv_gentl_stream_finalize (GObject *object)
{
	ArvGenTLStream *gentl_stream = ARV_GENTL_STREAM (object);
	ArvGenTLStreamPrivate *priv = arv_gentl_stream_get_instance_private (gentl_stream);

        if (priv->thread != NULL)
                arv_gentl_stream_stop_acquisition (ARV_STREAM(gentl_stream), NULL);

	if (priv->thread_data != NULL) {
		ArvGenTLStreamThreadData *thread_data;

		thread_data = priv->thread_data;

		arv_info_stream ("[GenTLStream::finalize] n_completed_buffers    = %" G_GUINT64_FORMAT,
				  thread_data->n_completed_buffers);
		arv_info_stream ("[GenTLStream::finalize] n_failures             = %" G_GUINT64_FORMAT,
				  thread_data->n_failures);
		arv_info_stream ("[GenTLStream::finalize] n_underruns            = %" G_GUINT64_FORMAT,
				  thread_data->n_underruns);
		arv_info_stream ("[GenTLStream::finalize] n_aborted              = %" G_GUINT64_FORMAT,
				  thread_data->n_aborted);
		arv_info_stream ("[GenTLStream::finalize] n_transferred_bytes    = %" G_GUINT64_FORMAT,
				  thread_data->n_transferred_bytes);

		g_mutex_clear (&thread_data->thread_started_mutex);
		g_cond_clear (&thread_data->thread_started_cond);
		g_mutex_clear (&thread_data->stream_mtx);
		g_cond_clear (&thread_data->stream_event);
		g_clear_pointer (&priv->thread_data, g_free);
	}

	g_clear_object (&priv->gentl_device);

	G_OBJECT_CLASS (arv_gentl_stream_parent_class)->finalize (object);

	/* After the buffers of the stream queues, which revoke their GenTL buffer on destruction */
        if (priv->gentl_data_stream != NULL)
                arv_gentl_data_stream_unref (priv->gentl_data_stream);
}

static void
arv_gentl_stream_class_init (ArvGenTLStreamClass *gentl_stream_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (gentl_stream_class);
	ArvStreamClass *stream_class = ARV_STREAM_CLASS (gentl_stream_class);

	object_class->constructed = arv_gentl_stream_constructed;
	object_class->finalize = arv_gentl_stream_finalize;
	object_class->get_property = arv_gentl_stream_get_property;
	object_class->set_property = arv_gentl_stream_set_property;

	stream_class->start_acquisition = arv_gentl_stream_start_acquisition;
	stream_class->stop_acquisition = arv_gentl_stream_stop_acquisition;
        stream_class->create_buffers = arv_gentl_stream_create_buffers;

	g_object_class_install_property
		(object_class,
		 PROP_STREAM_N_BUFFERS,
                 g_param_spec_uint ("n-buffers",
                                    "Number of buffers",
                                    "Number of buffers",
                                    ARV_GENTL_STREAM_MIN_N_BUFFERS,
                                    ARV_GENTL_STREAM_MAX_N_BUFFERS,
                                    ARV_GENTL_STREAM_DEFAULT_N_BUFFERS,
                                    G_PARAM_READWRITE));
}
