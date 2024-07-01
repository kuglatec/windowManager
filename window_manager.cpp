#include "window_manager.hpp"
extern "C" {
#include <X11/Xutil.h>
}
#include <cstring>
#include <algorithm>
#include <glog/logging.h>
#include "util.hpp"
#include <iostream>
using ::std::max;
using ::std::mutex;
using ::std::string;
using ::std::unique_ptr;

bool WindowManager::wm_detected_;
mutex WindowManager::wm_detected_mutex_;

unique_ptr<WindowManager> WindowManager::Create(const string& display_str) {
  // 1. Open X display.
  const char* display_c_str =
        display_str.empty() ? nullptr : display_str.c_str();
  Display* display = XOpenDisplay(display_c_str);
  if (display == nullptr) {
    LOG(ERROR) << "Failed to open X display " << XDisplayName(display_c_str);
    return nullptr;
  }
  // 2. Construct WindowManager instance.
  return unique_ptr<WindowManager>(new WindowManager(display));
}

struct Tuple {
    int x;
    int y;
};

WindowManager::WindowManager(Display* display)
    : display_(CHECK_NOTNULL(display)),
      root_(DefaultRootWindow(display_)),
      WM_PROTOCOLS(XInternAtom(display_, "WM_PROTOCOLS", false)),
      WM_DELETE_WINDOW(XInternAtom(display_, "WM_DELETE_WINDOW", false)) {
}

WindowManager::~WindowManager() {
  XCloseDisplay(display_);
}

