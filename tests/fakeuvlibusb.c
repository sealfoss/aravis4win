/* SPDX-License-Identifier:Unlicense */

/* Checks of the fake libusb itself, through the libusb API: the semantics the USB3 Vision tests rely on. */

#include "fakeuvtest.h"
#include <arvuvcpprivate.h>
#include <arvuvspprivate.h>
#include <string.h>

typedef struct {
	gint n_calls;
	enum libusb_transfer_status status;
	int actual_length;
	GThread *thread;
	int nested_result;
	libusb_device_handle *dev_handle;
	libusb_context *ctx;
} TransferResult;

static void LIBUSB_CALL
transfer_cb (struct libusb_transfer *transfer)
{
	TransferResult *result = transfer->user_data;

	result->status = transfer->status;
	result->actual_length = transfer->actual_length;
	result->thread = g_thread_self ();
	result->dev_handle = transfer->dev_handle;
	g_atomic_int_inc (&result->n_calls);
}

static void LIBUSB_CALL
synchronous_in_callback_cb (struct libusb_transfer *transfer)
{
	TransferResult *result = transfer->user_data;
	unsigned char data[16];
	int transferred;

	/* Synchronous I/O from a callback of the same context: real libusb deadlocks or spins */
	result->nested_result = libusb_bulk_transfer (transfer->dev_handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN,
						      data, sizeof (data), &transferred, 10);
	transfer_cb (transfer);
}

static void LIBUSB_CALL
nested_events_cb (struct libusb_transfer *transfer)
{
	TransferResult *result = transfer->user_data;
	struct timeval tv = { 0, 0 };

	/* Event handling from a callback: LIBUSB_ERROR_BUSY on Windows, a deadlock on Linux */
	result->nested_result = libusb_handle_events_timeout (result->ctx, &tv);
	transfer_cb (transfer);
}

static libusb_device *
find_device (libusb_device **devices, ssize_t n_devices, guint16 product_id)
{
	ssize_t i;

	for (i = 0; i < n_devices; i++) {
		struct libusb_device_descriptor descriptor;

		g_assert_cmpint (libusb_get_device_descriptor (devices[i], &descriptor), ==, LIBUSB_SUCCESS);
		if (descriptor.idProduct == product_id)
			return devices[i];
	}

	return NULL;
}

static libusb_device *
get_camera_device (libusb_context *ctx)
{
	libusb_device **devices;
	libusb_device *device;
	ssize_t n_devices;

	n_devices = libusb_get_device_list (ctx, &devices);
	g_assert_cmpint (n_devices, >, 0);
	device = libusb_ref_device (find_device (devices, n_devices, FAKE_USB_CAMERA_PRODUCT_ID));
	libusb_free_device_list (devices, 1);

	return device;
}

/* Opens the fake camera on a new context and claims its control and data interfaces */

static libusb_device_handle *
open_camera (libusb_context **ctx, libusb_device **device)
{
	libusb_device_handle *handle = NULL;

	g_assert_cmpint (libusb_init (ctx), ==, LIBUSB_SUCCESS);

	*device = get_camera_device (*ctx);

	g_assert_cmpint (libusb_open (*device, &handle), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_CONTROL_INTERFACE), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);

	return handle;
}

static void
close_camera (libusb_context *ctx, libusb_device *device, libusb_device_handle *handle)
{
	libusb_release_interface (handle, FAKE_USB_CAMERA_CONTROL_INTERFACE);
	libusb_release_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE);
	libusb_close (handle);
	libusb_unref_device (device);
	libusb_exit (ctx);
}

/* Control channel commands, built and parsed with Aravis's own packet code */

static void
send_read_memory_command (libusb_device_handle *handle, guint64 address, guint32 size, guint16 packet_id)
{
	ArvUvcpPacket *packet;
	size_t packet_size;
	int transferred = 0;

	packet = arv_uvcp_packet_new_read_memory_cmd (address, size, packet_id, &packet_size);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT, (unsigned char *) packet,
					       packet_size, &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, packet_size);
	arv_uvcp_packet_free (packet);
}

static void
write_memory (libusb_device_handle *handle, guint64 address, const void *data, guint32 size, guint16 packet_id)
{
	ArvUvcpPacket *packet;
	unsigned char ack[64];
	size_t packet_size;
	int transferred = 0;

	packet = arv_uvcp_packet_new_write_memory_cmd (address, size, packet_id, &packet_size);
	memcpy (arv_uvcp_packet_get_write_memory_cmd_data (packet), data, size);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT, (unsigned char *) packet,
					       packet_size, &transferred, 100), ==, LIBUSB_SUCCESS);
	arv_uvcp_packet_free (packet);

	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack, sizeof (ack),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (arv_uvcp_packet_get_command ((ArvUvcpPacket *) ack), ==, ARV_UVCP_COMMAND_WRITE_MEMORY_ACK);
	g_assert_cmpint (arv_uvcp_packet_get_status ((ArvUvcpPacket *) ack), ==, ARV_UVCP_STATUS_SUCCESS);
}

static void
write_sirm (libusb_device_handle *handle, guint32 offset, guint32 value, guint16 packet_id)
{
	value = GUINT32_TO_LE (value);
	write_memory (handle, FAKE_USB_CAMERA_SIRM_ADDRESS + offset, &value, sizeof (value), packet_id);
}

static void
submit_in (struct libusb_transfer *transfer, libusb_device_handle *handle, guint8 endpoint, unsigned char *buffer,
	   int length, libusb_transfer_cb_fn callback, TransferResult *result, unsigned int timeout)
{
	memset (result, 0, sizeof (TransferResult));
	libusb_fill_bulk_transfer (transfer, handle, endpoint, buffer, length, callback, result, timeout);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
}

static void
handle_events_until (libusb_context *ctx, TransferResult *result, gint n_calls)
{
	struct timeval tv = { 0, 100000 };
	gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS) *
		G_TIME_SPAN_MILLISECOND;

	while (g_atomic_int_get (&result->n_calls) < n_calls && g_get_monotonic_time () < end_time)
		g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);

	g_assert_cmpint (g_atomic_int_get (&result->n_calls), ==, n_calls);
}

typedef struct {
	guint n_sync;
	guint n_async;
	guint8 endpoints[16];
	int lengths[16];
} HookLog;

static void
transfer_hook (FakeUsbDevice *device, guint8 endpoint, int length, gboolean is_synchronous, gpointer user_data)
{
	HookLog *log = user_data;
	guint n = log->n_sync + log->n_async;

	if (n < G_N_ELEMENTS (log->endpoints)) {
		log->endpoints[n] = endpoint;
		log->lengths[n] = length;
	}
	if (is_synchronous)
		log->n_sync++;
	else
		log->n_async++;
}

