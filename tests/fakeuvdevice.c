/* SPDX-License-Identifier:Unlicense */

/* USB3 Vision device open and discovery through Aravis's API, against the fake libusb: unavailable configuration
 * descriptors, a data interface without endpoint, open errors, read and write memory chunking, pending and short
 * acknowledges, kernel driver auto-detach, hotplug registration, and the log of the RAW_IO state, off by default.
 *
 * Aravis logs on stderr. The tests that check its log run a scenario registered under /fakeuvdevice/subprocess/,
 * which GLib only runs through g_test_trap_subprocess (), and match the stderr of that subprocess. */

#include "fakeuvtest.h"
#include <arvuvcpprivate.h>
#include <arvuvdeviceprivate.h>
#include <stdio.h>
#include <string.h>

/* Unused by ArvFakeCamera's GenICam description, and writable through the fake camera */
#define SCRATCH_ADDRESS		0x8000

/* The version the fake libusb reports in the scenarios that check the log */
#define FAKE_LIBUSB_VERSION	1, 0, 30, 11946
#define FAKE_LIBUSB_VERSION_LOG	"libusb 1.0.30.11946"

/* Discovery and open */

static void
discovery_config_descriptor_test (void)
{
	FakeUsbDevice *broken;
	FakeUsbDevice *camera;
	char *device_id;

	/* A camera whose configuration descriptor is unavailable (libusb's Windows backend returns
	 * LIBUSB_ERROR_NOT_FOUND when it has no cached copy) is skipped, the others are listed */
	broken = fakeuv_add_camera ("UV1");
	fake_usb_device_set_config_descriptor_error (broken, LIBUSB_ERROR_NOT_FOUND);
	camera = fakeuv_add_camera ("UV2");

	arv_update_device_list ();

	g_assert_cmpuint (arv_get_n_devices (), ==, 1);
	device_id = fake_usb_camera_dup_device_id (camera);
	g_assert_cmpstr (arv_get_device_id (0), ==, device_id);
	g_free (device_id);
	g_assert_cmpuint (fake_usb_device_get_call_count (broken, FAKE_USB_CALL_OPEN), ==, 0);

	fake_usb_device_clear_injections (broken);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 2);
}

static void
open_config_descriptor_test (void)
{
	FakeUsbDevice *device;
	ArvDevice *arv_device;
	ArvCamera *camera;
	GError *error = NULL;

	device = fakeuv_add_camera ("UV1");
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	fake_usb_device_set_config_descriptor_error (device, LIBUSB_ERROR_NOT_FOUND);

	/* Open only considers the devices that pass discovery's USB3 Vision descriptor test, whose interface part
	 * needs the configuration descriptor: the camera is not found, by GUID (the GUID string index is in the
	 * configuration descriptor too) or by vendor, model and serial number, and not opened */
	camera = fakeuv_open_camera (device, &error);
	g_assert_null (camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_clear_error (&error);

	arv_device = arv_uv_device_new ("Aravis", "Fake", "UV1", &error);
	g_assert_null (arv_device);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND);
	g_clear_error (&error);

	/* Only discovery opened it, before the descriptor became unavailable */
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_OPEN), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);

	fake_usb_device_clear_injections (device);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_clear_object (&camera);
}

static void
no_data_endpoint_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	/* Discovery only checks the interfaces: the camera is listed */
	device = fakeuv_add_camera ("UV1");
	fake_usb_device_remove_endpoints (device, FAKE_USB_CAMERA_DATA_INTERFACE);
	arv_update_device_list ();
	g_assert_cmpuint (arv_get_n_devices (), ==, 1);

	/* With no endpoint to stream from, it cannot be opened */
	camera = fakeuv_open_camera (device, &error);
	g_assert_null (camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR);
	g_assert_nonnull (strstr (error->message, "no endpoint"));
	g_clear_error (&error);

	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLAIM_INTERFACE), ==, 0);
}

