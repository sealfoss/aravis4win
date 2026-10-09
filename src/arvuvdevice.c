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
 * SECTION: arvuvdevice
 * @short_description: USB3Vision device
 */

#include <arvuvstreamprivate.h>
#include <arvuvdeviceprivate.h>
#include <arvuvinterfaceprivate.h>
#include <arvuvcpprivate.h>
#include <arvgc.h>
#include <arvdebug.h>
#include <arvenumtypes.h>
#include <libusb.h>
#include <string.h>
#include <arvstr.h>
#include <arvzip.h>
#include <arvmisc.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

enum
{
	PROP_0,
	PROP_UV_DEVICE_VENDOR,
	PROP_UV_DEVICE_PRODUCT,
	PROP_UV_DEVICE_SERIAL_NUMBER,
	PROP_UV_DEVICE_GUID
};

#define ARV_UV_DEVICE_N_TRIES_MAX	5

/* arv_uv_device_check_connection () */
#define ARV_UV_DEVICE_CHECK_CONNECTION_TIMEOUT_MS	100
/* Control transfers failing: at most one check per interval, see _send_cmd_and_receive_ack () */
#define ARV_UV_DEVICE_CHECK_CONNECTION_INTERVAL_MS	100

/* arv_uv_device_request_event_thread_priority (): no stream thread asks for a priority */
#define ARV_UV_DEVICE_NO_THREAD_PRIORITY		G_MININT

typedef struct {
	char *vendor;
	char *product;
	char *serial_number;
	char *guid;

        unsigned int usb_major_version;
        unsigned int usb_minor_version;

	libusb_context *usb;
	libusb_device_handle *usb_device;

       	libusb_hotplug_callback_handle hotplug_cb_handle;
	gboolean hotplug_cb_registered;

	ArvGc *genicam;

	char *genicam_xml;
	size_t genicam_xml_size;

	guint16 packet_id;

	guint timeout_ms;
	guint cmd_packet_size_max;
	guint ack_packet_size_max;
	guint control_interface;
	guint data_interface;
	guint8 control_endpoint_out;
	guint8 control_endpoint_in;
	guint8 data_endpoint;
	gboolean disconnected;

	ArvUvUsbMode usb_mode;
        guint64 maximum_transfer_size;
        gint raw_io_disabled;
        /* Atomic, see arv_uv_device_get_raw_io () */
        gint raw_io;

        /* Atomic, see arv_uv_device_take_stream_interface () */
        gint n_stream_interface_setups;
        gint stream_interface_owner;

	int event_thread_run;
	GThread* event_thread;
        /* Atomic, see arv_uv_device_request_event_thread_priority () */
        gint event_thread_priority;

        GMutex transfer_mutex;
        /* Under transfer_mutex: the last connection check after failed control transfers, see
         * _send_cmd_and_receive_ack () */
        gint64 last_control_check_time;
} ArvUvDevicePrivate;

struct _ArvUvDevice {
	ArvDevice device;
};

struct _ArvUvDeviceClass {
	ArvDeviceClass parent_class;
};

G_DEFINE_TYPE_WITH_CODE (ArvUvDevice, arv_uv_device, ARV_TYPE_DEVICE, G_ADD_PRIVATE (ArvUvDevice))

static ArvDeviceError
arv_uvcp_status_to_device_error (ArvUvcpStatus status)
{
        switch (status) {
                case ARV_UVCP_STATUS_NOT_IMPLEMENTED:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_NOT_IMPLEMENTED;
                case ARV_UVCP_STATUS_INVALID_PARAMETER:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_INVALID_PARAMETER;
                case ARV_UVCP_STATUS_INVALID_ADDRESS:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_INVALID_ADDRESS;
                case ARV_UVCP_STATUS_WRITE_PROTECT:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_WRITE_PROTECT;
                case ARV_UVCP_STATUS_BAD_ALIGNMENT:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_BAD_ALIGNMENT;
                case ARV_UVCP_STATUS_ACCESS_DENIED:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_ACCESS_DENIED;
                case ARV_UVCP_STATUS_BUSY:
                        return ARV_DEVICE_ERROR_PROTOCOL_ERROR_BUSY;
                default:
                        break;
        }

        return ARV_DEVICE_ERROR_PROTOCOL_ERROR;
}

/* The functions libusb 1.0.30 added */

#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C

#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP

/* Windows binds the imports of a DLL when it loads it: an Aravis that called these functions directly would not load
 * at all with an older libusb-1.0.dll first in the DLL search order (the folder of the program, the system folders,
 * PATH), or already loaded in the process by another component. So they are looked up at run time, when one is needed
 * for the first time, in the module that holds libusb: the one that holds a string libusb returns. Not the one of the
 * address of libusb_init (): declared without dllimport, as libusb.h does, that is the address of a stub, in the module
 * of Aravis, which jumps to the function. A function that is not found is not available, as with another backend than
 * WinUSB: no RAW_IO, no device strings in the open errors.
 *
 * A libusb linked statically exports nothing, and its functions are not found. The build calls them directly then
 * (ARV_UV_LIBUSB_STATIC, see src/meson.build), and this code is not used. If libusb turns out to be in the module of
 * Aravis anyway, linked statically in a way the build did not see, the log says so, rather than blaming an older
 * libusb. */

typedef int (LIBUSB_CALL *ArvUvLibusbEndpointSupportsRawIo) (libusb_device_handle *dev_handle, uint8_t endpoint);
typedef int (LIBUSB_CALL *ArvUvLibusbEndpointSetRawIo) (libusb_device_handle *dev_handle, uint8_t endpoint,
                                                         int enable);
typedef int (LIBUSB_CALL *ArvUvLibusbGetMaxRawIoTransferSize) (libusb_device_handle *dev_handle, uint8_t endpoint);
typedef int (LIBUSB_CALL *ArvUvLibusbGetDeviceString) (libusb_device *dev,
                                                        enum libusb_device_string_type string_type,
                                                        char *data, int length);

typedef struct {
        ArvUvLibusbEndpointSupportsRawIo endpoint_supports_raw_io;
        ArvUvLibusbEndpointSetRawIo endpoint_set_raw_io;
        ArvUvLibusbGetMaxRawIoTransferSize get_max_raw_io_transfer_size;
        ArvUvLibusbGetDeviceString get_device_string;
} ArvUvLibusbFunctions;

static GMutex arv_uv_libusb_mutex;
static ArvUvLibusbFunctions arv_uv_libusb_functions;

/* GetProcAddress () returns a FARPROC, converted through void (*) (void), from which any function pointer type converts
 * without a warning */
#define ARV_UV_LIBUSB_PROC(module,name)	((void (*) (void)) GetProcAddress ((module), (name)))

static HMODULE
_get_module (gconstpointer address)
{
        HMODULE module = NULL;

        if (address == NULL ||
            !GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 (LPCWSTR) address, &module))
                return NULL;

        return module;
}

static void
_look_libusb_functions_up_locked (gconstpointer libusb_address, gconstpointer aravis_address)
{
        ArvUvLibusbFunctions functions = { NULL, NULL, NULL, NULL };
        HMODULE module;
        char *module_name = NULL;
        const char *missing = "missing (libusb older than 1.0.30)";

        module = _get_module (libusb_address);
        if (module != NULL) {
                wchar_t filename[MAX_PATH];

                functions.endpoint_supports_raw_io = (ArvUvLibusbEndpointSupportsRawIo)
                        ARV_UV_LIBUSB_PROC (module, "libusb_endpoint_supports_raw_io");
                functions.endpoint_set_raw_io = (ArvUvLibusbEndpointSetRawIo)
                        ARV_UV_LIBUSB_PROC (module, "libusb_endpoint_set_raw_io");
                functions.get_max_raw_io_transfer_size = (ArvUvLibusbGetMaxRawIoTransferSize)
                        ARV_UV_LIBUSB_PROC (module, "libusb_get_max_raw_io_transfer_size");
                functions.get_device_string = (ArvUvLibusbGetDeviceString)
                        ARV_UV_LIBUSB_PROC (module, "libusb_get_device_string");

                if (GetModuleFileNameW (module, filename, G_N_ELEMENTS (filename)) > 0) {
                        filename[G_N_ELEMENTS (filename) - 1] = L'\0';
                        module_name = g_utf16_to_utf8 ((const gunichar2 *) filename, -1, NULL, NULL, NULL);
                }

                /* libusb in the program or DLL that holds Aravis: it is linked statically there, and exports nothing,
                 * whatever its version */
                if (module == _get_module (aravis_address))
                        missing = "not exported (libusb linked statically, Aravis built for a libusb DLL)";
        }

        /* RAW_IO takes the three of them */
        if (functions.endpoint_supports_raw_io == NULL ||
            functions.endpoint_set_raw_io == NULL ||
            functions.get_max_raw_io_transfer_size == NULL) {
                functions.endpoint_supports_raw_io = NULL;
                functions.endpoint_set_raw_io = NULL;
                functions.get_max_raw_io_transfer_size = NULL;
        }

        arv_info_device ("[UvDevice::libusb] %s: RAW_IO functions %s, device string function %s",
                         module_name != NULL ? module_name : "module of libusb not found",
                         functions.endpoint_supports_raw_io != NULL ? "found" : missing,
                         functions.get_device_string != NULL ? "found" : missing);

        arv_uv_libusb_functions = functions;

        g_free (module_name);
}

static ArvUvLibusbFunctions
_get_libusb_functions (void)
{
        static gsize looked_up = 0;
        ArvUvLibusbFunctions functions;

        if (g_once_init_enter (&looked_up)) {
                /* Before taking the lock: libusb is called with no lock of Aravis held */
                const char *libusb_string = libusb_error_name (LIBUSB_SUCCESS);

                g_mutex_lock (&arv_uv_libusb_mutex);
                _look_libusb_functions_up_locked (libusb_string, &arv_uv_libusb_functions);
                g_mutex_unlock (&arv_uv_libusb_mutex);

                g_once_init_leave (&looked_up, 1);
        }

        g_mutex_lock (&arv_uv_libusb_mutex);
        functions = arv_uv_libusb_functions;
        g_mutex_unlock (&arv_uv_libusb_mutex);

        return functions;
}

/*
 * arv_uv_libusb_look_functions_up:
 * @libusb_address: (nullable): an address inside the module to look the functions up in, or %NULL for the module of
 * libusb
 * @aravis_address: (nullable): an address inside the module to take for the one of Aravis, or %NULL for the module of
 * Aravis
 *
 * For tests: looks the functions libusb 1.0.30 added up again, in a module that may lack them, as an older
 * libusb-1.0.dll does, or in the module of libusb. A module that lacks them and holds Aravis, as one that libusb is
 * linked into statically does, is logged as such. Call it while no device is in use.
 *
 * Returns: %TRUE if every function was found.
 */