static void
descriptors_test (void)
{
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device **devices;
	libusb_device *device;
	libusb_device_handle *handle;
	struct libusb_device_descriptor descriptor;
	struct libusb_config_descriptor *config;
	const struct libusb_interface_descriptor *control;
	unsigned char data[256];
	HookLog log = {0};
	ssize_t n_devices;

	fakeuv_test_reset ();
	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);

	fakeuv_add_other_device ();
	camera = fakeuv_add_camera ("UV1");

	g_assert_cmpint (libusb_init (&ctx), ==, LIBUSB_SUCCESS);
	n_devices = libusb_get_device_list (ctx, &devices);
	g_assert_cmpint (n_devices, ==, 2);
	device = find_device (devices, n_devices, FAKE_USB_CAMERA_PRODUCT_ID);
	g_assert_nonnull (device);
	g_assert_nonnull (find_device (devices, n_devices, FAKEUV_OTHER_DEVICE_PRODUCT_ID));

	g_assert_cmpint (libusb_get_device_descriptor (device, &descriptor), ==, LIBUSB_SUCCESS);
	g_assert_cmphex (descriptor.bDeviceClass, ==, 0xef);
	g_assert_cmphex (descriptor.bDeviceSubClass, ==, 0x02);
	g_assert_cmphex (descriptor.bDeviceProtocol, ==, 0x01);
	g_assert_cmphex (descriptor.bcdUSB, ==, 0x0300);

	g_assert_cmpint (libusb_get_config_descriptor (device, 0, &config), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (config->bNumInterfaces, ==, 3);
	control = &config->interface[FAKE_USB_CAMERA_CONTROL_INTERFACE].altsetting[0];
	g_assert_cmphex (control->bInterfaceClass, ==, 0xef);
	g_assert_cmphex (control->bInterfaceSubClass, ==, 0x05);
	g_assert_cmphex (control->bInterfaceProtocol, ==, 0x00);
	g_assert_cmpint (control->bNumEndpoints, ==, 2);
	/* The device info block, up to the serial number index Aravis reads */
	g_assert_cmpint (control->extra_length, ==, 18);
	g_assert_cmpint (control->extra[0], ==, 18);
	g_assert_cmpint (control->extra[11], ==, FAKE_USB_CAMERA_STRING_GUID);
	g_assert_cmpint (control->extra[17], ==, FAKE_USB_CAMERA_STRING_SERIAL_NUMBER);
	g_assert_cmpint (config->interface[FAKE_USB_CAMERA_DATA_INTERFACE].altsetting[0].bInterfaceProtocol, ==, 0x02);
	g_assert_cmphex (config->interface[FAKE_USB_CAMERA_DATA_INTERFACE].altsetting[0].endpoint[0].bEndpointAddress,
			 ==, FAKE_USB_CAMERA_DATA_ENDPOINT_IN);
	libusb_free_config_descriptor (config);

	g_assert_cmpint (libusb_get_max_packet_size (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 1024);
	fake_usb_device_set_max_packet_size (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 512);
	g_assert_cmpint (libusb_get_max_packet_size (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 512);
	g_assert_cmpint (libusb_get_max_packet_size (device, 0x8f), ==, LIBUSB_ERROR_NOT_FOUND);

	/* Edge cases the USB fixes need */
	fake_usb_device_set_config_descriptor_error (camera, LIBUSB_ERROR_NOT_FOUND);
	g_assert_cmpint (libusb_get_config_descriptor (device, 0, &config), ==, LIBUSB_ERROR_NOT_FOUND);
	/* libusb reads the config descriptor for the max packet size too */
	g_assert_cmpint (libusb_get_max_packet_size (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, LIBUSB_ERROR_OTHER);
	fake_usb_device_set_config_descriptor_error (camera, 0);

	fake_usb_device_remove_endpoints (camera, FAKE_USB_CAMERA_DATA_INTERFACE);
	g_assert_cmpint (libusb_get_config_descriptor (device, 0, &config), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (config->interface[FAKE_USB_CAMERA_DATA_INTERFACE].altsetting[0].bNumEndpoints, ==, 0);
	g_assert_null (config->interface[FAKE_USB_CAMERA_DATA_INTERFACE].altsetting[0].endpoint);
	libusb_free_config_descriptor (config);

	/* String descriptors are two control requests each, the language table then the string; on Windows, libusb
	 * claims an interface for each of them when the handle has none */
	g_assert_cmpint (libusb_open (device, &handle), ==, LIBUSB_SUCCESS);
	fake_usb_set_transfer_hook (transfer_hook, &log);
	g_assert_cmpint (libusb_get_string_descriptor_ascii (handle, descriptor.iManufacturer, data, sizeof (data)),
			 ==, 6);
	g_assert_cmpstr ((char *) data, ==, "Aravis");
	fake_usb_set_transfer_hook (NULL, NULL);
	g_assert_cmpuint (log.n_sync, ==, 2);
	g_assert_cmpuint (log.n_async, ==, 0);
	g_assert_cmphex (log.endpoints[0], ==, LIBUSB_ENDPOINT_IN);
	g_assert_cmpint (log.lengths[0], ==, 4);
	g_assert_cmpint (log.lengths[1], ==, 255);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_AUTO_CLAIMS), ==, 2);
	g_assert_false (fake_usb_device_is_interface_claimed (camera, FAKE_USB_CAMERA_CONTROL_INTERFACE));

	g_assert_cmpint (libusb_get_string_descriptor_ascii (handle, FAKE_USB_CAMERA_STRING_GUID, data, sizeof (data)),
			 ==, strlen (fake_usb_camera_get_guid (camera)));
	g_assert_cmpint (libusb_get_string_descriptor_ascii (handle, FAKE_USB_CAMERA_STRING_GUID, data, 5), ==, 4);
	g_assert_cmpint (strlen ((char *) data), ==, 4);
	g_assert_cmpint (libusb_get_string_descriptor_ascii (handle, 0, data, sizeof (data)), ==,
			 LIBUSB_ERROR_INVALID_PARAM);
	g_assert_cmpint (libusb_get_string_descriptor_ascii (handle, 42, data, sizeof (data)), ==, LIBUSB_ERROR_PIPE);
	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_GET_STRING_DESCRIPTOR_ASCII), ==, 5);
	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_CONTROL_TRANSFER), ==, 8);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_STRING_REQUESTS), ==, 8);

	/* Control transfer failures reach string reads */
	fake_usb_device_inject_error (camera, FAKE_USB_CALL_CONTROL_TRANSFER, -1, LIBUSB_ERROR_TIMEOUT, 1);
	g_assert_cmpint (libusb_get_string_descriptor_ascii (handle, descriptor.iManufacturer, data, sizeof (data)),
			 ==, LIBUSB_ERROR_TIMEOUT);

	g_assert_cmpint (libusb_control_transfer (handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
						  LIBUSB_DT_DEVICE << 8, 0, data, LIBUSB_DT_DEVICE_SIZE, 100), ==,
			 LIBUSB_DT_DEVICE_SIZE);
	g_assert_cmpint (data[0], ==, LIBUSB_DT_DEVICE_SIZE);
	g_assert_cmpint (data[1], ==, LIBUSB_DT_DEVICE);
	g_assert_cmpint (data[4], ==, 0xef);
	g_assert_cmpint (libusb_control_transfer (handle, (int) LIBUSB_ENDPOINT_IN | (int) LIBUSB_REQUEST_TYPE_VENDOR, 0x01,
						  0, 0, data, 4, 100), ==, LIBUSB_ERROR_PIPE);

	/* No interface left for the auto-claim of a second handle */
	{
		libusb_device_handle *other_handle;
		guint i;

		g_assert_cmpint (libusb_open (device, &other_handle), ==, LIBUSB_SUCCESS);
		for (i = 0; i < 3; i++)
			g_assert_cmpint (libusb_claim_interface (handle, i), ==, LIBUSB_SUCCESS);
		g_assert_cmpint (libusb_control_transfer (other_handle, LIBUSB_ENDPOINT_IN,
							  LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_DEVICE << 8, 0,
							  data, LIBUSB_DT_DEVICE_SIZE, 100), ==, LIBUSB_ERROR_NOT_FOUND);
		libusb_close (other_handle);
		for (i = 0; i < 3; i++)
			g_assert_cmpint (libusb_release_interface (handle, i), ==, LIBUSB_SUCCESS);

		/* Linux and macOS need no claimed interface */
		fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);
		g_assert_cmpint (libusb_control_transfer (handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
							  LIBUSB_DT_DEVICE << 8, 0, data, LIBUSB_DT_DEVICE_SIZE, 100),
				 ==, LIBUSB_DT_DEVICE_SIZE);
		g_assert_false (fake_usb_device_is_interface_claimed (camera, FAKE_USB_CAMERA_CONTROL_INTERFACE));
	}

	libusb_close (handle);

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	{
		char string[LIBUSB_DEVICE_STRING_BYTES_MAX];

		/* Works on a closed device, cached in the device object */
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
							   string, sizeof (string)), ==, 4);
		g_assert_cmpstr (string, ==, "UV1");
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
							   string, sizeof (string)), ==, 4);
		g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_GET_DEVICE_STRING), ==, 2);
		g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_DEVICE_STRING_FETCHES), ==, 1);

		/* The return value is the size needed, as snprintf (); truncation keeps UTF-8 characters whole */
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
							   string, 3), ==, 4);
		g_assert_cmpstr (string, ==, "UV");
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER, NULL, 10),
				 ==, 4);
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER, string, 0),
				 ==, LIBUSB_ERROR_INVALID_PARAM);
	}
#endif

	libusb_free_device_list (devices, 1);

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	{
		char string[8];

		/* "Caméra": 7 bytes and a NUL; 5 bytes of room would cut the 'é' in half */
		fake_usb_device_set_string (camera, FAKE_USB_CAMERA_STRING_PRODUCT, "Cam\xc3\xa9ra");
		device = get_camera_device (ctx);
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_PRODUCT, string, 5), ==, 8);
		g_assert_cmpstr (string, ==, "Cam");
		/* Room for 3 bytes, the 'é' starting after them, then for 5, the 'é' ending with them, and for none */
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_PRODUCT, string, 4), ==, 8);
		g_assert_cmpstr (string, ==, "Cam");
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_PRODUCT, string, 6), ==, 8);
		g_assert_cmpstr (string, ==, "Cam\xc3\xa9");
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_PRODUCT, string, 1), ==, 8);
		g_assert_cmpstr (string, ==, "");
		g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_PRODUCT, string, 8), ==, 8);
		g_assert_cmpstr (string, ==, "Cam\xc3\xa9ra");
		libusb_unref_device (device);
	}
#endif

	libusb_exit (ctx);

	g_assert_cmpuint (fake_usb_get_n_contexts (), ==, 0);
}

static void
async_semantics_test (void)
{
	TransferResult in_result = {0}, out_result = {0};
	struct libusb_transfer *in_transfer, *out_transfer;
	struct timeval tv = { 1, 0 };
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	ArvUvcpPacket *command;
	ArvUvcpPacket *ack;
	unsigned char ack_buffer[1024];
	size_t command_size;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* A read waits for data */
	in_transfer = libusb_alloc_transfer (0);
	libusb_fill_bulk_transfer (in_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack_buffer,
				   sizeof (ack_buffer), transfer_cb, &in_result, 0);
	g_assert_cmpint (libusb_submit_transfer (in_transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN), ==, 1);

	/* Resubmitting a transfer in flight is an application bug */
	g_assert_cmpint (libusb_submit_transfer (in_transfer), ==, LIBUSB_ERROR_BUSY);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_API_MISUSE), ==, 1);

	/* The command completes the write, and its acknowledge completes the read... */
	command = arv_uvcp_packet_new_read_memory_cmd (ARV_ABRM_MANUFACTURER_NAME, 64, 1, &command_size);
	out_transfer = libusb_alloc_transfer (0);
	libusb_fill_bulk_transfer (out_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT,
				   (unsigned char *) command, command_size, transfer_cb, &out_result, 0);
	g_assert_cmpint (libusb_submit_transfer (out_transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN), ==, 0);

	/* ...but callbacks only run inside event handling, on the calling thread */
	g_usleep (10000);
	g_assert_cmpint (g_atomic_int_get (&in_result.n_calls), ==, 0);
	g_assert_cmpint (g_atomic_int_get (&out_result.n_calls), ==, 0);

	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (out_result.n_calls, ==, 1);
	g_assert_cmpint (out_result.status, ==, LIBUSB_TRANSFER_COMPLETED);
	g_assert_cmpint (out_result.actual_length, ==, command_size);
	g_assert_true (out_result.thread == g_thread_self ());
	g_assert_cmpint (in_result.n_calls, ==, 1);
	g_assert_cmpint (in_result.status, ==, LIBUSB_TRANSFER_COMPLETED);
	g_assert_cmpint (in_result.actual_length, ==, arv_uvcp_packet_get_read_memory_ack_size (64));
	g_assert_true (in_result.thread == g_thread_self ());

	/* Parsed with Aravis's own code */
	ack = (ArvUvcpPacket *) ack_buffer;
	g_assert_cmpint (arv_uvcp_packet_get_status (ack), ==, ARV_UVCP_STATUS_SUCCESS);
	g_assert_cmpint (arv_uvcp_packet_get_command (ack), ==, ARV_UVCP_COMMAND_READ_MEMORY_ACK);
	g_assert_cmpint (arv_uvcp_packet_get_packet_id (ack), ==, 1);
	g_assert_cmpstr (arv_uvcp_packet_get_read_memory_ack_data (ack), ==, "Aravis");

	/* Cancellation completes with LIBUSB_TRANSFER_CANCELLED, from the event handler */
	submit_in (in_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack_buffer, sizeof (ack_buffer),
		   transfer_cb, &in_result, 0);
	g_assert_cmpint (libusb_cancel_transfer (in_transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_cancel_transfer (in_transfer), ==, LIBUSB_ERROR_NOT_FOUND);
	g_assert_cmpint (g_atomic_int_get (&in_result.n_calls), ==, 0);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (in_result.n_calls, ==, 1);
	g_assert_cmpint (in_result.status, ==, LIBUSB_TRANSFER_CANCELLED);

	/* Timeouts */
	submit_in (in_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack_buffer, sizeof (ack_buffer),
		   transfer_cb, &in_result, 20);
	handle_events_until (ctx, &in_result, 1);
	g_assert_cmpint (in_result.status, ==, LIBUSB_TRANSFER_TIMED_OUT);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_TIMEOUTS), ==, 1);

	{
		int transferred = -1;

		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack_buffer,
						       sizeof (ack_buffer), &transferred, 10), ==, LIBUSB_ERROR_TIMEOUT);
		g_assert_cmpint (transferred, ==, 0);
	}

	/* Synchronous calls from a callback are refused (real libusb would deadlock), and counted as misuse */
	memset (&out_result, 0, sizeof (out_result));
	libusb_fill_bulk_transfer (out_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT,
				   (unsigned char *) command, command_size, synchronous_in_callback_cb, &out_result, 0);
	g_assert_cmpint (libusb_submit_transfer (out_transfer), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &out_result, 1);
	g_assert_cmpint (out_result.nested_result, ==, LIBUSB_ERROR_BUSY);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_SYNC_IN_EVENT_HANDLER), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_API_MISUSE), ==, 2);

	/* The acknowledge of that last command is still queued */
	{
		int transferred = 0;

		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, ack_buffer,
						       sizeof (ack_buffer), &transferred, 100), ==, LIBUSB_SUCCESS);
	}

	arv_uvcp_packet_free (command);
	libusb_free_transfer (in_transfer);
	libusb_free_transfer (out_transfer);
	close_camera (ctx, device, handle);

	g_assert_cmpuint (fake_usb_get_n_transfers (), ==, 0);

	fakeuv_test_expect_misuse (2);
}

