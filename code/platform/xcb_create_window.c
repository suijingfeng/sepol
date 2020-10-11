#ifndef __linux__
#error You should not be including this file on non-Linux platforms
#endif

#include <xcb/xcb.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "win_public.h"
#include "sys_public.h"
#include "WinSys_Common.h"

struct WinData_s s_xcb_win;

int WinSys_GetWinWidth(void)
{
	return s_xcb_win.winWidth;
}

int WinSys_GetWinHeight(void)
{
	return s_xcb_win.winHeight;
}

int WinSys_IsWinFullscreen(void)
{
	return s_xcb_win.isFullScreen;
}

int WinSys_IsWinMinimized(void)
{
	return s_xcb_win.isMinimized;
}

int WinSys_IsWinLostFocused(void)
{
	return s_xcb_win.isLostFocused;
}


static cvar_t* r_mode;				// video mode
static cvar_t* r_fullscreen;


// use this function in non-fullscreen mode, 
// always return a valid mode ...

xcb_intern_atom_reply_t *atom_wm_delete_window;


void WinSys_Init(void ** pContext, int type)
{
    Com_Printf( "Initializing window subsystem. \n" );

    // despect its name prefixed with r_
    // but they have nothing to do with the renderer. 
    r_mode = Cvar_Get( "r_mode", "3", CVAR_ARCHIVE | CVAR_LATCH );
    r_fullscreen = Cvar_Get( "r_fullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH );

    WinSys_ConstructDislayModes();

    *pContext = &s_xcb_win;


    Com_Printf( "Setting up XCB connection... \n" );

    const char * const pDisplay_envar = getenv("DISPLAY");

    if (pDisplay_envar == NULL || pDisplay_envar[0] == '\0')
    {
        Com_Printf(" Environment variable DISPLAY requires a valid value.");
    }
    else
    {
        Com_Printf(" $(DISPLAY) = %s \n", pDisplay_envar);
    }

    // An X program first needs to open the connection to the X server
    // if displayname = NULL, uses the DISPLAY environment variable
    // returns the screen number of the connection, 
    // can provide NULL if you don't care.

    s_xcb_win.connection = xcb_connect(NULL, &s_xcb_win.screenIdx);

    if (xcb_connection_has_error(s_xcb_win.connection) > 0)
    {
        Com_Error(ERR_FATAL, "Cannot set up connection using XCB... ");
    }

    // ==============================================================================
    // Once we have opened a connection to an X server, we should check some basic
    // information about it: what screens it has, what is the size (width and height)
    // of the screen, how many colors it supports (black and white ? 256 colors ? ). 
    // We get such information from the xcbscreent structure:
    // =====================================================================

    // Get the screen whose number is screenNum
    const xcb_setup_t * const pXcbSetup = xcb_get_setup(s_xcb_win.connection);

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(pXcbSetup);

    // we want the screen at index screenNum of the iterator
    for (int i = 0; i < s_xcb_win.screenIdx; ++i) 
    {
        xcb_screen_next (&iter);
    }

    xcb_screen_t * pScreen = iter.data;

    s_xcb_win.desktopWidth = pScreen->width_in_pixels;
    s_xcb_win.desktopHeight = pScreen->height_in_pixels;
    s_xcb_win.root = pScreen->root;
    // report

    Com_Printf("\nInformations of screen: %d\n", pScreen->root);
    Com_Printf("  width.................: %d\n", pScreen->width_in_pixels);
    Com_Printf("  height................: %d\n", pScreen->height_in_pixels);
    Com_Printf("  white pixel...........: %d\n", pScreen->white_pixel);
    Com_Printf("  black pixel...........: %d\n", pScreen->black_pixel);
    Com_Printf("  allowed depths len....: %d\n", pScreen->allowed_depths_len);
    Com_Printf("  root depth............: %d\n", pScreen->root_depth);
    Com_Printf("\n");


    // =======================================================================
    // =======================================================================

    // After we got some basic information about our screen, 
    // we can create our first window. In the X Window System,
    // a window is characterized by an Id. So, in XCB, a window
    // is of type of uint32_t.
    // 
    // We first ask for a new Id for our window
    s_xcb_win.hWnd = xcb_generate_id(s_xcb_win.connection);

    /*
    Then, XCB supplies the following function to create new windows:

    xcb_void_cookie_t xcb_create_window ( xcb_connection_t *connection, 
    uint8_t depth, xcb_window_t wid, xcb_window_t parent,
    int16_t x, int16_t y, uint16_t width, uint16_t height,
    uint16_t border_width, uint16_t _class, xcb_visualid_t visual,
    uint32_t value_mask, const uint32_t* value_list );
    */
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = s_xcb_win.desktopWidth;
    uint32_t height = s_xcb_win.desktopHeight;

    // -------------------------------------------------
    // developing now, its prefix started with r_
    // but it have nothing to do with the renderer ...
    // r_fullscreen->integer = 0;
    // r_mode->integer = 3;
    // -------------------------------------------------
    if(r_fullscreen->integer)
    {
        // if fullscreen is true, then we use desktop video resolution
        r_mode->integer = -2;

        x = 0;
        y = 0;
        width = s_xcb_win.desktopWidth;
        height = s_xcb_win.desktopHeight;

        Com_Printf( " Setting fullscreen mode. \n" );
    }
    else
    {
        R_GetDisplayMode(r_mode->integer, &width, &height);

        // r_mode->integer = R_GetModeInfo(&width, &height,
        //        r_mode->integer, s_xcb_win.desktopWidth, s_xcb_win.desktopHeight);
        // width = 1280;
        // height = 720;
    }

    // R_SetWinMode(r_mode->integer, 640, 480, 60);

    // During the creation of a window, you should give it what kind of events it wishes to receive. 
    uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    // The values that a mask could take are given by the xcb_cw_t enumeration:
    uint32_t value_list[2];

    value_list[0] = pScreen->white_pixel;
    value_list[1] = XCB_EVENT_MASK_KEY_PRESS | 
                    XCB_EVENT_MASK_KEY_RELEASE | 
                    XCB_EVENT_MASK_BUTTON_PRESS |
                    XCB_EVENT_MASK_BUTTON_RELEASE | 
                    XCB_EVENT_MASK_ENTER_WINDOW | 
                    XCB_EVENT_MASK_LEAVE_WINDOW |
                    XCB_EVENT_MASK_POINTER_MOTION | 
                    XCB_EVENT_MASK_KEYMAP_STATE | 
                    XCB_EVENT_MASK_FOCUS_CHANGE |
                    XCB_EVENT_MASK_EXPOSURE |
                    XCB_EVENT_MASK_VISIBILITY_CHANGE |
                    XCB_EVENT_MASK_PROPERTY_CHANGE | 
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    //
    // XCB_COPY_FROM_PARENT: depth (same as root) the depth is taken from the parent window.
    // screen->root        : parent window
    // s_xcb_win.hWnd: The ID with which you will refer to the new window, created by xcb_generate_id. 

    // screen->root: The parent window of the new window.
    // 10: Width of the window's border (in pixels) 
    // XCB_WINDOW_CLASS_INPUT_OUTPUT: without documention ???
    //


    if(r_fullscreen->integer)
    {
        xcb_create_window( s_xcb_win.connection, 
            XCB_COPY_FROM_PARENT, 
            s_xcb_win.hWnd, 
            pScreen->root,
            0, 0, width, height, 0, 
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            pScreen->root_visual,
            value_mask, value_list);
    }
    else
    {
        xcb_create_window( s_xcb_win.connection, 
            XCB_COPY_FROM_PARENT, 
            s_xcb_win.hWnd, 
            pScreen->root,
            x, y, width, height, 10, 
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            pScreen->root_visual,
            value_mask, value_list);
    }

    s_xcb_win.winWidth = width;
    s_xcb_win.winHeight = height;


    // If the window has already been created, we can use the xcb_change_window_attributes()
    // function to set the events that the window will receive. The subsection Configuring a
    // window shows its prototype. 
    
    // configure to send notification when window is destroyed
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(s_xcb_win.connection, 1, 12, "WM_PROTOCOLS");
    xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(s_xcb_win.connection, 0, 16, "WM_DELETE_WINDOW");
    
    xcb_intern_atom_reply_t * reply = xcb_intern_atom_reply(s_xcb_win.connection, cookie, 0);
    
    atom_wm_delete_window = xcb_intern_atom_reply(s_xcb_win.connection, cookie2, 0);

    static const char* pVkTitle = "OpenArena";
    /* Set the title of the window */
    xcb_change_property (s_xcb_win.connection, XCB_PROP_MODE_REPLACE, s_xcb_win.hWnd,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(pVkTitle), pVkTitle);

    /* set the title of the window icon */

    static const char * pIconTitle = "OpenArena (iconified)";
    
    xcb_change_property (s_xcb_win.connection, XCB_PROP_MODE_REPLACE, s_xcb_win.hWnd, 
        XCB_ATOM_WM_ICON_NAME, XCB_ATOM_STRING, 8, strlen(pIconTitle), pIconTitle);

    xcb_change_property(s_xcb_win.connection, XCB_PROP_MODE_REPLACE, 
            s_xcb_win.hWnd, (*reply).atom, 4, 32, 1, &(*atom_wm_delete_window).atom);
    

    // The fact that we created the window does not mean that it will be drawn on screen.
    // By default, newly created windows are not mapped on the screen (they are invisible).
    // In order to make our window visible, we use the function xcb_map_window()
    //
    // Mapping a window causes the window to appear on the screen, Un-mapping it causes it 
    // to be removed from the screen (although the window as a logical entity still exists). 
    // This gives the effect of making a window hidden (unmapped) and shown again (mapped). 
    
    Com_Printf(" ... xcb map the window ... \n");
    
    xcb_map_window(s_xcb_win.connection, s_xcb_win.hWnd);
	    
    //  Make sure commands are sent before we pause so that the window gets shown
    xcb_flush (s_xcb_win.connection);
    
    free(reply);

    // input system ?
    IN_Init();
}


void WinSys_Shutdown(void)
{
    xcb_destroy_window( s_xcb_win.connection, s_xcb_win.hWnd);
    
    free(atom_wm_delete_window);
    
    //xcb_disconnect(connection);
    memset(&s_xcb_win, 0, sizeof(s_xcb_win));

    WinSys_DestructDislayModes( );

    Com_Printf(" Window subsystem destroyed. \n");
}


void WinSys_EndFrame(void)
{
    ;
}


void WinSys_SetGamma(unsigned char red[256], unsigned char green[256], unsigned char blue[256])
{
    Com_Printf(" WinSys_SetGamma: Not Implemented Now. \n");
}



// TODO: impl
char * Sys_GetClipboardData( void )
{
/*	
	const Atom xtarget = XInternAtom( dpy, "UTF8_STRING", 0 );
	unsigned long nitems, rem;
	unsigned char *data;
	Atom type;
	XEvent ev;
	char *buf;
	int format;

	XConvertSelection( dpy, XA_PRIMARY, xtarget, XA_PRIMARY, win, CurrentTime );
	XSync( dpy, False );
	XNextEvent( dpy, &ev );
	if ( !XFilterEvent( &ev, None ) && ev.type == SelectionNotify ) {
		if ( XGetWindowProperty( dpy, win, XA_PRIMARY, 0, 8, False, AnyPropertyType,
			&type, &format, &nitems, &rem, &data ) == 0 ) {
			if ( format == 8 ) {
				if ( nitems > 0 ) {
					buf = Z_Malloc( nitems + 1 );
					Q_strncpyz( buf, (char*)data, nitems + 1 );
					strtok( buf, "\n\r\b" );
					return buf;
				}
			} else {
				fprintf( stderr, "Bad clipboard format %i\n", format );
			}
		} else {
			fprintf( stderr, "Clipboard allocation failed\n" );
		}
	}
*/
	return NULL;
}
