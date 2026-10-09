/* SPDX-License-Identifier:Unlicense */

/*
 * fakeusb: a link-time fake of libusb-1.0, for testing Aravis's USB3 Vision code without hardware.
 *
 * The test programs of the 'fakeuv' meson suite link the object files of the Aravis library and this fake instead of
 * the real libusb (see tests/meson.build). Every libusb entry point Aravis calls is implemented here, on top of a set
 * of scripted devices. A device is either a generic USB device built from descriptors, or a USB3 Vision camera
 * (fake_usb_camera_new()) backed by ArvFakeCamera's register map and GenICam XML. The camera emulation is written from
 * Aravis's own protocol headers and parsing code (src/arvuvcpprivate.h, src/arvuvspprivate.h,
 * src/arvuvinterfaceprivate.h, src/arvuvdevice.c, src/arvuvstream.c), never from the USB3 Vision specification. The
 * libusb behaviour is taken from the libusb 1.0.30 sources (core.c, io.c, sync.c, descriptor.c, os/windows_*.c,
 * os/linux_usbfs.c), and the WinUSB pipe policies from Microsoft's "WinUSB functions for pipe policy modification".
 *
 * It builds against the headers of every libusb Aravis builds with (1.0.21 and later); the RAW_IO and
 * libusb_get_device_string() emulation needs the 1.0.30 headers (LIBUSB_API_VERSION >= 0x0100010C).
 *
 * libusb semantics that are emulated:
 *
 *  - Event handling, as in libusb's io.c: each context has an event lock. libusb_handle_events*() takes it, waits
 *    for events, then dispatches every completed transfer and hotplug event of the context on the calling thread,
 *    and releases it; a thread that finds the lock taken waits until the holder releases it (an "event waiter").
 *    Synchronous calls (bulk, interrupt and control transfers, string descriptors) are an asynchronous transfer plus
 *    libusb_handle_events_completed(): whichever thread holds the lock dispatches the callbacks of every transfer of
 *    the context, so an application callback can run on a thread that is in the middle of a synchronous transfer.
 *    Completion callbacks, including LIBUSB_TRANSFER_CANCELLED after libusb_cancel_transfer() and
 *    LIBUSB_TRANSFER_TIMED_OUT, only ever run inside event handling.
 *  - Within one round, completed transfers are dispatched before hotplug events on Windows and macOS, after them on
 *    Linux (libusb handles its event trigger before reaping URBs there); see fake_usb_set_hotplug_order().
 *  - Calling libusb_handle_events*() or a synchronous function from a callback, on the context being dispatched,
 *    returns LIBUSB_ERROR_BUSY and counts as API misuse: real libusb 1.0.30 deadlocks on Linux (its callbacks run
 *    with the event waiters lock held) and spins forever on Windows for a synchronous call.
 *  - libusb_close() takes the event lock, interrupting the event handler of the context first, as libusb does.
 *    Transfers still in flight on the handle are dropped without a callback and their dev_handle set to NULL; a
 *    synchronous transfer waiting on it returns LIBUSB_ERROR_NO_DEVICE (libusb's do_close () and sync.c).
 *  - Hotplug callbacks are delivered from event handling (LIBUSB_HOTPLUG_ENUMERATE ones from
 *    libusb_hotplug_register_callback() itself). Without LIBUSB_CAP_HAS_HOTPLUG, registration fails with
 *    LIBUSB_ERROR_NOT_SUPPORTED. With it, contexts keep the device objects of attached devices, so
 *    libusb_get_device_list() returns the same objects until the device leaves (core.c); without it, a device object
 *    is freed with its last reference, as with the default Windows libusb.
 *  - Bulk IN data is delivered with USB packet semantics: a read completes when it is full or when the device ends a
 *    transfer with a short packet, or a zero length packet. A zero length packet that follows a transfer that exactly
 *    filled a read completes the next read with 0 bytes. A read that is not a multiple of the max packet size and
 *    receives a packet that does not fit completes with LIBUSB_TRANSFER_OVERFLOW (Linux, macOS), or, with WinUSB's
 *    ALLOW_PARTIAL_READS policy that libusb enables at claim, completes with the bytes that fit, the rest of the packet
 *    starting the next read (see fake_usb_device_set_pipe_policies()).
 *  - Halted IN endpoints fail their transfers with LIBUSB_TRANSFER_STALL until libusb_clear_halt(), or, with
 *    WinUSB's AUTO_CLEAR_STALL policy that libusb enables at claim, only the transfers that meet the stall (those in
 *    flight when it happens, else the next one): the halt is cleared once they have failed. Not while RAW_IO is on
 *    for the endpoint, which bypasses WinUSB's error handling.
 *  - On Windows, bulk transfers and libusb_clear_halt() on an interface the handle has not claimed fail with
 *    LIBUSB_ERROR_NOT_FOUND (WinUSB backend), and a control transfer on a handle with no claimed interface claims the
 *    first free one for its duration (libusb's auto-claim), or fails with LIBUSB_ERROR_NOT_FOUND. On Linux the kernel
 *    claims the interface for the application. Both count as API misuse.
 *  - libusb_get_string_descriptor_ascii() is two libusb_control_transfer() calls, the language table then the
 *    string, as in descriptor.c: they are counted as FAKE_USB_CALL_CONTROL_TRANSFER, go through the transfer hook and
 *    the control transfer injections, delays and auto-claim.
 *  - WinUSB RAW_IO (libusb >= 1.0.30 headers only): once enabled on an endpoint, IN transfers whose length is not a
 *    multiple of the endpoint max packet size, or larger than the max RAW_IO transfer size, are rejected by
 *    libusb_submit_transfer() and libusb_bulk_transfer() with LIBUSB_ERROR_IO, as WinUSB's ReadPipe does. The policy
 *    lives in the WinUSB interface handle: releasing the interface or closing the device handle turns it off.
 *    Changing it while transfers are pending is accepted and counted as API misuse (libusb documents that it "should
 *    not be changed", and returns no specific error), see fake_usb_device_set_raw_io_pending_change_error().
 *  - Everything is protected by one global lock, so the fake is safe against Aravis's stream thread, its libusb
 *    event thread and the test thread. Callbacks are always invoked without that lock. A camera fills its frames
 *    without the lock, so that a free running camera does not starve the other threads.
 *
 * Not emulated: isochronous and bulk stream transfers, alternate settings, the event interface of the camera, the
 * libusb_lock_events() family and pollfds, and the per-context I/O completion port thread of the Windows backend
 * (completions reach the event handler at once).
 *
 * Unless stated otherwise, the functions below can be called from any thread, at any time, including while Aravis is
 * streaming. FakeUsbDevice pointers stay valid until fake_usb_reset() or fake_usb_device_remove(), even while
 * the device is unplugged.
 *
 * Set FAKE_USB_DEBUG=1 in the environment to log plugs, unplugs, refused opens, RAW_IO rejections, API misuse,
 * scripted transfer failures and every frame the cameras queue, on stderr.
 *
 * Typical use (see tests/fakeuvtest.h for helpers):
 *
 *	fake_usb_reset ();
 *	camera = fake_usb_camera_new ("UV1");				// plugged, listed by discovery
 *	fake_usb_camera_set_geometry (camera, 720, 540, ARV_PIXEL_FORMAT_MONO_8);
 *	fake_usb_set_capability (LIBUSB_CAP_HAS_HOTPLUG, TRUE);
 *	fake_usb_device_unplug_after_frames (camera, 3, 0);		// cable pulled after the 3rd frame trailer
 *	... drive Aravis ...
 *	g_assert_cmpuint (fake_usb_device_get_call_count (camera, FAKE_USB_CALL_OPEN), ==, 2);
 *	g_assert_cmpuint (fake_usb_get_total_stat (FAKE_USB_STAT_API_MISUSE), ==, 0);
 */