/* Event lock */

typedef struct {
	gint in_callback;
	gint max_in_callback;
	gint n_calls;
	guint sleep_ms;
} ConcurrencyLog;

static void LIBUSB_CALL
slow_cb (struct libusb_transfer *transfer)
{
	ConcurrencyLog *log = transfer->user_data;
	gint n = g_atomic_int_add (&log->in_callback, 1) + 1;
	gint max;

	do {
		max = g_atomic_int_get (&log->max_in_callback);
	} while (n > max && !g_atomic_int_compare_and_exchange (&log->max_in_callback, max, n));

	g_usleep (log->sleep_ms * 1000);

	g_atomic_int_add (&log->in_callback, -1);
	g_atomic_int_inc (&log->n_calls);
}

typedef struct {
	libusb_context *ctx;
	ConcurrencyLog *log;
	gint n_expected;
} EventThreadData;

static gpointer
event_thread (gpointer user_data)
{
	EventThreadData *data = user_data;
	struct timeval tv = { 0, 20000 };
	gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS) *
		G_TIME_SPAN_MILLISECOND;

	while (g_atomic_int_get (&data->log->n_calls) < data->n_expected && g_get_monotonic_time () < end_time)
		libusb_handle_events_timeout (data->ctx, &tv);

	return NULL;
}

static void
event_lock_test (void)
{
	TransferResult in_result, out_results[2];
	ConcurrencyLog log = {0};
	EventThreadData thread_data;
	struct libusb_transfer *in_transfer, *in_transfers[2], *out_transfers[2];
	ArvUvcpPacket *commands[2];
	size_t command_size;
	GThread *threads[2];
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffers[2][1024];
	guint i;

	fakeuv_test_reset ();
	fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* A synchronous call handles the events of its context: a read that completes meanwhile is dispatched inside
	 * it, on the calling thread */
	in_transfer = libusb_alloc_transfer (0);
	submit_in (in_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[0], sizeof (buffers[0]),
		   transfer_cb, &in_result, 0);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 64, 1);
	g_assert_cmpint (g_atomic_int_get (&in_result.n_calls), ==, 1);
	g_assert_true (in_result.thread == g_thread_self ());
	g_assert_cmpint (in_result.status, ==, LIBUSB_TRANSFER_COMPLETED);
	g_assert_cmpint (arv_uvcp_packet_get_packet_id ((ArvUvcpPacket *) buffers[0]), ==, 1);

	/* One thread at a time dispatches: two threads handling events never run callbacks concurrently */
	log.sleep_ms = 50;
	for (i = 0; i < 2; i++) {
		in_transfers[i] = libusb_alloc_transfer (0);
		libusb_fill_bulk_transfer (in_transfers[i], handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[i],
					   sizeof (buffers[i]), slow_cb, &log, 0);
		g_assert_cmpint (libusb_submit_transfer (in_transfers[i]), ==, LIBUSB_SUCCESS);
		commands[i] = arv_uvcp_packet_new_read_memory_cmd (ARV_ABRM_MANUFACTURER_NAME, 4, 2 + i, &command_size);
		out_transfers[i] = libusb_alloc_transfer (0);
		memset (&out_results[i], 0, sizeof (out_results[i]));
		libusb_fill_bulk_transfer (out_transfers[i], handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT,
					   (unsigned char *) commands[i], command_size, transfer_cb, &out_results[i], 0);
	}

	thread_data.ctx = ctx;
	thread_data.log = &log;
	thread_data.n_expected = 2;
	threads[0] = g_thread_new ("events-1", event_thread, &thread_data);
	threads[1] = g_thread_new ("events-2", event_thread, &thread_data);

	/* The first read completes, then the second one while the first callback runs */
	g_assert_cmpint (libusb_submit_transfer (out_transfers[0]), ==, LIBUSB_SUCCESS);
	g_usleep (10000);
	g_assert_cmpint (libusb_submit_transfer (out_transfers[1]), ==, LIBUSB_SUCCESS);

	g_thread_join (threads[0]);
	g_thread_join (threads[1]);

	g_assert_cmpint (g_atomic_int_get (&log.n_calls), ==, 2);
	g_assert_cmpint (g_atomic_int_get (&log.max_in_callback), ==, 1);

	/* Event handling from a callback of the same context is refused, and counted as misuse */
	submit_in (in_transfer, handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[0], sizeof (buffers[0]),
		   nested_events_cb, &in_result, 0);
	in_result.ctx = ctx;
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 4, 4);
	g_assert_cmpint (g_atomic_int_get (&in_result.n_calls), ==, 1);
	g_assert_cmpint (in_result.nested_result, ==, LIBUSB_ERROR_BUSY);
	g_assert_cmpuint (fake_usb_get_total_stat (FAKE_USB_STAT_SYNC_IN_EVENT_HANDLER), ==, 1);

	for (i = 0; i < 2; i++) {
		libusb_free_transfer (in_transfers[i]);
		libusb_free_transfer (out_transfers[i]);
		arv_uvcp_packet_free (commands[i]);
	}
	libusb_free_transfer (in_transfer);
	close_camera (ctx, device, handle);

	fakeuv_test_expect_misuse (1);
}

/* libusb_close () */

typedef struct {
	libusb_device_handle *handle;
	int result;
} BlockedRead;

static gpointer
blocked_read_thread (gpointer user_data)
{
	BlockedRead *data = user_data;
	unsigned char buffer[1024];
	int transferred;

	data->result = libusb_bulk_transfer (data->handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					     &transferred, 0);

	return NULL;
}

/* Event handling with a 10 s timeout (scaled), which a libusb_close () interrupts */

static gpointer
long_handle_events_thread (gpointer user_data)
{
	guint timeout_ms = fakeuv_timeout_ms (10000);
	struct timeval tv = { (long) (timeout_ms / 1000), (long) (timeout_ms % 1000) * 1000 };

	libusb_handle_events_timeout (user_data, &tv);

	return NULL;
}

typedef struct {
	FakeUsbDevice *device;
	guint8 endpoint;
	guint n_pending;
} PendingCondition;

static gboolean
pending_reached (gpointer user_data)
{
	PendingCondition *condition = user_data;

	return fake_usb_device_get_n_pending_transfers (condition->device, condition->endpoint) >= condition->n_pending;
}

static void
close_test (void)
{
	TransferResult result;
	BlockedRead blocked_read;
	PendingCondition condition;
	struct libusb_transfer *transfer;
	struct timeval tv = { 0, 100000 };
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffer[1024];
	GThread *thread;
	guint64 n_handle_events;
	gint64 start;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* A transfer in flight on a handle that is closed is forgotten, with no callback (do_close () of core.c) */
	transfer = libusb_alloc_transfer (0);
	submit_in (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer), transfer_cb, &result, 0);
	libusb_close (handle);
	g_assert_null (transfer->dev_handle);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 0);
	g_assert_false (fake_usb_device_is_interface_claimed (camera, FAKE_USB_CAMERA_DATA_INTERFACE));
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (g_atomic_int_get (&result.n_calls), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_API_MISUSE), ==, 1);
	libusb_free_transfer (transfer);

	/* A synchronous transfer waiting on a handle that another thread closes returns LIBUSB_ERROR_NO_DEVICE
	 * (sync.c), at once */
	g_assert_cmpint (libusb_open (device, &handle), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);
	blocked_read.handle = handle;
	blocked_read.result = 0;
	thread = g_thread_new ("blocked-read", blocked_read_thread, &blocked_read);
	condition.device = camera;
	condition.endpoint = FAKE_USB_CAMERA_DATA_ENDPOINT_IN;
	condition.n_pending = 1;
	g_assert_true (fakeuv_wait_for (pending_reached, &condition, FAKEUV_TIMEOUT_MS));
	start = g_get_monotonic_time ();
	libusb_close (handle);
	g_thread_join (thread);
	g_assert_cmpint (blocked_read.result, ==, LIBUSB_ERROR_NO_DEVICE);
	/* Not after the 60 s of an event handling round */
	g_assert_cmpint (g_get_monotonic_time () - start, <, (gint64) fakeuv_timeout_ms (5000) * G_TIME_SPAN_MILLISECOND);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_API_MISUSE), ==, 2);

	/* libusb_close () takes the event lock: it interrupts an event handler instead of waiting for its timeout */
	g_assert_cmpint (libusb_open (device, &handle), ==, LIBUSB_SUCCESS);
	n_handle_events = fake_usb_get_call_count (FAKE_USB_CALL_HANDLE_EVENTS);
	thread = g_thread_new ("events", long_handle_events_thread, ctx);
	while (fake_usb_get_call_count (FAKE_USB_CALL_HANDLE_EVENTS) == n_handle_events)
		g_usleep (1000);
	g_usleep (10000);
	start = g_get_monotonic_time ();
	libusb_close (handle);
	g_thread_join (thread);
	g_assert_cmpint (g_get_monotonic_time () - start, <, (gint64) fakeuv_timeout_ms (5000) * G_TIME_SPAN_MILLISECOND);

	libusb_unref_device (device);
	libusb_exit (ctx);

	fakeuv_test_expect_misuse (2);
}

