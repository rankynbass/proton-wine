/*		DirectInput Mouse device
 *
 * Copyright 1998 Marcus Meissner
 * Copyright 1998,1999 Lionel Ulmer
 *
 */

#include "config.h"
#include <string.h>
#ifdef HAVE_SYS_ERRNO_H
# include <sys/errno.h>
#endif

#include "debugtools.h"
#include "input.h"
#include "user.h"
#include "winbase.h"
#include "wingdi.h"
#include "winerror.h"
#include "windef.h"
#include "mouse.h"
#include "winuser.h"
#include "dinput.h"

#include "dinput_private.h"
#include "device_private.h"

#define MOUSE_HACK

DEFAULT_DEBUG_CHANNEL(dinput);

/* Wine mouse driver object instances */
#define WINE_MOUSE_X_AXIS_INSTANCE 0x0001
#define WINE_MOUSE_Y_AXIS_INSTANCE 0x0002
#define WINE_MOUSE_L_BUTTON_INSTANCE 0x0004
#define WINE_MOUSE_R_BUTTON_INSTANCE 0x0008
#define WINE_MOUSE_M_BUTTON_INSTANCE 0x0010

/* ------------------------------- */
/* Wine mouse internal data format */
/* ------------------------------- */

/* Constants used to access the offset array */
#define WINE_MOUSE_X_POSITION 0
#define WINE_MOUSE_Y_POSITION 1
#define WINE_MOUSE_L_POSITION 2
#define WINE_MOUSE_R_POSITION 3
#define WINE_MOUSE_M_POSITION 4

typedef struct {
  LONG lX;
  LONG lY;
  BYTE rgbButtons[4];
} Wine_InternalMouseData;

#define WINE_INTERNALMOUSE_NUM_OBJS 5

static DIOBJECTDATAFORMAT Wine_InternalMouseObjectFormat[WINE_INTERNALMOUSE_NUM_OBJS] = {
  { &GUID_XAxis,   FIELD_OFFSET(Wine_InternalMouseData, lX),
      DIDFT_MAKEINSTANCE(WINE_MOUSE_X_AXIS_INSTANCE) | DIDFT_RELAXIS, 0 },
  { &GUID_YAxis,   FIELD_OFFSET(Wine_InternalMouseData, lY),
      DIDFT_MAKEINSTANCE(WINE_MOUSE_Y_AXIS_INSTANCE) | DIDFT_RELAXIS, 0 },
  { &GUID_Button, (FIELD_OFFSET(Wine_InternalMouseData, rgbButtons)) + 0,
      DIDFT_MAKEINSTANCE(WINE_MOUSE_L_BUTTON_INSTANCE) | DIDFT_PSHBUTTON, 0 },
  { &GUID_Button, (FIELD_OFFSET(Wine_InternalMouseData, rgbButtons)) + 1,
      DIDFT_MAKEINSTANCE(WINE_MOUSE_R_BUTTON_INSTANCE) | DIDFT_PSHBUTTON, 0 },
  { &GUID_Button, (FIELD_OFFSET(Wine_InternalMouseData, rgbButtons)) + 2,
      DIDFT_MAKEINSTANCE(WINE_MOUSE_M_BUTTON_INSTANCE) | DIDFT_PSHBUTTON, 0 }
};

static DIDATAFORMAT Wine_InternalMouseFormat = {
  0, /* dwSize - unused */
  0, /* dwObjsize - unused */
  0, /* dwFlags - unused */
  sizeof(Wine_InternalMouseData),
  WINE_INTERNALMOUSE_NUM_OBJS, /* dwNumObjs */
  Wine_InternalMouseObjectFormat
};

static ICOM_VTABLE(IDirectInputDevice2A) SysMouseAvt;
static ICOM_VTABLE(IDirectInputDevice7A) SysMouse7Avt;
typedef struct SysMouseAImpl SysMouseAImpl;

typedef enum {
  WARP_NEEDED,  /* Warping is needed */
  WARP_STARTED, /* Warping has been done, waiting for the warp event */
  WARP_DONE     /* Warping has been done */
} WARP_STATUS;

struct SysMouseAImpl
{
        /* IDirectInputDevice2AImpl */
        ICOM_VFIELD(IDirectInputDevice2A);
        DWORD                           ref;
        GUID                            guid;

	IDirectInputAImpl *dinput;
	
	/* The current data format and the conversion between internal
	   and external data formats */
	LPDIDATAFORMAT			df;
	DataFormat                     *wine_df;
	int                             offset_array[5];
	
        /* SysMouseAImpl */
        BYTE                            absolute;
        /* Previous position for relative moves */
        LONG				prevX, prevY;
        LPMOUSE_EVENT_PROC		prev_handler;
        HWND				win;
        DWORD				win_centerX, win_centerY;
        LPDIDEVICEOBJECTDATA 		data_queue;
        int				queue_pos, queue_len;
        WARP_STATUS		        need_warp;
        int				acquired;
        HANDLE				hEvent;
	CRITICAL_SECTION		crit;

	/* This is for mouse reporting. */
	Wine_InternalMouseData          m_state;
};

static GUID DInput_Wine_Mouse_GUID = { /* 9e573ed8-7734-11d2-8d4a-23903fb6bdf7 */
  0x9e573ed8,
  0x7734,
  0x11d2,
  {0x8d, 0x4a, 0x23, 0x90, 0x3f, 0xb6, 0xbd, 0xf7}
};