#ifndef FAKE_USB_H
#define FAKE_USB_H

#include <arv.h>
#include <libusb.h>

G_BEGIN_DECLS

/* Interface numbers and endpoint addresses of the emulated USB3 Vision camera */

#define FAKE_USB_CAMERA_CONTROL_INTERFACE	0
#define FAKE_USB_CAMERA_EVENT_INTERFACE		1
#define FAKE_USB_CAMERA_DATA_INTERFACE		2

#define FAKE_USB_CAMERA_CONTROL_ENDPOINT_OUT	0x01
#define FAKE_USB_CAMERA_CONTROL_ENDPOINT_IN	0x81
#define FAKE_USB_CAMERA_EVENT_ENDPOINT_IN	0x82
#define FAKE_USB_CAMERA_DATA_ENDPOINT_IN	0x83

/* String descriptor indices of the emulated camera */

#define FAKE_USB_CAMERA_STRING_MANUFACTURER	1
#define FAKE_USB_CAMERA_STRING_PRODUCT		2
#define FAKE_USB_CAMERA_STRING_SERIAL_NUMBER	3
#define FAKE_USB_CAMERA_STRING_GUID		4
#define FAKE_USB_CAMERA_STRING_FAMILY		5

/* pid.codes test identifiers */
#define FAKE_USB_CAMERA_VENDOR_ID		0x1209
#define FAKE_USB_CAMERA_PRODUCT_ID		0x0001

/* Defaults */

#define FAKE_USB_DEFAULT_MAX_PACKET_SIZE		1024
#define FAKE_USB_DEFAULT_MAX_RAW_IO_TRANSFER_SIZE	(2 * 1024 * 1024)
#define FAKE_USB_CAMERA_DEFAULT_RESPONSE_TIME_MS	200
#define FAKE_USB_CAMERA_DEFAULT_MAX_CMD_TRANSFER	1024
#define FAKE_USB_CAMERA_DEFAULT_MAX_ACK_TRANSFER	1024
#define FAKE_USB_CAMERA_DEFAULT_ALIGNMENT_SHIFT		3

/* A delay meaning "never" (fake_usb_device_set_control_delay()) */
#define FAKE_USB_NEVER				G_MAXUINT

/* Address map of the U3V register blocks emulated by the camera (on top of ArvFakeCamera's register map at 0x0000,
 * whose GenICam XML lies at 0x10000) */

#define FAKE_USB_CAMERA_SBRM_ADDRESS		0x00100000
#define FAKE_USB_CAMERA_SIRM_ADDRESS		0x00100100
#define FAKE_USB_CAMERA_MANIFEST_ADDRESS	0x00100200

/**
 * FakeUsbPlatform:
 *
 * A set of platform dependent defaults, chosen to mimic libusb 1.0.30 on that platform:
 *
 * | | WINDOWS | LINUX | MACOS |
 * | --- | --- | --- | --- |
 * | LIBUSB_CAP_HAS_HOTPLUG | no (default libusb build) | yes | yes |
 * | LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER | no | yes | yes |
 * | libusb_endpoint_supports_raw_io () | 1 | 0 | 0 |
 * | libusb_dev_mem_alloc () | NULL | memory | NULL |
 * | ALLOW_PARTIAL_READS, AUTO_CLEAR_STALL | yes (set by libusb at claim) | no | no |
 * | transfers on an unclaimed interface | LIBUSB_ERROR_NOT_FOUND | accepted (the kernel claims) | LIBUSB_ERROR_NOT_FOUND |
 * | control transfer, no interface claimed | auto-claim, or LIBUSB_ERROR_NOT_FOUND | accepted | accepted |
 * | in-flight transfers at unplug | LIBUSB_TRANSFER_ERROR | LIBUSB_TRANSFER_NO_DEVICE | LIBUSB_TRANSFER_NO_DEVICE |
 * | libusb_submit_transfer () after unplug | LIBUSB_ERROR_IO | LIBUSB_ERROR_NO_DEVICE | LIBUSB_ERROR_NO_DEVICE |
 * | synchronous transfers after unplug | LIBUSB_ERROR_IO | LIBUSB_ERROR_NO_DEVICE | LIBUSB_ERROR_NO_DEVICE |
 * | hotplug events | 10 ms after the change, after the transfers of a round | at once, before the transfers of a round | at once, after the transfers |
 *
 * On Windows an in-flight transfer can end with any of LIBUSB_TRANSFER_NO_DEVICE (ERROR_DEVICE_NOT_CONNECTED and two
 * other codes), LIBUSB_TRANSFER_STALL (ERROR_GEN_FAILURE), LIBUSB_TRANSFER_CANCELLED (ERROR_OPERATION_ABORTED),
 * LIBUSB_TRANSFER_TIMED_OUT (ERROR_SEM_TIMEOUT) or LIBUSB_TRANSFER_ERROR (anything else) (windows_common.c); which one
 * a removal gives is unverified, so test all of them with fake_usb_device_set_unplug_errors(). The Windows hotplug
 * backend (an opt-in libusb build) posts DEVICE_LEFT after a 10 to 100 ms debounce and a full rescan.
 *
 * The default is the platform the test runs on (FAKE_USB_PLATFORM_HOST). Any of these can be overridden afterwards
 * with fake_usb_set_capability(), fake_usb_device_set_raw_io_supported(), fake_usb_set_dev_mem_supported(),
 * fake_usb_device_set_pipe_policies(), fake_usb_device_set_unplug_errors(), fake_usb_set_hotplug_delay() and
 * fake_usb_set_hotplug_order(). Tests that depend on one of these values should set it explicitly, so that they
 * behave the same on every platform.
 */

