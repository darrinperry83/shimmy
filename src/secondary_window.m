#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include "gpu.h"
#include "rd_output2_policy.h"
#include "rdgpu_scanout_client.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include "secondary_region_layout.h"
#include "present_feedback.h"

// Host-window lifecycle slice for a future Wayland xdg_toplevel mapping.
// This intentionally does not receive guest input or replace wlsrv's current
// compositor. No caller should expose it to a guest until those paths exist.
static NSMutableDictionary<NSNumber*, NSWindow*>* gWindows;
static NSMutableSet<NSNumber*>* gSkipNextCPU;
static NSMutableSet<NSNumber*>* gSkippedCPUCommit;
static NSMutableDictionary<NSNumber*, NSNumber*>* gDirectPublishedAt;
@interface RDSecondaryRegionFrame : NSObject
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic) struct rd_secondary_region crop;
@end
@implementation RDSecondaryRegionFrame
@end
static NSMutableDictionary<NSNumber*, RDSecondaryRegionFrame*>* gOutputRegions;
static struct rd_secondary_region gOutputSpecs[RD_SECONDARY_MAX_REGIONS];
static int gOutputSpecCount = -1;
static uint32_t gOutput2Wid;
static ino_t gRegionInode;
static time_t gRegionMtime;
static long gRegionMtimeNS;
static off_t gRegionSize;
static unsigned gRegionWidth, gRegionHeight;
static NSString* gAppliedRegionFocus;
static NSMutableDictionary<NSNumber*, NSWindow*>* gPopups;
static NSMutableDictionary<NSNumber*, NSArray<NSNumber*>*>* gPopupOwners;
static NSString* gDesktopSession;
static id gDesktopBridge;
static NSMutableDictionary<NSNumber*, NSValue*>* gOriginalFrames;
static id gMonitor;
static void on_main(dispatch_block_t block);
struct tagged_event { uint32_t window_id; struct gpuwin_event event; };
static struct tagged_event gEvents[256];
static unsigned gHead, gTail;
static pthread_mutex_t gEventLock = PTHREAD_MUTEX_INITIALIZER;
static void publish_windows(void);
static NSWindow* gInteractiveWindow;
static NSRect gInteractiveStartFrame;
static NSPoint gInteractiveStartPoint;
static uint32_t gInteractiveEdges;
static id gInteractiveMonitor;
static NSRect gInteractivePendingFrame;
static BOOL gInteractiveScheduled;
static unsigned gInteractiveUpdates;
static NSTimeInterval gInteractiveStartTime;
static double gInteractiveFrameMs;

static void apply_interactive_frame(void) {
  if (!gInteractiveScheduled || !gInteractiveWindow) return;
  gInteractiveScheduled = NO;
  NSTimeInterval started = NSProcessInfo.processInfo.systemUptime;
  [gInteractiveWindow setFrame:gInteractivePendingFrame display:YES animate:NO];
  gInteractiveFrameMs += (NSProcessInfo.processInfo.systemUptime - started) * 1000;
  if (gInteractiveUpdates++ == 0 && getenv("RDWL_PERF"))
    fprintf(stderr, "[wl-perf] request-to-host-frame %.2f ms frame %.0f,%.0f %.0fx%.0f\n",
            (NSProcessInfo.processInfo.systemUptime - gInteractiveStartTime) * 1000,
            NSMinX(gInteractivePendingFrame), NSMinY(gInteractivePendingFrame),
            NSWidth(gInteractivePendingFrame), NSHeight(gInteractivePendingFrame));
}

@interface RDSecondaryGuestWindow : NSWindow
@end
@implementation RDSecondaryGuestWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

static NSWindow* primary_window(void) {
  for (NSWindow* win in NSApp.windows)
    if (win.isVisible && ![gWindows.allValues containsObject:win] &&
        ![gPopups.allValues containsObject:win] &&
        ![[gOutputRegions.allValues valueForKey:@"window"] containsObject:win]) return win;
  return nil;
}

// Experimental second nested Hyprland output. A real second Aquamarine
// xdg_toplevel has this exact title; ordinary dialogs never use this route.
// Return nil if the requested display is absent, leaving the ordinary host
// window path untouched and making hot-unplug failure visible in diagnostics.
static NSScreen* second_output_screen(const char* title) {
  const char* requested = getenv("RD_MULTI_OUTPUT_DISPLAY_ID");
  if (!requested || !title || strcmp(title, "aquamarine - WAYLAND-2")) return nil;
  char* end = NULL;
  unsigned long wanted = strtoul(requested, &end, 10);
  if (!end || *end || wanted == 0 || wanted > UINT32_MAX) return nil;
  for (NSScreen* screen in NSScreen.screens) {
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    if (number && number.unsignedIntValue == wanted &&
        CGDisplayIsActive((CGDirectDisplayID)wanted)) return screen;
  }
  return nil;
}

static uint32_t resize_edge_flags(uint32_t edge) {
  // Valid xdg-shell enum values are bit combinations of top/bottom/left/right.
  switch (edge) {
  case 1: case 2: case 4: case 5: case 6: case 8: case 9: case 10: return edge;
  default: return 0;
  }
}

static void install_interactive_monitor(void) {
  if (gInteractiveMonitor) return;
  gInteractiveMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:
      NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp handler:^NSEvent*(NSEvent* event) {
    NSWindow* win = gInteractiveWindow;
    if (!win) return event;
    if (event.type == NSEventTypeLeftMouseUp) {
      apply_interactive_frame();
      if (getenv("RDWL_PERF"))
        fprintf(stderr, "[wl-perf] interactive host frame updates %u, AppKit setFrame mean %.2f ms\n",
                gInteractiveUpdates, gInteractiveUpdates ? gInteractiveFrameMs / gInteractiveUpdates : 0);
      gInteractiveWindow = nil;
      return event;
    }
    if (!win.isVisible || event.window != win) { gInteractiveWindow = nil; return event; }
    NSPoint screen = [win convertPointToScreen:event.locationInWindow];
    CGFloat dx = screen.x - gInteractiveStartPoint.x;
    CGFloat dy = screen.y - gInteractiveStartPoint.y;
    NSRect frame = gInteractiveStartFrame;
    uint32_t edges = gInteractiveEdges;
    if (!edges) { frame.origin.x += dx; frame.origin.y += dy; }
    else {
      if (edges & 4) { frame.origin.x += dx; frame.size.width -= dx; }
      if (edges & 8) frame.size.width += dx;
      if (edges & 1) frame.size.height += dy;
      if (edges & 2) { frame.origin.y += dy; frame.size.height -= dy; }
      if (frame.size.width < 160) {
        if (edges & 4) frame.origin.x -= 160 - frame.size.width;
        frame.size.width = 160;
      }
      if (frame.size.height < 100) {
        if (edges & 2) frame.origin.y -= 100 - frame.size.height;
        frame.size.height = 100;
      }
    }
    gInteractivePendingFrame = frame;
    if (!gInteractiveScheduled) {
      gInteractiveScheduled = YES;
      dispatch_async(dispatch_get_main_queue(), ^{ apply_interactive_frame(); });
    }
    return event;
  }];
}