gboolean
arv_uv_libusb_look_functions_up (gconstpointer libusb_address, gconstpointer aravis_address)
{
        const char *libusb_string = libusb_error_name (LIBUSB_SUCCESS);
        gboolean found;

        /* The first look up is over, and does not override this one */
        _get_libusb_functions ();

        g_mutex_lock (&arv_uv_libusb_mutex);
        _look_libusb_functions_up_locked (libusb_address != NULL ? libusb_address : libusb_string,
                                          aravis_address != NULL ? aravis_address : &arv_uv_libusb_functions);
        found = arv_uv_libusb_functions.endpoint_supports_raw_io != NULL &&
                arv_uv_libusb_functions.get_device_string != NULL;
        g_mutex_unlock (&arv_uv_libusb_mutex);

        return found;
}

#endif

int
arv_uv_libusb_endpoint_supports_raw_io (libusb_device_handle *dev_handle, uint8_t endpoint)
{
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
        ArvUvLibusbFunctions functions = _get_libusb_functions ();

        if (functions.endpoint_supports_raw_io == NULL)
                return LIBUSB_ERROR_NOT_SUPPORTED;

        return functions.endpoint_supports_raw_io (dev_handle, endpoint);
#else
        return libusb_endpoint_supports_raw_io (dev_handle, endpoint);
#endif
}

int
arv_uv_libusb_endpoint_set_raw_io (libusb_device_handle *dev_handle, uint8_t endpoint, int enable)
{
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
        ArvUvLibusbFunctions functions = _get_libusb_functions ();

        if (functions.endpoint_set_raw_io == NULL)
                return LIBUSB_ERROR_NOT_SUPPORTED;

        return functions.endpoint_set_raw_io (dev_handle, endpoint, enable);
#else
        return libusb_endpoint_set_raw_io (dev_handle, endpoint, enable);
#endif
}

int
arv_uv_libusb_get_max_raw_io_transfer_size (libusb_device_handle *dev_handle, uint8_t endpoint)
{
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
        ArvUvLibusbFunctions functions = _get_libusb_functions ();

        if (functions.get_max_raw_io_transfer_size == NULL)
                return LIBUSB_ERROR_NOT_SUPPORTED;

        return functions.get_max_raw_io_transfer_size (dev_handle, endpoint);
#else
        return libusb_get_max_raw_io_transfer_size (dev_handle, endpoint);
#endif
}

int
arv_uv_libusb_get_device_string (libusb_device *dev, enum libusb_device_string_type string_type, char *data,
                                 int length)
{
#ifdef ARV_UV_LIBUSB_RUN_TIME_LOOKUP
        ArvUvLibusbFunctions functions = _get_libusb_functions ();

        if (functions.get_device_string == NULL)
                return LIBUSB_ERROR_NOT_SUPPORTED;

        return functions.get_device_string (dev, string_type, data, length);
#else
        return libusb_get_device_string (dev, string_type, data, length);
#endif
}

#endif

/* ArvDevice implementation */

/* ArvUvDevice implementation */

gboolean
arv_uv_device_is_connected (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

        return !g_atomic_int_get (&priv->disconnected);
}

/* The device is gone: emits control-lost once, whichever thread notices first (hotplug callback, failed synchronous
 * transfer, connection check) */

static void
_set_disconnected (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

        if (g_atomic_int_compare_and_exchange (&priv->disconnected, FALSE, TRUE))
                arv_device_emit_control_lost_signal (ARV_DEVICE (uv_device));
}

/*
 * arv_uv_device_check_connection:
 * @uv_device: a #ArvUvDevice
 *
 * Checks that the device is still there, with a standard request every USB device answers: a read of its device
 * descriptor on the default control pipe, with a short timeout. Removal is not always reported as
 * LIBUSB_ERROR_NO_DEVICE: on Windows a failed submission is LIBUSB_ERROR_IO, a transfer in flight may complete with
 * LIBUSB_TRANSFER_ERROR or LIBUSB_TRANSFER_STALL, and a default libusb build has no hotplug support. On Windows, any
 * result but a success, a timeout (busy device), LIBUSB_ERROR_BUSY (a call from a libusb callback) or
 * LIBUSB_ERROR_NO_MEM (no memory for the request) marks the device as disconnected, and emits #ArvDevice::control-lost
 * once. Elsewhere libusb reports a removed device as LIBUSB_ERROR_NO_DEVICE, and only that counts: on Linux, a request
 * the kernel refuses for lack of usbfs memory (usbfs_memory_mb) fails with LIBUSB_ERROR_IO (linux_usbfs.c maps every
 * submission error of a control transfer to it, except ENODEV).
 *
 * This is synchronous I/O: never call it from a libusb callback. Aravis calls it from the stream thread, and from the
 * thread whose register access failed with a transfer error (_send_cmd_and_receive_ack ()), never from the event
 * thread of the device.
 *
 * Returns: %FALSE if the device is known to be disconnected.
 */

gboolean
arv_uv_device_check_connection (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;
        unsigned char descriptor[LIBUSB_DT_DEVICE_SIZE];
        int result;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        if (!arv_uv_device_is_connected (uv_device))
                return FALSE;

        /* libusb_get_descriptor (), with a shorter timeout */
        result = libusb_control_transfer (priv->usb_device, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
                                          (uint16_t) (LIBUSB_DT_DEVICE << 8), 0,
                                          descriptor, sizeof (descriptor),
                                          ARV_UV_DEVICE_CHECK_CONNECTION_TIMEOUT_MS);
        if (result >= 0) {
                arv_debug_device ("[UvDevice::check_connection] Device present");
                return TRUE;
        }

#ifdef G_OS_WIN32
        switch (result) {
                case LIBUSB_ERROR_TIMEOUT:
                case LIBUSB_ERROR_BUSY:
                case LIBUSB_ERROR_NO_MEM:
                        arv_info_device ("[UvDevice::check_connection] Inconclusive (%s)",
                                         libusb_error_name (result));
                        return TRUE;
                default:
                        break;
        }
#else
        if (result != LIBUSB_ERROR_NO_DEVICE) {
                arv_info_device ("[UvDevice::check_connection] Inconclusive (%s)", libusb_error_name (result));
                return TRUE;
        }
#endif

        arv_warning_device ("[UvDevice::check_connection] Device lost (%s)", libusb_error_name (result));

        _set_disconnected (uv_device);

        return FALSE;
}

/*
 * arv_uv_device_get_stream_packet_size:
 * @uv_device: a #ArvUvDevice
 *
 * Returns: the maximum packet size of the stream endpoint, as libusb_get_max_packet_size () gives it (1024 bytes at
 * SuperSpeed), or 0 if it is unknown.
 */

guint
arv_uv_device_get_stream_packet_size (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;
        int result;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), 0);

        priv = arv_uv_device_get_instance_private (uv_device);

        result = libusb_get_max_packet_size (libusb_get_device (priv->usb_device), priv->data_endpoint);
        if (result <= 0) {
                arv_info_device ("[UvDevice::get_stream_packet_size] Unknown packet size for endpoint 0x%02x (%s)",
                                 priv->data_endpoint, result < 0 ? libusb_error_name (result) : "0");
                return 0;
        }

        return result;
}

/*
 * arv_uv_device_get_raw_io_maximum_transfer_size:
 * @uv_device: a #ArvUvDevice
 *
 * WinUSB's RAW_IO policy (libusb 1.0.30 and later, on Windows) passes the reads of an endpoint straight to the USB
 * stack, instead of WinUSB queueing them one at a time. With it on, a read must be a whole number of packets, and not
 * larger than the size returned here: WinUSB rejects any other read.
 *
 * Returns: the largest read RAW_IO accepts on the stream endpoint, in bytes, or 0 if RAW_IO is not available there
 * (headers of libusb older than 1.0.30 at build time, or an older libusb-1.0.dll at run time, another backend or driver
 * than WinUSB, or turned off by arv_uv_device_disable_raw_io ()).
 */

guint
arv_uv_device_get_raw_io_maximum_transfer_size (ArvUvDevice *uv_device)
{
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
        ArvUvDevicePrivate *priv;
        int result;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), 0);

        priv = arv_uv_device_get_instance_private (uv_device);

        if (g_atomic_int_get (&priv->raw_io_disabled)) {
                arv_info_device ("[UvDevice::raw_io] Off for this device since an earlier acquisition");
                return 0;
        }

        result = arv_uv_libusb_endpoint_supports_raw_io (priv->usb_device, priv->data_endpoint);
        if (result != 1) {
                arv_debug_device ("[UvDevice::raw_io] Not supported on endpoint 0x%02x (%s)", priv->data_endpoint,
                                  result < 0 ? libusb_error_name (result) : "not a WinUSB endpoint");
                return 0;
        }

        result = arv_uv_libusb_get_max_raw_io_transfer_size (priv->usb_device, priv->data_endpoint);
        if (result <= 0) {
                arv_info_device ("[UvDevice::raw_io] Failed to get the maximum transfer size of endpoint 0x%02x (%s)",
                                 priv->data_endpoint, result < 0 ? libusb_error_name (result) : "0");
                return 0;
        }

        return result;
#else
        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), 0);

        arv_debug_device ("[UvDevice::raw_io] Not available before libusb 1.0.30");

        return 0;
#endif
}

/*
 * arv_uv_device_set_raw_io:
 * @uv_device: a #ArvUvDevice
 * @enable: whether RAW_IO should be on
 *
 * Turns WinUSB's RAW_IO policy on or off for the stream endpoint. libusb says the policy must not change while
 * transfers are pending on the endpoint: the stream calls this before its thread starts, and after it has joined.
 *
 * Returns: %TRUE on success.
 */

gboolean
arv_uv_device_set_raw_io (ArvUvDevice *uv_device, gboolean enable)
{
#if defined (LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
        ArvUvDevicePrivate *priv;
        int result;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        result = arv_uv_libusb_endpoint_set_raw_io (priv->usb_device, priv->data_endpoint, enable ? 1 : 0);
        if (result != LIBUSB_SUCCESS) {
                arv_info_device ("[UvDevice::set_raw_io] Failed to turn RAW_IO %s on endpoint 0x%02x (%s)",
                                 enable ? "on" : "off", priv->data_endpoint, libusb_error_name (result));
                return FALSE;
        }

        g_atomic_int_set (&priv->raw_io, enable ? 1 : 0);

        arv_debug_device ("[UvDevice::set_raw_io] RAW_IO %s on endpoint 0x%02x", enable ? "on" : "off",
                          priv->data_endpoint);

        return TRUE;
#else
        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        return !enable;
#endif
}

/*
 * arv_uv_device_get_raw_io:
 * @uv_device: a #ArvUvDevice
 *
 * The RAW_IO policy of the stream endpoint, as the last successful arv_uv_device_set_raw_io () left it. WinUSB keeps
 * it as long as the device keeps its interface claimed, from one acquisition to the next, and resets it when the
 * interface is claimed again (WinUsb_Initialize ()). A change that fails leaves it as it was: a stop that fails to turn
 * RAW_IO off, the camera still there, leaves it on for the next acquisition, whose reads must then follow its rules, or
 * turn it off first.
 *
 * Returns: %TRUE if RAW_IO is on.
 */

