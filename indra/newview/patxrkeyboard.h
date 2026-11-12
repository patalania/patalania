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
// Placement: until the hands have been seen, the window sits below and in front
// of the head (or where it was calibrated last time, if that fits the current
// head position). When both hands rest ~1 s at a similar height on the
// keyboard, the window moves onto them: centre, height, direction (line between
// the hands, checked against the direction of the fingers; a hand on the mouse
// is ignored) and width. Later resting periods near the window let it follow
// softly. Manual adjustments (shortcuts below) stay on top of that.
//
// Shortcuts (hold Ctrl+Shift):
//   arrows        move left/right, away/closer      PgUp/PgDn   higher/lower
//   Home/End      turn left/right                   Ins/Del     wider/narrower
//   Alt+PgUp/PgDn deeper/shallower                  Backspace   measure again
namespace PATXRKeyboard
{
    // Placement + shortcuts. Called once per frame with both eye poses.
    void update(F32 dt, const XrPosef& left_eye, const XrPosef& right_eye, const PATXRPalm palms[2]);

    // Draw into the currently bound framebuffer (viewport already set):
    // white frame + transparent keyboard window, plus a transparent square
    // around each tracked palm that is near desk height.
    void draw(const XrView& view, const PATXRPalm palms[2]);

    // Release GL objects and forget the placement (GL context current).
    void release();
}

#endif // PAT_XRKEYBOARD_H
