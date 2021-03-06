/////////////////////////////////////////////////////////////////////////////
// Name:        src/gtk/toplevel.cpp
// Purpose:
// Author:      Robert Roebling
// Id:          $Id: toplevel.cpp 59520 2009-03-13 23:07:53Z FM $
// Copyright:   (c) 1998 Robert Roebling
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

#ifdef __VMS
#define XIconifyWindow XICONIFYWINDOW
#endif

#include "wx/toplevel.h"

#ifndef WX_PRECOMP
    #include "wx/frame.h"
    #include "wx/icon.h"
    #include "wx/log.h"
    #include "wx/app.h"
#endif

#include "wx/gtk/private.h"
#include "wx/evtloop.h"
#include "wx/sysopt.h"

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "wx/gtk/private/win_gtk.h"

#include "wx/unix/utilsx11.h"

// XA_CARDINAL
#include <X11/Xatom.h>

#if wxUSE_LIBHILDON
    #include <hildon-widgets/hildon-program.h>
    #include <hildon-widgets/hildon-window.h>
#endif // wxUSE_LIBHILDON

// ----------------------------------------------------------------------------
// data
// ----------------------------------------------------------------------------

// this is incremented while a modal dialog is shown
int wxOpenModalDialogsCount = 0;

// the frame that is currently active (i.e. its child has focus). It is
// used to generate wxActivateEvents
static wxTopLevelWindowGTK *g_activeFrame = NULL;
static wxTopLevelWindowGTK *g_lastActiveFrame = NULL;

// if we detect that the app has got/lost the focus, we set this variable to
// either TRUE or FALSE and an activate event will be sent during the next
// OnIdle() call and it is reset to -1: this value means that we shouldn't
// send any activate events at all
static int g_sendActivateEvent = -1;

//-----------------------------------------------------------------------------
// RequestUserAttention related functions
//-----------------------------------------------------------------------------

extern "C" {
static void wxgtk_window_set_urgency_hint (GtkWindow *win,
                                           gboolean setting)
{
    wxASSERT_MSG( GTK_WIDGET_REALIZED(win), wxT("wxgtk_window_set_urgency_hint: GdkWindow not realized") );
    GdkWindow *window = GTK_WIDGET(win)->window;
    XWMHints *wm_hints;

    wm_hints = XGetWMHints(GDK_WINDOW_XDISPLAY(window), GDK_WINDOW_XWINDOW(window));

    if (!wm_hints)
        wm_hints = XAllocWMHints();

    if (setting)
        wm_hints->flags |= XUrgencyHint;
    else
        wm_hints->flags &= ~XUrgencyHint;

    XSetWMHints(GDK_WINDOW_XDISPLAY(window), GDK_WINDOW_XWINDOW(window), wm_hints);
    XFree(wm_hints);
}

static gboolean gtk_frame_urgency_timer_callback( wxTopLevelWindowGTK *win )
{
#if GTK_CHECK_VERSION(2,7,0)
    if(!gtk_check_version(2,7,0))
        gtk_window_set_urgency_hint(GTK_WINDOW( win->m_widget ), FALSE);
    else
#endif
        wxgtk_window_set_urgency_hint(GTK_WINDOW( win->m_widget ), FALSE);

    win->m_urgency_hint = -2;
    return FALSE;
}
}

//-----------------------------------------------------------------------------
// "focus_in_event"
//-----------------------------------------------------------------------------

extern "C" {
static gboolean gtk_frame_focus_in_callback( GtkWidget *widget,
                                         GdkEvent *WXUNUSED(event),
                                         wxTopLevelWindowGTK *win )
{
    switch ( g_sendActivateEvent )
    {
        case -1:
            // we've got focus from outside, synthetize wxActivateEvent
            g_sendActivateEvent = 1;
            break;

        case 0:
            // another our window just lost focus, it was already ours before
            // - don't send any wxActivateEvent
            g_sendActivateEvent = -1;
            break;
    }

    g_activeFrame = win;
    g_lastActiveFrame = g_activeFrame;

    // wxPrintf( wxT("active: %s\n"), win->GetTitle().c_str() );

    // MR: wxRequestUserAttention related block
    switch( win->m_urgency_hint )
    {
        default:
            g_source_remove( win->m_urgency_hint );
            // no break, fallthrough to remove hint too
        case -1:
#if GTK_CHECK_VERSION(2,7,0)
            if(!gtk_check_version(2,7,0))
                gtk_window_set_urgency_hint(GTK_WINDOW( widget ), FALSE);
            else
#endif
            {
                wxgtk_window_set_urgency_hint(GTK_WINDOW( widget ), FALSE);
            }

            win->m_urgency_hint = -2;
            break;

        case -2: break;
    }

    wxLogTrace(wxT("activate"), wxT("Activating frame %p (from focus_in)"), g_activeFrame);
    wxActivateEvent event(wxEVT_ACTIVATE, true, g_activeFrame->GetId());
    event.SetEventObject(g_activeFrame);
    g_activeFrame->HandleWindowEvent(event);

    return FALSE;
}
}

//-----------------------------------------------------------------------------
// "focus_out_event"
//-----------------------------------------------------------------------------

extern "C" {
static
gboolean gtk_frame_focus_out_callback(GtkWidget * WXUNUSED(widget),
                                      GdkEventFocus *WXUNUSED(gdk_event),
                                      wxTopLevelWindowGTK * WXUNUSED(win))
{
    // if the focus goes out of our app alltogether, OnIdle() will send
    // wxActivateEvent, otherwise gtk_window_focus_in_callback() will reset
    // g_sendActivateEvent to -1
    g_sendActivateEvent = 0;

    // wxASSERT_MSG( (g_activeFrame == win), wxT("TLW deactivatd although it wasn't active") );

    // wxPrintf( wxT("inactive: %s\n"), win->GetTitle().c_str() );

    if (g_activeFrame)
    {
        wxLogTrace(wxT("activate"), wxT("Activating frame %p (from focus_in)"), g_activeFrame);
        wxActivateEvent event(wxEVT_ACTIVATE, false, g_activeFrame->GetId());
        event.SetEventObject(g_activeFrame);
        g_activeFrame->HandleWindowEvent(event);

        g_activeFrame = NULL;
    }

    return FALSE;
}
}

