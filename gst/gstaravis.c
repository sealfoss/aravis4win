/* Aravis - Digital camera library
 *
 * Copyright © 2006 Eric Jonas <jonas@mit.edu>
 * Copyright © 2006 Antoine Tremblay <hexa00@gmail.com>
 * Copyright © 2010 United States Government, Joshua M. Doe <joshua.doe@us.army.mil>
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
 * SECTION:element-aravissrc
 *
 * Source using the Aravis vision library
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v aravissrc ! video/x-raw,format=UYVY,width=512,height=512,framerate=25/1 ! autovideosink
 * ]|
 * </refsect2>
 */

#include <gstaravis.h>
#include <gstaravisdeviceprovider.h>
#include <arvgvspprivate.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

/* TODO: Add l10n */
#define _(x) (x)

#define GST_ARAVIS_DEFAULT_N_BUFFERS		50
#define GST_ARAVIS_BUFFER_TIMEOUT_DEFAULT	2000000
/* Frames are copied rather than lent downstream when the stream would keep fewer buffers */
#define GST_ARAVIS_MIN_STREAM_BUFFERS		2
/* create () waits for a frame in slices of this many µs, and checks between them whether it must stop: unlock () and
 * a lost camera interrupt the wait within this delay */
#define GST_ARAVIS_WAIT_SLICE_US		50000
/* This many failed buffers in a row end the wait for a frame without timeout, and with a timeout, no longer restart it */
#define GST_ARAVIS_MAX_FAILED_BUFFERS		10
/* A camera that is unplugged or switched off first sends failed frames, then its device notices the loss: a
 * USB3Vision stream checks the connection of its device within 100 ms of a failed transfer, for example. Before
 * blaming the failed frames, the wait without timeout gives the device this many µs to emit control-lost. */
#define GST_ARAVIS_CONTROL_LOST_DELAY_US	250000
#define GST_ARAVIS_CONTROL_LOST_POLL_US		10000
/* The maximum transfer sizes the USB3Vision streams accept, besides 0, which selects the library default: the library
 * refuses the other sizes with a warning, and keeps its previous size. The maximum is 64 MiB. */
#define GST_ARAVIS_MIN_USB_TRANSFER_SIZE	1024
#define GST_ARAVIS_MAX_USB_TRANSFER_SIZE	67108864

GST_DEBUG_CATEGORY_STATIC (aravis_debug);
#define GST_CAT_DEFAULT aravis_debug

/* What the element and the frames it lends downstream share about a stream: the number of buffers the element gave the
 * stream, and the number of them lent downstream, which only create () increases, and which the lent frames decrease
 * when they release their buffer, from any thread, possibly after the stream is gone. The element counts the lent
 * buffers itself rather than relying on the counters of ArvStream, which are not exact during an acquisition: the
 * asynchronous USB3Vision stream updates its count of the buffers being filled from two threads, without a common
 * lock. Set once the stream is created: whether its frames may be lent at all, see gst_aravis_set_caps (), and the
 * size of its buffers. */

struct _GstAravisStreamState {
	gint n_buffers;
	gint n_lent;
	gboolean can_lend;
	gsize buffer_size;
};

enum
{
  /* Before camera-name: the tools which write the properties of an element in this order, like gst-device-monitor-1.0
   * for the launch line of a device, then write interface first, which spares camera-name a search of every enabled
   * interface */
  PROP_INTERFACE = 1,
  PROP_CAMERA_NAME,
  PROP_CAMERA,
  PROP_GAIN,
  PROP_GAIN_AUTO,
  PROP_EXPOSURE,
  PROP_EXPOSURE_AUTO,
  PROP_GAMMA,
  PROP_H_BINNING,
  PROP_V_BINNING,
  PROP_OFFSET_X,
  PROP_OFFSET_Y,
  PROP_PACKET_DELAY,
  PROP_PACKET_SIZE,
  PROP_AUTO_PACKET_SIZE,
  PROP_PACKET_RESEND,
  PROP_FEATURES,
  PROP_NUM_ARV_BUFFERS,
  PROP_USB_MODE,
  PROP_STREAM,
  PROP_TRIGGER,
  PROP_ADD_REFERENCE_TIMESTAMP_META,
  PROP_USB_MAXIMUM_TRANSFER_SIZE,
  PROP_BUFFER_TIMEOUT,
  N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES];

enum
{
  /* actions */
  SIGNAL_SOFTWARE_TRIGGER,
  LAST_SIGNAL
};

static guint gst_aravis_signals[LAST_SIGNAL] = { 0 };

#define GST_TYPE_ARV_AUTO (gst_arv_auto_get_type())
static GType
gst_arv_auto_get_type (void)
{
	static GType arv_auto_type = 0;

	static const GEnumValue arv_autos[] = {
		{ARV_AUTO_OFF, "Off", "off"},
		{ARV_AUTO_ONCE, "Once", "once"},
		{ARV_AUTO_CONTINUOUS, "Continuous", "on"},
		{0, NULL, NULL},
	};

	if (!arv_auto_type)
	{
		arv_auto_type = g_enum_register_static("GstArvAuto", arv_autos);
	}
	return arv_auto_type;
}

#define GST_TYPE_ARV_USB_MODE (gst_arv_usb_mode_get_type())
static GType
gst_arv_usb_mode_get_type (void)
{
	static GType arv_usb_mode_type = 0;

	static const GEnumValue arv_usb_modes[] = {
		{ARV_UV_USB_MODE_SYNC, "Synchronous", "sync"},
		{ARV_UV_USB_MODE_ASYNC, "Asynchronous", "async"},
		{ARV_UV_USB_MODE_DEFAULT, "Default", "default"},
		{0, NULL, NULL},
	};

	if (!arv_usb_mode_type)
	{
		arv_usb_mode_type = g_enum_register_static("GstArvUsbMode", arv_usb_modes);
	}
	return arv_usb_mode_type;
}

G_DEFINE_TYPE (GstAravis, gst_aravis, GST_TYPE_PUSH_SRC);

static GstStaticPadTemplate aravis_src_template = GST_STATIC_PAD_TEMPLATE ("src",
									   GST_PAD_SRC,
									   GST_PAD_ALWAYS,
									   GST_STATIC_CAPS ("ANY"));

static GstCaps *
gst_aravis_get_all_camera_caps (GstAravis *gst_aravis, GError **error)
{
	GError *local_error = NULL;
	GstCaps *caps;
	gint64 *pixel_formats = NULL;
	double min_frame_rate, max_frame_rate;
	int min_height, min_width;
	int max_height, max_width;
	unsigned int n_pixel_formats, i;
	int min_frame_rate_numerator;
	int min_frame_rate_denominator;
	int max_frame_rate_numerator;
	int max_frame_rate_denominator;
	gboolean use_frame_rate;

	g_return_val_if_fail (GST_IS_ARAVIS (gst_aravis), NULL);

	if (!ARV_IS_CAMERA (gst_aravis->camera))
		return NULL;

	GST_LOG_OBJECT (gst_aravis, "Get all camera caps");

	arv_camera_get_width_bounds (gst_aravis->camera, &min_width, &max_width, &local_error);
	if (!local_error) arv_camera_get_height_bounds (gst_aravis->camera, &min_height, &max_height, &local_error);
	if (!local_error) pixel_formats = arv_camera_dup_available_pixel_formats (gst_aravis->camera, &n_pixel_formats,
										  &local_error);
	use_frame_rate = arv_camera_is_frame_rate_available (gst_aravis->camera, NULL) &&
                gst_aravis->trigger_source == NULL;

	if (use_frame_rate) {
		if (!local_error) arv_camera_get_frame_rate_bounds (gst_aravis->camera,
								    &min_frame_rate, &max_frame_rate, &local_error);
		if (!local_error) {
			gst_util_double_to_fraction (min_frame_rate, &min_frame_rate_numerator, &min_frame_rate_denominator);
			gst_util_double_to_fraction (max_frame_rate, &max_frame_rate_numerator, &max_frame_rate_denominator);
		}
	}
	if (local_error) {
		g_propagate_error (error, local_error);
		g_free (pixel_formats);
		return NULL;
	}

	caps = gst_caps_new_empty ();
	for (i = 0; i < n_pixel_formats; i++) {
		const char *caps_string;

		caps_string = arv_pixel_format_to_gst_caps_string (pixel_formats[i]);

		if (caps_string != NULL) {
			GstStructure *structure;

			structure = gst_structure_from_string (caps_string, NULL);
			gst_structure_set (structure,
					   "width", GST_TYPE_INT_RANGE, min_width, max_width,
					   "height", GST_TYPE_INT_RANGE, min_height, max_height,
					   NULL);
			if (use_frame_rate)
				gst_structure_set (structure,
						   "framerate", GST_TYPE_FRACTION_RANGE,
						   min_frame_rate_numerator, min_frame_rate_denominator,
						   max_frame_rate_numerator, max_frame_rate_denominator,
						   NULL);
			gst_caps_append_structure (caps, structure);
		}
	}

	g_free (pixel_formats);

	return caps;
}

static GstCaps *
gst_aravis_get_caps (GstBaseSrc * src, GstCaps * filter)
{
	GstAravis* gst_aravis = GST_ARAVIS(src);
	GstCaps *caps;

	GST_OBJECT_LOCK (gst_aravis);
	if (gst_aravis->all_caps != NULL)
		caps = gst_caps_copy (gst_aravis->all_caps);
	else
		caps = gst_caps_new_any ();
	GST_OBJECT_UNLOCK (gst_aravis);

	GST_LOG_OBJECT (gst_aravis, "Available caps = %" GST_PTR_FORMAT, caps);

	return caps;
}

/* How error messages name the camera: by its label once it is open, see gst_aravis_build_camera_label (), by
 * camera-name otherwise, or as the first camera of the interface the element looks for. Called with the object lock
 * held. */

static char *
gst_aravis_describe_camera_locked (GstAravis *gst_aravis)
{
	if (gst_aravis->camera_label != NULL)
		return g_strdup_printf ("camera \"%s\"", gst_aravis->camera_label);
	if (gst_aravis->camera_name != NULL)
		return g_strdup_printf ("camera \"%s\"", gst_aravis->camera_name);
	if (gst_aravis->interface_id != NULL)
		return g_strdup_printf ("a camera on interface %s", gst_aravis->interface_id);

	return g_strdup ("a camera");
}

/* Stops the acquisition of the stream of the element, if the element started it. Called with the object lock held. */

static void
gst_aravis_stop_stream (GstAravis *gst_aravis)
{
	GError *error = NULL;

	if (!gst_aravis->stream_started)
		return;

	gst_aravis->stream_started = FALSE;

	if (!arv_stream_stop_acquisition (gst_aravis->stream, &error)) {
		GST_WARNING_OBJECT (gst_aravis, "Failed to stop the stream: %s",
				    error != NULL ? error->message : "unknown error");
		g_clear_error (&error);
	}
}