// Called only after the Wayland compositor validates the pointer press serial.
// The next host drag event moves or resizes the selected AppKit window.
int rd_guest_interactive(uint32_t wid, uint32_t edge, int32_t local_x, int32_t local_y) {
  __block int result = -1;
  on_main(^{
    NSWindow* win = wid ? gWindows[@(wid)] : primary_window();
    if (!win || !win.isVisible || (edge && !resize_edge_flags(edge))) return;
    install_interactive_monitor();
    gInteractiveWindow = win;
    gInteractiveStartFrame = win.frame;
    gInteractiveStartPoint = [win convertPointToScreen:
        NSMakePoint(local_x, win.contentView.bounds.size.height - local_y)];
    gInteractiveEdges = resize_edge_flags(edge);
    gInteractiveStartTime = NSProcessInfo.processInfo.systemUptime;
    gInteractiveUpdates = 0;
    gInteractiveFrameMs = 0;
    gInteractiveScheduled = NO;
    result = 0;
    fprintf(stderr, "[wl-host] interactive %s window %u edge %u\n",
            edge ? "resize" : "move", wid, edge);
  });
  return result;
}

@interface RDDesktopBridge : NSObject
- (void)command:(NSNotification*)notification;
@end
@implementation RDDesktopBridge
- (void)command:(NSNotification*)notification {
  NSDictionary* info = notification.userInfo;
  if ([info[@"pid"] intValue] != getpid()) return;
  uint32_t wid = [info[@"window"] unsignedIntValue];
  NSWindow* win = wid ? gWindows[@(wid)] : primary_window();
  NSString* action = info[@"action"];
  if (!win || ![action isKindOfClass:NSString.class]) return;
  if ([action isEqualToString:@"focus"]) {
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
  } else if ([action isEqualToString:@"left"] || [action isEqualToString:@"right"]) {
    NSScreen* screen = win.screen ?: NSScreen.mainScreen;
    if (!screen) return;
    NSRect available = screen.visibleFrame;
    if (!gOriginalFrames) gOriginalFrames = [NSMutableDictionary new];
    if (!gOriginalFrames[@(wid)]) gOriginalFrames[@(wid)] = [NSValue valueWithRect:win.frame];
    NSRect frame = NSMakeRect(NSMinX(available), NSMinY(available),
                              NSWidth(available) / 2, NSHeight(available));
    if ([action isEqualToString:@"right"]) frame.origin.x += NSWidth(available) / 2;
    [win setFrame:frame display:YES animate:NO];
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
  } else if ([action isEqualToString:@"restore"]) {
    NSValue* saved = gOriginalFrames[@(wid)];
    if (!saved) return;
    [win setFrame:saved.rectValue display:YES animate:NO];
    [gOriginalFrames removeObjectForKey:@(wid)];
  } else return;
  fprintf(stderr, "[multiwin] desktop %s window %u\n", action.UTF8String, wid);
  publish_windows();
}
@end

static void install_desktop_bridge(void) {
  if (gDesktopBridge) return;
  const char* token = getenv("RDWL_DESKTOP_SESSION");
  if (!token || !*token || strlen(token) > 128) return;
  gDesktopSession = [NSString stringWithUTF8String:token];
  if (!gDesktopSession) return;
  gDesktopBridge = [RDDesktopBridge new];
  [NSDistributedNotificationCenter.defaultCenter addObserver:gDesktopBridge
      selector:@selector(command:) name:@"org.linux-shim.desktop.command" object:gDesktopSession];
}

static void publish_windows(void) {
  if (!gDesktopSession) return;
  NSMutableArray* windows = [NSMutableArray new];
  NSWindow* primary = primary_window();
  if (primary) [windows addObject:@{@"id": @0, @"title": primary.title ?: @"Linux window"}];
  for (NSNumber* key in gWindows) {
    NSWindow* win = gWindows[key];
    if (win.isVisible) [windows addObject:@{@"id": key, @"title": win.title ?: @"Linux window"}];
  }
  [NSDistributedNotificationCenter.defaultCenter
      postNotificationName:@"org.linux-shim.desktop.windows" object:gDesktopSession
                userInfo:@{@"pid": @(getpid()), @"windows": windows} deliverImmediately:YES];
}

static void enqueue(uint32_t wid, struct gpuwin_event event) {
  pthread_mutex_lock(&gEventLock);
  unsigned next = (gHead + 1) % 256;
  if (next != gTail) { gEvents[gHead] = (struct tagged_event){ wid, event }; gHead = next; }
  pthread_mutex_unlock(&gEventLock);
}

@interface RDSecondaryDelegate : NSObject <NSWindowDelegate>
@end
@implementation RDSecondaryDelegate
- (void)windowDidResize:(NSNotification*)notification {
  NSWindow* sender = notification.object;
  for (NSNumber* key in gWindows) {
    if (gWindows[key] != sender) continue;
    NSSize size = sender.contentView.bounds.size;
    struct gpuwin_event ev = {0};
    ev.type = GPUWIN_EV_RESIZE;
    ev.x = (int32_t)size.width; ev.y = (int32_t)size.height;
    enqueue(key.unsignedIntValue, ev);
    return;
  }
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
  for (NSNumber* key in gWindows) {
    if (gWindows[key] != sender) continue;
    struct gpuwin_event ev = {0};
    ev.type = 9; // RD_SECONDARY_EV_CLOSE: Wayland xdg_toplevel.close request
    enqueue(key.unsignedIntValue, ev);
    return NO;  // guest owns teardown; its destroy closes the native window
  }
  return YES;
}
@end
static RDSecondaryDelegate* gDelegate;

int rd_secondary_next_event(uint32_t* wid, struct gpuwin_event* event) {
  pthread_mutex_lock(&gEventLock);
  int found = gTail != gHead;
  if (found) {
    if (wid) *wid = gEvents[gTail].window_id;
    if (event) *event = gEvents[gTail].event;
    gTail = (gTail + 1) % 256;
  }
  pthread_mutex_unlock(&gEventLock);
  return found;
}