//-----------------------------------------------------------------------------
// "size_allocate" from m_wxwindow
//-----------------------------------------------------------------------------

extern "C" {
static void
size_allocate(GtkWidget*, GtkAllocation* alloc, wxTopLevelWindowGTK* win)
{
    if (win->m_oldClientWidth  != alloc->width ||
        win->m_oldClientHeight != alloc->height)
    {
        win->m_oldClientWidth  = alloc->width;
        win->m_oldClientHeight = alloc->height;

        wxSize size(win->m_widget->allocation.width,
                    win->m_widget->allocation.height);
        size += win->m_decorSize;
        win->m_width  = size.x;
        win->m_height = size.y;

        if (!win->IsIconized())
        {
            wxSizeEvent event(size, win->GetId());
            event.SetEventObject(win);
            win->HandleWindowEvent(event);
        }
        // else the window is currently unmapped, don't generate size events
    }
}
}

// ----------------------------------------------------------------------------
// "size_request"
// ----------------------------------------------------------------------------

extern "C" {
static
void wxgtk_tlw_size_request_callback(GtkWidget * WXUNUSED(widget),
                                     GtkRequisition *requisition,
                                     wxTopLevelWindowGTK *win)
{
    // we must return the size of the window without WM decorations, otherwise
    // GTK+ gets confused, so don't call just GetSize() here
    win->GTKDoGetSize(&requisition->width, &requisition->height);
}
}

//-----------------------------------------------------------------------------
// "delete_event"
//-----------------------------------------------------------------------------

extern "C" {
static gboolean
gtk_frame_delete_callback( GtkWidget *WXUNUSED(widget),
                           GdkEvent *WXUNUSED(event),
                           wxTopLevelWindowGTK *win )
{
    if (win->IsEnabled() &&
        (wxOpenModalDialogsCount == 0 || (win->GetExtraStyle() & wxTOPLEVEL_EX_DIALOG) ||
         win->IsGrabbed()))
        win->Close();

    return TRUE;
}
}

//-----------------------------------------------------------------------------
// "configure_event"
//-----------------------------------------------------------------------------

extern "C" {
static gboolean
gtk_frame_configure_callback( GtkWidget* widget,
                              GdkEventConfigure *WXUNUSED(event),
                              wxTopLevelWindowGTK *win )
{
    if (!win->m_hasVMT || !win->IsShown())
        return FALSE;

    wxPoint point;
    gtk_window_get_position((GtkWindow*)widget, &point.x, &point.y);

    win->m_x = point.x;
    win->m_y = point.y;
    wxMoveEvent mevent(point, win->GetId());
    mevent.SetEventObject( win );
    win->HandleWindowEvent( mevent );

    return FALSE;
}
}

//-----------------------------------------------------------------------------
// "realize" from m_widget
//-----------------------------------------------------------------------------

// we cannot the WM hints and icons before the widget has been realized,
// so we do this directly after realization

extern "C" {
static void
gtk_frame_realized_callback( GtkWidget * WXUNUSED(widget),
                             wxTopLevelWindowGTK *win )
{
    gdk_window_set_decorations(win->m_widget->window,
                               (GdkWMDecoration)win->m_gdkDecor);
    gdk_window_set_functions(win->m_widget->window,
                               (GdkWMFunction)win->m_gdkFunc);

    // GTK's shrinking/growing policy
    if ( !(win->m_gdkFunc & GDK_FUNC_RESIZE) )
        gtk_window_set_resizable(GTK_WINDOW(win->m_widget), FALSE);
    else
        gtk_window_set_policy(GTK_WINDOW(win->m_widget), 1, 1, 1);

    const wxIconBundle& icons = win->GetIcons();
    if (icons.GetIconCount())
        win->SetIcons(icons);

    if (win->HasFlag(wxFRAME_SHAPED))
        win->SetShape(win->m_shape); // it will really set the window shape now
}
}

//-----------------------------------------------------------------------------
// "map_event" from m_widget
//-----------------------------------------------------------------------------

extern "C" {
static gboolean
gtk_frame_map_callback( GtkWidget*,
                        GdkEvent * WXUNUSED(event),
                        wxTopLevelWindow *win )
{
    const bool wasIconized = win->IsIconized();
    win->SetIconizeState(false);
    if (wasIconized)
    {
        // Because GetClientSize() returns (0,0) when IsIconized() is true,
        // a size event must be generated, just in case GetClientSize() was
        // called while iconized. This specifically happens when restoring a
        // tlw that was "rolled up" with some WMs.
        // Queue a resize rather than sending size event directly to allow
        // children to be made visible first.
        win->m_oldClientWidth = 0;
        gtk_widget_queue_resize(win->m_wxwindow);
    }
    // it is possible for m_isShown to be false here, see bug #9909
    if (win->wxWindowBase::Show(true))
    {
        wxShowEvent eventShow(win->GetId(), true);
        eventShow.SetEventObject(win);
        win->GetEventHandler()->ProcessEvent(eventShow);
    }
    return false;
}
}

//-----------------------------------------------------------------------------
// "unmap_event" from m_widget
//-----------------------------------------------------------------------------

extern "C" {
static gboolean
gtk_frame_unmap_callback( GtkWidget * WXUNUSED(widget),
                          GdkEvent * WXUNUSED(event),
                          wxTopLevelWindow *win )
{
    win->SetIconizeState(true);
    return false;
}
}