/* Whether the element streams from its camera: from the creation of its first stream until stop (), including while
 * set_caps () replaces the stream. The camera-name and interface setters don't open another camera meanwhile. Called
 * with the object lock held. */

static gboolean
gst_aravis_is_streaming_locked (GstAravis *gst_aravis)
{
	return gst_aravis->stream != NULL || gst_aravis->replacing_stream;
}

/* Releases the stream of the element, already stopped, and notifies the stream property, NULL until the next stream
 * exists, so that an application following the property releases the stream too: the stream of the previous caps is
 * then gone before set_caps () creates the next one, which a GenTL producer may need, as it may refuse to open a data
 * stream while another one of the device is open. Called with the object lock held, which it releases during the
 * notification. */

static void
gst_aravis_release_stream_locked (GstAravis *gst_aravis)
{
	ArvStream *stream = g_steal_pointer (&gst_aravis->stream);

	g_clear_pointer (&gst_aravis->stream_state, g_atomic_rc_box_release);

	if (stream == NULL)
		return;

	gst_aravis->replacing_stream = TRUE;
	GST_OBJECT_UNLOCK (gst_aravis);

	g_object_unref (stream);
	g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_STREAM]);

	GST_OBJECT_LOCK (gst_aravis);
	gst_aravis->replacing_stream = FALSE;
}

static gboolean
gst_aravis_set_caps (GstBaseSrc *src, GstCaps *caps)
{
	GError *error = NULL;
	GstAravis* gst_aravis = GST_ARAVIS(src);
	GstStructure *structure;
	ArvPixelFormat pixel_format;
	gint height, width;
	gint current_height, current_width;
	int depth = 0, bpp = 0;
	const GValue *frame_rate = NULL;
	const char *format_string;
	char *description;
	unsigned int i;
	gint n_buffers;
	gboolean result = FALSE;
	gboolean is_frame_rate_available;
	gboolean is_gain_available;
	gboolean is_gain_auto_available;
	gboolean is_exposure_time_available;
	gboolean is_exposure_auto_available;

	GST_LOG_OBJECT (gst_aravis, "Requested caps = %" GST_PTR_FORMAT, caps);

	structure = gst_caps_get_structure (caps, 0);

	GST_OBJECT_LOCK (gst_aravis);
	arv_camera_get_region (gst_aravis->camera, NULL, NULL, &current_width, &current_height, &error);
	if (error)
		goto errored;

        width = current_width;
        height = current_height;

	is_frame_rate_available = arv_camera_is_frame_rate_available (gst_aravis->camera, NULL);
	is_gain_available = arv_camera_is_gain_available (gst_aravis->camera, NULL);
	is_gain_auto_available = arv_camera_is_gain_auto_available (gst_aravis->camera, NULL);
	is_exposure_time_available = arv_camera_is_exposure_time_available (gst_aravis->camera, NULL);
	is_exposure_auto_available = arv_camera_is_exposure_auto_available (gst_aravis->camera, NULL);

	gst_structure_get_int (structure, "width", &width);
	gst_structure_get_int (structure, "height", &height);
	gst_structure_get_int (structure, "depth", &depth);
	gst_structure_get_int (structure, "bpp", &bpp);
	if (is_frame_rate_available)
		frame_rate = gst_structure_get_value (structure, "framerate");
	format_string = gst_structure_get_string (structure, "format");

	pixel_format = arv_pixel_format_from_gst_caps (gst_structure_get_name (structure), format_string, bpp, depth);

	if (!pixel_format) {
		GST_ERROR_OBJECT (src, "did not find matching pixel_format");
		goto failed;
	}

	/* The element stops and starts the acquisition of its own stream only. arv_camera_stop_acquisition () and
	 * arv_camera_start_acquisition () act on every stream of the device, among them an old stream of the element
	 * that the application still holds through the stream property: restarted, it would compete with the new stream
	 * for the frames, and stopping it when the application releases it would disable the streaming of the camera,
	 * which the new stream uses, on a USB3Vision camera for example. */
	arv_camera_execute_command (gst_aravis->camera, "AcquisitionStop", &error);
	gst_aravis_stop_stream (gst_aravis);

	/* Release the stream before creating the new one, see gst_aravis_release_stream_locked (). The frames lent
	 * downstream don't need the old stream: their buffers only go back to it while it exists. */
	gst_aravis_release_stream_locked (gst_aravis);

	if (!error) arv_camera_set_pixel_format (gst_aravis->camera, pixel_format, &error);
	if (!error) arv_camera_set_binning (gst_aravis->camera, gst_aravis->h_binning, gst_aravis->v_binning, &error);
	if (!error) {
                if (width != current_width || height != current_height)
                        arv_camera_set_region (gst_aravis->camera,
                                               gst_aravis->offset_x, gst_aravis->offset_y,
                                               width, height, &error);
                else
                        arv_camera_set_region (gst_aravis->camera,
                                               gst_aravis->offset_x, gst_aravis->offset_y,
                                               -1, -1, &error);
        }

	if (!error && arv_camera_is_gv_device (gst_aravis->camera)) {
		if (gst_aravis->packet_delay >= 0) {
			gint64 delay = 0;
			arv_camera_gv_set_packet_delay (gst_aravis->camera, gst_aravis->packet_delay, &error);
			if (!error) delay = arv_camera_gv_get_packet_delay (gst_aravis->camera, &error);
			if (!error && delay != gst_aravis->packet_delay)
				GST_WARNING_OBJECT (gst_aravis, "Packet delay is %" G_GINT64_FORMAT " ns instead of %" G_GINT64_FORMAT,
					delay, gst_aravis->packet_delay);
		}
		if (!error && gst_aravis->packet_size > 0)
			arv_camera_gv_set_packet_size (gst_aravis->camera, gst_aravis->packet_size, &error);
		if (!error && gst_aravis->auto_packet_size)
			arv_camera_gv_auto_packet_size (gst_aravis->camera, &error);
	}

	if (!error && frame_rate != NULL) {

		gst_aravis->frame_rate = (double) gst_value_get_fraction_numerator (frame_rate) /
			(double) gst_value_get_fraction_denominator (frame_rate);

		GST_DEBUG_OBJECT (gst_aravis, "Frame rate = %g Hz", gst_aravis->frame_rate);
		arv_camera_set_frame_rate (gst_aravis->camera, gst_aravis->frame_rate, &error);

		if (gst_aravis->frame_rate > 0.0)
			gst_aravis->buffer_timeout_us = MAX (GST_ARAVIS_BUFFER_TIMEOUT_DEFAULT,
                                                 3e6 / gst_aravis->frame_rate);
		else
			gst_aravis->buffer_timeout_us = GST_ARAVIS_BUFFER_TIMEOUT_DEFAULT;
	} else
		gst_aravis->buffer_timeout_us = GST_ARAVIS_BUFFER_TIMEOUT_DEFAULT;

	GST_DEBUG_OBJECT (gst_aravis, "Automatic buffer timeout = %" G_GUINT64_FORMAT " µs", gst_aravis->buffer_timeout_us);

	if (is_frame_rate_available)
		GST_DEBUG_OBJECT (gst_aravis, "Actual frame rate = %g Hz",
				  arv_camera_get_frame_rate (gst_aravis->camera, NULL));

	if (is_gain_auto_available && !error && gst_aravis->gain_auto_set) {
		arv_camera_set_gain_auto (gst_aravis->camera, gst_aravis->gain_auto, &error);
		GST_DEBUG_OBJECT (gst_aravis, "Auto Gain = %s", arv_auto_to_string(gst_aravis->gain_auto));
	}
	if (is_gain_available && gst_aravis->gain_auto == ARV_AUTO_OFF) {
		if (gst_aravis->gain >= 0) {
			GST_DEBUG_OBJECT (gst_aravis, "Gain = %g", gst_aravis->gain);
			if (is_gain_auto_available && !error && !gst_aravis->gain_auto_set)
				arv_camera_set_gain_auto (gst_aravis->camera, ARV_AUTO_OFF, &error);
			if (!error) arv_camera_set_gain (gst_aravis->camera, gst_aravis->gain, &error);
		}
		GST_DEBUG_OBJECT (gst_aravis, "Actual gain = %g", arv_camera_get_gain (gst_aravis->camera, NULL));
	}

	if (is_exposure_auto_available && !error && gst_aravis->exposure_auto_set) {
		arv_camera_set_exposure_time_auto (gst_aravis->camera, gst_aravis->exposure_auto, &error);
		GST_DEBUG_OBJECT (gst_aravis, "Auto Exposure = %s", arv_auto_to_string(gst_aravis->exposure_auto));
	}
	if (is_exposure_time_available && gst_aravis->exposure_auto == ARV_AUTO_OFF) {
		if (gst_aravis->exposure_time_us > 0.0) {
			GST_DEBUG_OBJECT (gst_aravis, "Exposure = %g µs", gst_aravis->exposure_time_us);
			if (is_exposure_auto_available && !error && !gst_aravis->exposure_auto_set)
				arv_camera_set_exposure_time_auto (gst_aravis->camera, ARV_AUTO_OFF, &error);
			if (!error) arv_camera_set_exposure_time (gst_aravis->camera, gst_aravis->exposure_time_us, &error);
		}
		GST_DEBUG_OBJECT (gst_aravis, "Actual exposure = %g µs", arv_camera_get_exposure_time (gst_aravis->camera, NULL));
	}

	if (!error) arv_device_set_features_from_string (arv_camera_get_device (gst_aravis->camera), gst_aravis->features, &error);

	if (!error) gst_aravis->payload = arv_camera_get_payload (gst_aravis->camera, &error);
	if (!error) gst_aravis->stream = arv_camera_create_stream (gst_aravis->camera, NULL, NULL, NULL, &error);
	if (error)
		goto errored;

	if (ARV_IS_GV_STREAM (gst_aravis->stream)) {
		if (gst_aravis->packet_resend)
			g_object_set (gst_aravis->stream, "packet-resend", ARV_GV_STREAM_PACKET_RESEND_ALWAYS, NULL);
		else
			g_object_set (gst_aravis->stream, "packet-resend", ARV_GV_STREAM_PACKET_RESEND_NEVER, NULL);
	}

	n_buffers = gst_aravis->num_arv_buffers;
	for (i = 0; i < (unsigned int) n_buffers; i++)
		arv_stream_push_buffer (gst_aravis->stream,
					arv_buffer_new (gst_aravis->payload, NULL));

	gst_aravis->stream_state = g_atomic_rc_box_new0 (GstAravisStreamState);
	gst_aravis->stream_state->n_buffers = n_buffers;
	gst_aravis->stream_state->buffer_size = gst_aravis->payload;
	/* A GenTL buffer holds its data stream, and through it the device, until it is destroyed. A frame lent downstream
	 * would keep them open after the element has released its stream or its camera, while a GenTL producer may
	 * refuse to open a data stream or a device again before it is closed, as the mock producer of the tests does:
	 * renegotiating while a sink keeps the last frame, as GstBaseSink does by default, or restarting while the
	 * application holds a frame, would fail. So the frames of a GenTL stream are copied. */
	gst_aravis->stream_state->can_lend = !ARV_IS_GENTL_STREAM (gst_aravis->stream);

	GST_LOG_OBJECT (gst_aravis, "Start acquisition");
	gst_aravis->stream_started = arv_stream_start_acquisition (gst_aravis->stream, &error);
	if (gst_aravis->stream_started)
		arv_camera_execute_command (gst_aravis->camera, "AcquisitionStart", &error);

	gst_aravis->timestamp_offset = 0;
	gst_aravis->last_timestamp = 0;

	if (error)
		goto errored;

	GST_OBJECT_UNLOCK (gst_aravis);

	g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_STREAM]);

	result = TRUE;
	goto done;

