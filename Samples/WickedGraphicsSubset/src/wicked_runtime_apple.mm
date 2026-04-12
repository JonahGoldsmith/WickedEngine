#if defined(__APPLE__)

#include "wiAppleHelper.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace wi::apple
{
void SetMetalLayerToWindow(void* window_handle, void* layer_handle)
{
    id host = (__bridge id)window_handle;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_handle;

    if (host == nil || layer == nil)
    {
        return;
    }

    NSView* target_view = nil;
    if ([host isKindOfClass:[NSWindow class]])
    {
        target_view = ((NSWindow*)host).contentView;
    }
    else if ([host isKindOfClass:[NSView class]])
    {
        target_view = (NSView*)host;
    }

    if (target_view == nil)
    {
        return;
    }

    layer.displaySyncEnabled = NO;
    layer.presentsWithTransaction = NO;
    layer.allowsNextDrawableTimeout = NO;
    if ([layer respondsToSelector:@selector(setMaximumDrawableCount:)])
    {
        layer.maximumDrawableCount = 3;
    }

    target_view.wantsLayer = YES;
    target_view.layer = layer;
}
}

#endif
