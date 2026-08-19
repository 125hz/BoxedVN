// IOSDisplayShim.h - hands DXMT a CAMetalLayer to render into.
//
// Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
//
// DXMT's winemetal unix side does not talk to a Wine display driver. It looks
// up macdrv_functions with dlsym(RTLD_DEFAULT, ...) and walks
// get_win_data -> client_cocoa_view -> macdrv_view_create_metal_view ->
// macdrv_view_get_metal_layer to reach a CAMetalLayer for an HWND. That lookup
// resolves inside this process, so the layer comes from the application rather
// than from Wine.
//
// This is why the graphics path does not need a Wine display driver, and why it
// works on iOS where none exists: nothing here creates a window. There is one
// screen, so every HWND resolves to the single layer registered below.

#ifndef IOS_DISPLAY_SHIM_H
#define IOS_DISPLAY_SHIM_H

#ifdef __OBJC__
#import <QuartzCore/CAMetalLayer.h>

/// Registers the layer DXMT-rendered frames are presented into. Must be called
/// before the first D3D11 swapchain is created, which in practice means before
/// Wine starts.
void bvn_display_set_layer(CAMetalLayer *layer);

/// True once a layer has been registered. The Swift side reports this so a run
/// that would have produced no picture says so before it starts.
_Bool bvn_display_has_layer(void);
#endif

#endif
