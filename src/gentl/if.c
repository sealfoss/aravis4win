/* Aravis - Digital camera library
 *
 * Copyright © 2023 Václav Šmilauer
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
 * Authors: Václav Šmilauer <eu@doxos.eu>
 */

#include "private.h"
#include <arvinterface.h>
#include <arvcamera.h>

#define _IF_CHECK_HANDLE GENTL_ENSURE_INIT; if(hIface==NULL || !(ARV_IS_INTERFACE(hIface))) return GC_ERR_INVALID_HANDLE;

GC_ERROR
IFClose (IF_HANDLE hIface)
{
	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p])",__FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface);

	/* The reference taken by TLOpenInterface */
	g_object_unref (hIface);

	return GC_ERR_SUCCESS;
}

GC_ERROR
IFGetInfo (IF_HANDLE hIface, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p], iInfoCmd=%d)",__FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface,iInfoCmd);

	return gentl_get_interface_info (g_object_get_data (G_OBJECT (hIface), GENTL_INTERFACE_ID_KEY),
					 iInfoCmd, piType, pBuffer, piSize);
}

GC_ERROR
IFGetNumDevices (IF_HANDLE hIface, uint32_t *piNumDevices)
{
	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p], piNumDevices=%p)",
                        __FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface,piNumDevices);

	if(piNumDevices==NULL)
                return GC_ERR_INVALID_PARAMETER;

	*piNumDevices=arv_interface_get_n_devices(hIface);
	arv_trace_gentl("    (returning %u)",*piNumDevices);

	return GC_ERR_SUCCESS;
}

GC_ERROR
IFGetDeviceID (IF_HANDLE hIface, uint32_t iIndex, char *sIDeviceID, size_t *piSize)
{
	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p], iIndex=%u, sIDeviceId=%p, piSize=%p)",
                        __FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface,iIndex,sIDeviceID,piSize);

	if (iIndex >= arv_interface_get_n_devices (hIface))
		return GC_ERR_INVALID_INDEX;

	return gentl_to_buf(INFO_DATATYPE_STRING,sIDeviceID,arv_interface_get_device_id(hIface,iIndex),piSize,NULL);
}

/* The device ids of an interface, for the comparison of the device lists */

static char *
_dup_device_ids (ArvInterface *iface)
{
	GString *ids = g_string_new (NULL);
	guint i;

	for (i = 0; i < arv_interface_get_n_devices (iface); i++) {
		const char *id = arv_interface_get_device_id (iface, i);

		g_string_append (ids, id != NULL ? id : "");
		g_string_append_c (ids, '\n');
	}

	return g_string_free (ids, FALSE);
}

GC_ERROR
IFUpdateDeviceList (IF_HANDLE hIface, bool8_t *pbChanged, uint64_t iTimeout)
{
	char *previous_ids;

	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p], pbChanged=%p, iTimeout=%" G_GUINT64_FORMAT ")",
                        __FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface,pbChanged,(guint64) iTimeout);

	previous_ids = _dup_device_ids (hIface);
	arv_interface_update_device_list(hIface);

	if (pbChanged != NULL) {
		char *ids = _dup_device_ids (hIface);

		*pbChanged = g_strcmp0 (previous_ids, ids) != 0;
		g_free (ids);
	}

	g_free (previous_ids);

	return GC_ERR_SUCCESS;
}

GC_ERROR
IFGetDeviceInfo (IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType,
                 void *pBuffer, size_t *piSize)
{
	gint i, iDev;

	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p], sDeviceID=%s, iInfoCmd=%d, piSize=%p)",
                        __FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface,sDeviceID,iInfoCmd,piSize);

	iDev=-1;
	for (i=0; i<arv_interface_get_n_devices(hIface); i++) {
		if (g_strcmp0(arv_interface_get_device_id(hIface,i),sDeviceID)==0) {
                        iDev=i;
                        break;
                }
	}
	if (iDev<0)
                return GC_ERR_INVALID_ID;

	#if 0
		/* do we need device objects for some of the data (like version, access status, timestamp frequency? must be freed again. */
		ArvDevice* dev=arv_interface_open_device(hIface,sDeviceID,&gentl_err);
		if(!dev) return GC_ERR_INVALID_ID;
	#endif

	switch(iInfoCmd) {
		case DEVICE_INFO_ID:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            arv_interface_get_device_id(hIface,iDev),piSize,piType);
		case DEVICE_INFO_VENDOR:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            arv_interface_get_device_vendor(hIface,iDev),piSize,piType);
		case DEVICE_INFO_MODEL:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            arv_interface_get_device_model(hIface,iDev),piSize,piType);
		case DEVICE_INFO_TLTYPE:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            gentl_get_tl_type (arv_interface_get_device_protocol(hIface,iDev)),
                                            piSize,piType);
		case DEVICE_INFO_DISPLAYNAME:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            /* ?? */arv_interface_get_device_id(hIface,iDev),piSize,piType);
		case DEVICE_INFO_USER_DEFINED_NAME:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            /* ?? */arv_interface_get_device_id(hIface,iDev),piSize,piType);
		case DEVICE_INFO_SERIAL_NUMBER:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,
                                            arv_interface_get_device_serial_nbr(hIface,iDev),piSize,piType);
		/* TODO: where is this supposed to come from? */
		case DEVICE_INFO_VERSION:
                        return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,"1.0",piSize,piType);
		case DEVICE_INFO_ACCESS_STATUS:
                        {
                                int32_t rw=DEVICE_ACCESS_STATUS_READWRITE;
                                return gentl_to_buf(INFO_DATATYPE_INT32,pBuffer,&rw,piSize,piType);
                        }
		case DEVICE_INFO_TIMESTAMP_FREQUENCY:
		default:
			GENTL_NYI_DETAIL("iInfoCmd=%d",iInfoCmd);
	}
}

GC_ERROR
IFOpenDevice (IF_HANDLE hIface, const char *sDeviceID, DEVICE_ACCESS_FLAGS iOpenFlag, DEV_HANDLE *phDevice)
{
	ArvDevice *device;
	ArvCamera* cam;
	GError *error = NULL;

	_IF_CHECK_HANDLE;
	arv_trace_gentl("%s (hIface=%s[%p], sDeviceID=%s, iOpenFlag=%d)",
                        __FUNCTION__,G_OBJECT_TYPE_NAME(hIface),hIface,sDeviceID,iOpenFlag);

	if (sDeviceID == NULL || phDevice == NULL)
		return GC_ERR_INVALID_PARAMETER;

	/* Opened through its interface: arv_open_device() would search every enabled interface, GenTL included, and
	 * take the lock of the Aravis device discovery, which the GenTL consumer of Aravis holds while it opens a device
	 * of this producer. */
	device = arv_interface_open_device (hIface, sDeviceID, &error);
	if (device == NULL) {
		if (error == NULL)
			return gentl_take_error (GC_ERR_INVALID_ID,
						 g_error_new (ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND,
							      "Device '%s' not found", sDeviceID));
		return gentl_take_error (GC_ERR_IO, error);
	}

	cam = arv_camera_new_with_device (device, &error);
	g_object_unref (device);
	if (cam == NULL)
		return gentl_take_error (GC_ERR_IO, error);

	*phDevice=cam;
	return GC_ERR_SUCCESS;
}

/* GenTL v1.4 */
GC_ERROR
IFGetParentTL (IF_HANDLE hIface, TL_HANDLE *phSystem)
{
        GENTL_NYI;
}

