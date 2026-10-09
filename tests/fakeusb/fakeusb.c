/* SPDX-License-Identifier:Unlicense */

/* Fake libusb-1.0, see fakeusb.h. This file implements the libusb entry points, contexts, device objects, handles,
 * transfers and the event loop. The USB3 Vision camera emulation lives in fakeusbcamera.c. */

/* On Windows, Aravis looks the functions libusb 1.0.30 added up at run time, in the module that holds libusb: here, the
 * test program, to which the fake is linked statically. The program exports them, as libusb-1.0.dll does. A
 * declaration that adds dllexport to an earlier one is a warning: their declarations in libusb.h are renamed. With
 * _WIN32, as GLib, which defines G_OS_WIN32, is not included yet. An Aravis built for a static libusb
 * (ARV_UV_LIBUSB_STATIC) calls them directly: the fake is a static libusb then, and exports nothing. */

#if defined (_WIN32) && !defined (ARV_UV_LIBUSB_STATIC)
#define FAKE_USB_EXPORT_LIBUSB_1_0_30	1
#endif

#ifdef FAKE_USB_EXPORT_LIBUSB_1_0_30
#define libusb_endpoint_supports_raw_io		fake_usb_unexported_endpoint_supports_raw_io
#define libusb_endpoint_set_raw_io		fake_usb_unexported_endpoint_set_raw_io
#define libusb_get_max_raw_io_transfer_size	fake_usb_unexported_get_max_raw_io_transfer_size
#define libusb_get_device_string		fake_usb_unexported_get_device_string
#endif

#include "fakeusbprivate.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef FAKE_USB_EXPORT_LIBUSB_1_0_30
#undef libusb_endpoint_supports_raw_io
#undef libusb_endpoint_set_raw_io
#undef libusb_get_max_raw_io_transfer_size
#undef libusb_get_device_string

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
__declspec(dllexport) int LIBUSB_CALL libusb_endpoint_supports_raw_io (libusb_device_handle *dev_handle,
								       uint8_t endpoint);
__declspec(dllexport) int LIBUSB_CALL libusb_endpoint_set_raw_io (libusb_device_handle *dev_handle,
								  uint8_t endpoint, int enable);
__declspec(dllexport) int LIBUSB_CALL libusb_get_max_raw_io_transfer_size (libusb_device_handle *dev_handle,
									   uint8_t endpoint);
__declspec(dllexport) int LIBUSB_CALL libusb_get_device_string (libusb_device *dev,
								enum libusb_device_string_type string_type,
								char *data, int length);
#endif
#endif

#define FAKE_USB_MAX_WAIT_US			(500 * G_TIME_SPAN_MILLISECOND)
#define FAKE_USB_DEFAULT_EVENT_TIMEOUT		(60 * G_TIME_SPAN_SECOND)
#define FAKE_USB_N_CAPABILITIES			4
#define FAKE_USB_WINDOWS_HOTPLUG_DELAY_MS	10	/* HOTPLUG_DEBOUNCE_MS, libusb's os/windows_hotplug.c */
#define FAKE_USB_STRING_TIMEOUT_MS		1000	/* libusb_get_string_descriptor (), libusb.h */
#define FAKE_USB_STRING_BUFFER_SIZE		255	/* union usbi_string_desc_buf, libusb's libusbi.h */

/* libusb opaque types */

typedef struct {
	int handle;
	int events;
	int flags;
	int vendor_id;
	int product_id;
	int dev_class;
	libusb_hotplug_callback_fn callback;
	void *user_data;
	gboolean deregistered;
} FakeUsbHotplugCallback;

typedef struct {
	libusb_hotplug_event event;
	libusb_device *device;
	gint64 due;
} FakeUsbHotplugMessage;

struct libusb_context {
	GList *devices;			/* live libusb_device objects of this context */
	GList *handles;			/* open libusb_device_handle */
	GList *hotplug_callbacks;	/* FakeUsbHotplugCallback */
	int next_hotplug_handle;
	guint hotplug_dispatch_depth;
	GQueue completed;		/* FakeUsbTransferPriv waiting for their callback */
	GQueue dispatching;		/* FakeUsbTransferPriv taken by the current dispatch round */
	GQueue hotplug_messages;	/* FakeUsbHotplugMessage ready for dispatch */
	GQueue delayed_hotplug_messages;	/* FakeUsbHotplugMessage not due yet, in order */
	guint interrupt_serial;
	GThread *event_owner;		/* the thread holding the event lock */
	guint event_lock_releases;
	guint device_close;		/* libusb_close () calls waiting for the event lock */
};

struct libusb_device {
	libusb_context *ctx;
	FakeUsbDevice *fake;
	guint generation;
	int ref_count;
	gboolean context_ref;		/* the context keeps it while attached (hotplug capable libusb) */
	char *strings[3];		/* libusb_get_device_string () cache */
};

struct libusb_device_handle {
	libusb_device *dev;
	libusb_context *ctx;
};

typedef enum {
	FAKE_USB_TRANSFER_IDLE,
	FAKE_USB_TRANSFER_PENDING,
	FAKE_USB_TRANSFER_COMPLETED,
	FAKE_USB_TRANSFER_DISPATCHING,
	FAKE_USB_TRANSFER_ORPHANED	/* its handle was closed while it was in flight */
} FakeUsbTransferState;

/* Private part of a transfer, allocated in front of the public struct libusb_transfer, as libusb does */

typedef struct {
	FakeUsbTransferState state;
	gboolean is_synchronous;
	int synchronous_done;
	gboolean counted;
	libusb_device_handle *handle;
	FakeUsbDevice *device;		/* holds a reference */
	libusb_context *ctx;
	GQueue *queue;			/* while pending: the endpoint or control queue it waits in */
	gint64 deadline;
	gint64 due;			/* control transfers: when the device answers */
	gsize transferred;
	gsize charged;			/* bytes counted against the submit memory limit */
	int auto_claimed_interface;	/* Windows: interface claimed by libusb for this control transfer, or -1 */
	struct libusb_transfer *transfer;
} FakeUsbTransferPriv;

#define FAKE_USB_TRANSFER_PRIV_SIZE	((sizeof (FakeUsbTransferPriv) + 15) & ~((gsize) 15))

/* Global state, protected by fake_mutex */

static GMutex fake_mutex;
static GCond fake_cond;

static GPtrArray *fake_devices = NULL;
static GList *fake_contexts = NULL;
static libusb_context *fake_default_context = NULL;
static guint fake_default_context_refs = 0;

static FakeUsbPlatform fake_platform = FAKE_USB_PLATFORM_HOST;
static int fake_capabilities[FAKE_USB_N_CAPABILITIES] = { -1, -1, -1, -1 };
static int fake_dev_mem_supported = -1;
static GHashTable *fake_dev_mem_blocks = NULL;
static const struct libusb_version *fake_version = NULL;
static GPtrArray *fake_versions = NULL;
static GList *fake_global_injected_errors = NULL;
static int fake_hotplug_delay_ms = -1;
static FakeUsbHotplugOrder fake_hotplug_order = FAKE_USB_HOTPLUG_ORDER_PLATFORM;
static gsize fake_submit_memory_limit = 0;
static int fake_submit_memory_error = 0;
static gsize fake_submit_memory_pending = 0;
static guint fake_time_scale = 1;

static guint64 fake_calls[FAKE_USB_N_CALLS];
static guint64 fake_total_stats[FAKE_USB_N_STATS];
static char *fake_last_misuse = NULL;
static guint fake_n_open_handles = 0;
static guint fake_n_transfers = 0;
static guint8 fake_next_address = 1;

static FakeUsbHandleEventsHook fake_handle_events_hook = NULL;
static gpointer fake_handle_events_hook_data = NULL;
static FakeUsbTransferHook fake_transfer_hook = NULL;
static gpointer fake_transfer_hook_data = NULL;

static const char *fake_call_names[] = {
	"libusb_init",
	"libusb_exit",
	"libusb_get_device_list",
	"libusb_free_device_list",
	"libusb_get_device_descriptor",
	"libusb_get_config_descriptor",
	"libusb_free_config_descriptor",
	"libusb_get_bus_number",
	"libusb_get_device_address",
	"libusb_get_port_number",
	"libusb_get_device_speed",
	"libusb_get_max_packet_size",
	"libusb_get_device_string",
	"libusb_open",
	"libusb_close",
	"libusb_get_device",
	"libusb_claim_interface",
	"libusb_release_interface",
	"libusb_set_auto_detach_kernel_driver",
	"libusb_get_string_descriptor_ascii",
	"libusb_control_transfer",
	"libusb_bulk_transfer",
	"libusb_interrupt_transfer",
	"libusb_clear_halt",
	"libusb_reset_device",
	"libusb_alloc_transfer",
	"libusb_free_transfer",
	"libusb_submit_transfer",
	"libusb_cancel_transfer",
	"libusb_handle_events",
	"libusb_interrupt_event_handler",
	"libusb_hotplug_register_callback",
	"libusb_hotplug_deregister_callback",
	"libusb_has_capability",
	"libusb_get_version",
	"libusb_error_name",
	"libusb_strerror",
	"libusb_set_option",
	"libusb_dev_mem_alloc",
	"libusb_dev_mem_free",
	"libusb_endpoint_supports_raw_io",
	"libusb_endpoint_set_raw_io",
	"libusb_get_max_raw_io_transfer_size"
};

G_STATIC_ASSERT (G_N_ELEMENTS (fake_call_names) == FAKE_USB_N_CALLS);

/* Debug output, enabled by the FAKE_USB_DEBUG environment variable */

void
fake_usb_debug (const char *format, ...)
{
	static int enabled = -1;
	va_list args;
	char *message;

	if (enabled < 0)
		enabled = g_getenv ("FAKE_USB_DEBUG") != NULL ? 1 : 0;
	if (enabled == 0)
		return;

	va_start (args, format);
	message = g_strdup_vprintf (format, args);
	va_end (args);

	g_printerr ("[fakeusb] %s\n", message);
	g_free (message);
}

/* Locking */

void
fake_usb_lock (void)
{
	g_mutex_lock (&fake_mutex);
}

void
fake_usb_unlock (void)
{
	g_mutex_unlock (&fake_mutex);
}

void
fake_usb_signal_locked (void)
{
	g_cond_broadcast (&fake_cond);
}

static void
_wait_locked (gint64 wakeup)
{
	g_cond_wait_until (&fake_cond, &fake_mutex, wakeup);
}

void
fake_usb_queue_clear_full (GQueue *queue, GDestroyNotify free_func)
{
	gpointer data;

	while ((data = g_queue_pop_head (queue)) != NULL)
		free_func (data);
}

gint64
fake_usb_scaled_us_locked (guint64 us)
{
	if (us > (guint64) (G_MAXINT64 / 4) / fake_time_scale)
		return G_MAXINT64 / 4;

	return (gint64) (us * fake_time_scale);
}

guint
fake_usb_get_time_scale_locked (void)
{
	return fake_time_scale;
}

/* Counters and statistics */

static void
_count_locked (FakeUsbCall call, FakeUsbDevice *device)
{
	fake_calls[call]++;
	if (device != NULL)
		device->calls[call]++;
}

static void
_count (FakeUsbCall call, FakeUsbDevice *device)
{
	fake_usb_lock ();
	_count_locked (call, device);
	fake_usb_unlock ();
}

void
fake_usb_device_stat_add_locked (FakeUsbDevice *device, FakeUsbStat stat, guint64 n)
{
	if (device != NULL)
		device->stats[stat] += n;
	fake_total_stats[stat] += n;
}

static void
_stat_max_locked (FakeUsbDevice *device, FakeUsbStat stat, guint64 value)
{
	device->stats[stat] = MAX (device->stats[stat], value);
	fake_total_stats[stat] = MAX (fake_total_stats[stat], value);
}

static void _misuse_locked (FakeUsbDevice *device, const char *format, ...) G_GNUC_PRINTF (2, 3);

/* Something libusb documents as an application bug */

static void
_misuse_locked (FakeUsbDevice *device, const char *format, ...)
{
	va_list args;

	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_API_MISUSE, 1);

	g_free (fake_last_misuse);
	va_start (args, format);
	fake_last_misuse = g_strdup_vprintf (format, args);
	va_end (args);

	fake_usb_debug ("API misuse: %s", fake_last_misuse);
}

/* Platform dependent behaviour */

static int
_capability_index (guint32 capability)
{
	switch (capability) {
		case LIBUSB_CAP_HAS_CAPABILITY:
			return 0;
		case LIBUSB_CAP_HAS_HOTPLUG:
			return 1;
		case LIBUSB_CAP_HAS_HID_ACCESS:
			return 2;
		case LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER:
			return 3;
		default:
			return -1;
	}
}

static gboolean
_has_capability_locked (guint32 capability)
{
	int index = _capability_index (capability);

	if (index < 0)
		return FALSE;
	if (fake_capabilities[index] >= 0)
		return fake_capabilities[index] != 0;

	switch (capability) {
		case LIBUSB_CAP_HAS_CAPABILITY:
			return TRUE;
		case LIBUSB_CAP_HAS_HOTPLUG:
		case LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER:
			return fake_platform != FAKE_USB_PLATFORM_WINDOWS;
		case LIBUSB_CAP_HAS_HID_ACCESS:
			return fake_platform != FAKE_USB_PLATFORM_LINUX;
		default:
			return FALSE;
	}
}

static gboolean
_dev_mem_supported_locked (void)
{
	if (fake_dev_mem_supported >= 0)
		return fake_dev_mem_supported != 0;

	return fake_platform == FAKE_USB_PLATFORM_LINUX;
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
static gboolean
_raw_io_supported_locked (FakeUsbDevice *device)
{
	if (device->raw_io_supported >= 0)
		return device->raw_io_supported != 0;

	return fake_platform == FAKE_USB_PLATFORM_WINDOWS;
}
#endif

/* WinUSB pipe policies libusb sets on every endpoint of a claimed interface (winusbx_configure_endpoints ()) */

static gboolean
_partial_reads_locked (FakeUsbDevice *device, FakeUsbEndpointState *state)
{
	if (state->raw_io)
		return FALSE;
	if (device->allow_partial_reads >= 0)
		return device->allow_partial_reads != 0;

	return fake_platform == FAKE_USB_PLATFORM_WINDOWS;
}

static gboolean
_auto_clear_stall_locked (FakeUsbDevice *device)
{
	if (device->auto_clear_stall >= 0)
		return device->auto_clear_stall != 0;

	return fake_platform == FAKE_USB_PLATFORM_WINDOWS;
}

static enum libusb_transfer_status
_unplug_status_locked (FakeUsbDevice *device)
{
	if (device->unplug_errors_set)
		return device->unplug_status;

	return fake_platform == FAKE_USB_PLATFORM_WINDOWS ? LIBUSB_TRANSFER_ERROR : LIBUSB_TRANSFER_NO_DEVICE;
}

static int
_unplug_submit_error_locked (FakeUsbDevice *device)
{
	if (device->unplug_errors_set)
		return device->unplug_submit_error;

	return fake_platform == FAKE_USB_PLATFORM_WINDOWS ? LIBUSB_ERROR_IO : LIBUSB_ERROR_NO_DEVICE;
}

static int
_unplug_synchronous_error_locked (FakeUsbDevice *device)
{
	if (device->unplug_errors_set)
		return device->unplug_synchronous_error;

	return fake_platform == FAKE_USB_PLATFORM_WINDOWS ? LIBUSB_ERROR_IO : LIBUSB_ERROR_NO_DEVICE;
}

static gboolean
_hotplug_first_locked (void)
{
	switch (fake_hotplug_order) {
		case FAKE_USB_HOTPLUG_ORDER_TRANSFERS_FIRST:
			return FALSE;
		case FAKE_USB_HOTPLUG_ORDER_HOTPLUG_FIRST:
			return TRUE;
		default:
			/* Linux libusb handles its event trigger (hotplug messages) before reaping URBs */
			return fake_platform == FAKE_USB_PLATFORM_LINUX;
	}
}

static gint64
_hotplug_delay_us_locked (void)
{
	int delay_ms = fake_hotplug_delay_ms;

	if (delay_ms < 0)
		delay_ms = fake_platform == FAKE_USB_PLATFORM_WINDOWS ? FAKE_USB_WINDOWS_HOTPLUG_DELAY_MS : 0;

	return fake_usb_scaled_us_locked ((guint64) delay_ms * 1000);
}

static const struct libusb_version *
_default_version (void)
{
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
	static const struct libusb_version version = { 1, 0, 30, 0, "", "fakeusb" };
#elif defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010B
	static const struct libusb_version version = { 1, 0, 29, 0, "", "fakeusb" };
#elif defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010A
	static const struct libusb_version version = { 1, 0, 27, 0, "", "fakeusb" };
#elif defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000109
	static const struct libusb_version version = { 1, 0, 25, 0, "", "fakeusb" };
#elif defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000108
	static const struct libusb_version version = { 1, 0, 24, 0, "", "fakeusb" };
#elif defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000107
	static const struct libusb_version version = { 1, 0, 23, 0, "", "fakeusb" };
#elif defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000106
	static const struct libusb_version version = { 1, 0, 22, 0, "", "fakeusb" };
#else
	static const struct libusb_version version = { 1, 0, 21, 0, "", "fakeusb" };
#endif

	return &version;
}

/* Injected errors */

static int
_take_injected_error_locked (GList **list, FakeUsbCall call, int target)
{
	GList *iter;

	for (iter = *list; iter != NULL; iter = iter->next) {
		FakeUsbInjectedError *injection = iter->data;
		int error;

		if (injection->call != call)
			continue;
		if (injection->target >= 0 && injection->target != target)
			continue;

		error = injection->error;
		if (injection->count > 0) {
			injection->count--;
			if (injection->count == 0) {
				*list = g_list_delete_link (*list, iter);
				g_free (injection);
			}
		}

		return error;
	}

	return 0;
}

static void
_add_injected_error_locked (GList **list, FakeUsbCall call, int target, int error, int count)
{
	GList *iter, *next;

	/* A new injection replaces the previous one for the same call and target */
	for (iter = *list; iter != NULL; iter = next) {
		FakeUsbInjectedError *injection = iter->data;

		next = iter->next;
		if (injection->call == call && injection->target == target) {
			*list = g_list_delete_link (*list, iter);
			g_free (injection);
		}
	}

	if (error != 0 && count != 0) {
		FakeUsbInjectedError *injection = g_new0 (FakeUsbInjectedError, 1);

		injection->call = call;
		injection->target = target;
		injection->error = error;
		injection->count = count;
		*list = g_list_append (*list, injection);
	}
}

/* Fake devices */

static guint
_endpoint_slot (guint8 endpoint)
{
	return (endpoint & 0x0f) | ((endpoint & LIBUSB_ENDPOINT_IN) != 0 ? 0x10 : 0x00);
}

static guint8
_slot_endpoint (guint slot)
{
	return (slot & 0x0f) | ((slot & 0x10) != 0 ? LIBUSB_ENDPOINT_IN : 0x00);
}

static guint8
_next_address_locked (void)
{
	guint8 address = fake_next_address;

	fake_next_address = fake_next_address < 127 ? fake_next_address + 1 : 1;

	return address;
}

static void
_interface_template_free (gpointer data)
{
	FakeUsbInterfaceTemplate *template = data;

	g_byte_array_unref (template->extra);
	g_array_unref (template->endpoints);
	g_free (template);
}

static void
_segment_free (gpointer data)
{
	FakeUsbSegment *segment = data;

	g_bytes_unref (segment->bytes);
	g_free (segment);
}

static FakeUsbEndpointTemplate *
_find_endpoint_template_locked (FakeUsbDevice *device, guint8 endpoint, guint8 *interface_number)
{
	guint i, j;

	for (i = 0; i < device->interfaces->len; i++) {
		FakeUsbInterfaceTemplate *template = g_ptr_array_index (device->interfaces, i);

		for (j = 0; j < template->endpoints->len; j++) {
			FakeUsbEndpointTemplate *endpoint_template;

			endpoint_template = &g_array_index (template->endpoints, FakeUsbEndpointTemplate, j);
			if (endpoint_template->address == endpoint) {
				if (interface_number != NULL)
					*interface_number = template->number;
				return endpoint_template;
			}
		}
	}

	return NULL;
}

static FakeUsbInterfaceTemplate *
_find_interface_template_locked (FakeUsbDevice *device, int interface_number)
{
	guint i;

	for (i = 0; i < device->interfaces->len; i++) {
		FakeUsbInterfaceTemplate *template = g_ptr_array_index (device->interfaces, i);

		if (template->number == interface_number)
			return template;
	}

	return NULL;
}

FakeUsbEndpointState *
fake_usb_device_get_endpoint_locked (FakeUsbDevice *device, guint8 endpoint)
{
	return &device->endpoints[_endpoint_slot (endpoint)];
}

guint16
fake_usb_device_get_max_packet_size_locked (FakeUsbDevice *device, guint8 endpoint)
{
	FakeUsbEndpointTemplate *template;

	template = _find_endpoint_template_locked (device, endpoint, NULL);
	if (template == NULL || template->max_packet_size == 0)
		return FAKE_USB_DEFAULT_MAX_PACKET_SIZE;

	return template->max_packet_size;
}

static gboolean
_is_stream_endpoint (FakeUsbDevice *device, guint8 endpoint)
{
	return device->camera != NULL && endpoint == FAKE_USB_CAMERA_DATA_ENDPOINT_IN;
}

static gboolean
_interface_claimed_by_locked (FakeUsbDevice *device, guint8 interface_number, libusb_device_handle *handle)
{
	return interface_number < FAKE_USB_MAX_INTERFACES && device->claims[interface_number] == handle;
}

static int
_first_claimed_interface_locked (FakeUsbDevice *device, libusb_device_handle *handle)
{
	guint i;

	for (i = 0; i < FAKE_USB_MAX_INTERFACES; i++)
		if (device->claims[i] == handle)
			return i;

	return -1;
}

/* WinUSB keeps pipe policies in its interface handles, which libusb frees when an interface is released or the
 * device closed */

static void
_interface_reset_raw_io_locked (FakeUsbDevice *device, int interface_number)
{
	FakeUsbInterfaceTemplate *template = _find_interface_template_locked (device, interface_number);
	guint i;

	if (template == NULL)
		return;

	for (i = 0; i < template->endpoints->len; i++) {
		FakeUsbEndpointTemplate *endpoint_template;

		endpoint_template = &g_array_index (template->endpoints, FakeUsbEndpointTemplate, i);
		fake_usb_device_get_endpoint_locked (device, endpoint_template->address)->raw_io = FALSE;
	}
}

void
fake_usb_device_push_segment_locked (FakeUsbDevice *device, guint8 endpoint, GBytes *bytes, gboolean zlp,
				     FakeUsbSegmentKind kind)
{
	FakeUsbSegment *segment;

	segment = g_new0 (FakeUsbSegment, 1);
	segment->bytes = bytes;
	segment->offset = 0;
	segment->zlp = zlp;
	segment->kind = kind;

	g_queue_push_tail (&fake_usb_device_get_endpoint_locked (device, endpoint)->segments, segment);

	fake_usb_signal_locked ();
}

void
fake_usb_device_flush_endpoint_locked (FakeUsbDevice *device, guint8 endpoint)
{
	FakeUsbEndpointState *state = fake_usb_device_get_endpoint_locked (device, endpoint);

	fake_usb_queue_clear_full (&state->segments, _segment_free);

	if (_is_stream_endpoint (device, endpoint))
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_STREAM_FLUSHES, 1);
}

