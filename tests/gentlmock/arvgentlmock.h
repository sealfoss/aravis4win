/* SPDX-License-Identifier:Unlicense */

/*
 * Mock GenTL producer for the GenTL consumer tests ('gentl' suite).
 *
 * The producer implements the GenTL C API as src/arvgentlprivate.h declares it. It exposes one system, one interface
 * by default (ARV_GENTL_MOCK_INTERFACE_ID), ARV_GENTL_MOCK_N_DEVICES devices per interface (one by default) named
 * ARV_GENTL_MOCK_GENTL_DEVICE_ID<n>, and one data stream per device. Each remote device is an ArvFakeCamera: its
 * registers are reached through GCReadPort and GCWritePort, and its fill pattern generates the frames, at the frame
 * rate of the fake camera.
 *
 * Variants, built by tests/gentlmock/meson.build:
 *
 *   ArvGenTLMock.cti         every function of src/arvgentlprivate.h
 *   ArvGenTLMockMinimal.cti  without the functions the consumer treats as optional
 *   ArvGenTLMockBroken.cti   without TLOpen, which the consumer requires
 *   ArvGenTLMockDep.cti      (Windows) depends on arvgentlmockhelper.dll, only found in its folder
 *
 * The transport layer vendor is ARV_GENTL_MOCK_TL_VENDOR followed by the variant name ("Minimal", "Broken" or
 * "Dep"), the device serial number ARV_GENTL_MOCK_SERIAL followed by the device index. The Aravis id of the first
 * device of ArvGenTLMock.cti is then "MockTL-MockVendor-MockCamera-MOCK0".
 *
 * Its behaviour is scripted with the following options, read from the environment by GCInitLib, or changed by
 * arv_gentl_mock_set_option() once the producer is initialized (GCInitLib resets them):
 *
 *   ARV_GENTL_MOCK_N_DEVICES         number of devices, 0 to 8 (environment only), on each interface
 *   ARV_GENTL_MOCK_N_INTERFACES      number of interfaces, 1 to 4 (environment only). Interface <k> is
 *                                    ARV_GENTL_MOCK_INTERFACE_ID_PREFIX<k>; its devices have the same GenTL device
 *                                    ids as the devices of the other interfaces, and, beyond the first interface,
 *                                    the serial number ARV_GENTL_MOCK_SERIAL<n>-IF<k>, also in their fake camera
 *   ARV_GENTL_MOCK_MISSING_INFO      comma separated list of TL_INFO_xxx, INTERFACE_INFO_xxx and DEVICE_INFO_xxx
 *                                    commands, for which the info functions fail with GC_ERR_NOT_AVAILABLE
 *   ARV_GENTL_MOCK_EMPTY_INFO        same, for string commands returning an empty string
 *   ARV_GENTL_MOCK_FAIL              comma separated list of <function>[@<n>][:<error>]: the GenTL function fails
 *                                    with <error> (GC_ERR_ERROR by default) from its <n>th call after GCInitLib,
 *                                    or after the option change (first call by default),
 *                                    e.g. "DSStartAcquisition,DSAllocAndAnnounceBuffer@3:-1010"
 *   ARV_GENTL_MOCK_EVENT_KILL        EventKill behaviour: "latch" (default, a kill without waiting thread aborts the
 *                                    next wait), "waiters" (only aborts a wait in progress, the kill is otherwise
 *                                    lost) or "ignore" (no effect)
 *   ARV_GENTL_MOCK_INCOMPLETE_EVERY  every <n>th frame is delivered incomplete, half filled (0, default: never)
 *   ARV_GENTL_MOCK_FRAME_PERIOD_US   frame period in µs (0, default: the frame period of the fake camera)
 *   ARV_GENTL_MOCK_BUF_ANNOUNCE_MIN  minimum number of announced buffers for DSStartAcquisition (default 1)
 *   ARV_GENTL_MOCK_N_PARTS           DSGetNumBufferParts result: "none" (default, GC_ERR_NOT_AVAILABLE) or a part
 *                                    count (0 or 1)
 *   ARV_GENTL_MOCK_CLOSE_DELAY_US    TLClose, IFClose and DevClose wait this long (1 s at most), without the mock
 *                                    lock, before closing, which widens the race windows of the consumer (0,
 *                                    default: no delay)
 *   ARV_GENTL_MOCK_DEVICE_EVENT_ID   up to 8 hexadecimal digits: the remote device event (EVENT_REMOTE_DEVICE) of
 *                                    each open device fires at the frame period, with this id (EVENT_DATA_ID),
 *                                    given without the zero ending a string, and a 64-bit value (EVENT_DATA_VALUE),
 *                                    the number of the event since the counter reset, in the host byte order (empty,
 *                                    default: no remote device event)
 *   ARV_GENTL_MOCK_SIZE_EXCESS       the complete frames tell a filled size (BUFFER_INFO_SIZE_FILLED) and a data
 *                                    size (BUFFER_INFO_DATA_SIZE, and BUFFER_PART_INFO_DATA_SIZE) larger than their
 *                                    buffer by this many bytes, without BUFFER_INFO_DATA_LARGER_THAN_BUFFER (0,
 *                                    default: the actual sizes)
 *   ARV_GENTL_MOCK_IMAGE_OFFSET      the data of each frame starts with this many bytes (at most
 *                                    ARV_GENTL_MOCK_IMAGE_OFFSET_MAX) of other data, like chunk data, filled with
 *                                    ARV_GENTL_MOCK_IMAGE_OFFSET_FILL, followed by the image: BUFFER_INFO_IMAGEOFFSET
 *                                    tells this offset, the data sizes (BUFFER_INFO_SIZE_FILLED, BUFFER_INFO_DATA_SIZE)
 *                                    count these bytes, and the image part of the multi-part functions
 *                                    (BUFFER_PART_INFO_BASE, BUFFER_PART_INFO_DATA_SIZE) starts after them (0, default)
 *
 * The control functions below are exported by the producer, and reached through GModule:
 *
 *   int arv_gentl_mock_get_counter (const char *name);
 *       "<GenTL function>"    number of calls of this function
 *       "violations"          number of API misuses: calls before GCInitLib, calls with an invalid handle, modules
 *                             closed before their children, queued buffers revoked, data stream closed with
 *                             announced buffers or while acquiring, ...
 *       "kills-unregistered"  EventKill calls on an unregistered event
 *       "frames"              number of delivered frames
 *       "underruns"           number of frames lost for lack of queued buffer
 *       "new-buffer-waits"    EventGetData calls on new buffer events, scripted failures included (the waits of the
 *                             device event thread of -Devent=true builds are not counted)
 *       "device-events"       number of remote device events delivered
 *       "new-buffer-waiters"  current number of threads waiting in EventGetData for a new buffer event
 *       "new-buffer-events"   current number of registered new buffer events
 *       "initialized"         1 between GCInitLib and GCCloseLib, 0 otherwise
 *       "open-tl", "open-interfaces", "open-devices", "open-streams", "announced-buffers", "registered-events"
 *                             current number of these objects
 *     Counters other than the current numbers are kept until arv_gentl_mock_reset_counters().
 *
 *   const char *arv_gentl_mock_get_last_violation (void);
 *   gboolean arv_gentl_mock_set_option (const char *name, const char *value);  NULL value restores the default
 *   gboolean arv_gentl_mock_set_ids (guint interface_index, const char *interface_id, const char *device_id);
 *       Changes the interface id and the id of its first device between discoveries, while the producer is
 *       initialized with no open transport layer. NULL leaves that id unchanged; the camera serial numbers stay
 *       unchanged, so a test can tell which camera an id reaches. Nonempty interface ids must fit in 32 bytes.
 *   void arv_gentl_mock_reset_counters (void);
 */

