// The few Cocoa calls the app window needs on macOS (mac_window.mm): menus, full screen, an error alert.
#pragma once
#ifdef __APPLE__
#include <string>

// The app menu (About, Hide, Quit), Edit (so copy / paste / undo work in the page's text fields), View (full
// screen) and Window menus, plus a minimum window size. `nswindow`: webview's window().
void mac_window_setup(void *nswindow, const char *product);
// Full screen on / off (macOS's own full screen, in its own Space); returns whether it is now on.
bool mac_toggle_fullscreen(void *nswindow);
// A modal alert (before / without the window).
void mac_alert(const std::string &title, const std::string &text);
// The page's alert() / confirm() / prompt() as macOS dialogs. WKWebView shows nothing for them unless its UI
// delegate implements them, so confirm() answered "no" at once (nothing could be removed). wkwebview: w.widget().
void mac_webview_dialogs(void *wkwebview);
#endif