FakeUsbDevice *
fake_usb_device_ref_locked (FakeUsbDevice *device)
{
	device->ref_count++;

	return device;
}

void
fake_usb_device_unref_locked (FakeUsbDevice *device)
{
	guint i;

	g_assert (device->ref_count > 0);

	device->ref_count--;
	if (device->ref_count > 0)
		return;

	if (device->camera != NULL)
		fake_usb_camera_free (device->camera);

	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++) {
		g_assert (g_queue_is_empty (&device->endpoints[i].pending));
		fake_usb_queue_clear_full (&device->endpoints[i].segments, _segment_free);
	}
	g_assert (g_queue_is_empty (&device->control_pending));

	g_list_free_full (device->injected_errors, g_free);
	g_ptr_array_unref (device->interfaces);
	g_ptr_array_unref (device->strings);
	g_free (device);
}

FakeUsbDevice *
fake_usb_device_new_unplugged (guint16 vendor_id, guint16 product_id,
			       guint8 device_class, guint8 device_subclass, guint8 device_protocol)
{
	FakeUsbDevice *device;
	guint i;

	device = g_new0 (FakeUsbDevice, 1);
	device->ref_count = 1;
	device->in_registry = TRUE;
	device->plugged = FALSE;

	device->descriptor.bLength = LIBUSB_DT_DEVICE_SIZE;
	device->descriptor.bDescriptorType = LIBUSB_DT_DEVICE;
	device->descriptor.bcdUSB = 0x0300;
	device->descriptor.bDeviceClass = device_class;
	device->descriptor.bDeviceSubClass = device_subclass;
	device->descriptor.bDeviceProtocol = device_protocol;
	device->descriptor.bMaxPacketSize0 = 9;
	device->descriptor.idVendor = vendor_id;
	device->descriptor.idProduct = product_id;
	device->descriptor.bcdDevice = 0x0100;
	device->descriptor.bNumConfigurations = 1;

	device->interfaces = g_ptr_array_new_with_free_func (_interface_template_free);
	device->strings = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (device->strings, NULL);	/* Index 0 is the language table */

	device->raw_io_supported = -1;
	device->max_raw_io_transfer_size = FAKE_USB_DEFAULT_MAX_RAW_IO_TRANSFER_SIZE;
	device->allow_partial_reads = -1;
	device->auto_clear_stall = -1;

	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++) {
		g_queue_init (&device->endpoints[i].pending);
		g_queue_init (&device->endpoints[i].segments);
	}
	g_queue_init (&device->control_pending);

	fake_usb_lock ();

	device->bus_number = 1;
	device->device_address = _next_address_locked ();
	device->port_number = device->device_address;

	if (fake_devices == NULL)
		fake_devices = g_ptr_array_new ();
	g_ptr_array_add (fake_devices, device);

	fake_usb_unlock ();

	return device;
}

static const char *
_device_get_string_locked (FakeUsbDevice *device, guint index)
{
	if (index == 0 || index >= device->strings->len)
		return NULL;

	return g_ptr_array_index (device->strings, index);
}

/* Per context device objects */

static gboolean
_device_object_attached_locked (libusb_device *dev)
{
	return dev->fake->plugged && dev->generation == dev->fake->generation;
}

static libusb_device *
_context_get_device_locked (libusb_context *ctx, FakeUsbDevice *device, guint generation)
{
	libusb_device *dev;
	GList *iter;

	for (iter = ctx->devices; iter != NULL; iter = iter->next) {
		dev = iter->data;
		if (dev->fake == device && dev->generation == generation) {
			dev->ref_count++;
			return dev;
		}
	}

	dev = g_new0 (libusb_device, 1);
	dev->ctx = ctx;
	dev->fake = fake_usb_device_ref_locked (device);
	dev->generation = generation;
	dev->ref_count = 1;

	if (device->plugged && generation == device->generation && _has_capability_locked (LIBUSB_CAP_HAS_HOTPLUG)) {
		/* With hotplug, the context keeps the attached devices (core.c usb_devs): libusb_get_device_list ()
		 * returns the same objects, with their string cache, until the device leaves */
		dev->context_ref = TRUE;
		dev->ref_count++;
	}

	ctx->devices = g_list_prepend (ctx->devices, dev);

	return dev;
}

static void
_device_object_unref_locked (libusb_device *dev)
{
	guint i;

	g_assert (dev->ref_count > 0);

	dev->ref_count--;
	if (dev->ref_count > 0)
		return;

	if (dev->ctx != NULL)
		dev->ctx->devices = g_list_remove (dev->ctx->devices, dev);

	for (i = 0; i < G_N_ELEMENTS (dev->strings); i++)
		g_free (dev->strings[i]);

	fake_usb_device_unref_locked (dev->fake);
	g_free (dev);
}

/* The device left: contexts drop the objects they kept for it */

static void
_drop_context_references_locked (FakeUsbDevice *device)
{
	GList *citer;

	for (citer = fake_contexts; citer != NULL; citer = citer->next) {
		libusb_context *ctx = citer->data;
		GList *devices = g_list_copy (ctx->devices);
		GList *iter;

		for (iter = devices; iter != NULL; iter = iter->next) {
			libusb_device *dev = iter->data;

			if (dev->fake == device && dev->context_ref) {
				dev->context_ref = FALSE;
				_device_object_unref_locked (dev);
			}
		}

		g_list_free (devices);
	}
}

/* Transfers */

static FakeUsbTransferPriv *
_transfer_get_priv (struct libusb_transfer *transfer)
{
	return (FakeUsbTransferPriv *) (((guint8 *) transfer) - FAKE_USB_TRANSFER_PRIV_SIZE);
}

static FakeUsbTransferPriv *
_transfer_priv_new (int iso_packets)
{
	FakeUsbTransferPriv *priv;
	gsize size;

	size = FAKE_USB_TRANSFER_PRIV_SIZE + sizeof (struct libusb_transfer) +
		(gsize) MAX (iso_packets, 0) * sizeof (struct libusb_iso_packet_descriptor);

	priv = g_malloc0 (size);
	priv->transfer = (struct libusb_transfer *) (((guint8 *) priv) + FAKE_USB_TRANSFER_PRIV_SIZE);
	priv->transfer->num_iso_packets = MAX (iso_packets, 0);
	priv->state = FAKE_USB_TRANSFER_IDLE;
	priv->auto_claimed_interface = -1;

	return priv;
}

static void
_release_charge_locked (FakeUsbTransferPriv *priv)
{
	if (priv->charged == 0)
		return;

	g_assert (fake_submit_memory_pending >= priv->charged);
	fake_submit_memory_pending -= priv->charged;
	priv->charged = 0;

	fake_usb_signal_locked ();
}

/* auto_release () of the WinUSB backend */

static void
_release_auto_claim_locked (FakeUsbTransferPriv *priv)
{
	int interface_number = priv->auto_claimed_interface;

	if (interface_number < 0)
		return;

	priv->auto_claimed_interface = -1;
	if (priv->device != NULL && priv->device->claims[interface_number] == priv->handle)
		priv->device->claims[interface_number] = NULL;
}

static void
_transfer_priv_free_locked (FakeUsbTransferPriv *priv)
{
	_release_charge_locked (priv);
	_release_auto_claim_locked (priv);

	if (priv->counted) {
		g_assert (fake_n_transfers > 0);
		fake_n_transfers--;
	}

	if ((priv->transfer->flags & LIBUSB_TRANSFER_FREE_BUFFER) != 0)
		free (priv->transfer->buffer);

	if (priv->device != NULL)
		fake_usb_device_unref_locked (priv->device);

	g_free (priv);
}

static int
_transfer_status_to_error (enum libusb_transfer_status status)
{
	/* Same mapping as the libusb synchronous API (sync.c) */
	switch (status) {
		case LIBUSB_TRANSFER_COMPLETED:
			return LIBUSB_SUCCESS;
		case LIBUSB_TRANSFER_TIMED_OUT:
			return LIBUSB_ERROR_TIMEOUT;
		case LIBUSB_TRANSFER_STALL:
			return LIBUSB_ERROR_PIPE;
		case LIBUSB_TRANSFER_OVERFLOW:
			return LIBUSB_ERROR_OVERFLOW;
		case LIBUSB_TRANSFER_NO_DEVICE:
			return LIBUSB_ERROR_NO_DEVICE;
		case LIBUSB_TRANSFER_ERROR:
		case LIBUSB_TRANSFER_CANCELLED:
			return LIBUSB_ERROR_IO;
		default:
			return LIBUSB_ERROR_OTHER;
	}
}

/* The device, or the host controller, is done with a transfer: it waits for event handling to dispatch it */

static void
_transfer_complete_locked (FakeUsbTransferPriv *priv, enum libusb_transfer_status status)
{
	struct libusb_transfer *transfer = priv->transfer;
	FakeUsbDevice *device = priv->device;
	gsize requested = (gsize) MAX (transfer->length, 0);

	if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL)
		requested = requested > LIBUSB_CONTROL_SETUP_SIZE ? requested - LIBUSB_CONTROL_SETUP_SIZE : 0;

	if (status == LIBUSB_TRANSFER_COMPLETED &&
	    (transfer->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) != 0 &&
	    priv->transferred != requested)
		status = LIBUSB_TRANSFER_ERROR;

	transfer->status = status;
	transfer->actual_length = (int) priv->transferred;

	switch (status) {
		case LIBUSB_TRANSFER_OVERFLOW:
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_OVERFLOWS, 1);
			break;
		case LIBUSB_TRANSFER_TIMED_OUT:
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_TIMEOUTS, 1);
			break;
		case LIBUSB_TRANSFER_CANCELLED:
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_CANCELLATIONS, 1);
			break;
		default:
			break;
	}

	_release_auto_claim_locked (priv);

	priv->queue = NULL;
	priv->state = FAKE_USB_TRANSFER_COMPLETED;
	g_queue_push_tail (&priv->ctx->completed, priv);

	fake_usb_signal_locked ();
}

static void
_queue_fail_all_locked (GQueue *queue, enum libusb_transfer_status status)
{
	FakeUsbTransferPriv *priv;

	while ((priv = g_queue_pop_head (queue)) != NULL)
		_transfer_complete_locked (priv, status);
}

static void
_device_fail_all_pending_locked (FakeUsbDevice *device, enum libusb_transfer_status status)
{
	guint i;

	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++)
		_queue_fail_all_locked (&device->endpoints[i].pending, status);
	_queue_fail_all_locked (&device->control_pending, status);
}

/* Hotplug */

static gboolean
_hotplug_callback_matches (FakeUsbHotplugCallback *callback, FakeUsbDevice *device, libusb_hotplug_event event)
{
	if (callback->deregistered || (callback->events & event) == 0)
		return FALSE;
	if (callback->vendor_id != LIBUSB_HOTPLUG_MATCH_ANY && callback->vendor_id != device->descriptor.idVendor)
		return FALSE;
	if (callback->product_id != LIBUSB_HOTPLUG_MATCH_ANY && callback->product_id != device->descriptor.idProduct)
		return FALSE;
	if (callback->dev_class != LIBUSB_HOTPLUG_MATCH_ANY && callback->dev_class != device->descriptor.bDeviceClass)
		return FALSE;

	return TRUE;
}

/* Hotplug events go through a delay queue, which the pump releases in order */

static void
_queue_hotplug_event_locked (libusb_context *ctx, FakeUsbDevice *device, libusb_hotplug_event event)
{
	FakeUsbHotplugMessage *message;
	gboolean match = FALSE;
	GList *iter;

	for (iter = ctx->hotplug_callbacks; iter != NULL && !match; iter = iter->next)
		match = _hotplug_callback_matches (iter->data, device, event);

	if (!match)
		return;

	message = g_new0 (FakeUsbHotplugMessage, 1);
	message->event = event;
	message->device = _context_get_device_locked (ctx, device, device->generation);
	message->due = g_get_monotonic_time () + _hotplug_delay_us_locked ();

	g_queue_push_tail (&ctx->delayed_hotplug_messages, message);
}

static void
_release_hotplug_messages_locked (gint64 now)
{
	GList *iter;

	for (iter = fake_contexts; iter != NULL; iter = iter->next) {
		libusb_context *ctx = iter->data;
		FakeUsbHotplugMessage *message;

		while ((message = g_queue_peek_head (&ctx->delayed_hotplug_messages)) != NULL && message->due <= now) {
			g_queue_pop_head (&ctx->delayed_hotplug_messages);
			g_queue_push_tail (&ctx->hotplug_messages, message);
			fake_usb_signal_locked ();
		}
	}
}

static void
_hotplug_messages_free_locked (GQueue *queue)
{
	FakeUsbHotplugMessage *message;

	while ((message = g_queue_pop_head (queue)) != NULL) {
		_device_object_unref_locked (message->device);
		g_free (message);
	}
}

static void
_hotplug_cleanup_locked (libusb_context *ctx)
{
	GList *iter, *next;

	if (ctx->hotplug_dispatch_depth > 0)
		return;

	for (iter = ctx->hotplug_callbacks; iter != NULL; iter = next) {
		FakeUsbHotplugCallback *callback = iter->data;

		next = iter->next;
		if (callback->deregistered) {
			ctx->hotplug_callbacks = g_list_delete_link (ctx->hotplug_callbacks, iter);
			g_free (callback);
		}
	}
}

/* Plug and unplug */

static void
_device_plug_locked (FakeUsbDevice *device)
{
	GList *iter;
	guint i;

	if (device->plugged || !device->in_registry)
		return;

	/* Transfers left hanging by the previous unplug belong to a dead handle */
	_device_fail_all_pending_locked (device, _unplug_status_locked (device));

	/* A replugged device is enumerated again, at a new address */
	if (device->generation > 0)
		device->device_address = _next_address_locked ();

	device->plugged = TRUE;
	device->generation++;

	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++) {
		device->endpoints[i].halted = FALSE;
		device->endpoints[i].raw_io = FALSE;
		device->endpoints[i].fail_armed = FALSE;
		fake_usb_queue_clear_full (&device->endpoints[i].segments, _segment_free);
	}
	for (i = 0; i < FAKE_USB_MAX_INTERFACES; i++)
		device->claims[i] = NULL;
	device->auto_detach_kernel_driver = FALSE;
	device->unplug_armed = FALSE;
	device->unplug_now = FALSE;

	if (device->camera != NULL)
		fake_usb_camera_power_on_locked (device);

	for (iter = fake_contexts; iter != NULL; iter = iter->next)
		_queue_hotplug_event_locked (iter->data, device, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);

	fake_usb_debug ("device %03d:%03d plugged (generation %u)",
			device->bus_number, device->device_address, device->generation);

	fake_usb_signal_locked ();
}

static void
_device_unplug_locked (FakeUsbDevice *device)
{
	enum libusb_transfer_status status;
	GList *iter;
	guint i;

	if (!device->plugged)
		return;

	device->plugged = FALSE;
	device->unplug_armed = FALSE;
	device->unplug_now = FALSE;
	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_UNPLUGS, 1);

	status = _unplug_status_locked (device);

	if (!device->unplug_transfers_hang)
		_device_fail_all_pending_locked (device, status);

	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++) {
		fake_usb_queue_clear_full (&device->endpoints[i].segments, _segment_free);
		device->endpoints[i].halted = FALSE;
		device->endpoints[i].fail_armed = FALSE;
	}

	if (device->camera != NULL)
		fake_usb_camera_unplugged_locked (device);

	for (iter = fake_contexts; iter != NULL; iter = iter->next)
		_queue_hotplug_event_locked (iter->data, device, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);

	_drop_context_references_locked (device);

	if (device->unplug_transfers_hang)
		fake_usb_debug ("device %03d:%03d unplugged, in-flight transfers left pending",
				device->bus_number, device->device_address);
	else
		fake_usb_debug ("device %03d:%03d unplugged, in-flight transfers completed with status %d",
				device->bus_number, device->device_address, status);

	fake_usb_signal_locked ();
}