errored:
	description = gst_aravis_describe_camera_locked (gst_aravis);
	GST_OBJECT_UNLOCK (gst_aravis);
	GST_ELEMENT_ERROR (gst_aravis, RESOURCE, WRITE,
		(_("Could not set caps on %s: %s"), description, error->message),
		(NULL));
	g_free (description);
	g_error_free (error);
	goto done;

failed:
	GST_OBJECT_UNLOCK (gst_aravis);
done:
	return result;
}

static void
gst_aravis_software_trigger (GstAravis *src) {
        if (ARV_IS_CAMERA (src->camera)) {
                arv_camera_software_trigger(src->camera, NULL);
        }
}

/* Opens the camera through a single interface instead of arv_camera_new (), whose arv_open_device () asks every
 * enabled interface in turn, so that for example a GenTL camera does not wait for the GigEVision discovery first. This
 * also reaches the interfaces disabled for device discovery. Without camera name, it opens the first camera of the
 * interface. arv_open_device_on_interface () runs under the lock of the device list of Aravis, as arv_open_device ()
 * and arv_update_device_list () do: the shared ArvInterface instances are never used concurrently, by another aravissrc,
 * the device provider, or the application. */

static ArvCamera *
gst_aravis_open_camera_on_interface (const char *interface_id, const char *camera_name, GError **error)
{
	ArvDevice *device;
	ArvCamera *camera;

	if (arv_get_interface_by_id (interface_id) == NULL) {
		GString *known_ids = g_string_new (NULL);
		unsigned int i;

		for (i = 0; i < arv_get_n_interfaces (); i++)
			g_string_append_printf (known_ids, "%s%s", i > 0 ? ", " : "", arv_get_interface_id (i));

		g_set_error (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_SETTINGS,
			     "Unknown interface '%s' (known interfaces: %s)", interface_id, known_ids->str);
		g_string_free (known_ids, TRUE);

		return NULL;
	}

	device = arv_open_device_on_interface (interface_id, camera_name, error);
	if (device == NULL)
		return NULL;

	camera = arv_camera_new_with_device (device, error);
	g_object_unref (device);

	return camera;
}

/* Device loss. The device of the camera may emit control-lost on any thread: the GigEVision heartbeat thread, a USB
 * event or stream thread, or any thread accessing the device, possibly with the object lock held, and possibly while
 * the element is finalized. So the handler only sets a flag in a reference-counted state it shares with the element,
 * and owns a reference of its own, released when the handler is disconnected and no emission uses it anymore. It
 * never touches the element. create () checks the flag. */

struct _GstAravisDeviceState {
	gint control_lost;
};

static void
gst_aravis_device_state_release (gpointer data, GClosure *closure)
{
	g_atomic_rc_box_release (data);
}

static void
gst_aravis_control_lost_cb (ArvDevice *device, GstAravisDeviceState *device_state)
{
	if (g_atomic_int_compare_and_exchange (&device_state->control_lost, FALSE, TRUE))
		GST_WARNING ("Control of the camera lost");
}

/* The name of the camera in error messages: camera-name, or for the first camera found, vendor-model-serial from the
 * GenICam features of the camera. GigEVision and USB3Vision identify their devices the same way, usually from the
 * same strings, but this is only a label: camera-name may not accept it, as GenTL device identifiers start with the
 * vendor of the transport layer, and the Fake interface has its own. */

static char *
gst_aravis_build_camera_label (ArvCamera *camera, const char *camera_name)
{
	char *vendor;
	char *model;
	char *serial;
	char *label;

	if (camera_name != NULL)
		return g_strdup (camera_name);

	vendor = g_strdup (arv_camera_get_vendor_name (camera, NULL));
	model = g_strdup (arv_camera_get_model_name (camera, NULL));
	serial = g_strdup (arv_camera_get_device_serial_number (camera, NULL));

	label = g_strdup_printf ("%s-%s-%s",
				 vendor != NULL ? vendor : "unknown",
				 model != NULL ? model : "unknown",
				 serial != NULL ? serial : "unknown");

	g_free (vendor);
	g_free (model);
	g_free (serial);

	return label;
}

/* Watches the device of the camera just opened. Called with the object lock held. */

static void
gst_aravis_watch_camera (GstAravis *gst_aravis)
{
	gst_aravis->device_state = g_atomic_rc_box_new0 (GstAravisDeviceState);
	gst_aravis->control_lost_handler =
		g_signal_connect_data (arv_camera_get_device (gst_aravis->camera), "control-lost",
				       G_CALLBACK (gst_aravis_control_lost_cb),
				       g_atomic_rc_box_acquire (gst_aravis->device_state),
				       gst_aravis_device_state_release, 0);
	gst_aravis->camera_label = gst_aravis_build_camera_label (gst_aravis->camera, gst_aravis->camera_name);
}

/* Takes the camera out of the element and stops watching its device. Called with the object lock held. Returns the
 * camera (transfer full), for the caller to release, preferably without the lock. */

static ArvCamera *
gst_aravis_steal_camera (GstAravis *gst_aravis)
{
	ArvCamera *camera = g_steal_pointer (&gst_aravis->camera);

	/* An emission in progress on another thread may still run the handler, with its own device state reference */
	if (camera != NULL && gst_aravis->control_lost_handler != 0)
		g_signal_handler_disconnect (arv_camera_get_device (camera), gst_aravis->control_lost_handler);
	gst_aravis->control_lost_handler = 0;

	g_clear_pointer (&gst_aravis->device_state, g_atomic_rc_box_release);
	g_clear_pointer (&gst_aravis->camera_label, g_free);

	return camera;
}

static gboolean
gst_aravis_init_camera (GstAravis *gst_aravis, gboolean *notify, GError **error)
{
	GError *local_error = NULL;
	ArvCamera *previous_camera;

	previous_camera = gst_aravis_steal_camera (gst_aravis);
	if (previous_camera != NULL) {
		g_object_unref (previous_camera);
		if (notify != NULL)
                        *notify = TRUE;
        } else
                if (notify != NULL)
                        *notify = FALSE;

	if (gst_aravis->interface_id != NULL) {
		GST_DEBUG_OBJECT (gst_aravis, "Open camera '%s' on interface '%s'",
				  gst_aravis->camera_name != NULL ? gst_aravis->camera_name : "(first)",
				  gst_aravis->interface_id);
		gst_aravis->camera = gst_aravis_open_camera_on_interface (gst_aravis->interface_id,
									  gst_aravis->camera_name, &local_error);
	} else
		gst_aravis->camera = arv_camera_new (gst_aravis->camera_name, &local_error);

	if (!local_error) arv_camera_get_region (gst_aravis->camera, &gst_aravis->offset_x,
                                                 &gst_aravis->offset_y, NULL, NULL, &local_error);
	if (!local_error) gst_aravis->payload = 0;
	if (!local_error && arv_camera_is_uv_device (gst_aravis->camera)) {
                arv_camera_uv_set_usb_mode (gst_aravis->camera, gst_aravis->usb_mode);
		/* 0 selects the library default, which a newly opened device already uses */
		if (gst_aravis->usb_maximum_transfer_size > 0)
			arv_camera_uv_set_maximum_transfer_size (gst_aravis->camera,
								 gst_aravis->usb_maximum_transfer_size);
	}
	if (!local_error && gst_aravis->trigger_source != NULL)
		arv_camera_set_trigger (gst_aravis->camera, gst_aravis->trigger_source, &local_error);

	if (local_error) {
		g_clear_object (&gst_aravis->camera);
		g_propagate_error (error, local_error);
		return FALSE;
	}

	gst_aravis_watch_camera (gst_aravis);

        if (notify != NULL)
                *notify = TRUE;

	return TRUE;
}

static void
gst_aravis_init_error (GstAravis *gst_aravis, GError *error)
{
	char *description;
	gboolean has_interface;

	GST_OBJECT_LOCK (gst_aravis);
	description = gst_aravis_describe_camera_locked (gst_aravis);
	has_interface = gst_aravis->interface_id != NULL;
	GST_OBJECT_UNLOCK (gst_aravis);

	if (error->domain == ARV_DEVICE_ERROR && error->code == ARV_DEVICE_ERROR_NOT_FOUND) {
		/* A GenTL camera, for example, is only found through its interface by default */
		if (has_interface)
			GST_ELEMENT_ERROR (gst_aravis, RESOURCE, NOT_FOUND,
				(_("Could not find %s: %s"), description, error->message),
				(NULL));
		else
			GST_ELEMENT_ERROR (gst_aravis, RESOURCE, NOT_FOUND,
				(_("Could not find %s: %s"), description, error->message),
				("Only the interfaces enabled for device discovery were searched: the interface property"
				 " selects another one, like GenTL or Fake, which are disabled by default"));
	} else if (g_error_matches (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_SETTINGS)) {
		GST_ELEMENT_ERROR (gst_aravis, RESOURCE, SETTINGS,
			("%s", error->message),
			(NULL));
	} else {
		GST_ELEMENT_ERROR (gst_aravis, RESOURCE, READ,
			(_("Could not read %s: %s"), description, error->message),
			(NULL));
	}

	g_free (description);
	g_error_free (error);
}

/* Reports a failure of the camera-name or interface setter to open the camera. Before the element starts, only as a
 * warning: start () opens the camera again and posts the error if it still fails, while the failure may be moot by
 * then, for example when camera-name, set before interface, looked for the camera on the other interfaces. An error
 * message would reach the bus of the pipeline of the element, which an application would stop. Once the element has
 * started, as an error message, since nothing would open the camera again. */