uint32_t rd_secondary_key_window_id(void) {
  __block uint32_t result = 0;
  on_main(^{
    NSWindow* key = NSApp.keyWindow;
    for (NSNumber* wid in gWindows)
      if (gWindows[wid] == key) { result = wid.unsignedIntValue; break; }
    if (!result) for (NSNumber* pid in gPopups)
      if (gPopups[pid] == key) { result = gPopupOwners[pid][0].unsignedIntValue; break; }
    if (!result) for (RDSecondaryRegionFrame* frame in gOutputRegions.allValues)
      if (frame.window == key) { result = gOutput2Wid; break; }
  });
  return result;
}

static const uint16_t kVK2EVDEV[128] = {
  [0x00]=30,[0x01]=31,[0x02]=32,[0x03]=33,[0x04]=35,[0x05]=34,
  [0x06]=44,[0x07]=45,[0x08]=46,[0x09]=47,[0x0B]=48,[0x0C]=16,
  [0x0D]=17,[0x0E]=18,[0x0F]=19,[0x10]=21,[0x11]=20,[0x12]=2,
  [0x13]=3,[0x14]=4,[0x15]=5,[0x16]=7,[0x17]=6,[0x18]=13,
  [0x19]=10,[0x1A]=8,[0x1B]=12,[0x1C]=9,[0x1D]=11,[0x1E]=27,
  [0x1F]=24,[0x20]=22,[0x21]=26,[0x22]=23,[0x23]=25,[0x24]=28,
  [0x25]=38,[0x26]=36,[0x27]=40,[0x28]=37,[0x29]=39,[0x2A]=43,
  [0x2B]=51,[0x2C]=53,[0x2D]=49,[0x2E]=50,[0x2F]=52,[0x30]=15,
  [0x31]=57,[0x32]=41,[0x33]=14,[0x35]=1,[0x37]=125,[0x38]=42,
  [0x39]=58,[0x3A]=56,[0x3B]=29,[0x3C]=54,[0x3D]=100,[0x3E]=97,
  [0x7A]=59,[0x78]=60,[0x63]=61,[0x76]=62,[0x60]=63,[0x61]=64,
  [0x62]=65,[0x64]=66,[0x65]=67,[0x6D]=68,[0x67]=87,[0x6F]=88,
  [0x72]=110,[0x73]=102,[0x74]=104,[0x75]=111,[0x77]=107,
  [0x79]=109,[0x7B]=105,[0x7C]=106,[0x7D]=108,[0x7E]=103,
};

static uint32_t modifiers(NSEventModifierFlags f) {
  uint32_t mods = 0;
  if (f & NSEventModifierFlagShift) mods |= GPUWIN_MOD_SHIFT;
  if (f & NSEventModifierFlagCapsLock) mods |= GPUWIN_MOD_CAPS;
  if (f & NSEventModifierFlagControl) mods |= GPUWIN_MOD_CTRL;
  if (f & NSEventModifierFlagOption) mods |= GPUWIN_MOD_ALT;
  if (f & NSEventModifierFlagCommand) mods |= GPUWIN_MOD_LOGO;
  return mods;
}

static void install_monitor(void) {
  if (gMonitor) return;
  NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp |
                     NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                     NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
                     NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                     NSEventMaskRightMouseDragged | NSEventMaskScrollWheel;
  gMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
    NSWindow* win = e.window;
    if (!win) return e;
    uint32_t wid = 0;
    for (NSNumber* key in gWindows)
      if (gWindows[key] == win) { wid = key.unsignedIntValue; break; }
    NSArray<NSNumber*>* popup = nil;
    if (!wid) for (NSNumber* key in gPopups)
      if (gPopups[key] == win) { popup = gPopupOwners[key]; wid = popup[0].unsignedIntValue; break; }
    RDSecondaryRegionFrame* region = nil;
    if (!wid) for (RDSecondaryRegionFrame* frame in gOutputRegions.allValues)
      if (frame.window == win) { region = frame; wid = gOutput2Wid; break; }
    if (!wid) return e;
    struct gpuwin_event ev = {0};
    ev.mods = modifiers(e.modifierFlags);
    NSPoint p = e.locationInWindow;
    ev.x = (int32_t)p.x;
    ev.y = (int32_t)(win.contentView.bounds.size.height - p.y);
    if (popup) { ev.x += popup[1].intValue; ev.y += popup[2].intValue; }
    NSEventType t = e.type;
    if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp) {
      uint16_t code = e.keyCode < 128 ? kVK2EVDEV[e.keyCode] : 0;
      if (code) { ev.type = GPUWIN_EV_KEY; ev.code = code; ev.state = t == NSEventTypeKeyDown; }
    } else if (t == NSEventTypeLeftMouseDown || t == NSEventTypeLeftMouseUp ||
               t == NSEventTypeRightMouseDown || t == NSEventTypeRightMouseUp) {
      ev.type = GPUWIN_EV_BUTTON;
      ev.code = (t == NSEventTypeRightMouseDown || t == NSEventTypeRightMouseUp) ? 0x111 : 0x110;
      ev.state = t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown;
    } else if (t == NSEventTypeScrollWheel) {
      ev.type = GPUWIN_EV_SCROLL;
      ev.x = (int32_t)-e.scrollingDeltaX;
      ev.y = (int32_t)-e.scrollingDeltaY;
    } else ev.type = GPUWIN_EV_MOTION;
    if (region && ev.type != GPUWIN_EV_SCROLL && ev.type != GPUWIN_EV_KEY) {
      struct rd_secondary_region crop = region.crop;
      if (rd_secondary_pointer(&crop, ev.x, ev.y, &ev.x, &ev.y)) return e;
    }
    if (ev.type) enqueue(wid, ev);
    return (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp) ? nil : e;
  }];
}

static void on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) block();
  else dispatch_sync(dispatch_get_main_queue(), block);
}