static void
_device_remove_locked (FakeUsbDevice *device)
{
	if (!device->in_registry)
		return;

	_device_unplug_locked (device);

	/* Gone for good: transfers left hanging cannot wait any longer */
	_device_fail_all_pending_locked (device, _unplug_status_locked (device));

	g_ptr_array_remove (fake_devices, device);
	device->in_registry = FALSE;
	fake_usb_device_unref_locked (device);
}

/* Device side data delivery */

static FakeUsbSegment *
_zero_length_packet_new (void)
{
	FakeUsbSegment *segment = g_new0 (FakeUsbSegment, 1);

	segment->bytes = g_bytes_new (NULL, 0);
	segment->kind = FAKE_USB_SEGMENT_ZLP;

	return segment;
}

/* How much of @wanted bytes the device may deliver before a scripted cut: the cable pulled by
 * fake_usb_device_unplug_after_frames (), or a failure armed by fake_usb_device_fail_transfers () */

static gsize
_delivery_limit_locked (FakeUsbDevice *device, FakeUsbEndpointState *state, gboolean is_stream, gsize wanted)
{
	gsize limit = wanted;

	if (is_stream && device->unplug_armed &&
	    device->stats[FAKE_USB_STAT_FRAMES_SENT] >= device->unplug_frame_target) {
		gsize available = device->unplug_extra_bytes > device->unplug_bytes_sent ?
			device->unplug_extra_bytes - device->unplug_bytes_sent : 0;

		if (available == 0)
			device->unplug_now = TRUE;
		limit = MIN (limit, available);
	}

	if (state->fail_armed)
		limit = MIN (limit, state->fail_after_bytes);

	return limit;
}

static void
_account_sent_locked (FakeUsbDevice *device, FakeUsbEndpointState *state, gboolean is_stream, gsize size)
{
	if (state->fail_armed)
		state->fail_after_bytes -= MIN (size, state->fail_after_bytes);

	if (!is_stream)
		return;

	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_STREAM_BYTES_SENT, size);

	if (device->unplug_armed && device->stats[FAKE_USB_STAT_FRAMES_SENT] >= device->unplug_frame_target) {
		device->unplug_bytes_sent += size;
		if (device->unplug_extra_bytes > 0 && device->unplug_bytes_sent >= device->unplug_extra_bytes)
			device->unplug_now = TRUE;
	}
}

static void
_segment_done_locked (FakeUsbDevice *device, FakeUsbEndpointState *state, gboolean is_stream)
{
	FakeUsbSegment *segment = g_queue_pop_head (&state->segments);

	if (is_stream && segment->kind == FAKE_USB_SEGMENT_TRAILER) {
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_FRAMES_SENT, 1);
		if (device->unplug_armed &&
		    device->unplug_extra_bytes == 0 &&
		    device->stats[FAKE_USB_STAT_FRAMES_SENT] >= device->unplug_frame_target)
			device->unplug_now = TRUE;
	}

	_segment_free (segment);
}

/* Moves device data into the IN transfer at the head of an endpoint queue, with USB packet semantics. Returns TRUE
 * when the transfer is complete, with its status; FALSE when it waits for more data, or when the delivery stopped at
 * a scripted cut. */

static gboolean
_serve_in_transfer_locked (FakeUsbDevice *device, FakeUsbEndpointState *state, FakeUsbTransferPriv *priv,
			   gsize max_packet_size, gboolean is_stream, gboolean partial_reads,
			   enum libusb_transfer_status *status)
{
	struct libusb_transfer *transfer = priv->transfer;
	gsize length = (gsize) MAX (transfer->length, 0);

	*status = LIBUSB_TRANSFER_COMPLETED;

	while (priv->transferred < length) {
		FakeUsbSegment *segment = g_queue_peek_head (&state->segments);
		const guint8 *data;
		gsize size, remaining, room, wanted, chunk;
		gboolean overflow;

		if (segment == NULL)
			return FALSE;

		data = g_bytes_get_data (segment->bytes, &size);
		remaining = size - segment->offset;
		room = length - priv->transferred;

		if (remaining == 0) {
			/* A zero length packet ends the read */
			_segment_done_locked (device, state, is_stream);
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_ZERO_LENGTH_PACKETS, 1);
			return TRUE;
		}

		/* In a read that is not a packet multiple, the next packet may not fit */
		overflow = remaining > room && room % max_packet_size != 0;
		wanted = MIN (remaining, room);

		chunk = _delivery_limit_locked (device, state, is_stream, wanted);
		if (chunk > 0)
			memcpy (transfer->buffer + priv->transferred, data + segment->offset, chunk);
		priv->transferred += chunk;

		if (chunk < wanted) {
			/* The cable is pulled, or the transfer fails, in the middle of this transfer */
			segment->offset += chunk;
			_account_sent_locked (device, state, is_stream, chunk);
			return FALSE;
		}

		if (overflow && !partial_reads) {
			/* The whole packet is consumed and the transfer completes with an overflow */
			gsize full_packets = room - room % max_packet_size;
			gsize packet = MIN (max_packet_size, remaining - full_packets);

			segment->offset += full_packets + packet;
			_account_sent_locked (device, state, is_stream, full_packets + packet);
			if (segment->offset >= size)
				_segment_done_locked (device, state, is_stream);
			*status = LIBUSB_TRANSFER_OVERFLOW;
			return TRUE;
		}

		segment->offset += chunk;
		_account_sent_locked (device, state, is_stream, chunk);

		if (overflow) {
			/* WinUSB ALLOW_PARTIAL_READS: the read completes with what fits, the rest of the packet starts
			 * the next read */
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_PARTIAL_READS, 1);
			return TRUE;
		}

		if (segment->offset >= size) {
			gboolean short_packet = (size % max_packet_size) != 0;
			gboolean zlp = segment->zlp && !short_packet;

			_segment_done_locked (device, state, is_stream);

			if (short_packet)
				return TRUE;

			if (zlp) {
				if (priv->transferred < length) {
					fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_ZERO_LENGTH_PACKETS, 1);
					return TRUE;
				}

				/* The read is complete because it is full: the zero length packet completes the next
				 * one, with no data */
				g_queue_push_head (&state->segments, _zero_length_packet_new ());
				return TRUE;
			}
		}
	}

	return TRUE;
}

/* fake_usb_device_fail_transfers (): fails the pending transfers once the byte budget is spent */

static gboolean
_endpoint_check_failure_locked (FakeUsbDevice *device, guint8 endpoint, FakeUsbEndpointState *state)
{
	guint n_failed;

	if (!state->fail_armed || state->fail_after_bytes > 0 || g_queue_is_empty (&state->pending))
		return FALSE;

	state->fail_armed = FALSE;

	n_failed = state->pending.length;
	_queue_fail_all_locked (&state->pending, state->fail_status);
	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_SCRIPTED_FAILURES, n_failed);

	if (state->fail_flush)
		fake_usb_device_flush_endpoint_locked (device, endpoint);

	fake_usb_debug ("device %03d:%03d: %u transfer(s) on endpoint 0x%02x failed with status %d%s",
			device->bus_number, device->device_address, n_failed, endpoint, state->fail_status,
			state->fail_flush ? ", endpoint data flushed" : "");

	return TRUE;
}

/* WinUSB AUTO_CLEAR_STALL, on IN pipes: once the transfers in flight have failed, the stall is cleared. Not with
 * RAW_IO on, which "bypasses queuing and error handling": Microsoft documents AUTO_CLEAR_STALL as "useful only when
 * you have multiple pending read requests to the endpoint when RAW_IO is disabled", and without it every later
 * transfer fails until WinUsb_ResetPipe () (libusb_clear_halt ()). */

static void
_endpoint_auto_clear_stall_locked (FakeUsbDevice *device, guint8 endpoint)
{
	FakeUsbEndpointState *state = fake_usb_device_get_endpoint_locked (device, endpoint);

	if (!state->halted || state->raw_io || (endpoint & LIBUSB_ENDPOINT_IN) == 0 ||
	    !_auto_clear_stall_locked (device))
		return;

	state->halted = FALSE;
	fake_usb_device_flush_endpoint_locked (device, endpoint);
	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_AUTO_CLEARED_STALLS, 1);
}

static void
_endpoint_set_halt_locked (FakeUsbDevice *device, guint8 endpoint, gboolean halted)
{
	FakeUsbEndpointState *state = fake_usb_device_get_endpoint_locked (device, endpoint);

	state->halted = halted;
	fake_usb_device_flush_endpoint_locked (device, endpoint);

	if (halted && !g_queue_is_empty (&state->pending)) {
		_queue_fail_all_locked (&state->pending, LIBUSB_TRANSFER_STALL);
		_endpoint_auto_clear_stall_locked (device, endpoint);
	}

	fake_usb_signal_locked ();
}

static enum libusb_transfer_status _control_request_locked (FakeUsbDevice *device, guint8 request_type,
							    guint8 request, guint16 value, guint16 index,
							    unsigned char *data, guint16 length, gsize *actual);

/* Control requests are answered when due, or time out */

static void
_serve_control_locked (FakeUsbDevice *device, gint64 now)
{
	GList *iter, *next;

	for (iter = device->control_pending.head; iter != NULL; iter = next) {
		FakeUsbTransferPriv *priv = iter->data;

		next = iter->next;

		if (device->plugged && priv->due <= now) {
			struct libusb_transfer *transfer = priv->transfer;
			struct libusb_control_setup *setup = (struct libusb_control_setup *) transfer->buffer;
			enum libusb_transfer_status status;
			gsize actual = 0;

			g_queue_delete_link (&device->control_pending, iter);
			status = _control_request_locked (device, setup->bmRequestType, setup->bRequest,
							  libusb_le16_to_cpu (setup->wValue),
							  libusb_le16_to_cpu (setup->wIndex),
							  transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE,
							  libusb_le16_to_cpu (setup->wLength), &actual);
			priv->transferred = actual;
			_transfer_complete_locked (priv, status);

			/* Start over: cheap, and safe whatever the request changed */
			next = device->control_pending.head;
		} else if (priv->deadline <= now) {
			g_queue_delete_link (&device->control_pending, iter);
			_transfer_complete_locked (priv, LIBUSB_TRANSFER_TIMED_OUT);
		}
	}
}

static void
_device_serve_locked (FakeUsbDevice *device, gint64 now)
{
	guint slot;

	_serve_control_locked (device, now);

	for (slot = 0; slot < FAKE_USB_N_ENDPOINT_SLOTS && !device->unplug_now; slot++) {
		FakeUsbEndpointState *state = &device->endpoints[slot];
		guint8 endpoint = _slot_endpoint (slot);
		gsize max_packet_size;
		gboolean is_stream, partial_reads;
		GList *iter, *next;

		if (g_queue_is_empty (&state->pending))
			continue;

		for (iter = state->pending.head; iter != NULL; iter = next) {
			FakeUsbTransferPriv *priv = iter->data;

			next = iter->next;
			if (priv->deadline <= now) {
				g_queue_delete_link (&state->pending, iter);
				_transfer_complete_locked (priv, LIBUSB_TRANSFER_TIMED_OUT);
			}
		}

		/* An unplugged device sends nothing; its hanging transfers can only time out */
		if (!device->plugged || g_queue_is_empty (&state->pending))
			continue;

		if (state->halted) {
			_queue_fail_all_locked (&state->pending, LIBUSB_TRANSFER_STALL);
			_endpoint_auto_clear_stall_locked (device, endpoint);
			continue;
		}

		max_packet_size = fake_usb_device_get_max_packet_size_locked (device, endpoint);
		is_stream = _is_stream_endpoint (device, endpoint);
		partial_reads = _partial_reads_locked (device, state);

		while (!device->unplug_now) {
			FakeUsbTransferPriv *priv;
			enum libusb_transfer_status status;

			if (_endpoint_check_failure_locked (device, endpoint, state))
				break;

			priv = g_queue_peek_head (&state->pending);
			if (priv == NULL)
				break;

			if (!_serve_in_transfer_locked (device, state, priv, max_packet_size, is_stream, partial_reads,
							&status)) {
				_endpoint_check_failure_locked (device, endpoint, state);
				break;
			}

			g_queue_pop_head (&state->pending);
			_transfer_complete_locked (priv, status);
		}
	}
}

/* Runs the devices: produces frames, delivers data, answers control requests, times transfers out, releases due
 * hotplug events. It may release the lock while a camera fills a frame, so callers must not keep pointers to fake
 * state across it, except to their own transfer. */

static void
_pump_locked (void)
{
	gboolean restart;

	do {
		guint i;

		restart = FALSE;

		_release_hotplug_messages_locked (g_get_monotonic_time ());

		for (i = 0; fake_devices != NULL && i < fake_devices->len && !restart; i++) {
			FakeUsbDevice *device = g_ptr_array_index (fake_devices, i);
			gint64 now = g_get_monotonic_time ();

			if (device->plugged && device->camera != NULL) {
				FakeUsbFrameJob *job;

				fake_usb_camera_tick_locked (device, now);

				job = fake_usb_camera_start_frame_locked (device, now);
				if (job != NULL) {
					fake_usb_unlock ();
					fake_usb_camera_run_frame (job);
					fake_usb_lock ();
					fake_usb_camera_finish_frame_locked (job);

					/* The device list may have changed while unlocked */
					restart = TRUE;
					continue;
				}
			}

			_device_serve_locked (device, now);

			if (device->unplug_now)
				_device_unplug_locked (device);
		}
	} while (restart);
}

static gint64
_get_next_wakeup_locked (gint64 now)
{
	gint64 wakeup = now + FAKE_USB_MAX_WAIT_US;
	GList *iter;
	guint i, slot;

	for (iter = fake_contexts; iter != NULL; iter = iter->next) {
		libusb_context *ctx = iter->data;
		FakeUsbHotplugMessage *message = g_queue_peek_head (&ctx->delayed_hotplug_messages);

		if (message != NULL)
			wakeup = MIN (wakeup, message->due);
	}

	if (fake_devices == NULL)
		return wakeup;

	for (i = 0; i < fake_devices->len; i++) {
		FakeUsbDevice *device = g_ptr_array_index (fake_devices, i);

		if (device->plugged && device->camera != NULL)
			wakeup = MIN (wakeup, fake_usb_camera_get_next_wakeup_locked (device, now));

		for (slot = 0; slot < FAKE_USB_N_ENDPOINT_SLOTS; slot++) {
			for (iter = device->endpoints[slot].pending.head; iter != NULL; iter = iter->next) {
				FakeUsbTransferPriv *priv = iter->data;

				wakeup = MIN (wakeup, priv->deadline);
			}
		}

		for (iter = device->control_pending.head; iter != NULL; iter = iter->next) {
			FakeUsbTransferPriv *priv = iter->data;

			wakeup = MIN (wakeup, priv->deadline);
			if (device->plugged)
				wakeup = MIN (wakeup, priv->due);
		}
	}

	return wakeup;
}

static void
_call_transfer_hook (FakeUsbDevice *device, guint8 endpoint, int length, gboolean is_synchronous)
{
	FakeUsbTransferHook hook;
	gpointer hook_data;

	fake_usb_lock ();
	hook = fake_transfer_hook;
	hook_data = fake_transfer_hook_data;
	fake_usb_unlock ();

	if (hook != NULL)
		hook (device, endpoint, length, is_synchronous, hook_data);
}

/* Descriptors */

static struct libusb_config_descriptor *
_config_descriptor_new_locked (FakeUsbDevice *device)
{
	struct libusb_config_descriptor *config;
	struct libusb_interface *interfaces;
	guint total_length = LIBUSB_DT_CONFIG_SIZE;
	guint i, j;

	config = g_new0 (struct libusb_config_descriptor, 1);
	interfaces = g_new0 (struct libusb_interface, MAX (device->interfaces->len, 1));

	for (i = 0; i < device->interfaces->len; i++) {
		FakeUsbInterfaceTemplate *template = g_ptr_array_index (device->interfaces, i);
		struct libusb_interface_descriptor *altsetting;

		altsetting = g_new0 (struct libusb_interface_descriptor, 1);
		altsetting->bLength = LIBUSB_DT_INTERFACE_SIZE;
		altsetting->bDescriptorType = LIBUSB_DT_INTERFACE;
		altsetting->bInterfaceNumber = template->number;
		altsetting->bAlternateSetting = 0;
		altsetting->bNumEndpoints = template->endpoints->len;
		altsetting->bInterfaceClass = template->interface_class;
		altsetting->bInterfaceSubClass = template->interface_subclass;
		altsetting->bInterfaceProtocol = template->interface_protocol;
		altsetting->iInterface = 0;

		total_length += LIBUSB_DT_INTERFACE_SIZE + template->extra->len;

		if (template->endpoints->len > 0) {
			struct libusb_endpoint_descriptor *endpoints;

			endpoints = g_new0 (struct libusb_endpoint_descriptor, template->endpoints->len);
			for (j = 0; j < template->endpoints->len; j++) {
				FakeUsbEndpointTemplate *endpoint_template;

				endpoint_template = &g_array_index (template->endpoints, FakeUsbEndpointTemplate, j);
				endpoints[j].bLength = LIBUSB_DT_ENDPOINT_SIZE;
				endpoints[j].bDescriptorType = LIBUSB_DT_ENDPOINT;
				endpoints[j].bEndpointAddress = endpoint_template->address;
				endpoints[j].bmAttributes = endpoint_template->attributes;
				endpoints[j].wMaxPacketSize = endpoint_template->max_packet_size;
				endpoints[j].bInterval = endpoint_template->interval;

				total_length += LIBUSB_DT_ENDPOINT_SIZE;
			}
			altsetting->endpoint = endpoints;
		}

		if (template->extra->len > 0) {
			unsigned char *extra = g_malloc (template->extra->len);

			memcpy (extra, template->extra->data, template->extra->len);
			altsetting->extra = extra;
			altsetting->extra_length = template->extra->len;
		}

		interfaces[i].altsetting = altsetting;
		interfaces[i].num_altsetting = 1;
	}

	config->bLength = LIBUSB_DT_CONFIG_SIZE;
	config->bDescriptorType = LIBUSB_DT_CONFIG;
	config->wTotalLength = total_length;
	config->bNumInterfaces = device->interfaces->len;
	config->bConfigurationValue = 1;
	config->iConfiguration = 0;
	config->bmAttributes = 0x80;
	config->MaxPower = 112;
	config->interface = interfaces;

	return config;
}

static void
_config_descriptor_free (struct libusb_config_descriptor *config)
{
	int i, j;

	if (config == NULL)
		return;

	for (i = 0; i < config->bNumInterfaces; i++) {
		const struct libusb_interface *iface = &config->interface[i];

		for (j = 0; j < iface->num_altsetting; j++) {
			g_free ((gpointer) iface->altsetting[j].endpoint);
			g_free ((gpointer) iface->altsetting[j].extra);
		}
		g_free ((gpointer) iface->altsetting);
	}

	g_free ((gpointer) config->interface);
	g_free ((gpointer) config->extra);
	g_free (config);
}

