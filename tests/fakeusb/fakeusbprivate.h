/* SPDX-License-Identifier:Unlicense */

/* Internals shared by fakeusb.c (the libusb entry points, contexts, devices, transfers and events) and
 * fakeusbcamera.c (the USB3 Vision camera emulation). Every function suffixed with _locked expects the fake lock
 * (fake_usb_lock()) to be held. */

#ifndef FAKE_USB_PRIVATE_H
#define FAKE_USB_PRIVATE_H

#include "fakeusb.h"

G_BEGIN_DECLS

/* Declarations that changed between the libusb versions Aravis builds with (1.0.21, the first one with
 * libusb_dev_mem_alloc (), and later):
 *  - libusb_set_option () is declared with LIBUSB_CALLV from 1.0.27 on, with LIBUSB_CALL before; both give a
 *    variadic function the same calling convention;
 *  - from 1.0.24 on (LIBUSB_API_VERSION 0x01000108), libusb_hotplug_register_callback () takes its events and flags
 *    as int, and libusb_strerror () its error code as int; they were enums before. */

#ifndef LIBUSB_CALLV
#define LIBUSB_CALLV LIBUSB_CALL
#endif

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000108
typedef int FakeUsbHotplugEvents;
typedef int FakeUsbHotplugFlags;
typedef int FakeUsbErrorCode;
#else
typedef libusb_hotplug_event FakeUsbHotplugEvents;
typedef libusb_hotplug_flag FakeUsbHotplugFlags;
typedef enum libusb_error FakeUsbErrorCode;
#endif

#define FAKE_USB_N_ENDPOINT_SLOTS	32
#define FAKE_USB_MAX_INTERFACES		32

typedef enum {
	FAKE_USB_SEGMENT_DATA,
	FAKE_USB_SEGMENT_ACK,
	FAKE_USB_SEGMENT_LEADER,
	FAKE_USB_SEGMENT_PAYLOAD,
	FAKE_USB_SEGMENT_TRAILER,
	FAKE_USB_SEGMENT_ZLP
} FakeUsbSegmentKind;

/* One device side transfer queued on an IN endpoint. The device sends it as full packets followed by a short packet
 * when its size is not a multiple of the max packet size, or by a zero length packet when zlp is set. An empty
 * segment is a zero length packet on its own (FAKE_USB_SEGMENT_ZLP: one left for the next read). */

typedef struct {
	GBytes *bytes;
	gsize offset;
	gboolean zlp;
	FakeUsbSegmentKind kind;
} FakeUsbSegment;

typedef struct {
	gboolean halted;
	gboolean raw_io;
	GQueue pending;			/* FakeUsbTransferPriv *, IN transfers waiting for data */
	GQueue segments;		/* FakeUsbSegment *, device to host data */
	enum libusb_transfer_status injected_status;
	int n_injected_statuses;

	gboolean fail_armed;		/* fake_usb_device_fail_transfers () */
	enum libusb_transfer_status fail_status;
	gsize fail_after_bytes;
	gboolean fail_flush;
} FakeUsbEndpointState;

typedef struct {
	guint8 address;
	guint8 attributes;
	guint16 max_packet_size;
	guint8 interval;
} FakeUsbEndpointTemplate;

typedef struct {
	guint8 number;
	guint8 interface_class;
	guint8 interface_subclass;
	guint8 interface_protocol;
	GByteArray *extra;
	GArray *endpoints;		/* FakeUsbEndpointTemplate */
} FakeUsbInterfaceTemplate;

typedef struct {
	FakeUsbCall call;
	int target;
	int error;
	int count;
} FakeUsbInjectedError;

typedef struct _FakeUsbCamera FakeUsbCamera;
typedef struct _FakeUsbFrameJob FakeUsbFrameJob;

struct _FakeUsbDevice {
	int ref_count;
	gboolean in_registry;
	gboolean plugged;
	guint generation;

	struct libusb_device_descriptor descriptor;
	GPtrArray *interfaces;		/* FakeUsbInterfaceTemplate * */
	GPtrArray *strings;		/* char *, by string descriptor index */

