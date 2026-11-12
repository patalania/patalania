/**
 * @file patxrsession.h
 * @brief <PAT:XR> OpenXR session for the Patalania VR mode (platform neutral).
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

#ifndef PAT_XRSESSION_H
#define PAT_XRSESSION_H

// The VR mode is switched by the setting "PATVREnabled". All OpenXR work happens
// on the main (render) thread from frame(); Firestorm code only calls the two hooks
// below. Without a runtime or headset the viewer stays in desktop mode.
//
// Platform split: everything here is platform neutral; only the graphics binding
// (OpenGL context -> OpenXR) lives in patxrsession_<platform>.cpp.
// Builds without PAT_XR (other platforms, PATALANIA_XR=OFF) get empty stubs.
class LLColor4;
class LLFontGL;
class LLVector3;
class LLVector3d;
class LLMatrix4;

class PATXRSession
{
public:
    // Hook: once per main-loop iteration, after display(), GL context current.
    static void frame();

    // Hook: on viewer shutdown, before the GL context is destroyed.
    static void shutdown();

    // True while an OpenXR session is running (frames are submitted).
    static bool isRunning();

    // Stereo hooks inside display() (llviewerdisplay.cpp), no-ops outside VR:
    // after display_update_camera(): camera of the left eye, eye-sized buffers.
    static void beginEyes();
    // after renderDeferredLighting(): finish the left eye, render the right eye.
    static void renderEyes();
    // top of LLPipeline::renderFinalize(): true = the desktop shows the left eye
    // (already post-processed), skip the rest of renderFinalize.
    static bool drawDesktopMirror();

    // True while an eye image is drawn (screen-space tricks of the desktop UI,
    // e.g. keeping name tags inside the window, must be skipped then).
    static bool inEyes();
    // hud_render_text() (llhudrender.cpp): while an eye is drawn, the text is
    // drawn as an upright plane in the world instead of flat on the screen
    // (which would roll with the head). Returns false outside VR.
    // LLViewerCamera::getPixelVectors(): while an eye is drawn, billboards (name
    // tags, hover text) are upright and face the head centre, the same for both
    // eyes. Returns false outside VR.
    static bool pixelVectors(const LLVector3& pos_agent, LLVector3& up, LLVector3& right);
    static bool drawHudText(const LLWString& text, const LLVector3& pos_agent, const LLVector3& right_axis,
                            const LLVector3& up_axis, const LLFontGL& font, U8 style, S32 shadow, const LLColor4& color);

    // Phase 8, the UI on a panel in VR (patxrui.h); all no-ops outside VR:
    // LLAppViewer::idle() before gAgentCamera.updateCamera(): Firestorm's own
    // camera back (from the end of a frame until then, i.e. during input
    // handling and hover, the camera follows the UI panel, see
    // PATXRStereo::setPointerCamera).
    static void restoreCamera();
    // render_ui(): HUD attachments and the 2D UI go into the panel texture.
    static void beginUI();
    static void endUI();
    // render_ui(), before the 2D UI: true while the UI goes into the panel; 3D
    // overlays drawn on the screen (name tags, hover text) are skipped there,
    // the eyes have them. Also re-enables alpha writes (the HUD pass turns them off).
    static bool uiOnPanel();
    // LLViewerWindow::mouseDirectionGlobal(): direction of the ray from the eye
    // centre through the panel point under window position x, y (scaled UI px).
    static bool mouseDirection(S32 x, S32 y, LLVector3& dir);
    // LLViewerWindow::cursorIntersect(): ray start = eye centre, near plane
    // across the ray (agent frame).
    static void mouseRayStart(LLVector3& origin, LLVector3& near_normal, const LLVector3& dir);
    // LLViewerWindow::mousePointOnPlaneGlobal/mousePointOnLandGlobal: ray start (global).
    static void mouseRayOriginGlobal(LLVector3d& origin);
    // LLViewerWindow::calcDisplayScale(): extra UI scale while the UI is on the panel.
    static F32 uiScale();
    // LLViewerWindow::updateUI(): the VR pointer may be outside the window (beyond the panel, in the world).
    static bool pointerBeyondWindow();
    // FSNearbyChatControl::autohide(): the whole UI is hidden in VR (the chat bar hides again after use).
    static bool uiHidden();
    // LLViewerWindow::performPick(): pick callbacks (manipulator highlights) run
    // with the panel camera, like the input handling between frames.
    struct PointerCamera
    {
        PointerCamera();
        ~PointerCamera();
        bool mActive = false;
    };
    // LLViewerCamera::calcProjection(): off-axis shift while the panel camera is set.
    static void offsetProjection(LLMatrix4& proj);
    // LLVOAvatar::idleCalcNameTagPosition(): Firestorm moves the tag towards the
    // camera (front of the body); in VR it stays at the avatar's depth. Returns
    // what to add back (zero outside VR).
    static LLVector3 nameTagDepth(const LLVector3& towards_camera);
};

#endif // PAT_XRSESSION_H
