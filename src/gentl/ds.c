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

/*
Data stream module: not implemented. The functions are exported for the consumers which require them, the Aravis
consumer included, and fail with GC_ERR_NOT_IMPLEMENTED. DevOpenDataStream fails the same way.
*/

GC_ERROR
DSAnnounceBuffer (DS_HANDLE hDataStream, void *pBuffer, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer)
{
        GENTL_NYI;
}

GC_ERROR
DSAllocAndAnnounceBuffer (DS_HANDLE hDataStream, size_t iBufferSize, void *pPrivate, BUFFER_HANDLE *phBuffer)
{
        GENTL_NYI;
}

GC_ERROR
DSFlushQueue (DS_HANDLE hDataStream, ACQ_QUEUE_TYPE iOperation)
{
        GENTL_NYI;
}

GC_ERROR
DSStartAcquisition (DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire)
{
        GENTL_NYI;
}

GC_ERROR
DSStopAcquisition (DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags)
{
        GENTL_NYI;
}

GC_ERROR
DSGetInfo (DS_HANDLE hDataStream, STREAM_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
        GENTL_NYI;
}

GC_ERROR
DSGetBufferID (DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer)
{
        GENTL_NYI;
}

GC_ERROR
DSClose (DS_HANDLE hDataStream)
{
        GENTL_NYI;
}

GC_ERROR
DSRevokeBuffer (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **ppBuffer, void **ppPrivate)
{
        GENTL_NYI;
}

GC_ERROR
DSQueueBuffer (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer)
{
        GENTL_NYI;
}

GC_ERROR
DSGetBufferInfo (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd, INFO_DATATYPE *piType,
                 void *pBuffer, size_t *piSize)
{
        GENTL_NYI;
}

/* GenTL v1.3 */

GC_ERROR
DSGetBufferChunkData (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, SINGLE_CHUNK_DATA *pChunkData, size_t *piNumChunks)
{
        GENTL_NYI;
}

/* GenTL v1.4 */

GC_ERROR
DSGetParentDev (DS_HANDLE hDataStream, DEV_HANDLE *phDevice)
{
        GENTL_NYI;
}

/* GenTL v1.5 */

GC_ERROR
DSGetNumBufferParts (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t *piNumParts)
{
        GENTL_NYI;
}

GC_ERROR
DSGetBufferPartInfo (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t iPartIndex, BUFFER_PART_INFO_CMD iInfoCmd,
                     INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
        GENTL_NYI;
}

/* GenTL v1.6 */

GC_ERROR
DSAnnounceCompositeBuffer (DS_HANDLE hDataStream, size_t iNumSegments, void **ppSegments, size_t *piSizes,
                           void *pPrivate, BUFFER_HANDLE *phBuffer)
{
        GENTL_NYI;
}

GC_ERROR
DSGetBufferInfoStacked (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, DS_BUFFER_INFO_STACKED *pInfoStacked,
                        size_t iNumInfos)
{
        GENTL_NYI;
}

GC_ERROR
DSGetBufferPartInfoStacked (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, DS_BUFFER_PART_INFO_STACKED *pInfoStacked,
                            size_t iNumInfos)
{
        GENTL_NYI;
}

GC_ERROR
DSGetNumFlows (DS_HANDLE hDataStream, uint32_t *piNumFlows)
{
        GENTL_NYI;
}

GC_ERROR
DSGetFlowInfo (DS_HANDLE hDataStream, uint32_t iFlowIndex, FLOW_INFO_CMD iInfoCmd, INFO_DATATYPE *piType,
               void *pBuffer, size_t *piSize)
{
        GENTL_NYI;
}

GC_ERROR
DSGetNumBufferSegments (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t *piNumSegments)
{
        GENTL_NYI;
}

GC_ERROR
DSGetBufferSegmentInfo (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t iSegmentIndex,
                        SEGMENT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
        GENTL_NYI;
}