/* FIXME: This is ugly and not thread safe :/ */
static IDirectInputDevice2A* current_lock = NULL;


static BOOL mousedev_enum_device(DWORD dwDevType, DWORD dwFlags, LPCDIDEVICEINSTANCEA lpddi)
{
  if ((dwDevType == 0) || (dwDevType == DIDEVTYPE_MOUSE)) {
    TRACE("Enumerating the Mouse device\n");

    /* Return mouse */
    lpddi->guidInstance = GUID_SysMouse;/* DInput's GUID */
    lpddi->guidProduct = DInput_Wine_Mouse_GUID; /* Vendor's GUID */
    lpddi->dwDevType = DIDEVTYPE_MOUSE | (DIDEVTYPEMOUSE_UNKNOWN << 8);
    strcpy(lpddi->tszInstanceName, "Mouse");
    strcpy(lpddi->tszProductName, "Wine Mouse");

    return TRUE;
  }

  return FALSE;
}

static SysMouseAImpl *alloc_device(REFGUID rguid, ICOM_VTABLE(IDirectInputDevice2A) *mvt, IDirectInputAImpl *dinput)
{
    int offset_array[5] = {
      FIELD_OFFSET(Wine_InternalMouseData, lX),
      FIELD_OFFSET(Wine_InternalMouseData, lY),
      FIELD_OFFSET(Wine_InternalMouseData, rgbButtons) + 0,
      FIELD_OFFSET(Wine_InternalMouseData, rgbButtons) + 1,
      FIELD_OFFSET(Wine_InternalMouseData, rgbButtons) + 2
    };
    SysMouseAImpl* newDevice;
    newDevice = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(SysMouseAImpl));
    newDevice->ref = 1;
    ICOM_VTBL(newDevice) = mvt;
    InitializeCriticalSection(&(newDevice->crit));
    memcpy(&(newDevice->guid),rguid,sizeof(*rguid));
    
    /* Per default, Wine uses its internal data format */
    newDevice->df = &Wine_InternalMouseFormat;
    memcpy(newDevice->offset_array, offset_array, 5 * sizeof(int));
    newDevice->wine_df = (DataFormat *) HeapAlloc(GetProcessHeap(), 0, sizeof(DataFormat));
    newDevice->wine_df->size = 0;
    newDevice->wine_df->internal_format_size = Wine_InternalMouseFormat.dwDataSize;
    newDevice->wine_df->dt = NULL;
    newDevice->dinput = dinput;

    return newDevice;
}

static HRESULT mousedev_create_device(IDirectInputAImpl *dinput, REFGUID rguid, REFIID riid, LPDIRECTINPUTDEVICEA* pdev)
{
  if ((IsEqualGUID(&GUID_SysMouse,rguid)) ||             /* Generic Mouse */
      (IsEqualGUID(&DInput_Wine_Mouse_GUID,rguid))) { /* Wine Mouse */
    if ((riid == NULL) || (IsEqualGUID(&IID_IDirectInputDevice2A,riid)) || (IsEqualGUID(&IID_IDirectInputDevice2A,riid))) {
      *pdev=(IDirectInputDeviceA*) alloc_device(rguid, &SysMouseAvt, dinput);
    
      TRACE("Creating a Mouse device (%p)\n", *pdev);
      return DI_OK;
    }else if (IsEqualGUID(&IID_IDirectInputDevice7A,riid)) {
      *pdev=(IDirectInputDeviceA*) alloc_device(rguid, (ICOM_VTABLE(IDirectInputDevice2A) *) &SysMouse7Avt, dinput);
    
      TRACE("Creating a Mouse DInput7A device (%p)\n", *pdev);
      return DI_OK;
    } else
      return DIERR_NOINTERFACE;
  }

  return DIERR_DEVICENOTREG;
}

static dinput_device mousedev = {
  100,
  mousedev_enum_device,
  mousedev_create_device
};

DECL_GLOBAL_CONSTRUCTOR(mousedev_register) { dinput_register_device(&mousedev); }

/******************************************************************************
 *	SysMouseA (DInput Mouse support)
 */

/******************************************************************************
  *     Release : release the mouse buffer.
  */
static ULONG WINAPI SysMouseAImpl_Release(LPDIRECTINPUTDEVICE2A iface)
{
	ICOM_THIS(SysMouseAImpl,iface);

	This->ref--;
	if (This->ref)
		return This->ref;

	/* Free the data queue */
	if (This->data_queue != NULL)
	  HeapFree(GetProcessHeap(),0,This->data_queue);

	/* Install the previous event handler (in case of releasing an acquired
	   mouse device) */
	if (This->prev_handler != NULL)
	  MOUSE_Enable(This->prev_handler);
	DeleteCriticalSection(&(This->crit));

	/* Free the DataFormat */
	if (This->df != &(Wine_InternalMouseFormat)) {
	  HeapFree(GetProcessHeap(), 0, This->df->rgodf);
	  HeapFree(GetProcessHeap(), 0, This->df);
	}
	
	HeapFree(GetProcessHeap(),0,This);
	return 0;
}


/******************************************************************************
  *     SetCooperativeLevel : store the window in which we will do our
  *   grabbing.
  */
