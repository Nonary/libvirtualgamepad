// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#ifndef LVG_ENABLE_KEYBOARD_MOUSE_SOURCE
#define LVG_ENABLE_KEYBOARD_MOUSE_SOURCE 0
#endif
#ifndef LVG_KM_NATIVE_CONTRACT_ACKNOWLEDGED
#define LVG_KM_NATIVE_CONTRACT_ACKNOWLEDGED 0
#endif
#if LVG_ENABLE_KEYBOARD_MOUSE_SOURCE != 0 && LVG_ENABLE_KEYBOARD_MOUSE_SOURCE != 1
#error LVG_ENABLE_KEYBOARD_MOUSE_SOURCE must be zero or one
#endif
#if LVG_ENABLE_KEYBOARD_MOUSE_SOURCE
// Source opt-in only. These external inputs are not runtime/trust qualification.
#if LVG_KM_NATIVE_CONTRACT_ACKNOWLEDGED != 1
#error Review the actual UMDF2/VhfUm SDK and native contracts before enabling this source stage
#endif
#if !defined(LVG_KM_VENDOR_ID) || !defined(LVG_KM_PRODUCT_ID)
#error Supply separately reviewed project-owned keyboard/mouse identity values
#endif
static_assert(LVG_KM_VENDOR_ID > 0 && LVG_KM_VENDOR_ID <= 0xffff);
static_assert(LVG_KM_PRODUCT_ID > 0 && LVG_KM_PRODUCT_ID <= 0xffff);
#include "keyboard_mouse.h"
#include "libvirtualgamepad/keyboard_mouse_control.h"
#endif