//-----------------------------------------------------------------------------

bool wxGetFrameExtents(GdkWindow* window, int* left, int* right, int* top, int* bottom)
{
    static GdkAtom property = gdk_atom_intern("_NET_FRAME_EXTENTS", false);
    Atom xproperty = gdk_x11_atom_to_xatom_for_display(
                        gdk_drawable_get_display(window), property);
    Atom type;
    int format;
    gulong nitems, bytes_after;
    guchar* data;
    Status status = XGetWindowProperty(
        gdk_x11_drawable_get_xdisplay(window),
        gdk_x11_drawable_get_xid(window),
        xproperty,
        0, 4, false, XA_CARDINAL,
        &type, &format, &nitems, &bytes_after, &data);
    const bool success = status == Success && data && nitems == 4;
    if (success)
    {
        long* p = (long*)data;
        if (left)   *left   = int(p[0]);
        if (right)  *right  = int(p[1]);
        if (top)    *top    = int(p[2]);
        if (bottom) *bottom = int(p[3]);
    }
    if (data)
        XFree(data);
    return success;
}

//-----------------------------------------------------------------------------
// "property_notify_event" from m_widget
//-----------------------------------------------------------------------------

extern "C" {
static gboolean property_notify_event(
    GtkWidget*, GdkEventProperty* event, wxTopLevelWindowGTK* win)
{
    // Watch for changes to _NET_FRAME_EXTENTS property
    static GdkAtom property = gdk_atom_intern("_NET_FRAME_EXTENTS", false);
    if (event->state == GDK_PROPERTY_NEW_VALUE && event->atom == property)
    {
        int left, right, top, bottom;
        if (wxGetFrameExtents(event->window, &left, &right, &top, &bottom))
        {
            const wxSize decorSize =
                wxSize(left + right, top + bottom);
            win->GTKUpdateDecorSize(decorSize);
        }
    }
    return false;
}
}

// ----------------------------------------------------------------------------
// wxTopLevelWindowGTK creation
// ----------------------------------------------------------------------------

void wxTopLevelWindowGTK::Init()
{
    m_mainWidget = NULL;
    m_isIconized = false;
    m_fsIsShowing = false;
    m_themeEnabled = true;
    m_gdkDecor =
    m_gdkFunc = 0;
    m_grabbed = false;
    m_deferShow = true;
    m_deferShowAllowed = true;
    m_updateDecorSize = true;

    m_urgency_hint = -2;
}

bool wxTopLevelWindowGTK::Create( wxWindow *parent,
                                  wxWindowID id,
                                  const wxString& title,
                                  const wxPoint& pos,
                                  const wxSize& sizeOrig,
                                  long style,
                                  const wxString &name )
{
    // always create a frame of some reasonable, even if arbitrary, size (at
    // least for MSW compatibility)
    wxSize size = sizeOrig;
    size.x = WidthDefault(size.x);
    size.y = HeightDefault(size.y);

    wxTopLevelWindows.Append( this );

    if (!PreCreation( parent, pos, size ) ||
        !CreateBase( parent, id, pos, size, style, wxDefaultValidator, name ))
    {
        wxFAIL_MSG( wxT("wxTopLevelWindowGTK creation failed") );
        return false;
    }

    m_title = title;

    // NB: m_widget may be !=NULL if it was created by derived class' Create,
    //     e.g. in wxTaskBarIconAreaGTK
    if (m_widget == NULL)
    {
#if wxUSE_LIBHILDON
        // we must create HildonWindow and not a normal GtkWindow as the latter
        // doesn't look correctly in Maemo environment and it must also be
        // registered with the main program object
        m_widget = hildon_window_new();
        hildon_program_add_window(wxTheApp->GetHildonProgram(),
                                  HILDON_WINDOW(m_widget));
#else // !wxUSE_LIBHILDON
        m_widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        if (GetExtraStyle() & wxTOPLEVEL_EX_DIALOG)
        {
            // Tell WM that this is a dialog window and make it center
            // on parent by default (this is what GtkDialog ctor does):
            gtk_window_set_type_hint(GTK_WINDOW(m_widget),
                                     GDK_WINDOW_TYPE_HINT_DIALOG);
            gtk_window_set_position(GTK_WINDOW(m_widget),
                                    GTK_WIN_POS_CENTER_ON_PARENT);
        }
        else
        {
            if (style & wxFRAME_TOOL_WINDOW)
            {
                gtk_window_set_type_hint(GTK_WINDOW(m_widget),
                                         GDK_WINDOW_TYPE_HINT_UTILITY);

                // On some WMs, like KDE, a TOOL_WINDOW will still show
                // on the taskbar, but on Gnome a TOOL_WINDOW will not.
                // For consistency between WMs and with Windows, we
                // should set the NO_TASKBAR flag which will apply
                // the set_skip_taskbar_hint if it is available,
                // ensuring no taskbar entry will appear.
                style |= wxFRAME_NO_TASKBAR;
            }
        }
#endif // wxUSE_LIBHILDON/!wxUSE_LIBHILDON

        g_object_ref(m_widget);
    }

    wxWindow *topParent = wxGetTopLevelParent(m_parent);
    if (topParent && (((GTK_IS_WINDOW(topParent->m_widget)) &&
                       (GetExtraStyle() & wxTOPLEVEL_EX_DIALOG)) ||
                       (style & wxFRAME_FLOAT_ON_PARENT)))
    {
        gtk_window_set_transient_for( GTK_WINDOW(m_widget),
                                      GTK_WINDOW(topParent->m_widget) );
    }

    if (style & wxFRAME_NO_TASKBAR)
    {
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(m_widget), TRUE);
    }

    if (style & wxSTAY_ON_TOP)
    {
        gtk_window_set_keep_above(GTK_WINDOW(m_widget), TRUE);
    }