void WindowManager::Run() {
  {
    ::std::lock_guard<mutex> lock(wm_detected_mutex_);

    wm_detected_ = false;
    XSetErrorHandler(&WindowManager::OnWMDetected);
    XSelectInput(
        display_,
        root_,
        SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(display_, false);
    if (wm_detected_) {
      LOG(ERROR) << "Detected another window manager on display "
                 << XDisplayString(display_);
      return;
    }
  }
  XSetErrorHandler(&WindowManager::OnXError);
  //   c. Grab X server to prevent windows from changing under us.
  XGrabServer(display_);
  //   d. Reparent existing top-level windows.
  //     i. Query existing top-level windows.
  Window returned_root, returned_parent;
  Window* top_level_windows;
  unsigned int num_top_level_windows;
  CHECK(XQueryTree(
      display_,
      root_,
      &returned_root,
      &returned_parent,
      &top_level_windows,
      &num_top_level_windows));
  CHECK_EQ(returned_root, root_);
  //     ii. Frame each top-level window.
  for (unsigned int i = 0; i < num_top_level_windows; ++i) {
    Frame(top_level_windows[i], true);
  }
  //     iii. Free top-level window array.
  XFree(top_level_windows);
  //   e. Ungrab X server.
  XUngrabServer(display_);

  // 2. Main event loop.
  for (;;) {
    // 1. Get next event.
    //
    WindowManager::getCursor(display_);
    XEvent e;
    XNextEvent(display_, &e);
    LOG(INFO) << "Received event: " << ToString(e);

    // 2. Dispatch event.
    switch (e.type) {
      case CreateNotify:
        OnCreateNotify(e.xcreatewindow);
        break;
      case DestroyNotify:
        OnDestroyNotify(e.xdestroywindow);
        break;
      case ReparentNotify:
        OnReparentNotify(e.xreparent);
        break;
      case MapNotify:
        OnMapNotify(e.xmap);
        break;
      case UnmapNotify:
        OnUnmapNotify(e.xunmap);
        break;
      case ConfigureNotify:
        OnConfigureNotify(e.xconfigure);
        break;
      case MapRequest:
        OnMapRequest(e.xmaprequest);
        break;
      case ConfigureRequest:
        OnConfigureRequest(e.xconfigurerequest);
        break;
      case ButtonPress:
        OnButtonPress(e.xbutton);
        break;
      case ButtonRelease:
        OnButtonRelease(e.xbutton);
        break;
      case MotionNotify:
        // Skip any already pending motion events.
        while (XCheckTypedWindowEvent(
            display_, e.xmotion.window, MotionNotify, &e)) {}
        OnMotionNotify(e.xmotion);
        break;
      case KeyPress:
        OnKeyPress(e.xkey);
        break;
      case KeyRelease:
        OnKeyRelease(e.xkey);
        break;
      default:
        LOG(WARNING) << "Ignored event";
    }
  }
}

void WindowManager::Frame(Window w, bool was_created_before_window_manager) {
  // Visual properties of the frame to create.
  const unsigned int BORDER_WIDTH = 3;
  const unsigned long BORDER_COLOR = 0xff0000;
  const unsigned long BG_COLOR = 0x0000ff;

  // We shouldn't be framing windows we've already framed.
  CHECK(!clients_.count(w));

  // 1. Retrieve attributes of window to frame.
  XWindowAttributes x_window_attrs;
  CHECK(XGetWindowAttributes(display_, w, &x_window_attrs));

  // 2. If window was created before window manager started, we should frame
  // it only if it is visible and doesn't set override_redirect.
  if (was_created_before_window_manager) {
    if (x_window_attrs.override_redirect ||
        x_window_attrs.map_state != IsViewable) {
      return;
    }
  }

  // 3. Create frame.
  const Window frame = XCreateSimpleWindow(
      display_,
      root_,
      x_window_attrs.x,
      x_window_attrs.y,
      x_window_attrs.width,
      x_window_attrs.height,
      BORDER_WIDTH,
      BORDER_COLOR,
      BG_COLOR);
  // 4. Select events on frame.
  XSelectInput(
      display_,
      frame,
      SubstructureRedirectMask | SubstructureNotifyMask);
  // 5. Add client to save set, so that it will be restored and kept alive if we
  // crash.
  XAddToSaveSet(display_, w);
  // 6. Reparent client window.
  XReparentWindow(
      display_,
      w,
      frame,
      0, 0);  // Offset of client window within frame.
  // 7. Map frame.
  XMapWindow(display_, frame);
  // 8. Save frame handle.
  clients_[w] = frame;
  // 9. Grab universal window management actions on client window.
  //   a. Move windows with alt + left button.
  XGrabButton(
      display_,
      Button1,
      Mod1Mask,
      w,
      false,
      ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
      GrabModeAsync,
      GrabModeAsync,
      None,
      None);
  //   b. Resize windows with alt + right button.
  XGrabButton(
      display_,
      Button3,
      Mod1Mask,
      w,
      false,
      ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
      GrabModeAsync,
      GrabModeAsync,
      None,
      None);
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_Q),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  //   d. Switch windows with alt + tab.
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_Return),
      Mod1Mask,
      root_,
      false,
      GrabModeAsync,
      GrabModeAsync);

  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_Tab),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  XGrabKey( //Tile windows
      display_,
      XKeysymToKeycode(display_, XK_T),
      Mod1Mask,
      root_,
      false,
      GrabModeAsync,
      GrabModeAsync);
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_F),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_Right),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_Left),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_D),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  XGrabKey(
      display_,
      XKeysymToKeycode(display_, XK_A),
      Mod1Mask,
      w,
      false,
      GrabModeAsync,
      GrabModeAsync);
  LOG(INFO) << "Framed window " << w << " [" << frame << "]";
}

void WindowManager::Unframe(Window w) {
  CHECK(clients_.count(w));

  // We reverse the steps taken in Frame().
  const Window frame = clients_[w];
  // 1. Unmap frame.
  XUnmapWindow(display_, frame);
  // 2. Reparent client window.
  XReparentWindow(
      display_,
      w,
      root_,
      0, 0);  // Offset of client window within root.
  // 3. Remove client window from save set, as it is now unrelated to us.
  XRemoveFromSaveSet(display_, w);
  // 4. Destroy frame.
  XDestroyWindow(display_, frame);
  // 5. Drop reference to frame handle.
  clients_.erase(w);

  LOG(INFO) << "Unframed window " << w << " [" << frame << "]";
}

void WindowManager::OnCreateNotify(const XCreateWindowEvent& e) {}