static HRESULT WINAPI SysMouseAImpl_SetCooperativeLevel(
	LPDIRECTINPUTDEVICE2A iface,HWND hwnd,DWORD dwflags
)
{
  ICOM_THIS(SysMouseAImpl,iface);

  TRACE("(this=%p,0x%08lx,0x%08lx)\n",This,(DWORD)hwnd,dwflags);

  if (TRACE_ON(dinput))
    _dump_cooperativelevel_DI(dwflags);

  /* Store the window which asks for the mouse */
  This->win = hwnd;
  
  return 0;
}


/******************************************************************************
  *     SetDataFormat : the application can choose the format of the data
  *   the device driver sends back with GetDeviceState.
  *
  *   For the moment, only the "standard" configuration (c_dfDIMouse) is supported
  *   in absolute and relative mode.
  */
static HRESULT WINAPI SysMouseAImpl_SetDataFormat(
	LPDIRECTINPUTDEVICE2A iface,LPCDIDATAFORMAT df
)
{
  ICOM_THIS(SysMouseAImpl,iface);
  int i;
  
  TRACE("(this=%p,%p)\n",This,df);

  TRACE("(df.dwSize=%ld)\n",df->dwSize);
  TRACE("(df.dwObjsize=%ld)\n",df->dwObjSize);
  TRACE("(df.dwFlags=0x%08lx)\n",df->dwFlags);
  TRACE("(df.dwDataSize=%ld)\n",df->dwDataSize);
  TRACE("(df.dwNumObjs=%ld)\n",df->dwNumObjs);

  for (i=0;i<df->dwNumObjs;i++) {

    TRACE("df.rgodf[%d].guid %s (%p)\n",i, debugstr_guid(df->rgodf[i].pguid), df->rgodf[i].pguid);
    TRACE("df.rgodf[%d].dwOfs %ld\n",i,df->rgodf[i].dwOfs);
    TRACE("dwType 0x%02x,dwInstance %d\n",DIDFT_GETTYPE(df->rgodf[i].dwType),DIDFT_GETINSTANCE(df->rgodf[i].dwType));
    TRACE("df.rgodf[%d].dwFlags 0x%08lx\n",i,df->rgodf[i].dwFlags);
  }

  /* Check if the mouse is in absolute or relative mode */
  if (df->dwFlags == DIDF_ABSAXIS)
    This->absolute = 1;
  else if (df->dwFlags == DIDF_RELAXIS)
    This->absolute = 0;
  else
    ERR("Neither absolute nor relative flag set.");
  
  /* Store the new data format */
  This->df = HeapAlloc(GetProcessHeap(),0,df->dwSize);
  memcpy(This->df, df, df->dwSize);
  This->df->rgodf = HeapAlloc(GetProcessHeap(),0,df->dwNumObjs*df->dwObjSize);
  memcpy(This->df->rgodf,df->rgodf,df->dwNumObjs*df->dwObjSize);

  /* Prepare all the data-conversion filters */
  This->wine_df = create_DataFormat(&(Wine_InternalMouseFormat), df, This->offset_array);
  
  return 0;
}
  