#if 0
    if (!name.empty())
        gtk_window_set_role( GTK_WINDOW(m_widget), wxGTK_CONV( name ) );
#endif

    gtk_window_set_title( GTK_WINDOW(m_widget), wxGTK_CONV( title ) );
    GTK_WIDGET_UNSET_FLAGS( m_widget, GTK_CAN_FOCUS );

    g_signal_connect (m_widget, "delete_event",
                      G_CALLBACK (gtk_frame_delete_callback), this);

    // m_mainWidget is a GtkVBox, holding the bars and client area (m_wxwindow)
    m_mainWidget = gtk_vbox_new(false, 0);
    gtk_widget_show( m_mainWidget );
    GTK_WIDGET_UNSET_FLAGS( m_mainWidget, GTK_CAN_FOCUS );
    gtk_container_add( GTK_CONTAINER(m_widget), m_mainWidget );

    // m_wxwindow is the client area
    m_wxwindow = wxPizza::New(0,this);
    gtk_widget_show( m_wxwindow );
    gtk_container_add( GTK_CONTAINER(m_mainWidget), m_wxwindow );

    // we donm't allow the frame to get the focus as otherwise
    // the frame will grab it at arbitrary focus changes
    GTK_WIDGET_UNSET_FLAGS( m_wxwindow, GTK_CAN_FOCUS );

    if (m_parent) m_parent->AddChild( this );

    g_signal_connect(m_wxwindow, "size_allocate",
        G_CALLBACK(size_allocate), this);

    g_signal_connect (m_widget, "size_request",
                      G_CALLBACK (wxgtk_tlw_size_request_callback), this);
    PostCreation();

    if ((m_x != -1) || (m_y != -1))
        gtk_widget_set_uposition( m_widget, m_x, m_y );

    //  we cannot set MWM hints and icons before the widget has
    //  been realized, so we do this directly after realization
    g_signal_connect (m_widget, "realize",
                      G_CALLBACK (gtk_frame_realized_callback), this);

    // map and unmap for iconized state
    g_signal_connect (m_widget, "map_event",
                      G_CALLBACK (gtk_frame_map_callback), this);
    g_signal_connect (m_widget, "unmap_event",
                      G_CALLBACK (gtk_frame_unmap_callback), this);

    // for wxMoveEvent
    g_signal_connect (m_widget, "configure_event",
                      G_CALLBACK (gtk_frame_configure_callback), this);

    // activation
    g_signal_connect_after (m_widget, "focus_in_event",
                      G_CALLBACK (gtk_frame_focus_in_callback), this);
    g_signal_connect_after (m_widget, "focus_out_event",
                      G_CALLBACK (gtk_frame_focus_out_callback), this);

    gtk_widget_add_events(m_widget, GDK_PROPERTY_CHANGE_MASK);
    g_signal_connect(m_widget, "property_notify_event",
        G_CALLBACK(property_notify_event), this);

    // translate wx decorations styles into Motif WM hints (they are recognized
    // by other WMs as well)

    // always enable moving the window as we have no separate flag for enabling
    // it
    m_gdkFunc = GDK_FUNC_MOVE;

    if ( style & wxCLOSE_BOX )
        m_gdkFunc |= GDK_FUNC_CLOSE;

    if ( style & wxMINIMIZE_BOX )
        m_gdkFunc |= GDK_FUNC_MINIMIZE;

    if ( style & wxMAXIMIZE_BOX )
        m_gdkFunc |= GDK_FUNC_MAXIMIZE;

    if ( (style & wxSIMPLE_BORDER) || (style & wxNO_BORDER) )
    {
        m_gdkDecor = 0;
    }
    else // have border
    {
        m_gdkDecor = GDK_DECOR_BORDER;

        if ( style & wxCAPTION )
            m_gdkDecor |= GDK_DECOR_TITLE;

        if ( style & wxSYSTEM_MENU )
            m_gdkDecor |= GDK_DECOR_MENU;

        if ( style & wxMINIMIZE_BOX )
            m_gdkDecor |= GDK_DECOR_MINIMIZE;

        if ( style & wxMAXIMIZE_BOX )
            m_gdkDecor |= GDK_DECOR_MAXIMIZE;

        if ( style & wxRESIZE_BORDER )
        {
           m_gdkFunc |= GDK_FUNC_RESIZE;
           m_gdkDecor |= GDK_DECOR_RESIZEH;
        }
    }

    m_decorSize = GetCachedDecorSize();
    int w, h;
    GTKDoGetSize(&w, &h);
    gtk_window_set_default_size(GTK_WINDOW(m_widget), w, h);

    return true;
}

wxTopLevelWindowGTK::~wxTopLevelWindowGTK()
{
#if wxUSE_LIBHILDON
    // it can also be a (standard) dialog
    if ( HILDON_IS_WINDOW(m_widget) )
    {
        hildon_program_remove_window(wxTheApp->GetHildonProgram(),
                                     HILDON_WINDOW(m_widget));
    }
#endif // wxUSE_LIBHILDON

    if (m_grabbed)
    {
        wxFAIL_MSG(_T("Window still grabbed"));
        RemoveGrab();
    }

    SendDestroyEvent();

    // it may also be GtkScrolledWindow in the case of an MDI child
    if (GTK_IS_WINDOW(m_widget))
    {
        gtk_window_set_focus( GTK_WINDOW(m_widget), NULL );
    }

    if (g_activeFrame == this)
        g_activeFrame = NULL;
    if (g_lastActiveFrame == this)
        g_lastActiveFrame = NULL;
}

bool wxTopLevelWindowGTK::EnableCloseButton( bool enable )
{
    if (enable)
        m_gdkFunc |= GDK_FUNC_CLOSE;
    else
        m_gdkFunc &= ~GDK_FUNC_CLOSE;

    if (GTK_WIDGET_REALIZED(m_widget) && (m_widget->window))
        gdk_window_set_functions( m_widget->window, (GdkWMFunction)m_gdkFunc );

    return true;
}

