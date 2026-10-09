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
Event module: not implemented. The functions are exported for the consumers which require them, the Aravis consumer
included, and fail with GC_ERR_NOT_IMPLEMENTED. GCRegisterEvent fails the same way, so no event handle exists.
*/

GC_ERROR
EventGetData (EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout)
{
        GENTL_NYI;
}

GC_ERROR
EventGetDataInfo (EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize, EVENT_DATA_INFO_CMD iInfoCmd,
                  INFO_DATATYPE *piType, void *pOutBuffer, size_t *piOutSize)
{
        GENTL_NYI;
}

GC_ERROR
EventGetInfo (EVENT_HANDLE hEvent, EVENT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
        GENTL_NYI;
}

GC_ERROR
EventFlush (EVENT_HANDLE hEvent)
{
        GENTL_NYI;
}

GC_ERROR
EventKill (EVENT_HANDLE hEvent)
{
        GENTL_NYI;
}