static void
packets_test (void)
{
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffer[2048];
	int transferred, n_halt_requests;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* A short packet ends a larger read */
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 64, 2);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, arv_uvcp_packet_get_read_memory_ack_size (64));

	/* A packet larger than what is left of a read that is not a packet multiple overflows on Linux and macOS... */
	fake_usb_device_set_pipe_policies (camera, 0, -1);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 64, 3);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, 50,
					       &transferred, 100), ==, LIBUSB_ERROR_OVERFLOW);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_OVERFLOWS), ==, 1);

	/* ...while WinUSB's ALLOW_PARTIAL_READS, that libusb sets on Windows, completes the read with what fits and
	 * gives the rest of the packet to the next read */
	fake_usb_device_set_pipe_policies (camera, 1, -1);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 64, 4);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, 50,
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, 50);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer + 50, 1024,
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, arv_uvcp_packet_get_read_memory_ack_size (64) - 50);
	g_assert_cmpint (arv_uvcp_packet_get_packet_id ((ArvUvcpPacket *) buffer), ==, 4);
	g_assert_cmpstr (arv_uvcp_packet_get_read_memory_ack_data ((ArvUvcpPacket *) buffer), ==, "Aravis");
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_PARTIAL_READS), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_OVERFLOWS), ==, 1);

	/* Invalid addresses are answered with an error status */
	send_read_memory_command (handle, 0x40000000, 4, 5);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, sizeof (ArvUvcpHeader));
	g_assert_cmpint (arv_uvcp_packet_get_status ((ArvUvcpPacket *) buffer), ==, ARV_UVCP_STATUS_INVALID_ADDRESS);

	/* The bootstrap registers are read-only */
	{
		ArvUvcpPacket *packet;
		guint32 value = 0;
		size_t packet_size;

		packet = arv_uvcp_packet_new_write_memory_cmd (ARV_ABRM_DEVICE_CONFIGURATION, sizeof (value), 6,
							       &packet_size);
		memcpy (arv_uvcp_packet_get_write_memory_cmd_data (packet), &value, sizeof (value));
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT,
						       (unsigned char *) packet, packet_size, &transferred, 100), ==,
				 LIBUSB_SUCCESS);
		arv_uvcp_packet_free (packet);
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer,
						       sizeof (buffer), &transferred, 100), ==, LIBUSB_SUCCESS);
		g_assert_cmpint (arv_uvcp_packet_get_status ((ArvUvcpPacket *) buffer), ==,
				 ARV_UVCP_STATUS_WRITE_PROTECT);
	}

	/* Scripted pending acknowledge, then the real one */
	fake_usb_camera_set_pending_acks (camera, 1, 50, 0);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 4, 7);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (arv_uvcp_packet_get_command ((ArvUvcpPacket *) buffer), ==, ARV_UVCP_COMMAND_PENDING_ACK);
	g_assert_cmpint (arv_uvcp_packet_get_pending_ack_timeout ((ArvUvcpPacket *) buffer), ==,
			 fakeuv_timeout_ms (50));
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (arv_uvcp_packet_get_command ((ArvUvcpPacket *) buffer), ==, ARV_UVCP_COMMAND_READ_MEMORY_ACK);

	/* Scripted acknowledges cut short: without their last 2 bytes, then without any, a zero length packet */
	fake_usb_camera_truncate_acks (camera, 1, 2);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 4, 8);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, arv_uvcp_packet_get_read_memory_ack_size (4) - 2);
	g_assert_cmpint (arv_uvcp_packet_get_packet_id ((ArvUvcpPacket *) buffer), ==, 8);
	fake_usb_camera_truncate_acks (camera, 1, G_MAXSIZE);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 4, 9);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, 0);
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 4, 10);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, arv_uvcp_packet_get_read_memory_ack_size (4));

	/* A halted IN endpoint stalls every transfer until libusb_clear_halt ()... */
	fake_usb_device_set_pipe_policies (camera, -1, 0);
	n_halt_requests = libusb_control_transfer (handle, (int) LIBUSB_ENDPOINT_OUT | (int) LIBUSB_REQUEST_TYPE_STANDARD |
						   (int) LIBUSB_RECIPIENT_ENDPOINT, LIBUSB_REQUEST_SET_FEATURE, 0,
						   FAKE_USB_CAMERA_DATA_ENDPOINT_IN, NULL, 0, 100);
	g_assert_cmpint (n_halt_requests, ==, 0);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_ERROR_PIPE);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_ERROR_PIPE);
	g_assert_cmpint (libusb_clear_halt (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 10), ==, LIBUSB_ERROR_TIMEOUT);

	/* ...or, with WinUSB's AUTO_CLEAR_STALL that libusb sets on Windows, only the transfer that meets it */
	fake_usb_device_set_pipe_policies (camera, -1, 1);
	g_assert_cmpint (libusb_control_transfer (handle, (int) LIBUSB_ENDPOINT_OUT | (int) LIBUSB_REQUEST_TYPE_STANDARD |
						  (int) LIBUSB_RECIPIENT_ENDPOINT, LIBUSB_REQUEST_SET_FEATURE, 0,
						  FAKE_USB_CAMERA_DATA_ENDPOINT_IN, NULL, 0, 100), ==, 0);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_ERROR_PIPE);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 10), ==, LIBUSB_ERROR_TIMEOUT);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_AUTO_CLEARED_STALLS), ==, 1);

	close_camera (ctx, device, handle);
}

static void
zero_length_packets_test (void)
{
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffer[2048];
	int transferred;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	g_assert_cmpint (arv_uvcp_packet_get_read_memory_ack_size (1012), ==, 1024);

	/* A 1024 byte acknowledge, a packet multiple, followed by a zero length packet: a read of exactly 1024 bytes
	 * completes because it is full, and the zero length packet completes the next read, with no data */
	fake_usb_camera_set_ack_zero_length_packets (camera, TRUE);
	send_read_memory_command (handle, ARV_FAKE_CAMERA_MEMORY_SIZE, 1012, 1);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, 1024,
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, 1024);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_ZERO_LENGTH_PACKETS), ==, 0);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, 1024,
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_ZERO_LENGTH_PACKETS), ==, 1);

	/* In a larger read, the zero length packet ends the read */
	send_read_memory_command (handle, ARV_FAKE_CAMERA_MEMORY_SIZE, 1012, 2);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, 1024);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_ZERO_LENGTH_PACKETS), ==, 2);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 10), ==, LIBUSB_ERROR_TIMEOUT);

	/* Without zero length packets, the default, nothing follows a packet multiple */
	fake_usb_camera_set_ack_zero_length_packets (camera, FALSE);
	send_read_memory_command (handle, ARV_FAKE_CAMERA_MEMORY_SIZE, 1012, 3);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, 1024,
					       &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, 1024);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, 1024,
					       &transferred, 10), ==, LIBUSB_ERROR_TIMEOUT);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_ZERO_LENGTH_PACKETS), ==, 2);

	close_camera (ctx, device, handle);
}