bool wxTopLevelWindowGTK::ShowFullScreen(bool show, long)
{
    if (show == m_fsIsShowing)
        return false; // return what?

    m_fsIsShowing = show;

    wxX11FullScreenMethod method =
        wxGetFullScreenMethodX11((WXDisplay*)GDK_DISPLAY(),
                                 (WXWindow)GDK_ROOT_WINDOW());

    // NB: gtk_window_fullscreen() uses freedesktop.org's WMspec extensions
    //     to switch to fullscreen, which is not always available. We must
    //     check if WM supports the spec and use legacy methods if it
    //     doesn't.
    if ( method == wxX11_FS_WMSPEC )
    {
        if (show)
            gtk_window_fullscreen( GTK_WINDOW( m_widget ) );
        else
            gtk_window_unfullscreen( GTK_WINDOW( m_widget ) );
    }
    else
    {
        GdkWindow *window = m_widget->window;

        if (show)
        {
            GetPosition( &m_fsSaveFrame.x, &m_fsSaveFrame.y );
            GetSize( &m_fsSaveFrame.width, &m_fsSaveFrame.height );

            int screen_width,screen_height;
            wxDisplaySize( &screen_width, &screen_height );

            gint client_x, client_y, root_x, root_y;
            gint width, height;

            m_fsSaveGdkFunc = m_gdkFunc;
            m_fsSaveGdkDecor = m_gdkDecor;
            m_gdkFunc = m_gdkDecor = 0;
            gdk_window_set_decorations(window, (GdkWMDecoration)0);
            gdk_window_set_functions(window, (GdkWMFunction)0);

            gdk_window_get_origin (m_widget->window, &root_x, &root_y);
            gdk_window_get_geometry (m_widget->window, &client_x, &client_y,
                         &width, &height, NULL);

            gdk_window_move_resize (m_widget->window, -client_x, -client_y,
                        screen_width + 1, screen_height + 1);

            wxSetFullScreenStateX11((WXDisplay*)GDK_DISPLAY(),
                                    (WXWindow)GDK_ROOT_WINDOW(),
                                    (WXWindow)GDK_WINDOW_XWINDOW(window),
                                    show, &m_fsSaveFrame, method);
        }
        else // hide
        {
            m_gdkFunc = m_fsSaveGdkFunc;
            m_gdkDecor = m_fsSaveGdkDecor;
            gdk_window_set_decorations(window, (GdkWMDecoration)m_gdkDecor);
            gdk_window_set_functions(window, (GdkWMFunction)m_gdkFunc);

            wxSetFullScreenStateX11((WXDisplay*)GDK_DISPLAY(),
                                    (WXWindow)GDK_ROOT_WINDOW(),
                                    (WXWindow)GDK_WINDOW_XWINDOW(window),
                                    show, &m_fsSaveFrame, method);

            SetSize(m_fsSaveFrame.x, m_fsSaveFrame.y,
                    m_fsSaveFrame.width, m_fsSaveFrame.height);
        }
    }

    // documented behaviour is to show the window if it's still hidden when
    // showing it full screen
    if (show)
        Show();

    return true;
}

// ----------------------------------------------------------------------------
// overridden wxWindow methods
// ----------------------------------------------------------------------------

void wxTopLevelWindowGTK::Refresh( bool WXUNUSED(eraseBackground), const wxRect *WXUNUSED(rect) )
{
    wxCHECK_RET( m_widget, wxT("invalid frame") );
    
    gtk_widget_queue_draw( m_widget );

    if (m_wxwindow && m_wxwindow->window)
        gdk_window_invalidate_rect( m_wxwindow->window, NULL, TRUE );
}

bool wxTopLevelWindowGTK::Show( bool show )
{
    wxASSERT_MSG( (m_widget != NULL), wxT("invalid frame") );

    bool deferShow = show && !m_isShown && m_deferShow && m_deferShowAllowed;
    if (deferShow)
    {
        deferShow = false;
        if (!GTK_WIDGET_REALIZED(m_widget) &&
            g_signal_handler_find(m_widget,
                GSignalMatchType(G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DATA),
                g_signal_lookup("property_notify_event", GTK_TYPE_WIDGET),
                0, NULL, NULL, this))
        {
            if (gdk_x11_screen_supports_net_wm_hint(
                gtk_widget_get_screen(m_widget),
                gdk_atom_intern("_NET_REQUEST_FRAME_EXTENTS", false)))
            {
                deferShow = true;
            }
            else
            {
                // Don't allow changes to m_decorSize, it breaks saving/restoring
                // window size with GetSize()/SetSize() because it makes window
                // bigger between each restore and save.
                m_updateDecorSize = false;
            }
        }
        m_deferShow = deferShow;
    }
    if (deferShow)
    {
        // Initial show. If WM supports _NET_REQUEST_FRAME_EXTENTS, defer
        // calling gtk_widget_show() until _NET_FRAME_EXTENTS property
        // notification is received, so correct frame extents are known.
        // This allows resizing m_widget to keep the overall size in sync with
        // what wxWidgets expects it to be without an obvious change in the
        // window size immediately after it becomes visible.

        // Realize m_widget, so m_widget->window can be used. Realizing normally
        // causes the widget tree to be size_allocated, which generates size
        // events in the wrong order. However, the size_allocates will not be
        // done if the allocation is not the default (1,1).
        const int alloc_width = m_widget->allocation.width;
        if (alloc_width == 1)
            m_widget->allocation.width = 2;
        gtk_widget_realize(m_widget);
        if (alloc_width == 1)
            m_widget->allocation.width = 1;

        // send _NET_REQUEST_FRAME_EXTENTS
        XClientMessageEvent xevent;
        memset(&xevent, 0, sizeof(xevent));
        xevent.type = ClientMessage;
        xevent.window = gdk_x11_drawable_get_xid(m_widget->window);
        xevent.message_type = gdk_x11_atom_to_xatom_for_display(
            gdk_drawable_get_display(m_widget->window),
            gdk_atom_intern("_NET_REQUEST_FRAME_EXTENTS", false));
        xevent.format = 32;
        Display* display = gdk_x11_drawable_get_xdisplay(m_widget->window);
        XSendEvent(display, DefaultRootWindow(display), false,
            SubstructureNotifyMask | SubstructureRedirectMask,
            (XEvent*)&xevent);

        // defer calling gtk_widget_show()
        m_isShown = true;
        return true;
    }

    if (show && !GTK_WIDGET_REALIZED(m_widget))
    {
        // size_allocate signals occur in reverse order (bottom to top).
        // Things work better if the initial wxSizeEvents are sent (from the
        // top down), before the initial size_allocate signals occur.
        wxSizeEvent event(GetSize(), GetId());
        event.SetEventObject(this);
        HandleWindowEvent(event);
    }

    bool change = wxTopLevelWindowBase::Show(show);

    if (change && !show)
    {
        // make sure window has a non-default position, so when it is shown
        // again, it won't be repositioned by WM as if it were a new window
        // Note that this must be done _after_ the window is hidden.
        gtk_window_move((GtkWindow*)m_widget, m_x, m_y);
    }

    return change;
}