gboolean
arv_uv_device_get_raw_io (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        return g_atomic_int_get (&priv->raw_io) != 0;
}

/*
 * arv_uv_device_disable_raw_io:
 * @uv_device: a #ArvUvDevice
 *
 * RAW_IO does not work with this device: WinUSB rejected a read while it was on, which points at a packet size or a
 * maximum transfer size that RAW_IO does not accept, or the camera ends its transfers of whole packets with a zero
 * length packet. arv_uv_device_get_raw_io_maximum_transfer_size () returns 0 from now on, so that the next
 * acquisitions go without RAW_IO.
 */

void
arv_uv_device_disable_raw_io (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;

        g_return_if_fail (ARV_IS_UV_DEVICE (uv_device));

        priv = arv_uv_device_get_instance_private (uv_device);

        g_atomic_int_set (&priv->raw_io_disabled, TRUE);
}

/*
 * arv_uv_device_clear_stream_halt:
 * @uv_device: a #ArvUvDevice
 *
 * Clears a halt of the stream endpoint and resets its pipe: libusb_clear_halt (), which is WinUsb_ResetPipe () on
 * Windows. Without RAW_IO, the AUTO_CLEAR_STALL policy libusb sets makes WinUSB do it by itself after a failed read.
 * With RAW_IO on, WinUSB bypasses its error handling: Microsoft documents AUTO_CLEAR_STALL as useful only when RAW_IO
 * is disabled, and every later read fails until the pipe is reset. Call it with no transfer pending on the endpoint.
 *
 * Returns: %TRUE on success.
 */

gboolean
arv_uv_device_clear_stream_halt (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;
        int result;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        result = libusb_clear_halt (priv->usb_device, priv->data_endpoint);
        if (result != LIBUSB_SUCCESS) {
                arv_warning_device ("[UvDevice::clear_stream_halt] Failed to reset the pipe of endpoint 0x%02x (%s)",
                                    priv->data_endpoint, libusb_error_name (result));
                return FALSE;
        }

        arv_info_device ("[UvDevice::clear_stream_halt] Pipe of endpoint 0x%02x reset", priv->data_endpoint);

        return TRUE;
}

/*
 * arv_uv_device_take_stream_interface:
 * @uv_device: a #ArvUvDevice
 *
 * Each acquisition start sets the stream interface of the device up for its stream: the SIRM registers, and the RAW_IO
 * policy of the stream endpoint. Two streams of one device cannot acquire together, as they would read the same
 * endpoint, but a stream may stop after another one has started, as when an application keeps a stream while it
 * creates a new one. Only the stream that set the interface up last may undo that setup: another one would disable the
 * streaming of the newer stream, and change the RAW_IO policy under its pending transfers. So a stream calls this once
 * its start has set the interface up, and arv_uv_device_release_stream_interface () when it stops.
 *
 * Returns: a token for arv_uv_device_release_stream_interface (), never 0.
 */

guint
arv_uv_device_take_stream_interface (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;
        guint owner;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), 0);

        priv = arv_uv_device_get_instance_private (uv_device);

        do {
                owner = (guint) g_atomic_int_add (&priv->n_stream_interface_setups, 1) + 1;
        } while (owner == 0);

        g_atomic_int_set (&priv->stream_interface_owner, (gint) owner);

        return owner;
}

/*
 * arv_uv_device_release_stream_interface:
 * @uv_device: a #ArvUvDevice
 * @owner: the token arv_uv_device_take_stream_interface () returned
 *
 * Returns: %TRUE if the stream interface is still set up as @owner left it: the caller may then undo that setup, and
 * no stream owns it afterwards. %FALSE if a stream has set it up since.
 */

gboolean
arv_uv_device_release_stream_interface (ArvUvDevice *uv_device, guint owner)
{
        ArvUvDevicePrivate *priv;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        return owner != 0 && g_atomic_int_compare_and_exchange (&priv->stream_interface_owner, (gint) owner, 0);
}

/*
 * arv_uv_device_has_hotplug:
 * @uv_device: a #ArvUvDevice
 *
 * Returns: %TRUE if libusb reports the removal of the device through the hotplug callback the device registered when
 * it was created. Not with a libusb build without hotplug support (the default one on Windows), nor when the
 * registration failed.
 */

gboolean
arv_uv_device_has_hotplug (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        return priv->hotplug_cb_registered;
}

/*
 * arv_uv_device_request_event_thread_priority:
 * @uv_device: a #ArvUvDevice
 *
 * Windows only. The transfer callbacks of a device run on its libusb event thread: in async mode they complete the
 * buffers, and in sync mode they wake the stream thread up. The application cannot reach that thread. It can only
 * raise the priority of the stream thread, from the stream callback at %ARV_STREAM_CALLBACK_TYPE_INIT, as
 * arv-camera-test --realtime does. The event thread keeps the priority it started with: g_thread_new () gives it the
 * one of the thread that opened the device, on Windows.
 *
 * So a stream thread calls this once the INIT callback has returned. The event thread then takes the priority of the
 * calling thread, read now, at the start of its next turn, if it is higher than its own: the request never lowers it,
 * as when the acquisition starts from a thread of lower priority than the one that opened the device. It keeps it until
 * arv_uv_device_release_event_thread_priority (), then goes back to the priority it started with.
 *
 * Each device has its own libusb context and event thread, so devices do not interfere. A device takes one request at
 * a time. Another stream acquiring from the same device at the same time would read the same data endpoint, which
 * does not work. Its thread does not change the priority.
 *
 * Returns: %TRUE if the request was taken; the caller then calls arv_uv_device_release_event_thread_priority ().
 * Always %FALSE except on Windows.
 */

gboolean
arv_uv_device_request_event_thread_priority (ArvUvDevice *uv_device)
{
#ifdef G_OS_WIN32
        ArvUvDevicePrivate *priv;
        int priority;

        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        priv = arv_uv_device_get_instance_private (uv_device);

        priority = GetThreadPriority (GetCurrentThread ());
        if (priority == THREAD_PRIORITY_ERROR_RETURN) {
                arv_warning_device ("[UvDevice::request_event_thread_priority] GetThreadPriority failed (error %lu)",
                                    GetLastError ());
                return FALSE;
        }

        if (!g_atomic_int_compare_and_exchange (&priv->event_thread_priority,
                                                ARV_UV_DEVICE_NO_THREAD_PRIORITY, priority)) {
                arv_info_device ("[UvDevice::request_event_thread_priority] Another stream thread has set the priority "
                                 "of the event thread already");
                return FALSE;
        }

        arv_debug_device ("[UvDevice::request_event_thread_priority] Event thread priority %d requested", priority);

        return TRUE;
#else
        g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);

        return FALSE;
#endif
}

/*
 * arv_uv_device_release_event_thread_priority:
 * @uv_device: a #ArvUvDevice
 *
 * Withdraws the request of arv_uv_device_request_event_thread_priority (), which returned %TRUE: the event thread
 * goes back to its own priority at the start of its next turn, within 100 ms.
 */

void
arv_uv_device_release_event_thread_priority (ArvUvDevice *uv_device)
{
        ArvUvDevicePrivate *priv;

        g_return_if_fail (ARV_IS_UV_DEVICE (uv_device));

        priv = arv_uv_device_get_instance_private (uv_device);

        g_atomic_int_set (&priv->event_thread_priority, ARV_UV_DEVICE_NO_THREAD_PRIORITY);
}

#ifdef G_OS_WIN32

/* Event thread only: applies the priority a stream thread requested, when it is higher than @own_priority, the one
 * the event thread started with, or @own_priority. @applied_request: the request applied last,
 * ARV_UV_DEVICE_NO_THREAD_PRIORITY at first. A failure is reported once per request, not at each turn. */

static void
_event_thread_update_priority (ArvUvDevicePrivate *priv, int own_priority, int *applied_request)
{
        int request = g_atomic_int_get (&priv->event_thread_priority);
        int priority;

        if (request == *applied_request)
                return;

        *applied_request = request;

        if (own_priority == THREAD_PRIORITY_ERROR_RETURN)
                own_priority = THREAD_PRIORITY_NORMAL;

        /* Never lower: the THREAD_PRIORITY_* values, those of the realtime class included, are in increasing order */
        if (request != ARV_UV_DEVICE_NO_THREAD_PRIORITY)
                priority = MAX (request, own_priority);
        else
                priority = own_priority;

        if (priority == GetThreadPriority (GetCurrentThread ()))
                return;

        if (SetThreadPriority (GetCurrentThread (), priority))
                arv_info_device ("[UvDevice::event_thread] Thread priority %d (%s)", priority,
                                 priority != own_priority ? "from the stream thread" : "its own");
        else
                arv_warning_device ("[UvDevice::event_thread] SetThreadPriority (%d) failed (error %lu)",
                                    priority, GetLastError ());
}

#endif

void
arv_uv_device_fill_bulk_transfer (struct libusb_transfer* transfer, ArvUvDevice *uv_device,
                                  ArvUvEndpointType endpoint_type, unsigned char endpoint_flags,
                                  void *data, size_t size,
                                  libusb_transfer_cb_fn callback, void* callback_data,
                                  unsigned int timeout)
{
        ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
        guint8 endpoint;

        if (endpoint_type == ARV_UV_ENDPOINT_CONTROL) {
                endpoint = (endpoint_flags == LIBUSB_ENDPOINT_IN) ? priv->control_endpoint_in : priv->control_endpoint_out;
        } else {
                endpoint = priv->data_endpoint;
        }

        libusb_fill_bulk_transfer (transfer, priv->usb_device, endpoint, data, size,
                                   callback, callback_data, timeout );
}

/* arv_uv_device_bulk_transfer (), which also gives the libusb result in @libusb_result: LIBUSB_SUCCESS, or a
 * LIBUSB_ERROR_* code (LIBUSB_ERROR_NO_DEVICE when the device is known to be gone already) */