static void
injection_test (void)
{
	TransferResult result = {0};
	struct timeval tv = { 1, 0 };
	struct libusb_transfer *transfer;
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	libusb_device_handle *second_handle;
	unsigned char buffer[1024];

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* Exclusive open, as WinUSB */
	g_assert_cmpint (libusb_open (device, &second_handle), ==, LIBUSB_SUCCESS);
	libusb_close (second_handle);
	fake_usb_device_set_exclusive_open (camera, TRUE);
	g_assert_cmpint (libusb_open (device, &second_handle), ==, LIBUSB_ERROR_ACCESS);
	fake_usb_device_set_exclusive_open (camera, FALSE);

	/* Permanent open error */
	fake_usb_device_set_open_error (camera, LIBUSB_ERROR_NOT_SUPPORTED);
	g_assert_cmpint (libusb_open (device, &second_handle), ==, LIBUSB_ERROR_NOT_SUPPORTED);
	g_assert_cmpint (libusb_open (device, &second_handle), ==, LIBUSB_ERROR_NOT_SUPPORTED);
	fake_usb_device_set_open_error (camera, 0);

	/* Counted submit errors on one endpoint */
	transfer = libusb_alloc_transfer (0);
	libusb_fill_bulk_transfer (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
				   transfer_cb, &result, 0);
	fake_usb_device_inject_error (camera, FAKE_USB_CALL_SUBMIT_TRANSFER, FAKE_USB_CAMERA_DATA_ENDPOINT_IN,
				      LIBUSB_ERROR_NO_MEM, 2);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_ERROR_NO_MEM);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_ERROR_NO_MEM);

	/* Completion status of the next transfer */
	fake_usb_device_inject_transfer_status (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, LIBUSB_TRANSFER_STALL, 1);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (result.n_calls, ==, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_STALL);

	/* Claim errors, per interface */
	fake_usb_device_inject_error (camera, FAKE_USB_CALL_CLAIM_INTERFACE, FAKE_USB_CAMERA_EVENT_INTERFACE,
				      LIBUSB_ERROR_NOT_SUPPORTED, -1);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_EVENT_INTERFACE), ==,
			 LIBUSB_ERROR_NOT_SUPPORTED);
	fake_usb_device_clear_injections (camera);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_EVENT_INTERFACE), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_release_interface (handle, FAKE_USB_CAMERA_EVENT_INTERFACE), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_claim_interface (handle, 32), ==, LIBUSB_ERROR_INVALID_PARAM);
	g_assert_cmpint (libusb_claim_interface (handle, 7), ==, LIBUSB_ERROR_NOT_FOUND);
	g_assert_cmpint (libusb_release_interface (handle, FAKE_USB_CAMERA_EVENT_INTERFACE), ==,
			 LIBUSB_ERROR_NOT_FOUND);

	/* Transfers on an interface the handle did not claim: the WinUSB and macOS backends refuse them, the Linux
	 * kernel claims the interface; misuse everywhere */
	g_assert_cmpint (libusb_release_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);
	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_ERROR_NOT_FOUND);
	g_assert_cmpint (libusb_clear_halt (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, LIBUSB_ERROR_NOT_FOUND);
	fake_usb_set_platform (FAKE_USB_PLATFORM_MACOS);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_ERROR_NOT_FOUND);
	fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);
	g_assert_cmpint (libusb_clear_halt (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, LIBUSB_SUCCESS);
	memset (&result, 0, sizeof (result));
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_cancel_transfer (transfer), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_API_MISUSE), ==, 5);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);

	/* Kernel driver detach, depending on the capability */
	fake_usb_set_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER, FALSE);
	g_assert_cmpint (libusb_set_auto_detach_kernel_driver (handle, 1), ==, LIBUSB_ERROR_NOT_SUPPORTED);
	fake_usb_set_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER, TRUE);
	g_assert_cmpint (libusb_set_auto_detach_kernel_driver (handle, 1), ==, LIBUSB_SUCCESS);
	g_assert_true (fake_usb_device_get_auto_detach_kernel_driver (camera));
	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER), ==, 2);

	libusb_free_transfer (transfer);
	close_camera (ctx, device, handle);

	fakeuv_test_expect_misuse (5);
}

typedef struct {
	GString *sequence;
	char name;
	enum libusb_transfer_status status;
	int actual_length;
} SequencedResult;

static void LIBUSB_CALL
sequenced_cb (struct libusb_transfer *transfer)
{
	SequencedResult *result = transfer->user_data;

	result->status = transfer->status;
	result->actual_length = transfer->actual_length;
	g_string_append_c (result->sequence, result->name);
}

/* An injected transfer status fails the transfers already pending on the endpoint first, in the order they were
 * submitted and with the bytes they received, as a pipe that starts failing does: a transfer never completes before
 * one submitted earlier on the same endpoint. They do not count. An injection cleared with a count of 0 leaves them
 * pending. */

static void
injected_status_order_test (void)
{
	SequencedResult results[4];
	struct libusb_transfer *transfers[4];
	GString *sequence = g_string_new (NULL);
	struct timeval tv = { 0, 100000 };
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffers[4][2048];
	gint64 end_time;
	guint i;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	for (i = 0; i < G_N_ELEMENTS (transfers); i++) {
		transfers[i] = libusb_alloc_transfer (0);
		memset (&results[i], 0, sizeof (results[i]));
		results[i].sequence = sequence;
		results[i].name = (char) ('a' + i);
		libusb_fill_bulk_transfer (transfers[i], handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[i],
					   sizeof (buffers[i]), sequenced_cb, &results[i], 0);
	}

	/* A 1024 byte acknowledge, a packet multiple with no zero length packet after it: the first read gets it and
	 * waits for more, the second gets nothing */
	g_assert_cmpint (arv_uvcp_packet_get_read_memory_ack_size (1012), ==, 1024);
	g_assert_cmpint (libusb_submit_transfer (transfers[0]), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_submit_transfer (transfers[1]), ==, LIBUSB_SUCCESS);
	send_read_memory_command (handle, ARV_FAKE_CAMERA_MEMORY_SIZE, 1012, 1);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN), ==, 2);

	fake_usb_device_inject_transfer_status (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, LIBUSB_TRANSFER_STALL, 0);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN), ==, 2);

	fake_usb_device_inject_transfer_status (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, LIBUSB_TRANSFER_ERROR, 1);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN), ==, 0);
	/* The one injected failure, then a read that waits again */
	g_assert_cmpint (libusb_submit_transfer (transfers[2]), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_submit_transfer (transfers[3]), ==, LIBUSB_SUCCESS);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN), ==, 1);
	g_assert_cmpint (libusb_cancel_transfer (transfers[3]), ==, LIBUSB_SUCCESS);

	end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS) * G_TIME_SPAN_MILLISECOND;
	while (sequence->len < G_N_ELEMENTS (transfers) && g_get_monotonic_time () < end_time)
		g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);

	g_assert_cmpstr (sequence->str, ==, "abcd");
	g_assert_cmpint (results[0].status, ==, LIBUSB_TRANSFER_ERROR);
	g_assert_cmpint (results[0].actual_length, ==, 1024);
	g_assert_cmpint (results[1].status, ==, LIBUSB_TRANSFER_ERROR);
	g_assert_cmpint (results[1].actual_length, ==, 0);
	g_assert_cmpint (results[2].status, ==, LIBUSB_TRANSFER_ERROR);
	g_assert_cmpint (results[2].actual_length, ==, 0);
	g_assert_cmpint (results[3].status, ==, LIBUSB_TRANSFER_CANCELLED);

	for (i = 0; i < G_N_ELEMENTS (transfers); i++)
		libusb_free_transfer (transfers[i]);
	g_string_free (sequence, TRUE);
	close_camera (ctx, device, handle);
}

typedef struct {
	gint n_arrived;
	gint n_left;
	libusb_device *last_device;
	gboolean deregister;
	GString *sequence;
} HotplugResult;

static int LIBUSB_CALL
hotplug_cb (libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data)
{
	HotplugResult *result = user_data;

	if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
		result->n_arrived++;
	else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
		result->n_left++;
	result->last_device = device;
	if (result->sequence != NULL)
		g_string_append_c (result->sequence, 'H');

	return result->deregister ? 1 : 0;
}

static void LIBUSB_CALL
sequence_transfer_cb (struct libusb_transfer *transfer)
{
	g_string_append_c (transfer->user_data, 'T');
}

static void
hotplug_test (void)
{
	HotplugResult result = {0};
	struct timeval tv = { 0, 50000 };
	struct timeval no_wait = { 0, 0 };
	libusb_hotplug_callback_handle callback_handle = 0;
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	guint8 address, new_address;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* No hotplug in the default Windows libusb */
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);
	g_assert_cmpint (libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG), ==, 0);
	g_assert_cmpint (libusb_hotplug_register_callback (ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0,
							   LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
							   LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb, &result,
							   &callback_handle), ==, LIBUSB_ERROR_NOT_SUPPORTED);
	libusb_hotplug_deregister_callback (ctx, callback_handle);

	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, TRUE);
	fake_usb_set_hotplug_delay (0);
	g_assert_cmpint (libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG), ==, 1);

	/* Enumeration happens during registration */
	g_assert_cmpint (libusb_hotplug_register_callback (ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
							   LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
							   LIBUSB_HOTPLUG_ENUMERATE,
							   FAKE_USB_CAMERA_VENDOR_ID, LIBUSB_HOTPLUG_MATCH_ANY,
							   LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb, &result,
							   &callback_handle), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (callback_handle, >, 0);
	g_assert_cmpint (result.n_arrived, ==, 1);

	/* Later events are delivered from the event handler, with the device object of the open handle */
	fake_usb_device_get_location (camera, NULL, &address, NULL);
	fake_usb_device_unplug (camera);
	g_assert_cmpint (result.n_left, ==, 0);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (result.n_left, ==, 1);
	g_assert_true (result.last_device == libusb_get_device (handle));

	/* A stale device object cannot be opened */
	{
		libusb_device_handle *stale_handle;

		g_assert_cmpint (libusb_open (device, &stale_handle), ==, LIBUSB_ERROR_NO_DEVICE);
	}

	/* Replugged: a new device object, at a new address */
	fake_usb_device_plug (camera);
	fake_usb_device_get_location (camera, NULL, &new_address, NULL);
	g_assert_cmpuint (new_address, !=, address);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (result.n_arrived, ==, 2);
	g_assert_true (result.last_device != device);
	g_assert_cmpint (libusb_get_device_address (result.last_device), ==, new_address);

	/* Hotplug events can lag behind the change, as with the Windows hotplug backend */
	fake_usb_set_hotplug_delay (100);
	fake_usb_device_unplug (camera);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &no_wait), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (result.n_left, ==, 1);
	{
		gint64 end_time = g_get_monotonic_time () + (gint64) fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS) *
			G_TIME_SPAN_MILLISECOND;

		while (result.n_left < 2 && g_get_monotonic_time () < end_time)
			libusb_handle_events_timeout (ctx, &tv);
	}
	g_assert_cmpint (result.n_left, ==, 2);
	fake_usb_set_hotplug_delay (0);

	/* The callback deregisters itself by returning 1 */
	result.deregister = TRUE;
	fake_usb_device_plug (camera);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (result.n_arrived, ==, 3);
	fake_usb_device_unplug (camera);
	libusb_handle_events_timeout (ctx, &tv);
	g_assert_cmpint (result.n_left, ==, 2);

	libusb_hotplug_deregister_callback (ctx, callback_handle);

	close_camera (ctx, device, handle);

	/* Order within an event handling round: hotplug events after the transfers on Windows and macOS, before them
	 * on Linux */
	{
		FakeUsbHotplugOrder orders[] = { FAKE_USB_HOTPLUG_ORDER_TRANSFERS_FIRST, FAKE_USB_HOTPLUG_ORDER_HOTPLUG_FIRST };
		const char *sequences[] = { "TH", "HT" };
		guint i;

		for (i = 0; i < G_N_ELEMENTS (orders); i++) {
			HotplugResult order_result = {0};
			struct libusb_transfer *transfer;
			unsigned char buffer[1024];

			fakeuv_test_reset ();
			fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, TRUE);
			fake_usb_set_hotplug_delay (0);
			fake_usb_set_hotplug_order (orders[i]);
			camera = fakeuv_add_camera ("UV1");
			handle = open_camera (&ctx, &device);

			order_result.sequence = g_string_new (NULL);
			g_assert_cmpint (libusb_hotplug_register_callback (ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0,
									   LIBUSB_HOTPLUG_MATCH_ANY,
									   LIBUSB_HOTPLUG_MATCH_ANY,
									   LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb,
									   &order_result, &callback_handle),
					 ==, LIBUSB_SUCCESS);

			transfer = libusb_alloc_transfer (0);
			libusb_fill_bulk_transfer (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer,
						   sizeof (buffer), sequence_transfer_cb, order_result.sequence, 0);
			g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);

			fake_usb_device_unplug (camera);
			g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
			g_assert_cmpstr (order_result.sequence->str, ==, sequences[i]);

			libusb_hotplug_deregister_callback (ctx, callback_handle);
			libusb_free_transfer (transfer);
			g_string_free (order_result.sequence, TRUE);
			close_camera (ctx, device, handle);
		}
	}
}