void WindowManager::OnDestroyNotify(const XDestroyWindowEvent& e) {}

void WindowManager::OnReparentNotify(const XReparentEvent& e) {}

void WindowManager::OnMapNotify(const XMapEvent& e) {}

void WindowManager::OnUnmapNotify(const XUnmapEvent& e) {
  // If the window is a client window we manage, unframe it upon UnmapNotify. We
  // need the check because we will receive an UnmapNotify event for a frame
  // window we just destroyed ourselves.
  if (!clients_.count(e.window)) {
    LOG(INFO) << "Ignore UnmapNotify for non-client window " << e.window;
    return;
  }

  // Ignore event if it is triggered by reparenting a window that was mapped
  // before the window manager started.
  //
  // Since we receive UnmapNotify events from the SubstructureNotify mask, the
  // event attribute specifies the parent window of the window that was
  // unmapped. This means that an UnmapNotify event from a normal client window
  // should have this attribute set to a frame window we maintain. Only an
  // UnmapNotify event triggered by reparenting a pre-existing window will have
  // this attribute set to the root window.
  if (e.event == root_) {
    LOG(INFO) << "Ignore UnmapNotify for reparented pre-existing window "
              << e.window;
    return;
  }

  Unframe(e.window);
}

void WindowManager::OnConfigureNotify(const XConfigureEvent& e) {}

void WindowManager::OnMapRequest(const XMapRequestEvent& e) {
  // 1. Frame or re-frame window.
  Frame(e.window, false);
  // 2. Actually map window.
  XMapWindow(display_, e.window);
  XWindowChanges changes;
  Tuple CursorPosition = getCursor(display_);
  
  int screenWidth = GetScreenWidth(display_);
  int screenHeight = GetScreenHeight(display_);
  
  Window returned_root, returned_parent;
  Window* top_level_windows;
  unsigned int num_top_level_windows;
  
  const Window frame = clients_[e.window]; //get frame of mapped window to move it
  Tuple crsr = getCursor(display_);
  XWindowAttributes attrs;
  XGetWindowAttributes(display_, frame, &attrs);
  XMoveWindow(display_, frame, crsr.x - (attrs.width / 2), crsr.y - (attrs.height / 2));
}

void WindowManager::iterateWindows(int num_top_level_windows, Window* top_level_windows) {
  for (int i = 0; i < num_top_level_windows; i++) {
    XResizeWindow(display_, top_level_windows[i], GetScreenWidth(display_) / (num_top_level_windows),  GetScreenHeight(display_));
    XMoveWindow(display_, top_level_windows[i], (GetScreenWidth(display_) / num_top_level_windows) * i, 0);
    Window root_return;
    Window parent_return;
    Window *children_return;
    unsigned int nchildren_return;
    XQueryTree(
        display_,
        top_level_windows[i],
        &root_return,
        &parent_return,
        &children_return,
        &nchildren_return);
    XResizeWindow(display_, children_return[0], GetScreenWidth(display_) / (num_top_level_windows), GetScreenHeight(display_));
    XFree(children_return);
    printf("\n\n Resized window number %d to %d X %d", i, GetScreenWidth(display_) / (num_top_level_windows), GetScreenHeight(display_));
  }
}

void WindowManager::OnConfigureRequest(const XConfigureRequestEvent& e) {
  XWindowChanges changes;
  changes.x = e.x;
  changes.y = e.y;
  changes.width = e.width;
  changes.height = e.height;
  changes.border_width = e.border_width;
  changes.sibling = e.above;
  changes.stack_mode = e.detail;
  if (clients_.count(e.window)) {
    const Window frame = clients_[e.window];
    XConfigureWindow(display_, frame, e.value_mask, &changes);
    LOG(INFO) << "Resize [" << frame << "] to " << Size<int>(e.width, e.height);
  }
  XConfigureWindow(display_, e.window, e.value_mask, &changes);
  LOG(INFO) << "Resize " << e.window << " to " << Size<int>(e.width, e.height);
}