void wxTopLevelWindowGTK::Raise()
{
    gtk_window_present( GTK_WINDOW( m_widget ) );
}

void wxTopLevelWindowGTK::DoMoveWindow(int WXUNUSED(x), int WXUNUSED(y), int WXUNUSED(width), int WXUNUSED(height) )
{
    wxFAIL_MSG( wxT("DoMoveWindow called for wxTopLevelWindowGTK") );
}

// ----------------------------------------------------------------------------
// window geometry
// ----------------------------------------------------------------------------

void wxTopLevelWindowGTK::GTKDoGetSize(int *width, int *height) const
{
    wxSize size(m_width, m_height);
    size -= m_decorSize;
    if (size.x < 0) size.x = 0;
    if (size.y < 0) size.y = 0;
    if (width)  *width  = size.x;
    if (height) *height = size.y;
}

void wxTopLevelWindowGTK::DoSetSize( int x, int y, int width, int height, int sizeFlags )
{
    wxCHECK_RET( m_widget, wxT("invalid frame") );

    m_deferShowAllowed = true;

    // deal with the position first
    int old_x = m_x;
    int old_y = m_y;

    if ( !(sizeFlags & wxSIZE_ALLOW_MINUS_ONE) )
    {
        // -1 means "use existing" unless the flag above is specified
        if ( x != -1 )
            m_x = x;
        if ( y != -1 )
            m_y = y;
    }
    else // wxSIZE_ALLOW_MINUS_ONE
    {
        m_x = x;
        m_y = y;
    }

    if ( m_x != old_x || m_y != old_y )
    {
        gtk_window_move( GTK_WINDOW(m_widget), m_x, m_y );
    }

    const wxSize oldSize(m_width, m_height);
    if (width >= 0)
        m_width = width;
    if (height >= 0)
        m_height = height;
    ConstrainSize();
    if (m_width != oldSize.x || m_height != oldSize.y)
    {
        int w, h;
        GTKDoGetSize(&w, &h);
        gtk_window_resize(GTK_WINDOW(m_widget), w, h);

        GetClientSize(&m_oldClientWidth, &m_oldClientHeight);
        wxSizeEvent event(GetSize(), GetId());
        event.SetEventObject(this);
        HandleWindowEvent(event);
    }
}

void wxTopLevelWindowGTK::DoSetClientSize(int width, int height)
{
    // Since client size is being explicitly set, don't change it later
    m_deferShowAllowed = false;

    wxTopLevelWindowBase::DoSetClientSize(width, height);
}

void wxTopLevelWindowGTK::DoGetClientSize( int *width, int *height ) const
{
    wxASSERT_MSG(m_widget, wxT("invalid frame"));

    if ( IsIconized() )
    {
        // for consistency with wxMSW, client area is supposed to be empty for
        // the iconized windows
        if ( width )
            *width = 0;
        if ( height )
            *height = 0;
    }
    else
    {
        GTKDoGetSize(width, height);
    }
}

void wxTopLevelWindowGTK::DoSetSizeHints( int minW, int minH,
                                          int maxW, int maxH,
                                          int incW, int incH )
{
    wxTopLevelWindowBase::DoSetSizeHints( minW, minH, maxW, maxH, incW, incH );

    const wxSize minSize = GetMinSize();
    const wxSize maxSize = GetMaxSize();
    GdkGeometry hints;
    int hints_mask = 0;
    if (minSize.x > 0 || minSize.y > 0)
    {
        hints_mask |= GDK_HINT_MIN_SIZE;
        hints.min_width = minSize.x - m_decorSize.x;
        if (hints.min_width < 0)
            hints.min_width = 0;
        hints.min_height = minSize.y - m_decorSize.y;
        if (hints.min_height < 0)
            hints.min_height = 0;
    }
    if (maxSize.x > 0 || maxSize.y > 0)
    {
        hints_mask |= GDK_HINT_MAX_SIZE;
        hints.max_width = maxSize.x - m_decorSize.x;
        if (hints.max_width < 0)
            hints.max_width = INT_MAX;
        hints.max_height = maxSize.y - m_decorSize.y;
        if (hints.max_height < 0)
            hints.max_height = INT_MAX;
    }
    if (incW > 0 || incH > 0)
    {
        hints_mask |= GDK_HINT_RESIZE_INC;
        hints.width_inc  = incW > 0 ? incW : 1;
        hints.height_inc = incH > 0 ? incH : 1;
    }
    gtk_window_set_geometry_hints(
        (GtkWindow*)m_widget, NULL, &hints, (GdkWindowHints)hints_mask);
}

