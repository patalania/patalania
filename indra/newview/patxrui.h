/**
 * @file patxrui.h
 * @brief <PAT:XR> The desktop UI as a panel in VR, mouse pointer and pointer rays (platform neutral).
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

#ifndef PAT_XRUI_H
#define PAT_XRUI_H

#include <openxr/openxr.h>

// How the UI gets into the headset:
//
//  render_ui() (Firestorm)                      PATXRUI
//  -----------------------                      ---------------------------------------
//  renderFinalize -> desktop mirror (phase 7)
//  render_hud_elements (desktop only)
//                     -> hook --------------->  beginCapture(): own render target of the
//                                               window size, cleared to alpha 0
//  HUD attachments, 3D UI, 2D UI, debug text    (drawn into the target as usual)
//                     -> hook --------------->  endCapture(): target composited onto the
//                                               monitor (unchanged look there)
//  PATXRSession::frame() -> submit ---------->  prepare(): panel texture = UI with fixed
//                                               alpha + mouse pointer; then either a quad
//                                               layer (own swapchain) or drawn into both
//                                               eye images (setting PATVRUIQuadLayer)
//
// The panel is the whole viewer window. Default (PATVRUIMode 0): fixed in the
// real room (LOCAL space) in front of the view at VR start, like a monitor;
// walking or turning the avatar never moves it. Optional: 1 = fixed to the
// head, 2 = follows the view with a delay. Ctrl+Shift+Space short = hide/show
// the whole UI (HUDs included; the pointer stays), held = panel in front of
// the current view. Ctrl+Shift+Alt+Home/End = larger/smaller. A mouse position (window coordinates) is a point on the panel;
// clicks that reach the world use the ray from the eye centre through that point.
//
// Alpha: Firestorm blends the UI with glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
// for colour and alpha, so a single layer leaves alpha^2 in the target and a
// colour that is already multiplied by alpha. The panel texture stores
// sqrt(alpha): premultiplied alpha, exact for one layer, close for stacked ones.
// Where the panel has alpha 0 the world (and the passthrough windows) stay visible.
namespace PATXRUI
{
    // Session start/end (GL context current).
    void init();
    void release();

    // True when the UI goes into the panel (VR running, in the world).
    void setActive(bool active);
    bool isActive();
    // Whole UI hidden with Ctrl+Shift+Space.
    bool isHidden();

    // render_ui hooks.
    void beginCapture();
    void endCapture();
    bool capturing();

    // Once per frame with the located eye poses (LOCAL space): placement and keys.
    void update(F32 dt, const XrPosef& left_eye, const XrPosef& right_eye);
    // Place the panel in front of the current view at the next update.
    void recentre();

    // A UI image exists (captured at least once since VR start).
    bool hasImage();
    // Build the panel texture (alpha fix, pointer) from the last capture. Call once per submitted frame.
    void prepare();

    // Quad layer mode.
    bool useQuadLayer();
    S32 imageWidth();
    S32 imageHeight();
    void copyToSwapchain(GLuint texture, S32 width, S32 height);
    void fillQuadLayer(XrCompositionLayerQuad& layer, XrSpace space, XrSwapchain swapchain);
    // In-eye mode: draw the panel over an eye image (swapchain texture).
    void drawInEye(const XrView& view, GLuint texture, S32 width, S32 height);

    // Pointer ray in the LOCAL space through window point (u, v) in 0..1 (GL: v = 0 at the bottom).
    bool rayRoom(F32 u, F32 v, XrVector3f& origin, XrVector3f& dir);
    // Camera for screen-space logic (manipulator highlights, rectangle select):
    // at the eye centre, looking along the panel normal; the panel is an
    // off-axis window of it, (ndc_x, ndc_y) = shift of the panel centre in NDC.
    bool pointerCamera(XrVector3f& origin, XrVector3f& at, XrVector3f& up, F32& vfov, F32& aspect, F32& ndc_x, F32& ndc_y);
    // Pointer outside the panel (virtual pointer beyond the window): drawn into
    // the eye image on the plane of the panel, so the move panel <-> world is seamless.
    void drawWorldPointer(const XrView& view, GLuint texture, S32 width, S32 height);
    // Depth of the pointer, from the hover in LLViewerWindow::updateUI() (PATXRSession::pointerHover):
    // on_ui = over a UI element (HUDs included) on the panel, drawn into the panel; otherwise drawn
    // into the eye images at dist = metres from the eye centre along the pointer ray to what it
    // points at (<= 0: nothing hit, the sky). Stereo-correct, so the eyes need not converge on the
    // panel plane while looking at a near object; depth changes at once (no smoothing).
    void setPointerTarget(bool on_ui, F32 dist);
    // Log (PATXRPointer, at most 2 per second): which path drew the pointer this
    // frame (panel / world) and at which depth, with the angle of its hot spot in
    // each eye and the resulting disparity.
    void logPointerPath(const XrView views[2]);
    // The pointer is shown (not hidden by mouselook / camera steering).
    bool pointerVisible();

    // UI scale while the UI is on the panel (1 otherwise).
    F32 uiScale();
}

#endif // PAT_XRUI_H
