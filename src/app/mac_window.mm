// Cocoa helpers for the app window on macOS (see mac_window.h). Built with ARC.
#include "mac_window.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>

namespace {
NSString *ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSMenuItem *item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods = NSEventModifierFlagCommand) {
  NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
  i.keyEquivalentModifierMask = mods;
  return i;
}
}  // namespace

void mac_window_setup(void *nswindow, const char *product) {
  NSString *name = ns(product);
  NSMenu *bar = [[NSMenu alloc] init];

  NSMenuItem *appItem = [[NSMenuItem alloc] init];
  NSMenu *app = [[NSMenu alloc] initWithTitle:name];
  [app addItem:item([@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"", 0)];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItem:item([@"Hide " stringByAppendingString:name], @selector(hide:), @"h")];
  [app addItem:item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                    NSEventModifierFlagCommand | NSEventModifierFlagOption)];
  [app addItem:item(@"Show All", @selector(unhideAllApplications:), @"", 0)];
  [app addItem:[NSMenuItem separatorItem]];
  // Quit closes the window: the app then shuts down in order (radios, servers) instead of exiting at once
  [app addItem:item([@"Quit " stringByAppendingString:name], @selector(performClose:), @"q")];
  appItem.submenu = app;
  [bar addItem:appItem];

  NSMenuItem *editItem = [[NSMenuItem alloc] init];
  NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItem:item(@"Undo", NSSelectorFromString(@"undo:"), @"z")];
  [edit addItem:item(@"Redo", NSSelectorFromString(@"redo:"), @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift)];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:item(@"Cut", @selector(cut:), @"x")];
  [edit addItem:item(@"Copy", @selector(copy:), @"c")];
  [edit addItem:item(@"Paste", @selector(paste:), @"v")];
  [edit addItem:item(@"Select All", @selector(selectAll:), @"a")];
  editItem.submenu = edit;
  [bar addItem:editItem];

  NSMenuItem *viewItem = [[NSMenuItem alloc] init];
  NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
  [view addItem:item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f",
                     NSEventModifierFlagCommand | NSEventModifierFlagControl)];
  viewItem.submenu = view;
  [bar addItem:viewItem];

  NSMenuItem *winItem = [[NSMenuItem alloc] init];
  NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
  [win addItem:item(@"Minimize", @selector(performMiniaturize:), @"m")];
  [win addItem:item(@"Zoom", @selector(performZoom:), @"", 0)];
  winItem.submenu = win;
  [bar addItem:winItem];
  NSApp.windowsMenu = win;

  NSApp.mainMenu = bar;
  if (NSWindow *w = (__bridge NSWindow *)nswindow) {
    w.minSize = NSMakeSize(900, 600);
    w.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    [w center];
  }
  [NSApp activateIgnoringOtherApps:YES];
}

bool mac_toggle_fullscreen(void *nswindow) {
  NSWindow *w = (__bridge NSWindow *)nswindow;
  if (!w) return false;
  const bool was = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
  [w toggleFullScreen:nil];
  return !was;
}

void mac_alert(const std::string &title, const std::string &text) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    NSAlert *a = [[NSAlert alloc] init];
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = ns(title);
    a.informativeText = ns(text);
    [a runModal];
  }
}

namespace {
// The page's text as a macOS dialog: the first paragraph bold, the rest below it (confirm texts use "\n\n").
NSAlert *page_alert(NSString *msg) {
  NSAlert *a = [[NSAlert alloc] init];
  NSRange r = [msg rangeOfString:@"\n\n"];
  a.messageText = r.location == NSNotFound ? msg : [msg substringToIndex:r.location];
  if (r.location != NSNotFound) a.informativeText = [msg substringFromIndex:r.location + 2];
  return a;
}
}  // namespace

void mac_webview_dialogs(void *wkwebview) {
  WKWebView *web = (__bridge WKWebView *)wkwebview;
  id<WKUIDelegate> d = web.UIDelegate;  // webview's own delegate (it handles the file picker): add to its class
  if (!d) return;
  Class cls = [(NSObject *)d class];
  SEL sa = @selector(webView:runJavaScriptAlertPanelWithMessage:initiatedByFrame:completionHandler:);
  if (!class_getInstanceMethod(cls, sa))
    class_addMethod(cls, sa, imp_implementationWithBlock(^(id, WKWebView *, NSString *msg, WKFrameInfo *, void (^done)(void)) {
      NSAlert *a = page_alert(msg);
      [a addButtonWithTitle:@"OK"];
      [a runModal];
      done();
    }), "v@:@@@@");
  SEL sc = @selector(webView:runJavaScriptConfirmPanelWithMessage:initiatedByFrame:completionHandler:);
  if (!class_getInstanceMethod(cls, sc))
    class_addMethod(cls, sc, imp_implementationWithBlock(^(id, WKWebView *, NSString *msg, WKFrameInfo *, void (^done)(BOOL)) {
      NSAlert *a = page_alert(msg);
      [a addButtonWithTitle:@"OK"];
      [a addButtonWithTitle:@"Cancel"];
      done([a runModal] == NSAlertFirstButtonReturn);
    }), "v@:@@@@");
  SEL sp = @selector(webView:runJavaScriptTextInputPanelWithPrompt:defaultText:initiatedByFrame:completionHandler:);
  if (!class_getInstanceMethod(cls, sp))
    class_addMethod(cls, sp, imp_implementationWithBlock(^(id, WKWebView *, NSString *msg, NSString *def, WKFrameInfo *,
                                                           void (^done)(NSString *)) {
      NSAlert *a = page_alert(msg);
      [a addButtonWithTitle:@"OK"];
      [a addButtonWithTitle:@"Cancel"];
      NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
      f.stringValue = def ?: @"";
      a.accessoryView = f;
      a.window.initialFirstResponder = f;
      done([a runModal] == NSAlertFirstButtonReturn ? f.stringValue : nil);
    }), "v@:@@@@@");
  // WebKit notes which of these the delegate answers when it is set: set it again so it sees the new ones
  web.UIDelegate = nil;
  web.UIDelegate = d;
}
