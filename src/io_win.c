/*
 *	wiiuse
 *
 *	Written By:
 *		Michael Laforest	< para >
 *		Email: < thepara (--AT--) g m a i l [--DOT--] com >
 *
 *	Copyright 2006-2007
 *
 *	This file is part of wiiuse.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *	$Header$
 *
 */

/**
 *	@file
 *	@brief Handles device I/O for Windows.
 */


#include "io.h"

#ifdef WIIUSE_WIN32
#include <stdlib.h>

#include <hidsdi.h>
#include <setupapi.h>

#ifdef __MINGW32__
/* this prototype is missing from the mingw headers so we must add it
	or suffer linker errors. */
#	ifdef __cplusplus
extern "C" {
#	endif
	WINHIDSDI BOOL WINAPI HidD_SetOutputReport(HANDLE, PVOID, ULONG);
#	ifdef __cplusplus
}
#	endif
#endif

/*This internal variable is passed to
  the ReadFile functions to avoid an error.

  It doesn't seem to matter that they're all
  asynchronously using the same one,
  but we can't let the variable fall out of scope.
  */
static DWORD wu_dummy;

/**
  Enqueue a read on the device's 
  stream and check for device
  disconnect.
*/
void wiiuse_setup_read(struct wiimote_t* wm)
{
  DWORD err;
  if (!ReadFile(wm->dev_handle, wm->event_buf, sizeof(wm->event_buf), &wu_dummy, &wm->hid_overlap)) {
     err = GetLastError();
     switch (err) {
       case ERROR_HANDLE_EOF:
       case ERROR_DEVICE_NOT_CONNECTED:
         wiiuse_disconnected(wm);
         break;
       case ERROR_IO_PENDING:
         /*Not an error, this is what we intended.*/
         break;
       default:
         /*Should we report an unexpected error?*/
         break;
     }
  } else {
    /* We read a packet.  We'll deal with it when we poll next. */
    /* WaitForSingleObject will return immediately. */
  }
}


int wiiuse_find(struct wiimote_t** wm, int max_wiimotes, int timeout) {
	GUID device_id;
	HANDLE dev;
	HDEVINFO device_info;
	int i, index;
	DWORD len;
	SP_DEVICE_INTERFACE_DATA device_data;
	PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data = NULL;
	HIDD_ATTRIBUTES	attr;
	int found = 0;

	(void) timeout; /* unused */

	device_data.cbSize = sizeof(device_data);
	index = 0;

	/* get the device id */
	HidD_GetHidGuid(&device_id);

	/* get all hid devices connected */
	device_info = SetupDiGetClassDevs(&device_id, NULL, NULL, (DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));

	for (;; ++index) {

		if (detail_data) {
			free(detail_data);
			detail_data = NULL;
		}

		/* query the next hid device info */
		if (!SetupDiEnumDeviceInterfaces(device_info, NULL, &device_id, index, &device_data))
			break;

		/* get the size of the data block required */
		i = SetupDiGetDeviceInterfaceDetail(device_info, &device_data, NULL, 0, &len, NULL);
		detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)malloc(len);
		detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		/* query the data for this device */
		if (!SetupDiGetDeviceInterfaceDetail(device_info, &device_data, detail_data, len, NULL, NULL))
			continue;

		/* open the device */
		dev = CreateFile(detail_data->DevicePath,
						(GENERIC_READ | GENERIC_WRITE),
						(FILE_SHARE_READ | FILE_SHARE_WRITE),
						NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (dev == INVALID_HANDLE_VALUE)
			continue;

		/* get device attributes */
		attr.Size = sizeof(attr);
		i = HidD_GetAttributes(dev, &attr);

		if ((attr.VendorID == WM_VENDOR_ID) && (attr.ProductID == WM_PRODUCT_ID)) {
			/* this is a wiimote */
			wm[found]->dev_handle = dev;

			wm[found]->hid_overlap.hEvent = CreateEvent(NULL, 1, 1, "");
			wm[found]->hid_overlap.Offset = 0;
			wm[found]->hid_overlap.OffsetHigh = 0;

			WIIMOTE_ENABLE_STATE(wm[found], WIIMOTE_STATE_DEV_FOUND);
			WIIMOTE_ENABLE_STATE(wm[found], WIIMOTE_STATE_CONNECTED);

			/* try to set the output report to see if the device is actually connected */
			if (!wiiuse_set_report_type(wm[found])) {
				WIIMOTE_DISABLE_STATE(wm[found], WIIMOTE_STATE_CONNECTED);
				continue;
			}

			/* do the handshake */
			wiiuse_handshake(wm[found], NULL, 0);

			WIIUSE_INFO("Connected to wiimote [id %i].", wm[found]->unid);
      /* Issue an initial read-request. */
      wiiuse_setup_read(wm[found]);

			++found;
			if (found >= max_wiimotes)
				break;
		} else {
			/* not a wiimote */
			CloseHandle(dev);
		}
	}

	if (detail_data)
		free(detail_data);

	SetupDiDestroyDeviceInfoList(device_info);

	return found;
}