void WindowManager::OnButtonPress(const XButtonEvent& e) {
  CHECK(clients_.count(e.window));
  const Window frame = clients_[e.window];

  // 1. Save initial cursor position.
  drag_start_pos_ = Position<int>(e.x_root, e.y_root);

  // 2. Save initial window info.
  Window returned_root;
  int x, y;
  unsigned width, height, border_width, depth;
  CHECK(XGetGeometry(
      display_,
      frame,
      &returned_root,
      &x, &y,
      &width, &height,
      &border_width,
      &depth));
  drag_start_frame_pos_ = Position<int>(x, y);
  drag_start_frame_size_ = Size<int>(width, height);

  // 3. Raise clicked window to top.
  XRaiseWindow(display_, frame);
}

void WindowManager::OnButtonRelease(const XButtonEvent& e) {}

void WindowManager::OnMotionNotify(const XMotionEvent& e) {
  CHECK(clients_.count(e.window));
  const Window frame = clients_[e.window];
  const Position<int> drag_pos(e.x_root, e.y_root);
  const Vector2D<int> delta = drag_pos - drag_start_pos_;

  if (e.state & Button1Mask ) {
    // alt + left button: Move window.
    const Position<int> dest_frame_pos = drag_start_frame_pos_ + delta;
    std::cout << "\n\n";   
    std::cout << dest_frame_pos.x;
    XMoveWindow(
        display_,
        frame,
        dest_frame_pos.x, dest_frame_pos.y);
  } else if (e.state & Button3Mask) {
    // alt + right button: Resize window.
    // Window dimensions cannot be negative.
    const Vector2D<int> size_delta(
        max(delta.x, -drag_start_frame_size_.width),
        max(delta.y, -drag_start_frame_size_.height));
    const Size<int> dest_frame_size = drag_start_frame_size_ + size_delta;
    // 1. Resize frame.
    XResizeWindow(
        display_,
        frame,
        dest_frame_size.width, dest_frame_size.height);
    // 2. Resize client window.
    XResizeWindow(
        display_,
        e.window,
        dest_frame_size.width, dest_frame_size.height);
  }
}