/* Our private mouse event handler */
static void WINAPI dinput_mouse_event( DWORD dwFlags, DWORD dx, DWORD dy,
				      DWORD cButtons, DWORD dwExtraInfo )
{
  long posX = -1, posY = -1;
  DWORD keyState, xtime, extra;
  SysMouseAImpl* This = (SysMouseAImpl*) current_lock;
  
  EnterCriticalSection(&(This->crit));
  /* Mouse moved -> send event if asked */
  if (This->hEvent)
    SetEvent(This->hEvent);
  
  if (   !IsBadReadPtr( (LPVOID)dwExtraInfo, sizeof(WINE_MOUSEEVENT) )
      && ((WINE_MOUSEEVENT *)dwExtraInfo)->magic == WINE_MOUSEEVENT_MAGIC ) {
    WINE_MOUSEEVENT *wme = (WINE_MOUSEEVENT *)dwExtraInfo;
    keyState = wme->keyState;
    xtime = wme->time;
    extra = (DWORD)wme->hWnd;
    
    if (dwFlags & MOUSEEVENTF_MOVE) {
      if (dwFlags & MOUSEEVENTF_ABSOLUTE) {
	posX = (dx * GetSystemMetrics(SM_CXSCREEN)) >> 16;
	posY = (dy * GetSystemMetrics(SM_CYSCREEN)) >> 16;

	if (This->absolute) {
	  if (posX != This->prevX)
	    GEN_EVENT(This->offset_array[WINE_MOUSE_X_POSITION], posX, xtime, 0);
	  if (posY != This->prevY)
	    GEN_EVENT(This->offset_array[WINE_MOUSE_Y_POSITION], posY, xtime, 0);
	} else {
	  /* Now, warp handling */
	  if ((This->need_warp == WARP_STARTED) &&
	      (posX == This->win_centerX) && (posX == This->win_centerX)) {
	    /* Warp has been done... */
	    This->need_warp = WARP_DONE;
	    LeaveCriticalSection(&(This->crit));
	    return;
	  }
	      	  
	  /* Relative mouse input with absolute mouse event : the real fun starts here... */
	  if ((This->need_warp == WARP_NEEDED) ||
	      (This->need_warp == WARP_STARTED)) {
	    if (posX != This->prevX)
	      GEN_EVENT(This->offset_array[WINE_MOUSE_X_POSITION], posX - This->prevX, xtime, (This->dinput->evsequence)++);
	    if (posY != This->prevY)
	      GEN_EVENT(This->offset_array[WINE_MOUSE_Y_POSITION], posY - This->prevY, xtime, (This->dinput->evsequence)++);
	  } else {
	    /* This is the first time the event handler has been called after a
	       GetData of GetState. */
	    if (posX != This->win_centerX) {
	      GEN_EVENT(This->offset_array[WINE_MOUSE_X_POSITION], posX - This->win_centerX, xtime, (This->dinput->evsequence)++);
	      This->need_warp = WARP_NEEDED;
	    }
	    
	    if (posY != This->win_centerY) {
	      GEN_EVENT(This->offset_array[WINE_MOUSE_Y_POSITION], posY - This->win_centerY, xtime, (This->dinput->evsequence)++);
	      This->need_warp = WARP_NEEDED;
	    }
	  }
	}

	This->prevX = posX;
	This->prevY = posY;
	
	if (This->absolute) {
	  This->m_state.lX = posX;
	  This->m_state.lY = posY;
	} else {
	  This->m_state.lX = posX - This->win_centerX;
	  This->m_state.lY = posY - This->win_centerY;
	}
      } else {
	/* Mouse reporting is in relative mode */
	posX = (long) dx;
	posY = (long) dy;

	if (This->absolute) {
	  long aposX, aposY;
	  
	  aposX = This->m_state.lX + posX;
	  if (aposX < 0)
	    aposX = 0;
	  if (aposX >= GetSystemMetrics(SM_CXSCREEN))
	    aposX = GetSystemMetrics(SM_CXSCREEN);
	  
	  aposY = This->m_state.lY + posY;
	  if (aposY < 0)
	    aposY = 0;
	  if (aposY >= GetSystemMetrics(SM_CYSCREEN))
	    aposY = GetSystemMetrics(SM_CYSCREEN);
	  
	  if (posX != 0)
	    GEN_EVENT(This->offset_array[WINE_MOUSE_X_POSITION], aposX, xtime, (This->dinput->evsequence)++);
	  if (posY != 0)
	    GEN_EVENT(This->offset_array[WINE_MOUSE_Y_POSITION], aposY, xtime, (This->dinput->evsequence)++);

	  This->m_state.lX = aposX;
	  This->m_state.lY = aposY;
	} else {
	  if (posX != 0)
	    GEN_EVENT(This->offset_array[WINE_MOUSE_X_POSITION], posX, xtime, (This->dinput->evsequence)++);
	  if (posY != 0)
	    GEN_EVENT(This->offset_array[WINE_MOUSE_Y_POSITION], posY, xtime, (This->dinput->evsequence)++);

	  This->m_state.lX = posX;
	  This->m_state.lY = posY;
	}
      }
    }
  } else {
    ERR("Mouse event not supported...\n");
    LeaveCriticalSection(&(This->crit));
    return ;
  }

  if (TRACE_ON(dinput)) {
    if (dwFlags & MOUSEEVENTF_MOVE)
      TRACE(" %ld %ld (%s)", posX, posY,
	    (dwFlags & MOUSEEVENTF_ABSOLUTE ? "abs" : "rel"));
    
    if ( dwFlags & MOUSEEVENTF_LEFTDOWN ) DPRINTF(" LD ");
    if ( dwFlags & MOUSEEVENTF_LEFTUP )   DPRINTF(" LU ");

    if ( dwFlags & MOUSEEVENTF_RIGHTDOWN ) DPRINTF(" RD ");
    if ( dwFlags & MOUSEEVENTF_RIGHTUP )   DPRINTF(" RU ");

    if ( dwFlags & MOUSEEVENTF_MIDDLEDOWN ) DPRINTF(" MD ");
    if ( dwFlags & MOUSEEVENTF_MIDDLEUP )   DPRINTF(" MU ");

    if (!(This->absolute)) DPRINTF(" W=%d ", This->need_warp);
    
    DPRINTF("\n");
  }

  
  if ( dwFlags & MOUSEEVENTF_LEFTDOWN ) {
    GEN_EVENT(This->offset_array[WINE_MOUSE_L_POSITION], 0xFF, xtime, (This->dinput->evsequence)++);
    This->m_state.rgbButtons[0] = 0xFF;
  }
  if ( dwFlags & MOUSEEVENTF_LEFTUP ) {
    GEN_EVENT(This->offset_array[WINE_MOUSE_L_POSITION], 0x00, xtime, (This->dinput->evsequence)++);
    This->m_state.rgbButtons[0] = 0x00;
  }
  if ( dwFlags & MOUSEEVENTF_RIGHTDOWN ) {
    GEN_EVENT(This->offset_array[WINE_MOUSE_R_POSITION], 0xFF, xtime, (This->dinput->evsequence)++);
    This->m_state.rgbButtons[1] = 0xFF;
  }
  if ( dwFlags & MOUSEEVENTF_RIGHTUP ) {
    GEN_EVENT(This->offset_array[WINE_MOUSE_R_POSITION], 0x00, xtime, (This->dinput->evsequence)++);
    This->m_state.rgbButtons[1] = 0x00;
  }
  if ( dwFlags & MOUSEEVENTF_MIDDLEDOWN ) {
    GEN_EVENT(This->offset_array[WINE_MOUSE_M_POSITION], 0xFF, xtime, (This->dinput->evsequence)++);
    This->m_state.rgbButtons[2] = 0xFF;
  }
  if ( dwFlags & MOUSEEVENTF_MIDDLEUP ) {
    GEN_EVENT(This->offset_array[WINE_MOUSE_M_POSITION], 0x00, xtime, (This->dinput->evsequence)++);
    This->m_state.rgbButtons[2] = 0x00;
  }

  TRACE("(X: %ld - Y: %ld   L: %02x M: %02x R: %02x)\n",
	This->m_state.lX, This->m_state.lY,
	This->m_state.rgbButtons[0], This->m_state.rgbButtons[2], This->m_state.rgbButtons[1]);
  
  LeaveCriticalSection(&(This->crit));
}