int wiiuse_connect(struct wiimote_t** wm, int wiimotes) {
	int connected = 0;
	int i = 0;

	for (; i < wiimotes; ++i) {
		if (!wm[i])
			continue;
		if (WIIMOTE_IS_SET(wm[i], WIIMOTE_STATE_CONNECTED))
			++connected;
	}

	return connected;
}


void wiiuse_disconnect(struct wiimote_t* wm) {
	if (!wm || WIIMOTE_IS_CONNECTED(wm))
		return;

	CloseHandle(wm->dev_handle);
	wm->dev_handle = 0;

	ResetEvent(&wm->hid_overlap);

	wm->event = WIIUSE_NONE;

	WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_CONNECTED);
	WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_HANDSHAKE);
}

/*
 *  This function was changed to 
 *  make the reading be more like 
 *  the *nix style: instead of blocking
 *  if no data are available, we just
 *  move on until some packets arrive.
*/
int wiiuse_io_read(struct wiimote_t* wm) {
	DWORD  r;
  int ii;

	if (!wm || !WIIMOTE_IS_CONNECTED(wm))
		return 0;
  /* 
   * A packet is either ready or we'll check back next loop.
   */
  r = WaitForSingleObject(wm->hid_overlap.hEvent, 1);
  switch (r) {
    case WAIT_TIMEOUT:
      /* No sweat.  We just don't have a packet yet.*/
      return 0;
      break;
    case WAIT_OBJECT_0:
      /* Got some data. 
       * Clear the overlap event
       * for next time around.
       */
      ResetEvent(wm->hid_overlap.hEvent);
      return 1;
      break;
    case WAIT_FAILED:
      /* Problem. */
      WIIUSE_WARNING("A wait error occured on reading from wiimote %i.", wm->unid);
      /* Fall through */
    default:
      /* Uh oh. */      
      CancelIo(wm->dev_handle);
      ResetEvent(wm->hid_overlap.hEvent);
      wiiuse_setup_read(wm);
      return 0;
      break;
  }
  return 0;
}
/*
int wiiuse_io_read(struct wiimote_t* wm) {
	DWORD b, r;

	if (!wm || !WIIMOTE_IS_CONNECTED(wm))
		return 0;

	if (!ReadFile(wm->dev_handle, wm->event_buf, sizeof(wm->event_buf), &b, &wm->hid_overlap)) {
		// partial read //
		b = GetLastError();

		if ((b == ERROR_HANDLE_EOF) || (b == ERROR_DEVICE_NOT_CONNECTED)) {
			// remote disconnect //
			wiiuse_disconnected(wm);
			return 0;
		}

		r = WaitForSingleObject(wm->hid_overlap.hEvent, wm->timeout);
		if (r == WAIT_TIMEOUT) {
			// timeout - cancel and continue //

			if (*wm->event_buf)
				WIIUSE_WARNING("Packet ignored.  This may indicate a problem (timeout is %i ms).", wm->timeout);

			CancelIo(wm->dev_handle);
			ResetEvent(wm->hid_overlap.hEvent);
			return 0;
		} else if (r == WAIT_FAILED) {
			WIIUSE_WARNING("A wait error occured on reading from wiimote %i.", wm->unid);
			return 0;
		}

		if (!GetOverlappedResult(wm->dev_handle, &wm->hid_overlap, &b, 0))
			return 0;
	}

	ResetEvent(wm->hid_overlap.hEvent);
	return 1;
}
*/

int wiiuse_io_write(struct wiimote_t* wm, byte* buf, int len) {
	DWORD bytes;
	int i;

	if (!wm || !WIIMOTE_IS_CONNECTED(wm))
		return 0;

	switch (wm->stack) {
		case WIIUSE_STACK_UNKNOWN:
		{
			/* try to auto-detect the stack type */
			if (i = WriteFile(wm->dev_handle, buf, 22, &bytes, &wm->hid_overlap)) {
				/* bluesoleil will always return 1 here, even if it's not connected */
				wm->stack = WIIUSE_STACK_BLUESOLEIL;
				return i;
			}

			if (i = HidD_SetOutputReport(wm->dev_handle, buf, len)) {
				wm->stack = WIIUSE_STACK_MS;
				return i;
			}

			WIIUSE_ERROR("Unable to determine bluetooth stack type.");
			return 0;
		}

		case WIIUSE_STACK_MS:
			return HidD_SetOutputReport(wm->dev_handle, buf, len);

		case WIIUSE_STACK_BLUESOLEIL:
			return WriteFile(wm->dev_handle, buf, 22, &bytes, &wm->hid_overlap);
	}

	return 0;
}

void wiiuse_init_platform_fields(struct wiimote_t* wm) {
	wm->dev_handle = 0;
	wm->stack = WIIUSE_STACK_UNKNOWN;
	wm->normal_timeout = WIIMOTE_DEFAULT_TIMEOUT;
	wm->exp_timeout = WIIMOTE_EXP_TIMEOUT;
	wm->timeout = wm->normal_timeout;
}

void wiiuse_cleanup_platform_fields(struct wiimote_t* wm) {
	wm->dev_handle = 0;
}

#endif /* ifdef WIIUSE_WIN32 */