void WindowManager::OnKeyPress(const XKeyEvent& e) {
  if ((e.state & Mod1Mask) &&
      (e.keycode == XKeysymToKeycode(display_, XK_Q))) {
     
     LOG(INFO) << "Killing window " << e.window;
     XKillClient(display_, e.window);
     Window returned_root, returned_parent;
     Window* top_level_windows;
     unsigned int num_top_level_windows;
    printf("\nKilled window\n");

    }
 
  else if ((e.state & Mod1Mask) &&
      (e.keycode == XKeysymToKeycode(display_, XK_Right))) {
      Window returned_root, returned_parent;
      Window* top_level_windows;
      unsigned int num_top_level_windows;	
      CHECK(XQueryTree(
      display_,
      root_,
      &returned_root,
      &returned_parent,
      &top_level_windows,
      &num_top_level_windows));
      XWindowAttributes wattrs;
      XGetWindowAttributes(display_, clients_[e.window], &wattrs);
      
      for (int i = 0; i < num_top_level_windows; i++) {
        XWindowAttributes nwattrs;
        XGetWindowAttributes(display_, top_level_windows[i], &nwattrs);
        if (nwattrs.x == wattrs.x + wattrs.width && nwattrs.width > 100) {
          printf("Width: \n \n --\n%d\n", nwattrs.width);
          XMoveWindow(display_, top_level_windows[i], nwattrs.x + 100, nwattrs.y);
          printf("\nHere\n");
          XResizeWindow(display_, top_level_windows[i], nwattrs.width - 100, nwattrs.height);
 	        XResizeWindow(display_, clients_[e.window], wattrs.width + 100, wattrs.height);
     	    XResizeWindow(display_, e.window, wattrs.width + 100, wattrs.height);
       	  XRaiseWindow(display_, clients_[e.window]);
        }

      }
    

  }

  else if ((e.state & Mod1Mask) &&
      (e.keycode == XKeysymToKeycode(display_, XK_Left))) {
      Window returned_root, returned_parent;
      Window* top_level_windows;
      unsigned int num_top_level_windows;	
      CHECK(XQueryTree(
      display_,
      root_,
      &returned_root,
      &returned_parent,
      &top_level_windows,
      &num_top_level_windows));
      XWindowAttributes wattrs;
      XGetWindowAttributes(display_, clients_[e.window], &wattrs);
      printf("\nWidth: %d", wattrs.width);
      if (wattrs.width > 100) {
       for (int i = 0; i < num_top_level_windows; i++) {
        XWindowAttributes nwattrs;
        XGetWindowAttributes(display_, top_level_windows[i], &nwattrs);
        if (nwattrs.x == wattrs.x + wattrs.width) {
          printf("Width: \n \n --\n%d\n", nwattrs.width);
          XMoveWindow(display_, top_level_windows[i], nwattrs.x - 100, nwattrs.y);
          printf("\nHere\n");
          Window root_return, parent_return;
          Window *children_return;
          unsigned int nchildren_return;
          XQueryTree(display_, top_level_windows[i], &root_return, &parent_return, &children_return, &nchildren_return);
          XResizeWindow(display_, children_return[0], nwattrs.width + 100, nwattrs.height);
          XResizeWindow(display_, top_level_windows[i], nwattrs.width + 100, nwattrs.height);
	  XResizeWindow(display_, clients_[e.window], wattrs.width - 100, wattrs.height);
      	  XResizeWindow(display_, e.window, wattrs.width - 100, wattrs.height);
        }

      }
      printf("\n WidthofWin: %d", wattrs.width - 100);
      }
  }

  else if ((e.state & Mod1Mask) && //swap window to right
      (e.keycode == XKeysymToKeycode(display_, XK_D))){
      Window returned_root, returned_parent;
      Window* top_level_windows;
      unsigned int num_top_level_windows;
      CHECK(XQueryTree(
          display_,
          root_,
          &returned_root,
          &returned_parent,
          &top_level_windows,
          &num_top_level_windows));
      for (int i = 0; i < num_top_level_windows; i++) {
        XWindowAttributes wattrs, nwattrs;
        XGetWindowAttributes(display_, clients_[e.window], &wattrs);
        XGetWindowAttributes(display_, top_level_windows[i], &nwattrs);
        if (nwattrs.x == wattrs.x + wattrs.width) {
             Window root_return;
             Window parent_return;
             Window *children_return;
             unsigned int nchildren_return;

            XQueryTree(display_, top_level_windows[i], &root_return, &parent_return, &children_return, &nchildren_return);
            XMoveWindow(display_, clients_[e.window], nwattrs.x, nwattrs.y);
            XMoveWindow(display_, top_level_windows[i], wattrs.x, wattrs.y);
            XResizeWindow(display_, clients_[e.window], nwattrs.width, nwattrs.height);
            XResizeWindow(display_, e.window, nwattrs.width, nwattrs.height);
            XResizeWindow(display_, top_level_windows[i], wattrs.width, wattrs.height);
            XResizeWindow(display_, children_return[0], wattrs.width, wattrs.height);
            break;

        } 
      }

  }
	
  else if ((e.state & Mod1Mask) && //swap window to right
      (e.keycode == XKeysymToKeycode(display_, XK_A))){
      Window returned_root, returned_parent;
      Window* top_level_windows;
      unsigned int num_top_level_windows;
      CHECK(XQueryTree(
          display_,
          root_,
          &returned_root,
          &returned_parent,
          &top_level_windows,
          &num_top_level_windows));
      for (int i = 0; i < num_top_level_windows; i++) {
        XWindowAttributes wattrs, nwattrs;
        XGetWindowAttributes(display_, clients_[e.window], &wattrs);
        XGetWindowAttributes(display_, top_level_windows[i], &nwattrs);
        if (nwattrs.x + nwattrs.width == wattrs.x) {
             Window root_return;
             Window parent_return;
             Window *children_return;
             unsigned int nchildren_return;

            XQueryTree(display_, top_level_windows[i], &root_return, &parent_return, &children_return, &nchildren_return);
            XMoveWindow(display_, clients_[e.window], nwattrs.x, nwattrs.y);
            XMoveWindow(display_, top_level_windows[i], wattrs.x, wattrs.y);
            XResizeWindow(display_, clients_[e.window], nwattrs.width, nwattrs.height);
            XResizeWindow(display_, e.window, nwattrs.width, nwattrs.height);
            XResizeWindow(display_, top_level_windows[i], wattrs.width, wattrs.height);
            XResizeWindow(display_, children_return[0], wattrs.width, wattrs.height);
            break;

        } 
      }

  }

  else if ((e.state & Mod1Mask) &&
      (e.keycode == XKeysymToKeycode(display_, XK_T))){
      
      Window returned_root, returned_parent;
      Window* top_level_windows;
      unsigned int num_top_level_windows;
      XQueryTree(
        display_,
        root_,
        &returned_root,
        &returned_parent,
        &top_level_windows,
        &num_top_level_windows);
      iterateWindows(num_top_level_windows, top_level_windows);
      XFree(top_level_windows);
  }
  else if ((e.state & Mod1Mask) &&
      (e.keycode == XKeysymToKeycode(display_, XK_Return))){
      system("rofi -show drun");

  }
  else if ((e.state & Mod1Mask) &&
             (e.keycode == XKeysymToKeycode(display_, XK_Tab))) {
    // alt + tab: Switch window.
    // 1. Find next window.
    auto i = clients_.find(e.window);
    CHECK(i != clients_.end());
    ++i;
    if (i == clients_.end()) {
      i = clients_.begin();
    }
    // 2. Raise and set focus.
    XRaiseWindow(display_, i->second);
    XSetInputFocus(display_, i->first, RevertToPointerRoot, CurrentTime);
  }
}