static gboolean
_bulk_transfer (ArvUvDevice *uv_device, ArvUvEndpointType endpoint_type, unsigned char endpoint_flags, void *data,
		size_t size, size_t *transferred_size, guint32 timeout_ms, int *libusb_result, GError **error)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	gboolean success;
	guint8 endpoint;
	int transferred = 0;
	int result;

	g_return_val_if_fail (ARV_IS_UV_DEVICE (uv_device), FALSE);
	g_return_val_if_fail (data != NULL, FALSE);
	g_return_val_if_fail (size > 0, FALSE);

	if (!arv_uv_device_is_connected (uv_device)) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED,
			     "Not connected");
                if (libusb_result != NULL)
                        *libusb_result = LIBUSB_ERROR_NO_DEVICE;
		return FALSE;
	}

	if (endpoint_type == ARV_UV_ENDPOINT_CONTROL) {
		endpoint = (endpoint_flags == LIBUSB_ENDPOINT_IN) ? priv->control_endpoint_in : priv->control_endpoint_out;
	} else {
		endpoint = priv->data_endpoint;
	}
	result = libusb_bulk_transfer (priv->usb_device, endpoint, data, size, &transferred,
				       timeout_ms > 0 ? timeout_ms : priv->timeout_ms);

	success = result >= 0;

	if (!success)
		g_set_error (error, ARV_DEVICE_ERROR,
                             result == LIBUSB_ERROR_TIMEOUT ? ARV_DEVICE_ERROR_TIMEOUT : ARV_DEVICE_ERROR_TRANSFER_ERROR,
			     "%s", libusb_error_name (result));

	if (transferred_size != NULL)
		*transferred_size = transferred;
        if (libusb_result != NULL)
                *libusb_result = success ? LIBUSB_SUCCESS : result;

	if (result == LIBUSB_ERROR_NO_DEVICE)
                _set_disconnected (uv_device);

	return success;
}

gboolean
arv_uv_device_bulk_transfer (ArvUvDevice *uv_device, ArvUvEndpointType endpoint_type, unsigned char endpoint_flags, void *data,
			     size_t size, size_t *transferred_size, guint32 timeout_ms, GError **error)
{
        return _bulk_transfer (uv_device, endpoint_type, endpoint_flags, data, size, transferred_size, timeout_ms, NULL,
                               error);
}

void *
arv_uv_device_usb_mem_alloc (ArvUvDevice *uv_device, size_t size)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

        return libusb_dev_mem_alloc (priv->usb_device, size);
}

void
arv_uv_device_usb_mem_free (ArvUvDevice *uv_device, void *data, size_t size)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

        libusb_dev_mem_free (priv->usb_device, data, size);
}

static ArvStream *
arv_uv_device_create_stream (ArvDevice *device, ArvStreamCallback callback, void *user_data, GDestroyNotify destroy, GError **error)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (ARV_UV_DEVICE (device));
	return arv_uv_stream_new (ARV_UV_DEVICE (device), callback, user_data, destroy,
                                  priv->usb_mode, priv->maximum_transfer_size, error);
}

static gboolean
_send_cmd_and_receive_ack (ArvUvDevice *uv_device, ArvUvcpCommand command,
			   guint64 address, guint32 size, void *buffer, GError **error)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	ArvUvcpCommand expected_ack_command;
	ArvUvcpPacket *ack_packet;
	ArvUvcpPacket *packet;
	const char *operation;
	size_t packet_size;
	size_t ack_size;
	size_t ack_buffer_size;
	size_t minimum_ack_size;
	unsigned n_tries = 0;
	gboolean success = FALSE;
	gboolean check_connection = FALSE;
	ArvUvcpStatus status = ARV_UVCP_STATUS_SUCCESS;
	GError *transfer_error = NULL;
	int transfer_result = LIBUSB_SUCCESS;

	switch (command) {
		case ARV_UVCP_COMMAND_READ_MEMORY_CMD:
			operation = "read_memory";
			expected_ack_command = ARV_UVCP_COMMAND_READ_MEMORY_ACK;
			ack_size = arv_uvcp_packet_get_read_memory_ack_size (size);
			/* The data read */
			minimum_ack_size = ack_size;
			break;
		case ARV_UVCP_COMMAND_WRITE_MEMORY_CMD:
			operation = "write_memory";
			expected_ack_command = ARV_UVCP_COMMAND_WRITE_MEMORY_ACK;
			ack_size = arv_uvcp_packet_get_write_memory_ack_size ();
			/* Nothing beyond the header is used */
			minimum_ack_size = sizeof (ArvUvcpHeader);
			break;
		default:
			g_assert_not_reached ();
	}

	/* Room for a pending acknowledge, which the device may send instead of the acknowledge of a read of less than 4
	 * bytes. Into a smaller read, the end of it would be cut off (WinUSB partial reads), and would start the next
	 * read, or the read would overflow. */
	ack_buffer_size = MAX (ack_size, sizeof (ArvUvcpPendingAck));

	/* Before taking the transfer mutex: a control-lost handler may access the device from a thread that holds it */
	if (!arv_uv_device_is_connected (uv_device)) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED,
			     "USB3Vision %s error (device disconnected)", operation);
		return FALSE;
	}

	if (ack_size > priv->ack_packet_size_max) {
		arv_info_device ("Invalid uv %s acknowledge packet size (%" G_GSIZE_FORMAT " / max: %d)",
				  operation, ack_size, priv->ack_packet_size_max);
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
			     "USB3Vision %s acknowledge of %" G_GSIZE_FORMAT " bytes exceeds the device maximum (%u)",
			     operation, ack_size, priv->ack_packet_size_max);
		return FALSE;
	}

	switch (command) {
		case ARV_UVCP_COMMAND_READ_MEMORY_CMD:
			packet = arv_uvcp_packet_new_read_memory_cmd (address, size, 0, &packet_size);
			break;
		case ARV_UVCP_COMMAND_WRITE_MEMORY_CMD:
			packet = arv_uvcp_packet_new_write_memory_cmd (address, size, 0, &packet_size);
			break;
		default:
			g_assert_not_reached ();
	}

	if (packet_size > priv->cmd_packet_size_max) {
		arv_info_device ("Invalid us %s command packet size (%" G_GSIZE_FORMAT " / max: %d)",
				  operation, packet_size, priv->cmd_packet_size_max);
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
			     "USB3Vision %s command of %" G_GSIZE_FORMAT " bytes exceeds the device maximum (%u)",
			     operation, packet_size, priv->cmd_packet_size_max);
		arv_uvcp_packet_free (packet);
		return FALSE;
	}


	switch (command) {
		case ARV_UVCP_COMMAND_READ_MEMORY_CMD:
			break;
		case ARV_UVCP_COMMAND_WRITE_MEMORY_CMD:
			memcpy (arv_uvcp_packet_get_write_memory_cmd_data (packet), buffer, size);
			break;
		default:
			g_assert_not_reached ();
	}

	ack_packet = g_malloc (ack_buffer_size);

	g_mutex_lock (&priv->transfer_mutex);

	do {
		GError *local_error = NULL;
		size_t transferred;
		int result = LIBUSB_SUCCESS;

		priv->packet_id = arv_uvcp_next_packet_id (priv->packet_id);
		arv_uvcp_packet_set_packet_id (packet, priv->packet_id);

		arv_uvcp_packet_debug (packet, ARV_DEBUG_LEVEL_DEBUG);

		success = TRUE;
		success = success && _bulk_transfer (uv_device,
						     ARV_UV_ENDPOINT_CONTROL,
						     LIBUSB_ENDPOINT_OUT,
						     packet, packet_size,
						     NULL, 0, &result, &local_error);
		if (success) {
			gint timeout_ms;
                        gint64 timeout_stop_ms;

			gboolean pending_ack;
			gboolean expected_answer;

			timeout_stop_ms = g_get_monotonic_time () / 1000 + priv->timeout_ms;

			do {
				pending_ack = FALSE;

				timeout_ms = timeout_stop_ms - g_get_monotonic_time () / 1000;
                                if (timeout_ms < 0)
                                        timeout_ms = 0;

				success = TRUE;
				success = success && _bulk_transfer (uv_device,
								     ARV_UV_ENDPOINT_CONTROL,
								     LIBUSB_ENDPOINT_IN,
								     ack_packet, ack_buffer_size,
								     &transferred, timeout_ms,
								     &result, &local_error);

				if (success && transferred < sizeof (ArvUvcpHeader)) {
					/* No acknowledge: a zero length packet, or the piece of a packet. Nothing to
					 * parse, the buffer holds an earlier answer, or nothing yet. */
					expected_answer = FALSE;
					arv_info_device ("[UvDevice::%s] Try %d/%d: %" G_GSIZE_FORMAT
							 " byte(s) received, no acknowledge", operation, n_tries + 1,
							 ARV_UV_DEVICE_N_TRIES_MAX, transferred);
				} else if (success) {
					ArvUvcpCommand ack_command;
					guint16 packet_id;

					/* Not further than the data received */
					if (sizeof (ArvUvcpHeader) + GUINT16_FROM_LE (ack_packet->header.size) <=
					    transferred)
						arv_uvcp_packet_debug (ack_packet, ARV_DEBUG_LEVEL_DEBUG);

					status = arv_uvcp_packet_get_status (ack_packet);
					ack_command = arv_uvcp_packet_get_command (ack_packet);
					packet_id = arv_uvcp_packet_get_packet_id (ack_packet);

					if (ack_command == ARV_UVCP_COMMAND_PENDING_ACK &&
					    transferred >= sizeof (ArvUvcpPendingAck)) {
						gint64 pending_ack_timeout_ms;
						pending_ack = TRUE;
						expected_answer = FALSE;

						pending_ack_timeout_ms = arv_uvcp_packet_get_pending_ack_timeout (ack_packet);

						timeout_stop_ms = g_get_monotonic_time () / 1000 + pending_ack_timeout_ms;

						arv_debug_device ("[UvDevice::%s] Try %d/%d: "
                                                                  "pending ack timeout = %" G_GINT64_FORMAT,
                                                                  operation, n_tries + 1, ARV_UV_DEVICE_N_TRIES_MAX,
                                                                  pending_ack_timeout_ms);
					} else if (status != ARV_UVCP_STATUS_SUCCESS) {
						expected_answer = ack_command == expected_ack_command &&
							packet_id == priv->packet_id;
						if (!expected_answer) {
							arv_info_device ("[[UvDevice::%s] Try %d/%d: "
                                                                         "unexpected answer (0x%04x)",
                                                                         operation, n_tries + 1,
                                                                         ARV_UV_DEVICE_N_TRIES_MAX,
									 status);
						}
					} else {
						expected_answer = status == ARV_UVCP_STATUS_SUCCESS &&
							ack_command == expected_ack_command &&
							packet_id == priv->packet_id;
						if (expected_answer && transferred < minimum_ack_size) {
							/* Cut short: the data read is not all there */
							expected_answer = FALSE;
							arv_info_device ("[UvDevice::%s] Try %d/%d: acknowledge of %"
									 G_GSIZE_FORMAT " bytes, %" G_GSIZE_FORMAT
									 " expected", operation, n_tries + 1,
									 ARV_UV_DEVICE_N_TRIES_MAX, transferred,
									 minimum_ack_size);
						} else if (!expected_answer)
							arv_info_device ("[[UvDevice::%s] Try %d/%d: "
                                                                         "unexpected answer (0x%04x)",
                                                                         operation, n_tries + 1,
                                                                         ARV_UV_DEVICE_N_TRIES_MAX,
									 status);
					}
				} else {
					expected_answer = FALSE;
					if (local_error != NULL)
                                                arv_warning_device ("[UvDevice::%s] Try %d/%d: ack reception error: %s",
                                                                    operation, n_tries + 1, ARV_UV_DEVICE_N_TRIES_MAX,
                                                                    local_error->message);
                                        g_clear_error (&transfer_error);
                                        transfer_error = g_steal_pointer (&local_error);
                                        transfer_result = result;
                                }

			} while (arv_uv_device_is_connected (uv_device) &&
                                 (pending_ack || (!expected_answer && timeout_ms)));

			success = success && expected_answer;

			if (success && status == ARV_UVCP_STATUS_SUCCESS) {
				switch (command) {
					case ARV_UVCP_COMMAND_READ_MEMORY_CMD:
						memcpy (buffer, arv_uvcp_packet_get_read_memory_ack_data (ack_packet), size);
						break;
					case ARV_UVCP_COMMAND_WRITE_MEMORY_CMD:
						break;
					default:
						g_assert_not_reached();
				}
			}
		} else {
			if (local_error != NULL)
				arv_warning_device ("[UvDevice::%s] Try %d/%d: command sending error: %s",
                                                    operation, n_tries + 1, ARV_UV_DEVICE_N_TRIES_MAX,
                                                    local_error->message);
                        g_clear_error (&transfer_error);
                        transfer_error = g_steal_pointer (&local_error);
                        transfer_result = result;
                }

		n_tries++;
	} while (!success && n_tries < ARV_UV_DEVICE_N_TRIES_MAX && arv_uv_device_is_connected (uv_device));

        /* The tries failed, the last one with a transfer error. A removed device is not always reported as
         * LIBUSB_ERROR_NO_DEVICE (LIBUSB_ERROR_IO on Windows), and without hotplug nothing else may tell, as when it
         * does not stream: its connection is checked, at most every ARV_UV_DEVICE_CHECK_CONNECTION_INTERVAL_MS. Not
         * after a timeout, nor after LIBUSB_ERROR_BUSY, libusb's answer to synchronous I/O from a callback of its event
         * handling. Not on the event thread of the device either, which runs those callbacks. */
        if (!success && status == ARV_UVCP_STATUS_SUCCESS && transfer_error != NULL &&
            transfer_result != LIBUSB_ERROR_TIMEOUT && transfer_result != LIBUSB_ERROR_BUSY &&
            g_thread_self () != priv->event_thread && arv_uv_device_is_connected (uv_device)) {
                gint64 now = g_get_monotonic_time ();

                if (priv->last_control_check_time == 0 ||
                    now - priv->last_control_check_time >=
                    ARV_UV_DEVICE_CHECK_CONNECTION_INTERVAL_MS * G_TIME_SPAN_MILLISECOND) {
                        priv->last_control_check_time = now;
                        check_connection = TRUE;
                }
        }

	g_mutex_unlock (&priv->transfer_mutex);

	g_free (ack_packet);
	arv_uvcp_packet_free (packet);

        /* Without the transfer mutex: a lost device emits control-lost, whose handlers may access the device */
        if (check_connection)
                arv_uv_device_check_connection (uv_device);

	success = success && status == ARV_UVCP_STATUS_SUCCESS;

	/* Not a timeout when the device is gone, or when the transfers failed */
	if (!success) {
                if (status != ARV_UVCP_STATUS_SUCCESS)
                        g_set_error (error, ARV_DEVICE_ERROR, arv_uvcp_status_to_device_error (status),
                                     "USB3Vision %s error (%s)", operation,
                                     arv_uvcp_status_to_string (status));
                else if (!arv_uv_device_is_connected (uv_device))
                        g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_CONNECTED,
                                     "USB3Vision %s error (device disconnected)", operation);
                else if (transfer_error != NULL &&
                         !g_error_matches (transfer_error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT))
                        g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TRANSFER_ERROR,
                                     "USB3Vision %s error (%s)", operation, transfer_error->message);
                else
                        g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_TIMEOUT,
                                     "USB3Vision %s timeout", operation);
        }

        g_clear_error (&transfer_error);

	return success;
}