static void
open_error_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	device = fakeuv_add_camera ("UV1");
	arv_update_device_list ();
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_OPEN), ==, 1);

	/* The control interface claim fails at the first open, the data interface claim at the second one */
	fake_usb_device_inject_error (device, FAKE_USB_CALL_CLAIM_INTERFACE, FAKE_USB_CAMERA_CONTROL_INTERFACE,
				      LIBUSB_ERROR_BUSY, 1);
	fake_usb_device_inject_error (device, FAKE_USB_CALL_CLAIM_INTERFACE, FAKE_USB_CAMERA_DATA_INTERFACE,
				      LIBUSB_ERROR_BUSY, 1);

	/* A known device that fails to open is reported at once, with its error: no discovery, no second open whose
	 * error would replace (and leak) the first one */
	camera = fakeuv_open_camera (device, &error);
	g_assert_null (camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR);
	g_assert_nonnull (strstr (error->message, "control interface"));
	g_assert_nonnull (strstr (error->message, "LIBUSB_ERROR_BUSY"));
	g_clear_error (&error);

	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_OPEN), ==, 2);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_CLAIM_INTERFACE), ==, 1);
	g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);

	/* A device the interface does not know yet is still looked for with a new discovery */
	fake_usb_device_clear_injections (device);
	fakeuv_add_camera ("UV2");
	camera = arv_camera_new ("Aravis-Fake-UV2", &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_clear_object (&camera);
}

/* Read and write memory */

typedef struct {
	GMutex mutex;
	int max_command;
	int max_ack;
} ControlTransfers;

static void
control_transfers_hook (FakeUsbDevice *device, guint8 endpoint, int length, gboolean is_synchronous,
			gpointer user_data)
{
	ControlTransfers *transfers = user_data;

	g_mutex_lock (&transfers->mutex);
	if (endpoint == FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT)
		transfers->max_command = MAX (transfers->max_command, length);
	else if (endpoint == FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN)
		transfers->max_ack = MAX (transfers->max_ack, length);
	g_mutex_unlock (&transfers->mutex);
}

static void
fill_pattern (guint8 *data, gsize size, guint seed)
{
	gsize i;

	for (i = 0; i < size; i++)
		data[i] = (guint8) (i * 31 + seed);
}

typedef struct {
	guint32 max_cmd_transfer;
	guint32 max_ack_transfer;
	/* The largest command and acknowledge: their headers, and as many whole 32 bit words of data as fit, so that
	 * every block starts aligned */
	guint32 largest_command;
	guint32 largest_ack;
} MaxTransfers;

static const MaxTransfers max_transfers[] = {
	{ 1024, 1024, 1024, 1024 },	/* The fake camera's defaults */
	{ 512, 2048, 512, 2048 },
	{ 2048, 512, 2048, 512 },
	{ 24, 1024, 24, 1024 },		/* Room for 4 bytes in a write command */
	{ 1023, 1023, 1020, 1020 }	/* Write blocks of 1000 bytes, read blocks of 1008 bytes */
};

/* Around the largest block of one command with the default maximum transfers: 1004 bytes per write, 1012 per read */
static const guint32 memory_sizes[] = { 4, 8, 1004, 1005, 1012, 1013, 4000 };