static void
_append_le16 (GByteArray *array, guint16 value)
{
	guint8 bytes[2];

	bytes[0] = value & 0xff;
	bytes[1] = value >> 8;
	g_byte_array_append (array, bytes, 2);
}

/* Standard descriptors as sent on the wire, for GET_DESCRIPTOR control requests */

static GByteArray *
_descriptor_bytes_locked (FakeUsbDevice *device, guint8 type, guint8 index)
{
	GByteArray *array = g_byte_array_new ();
	guint i, j;

	switch (type) {
		case LIBUSB_DT_DEVICE:
			{
				struct libusb_device_descriptor *descriptor = &device->descriptor;
				guint8 bytes[4];

				bytes[0] = LIBUSB_DT_DEVICE_SIZE;
				bytes[1] = LIBUSB_DT_DEVICE;
				g_byte_array_append (array, bytes, 2);
				_append_le16 (array, descriptor->bcdUSB);
				bytes[0] = descriptor->bDeviceClass;
				bytes[1] = descriptor->bDeviceSubClass;
				bytes[2] = descriptor->bDeviceProtocol;
				bytes[3] = descriptor->bMaxPacketSize0;
				g_byte_array_append (array, bytes, 4);
				_append_le16 (array, descriptor->idVendor);
				_append_le16 (array, descriptor->idProduct);
				_append_le16 (array, descriptor->bcdDevice);
				bytes[0] = descriptor->iManufacturer;
				bytes[1] = descriptor->iProduct;
				bytes[2] = descriptor->iSerialNumber;
				bytes[3] = descriptor->bNumConfigurations;
				g_byte_array_append (array, bytes, 4);
			}
			return array;
		case LIBUSB_DT_CONFIG:
			if (index != 0)
				break;
			{
				guint8 bytes[9];

				bytes[0] = LIBUSB_DT_CONFIG_SIZE;
				bytes[1] = LIBUSB_DT_CONFIG;
				g_byte_array_append (array, bytes, 2);
				_append_le16 (array, 0);
				bytes[0] = device->interfaces->len;
				bytes[1] = 1;
				bytes[2] = 0;
				bytes[3] = 0x80;
				bytes[4] = 112;
				g_byte_array_append (array, bytes, 5);

				for (i = 0; i < device->interfaces->len; i++) {
					FakeUsbInterfaceTemplate *template = g_ptr_array_index (device->interfaces, i);

					bytes[0] = LIBUSB_DT_INTERFACE_SIZE;
					bytes[1] = LIBUSB_DT_INTERFACE;
					bytes[2] = template->number;
					bytes[3] = 0;
					bytes[4] = template->endpoints->len;
					bytes[5] = template->interface_class;
					bytes[6] = template->interface_subclass;
					bytes[7] = template->interface_protocol;
					bytes[8] = 0;
					g_byte_array_append (array, bytes, 9);
					g_byte_array_append (array, template->extra->data, template->extra->len);

					for (j = 0; j < template->endpoints->len; j++) {
						FakeUsbEndpointTemplate *endpoint_template;

						endpoint_template = &g_array_index (template->endpoints,
										    FakeUsbEndpointTemplate, j);
						bytes[0] = LIBUSB_DT_ENDPOINT_SIZE;
						bytes[1] = LIBUSB_DT_ENDPOINT;
						bytes[2] = endpoint_template->address;
						bytes[3] = endpoint_template->attributes;
						g_byte_array_append (array, bytes, 4);
						_append_le16 (array, endpoint_template->max_packet_size);
						bytes[0] = endpoint_template->interval;
						g_byte_array_append (array, bytes, 1);
					}
				}

				array->data[2] = array->len & 0xff;
				array->data[3] = (array->len >> 8) & 0xff;
			}
			return array;
		case LIBUSB_DT_STRING:
			{
				guint8 bytes[2];

				if (index == 0) {
					bytes[0] = 4;
					bytes[1] = LIBUSB_DT_STRING;
					g_byte_array_append (array, bytes, 2);
					_append_le16 (array, 0x0409);
					return array;
				} else {
					const char *string = _device_get_string_locked (device, index);
					const char *p;
					guint n_chars = 0;

					if (string == NULL)
						break;

					bytes[0] = 0;
					bytes[1] = LIBUSB_DT_STRING;
					g_byte_array_append (array, bytes, 2);
					for (p = string; *p != '\0' && n_chars < 126; p = g_utf8_next_char (p), n_chars++) {
						gunichar c = g_utf8_get_char (p);

						_append_le16 (array, c < 0x10000 ? c : 0xfffd);
					}
					array->data[0] = array->len;
					return array;
				}
			}
		default:
			break;
	}

	g_byte_array_unref (array);

	return NULL;
}

/* Executes a control request on the default control pipe. Only standard requests are supported, others stall. */

static enum libusb_transfer_status
_control_request_locked (FakeUsbDevice *device, guint8 request_type, guint8 request, guint16 value, guint16 index,
			 unsigned char *data, guint16 length, gsize *actual)
{
	*actual = 0;

	if ((request_type & (0x03 << 5)) != LIBUSB_REQUEST_TYPE_STANDARD)
		return LIBUSB_TRANSFER_STALL;

	switch (request) {
		case LIBUSB_REQUEST_GET_DESCRIPTOR:
			{
				GByteArray *bytes;

				if ((request_type & LIBUSB_ENDPOINT_IN) == 0)
					return LIBUSB_TRANSFER_STALL;

				bytes = _descriptor_bytes_locked (device, value >> 8, value & 0xff);
				if (bytes == NULL)
					return LIBUSB_TRANSFER_STALL;

				*actual = MIN (length, bytes->len);
				if (*actual > 0)
					memcpy (data, bytes->data, *actual);
				g_byte_array_unref (bytes);
			}
			return LIBUSB_TRANSFER_COMPLETED;
		case LIBUSB_REQUEST_GET_STATUS:
			*actual = MIN (length, 2);
			if (*actual > 0)
				memset (data, 0, *actual);
			return LIBUSB_TRANSFER_COMPLETED;
		case LIBUSB_REQUEST_GET_CONFIGURATION:
			if (length > 0) {
				data[0] = 1;
				*actual = 1;
			}
			return LIBUSB_TRANSFER_COMPLETED;
		case LIBUSB_REQUEST_SET_CONFIGURATION:
		case LIBUSB_REQUEST_SET_INTERFACE:
			return LIBUSB_TRANSFER_COMPLETED;
		case LIBUSB_REQUEST_SET_FEATURE:
		case LIBUSB_REQUEST_CLEAR_FEATURE:
			if ((request_type & 0x1f) == LIBUSB_RECIPIENT_ENDPOINT && value == 0) {
				guint8 endpoint = index & 0xff;

				if (_find_endpoint_template_locked (device, endpoint, NULL) == NULL)
					return LIBUSB_TRANSFER_STALL;

				_endpoint_set_halt_locked (device, endpoint, request == LIBUSB_REQUEST_SET_FEATURE);
			}
			return LIBUSB_TRANSFER_COMPLETED;
		default:
			return LIBUSB_TRANSFER_STALL;
	}
}

/* Contexts */

static libusb_context *
_context_new_locked (void)
{
	libusb_context *ctx;

	ctx = g_new0 (libusb_context, 1);
	ctx->next_hotplug_handle = 1;
	g_queue_init (&ctx->completed);
	g_queue_init (&ctx->dispatching);
	g_queue_init (&ctx->hotplug_messages);
	g_queue_init (&ctx->delayed_hotplug_messages);

	fake_contexts = g_list_append (fake_contexts, ctx);

	return ctx;
}

static libusb_context *
_resolve_context_locked (libusb_context *ctx)
{
	return ctx != NULL ? ctx : fake_default_context;
}

static gboolean
_holds_event_lock_locked (libusb_context *ctx)
{
	return ctx != NULL && ctx->event_owner == g_thread_self ();
}

/* do_close () in core.c: transfers still in flight on the handle are forgotten, with no callback, and their
 * dev_handle set to NULL; a synchronous transfer waiting on one of them then reports LIBUSB_TRANSFER_NO_DEVICE */

static void
_orphan_transfers_locked (GQueue *queue, libusb_device_handle *handle, gboolean attached)
{
	GList *iter, *next;

	for (iter = queue->head; iter != NULL; iter = next) {
		FakeUsbTransferPriv *priv = iter->data;

		next = iter->next;
		if (handle != NULL && priv->handle != handle)
			continue;

		g_queue_delete_link (queue, iter);

		/* libusb only complains when the device is still connected */
		if (attached)
			_misuse_locked (priv->device, "libusb_close () with a transfer in flight on endpoint 0x%02x",
					priv->transfer->endpoint);

		_release_charge_locked (priv);
		_release_auto_claim_locked (priv);
		priv->transfer->dev_handle = NULL;
		priv->queue = NULL;
		priv->state = FAKE_USB_TRANSFER_ORPHANED;
	}
}

static void
_handle_close_locked (libusb_device_handle *handle)
{
	FakeUsbDevice *device = handle->dev->fake;
	libusb_context *ctx = handle->ctx;
	gboolean attached = _device_object_attached_locked (handle->dev);
	guint i;

	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++)
		_orphan_transfers_locked (&device->endpoints[i].pending, handle, attached);
	_orphan_transfers_locked (&device->control_pending, handle, attached);
	_orphan_transfers_locked (&ctx->completed, handle, attached);
	_orphan_transfers_locked (&ctx->dispatching, handle, attached);

	for (i = 0; i < FAKE_USB_MAX_INTERFACES; i++) {
		if (device->claims[i] == handle) {
			_interface_reset_raw_io_locked (device, i);
			device->claims[i] = NULL;
		}
	}

	ctx->handles = g_list_remove (ctx->handles, handle);
	ctx->interrupt_serial++;

	g_assert (device->n_open_handles > 0);
	device->n_open_handles--;
	fake_n_open_handles--;

	_device_object_unref_locked (handle->dev);
	g_free (handle);

	fake_usb_signal_locked ();
}

static void
_context_free_locked (libusb_context *ctx)
{
	GList *devices, *iter;

	fake_contexts = g_list_remove (fake_contexts, ctx);

	while (ctx->handles != NULL) {
		libusb_device_handle *handle = ctx->handles->data;

		_misuse_locked (handle->dev->fake, "libusb_exit () with an open device handle");
		_handle_close_locked (handle);
	}

	/* Every transfer of the context belonged to one of its handles, and was dropped with it, unless libusb_exit ()
	 * was called from a callback */
	_orphan_transfers_locked (&ctx->completed, NULL, FALSE);
	_orphan_transfers_locked (&ctx->dispatching, NULL, FALSE);

	_hotplug_messages_free_locked (&ctx->hotplug_messages);
	_hotplug_messages_free_locked (&ctx->delayed_hotplug_messages);

	g_list_free_full (ctx->hotplug_callbacks, g_free);

	/* Device objects still referenced by the application outlive the context */
	devices = ctx->devices;
	ctx->devices = NULL;
	for (iter = devices; iter != NULL; iter = iter->next)
		((libusb_device *) iter->data)->ctx = NULL;
	for (iter = devices; iter != NULL; iter = iter->next) {
		libusb_device *dev = iter->data;

		if (dev->context_ref) {
			dev->context_ref = FALSE;
			_device_object_unref_locked (dev);
		}
	}
	g_list_free (devices);

	g_free (ctx);

	fake_usb_signal_locked ();
}

/* Event handling */

static void
_record_event_thread_priority_locked (libusb_context *ctx)
{
#ifdef G_OS_WIN32
	int priority = GetThreadPriority (GetCurrentThread ());
	GList *iter;

	for (iter = ctx->handles; iter != NULL; iter = iter->next) {
		libusb_device_handle *handle = iter->data;
		FakeUsbDevice *device = handle->dev->fake;

		device->event_priority_last = priority;
		if (!device->event_priority_seen || priority > device->event_priority_highest)
			device->event_priority_highest = priority;
		device->event_priority_seen = TRUE;
	}
#endif
}

static void
_dispatch_transfers_locked (libusb_context *ctx)
{
	FakeUsbTransferPriv *priv;

	while ((priv = g_queue_pop_head (&ctx->dispatching)) != NULL) {
		struct libusb_transfer *transfer = priv->transfer;
		libusb_transfer_cb_fn callback;
		gboolean free_transfer;

		/* Reaped: the usbfs memory is given back */
		_release_charge_locked (priv);

		priv->state = FAKE_USB_TRANSFER_IDLE;

		if (priv->is_synchronous) {
			/* sync_transfer_cb () of sync.c: the waiting thread takes it from here */
			priv->synchronous_done = TRUE;
			fake_usb_signal_locked ();
			continue;
		}

		callback = transfer->callback;
		free_transfer = (transfer->flags & LIBUSB_TRANSFER_FREE_TRANSFER) != 0;

		fake_usb_unlock ();

		if (callback != NULL)
			callback (transfer);
		if (free_transfer)
			libusb_free_transfer (transfer);

		fake_usb_lock ();
	}
}

static void
_dispatch_hotplug_locked (libusb_context *ctx, GQueue *messages, GList *callbacks)
{
	FakeUsbHotplugMessage *message;

	while ((message = g_queue_pop_head (messages)) != NULL) {
		GList *iter;

		for (iter = callbacks; iter != NULL; iter = iter->next) {
			FakeUsbHotplugCallback *callback = iter->data;
			int result;

			if (!_hotplug_callback_matches (callback, message->device->fake, message->event))
				continue;

			fake_usb_unlock ();
			result = callback->callback (ctx, message->device, message->event, callback->user_data);
			fake_usb_lock ();

			if (result != 0)
				callback->deregistered = TRUE;
		}

		_device_object_unref_locked (message->device);
		g_free (message);
	}
}

/* Dispatches everything that completed on the context, on the calling thread, which holds the event lock */

static void
_dispatch_locked (libusb_context *ctx)
{
	GQueue messages = G_QUEUE_INIT;
	GList *callbacks = NULL;
	FakeUsbTransferPriv *priv;
	FakeUsbHotplugMessage *message;
	gboolean hotplug_first = _hotplug_first_locked ();
	gboolean dispatching_hotplug = FALSE;

	while ((priv = g_queue_pop_head (&ctx->completed)) != NULL) {
		priv->state = FAKE_USB_TRANSFER_DISPATCHING;
		g_queue_push_tail (&ctx->dispatching, priv);
	}
	while ((message = g_queue_pop_head (&ctx->hotplug_messages)) != NULL)
		g_queue_push_tail (&messages, message);

	if (!g_queue_is_empty (&messages)) {
		ctx->hotplug_dispatch_depth++;
		dispatching_hotplug = TRUE;
		callbacks = g_list_copy (ctx->hotplug_callbacks);
	}

	if (hotplug_first)
		_dispatch_hotplug_locked (ctx, &messages, callbacks);
	_dispatch_transfers_locked (ctx);
	if (!hotplug_first)
		_dispatch_hotplug_locked (ctx, &messages, callbacks);

	if (dispatching_hotplug) {
		ctx->hotplug_dispatch_depth--;
		_hotplug_cleanup_locked (ctx);
	}
	g_list_free (callbacks);

	fake_usb_signal_locked ();
}

/* One round of event handling, with the event lock held: wait for something to dispatch, then dispatch it */

static void
_run_events_locked (libusb_context *ctx, gint64 timeout_us, int *completed)
{
	guint serial = ctx->interrupt_serial;
	gint64 deadline = timeout_us >= 0 ? g_get_monotonic_time () + timeout_us : G_MAXINT64;

	for (;;) {
		gint64 now;

		_pump_locked ();

		if (!g_queue_is_empty (&ctx->completed) || !g_queue_is_empty (&ctx->hotplug_messages))
			break;
		if (ctx->interrupt_serial != serial || ctx->device_close > 0)
			break;
		if (completed != NULL && *completed)
			break;

		now = g_get_monotonic_time ();
		if (now >= deadline)
			break;

		_wait_locked (MIN (deadline, _get_next_wakeup_locked (now)));
	}

	_dispatch_locked (ctx);
}

/* libusb_handle_events_timeout_completed () of io.c: takes the event lock and handles events, or, when another
 * thread holds it (or a libusb_close () waits for it), waits until it is released. Returns LIBUSB_ERROR_BUSY when
 * the calling thread holds it already, that is from a callback. */

static int
_handle_events_locked (libusb_context *ctx, gint64 timeout_us, int *completed)
{
	GThread *self = g_thread_self ();
	gint64 deadline;
	guint releases;

	if (ctx->event_owner == self)
		return LIBUSB_ERROR_BUSY;

	if (ctx->event_owner == NULL && ctx->device_close == 0) {
		ctx->event_owner = self;
		if (completed == NULL || !*completed)
			_run_events_locked (ctx, timeout_us, completed);
		ctx->event_owner = NULL;
		ctx->event_lock_releases++;
		fake_usb_signal_locked ();
		return LIBUSB_SUCCESS;
	}

	deadline = timeout_us >= 0 ? g_get_monotonic_time () + timeout_us : G_MAXINT64;
	releases = ctx->event_lock_releases;

	while (ctx->event_lock_releases == releases && (completed == NULL || !*completed)) {
		gint64 now = g_get_monotonic_time ();

		if (now >= deadline)
			break;

		_wait_locked (MIN (deadline, now + FAKE_USB_MAX_WAIT_US));
	}

	return LIBUSB_SUCCESS;
}

/* Synchronous calls refuse to run from a callback of their context: real libusb would deadlock (Linux) or spin
 * (Windows) */

static gboolean
_synchronous_from_callback_locked (libusb_context *ctx, FakeUsbDevice *device)
{
	if (!_holds_event_lock_locked (ctx))
		return FALSE;

	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_SYNC_IN_EVENT_HANDLER, 1);
	_misuse_locked (device, "synchronous libusb call from a libusb callback (real libusb deadlocks or spins)");

	return TRUE;
}

/* sync_transfer_wait_for_completion () of sync.c */

static void
_wait_synchronous_locked (FakeUsbTransferPriv *priv)
{
	libusb_context *ctx = priv->ctx;

	while (!priv->synchronous_done) {
		if (priv->transfer->dev_handle == NULL) {
			/* Transfer completion after libusb_close () */
			priv->transfer->status = LIBUSB_TRANSFER_NO_DEVICE;
			break;
		}

		_handle_events_locked (ctx, FAKE_USB_DEFAULT_EVENT_TIMEOUT, &priv->synchronous_done);
	}
}

/* Library initialisation */

int LIBUSB_CALL
libusb_init (libusb_context **ctx)
{
	libusb_context *context;
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_INIT, NULL);

	error = _take_injected_error_locked (&fake_global_injected_errors, FAKE_USB_CALL_INIT, -1);
	if (error != 0) {
		fake_usb_unlock ();
		return error;
	}

	if (ctx == NULL) {
		if (fake_default_context == NULL)
			fake_default_context = _context_new_locked ();
		fake_default_context_refs++;
		fake_usb_unlock ();
		return LIBUSB_SUCCESS;
	}

	context = _context_new_locked ();

	fake_usb_unlock ();

	*ctx = context;

	return LIBUSB_SUCCESS;
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010A
int LIBUSB_CALL
libusb_init_context (libusb_context **ctx, const struct libusb_init_option options[], int num_options)
{
	return libusb_init (ctx);
}
#endif