	guint8 bus_number;
	guint8 device_address;
	guint8 port_number;

	GList *injected_errors;		/* FakeUsbInjectedError * */
	gboolean exclusive_open;

	gboolean unplug_errors_set;
	enum libusb_transfer_status unplug_status;
	int unplug_submit_error;
	int unplug_synchronous_error;
	gboolean unplug_transfers_hang;

	int raw_io_supported;		/* -1: platform default */
	int max_raw_io_transfer_size;
	int raw_io_pending_change_error;

	int allow_partial_reads;	/* -1: platform default */
	int auto_clear_stall;		/* -1: platform default */

	guint control_delay_ms;
	GQueue control_pending;		/* FakeUsbTransferPriv *, control transfers waiting for their answer */

	FakeUsbEndpointState endpoints[FAKE_USB_N_ENDPOINT_SLOTS];
	libusb_device_handle *claims[FAKE_USB_MAX_INTERFACES];
	gboolean auto_detach_kernel_driver;
	guint n_open_handles;

	gboolean unplug_armed;
	guint64 unplug_frame_target;
	gsize unplug_extra_bytes;
	gsize unplug_bytes_sent;
	gboolean unplug_now;

	guint64 calls[FAKE_USB_N_CALLS];
	guint64 stats[FAKE_USB_N_STATS];

	gboolean event_priority_seen;
	int event_priority_last;
	int event_priority_highest;

	FakeUsbCamera *camera;
};

void		fake_usb_debug					(const char *format, ...) G_GNUC_PRINTF (1, 2);

void		fake_usb_lock					(void);
void		fake_usb_unlock					(void);
void		fake_usb_signal_locked				(void);

/* GLib >= 2.60 has g_queue_clear_full (); Aravis supports 2.58 */
void		fake_usb_queue_clear_full			(GQueue *queue, GDestroyNotify free_func);

/* A device side delay or timeout of @us microseconds, multiplied by the time scale */
gint64		fake_usb_scaled_us_locked			(guint64 us);
guint		fake_usb_get_time_scale_locked			(void);

void		fake_usb_device_stat_add_locked			(FakeUsbDevice *device, FakeUsbStat stat, guint64 n);

FakeUsbDevice *	fake_usb_device_new_unplugged			(guint16 vendor_id, guint16 product_id,
								 guint8 device_class, guint8 device_subclass,
								 guint8 device_protocol);

FakeUsbDevice *	fake_usb_device_ref_locked			(FakeUsbDevice *device);
void		fake_usb_device_unref_locked			(FakeUsbDevice *device);

FakeUsbEndpointState *	fake_usb_device_get_endpoint_locked	(FakeUsbDevice *device, guint8 endpoint);
guint16		fake_usb_device_get_max_packet_size_locked	(FakeUsbDevice *device, guint8 endpoint);
void		fake_usb_device_push_segment_locked		(FakeUsbDevice *device, guint8 endpoint, GBytes *bytes,
								 gboolean zlp, FakeUsbSegmentKind kind);
void		fake_usb_device_flush_endpoint_locked		(FakeUsbDevice *device, guint8 endpoint);

/* Implemented in fakeusbcamera.c */

void		fake_usb_camera_free				(FakeUsbCamera *camera);
void		fake_usb_camera_power_on_locked			(FakeUsbDevice *device);
void		fake_usb_camera_unplugged_locked		(FakeUsbDevice *device);
void		fake_usb_camera_receive_locked			(FakeUsbDevice *device, guint8 endpoint,
								 const guint8 *data, gsize size, gint64 now);
void		fake_usb_camera_tick_locked			(FakeUsbDevice *device, gint64 now);
gint64		fake_usb_camera_get_next_wakeup_locked		(FakeUsbDevice *device, gint64 now);

/* Frame production: start and finish with the lock held, run without it */

FakeUsbFrameJob *	fake_usb_camera_start_frame_locked	(FakeUsbDevice *device, gint64 now);
void		fake_usb_camera_run_frame			(FakeUsbFrameJob *job);
void		fake_usb_camera_finish_frame_locked		(FakeUsbFrameJob *job);

G_END_DECLS

#endif