static void
gst_aravis_setter_error (GstAravis *gst_aravis, GError *error)
{
	gboolean is_started;
	char *camera_name;

	GST_OBJECT_LOCK (gst_aravis);
	is_started = (GST_OBJECT_FLAGS (gst_aravis) & (GST_BASE_SRC_FLAG_STARTING | GST_BASE_SRC_FLAG_STARTED)) != 0;
	camera_name = g_strdup (gst_aravis->camera_name);
	GST_OBJECT_UNLOCK (gst_aravis);

	if (is_started)
		gst_aravis_init_error (gst_aravis, error);
	else {
		GST_WARNING_OBJECT (gst_aravis, "Could not open camera '%s', trying again at start: %s",
				    camera_name != NULL ? camera_name : "(first)", error->message);
		g_error_free (error);
	}

	g_free (camera_name);
}

/* Selects the camera the element opens when it starts, as the interface and camera-name properties do, but without
 * opening it now, as the camera-name setter does: so that creating the element of a device of the device provider is
 * cheap and takes no camera, and that a failure to open the camera reaches the bus of the pipeline. A camera already
 * open is released. Returns FALSE, without changing anything, once the element has started. */

gboolean
gst_aravis_select_camera (GstAravis *gst_aravis, const char *interface_id, const char *camera_name)
{
	ArvCamera *camera = NULL;
	gboolean is_started;

	g_return_val_if_fail (GST_IS_ARAVIS (gst_aravis), FALSE);

	if (interface_id != NULL && interface_id[0] == '\0')
		interface_id = NULL;

	GST_OBJECT_LOCK (gst_aravis);
	is_started = (GST_OBJECT_FLAGS (gst_aravis) & (GST_BASE_SRC_FLAG_STARTING | GST_BASE_SRC_FLAG_STARTED)) != 0;
	if (!is_started) {
		g_free (gst_aravis->interface_id);
		gst_aravis->interface_id = g_strdup (interface_id);
		g_free (gst_aravis->camera_name);
		gst_aravis->camera_name = g_strdup (camera_name);
		camera = gst_aravis_steal_camera (gst_aravis);
	}
	GST_OBJECT_UNLOCK (gst_aravis);

	if (is_started) {
		GST_WARNING_OBJECT (gst_aravis, "Can't select another camera once started");
		return FALSE;
	}

	GST_DEBUG_OBJECT (gst_aravis, "Camera '%s' on interface '%s' selected, opened at start",
			  camera_name != NULL ? camera_name : "(first)", interface_id != NULL ? interface_id : "(any)");

	g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_INTERFACE]);
	g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_CAMERA_NAME]);
	if (camera != NULL) {
		g_object_unref (camera);
		g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_CAMERA]);
	}

	return TRUE;
}

static gboolean
gst_aravis_start (GstBaseSrc *src)
{
	GError *error = NULL;
	gboolean result = TRUE;
	gboolean notify = FALSE;
	GstAravis* gst_aravis = GST_ARAVIS(src);

        if (gst_aravis->camera_name != NULL)
                GST_LOG_OBJECT (gst_aravis, "Open camera '%s'", gst_aravis->camera_name);
        else
                GST_LOG_OBJECT (gst_aravis, "Open first available camera");

	GST_OBJECT_LOCK (gst_aravis);
	if (gst_aravis->camera == NULL)
		result = gst_aravis_init_camera (gst_aravis, &notify, &error);

	if (result) gst_aravis->all_caps = gst_aravis_get_all_camera_caps (gst_aravis, &error);
	GST_OBJECT_UNLOCK (gst_aravis);

	if (notify)
                g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_CAMERA]);

	if (error)
                gst_aravis_init_error (gst_aravis, error);

	return result;
}


static gboolean
gst_aravis_stop( GstBaseSrc * src )
{
	GError *error = NULL;
	GstAravis* gst_aravis = GST_ARAVIS(src);
	ArvCamera *camera;
	ArvStream *stream;
	GstAravisStreamState *stream_state;
	GstCaps *all_caps;
	gboolean notify_stream;
	gboolean notify_camera;

	GST_OBJECT_LOCK (gst_aravis);
	/* As in gst_aravis_set_caps (): only the stream of the element */
	if (gst_aravis->camera != NULL)
		arv_camera_execute_command (gst_aravis->camera, "AcquisitionStop", &error);
	gst_aravis_stop_stream (gst_aravis);
	camera = gst_aravis_steal_camera (gst_aravis);
	stream = g_steal_pointer (&gst_aravis->stream);
	stream_state = g_steal_pointer (&gst_aravis->stream_state);
	all_caps = g_steal_pointer (&gst_aravis->all_caps);
	GST_OBJECT_UNLOCK (gst_aravis);

	notify_stream = stream != NULL;
	notify_camera = camera != NULL;

	if (stream != NULL)
		g_object_unref (stream);
	if (stream_state != NULL)
		g_atomic_rc_box_release (stream_state);
	if (camera != NULL)
		g_object_unref (camera);
	if (all_caps != NULL)
		gst_caps_unref (all_caps);

	/* The stream and the camera keep the device of the camera open: an application following these properties
	 * releases them too, so that the camera can open again, at the next start for example, through an interface
	 * that refuses to open a device that is open, as GenTL producers may do */
	if (notify_stream)
		g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_STREAM]);
	if (notify_camera)
		g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_CAMERA]);

	GST_DEBUG_OBJECT (gst_aravis, "Stop acquisition");
	if (error) {
		// GigEVision write_register timeout is common if
		// stopping due to lost camera
		GST_ERROR_OBJECT (src, "Acquisition stop error: %s", error->message);
		g_error_free (error);
	}

	return TRUE;
}

/* Until unlock_stop (), create () checks within GST_ARAVIS_WAIT_SLICE_US whether GstBaseSrc flushes, then returns
 * GST_FLOW_FLUSHING, or pauses: see gst_aravis_wait_for_frame () */

static gboolean
gst_aravis_unlock (GstBaseSrc *src)
{
	GstAravis *gst_aravis = GST_ARAVIS (src);

	GST_DEBUG_OBJECT (gst_aravis, "Unlock");
	g_atomic_int_set (&gst_aravis->flushing, TRUE);

	return TRUE;
}

static gboolean
gst_aravis_unlock_stop (GstBaseSrc *src)
{
	GstAravis *gst_aravis = GST_ARAVIS (src);

	GST_DEBUG_OBJECT (gst_aravis, "Unlock stop");
	g_atomic_int_set (&gst_aravis->flushing, FALSE);

	return TRUE;
}

static void
gst_aravis_get_times (GstBaseSrc * basesrc, GstBuffer * buffer,
		      GstClockTime * start, GstClockTime * end)
{
	if (gst_base_src_is_live (basesrc)) {
		GstClockTime timestamp = GST_BUFFER_PTS (buffer);

		if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
			GstClockTime duration = GST_BUFFER_DURATION (buffer);

			if (GST_CLOCK_TIME_IS_VALID (duration)) {
				*end = timestamp + duration;
			}
			*start = timestamp;
		}
	} else {
		*start = -1;
		*end = -1;
	}
}

/* Reference caps for the optional capture-time meta. The device timestamp is
 * nanoseconds on the PTP timescale when the device is PTP-synchronized (IEEE
 * 1588), matching the timestamp/x-ptp convention of rtpjitterbuffer. */
static GstStaticCaps ptp_reference_timestamp_caps = GST_STATIC_CAPS ("timestamp/x-ptp");

/* Frames go downstream without a copy: the GstBuffer wraps the data of the ArvBuffer and owns it, and the ArvBuffer
 * goes back to its stream when the GstBuffer memory is freed. A frame is copied instead when lending its ArvBuffer
 * would leave the stream with fewer than GST_ARAVIS_MIN_STREAM_BUFFERS buffers, for example while a queue downstream
 * waits to have enough buffers, so that the camera never starves, and always for a GenTL stream, see
 * gst_aravis_set_caps (). */

typedef struct {
	ArvBuffer *arv_buffer;
	GWeakRef stream;
	GstAravisStreamState *stream_state;
} GstAravisBufferContext;

/* Destroy notify of the wrapped memory. It runs on whichever thread drops the last reference to the memory, possibly
 * after the element was stopped, or renegotiated with a new stream, and it only uses the context: the ArvBuffer goes
 * back to the stream it came from if that stream still exists, never to a later one, whose frames may have another
 * size, and is freed otherwise. If the element has dropped its own reference to the stream meanwhile, the stream is
 * finalized here. */

static void
gst_aravis_buffer_context_free (gpointer data)
{
	GstAravisBufferContext *context = data;
	ArvStream *stream;

	stream = g_weak_ref_get (&context->stream);
	if (stream != NULL) {
		arv_stream_push_buffer (stream, context->arv_buffer);
		g_object_unref (stream);
	} else
		g_object_unref (context->arv_buffer);

	/* Once back in the stream */
	g_atomic_int_add (&context->stream_state->n_lent, -1);
	g_atomic_rc_box_release (context->stream_state);

	g_weak_ref_clear (&context->stream);
	g_free (context);
}

/* Whether the frames of the stream may be lent, and the stream keeps enough buffers, waiting, being filled or filled,
 * if the buffer just popped is lent downstream: all those the stream was given, but the one just popped, and those
 * already lent */

static gboolean
gst_aravis_can_lend_buffer (GstAravisStreamState *stream_state)
{
	return stream_state->can_lend &&
		stream_state->n_buffers - 1 - g_atomic_int_get (&stream_state->n_lent) >= GST_ARAVIS_MIN_STREAM_BUFFERS;
}

static const char *
gst_aravis_buffer_status_to_string (ArvBufferStatus status)
{
	switch (status) {
		case ARV_BUFFER_STATUS_SUCCESS:
			return "success";
		case ARV_BUFFER_STATUS_CLEARED:
			return "cleared";
		case ARV_BUFFER_STATUS_TIMEOUT:
			return "timeout";
		case ARV_BUFFER_STATUS_MISSING_PACKETS:
			return "missing packets";
		case ARV_BUFFER_STATUS_WRONG_PACKET_ID:
			return "wrong packet id";
		case ARV_BUFFER_STATUS_SIZE_MISMATCH:
			return "size mismatch";
		case ARV_BUFFER_STATUS_FILLING:
			return "filling";
		case ARV_BUFFER_STATUS_ABORTED:
			return "aborted";
		case ARV_BUFFER_STATUS_PAYLOAD_NOT_SUPPORTED:
			return "payload not supported";
		default:
			return "unknown";
	}
}

/* The stream statistics, and the number of buffers lent downstream, for the details of error messages */