void LIBUSB_CALL
libusb_exit (libusb_context *ctx)
{
	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_EXIT, NULL);

	if (ctx == NULL) {
		if (fake_default_context == NULL) {
			fake_usb_unlock ();
			return;
		}
		g_assert (fake_default_context_refs > 0);
		fake_default_context_refs--;
		if (fake_default_context_refs > 0) {
			fake_usb_unlock ();
			return;
		}
		ctx = fake_default_context;
		fake_default_context = NULL;
	}

	_context_free_locked (ctx);

	fake_usb_unlock ();
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000106
int LIBUSB_CALLV
libusb_set_option (libusb_context *ctx, enum libusb_option option, ...)
{
	_count (FAKE_USB_CALL_SET_OPTION, NULL);

	return LIBUSB_SUCCESS;
}
#endif

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000107
void LIBUSB_CALL
libusb_set_log_cb (libusb_context *ctx, libusb_log_cb cb, int mode)
{
}
#endif

/* Device enumeration */

ssize_t LIBUSB_CALL
libusb_get_device_list (libusb_context *ctx, libusb_device ***list)
{
	libusb_device **devices;
	ssize_t n_devices = 0;
	int error;
	guint i;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_DEVICE_LIST, NULL);

	ctx = _resolve_context_locked (ctx);
	if (ctx == NULL || list == NULL) {
		fake_usb_unlock ();
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	error = _take_injected_error_locked (&fake_global_injected_errors, FAKE_USB_CALL_GET_DEVICE_LIST, -1);
	if (error != 0) {
		fake_usb_unlock ();
		return error;
	}

	devices = g_new0 (libusb_device *, (fake_devices != NULL ? fake_devices->len : 0) + 1);

	for (i = 0; fake_devices != NULL && i < fake_devices->len; i++) {
		FakeUsbDevice *device = g_ptr_array_index (fake_devices, i);

		if (device->plugged)
			devices[n_devices++] = _context_get_device_locked (ctx, device, device->generation);
	}

	fake_usb_unlock ();

	*list = devices;

	return n_devices;
}

void LIBUSB_CALL
libusb_free_device_list (libusb_device **list, int unref_devices)
{
	int i;

	if (list == NULL)
		return;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_FREE_DEVICE_LIST, NULL);

	if (unref_devices)
		for (i = 0; list[i] != NULL; i++)
			_device_object_unref_locked (list[i]);

	fake_usb_unlock ();

	g_free (list);
}

libusb_device * LIBUSB_CALL
libusb_ref_device (libusb_device *dev)
{
	fake_usb_lock ();
	dev->ref_count++;
	fake_usb_unlock ();

	return dev;
}

void LIBUSB_CALL
libusb_unref_device (libusb_device *dev)
{
	if (dev == NULL)
		return;

	fake_usb_lock ();
	_device_object_unref_locked (dev);
	fake_usb_unlock ();
}

int LIBUSB_CALL
libusb_get_device_descriptor (libusb_device *dev, struct libusb_device_descriptor *desc)
{
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_DEVICE_DESCRIPTOR, dev->fake);

	error = _take_injected_error_locked (&dev->fake->injected_errors, FAKE_USB_CALL_GET_DEVICE_DESCRIPTOR, -1);
	if (error == 0)
		*desc = dev->fake->descriptor;

	fake_usb_unlock ();

	return error;
}

int LIBUSB_CALL
libusb_get_config_descriptor (libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config)
{
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_CONFIG_DESCRIPTOR, dev->fake);

	error = _take_injected_error_locked (&dev->fake->injected_errors, FAKE_USB_CALL_GET_CONFIG_DESCRIPTOR, -1);
	if (error == 0) {
		if (config_index >= dev->fake->descriptor.bNumConfigurations)
			error = LIBUSB_ERROR_NOT_FOUND;
		else
			*config = _config_descriptor_new_locked (dev->fake);
	}

	fake_usb_unlock ();

	return error;
}

int LIBUSB_CALL
libusb_get_active_config_descriptor (libusb_device *dev, struct libusb_config_descriptor **config)
{
	return libusb_get_config_descriptor (dev, 0, config);
}

int LIBUSB_CALL
libusb_get_config_descriptor_by_value (libusb_device *dev, uint8_t bConfigurationValue,
				       struct libusb_config_descriptor **config)
{
	if (bConfigurationValue != 1)
		return LIBUSB_ERROR_NOT_FOUND;

	return libusb_get_config_descriptor (dev, 0, config);
}

void LIBUSB_CALL
libusb_free_config_descriptor (struct libusb_config_descriptor *config)
{
	_count (FAKE_USB_CALL_FREE_CONFIG_DESCRIPTOR, NULL);

	_config_descriptor_free (config);
}

uint8_t LIBUSB_CALL
libusb_get_bus_number (libusb_device *dev)
{
	uint8_t bus_number;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_GET_BUS_NUMBER, dev->fake);
	bus_number = dev->fake->bus_number;
	fake_usb_unlock ();

	return bus_number;
}

uint8_t LIBUSB_CALL
libusb_get_device_address (libusb_device *dev)
{
	uint8_t device_address;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_GET_DEVICE_ADDRESS, dev->fake);
	device_address = dev->fake->device_address;
	fake_usb_unlock ();

	return device_address;
}

uint8_t LIBUSB_CALL
libusb_get_port_number (libusb_device *dev)
{
	uint8_t port_number;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_GET_PORT_NUMBER, dev->fake);
	port_number = dev->fake->port_number;
	fake_usb_unlock ();

	return port_number;
}

int LIBUSB_CALL
libusb_get_port_numbers (libusb_device *dev, uint8_t *port_numbers, int port_numbers_len)
{
	if (port_numbers == NULL || port_numbers_len <= 0)
		return LIBUSB_ERROR_INVALID_PARAM;

	port_numbers[0] = libusb_get_port_number (dev);

	return 1;
}

libusb_device * LIBUSB_CALL
libusb_get_parent (libusb_device *dev)
{
	return NULL;
}

int LIBUSB_CALL
libusb_get_device_speed (libusb_device *dev)
{
	int speed;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_GET_DEVICE_SPEED, dev->fake);
	speed = dev->fake->descriptor.bcdUSB >= 0x0300 ? LIBUSB_SPEED_SUPER : LIBUSB_SPEED_HIGH;
	fake_usb_unlock ();

	return speed;
}

int LIBUSB_CALL
libusb_get_max_packet_size (libusb_device *dev, unsigned char endpoint)
{
	FakeUsbEndpointTemplate *template;
	int result;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_MAX_PACKET_SIZE, dev->fake);

	result = _take_injected_error_locked (&dev->fake->injected_errors, FAKE_USB_CALL_GET_MAX_PACKET_SIZE,
					      endpoint);
	/* core.c reads the active config descriptor, and fails with LIBUSB_ERROR_OTHER when it cannot */
	if (result == 0 &&
	    _take_injected_error_locked (&dev->fake->injected_errors, FAKE_USB_CALL_GET_CONFIG_DESCRIPTOR, -1) != 0)
		result = LIBUSB_ERROR_OTHER;
	if (result == 0) {
		template = _find_endpoint_template_locked (dev->fake, endpoint, NULL);
		result = template != NULL ? template->max_packet_size : LIBUSB_ERROR_NOT_FOUND;
	}

	fake_usb_unlock ();

	return result;
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C

/* The string as libusb_get_device_string () gives it: as much of it as fits in @target_size bytes with a NUL, without
 * the bytes of a character that does not fit whole. Returns the size of the whole string, its NUL included, whether it
 * fits or not, as snprintf () does. */

static int
_copy_device_string (char *target, const char *source, int target_size)
{
	gsize size = strlen (source) + 1;

	if (target != NULL && target_size > 0) {
		gsize length = size - 1;

		if (size > (gsize) target_size) {
			/* The byte at target_size - 1 is the first one without room: the copy stops at the start of its
			 * character */
			const char *end = g_utf8_find_prev_char (source, source + target_size);

			length = end != NULL ? (gsize) (end - source) : 0;
		}

		memcpy (target, source, length);
		target[length] = '\0';
	}

	return (int) size;
}

int LIBUSB_CALL
libusb_get_device_string (libusb_device *dev, enum libusb_device_string_type string_type, char *data, int length)
{
	const char *string;
	int result = 0;

	if (dev == NULL || (int) string_type < 0 || (int) string_type >= LIBUSB_DEVICE_STRING_COUNT || length <= 0) {
		_count (FAKE_USB_CALL_GET_DEVICE_STRING, dev != NULL ? dev->fake : NULL);
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (data == NULL)
		length = 0;
	else
		data[0] = '\0';

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_DEVICE_STRING, dev->fake);

	if (dev->strings[string_type] == NULL) {
		/* Not cached in this device object yet: ask the device, as the platform backends do */
		result = _take_injected_error_locked (&dev->fake->injected_errors, FAKE_USB_CALL_GET_DEVICE_STRING,
						      -1);
		if (result == 0 && !_device_object_attached_locked (dev))
			result = LIBUSB_ERROR_NO_DEVICE;
		if (result == 0) {
			guint8 index;

			switch (string_type) {
				case LIBUSB_DEVICE_STRING_MANUFACTURER:
					index = dev->fake->descriptor.iManufacturer;
					break;
				case LIBUSB_DEVICE_STRING_PRODUCT:
					index = dev->fake->descriptor.iProduct;
					break;
				default:
					index = dev->fake->descriptor.iSerialNumber;
					break;
			}

			if (index == 0) {
				result = LIBUSB_ERROR_NOT_FOUND;
			} else {
				string = _device_get_string_locked (dev->fake, index);
				if (string == NULL) {
					result = LIBUSB_ERROR_IO;
				} else {
					dev->strings[string_type] = g_strdup (string);
					fake_usb_device_stat_add_locked (dev->fake,
									 FAKE_USB_STAT_DEVICE_STRING_FETCHES, 1);
				}
			}
		}
	}

	if (result == 0)
		result = _copy_device_string (data, dev->strings[string_type], length);

	fake_usb_unlock ();

	return result;
}

#endif

/* Device handles */

int LIBUSB_CALL
libusb_open (libusb_device *dev, libusb_device_handle **dev_handle)
{
	libusb_device_handle *handle;
	FakeUsbDevice *device;
	int error;

	if (dev == NULL || dev_handle == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	device = dev->fake;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_OPEN, device);

	if (!_device_object_attached_locked (dev) || dev->ctx == NULL)
		error = LIBUSB_ERROR_NO_DEVICE;
	else
		error = _take_injected_error_locked (&device->injected_errors, FAKE_USB_CALL_OPEN, -1);
	if (error == 0 && device->exclusive_open && device->n_open_handles > 0)
		error = LIBUSB_ERROR_ACCESS;

	if (error != 0) {
		fake_usb_debug ("libusb_open on device %03d:%03d failed with %d",
				device->bus_number, device->device_address, error);
		fake_usb_unlock ();
		return error;
	}

	handle = g_new0 (libusb_device_handle, 1);
	handle->dev = dev;
	handle->ctx = dev->ctx;
	dev->ref_count++;

	dev->ctx->handles = g_list_append (dev->ctx->handles, handle);
	device->n_open_handles++;
	fake_n_open_handles++;

	fake_usb_unlock ();

	*dev_handle = handle;

	return LIBUSB_SUCCESS;
}

libusb_device_handle * LIBUSB_CALL
libusb_open_device_with_vid_pid (libusb_context *ctx, uint16_t vendor_id, uint16_t product_id)
{
	libusb_device_handle *handle = NULL;
	libusb_device **devices = NULL;
	ssize_t n_devices, i;

	n_devices = libusb_get_device_list (ctx, &devices);
	if (n_devices < 0 || devices == NULL)
		return NULL;

	for (i = 0; i < n_devices && handle == NULL; i++) {
		struct libusb_device_descriptor descriptor;

		if (libusb_get_device_descriptor (devices[i], &descriptor) == LIBUSB_SUCCESS &&
		    descriptor.idVendor == vendor_id && descriptor.idProduct == product_id &&
		    libusb_open (devices[i], &handle) != LIBUSB_SUCCESS)
			handle = NULL;
	}

	libusb_free_device_list (devices, 1);

	return handle;
}

/* libusb_close () takes the event lock, after interrupting the event handler, unless it is called from a callback */

void LIBUSB_CALL
libusb_close (libusb_device_handle *dev_handle)
{
	libusb_context *ctx;

	if (dev_handle == NULL)
		return;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_CLOSE, dev_handle->dev->fake);

	ctx = dev_handle->ctx;

	if (_holds_event_lock_locked (ctx)) {
		_handle_close_locked (dev_handle);
	} else {
		ctx->device_close++;
		ctx->interrupt_serial++;
		fake_usb_signal_locked ();

		while (ctx->event_owner != NULL)
			g_cond_wait (&fake_cond, &fake_mutex);

		ctx->event_owner = g_thread_self ();
		_handle_close_locked (dev_handle);
		ctx->event_owner = NULL;
		ctx->event_lock_releases++;
		ctx->device_close--;
		fake_usb_signal_locked ();
	}

	fake_usb_unlock ();
}

libusb_device * LIBUSB_CALL
libusb_get_device (libusb_device_handle *dev_handle)
{
	_count (FAKE_USB_CALL_GET_DEVICE, dev_handle->dev->fake);

	return dev_handle->dev;
}

int LIBUSB_CALL
libusb_get_configuration (libusb_device_handle *dev_handle, int *config)
{
	if (config != NULL)
		*config = 1;

	return LIBUSB_SUCCESS;
}

int LIBUSB_CALL
libusb_set_configuration (libusb_device_handle *dev_handle, int configuration)
{
	return configuration == 1 || configuration == -1 ? LIBUSB_SUCCESS : LIBUSB_ERROR_NOT_FOUND;
}

int LIBUSB_CALL
libusb_claim_interface (libusb_device_handle *dev_handle, int interface_number)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_CLAIM_INTERFACE, device);

	/* Checks in the order of core.c, then of the backend */
	if (interface_number < 0 || interface_number >= FAKE_USB_MAX_INTERFACES)
		error = LIBUSB_ERROR_INVALID_PARAM;
	else if (!_device_object_attached_locked (dev_handle->dev))
		error = LIBUSB_ERROR_NO_DEVICE;
	else if (device->claims[interface_number] == dev_handle)
		error = LIBUSB_SUCCESS;
	else if (_find_interface_template_locked (device, interface_number) == NULL)
		error = LIBUSB_ERROR_NOT_FOUND;
	else {
		error = _take_injected_error_locked (&device->injected_errors, FAKE_USB_CALL_CLAIM_INTERFACE,
						     interface_number);
		if (error == 0 && device->claims[interface_number] != NULL)
			error = LIBUSB_ERROR_BUSY;
		if (error == 0)
			device->claims[interface_number] = dev_handle;
	}

	fake_usb_unlock ();

	return error;
}

int LIBUSB_CALL
libusb_release_interface (libusb_device_handle *dev_handle, int interface_number)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_RELEASE_INTERFACE, device);

	/* core.c keeps the interface claimed when the backend fails */
	if (interface_number < 0 || interface_number >= FAKE_USB_MAX_INTERFACES)
		error = LIBUSB_ERROR_INVALID_PARAM;
	else if (device->claims[interface_number] != dev_handle)
		error = LIBUSB_ERROR_NOT_FOUND;
	else if (!_device_object_attached_locked (dev_handle->dev))
		error = LIBUSB_ERROR_NO_DEVICE;
	else
		error = _take_injected_error_locked (&device->injected_errors, FAKE_USB_CALL_RELEASE_INTERFACE,
						     interface_number);

	if (error == 0) {
		_interface_reset_raw_io_locked (device, interface_number);
		device->claims[interface_number] = NULL;
	}

	fake_usb_unlock ();

	return error;
}

int LIBUSB_CALL
libusb_set_auto_detach_kernel_driver (libusb_device_handle *dev_handle, int enable)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER, device);

	if (!_has_capability_locked (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER))
		error = LIBUSB_ERROR_NOT_SUPPORTED;
	else
		error = _take_injected_error_locked (&device->injected_errors,
						     FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER, -1);
	if (error == 0)
		device->auto_detach_kernel_driver = enable != 0;

	fake_usb_unlock ();

	return error;
}

int LIBUSB_CALL
libusb_reset_device (libusb_device_handle *dev_handle)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int error;
	guint i;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_RESET_DEVICE, device);

	if (!_device_object_attached_locked (dev_handle->dev))
		error = LIBUSB_ERROR_NO_DEVICE;
	else
		error = _take_injected_error_locked (&device->injected_errors, FAKE_USB_CALL_RESET_DEVICE, -1);
	if (error == 0)
		for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++)
			if (!g_queue_is_empty (&device->endpoints[i].segments))
				fake_usb_device_flush_endpoint_locked (device, _slot_endpoint (i));

	fake_usb_unlock ();

	return error;
}

int LIBUSB_CALL
libusb_clear_halt (libusb_device_handle *dev_handle, unsigned char endpoint)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	guint8 interface_number = 0;
	int error;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_CLEAR_HALT, device);

	if (!_device_object_attached_locked (dev_handle->dev)) {
		error = LIBUSB_ERROR_NO_DEVICE;
	} else if (_find_endpoint_template_locked (device, endpoint, &interface_number) == NULL) {
		error = LIBUSB_ERROR_NOT_FOUND;
	} else {
		error = 0;
		if (!_interface_claimed_by_locked (device, interface_number, dev_handle)) {
			_misuse_locked (device, "libusb_clear_halt () on endpoint 0x%02x of interface %u, "
					"which the handle did not claim", endpoint, interface_number);
			/* The WinUSB and macOS backends look the endpoint up in the claimed interfaces; on Linux, the
			 * kernel claims the interface */
			if (fake_platform != FAKE_USB_PLATFORM_LINUX)
				error = LIBUSB_ERROR_NOT_FOUND;
		}
		if (error == 0)
			error = _take_injected_error_locked (&device->injected_errors, FAKE_USB_CALL_CLEAR_HALT,
							     endpoint);
	}
	if (error == 0)
		_endpoint_set_halt_locked (device, endpoint, FALSE);

	fake_usb_unlock ();

	return error;
}

/* libusb_get_string_descriptor_ascii () of descriptor.c: two control transfers, the language table then the string */

int LIBUSB_CALL
libusb_get_string_descriptor_ascii (libusb_device_handle *dev_handle, uint8_t desc_index,
				    unsigned char *data, int length)
{
	unsigned char buffer[FAKE_USB_STRING_BUFFER_SIZE];
	FakeUsbDevice *device;
	guint16 langid;
	int result, n_chars, i;

	if (dev_handle == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	device = dev_handle->dev->fake;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_STRING_DESCRIPTOR_ASCII, device);

	if (_synchronous_from_callback_locked (dev_handle->ctx, device))
		result = LIBUSB_ERROR_BUSY;
	else
		result = _take_injected_error_locked (&device->injected_errors,
						      FAKE_USB_CALL_GET_STRING_DESCRIPTOR_ASCII, desc_index);

	fake_usb_unlock ();

	if (result != 0)
		return result;

	if (desc_index == 0 || data == NULL || length <= 0)
		return LIBUSB_ERROR_INVALID_PARAM;

	result = libusb_control_transfer (dev_handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
					  LIBUSB_DT_STRING << 8, 0, buffer, 4, FAKE_USB_STRING_TIMEOUT_MS);
	if (result < 0)
		return result;
	if (result != 4 || buffer[0] < 4 || buffer[1] != LIBUSB_DT_STRING)
		return LIBUSB_ERROR_IO;

	langid = buffer[2] | (buffer[3] << 8);

	result = libusb_control_transfer (dev_handle, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
					  (LIBUSB_DT_STRING << 8) | desc_index, langid, buffer, sizeof (buffer),
					  FAKE_USB_STRING_TIMEOUT_MS);
	if (result < 0)
		return result;
	if (result < 2 || buffer[0] > result || buffer[1] != LIBUSB_DT_STRING)
		return LIBUSB_ERROR_IO;

	n_chars = MIN (length - 1, (buffer[0] - 2) / 2);
	for (i = 0; i < n_chars; i++) {
		guint16 c = buffer[2 + 2 * i] | (buffer[3 + 2 * i] << 8);

		data[i] = c < 0x80 ? (unsigned char) c : '?';
	}
	data[n_chars] = '\0';

	return n_chars;
}

