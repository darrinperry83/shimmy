#import <AppKit/AppKit.h>
#include "gpu.h"
#include <stdint.h>
#include <stdio.h>

int rd_secondary_open(uint32_t, uint32_t, uint32_t, const char*, uint32_t);
int rd_secondary_present(uint32_t, const uint8_t*, uint32_t, uint32_t);
int rd_secondary_popup_present(uint32_t, uint32_t, int32_t, int32_t,
                               const uint8_t*, uint32_t, uint32_t);
void rd_secondary_popup_close(uint32_t);
void rd_secondary_close(uint32_t);
int rd_secondary_count(void);
int rd_secondary_window_number(uint32_t);
int rd_secondary_has_parent(uint32_t, uint32_t);
int rd_secondary_next_event(uint32_t*, struct gpuwin_event*);

static int first_pixel_is(int number, uint8_t b, uint8_t g, uint8_t r) {
  for (NSWindow* win in [NSApp windows]) {
    if (win.windowNumber != number) continue;
    CGImageRef image = (__bridge CGImageRef)win.contentView.layer.contents;
    if (!image) return 0;
    CFDataRef bytes = CGDataProviderCopyData(CGImageGetDataProvider(image));
    if (!bytes || CFDataGetLength(bytes) < 4) { if (bytes) CFRelease(bytes); return 0; }
    const uint8_t* px = CFDataGetBytePtr(bytes);
    int match = px[0] == b && px[1] == g && px[2] == r;
    CFRelease(bytes);
    return match;
  }
  return 0;
}

int main(void) { @autoreleasepool {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  uint8_t red[8 * 8 * 4], blue[8 * 8 * 4];
  for (int i = 0; i < 64; i++) {
    red[i * 4] = 0; red[i * 4 + 1] = 0; red[i * 4 + 2] = 255; red[i * 4 + 3] = 255;
    blue[i * 4] = 255; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 0; blue[i * 4 + 3] = 255;
  }
  if (rd_secondary_open(11, 320, 200, "Primary fixture", 0) ||
      rd_secondary_open(12, 180, 100, "Child fixture", 11)) return 1;
  if (rd_secondary_count() != 2 ||
      rd_secondary_window_number(11) <= 0 ||
      rd_secondary_window_number(12) <= 0 ||
      rd_secondary_window_number(11) == rd_secondary_window_number(12) ||
      !rd_secondary_has_parent(12, 11)) return 2;
  if (rd_secondary_present(11, red, 8, 8) || rd_secondary_present(12, blue, 8, 8) ||
      !first_pixel_is(rd_secondary_window_number(11), 0, 0, 255) ||
      !first_pixel_is(rd_secondary_window_number(12), 255, 0, 0)) return 3;
  NSEvent* motion = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                     location:NSMakePoint(8, 6) modifierFlags:0 timestamp:0
                                 windowNumber:rd_secondary_window_number(12) context:nil
                                   eventNumber:0 clickCount:0 pressure:0];
  [NSApp sendEvent:motion];
  uint32_t event_window = 0;
  struct gpuwin_event event = {0};
  if (!rd_secondary_next_event(&event_window, &event) || event_window != 12 ||
      event.type != GPUWIN_EV_MOTION || event.x != 8 || event.y != 94) return 6;
  NSWindow* child = nil;
  for (NSWindow* win in NSApp.windows)
    if (win.windowNumber == rd_secondary_window_number(12)) child = win;
  if (!child || !(child.styleMask & NSWindowStyleMaskResizable)) return 8;
  [child setContentSize:NSMakeSize(210, 130)];
  if (!rd_secondary_next_event(&event_window, &event) || event_window != 12 ||
      event.type != GPUWIN_EV_RESIZE || event.x != 210 || event.y != 130) return 9;
  if (rd_secondary_popup_present(13, 12, 15, 125, red, 8, 8)) return 10;
  NSWindow* popup = nil;
  for (NSWindow* win in NSApp.windows)
    if (win.parentWindow == child && !(win.styleMask & NSWindowStyleMaskTitled)) popup = win;
  if (!popup || popup.frame.origin.y >= child.frame.origin.y + child.contentView.bounds.size.height - 125)
    return 11;
  NSEvent* popupMotion = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                           location:NSMakePoint(4, 6) modifierFlags:0 timestamp:0
                                       windowNumber:popup.windowNumber context:nil
                                         eventNumber:0 clickCount:0 pressure:0];
  [NSApp sendEvent:popupMotion];
  if (!rd_secondary_next_event(&event_window, &event) || event_window != 12 ||
      event.type != GPUWIN_EV_MOTION || event.x != 19 || event.y != 127) return 12;
  rd_secondary_popup_close(13);
  if (popup.isVisible) return 13;
  for (uint32_t target = 11; target <= 12; target++) {
    NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                               modifierFlags:0 timestamp:0 windowNumber:rd_secondary_window_number(target)
                                   context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                                 isARepeat:NO keyCode:0];
    [NSApp sendEvent:key];
    if (!rd_secondary_next_event(&event_window, &event) || event_window != target ||
        event.type != GPUWIN_EV_KEY || event.code != 30 || !event.state) return 14;
  }
  for (NSWindow* win in NSApp.windows)
    if (win.windowNumber == rd_secondary_window_number(12)) [win performClose:nil];
  if (!rd_secondary_next_event(&event_window, &event) || event_window != 12 ||
      event.type != 9 || rd_secondary_count() != 2) return 7;
  rd_secondary_close(12);
  if (rd_secondary_count() != 1) return 4;
  rd_secondary_close(11);
  if (rd_secondary_count() != 0) return 5;
  puts("SecondaryWindowHost: distinct resizable windows, tagged key input, popup overflow/input, parent, close, teardown passed");
  return 0;
} }