/******************************************************************************
  *     Acquire : gets exclusive control of the mouse
  */
static HRESULT WINAPI SysMouseAImpl_Acquire(LPDIRECTINPUTDEVICE2A iface)
{
  ICOM_THIS(SysMouseAImpl,iface);
  RECT	rect;
  
  TRACE("(this=%p)\n",This);

  if (This->acquired == 0) {
    POINT       point;
    
    /* This stores the current mouse handler. */
    This->prev_handler = mouse_event;
    
    /* Store (in a global variable) the current lock */
    current_lock = (IDirectInputDevice2A*)This;

    /* Init the mouse state */
    if (This->absolute) {
      GetCursorPos( &point );
      This->m_state.lX = point.x;
      This->m_state.lY = point.y;
      This->prevX = point.x;
      This->prevY = point.y;
    } else {
      This->m_state.lX = 0;
      This->m_state.lY = 0;
    }
    This->m_state.rgbButtons[0] = (GetKeyState(VK_LBUTTON) ? 0xFF : 0x00);
    This->m_state.rgbButtons[1] = (GetKeyState(VK_MBUTTON) ? 0xFF : 0x00);
    This->m_state.rgbButtons[2] = (GetKeyState(VK_RBUTTON) ? 0xFF : 0x00);

    /* Install our own mouse event handler */
    MOUSE_Enable(dinput_mouse_event);
    
    /* Get the window dimension and find the center */
    GetWindowRect(This->win, &rect);
    This->win_centerX = (rect.right  - rect.left) / 2;
    This->win_centerY = (rect.bottom - rect.top ) / 2;

    /* Warp the mouse to the center of the window */
    if (This->absolute == 0) {
      TRACE("Warping mouse to %ld - %ld\n", This->win_centerX, This->win_centerY);
      point.x = This->win_centerX;
      point.y = This->win_centerY;
      MapWindowPoints(This->win, HWND_DESKTOP, &point, 1);
      SetCursorPos( point.x, point.y );
#ifdef MOUSE_HACK
      This->need_warp = WARP_DONE;
#else
      This->need_warp = WARP_STARTED;
#endif
    }

    This->acquired = 1;
  }
  return DI_OK;
}

/******************************************************************************
  *     Unacquire : frees the mouse
  */
static HRESULT WINAPI SysMouseAImpl_Unacquire(LPDIRECTINPUTDEVICE2A iface)
{
    ICOM_THIS(SysMouseAImpl,iface);

    TRACE("(this=%p)\n",This);

    if (This->acquired)
    {
        /* Reinstall previous mouse event handler */
        MOUSE_Enable(This->prev_handler);
        This->prev_handler = NULL;
  
        /* No more locks */
        current_lock = NULL;

        /* Unacquire device */
        This->acquired = 0;
    }
    else
	ERR("Unacquiring a not-acquired device !!!\n");
  
    return DI_OK;
}

/******************************************************************************
  *     GetDeviceState : returns the "state" of the mouse.
  *
  *   For the moment, only the "standard" return structure (DIMOUSESTATE) is
  *   supported.
  */
static HRESULT WINAPI SysMouseAImpl_GetDeviceState(
	LPDIRECTINPUTDEVICE2A iface,DWORD len,LPVOID ptr
) {
  ICOM_THIS(SysMouseAImpl,iface);
  
  EnterCriticalSection(&(This->crit));
  TRACE("(this=%p,0x%08lx,%p): \n",This,len,ptr);
  
  /* Copy the current mouse state */
  fill_DataFormat(ptr, &(This->m_state), This->wine_df);
  
  /* Initialize the buffer when in relative mode */
  if (This->absolute == 0) {
    This->m_state.lX = 0;
    This->m_state.lY = 0;
  }
  
  /* Check if we need to do a mouse warping */
  if (This->need_warp == WARP_NEEDED) {
    POINT point;

    TRACE("Warping mouse to %ld - %ld\n", This->win_centerX, This->win_centerY);
    point.x = This->win_centerX;
    point.y = This->win_centerY;
    MapWindowPoints(This->win, HWND_DESKTOP, &point, 1);
    SetCursorPos( point.x, point.y );

#ifdef MOUSE_HACK
    This->need_warp = WARP_DONE;
#else
    This->need_warp = WARP_STARTED;
#endif
  }

  LeaveCriticalSection(&(This->crit));
  
  TRACE("(X: %ld - Y: %ld   L: %02x M: %02x R: %02x)\n",
	This->m_state.lX, This->m_state.lY,
	This->m_state.rgbButtons[0], This->m_state.rgbButtons[2], This->m_state.rgbButtons[1]);
  
  return 0;
}