void wxTopLevelWindowGTK::GTKUpdateDecorSize(const wxSize& decorSize)
{
    if (!IsMaximized() && !IsFullScreen())
        GetCachedDecorSize() = decorSize;
    if (m_updateDecorSize && m_decorSize != decorSize)
    {
        const wxSize diff = decorSize - m_decorSize;
        m_decorSize = decorSize;
        bool resized = false;
        if (m_deferShow)
        {
            // keep overall size unchanged by shrinking m_widget
            int w, h;
            GTKDoGetSize(&w, &h);
            // but not if size would be less than minimum, it won't take effect
            const wxSize minSize = GetMinSize();
            if (w >= minSize.x && h >= minSize.y)
            {
                gtk_window_resize(GTK_WINDOW(m_widget), w, h);
                resized = true;
            }
        }
        if (!resized)
        {
            // adjust overall size to match change in frame extents
            m_width  += diff.x;
            m_height += diff.y;
            if (m_width  < 0) m_width  = 0;
            if (m_height < 0) m_height = 0;
            m_oldClientWidth = 0;
            gtk_widget_queue_resize(m_wxwindow);
        }
    }
    if (m_deferShow)
    {
        // gtk_widget_show() was deferred, do it now
        m_deferShow = false;
        GetClientSize(&m_oldClientWidth, &m_oldClientHeight);
        wxSizeEvent sizeEvent(GetSize(), GetId());
        sizeEvent.SetEventObject(this);
        HandleWindowEvent(sizeEvent);

        gtk_widget_show(m_widget);

        wxShowEvent showEvent(GetId(), true);
        showEvent.SetEventObject(this);
        HandleWindowEvent(showEvent);
    }
}

wxSize& wxTopLevelWindowGTK::GetCachedDecorSize()
{
    static wxSize size[8];

    int index = 0;
    // title bar
    if (m_gdkDecor & (GDK_DECOR_MENU | GDK_DECOR_MINIMIZE | GDK_DECOR_MAXIMIZE | GDK_DECOR_TITLE))
        index = 1;
    // border
    if (m_gdkDecor & GDK_DECOR_BORDER)
        index |= 2;
    // utility window decor can be different
    if (m_windowStyle & wxFRAME_TOOL_WINDOW)
        index |= 4;
    return size[index];
}

void wxTopLevelWindowGTK::OnInternalIdle()
{
    wxWindow::OnInternalIdle();

    // Synthetize activate events.
    if ( g_sendActivateEvent != -1 )
    {
        bool activate = g_sendActivateEvent != 0;

        // if (!activate) wxPrintf( wxT("de") );
        // wxPrintf( wxT("activate\n") );

        // do it only once
        g_sendActivateEvent = -1;

        wxTheApp->SetActive(activate, (wxWindow *)g_lastActiveFrame);
    }
}

// ----------------------------------------------------------------------------
// frame title/icon
// ----------------------------------------------------------------------------

void wxTopLevelWindowGTK::SetTitle( const wxString &title )
{
    wxASSERT_MSG( (m_widget != NULL), wxT("invalid frame") );

    if ( title == m_title )
        return;

    m_title = title;

    gtk_window_set_title( GTK_WINDOW(m_widget), wxGTK_CONV( title ) );
}

void wxTopLevelWindowGTK::SetIcons( const wxIconBundle &icons )
{
    wxASSERT_MSG( (m_widget != NULL), wxT("invalid frame") );

    wxTopLevelWindowBase::SetIcons( icons );

    // Setting icons before window is realized can cause a GTK assertion if
    // another TLW is realized before this one, and it has this one as it's
    // transient parent. The life demo exibits this problem.
    if (GTK_WIDGET_REALIZED(m_widget))
    {
        GList* list = NULL;
        for (size_t i = icons.GetIconCount(); i--;)
            list = g_list_prepend(list, icons.GetIconByIndex(i).GetPixbuf());
        gtk_window_set_icon_list(GTK_WINDOW(m_widget), list);
        g_list_free(list);
    }
}

// ----------------------------------------------------------------------------
// frame state: maximized/iconized/normal
// ----------------------------------------------------------------------------

void wxTopLevelWindowGTK::Maximize(bool maximize)
{
    if (maximize)
        gtk_window_maximize( GTK_WINDOW( m_widget ) );
    else
        gtk_window_unmaximize( GTK_WINDOW( m_widget ) );
}

bool wxTopLevelWindowGTK::IsMaximized() const
{
    if(!m_widget->window)
        return false;

    return gdk_window_get_state(m_widget->window) & GDK_WINDOW_STATE_MAXIMIZED;
}

void wxTopLevelWindowGTK::Restore()
{
    // "Present" seems similar enough to "restore"
    gtk_window_present( GTK_WINDOW( m_widget ) );
}

void wxTopLevelWindowGTK::Iconize( bool iconize )
{
    if (iconize)
        gtk_window_iconify( GTK_WINDOW( m_widget ) );
    else
        gtk_window_deiconify( GTK_WINDOW( m_widget ) );
}

bool wxTopLevelWindowGTK::IsIconized() const
{
    return m_isIconized;
}

void wxTopLevelWindowGTK::SetIconizeState(bool iconize)
{
    if ( iconize != m_isIconized )
    {
        m_isIconized = iconize;
        (void)SendIconizeEvent(iconize);
    }
}

void wxTopLevelWindowGTK::AddGrab()
{
    if (!m_grabbed)
    {
        m_grabbed = true;
        gtk_grab_add( m_widget );
        wxGUIEventLoop().Run();
        gtk_grab_remove( m_widget );
    }
}

