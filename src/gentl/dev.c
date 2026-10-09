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

#include"private.h"
#include<arvcamera.h>

/*
GenTL's Device is Aravis' Camera. GenTL's Device port is Aravis' Device.
*/

#define _DEV_CHECK_HANDLE GENTL_ENSURE_INIT; if(hDevice==NULL || !(ARV_IS_CAMERA(hDevice))) return GC_ERR_INVALID_HANDLE;

/* helper function to get string from camera via function all (e.g. arv_camera_get_device_id) and return it to the GenTL
 * buffer */

static GC_ERROR
gentl_to_buf__cam_string (DEV_HANDLE hDevice, const char* (*func)(ArvCamera*,GError**),
                          INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	GError *error = NULL;
	const char* n;

	n=func(hDevice,&error);
	if(n==NULL)
                return gentl_take_error (GC_ERR_IO, error);

	return gentl_to_buf(INFO_DATATYPE_STRING,pBuffer,n,piSize,piType);
};

/* An Aravis device has one stream, a GigE Vision device may have more channels */

static GC_ERROR
_get_n_data_streams (DEV_HANDLE hDevice, guint *n_data_streams)
{
	GError *error = NULL;

	if (!arv_camera_is_gv_device (hDevice)) {
		*n_data_streams = 1;
		return GC_ERR_SUCCESS;
	}

	*n_data_streams = arv_camera_gv_get_n_stream_channels (hDevice, &error);
	if (error != NULL)
		return gentl_take_error (GC_ERR_IO, error);

	return GC_ERR_SUCCESS;
}

GC_ERROR
DevGetPort (DEV_HANDLE hDevice, PORT_HANDLE *phRemoteDevice)
{
        _DEV_CHECK_HANDLE;
        arv_trace_gentl("%s (hDevice=%s[%p])",__FUNCTION__,G_OBJECT_TYPE_NAME(hDevice),hDevice);

        *phRemoteDevice=arv_camera_get_device(hDevice);

        return GC_ERR_SUCCESS;
}

GC_ERROR
DevGetNumDataStreams (DEV_HANDLE hDevice, uint32_t *piNumDataStreams)
{
	GC_ERROR error;
	guint n_data_streams;

	_DEV_CHECK_HANDLE
	arv_trace_gentl("%s (hDevice=%s[%p])",__FUNCTION__,G_OBJECT_TYPE_NAME(hDevice),hDevice);

	if(piNumDataStreams==NULL)
                return GC_ERR_INVALID_PARAMETER;

        /* GenTL provides no way to clear last error, which is kept in gentl_err as required by the specification:
         * the Aravis calls use their own error. */
	error = _get_n_data_streams (hDevice, &n_data_streams);
	if (error != GC_ERR_SUCCESS)
		return error;

	*piNumDataStreams = n_data_streams;
	arv_trace_gentl("   (returning %u)",*piNumDataStreams);

	return GC_ERR_SUCCESS;
}

GC_ERROR
DevGetDataStreamID (DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize)
{
	GC_ERROR error;
	guint chan;
	char buf[16];

	_DEV_CHECK_HANDLE
	arv_trace_gentl("%s (hDevice=%s[%p], iIndex=%u)",__FUNCTION__,G_OBJECT_TYPE_NAME(hDevice),hDevice,iIndex);

	error = _get_n_data_streams (hDevice, &chan);
	if (error != GC_ERR_SUCCESS)
		return error;
	if(chan==0)
                return GC_ERR_NOT_IMPLEMENTED;
	if (iIndex >= chan)
                return GC_ERR_INVALID_INDEX;

	g_snprintf(buf, sizeof(buf), "%u", iIndex);

	/* GenTL identifies streams by strings, so we just use 0-based channel index */
	return gentl_to_buf(INFO_DATATYPE_STRING,sDataStreamID,buf,piSize,NULL);
}

GC_ERROR
DevOpenDataStream (DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream)
{
	GENTL_NYI;
}

GC_ERROR
DevGetInfo ( DEV_HANDLE hDevice, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{
	_DEV_CHECK_HANDLE;
	arv_trace_gentl("%s (hDevice=%s[%p],iInfoCmd=%d)",__FUNCTION__,G_OBJECT_TYPE_NAME(hDevice),hDevice,iInfoCmd);

	switch(iInfoCmd){
		case DEVICE_INFO_ID:
			return gentl_to_buf__cam_string(hDevice,arv_camera_get_device_id,piType,pBuffer,piSize);
		default:
			GENTL_NYI_DETAIL("iInfoCmd=%d",iInfoCmd);
	}
}

GC_ERROR
DevClose (DEV_HANDLE hDevice)
{
	_DEV_CHECK_HANDLE;
	arv_trace_gentl("%s (hDevice=%s[%p])",__FUNCTION__,G_OBJECT_TYPE_NAME(hDevice),hDevice);

	/* The camera created by IFOpenDevice */
	g_object_unref (hDevice);

	return GC_ERR_SUCCESS;
}

/* GenTL v1.4 */

GC_ERROR
DevGetParentIF (DEV_HANDLE hDevice, IF_HANDLE *phIface)
{
        GENTL_NYI;
}
