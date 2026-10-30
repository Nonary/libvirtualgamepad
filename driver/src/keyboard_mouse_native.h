// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#ifndef LVG_ENABLE_KEYBOARD_MOUSE_SOURCE
#define LVG_ENABLE_KEYBOARD_MOUSE_SOURCE 0
#endif
#if LVG_ENABLE_KEYBOARD_MOUSE_SOURCE != 0 && LVG_ENABLE_KEYBOARD_MOUSE_SOURCE != 1
#error LVG_ENABLE_KEYBOARD_MOUSE_SOURCE must be zero or one
#endif
#if LVG_ENABLE_KEYBOARD_MOUSE_SOURCE
// VHF permits zero vendor/product identifiers. A product may override these
// when it has a registered identity, without borrowing another vendor's ID.
#ifndef LVG_KM_VENDOR_ID
#define LVG_KM_VENDOR_ID 0
#endif
#ifndef LVG_KM_PRODUCT_ID
#define LVG_KM_PRODUCT_ID 0
#endif
static_assert(LVG_KM_VENDOR_ID >= 0 && LVG_KM_VENDOR_ID <= 0xffff);
static_assert(LVG_KM_PRODUCT_ID >= 0 && LVG_KM_PRODUCT_ID <= 0xffff);
#include "keyboard_mouse.h"
#include "libvirtualgamepad/keyboard_mouse_control.h"
#endif
