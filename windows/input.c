/*
 * USER Input processing
 *
 * Copyright 1993 Bob Amstadt
 * Copyright 1996 Albrecht Kleine 
 * Copyright 1997 David Faure
 * Copyright 1998 Morten Welinder
 * Copyright 1998 Ulrich Weigand
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>

#include "windef.h"
#include "winnls.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/winbase16.h"
#include "wine/winuser16.h"
#include "wine/keyboard16.h"
#include "win.h"
#include "heap.h"
#include "input.h"
#include "keyboard.h"
#include "mouse.h"
#include "message.h"
#include "queue.h"
#include "debugtools.h"
#include "winerror.h"
#include "task.h"

DECLARE_DEBUG_CHANNEL(event);
DECLARE_DEBUG_CHANNEL(key);
DECLARE_DEBUG_CHANNEL(keyboard);
DECLARE_DEBUG_CHANNEL(win);

static BOOL InputEnabled = TRUE;
static BOOL SwappedButtons;

BOOL MouseButtonsStates[3];
BOOL AsyncMouseButtonsStates[3];
BYTE InputKeyStateTable[256];
BYTE QueueKeyStateTable[256];
BYTE AsyncKeyStateTable[256];

/* Storage for the USER-maintained mouse positions */
static DWORD PosX, PosY;

#define GET_KEYSTATE() \
     ((MouseButtonsStates[SwappedButtons ? 2 : 0]  ? MK_LBUTTON : 0) | \
      (MouseButtonsStates[1]                       ? MK_RBUTTON : 0) | \
      (MouseButtonsStates[SwappedButtons ? 0 : 2]  ? MK_MBUTTON : 0) | \
      (InputKeyStateTable[VK_SHIFT]   & 0x80       ? MK_SHIFT   : 0) | \
      (InputKeyStateTable[VK_CONTROL] & 0x80       ? MK_CONTROL : 0))

typedef union
{
    struct
    {
	unsigned long count : 16;
	unsigned long code : 8;
	unsigned long extended : 1;
	unsigned long unused : 2;
	unsigned long win_internal : 2;
	unsigned long context : 1;
	unsigned long previous : 1;
	unsigned long transition : 1;
    } lp1;
    unsigned long lp2;
} KEYLP;

/***********************************************************************
 *           keybd_event   (USER32.583)
 */
void WINAPI keybd_event( BYTE bVk, BYTE bScan,
                         DWORD dwFlags, DWORD dwExtraInfo )
{
    DWORD time, extra;
    WORD message;
    KEYLP keylp;
    keylp.lp2 = 0;

    if (!InputEnabled) return;

    /*
     * If we are called by the Wine keyboard driver, use the additional
     * info pointed to by the dwExtraInfo argument.
     * Otherwise, we need to determine that info ourselves (probably
     * less accurate, but we can't help that ...).
     */
    if (   !IsBadReadPtr( (LPVOID)dwExtraInfo, sizeof(WINE_KEYBDEVENT) )
        && ((WINE_KEYBDEVENT *)dwExtraInfo)->magic == WINE_KEYBDEVENT_MAGIC )
    {
        WINE_KEYBDEVENT *wke = (WINE_KEYBDEVENT *)dwExtraInfo;
        time = wke->time;
        extra = 0;
    }
    else
    {
        time = GetTickCount();
        extra = dwExtraInfo;
    }


    keylp.lp1.count = 1;
    keylp.lp1.code = bScan;
    keylp.lp1.extended = (dwFlags & KEYEVENTF_EXTENDEDKEY) != 0;
    keylp.lp1.win_internal = 0; /* this has something to do with dialogs,
                                * don't remember where I read it - AK */
                                /* it's '1' under windows, when a dialog box appears
                                 * and you press one of the underlined keys - DF*/

    if ( dwFlags & KEYEVENTF_KEYUP )
    {
        BOOL sysKey = (InputKeyStateTable[VK_MENU] & 0x80)
                && !(InputKeyStateTable[VK_CONTROL] & 0x80)
                && !(dwFlags & KEYEVENTF_WINE_FORCEEXTENDED); /* for Alt from AltGr */

        InputKeyStateTable[bVk] &= ~0x80;
        keylp.lp1.previous = 1;
        keylp.lp1.transition = 1;
        message = sysKey ? WM_SYSKEYUP : WM_KEYUP;
    }
    else
    {
        keylp.lp1.previous = (InputKeyStateTable[bVk] & 0x80) != 0;
        keylp.lp1.transition = 0;

        if (!(InputKeyStateTable[bVk] & 0x80))
            InputKeyStateTable[bVk] ^= 0x01;
        InputKeyStateTable[bVk] |= 0x80;

        message = (InputKeyStateTable[VK_MENU] & 0x80)
              && !(InputKeyStateTable[VK_CONTROL] & 0x80)
              ? WM_SYSKEYDOWN : WM_KEYDOWN;
    }

    if ( message == WM_SYSKEYDOWN || message == WM_SYSKEYUP )
        keylp.lp1.context = (InputKeyStateTable[VK_MENU] & 0x80) != 0; /* 1 if alt */


    TRACE_(key)("            wParam=%04X, lParam=%08lX\n", bVk, keylp.lp2 );
    TRACE_(key)("            InputKeyState=%X\n", InputKeyStateTable[bVk] );

    hardware_event( message, bVk, keylp.lp2, PosX, PosY, time, extra );
}