void wxTopLevelWindowGTK::RemoveGrab()
{
    if (m_grabbed)
    {
        gtk_main_quit();
        m_grabbed = false;
    }
}


// helper
static bool do_shape_combine_region(GdkWindow* window, const wxRegion& region)
{
    if (window)
    {
        if (region.IsEmpty())
        {
            gdk_window_shape_combine_mask(window, NULL, 0, 0);
        }
        else
        {
            gdk_window_shape_combine_region(window, region.GetRegion(), 0, 0);
            return true;
        }
    }
    return false;
}


bool wxTopLevelWindowGTK::SetShape(const wxRegion& region)
{
    wxCHECK_MSG( HasFlag(wxFRAME_SHAPED), false,
                 _T("Shaped windows must be created with the wxFRAME_SHAPED style."));

    if ( GTK_WIDGET_REALIZED(m_widget) )
    {
        if ( m_wxwindow )
            do_shape_combine_region(m_wxwindow->window, region);

        return do_shape_combine_region(m_widget->window, region);
    }
    else // not realized yet
    {
        // store the shape to set, it will be really set once we're realized
        m_shape = region;

        // we don't know if we're going to succeed or fail, be optimistic by
        // default
        return true;
    }
}

bool wxTopLevelWindowGTK::IsActive()
{
    return (this == (wxTopLevelWindowGTK*)g_activeFrame);
}

void wxTopLevelWindowGTK::RequestUserAttention(int flags)
{
    bool new_hint_value = false;

    // FIXME: This is a workaround to focus handling problem
    // If RequestUserAttention is called for example right after a wxSleep, OnInternalIdle
    // hasn't yet been processed, and the internal focus system is not up to date yet.
    // YieldFor(wxEVT_CATEGORY_UI) ensures the processing of it (hopefully it
    // won't have side effects) - MR
    wxEventLoopBase::GetActive()->YieldFor(wxEVT_CATEGORY_UI);

    if(m_urgency_hint >= 0)
        g_source_remove(m_urgency_hint);

    m_urgency_hint = -2;

    if( GTK_WIDGET_REALIZED(m_widget) && !IsActive() )
    {
        new_hint_value = true;

        if (flags & wxUSER_ATTENTION_INFO)
        {
            m_urgency_hint = g_timeout_add(5000, (GSourceFunc)gtk_frame_urgency_timer_callback, this);
        } else {
            m_urgency_hint = -1;
        }
    }

#if GTK_CHECK_VERSION(2,7,0)
    if(!gtk_check_version(2,7,0))
        gtk_window_set_urgency_hint(GTK_WINDOW( m_widget ), new_hint_value);
    else
#endif
        wxgtk_window_set_urgency_hint(GTK_WINDOW( m_widget ), new_hint_value);
}

void wxTopLevelWindowGTK::SetWindowStyleFlag( long style )
{
    // Store which styles were changed
    long styleChanges = style ^ m_windowStyle;

    // Process wxWindow styles. This also updates the internal variable
    // Therefore m_windowStyle bits carry now the _new_ style values
    wxWindow::SetWindowStyleFlag(style);

    // just return for now if widget does not exist yet
    if (!m_widget)
        return;

    if ( styleChanges & wxSTAY_ON_TOP )
    {
        gtk_window_set_keep_above(GTK_WINDOW(m_widget),
                                  m_windowStyle & wxSTAY_ON_TOP);
    }

    if ( styleChanges & wxFRAME_NO_TASKBAR )
    {
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(m_widget),
                                         m_windowStyle & wxFRAME_NO_TASKBAR);
    }
}

/* Get the X Window between child and the root window.
   This should usually be the WM managed XID */
static Window wxGetTopmostWindowX11(Display *dpy, Window child)
{
    Window root, parent;
    Window* children;
    unsigned int nchildren;

    XQueryTree(dpy, child, &root, &parent, &children, &nchildren);
    XFree(children);

    while (parent != root) {
        child = parent;
        XQueryTree(dpy, child, &root, &parent, &children, &nchildren);
        XFree(children);
    }

    return child;
}

bool wxTopLevelWindowGTK::SetTransparent(wxByte alpha)
{
    if (!m_widget || !m_widget->window)
        return false;

    Display* dpy = GDK_WINDOW_XDISPLAY (m_widget->window);
    // We need to get the X Window that has the root window as the immediate parent
    // and m_widget->window as a child. This should be the X Window that the WM manages and
    // from which the opacity property is checked from.
    Window win = wxGetTopmostWindowX11(dpy, GDK_WINDOW_XID (m_widget->window));


    // Using pure Xlib to not have a GTK version check mess due to gtk2.0 not having GdkDisplay
    if (alpha == 0xff)
        XDeleteProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False));
    else
    {
        long opacity = alpha * 0x1010101L;
        XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False),
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *) &opacity, 1L);
    }
    XSync(dpy, False);
    return true;
}

bool wxTopLevelWindowGTK::CanSetTransparent()
{
    // allow to override automatic detection as it's far from perfect
    static const wxChar *SYSOPT_TRANSPARENT = wxT("gtk.tlw.can-set-transparent");
    if ( wxSystemOptions::HasOption(SYSOPT_TRANSPARENT) )
    {
        return wxSystemOptions::GetOptionInt(SYSOPT_TRANSPARENT) != 0;
    }

#if GTK_CHECK_VERSION(2,10,0)
    if (!gtk_check_version(2,10,0))
    {
        return (gtk_widget_is_composited (m_widget));
    }
    else
#endif // In case of lower versions than gtk+-2.10.0 we could look for _NET_WM_CM_Sn ourselves
    {
        return false;
    }

#if 0 // Don't be optimistic here for the sake of wxAUI
    int opcode, event, error;
    // Check for the existence of a RGBA visual instead?
    return XQueryExtension(gdk_x11_get_default_xdisplay (),
                           "Composite", &opcode, &event, &error);
#endif
}