/* With hotplug, contexts keep the device objects of attached devices: the same objects, and their string cache,
 * from one enumeration to the next (core.c) */

static void
device_objects_test (void)
{
	libusb_context *ctx;
	libusb_device *first, *second;
	guint hotplug;

	for (hotplug = 0; hotplug < 2; hotplug++) {
		FakeUsbDevice *camera;
		guint i;

		fakeuv_test_reset ();
		fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, hotplug);
		camera = fakeuv_add_camera ("UV1");

		g_assert_cmpint (libusb_init (&ctx), ==, LIBUSB_SUCCESS);

		first = get_camera_device (ctx);
		second = get_camera_device (ctx);
		/* Referenced by this test: the same object either way */
		g_assert_true (first == second);
		libusb_unref_device (first);
		libusb_unref_device (second);

		for (i = 0; i < 3; i++) {
			libusb_device *device = get_camera_device (ctx);

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
			{
				char string[LIBUSB_DEVICE_STRING_BYTES_MAX];

				g_assert_cmpint (libusb_get_device_string (device, LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
									   string, sizeof (string)), ==, 4);
			}
#endif
			libusb_unref_device (device);
		}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
		g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_DEVICE_STRING_FETCHES), ==,
				  hotplug ? 1 : 3);
#endif

		/* The device leaves: the context drops its object */
		fake_usb_device_unplug (camera);
		fake_usb_device_plug (camera);
		first = get_camera_device (ctx);
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
		{
			char string[LIBUSB_DEVICE_STRING_BYTES_MAX];

			g_assert_cmpint (libusb_get_device_string (first, LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
								   string, sizeof (string)), ==, 4);
			g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_DEVICE_STRING_FETCHES), ==,
					  hotplug ? 2 : 4);
		}
#endif
		libusb_unref_device (first);

		libusb_exit (ctx);
	}
}

static void
unplug_errors_test (void)
{
	TransferResult result = {0};
	struct timeval tv = { 1, 0 };
	struct libusb_transfer *transfer;
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffer[1024];
	int transferred;
	guint platform;

	for (platform = 0; platform < 3; platform++) {
		enum libusb_transfer_status expected_status;
		int expected_error;

		fakeuv_test_reset ();
		fake_usb_set_platform (platform == 0 ? FAKE_USB_PLATFORM_LINUX :
				       platform == 1 ? FAKE_USB_PLATFORM_WINDOWS : FAKE_USB_PLATFORM_LINUX);
		camera = fakeuv_add_camera ("UV1");
		if (platform == 2)
			fake_usb_device_set_unplug_errors (camera, LIBUSB_TRANSFER_STALL, LIBUSB_ERROR_NO_MEM,
							   LIBUSB_ERROR_PIPE);
		handle = open_camera (&ctx, &device);

		expected_status = platform == 0 ? LIBUSB_TRANSFER_NO_DEVICE :
			platform == 1 ? LIBUSB_TRANSFER_ERROR : LIBUSB_TRANSFER_STALL;

		memset (&result, 0, sizeof (result));
		transfer = libusb_alloc_transfer (0);
		libusb_fill_bulk_transfer (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer),
					   transfer_cb, &result, 0);
		g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);

		fake_usb_device_unplug (camera);

		g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
		g_assert_cmpint (result.n_calls, ==, 1);
		g_assert_cmpint (result.status, ==, expected_status);

		expected_error = platform == 0 ? LIBUSB_ERROR_NO_DEVICE :
			platform == 1 ? LIBUSB_ERROR_IO : LIBUSB_ERROR_NO_MEM;
		g_assert_cmpint (libusb_submit_transfer (transfer), ==, expected_error);

		expected_error = platform == 0 ? LIBUSB_ERROR_NO_DEVICE :
			platform == 1 ? LIBUSB_ERROR_IO : LIBUSB_ERROR_PIPE;
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer,
						       sizeof (buffer), &transferred, 100), ==, expected_error);
		g_assert_cmpint (libusb_control_transfer (handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
							  LIBUSB_DT_DEVICE << 8, 0, buffer, LIBUSB_DT_DEVICE_SIZE, 100),
				 ==, expected_error);

		/* Calls that core.c checks against the device state fail the same way everywhere */
		g_assert_cmpint (libusb_clear_halt (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
				 LIBUSB_ERROR_NO_DEVICE);
		g_assert_cmpint (libusb_reset_device (handle), ==, LIBUSB_ERROR_NO_DEVICE);
		g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_EVENT_INTERFACE), ==,
				 LIBUSB_ERROR_NO_DEVICE);
		g_assert_cmpint (libusb_release_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==,
				 LIBUSB_ERROR_NO_DEVICE);
		/* The cached descriptors stay readable */
		g_assert_cmpint (libusb_get_max_packet_size (device, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 1024);

		libusb_free_transfer (transfer);
		close_camera (ctx, device, handle);
	}
}

/* fake_usb_device_set_unplug_errors () with 0: the transfer is accepted, then fails at once; and transfers that
 * hang after the unplug */

static void
unplug_accepted_test (void)
{
	TransferResult result;
	struct libusb_transfer *transfer;
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffer[1024];
	int transferred = -1;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	fake_usb_device_set_unplug_errors (camera, LIBUSB_TRANSFER_ERROR, 0, 0);
	handle = open_camera (&ctx, &device);
	fake_usb_device_unplug (camera);

	transfer = libusb_alloc_transfer (0);
	submit_in (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer), transfer_cb, &result, 0);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_ERROR);
	g_assert_cmpint (result.actual_length, ==, 0);

	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffer, sizeof (buffer),
					       &transferred, 100), ==, LIBUSB_ERROR_IO);
	g_assert_cmpint (transferred, ==, 0);
	g_assert_cmpint (libusb_control_transfer (handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
						  LIBUSB_DT_DEVICE << 8, 0, buffer, LIBUSB_DT_DEVICE_SIZE, 100),
			 ==, LIBUSB_ERROR_IO);

	libusb_free_transfer (transfer);
	close_camera (ctx, device, handle);

	/* Transfers in flight that never complete after the unplug: only cancellation or their timeout end them */
	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	fake_usb_device_set_unplug_errors (camera, LIBUSB_TRANSFER_ERROR, 0, LIBUSB_ERROR_IO);
	fake_usb_device_set_unplug_transfers_hang (camera, TRUE);
	handle = open_camera (&ctx, &device);

	transfer = libusb_alloc_transfer (0);
	submit_in (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer), transfer_cb, &result, 0);
	fake_usb_device_unplug (camera);
	{
		struct timeval tv = { 0, 50000 };

		g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	}
	g_assert_cmpint (g_atomic_int_get (&result.n_calls), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 1);
	g_assert_cmpint (libusb_cancel_transfer (transfer), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_CANCELLED);

	/* Accepted after the unplug, and hanging too, until the timeout */
	submit_in (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer), transfer_cb, &result, 20);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_TIMED_OUT);

	/* Removing the device ends them for good */
	submit_in (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, sizeof (buffer), transfer_cb, &result, 0);
	fake_usb_device_remove (camera);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_ERROR);

	libusb_free_transfer (transfer);
	close_camera (ctx, device, handle);
}

/* fake_usb_device_fail_transfers (): transfers fail, the device stays */

static void
fail_transfers_test (void)
{
	TransferResult results[2];
	struct libusb_transfer *transfers[2];
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffers[2][1024];
	int transferred;
	guint i;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	/* After 30 bytes of a 76 byte acknowledge: the read receiving it and the next pending one fail */
	fake_usb_device_fail_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, LIBUSB_TRANSFER_ERROR, 30, FALSE);
	for (i = 0; i < 2; i++) {
		transfers[i] = libusb_alloc_transfer (0);
		submit_in (transfers[i], handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[i], sizeof (buffers[i]),
			   transfer_cb, &results[i], 0);
	}
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 64, 1);
	handle_events_until (ctx, &results[1], 1);
	g_assert_cmpint (results[0].n_calls, ==, 1);
	g_assert_cmpint (results[0].status, ==, LIBUSB_TRANSFER_ERROR);
	g_assert_cmpint (results[0].actual_length, ==, 30);
	g_assert_cmpint (results[1].status, ==, LIBUSB_TRANSFER_ERROR);
	g_assert_cmpint (results[1].actual_length, ==, 0);
	g_assert_true (fake_usb_device_is_plugged (camera));
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_SCRIPTED_FAILURES), ==, 2);

	/* Without flush, the rest of the acknowledge comes next */
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[0],
					       sizeof (buffers[0]), &transferred, 100), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (transferred, ==, arv_uvcp_packet_get_read_memory_ack_size (64) - 30);

	/* At once, with flush: what the device did not send yet is dropped */
	send_read_memory_command (handle, ARV_ABRM_MANUFACTURER_NAME, 64, 2);
	fake_usb_device_fail_transfers (camera, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, LIBUSB_TRANSFER_STALL, 0, TRUE);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[0],
					       sizeof (buffers[0]), &transferred, 100), ==, LIBUSB_ERROR_PIPE);
	g_assert_cmpint (transferred, ==, 0);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN, buffers[0],
					       sizeof (buffers[0]), &transferred, 10), ==, LIBUSB_ERROR_TIMEOUT);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_SCRIPTED_FAILURES), ==, 3);

	for (i = 0; i < 2; i++)
		libusb_free_transfer (transfers[i]);
	close_camera (ctx, device, handle);
}