/* Transfer API */

/* auto_claim () of the WinUSB backend: a control transfer needs a claimed interface, so libusb claims the first one
 * it can, for the duration of the transfer */

static int
_auto_claim_locked (FakeUsbDevice *device, libusb_device_handle *handle)
{
	guint i;

	for (i = 0; i < device->interfaces->len; i++) {
		FakeUsbInterfaceTemplate *template = g_ptr_array_index (device->interfaces, i);

		if (template->number < FAKE_USB_MAX_INTERFACES && device->claims[template->number] == NULL) {
			device->claims[template->number] = handle;
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_AUTO_CLAIMS, 1);
			return template->number;
		}
	}

	return -1;
}

/* usbfs memory, see fake_usb_set_submit_memory_limit () */

static int
_charge_locked (FakeUsbTransferPriv *priv)
{
	gsize length = (gsize) priv->transfer->length;

	if (fake_submit_memory_limit > 0 && fake_submit_memory_pending + length > fake_submit_memory_limit) {
		fake_usb_device_stat_add_locked (priv->device, FAKE_USB_STAT_MEMORY_LIMIT_REJECTIONS, 1);
		return fake_submit_memory_error != 0 ? fake_submit_memory_error : LIBUSB_ERROR_NO_MEM;
	}

	fake_submit_memory_pending += length;
	priv->charged = length;

	return LIBUSB_SUCCESS;
}

/* A transfer on a device that is gone, with a submit or synchronous error of 0: accepted, then it fails at once, or
 * never (fake_usb_device_set_unplug_transfers_hang ()) */

static int
_transfer_start_detached_locked (FakeUsbTransferPriv *priv)
{
	struct libusb_transfer *transfer = priv->transfer;
	FakeUsbDevice *device = priv->device;
	int error;

	error = priv->is_synchronous ? _unplug_synchronous_error_locked (device) : _unplug_submit_error_locked (device);
	if (error != 0)
		return error;

	if (device->unplug_transfers_hang) {
		priv->queue = transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL ? &device->control_pending :
			&fake_usb_device_get_endpoint_locked (device, transfer->endpoint)->pending;
		priv->due = G_MAXINT64;
		priv->state = FAKE_USB_TRANSFER_PENDING;
		g_queue_push_tail (priv->queue, priv);
		return LIBUSB_SUCCESS;
	}

	_transfer_complete_locked (priv, _unplug_status_locked (device));

	return LIBUSB_SUCCESS;
}

static int
_control_transfer_start_locked (FakeUsbTransferPriv *priv, gint64 now)
{
	struct libusb_transfer *transfer = priv->transfer;
	FakeUsbDevice *device = priv->device;
	struct libusb_control_setup *setup;
	guint16 length;

	if (transfer->length < LIBUSB_CONTROL_SETUP_SIZE || transfer->buffer == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	setup = (struct libusb_control_setup *) transfer->buffer;
	length = libusb_le16_to_cpu (setup->wLength);
	if (transfer->length < LIBUSB_CONTROL_SETUP_SIZE + length)
		return LIBUSB_ERROR_INVALID_PARAM;

	/* winusbx_submit_control_transfer () sends the request through a claimed interface */
	if (fake_platform == FAKE_USB_PLATFORM_WINDOWS && _first_claimed_interface_locked (device, priv->handle) < 0) {
		priv->auto_claimed_interface = _auto_claim_locked (device, priv->handle);
		if (priv->auto_claimed_interface < 0)
			return LIBUSB_ERROR_NOT_FOUND;
	}

	fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_CONTROL_REQUESTS, 1);
	if ((setup->bmRequestType & LIBUSB_ENDPOINT_IN) != 0 &&
	    setup->bRequest == LIBUSB_REQUEST_GET_DESCRIPTOR &&
	    (libusb_le16_to_cpu (setup->wValue) >> 8) == LIBUSB_DT_STRING)
		fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_STRING_REQUESTS, 1);

	priv->due = device->control_delay_ms == FAKE_USB_NEVER ? G_MAXINT64 :
		now + fake_usb_scaled_us_locked ((guint64) device->control_delay_ms * 1000);
	priv->queue = &device->control_pending;
	priv->state = FAKE_USB_TRANSFER_PENDING;
	g_queue_push_tail (priv->queue, priv);

	_pump_locked ();

	return LIBUSB_SUCCESS;
}

static int
_transfer_start_locked (FakeUsbTransferPriv *priv, gboolean is_synchronous, FakeUsbCall call)
{
	struct libusb_transfer *transfer = priv->transfer;
	libusb_device_handle *handle = transfer->dev_handle;
	FakeUsbDevice *device = handle->dev->fake;
	FakeUsbEndpointTemplate *template;
	FakeUsbEndpointState *state;
	guint8 interface_number = 0;
	gboolean is_in;
	int error;
	gint64 now;

	if (transfer->length < 0)
		return LIBUSB_ERROR_INVALID_PARAM;

	if (priv->device != device) {
		if (priv->device != NULL)
			fake_usb_device_unref_locked (priv->device);
		priv->device = fake_usb_device_ref_locked (device);
	}

	now = g_get_monotonic_time ();

	priv->handle = handle;
	priv->ctx = handle->ctx;
	priv->is_synchronous = is_synchronous;
	priv->synchronous_done = FALSE;
	priv->transferred = 0;
	priv->auto_claimed_interface = -1;
	priv->deadline = transfer->timeout > 0 ? now + (gint64) transfer->timeout * G_TIME_SPAN_MILLISECOND : G_MAXINT64;
	priv->due = now;
	transfer->actual_length = 0;

	if (!_device_object_attached_locked (handle->dev))
		return _transfer_start_detached_locked (priv);

	error = _take_injected_error_locked (&device->injected_errors, call, transfer->endpoint);
	if (error != 0)
		return error;

	if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL)
		return _control_transfer_start_locked (priv, now);

	if (transfer->type != LIBUSB_TRANSFER_TYPE_BULK && transfer->type != LIBUSB_TRANSFER_TYPE_INTERRUPT)
		return LIBUSB_ERROR_NOT_SUPPORTED;

	template = _find_endpoint_template_locked (device, transfer->endpoint, &interface_number);
	if (template == NULL)
		return LIBUSB_ERROR_NOT_FOUND;

	if (!_interface_claimed_by_locked (device, interface_number, handle)) {
		_misuse_locked (device, "transfer on endpoint 0x%02x of interface %u, which the handle did not claim",
				transfer->endpoint, interface_number);
		/* The WinUSB and macOS backends look the endpoint up in the claimed interfaces; on Linux, the kernel
		 * claims the interface */
		if (fake_platform != FAKE_USB_PLATFORM_LINUX)
			return LIBUSB_ERROR_NOT_FOUND;
	}

	is_in = (transfer->endpoint & LIBUSB_ENDPOINT_IN) != 0;
	state = fake_usb_device_get_endpoint_locked (device, transfer->endpoint);

	if (is_in) {
		gsize max_packet_size = template->max_packet_size > 0 ? template->max_packet_size :
			FAKE_USB_DEFAULT_MAX_PACKET_SIZE;

		if (state->raw_io &&
		    ((gsize) transfer->length % max_packet_size != 0 ||
		     transfer->length > device->max_raw_io_transfer_size)) {
			/* WinUSB's ReadPipe rejects these when RAW_IO is enabled, and libusb returns the failure
			 * as LIBUSB_ERROR_IO */
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS, 1);
			fake_usb_debug ("RAW_IO read of %d bytes on endpoint 0x%02x rejected", transfer->length,
					transfer->endpoint);
			return LIBUSB_ERROR_IO;
		}

		if (_is_stream_endpoint (device, transfer->endpoint)) {
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_STREAM_READS, 1);
			if ((gsize) transfer->length % max_packet_size != 0)
				fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_UNALIGNED_STREAM_READS, 1);
			_stat_max_locked (device, FAKE_USB_STAT_MAX_STREAM_READ, (guint64) transfer->length);
		}
	}

	error = _charge_locked (priv);
	if (error != 0)
		return error;

	if (state->n_injected_statuses != 0) {
		if (state->n_injected_statuses > 0)
			state->n_injected_statuses--;
		_transfer_complete_locked (priv, state->injected_status);
		return LIBUSB_SUCCESS;
	}

	if (state->halted) {
		_transfer_complete_locked (priv, LIBUSB_TRANSFER_STALL);
		_endpoint_auto_clear_stall_locked (device, transfer->endpoint);
		return LIBUSB_SUCCESS;
	}

	if (!is_in) {
		/* The device always accepts OUT data at once */
		priv->transferred = transfer->length;
		if (device->camera != NULL)
			fake_usb_camera_receive_locked (device, transfer->endpoint, transfer->buffer,
							transfer->length, now);
		_transfer_complete_locked (priv, LIBUSB_TRANSFER_COMPLETED);
		_pump_locked ();
		return LIBUSB_SUCCESS;
	}

	priv->queue = &state->pending;
	priv->state = FAKE_USB_TRANSFER_PENDING;
	g_queue_push_tail (priv->queue, priv);

	if (_is_stream_endpoint (device, transfer->endpoint))
		_stat_max_locked (device, FAKE_USB_STAT_MAX_PENDING_STREAM_READS, state->pending.length);

	_pump_locked ();
	fake_usb_signal_locked ();

	return LIBUSB_SUCCESS;
}

struct libusb_transfer * LIBUSB_CALL
libusb_alloc_transfer (int iso_packets)
{
	FakeUsbTransferPriv *priv;

	if (iso_packets < 0)
		return NULL;

	priv = _transfer_priv_new (iso_packets);
	priv->counted = TRUE;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_ALLOC_TRANSFER, NULL);
	fake_n_transfers++;
	fake_usb_unlock ();

	return priv->transfer;
}

void LIBUSB_CALL
libusb_free_transfer (struct libusb_transfer *transfer)
{
	FakeUsbTransferPriv *priv;

	if (transfer == NULL)
		return;

	priv = _transfer_get_priv (transfer);

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_FREE_TRANSFER, priv->device);

	switch (priv->state) {
		case FAKE_USB_TRANSFER_PENDING:
			g_queue_remove (priv->queue, priv);
			_misuse_locked (priv->device, "libusb_free_transfer () of an in-flight transfer");
			break;
		case FAKE_USB_TRANSFER_COMPLETED:
			g_queue_remove (&priv->ctx->completed, priv);
			_misuse_locked (priv->device, "libusb_free_transfer () of a transfer waiting for its callback");
			break;
		case FAKE_USB_TRANSFER_DISPATCHING:
			g_queue_remove (&priv->ctx->dispatching, priv);
			_misuse_locked (priv->device, "libusb_free_transfer () of a transfer being dispatched");
			break;
		default:
			break;
	}

	_transfer_priv_free_locked (priv);

	fake_usb_unlock ();
}

int LIBUSB_CALL
libusb_submit_transfer (struct libusb_transfer *transfer)
{
	FakeUsbTransferPriv *priv;
	FakeUsbDevice *device;
	int result;

	if (transfer == NULL || transfer->dev_handle == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	priv = _transfer_get_priv (transfer);
	device = transfer->dev_handle->dev->fake;

	_call_transfer_hook (device, transfer->endpoint, transfer->length, FALSE);

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_SUBMIT_TRANSFER, device);

	switch (priv->state) {
		case FAKE_USB_TRANSFER_PENDING:
		case FAKE_USB_TRANSFER_COMPLETED:
		case FAKE_USB_TRANSFER_DISPATCHING:
			_misuse_locked (device, "libusb_submit_transfer () of an in-flight transfer");
			result = LIBUSB_ERROR_BUSY;
			break;
		default:
			result = _transfer_start_locked (priv, FALSE, FAKE_USB_CALL_SUBMIT_TRANSFER);
			break;
	}

	fake_usb_unlock ();

	return result;
}

int LIBUSB_CALL
libusb_cancel_transfer (struct libusb_transfer *transfer)
{
	FakeUsbTransferPriv *priv;
	int result;

	if (transfer == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	priv = _transfer_get_priv (transfer);

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_CANCEL_TRANSFER, priv->device);

	if (priv->state == FAKE_USB_TRANSFER_PENDING && !priv->is_synchronous) {
		g_queue_remove (priv->queue, priv);
		_transfer_complete_locked (priv, LIBUSB_TRANSFER_CANCELLED);
		result = LIBUSB_SUCCESS;
	} else {
		/* Not in flight, or already completed and waiting for its callback */
		result = LIBUSB_ERROR_NOT_FOUND;
	}

	fake_usb_unlock ();

	return result;
}

/* The synchronous API of sync.c: an asynchronous transfer, then event handling until it completes */

static int
_synchronous_transfer (libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length,
		       int *transferred, unsigned int timeout, unsigned char type, FakeUsbCall call)
{
	FakeUsbTransferPriv *priv;
	FakeUsbDevice *device;
	int result;

	if (transferred != NULL)
		*transferred = 0;

	if (dev_handle == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	device = dev_handle->dev->fake;

	_call_transfer_hook (device, endpoint, length, TRUE);

	fake_usb_lock ();

	_count_locked (call, device);

	if (_synchronous_from_callback_locked (dev_handle->ctx, device)) {
		fake_usb_unlock ();
		return LIBUSB_ERROR_BUSY;
	}

	priv = _transfer_priv_new (0);
	priv->transfer->dev_handle = dev_handle;
	priv->transfer->endpoint = endpoint;
	priv->transfer->type = type;
	priv->transfer->timeout = timeout;
	priv->transfer->buffer = data;
	priv->transfer->length = length;

	result = _transfer_start_locked (priv, TRUE, call);
	if (result == LIBUSB_SUCCESS) {
		_wait_synchronous_locked (priv);

		if (transferred != NULL)
			*transferred = priv->transfer->actual_length;
		result = _transfer_status_to_error (priv->transfer->status);
	}

	_transfer_priv_free_locked (priv);

	fake_usb_unlock ();

	return result;
}

int LIBUSB_CALL
libusb_bulk_transfer (libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length,
		      int *actual_length, unsigned int timeout)
{
	return _synchronous_transfer (dev_handle, endpoint, data, length, actual_length, timeout,
				      LIBUSB_TRANSFER_TYPE_BULK, FAKE_USB_CALL_BULK_TRANSFER);
}

int LIBUSB_CALL
libusb_interrupt_transfer (libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data,
			   int length, int *actual_length, unsigned int timeout)
{
	return _synchronous_transfer (dev_handle, endpoint, data, length, actual_length, timeout,
				      LIBUSB_TRANSFER_TYPE_INTERRUPT, FAKE_USB_CALL_INTERRUPT_TRANSFER);
}

int LIBUSB_CALL
libusb_control_transfer (libusb_device_handle *dev_handle, uint8_t request_type, uint8_t bRequest,
			 uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength,
			 unsigned int timeout)
{
	struct libusb_control_setup *setup;
	FakeUsbTransferPriv *priv;
	FakeUsbDevice *device;
	unsigned char *buffer;
	int result;

	if (dev_handle == NULL)
		return LIBUSB_ERROR_INVALID_PARAM;

	device = dev_handle->dev->fake;

	_call_transfer_hook (device, request_type & LIBUSB_ENDPOINT_IN, wLength, TRUE);

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_CONTROL_TRANSFER, device);

	if (_synchronous_from_callback_locked (dev_handle->ctx, device)) {
		fake_usb_unlock ();
		return LIBUSB_ERROR_BUSY;
	}

	if (wLength > 0 && data == NULL) {
		fake_usb_unlock ();
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	buffer = g_malloc0 (LIBUSB_CONTROL_SETUP_SIZE + wLength);
	setup = (struct libusb_control_setup *) buffer;
	setup->bmRequestType = request_type;
	setup->bRequest = bRequest;
	setup->wValue = libusb_cpu_to_le16 (wValue);
	setup->wIndex = libusb_cpu_to_le16 (wIndex);
	setup->wLength = libusb_cpu_to_le16 (wLength);
	if ((request_type & LIBUSB_ENDPOINT_IN) == 0 && wLength > 0)
		memcpy (buffer + LIBUSB_CONTROL_SETUP_SIZE, data, wLength);

	priv = _transfer_priv_new (0);
	priv->transfer->dev_handle = dev_handle;
	priv->transfer->endpoint = 0;
	priv->transfer->type = LIBUSB_TRANSFER_TYPE_CONTROL;
	priv->transfer->timeout = timeout;
	priv->transfer->buffer = buffer;
	priv->transfer->length = LIBUSB_CONTROL_SETUP_SIZE + wLength;

	result = _transfer_start_locked (priv, TRUE, FAKE_USB_CALL_CONTROL_TRANSFER);
	if (result == LIBUSB_SUCCESS) {
		_wait_synchronous_locked (priv);

		if ((request_type & LIBUSB_ENDPOINT_IN) != 0 && priv->transfer->actual_length > 0)
			memcpy (data, buffer + LIBUSB_CONTROL_SETUP_SIZE, priv->transfer->actual_length);

		if (priv->transfer->status == LIBUSB_TRANSFER_COMPLETED)
			result = priv->transfer->actual_length;
		else
			result = _transfer_status_to_error (priv->transfer->status);
	}

	_transfer_priv_free_locked (priv);

	fake_usb_unlock ();

	g_free (buffer);

	return result;
}

/* Events */

static int
_handle_events (libusb_context *ctx, struct timeval *tv, int *completed)
{
	FakeUsbHandleEventsHook hook;
	gpointer hook_data;
	gint64 timeout_us = FAKE_USB_DEFAULT_EVENT_TIMEOUT;
	GList *iter;
	int result;

	if (tv != NULL) {
		/* TIMEVAL_IS_VALID () */
		if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= G_USEC_PER_SEC)
			return LIBUSB_ERROR_INVALID_PARAM;
		timeout_us = (gint64) tv->tv_sec * G_USEC_PER_SEC + tv->tv_usec;
	}

	fake_usb_lock ();

	ctx = _resolve_context_locked (ctx);
	if (ctx == NULL) {
		fake_usb_unlock ();
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	_count_locked (FAKE_USB_CALL_HANDLE_EVENTS, NULL);
	for (iter = ctx->handles; iter != NULL; iter = iter->next) {
		libusb_device_handle *handle = iter->data;

		handle->dev->fake->calls[FAKE_USB_CALL_HANDLE_EVENTS]++;
	}

	hook = fake_handle_events_hook;
	hook_data = fake_handle_events_hook_data;

	fake_usb_unlock ();

	if (hook != NULL)
		hook (ctx, hook_data);

	fake_usb_lock ();

	_record_event_thread_priority_locked (ctx);

	result = _handle_events_locked (ctx, timeout_us, completed);
	if (result == LIBUSB_ERROR_BUSY) {
		fake_usb_device_stat_add_locked (NULL, FAKE_USB_STAT_SYNC_IN_EVENT_HANDLER, 1);
		_misuse_locked (NULL, "libusb_handle_events*() from a libusb callback "
				"(real libusb deadlocks on Linux, returns LIBUSB_ERROR_BUSY on Windows)");
	}

	fake_usb_unlock ();

	return result;
}

int LIBUSB_CALL
libusb_handle_events_timeout_completed (libusb_context *ctx, struct timeval *tv, int *completed)
{
	return _handle_events (ctx, tv, completed);
}

int LIBUSB_CALL
libusb_handle_events_timeout (libusb_context *ctx, struct timeval *tv)
{
	return _handle_events (ctx, tv, NULL);
}

int LIBUSB_CALL
libusb_handle_events (libusb_context *ctx)
{
	return _handle_events (ctx, NULL, NULL);
}

int LIBUSB_CALL
libusb_handle_events_completed (libusb_context *ctx, int *completed)
{
	return _handle_events (ctx, NULL, completed);
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000105
void LIBUSB_CALL
libusb_interrupt_event_handler (libusb_context *ctx)
{
	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_INTERRUPT_EVENT_HANDLER, NULL);

	ctx = _resolve_context_locked (ctx);
	if (ctx != NULL) {
		ctx->interrupt_serial++;
		fake_usb_signal_locked ();
	}

	fake_usb_unlock ();
}
#endif

/* Hotplug API */

int LIBUSB_CALL
libusb_hotplug_register_callback (libusb_context *ctx, FakeUsbHotplugEvents hotplug_events,
				  FakeUsbHotplugFlags hotplug_flags, int vendor_id, int product_id,
				  int dev_class, libusb_hotplug_callback_fn cb_fn, void *user_data,
				  libusb_hotplug_callback_handle *callback_handle)
{
	FakeUsbHotplugCallback *callback;
	GPtrArray *enumerated = NULL;
	int events = (int) hotplug_events;
	int flags = (int) hotplug_flags;
	int error = 0;
	guint i;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK, NULL);

	ctx = _resolve_context_locked (ctx);

	if (ctx == NULL || events == 0 ||
	    (events & ~(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)) != 0 ||
	    (flags & ~LIBUSB_HOTPLUG_ENUMERATE) != 0 ||
	    (vendor_id != LIBUSB_HOTPLUG_MATCH_ANY && (vendor_id & ~0xffff) != 0) ||
	    (product_id != LIBUSB_HOTPLUG_MATCH_ANY && (product_id & ~0xffff) != 0) ||
	    (dev_class != LIBUSB_HOTPLUG_MATCH_ANY && (dev_class & ~0xff) != 0) ||
	    cb_fn == NULL)
		error = LIBUSB_ERROR_INVALID_PARAM;
	else if (!_has_capability_locked (LIBUSB_CAP_HAS_HOTPLUG))
		error = LIBUSB_ERROR_NOT_SUPPORTED;
	else
		error = _take_injected_error_locked (&fake_global_injected_errors,
						     FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK, -1);

	if (error != 0) {
		fake_usb_unlock ();
		return error;
	}

	callback = g_new0 (FakeUsbHotplugCallback, 1);
	callback->handle = ctx->next_hotplug_handle++;
	callback->events = events;
	callback->flags = flags;
	callback->vendor_id = vendor_id;
	callback->product_id = product_id;
	callback->dev_class = dev_class;
	callback->callback = cb_fn;
	callback->user_data = user_data;

	if (ctx->next_hotplug_handle < 0)
		ctx->next_hotplug_handle = 1;

	ctx->hotplug_callbacks = g_list_append (ctx->hotplug_callbacks, callback);

	if ((flags & LIBUSB_HOTPLUG_ENUMERATE) != 0 && (events & LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) != 0) {
		enumerated = g_ptr_array_new ();
		for (i = 0; fake_devices != NULL && i < fake_devices->len; i++) {
			FakeUsbDevice *device = g_ptr_array_index (fake_devices, i);

			if (device->plugged &&
			    _hotplug_callback_matches (callback, device, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED))
				g_ptr_array_add (enumerated,
						 _context_get_device_locked (ctx, device, device->generation));
		}
		ctx->hotplug_dispatch_depth++;
	}

	if (callback_handle != NULL)
		*callback_handle = callback->handle;

	fake_usb_unlock ();

	if (enumerated != NULL) {
		/* Like libusb, the return value of the callback is ignored during enumeration */
		for (i = 0; i < enumerated->len; i++)
			cb_fn (ctx, g_ptr_array_index (enumerated, i), LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, user_data);

		fake_usb_lock ();
		for (i = 0; i < enumerated->len; i++)
			_device_object_unref_locked (g_ptr_array_index (enumerated, i));
		ctx->hotplug_dispatch_depth--;
		_hotplug_cleanup_locked (ctx);
		fake_usb_unlock ();

		g_ptr_array_unref (enumerated);
	}

	return LIBUSB_SUCCESS;
}