typedef enum {
	FAKE_USB_PLATFORM_LINUX,
	FAKE_USB_PLATFORM_WINDOWS,
	FAKE_USB_PLATFORM_MACOS
} FakeUsbPlatform;

#if defined (G_OS_WIN32)
#define FAKE_USB_PLATFORM_HOST FAKE_USB_PLATFORM_WINDOWS
#elif defined (__APPLE__)
#define FAKE_USB_PLATFORM_HOST FAKE_USB_PLATFORM_MACOS
#else
#define FAKE_USB_PLATFORM_HOST FAKE_USB_PLATFORM_LINUX
#endif

/**
 * FakeUsbHotplugOrder:
 * @FAKE_USB_HOTPLUG_ORDER_PLATFORM: as the platform preset (hotplug first on Linux only)
 * @FAKE_USB_HOTPLUG_ORDER_TRANSFERS_FIRST: completed transfers, then hotplug events
 * @FAKE_USB_HOTPLUG_ORDER_HOTPLUG_FIRST: hotplug events, then completed transfers
 *
 * Dispatch order within one event handling round.
 */

typedef enum {
	FAKE_USB_HOTPLUG_ORDER_PLATFORM,
	FAKE_USB_HOTPLUG_ORDER_TRANSFERS_FIRST,
	FAKE_USB_HOTPLUG_ORDER_HOTPLUG_FIRST
} FakeUsbHotplugOrder;

/**
 * FakeUsbCall:
 *
 * The libusb entry points counted by the fake, globally (fake_usb_get_call_count()) and per device
 * (fake_usb_device_get_call_count(), for the calls that take a device, a device handle or a transfer). The same
 * values select the calls that fake_usb_device_inject_error() makes fail.
 *
 * Synchronous transfers are counted as FAKE_USB_CALL_BULK_TRANSFER, FAKE_USB_CALL_INTERRUPT_TRANSFER or
 * FAKE_USB_CALL_CONTROL_TRANSFER only, never as submissions; the two control transfers of
 * libusb_get_string_descriptor_ascii() are counted as FAKE_USB_CALL_CONTROL_TRANSFER too.
 * libusb_get_active_config_descriptor() and libusb_get_config_descriptor_by_value() count as
 * FAKE_USB_CALL_GET_CONFIG_DESCRIPTOR. FAKE_USB_CALL_HANDLE_EVENTS counts every libusb_handle_events*() call made by
 * the application (not the event handling done inside synchronous calls); per device, the calls on a context where
 * the device has an open handle. Windows auto-claims are not counted as FAKE_USB_CALL_CLAIM_INTERFACE (see
 * FAKE_USB_STAT_AUTO_CLAIMS).
 */

typedef enum {
	FAKE_USB_CALL_INIT,
	FAKE_USB_CALL_EXIT,
	FAKE_USB_CALL_GET_DEVICE_LIST,
	FAKE_USB_CALL_FREE_DEVICE_LIST,
	FAKE_USB_CALL_GET_DEVICE_DESCRIPTOR,
	FAKE_USB_CALL_GET_CONFIG_DESCRIPTOR,
	FAKE_USB_CALL_FREE_CONFIG_DESCRIPTOR,
	FAKE_USB_CALL_GET_BUS_NUMBER,
	FAKE_USB_CALL_GET_DEVICE_ADDRESS,
	FAKE_USB_CALL_GET_PORT_NUMBER,
	FAKE_USB_CALL_GET_DEVICE_SPEED,
	FAKE_USB_CALL_GET_MAX_PACKET_SIZE,
	FAKE_USB_CALL_GET_DEVICE_STRING,
	FAKE_USB_CALL_OPEN,
	FAKE_USB_CALL_CLOSE,
	FAKE_USB_CALL_GET_DEVICE,
	FAKE_USB_CALL_CLAIM_INTERFACE,
	FAKE_USB_CALL_RELEASE_INTERFACE,
	FAKE_USB_CALL_SET_AUTO_DETACH_KERNEL_DRIVER,
	FAKE_USB_CALL_GET_STRING_DESCRIPTOR_ASCII,
	FAKE_USB_CALL_CONTROL_TRANSFER,
	FAKE_USB_CALL_BULK_TRANSFER,
	FAKE_USB_CALL_INTERRUPT_TRANSFER,
	FAKE_USB_CALL_CLEAR_HALT,
	FAKE_USB_CALL_RESET_DEVICE,
	FAKE_USB_CALL_ALLOC_TRANSFER,
	FAKE_USB_CALL_FREE_TRANSFER,
	FAKE_USB_CALL_SUBMIT_TRANSFER,
	FAKE_USB_CALL_CANCEL_TRANSFER,
	FAKE_USB_CALL_HANDLE_EVENTS,
	FAKE_USB_CALL_INTERRUPT_EVENT_HANDLER,
	FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK,
	FAKE_USB_CALL_HOTPLUG_DEREGISTER_CALLBACK,
	FAKE_USB_CALL_HAS_CAPABILITY,
	FAKE_USB_CALL_GET_VERSION,
	FAKE_USB_CALL_ERROR_NAME,
	FAKE_USB_CALL_STRERROR,
	FAKE_USB_CALL_SET_OPTION,
	FAKE_USB_CALL_DEV_MEM_ALLOC,
	FAKE_USB_CALL_DEV_MEM_FREE,
	FAKE_USB_CALL_ENDPOINT_SUPPORTS_RAW_IO,
	FAKE_USB_CALL_ENDPOINT_SET_RAW_IO,
	FAKE_USB_CALL_GET_MAX_RAW_IO_TRANSFER_SIZE,
	FAKE_USB_N_CALLS
} FakeUsbCall;