int rd_secondary_open(uint32_t wid, uint32_t width, uint32_t height,
                      const char* title, uint32_t parent_id) {
  if (!wid || !width || !height || width > 16384 || height > 16384) return -1;
  __block int result = 0;
  on_main(^{ @autoreleasepool {
    if (!gWindows) gWindows = [NSMutableDictionary new];
    install_desktop_bridge();
    NSNumber* key = @(wid);
    NSWindow* win = gWindows[key];
    BOOL changed = !win;
    if (!win) {
      BOOL clientDecorations = getenv("RDWL_CLIENT_DECORATIONS") != NULL;
      NSWindowStyleMask style = clientDecorations ? NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable :
          NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
      Class windowClass = clientDecorations ? RDSecondaryGuestWindow.class : NSWindow.class;
      win = [[windowClass alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                       styleMask:style
                                         backing:NSBackingStoreBuffered defer:NO];
      if (!win) { result = -1; return; }
      if (clientDecorations)
        fprintf(stderr, "[wl-host] detached window %u uses guest decorations\n", wid);
      win.releasedWhenClosed = NO;
      if (!gDelegate) gDelegate = [RDSecondaryDelegate new];
      win.delegate = gDelegate;
      win.contentView.wantsLayer = YES;
      win.contentView.layer.contentsGravity = kCAGravityResize;
      gWindows[key] = win;
      install_monitor();
      NSScreen* outputScreen = second_output_screen(title);
      NSWindow* parent = outputScreen ? nil : gWindows[@(parent_id)];
      if (!outputScreen && !parent && parent_id == UINT32_MAX) {
        for (NSWindow* candidate in NSApp.windows) {
          if (candidate != win && candidate.isVisible && ![gWindows.allValues containsObject:candidate]) {
            parent = candidate;  // the shim's existing primary NSWindow
            break;
          }
        }
      }
      if (parent) [parent addChildWindow:win ordered:NSWindowAbove];
      else if (outputScreen) {
        if (getenv("RD_SECONDARY_REGION_LIST_PATH")) gOutput2Wid = wid;
        NSRect frame = outputScreen.frame;
        [win setFrameOrigin:getenv("RDNATIVE_FIT_DISPLAY") ? frame.origin :
             NSMakePoint(NSMidX(frame) - NSWidth(win.frame) / 2,
                         NSMidY(frame) - NSHeight(win.frame) / 2)];
        NSNumber* displayNumber = outputScreen.deviceDescription[@"NSScreenNumber"];
        fprintf(stderr, "[multi-output] %s surface %u host display %lu origin %.0f,%.0f\n",
                title, wid, (unsigned long)displayNumber.unsignedIntValue,
                NSMinX(win.frame), NSMinY(win.frame));
      } else [win center];
      fprintf(stderr, "[multiwin] surface %u opened native window %ld parent %ld\n",
              wid, (long)win.windowNumber, (long)parent.windowNumber);
      // In regional output mode this is a backing identity/input owner only.
      // Showing its opaque full desktop before the coordinator's crop manifest
      // arrives briefly stacks a second Hyprland surface over the real Mac
      // window. The actual background/crop NSWindows are shown separately.
      if (wid == gOutput2Wid && getenv("RD_SECONDARY_REGION_LIST_PATH")) {
        [win orderOut:nil];
        fprintf(stderr,"[gpu-output2-region] full backing window %ld starts hidden\n",
                (long)win.windowNumber);
      } else [win orderFrontRegardless];
      if (getenv("RDWL_MULTIWINDOW_TEST_ESCAPE")) {
        NSInteger number = win.windowNumber;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
          NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
          for (int down = 1; down >= 0; down--) {
            NSEvent* key = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                             location:NSZeroPoint modifierFlags:0 timestamp:now
                                         windowNumber:number context:nil characters:@""
                          charactersIgnoringModifiers:@"" isARepeat:NO keyCode:0x35];
            if (key) [NSApp postEvent:key atStart:NO];
          }
          fprintf(stderr, "[multiwin] injected Escape into native window %ld\n", (long)number);
        });
      }
      const char* click = getenv("RDWL_MULTIWINDOW_TEST_CLICK");
      if (click) {
        int x = 0, y = 0;
        if (sscanf(click, "%d,%d", &x, &y) == 2) {
          NSInteger number = win.windowNumber;
          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
            NSPoint at = NSMakePoint(x, win.contentView.bounds.size.height - y);
            for (int down = 1; down >= 0; down--) {
              NSEvent* event = [NSEvent mouseEventWithType:down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp
                                                location:at modifierFlags:0 timestamp:now windowNumber:number
                                                  context:nil eventNumber:0 clickCount:1 pressure:down ? 1 : 0];
              if (event) [NSApp postEvent:event atStart:NO];
            }
            fprintf(stderr, "[multiwin] injected click at %d,%d into native window %ld\n",
                    x, y, (long)number);
          });
        }
      }
      if (getenv("RDWL_MULTIWINDOW_TEST_HOST_CLOSE")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
          [win performClose:nil];
          fprintf(stderr, "[multiwin] injected native close for surface %u\n", wid);
        });
      }
      const char* resize = getenv("RDWL_MULTIWINDOW_TEST_RESIZE");
      if (resize) {
        int rw = 0, rh = 0;
        if (sscanf(resize, "%d,%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [win setContentSize:NSMakeSize(rw, rh)];
            fprintf(stderr, "[multiwin] injected native resize to %d,%d for surface %u\n", rw, rh, wid);
          });
        }
      }
    }
    NSString* nextTitle = title ? [NSString stringWithUTF8String:title] ?: @"Linux window" : @"Linux window";
    if (![win.title isEqualToString:nextTitle]) changed = YES;
    win.title = nextTitle;
    if (win.contentView.bounds.size.width != width || win.contentView.bounds.size.height != height)
      [win setContentSize:NSMakeSize(width, height)];
    NSScreen* fitScreen = getenv("RDNATIVE_FIT_DISPLAY") ? second_output_screen(title) : nil;
    if (fitScreen && !NSEqualPoints(win.frame.origin, fitScreen.frame.origin))
      [win setFrameOrigin:fitScreen.frame.origin];
    if (changed) publish_windows();
  } });
  return result;
}

static int refresh_output_regions(unsigned width, unsigned height) {
  const char* path = getenv("RD_SECONDARY_REGION_LIST_PATH");
  if (!path || !gOutput2Wid) return -1;
  struct stat st;
  if (stat(path, &st) || st.st_size < 1 || st.st_size > 8192) return -1;
  if (gRegionInode == st.st_ino && gRegionMtime == st.st_mtimespec.tv_sec &&
      gRegionMtimeNS == st.st_mtimespec.tv_nsec && gRegionSize == st.st_size &&
      gRegionWidth == width && gRegionHeight == height) return gOutputSpecCount;
  gRegionInode = st.st_ino; gRegionMtime = st.st_mtimespec.tv_sec;
  gRegionMtimeNS = st.st_mtimespec.tv_nsec; gRegionSize = st.st_size;
  gRegionWidth = width; gRegionHeight = height;
  gOutputSpecCount = -1;
  FILE* file = fopen(path, "rb");
  if (!file) return -1;
  char text[8193];
  size_t length = fread(text, 1, sizeof text, file);
  fclose(file);
  if (!length || length > 8192 || memchr(text, 0, length)) return -1;
  text[length] = 0;
  struct rd_secondary_region parsed[RD_SECONDARY_MAX_REGIONS];
  int count = rd_parse_secondary_regions(text, width, height, parsed);
  if (count < 1) return -1;
  memcpy(gOutputSpecs, parsed, (size_t)count * sizeof parsed[0]);
  gOutputSpecCount = count;
  return count;
}

