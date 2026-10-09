/**
 ******************************************************************************
 * LibertyRecomp : GTA IV for modern platforms                               *
 ******************************************************************************
 * Copyright 2026 LibertyRecomp contributors. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/graphics/IOGraphicsTypes.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/ui/surface_mac.h>

namespace rex::ui {

bool NativeDisplayPixelSizeForWindow(void* ns_window, uint32_t& width, uint32_t& height) {
  NSWindow* window = (__bridge NSWindow*)ns_window;
  NSScreen* screen = window ? window.screen : NSScreen.mainScreen;
  NSNumber* number = screen ? screen.deviceDescription[@"NSScreenNumber"] : nil;
  if (!number) return false;
  const CGDirectDisplayID display = number.unsignedIntValue;
  NSDictionary* options = @{(__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes: @YES};
  CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options);
  if (!modes) return false;
  uint32_t native_w = 0, native_h = 0, largest_w = 0, largest_h = 0;
  for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
    auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
    const uint32_t w = uint32_t(CGDisplayModeGetPixelWidth(mode));
    const uint32_t h = uint32_t(CGDisplayModeGetPixelHeight(mode));
    if (CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag) { native_w = w; native_h = h; }
    if (uint64_t(w) * h > uint64_t(largest_w) * largest_h) { largest_w = w; largest_h = h; }
  }
  CFRelease(modes);
  // Scaled modes expose a backing store larger than the panel; the native-flagged mode is the
  // panel itself. Without the flag, the largest mode is the best guess.
  width = native_w ? native_w : largest_w;
  height = native_h ? native_h : largest_h;
  return width && height;
}

void ConfigureMetalLayerForPresentation(void* layer_pointer) {
  CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_pointer;
  if (!layer) {
    return;
  }

  // Core Animation owns a finite drawable pool. A permanent nextDrawable
  // wait can otherwise strand MoltenVK's asynchronous queue and anything
  // waiting on its Vulkan fence. Keep Apple's bounded behavior explicit.
  layer.allowsNextDrawableTimeout = YES;
  // Vulkan presentation is committed independently of Core Animation
  // transactions. This is the normal low-latency CAMetalLayer mode.
  layer.presentsWithTransaction = NO;
}

}  // namespace rex::ui