/* A usbfs-like memory budget: transfers not reaped yet count against it */

static void
memory_limit_test (void)
{
	TransferResult results[3];
	struct libusb_transfer *transfers[3];
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffers[3][1024];
	unsigned char big_buffer[4096];
	int transferred;
	guint i;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	fake_usb_set_submit_memory_limit (2048, 0);

	for (i = 0; i < 3; i++) {
		transfers[i] = libusb_alloc_transfer (0);
		memset (&results[i], 0, sizeof (results[i]));
		libusb_fill_bulk_transfer (transfers[i], handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffers[i],
					   sizeof (buffers[i]), transfer_cb, &results[i], 0);
	}

	g_assert_cmpint (libusb_submit_transfer (transfers[0]), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_submit_transfer (transfers[1]), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_submit_transfer (transfers[2]), ==, LIBUSB_ERROR_NO_MEM);
	g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, big_buffer, sizeof (big_buffer),
					       &transferred, 10), ==, LIBUSB_ERROR_NO_MEM);

	/* Completed is not enough: the memory comes back once the completion is reaped by event handling */
	g_assert_cmpint (libusb_cancel_transfer (transfers[0]), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_submit_transfer (transfers[2]), ==, LIBUSB_ERROR_NO_MEM);
	handle_events_until (ctx, &results[0], 1);
	g_assert_cmpint (libusb_submit_transfer (transfers[2]), ==, LIBUSB_SUCCESS);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_MEMORY_LIMIT_REJECTIONS), ==, 3);

	/* Another error, as old libusb reported it */
	fake_usb_set_submit_memory_limit (2048, LIBUSB_ERROR_IO);
	g_assert_cmpint (libusb_submit_transfer (transfers[0]), ==, LIBUSB_ERROR_IO);

	fake_usb_set_submit_memory_limit (0, 0);
	g_assert_cmpint (libusb_cancel_transfer (transfers[1]), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_cancel_transfer (transfers[2]), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &results[1], 1);
	handle_events_until (ctx, &results[2], 1);

	for (i = 0; i < 3; i++)
		libusb_free_transfer (transfers[i]);
	close_camera (ctx, device, handle);
}

/* fake_usb_device_set_control_delay (): a slow or silent default control pipe */

static void
control_delay_test (void)
{
	TransferResult result;
	struct libusb_transfer *transfer;
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char buffer[LIBUSB_CONTROL_SETUP_SIZE + LIBUSB_DT_DEVICE_SIZE];
	gint64 start;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	fake_usb_device_set_control_delay (camera, 50);
	start = g_get_monotonic_time ();
	g_assert_cmpint (libusb_control_transfer (handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
						  LIBUSB_DT_DEVICE << 8, 0, buffer, LIBUSB_DT_DEVICE_SIZE,
						  fakeuv_timeout_ms (1000)), ==,
			 LIBUSB_DT_DEVICE_SIZE);
	g_assert_cmpint (g_get_monotonic_time () - start, >=, 40 * G_TIME_SPAN_MILLISECOND);

	/* A device that never answers: the transfer times out */
	fake_usb_device_set_control_delay (camera, FAKE_USB_NEVER);
	g_assert_cmpint (libusb_control_transfer (handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
						  LIBUSB_DT_DEVICE << 8, 0, buffer, LIBUSB_DT_DEVICE_SIZE, 30), ==,
			 LIBUSB_ERROR_TIMEOUT);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_TIMEOUTS), ==, 1);

	/* Asynchronous, it waits until cancelled */
	transfer = libusb_alloc_transfer (0);
	libusb_fill_control_setup (buffer, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_DEVICE << 8, 0,
				   LIBUSB_DT_DEVICE_SIZE);
	memset (&result, 0, sizeof (result));
	libusb_fill_control_transfer (transfer, handle, buffer, transfer_cb, &result, 0);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpuint (fake_usb_device_get_n_pending_transfers (camera, 0x00), ==, 1);
	g_assert_cmpint (libusb_cancel_transfer (transfer), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_CANCELLED);

	fake_usb_device_set_control_delay (camera, 0);
	memset (&result, 0, sizeof (result));
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &result, 1);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_COMPLETED);
	g_assert_cmpint (result.actual_length, ==, LIBUSB_DT_DEVICE_SIZE);
	g_assert_cmpint (libusb_control_transfer_get_data (transfer)[1], ==, LIBUSB_DT_DEVICE);

	libusb_free_transfer (transfer);
	close_camera (ctx, device, handle);
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
static void
raw_io_test (void)
{
	TransferResult result = {0};
	struct timeval tv = { 1, 0 };
	struct libusb_transfer *transfer;
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char *buffer;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	fake_usb_device_set_raw_io_supported (camera, FALSE);
	g_assert_cmpint (libusb_endpoint_supports_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 0);
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==,
			 LIBUSB_ERROR_NOT_SUPPORTED);
	g_assert_cmpint (libusb_get_max_raw_io_transfer_size (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
			 LIBUSB_ERROR_NOT_SUPPORTED);

	fake_usb_device_set_raw_io_supported (camera, TRUE);
	fake_usb_device_set_max_raw_io_transfer_size (camera, 64 * 1024);
	g_assert_cmpint (libusb_endpoint_supports_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 1);
	g_assert_cmpint (libusb_endpoint_supports_raw_io (handle, FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT), ==,
			 LIBUSB_ERROR_INVALID_PARAM);
	g_assert_cmpint (libusb_endpoint_supports_raw_io (handle, FAKE_USB_CAMERA_EVENT_ENDPOINT_IN), ==,
			 LIBUSB_ERROR_NOT_FOUND);
	g_assert_cmpint (libusb_get_max_raw_io_transfer_size (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 64 * 1024);

	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==, LIBUSB_SUCCESS);
	g_assert_true (fake_usb_device_get_raw_io (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	buffer = g_malloc (128 * 1024);
	transfer = libusb_alloc_transfer (0);

	/* Not a multiple of the max packet size, or larger than the RAW_IO maximum: rejected like WinUSB does */
	libusb_fill_bulk_transfer (transfer, handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, 1000,
				   transfer_cb, &result, 0);
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_ERROR_IO);
	transfer->length = 128 * 1024;
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_ERROR_IO);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS), ==, 2);
	{
		int transferred;

		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, 52,
						       &transferred, 10), ==, LIBUSB_ERROR_IO);
	}

	/* A change while a transfer is pending: libusb checks nothing, it is an application bug... */
	transfer->length = 2048;
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 0), ==, LIBUSB_SUCCESS);
	g_assert_false (fake_usb_device_get_raw_io (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_RAW_IO_CHANGES_WHILE_PENDING), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_API_MISUSE), ==, 1);

	/* ...which can also be made to fail, with an error WinUSB can return */
	fake_usb_device_set_raw_io_pending_change_error (camera, LIBUSB_ERROR_OTHER);
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==,
			 LIBUSB_ERROR_OTHER);
	g_assert_false (fake_usb_device_get_raw_io (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));

	g_assert_cmpint (libusb_cancel_transfer (transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_handle_events_timeout (ctx, &tv), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (result.status, ==, LIBUSB_TRANSFER_CANCELLED);
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==, LIBUSB_SUCCESS);
	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO), ==, 5);

	/* The policy lives in the WinUSB interface handle: releasing the interface, or closing, turns it off */
	g_assert_cmpint (libusb_release_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);
	g_assert_false (fake_usb_device_get_raw_io (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==,
			 LIBUSB_ERROR_NOT_FOUND);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==, LIBUSB_SUCCESS);
	libusb_close (handle);
	g_assert_false (fake_usb_device_get_raw_io (camera, FAKE_USB_CAMERA_DATA_ENDPOINT_IN));
	g_assert_cmpint (libusb_open (device, &handle), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_claim_interface (handle, FAKE_USB_CAMERA_DATA_INTERFACE), ==, LIBUSB_SUCCESS);
	transfer->dev_handle = handle;
	transfer->length = 52;
	memset (&result, 0, sizeof (result));
	g_assert_cmpint (libusb_submit_transfer (transfer), ==, LIBUSB_SUCCESS);
	g_assert_cmpint (libusb_cancel_transfer (transfer), ==, LIBUSB_SUCCESS);
	handle_events_until (ctx, &result, 1);

	/* Once the device is gone: the support query needs no device, the policy calls fail */
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 1), ==, LIBUSB_SUCCESS);
	fake_usb_device_unplug (camera);
	g_assert_cmpint (libusb_endpoint_supports_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==, 1);
	g_assert_cmpint (libusb_endpoint_set_raw_io (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, 0), ==,
			 LIBUSB_ERROR_OTHER);
	g_assert_cmpint (libusb_get_max_raw_io_transfer_size (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN), ==,
			 LIBUSB_ERROR_OTHER);

	libusb_free_transfer (transfer);
	g_free (buffer);
	close_camera (ctx, device, handle);

	fakeuv_test_expect_misuse (1);
}
#endif

/* The camera on the wire, driven through the control channel as Aravis does: leader, payload split as the SIRM
 * registers say (with the final transfer padded to TRANSFER1_SIZE on request), trailer, 64 bit frame ids */