/* Largest block of data one read or write memory command can carry. Read data comes back in the acknowledge, after
 * its header; write data goes in the command, after its header and address. The 16 bit size fields of the packets
 * set a further limit. Blocks are whole 32 bit words, so that each one starts as aligned as the first one. */

static gboolean
_get_data_size_max (ArvUvDevicePrivate *priv, ArvUvcpCommand command, guint *data_size_max, GError **error)
{
	const char *operation;
	size_t packet_size_max;
	size_t overhead;
	size_t size_field_max;
	size_t room;

	switch (command) {
		case ARV_UVCP_COMMAND_READ_MEMORY_CMD:
			operation = "read_memory";
			packet_size_max = priv->ack_packet_size_max;
			overhead = arv_uvcp_packet_get_read_memory_ack_size (0);
			size_field_max = G_MAXUINT16;
			break;
		case ARV_UVCP_COMMAND_WRITE_MEMORY_CMD:
			operation = "write_memory";
			packet_size_max = priv->cmd_packet_size_max;
			overhead = arv_uvcp_packet_get_write_memory_cmd_size (0);
			size_field_max = G_MAXUINT16 - sizeof (ArvUvcpWriteMemoryCmdInfos);
			break;
		default:
			g_assert_not_reached ();
	}

	room = packet_size_max > overhead ? packet_size_max - overhead : 0;
	*data_size_max = (guint) (MIN (room, size_field_max) & ~((size_t) 3));

	if (*data_size_max == 0) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
			     "USB3Vision %s impossible: the device maximum %s size (%" G_GSIZE_FORMAT
			     " bytes) leaves no room for 4 bytes of data", operation,
			     command == ARV_UVCP_COMMAND_READ_MEMORY_CMD ? "acknowledge" : "command",
			     packet_size_max);
		return FALSE;
	}

	return TRUE;
}