/******************************************************************************
  *     GetDeviceState : gets buffered input data.
  */
static HRESULT WINAPI SysMouseAImpl_GetDeviceData(LPDIRECTINPUTDEVICE2A iface,
					      DWORD dodsize,
					      LPDIDEVICEOBJECTDATA dod,
					      LPDWORD entries,
					      DWORD flags
) {
  ICOM_THIS(SysMouseAImpl,iface);
  
  EnterCriticalSection(&(This->crit));
  TRACE("(%p)->(dods=%ld,entries=%ld,fl=0x%08lx)\n",This,dodsize,*entries,flags);

  if (flags & DIGDD_PEEK)
    FIXME("DIGDD_PEEK\n");

  if (dod == NULL) {
    *entries = This->queue_pos;
    This->queue_pos = 0;
  } else {
    /* Check for buffer overflow */
    if (This->queue_pos > *entries) {
      WARN("Buffer overflow not handled properly yet...\n");
      This->queue_pos = *entries;
    }
    if (dodsize != sizeof(DIDEVICEOBJECTDATA)) {
      ERR("Wrong structure size !\n");
      LeaveCriticalSection(&(This->crit));
      return DIERR_INVALIDPARAM;
    }

    if (This->queue_pos)
    	TRACE("Application retrieving %d event(s).\n", This->queue_pos); 
    
    /* Copy the buffered data into the application queue */
    memcpy(dod, This->data_queue, This->queue_pos * dodsize);
    *entries = This->queue_pos;

    /* Reset the event queue */
    This->queue_pos = 0;
  }
  LeaveCriticalSection(&(This->crit));
  
  /* Check if we need to do a mouse warping */
  if (This->need_warp == WARP_NEEDED) {
    POINT point;

    TRACE("Warping mouse to %ld - %ld\n", This->win_centerX, This->win_centerY);
    point.x = This->win_centerX;
    point.y = This->win_centerY;
    MapWindowPoints(This->win, HWND_DESKTOP, &point, 1);
    SetCursorPos( point.x, point.y );

#ifdef MOUSE_HACK
    This->need_warp = WARP_DONE;
#else
    This->need_warp = WARP_STARTED;
#endif
  }
  return 0;
}

/******************************************************************************
  *     SetProperty : change input device properties
  */
static HRESULT WINAPI SysMouseAImpl_SetProperty(LPDIRECTINPUTDEVICE2A iface,
					    REFGUID rguid,
					    LPCDIPROPHEADER ph)
{
  ICOM_THIS(SysMouseAImpl,iface);

  TRACE("(this=%p,%s,%p)\n",This,debugstr_guid(rguid),ph);
  
  if (!HIWORD(rguid)) {
    switch ((DWORD)rguid) {
    case (DWORD) DIPROP_BUFFERSIZE: {
      LPCDIPROPDWORD	pd = (LPCDIPROPDWORD)ph;
      
      TRACE("buffersize = %ld\n",pd->dwData);

      This->data_queue = (LPDIDEVICEOBJECTDATA)HeapAlloc(GetProcessHeap(),0,
							  pd->dwData * sizeof(DIDEVICEOBJECTDATA));
      This->queue_pos  = 0;
      This->queue_len  = pd->dwData;
      break;
    }
    case (DWORD) DIPROP_AXISMODE: {
      LPCDIPROPDWORD    pd = (LPCDIPROPDWORD)ph;
      This->absolute = !(pd->dwData);
      TRACE("absolute mode: %d\n", This->absolute);
      break;
    }
    default:
      FIXME("Unknown type %ld (%s)\n",(DWORD)rguid,debugstr_guid(rguid));
      break;
    }
  }
  
  return 0;
}

/******************************************************************************
  *     GetProperty : get input device properties
  */
static HRESULT WINAPI SysMouseAImpl_GetProperty(LPDIRECTINPUTDEVICE2A iface,
						REFGUID rguid,
						LPDIPROPHEADER pdiph)
{
  ICOM_THIS(SysMouseAImpl,iface);

  TRACE("(this=%p,%s,%p): stub!\n",
	iface, debugstr_guid(rguid), pdiph);

  if (TRACE_ON(dinput))
    _dump_DIPROPHEADER(pdiph);
  
  if (!HIWORD(rguid)) {
    switch ((DWORD)rguid) {
    case (DWORD) DIPROP_BUFFERSIZE: {
      LPDIPROPDWORD	pd = (LPDIPROPDWORD)pdiph;
      
      TRACE(" return buffersize = %d\n",This->queue_len);
      pd->dwData = This->queue_len;
      break;
    }

    case (DWORD) DIPROP_RANGE: {
      LPDIPROPRANGE pr = (LPDIPROPRANGE) pdiph;

      if ((pdiph->dwHow == DIPH_BYID) &&
	  ((pdiph->dwObj == (DIDFT_MAKEINSTANCE(WINE_MOUSE_X_AXIS_INSTANCE) | DIDFT_RELAXIS)) ||
	   (pdiph->dwObj == (DIDFT_MAKEINSTANCE(WINE_MOUSE_Y_AXIS_INSTANCE) | DIDFT_RELAXIS)))) {
	/* Querying the range of either the X or the Y axis.  As I do
	   not know the range, do as if the range where
	   unrestricted...*/
	pr->lMin = DIPROPRANGE_NOMIN;
	pr->lMax = DIPROPRANGE_NOMAX;
      }
      
      break;
    }
      
    default:
      FIXME("Unknown type %ld (%s)\n",(DWORD)rguid,debugstr_guid(rguid));
      break;
    }
  }
  
  
  return DI_OK;
}



