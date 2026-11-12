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
};

#endif // PAT_XRSESSION_H