static gboolean
arv_uv_device_read_memory (ArvDevice *device, guint64 address, guint32 size, void *buffer, GError **error)
{
	ArvUvDevice *uv_device = ARV_UV_DEVICE (device);
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	int i;
	gint32 block_size;
	guint data_size_max;

	if (!_get_data_size_max (priv, ARV_UVCP_COMMAND_READ_MEMORY_CMD, &data_size_max, error))
		return FALSE;

	for (i = 0; i < (size + data_size_max - 1) / data_size_max; i++) {
		block_size = MIN (data_size_max, size - i * data_size_max);
		if (!_send_cmd_and_receive_ack (uv_device, ARV_UVCP_COMMAND_READ_MEMORY_CMD,
						address + i * data_size_max,
						block_size, ((char *) buffer) + i * data_size_max, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
arv_uv_device_write_memory (ArvDevice *device, guint64 address, guint32 size, const void *buffer, GError **error)
{
	ArvUvDevice *uv_device = ARV_UV_DEVICE (device);
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	int i;
	gint32 block_size;
	guint data_size_max;

	if (!_get_data_size_max (priv, ARV_UVCP_COMMAND_WRITE_MEMORY_CMD, &data_size_max, error))
		return FALSE;

	for (i = 0; i < (size + data_size_max - 1) / data_size_max; i++) {
		block_size = MIN (data_size_max, size - i * data_size_max);
		if (!_send_cmd_and_receive_ack (uv_device, ARV_UVCP_COMMAND_WRITE_MEMORY_CMD,
						address + i * data_size_max,
						block_size, ((char *) buffer) + i * data_size_max, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
arv_uv_device_read_register (ArvDevice *device, guint64 address, guint32 *value, GError **error)
{
	return arv_uv_device_read_memory (device, address, sizeof (guint32), value, error);
}

static gboolean
arv_uv_device_write_register (ArvDevice *device, guint64 address, guint32 value, GError **error)
{
	return arv_uv_device_write_memory (device, address, sizeof (guint32), &value, error);
}

static gboolean
_bootstrap (ArvUvDevice *uv_device)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	ArvDevice *device = ARV_DEVICE (uv_device);
	guint64 offset;
	guint32 response_time;
	guint64 manifest_table_address;
	guint64 device_capability;
	guint32 max_cmd_transfer;
	guint32 max_ack_transfer;
	guint64 u3vcp_capability;
	guint64 sirm_offset;
	guint32 si_info;
	guint32 si_control;
	guint64 si_req_payload_size;
	guint32 si_req_leader_size;
	guint32 si_req_trailer_size;
	guint32 si_max_leader_size;
	guint32 si_payload_size;
	guint32 si_payload_count;
	guint32 si_transfer1_size;
	guint32 si_transfer2_size;
	guint32 si_max_trailer_size;
	guint64 manifest_n_entries;
	ArvUvcpManifestEntry entry;
	ArvUvcpManifestSchemaType schema_type;
	GString *string;
	void *data;
	char manufacturer[64];
	gboolean success = TRUE;
        char *genicam_url = NULL;

	arv_info_device ("Get genicam");

	success = success && arv_device_read_memory(device, ARV_ABRM_MANUFACTURER_NAME, 64, &manufacturer, NULL);
	if (!success) {
		arv_warning_device ("[UvDevice::_bootstrap] Error during memory read");
		return FALSE;
	}
	manufacturer[63] = 0;
	arv_info_device ("MANUFACTURER_NAME =        '%s'", manufacturer);

	success = success && arv_device_read_memory (device, ARV_ABRM_SBRM_ADDRESS,
                                                     sizeof (offset), &offset, NULL);
	success = success && arv_device_read_memory (device, ARV_ABRM_MAX_DEVICE_RESPONSE_TIME,
                                                     sizeof (response_time), &response_time, NULL);
	success = success && arv_device_read_memory (device, ARV_ABRM_DEVICE_CAPABILITY,
                                                     sizeof (device_capability), &device_capability, NULL);
	success = success && arv_device_read_memory (device, ARV_ABRM_MANIFEST_TABLE_ADDRESS,
                                                     sizeof (manifest_table_address), &manifest_table_address, NULL);
	if (!success) {
		arv_warning_device ("[UvDevice::_bootstrap] Error during memory read");
		return FALSE;
	}

	arv_info_device ("MAX_DEVICE_RESPONSE_TIME = 0x%08x", response_time);
	arv_info_device ("DEVICE_CAPABILITY        = 0x%016" G_GINT64_MODIFIER "x", device_capability);
	arv_info_device ("SRBM_ADDRESS =             0x%016" G_GINT64_MODIFIER "x", offset);
	arv_info_device ("MANIFEST_TABLE_ADDRESS =   0x%016" G_GINT64_MODIFIER "x", manifest_table_address);

	priv->timeout_ms = MAX (ARV_UVCP_DEFAULT_RESPONSE_TIME_MS, response_time);

	success = success && arv_device_read_memory (device, offset + ARV_SBRM_U3VCP_CAPABILITY,
                                                     sizeof (u3vcp_capability), &u3vcp_capability, NULL);
	success = success && arv_device_read_memory (device, offset + ARV_SBRM_MAX_CMD_TRANSFER,
                                                     sizeof (max_cmd_transfer), &max_cmd_transfer, NULL);
	success = success && arv_device_read_memory (device, offset + ARV_SBRM_MAX_ACK_TRANSFER,
                                                     sizeof (max_ack_transfer), &max_ack_transfer, NULL);
	success = success && arv_device_read_memory (device, offset + ARV_SBRM_SIRM_ADDRESS,
                                                     sizeof (sirm_offset), &sirm_offset, NULL);
	if (!success) {
		arv_warning_device ("[UvDevice::_bootstrap] Error during memory read");
		return FALSE;
	}

	arv_info_device ("U3VCP_CAPABILITY =         0x%016" G_GINT64_MODIFIER "x", u3vcp_capability);
	arv_info_device ("MAX_CMD_TRANSFER =         0x%08x", max_cmd_transfer);
	arv_info_device ("MAX_ACK_TRANSFER =         0x%08x", max_ack_transfer);
	arv_info_device ("SIRM_OFFSET =              0x%016" G_GINT64_MODIFIER "x", sirm_offset);

	priv->cmd_packet_size_max = MIN (priv->cmd_packet_size_max, max_cmd_transfer);
	priv->ack_packet_size_max = MIN (priv->ack_packet_size_max, max_ack_transfer);

	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_INFO,
                                                     sizeof (si_info), &si_info, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_CONTROL,
                                                     sizeof (si_control), &si_control, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_REQ_PAYLOAD_SIZE,
                                                     sizeof (si_req_payload_size), &si_req_payload_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_REQ_LEADER_SIZE,
                                                     sizeof (si_req_leader_size), &si_req_leader_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_REQ_TRAILER_SIZE,
                                                     sizeof (si_req_trailer_size), &si_req_trailer_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_MAX_LEADER_SIZE,
                                                     sizeof (si_max_leader_size), &si_max_leader_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_PAYLOAD_SIZE,
                                                     sizeof (si_payload_size), &si_payload_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_PAYLOAD_COUNT,
                                                     sizeof (si_payload_count), &si_payload_count, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_TRANSFER1_SIZE,
                                                     sizeof (si_transfer1_size), &si_transfer1_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_TRANSFER2_SIZE,
                                                     sizeof (si_transfer2_size), &si_transfer2_size, NULL);
	success = success && arv_device_read_memory (device, sirm_offset + ARV_SIRM_MAX_TRAILER_SIZE,
                                                     sizeof (si_max_trailer_size), &si_max_trailer_size, NULL);
	if (!success) {
		arv_warning_device ("[UvDevice::_bootstrap] Error during memory read");
		return FALSE;
	}

	arv_info_device ("SIRM_INFO =                0x%08x", si_info);
	arv_info_device ("SIRM_CONTROL =             0x%08x", si_control);
	arv_info_device ("SIRM_REQ_PAYLOAD_SIZE =    0x%016" G_GINT64_MODIFIER "x", si_req_payload_size);
	arv_info_device ("SIRM_REQ_LEADER_SIZE =     0x%08x", si_req_leader_size);
	arv_info_device ("SIRM_REQ_TRAILER_SIZE =    0x%08x", si_req_trailer_size);
	arv_info_device ("SIRM_MAX_LEADER_SIZE =     0x%08x", si_max_leader_size);
	arv_info_device ("SIRM_PAYLOAD_SIZE =        0x%08x", si_payload_size);
	arv_info_device ("SIRM_PAYLOAD_COUNT =       0x%08x", si_payload_count);
	arv_info_device ("SIRM_TRANSFER1_SIZE =      0x%08x", si_transfer1_size);
	arv_info_device ("SIRM_TRANSFER2_SIZE =      0x%08x", si_transfer2_size);
	arv_info_device ("SIRM_MAX_TRAILER_SIZE =    0x%08x", si_max_trailer_size);

	success = success && arv_device_read_memory (device, manifest_table_address,
                                                     sizeof (manifest_n_entries), &manifest_n_entries, NULL);
	success = success && arv_device_read_memory (device, manifest_table_address + 0x08,
                                                     sizeof (entry), &entry, NULL);
	if (!success) {
		arv_warning_device ("[UvDevice::_bootstrap] Error during memory read");
		return FALSE;
	}

	arv_info_device ("MANIFEST_N_ENTRIES =       0x%016" G_GINT64_MODIFIER "x", manifest_n_entries);

	string = g_string_new ("");
	arv_g_string_append_hex_dump (string, &entry, sizeof (entry));
	arv_info_device ("MANIFEST ENTRY\n%s", string->str);
	g_string_free (string, TRUE);

	arv_info_device ("genicam address =          0x%016" G_GINT64_MODIFIER "x", entry.address);
	arv_info_device ("genicam size    =          0x%016" G_GINT64_MODIFIER "x", entry.size);

	data = g_malloc0 (entry.size);
	success = success && arv_device_read_memory (device, entry.address, entry.size, data, NULL);
	if (!success){
		arv_warning_device ("[UvDevice::_bootstrap] Error during memory read");
		g_free(data);
		return FALSE;
	}

#if 0
	string = g_string_new ("");
	arv_g_string_append_hex_dump (string, data, entry.size);
	arv_info_device ("GENICAM\n%s", string->str);
	g_string_free (string, TRUE);
#endif

	schema_type = arv_uvcp_manifest_entry_get_schema_type (&entry);

	switch (schema_type) {
		case ARV_UVCP_SCHEMA_ZIP:
			{
				ArvZip *zip;
				const GSList *zip_files;

				zip = arv_zip_new (data, entry.size);
				zip_files = arv_zip_get_file_list (zip);

				if (zip_files != NULL) {
					const char *zip_filename;

					zip_filename = arv_zip_file_get_name (zip_files->data);
                                        priv->genicam_xml = arv_zip_get_file (zip,
                                                                              zip_filename,
                                                                              &priv->genicam_xml_size);

					arv_info_device ("zip file =                 %s", zip_filename);

#if 0
					string = g_string_new ("");
					arv_g_string_append_hex_dump (string, priv->genicam_xml,
								      priv->genicam_xml_size);
					arv_info_device ("GENICAM\n%s", string->str);
					g_string_free (string, TRUE);
#endif

					priv->genicam = arv_gc_new (ARV_DEVICE (uv_device),
								    priv->genicam_xml,
								    priv->genicam_xml_size);

                                        genicam_url = g_strdup_printf("local:///DeviceU3V.zip;%"
                                                                      G_GINT64_MODIFIER "x;%" G_GINT64_MODIFIER "x",
                                                                      entry.address, entry.size);
                                        arv_dom_document_set_url(ARV_DOM_DOCUMENT(priv->genicam), genicam_url);
                                        g_free (genicam_url);
				}

				arv_zip_free (zip);
				g_free (data);
			}
			break;
		case ARV_UVCP_SCHEMA_RAW:
			{
				priv->genicam_xml = data;
				priv->genicam_xml_size = entry.size;
				priv->genicam = arv_gc_new (ARV_DEVICE (uv_device),
							    priv->genicam_xml,
							    priv->genicam_xml_size);
                                genicam_url =
                                        g_strdup_printf("local:///DeviceU3V.xml;%" G_GINT64_MODIFIER "x;%"
                                                        G_GINT64_MODIFIER "x",
                                                        entry.address, entry.size);
                                arv_dom_document_set_url(ARV_DOM_DOCUMENT(priv->genicam), genicam_url);
                                g_free (genicam_url);
                        }
                        break;
                default:
                        arv_warning_device ("Unknown USB3Vision manifest schema type (%d)", schema_type);
        }

#if 0
	arv_info_device("GENICAM\n:%s", priv->genicam_xml);
#endif

	return TRUE;
}

static ArvGc *
arv_uv_device_get_genicam (ArvDevice *device)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (ARV_UV_DEVICE (device));

	return priv->genicam;
}

static const char *
arv_uv_device_get_genicam_xml (ArvDevice *device, size_t *size)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (ARV_UV_DEVICE (device));

	if (size != NULL)
		*size = priv->genicam_xml_size;

	return priv->genicam_xml;
}

static void
reset_endpoint (libusb_device_handle *usb_device, guint8 endpoint)
{
	int errcode;

	/* Set endpoint in halt condition. The request type combines three enumerations: as integers. */
	errcode = libusb_control_transfer(usb_device,
					      (uint8_t) ((int) LIBUSB_ENDPOINT_OUT |
							 (int) LIBUSB_REQUEST_TYPE_STANDARD |
							 (int) LIBUSB_RECIPIENT_ENDPOINT),
					      LIBUSB_REQUEST_SET_FEATURE,
					      0, /* Value: 0=endpoint_halt */
					      endpoint,
					      0, 0,
					      1000);
	if (errcode < 0) {
		arv_warning_device("Failed to set endpoint %x in halt condition: %s",
				   endpoint, libusb_error_name (errcode));
		return;
	}

	/* Clear halt condtion on the endpoint, effectivelly resetting the pipe */
	errcode = libusb_clear_halt(usb_device, endpoint);
	if (errcode < 0) {
		arv_warning_device("Failed to clear halt contidion on endpoint: %s",
				   libusb_error_name (errcode));
		return;
	}
}

gboolean
arv_uv_device_reset_stream_endpoint (ArvUvDevice *device)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (ARV_UV_DEVICE (device));

        g_return_val_if_fail(ARV_IS_UV_DEVICE(device), FALSE);

        reset_endpoint (priv->usb_device, priv->data_endpoint);

        return TRUE;
}

/* Finds the control and data interfaces of the device and their endpoints */

static gboolean
_find_endpoints (ArvUvDevicePrivate *priv, libusb_device *device, GError **error)
{
	struct libusb_config_descriptor *config;
	struct libusb_endpoint_descriptor endpoint;
	const struct libusb_interface *inter;
	const struct libusb_interface_descriptor *interdesc;
	gboolean data_endpoint_missing = FALSE;
	int result;
	int i, j, k;

	result = libusb_get_config_descriptor (device, 0, &config);
	if (result != LIBUSB_SUCCESS) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
			     "Failed to get configuration descriptor of USB device '%s-%s-%s-%s': %s",
			     priv->vendor, priv->product, priv->serial_number, priv->guid,
			     libusb_error_name (result));
		return FALSE;
	}

	for (i = 0; i < (int) config->bNumInterfaces; i++) {
		inter = &config->interface[i];
		for (j = 0; j < inter->num_altsetting; j++) {
			interdesc = &inter->altsetting[j];
			if (interdesc->bInterfaceClass == ARV_UV_INTERFACE_INTERFACE_CLASS &&
			    interdesc->bInterfaceSubClass == ARV_UV_INTERFACE_INTERFACE_SUBCLASS) {
				if (interdesc->bInterfaceProtocol == ARV_UV_INTERFACE_CONTROL_PROTOCOL) {
					for (k = 0; k < interdesc->bNumEndpoints; k++) {
						endpoint = interdesc->endpoint[k];
						if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0) {
							priv->control_endpoint_in = endpoint.bEndpointAddress;
						} else {
							priv->control_endpoint_out = endpoint.bEndpointAddress;
						}
					}
					priv->control_interface = interdesc->bInterfaceNumber;
				}
				if (interdesc->bInterfaceProtocol == ARV_UV_INTERFACE_DATA_PROTOCOL) {
					if (interdesc->bNumEndpoints > 0) {
						endpoint = interdesc->endpoint[0];
						priv->data_endpoint = endpoint.bEndpointAddress;
						priv->data_interface = interdesc->bInterfaceNumber;
					} else
						data_endpoint_missing = TRUE;
				}
			}
		}
	}
	libusb_free_config_descriptor (config);

	if (data_endpoint_missing && priv->data_endpoint == 0) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
			     "USB3Vision data interface of USB device '%s-%s-%s-%s' has no endpoint",
			     priv->vendor, priv->product, priv->serial_number, priv->guid);
		return FALSE;
	}

	return TRUE;
}