void LIBUSB_CALL
libusb_hotplug_deregister_callback (libusb_context *ctx, libusb_hotplug_callback_handle callback_handle)
{
	gboolean found = FALSE;
	GList *iter;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK, NULL);

	ctx = _resolve_context_locked (ctx);
	if (ctx == NULL) {
		fake_usb_unlock ();
		return;
	}

	/* Without the capability nothing was registered, and there is nothing to find */
	for (iter = ctx->hotplug_callbacks; iter != NULL; iter = iter->next) {
		FakeUsbHotplugCallback *callback = iter->data;

		if (callback->handle == callback_handle && !callback->deregistered) {
			callback->deregistered = TRUE;
			found = TRUE;
			break;
		}
	}

	if (found) {
		/* libusb signals its event source, waking up event handlers */
		ctx->interrupt_serial++;
		fake_usb_signal_locked ();
	}

	_hotplug_cleanup_locked (ctx);

	fake_usb_unlock ();
}

/* Miscellaneous */

int LIBUSB_CALL
libusb_has_capability (uint32_t capability)
{
	int result;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_HAS_CAPABILITY, NULL);
	result = _has_capability_locked (capability) ? 1 : 0;
	fake_usb_unlock ();

	return result;
}

const struct libusb_version * LIBUSB_CALL
libusb_get_version (void)
{
	const struct libusb_version *version;

	fake_usb_lock ();
	_count_locked (FAKE_USB_CALL_GET_VERSION, NULL);
	version = fake_version != NULL ? fake_version : _default_version ();
	fake_usb_unlock ();

	return version;
}

const char * LIBUSB_CALL
libusb_error_name (int error_code)
{
	_count (FAKE_USB_CALL_ERROR_NAME, NULL);

	switch (error_code) {
		case LIBUSB_ERROR_IO:
			return "LIBUSB_ERROR_IO";
		case LIBUSB_ERROR_INVALID_PARAM:
			return "LIBUSB_ERROR_INVALID_PARAM";
		case LIBUSB_ERROR_ACCESS:
			return "LIBUSB_ERROR_ACCESS";
		case LIBUSB_ERROR_NO_DEVICE:
			return "LIBUSB_ERROR_NO_DEVICE";
		case LIBUSB_ERROR_NOT_FOUND:
			return "LIBUSB_ERROR_NOT_FOUND";
		case LIBUSB_ERROR_BUSY:
			return "LIBUSB_ERROR_BUSY";
		case LIBUSB_ERROR_TIMEOUT:
			return "LIBUSB_ERROR_TIMEOUT";
		case LIBUSB_ERROR_OVERFLOW:
			return "LIBUSB_ERROR_OVERFLOW";
		case LIBUSB_ERROR_PIPE:
			return "LIBUSB_ERROR_PIPE";
		case LIBUSB_ERROR_INTERRUPTED:
			return "LIBUSB_ERROR_INTERRUPTED";
		case LIBUSB_ERROR_NO_MEM:
			return "LIBUSB_ERROR_NO_MEM";
		case LIBUSB_ERROR_NOT_SUPPORTED:
			return "LIBUSB_ERROR_NOT_SUPPORTED";
		case LIBUSB_ERROR_OTHER:
			return "LIBUSB_ERROR_OTHER";
		case LIBUSB_TRANSFER_ERROR:
			return "LIBUSB_TRANSFER_ERROR";
		case LIBUSB_TRANSFER_TIMED_OUT:
			return "LIBUSB_TRANSFER_TIMED_OUT";
		case LIBUSB_TRANSFER_CANCELLED:
			return "LIBUSB_TRANSFER_CANCELLED";
		case LIBUSB_TRANSFER_STALL:
			return "LIBUSB_TRANSFER_STALL";
		case LIBUSB_TRANSFER_NO_DEVICE:
			return "LIBUSB_TRANSFER_NO_DEVICE";
		case LIBUSB_TRANSFER_OVERFLOW:
			return "LIBUSB_TRANSFER_OVERFLOW";
		case 0:
			return "LIBUSB_SUCCESS";
		default:
			return "unknown libusb code";
	}
}

const char * LIBUSB_CALL
libusb_strerror (FakeUsbErrorCode errcode)
{
	/* The fake's own wording, indexed by minus the error code; anything else is LIBUSB_ERROR_OTHER */
	static const char * const messages[] = {
		"No error",			/* LIBUSB_SUCCESS */
		"Input or output failed",	/* LIBUSB_ERROR_IO */
		"Bad argument",			/* LIBUSB_ERROR_INVALID_PARAM */
		"Permission denied",		/* LIBUSB_ERROR_ACCESS */
		"Device gone",			/* LIBUSB_ERROR_NO_DEVICE */
		"Not found",			/* LIBUSB_ERROR_NOT_FOUND */
		"Busy",				/* LIBUSB_ERROR_BUSY */
		"Timed out",			/* LIBUSB_ERROR_TIMEOUT */
		"More data than room for it",	/* LIBUSB_ERROR_OVERFLOW */
		"Endpoint halted",		/* LIBUSB_ERROR_PIPE */
		"Interrupted",			/* LIBUSB_ERROR_INTERRUPTED */
		"Out of memory",		/* LIBUSB_ERROR_NO_MEM */
		"Not supported here",		/* LIBUSB_ERROR_NOT_SUPPORTED */
		"Unspecified error"		/* LIBUSB_ERROR_OTHER */
	};
	int index = -(int) errcode;

	_count (FAKE_USB_CALL_STRERROR, NULL);

	if (index < 0 || index >= (int) G_N_ELEMENTS (messages))
		index = G_N_ELEMENTS (messages) - 1;

	return messages[index];
}

int LIBUSB_CALL
libusb_setlocale (const char *locale)
{
	return LIBUSB_SUCCESS;
}

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000105
unsigned char * LIBUSB_CALL
libusb_dev_mem_alloc (libusb_device_handle *dev_handle, size_t length)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	unsigned char *buffer = NULL;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_DEV_MEM_ALLOC, device);

	if (_dev_mem_supported_locked () &&
	    _device_object_attached_locked (dev_handle->dev) &&
	    _take_injected_error_locked (&device->injected_errors, FAKE_USB_CALL_DEV_MEM_ALLOC, -1) == 0) {
		buffer = g_malloc0 (MAX (length, 1));
		if (fake_dev_mem_blocks == NULL)
			fake_dev_mem_blocks = g_hash_table_new (g_direct_hash, g_direct_equal);
		g_hash_table_add (fake_dev_mem_blocks, buffer);
	}

	fake_usb_unlock ();

	return buffer;
}

int LIBUSB_CALL
libusb_dev_mem_free (libusb_device_handle *dev_handle, unsigned char *buffer, size_t length)
{
	int result = LIBUSB_SUCCESS;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_DEV_MEM_FREE, dev_handle != NULL ? dev_handle->dev->fake : NULL);

	if (fake_dev_mem_blocks != NULL && g_hash_table_remove (fake_dev_mem_blocks, buffer))
		g_free (buffer);
	else if (!_dev_mem_supported_locked ())
		result = LIBUSB_ERROR_NOT_SUPPORTED;

	fake_usb_unlock ();

	return result;
}
#endif

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C

/* RAW_IO, as implemented by the WinUSB backend of libusb 1.0.30 (winusbx_endpoint_set_raw_io () and friends): the
 * endpoint must be one of an interface the handle claimed; a policy call on a device that is gone fails with
 * LIBUSB_ERROR_OTHER (SetPipePolicy () fails, with no specific mapping) */

static int
_raw_io_check_locked (libusb_device_handle *dev_handle, uint8_t endpoint, FakeUsbCall call, gboolean require_in)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	guint8 interface_number = 0;

	if ((endpoint & ~(LIBUSB_ENDPOINT_DIR_MASK | LIBUSB_ENDPOINT_ADDRESS_MASK)) != 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (require_in && (endpoint & LIBUSB_ENDPOINT_IN) == 0)
		return LIBUSB_ERROR_INVALID_PARAM;

	if (_find_endpoint_template_locked (device, endpoint, &interface_number) == NULL ||
	    !_interface_claimed_by_locked (device, interface_number, dev_handle))
		return LIBUSB_ERROR_NOT_FOUND;

	return _take_injected_error_locked (&device->injected_errors, call, endpoint);
}

int LIBUSB_CALL
libusb_endpoint_supports_raw_io (libusb_device_handle *dev_handle, uint8_t endpoint)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int result;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO, device);

	if (!_raw_io_supported_locked (device)) {
		result = 0;
	} else {
		/* No device access: still 1 once the device is gone */
		result = _raw_io_check_locked (dev_handle, endpoint, FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO, TRUE);
		if (result == LIBUSB_SUCCESS)
			result = 1;
	}

	fake_usb_unlock ();

	return result;
}

int LIBUSB_CALL
libusb_endpoint_set_raw_io (libusb_device_handle *dev_handle, uint8_t endpoint, int enable)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int result;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_ENDPOINT_SET_RAW_IO, device);

	if (!_raw_io_supported_locked (device))
		result = LIBUSB_ERROR_NOT_SUPPORTED;
	else
		result = _raw_io_check_locked (dev_handle, endpoint, FAKE_USB_CALL_ENDPOINT_SET_RAW_IO, TRUE);
	if (result == LIBUSB_SUCCESS && !_device_object_attached_locked (dev_handle->dev))
		result = LIBUSB_ERROR_OTHER;

	if (result == LIBUSB_SUCCESS) {
		FakeUsbEndpointState *state = fake_usb_device_get_endpoint_locked (device, endpoint);

		if (!g_queue_is_empty (&state->pending)) {
			/* libusb documents that the option "should not be changed when any transfer is in
			 * progress", and checks nothing */
			fake_usb_device_stat_add_locked (device, FAKE_USB_STAT_RAW_IO_CHANGES_WHILE_PENDING, 1);
			if (device->raw_io_pending_change_error != 0)
				result = device->raw_io_pending_change_error;
			else
				_misuse_locked (device, "RAW_IO change on endpoint 0x%02x with %u transfer(s) pending",
						endpoint, state->pending.length);
		}

		if (result == LIBUSB_SUCCESS)
			state->raw_io = enable != 0;
	}

	fake_usb_unlock ();

	return result;
}

int LIBUSB_CALL
libusb_get_max_raw_io_transfer_size (libusb_device_handle *dev_handle, uint8_t endpoint)
{
	FakeUsbDevice *device = dev_handle->dev->fake;
	int result;

	fake_usb_lock ();

	_count_locked (FAKE_USB_CALL_GET_MAX_RAW_IO_TRANSFER_SIZE, device);

	if (!_raw_io_supported_locked (device))
		result = LIBUSB_ERROR_NOT_SUPPORTED;
	else
		result = _raw_io_check_locked (dev_handle, endpoint, FAKE_USB_CALL_GET_MAX_RAW_IO_TRANSFER_SIZE,
					       FALSE);
	if (result == LIBUSB_SUCCESS && !_device_object_attached_locked (dev_handle->dev))
		result = LIBUSB_ERROR_OTHER;
	if (result == LIBUSB_SUCCESS)
		result = device->max_raw_io_transfer_size;

	fake_usb_unlock ();

	return result;
}

#endif

/* Scripting and inspection API, see fakeusb.h */

void
fake_usb_reset (void)
{
	guint i;

	fake_usb_lock ();

	while (fake_devices != NULL && fake_devices->len > 0)
		_device_remove_locked (g_ptr_array_index (fake_devices, fake_devices->len - 1));

	fake_platform = FAKE_USB_PLATFORM_HOST;
	for (i = 0; i < FAKE_USB_N_CAPABILITIES; i++)
		fake_capabilities[i] = -1;
	fake_dev_mem_supported = -1;
	fake_version = NULL;
	g_list_free_full (fake_global_injected_errors, g_free);
	fake_global_injected_errors = NULL;
	fake_hotplug_delay_ms = -1;
	fake_hotplug_order = FAKE_USB_HOTPLUG_ORDER_PLATFORM;
	fake_submit_memory_limit = 0;
	fake_submit_memory_error = 0;
	memset (fake_calls, 0, sizeof (fake_calls));
	memset (fake_total_stats, 0, sizeof (fake_total_stats));
	g_clear_pointer (&fake_last_misuse, g_free);
	fake_next_address = 1;
	fake_handle_events_hook = NULL;
	fake_handle_events_hook_data = NULL;
	fake_transfer_hook = NULL;
	fake_transfer_hook_data = NULL;

	fake_usb_signal_locked ();

	fake_usb_unlock ();
}

void
fake_usb_set_platform (FakeUsbPlatform platform)
{
	fake_usb_lock ();
	fake_platform = platform;
	fake_usb_signal_locked ();
	fake_usb_unlock ();
}

FakeUsbPlatform
fake_usb_get_platform (void)
{
	FakeUsbPlatform platform;

	fake_usb_lock ();
	platform = fake_platform;
	fake_usb_unlock ();

	return platform;
}

void
fake_usb_set_capability (guint32 capability, gboolean available)
{
	int index = _capability_index (capability);

	g_return_if_fail (index >= 0);

	fake_usb_lock ();
	fake_capabilities[index] = available ? 1 : 0;
	fake_usb_unlock ();
}

void
fake_usb_set_dev_mem_supported (gboolean supported)
{
	fake_usb_lock ();
	fake_dev_mem_supported = supported ? 1 : 0;
	fake_usb_unlock ();
}

void
fake_usb_set_version (guint16 major, guint16 minor, guint16 micro, guint16 nano)
{
	struct libusb_version version = { major, minor, micro, nano, "", "fakeusb" };
	struct libusb_version *copy;

	/* Versions handed out earlier must stay valid, so they are never freed */
	copy = g_malloc (sizeof (struct libusb_version));
	memcpy (copy, &version, sizeof (struct libusb_version));

	fake_usb_lock ();
	if (fake_versions == NULL)
		fake_versions = g_ptr_array_new ();
	g_ptr_array_add (fake_versions, copy);
	fake_version = copy;
	fake_usb_unlock ();
}

void
fake_usb_inject_error (FakeUsbCall call, int error, int count)
{
	g_return_if_fail (call == FAKE_USB_CALL_INIT ||
			  call == FAKE_USB_CALL_GET_DEVICE_LIST ||
			  call == FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK);

	fake_usb_lock ();
	_add_injected_error_locked (&fake_global_injected_errors, call, -1, error, count);
	fake_usb_unlock ();
}

void
fake_usb_set_hotplug_delay (int delay_ms)
{
	fake_usb_lock ();
	fake_hotplug_delay_ms = delay_ms;
	fake_usb_unlock ();
}

void
fake_usb_set_hotplug_order (FakeUsbHotplugOrder order)
{
	fake_usb_lock ();
	fake_hotplug_order = order;
	fake_usb_unlock ();
}

