/**
 * @file patxrkeyboard.h
 * @brief <PAT:XR> Passthrough window for the real keyboard (and hands) in VR.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Patalania Viewer Source Code
 * Copyright (C) 2026, the Patalania contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef PAT_XRKEYBOARD_H
#define PAT_XRKEYBOARD_H

#include <openxr/openxr.h>

#include "patxrhands.h"

// How it works (WiVRn): the session submits frames with
// XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND; WiVRn streams the alpha channel and the
// headset shows its camera image wherever alpha is 0. So we draw the keyboard
// area (a horizontal rectangle anchored in the LOCAL space) with alpha 0.
// No chroma key and no runtime configuration needed. Platform neutral (OpenGL).
//
// Placement: WiVRn's LOCAL origin/yaw is not the head at session start, so the
// default window is placed relative to the first head pose (below and in front,
// turned to the viewing direction). As soon as both hands rest ~1 s at a similar
// height, the window is moved onto them (centre, height, direction, width) and
// stays anchored in the room. Ctrl+Shift+Backspace measures again.
namespace PATXRKeyboard
{
    // Placement + test shortcuts (Ctrl+Shift + arrows/PgUp/PgDn/Home/End/Ins/Del,
    // Ctrl+Shift+Backspace = measure again). Called once per frame with both eye poses.
    void update(F32 dt, const XrPosef& left_eye, const XrPosef& right_eye, const PATXRPalm palms[2]);

    // Draw into the currently bound framebuffer (viewport already set):
    // white frame + transparent keyboard window, plus a transparent square
    // around each tracked palm that is near desk height.
    void draw(const XrView& view, const PATXRPalm palms[2]);

    // Release GL objects and forget the placement (GL context current).
    void release();
}

#endif // PAT_XRKEYBOARD_H