static char *
gst_aravis_dup_stream_statistics (ArvStream *stream, GstAravisStreamState *stream_state)
{
	GString *statistics;
	gint n_input_buffers = 0;
	gint n_output_buffers = 0;
	gint n_buffer_filling = 0;
	guint i;

	statistics = g_string_new ("Stream statistics:");

	for (i = 0; i < arv_stream_get_n_infos (stream); i++) {
		GType type = arv_stream_get_info_type (stream, i);

		if (type == G_TYPE_UINT64)
			g_string_append_printf (statistics, " %s = %" G_GUINT64_FORMAT ",",
						arv_stream_get_info_name (stream, i),
						arv_stream_get_info_uint64 (stream, i));
		else if (type == G_TYPE_DOUBLE)
			g_string_append_printf (statistics, " %s = %g,",
						arv_stream_get_info_name (stream, i),
						arv_stream_get_info_double (stream, i));
	}

	arv_stream_get_n_owned_buffers (stream, &n_input_buffers, &n_output_buffers, &n_buffer_filling);
	g_string_append_printf (statistics, " n_input_buffers = %d, n_output_buffers = %d, n_buffer_filling = %d,"
				" n_lent_buffers = %d (of %d)",
				n_input_buffers, n_output_buffers, n_buffer_filling,
				g_atomic_int_get (&stream_state->n_lent), stream_state->n_buffers);

	return g_string_free (statistics, FALSE);
}

static char *
gst_aravis_dup_camera_label (GstAravis *gst_aravis)
{
	char *label;

	GST_OBJECT_LOCK (gst_aravis);
	label = g_strdup (gst_aravis->camera_label != NULL ? gst_aravis->camera_label : "");
	GST_OBJECT_UNLOCK (gst_aravis);

	return label;
}

typedef enum {
	GST_ARAVIS_WAIT_CONTROL_LOST,
	GST_ARAVIS_WAIT_TIMEOUT,
	GST_ARAVIS_WAIT_FAILED_BUFFERS
} GstAravisWaitError;

/* Waits up to delay_us for the device of the camera to emit control-lost, unless unlock () comes meanwhile. Returns
 * whether it did. */

static gboolean
gst_aravis_wait_for_control_lost (GstAravis *gst_aravis, GstAravisDeviceState *device_state, gint64 delay_us)
{
	gint64 end_time = g_get_monotonic_time () + delay_us;

	for (;;) {
		gint64 now;

		if (g_atomic_int_get (&device_state->control_lost))
			return TRUE;

		now = g_get_monotonic_time ();
		if (now >= end_time || g_atomic_int_get (&gst_aravis->flushing))
			return FALSE;

		g_usleep (MIN (GST_ARAVIS_CONTROL_LOST_POLL_US, end_time - now));
	}
}

/* Waits for a complete frame, without the object lock, in slices of at most GST_ARAVIS_WAIT_SLICE_US. Returns:
 *  - GST_FLOW_OK, with the frame in arv_buffer;
 *  - GST_FLOW_FLUSHING after unlock (), when the element flushes or stops;
 *  - GST_FLOW_ERROR, after posting an error message naming the camera, when its device emitted control-lost, when no
 *    frame arrived within timeout_us, or after too many failed buffers, see below.
 *
 * A failed buffer, an incomplete one for example, goes back to the stream at once. It shows that the camera still
 * sends frames, so it restarts the wait: an occasional incomplete frame, on a busy network for example, must not end
 * a stream whose frames come less than timeout_us apart, like that of a camera triggered every 1.5 s with the
 * automatic timeout of 2 s. Not once GST_ARAVIS_MAX_FAILED_BUFFERS failed buffers came since the last complete frame,
 * which is no passing loss: a stream of failed buffers ends as a stream of no buffer, timeout_us after the last complete
 * frame, with an error saying why. Without timeout (0), meant for a camera waiting for triggers, the absence of trigger
 * can't explain failed buffers either: that many in a row end the wait. Not at once: a camera that is unplugged or
 * switched off fails its last frames before its device notices the loss, so the error says the camera was lost if
 * the device emits control-lost within GST_ARAVIS_CONTROL_LOST_DELAY_US. */