static void order_out_output_regions(void) {
  for (RDSecondaryRegionFrame* frame in gOutputRegions.allValues) [frame.window orderOut:nil];
}

static void update_output_focus(void) {
  const char* path = getenv("RD_SECONDARY_REGION_FOCUS_PATH");
  if (!path) return;
  FILE* file = fopen(path, "r");
  if (!file) return;
  char target[64] = {0};
  (void)fgets(target, sizeof target, file); fclose(file);
  NSString* focus = [NSString stringWithUTF8String:target] ?: @"";
  if ([gAppliedRegionFocus isEqualToString:focus]) return;
  gAppliedRegionFocus = focus;
  if ([focus hasPrefix:@"mac"]) {
    for (RDSecondaryRegionFrame* frame in gOutputRegions.allValues)
      if (frame.crop.role >= 0) [frame.window orderBack:nil];
    return;
  }
  unsigned long long ident = strtoull(target, NULL, 0);
  RDSecondaryRegionFrame* selected = gOutputRegions[@(ident)];
  if (selected && selected.crop.role == 0) {
    [NSApp activateIgnoringOtherApps:YES];
    [selected.window makeKeyAndOrderFront:nil];
  }
}

static int present_output_regions(NSWindow* full, CGImageRef image,
                                  unsigned width, unsigned height) {
  if (refresh_output_regions(width, height) < 1) {
    order_out_output_regions();
    [full orderOut:nil];
    // The coordinator can publish its first region manifest just after the
    // first output commit. Keep the detached output alive but invisible while
    // it catches up; returning -1 here permanently disables multiwindow.
    static unsigned pending;
    if (pending++ < 4) fprintf(stderr,"[gpu-output2-region] waiting for first valid region manifest\n");
    return 0;
  }
  NSScreen* screen = second_output_screen("aquamarine - WAYLAND-2");
  const char* expected = getenv("RD_SECONDARY_HOST_FRAME");
  int verified_frame = screen && rd_secondary_host_frame_matches(expected,
      (long)NSMinX(screen.frame), (long)NSMinY(screen.frame),
      (long)NSWidth(screen.frame), (long)NSHeight(screen.frame));
  if (!screen || (unsigned)NSWidth(screen.frame) != width ||
      (unsigned)NSHeight(screen.frame) != height || !verified_frame) {
    static unsigned mismatched;
    if (mismatched++ < 8)
      fprintf(stderr,"[gpu-output2-region] host frame mismatch live=%d size=%.0fx%.0f guest=%ux%u verified=%d\n",
              !!screen,screen ? NSWidth(screen.frame) : 0,screen ? NSHeight(screen.frame) : 0,
              width,height,verified_frame);
    order_out_output_regions();
    [full orderOut:nil];
    return -1; // never relocate a parked Mac window after host display loss
  }
  if (!gOutputRegions) gOutputRegions = [NSMutableDictionary new];
  NSMutableSet<NSNumber*>* wanted = [NSMutableSet new];
  for (int i = 0; i < gOutputSpecCount; i++) {
    struct rd_secondary_region spec = gOutputSpecs[i];
    NSNumber* key = @(spec.id);
    [wanted addObject:key];
    RDSecondaryRegionFrame* frame = gOutputRegions[key];
    if (!frame) {
      NSWindow* win = [[RDSecondaryGuestWindow alloc]
          initWithContentRect:NSMakeRect(0, 0, spec.w, spec.h)
                     styleMask:NSWindowStyleMaskBorderless
                       backing:NSBackingStoreBuffered defer:NO];
      if (!win) return -1;
      win.title = [NSString stringWithFormat:@"Linux 0x%llx", (unsigned long long)spec.id];
      win.releasedWhenClosed = NO;
      win.contentView.wantsLayer = YES;
      frame = [RDSecondaryRegionFrame new];
      frame.window = win;
      gOutputRegions[key] = frame;
      install_monitor();
    }
    frame.crop = spec;
    NSWindow* win = frame.window;
    if (win.contentView.bounds.size.width != spec.w ||
        win.contentView.bounds.size.height != spec.h)
      [win setContentSize:NSMakeSize(spec.w, spec.h)];
    NSPoint origin = NSMakePoint(NSMinX(screen.frame) + spec.x,
          NSMinY(screen.frame) + (CGFloat)height - spec.y - spec.h);
    if (!NSEqualPoints(win.frame.origin, origin)) [win setFrameOrigin:origin];
    NSWindowLevel level;
    if (spec.role < 0 && getenv("RDNATIVE_KIOSK")) {
      // Kiosk takeover: raise the full-screen output background ABOVE the menu
      // bar and Dock (the same level window.m gives the primary desktop), so the
      // second display shows only Hyprland and no macOS chrome pokes through.
      level = CGWindowLevelForKey(kCGStatusWindowLevelKey) + 1;
    } else {
      level = (spec.role > 0 && (spec.id & (1ULL << 62)) &&
               !(spec.id & (1ULL << 63))) ?
              CGWindowLevelForKey(kCGStatusWindowLevelKey) + 2 :
              spec.role < 0 ? NSNormalWindowLevel - 1 :
              spec.role > 0 ? NSFloatingWindowLevel : NSNormalWindowLevel;
    }
    if (win.level != level) {
      [win setLevel:level];
      if (spec.role < 0 && getenv("RDNATIVE_KIOSK")) {
        NSNumber* display = screen.deviceDescription[@"NSScreenNumber"];
        fprintf(stderr, "[kiosk] WAYLAND-2 background raised to level %ld display %u (above menu bar + Dock)\n",
                (long)level, display.unsignedIntValue);
      }
    }
    if (image) {
      if ([win.contentView.layer isKindOfClass:CAMetalLayer.class]) {
        CALayer* fallback = [CALayer layer];
        fallback.contentsGravity = kCAGravityResize;
        win.contentView.layer = fallback;
      }
      CGImageRef crop = CGImageCreateWithImageInRect(image,
          CGRectMake(spec.x, spec.y, spec.w, spec.h));
      if (!crop) return -1;
      win.contentView.layer.contents = (__bridge id)crop;
      CGImageRelease(crop);
    }
    if (!win.isVisible) {
      [win orderFront:nil];
      // The output-2 regional presenter owns different NSWindows from the
      // primary region path. Publish the exact host identity only after
      // ordering the window, so chrome readiness cannot mistake a manifest
      // row for a presentable replacement bar.
      fprintf(stderr, "[gpu-output2-region] created id=0x%llx window=%ld\n",
              (unsigned long long)spec.id, (long)win.windowNumber);
    }
  }
  for (NSNumber* key in gOutputRegions.allKeys) {
    if ([wanted containsObject:key]) continue;
    RDSecondaryRegionFrame* gone = gOutputRegions[key];
    [gone.window orderOut:nil]; [gone.window close];
    fprintf(stderr, "[gpu-output2-region] removed id=0x%llx\n",
            (unsigned long long)key.unsignedLongLongValue);
    [gOutputRegions removeObjectForKey:key];
  }
  [full orderOut:nil];
  static unsigned inventory;
  if (++inventory <= 3 || inventory % 100 == 0) {
    unsigned visible = 0;
    for (RDSecondaryRegionFrame* region in gOutputRegions.allValues) {
      visible += region.window.isVisible ? 1 : 0;
      if (inventory == 1) {
        NSRect rect = region.window.frame;
        NSNumber* display = region.window.screen.deviceDescription[@"NSScreenNumber"];
        fprintf(stderr,"[gpu-output2-region] window id=0x%llx display=%u frame=%.0f,%.0f %.0fx%.0f level=%ld\n",
                (unsigned long long)region.crop.id,display.unsignedIntValue,
                NSMinX(rect),NSMinY(rect),NSWidth(rect),NSHeight(rect),(long)region.window.level);
      }
    }
    fprintf(stderr,"[gpu-output2-region] inventory frame=%u full_visible=%d region_visible=%u wanted=%lu\n",
            inventory,full.isVisible,visible,(unsigned long)wanted.count);
  }
  update_output_focus();
  return 0;
}