/**
 * FakeUsbStat:
 * @FAKE_USB_STAT_FRAMES_STARTED: frames queued by the camera (leader, payload and trailer)
 * @FAKE_USB_STAT_FRAMES_SENT: frames whose trailer was completely read by the host
 * @FAKE_USB_STAT_STREAM_BYTES_SENT: bytes read by the host on the camera data endpoint
 * @FAKE_USB_STAT_STREAM_READS: IN transfers (asynchronous or synchronous) submitted on the camera data endpoint
 * @FAKE_USB_STAT_UNALIGNED_STREAM_READS: the ones whose length is not a multiple of the endpoint max packet size
 * @FAKE_USB_STAT_MAX_STREAM_READ: largest of their lengths
 * @FAKE_USB_STAT_MAX_PENDING_STREAM_READS: highest number of transfers pending at once on the data endpoint
 * @FAKE_USB_STAT_STREAM_FLUSHES: data endpoint flushes (stream disabled, endpoint halted or halt cleared, scripted
 * failure with flush)
 * @FAKE_USB_STAT_COMMANDS: control channel commands received by the camera
 * @FAKE_USB_STAT_PENDING_ACKS: pending acknowledges sent by the camera
 * @FAKE_USB_STAT_DROPPED_ACKS: acknowledges dropped on purpose (fake_usb_camera_drop_acks())
 * @FAKE_USB_STAT_INVALID_COMMANDS: commands with a bad magic, an unknown code or an invalid address
 * @FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS: transfers rejected with LIBUSB_ERROR_IO by the RAW_IO length rules
 * @FAKE_USB_STAT_RAW_IO_CHANGES_WHILE_PENDING: RAW_IO policy changes made while transfers were pending on the
 * endpoint (also counted as API misuse when accepted)
 * @FAKE_USB_STAT_OVERFLOWS: transfers completed with LIBUSB_TRANSFER_OVERFLOW
 * @FAKE_USB_STAT_TIMEOUTS: transfers completed with LIBUSB_TRANSFER_TIMED_OUT
 * @FAKE_USB_STAT_CANCELLATIONS: transfers completed with LIBUSB_TRANSFER_CANCELLED
 * @FAKE_USB_STAT_DEVICE_STRING_FETCHES: libusb_get_device_string() calls not served from the device object cache
 * @FAKE_USB_STAT_SYNC_IN_EVENT_HANDLER: synchronous calls, or libusb_handle_events*() calls, refused with
 * LIBUSB_ERROR_BUSY because they were made from a callback of the context being dispatched (also API misuse)
 * @FAKE_USB_STAT_UNPLUGS: times the device was unplugged
 * @FAKE_USB_STAT_API_MISUSE: things libusb documents as application bugs: freeing or resubmitting an in-flight
 * transfer, closing a handle with transfers in flight, exiting a context with open handles, transfers or
 * libusb_clear_halt() on an interface the handle did not claim, synchronous I/O or event handling from a callback,
 * RAW_IO changes with transfers pending
 * @FAKE_USB_STAT_CONTROL_REQUESTS: control requests sent to the device (synchronous, asynchronous, and the two of
 * each string descriptor read)
 * @FAKE_USB_STAT_STRING_REQUESTS: the GET_DESCRIPTOR (string) requests among them
 * @FAKE_USB_STAT_AUTO_CLAIMS: interfaces claimed by libusb for a control transfer (Windows auto-claim)
 * @FAKE_USB_STAT_ZERO_LENGTH_PACKETS: zero length packets that ended a host read
 * @FAKE_USB_STAT_PARTIAL_READS: reads completed with part of a packet, the rest left for the next read (WinUSB
 * ALLOW_PARTIAL_READS)
 * @FAKE_USB_STAT_AUTO_CLEARED_STALLS: halts cleared by the WinUSB AUTO_CLEAR_STALL policy
 * @FAKE_USB_STAT_MEMORY_LIMIT_REJECTIONS: transfers refused by fake_usb_set_submit_memory_limit()
 * @FAKE_USB_STAT_SCRIPTED_FAILURES: transfers failed by fake_usb_device_fail_transfers()
 */

typedef enum {
	FAKE_USB_STAT_FRAMES_STARTED,
	FAKE_USB_STAT_FRAMES_SENT,
	FAKE_USB_STAT_STREAM_BYTES_SENT,
	FAKE_USB_STAT_STREAM_READS,
	FAKE_USB_STAT_UNALIGNED_STREAM_READS,
	FAKE_USB_STAT_MAX_STREAM_READ,
	FAKE_USB_STAT_MAX_PENDING_STREAM_READS,
	FAKE_USB_STAT_STREAM_FLUSHES,
	FAKE_USB_STAT_COMMANDS,
	FAKE_USB_STAT_PENDING_ACKS,
	FAKE_USB_STAT_DROPPED_ACKS,
	FAKE_USB_STAT_INVALID_COMMANDS,
	FAKE_USB_STAT_RAW_IO_REJECTED_TRANSFERS,
	FAKE_USB_STAT_RAW_IO_CHANGES_WHILE_PENDING,
	FAKE_USB_STAT_OVERFLOWS,
	FAKE_USB_STAT_TIMEOUTS,
	FAKE_USB_STAT_CANCELLATIONS,
	FAKE_USB_STAT_DEVICE_STRING_FETCHES,
	FAKE_USB_STAT_SYNC_IN_EVENT_HANDLER,
	FAKE_USB_STAT_UNPLUGS,
	FAKE_USB_STAT_API_MISUSE,
	FAKE_USB_STAT_CONTROL_REQUESTS,
	FAKE_USB_STAT_STRING_REQUESTS,
	FAKE_USB_STAT_AUTO_CLAIMS,
	FAKE_USB_STAT_ZERO_LENGTH_PACKETS,
	FAKE_USB_STAT_PARTIAL_READS,
	FAKE_USB_STAT_AUTO_CLEARED_STALLS,
	FAKE_USB_STAT_MEMORY_LIMIT_REJECTIONS,
	FAKE_USB_STAT_SCRIPTED_FAILURES,
	FAKE_USB_N_STATS
} FakeUsbStat;

typedef struct _FakeUsbDevice FakeUsbDevice;

/**
 * FakeUsbHandleEventsHook:
 * @ctx: the context passed to libusb_handle_events*()
 * @user_data: data given to fake_usb_set_handle_events_hook()
 *
 * Called on the calling thread at the start of every libusb_handle_events*() call made by the application, before
 * the event lock is taken and without the fake lock held. It must not call libusb functions of the same context. On
 * Windows, the fake also records the thread priority itself, see fake_usb_device_get_event_thread_priority().
 */

