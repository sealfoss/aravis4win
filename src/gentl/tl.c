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

#include<arvsystem.h>
#include<arvinterface.h>

#define _TL_CHECK_HANDLE { GENTL_ENSURE_INIT; if(hTL==NULL || !ARV_IS_TRANSPORT_LAYER(hTL)) return GC_ERR_INVALID_HANDLE; if(gentl_transport_layer==NULL) return GC_ERR_NOT_INITIALIZED; }

GC_ERROR
TLOpen (TL_HANDLE *phTL)
{
	arv_trace_gentl(__FUNCTION__);
	GENTL_ENSURE_INIT;

	if(phTL==NULL)
                return GC_ERR_INVALID_PARAMETER;
	if (gentl_transport_layer!=NULL)
                return GC_ERR_RESOURCE_IN_USE;

	/* The interface selection of the process is left as it is: the GenTL interface is not exposed, and the
	 * devices are opened by their interface. */

	gentl_transport_layer= g_object_new (ARV_TYPE_TRANSPORT_LAYER,NULL);
	*phTL=(void*)gentl_transport_layer;

	return GC_ERR_SUCCESS;
}

GC_ERROR
TLClose (TL_HANDLE hTL)
{
	arv_trace_gentl("%s (phTL=%s[%p])",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL);
	_TL_CHECK_HANDLE;

	g_object_unref(gentl_transport_layer);
	gentl_transport_layer=NULL;

	return GC_ERR_SUCCESS;
}

GC_ERROR
TLGetInfo (TL_HANDLE hTL, TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	arv_trace_gentl("%s (hTL=%s[%p], iInfoCmd=%d)",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL,iInfoCmd);
	_TL_CHECK_HANDLE;

	/* The information of the system module */
	return GCGetInfo (iInfoCmd, piType, pBuffer, piSize);
}

GC_ERROR
TLGetNumInterfaces (TL_HANDLE hTL, uint32_t *piNumIfaces)
{
	arv_trace_gentl("%s (hTL=%s[%p])",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL);
	_TL_CHECK_HANDLE;

	if(!piNumIfaces)
                return GC_ERR_INVALID_PARAMETER;

	*piNumIfaces=gentl_get_n_interfaces();
	arv_trace_gentl("   → %u interfaces",*piNumIfaces);

	return GC_ERR_SUCCESS;
}

GC_ERROR
TLGetInterfaceID (TL_HANDLE hTL, uint32_t iIndex,  char *sID, size_t *piSize)
{
	arv_trace_gentl("%s (hTL=%s[%p],iIndex=%u,sID=%p,piSize=%p)",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL,iIndex,sID,piSize);
	_TL_CHECK_HANDLE;

	if(iIndex>=gentl_get_n_interfaces())
                return GC_ERR_INVALID_INDEX;

	return gentl_to_buf(INFO_DATATYPE_STRING,sID,gentl_get_interface_id(iIndex),piSize,NULL);
}

GC_ERROR
TLGetInterfaceInfo (TL_HANDLE hTL, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType,
                    void *pBuffer, size_t *piSize)
{
	arv_trace_gentl("%s (hTL=%s[%p], sIfaceID=%s, iInfoCmd=%d)",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL,sIfaceID,iInfoCmd);
	_TL_CHECK_HANDLE;

	/* Available before TLOpenInterface, which updates the device list */
	return gentl_get_interface_info (sIfaceID, iInfoCmd, piType, pBuffer, piSize);
}

GC_ERROR
TLOpenInterface (TL_HANDLE hTL, const char *sIfaceID, IF_HANDLE *phIface)
{
	ArvInterface* iface;
	guint i;

	/* TODO: return GC_ERR_RESOURCE_IN_USE if the iface was already open via TLOpenInterface */
	arv_trace_gentl("%s (hTL=%s[%p], sIfaceID=%p, phIface=%p)",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL,sIfaceID,phIface);
	_TL_CHECK_HANDLE;

	if (sIfaceID==NULL || phIface==NULL)
                return GC_ERR_INVALID_PARAMETER;

	/* Only the interfaces of the transport layer, which excludes GenTL */
	for (i = 0; i < gentl_get_n_interfaces (); i++) {
		const char *interface_id = gentl_get_interface_id (i);

		if (g_strcmp0 (interface_id, sIfaceID) == 0) {
			iface = arv_get_interface_by_id (interface_id);
			if (!iface)
				return GC_ERR_INVALID_ID;

			/* For IFGetInfo */
			g_object_set_data (G_OBJECT (iface), GENTL_INTERFACE_ID_KEY, (gpointer) interface_id);

			/* Referenced until IFClose: arv_shutdown() destroys the interface instances of Aravis, while
			 * the consumer may still use the handle, at least to close it with a device opened before. */
			*phIface=(void*)g_object_ref (iface);
			arv_interface_update_device_list(iface);

			return GC_ERR_SUCCESS;
		}
	}

	return GC_ERR_INVALID_ID;
}

GC_ERROR
TLUpdateInterfaceList (TL_HANDLE hTL, bool8_t *pbChanged, uint64_t iTimeout)
{
	arv_trace_gentl("%s (hTL=%s[%p], pbChanged=%p, iTimeout=%" G_GUINT64_FORMAT ")",__FUNCTION__,G_OBJECT_TYPE_NAME(hTL),hTL,pbChanged,(guint64) iTimeout);
	_TL_CHECK_HANDLE;

	/* The interface list never changes */
	if(pbChanged)
                *pbChanged=0;

	return GC_ERR_SUCCESS;
}