/******************************************************************************
  *     SetEventNotification : specifies event to be sent on state change
  */
static HRESULT WINAPI SysMouseAImpl_SetEventNotification(LPDIRECTINPUTDEVICE2A iface,
							 HANDLE hnd) {
  ICOM_THIS(SysMouseAImpl,iface);

  TRACE("(this=%p,0x%08lx)\n",This,(DWORD)hnd);

  This->hEvent = hnd;

  return DI_OK;
}

/******************************************************************************
  *     GetCapabilities : get the device capablitites
  */
static HRESULT WINAPI SysMouseAImpl_GetCapabilities(
	LPDIRECTINPUTDEVICE2A iface,
	LPDIDEVCAPS lpDIDevCaps)
{
  ICOM_THIS(SysMouseAImpl,iface);

  TRACE("(this=%p,%p)\n",This,lpDIDevCaps);

  if (lpDIDevCaps->dwSize == sizeof(DIDEVCAPS)) {
    lpDIDevCaps->dwFlags = DIDC_ATTACHED;
    lpDIDevCaps->dwDevType = DIDEVTYPE_MOUSE;
    lpDIDevCaps->dwAxes = 2;
    lpDIDevCaps->dwButtons = 3;
    lpDIDevCaps->dwPOVs = 0;
    lpDIDevCaps->dwFFSamplePeriod = 0;
    lpDIDevCaps->dwFFMinTimeResolution = 0;
    lpDIDevCaps->dwFirmwareRevision = 100;
    lpDIDevCaps->dwHardwareRevision = 100;
    lpDIDevCaps->dwFFDriverVersion = 0;
  } else {
    /* DirectX 3.0 */
    FIXME("DirectX 3.0 not supported....\n");
  }
  
  return DI_OK;
}


/******************************************************************************
  *     EnumObjects : enumerate the different buttons and axis...
  */
static HRESULT WINAPI SysMouseAImpl_EnumObjects(
	LPDIRECTINPUTDEVICE2A iface,
	LPDIENUMDEVICEOBJECTSCALLBACKA lpCallback,
	LPVOID lpvRef,
	DWORD dwFlags)
{
  ICOM_THIS(SysMouseAImpl,iface);
  DIDEVICEOBJECTINSTANCEA ddoi;
  
  TRACE("(this=%p,%p,%p,%08lx)\n", This, lpCallback, lpvRef, dwFlags);
  if (TRACE_ON(dinput)) {
    DPRINTF("  - flags = ");
    _dump_EnumObjects_flags(dwFlags);
    DPRINTF("\n");
  }

  /* Only the fields till dwFFMaxForce are relevant */
  ddoi.dwSize = FIELD_OFFSET(DIDEVICEOBJECTINSTANCEA, dwFFMaxForce);
    
  /* In a mouse, we have : two relative axis and three buttons */
  if ((dwFlags == DIDFT_ALL) ||
      (dwFlags & DIDFT_AXIS)) {
    /* X axis */
    ddoi.guidType = GUID_XAxis;
    ddoi.dwOfs = This->offset_array[WINE_MOUSE_X_POSITION];
    ddoi.dwType = DIDFT_MAKEINSTANCE(WINE_MOUSE_X_AXIS_INSTANCE) | DIDFT_RELAXIS;
    strcpy(ddoi.tszName, "X-Axis");
    _dump_OBJECTINSTANCEA(&ddoi);
    if (lpCallback(&ddoi, lpvRef) != DIENUM_CONTINUE) return DI_OK;
    
    /* Y axis */
    ddoi.guidType = GUID_YAxis;
    ddoi.dwOfs = This->offset_array[WINE_MOUSE_Y_POSITION];
    ddoi.dwType = DIDFT_MAKEINSTANCE(WINE_MOUSE_Y_AXIS_INSTANCE) | DIDFT_RELAXIS;
    strcpy(ddoi.tszName, "Y-Axis");
    _dump_OBJECTINSTANCEA(&ddoi);
    if (lpCallback(&ddoi, lpvRef) != DIENUM_CONTINUE) return DI_OK;
  }

  if ((dwFlags == DIDFT_ALL) ||
      (dwFlags & DIDFT_BUTTON)) {
    ddoi.guidType = GUID_Button;

    /* Left button */
    ddoi.dwOfs = This->offset_array[WINE_MOUSE_L_POSITION];
    ddoi.dwType = DIDFT_MAKEINSTANCE(WINE_MOUSE_L_BUTTON_INSTANCE) | DIDFT_PSHBUTTON;
    strcpy(ddoi.tszName, "Left-Button");
    _dump_OBJECTINSTANCEA(&ddoi);
    if (lpCallback(&ddoi, lpvRef) != DIENUM_CONTINUE) return DI_OK;

    /* Right button */
    ddoi.dwOfs = This->offset_array[WINE_MOUSE_R_POSITION];
    ddoi.dwType = DIDFT_MAKEINSTANCE(WINE_MOUSE_R_BUTTON_INSTANCE) | DIDFT_PSHBUTTON;
    strcpy(ddoi.tszName, "Right-Button");
    _dump_OBJECTINSTANCEA(&ddoi);
    if (lpCallback(&ddoi, lpvRef) != DIENUM_CONTINUE) return DI_OK;

    /* Middle button */
    ddoi.dwOfs = This->offset_array[WINE_MOUSE_M_POSITION];
    ddoi.dwType = DIDFT_MAKEINSTANCE(WINE_MOUSE_M_BUTTON_INSTANCE) | DIDFT_PSHBUTTON;
    strcpy(ddoi.tszName, "Middle-Button");
    _dump_OBJECTINSTANCEA(&ddoi);
    if (lpCallback(&ddoi, lpvRef) != DIENUM_CONTINUE) return DI_OK;
  }

  return DI_OK;
}