int rd_secondary_present(uint32_t wid, const uint8_t* bgra, uint32_t width, uint32_t height) {
  if (!wid || !bgra || !width || !height || width > 16384 || height > 16384) return -1;
  size_t size = (size_t)width * height * 4;
  if (size / 4 != (size_t)width * height) return -1;
  __block int result = 0;
  on_main(^{ @autoreleasepool {
    NSWindow* win = gWindows[@(wid)];
    if (!win) { result = -1; return; }
    NSNumber* key = @(wid);
    NSNumber* published = gDirectPublishedAt[key];
    BOOL matchingGPUCommit = published && rd_output2_fresh_commit(
      [gSkipNextCPU containsObject:key], NSProcessInfo.processInfo.systemUptime - published.doubleValue);
    [gSkipNextCPU removeObject:key];
    [gDirectPublishedAt removeObjectForKey:key];
    if (matchingGPUCommit) {
      if (!gSkippedCPUCommit) gSkippedCPUCommit = [NSMutableSet new];
      [gSkippedCPUCommit addObject:key];
      return;
    }
    NSData* copy = [NSData dataWithBytes:bgra length:size];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    if (!provider) { result = -1; return; }
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, color,
                                    kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                    provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(color);
    CGDataProviderRelease(provider);
    if (!image) { result = -1; return; }
    if (wid == gOutput2Wid && getenv("RD_SECONDARY_REGION_LIST_PATH")) {
      if (present_output_regions(win, image, width, height) != 0) result = -1;
    } else win.contentView.layer.contents = (__bridge id)image;
    CGImageRelease(image);
  } });
  return result;
}

int rd_secondary_direct_consumed(uint32_t wid) {
  __block int result = 0;
  on_main(^{
    NSNumber* key = @(wid);
    result = [gSkippedCPUCommit containsObject:key];
    [gSkippedCPUCommit removeObject:key];
  });
  return result;
}

int rd_secondary_direct_claim(uint32_t wid) {
  __block int result = 0;
  on_main(^{
    NSNumber* key = @(wid);
    NSNumber* published = gDirectPublishedAt[key];
    result = published && rd_output2_fresh_commit([gSkipNextCPU containsObject:key],
      NSProcessInfo.processInfo.systemUptime - published.doubleValue);
    [gSkipNextCPU removeObject:key];
    [gDirectPublishedAt removeObjectForKey:key];
  });
  return result;
}