static void
memory_chunks_test (void)
{
	guint i, j;

	for (i = 0; i < G_N_ELEMENTS (max_transfers); i++) {
		ControlTransfers transfers = { 0 };
		FakeUsbDevice *device;
		ArvCamera *camera;
		ArvDevice *arv_device;
		GError *error = NULL;

		fakeuv_test_reset ();

		g_mutex_init (&transfers.mutex);
		fake_usb_set_transfer_hook (control_transfers_hook, &transfers);

		device = fakeuv_add_camera ("UV1");
		fake_usb_camera_set_max_transfers (device, max_transfers[i].max_cmd_transfer,
						   max_transfers[i].max_ack_transfer);

		camera = fakeuv_open_camera (device, &error);
		g_assert_no_error (error);
		arv_device = arv_camera_get_device (camera);

		for (j = 0; j < G_N_ELEMENTS (memory_sizes); j++) {
			guint32 size = memory_sizes[j];
			guint8 *written = g_malloc (size);
			guint8 *in_camera = g_malloc0 (size);
			guint8 *read = g_malloc0 (size);

			fill_pattern (written, size, (guint) (i * G_N_ELEMENTS (memory_sizes) + j));

			g_assert_true (arv_device_write_memory (arv_device, SCRATCH_ADDRESS, size, written, &error));
			g_assert_no_error (error);
			g_assert_true (fake_usb_camera_read_memory (device, SCRATCH_ADDRESS, size, in_camera));
			g_assert_cmpmem (in_camera, size, written, size);

			g_assert_true (arv_device_read_memory (arv_device, SCRATCH_ADDRESS, size, read, &error));
			g_assert_no_error (error);
			g_assert_cmpmem (read, size, written, size);

			g_free (written);
			g_free (in_camera);
			g_free (read);
		}

		fake_usb_set_transfer_hook (NULL, NULL);

		g_assert_cmpint (transfers.max_command, ==, max_transfers[i].largest_command);
		g_assert_cmpint (transfers.max_ack, ==, max_transfers[i].largest_ack);
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_INVALID_COMMANDS), ==, 0);

		g_clear_object (&camera);
		g_mutex_clear (&transfers.mutex);
	}
}

static void
memory_large_test (void)
{
	ControlTransfers transfers = { 0 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvDevice *arv_device;
	GError *error = NULL;
	guint32 size = 70000;
	guint8 *expected;
	guint8 *data;

	g_mutex_init (&transfers.mutex);

	/* Transfers larger than the 16 bit size fields of the packets can describe */
	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_max_transfers (device, 0x20000, 0x20000);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);

	fake_usb_set_transfer_hook (control_transfers_hook, &transfers);

	/* The camera's register map (64 KiB), then the start of its GenICam data */
	expected = g_malloc (size);
	data = g_malloc0 (size);
	g_assert_true (fake_usb_camera_read_memory (device, 0, size, expected));
	g_assert_true (arv_device_read_memory (arv_device, 0, size, data, &error));
	g_assert_no_error (error);
	g_assert_cmpmem (data, size, expected, size);

	/* No writable range of the camera is as large as one write block. A write that runs into the read-only GenICam
	 * data shows that the first block was well formed: the camera refuses it for write protection, not for an
	 * invalid header */
	fill_pattern (data, size, 1);
	g_assert_false (arv_device_write_memory (arv_device, 0x1f0, size, data, &error));
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR_WRITE_PROTECT);
	g_clear_error (&error);

	fake_usb_set_transfer_hook (NULL, NULL);

	/* The size field of a packet counts the bytes after its header, 65535 at most: the address of a write command
	 * and 65524 bytes of data (whole 32 bit words), or 65532 bytes of read data */
	g_assert_cmpint (transfers.max_command, ==, sizeof (ArvUvcpWriteMemoryCmd) + 65524);
	g_assert_cmpint (transfers.max_ack, ==, sizeof (ArvUvcpHeader) + 65532);

	g_free (expected);
	g_free (data);
	g_clear_object (&camera);
	g_mutex_clear (&transfers.mutex);
}