/***********************************************************************
 *           WIN16_keybd_event   (USER.289)
 */
void WINAPI WIN16_keybd_event( CONTEXT86 *context )
{
    DWORD dwFlags = 0;

    if (AH_reg(context) & 0x80) dwFlags |= KEYEVENTF_KEYUP;
    if (BH_reg(context) & 1   ) dwFlags |= KEYEVENTF_EXTENDEDKEY;

    keybd_event( AL_reg(context), BL_reg(context),
                 dwFlags, MAKELONG(SI_reg(context), DI_reg(context)) );
}

/***********************************************************************
 *           mouse_event   (USER32.584)
 */
void WINAPI mouse_event( DWORD dwFlags, DWORD dx, DWORD dy,
                         DWORD cButtons, DWORD dwExtraInfo )
{
    DWORD time, extra;
    DWORD keyState;
    
    if (!InputEnabled) return;

    if ( dwFlags & MOUSEEVENTF_MOVE )
      {
	if ( dwFlags & MOUSEEVENTF_ABSOLUTE )
	  {
	    PosX = (dx * GetSystemMetrics(SM_CXSCREEN)) >> 16;
	    PosY = (dy * GetSystemMetrics(SM_CYSCREEN)) >> 16;
	  }
	else
	  {
	    int width  = GetSystemMetrics(SM_CXSCREEN);
	    int height = GetSystemMetrics(SM_CYSCREEN);
	    long posX = (long) PosX, posY = (long) PosY;

	    /* dx and dy can be negative numbers for relative movements */
	    posX += (long) dx;
	    posY += (long) dy;

	    /* Clip to the current screen size */
	    if (posX < 0) PosX = 0;
	    else if (posX >= width) PosX = width - 1;
	    else PosX = posX;

	    if (posY < 0) PosY = 0;
	    else if (posY >= height) PosY = height - 1;
	    else PosY = posY;
	  }
      }

    /*
     * If we are called by the Wine mouse driver, use the additional
     * info pointed to by the dwExtraInfo argument.
     * Otherwise, we need to determine that info ourselves (probably
     * less accurate, but we can't help that ...).
     */
    if (dwExtraInfo && !IsBadReadPtr( (LPVOID)dwExtraInfo, sizeof(WINE_MOUSEEVENT) )
        && ((WINE_MOUSEEVENT *)dwExtraInfo)->magic == WINE_MOUSEEVENT_MAGIC )
    {
        WINE_MOUSEEVENT *wme = (WINE_MOUSEEVENT *)dwExtraInfo;
        time = wme->time;
        extra = (DWORD)wme->hWnd;
	keyState = wme->keyState;
	
	if (keyState != GET_KEYSTATE()) {
	  /* We need to update the keystate with what X provides us */
	  MouseButtonsStates[SwappedButtons ? 2 : 0] = (keyState & MK_LBUTTON ? TRUE : FALSE);
	  MouseButtonsStates[SwappedButtons ? 0 : 2] = (keyState & MK_RBUTTON ? TRUE : FALSE);
	  MouseButtonsStates[1]                      = (keyState & MK_MBUTTON ? TRUE : FALSE);
	  InputKeyStateTable[VK_SHIFT]               = (keyState & MK_SHIFT   ? 0x80 : 0);
	  InputKeyStateTable[VK_CONTROL]             = (keyState & MK_CONTROL ? 0x80 : 0);
	}
    }
    else
    {
        time = GetTickCount();
        extra = dwExtraInfo;
	keyState = GET_KEYSTATE();
	
	if ( dwFlags & MOUSEEVENTF_MOVE )
	  {
	    /* We have to actually move the cursor */
	    SetCursorPos( PosX, PosY );
	  }
    }


    if ( dwFlags & MOUSEEVENTF_MOVE )
    {
        hardware_event( WM_MOUSEMOVE,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & (!SwappedButtons? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN) )
    {
        MouseButtonsStates[0] = AsyncMouseButtonsStates[0] = TRUE;
        hardware_event( WM_LBUTTONDOWN,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & (!SwappedButtons? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP) )
    {
        MouseButtonsStates[0] = FALSE;
        hardware_event( WM_LBUTTONUP,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & (!SwappedButtons? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN) )
    {
        MouseButtonsStates[2] = AsyncMouseButtonsStates[2] = TRUE;
        hardware_event( WM_RBUTTONDOWN,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & (!SwappedButtons? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP) )
    {
        MouseButtonsStates[2] = FALSE;
        hardware_event( WM_RBUTTONUP,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & MOUSEEVENTF_MIDDLEDOWN )
    {
        MouseButtonsStates[1] = AsyncMouseButtonsStates[1] = TRUE;
        hardware_event( WM_MBUTTONDOWN,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & MOUSEEVENTF_MIDDLEUP )
    {
        MouseButtonsStates[1] = FALSE;
        hardware_event( WM_MBUTTONUP,
                        keyState, 0L, PosX, PosY, time, extra );
    }
    if ( dwFlags & MOUSEEVENTF_WHEEL )
    {
        hardware_event( WM_MOUSEWHEEL,
                        keyState, 0L, PosX, PosY, time, extra );
    }
}

/***********************************************************************
 *           WIN16_mouse_event   (USER.299)
 */
void WINAPI WIN16_mouse_event( CONTEXT86 *context )
{
    mouse_event( AX_reg(context), BX_reg(context), CX_reg(context),
                 DX_reg(context), MAKELONG(SI_reg(context), DI_reg(context)) );
}

/***********************************************************************
 *           GetMouseEventProc   (USER.337)
 */
FARPROC16 WINAPI GetMouseEventProc16(void)
{
    HMODULE16 hmodule = GetModuleHandle16("USER");
    return GetProcAddress16( hmodule, "mouse_event" );
}


/**********************************************************************
 *                      EnableHardwareInput   (USER.331)
 */
BOOL16 WINAPI EnableHardwareInput16(BOOL16 bEnable)
{
  BOOL16 bOldState = InputEnabled;
  FIXME_(event)("(%d) - stub\n", bEnable);
  InputEnabled = bEnable;
  return bOldState;
}


/***********************************************************************
 *           SwapMouseButton16   (USER.186)
 */
BOOL16 WINAPI SwapMouseButton16( BOOL16 fSwap )
{
    BOOL16 ret = SwappedButtons;
    SwappedButtons = fSwap;
    return ret;
}


/***********************************************************************
 *           SwapMouseButton   (USER32.537)
 */
BOOL WINAPI SwapMouseButton( BOOL fSwap )
{
    BOOL ret = SwappedButtons;
    SwappedButtons = fSwap;
    return ret;
}


/***********************************************************************
 *           GetCursorPos16    (USER.17)
 */
BOOL16 WINAPI GetCursorPos16( POINT16 *pt )
{
    if (!pt) return 0;
    pt->x = PosX;
    pt->y = PosY;
    return 1;
}


/***********************************************************************
 *           GetCursorPos    (USER32.229)
 */
BOOL WINAPI GetCursorPos( POINT *pt )
{
    if (!pt) return 0;
    pt->x = PosX;
    pt->y = PosY;
    return 1;
}


/**********************************************************************
 *              EVENT_Capture
 *
 * We need this to be able to generate double click messages
 * when menu code captures mouse in the window without CS_DBLCLK style.
 */
HWND EVENT_Capture(HWND hwnd, INT16 ht)
{
    HWND capturePrev = 0, captureWnd = 0;
    MESSAGEQUEUE *pMsgQ = 0, *pCurMsgQ = 0;
    WND* wndPtr = 0;
    INT16 captureHT = 0;

    /* Get the messageQ for the current thread */
    if (!(pCurMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( GetFastQueue16() )))
    {
        WARN_(win)("\tCurrent message queue not found. Exiting!\n" );
        goto CLEANUP;
    }
    
    /* Get the current capture window from the perQ data of the current message Q */
    capturePrev = PERQDATA_GetCaptureWnd( pCurMsgQ->pQData );

    if (!hwnd)
    {
        captureWnd = 0L;
        captureHT = 0;
    }
    else
    {
        wndPtr = WIN_FindWndPtr( hwnd );
        if (wndPtr)
        {
            TRACE_(win)("(0x%04x)\n", hwnd );
            captureWnd   = hwnd;
            captureHT    = ht;
        }
    }

    /* Update the perQ capture window and send messages */
    if( capturePrev != captureWnd )
    {
        if (wndPtr)
        {
            /* Retrieve the message queue associated with this window */
            pMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( wndPtr->hmemTaskQ );
            if ( !pMsgQ )
            {
                WARN_(win)("\tMessage queue not found. Exiting!\n" );
                goto CLEANUP;
            }
    
            /* Make sure that message queue for the window we are setting capture to
             * shares the same perQ data as the current threads message queue.
             */
            if ( pCurMsgQ->pQData != pMsgQ->pQData )
                goto CLEANUP;
        }

        PERQDATA_SetCaptureWnd( pCurMsgQ->pQData, captureWnd );
        PERQDATA_SetCaptureInfo( pCurMsgQ->pQData, captureHT );
        
        if( capturePrev )
    {
        WND* xwndPtr = WIN_FindWndPtr( capturePrev );
        if( xwndPtr && (xwndPtr->flags & WIN_ISWIN32) )
            SendMessageA( capturePrev, WM_CAPTURECHANGED, 0L, hwnd);
	WIN_ReleaseWndPtr(xwndPtr);
    }
}

CLEANUP:
    /* Unlock the queues before returning */
    if ( pMsgQ )
        QUEUE_Unlock( pMsgQ );
    if ( pCurMsgQ )
        QUEUE_Unlock( pCurMsgQ );
    
    WIN_ReleaseWndPtr(wndPtr);
    return capturePrev;
}


/**********************************************************************
 *              SetCapture16   (USER.18)
 */
HWND16 WINAPI SetCapture16( HWND16 hwnd )
{
    return (HWND16)EVENT_Capture( hwnd, HTCLIENT );
}


/**********************************************************************
 *              SetCapture   (USER32.464)
 */
HWND WINAPI SetCapture( HWND hwnd )
{
    return EVENT_Capture( hwnd, HTCLIENT );
}


/**********************************************************************
 *              ReleaseCapture   (USER.19) (USER32.439)
 */
BOOL WINAPI ReleaseCapture(void)
{
    return (EVENT_Capture( 0, 0 ) != 0);
}


/**********************************************************************
 *              GetCapture16   (USER.236)
 */
HWND16 WINAPI GetCapture16(void)
{
    return (HWND16)GetCapture();
}

/**********************************************************************
 *              GetCapture   (USER32.208)
 */
HWND WINAPI GetCapture(void)
{
    MESSAGEQUEUE *pCurMsgQ = 0;
    HWND hwndCapture = 0;

    /* Get the messageQ for the current thread */
    if (!(pCurMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( GetFastQueue16() )))
{
        TRACE_(win)("GetCapture: Current message queue not found. Exiting!\n" );
        return 0;
    }
    
    /* Get the current capture window from the perQ data of the current message Q */
    hwndCapture = PERQDATA_GetCaptureWnd( pCurMsgQ->pQData );

    QUEUE_Unlock( pCurMsgQ );
    return hwndCapture;
}

/**********************************************************************
 *           GetKeyState      (USER.106)
 */
INT16 WINAPI GetKeyState16(INT16 vkey)
{
    return GetKeyState(vkey);
}

/**********************************************************************
 *           GetKeyState      (USER32.249)
 *
 * An application calls the GetKeyState function in response to a
 * keyboard-input message.  This function retrieves the state of the key
 * at the time the input message was generated.  (SDK 3.1 Vol 2. p 390)
 */
SHORT WINAPI GetKeyState(INT vkey)
{
    INT retval;

    switch (vkey)
	{
	case VK_LBUTTON : /* VK_LBUTTON is 1 */
	    retval = MouseButtonsStates[0] ? 0x8000 : 0;
	    break;
	case VK_MBUTTON : /* VK_MBUTTON is 4 */
	    retval = MouseButtonsStates[1] ? 0x8000 : 0;
	    break;
	case VK_RBUTTON : /* VK_RBUTTON is 2 */
	    retval = MouseButtonsStates[2] ? 0x8000 : 0;
	    break;
	default :
	    if (vkey >= 'a' && vkey <= 'z')
		vkey += 'A' - 'a';
	    retval = ( (WORD)(QueueKeyStateTable[vkey] & 0x80) << 8 ) |
		       (WORD)(QueueKeyStateTable[vkey] & 0x01);
	}
    /* TRACE(key, "(0x%x) -> %x\n", vkey, retval); */
    return retval;
}

/**********************************************************************
 *           GetKeyboardState      (USER.222)(USER32.254)
 *
 * An application calls the GetKeyboardState function in response to a
 * keyboard-input message.  This function retrieves the state of the keyboard
 * at the time the input message was generated.  (SDK 3.1 Vol 2. p 387)
 */
BOOL WINAPI GetKeyboardState(LPBYTE lpKeyState)
{
    TRACE_(key)("(%p)\n", lpKeyState);
    if (lpKeyState != NULL) {
	QueueKeyStateTable[VK_LBUTTON] = MouseButtonsStates[0] ? 0x80 : 0;
	QueueKeyStateTable[VK_MBUTTON] = MouseButtonsStates[1] ? 0x80 : 0;
	QueueKeyStateTable[VK_RBUTTON] = MouseButtonsStates[2] ? 0x80 : 0;
	memcpy(lpKeyState, QueueKeyStateTable, 256);
    }

    return TRUE;
}

/**********************************************************************
 *          SetKeyboardState      (USER.223)(USER32.484)
 */
BOOL WINAPI SetKeyboardState(LPBYTE lpKeyState)
{
    TRACE_(key)("(%p)\n", lpKeyState);
    if (lpKeyState != NULL) {
	memcpy(QueueKeyStateTable, lpKeyState, 256);
	MouseButtonsStates[0] = (QueueKeyStateTable[VK_LBUTTON] != 0);
	MouseButtonsStates[1] = (QueueKeyStateTable[VK_MBUTTON] != 0);
	MouseButtonsStates[2] = (QueueKeyStateTable[VK_RBUTTON] != 0);
    }

    return TRUE;
}

/**********************************************************************
 *           GetAsyncKeyState      (USER32.207)
 *
 *	Determine if a key is or was pressed.  retval has high-order 
 * bit set to 1 if currently pressed, low-order bit set to 1 if key has
 * been pressed.
 *
 *	This uses the variable AsyncMouseButtonsStates and
 * AsyncKeyStateTable (set in event.c) which have the mouse button
 * number or key number (whichever is applicable) set to true if the
 * mouse or key had been depressed since the last call to 
 * GetAsyncKeyState.
 */
WORD WINAPI GetAsyncKeyState(INT nKey)
{
    short retval;	

    switch (nKey) {
     case VK_LBUTTON:
	retval = (AsyncMouseButtonsStates[0] ? 0x0001 : 0) | 
                 (MouseButtonsStates[0] ? 0x8000 : 0);
	break;
     case VK_MBUTTON:
	retval = (AsyncMouseButtonsStates[1] ? 0x0001 : 0) | 
                 (MouseButtonsStates[1] ? 0x8000 : 0);
	break;
     case VK_RBUTTON:
	retval = (AsyncMouseButtonsStates[2] ? 0x0001 : 0) | 
                 (MouseButtonsStates[2] ? 0x8000 : 0);
	break;
     default:
	retval = AsyncKeyStateTable[nKey] | 
	  	((InputKeyStateTable[nKey] & 0x80) ? 0x8000 : 0); 
	break;
    }

    /* all states to false */
    memset( AsyncMouseButtonsStates, 0, sizeof(AsyncMouseButtonsStates) );
    memset( AsyncKeyStateTable, 0, sizeof(AsyncKeyStateTable) );

    TRACE_(key)("(%x) -> %x\n", nKey, retval);
    return retval;
}

/**********************************************************************
 *            GetAsyncKeyState16        (USER.249)
 */
WORD WINAPI GetAsyncKeyState16(INT16 nKey)
{
    return GetAsyncKeyState(nKey);
}

/***********************************************************************
 *              IsUserIdle      (USER.333)
 */
BOOL16 WINAPI IsUserIdle16(void)
{
    if ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 )
        return FALSE;

    if ( GetAsyncKeyState( VK_RBUTTON ) & 0x8000 )
        return FALSE;

    if ( GetAsyncKeyState( VK_MBUTTON ) & 0x8000 )
        return FALSE;

    /* Should check for screen saver activation here ... */

    return TRUE;
}

/**********************************************************************
 *           VkKeyScanA      (USER32.573)
 */
WORD WINAPI VkKeyScanA(CHAR cChar)
{
	return VkKeyScan16(cChar);
}

/******************************************************************************
 *    	VkKeyScanW      (USER32.576)
 */
WORD WINAPI VkKeyScanW(WCHAR cChar)
{
	return VkKeyScanA((CHAR)cChar); /* FIXME: check unicode */
}

/**********************************************************************
 *           VkKeyScanExA      (USER32.574)
 */
WORD WINAPI VkKeyScanExA(CHAR cChar, HKL dwhkl)
{
				/* FIXME: complete workaround this is */
				return VkKeyScan16(cChar);
}

/******************************************************************************
 *      VkKeyScanExW      (USER32.575)
 */
WORD WINAPI VkKeyScanExW(WCHAR cChar, HKL dwhkl)
{
				/* FIXME: complete workaround this is */
				return VkKeyScanA((CHAR)cChar); /* FIXME: check unicode */
}
 
/******************************************************************************
 *    	GetKeyboardType      (USER32.255)
 */
INT WINAPI GetKeyboardType(INT nTypeFlag)
{
  return GetKeyboardType16(nTypeFlag);
}

/******************************************************************************
 *    	MapVirtualKeyA      (USER32.383)
 */
UINT WINAPI MapVirtualKeyA(UINT code, UINT maptype)
{
    return MapVirtualKey16(code,maptype);
}

/******************************************************************************
 *    	MapVirtualKeyW      (USER32.385)
 */
UINT WINAPI MapVirtualKeyW(UINT code, UINT maptype)
{
    return MapVirtualKey16(code,maptype);
}

/******************************************************************************
 *    	MapVirtualKeyExA      (USER32.384)
 */
UINT WINAPI MapVirtualKeyExA(UINT code, UINT maptype, HKL hkl)
{
    if (hkl)
    	FIXME_(keyboard)("(%d,%d,0x%08lx), hkl unhandled!\n",code,maptype,(DWORD)hkl);
    return MapVirtualKey16(code,maptype);
}

/******************************************************************************
 *    	MapVirtualKeyExW      (USER32.???)
 */
UINT WINAPI MapVirtualKeyExW(UINT code, UINT maptype, HKL hkl)
{
    if (hkl)
    	FIXME_(keyboard)("(%d,%d,0x%08lx), hkl unhandled!\n",code,maptype,(DWORD)hkl);
    return MapVirtualKey16(code,maptype);
}

/****************************************************************************
 *	GetKBCodePage   (USER32.246)
 */
UINT WINAPI GetKBCodePage(void)
{
    return GetKBCodePage16();
}

/****************************************************************************
 *      GetKeyboardLayoutName16   (USER.477)
 */
INT16 WINAPI GetKeyboardLayoutName16(LPSTR pwszKLID)
{
	return GetKeyboardLayoutNameA(pwszKLID);
}

/***********************************************************************
 *           GetKeyboardLayout			(USER32.250)
 *
 * FIXME: - device handle for keyboard layout defaulted to 
 *          the language id. This is the way Windows default works.
 *        - the thread identifier (dwLayout) is also ignored.
 */
HKL WINAPI GetKeyboardLayout(DWORD dwLayout)
{
        HKL layout;
        layout = GetSystemDefaultLCID(); /* FIXME */
        layout |= (layout<<16);          /* FIXME */
        TRACE_(keyboard)("returning %08x\n",layout);
        return layout;
}

/****************************************************************************
 *	GetKeyboardLayoutNameA   (USER32.252)
 */
INT WINAPI GetKeyboardLayoutNameA(LPSTR pwszKLID)
{
        sprintf(pwszKLID, "%08x",GetKeyboardLayout(0));
        return 1;
}

/****************************************************************************
 *	GetKeyboardLayoutNameW   (USER32.253)
 */
INT WINAPI GetKeyboardLayoutNameW(LPWSTR pwszKLID)
{
        char buf[KL_NAMELENGTH];
	int res = GetKeyboardLayoutNameA(buf);
        MultiByteToWideChar( CP_ACP, 0, buf, -1, pwszKLID, KL_NAMELENGTH );
	return res;
}

/****************************************************************************
 *	GetKeyNameTextA   (USER32.247)
 */
INT WINAPI GetKeyNameTextA(LONG lParam, LPSTR lpBuffer, INT nSize)
{
	return GetKeyNameText16(lParam,lpBuffer,nSize);
}

/****************************************************************************
 *	GetKeyNameTextW   (USER32.248)
 */
INT WINAPI GetKeyNameTextW(LONG lParam, LPWSTR lpBuffer, INT nSize)
{
	int res;
	LPSTR buf = HeapAlloc( GetProcessHeap(), 0, nSize );
	if(buf == NULL) return 0; /* FIXME: is this the correct failure value?*/
	res = GetKeyNameTextA(lParam,buf,nSize);

        if (nSize > 0 && !MultiByteToWideChar( CP_ACP, 0, buf, -1, lpBuffer, nSize ))
            lpBuffer[nSize-1] = 0;
	HeapFree( GetProcessHeap(), 0, buf );
	return res;
}

/****************************************************************************
 *	ToUnicode      (USER32.@)
 */
INT WINAPI ToUnicode(UINT virtKey, UINT scanCode, LPBYTE lpKeyState,
		     LPWSTR lpwStr, int size, UINT flags)
{
    return USER_Driver.pToUnicode(virtKey, scanCode, lpKeyState, lpwStr, size, flags);
}

/****************************************************************************
 *	ToUnicodeEx      (USER32.@)
 */
INT WINAPI ToUnicodeEx(UINT virtKey, UINT scanCode, LPBYTE lpKeyState,
		       LPWSTR lpwStr, int size, UINT flags, HKL hkl)
{
    /* FIXME: need true implementation */
    return ToUnicode(virtKey, scanCode, lpKeyState, lpwStr, size, flags);
}

/****************************************************************************
 *	ToAscii      (USER32.546)
 */
INT WINAPI ToAscii( UINT virtKey,UINT scanCode,LPBYTE lpKeyState,
                        LPWORD lpChar,UINT flags )
{
    WCHAR uni_chars[2];
    INT ret, n_ret;

    ret = ToUnicode(virtKey, scanCode, lpKeyState, uni_chars, 2, flags);
    if(ret < 0) n_ret = 1; /* FIXME: make ToUnicode return 2 for dead chars */
    else n_ret = ret;
    WideCharToMultiByte(CP_ACP, 0, uni_chars, n_ret, (LPSTR)lpChar, 2, NULL, NULL);
    return ret;
}

/****************************************************************************
 *	ToAsciiEx      (USER32.547)
 */
INT WINAPI ToAsciiEx( UINT virtKey, UINT scanCode, LPBYTE lpKeyState,
                      LPWORD lpChar, UINT flags, HKL dwhkl )
{
    /* FIXME: need true implementation */
    return ToAscii(virtKey, scanCode, lpKeyState, lpChar, flags);
}

/**********************************************************************
 *           ActivateKeyboardLayout      (USER32.1)
 *
 * Call ignored. WINE supports only system default keyboard layout.
 */
HKL WINAPI ActivateKeyboardLayout(HKL hLayout, UINT flags)
{
    TRACE_(keyboard)("(%d, %d)\n", hLayout, flags);
    ERR_(keyboard)("Only default system keyboard layout supported. Call ignored.\n");
    return 0;
}


/***********************************************************************
 *           GetKeyboardLayoutList		(USER32.251)
 *
 * FIXME: Supports only the system default language and layout and 
 *          returns only 1 value.
 *
 * Return number of values available if either input parm is 
 *  0, per MS documentation.
 *
 */
INT WINAPI GetKeyboardLayoutList(INT nBuff,HKL *layouts)
{
        TRACE_(keyboard)("(%d,%p)\n",nBuff,layouts);
        if (!nBuff || !layouts)
            return 1;
	if (layouts)
		layouts[0] = GetKeyboardLayout(0);
	return 1;
}


/***********************************************************************
 *           RegisterHotKey			(USER32.433)
 */
BOOL WINAPI RegisterHotKey(HWND hwnd,INT id,UINT modifiers,UINT vk) {
	FIXME_(keyboard)("(0x%08x,%d,0x%08x,%d): stub\n",hwnd,id,modifiers,vk);
	return TRUE;
}

/***********************************************************************
 *           UnregisterHotKey			(USER32.565)
 */
BOOL WINAPI UnregisterHotKey(HWND hwnd,INT id) {
	FIXME_(keyboard)("(0x%08x,%d): stub\n",hwnd,id);
	return TRUE;
}

/***********************************************************************
 *           LoadKeyboardLayoutA                (USER32.367)
 * Call ignored. WINE supports only system default keyboard layout.
 */
HKL WINAPI LoadKeyboardLayoutA(LPCSTR pwszKLID, UINT Flags)
{
    TRACE_(keyboard)("(%s, %d)\n", pwszKLID, Flags);
    ERR_(keyboard)("Only default system keyboard layout supported. Call ignored.\n");
  return 0; 
}

/***********************************************************************
 *           LoadKeyboardLayoutW                (USER32.368)
 */
HKL WINAPI LoadKeyboardLayoutW(LPCWSTR pwszKLID, UINT Flags)
{
    char buf[9];
    
    WideCharToMultiByte( CP_ACP, 0, pwszKLID, -1, buf, sizeof(buf), NULL, NULL );
    buf[8] = 0;
    return LoadKeyboardLayoutA(buf, Flags);
}
