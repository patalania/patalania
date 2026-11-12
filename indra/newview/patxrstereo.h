/**
 * @file patxrstereo.h
 * @brief <PAT:XR> Stereo rendering of the world for the VR mode (platform neutral).
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

#ifndef PAT_XRSTEREO_H
#define PAT_XRSTEREO_H

#include <openxr/openxr.h>

#include "patxrhands.h"

class LLColor4;
class LLFontGL;
class LLVector3;

// How the world gets into both eyes with as few changes to Firestorm as possible:
//
//  display() (Firestorm, unchanged flow)        PATXRStereo
//  -------------------------------------        -----------------------------------------
//  display_update_camera()  -> hook ----------> beginEyes(): viewer camera x head pose,
//                                               eye-sized pipeline buffers, LEFT eye
//                                               camera; culling with a frustum that
//                                               covers both eyes
//  cull, shadows (once), stateSort (once),
//  renderGeomDeferred, renderDeferredLighting   (= left eye)
//                           -> hook ----------> renderEyes(): post effects + name tags of
//                                               the left eye into its own target; RIGHT
//                                               eye: geometry + lighting + post; restore
//                                               the desktop camera
//  render_ui(): renderFinalize -> hook -------> drawDesktopMirror(): left eye on the
//               UI (desktop)                    monitor instead of a third post pass
//
// Everything per frame that is not per eye (object updates, textures, shadows,
// culling, sorting, sky) runs once. Head pose: only the yaw of the viewer camera
// is used as base (the horizon always stays level), the head's position and
// rotation relative to the pose at VR start come on top, 1 m = 1 m.
namespace PATXRStereo
{
    // Called by the session at VR start; size = swapchain size per eye.
    bool init(S32 width, S32 height);

    // Forget the reference head pose (VR start, runtime recentred the LOCAL space).
    void resetReference();

    // Hook 1 (after display_update_camera). Returns true when the eyes are drawn
    // this frame. views = located views for the predicted display time.
    bool beginEyes(const XrView views[2]);

    // Hook 2 (after renderDeferredLighting). Draws both eyes into own targets; the
    // keyboard passthrough windows (alpha 0) are drawn last, alpha is 1 elsewhere.
    void renderEyes(const XrView views[2], const PATXRPalm palms[2]);

    // True while between beginEyes() and the end of renderEyes().
    bool inEyes();

    // Billboard axes (see PATXRSession::pixelVectors).
    bool pixelVectors(const LLVector3& pos_agent, LLVector3& up, LLVector3& right);

    // Floating text as an upright plane in the world (see PATXRSession::drawHudText).
    bool drawHudText(const LLWString& text, const LLVector3& pos_agent, const LLVector3& right_axis,
                     const LLVector3& up_axis, const LLFontGL& font, U8 style, S32 shadow, const LLColor4& color);

    // Hook 3 (top of renderFinalize). True = desktop mirror drawn, skip the rest.
    bool drawDesktopMirror();

    // Phase 8: LOCAL space -> agent frame, with the mapping of the last rendered
    // frame (viewer camera x head reference). False before the first eye frame.
    bool roomPointToAgent(const XrVector3f& p, LLVector3& out);
    bool roomDirToAgent(const XrVector3f& d, LLVector3& out);

    // Between two frames (input handling, idle) LLViewerCamera is the pointer
    // camera of the UI panel, so that screen-space logic of the tools
    // (manipulator highlights, rectangle selection, hover) matches what the
    // pointer covers in the headset. restoreCamera() in idle(), right before
    // gAgentCamera.updateCamera(), gives Firestorm its own camera back.
    void setPointerCamera(const XrVector3f& origin, const XrVector3f& at, const XrVector3f& up, F32 vfov, F32 aspect,
                          F32 ndc_x, F32 ndc_y);
    void restoreCamera();
    bool pointerCameraActive();
    // Off-axis shift of the pointer camera (LLViewerCamera::calcProjection).
    void offsetProjection(LLMatrix4& proj);

    // Copy eye 'eye' into an OpenXR swapchain image (GL texture) of the same size.
    void copyToSwapchain(U32 eye, GLuint texture, S32 width, S32 height);

    // Safety net: leave the eye mode (restore camera/world view) if still active.
    void abort();

    // Release GL objects; restore window-sized pipeline buffers and VSync unless
    // the viewer is shutting down.
    void release(bool viewer_shutdown);
}

#endif // PAT_XRSTEREO_H