typedef void (*FakeUsbHandleEventsHook) (libusb_context *ctx, gpointer user_data);

/**
 * FakeUsbTransferHook:
 * @device: the target device
 * @endpoint: the endpoint address (0x00 or 0x80 for control transfers)
 * @length: the requested length, in bytes (the data stage for control transfers)
 * @is_synchronous: whether it comes from libusb_bulk_transfer(), libusb_interrupt_transfer() or
 * libusb_control_transfer() (string descriptor reads included) rather than libusb_submit_transfer()
 * @user_data: data given to fake_usb_set_transfer_hook()
 *
 * Called on the calling thread for every transfer request, before it is validated (so rejected requests are seen
 * too), without the fake lock held. It must not call libusb functions.
 */

typedef void (*FakeUsbTransferHook) (FakeUsbDevice *device, guint8 endpoint, int length, gboolean is_synchronous,
				     gpointer user_data);

/* Global state
 *
 * fake_usb_reset(): removes every device (unplugging it first), and puts the platform, capabilities, version,
 * hotplug timing, memory limit, counters, statistics, hooks and injected errors back to their defaults. Contexts and
 * handles opened by Aravis survive. The time scale is kept.
 *
 * fake_usb_set_capability(): takes effect for later calls, on every context. Unlike real libusb, whose capabilities
 * are fixed for a build, it may change while contexts exist: hotplug callbacks registered before stay registered and
 * can still be deregistered.
 *
 * fake_usb_inject_error(): the next @count calls of @call (FAKE_USB_CALL_INIT, FAKE_USB_CALL_GET_DEVICE_LIST or
 * FAKE_USB_CALL_HOTPLUG_REGISTER_CALLBACK) fail with @error; @count < 0 means until cleared with @error = 0.
 *
 * fake_usb_set_hotplug_delay(): hotplug events reach the contexts @delay_ms after the device change (< 0: the
 * platform default, 10 ms on Windows, 0 elsewhere). fake_usb_set_hotplug_order(): see FakeUsbHotplugOrder.
 *
 * fake_usb_set_submit_memory_limit(): a usbfs-like memory budget, shared by every device as the usbfs_memory_mb limit
 * of Linux is: a bulk or interrupt transfer (asynchronous or synchronous) is refused with @error (0 means
 * LIBUSB_ERROR_NO_MEM, as libusb's linux_usbfs.c reports it) when the bytes of the transfers not reaped yet plus its
 * own length exceed @limit. A transfer is reaped when its completion is dispatched by event handling. @limit 0 removes
 * the limit.
 *
 * fake_usb_set_time_scale(): multiplies every device side delay and timeout the fake emulates (frame periods, the
 * response time and pending acknowledge timeout the camera advertises, acknowledge, control and hotplug delays), for
 * slow runs such as valgrind's. Process wide, not reset by fake_usb_reset(). tests/fakeuvtest.c sets it from the
 * FAKEUV_TIME_SCALE environment variable.
 *
 * fake_usb_get_total_stat(): a statistic summed over every device (or the highest value, for the MAX ones),
 * including devices removed since the last fake_usb_reset() or fake_usb_reset_counters();
 * fake_usb_dup_last_misuse() describes the last API misuse.
 *
 * fake_usb_reset_counters(): resets call counts and statistics, keeping a pending fake_usb_device_unplug_after_frames()
 * (its target is counted from the reset) and clearing the recorded event thread priorities. */

void			fake_usb_reset				(void);
void			fake_usb_set_platform			(FakeUsbPlatform platform);
FakeUsbPlatform		fake_usb_get_platform			(void);
void			fake_usb_set_capability			(guint32 capability, gboolean available);
void			fake_usb_set_dev_mem_supported		(gboolean supported);
void			fake_usb_set_version			(guint16 major, guint16 minor, guint16 micro,
								 guint16 nano);
void			fake_usb_inject_error			(FakeUsbCall call, int error, int count);
void			fake_usb_set_hotplug_delay		(int delay_ms);
void			fake_usb_set_hotplug_order		(FakeUsbHotplugOrder order);
void			fake_usb_set_submit_memory_limit	(gsize limit, int error);
void			fake_usb_set_time_scale			(guint scale);
guint			fake_usb_get_time_scale			(void);

guint64			fake_usb_get_call_count			(FakeUsbCall call);
const char *		fake_usb_call_to_string			(FakeUsbCall call);
guint64			fake_usb_get_total_stat			(FakeUsbStat stat);
char *			fake_usb_dup_last_misuse		(void);
void			fake_usb_reset_counters			(void);
guint			fake_usb_get_n_contexts			(void);
guint			fake_usb_get_n_open_handles		(void);
guint			fake_usb_get_n_transfers		(void);

void			fake_usb_set_handle_events_hook		(FakeUsbHandleEventsHook hook, gpointer user_data);
void			fake_usb_set_transfer_hook		(FakeUsbTransferHook hook, gpointer user_data);

guint32			fake_usb_checksum			(const void *data, gsize size);

/* Devices
 *
 * fake_usb_device_new() creates a plugged-in device with no interface (bcdUSB 3.00, bus 1, a new address), which
 * fake_usb_device_remove() unplugs and forgets. Interfaces get numbers 0, 1, 2... in creation order;
 * fake_usb_device_add_endpoint() takes a LIBUSB_TRANSFER_TYPE_* value. Strings are UTF-8, by descriptor index;
 * fake_usb_device_add_string() returns the next free index. fake_usb_device_set_max_packet_size() sets one
 * endpoint, or every bulk endpoint when @endpoint is 0. All descriptors can be changed at any time;
 * libusb_get_config_descriptor() always returns a fresh copy. A replugged device gets a new address; call
 * fake_usb_device_set_location() after fake_usb_device_plug() to choose it. */

FakeUsbDevice *		fake_usb_device_new			(guint16 vendor_id, guint16 product_id,
								 guint8 device_class, guint8 device_subclass,
								 guint8 device_protocol);
void			fake_usb_device_remove			(FakeUsbDevice *device);

guint8			fake_usb_device_add_interface		(FakeUsbDevice *device,
								 guint8 interface_class, guint8 interface_subclass,
								 guint8 interface_protocol,
								 const guint8 *extra, gsize extra_length);
void			fake_usb_device_add_endpoint		(FakeUsbDevice *device, guint8 interface_number,
								 guint8 address, guint8 transfer_type,
								 guint16 max_packet_size);
