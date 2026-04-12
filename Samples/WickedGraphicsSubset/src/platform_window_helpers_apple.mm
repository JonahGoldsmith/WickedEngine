#if defined(__APPLE__)

#include "platform_window_helpers.h"

#import <QuartzCore/CAMetalLayer.h>

void WICConfigureMetalLayerForUncapped(void* metalLayerHandle)
{
    CAMetalLayer* layer = (__bridge CAMetalLayer*)metalLayerHandle;
    if (layer == nil)
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
}

#endif