void WindowManager::OnKeyRelease(const XKeyEvent& e) {}

int WindowManager::OnXError(Display* display, XErrorEvent* e) {
  const int MAX_ERROR_TEXT_LENGTH = 1024;
  char error_text[MAX_ERROR_TEXT_LENGTH];
  XGetErrorText(display, e->error_code, error_text, sizeof(error_text));
  LOG(ERROR) << "Received X error:\n"
             << "    Request: " << int(e->request_code)
             << " - " << XRequestCodeToString(e->request_code) << "\n"
             << "    Error code: " << int(e->error_code)
             << " - " << error_text << "\n"
             << "    Resource ID: " << e->resourceid;
  // The return value is ignored.
  return 0;
}

int WindowManager::OnWMDetected(Display* display, XErrorEvent* e) {
  CHECK_EQ(static_cast<int>(e->error_code), BadAccess);
  // Set flag.
  wm_detected_ = true;
  return 0;
}
int WindowManager::GetScreenHeight(Display* display) {
    Screen *screen;

    display = XOpenDisplay(NULL);

    screen = ScreenOfDisplay(display, 0); //No multi monitor support yet 
    
    return screen->height;

}
int WindowManager::GetScreenWidth(Display* display) {
  Screen *screen;
  display = XOpenDisplay(NULL);

  screen = ScreenOfDisplay(display, 0);
  return screen->width;
}

struct Tuple WindowManager::getCursor(Display *display) {
    int number_of_screens;
    int i;
    Bool result;
    Window window_returned;
    int root_x, root_y;
    int win_x, win_y;
    unsigned int mask_return;
    
    number_of_screens = XScreenCount(display);
    Window *root_windows = new Window(sizeof(Window) * number_of_screens);
    for (i = 0; i < number_of_screens; i++) {
        root_windows[i] = XRootWindow(display, i);
    }
    for (i = 0; i < number_of_screens; i++) {
        result = XQueryPointer(display, root_windows[i], &window_returned,
                &window_returned, &root_x, &root_y, &win_x, &win_y,
                &mask_return);
        if (result == True) {
            break;
        }
    }
    printf("Mouse is at (%d,%d)\n", root_x, root_y);
    
    XFree(root_windows);
    struct Tuple CursorPos = {.x = root_x, .y = root_y};
    return CursorPos;
}