static void
memory_small_max_test (void)
{
	ControlTransfers transfers = { 0 };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvDevice *arv_device;
	GError *error = NULL;
	guint8 written[8];
	guint8 read[8] = { 0 };
	guint32 room;

	device = fakeuv_add_camera ("UV1");

	/* Acknowledges with no room for data, or for less than a 32 bit word, which would make blocks that end and
	 * start unaligned: reading the camera fails with an error */
	for (room = 0; room < 4; room++) {
		fake_usb_camera_set_max_transfers (device, 1024, sizeof (ArvUvcpHeader) + room);

		camera = fakeuv_open_camera (device, &error);
		g_assert_null (camera);
		g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR);
		g_clear_error (&error);

		g_assert_cmpuint (fake_usb_device_get_n_open_handles (device), ==, 0);
	}

	/* Room for one 32 bit word, also the size of a write acknowledge: the camera opens, and reads go 4 bytes at a
	 * time */
	fake_usb_camera_set_max_transfers (device, 1024, sizeof (ArvUvcpHeader) + 4);

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);

	g_mutex_init (&transfers.mutex);
	fake_usb_set_transfer_hook (control_transfers_hook, &transfers);

	fill_pattern (written, sizeof (written), 7);
	g_assert_true (arv_device_write_memory (arv_device, SCRATCH_ADDRESS, sizeof (written), written, &error));
	g_assert_no_error (error);
	g_assert_true (arv_device_read_memory (arv_device, SCRATCH_ADDRESS, sizeof (read), read, &error));
	g_assert_no_error (error);
	g_assert_cmpmem (read, sizeof (read), written, sizeof (written));

	fake_usb_set_transfer_hook (NULL, NULL);

	/* One write command, read acknowledges of 4 bytes */
	g_assert_cmpint (transfers.max_command, ==, sizeof (ArvUvcpWriteMemoryCmd) + sizeof (written));
	g_assert_cmpint (transfers.max_ack, ==, sizeof (ArvUvcpHeader) + 4);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_INVALID_COMMANDS), ==, 0);

	g_clear_object (&camera);
	g_mutex_clear (&transfers.mutex);
}

/* Acknowledges */

/* A camera may answer a read with a pending acknowledge (16 bytes), longer than the acknowledge of a read of 1 to 3
 * bytes. Read into a buffer of that size, it was cut off: its timeout was read past the end of the buffer, and the
 * rest of the packet started the next read (WinUSB partial reads), or the read overflowed (Linux) and the request for
 * more time was lost. The camera answers after the response time, within the time the pending acknowledge asks for. */

static void
pending_ack_small_read_test (void)
{
	static const FakeUsbPlatform platforms[] = { FAKE_USB_PLATFORM_WINDOWS, FAKE_USB_PLATFORM_LINUX };
	static const guint8 written[4] = { 0x0d, 0x0c, 0x0b, 0x0a };
	guint i, size;

	for (i = 0; i < G_N_ELEMENTS (platforms); i++) {
		FakeUsbDevice *device;
		ArvCamera *camera;
		ArvDevice *arv_device;
		GError *error = NULL;
		guint64 n_partial_reads, n_overflows;

		fakeuv_test_reset ();
		fake_usb_set_platform (platforms[i]);

		device = fakeuv_add_camera ("UV1");
		camera = fakeuv_open_camera (device, &error);
		g_assert_no_error (error);
		arv_device = arv_camera_get_device (camera);

		g_assert_true (arv_device_write_memory (arv_device, SCRATCH_ADDRESS, sizeof (written), written,
							&error));
		g_assert_no_error (error);

		n_partial_reads = fake_usb_device_get_stat (device, FAKE_USB_STAT_PARTIAL_READS);
		n_overflows = fake_usb_device_get_stat (device, FAKE_USB_STAT_OVERFLOWS);

		for (size = 1; size < 4; size++) {
			guint8 read[4] = { 0 };
			guint64 n_commands = fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS);
			guint64 n_pending_acks = fake_usb_device_get_stat (device, FAKE_USB_STAT_PENDING_ACKS);

			fake_usb_camera_set_pending_acks (device, 1, 1000,
							  FAKE_USB_CAMERA_DEFAULT_RESPONSE_TIME_MS * 2);

			g_assert_true (arv_device_read_memory (arv_device, SCRATCH_ADDRESS, size, read, &error));
			g_assert_no_error (error);
			g_assert_cmpmem (read, size, written, size);

			/* Received whole, its timeout applied: a single command */
			g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_PENDING_ACKS), ==,
					  n_pending_acks + 1);
			g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS), ==,
					  n_commands + 1);
		}

		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_PARTIAL_READS), ==, n_partial_reads);
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_OVERFLOWS), ==, n_overflows);

		g_clear_object (&camera);
	}
}

/* Acknowledges cut short are not taken for answers: shorter than a header, or than the data of a read. The camera is
 * asked again once the response time is over. A write acknowledge needs its header only. */