static GstFlowReturn
gst_aravis_wait_for_frame (GstAravis *gst_aravis, ArvStream *stream, GstAravisStreamState *stream_state,
			   GstAravisDeviceState *device_state, guint64 timeout_us, ArvBuffer **arv_buffer)
{
	GstAravisWaitError wait_error;
	ArvBufferStatus last_status = ARV_BUFFER_STATUS_UNKNOWN;
	guint n_failed_buffers = 0;
	gint64 start_time;
	gint64 last_frame_time;
	char *label;
	char *statistics;

	/* The wait starts right after the previous complete frame, and restarts at each failed frame */
	start_time = g_get_monotonic_time ();
	last_frame_time = start_time;

	for (;;) {
		ArvBuffer *popped;
		guint64 slice_us = GST_ARAVIS_WAIT_SLICE_US;

		/* unlock () comes when the element flushes or stops, and from GStreamer 1.24 on, also when a live
		 * source pauses, with unlock_stop () just before the create () call after it resumes. Returning
		 * GST_FLOW_FLUSHING after a pause would stop the streaming thread for good if the pipeline has resumed
		 * meanwhile: wait for playing instead, as gst_base_src_wait_playing () documents, and return
		 * GST_FLOW_FLUSHING only when GstBaseSrc flushes. Until unlock_stop (), check at each slice: a flush
		 * may follow a resume. */
		if (g_atomic_int_get (&gst_aravis->flushing)) {
			GstFlowReturn playing;
			gint64 pause_time = g_get_monotonic_time ();
			gint64 paused_time;

			if (!gst_base_src_is_live (GST_BASE_SRC (gst_aravis)))
				return GST_FLOW_FLUSHING;

			playing = gst_base_src_wait_playing (GST_BASE_SRC (gst_aravis));
			if (playing != GST_FLOW_OK)
				return playing;

			/* The time spent paused does not count */
			paused_time = g_get_monotonic_time () - pause_time;
			start_time += paused_time;
			last_frame_time += paused_time;
		}

		if (g_atomic_int_get (&device_state->control_lost)) {
			wait_error = GST_ARAVIS_WAIT_CONTROL_LOST;
			break;
		}

		if (timeout_us > 0) {
			gint64 now = g_get_monotonic_time ();
			gint64 deadline;

			if (n_failed_buffers < GST_ARAVIS_MAX_FAILED_BUFFERS)
				deadline = last_frame_time + (gint64) timeout_us;
			else
				deadline = start_time + (gint64) timeout_us;

			if (now >= deadline) {
				wait_error = GST_ARAVIS_WAIT_TIMEOUT;
				break;
			}
			slice_us = MIN (slice_us, (guint64) (deadline - now));
		}

		popped = arv_stream_timeout_pop_buffer (stream, slice_us);
		if (popped == NULL)
			continue;

		/* Even if unlock () came during the pop: GstBaseSrc drops a frame produced while it pauses or flushes */
		if (arv_buffer_get_status (popped) == ARV_BUFFER_STATUS_SUCCESS) {
			*arv_buffer = popped;
			return GST_FLOW_OK;
		}

		last_status = arv_buffer_get_status (popped);
		last_frame_time = g_get_monotonic_time ();
		n_failed_buffers++;
		arv_stream_push_buffer (stream, popped);

		GST_LOG_OBJECT (gst_aravis, "Failed buffer (%s)", gst_aravis_buffer_status_to_string (last_status));

		if (timeout_us == 0 && n_failed_buffers >= GST_ARAVIS_MAX_FAILED_BUFFERS) {
			wait_error = GST_ARAVIS_WAIT_FAILED_BUFFERS;
			break;
		}
	}

	if (wait_error == GST_ARAVIS_WAIT_FAILED_BUFFERS &&
	    gst_aravis_wait_for_control_lost (gst_aravis, device_state, GST_ARAVIS_CONTROL_LOST_DELAY_US))
		wait_error = GST_ARAVIS_WAIT_CONTROL_LOST;

	label = gst_aravis_dup_camera_label (gst_aravis);
	statistics = gst_aravis_dup_stream_statistics (stream, stream_state);

	switch (wait_error) {
		case GST_ARAVIS_WAIT_CONTROL_LOST:
			GST_ELEMENT_ERROR (gst_aravis, RESOURCE, READ,
				(_("Lost control of camera \"%s\""), label),
				("The device emitted control-lost: the camera was disconnected or switched off, or another"
				 " application took control of it. %s", statistics));
			break;
		case GST_ARAVIS_WAIT_TIMEOUT:
			if (n_failed_buffers == 0)
				GST_ELEMENT_ERROR (gst_aravis, RESOURCE, READ,
					(_("No frame received from camera \"%s\" within %u ms"),
					 label, (guint) (timeout_us / 1000)),
					("%s", statistics));
			else
				GST_ELEMENT_ERROR (gst_aravis, RESOURCE, READ,
					(_("No complete frame received from camera \"%s\" within %u ms: %u failed frames"
					   " (last status: %s)"),
					 label, (guint) (timeout_us / 1000), n_failed_buffers,
					 gst_aravis_buffer_status_to_string (last_status)),
					("%s", statistics));
			break;
		case GST_ARAVIS_WAIT_FAILED_BUFFERS:
		default:
			GST_ELEMENT_ERROR (gst_aravis, RESOURCE, READ,
				(_("Camera \"%s\" sent %u failed frames in a row (last status: %s)"),
				 label, n_failed_buffers, gst_aravis_buffer_status_to_string (last_status)),
				("%s", statistics));
			break;
	}

	g_free (label);
	g_free (statistics);

	return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_aravis_create (GstPushSrc * push_src, GstBuffer ** buffer)
{
	GstAravis *gst_aravis;
	ArvStream *stream;
	GstAravisStreamState *stream_state;
	GstAravisDeviceState *device_state;
	GstFlowReturn flow_return;
	guint64 timeout_us;
	int arv_row_stride;
	int width, height;
	char *buffer_data;
	size_t buffer_size;
	guint64 timestamp_ns;
	gboolean base_src_does_timestamp;
	gboolean add_reference_timestamp_meta;
	ArvBuffer *arv_buffer = NULL;

	gst_aravis = GST_ARAVIS (push_src);
	base_src_does_timestamp = gst_base_src_get_do_timestamp(GST_BASE_SRC(push_src));

	/* The wait does not hold the object lock, so that state changes, property accesses and queries never wait for a
	 * frame: take what it needs first */
	GST_OBJECT_LOCK (gst_aravis);
	stream = gst_aravis->stream != NULL ? g_object_ref (gst_aravis->stream) : NULL;
	stream_state = gst_aravis->stream_state != NULL ? g_atomic_rc_box_acquire (gst_aravis->stream_state) : NULL;
	device_state = gst_aravis->device_state != NULL ? g_atomic_rc_box_acquire (gst_aravis->device_state) : NULL;
	if (gst_aravis->buffer_timeout_ms < 0)
		timeout_us = gst_aravis->buffer_timeout_us;
	else
		timeout_us = (guint64) gst_aravis->buffer_timeout_ms * 1000;
	GST_OBJECT_UNLOCK (gst_aravis);

	if (stream == NULL || stream_state == NULL || device_state == NULL) {
		g_clear_object (&stream);
		g_clear_pointer (&stream_state, g_atomic_rc_box_release);
		g_clear_pointer (&device_state, g_atomic_rc_box_release);
		return GST_FLOW_NOT_NEGOTIATED;
	}

	flow_return = gst_aravis_wait_for_frame (gst_aravis, stream, stream_state, device_state, timeout_us,
						 &arv_buffer);
	g_atomic_rc_box_release (device_state);
	if (flow_return != GST_FLOW_OK) {
		g_atomic_rc_box_release (stream_state);
		g_object_unref (stream);
		return flow_return;
	}

	buffer_data = (char *) arv_buffer_get_data (arv_buffer, &buffer_size);
	/* Lent or copied, never more than the buffer holds */
	buffer_size = MIN (buffer_size, stream_state->buffer_size);
	arv_buffer_get_image_region (arv_buffer, NULL, NULL, &width, &height);
	arv_row_stride = width * ARV_PIXEL_FORMAT_BIT_PER_PIXEL (arv_buffer_get_image_pixel_format (arv_buffer)) / 8;
	timestamp_ns = arv_buffer_get_timestamp (arv_buffer);

	/* Gstreamer requires row stride to be a multiple of 4 */
	if ((arv_row_stride & 0x3) != 0) {
		int gst_row_stride;
		size_t size;
		char *data;
		int i;

		gst_row_stride = (arv_row_stride & ~(0x3)) + 4;

		size = height * gst_row_stride;
		data = g_malloc (size);

		for (i = 0; i < height; i++)
			memcpy (data + i * gst_row_stride, buffer_data + i * arv_row_stride, arv_row_stride);

		*buffer = gst_buffer_new_wrapped (data, size);
	} else if (gst_aravis_can_lend_buffer (stream_state)) {
		GstAravisBufferContext *context;

		g_atomic_int_inc (&stream_state->n_lent);

		context = g_new (GstAravisBufferContext, 1);
		context->arv_buffer = g_steal_pointer (&arv_buffer);
		g_weak_ref_init (&context->stream, stream);
		context->stream_state = g_atomic_rc_box_acquire (stream_state);

		*buffer = gst_buffer_new_wrapped_full (0, buffer_data, buffer_size, 0, buffer_size,
						       context, gst_aravis_buffer_context_free);
	} else {
		char *data;

		if (stream_state->can_lend)
			GST_LOG_OBJECT (gst_aravis, "Copy the frame: the other stream buffers are held downstream");

		data = g_malloc (buffer_size);
		memcpy (data, buffer_data, buffer_size);

		*buffer = gst_buffer_new_wrapped (data, buffer_size);
	}

	GST_OBJECT_LOCK (gst_aravis);
	if (!base_src_does_timestamp) {
		if (gst_aravis->timestamp_offset == 0) {
			gst_aravis->timestamp_offset = timestamp_ns;
			gst_aravis->last_timestamp = timestamp_ns;
		}

		GST_BUFFER_PTS (*buffer) = timestamp_ns - gst_aravis->timestamp_offset;
		GST_BUFFER_DURATION (*buffer) = timestamp_ns - gst_aravis->last_timestamp;

		gst_aravis->last_timestamp = timestamp_ns;
	}
	add_reference_timestamp_meta = gst_aravis->add_reference_timestamp_meta;
	GST_OBJECT_UNLOCK (gst_aravis);

	/* Optionally expose the absolute device timestamp, which is otherwise lost
	 * when it is folded into the first-frame-relative GST_BUFFER_PTS. A zero
	 * timestamp means the device did not provide one. */
	if (add_reference_timestamp_meta && timestamp_ns != 0) {
		GstCaps *reference = gst_static_caps_get (&ptp_reference_timestamp_caps);

		gst_buffer_add_reference_timestamp_meta (*buffer, reference,
				timestamp_ns, GST_CLOCK_TIME_NONE);
		gst_caps_unref (reference);
	}

	/* Unless the GstBuffer took it */
	if (arv_buffer != NULL)
		arv_stream_push_buffer (stream, arv_buffer);
	g_atomic_rc_box_release (stream_state);
	g_object_unref (stream);

	return GST_FLOW_OK;
}

static GstCaps *
gst_aravis_fixate_caps (GstBaseSrc * bsrc, GstCaps * caps)
{
	GError *error = NULL;
	GstAravis *gst_aravis = GST_ARAVIS (bsrc);
	GstStructure *structure;
	gint width;
	gint height;
	double frame_rate = 0.0;
	gboolean use_frame_rate;
	char *description = NULL;

	g_return_val_if_fail (GST_IS_ARAVIS (bsrc), NULL);

	GST_OBJECT_LOCK (gst_aravis);

	arv_camera_get_region (gst_aravis->camera, NULL, NULL, &width, &height, &error);
	use_frame_rate = arv_camera_is_frame_rate_available (gst_aravis->camera, NULL) &&
                gst_aravis->trigger_source == NULL;

	if (use_frame_rate)
		if (!error) frame_rate = arv_camera_get_frame_rate (gst_aravis->camera, &error);

	if (error)
		description = gst_aravis_describe_camera_locked (gst_aravis);

	GST_OBJECT_UNLOCK (gst_aravis);
	if (error) {
		GST_ELEMENT_ERROR (gst_aravis, RESOURCE, READ,
			(_("Could not read %s: %s"), description, error->message),
			(NULL));
		g_free (description);
		g_error_free (error);
	} else {
		structure = gst_caps_get_structure (caps, 0);

		gst_structure_fixate_field_nearest_int (structure, "width", width);
		gst_structure_fixate_field_nearest_int (structure, "height", height);
		if (use_frame_rate)
			gst_structure_fixate_field_nearest_fraction (structure, "framerate",
								     (double) (0.5 + frame_rate), 1);

		GST_LOG_OBJECT (gst_aravis, "Fixate caps");
	}

	return GST_BASE_SRC_CLASS(gst_aravis_parent_class)->fixate(bsrc, caps);
}

static void
gst_aravis_init (GstAravis *gst_aravis)
{
	gst_base_src_set_live (GST_BASE_SRC (gst_aravis), TRUE);
	gst_base_src_set_format (GST_BASE_SRC (gst_aravis), GST_FORMAT_TIME);

	gst_aravis->camera_name = NULL;
	gst_aravis->interface_id = NULL;

	gst_aravis->gain = -1;
	gst_aravis->gain_auto = ARV_AUTO_OFF;
	gst_aravis->gain_auto_set = FALSE;
	gst_aravis->exposure_time_us = -1;
	gst_aravis->exposure_auto = ARV_AUTO_OFF;
	gst_aravis->exposure_auto_set = FALSE;
	gst_aravis->gamma = 1;
	gst_aravis->offset_x = 0;
	gst_aravis->offset_y = 0;
	gst_aravis->h_binning = -1;
	gst_aravis->v_binning = -1;
	gst_aravis->packet_delay = -1;
	gst_aravis->packet_size = -1;
	gst_aravis->auto_packet_size = FALSE;
        gst_aravis->packet_resend = TRUE;
	gst_aravis->num_arv_buffers = GST_ARAVIS_DEFAULT_N_BUFFERS;
	gst_aravis->payload = 0;
	gst_aravis->usb_mode = ARV_UV_USB_MODE_DEFAULT;
	gst_aravis->usb_maximum_transfer_size = 0;

	gst_aravis->buffer_timeout_ms = -1;
	gst_aravis->buffer_timeout_us = GST_ARAVIS_BUFFER_TIMEOUT_DEFAULT;
	gst_aravis->frame_rate = 0.0;

	gst_aravis->flushing = FALSE;

	gst_aravis->trigger_source = NULL;
	gst_aravis->add_reference_timestamp_meta = FALSE;

	gst_aravis->camera = NULL;
	gst_aravis->stream = NULL;
	gst_aravis->stream_started = FALSE;
	gst_aravis->replacing_stream = FALSE;
	gst_aravis->stream_state = NULL;

	gst_aravis->device_state = NULL;
	gst_aravis->control_lost_handler = 0;
	gst_aravis->camera_label = NULL;

	gst_aravis->all_caps = NULL;
}

static void
gst_aravis_finalize (GObject * object)
{
	GstAravis *gst_aravis = GST_ARAVIS (object);
	ArvCamera *camera;
	ArvStream *stream;
	GstAravisStreamState *stream_state;
	GstCaps *all_caps;

	GST_OBJECT_LOCK (gst_aravis);
	camera = gst_aravis_steal_camera (gst_aravis);
	stream = g_steal_pointer (&gst_aravis->stream);
	stream_state = g_steal_pointer (&gst_aravis->stream_state);
	all_caps = g_steal_pointer (&gst_aravis->all_caps);
	g_clear_pointer (&gst_aravis->camera_name, g_free);
	g_clear_pointer (&gst_aravis->interface_id, g_free);
	g_clear_pointer (&gst_aravis->features, g_free);
        g_clear_pointer (&gst_aravis->trigger_source, g_free);
	GST_OBJECT_UNLOCK (gst_aravis);

	if (camera != NULL)
		g_object_unref (camera);
	if (stream != NULL)
		g_object_unref (stream);
	if (stream_state != NULL)
		g_atomic_rc_box_release (stream_state);
	if (all_caps != NULL)
		gst_caps_unref (all_caps);

	G_OBJECT_CLASS (gst_aravis_parent_class)->finalize (object);
}

static void
gst_aravis_set_property (GObject * object, guint prop_id,
			 const GValue * value, GParamSpec * pspec)
{
	GError *error = NULL;
	GstAravis *gst_aravis = GST_ARAVIS (object);

	GST_DEBUG_OBJECT (gst_aravis, "setting property %s", pspec->name);

	switch (prop_id) {
		gboolean notify;
		case PROP_CAMERA_NAME:
                        notify= FALSE;
			GST_OBJECT_LOCK (gst_aravis);
			/* check if we are currently active
			   prevent setting camera and other values to something not representing the active camera */
			if (!gst_aravis_is_streaming_locked (gst_aravis)) {
				g_free (gst_aravis->camera_name);
				gst_aravis->camera_name = g_strdup (g_value_get_string (value));
				gst_aravis_init_camera (gst_aravis, &notify, &error);
			}

			GST_LOG_OBJECT (gst_aravis, "Set camera name to %s", gst_aravis->camera_name);
			GST_OBJECT_UNLOCK (gst_aravis);
			if (error)
                                gst_aravis_setter_error (gst_aravis, error);
			if (notify)
				g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_CAMERA]);
			break;
		case PROP_GAIN:
			GST_OBJECT_LOCK (gst_aravis);
			gst_aravis->gain = g_value_get_double (value);
			if (gst_aravis->camera != NULL && arv_camera_is_gain_available (gst_aravis->camera, NULL))
				arv_camera_set_gain (gst_aravis->camera, gst_aravis->gain, NULL);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_GAIN_AUTO:
			GST_OBJECT_LOCK (gst_aravis);
			gst_aravis->gain_auto = g_value_get_enum (value);
			gst_aravis->gain_auto_set = TRUE;
			if (gst_aravis->camera != NULL && arv_camera_is_gain_auto_available (gst_aravis->camera, NULL))
				arv_camera_set_gain_auto (gst_aravis->camera, gst_aravis->gain_auto, NULL);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_EXPOSURE:
			GST_OBJECT_LOCK (gst_aravis);
			gst_aravis->exposure_time_us = g_value_get_double (value);
			if (gst_aravis->camera != NULL &&
			    arv_camera_is_exposure_time_available (gst_aravis->camera, NULL))
				arv_camera_set_exposure_time (gst_aravis->camera, gst_aravis->exposure_time_us, NULL);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_EXPOSURE_AUTO:
			GST_OBJECT_LOCK (gst_aravis);
			gst_aravis->exposure_auto = g_value_get_enum (value);
			gst_aravis->exposure_auto_set = TRUE;
			if (gst_aravis->camera != NULL &&
			    arv_camera_is_exposure_auto_available (gst_aravis->camera, NULL))
				arv_camera_set_exposure_time_auto (gst_aravis->camera, gst_aravis->exposure_auto, NULL);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_GAMMA:
			GST_OBJECT_LOCK (gst_aravis);
			gst_aravis->gamma = g_value_get_double (value);
			if (gst_aravis->camera != NULL &&
                            arv_camera_is_feature_available (gst_aravis->camera, "Gamma", NULL)) {
                                double gamma_max, gamma_min;

                                arv_camera_get_float_bounds(gst_aravis->camera, "Gamma", &gamma_min, &gamma_max, NULL);
                                if (gamma_min <= gst_aravis->gamma && gst_aravis->gamma <= gamma_max) {
                                        arv_camera_set_float (gst_aravis->camera, "Gamma",
                                                              gst_aravis->gamma, NULL);
                                }
                        }
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_OFFSET_X:
                        gst_aravis->offset_x = g_value_get_int (value);
                        break;
                case PROP_OFFSET_Y:
                        gst_aravis->offset_y = g_value_get_int (value);
                        break;
                case PROP_H_BINNING:
                        gst_aravis->h_binning = g_value_get_int (value);
                        break;
                case PROP_V_BINNING:
                        gst_aravis->v_binning = g_value_get_int (value);
                        break;
                case PROP_PACKET_DELAY:
                        GST_OBJECT_LOCK (gst_aravis);
                        gst_aravis->packet_delay = g_value_get_int64 (value);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_PACKET_SIZE:
                        gst_aravis->packet_size = g_value_get_int (value);
                        break;
                case PROP_AUTO_PACKET_SIZE:
                        gst_aravis->auto_packet_size = g_value_get_boolean (value);
                        break;
                case PROP_PACKET_RESEND:
                        gst_aravis->packet_resend = g_value_get_boolean (value);
                        break;
                case PROP_FEATURES:
                        GST_OBJECT_LOCK (gst_aravis);
                        g_free (gst_aravis->features);
                        gst_aravis->features = g_value_dup_string (value);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_NUM_ARV_BUFFERS:
                        gst_aravis->num_arv_buffers = g_value_get_int (value);
                        break;
                case PROP_USB_MODE:
                        GST_OBJECT_LOCK (gst_aravis);
                        gst_aravis->usb_mode = g_value_get_enum (value);
                        /* The camera-name setter opens the camera: apply the mode to it too, so that the result
                         * does not depend on the property order. It is used when the stream is created. */
                        if (gst_aravis->camera != NULL && arv_camera_is_uv_device (gst_aravis->camera))
                                arv_camera_uv_set_usb_mode (gst_aravis->camera, gst_aravis->usb_mode);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_TRIGGER:
                        GST_OBJECT_LOCK (gst_aravis);
                        g_free (gst_aravis->trigger_source);
                        gst_aravis->trigger_source = g_strdup (g_value_get_string (value));
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_ADD_REFERENCE_TIMESTAMP_META:
                        GST_OBJECT_LOCK (gst_aravis);
                        gst_aravis->add_reference_timestamp_meta = g_value_get_boolean (value);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_USB_MAXIMUM_TRANSFER_SIZE:
                        /* The range of the property can't exclude the sizes between 0 and the minimum: refused as
                         * GObject refuses the sizes above the maximum, with a warning. The USB3Vision streams would
                         * refuse them too, and keep their previous size. */
                        if (g_value_get_uint (value) > 0 &&
                            g_value_get_uint (value) < GST_ARAVIS_MIN_USB_TRANSFER_SIZE) {
                                g_warning ("USB maximum transfer size %u refused: it must be 0, for the library"
                                           " default, or %u to %u bytes", g_value_get_uint (value),
                                           GST_ARAVIS_MIN_USB_TRANSFER_SIZE, GST_ARAVIS_MAX_USB_TRANSFER_SIZE);
                                break;
                        }
                        GST_OBJECT_LOCK (gst_aravis);
                        /* As for usb-mode. Going back to 0 restores the library default. */
                        if (gst_aravis->usb_maximum_transfer_size != g_value_get_uint (value) &&
                            gst_aravis->camera != NULL && arv_camera_is_uv_device (gst_aravis->camera))
                                arv_camera_uv_set_maximum_transfer_size (gst_aravis->camera, g_value_get_uint (value));
                        gst_aravis->usb_maximum_transfer_size = g_value_get_uint (value);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                case PROP_INTERFACE:
                        notify = FALSE;
                        GST_OBJECT_LOCK (gst_aravis);
                        if (!gst_aravis_is_streaming_locked (gst_aravis)) {
                                const char *interface_id = g_value_get_string (value);

                                if (interface_id != NULL && interface_id[0] == '\0')
                                        interface_id = NULL;

                                if (g_strcmp0 (interface_id, gst_aravis->interface_id) != 0) {
                                        g_free (gst_aravis->interface_id);
                                        gst_aravis->interface_id = g_strdup (interface_id);
                                        /* Like the camera-name setter, (re)open the camera, now through the
                                         * selected interface, so that the property order does not matter. */
                                        if (gst_aravis->camera != NULL || gst_aravis->camera_name != NULL)
                                                gst_aravis_init_camera (gst_aravis, &notify, &error);
                                }
                        } else
                                GST_WARNING_OBJECT (gst_aravis, "Can't change the interface while streaming");

                        GST_LOG_OBJECT (gst_aravis, "Set interface to %s", gst_aravis->interface_id);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        if (error)
                                gst_aravis_setter_error (gst_aravis, error);
                        if (notify)
                                g_object_notify_by_pspec (G_OBJECT (gst_aravis), properties[PROP_CAMERA]);
                        break;
                case PROP_BUFFER_TIMEOUT:
                        /* Applies from the next frame on */
                        GST_OBJECT_LOCK (gst_aravis);
                        gst_aravis->buffer_timeout_ms = g_value_get_int (value);
                        GST_OBJECT_UNLOCK (gst_aravis);
                        break;
                default:
                        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                        break;
        }
}