typedef struct {
	libusb_device *device;
	int error;
} ArvUvDeviceOpenError;

/* Opens @device if it is the USB3 Vision device that @uv_device looks for, which its GUID, or its manufacturer, product
 * and serial number strings tell. Returns TRUE if it did; on a failure to open it, adds the error to @open_errors. */

static gboolean
_try_usb_device (ArvUvDevice *uv_device, libusb_device *device, GArray *open_errors, GError **error)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	libusb_device_handle *usb_device;
	struct libusb_device_descriptor desc;
	ArvUvInterfaceStringIndices string_indices;
	unsigned char *manufacturer;
	unsigned char *product;
	unsigned char *serial_number;
	unsigned char *guid;
	int index;
	int result;

	/* Only the devices discovery lists: no open, and no request, to any other device */
	if (!arv_uv_interface_match_descriptors (device, &desc, &string_indices))
		return FALSE;

	result = libusb_open (device, &usb_device);
	if (result != LIBUSB_SUCCESS) {
		ArvUvDeviceOpenError open_error = { device, result };

		arv_debug_device ("[UvDevice::_open_usb_device] Failed to open USB3 Vision device "
				  "%04x:%04x: %s", desc.idVendor, desc.idProduct, libusb_error_name (result));
		g_array_append_val (open_errors, open_error);

		return FALSE;
	}

	manufacturer = g_malloc0 (256);
	product = g_malloc0 (256);
	serial_number = g_malloc0 (256);
	guid = g_malloc0 (256);

	index = desc.iManufacturer;
	if (index > 0)
		libusb_get_string_descriptor_ascii (usb_device, index, manufacturer, 256);
	index = desc.iProduct;
	if (index > 0)
		libusb_get_string_descriptor_ascii (usb_device, index, product, 256);
	index = desc.iSerialNumber;
	if (index > 0)
		libusb_get_string_descriptor_ascii (usb_device, index, serial_number, 256);
	index = string_indices.guid;
	if (index > 0)
		libusb_get_string_descriptor_ascii (usb_device, index, guid, 256);

	if ((priv->vendor != NULL &&
	     g_strcmp0 ((char * ) manufacturer, priv->vendor) == 0 &&
	     priv->product != NULL &&
	     g_strcmp0 ((char * ) product, priv->product) == 0 &&
	     priv->serial_number != NULL &&
	     g_strcmp0 ((char * ) serial_number, priv->serial_number) == 0) ||
	    (priv->guid != NULL &&
	     g_strcmp0 ((char * ) guid, priv->guid) == 0)) {
		priv->usb_device = usb_device;

		if (priv->vendor == NULL)
			priv->vendor = g_strdup ((char *) manufacturer);
		if (priv->product == NULL)
			priv->product = g_strdup ((char *) product);
		if (priv->serial_number == NULL)
			priv->serial_number = g_strdup ((char *) serial_number);
		if (priv->guid == NULL)
			priv->guid = g_strdup ((char *) guid);

		priv->usb_major_version = ((desc.bcdUSB >> 12) & 0xf) * 10 + ((desc.bcdUSB >> 8) & 0xf);
		priv->usb_minor_version = ((desc.bcdUSB >> 4) & 0xf) * 10 + (desc.bcdUSB & 0xf);

		/* Without the capability (Windows, BSD), this can only fail with LIBUSB_ERROR_NOT_SUPPORTED */
		if (libusb_has_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER)) {
			result = libusb_set_auto_detach_kernel_driver (usb_device, 1);
			if (result != 0) {
				arv_warning_device ("Failed to set auto kernel detach feature "
						    "for USB device '%s-%s-%s': %s",
						    priv->vendor, priv->product, priv->serial_number,
						    libusb_error_name (result));
			}
		}

		if (!_find_endpoints (priv, device, error)) {
			priv->usb_device = NULL;
			libusb_close (usb_device);
		}
	} else
		libusb_close (usb_device);

	g_free (manufacturer);
	g_free (product);
	g_free (serial_number);
	g_free (guid);

	return priv->usb_device != NULL;
}

static gboolean
_open_usb_device (ArvUvDevice *uv_device, GError **error)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	ArvUvInterfaceDeviceLocation location;
	libusb_device **devices;
	libusb_device *opened = NULL;
	GArray *open_errors;
	GError *local_error = NULL;
	ssize_t located_index = -1;
	ssize_t count;
	ssize_t i;

	count = libusb_get_device_list (priv->usb, &devices);
	if (count < 0) {
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
			     "Failed to get USB device list: %s", libusb_error_name (count));
		return FALSE;
	}

	open_errors = g_array_new (FALSE, FALSE, sizeof (ArvUvDeviceOpenError));

	/* First the device where discovery, or an earlier open, found the camera with this GUID: when it is still there,
	 * no other device is opened, as some other USB3 Vision device may be in use, in this process or another one */
	if (arv_uv_interface_lookup_device_location (priv->guid, &location)) {
		for (i = 0; i < count && located_index < 0; i++)
			if (arv_uv_interface_is_device_at_location (devices[i], &location))
				located_index = i;

		if (located_index >= 0 &&
		    _try_usb_device (uv_device, devices[located_index], open_errors, &local_error))
			opened = devices[located_index];

		arv_debug_device ("[UvDevice::_open_usb_device] Device with GUID %s last found on bus %u, address %u: %s",
				  priv->guid, (guint) location.bus_number, (guint) location.device_address,
				  opened != NULL ? "opened there" :
				  located_index >= 0 ? "not opened there" : "no device there any more");
	}

	/* Then every USB3 Vision device, as when that location is unknown: no device there any more (a camera plugged in
	 * again gets another address), another one, or one that failed to open */
	for (i = 0; i < count && opened == NULL && local_error == NULL; i++)
		if (i != located_index && _try_usb_device (uv_device, devices[i], open_errors, &local_error))
			opened = devices[i];

	if (opened != NULL)
		arv_uv_interface_remember_device_location (priv->guid, opened);

	if (local_error == NULL && priv->usb_device == NULL) {
		GString *message = g_string_new (NULL);
		guint j;

		if (priv->vendor != NULL && priv->product != NULL && priv->serial_number != NULL)
			g_string_append_printf (message, "USB device '%s:%s:%s' not found",
						priv->vendor, priv->product, priv->serial_number);
		else
			g_string_append_printf (message, "USB device with GUID %s not found", priv->guid);

		/* The USB3 Vision devices libusb could not open, the one looked for may be among them. Without their
		 * device strings, which would be requests to these other devices on Windows. */
		for (j = 0; j < open_errors->len; j++) {
			ArvUvDeviceOpenError *open_error = &g_array_index (open_errors, ArvUvDeviceOpenError, j);
			char *description;

			description = arv_uv_interface_describe_open_error (open_error->device, open_error->error,
									    FALSE);
			g_string_append_printf (message, "\n%s", description);
			g_free (description);
		}

		local_error = g_error_new_literal (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND, message->str);
		g_string_free (message, TRUE);
	}

	g_array_free (open_errors, TRUE);
	libusb_free_device_list (devices, 1);

	if (local_error != NULL) {
		g_propagate_error (error, local_error);
		return FALSE;
	}

	return TRUE;
}

static gpointer
event_thread_func(void *p)
{
	ArvUvDevicePrivate *priv = (ArvUvDevicePrivate *)p;
#ifdef G_OS_WIN32
        /* What GLib gave it: g_thread_new () copies the priority of the creating thread on Windows */
        int own_priority = GetThreadPriority (GetCurrentThread ());
        int applied_request = ARV_UV_DEVICE_NO_THREAD_PRIORITY;
#endif

	struct timeval tv = { 0, 100000 };

        while (priv->event_thread_run)
        {
#ifdef G_OS_WIN32
                /* The priority of the stream thread, see arv_uv_device_request_event_thread_priority () */
                _event_thread_update_priority (priv, own_priority, &applied_request);
#endif
                libusb_handle_events_timeout(priv->usb, &tv);
        }

        return NULL;
}

/**
 * arv_uv_device_set_usb_mode:
 * @uv_device: a #ArvUvDevice
 * @usb_mode: a #ArvUvUsbMode option
 *
 * Sets the option to utilize the USB synchronous or asynchronous device I/O API. The default mode is
 * @ARV_UV_USB_MODE_DEFAULT, which is @ARV_UV_USB_MODE_ASYNC: several USB bulk transfers are kept in flight, which
 * gives the best bandwidth with high framerate devices. @ARV_UV_USB_MODE_SYNC executes one synchronous bulk transfer
 * at a time.
 *
 * Since: 0.8.17
 */

void
arv_uv_device_set_usb_mode (ArvUvDevice *uv_device, ArvUvUsbMode usb_mode)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

	g_return_if_fail (ARV_IS_UV_DEVICE (uv_device));

	priv->usb_mode = usb_mode;
}

/**
 * arv_uv_device_set_maximum_transfer_size:
 * @uv_device: a #ArvUvDevice
 * @size: maximum transfer size, in bytes, from 1 KiB (1024) to 64 MiB (67108864), or 0 for the default (1 MiB)
 *
 * Set the maximum transfer size for a USB bulk transfer, for the streams created afterwards. A size out of range is
 * refused with a warning, and the previous size is kept.
 *
 * At each acquisition start, the stream reads the payload in transfers of the largest size that is not larger than
 * this one and is a multiple of both the payload alignment the camera requires and the packet size of the stream
 * endpoint. When WinUSB's RAW_IO policy is on (Windows, libusb 1.0.30 or later), that size is also capped at the
 * largest read RAW_IO accepts. The info log of the stream shows the result.
 *
 * Since: 0.10.0
 */