// Publish a named Hyprland output through the same per-region native windows
// used by the CPU path. The full output window stays hidden, leaving real Mac
// windows between the background and Linux region windows. No pixel readback.
int rd_secondary_present_iosurface_name(const char* name, void* source,
                                        uint32_t width, uint32_t height) {
  if (!name || strcmp(name, "WAYLAND-2") || !source || !width || !height ||
      width > 16384 || height > 16384) return -1;
  IOSurfaceRef surface = (IOSurfaceRef)source;
  if (IOSurfaceGetWidth(surface) != width || IOSurfaceGetHeight(surface) != height ||
      IOSurfaceGetPixelFormat(surface) != (uint32_t)'BGRA') return -1;
  // RD_OUTPUT2_CAPTURE_PREFIX=<prefix> — per-frame dump of the builtin's presented
  // output (host-side, every frame, not damage-limited) for a both-screens video.
  {
    static long out2Frames;
    const char* capPrefix = getenv("RD_OUTPUT2_CAPTURE_PREFIX");
    const char* capEvery = getenv("RD_OUTPUT2_CAPTURE_EVERY");
    long every = capEvery ? atol(capEvery) : 1;
    if (capPrefix && (++out2Frames % every) == 0) {
      char path[1024];
      snprintf(path, sizeof path, "%s-%05ld.ppm", capPrefix, out2Frames);
      IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
      const uint8_t* base = (const uint8_t*)IOSurfaceGetBaseAddress(surface);
      size_t pitch = IOSurfaceGetBytesPerRow(surface);
      FILE* f = fopen(path, "wb");
      if (f && base) {
        fprintf(f, "P6\n%u %u\n255\n", width, height);
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* s = base + y * pitch;
          for (uint32_t x = 0; x < width; ++x) {
            fputc(s[x * 4 + 2], f); fputc(s[x * 4 + 1], f); fputc(s[x * 4 + 0], f);
          }
        }
        fclose(f);
      } else if (f) fclose(f);
      IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    }
  }
  __block int result = -1;
  on_main(^{ @autoreleasepool {
    NSWindow* full = nil;
    NSNumber* matchID = nil;
    for (NSNumber* wid in gWindows) {
      NSWindow* candidate = gWindows[wid];
      if (![candidate.title isEqualToString:@"aquamarine - WAYLAND-2"])
        continue;
      if (full) return; // ambiguous output identity
      full = candidate;
      matchID = wid;
    }
    if (!full || matchID.unsignedIntValue != gOutput2Wid ||
        !getenv("RD_SECONDARY_REGION_LIST_PATH")) {
      static unsigned missing;
      if (missing++ < 8) fprintf(stderr,"[gpu-output2-region] output mapping not ready full=%d id=%u expected=%u\n",
                                 !!full,matchID.unsignedIntValue,gOutput2Wid);
      return;
    }
    NSScreen* screen = second_output_screen("aquamarine - WAYLAND-2");
    if (!screen) { fprintf(stderr,"[gpu-output2-region] host display unavailable\n"); return; }
    for (NSNumber* pid in gPopupOwners)
      if (!rd_output2_direct_allowed(
            gPopupOwners[pid][0].unsignedIntValue == matchID.unsignedIntValue)) return;
    const int regionsReady = present_output_regions(full, NULL, width, height);
    if (regionsReady != 0 || gOutputSpecCount < 1) {
      const char* path = getenv("RD_SECONDARY_REGION_LIST_PATH");
      if (regionsReady == 0 && gOutputSpecCount < 1 &&
          rd_secondary_manifest_unpublished(path)) {
        // The first atomic manifest is pending. Keep the backing window
        // hidden and skip the exact matching wl_shm full-frame CPU copy.
        if (!gSkipNextCPU) gSkipNextCPU = [NSMutableSet new];
        if (!gDirectPublishedAt) gDirectPublishedAt = [NSMutableDictionary new];
        [gSkipNextCPU addObject:matchID];
        gDirectPublishedAt[matchID] = @(NSProcessInfo.processInfo.systemUptime);
        result = 1;
      }
      static unsigned layout;
      if (layout++ < 8) fprintf(stderr,"[gpu-output2-region] region layout not ready count=%d\n",gOutputSpecCount);
      return;
    }
    typedef id<MTLCommandQueue> (*queue_fn)(void);
    queue_fn getQueue = (queue_fn)dlsym(RTLD_DEFAULT, "gpu_metal_queue");
    id<MTLCommandQueue> queue = getQueue ? getQueue() : nil;
    if (!queue || gOutputSpecCount < 1 || gOutputSpecCount > RD_SECONDARY_MAX_REGIONS) return;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
      width:width height:height mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [queue.device newTextureWithDescriptor:td iosurface:surface plane:0];
    if (!texture) return;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!command) return;
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) return;
    id<CAMetalDrawable> drawables[RD_SECONDARY_MAX_REGIONS] = {nil};
    for (int i = 0; i < gOutputSpecCount; i++) {
      struct rd_secondary_region spec = gOutputSpecs[i];
      RDSecondaryRegionFrame* frame = gOutputRegions[@(spec.id)];
      if (!frame || !frame.window.isVisible) { [blit endEncoding]; return; }
      NSView* view = frame.window.contentView;
      CAMetalLayer* layer = [view.layer isKindOfClass:CAMetalLayer.class] ? (CAMetalLayer*)view.layer : nil;
      if (!layer) {
        layer = [CAMetalLayer layer];
        layer.device = queue.device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.opaque = YES;
        layer.allowsNextDrawableTimeout = YES;
        view.wantsLayer = YES;
        view.layer = layer;
      }
      if (layer.device != queue.device) { [blit endEncoding]; return; }
      layer.drawableSize = CGSizeMake(spec.w, spec.h);
      drawables[i] = [layer nextDrawable];
      if (!drawables[i] || drawables[i].texture.width != spec.w ||
          drawables[i].texture.height != spec.h) { [blit endEncoding]; return; }
      [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
        sourceOrigin:MTLOriginMake(spec.x,spec.y,0) sourceSize:MTLSizeMake(spec.w,spec.h,1)
        toTexture:drawables[i].texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0,0,0)];
    }
    [blit endEncoding];
    uint64_t feedbackSequence = getenv("RDNATIVE_PRESENT_FEEDBACK") ? rd_feedback_submit(1) : 0;
    __block int remaining = gOutputSpecCount;
    for (int i = 0; i < gOutputSpecCount; i++) {
      if (feedbackSequence) {
        [drawables[i] addPresentedHandler:^(id<MTLDrawable> presented) {
          (void)presented;
          dispatch_async(dispatch_get_main_queue(), ^{
            if (--remaining == 0) rd_feedback_finish(1, feedbackSequence);
          });
        }];
      }
      [command presentDrawable:drawables[i]];
    }
    if (feedbackSequence)
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                     dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        rd_feedback_finish(1, feedbackSequence); // display occlusion/drop recovery
      });
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      if (feedbackSequence) rd_feedback_finish(1, feedbackSequence);
      return;
    }
    // RDGPU_SCANOUT=1: hand WAYLAND-2's surface (now fenced by the waitUntilCompleted
    // above) to the scanout service as the SECONDARY display (role 1), mirroring the
    // primary handoff in window.m's present_source. The region CAMetalLayer present
    // above stays as the WindowServer path for non-scanout runs; in scanout mode the
    // builtin SLCADisplay trampoline substitutes this surface at present_iosurface time.
    if (rdgpu_scanout_enabled()) {
      int sr = rdgpu_scanout_present(surface, RDGPU_SCANOUT_ROLE_SECONDARY);
      if (sr != 0) {
        static unsigned ssaid;
        if (ssaid++ < 4)
          fprintf(stderr, "[rdgpu-scanout] secondary present failed rc=%d\n", sr);
      }
    }
    if (!gSkipNextCPU) gSkipNextCPU = [NSMutableSet new];
    if (!gDirectPublishedAt) gDirectPublishedAt = [NSMutableDictionary new];
    [gSkipNextCPU addObject:matchID];
    gDirectPublishedAt[matchID] = @(NSProcessInfo.processInfo.systemUptime);
    static unsigned frames;
    if (++frames <= 3 || frames % 100 == 0)
      fprintf(stderr,"[gpu-output2-region] direct Metal frame=%u regions=%d window=%u\n",
              frames,gOutputSpecCount,matchID.unsignedIntValue);
    result = 0;
  } });
  return result;
}