void			fake_usb_device_remove_endpoints	(FakeUsbDevice *device, guint8 interface_number);
void			fake_usb_device_set_class		(FakeUsbDevice *device, guint8 device_class,
								 guint8 device_subclass, guint8 device_protocol);
void			fake_usb_device_set_bcd_usb		(FakeUsbDevice *device, guint16 bcd_usb);
guint8			fake_usb_device_add_string		(FakeUsbDevice *device, const char *string);
void			fake_usb_device_set_string		(FakeUsbDevice *device, guint8 index,
								 const char *string);
void			fake_usb_device_set_descriptor_strings	(FakeUsbDevice *device, guint8 i_manufacturer,
								 guint8 i_product, guint8 i_serial_number);
void			fake_usb_device_set_location		(FakeUsbDevice *device, guint8 bus_number,
								 guint8 device_address, guint8 port_number);
void			fake_usb_device_set_max_packet_size	(FakeUsbDevice *device, guint8 endpoint,
								 guint16 max_packet_size);

/* Failure scripting
 *
 * fake_usb_device_inject_error(): the next @count calls of @call on @device fail with @error (a negative
 * LIBUSB_ERROR_* code) before doing anything; @count < 0 means until cleared. @target restricts the injection to one
 * endpoint address (transfers, clear halt, RAW_IO calls), one interface number (claim, release) or one string index
 * (GET_STRING_DESCRIPTOR_ASCII); -1 matches any. Supported calls: OPEN, GET_DEVICE_DESCRIPTOR, GET_CONFIG_DESCRIPTOR
 * (libusb_get_max_packet_size() then fails with LIBUSB_ERROR_OTHER, as libusb reads the config descriptor too),
 * GET_DEVICE_STRING, GET_MAX_PACKET_SIZE, CLAIM_INTERFACE, RELEASE_INTERFACE, SET_AUTO_DETACH_KERNEL_DRIVER,
 * GET_STRING_DESCRIPTOR_ASCII, CONTROL_TRANSFER (string descriptor reads included), BULK_TRANSFER,
 * INTERRUPT_TRANSFER, SUBMIT_TRANSFER, CLEAR_HALT, RESET_DEVICE, DEV_MEM_ALLOC (returns NULL),
 * ENDPOINT_SUPPORTS_RAW_IO, ENDPOINT_SET_RAW_IO, GET_MAX_RAW_IO_TRANSFER_SIZE.
 *
 * fake_usb_device_inject_transfer_status(): the next @count transfers submitted (or performed synchronously) on
 * @endpoint are accepted, then complete at once with @status and no data, as if the hardware had failed them. When
 * @count is not 0, the transfers already pending on @endpoint fail first, at once, with @status and the bytes they
 * received, in the order they were submitted, as on a pipe that starts failing: no transfer completes before one
 * submitted earlier on the same endpoint. They do not count in @count.
 *
 * fake_usb_device_fail_transfers(): once @after_bytes more bytes have been delivered on the IN @endpoint (0: at
 * once), the transfer receiving data and every other transfer pending on @endpoint complete with @status, keeping the
 * bytes they received; the device stays connected. With @flush, the device also drops what it has not sent yet on the
 * endpoint (a camera then starts its next frame with a leader); without it, the next reads get the rest of the data.
 * If nothing is pending when the failure triggers, the next transfer submitted on @endpoint fails.
 *
 * fake_usb_device_halt_endpoint(): the device stalls the IN @endpoint, as after a SET_FEATURE (ENDPOINT_HALT) request:
 * the transfers pending on it fail with LIBUSB_TRANSFER_STALL, and so do later ones until libusb_clear_halt(), unless
 * WinUSB's AUTO_CLEAR_STALL policy clears the halt (Windows, RAW_IO off). What the device has not sent yet on the
 * endpoint is dropped, and dropped again when the halt is cleared.
 *
 * fake_usb_device_set_control_delay(): the device answers control requests @delay_ms after they are sent
 * (FAKE_USB_NEVER: never); a transfer whose timeout expires first completes with LIBUSB_TRANSFER_TIMED_OUT and the
 * request has no effect.
 *
 * fake_usb_device_set_pipe_policies(): the WinUSB ALLOW_PARTIAL_READS and AUTO_CLEAR_STALL policies (1: on, 0: off,
 * -1: the platform default, on for Windows). Neither applies to an endpoint with RAW_IO on.
 *
 * fake_usb_device_set_open_error() and fake_usb_device_set_config_descriptor_error() are shortcuts for a permanent
 * injection (0 clears it). A camera whose config descriptor is unavailable: set_config_descriptor_error
 * (LIBUSB_ERROR_NOT_FOUND). A data interface with no endpoint: fake_usb_device_remove_endpoints (camera,
 * FAKE_USB_CAMERA_DATA_INTERFACE). USB3 Vision interfaces on a device of another class: fake_usb_device_set_class
 * (camera, 0x00, 0x00, 0x00). Second interface claim failing as with libusb < 1.0.27 on Windows:
 * fake_usb_device_inject_error (camera, FAKE_USB_CALL_CLAIM_INTERFACE, FAKE_USB_CAMERA_DATA_INTERFACE,
 * LIBUSB_ERROR_NOT_SUPPORTED, -1). Linux usbfs memory exhaustion: fake_usb_set_submit_memory_limit(), or a counted
 * (camera, FAKE_USB_CALL_SUBMIT_TRANSFER, FAKE_USB_CAMERA_DATA_ENDPOINT_IN, LIBUSB_ERROR_NO_MEM, n). A probe that times
 * out: fake_usb_device_set_control_delay (camera, FAKE_USB_NEVER), or a counted (camera,
 * FAKE_USB_CALL_CONTROL_TRANSFER, -1, LIBUSB_ERROR_TIMEOUT, 1).
 *
 * fake_usb_device_set_exclusive_open(): a second libusb_open() while a handle is open fails with
 * LIBUSB_ERROR_ACCESS, as with WinUSB. Off by default on every platform. */

void			fake_usb_device_inject_error		(FakeUsbDevice *device, FakeUsbCall call,
								 int target, int error, int count);
void			fake_usb_device_inject_transfer_status	(FakeUsbDevice *device, guint8 endpoint,
								 enum libusb_transfer_status status, int count);
void			fake_usb_device_fail_transfers		(FakeUsbDevice *device, guint8 endpoint,
								 enum libusb_transfer_status status, gsize after_bytes,
								 gboolean flush);