static ICOM_VTABLE(IDirectInputDevice2A) SysMouseAvt = 
{
	ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
	IDirectInputDevice2AImpl_QueryInterface,
	IDirectInputDevice2AImpl_AddRef,
	SysMouseAImpl_Release,
	SysMouseAImpl_GetCapabilities,
	SysMouseAImpl_EnumObjects,
	SysMouseAImpl_GetProperty,
	SysMouseAImpl_SetProperty,
	SysMouseAImpl_Acquire,
	SysMouseAImpl_Unacquire,
	SysMouseAImpl_GetDeviceState,
	SysMouseAImpl_GetDeviceData,
	SysMouseAImpl_SetDataFormat,
	SysMouseAImpl_SetEventNotification,
	SysMouseAImpl_SetCooperativeLevel,
	IDirectInputDevice2AImpl_GetObjectInfo,
	IDirectInputDevice2AImpl_GetDeviceInfo,
	IDirectInputDevice2AImpl_RunControlPanel,
	IDirectInputDevice2AImpl_Initialize,
	IDirectInputDevice2AImpl_CreateEffect,
	IDirectInputDevice2AImpl_EnumEffects,
	IDirectInputDevice2AImpl_GetEffectInfo,
	IDirectInputDevice2AImpl_GetForceFeedbackState,
	IDirectInputDevice2AImpl_SendForceFeedbackCommand,
	IDirectInputDevice2AImpl_EnumCreatedEffectObjects,
	IDirectInputDevice2AImpl_Escape,
	IDirectInputDevice2AImpl_Poll,
	IDirectInputDevice2AImpl_SendDeviceData,
};

#if !defined(__STRICT_ANSI__) && defined(__GNUC__)
# define XCAST(fun)	(typeof(SysMouse7Avt.fn##fun))
#else
# define XCAST(fun)	(void*)
#endif

static ICOM_VTABLE(IDirectInputDevice7A) SysMouse7Avt = 
{
	ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
	XCAST(QueryInterface)IDirectInputDevice2AImpl_QueryInterface,
	XCAST(AddRef)IDirectInputDevice2AImpl_AddRef,
	XCAST(Release)SysMouseAImpl_Release,
	XCAST(GetCapabilities)SysMouseAImpl_GetCapabilities,
	XCAST(EnumObjects)SysMouseAImpl_EnumObjects,
	XCAST(GetProperty)SysMouseAImpl_GetProperty,
	XCAST(SetProperty)SysMouseAImpl_SetProperty,
	XCAST(Acquire)SysMouseAImpl_Acquire,
	XCAST(Unacquire)SysMouseAImpl_Unacquire,
	XCAST(GetDeviceState)SysMouseAImpl_GetDeviceState,
	XCAST(GetDeviceData)SysMouseAImpl_GetDeviceData,
	XCAST(SetDataFormat)SysMouseAImpl_SetDataFormat,
	XCAST(SetEventNotification)SysMouseAImpl_SetEventNotification,
	XCAST(SetCooperativeLevel)SysMouseAImpl_SetCooperativeLevel,
	XCAST(GetObjectInfo)IDirectInputDevice2AImpl_GetObjectInfo,
	XCAST(GetDeviceInfo)IDirectInputDevice2AImpl_GetDeviceInfo,
	XCAST(RunControlPanel)IDirectInputDevice2AImpl_RunControlPanel,
	XCAST(Initialize)IDirectInputDevice2AImpl_Initialize,
	XCAST(CreateEffect)IDirectInputDevice2AImpl_CreateEffect,
	XCAST(EnumEffects)IDirectInputDevice2AImpl_EnumEffects,
	XCAST(GetEffectInfo)IDirectInputDevice2AImpl_GetEffectInfo,
	XCAST(GetForceFeedbackState)IDirectInputDevice2AImpl_GetForceFeedbackState,
	XCAST(SendForceFeedbackCommand)IDirectInputDevice2AImpl_SendForceFeedbackCommand,
	XCAST(EnumCreatedEffectObjects)IDirectInputDevice2AImpl_EnumCreatedEffectObjects,
	XCAST(Escape)IDirectInputDevice2AImpl_Escape,
	XCAST(Poll)IDirectInputDevice2AImpl_Poll,
	XCAST(SendDeviceData)IDirectInputDevice2AImpl_SendDeviceData,
	IDirectInputDevice7AImpl_EnumEffectsInFile,
	IDirectInputDevice7AImpl_WriteEffectToFile
};

#undef XCAST