static void
gst_aravis_get_property (GObject * object, guint prop_id, GValue * value,
			 GParamSpec * pspec)
{
	GstAravis *gst_aravis = GST_ARAVIS (object);

	GST_DEBUG_OBJECT (gst_aravis, "getting property %s", pspec->name);

	switch (prop_id) {
		case PROP_CAMERA_NAME:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_string (value, gst_aravis->camera_name);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_CAMERA:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_object (value, gst_aravis->camera);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_GAIN:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_double (value, gst_aravis->gain);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_GAIN_AUTO:
			GST_OBJECT_LOCK (gst_aravis);
			if (!gst_aravis->gain_auto_set && gst_aravis->camera != NULL &&
			    arv_camera_is_gain_auto_available (gst_aravis->camera, NULL)) {
				gst_aravis->gain_auto = arv_camera_get_gain_auto(gst_aravis->camera, NULL);
			}
			g_value_set_enum (value, gst_aravis->gain_auto);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_EXPOSURE:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_double (value, gst_aravis->exposure_time_us);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_EXPOSURE_AUTO:
			GST_OBJECT_LOCK (gst_aravis);
			if (!gst_aravis->exposure_auto_set && gst_aravis->camera != NULL &&
			    arv_camera_is_exposure_auto_available (gst_aravis->camera, NULL)) {
				gst_aravis->exposure_auto = arv_camera_get_exposure_time_auto(gst_aravis->camera, NULL);
			}
			g_value_set_enum (value, gst_aravis->exposure_auto);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_GAMMA:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_double (value, gst_aravis->gamma);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_OFFSET_X:
			g_value_set_int (value, gst_aravis->offset_x);
			break;
		case PROP_OFFSET_Y:
			g_value_set_int (value, gst_aravis->offset_y);
			break;
		case PROP_H_BINNING:
			GST_OBJECT_LOCK (gst_aravis);
			if (gst_aravis->h_binning < 0 && gst_aravis->camera) {
				arv_camera_get_binning (gst_aravis->camera, &gst_aravis->h_binning, NULL, NULL);
			}
			g_value_set_int (value, gst_aravis->h_binning);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_V_BINNING:
			GST_OBJECT_LOCK (gst_aravis);
			if (gst_aravis->v_binning < 0 && gst_aravis->camera) {
				arv_camera_get_binning (gst_aravis->camera, NULL, &gst_aravis->v_binning, NULL);
			}
			g_value_set_int (value, gst_aravis->v_binning);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_PACKET_DELAY:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_int64 (value, gst_aravis->packet_delay);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
        	case PROP_PACKET_SIZE:
                        g_value_set_int (value, gst_aravis->packet_size);
                        break;
        	case PROP_AUTO_PACKET_SIZE:
                        g_value_set_boolean (value, gst_aravis->auto_packet_size);
                        break;
        	case PROP_PACKET_RESEND:
                        g_value_set_boolean (value, gst_aravis->packet_resend);
                        break;
		case PROP_FEATURES:
			g_value_set_string (value, gst_aravis->features);
			break;
		case PROP_NUM_ARV_BUFFERS:
			g_value_set_int (value, gst_aravis->num_arv_buffers);
			break;
		case PROP_USB_MODE:
			g_value_set_enum(value, gst_aravis->usb_mode);
			break;
		case PROP_STREAM:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_object (value, gst_aravis->stream);
			GST_OBJECT_UNLOCK (gst_aravis);
                        break;
		case PROP_TRIGGER:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_string (value, gst_aravis->trigger_source);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_ADD_REFERENCE_TIMESTAMP_META:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_boolean (value, gst_aravis->add_reference_timestamp_meta);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_USB_MAXIMUM_TRANSFER_SIZE:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_uint (value, gst_aravis->usb_maximum_transfer_size);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_INTERFACE:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_string (value, gst_aravis->interface_id);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		case PROP_BUFFER_TIMEOUT:
			GST_OBJECT_LOCK (gst_aravis);
			g_value_set_int (value, gst_aravis->buffer_timeout_ms);
			GST_OBJECT_UNLOCK (gst_aravis);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static gboolean
gst_aravis_query (GstBaseSrc *bsrc, GstQuery *query)
{
	GstAravis *src = GST_ARAVIS (bsrc);
	gboolean res = FALSE;

	switch (GST_QUERY_TYPE (query))
	{
		case GST_QUERY_LATENCY:
		{
			GstClockTime min_latency;
			GstClockTime max_latency;
			gboolean is_streaming;
			gboolean is_triggered;
			gdouble frame_rate;

			/* create () does not hold the lock while it waits for a frame */
			GST_OBJECT_LOCK (src);
			is_streaming = gst_aravis_is_streaming_locked (src);
			is_triggered = src->trigger_source != NULL;
			frame_rate = src->frame_rate;
			GST_OBJECT_UNLOCK (src);

			/* device must be open */
			if (!is_streaming)
			{
				GST_WARNING_OBJECT (src, "Can't give latency since device isn't open !");
				goto done;
			}

			/* we must have a framerate */
			if (frame_rate <= 0.0 || is_triggered)
			{
				GST_WARNING_OBJECT (src, "Can't give latency since framerate isn't fixated !");
				goto done;
			}

			/* min latency is the time to capture one frame/field, i.e. one frame period, as in v4l2src */
			min_latency = gst_util_gdouble_to_guint64 ((gdouble) GST_SECOND / frame_rate);

			/* max latency is set to NONE because cameras may enter trigger mode
			   and not deliver images for an unspecified amount of time */
			max_latency = GST_CLOCK_TIME_NONE;

			GST_DEBUG_OBJECT (bsrc, "report latency min %" GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
					  GST_TIME_ARGS (min_latency), GST_TIME_ARGS (max_latency));

			/* we are always live, the min latency is 1 frame and the max latency is
			 * the complete buffer of frames. */
			gst_query_set_latency (query, TRUE, min_latency, max_latency);

			res = TRUE;
			break;
		}
		default:
		{
			res = GST_BASE_SRC_CLASS (gst_aravis_parent_class)->query (bsrc, query);
			break;
		}
	}

done:

	return res;
}

static void
gst_aravis_class_init (GstAravisClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
	GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

	gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_aravis_finalize);
	gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_aravis_set_property);
	gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_aravis_get_property);

	properties[PROP_CAMERA_NAME] =
		g_param_spec_string ("camera-name",
				     "Camera name",
				     "Name of the camera",
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_CAMERA] =
		g_param_spec_object ("camera",
				     "Camera Object",
				     "Camera instance to retrieve additional information. Notified when the element"
				     " opens or releases its camera (then NULL): an application holding the camera"
				     " should release it at each notification",
				     ARV_TYPE_CAMERA,
				     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_GAIN] =
		g_param_spec_double ("gain",
				     "Gain",
				     "Gain (dB)",
				     -1.0, 500.0, 0.0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_GAIN_AUTO] =
		g_param_spec_enum ("gain-auto",
				   "Auto Gain",
				   "Auto Gain Mode",
				   GST_TYPE_ARV_AUTO, ARV_AUTO_OFF,
				   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_EXPOSURE] =
		g_param_spec_double ("exposure",
				     "Exposure",
				     "Exposure time (µs)",
				     -1, 100000000.0, 500.0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_EXPOSURE_AUTO] =
		g_param_spec_enum ("exposure-auto",
				   "Auto Exposure",
				   "Auto Exposure Mode",
				   GST_TYPE_ARV_AUTO, ARV_AUTO_OFF,
				   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_GAMMA] =
		g_param_spec_double ("gamma",
				     "Gamma",
				     "Gamma",
				     0.0, 3.9, 1.0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_OFFSET_X] =
		g_param_spec_int ("offset-x",
				  "x Offset",
				  "Offset in x direction",
				  0, G_MAXINT, 0,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_OFFSET_Y] =
		g_param_spec_int ("offset-y",
				  "y Offset",
				  "Offset in y direction",
				  0, G_MAXINT, 0,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_H_BINNING] =
		g_param_spec_int ("h-binning",
				  "Horizontal binning",
				  "CCD horizontal binning",
				  1, G_MAXINT, 1,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_V_BINNING] =
		g_param_spec_int ("v-binning",
				  "Vertical binning",
				  "CCD vertical binning",
				  1, G_MAXINT, 1,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_PACKET_DELAY] =
		g_param_spec_int64 ("packet-delay",
				    "Packet delay",
				    "GigEVision streaming inter packet delay (in ns, -1 = default)",
				    0, G_MAXINT64 / 1000000000LL, 0,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_PACKET_SIZE] =
		g_param_spec_int ("packet-size",
				  "Packet size",
				  "GigEVision streaming packet size",
				  ARV_GVSP_PACKET_PROTOCOL_OVERHEAD (FALSE), 65500, 1500,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_AUTO_PACKET_SIZE] =
		g_param_spec_boolean ("auto-packet-size",
				      "Auto Packet Size",
				      "Negotiate GigEVision streaming packet size",
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_PACKET_RESEND] =
		g_param_spec_boolean ("packet-resend",
				      "Packet Resend",
				      "Request dropped packets to be reissued by the camera",
				      TRUE,
				      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_FEATURES] =
		g_param_spec_string ("features",
				     "String of feature values",
				     "Additional configuration parameters as a space separated list of feature assignations",
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_NUM_ARV_BUFFERS] =
		g_param_spec_int ("num-arv-buffers",
				  "Number of Buffers allocated",
				  "Number of video buffers to allocate for video frames. Frames go downstream without a"
				  " copy while at least " G_STRINGIFY (GST_ARAVIS_MIN_STREAM_BUFFERS) " buffers stay with"
				  " the stream, so with " G_STRINGIFY (GST_ARAVIS_MIN_STREAM_BUFFERS) " or fewer every"
				  " frame is copied",
				  1, G_MAXINT, GST_ARAVIS_DEFAULT_N_BUFFERS,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_USB_MODE] =
		g_param_spec_enum ("usb-mode",
				   "USB mode",
				   "USB mode (synchronous/asynchronous)",
				   GST_TYPE_ARV_USB_MODE, ARV_UV_USB_MODE_DEFAULT,
				   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_STREAM] =
		g_param_spec_object ("stream",
				     "Stream Object",
				     "Stream instance to retrieve additional information. Notified when the element"
				     " creates or releases its stream (then NULL), as at each caps change: an"
				     " application holding the stream should release it at each notification",
				     ARV_TYPE_STREAM,
				     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_TRIGGER] =
                g_param_spec_string("trigger",
                                    "Configure the trigger mode",
                                    "Enable the trigger mode using the given source",
                                    NULL,
                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_ADD_REFERENCE_TIMESTAMP_META] =
                g_param_spec_boolean("add-reference-timestamp-meta",
                                     "Add reference timestamp meta",
                                     "Add a GstReferenceTimestampMeta with timestamp/x-ptp reference"
                                     " caps to buffers, carrying the device timestamp. Only an"
                                     " absolute time if the device clock is PTP-synchronized",
                                     FALSE,
                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_USB_MAXIMUM_TRANSFER_SIZE] =
		g_param_spec_uint ("usb-maximum-transfer-size",
				   "USB maximum transfer size",
				   "USB3Vision maximum bulk transfer size, in bytes: 0 for the library default, or "
				   G_STRINGIFY (GST_ARAVIS_MIN_USB_TRANSFER_SIZE) " to "
				   G_STRINGIFY (GST_ARAVIS_MAX_USB_TRANSFER_SIZE) " (64 MiB)",
				   0, GST_ARAVIS_MAX_USB_TRANSFER_SIZE, 0,
				   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_INTERFACE] =
		g_param_spec_string ("interface",
				     "Interface",
				     "Look for the camera on this interface only, for example USB3Vision, GigEVision"
				     " or GenTL, even if it is disabled for device discovery (NULL = all enabled"
				     " interfaces)",
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_BUFFER_TIMEOUT] =
		g_param_spec_int ("buffer-timeout",
				  "Buffer timeout",
				  "Longest wait for a frame before an error, in ms. A failed frame restarts it, unless "
				  G_STRINGIFY (GST_ARAVIS_MAX_FAILED_BUFFERS) " failed frames came since the last complete"
				  " one (-1 = the longest of 2 s and 3 frame periods, 0 = wait forever, but stop after "
				  G_STRINGIFY (GST_ARAVIS_MAX_FAILED_BUFFERS) " failed frames in a row)",
				  -1, G_MAXINT, -1,
				  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (gobject_class,
					   G_N_ELEMENTS (properties),
					   properties);

        gst_aravis_signals[SIGNAL_SOFTWARE_TRIGGER] =
                g_signal_new ("software-trigger", G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                              G_STRUCT_OFFSET (GstAravisClass,
                                               software_trigger), NULL, NULL, NULL,
                              G_TYPE_NONE, 0);

        GST_DEBUG_CATEGORY_INIT (aravis_debug, "aravissrc", 0, "Aravis interface");

	gst_element_class_set_details_simple (element_class,
					      "Aravis Video Source",
					      "Source/Video",
					      "Aravis based source",
					      "Emmanuel Pacaud <emmanuel.pacaud@free.fr>");
	gst_element_class_add_pad_template (element_class,
					    gst_static_pad_template_get (&aravis_src_template));

	gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_aravis_get_caps);
	gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_aravis_set_caps);
	gstbasesrc_class->fixate = GST_DEBUG_FUNCPTR (gst_aravis_fixate_caps);
	gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_aravis_start);
	gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_aravis_stop);
	gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_aravis_unlock);
	gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_aravis_unlock_stop);
	gstbasesrc_class->query = GST_DEBUG_FUNCPTR (gst_aravis_query);

	gstbasesrc_class->get_times = GST_DEBUG_FUNCPTR (gst_aravis_get_times);

	gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_aravis_create);

        klass->software_trigger = gst_aravis_software_trigger;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	/* GstDeviceMonitor only uses the device providers of rank 1 or above. Not higher, as each probe costs a discovery
	 * on every enabled interface, see gst/pipelines.md. The rank is cached in the registry: the environment variable
	 * GST_ARAVIS_DEVICE_PROVIDER, read at each probe, is what disables the provider without code. */
	return gst_element_register (plugin, "aravissrc", GST_RANK_NONE, GST_TYPE_ARAVIS) &&
		gst_device_provider_register (plugin, "aravisdeviceprovider", GST_RANK_MARGINAL,
					      GST_TYPE_ARAVIS_DEVICE_PROVIDER);
}

#define PACKAGE "aravis"

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
		   GST_VERSION_MINOR,
		   aravis,
		   "Aravis Video Source",
		   plugin_init,
		   ARAVIS_VERSION,
		   "LGPL",
		   "aravis",
		   "http://blogs.gnome.org/emmanuel")