void			fake_usb_device_halt_endpoint		(FakeUsbDevice *device, guint8 endpoint);
void			fake_usb_device_set_control_delay	(FakeUsbDevice *device, guint delay_ms);
void			fake_usb_device_set_pipe_policies	(FakeUsbDevice *device, int allow_partial_reads,
								 int auto_clear_stall);
void			fake_usb_device_clear_injections	(FakeUsbDevice *device);
void			fake_usb_device_set_open_error		(FakeUsbDevice *device, int error);
void			fake_usb_device_set_config_descriptor_error (FakeUsbDevice *device, int error);
void			fake_usb_device_set_exclusive_open	(FakeUsbDevice *device, gboolean exclusive);

/* Plug and unplug
 *
 * fake_usb_device_unplug(): the device disappears now. Transfers in flight complete with the in-flight status, later
 * submissions fail with the submit error, synchronous calls with the synchronous error (see
 * fake_usb_device_set_unplug_errors() and FakeUsbPlatform for the defaults), libusb_open() on its stale device objects
 * fails with LIBUSB_ERROR_NO_DEVICE and it is no longer listed. libusb_claim_interface(), libusb_release_interface(),
 * libusb_clear_halt() and libusb_reset_device() fail with LIBUSB_ERROR_NO_DEVICE (core.c),
 * libusb_endpoint_set_raw_io() and libusb_get_max_raw_io_transfer_size() with LIBUSB_ERROR_OTHER (the WinUSB pipe
 * policy call fails), while libusb_endpoint_supports_raw_io() still answers 1. A LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT event
 * is queued on every context with a matching hotplug callback, after the hotplug delay.
 *
 * fake_usb_device_set_unplug_errors(): a @submit_error or @synchronous_error of 0 means the transfer is accepted, then
 * completes at once with @in_flight_status and no data (on Windows, ReadPipe can return ERROR_IO_PENDING and fail
 * asynchronously). fake_usb_device_set_unplug_transfers_hang(): transfers in flight at the unplug, and those accepted
 * afterwards, never complete by themselves: only libusb_cancel_transfer() (LIBUSB_TRANSFER_CANCELLED) or their
 * timeout end them, as long as the device is not removed.
 *
 * fake_usb_device_unplug_after_frames(): arms an unplug that happens once the host has read @n_frames more complete
 * frames (counted from this call), plus @extra_bytes of the next frame (leader, payload and trailer bytes, in order).
 * With @extra_bytes == 0 the cable is pulled right after the last trailer is read. Cameras only.
 *
 * fake_usb_device_plug(): plugs an unplugged device back, as a new device object at a new address with fresh state (a
 * camera is power cycled: registers back to their defaults, geometry back to the one set with
 * fake_usb_camera_set_geometry(), frame ids from 1, and a new ArvFakeCamera behind fake_usb_camera_get_fake_camera()).
 * A LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED event is queued on every context with a matching hotplug callback, after the
 * hotplug delay. */

void			fake_usb_device_unplug			(FakeUsbDevice *device);
void			fake_usb_device_unplug_after_frames	(FakeUsbDevice *device, guint n_frames,
								 gsize extra_bytes);
void			fake_usb_device_plug			(FakeUsbDevice *device);
void			fake_usb_device_set_unplug_errors	(FakeUsbDevice *device,
								 enum libusb_transfer_status in_flight_status,
								 int submit_error, int synchronous_error);
void			fake_usb_device_set_unplug_transfers_hang (FakeUsbDevice *device, gboolean hang);
gboolean		fake_usb_device_is_plugged		(FakeUsbDevice *device);

/* WinUSB RAW_IO emulation (only reachable through libusb >= 1.0.30 headers)
 *
 * fake_usb_device_set_raw_io_pending_change_error(): the error libusb_endpoint_set_raw_io() returns while transfers
 * are pending on the endpoint; 0 (the default) accepts the change and counts it as API misuse. */

void			fake_usb_device_set_raw_io_supported	(FakeUsbDevice *device, gboolean supported);
void			fake_usb_device_set_max_raw_io_transfer_size (FakeUsbDevice *device, int size);
void			fake_usb_device_set_raw_io_pending_change_error (FakeUsbDevice *device, int error);
gboolean		fake_usb_device_get_raw_io		(FakeUsbDevice *device, guint8 endpoint);

/* Inspection
 *
 * fake_usb_device_get_event_thread_priority(): the GetThreadPriority() of the threads that called
 * libusb_handle_events*() on a context where @device has an open handle, last and highest, recorded at each call
 * (Windows only; FALSE elsewhere or when no call was recorded since the last reset). Callbacks can also run on
 * threads doing synchronous I/O on the context, which are not recorded. */

guint64			fake_usb_device_get_call_count		(FakeUsbDevice *device, FakeUsbCall call);
guint64			fake_usb_device_get_stat		(FakeUsbDevice *device, FakeUsbStat stat);
guint			fake_usb_device_get_n_open_handles	(FakeUsbDevice *device);
guint			fake_usb_device_get_n_pending_transfers	(FakeUsbDevice *device, guint8 endpoint);
gboolean		fake_usb_device_is_interface_claimed	(FakeUsbDevice *device, guint8 interface_number);
gboolean		fake_usb_device_get_auto_detach_kernel_driver (FakeUsbDevice *device);
void			fake_usb_device_get_location		(FakeUsbDevice *device, guint8 *bus_number,
								 guint8 *device_address, guint8 *port_number);
gboolean		fake_usb_device_get_event_thread_priority (FakeUsbDevice *device, int *last_priority,
								   int *highest_priority);
void			fake_usb_device_reset_event_thread_priority (FakeUsbDevice *device);