static void
short_ack_test (void)
{
	static const gsize missing_bytes[] = { 2, sizeof (ArvUvcpHeader) - 4, G_MAXSIZE };
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvDevice *arv_device;
	GError *error = NULL;
	guint64 n_commands;
	guint32 value;
	guint i;

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);

	for (i = 0; i < G_N_ELEMENTS (missing_bytes); i++) {
		guint32 expected = 0x12345678 + i;

		g_assert_true (arv_device_write_register (arv_device, SCRATCH_ADDRESS, expected, &error));
		g_assert_no_error (error);

		n_commands = fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS);
		fake_usb_camera_truncate_acks (device, 1, missing_bytes[i]);

		value = 0;
		g_assert_true (arv_device_read_register (arv_device, SCRATCH_ADDRESS, &value, &error));
		g_assert_no_error (error);
		g_assert_cmphex (value, ==, expected);
		g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS), ==, n_commands + 2);
	}

	n_commands = fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS);
	fake_usb_camera_truncate_acks (device, 1, sizeof (ArvUvcpWriteMemoryAckInfos));
	g_assert_true (arv_device_write_register (arv_device, SCRATCH_ADDRESS, 0x87654321, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_COMMANDS), ==, n_commands + 1);
	g_assert_true (arv_device_read_register (arv_device, SCRATCH_ADDRESS, &value, &error));
	g_assert_no_error (error);
	g_assert_cmphex (value, ==, 0x87654321);

	g_clear_object (&camera);
}

/* Kernel driver auto-detach */

static void
auto_detach_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	device = fakeuv_add_camera ("UV1");

	/* Not attempted without the capability (Windows) */
	fake_usb_set_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER, FALSE);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER), ==, 0);
	g_assert_false (fake_usb_device_get_auto_detach_kernel_driver (device));
	g_clear_object (&camera);

	/* Enabled with it (Linux, macOS) */
	fake_usb_set_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER, TRUE);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER), ==, 1);
	g_assert_true (fake_usb_device_get_auto_detach_kernel_driver (device));
	g_clear_object (&camera);

	/* A failure does not prevent the open */
	fake_usb_device_inject_error (device, FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER, -1, LIBUSB_ERROR_IO, 1);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_device_get_call_count (device, FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER), ==, 2);
	g_clear_object (&camera);
}

/* Hotplug */

static void
hotplug_test (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	device = fakeuv_add_camera ("UV1");

	/* Without the capability (the default Windows libusb build): no registration, no deregistration */
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, FALSE);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_clear_object (&camera);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK), ==, 0);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK), ==, 0);

	/* With it: registered at open, deregistered at close (the unplug tests of fakeuv.c use it) */
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, TRUE);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK), ==, 1);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK), ==, 0);
	g_clear_object (&camera);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK), ==, 1);

	/* A failed registration leaves the camera usable, and nothing to deregister */
	fake_usb_inject_error (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK, LIBUSB_ERROR_NO_MEM, 1);
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (arv_camera_get_vendor_name (camera, &error), ==, "Aravis");
	g_assert_no_error (error);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK), ==, 2);
	g_clear_object (&camera);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK), ==, 1);

	/* Nor does an open that fails before the registration */
	fake_usb_device_inject_error (device, FAKE_USB_CALL_CLAIM_INTERFACE, FAKE_USB_CAMERA_DATA_INTERFACE,
				      LIBUSB_ERROR_BUSY, 1);
	camera = fakeuv_open_camera (device, &error);
	g_assert_null (camera);
	g_assert_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR);
	g_clear_error (&error);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK), ==, 2);
	g_assert_cmpuint (fake_usb_get_call_count (FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK), ==, 1);
}

/* Log checks. The scenarios run in a subprocess (registered under /fakeuvdevice/subprocess/), whose stderr carries
 * Aravis's log. */

static void
enable_debug_output (const char *categories)
{
	arv_debug_enable (categories);
}

static FakeUsbDevice *
setup_logged_camera (FakeUsbPlatform platform)
{
	gboolean windows = platform == FAKE_USB_PLATFORM_WINDOWS;

	enable_debug_output ("device:2");

	fake_usb_set_platform (platform);
	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, !windows);
	fake_usb_set_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER, !windows);
	fake_usb_set_version (FAKE_LIBUSB_VERSION);

	return fakeuv_add_camera ("UV1");
}