void
arv_uv_device_set_maximum_transfer_size (ArvUvDevice *uv_device, guint64 size)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

	g_return_if_fail (ARV_IS_UV_DEVICE (uv_device));

        if (size == 0)
                size = ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_DEFAULT;

        if (size < ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MIN || size > ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MAX) {
                g_warning ("[UvDevice::set_maximum_transfer_size] Invalid USB maximum transfer size %" G_GUINT64_FORMAT
                           " (from %d to %d bytes, or 0 for the default), keeping %" G_GUINT64_FORMAT,
                           size, ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MIN, ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MAX,
                           priv->maximum_transfer_size);
                return;
        }

	priv->maximum_transfer_size = size;
}
/**
 * arv_uv_device_new:
 * @vendor: USB3 vendor string
 * @product: USB3 product string
 * @serial_number: device serial number
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Returns: a newly created #ArvDevice using USB3 based protocol
 *
 * Since: 0.8.0
 */

ArvDevice *
arv_uv_device_new (const char *vendor, const char *product, const char *serial_number, GError **error)
{
	return g_initable_new (ARV_TYPE_UV_DEVICE, NULL, error,
			       "vendor", vendor,
			       "product", product,
			       "serial-number", serial_number,
			       NULL);
}

/**
 * arv_uv_device_new_from_guid:
 * @guid: device GUID
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Returns: a newly created #ArvDevice using USB3 based protocol
 *
 * Since: 0.8.17
 */

ArvDevice *
arv_uv_device_new_from_guid (const char *guid, GError **error)
{
	return g_initable_new (ARV_TYPE_UV_DEVICE, NULL, error,
			       "guid", guid,
			       NULL);
}

static int LIBUSB_CALL _disconnect_event (libusb_context *ctx,
                              libusb_device *device,
                              libusb_hotplug_event event,
                              void *user_data)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (ARV_UV_DEVICE (user_data));

        if (device == libusb_get_device (priv->usb_device))
                _set_disconnected (ARV_UV_DEVICE (user_data));

        return 0;
}

static void
arv_uv_device_constructed (GObject *object)
{
	ArvUvDevice *uv_device = ARV_UV_DEVICE (object);
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);
	const struct libusb_version *version;
	GError *error = NULL;
        int result;

        G_OBJECT_CLASS (arv_uv_device_parent_class)->constructed (object);

        g_mutex_init (&priv->transfer_mutex);

	result = libusb_init (&priv->usb);
        if (result != 0) {
                arv_device_take_init_error (ARV_DEVICE (uv_device),
                                            g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
                                                         "Failed to initialize USB library: %s",
                                                         libusb_error_name (result)));
                return;
        }

        version = libusb_get_version ();
        arv_info_device ("[UvDevice::new] libusb %d.%d.%d.%d%s",
                         version->major, version->minor, version->micro, version->nano,
                         version->rc != NULL ? version->rc : "");

        if (priv->vendor != NULL)
                arv_info_device ("[UvDevice::new] Vendor  = %s", priv->vendor);
        if (priv->product != NULL)
                arv_info_device ("[UvDevice::new] Product = %s", priv->product);
        if (priv->serial_number != NULL)
                arv_info_device ("[UvDevice::new] S/N     = %s", priv->serial_number);
        if (priv->guid != NULL)
                arv_info_device ("[UvDevice::new] GUID    = %s", priv->guid);

	priv->packet_id = 65300; /* Start near the end of the circular counter */
	priv->timeout_ms = 32;

	if (!_open_usb_device (uv_device, &error)) {
		arv_device_take_init_error (ARV_DEVICE (uv_device), error);
                return;
	}

        if (priv->usb_major_version < 3)
                arv_warning_device ("[UvDevice::new] USB %d.%d (Low bandwidth)",
                                    priv->usb_major_version, priv->usb_minor_version);
        else
                arv_info_device ("[UvDevice::new] USB %d.%d",
                                 priv->usb_major_version, priv->usb_minor_version);

	arv_info_device("[UvDevice::new] Using control endpoint OUT=0x%02x IN=0x%02x, interface %d",
			 priv->control_endpoint_out, priv->control_endpoint_in, priv->control_interface);
	arv_info_device("[UvDevice::new] Using data endpoint 0x%02x, interface %d",
			 priv->data_endpoint, priv->data_interface);

        result = libusb_claim_interface (priv->usb_device, priv->control_interface);
        if (result != 0) {
                arv_device_take_init_error (ARV_DEVICE (uv_device),
                                            g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
                                                         "Failed to claim USB control interface to '%s-%s-%s-%s': %s",
                                                         priv->vendor, priv->product, priv->serial_number, priv->guid,
                                                         libusb_error_name (result)));
                return;
        }

        result = libusb_claim_interface (priv->usb_device, priv->data_interface);
        if (result != 0) {
                arv_device_take_init_error (ARV_DEVICE (uv_device),
                                            g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
                                                         "Failed to claim USB data interface to '%s-%s-%s-%s': %s",
                                                         priv->vendor, priv->product, priv->serial_number, priv->guid,
                                                         libusb_error_name (result)));
                return;
        }

	if ( !_bootstrap (uv_device)){
		arv_device_take_init_error (ARV_DEVICE (uv_device),
                                            g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
                                                         "Failed to bootstrap USB device '%s-%s-%s-%s'",
                                                         priv->vendor, priv->product, priv->serial_number, priv->guid));
                return;
        }

	if (!ARV_IS_GC (priv->genicam)) {
		arv_device_take_init_error (ARV_DEVICE (uv_device),
                                            g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_GENICAM_NOT_FOUND,
                                                         "Failed to load Genicam data for USB device '%s-%s-%s-%s'",
                                                         priv->vendor, priv->product, priv->serial_number, priv->guid));
                return;
        }

        /* Hotplug is optional in libusb: the default Windows build has none */
        if (libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG)) {
                result = libusb_hotplug_register_callback (priv->usb, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0,
                                                           LIBUSB_HOTPLUG_MATCH_ANY,
                                                           LIBUSB_HOTPLUG_MATCH_ANY,
                                                           LIBUSB_HOTPLUG_MATCH_ANY,
                                                           _disconnect_event,
                                                           uv_device,
                                                           &priv->hotplug_cb_handle);
                priv->hotplug_cb_registered = result == LIBUSB_SUCCESS;
                if (priv->hotplug_cb_registered)
                        arv_info_device ("[UvDevice::new] Hotplug enabled");
                else
                        arv_warning_device ("[UvDevice::new] Hotplug unavailable: "
                                            "failed to register the callback (%s)",
                                            libusb_error_name (result));
        } else
                arv_info_device ("[UvDevice::new] Hotplug unavailable: not supported by this libusb");

	priv->usb_mode = ARV_UV_USB_MODE_DEFAULT;
        priv->maximum_transfer_size = ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_DEFAULT;

	priv->event_thread_run = 1;
	priv->event_thread = g_thread_new ( "arv_libusb", event_thread_func, priv);
}

static void
arv_uv_device_init (ArvUvDevice *uv_device)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

	priv->cmd_packet_size_max = 65536 + sizeof (ArvUvcpHeader);
	priv->ack_packet_size_max = 65536 + sizeof (ArvUvcpHeader);
	priv->disconnected = FALSE;
        priv->event_thread_priority = ARV_UV_DEVICE_NO_THREAD_PRIORITY;
}

static void
arv_uv_device_finalize (GObject *object)
{
	ArvUvDevice *uv_device = ARV_UV_DEVICE (object);

	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (uv_device);

        if (priv->hotplug_cb_registered)
                libusb_hotplug_deregister_callback (priv->usb, priv->hotplug_cb_handle);

	priv->event_thread_run = 0;
        if (priv->event_thread)
                g_thread_join (priv->event_thread);

	g_clear_object (&priv->genicam);

	g_clear_pointer (&priv->vendor, g_free);
	g_clear_pointer (&priv->product, g_free);
	g_clear_pointer (&priv->serial_number, g_free);
        g_clear_pointer (&priv->guid, g_free);
	g_clear_pointer (&priv->genicam_xml, g_free);
	if (priv->usb_device != NULL) {
		libusb_release_interface (priv->usb_device, priv->control_interface);
		libusb_release_interface (priv->usb_device, priv->data_interface);
		libusb_close (priv->usb_device);
	}
        if (priv->usb != NULL)
                libusb_exit (priv->usb);
        g_mutex_clear (&priv->transfer_mutex);

	G_OBJECT_CLASS (arv_uv_device_parent_class)->finalize (object);
}

static void
arv_uv_device_set_property (GObject *self, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	ArvUvDevicePrivate *priv = arv_uv_device_get_instance_private (ARV_UV_DEVICE (self));

	switch (prop_id)
	{
		case PROP_UV_DEVICE_VENDOR:
			g_free (priv->vendor);
			priv->vendor = g_value_dup_string (value);
			break;
		case PROP_UV_DEVICE_PRODUCT:
			g_free (priv->product);
			priv->product = g_value_dup_string (value);
			break;
		case PROP_UV_DEVICE_SERIAL_NUMBER:
			g_free (priv->serial_number);
			priv->serial_number = g_value_dup_string (value);
			break;
		case PROP_UV_DEVICE_GUID:
			g_free (priv->guid);
			priv->guid = g_value_dup_string (value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
			break;
	}
}

static void
arv_uv_device_class_init (ArvUvDeviceClass *uv_device_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (uv_device_class);
	ArvDeviceClass *device_class = ARV_DEVICE_CLASS (uv_device_class);

	object_class->finalize = arv_uv_device_finalize;
	object_class->constructed = arv_uv_device_constructed;
	object_class->set_property = arv_uv_device_set_property;

	device_class->create_stream = arv_uv_device_create_stream;
	device_class->get_genicam_xml = arv_uv_device_get_genicam_xml;
	device_class->get_genicam = arv_uv_device_get_genicam;
	device_class->read_memory = arv_uv_device_read_memory;
	device_class->write_memory = arv_uv_device_write_memory;
	device_class->read_register = arv_uv_device_read_register;
	device_class->write_register = arv_uv_device_write_register;

	g_object_class_install_property
		(object_class,
		 PROP_UV_DEVICE_VENDOR,
		 g_param_spec_string ("vendor",
				      "Vendor",
				      "USB3 device vendor string",
				      NULL,
				      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property
		(object_class,
		 PROP_UV_DEVICE_PRODUCT,
		 g_param_spec_string ("product",
				      "Product",
				      "USB3 device product string",
				      NULL,
				      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property
		(object_class,
		 PROP_UV_DEVICE_SERIAL_NUMBER,
		 g_param_spec_string ("serial-number",
				      "Serial number",
				      "USB3 device serial number",
				      NULL,
				      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property
		(object_class,
		 PROP_UV_DEVICE_GUID,
		 g_param_spec_string ("guid",
				      "GUID",
				      "USB3 device GUID",
				      NULL,
				      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
}