static void
camera_wire_test (void)
{
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	unsigned char *buffer;
	ArvUvspPacket *packet;
	guint64 frame_id = G_GUINT64_CONSTANT (1) << 40;
	gsize image_size = 1440 * 1080;
	gsize remainder = image_size - 1024 * 1024;
	gsize transfer1_size = (remainder + 1023) / 1024 * 1024;
	guint32 checksum, sent_checksum;
	gsize sent_size;
	int transferred;
	guint frame;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_geometry (camera, 1440, 1080, ARV_PIXEL_FORMAT_MONO_8);
	fake_usb_camera_set_alignment_shift (camera, 10);
	fake_usb_camera_set_pad_final_transfer (camera, TRUE);
	fake_usb_camera_set_next_frame_id (camera, frame_id);
	handle = open_camera (&ctx, &device);

	g_assert_cmpuint (fake_usb_camera_get_sirm_register (camera, ARV_SIRM_INFO) >> ARV_SIRM_INFO_ALIGNMENT_SHIFT,
			  ==, 10);

	/* What arv_uv_stream_start_acquisition () would write for this geometry and a 1 MiB transfer size */
	write_sirm (handle, ARV_SIRM_PAYLOAD_SIZE, 1024 * 1024, 1);
	write_sirm (handle, ARV_SIRM_PAYLOAD_COUNT, 1, 2);
	write_sirm (handle, ARV_SIRM_TRANSFER1_SIZE, transfer1_size, 3);
	write_sirm (handle, ARV_SIRM_TRANSFER2_SIZE, 0, 4);
	write_sirm (handle, ARV_SIRM_MAX_LEADER_SIZE, 1024, 5);
	write_sirm (handle, ARV_SIRM_MAX_TRAILER_SIZE, 1024, 6);
	arv_fake_camera_write_register (fake_usb_camera_get_fake_camera (camera), ARV_FAKE_CAMERA_REGISTER_ACQUISITION, 1);
	write_sirm (handle, ARV_SIRM_CONTROL, ARV_SIRM_CONTROL_STREAM_ENABLE, 7);

	buffer = g_malloc (2 * 1024 * 1024);

	for (frame = 0; frame < 2; frame++) {
		/* Leader */
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, 1024,
						       &transferred, fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS)), ==,
				 LIBUSB_SUCCESS);
		packet = (ArvUvspPacket *) buffer;
		g_assert_cmpint (transferred, ==, sizeof (ArvUvspLeader));
		g_assert_cmpint (arv_uvsp_packet_get_packet_type (packet), ==, ARV_UVSP_PACKET_TYPE_LEADER);
		g_assert_cmpuint (arv_uvsp_packet_get_frame_id (packet), ==, frame_id + frame);
		g_assert_cmphex (arv_uvsp_packet_get_pixel_format (packet), ==, ARV_PIXEL_FORMAT_MONO_8);

		/* Payload: one 1 MiB transfer, then the remainder padded to TRANSFER1_SIZE */
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, 1024 * 1024,
						       &transferred, fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS)), ==,
				 LIBUSB_SUCCESS);
		g_assert_cmpint (transferred, ==, 1024 * 1024);
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer + 1024 * 1024,
						       transfer1_size, &transferred, fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS)),
				 ==, LIBUSB_SUCCESS);
		g_assert_cmpint (transferred, ==, transfer1_size);
		g_assert_cmpuint (transfer1_size, >, remainder);
		g_assert_cmpint (buffer[image_size], ==, 0);
		g_assert_cmpint (buffer[1024 * 1024 + transfer1_size - 1], ==, 0);

		g_assert_true (fake_usb_camera_get_frame_checksum (camera, frame_id + frame, &sent_checksum, &sent_size));
		g_assert_cmpuint (sent_size, ==, image_size);
		checksum = fake_usb_checksum (buffer, image_size);
		g_assert_cmphex (checksum, ==, sent_checksum);

		if (frame == 0) {
			/* Arm an unplug after the next frame, then reset the counters: the target follows */
			g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_FRAMES_SENT), ==, 0);
		}

		/* Trailer */
		g_assert_cmpint (libusb_bulk_transfer (handle, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, buffer, 1024,
						       &transferred, fakeuv_timeout_ms (FAKEUV_TIMEOUT_MS)), ==,
				 LIBUSB_SUCCESS);
		packet = (ArvUvspPacket *) buffer;
		g_assert_cmpint (transferred, ==, sizeof (ArvUvspTrailer));
		g_assert_cmpint (arv_uvsp_packet_get_packet_type (packet), ==, ARV_UVSP_PACKET_TYPE_TRAILER);
		g_assert_cmpuint (arv_uvsp_packet_get_frame_id (packet), ==, frame_id + frame);
		g_assert_cmpuint (GUINT64_FROM_LE (((ArvUvspTrailer *) buffer)->infos.payload_size), ==, image_size);

		if (frame == 0) {
			g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_FRAMES_SENT), ==, 1);
			fake_usb_device_unplug_after_frames (camera, 1, 0);
			fake_usb_reset_counters ();
		}
	}

	/* The unplug armed before the counter reset happened after the second frame */
	g_assert_false (fake_usb_device_is_plugged (camera));
	g_assert_cmpuint (fake_usb_device_get_stat (camera, FAKE_USB_STAT_FRAMES_SENT), ==, 1);

	g_free (buffer);
	close_camera (ctx, device, handle);
}

static void
misc_test (void)
{
	const struct libusb_version *version;
	guint time_scale;

	fakeuv_test_reset ();

	fake_usb_set_version (1, 0, 26, 0);
	version = libusb_get_version ();
	g_assert_cmpint (version->major, ==, 1);
	g_assert_cmpint (version->micro, ==, 26);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_GET_VERSION), ==, 1);

	g_assert_cmpstr (libusb_error_name (LIBUSB_ERROR_NO_DEVICE), ==, "LIBUSB_ERROR_NO_DEVICE");
	g_assert_cmpstr (libusb_error_name (LIBUSB_TRANSFER_STALL), ==, "LIBUSB_TRANSFER_STALL");
	g_assert_cmpstr (libusb_strerror (LIBUSB_ERROR_ACCESS), ==, "Permission denied");
	g_assert_cmpstr (libusb_strerror (LIBUSB_ERROR_OTHER), ==, "Unspecified error");

	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	g_assert_cmpint (libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG), ==, 0);
	g_assert_cmpint (libusb_has_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER), ==, 0);
	fake_usb_set_platform (FAKE_USB_PLATFORM_LINUX);
	g_assert_cmpint (libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG), ==, 1);
	g_assert_cmpint (libusb_has_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER), ==, 1);

	fake_usb_inject_error (FAKE_USB_CALL_INIT, LIBUSB_ERROR_OTHER, 1);
	{
		libusb_context *ctx = NULL;
		struct timeval invalid = { 0, 1000000 };

		g_assert_cmpint (libusb_init (&ctx), ==, LIBUSB_ERROR_OTHER);
		g_assert_cmpint (libusb_init (&ctx), ==, LIBUSB_SUCCESS);
		g_assert_cmpint (libusb_handle_events_timeout (ctx, &invalid), ==, LIBUSB_ERROR_INVALID_PARAM);
		libusb_exit (ctx);
	}

	g_assert_cmpstr (fake_usb_call_to_string (FAKE_USB_CALL_SUBMIT_TRANSFER), ==, "libusb_submit_transfer");

	/* Process wide, kept by fake_usb_reset () */
	time_scale = fake_usb_get_time_scale ();
	fake_usb_set_time_scale (3);
	fake_usb_reset ();
	g_assert_cmpuint (fake_usb_get_time_scale (), ==, 3);
	fake_usb_set_time_scale (time_scale);
}

#ifdef G_OS_WIN32
static gpointer
priority_thread (gpointer data)
{
	libusb_context *ctx = data;
	struct timeval tv = { 0, 1000 };

	SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_HIGHEST);
	libusb_handle_events_timeout (ctx, &tv);
	SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL);
	libusb_handle_events_timeout (ctx, &tv);

	return NULL;
}

static void
thread_priority_test (void)
{
	FakeUsbDevice *camera;
	libusb_context *ctx;
	libusb_device *device;
	libusb_device_handle *handle;
	int last, highest;

	fakeuv_test_reset ();
	camera = fakeuv_add_camera ("UV1");
	handle = open_camera (&ctx, &device);

	g_thread_join (g_thread_new ("priority", priority_thread, ctx));

	g_assert_true (fake_usb_device_get_event_thread_priority (camera, &last, &highest));
	g_assert_cmpint (last, ==, THREAD_PRIORITY_NORMAL);
	g_assert_cmpint (highest, ==, THREAD_PRIORITY_HIGHEST);

	close_camera (ctx, device, handle);
}
#endif

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvlibusb/descriptors", descriptors_test);
	fakeuv_test_add ("/fakeuvlibusb/async-semantics", async_semantics_test);
	fakeuv_test_add ("/fakeuvlibusb/event-lock", event_lock_test);
	fakeuv_test_add ("/fakeuvlibusb/close", close_test);
	fakeuv_test_add ("/fakeuvlibusb/packets", packets_test);
	fakeuv_test_add ("/fakeuvlibusb/zero-length-packets", zero_length_packets_test);
	fakeuv_test_add ("/fakeuvlibusb/injection", injection_test);
	fakeuv_test_add ("/fakeuvlibusb/injected-status-order", injected_status_order_test);
	fakeuv_test_add ("/fakeuvlibusb/hotplug", hotplug_test);
	fakeuv_test_add ("/fakeuvlibusb/device-objects", device_objects_test);
	fakeuv_test_add ("/fakeuvlibusb/unplug-errors", unplug_errors_test);
	fakeuv_test_add ("/fakeuvlibusb/unplug-accepted", unplug_accepted_test);
	fakeuv_test_add ("/fakeuvlibusb/fail-transfers", fail_transfers_test);
	fakeuv_test_add ("/fakeuvlibusb/memory-limit", memory_limit_test);
	fakeuv_test_add ("/fakeuvlibusb/control-delay", control_delay_test);
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	fakeuv_test_add ("/fakeuvlibusb/raw-io", raw_io_test);
#endif
	fakeuv_test_add ("/fakeuvlibusb/camera-wire", camera_wire_test);
	fakeuv_test_add ("/fakeuvlibusb/misc", misc_test);
#ifdef G_OS_WIN32
	fakeuv_test_add ("/fakeuvlibusb/thread-priority", thread_priority_test);
#endif

	return fakeuv_test_run ();
}