static void
open_and_close_camera (FakeUsbDevice *device)
{
	ArvCamera *camera;
	GError *error = NULL;

	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_true (ARV_IS_CAMERA (camera));
	g_clear_object (&camera);
}

static void
subprocess_windows (void)
{
	open_and_close_camera (setup_logged_camera (FAKE_USB_PLATFORM_WINDOWS));
}

static void
subprocess_linux (void)
{
	open_and_close_camera (setup_logged_camera (FAKE_USB_PLATFORM_LINUX));
}

static void
subprocess_hotplug_failure (void)
{
	FakeUsbDevice *device = setup_logged_camera (FAKE_USB_PLATFORM_LINUX);

	fake_usb_inject_error (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK, LIBUSB_ERROR_NO_MEM, 1);
	open_and_close_camera (device);
}

static void
subprocess_auto_detach_failure (void)
{
	FakeUsbDevice *device = setup_logged_camera (FAKE_USB_PLATFORM_LINUX);

	fake_usb_device_inject_error (device, FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER, -1, LIBUSB_ERROR_IO, 1);
	open_and_close_camera (device);
}

static void
subprocess_pending_acks (void)
{
	FakeUsbDevice *device;

	enable_debug_output ("device:3");

	/* The camera asks for more time on the first commands, then answers */
	device = fakeuv_add_camera ("UV1");
	fake_usb_camera_set_pending_acks (device, 5, 100, 5);

	open_and_close_camera (device);

	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_PENDING_ACKS), ==, 5);
}