// xdg_popup can extend beyond the content rect of its owning toplevel. Give it
// a borderless child window so WindowServer clips it at the screen, not at the
// dialog edge. Input is translated back to owner-local Wayland coordinates.
int rd_secondary_popup_present(uint32_t pid, uint32_t owner_id, int32_t x, int32_t y,
                               const uint8_t* bgra, uint32_t width, uint32_t height) {
  if (!pid || !owner_id || !bgra || !width || !height || width > 16384 || height > 16384) return -1;
  size_t size = (size_t)width * height * 4;
  if (size / 4 != (size_t)width * height) return -1;
  NSData* copy = [NSData dataWithBytes:bgra length:size];
  __block int result = 0;
  on_main(^{ @autoreleasepool {
    NSWindow* owner = gWindows[@(owner_id)];
    if (!owner) { result = -1; return; }
    if (!gPopups) { gPopups = [NSMutableDictionary new]; gPopupOwners = [NSMutableDictionary new]; }
    NSWindow* win = gPopups[@(pid)];
    BOOL isNew = !win;
    if (!win) {
      win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                      styleMask:NSWindowStyleMaskBorderless
                                        backing:NSBackingStoreBuffered defer:NO];
      if (!win) { result = -1; return; }
      win.releasedWhenClosed = NO;
      win.opaque = NO;
      win.backgroundColor = NSColor.clearColor;
      win.hasShadow = YES;
      win.contentView.wantsLayer = YES;
      gPopups[@(pid)] = win;
      install_monitor();
      [owner addChildWindow:win ordered:NSWindowAbove];
    }
    gPopupOwners[@(pid)] = @[@(owner_id), @(x), @(y)];
    if (win.contentView.bounds.size.width != width || win.contentView.bounds.size.height != height)
      [win setContentSize:NSMakeSize(width, height)];
    NSPoint topLeft = [owner convertPointToScreen:NSMakePoint(0, owner.contentView.bounds.size.height)];
    [win setFrameOrigin:NSMakePoint(topLeft.x + x, topLeft.y - y - height)];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    if (!provider) { result = -1; return; }
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, color,
                                    kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                    provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(color); CGDataProviderRelease(provider);
    if (!image) { result = -1; return; }
    win.contentView.layer.contents = (__bridge id)image;
    CGImageRelease(image);
    [win orderFrontRegardless];
    if (isNew) fprintf(stderr, "[multiwin] popup %u opened over surface %u at %d,%d size %ux%u\n",
                       pid, owner_id, x, y, width, height);
    const char* click = getenv("RDWL_MULTIWINDOW_TEST_POPUP_CLICK");
    if (click && isNew) {
      int cx = 0, cy = 0;
      if (sscanf(click, "%d,%d", &cx, &cy) == 2) {
        NSInteger number = win.windowNumber;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
          NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
          NSPoint at = NSMakePoint(cx, win.contentView.bounds.size.height - cy);
          for (int down = 1; down >= 0; down--) {
            NSEvent* event = [NSEvent mouseEventWithType:down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp
                                              location:at modifierFlags:0 timestamp:now windowNumber:number
                                                context:nil eventNumber:0 clickCount:1 pressure:down ? 1 : 0];
            if (event) [NSApp postEvent:event atStart:NO];
          }
          fprintf(stderr, "[multiwin] injected popup click at %d,%d into native window %ld\n",
                  cx, cy, (long)number);
        });
      }
    }
  } });
  return result;
}

void rd_secondary_popup_close(uint32_t pid) {
  on_main(^{
    NSWindow* win = gPopups[@(pid)];
    if (!win) return;
    [win.parentWindow removeChildWindow:win];
    [win orderOut:nil]; [win close];
    [gPopups removeObjectForKey:@(pid)];
    [gPopupOwners removeObjectForKey:@(pid)];
    fprintf(stderr, "[multiwin] popup %u closed\n", pid);
  });
}

void rd_secondary_close(uint32_t wid) {
  on_main(^{ @autoreleasepool {
    NSNumber* key = @(wid);
    [gSkipNextCPU removeObject:key];
    [gSkippedCPUCommit removeObject:key];
    [gDirectPublishedAt removeObjectForKey:key];
    NSWindow* win = gWindows[@(wid)];
    if (!win) return;
    if (wid == gOutput2Wid) {
      for (RDSecondaryRegionFrame* frame in gOutputRegions.allValues) {
        [frame.window orderOut:nil];
        [frame.window close];
      }
      [gOutputRegions removeAllObjects];
      gOutput2Wid = 0;
      gOutputSpecCount = -1;
      gRegionInode = 0;
      gAppliedRegionFocus = nil;
    }
    NSWindow* parent = win.parentWindow;
    for (NSNumber* pid in [gPopupOwners allKeys])
      if (gPopupOwners[pid][0].unsignedIntValue == wid) rd_secondary_popup_close(pid.unsignedIntValue);
    [win.parentWindow removeChildWindow:win];
    [win orderOut:nil];
    [win close];
    [gWindows removeObjectForKey:@(wid)];
    [gOriginalFrames removeObjectForKey:@(wid)];
    fprintf(stderr, "[multiwin] surface %u closed\n", wid);
    publish_windows();
    if (parent && getenv("RDWL_MULTIWINDOW_TEST_PRIMARY_CLICK_AFTER_CLOSE")) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        NSPoint at = NSMakePoint(100, 100);
        for (int down = 1; down >= 0; down--) {
          NSEvent* click = [NSEvent mouseEventWithType:down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp
                                             location:at modifierFlags:0
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:parent.windowNumber context:nil eventNumber:0
                                           clickCount:1 pressure:down ? 1 : 0];
          if (click) [NSApp postEvent:click atStart:NO];
        }
        fprintf(stderr, "[multiwin] injected primary click after surface %u closed\n", wid);
      });
    }
  } });
}

int rd_secondary_count(void) {
  __block int count = 0;
  on_main(^{ count = (int)gWindows.count; });
  return count;
}

int rd_secondary_window_number(uint32_t wid) {
  __block int number = 0;
  on_main(^{ number = (int)[gWindows[@(wid)] windowNumber]; });
  return number;
}

int rd_secondary_has_parent(uint32_t child_id, uint32_t parent_id) {
  __block int linked = 0;
  on_main(^{
    NSWindow* child = gWindows[@(child_id)];
    NSWindow* parent = gWindows[@(parent_id)];
    linked = child && parent && child.parentWindow == parent;
  });
  return linked;
}