#ifndef ARV_GENTL_MOCK_H
#define ARV_GENTL_MOCK_H

#include <glib.h>

G_BEGIN_DECLS

#define ARV_GENTL_MOCK_TL_VENDOR		"MockTL"
#define ARV_GENTL_MOCK_VENDOR			"MockVendor"
#define ARV_GENTL_MOCK_MODEL			"MockCamera"
#define ARV_GENTL_MOCK_SERIAL			"MOCK"
#define ARV_GENTL_MOCK_TL_TYPE			"Custom"
#define ARV_GENTL_MOCK_INTERFACE_ID_PREFIX	"MockInterface"
#define ARV_GENTL_MOCK_INTERFACE_ID		ARV_GENTL_MOCK_INTERFACE_ID_PREFIX "0"
#define ARV_GENTL_MOCK_GENTL_DEVICE_ID		"MockDevice"
#define ARV_GENTL_MOCK_STREAM_ID		"MockStream0"

#define ARV_GENTL_MOCK_MAX_DEVICES		8
#define ARV_GENTL_MOCK_MAX_INTERFACES		4

#define ARV_GENTL_MOCK_IMAGE_OFFSET_MAX		65536
#define ARV_GENTL_MOCK_IMAGE_OFFSET_FILL	0xa5

#define ARV_GENTL_MOCK_GET_COUNTER		"arv_gentl_mock_get_counter"
#define ARV_GENTL_MOCK_GET_LAST_VIOLATION	"arv_gentl_mock_get_last_violation"
#define ARV_GENTL_MOCK_SET_OPTION		"arv_gentl_mock_set_option"
#define ARV_GENTL_MOCK_SET_IDS		"arv_gentl_mock_set_ids"
#define ARV_GENTL_MOCK_RESET_COUNTERS		"arv_gentl_mock_reset_counters"

typedef int		(*ArvGenTLMockGetCounter)		(const char *name);
typedef const char *	(*ArvGenTLMockGetLastViolation)		(void);
typedef gboolean	(*ArvGenTLMockSetOption)		(const char *name, const char *value);
typedef gboolean	(*ArvGenTLMockSetIds)		(guint interface_index, const char *interface_id,
							 const char *device_id);
typedef void		(*ArvGenTLMockResetCounters)		(void);

G_END_DECLS

#endif