void
fake_usb_set_submit_memory_limit (gsize limit, int error)
{
	fake_usb_lock ();
	fake_submit_memory_limit = limit;
	fake_submit_memory_error = error;
	fake_usb_signal_locked ();
	fake_usb_unlock ();
}

void
fake_usb_set_time_scale (guint scale)
{
	g_return_if_fail (scale > 0);

	fake_usb_lock ();
	fake_time_scale = scale;
	fake_usb_signal_locked ();
	fake_usb_unlock ();
}

guint
fake_usb_get_time_scale (void)
{
	guint scale;

	fake_usb_lock ();
	scale = fake_time_scale;
	fake_usb_unlock ();

	return scale;
}

guint64
fake_usb_get_call_count (FakeUsbCall call)
{
	guint64 count;

	g_return_val_if_fail (call < FAKE_USB_N_CALLS, 0);

	fake_usb_lock ();
	count = fake_calls[call];
	fake_usb_unlock ();

	return count;
}

const char *
fake_usb_call_to_string (FakeUsbCall call)
{
	g_return_val_if_fail (call < FAKE_USB_N_CALLS, NULL);

	return fake_call_names[call];
}

guint64
fake_usb_get_total_stat (FakeUsbStat stat)
{
	guint64 value;

	g_return_val_if_fail (stat < FAKE_USB_N_STATS, 0);

	fake_usb_lock ();
	value = fake_total_stats[stat];
	fake_usb_unlock ();

	return value;
}

char *
fake_usb_dup_last_misuse (void)
{
	char *misuse;

	fake_usb_lock ();
	misuse = g_strdup (fake_last_misuse);
	fake_usb_unlock ();

	return misuse;
}

void
fake_usb_reset_counters (void)
{
	guint i;

	fake_usb_lock ();

	memset (fake_calls, 0, sizeof (fake_calls));
	memset (fake_total_stats, 0, sizeof (fake_total_stats));
	g_clear_pointer (&fake_last_misuse, g_free);

	for (i = 0; fake_devices != NULL && i < fake_devices->len; i++) {
		FakeUsbDevice *device = g_ptr_array_index (fake_devices, i);
		guint64 frames_sent = device->stats[FAKE_USB_STAT_FRAMES_SENT];

		/* A pending unplug_after_frames () target is counted in frames sent: rebase it */
		device->unplug_frame_target -= MIN (device->unplug_frame_target, frames_sent);

		memset (device->calls, 0, sizeof (device->calls));
		memset (device->stats, 0, sizeof (device->stats));
		device->event_priority_seen = FALSE;
	}

	fake_usb_unlock ();
}

guint
fake_usb_get_n_contexts (void)
{
	guint n_contexts;

	fake_usb_lock ();
	n_contexts = g_list_length (fake_contexts);
	fake_usb_unlock ();

	return n_contexts;
}

guint
fake_usb_get_n_open_handles (void)
{
	guint n_open_handles;

	fake_usb_lock ();
	n_open_handles = fake_n_open_handles;
	fake_usb_unlock ();

	return n_open_handles;
}

guint
fake_usb_get_n_transfers (void)
{
	guint n_transfers;

	fake_usb_lock ();
	n_transfers = fake_n_transfers;
	fake_usb_unlock ();

	return n_transfers;
}

void
fake_usb_set_handle_events_hook (FakeUsbHandleEventsHook hook, gpointer user_data)
{
	fake_usb_lock ();
	fake_handle_events_hook = hook;
	fake_handle_events_hook_data = user_data;
	fake_usb_unlock ();
}

void
fake_usb_set_transfer_hook (FakeUsbTransferHook hook, gpointer user_data)
{
	fake_usb_lock ();
	fake_transfer_hook = hook;
	fake_transfer_hook_data = user_data;
	fake_usb_unlock ();
}

guint32
fake_usb_checksum (const void *data, gsize size)
{
	const guint8 *bytes = data;
	guint32 hash = 2166136261U;
	gsize i;

	/* FNV-1a */
	for (i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 16777619U;
	}

	return hash;
}

/* Devices */

FakeUsbDevice *
fake_usb_device_new (guint16 vendor_id, guint16 product_id,
		     guint8 device_class, guint8 device_subclass, guint8 device_protocol)
{
	FakeUsbDevice *device;

	device = fake_usb_device_new_unplugged (vendor_id, product_id, device_class, device_subclass,
						device_protocol);
	fake_usb_device_plug (device);

	return device;
}

void
fake_usb_device_remove (FakeUsbDevice *device)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	_device_remove_locked (device);
	fake_usb_unlock ();
}

guint8
fake_usb_device_add_interface (FakeUsbDevice *device, guint8 interface_class, guint8 interface_subclass,
			       guint8 interface_protocol, const guint8 *extra, gsize extra_length)
{
	FakeUsbInterfaceTemplate *template;
	guint8 number;

	g_return_val_if_fail (device != NULL, 0);

	template = g_new0 (FakeUsbInterfaceTemplate, 1);
	template->interface_class = interface_class;
	template->interface_subclass = interface_subclass;
	template->interface_protocol = interface_protocol;
	template->extra = g_byte_array_new ();
	if (extra != NULL && extra_length > 0)
		g_byte_array_append (template->extra, extra, extra_length);
	template->endpoints = g_array_new (FALSE, TRUE, sizeof (FakeUsbEndpointTemplate));

	fake_usb_lock ();
	number = device->interfaces->len;
	template->number = number;
	g_ptr_array_add (device->interfaces, template);
	fake_usb_unlock ();

	return number;
}

void
fake_usb_device_add_endpoint (FakeUsbDevice *device, guint8 interface_number, guint8 address, guint8 transfer_type,
			      guint16 max_packet_size)
{
	FakeUsbInterfaceTemplate *template;
	FakeUsbEndpointTemplate endpoint;

	g_return_if_fail (device != NULL);

	memset (&endpoint, 0, sizeof (endpoint));
	endpoint.address = address;
	endpoint.attributes = transfer_type & 0x03;
	endpoint.max_packet_size = max_packet_size;
	endpoint.interval = (transfer_type & 0x03) == LIBUSB_TRANSFER_TYPE_INTERRUPT ? 4 : 0;

	fake_usb_lock ();
	template = _find_interface_template_locked (device, interface_number);
	if (template != NULL)
		g_array_append_val (template->endpoints, endpoint);
	fake_usb_unlock ();

	g_return_if_fail (template != NULL);
}

void
fake_usb_device_remove_endpoints (FakeUsbDevice *device, guint8 interface_number)
{
	FakeUsbInterfaceTemplate *template;

	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	template = _find_interface_template_locked (device, interface_number);
	if (template != NULL)
		g_array_set_size (template->endpoints, 0);
	fake_usb_unlock ();
}

void
fake_usb_device_set_class (FakeUsbDevice *device, guint8 device_class, guint8 device_subclass,
			   guint8 device_protocol)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->descriptor.bDeviceClass = device_class;
	device->descriptor.bDeviceSubClass = device_subclass;
	device->descriptor.bDeviceProtocol = device_protocol;
	fake_usb_unlock ();
}

void
fake_usb_device_set_bcd_usb (FakeUsbDevice *device, guint16 bcd_usb)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->descriptor.bcdUSB = bcd_usb;
	fake_usb_unlock ();
}

guint8
fake_usb_device_add_string (FakeUsbDevice *device, const char *string)
{
	guint8 index;

	g_return_val_if_fail (device != NULL, 0);
	g_return_val_if_fail (string != NULL, 0);

	fake_usb_lock ();
	index = device->strings->len;
	g_ptr_array_add (device->strings, g_strdup (string));
	fake_usb_unlock ();

	return index;
}

void
fake_usb_device_set_string (FakeUsbDevice *device, guint8 index, const char *string)
{
	g_return_if_fail (device != NULL);
	g_return_if_fail (index > 0);

	fake_usb_lock ();
	while (device->strings->len <= index)
		g_ptr_array_add (device->strings, NULL);
	g_free (g_ptr_array_index (device->strings, index));
	g_ptr_array_index (device->strings, index) = g_strdup (string);
	fake_usb_unlock ();
}

void
fake_usb_device_set_descriptor_strings (FakeUsbDevice *device, guint8 i_manufacturer, guint8 i_product,
					guint8 i_serial_number)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->descriptor.iManufacturer = i_manufacturer;
	device->descriptor.iProduct = i_product;
	device->descriptor.iSerialNumber = i_serial_number;
	fake_usb_unlock ();
}

void
fake_usb_device_set_location (FakeUsbDevice *device, guint8 bus_number, guint8 device_address, guint8 port_number)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->bus_number = bus_number;
	device->device_address = device_address;
	device->port_number = port_number;
	fake_usb_unlock ();
}

void
fake_usb_device_get_location (FakeUsbDevice *device, guint8 *bus_number, guint8 *device_address,
			      guint8 *port_number)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	if (bus_number != NULL)
		*bus_number = device->bus_number;
	if (device_address != NULL)
		*device_address = device->device_address;
	if (port_number != NULL)
		*port_number = device->port_number;
	fake_usb_unlock ();
}

void
fake_usb_device_set_max_packet_size (FakeUsbDevice *device, guint8 endpoint, guint16 max_packet_size)
{
	guint i, j;

	g_return_if_fail (device != NULL);

	fake_usb_lock ();

	for (i = 0; i < device->interfaces->len; i++) {
		FakeUsbInterfaceTemplate *template = g_ptr_array_index (device->interfaces, i);

		for (j = 0; j < template->endpoints->len; j++) {
			FakeUsbEndpointTemplate *endpoint_template;

			endpoint_template = &g_array_index (template->endpoints, FakeUsbEndpointTemplate, j);
			if ((endpoint == 0 && endpoint_template->attributes == LIBUSB_TRANSFER_TYPE_BULK) ||
			    endpoint_template->address == endpoint)
				endpoint_template->max_packet_size = max_packet_size;
		}
	}

	fake_usb_unlock ();
}

void
fake_usb_device_inject_error (FakeUsbDevice *device, FakeUsbCall call, int target, int error, int count)
{
	g_return_if_fail (device != NULL);
	g_return_if_fail (call < FAKE_USB_N_CALLS);

	fake_usb_lock ();
	_add_injected_error_locked (&device->injected_errors, call, target, error, count);
	fake_usb_unlock ();
}

void
fake_usb_device_inject_transfer_status (FakeUsbDevice *device, guint8 endpoint, enum libusb_transfer_status status,
					int count)
{
	FakeUsbEndpointState *state;
	guint n_failed;

	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	state = fake_usb_device_get_endpoint_locked (device, endpoint);
	state->injected_status = status;
	state->n_injected_statuses = count;

	/* A pipe that starts failing fails the reads it holds first, in the order they were submitted: a transfer
	 * submitted from now on must not complete before them */
	n_failed = count != 0 ? state->pending.length : 0;
	if (n_failed > 0) {
		_queue_fail_all_locked (&state->pending, status);
		fake_usb_debug ("device %03d:%03d: %u pending transfer(s) on endpoint 0x%02x failed with status %d",
				device->bus_number, device->device_address, n_failed, endpoint, status);
	}

	fake_usb_unlock ();
}

void
fake_usb_device_fail_transfers (FakeUsbDevice *device, guint8 endpoint, enum libusb_transfer_status status,
				gsize after_bytes, gboolean flush)
{
	FakeUsbEndpointState *state;

	g_return_if_fail (device != NULL);
	g_return_if_fail ((endpoint & LIBUSB_ENDPOINT_IN) != 0);

	fake_usb_lock ();
	state = fake_usb_device_get_endpoint_locked (device, endpoint);
	state->fail_armed = TRUE;
	state->fail_status = status;
	state->fail_after_bytes = after_bytes;
	state->fail_flush = flush;
	fake_usb_signal_locked ();
	fake_usb_unlock ();
}

void
fake_usb_device_halt_endpoint (FakeUsbDevice *device, guint8 endpoint)
{
	g_return_if_fail (device != NULL);
	g_return_if_fail ((endpoint & LIBUSB_ENDPOINT_IN) != 0);

	fake_usb_lock ();
	_endpoint_set_halt_locked (device, endpoint, TRUE);
	fake_usb_unlock ();
}

void
fake_usb_device_set_control_delay (FakeUsbDevice *device, guint delay_ms)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->control_delay_ms = delay_ms;
	fake_usb_signal_locked ();
	fake_usb_unlock ();
}

void
fake_usb_device_set_pipe_policies (FakeUsbDevice *device, int allow_partial_reads, int auto_clear_stall)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->allow_partial_reads = allow_partial_reads < 0 ? -1 : allow_partial_reads != 0;
	device->auto_clear_stall = auto_clear_stall < 0 ? -1 : auto_clear_stall != 0;
	fake_usb_unlock ();
}

void
fake_usb_device_clear_injections (FakeUsbDevice *device)
{
	guint i;

	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	g_list_free_full (device->injected_errors, g_free);
	device->injected_errors = NULL;
	for (i = 0; i < FAKE_USB_N_ENDPOINT_SLOTS; i++) {
		device->endpoints[i].n_injected_statuses = 0;
		device->endpoints[i].fail_armed = FALSE;
	}
	fake_usb_unlock ();
}

void
fake_usb_device_set_open_error (FakeUsbDevice *device, int error)
{
	fake_usb_device_inject_error (device, FAKE_USB_CALL_OPEN, -1, error, -1);
}

void
fake_usb_device_set_config_descriptor_error (FakeUsbDevice *device, int error)
{
	fake_usb_device_inject_error (device, FAKE_USB_CALL_GET_CONFIG_DESCRIPTOR, -1, error, -1);
}

void
fake_usb_device_set_exclusive_open (FakeUsbDevice *device, gboolean exclusive)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->exclusive_open = exclusive;
	fake_usb_unlock ();
}

void
fake_usb_device_unplug (FakeUsbDevice *device)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	_device_unplug_locked (device);
	fake_usb_unlock ();
}

void
fake_usb_device_unplug_after_frames (FakeUsbDevice *device, guint n_frames, gsize extra_bytes)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();

	if (device->plugged) {
		if (n_frames == 0 && extra_bytes == 0) {
			_device_unplug_locked (device);
		} else {
			device->unplug_armed = TRUE;
			device->unplug_frame_target = device->stats[FAKE_USB_STAT_FRAMES_SENT] + n_frames;
			device->unplug_extra_bytes = extra_bytes;
			device->unplug_bytes_sent = 0;
			device->unplug_now = FALSE;
			fake_usb_signal_locked ();
		}
	}

	fake_usb_unlock ();
}

void
fake_usb_device_plug (FakeUsbDevice *device)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	_device_plug_locked (device);
	fake_usb_unlock ();
}

void
fake_usb_device_set_unplug_errors (FakeUsbDevice *device, enum libusb_transfer_status in_flight_status,
				   int submit_error, int synchronous_error)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->unplug_errors_set = TRUE;
	device->unplug_status = in_flight_status;
	device->unplug_submit_error = submit_error;
	device->unplug_synchronous_error = synchronous_error;
	fake_usb_unlock ();
}

void
fake_usb_device_set_unplug_transfers_hang (FakeUsbDevice *device, gboolean hang)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->unplug_transfers_hang = hang;
	fake_usb_unlock ();
}

gboolean
fake_usb_device_is_plugged (FakeUsbDevice *device)
{
	gboolean plugged;

	g_return_val_if_fail (device != NULL, FALSE);

	fake_usb_lock ();
	plugged = device->plugged;
	fake_usb_unlock ();

	return plugged;
}

void
fake_usb_device_set_raw_io_supported (FakeUsbDevice *device, gboolean supported)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->raw_io_supported = supported ? 1 : 0;
	fake_usb_unlock ();
}

void
fake_usb_device_set_max_raw_io_transfer_size (FakeUsbDevice *device, int size)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->max_raw_io_transfer_size = size;
	fake_usb_unlock ();
}

void
fake_usb_device_set_raw_io_pending_change_error (FakeUsbDevice *device, int error)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->raw_io_pending_change_error = error;
	fake_usb_unlock ();
}

gboolean
fake_usb_device_get_raw_io (FakeUsbDevice *device, guint8 endpoint)
{
	gboolean raw_io;

	g_return_val_if_fail (device != NULL, FALSE);

	fake_usb_lock ();
	raw_io = fake_usb_device_get_endpoint_locked (device, endpoint)->raw_io;
	fake_usb_unlock ();

	return raw_io;
}

guint64
fake_usb_device_get_call_count (FakeUsbDevice *device, FakeUsbCall call)
{
	guint64 count;

	g_return_val_if_fail (device != NULL, 0);
	g_return_val_if_fail (call < FAKE_USB_N_CALLS, 0);

	fake_usb_lock ();
	count = device->calls[call];
	fake_usb_unlock ();

	return count;
}

guint64
fake_usb_device_get_stat (FakeUsbDevice *device, FakeUsbStat stat)
{
	guint64 value;

	g_return_val_if_fail (device != NULL, 0);
	g_return_val_if_fail (stat < FAKE_USB_N_STATS, 0);

	fake_usb_lock ();
	value = device->stats[stat];
	fake_usb_unlock ();

	return value;
}

guint
fake_usb_device_get_n_open_handles (FakeUsbDevice *device)
{
	guint n_open_handles;

	g_return_val_if_fail (device != NULL, 0);

	fake_usb_lock ();
	n_open_handles = device->n_open_handles;
	fake_usb_unlock ();

	return n_open_handles;
}

guint
fake_usb_device_get_n_pending_transfers (FakeUsbDevice *device, guint8 endpoint)
{
	guint n_pending;

	g_return_val_if_fail (device != NULL, 0);

	fake_usb_lock ();
	if ((endpoint & LIBUSB_ENDPOINT_ADDRESS_MASK) == 0)
		n_pending = device->control_pending.length;
	else
		n_pending = fake_usb_device_get_endpoint_locked (device, endpoint)->pending.length;
	fake_usb_unlock ();

	return n_pending;
}

gboolean
fake_usb_device_is_interface_claimed (FakeUsbDevice *device, guint8 interface_number)
{
	gboolean claimed;

	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (interface_number < FAKE_USB_MAX_INTERFACES, FALSE);

	fake_usb_lock ();
	claimed = device->claims[interface_number] != NULL;
	fake_usb_unlock ();

	return claimed;
}

gboolean
fake_usb_device_get_auto_detach_kernel_driver (FakeUsbDevice *device)
{
	gboolean auto_detach;

	g_return_val_if_fail (device != NULL, FALSE);

	fake_usb_lock ();
	auto_detach = device->auto_detach_kernel_driver;
	fake_usb_unlock ();

	return auto_detach;
}

gboolean
fake_usb_device_get_event_thread_priority (FakeUsbDevice *device, int *last_priority, int *highest_priority)
{
	gboolean seen;

	g_return_val_if_fail (device != NULL, FALSE);

	fake_usb_lock ();
	seen = device->event_priority_seen;
	if (last_priority != NULL)
		*last_priority = seen ? device->event_priority_last : 0;
	if (highest_priority != NULL)
		*highest_priority = seen ? device->event_priority_highest : 0;
	fake_usb_unlock ();

	return seen;
}

void
fake_usb_device_reset_event_thread_priority (FakeUsbDevice *device)
{
	g_return_if_fail (device != NULL);

	fake_usb_lock ();
	device->event_priority_seen = FALSE;
	device->event_priority_last = 0;
	device->event_priority_highest = 0;
	fake_usb_unlock ();
}