/* USB3 Vision cameras
 *
 * fake_usb_camera_new() creates a plugged-in camera: device class EF/02/01, a control interface EF/05/00 with bulk
 * endpoints 0x01 and 0x81 and a device info block giving the string indices Aravis reads, an event interface
 * EF/05/01 with endpoint 0x82 and a data interface EF/05/02 with bulk endpoint 0x83, all with 1024 byte packets.
 * Its strings are "Aravis", "Fake", @serial_number and a GUID, so Aravis lists it as "Aravis-Fake-<serial_number>".
 * Its registers are ArvFakeCamera's, plus the U3V bootstrap (read-only), stream interface and manifest blocks Aravis
 * reads; the manifest points at ArvFakeCamera's GenICam XML. Frames are produced while the stream interface is enabled
 * and AcquisitionStart has been executed, one per frame period (AcquisitionFramePeriod, or
 * fake_usb_camera_set_frame_period()), when the host is reading and the previous frame was completely read; with
 * TriggerMode On and TriggerSource Software, one per TriggerSoftware. Their content is ArvFakeCamera's fill pattern,
 * their ids a 64 bit counter that starts at 1 at power on (fake_usb_camera_set_next_frame_id()).
 * fake_usb_camera_get_frame_checksum() gives the fake_usb_checksum() and size of the image data of each of the last
 * 4096 frames, by frame id.
 *
 * On the wire, a frame is a leader (ArvUvspLeader), the payload split as the host asked in the SIRM registers
 * (payload_count transfers of payload_size bytes, then the remainder) and a trailer (ArvUvspTrailer). Every device
 * side transfer ends with a short packet when its size is not a multiple of the max packet size; when it is, nothing
 * follows unless fake_usb_camera_set_zero_length_packets() is on (acknowledges: fake_usb_camera_set_ack_zero_length_
 * packets()). With fake_usb_camera_set_pad_final_transfer(), the remainder is padded with zeros to SIRM TRANSFER1_SIZE
 * when the host asked for more. A stream disable, or a halt set or cleared on the data endpoint, discards what the
 * host has not read yet.
 *
 * fake_usb_camera_set_payload_type (camera, ARV_UVSP_PAYLOAD_TYPE_GENDC_CONTAINER) sends frames as a GenDC container,
 * built from Aravis's ArvUvspGenDC* structures (arvuvspprivate.h): a descriptor of
 * fake_usb_camera_get_gendc_descriptor_size() bytes (container header, one intensity component with one part), then
 * the image data. The camera's SIRM REQ_PAYLOAD_SIZE includes the descriptor, its GenICam PayloadSize does not: give
 * Aravis buffers of fake_usb_camera_get_sirm_register (camera, ARV_SIRM_REQ_PAYLOAD_SIZE) bytes.
 *
 * Defaults: frame period from AcquisitionFramePeriod (25 Hz), alignment 1 << 3 in SIRM info, maximum device response
 * time 200 ms, maximum command and acknowledge transfers 1024 bytes, no zero length packets, image payloads.
 *
 * Control channel scripting: fake_usb_camera_set_pending_acks() answers the next @n_commands commands with a
 * pending acknowledge (timeout @timeout_ms), followed by the real acknowledge @delay_ms later;
 * fake_usb_camera_drop_acks() does not answer the next commands; fake_usb_camera_set_command_status() answers the
 * next commands with an error status; fake_usb_camera_truncate_acks() sends the next @n_acks acknowledges, pending
 * ones included, with their last @n_missing_bytes bytes cut off (all of them: a zero length packet).
 * fake_usb_camera_truncate_payloads() sends the next frames with @n_missing_bytes less payload, and
 * fake_usb_camera_drop_trailers() sends the next frames without their trailer: the next leader follows their payload.
 * fake_usb_camera_read_memory() reads the camera address space without USB traffic, and
 * fake_usb_camera_get_sirm_register() one 32 bit stream interface register (offsets from arvuvcpprivate.h), for
 * instance to check what Aravis wrote in ARV_SIRM_PAYLOAD_SIZE.
 *
 * Write ArvFakeCamera registers through Aravis or the fake_usb_camera_* setters, not directly while streaming;
 * arv_fake_camera_set_fill_pattern() is safe. */

FakeUsbDevice *		fake_usb_camera_new			(const char *serial_number);
FakeUsbDevice *		fake_usb_camera_new_full		(const char *serial_number,
								 const char *genicam_filename);
gboolean		fake_usb_device_is_camera		(FakeUsbDevice *device);
ArvFakeCamera *		fake_usb_camera_get_fake_camera		(FakeUsbDevice *camera);
const char *		fake_usb_camera_get_guid		(FakeUsbDevice *camera);
char *			fake_usb_camera_dup_device_id		(FakeUsbDevice *camera);

void			fake_usb_camera_set_geometry		(FakeUsbDevice *camera, guint32 width, guint32 height,
								 ArvPixelFormat pixel_format);
void			fake_usb_camera_set_frame_period	(FakeUsbDevice *camera, guint64 period_us);
void			fake_usb_camera_set_alignment_shift	(FakeUsbDevice *camera, guint alignment_shift);
void			fake_usb_camera_set_max_transfers	(FakeUsbDevice *camera, guint32 max_cmd_transfer,
								 guint32 max_ack_transfer);
void			fake_usb_camera_set_response_time	(FakeUsbDevice *camera, guint32 response_time_ms);
void			fake_usb_camera_set_zero_length_packets	(FakeUsbDevice *camera, gboolean enable);
void			fake_usb_camera_set_ack_zero_length_packets (FakeUsbDevice *camera, gboolean enable);
void			fake_usb_camera_set_pad_final_transfer	(FakeUsbDevice *camera, gboolean pad);
void			fake_usb_camera_set_next_frame_id	(FakeUsbDevice *camera, guint64 frame_id);
void			fake_usb_camera_set_payload_type	(FakeUsbDevice *camera, guint16 payload_type);
gsize			fake_usb_camera_get_gendc_descriptor_size (void);
void			fake_usb_camera_set_pending_acks	(FakeUsbDevice *camera, guint n_commands,
								 guint16 timeout_ms, guint delay_ms);
void			fake_usb_camera_drop_acks		(FakeUsbDevice *camera, guint n_commands);
void			fake_usb_camera_truncate_acks		(FakeUsbDevice *camera, guint n_acks,
								 gsize n_missing_bytes);
void			fake_usb_camera_set_command_status	(FakeUsbDevice *camera, guint n_commands,
								 guint16 status);
void			fake_usb_camera_truncate_payloads	(FakeUsbDevice *camera, guint n_frames,
								 gsize n_missing_bytes);
void			fake_usb_camera_drop_trailers		(FakeUsbDevice *camera, guint n_frames);

gboolean		fake_usb_camera_read_memory		(FakeUsbDevice *camera, guint64 address, guint32 size,
								 void *buffer);
guint32			fake_usb_camera_get_sirm_register	(FakeUsbDevice *camera, guint32 offset);
gboolean		fake_usb_camera_is_streaming		(FakeUsbDevice *camera);
gboolean		fake_usb_camera_get_frame_checksum	(FakeUsbDevice *camera, guint64 frame_id,
								 guint32 *checksum, gsize *size);

G_END_DECLS

#endif