static void
subprocess_ack_zero_length_packets (void)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	ArvDevice *arv_device;
	GError *error = NULL;
	guint8 data[1012];
	guint32 value = 0;

	enable_debug_output ("device:2");

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	arv_device = arv_camera_get_device (camera);

	/* The acknowledge of a 1012 byte read is 1024 bytes, a whole packet, which the camera follows with a zero
	 * length packet: the next command reads it first */
	fake_usb_camera_set_ack_zero_length_packets (device, TRUE);
	g_assert_true (arv_device_read_memory (arv_device, ARV_FAKE_CAMERA_MEMORY_SIZE, sizeof (data), data, &error));
	g_assert_no_error (error);
	g_assert_true (arv_device_read_register (arv_device, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (value, >, 0);
	g_assert_cmpuint (fake_usb_device_get_stat (device, FAKE_USB_STAT_ZERO_LENGTH_PACKETS), ==, 1);

	g_clear_object (&camera);
}

/* An acquisition of a few frames in async mode, logged, with ARV_UV_RAW_IO set to @raw_io (NULL: unset) */

static void
acquire_logged_frames (const char *raw_io)
{
	FakeUsbDevice *device;
	ArvCamera *camera;
	GError *error = NULL;

	fake_usb_set_platform (FAKE_USB_PLATFORM_WINDOWS);
	if (raw_io != NULL)
		g_setenv ("ARV_UV_RAW_IO", raw_io, TRUE);
	else
		g_unsetenv ("ARV_UV_RAW_IO");

	device = fakeuv_add_camera ("UV1");
	camera = fakeuv_open_camera (device, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (fakeuv_acquire_frames (camera, device, ARV_UV_USB_MODE_ASYNC, 4, 3, FAKEUV_TIMEOUT_MS), ==,
			  3);
	g_clear_object (&camera);
}

static void
subprocess_raw_io_default (void)
{
	enable_debug_output ("stream:2,device:2");
	acquire_logged_frames (NULL);
}

static void
subprocess_raw_io_on (void)
{
	enable_debug_output ("stream:2,device:2");
	acquire_logged_frames ("1");
}

#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
static void
subprocess_raw_io_older_libusb (void)
{
	enable_debug_output ("stream:2,device:2");

	/* An older libusb-1.0.dll lacks them, as kernel32.dll does */
	g_assert_false (arv_uv_libusb_look_functions_up (GetModuleHandleW (L"kernel32.dll"), NULL));

	acquire_logged_frames ("1");
}

static void
subprocess_raw_io_static_libusb (void)
{
	HMODULE module = GetModuleHandleW (L"kernel32.dll");

	enable_debug_output ("stream:2,device:2");

	/* A program or DLL that holds Aravis and a libusb linked statically exports nothing, as kernel32.dll does */
	g_assert_false (arv_uv_libusb_look_functions_up (module, module));

	acquire_logged_frames ("1");
}
#endif

static void
run_subprocess (const char *path)
{
	/* Longer than the waits of the subprocess, which then fails by itself */
	g_test_trap_subprocess (path, (guint64) fakeuv_timeout_ms (2 * FAKEUV_TIMEOUT_MS) * 1000, 0);
	g_test_trap_assert_passed ();
}

static void
pending_ack_log_test (void)
{
	/* Pending acknowledges are not taken for unexpected answers */
	run_subprocess ("/fakeuvdevice/subprocess/pending-acks");
	g_test_trap_assert_stderr ("*pending ack timeout*");
	g_test_trap_assert_stderr_unmatched ("*unexpected answer*");
}

static void
ack_zero_length_packet_log_test (void)
{
	/* No answer, rather than an unexpected one, parsed from what the buffer held before */
	run_subprocess ("/fakeuvdevice/subprocess/ack-zero-length-packets");
	g_test_trap_assert_stderr ("*0 byte(s) received, no acknowledge*");
	g_test_trap_assert_stderr_unmatched ("*unexpected answer*");
}

static void
auto_detach_log_test (void)
{
	/* No warning, whether libusb can detach kernel drivers or not */
	run_subprocess ("/fakeuvdevice/subprocess/windows");
	g_test_trap_assert_stderr ("*Using data endpoint*");
	g_test_trap_assert_stderr_unmatched ("*auto kernel detach*");

	run_subprocess ("/fakeuvdevice/subprocess/linux");
	g_test_trap_assert_stderr ("*Using data endpoint*");
	g_test_trap_assert_stderr_unmatched ("*auto kernel detach*");

	/* Unless it fails */
	run_subprocess ("/fakeuvdevice/subprocess/auto-detach-failure");
	g_test_trap_assert_stderr ("*Failed to set auto kernel detach feature*LIBUSB_ERROR_IO*");
}

static void
hotplug_log_test (void)
{
	/* The libusb version, then whether hotplug is available */
	run_subprocess ("/fakeuvdevice/subprocess/windows");
	g_test_trap_assert_stderr ("*" FAKE_LIBUSB_VERSION_LOG "*Hotplug unavailable: not supported by this libusb*");

	run_subprocess ("/fakeuvdevice/subprocess/linux");
	g_test_trap_assert_stderr ("*" FAKE_LIBUSB_VERSION_LOG "*Hotplug enabled*");

	run_subprocess ("/fakeuvdevice/subprocess/hotplug-failure");
	g_test_trap_assert_stderr ("*" FAKE_LIBUSB_VERSION_LOG
				   "*Hotplug unavailable: failed to register the callback (LIBUSB_ERROR_NO_MEM)*");
}

/* RAW_IO is off by default; where it is available, the log of the acquisition start says so, and how to turn it on.
 * On Windows, with libusb as a DLL, the log tells which libusb has the functions RAW_IO needs, looked up at run time,
 * and why they are missing: an older libusb, or a libusb linked statically into the module of Aravis, for which the
 * build did not call them directly. */

static void
raw_io_log_test (void)
{
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	run_subprocess ("/fakeuvdevice/subprocess/raw-io-default");
	g_test_trap_assert_stderr ("*packet size 1024 bytes, RAW_IO off (default, ARV_UV_RAW_IO=1 turns it on)*");

	run_subprocess ("/fakeuvdevice/subprocess/raw-io-on");
	g_test_trap_assert_stderr ("*packet size 1024 bytes, RAW_IO on (maximum 2097152 bytes)*");
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
	/* In the test program, which the fake libusb is part of */
	g_test_trap_assert_stderr ("*fakeuvdevice*: RAW_IO functions found, device string function found*");

	run_subprocess ("/fakeuvdevice/subprocess/raw-io-older-libusb");
	g_test_trap_assert_stderr ("*: RAW_IO functions missing (libusb older than 1.0.30), device string function "
				   "missing (libusb older than 1.0.30)*RAW_IO off (not available)*");

	run_subprocess ("/fakeuvdevice/subprocess/raw-io-static-libusb");
	g_test_trap_assert_stderr ("*: RAW_IO functions not exported (libusb linked statically, Aravis built for a "
				   "libusb DLL), device string function not exported (libusb linked statically, "
				   "Aravis built for a libusb DLL)*RAW_IO off (not available)*");
	g_test_trap_assert_stderr_unmatched ("*older than 1.0.30*");
#endif
#else
	/* Not without the headers of libusb 1.0.30 */
	run_subprocess ("/fakeuvdevice/subprocess/raw-io-on");
	g_test_trap_assert_stderr ("*packet size 1024 bytes, RAW_IO off (not available)*");
#endif
}

int
main (int argc, char *argv[])
{
	fakeuv_test_init (&argc, &argv);

	fakeuv_test_add ("/fakeuvdevice/discovery-config-descriptor", discovery_config_descriptor_test);
	fakeuv_test_add ("/fakeuvdevice/open-config-descriptor", open_config_descriptor_test);
	fakeuv_test_add ("/fakeuvdevice/no-data-endpoint", no_data_endpoint_test);
	fakeuv_test_add ("/fakeuvdevice/open-error", open_error_test);
	fakeuv_test_add ("/fakeuvdevice/memory-chunks", memory_chunks_test);
	fakeuv_test_add ("/fakeuvdevice/memory-large", memory_large_test);
	fakeuv_test_add ("/fakeuvdevice/memory-small-max", memory_small_max_test);
	fakeuv_test_add ("/fakeuvdevice/pending-ack-log", pending_ack_log_test);
	fakeuv_test_add ("/fakeuvdevice/pending-ack-small-read", pending_ack_small_read_test);
	fakeuv_test_add ("/fakeuvdevice/short-ack", short_ack_test);
	fakeuv_test_add ("/fakeuvdevice/ack-zero-length-packet-log", ack_zero_length_packet_log_test);
	fakeuv_test_add ("/fakeuvdevice/auto-detach", auto_detach_test);
	fakeuv_test_add ("/fakeuvdevice/auto-detach-log", auto_detach_log_test);
	fakeuv_test_add ("/fakeuvdevice/hotplug", hotplug_test);
	fakeuv_test_add ("/fakeuvdevice/hotplug-log", hotplug_log_test);
	fakeuv_test_add ("/fakeuvdevice/raw-io-log", raw_io_log_test);

	/* Only run by the log tests, through g_test_trap_subprocess () */
	fakeuv_test_add ("/fakeuvdevice/subprocess/windows", subprocess_windows);
	fakeuv_test_add ("/fakeuvdevice/subprocess/linux", subprocess_linux);
	fakeuv_test_add ("/fakeuvdevice/subprocess/hotplug-failure", subprocess_hotplug_failure);
	fakeuv_test_add ("/fakeuvdevice/subprocess/auto-detach-failure", subprocess_auto_detach_failure);
	fakeuv_test_add ("/fakeuvdevice/subprocess/pending-acks", subprocess_pending_acks);
	fakeuv_test_add ("/fakeuvdevice/subprocess/ack-zero-length-packets", subprocess_ack_zero_length_packets);
	fakeuv_test_add ("/fakeuvdevice/subprocess/raw-io-default", subprocess_raw_io_default);
	fakeuv_test_add ("/fakeuvdevice/subprocess/raw-io-on", subprocess_raw_io_on);
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
	fakeuv_test_add ("/fakeuvdevice/subprocess/raw-io-older-libusb", subprocess_raw_io_older_libusb);
	fakeuv_test_add ("/fakeuvdevice/subprocess/raw-io-static-libusb", subprocess_raw_io_static_libusb);
#endif

	return fakeuv_test_run ();
}
