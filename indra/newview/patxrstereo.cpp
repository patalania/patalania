/**
 * @file patxrstereo.cpp
 * @brief <PAT:XR> Stereo rendering of the world for the VR mode (platform neutral).
 *
 * Phase 7: viewer camera x head pose, one asymmetric frustum per eye, both eyes
 * rendered through Firestorm's deferred pipeline at eye resolution. See
 * patxrstereo.h for the overall flow.
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

#include "llviewerprecompiledheaders.h"

#if PAT_XR

#include "patxrstereo.h"
#include "patxrui.h"
#include "patxrkeyboard.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "lltoolfocus.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llglstates.h"
#include "llhudobject.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llspatialpartition.h"
#include "llsmoothstep.h"
#include "llui.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "pipeline.h"
#include "llviewertexture.h"
#include "llviewershadermgr.h"
#include "llvisualeffect.h"
#include "rlvactions.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

extern bool gSnapshot;      // llviewerdisplay.cpp
extern bool gCubeSnapshot;  // pipeline.cpp
void render_hud_elements(); // pipeline.cpp

namespace
{
    // The pipeline reads the size of the 3D view from LLViewerWindow's world view
    // rectangle in many places (viewport of the post effects, FXAA, ...). While
    // the eyes are drawn it must be the eye image, exactly as Firestorm's own
    // high-resolution snapshots do it (LLViewerWindow::rawSnapshot). The member is
    // private and has no setter; instead of changing llviewerwindow.h (which would
    // touch a central Firestorm header), we reach it through the standard C++ rule
    // that access checks do not apply to explicit template instantiations.
    template <typename Tag, typename Tag::type Member>
    struct PATMemberAccess
    {
        friend typename Tag::type patMember(Tag) { return Member; }
    };
    struct WorldViewRectTag
    {
        using type = LLRect LLViewerWindow::*;
        friend type patMember(WorldViewRectTag);
    };
    template struct PATMemberAccess<WorldViewRectTag, &LLViewerWindow::mWorldViewRectRaw>;

    LLRect& worldViewRect()
    {
        return gViewerWindow->*patMember(WorldViewRectTag());
    }

    constexpr F32 CULL_BACKOFF = 0.05f;   // culling frustum starts 5 cm behind the eyes

    struct Vec3 { F32 x, y, z; };

    struct BaseCamera
    {
        LLVector3 origin, at, left, up;
        F32 view = 1.f;
        F32 aspect = 1.f;
        S32 height = 1;
    };

    S32 sW = 0, sH = 0;
    LLRenderTarget sEyeRT[2];
    GLuint sReadFbo = 0;
    GLuint sDrawFbo = 0;
    bool sInitialised = false;
    bool sInEyes = false;
    bool sMirrorPending = false;
    bool sEyeBuffers = false;  // pipeline buffers are eye-sized
    bool sVSyncOff = false;

    bool sHaveRef = false;
    Vec3 sRefPos = { 0.f, 0.f, 0.f };
    F32 sRefYaw = 0.f;

    BaseCamera sBase;
    // Mapping LOCAL space -> agent frame of the last eye frame (phase 8 rays).
    bool sHaveMap = false;
    LLVector3 sMapBase, sMapFwd, sMapLeft, sMapUp;
    // Firestorm's camera while the pointer camera is set between frames.
    bool sPointerCamera = false;
    BaseCamera sSavedCamera;
    F32 sNdcOffset[2] = { 0.f, 0.f };
    LLRect sWindowWorldRect;
    glm::mat4 sProj[2];
    glm::mat4 sView[2];
    LLVector3 sEyeOrigin[2], sEyeAt[2], sEyeLeft[2], sEyeUp[2];
    U64 sEyeFrames = 0;
    // RLVa @setoverlay, head-locked: the texture spans the union of both eye
    // FOVs (tangents in the head frame) with the same mapping in both eyes, i.e.
    // it sits at infinity and both eyes see the holes at the same place.
    F32 sEyeTan[2][4] = {};          // per eye: left, right, down, up
    F32 sUnionTan[4] = { -1.f, 1.f, -1.f, 1.f };
    bool sHaveOverlayFrame = false;  // eye frame for the overlay pick (last rendered frame)

    // Pointer depth samples (samplePointerDepth): two pixel pack buffers in turn.
    constexpr U32 DEPTH_SAMPLES = 64;
    constexpr F32 DEPTH_NEAR = 0.1f;  // m along the ray
    constexpr F32 DEPTH_FAR = 30.f;
    struct DepthSample
    {
        F32 t = 0.f;       // distance along the ray
        F32 z = 0.f;       // view depth of the ray point in the left eye
        bool valid = false;
    };
    GLuint sDepthPbo[2] = { 0, 0 };
    GLuint sDepthFbo = 0;
    DepthSample sDepthSamples[2][DEPTH_SAMPLES];
    F32 sDepthA[2] = { 0.f, 0.f }, sDepthB[2] = { 0.f, 0.f }; // projection terms of that frame
    bool sDepthPending[2] = { false, false };
    U32 sDepthSlot = 0;
    bool sDepthValid = false;
    F32 sDepthDist = -1.f;
    bool sPointerInEyes = false;   // the pointer was drawn in the world pass of this frame
    bool sHaveTagFrame = false;      // name tag geometry of the last rendered eye (pick)
    LLVector3 sTagHead;              // head centre of that eye frame (agent)
    F32 sTagPixelScale = 0.f;        // tan(vfov/2) / (eye height in pixels / 2)
    S32 sCurEye = -1;                // eye being finished (finishEye)
    bool sMirrorShown = false;       // desktop shows the left eye this frame

    Vec3 rotate(const XrQuaternionf& q, const Vec3& v)
    {
        // t = 2 * cross(q.xyz, v); v' = v + qw * t + cross(q.xyz, t)
        const F32 tx = 2.f * (q.y * v.z - q.z * v.y);
        const F32 ty = 2.f * (q.z * v.x - q.x * v.z);
        const F32 tz = 2.f * (q.x * v.y - q.y * v.x);
        return { v.x + q.w * tx + (q.y * tz - q.z * ty),
                 v.y + q.w * ty + (q.z * tx - q.x * tz),
                 v.z + q.w * tz + (q.x * ty - q.y * tx) };
    }

    // Rotation about +Y (OpenXR up) by -sRefYaw: LOCAL space -> reference frame.
    Vec3 unyaw(const Vec3& v)
    {
        const F32 c = cosf(-sRefYaw), s = sinf(-sRefYaw);
        return { c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
    }

    // Yaw of a pose: rotation about +Y that turns -Z into the horizontal forward.
    F32 yawOf(const XrQuaternionf& q)
    {
        const Vec3 f = rotate(q, { 0.f, 0.f, -1.f });
        return atan2f(-f.x, -f.z);
    }

    // OpenXR view axes (x right, y up, -z forward) -> base camera frame (forward, left, up).
    LLVector3 toWorld(const Vec3& v, const LLVector3& fwd, const LLVector3& left, const LLVector3& up)
    {
        return fwd * (-v.z) + left * (-v.x) + up * v.y;
    }

    // Phase 9: pitch of the base camera. Normally only the yaw of the viewer
    // camera is used (level horizon, the head does the pitching). With the
    // camera focused on something else than the own avatar (Alt camera, Alt+E /
    // Alt+C) its pitch towards the focus is taken over exactly as the camera
    // moves (user test: any lag made the object jump back into the centre when
    // the key was released). Optionally also the mouse pitch in mouselook
    // (smoothed there). Only entering/leaving these modes blends the pitch in
    // or out (weight 0..1 over a short time) so a mode switch does not jump.
    // Roll stays 0.
    F32 sPitchWeight = 0.f;     // 0 = level, 1 = camera pitch
    F32 sMouselookPitch = 0.f;  // smoothed mouse pitch (mouselook)
    F64 sPitchTime = 0.0;
    constexpr F32 MAX_PITCH = 80.f * DEG_TO_RAD;

    F32 updatePitch(const LLVector3& cam_at)
    {
        static LLCachedControl<bool> focus_pitch(gSavedSettings, "PATVRFocusPitch", true);
        static LLCachedControl<bool> mouselook_pitch(gSavedSettings, "PATVRMouselookMousePitch", false);
        static LLCachedControl<F32> smoothing(gSavedSettings, "PATVRPitchSmoothing", 0.25f);
        static LLCachedControl<F32> blend_time(gSavedSettings, "PATVRPitchBlendTime", 0.3f);
        const bool mouselook = gAgentCamera.cameraMouselook();
        // Mouse steering (left button held on the own avatar) orbits the camera over/under the
        // avatar like the focus camera: the same direct pitch take-over.
        const bool steering = LLToolCamera::getInstance()->mouseSteerMode();
        const bool focus = !mouselook && (!gAgentCamera.getFocusOnAvatar() || steering);
        const bool active = (focus && focus_pitch) || (mouselook && mouselook_pitch);

        const F64 now = LLTimer::getTotalSeconds();
        const F32 dt = sPitchTime > 0.0 ? llclamp((F32)(now - sPitchTime), 0.f, 0.25f) : 1.f;
        sPitchTime = now;

        const F32 cam_pitch = llclamp(asinf(llclamp(cam_at.mV[VZ], -1.f, 1.f)), -MAX_PITCH, MAX_PITCH);
        const F32 tau = llclamp((F32)smoothing, 0.f, 2.f);
        if (mouselook)
        {
            sMouselookPitch = tau < 0.001f ? cam_pitch : sMouselookPitch + (cam_pitch - sMouselookPitch) * (1.f - expf(-dt / tau));
        }
        else
        {
            sMouselookPitch = cam_pitch;
        }

        const F32 t = llclamp((F32)blend_time, 0.f, 2.f);
        const F32 step = t < 0.001f ? 1.f : dt / t;
        sPitchWeight = active ? llmin(1.f, sPitchWeight + step) : llmax(0.f, sPitchWeight - step);
        const F32 w = llsmoothstep(0.f, 1.f, sPitchWeight);
        return w * (mouselook ? sMouselookPitch : cam_pitch);
    }

    glm::mat4 frustumOf(F32 tan_l, F32 tan_r, F32 tan_d, F32 tan_u, F32 n, F32 f)
    {
        return glm::frustum(tan_l * n, tan_r * n, tan_d * n, tan_u * n, n, f);
    }

    glm::mat4 modelviewOf(const LLViewerCamera& cam)
    {
        glm::mat4 modelview(glm::make_mat4((GLfloat*)OGL_TO_CFR_ROTATION));
        GLfloat ogl_matrix[16];
        cam.getOpenGLTransform(ogl_matrix);
        return modelview * glm::make_mat4(ogl_matrix);
    }

    void loadMatrices(const glm::mat4& proj, const glm::mat4& view)
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.loadMatrix(glm::value_ptr(proj));
        set_current_projection(proj);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(glm::value_ptr(view));
        set_current_modelview(view);
    }

    void eyeViewport()
    {
        gGLViewport[0] = 0;
        gGLViewport[1] = 0;
        gGLViewport[2] = sW;
        gGLViewport[3] = sH;
        glViewport(0, 0, sW, sH);
    }

    void applyEye(U32 eye)
    {
        LLViewerCamera& cam = LLViewerCamera::instance();
        cam.setOrigin(sEyeOrigin[eye]);
        cam.setAxes(sEyeAt[eye], sEyeLeft[eye], sEyeUp[eye]);
        sView[eye] = modelviewOf(cam);
        loadMatrices(sProj[eye], sView[eye]);
        eyeViewport();
    }

    void restoreDesktop()
    {
        LLViewerCamera& cam = LLViewerCamera::instance();
        cam.setOrigin(sBase.origin);
        cam.setAxes(sBase.at, sBase.left, sBase.up);
        cam.setViewNoBroadcast(sBase.view);
        cam.setAspect(sBase.aspect);
        cam.setViewHeightInPixels(sBase.height);
        worldViewRect() = sWindowWorldRect;
        gViewerWindow->setup3DRender(); // desktop matrices, frustum planes and viewport
        sInEyes = false;
    }

    // Everything outside the passthrough windows must be opaque, otherwise the
    // real room shows through (sky, water, glow and blended overlays all leave
    // alpha < 1 in places). The keyboard windows are drawn afterwards.
    void forceOpaqueAlpha()
    {
        GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        GLfloat clear[4] = { 0.f, 0.f, 0.f, 0.f };
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        if (scissor)
        {
            glEnable(GL_SCISSOR_TEST);
        }
    }

    // Post effects (tone mapping, glow, FXAA, ...) of the eye just lit, then the
    // 3D overlays that Firestorm normally draws with the UI (name tags, hover
    // text, selection outlines, beacons), then alpha and the keyboard windows.
    void finishEye(U32 eye, const XrView& view, const PATXRPalm palms[2])
    {
        LLRenderTarget& rt = sEyeRT[eye];
        rt.bindTarget();
        glClearColor(0.f, 0.f, 0.f, 1.f);
        rt.clear();

        gPipeline.renderFinalize();

        loadMatrices(sProj[eye], sView[eye]);
        eyeViewport();
        // Name tags and hover text are billboards built from the camera's up and
        // left axes. With a tilted head they would roll along; give them a level
        // camera frame (world up, turned only about the vertical axis). The
        // matrices stay those of the eye, so positions are unchanged.
        LLViewerCamera& cam = LLViewerCamera::instance();
        LLVector3 level_at(sEyeAt[eye].mV[VX], sEyeAt[eye].mV[VY], 0.f);
        if (level_at.normVec() < 0.1f)
        {
            // looking straight up or down: use the head's up/down axis instead
            level_at = LLVector3(sEyeUp[eye].mV[VX], sEyeUp[eye].mV[VY], 0.f) * (sEyeAt[eye].mV[VZ] < 0.f ? 1.f : -1.f);
            if (level_at.normVec() < 0.01f)
            {
                level_at = LLVector3::x_axis;
            }
        }
        cam.setAxes(level_at, LLVector3::z_axis % level_at, LLVector3::z_axis);
        render_hud_elements();
        // RLVa @setoverlay: same layer as on the desktop (render_ui): above the
        // world and render_hud_elements(), below name tags/hover text, HUDs and
        // the UI (those are on the panel, a layer above the eyes). The effect's
        // run() calls PATXRStereo::drawOverlay() (hook in rlveffects.cpp).
        if (RlvActions::hasBehaviour(RLV_BHVR_SETOVERLAY))
        {
            sCurEye = (S32)eye;
            loadMatrices(sProj[eye], sView[eye]);
            LLVfxManager::instance().runEffect(EVisualEffect::RlvOverlay);
            sCurEye = -1;
            loadMatrices(sProj[eye], sView[eye]);
            eyeViewport();
        }
        {
            LLGLSUIDefault gls_ui;
            gPipeline.disableLights();
            LLHUDObject::renderAll();
        }
        gGL.flush();
        cam.setAxes(sEyeAt[eye], sEyeLeft[eye], sEyeUp[eye]);

        // The VR pointer over the world, with exactly the camera this eye's world
        // was rendered with (above everything, as before). Off: drawn by the XR
        // overlay path onto the finished eye images (PATXRUI::drawWorldPointer).
        static LLCachedControl<bool> pointer_in_world(gSavedSettings, "PATVRPointerInWorldPass", true);
        if (pointer_in_world)
        {
            const glm::mat4 pv = sProj[eye] * sView[eye];
            const bool drawn = PATXRUI::drawWorldPointerWith(
                [&](const XrVector3f& p, F32* out)
                {
                    LLVector3 a;
                    if (!PATXRStereo::roomPointToAgent(p, a))
                    {
                        return false;
                    }
                    const glm::vec4 c = pv * glm::vec4(a.mV[VX], a.mV[VY], a.mV[VZ], 1.f);
                    out[0] = c.x;
                    out[1] = c.y;
                    out[2] = c.z;
                    out[3] = c.w;
                    return true;
                },
                rt.getTexture(), sW, sH);
            sPointerInEyes = sPointerInEyes || drawn; // (framebuffer, viewport and GL state restored by PATXRUI)
        }

        forceOpaqueAlpha();
        PATXRKeyboard::draw(view, palms);
        rt.flush();
    }

    void renderSecondEye()
    {
        LLViewerCamera& cam = LLViewerCamera::instance();
        applyEye(1);
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

        // Same steps as display() does for the main view (geometry, lighting).
        gGL.setColorMask(true, true);
        gPipeline.mRT->deferredScreen.bindTarget();
        glClearColor(1, 0, 1, 1);
        gPipeline.mRT->deferredScreen.clear();
        gGL.setColorMask(true, true);
        gPipeline.renderGeomDeferred(cam, false); // occlusion queries ran for the left eye

        for (S32 i = 0; i < gGLManager.mNumTextureImageUnits; i++)
        {
            if (gGL.getTexUnit(i)->getCurrType() != LLTexUnit::TT_NONE)
            {
                gGL.getTexUnit(i)->unbind(gGL.getTexUnit(i)->getCurrType());
                gGL.getTexUnit(i)->disable();
            }
        }
        gPipeline.mRT->deferredScreen.flush();

        if (LLPipeline::sRenderDeferred)
        {
            gPipeline.renderDeferredLighting();
        }
    }
}

bool PATXRStereo::init(S32 width, S32 height)
{
    release(false);
    sW = width;
    sH = height;
    for (LLRenderTarget& rt : sEyeRT)
    {
        if (!rt.allocate(sW, sH, GL_RGBA, true))
        {
            LL_WARNS("PATXR") << "Could not allocate the eye render targets (" << sW << "x" << sH << ")" << LL_ENDL;
            release(false);
            return false;
        }
    }
    glGenFramebuffers(1, &sReadFbo);
    glGenFramebuffers(1, &sDrawFbo);

    // The headset sets the pace (xrWaitFrame); a monitor VSync on top would make
    // every frame wait twice.
    if (gViewerWindow && gViewerWindow->getWindow())
    {
        gViewerWindow->getWindow()->toggleVSync(false);
        sVSyncOff = true;
    }
    // Own body visible when looking down in mouselook (Firestorm's "show avatar in
    // mouselook"); only the runtime flag, the user's setting stays untouched.
    // The full first-person mode follows in phase 9.
    LLVOAvatar::sVisibleInFirstPerson = gSavedSettings.getBOOL("PATVRBodyInMouselook");
    if (isAgentAvatarValid())
    {
        // Attachment visibility (mesh bodies!) is only recomputed on a camera
        // mode change; already being in mouselook must not keep them hidden.
        gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
    }

    sInitialised = true;
    sHaveRef = false;
    sEyeFrames = 0;
    sPitchWeight = 0.f;
    sPitchTime = 0.0;
    LL_INFOS("PATXR") << "Stereo rendering ready: " << sW << "x" << sH << " per eye" << LL_ENDL;
    return true;
}

void PATXRStereo::resetReference()
{
    sHaveRef = false;
    sHaveMap = false;
}

bool PATXRStereo::beginEyes(const XrView views[2])
{
    sPointerInEyes = false;
    if (!sInitialised || sInEyes || gSnapshot || gCubeSnapshot || !gViewerWindow)
    {
        return false;
    }
    {
        // Preferences page: own body in mouselook on/off while VR runs.
        static LLCachedControl<bool> body(gSavedSettings, "PATVRBodyInMouselook", true);
        if (LLVOAvatar::sVisibleInFirstPerson != (bool)body)
        {
            LLVOAvatar::sVisibleInFirstPerson = body;
            if (isAgentAvatarValid())
            {
                gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
            }
        }
    }
    LLViewerCamera& cam = LLViewerCamera::instance();
    // The pointer camera must have been restored in idle();
    // never restore it later over the camera Firestorm computed for this frame.
    sPointerCamera = false;

    // Reference: head pose at the first frame (WiVRn's LOCAL origin is not the head).
    const Vec3 centre = { (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                          (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                          (views[0].pose.position.z + views[1].pose.position.z) * 0.5f };
    if (!sHaveRef)
    {
        sRefPos = centre;
        sRefYaw = yawOf(views[0].pose.orientation);
        sHaveRef = true;
        LL_INFOS("PATXR") << llformat("Head reference: x=%.2f y=%.2f z=%.2f m, yaw %.0f deg",
                                      centre.x, centre.y, centre.z, sRefYaw * RAD_TO_DEG) << LL_ENDL;
    }

    // Base = the viewer camera as Firestorm computed it this frame, yaw only.
    sBase.origin = cam.getOrigin();
    LLVector3 eye_base = sBase.origin; // only for the eyes; the desktop camera is restored unchanged
    // Mouselook with the body visible: Firestorm keeps the camera ~20 cm in
    // front of the head (and shifts the body to match) so the desktop view does
    // not clip into the head. In VR the eyes belong at the avatar's eyes,
    // otherwise one has to bend far over to see the own body. Smoothing of
    // animation bobbing and the full first-person mode follow in phase 9.
    if (gAgentCamera.cameraMouselook() && isAgentAvatarValid() &&
        gAgentAvatarp->mEyeLeftp && gAgentAvatarp->mEyeRightp)
    {
        eye_base = (gAgentAvatarp->mEyeLeftp->getWorldPosition() + gAgentAvatarp->mEyeRightp->getWorldPosition()) * 0.5f;
        // Slightly in front of the body (user test: at the eye joints one sees
        // the own back when looking down to the side). Along the body's forward
        // direction (pelvis yaw), not the viewing direction.
        static LLCachedControl<F32> fwd_offset(gSavedSettings, "PATVRFirstPersonForwardOffset", 0.05f);
        LLVector3 body_fwd = LLVector3::x_axis * gAgentAvatarp->mPelvisp->getWorldRotation();
        body_fwd.mV[VZ] = 0.f;
        if (body_fwd.normVec() > 0.1f)
        {
            eye_base += body_fwd * llclamp((F32)fwd_offset, -0.3f, 0.5f);
        }
        // A bit higher (user test: otherwise one partly looks into the own body).
        static LLCachedControl<F32> up_offset(gSavedSettings, "PATVRFirstPersonUpOffset", 0.04f);
        eye_base.mV[VZ] += llclamp((F32)up_offset, -0.2f, 0.3f);
    }
    sBase.at = cam.getAtAxis();
    sBase.left = cam.getLeftAxis();
    sBase.up = cam.getUpAxis();
    sBase.view = cam.getView();
    sBase.aspect = cam.getAspect();
    sBase.height = cam.getViewHeightInPixels();

    LLVector3 fwd = sBase.left % LLVector3::z_axis; // horizontal forward (camera has no roll)
    if (fwd.normVec() < 0.1f)
    {
        fwd = LLVector3(sBase.at.mV[VX], sBase.at.mV[VY], 0.f);
        if (fwd.normVec() < 0.1f)
        {
            fwd = LLVector3::x_axis;
        }
    }
    const LLVector3 left = LLVector3::z_axis % fwd;
    // Pitch about the (horizontal) left axis: no roll.
    const F32 pitch = updatePitch(sBase.at);
    const F32 cp = cosf(pitch), sp = sinf(pitch);
    const LLVector3 level_fwd = fwd;
    fwd = level_fwd * cp + LLVector3::z_axis * sp;
    const LLVector3 up = LLVector3::z_axis * cp - level_fwd * sp;
    sMapBase = eye_base;
    sMapFwd = fwd;
    sMapLeft = left;
    sMapUp = up;
    sHaveMap = true;

    // Eye poses in the agent frame; frusta from the runtime's (asymmetric) FOV.
    const F32 z_near = cam.getNear();
    const F32 z_far = MAX_FAR_CLIP * 2.f; // as LLViewerWindow::setup3DRender
    F32 tl = 0.f, tr = 0.f, td = 0.f, tu = 0.f;
    for (U32 i = 0; i < 2; ++i)
    {
        const XrPosef& pose = views[i].pose;
        const Vec3 rel = unyaw({ pose.position.x - sRefPos.x, pose.position.y - sRefPos.y, pose.position.z - sRefPos.z });
        sEyeOrigin[i] = eye_base + toWorld(rel, fwd, left, up);
        sEyeAt[i] = toWorld(unyaw(rotate(pose.orientation, { 0.f, 0.f, -1.f })), fwd, left, up);
        sEyeLeft[i] = toWorld(unyaw(rotate(pose.orientation, { -1.f, 0.f, 0.f })), fwd, left, up);
        sEyeUp[i] = toWorld(unyaw(rotate(pose.orientation, { 0.f, 1.f, 0.f })), fwd, left, up);
        sEyeAt[i].normVec();
        sEyeLeft[i].normVec();
        sEyeUp[i].normVec();

        const XrFovf& fov = views[i].fov;
        sProj[i] = frustumOf(tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleDown), tanf(fov.angleUp), z_near, z_far);
        sEyeTan[i][0] = tanf(fov.angleLeft);
        sEyeTan[i][1] = tanf(fov.angleRight);
        sEyeTan[i][2] = tanf(fov.angleDown);
        sEyeTan[i][3] = tanf(fov.angleUp);
        tl = llmin(tl, tanf(fov.angleLeft));
        tr = llmax(tr, tanf(fov.angleRight));
        td = llmin(td, tanf(fov.angleDown));
        tu = llmax(tu, tanf(fov.angleUp));
    }

    // Log (PATXRPointer, every 5 s): world eye cameras against the XR views.
    {
        static F64 s_last = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (now - s_last > 5.0)
        {
            s_last = now;
            const XrVector3f& a = views[0].pose.position;
            const XrVector3f& b = views[1].pose.position;
            const F32 xr_ipd = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            LL_INFOS("PATXRPointer") << llformat("eyes: world separation %.4f m, XR IPD %.4f m, eye axes angle %.3f deg, "
                                                 "fov L %.1f/%.1f/%.1f/%.1f R %.1f/%.1f/%.1f/%.1f deg, image %dx%d",
                                                 dist_vec(sEyeOrigin[0], sEyeOrigin[1]), xr_ipd,
                                                 acosf(llclamp(sEyeAt[0] * sEyeAt[1], -1.f, 1.f)) * RAD_TO_DEG,
                                                 views[0].fov.angleLeft * RAD_TO_DEG, views[0].fov.angleRight * RAD_TO_DEG,
                                                 views[0].fov.angleDown * RAD_TO_DEG, views[0].fov.angleUp * RAD_TO_DEG,
                                                 views[1].fov.angleLeft * RAD_TO_DEG, views[1].fov.angleRight * RAD_TO_DEG,
                                                 views[1].fov.angleDown * RAD_TO_DEG, views[1].fov.angleUp * RAD_TO_DEG,
                                                 sW, sH) << LL_ENDL;
        }
    }

    if (tr - tl > 0.01f && tu - td > 0.01f)
    {
        sUnionTan[0] = tl;
        sUnionTan[1] = tr;
        sUnionTan[2] = td;
        sUnionTan[3] = tu;
        sHaveOverlayFrame = true;
    }

    // From here on the pipeline renders at eye resolution.
    sWindowWorldRect = worldViewRect();
    worldViewRect() = LLRect(0, sH, sW, 0);
    gPipeline.resizeScreenTexture(); // reallocates only when the size changed
    sEyeBuffers = true;

    // Camera FOV for LOD, shadows and name tags: symmetric bound of both eyes.
    const F32 half_v = llmax(tu, -td);
    const F32 half_h = llmax(tr, -tl);
    cam.setViewNoBroadcast(2.f * atanf(half_v));
    cam.setAspect(half_h / half_v);
    cam.setViewHeightInPixels(ll_round((F32)sH * 2.f * half_v / (tu - td)));

    // Culling (and the shadow cascades) use one frustum covering both eyes:
    // between the eyes, slightly behind them, union of both FOVs.
    cam.setAxes(sEyeAt[0], sEyeLeft[0], sEyeUp[0]);
    cam.setOrigin((sEyeOrigin[0] + sEyeOrigin[1]) * 0.5f - sEyeAt[0] * CULL_BACKOFF);
    loadMatrices(frustumOf(tl, tr, td, tu, z_near, z_far), modelviewOf(cam));
    eyeViewport();
    LLViewerCamera::updateFrustumPlanes(cam);

    // Diagnosis for the own body in mouselook (phase 7 test: body not visible).
    static F64 last_ml_log = 0.0;
    if (gAgentCamera.cameraMouselook() && isAgentAvatarValid() && LLTimer::getTotalSeconds() - last_ml_log > 5.0)
    {
        last_ml_log = LLTimer::getTotalSeconds();
        S32 shown = 0, total = 0;
        for (const auto& point : gAgentAvatarp->mAttachmentPoints)
        {
            if (!point.second)
            {
                continue;
            }
            for (auto& obj : point.second->mAttachedObjects)
            {
                if (obj && obj->mDrawable.notNull() && obj->mDrawable->getSpatialBridge() && !obj->isHUDAttachment())
                {
                    ++total;
                    shown += obj->mDrawable->getSpatialBridge()->mDrawableType != 0 ? 1 : 0;
                }
            }
        }
        const LLVector3 head = gAgentAvatarp->mHeadp->getWorldPosition();
        const LLVector3 centre_eye = (sEyeOrigin[0] + sEyeOrigin[1]) * 0.5f;
        const LLVector3 avatar_eyes = (gAgentAvatarp->mEyeLeftp && gAgentAvatarp->mEyeRightp)
            ? (gAgentAvatarp->mEyeLeftp->getWorldPosition() + gAgentAvatarp->mEyeRightp->getWorldPosition()) * 0.5f : head;
        LL_INFOS("PATXR") << "Mouselook: visibleInFirstPerson=" << LLVOAvatar::sVisibleInFirstPerson
                          << " needsRenderAvatar=" << gAgent.needsRenderAvatar()
                          << " avatarVisible=" << gAgentAvatarp->isVisible()
                          << " world attachments shown " << shown << "/" << total
                          << llformat(" eye-head %.2f m, eyes centre to avatar eyes %.2f m (along view %.2f m), "
                                      "head tracking offset %.2f m, desktop camera to avatar eyes %.2f m",
                                      dist_vec(sEyeOrigin[0], head), dist_vec(centre_eye, avatar_eyes),
                                      (centre_eye - avatar_eyes) * sEyeAt[0], dist_vec(centre_eye, eye_base),
                                      dist_vec(sBase.origin, avatar_eyes)) << LL_ENDL;
    }

    // The main view of display() becomes the left eye.
    applyEye(0);
    sInEyes = true;
    return true;
}

void PATXRStereo::renderEyes(const XrView views[2], const PATXRPalm palms[2])
{
    if (!sInEyes)
    {
        return;
    }
    finishEye(0, views[0], palms);
    renderSecondEye();
    finishEye(1, views[1], palms);
    restoreDesktop();
    sMirrorPending = true;
    if (sEyeFrames++ == 0)
    {
        LL_INFOS("PATXR") << "First stereo frame rendered" << LL_ENDL;
    }
}

bool PATXRStereo::inEyes()
{
    return sInEyes;
}

namespace
{
    // Billboard axes facing 'head' with 'pixel_scale' = tan(vfov/2) / (eye height
    // in pixels / 2): the rule used for drawing in the eyes and for picking.
    void tagVectors(const LLVector3& pos_agent, const LLVector3& head, F32 pixel_scale, LLVector3& up, LLVector3& right);
}

bool PATXRStereo::pixelVectors(const LLVector3& pos_agent, LLVector3& up, LLVector3& right)
{
    if (!sInEyes)
    {
        return false;
    }
    // Face the head centre (not the eye, not the viewing direction): turning the
    // head on the spot changes nothing, both eyes see the same plane.
    const LLViewerCamera& cam = LLViewerCamera::instance();
    sTagHead = (sEyeOrigin[0] + sEyeOrigin[1]) * 0.5f;
    sTagPixelScale = tanf(cam.getView() * 0.5f) / llmax(1.f, cam.getViewHeightInPixels() * 0.5f);
    sHaveTagFrame = true;
    tagVectors(pos_agent, sTagHead, sTagPixelScale, up, right);
    return true;
}

bool PATXRStereo::tagPickVectors(const LLVector3& pos_agent, LLVector3& up, LLVector3& right)
{
    // Between frames (VR pointer ray): the plate as the last eye frame drew it.
    if (sInEyes || !sInitialised || !sHaveTagFrame)
    {
        return false;
    }
    tagVectors(pos_agent, sTagHead, sTagPixelScale, up, right);
    return true;
}

namespace
{
void tagVectors(const LLVector3& pos_agent, const LLVector3& head, F32 pixel_scale, LLVector3& up, LLVector3& right)
{
    const LLVector3 d = pos_agent - head;
    LLVector3 flat(d.mV[VX], d.mV[VY], 0.f);
    if (flat.normVec() < 0.01f)
    {
        flat = LLVector3(sEyeAt[0].mV[VX], sEyeAt[0].mV[VY], 0.f);
        if (flat.normVec() < 0.01f)
        {
            flat = LLVector3::x_axis;
        }
    }
    LLVector3 right_dir = flat % LLVector3::z_axis; // right as seen from the head
    right_dir.normVec();

    // Same pixel size rule as LLViewerCamera::getPixelVectors, with the distance
    // to the head centre.
    const F32 distance = d.magVec();
    F32 meters_per_pixel = distance * pixel_scale;
    // Phase 8 (user test): beyond a distance the tags keep their size in the
    // world, so they get smaller with distance like everything else, down to a
    // minimum share of the screen size so they stay readable at medium range.
    static LLCachedControl<F32> full_size_distance(gSavedSettings, "PATVRNameTagFullSizeDistance", 4.f);
    static LLCachedControl<F32> min_scale(gSavedSettings, "PATVRNameTagMinScale", 0.35f);
    const F32 full = llmax(0.5f, (F32)full_size_distance);
    if (distance > full)
    {
        meters_per_pixel *= llmax(full / distance, llclamp((F32)min_scale, 0.05f, 1.f));
    }
    up = LLVector3::z_axis * meters_per_pixel * gViewerWindow->getDisplayScale().mV[VY];
    right = right_dir * meters_per_pixel * gViewerWindow->getDisplayScale().mV[VX];
}
}

bool PATXRStereo::drawHudText(const LLWString& text, const LLVector3& pos_agent, const LLVector3& right_axis,
                              const LLVector3& up_axis, const LLFontGL& font, U8 style, S32 shadow, const LLColor4& color)
{
    if (!sInEyes)
    {
        return false;
    }
    // Firestorm draws floating text flat on the screen at the projected anchor;
    // in VR that rolls with the head. Here the text lies in a plane in the world:
    // one font pixel = one camera pixel vector. finishEye() gives the camera a
    // level frame, so right_axis is horizontal and up_axis is world up.
    const LLViewerCamera& cam = LLViewerCamera::instance();
    LLVector3 to_cam = cam.getOrigin() - pos_agent;
    to_cam.normVec();
    const LLVector3 pos = pos_agent + to_cam * 0.01f; // 1 cm in front of the tag background
    const LLVector3 rx = right_axis / llmax(0.01f, LLFontGL::sScaleX);
    const LLVector3 uy = up_axis / llmax(0.01f, LLFontGL::sScaleY);
    LLVector3 n = rx % uy;
    n.normVec();

    glm::mat4 plane(1.f);
    plane[0] = glm::vec4(rx.mV[VX], rx.mV[VY], rx.mV[VZ], 0.f);
    plane[1] = glm::vec4(uy.mV[VX], uy.mV[VY], uy.mV[VZ], 0.f);
    plane[2] = glm::vec4(n.mV[VX], n.mV[VY], n.mV[VZ], 0.f);
    plane[3] = glm::vec4(pos.mV[VX], pos.mV[VY], pos.mV[VZ], 1.f);

    static F64 last_log = 0.0;
    const F64 now = LLTimer::getTotalSeconds();
    if (now - last_log > 5.0)
    {
        last_log = now;
        LL_INFOS("PATXR") << llformat("Floating text in VR: up (%.2f %.2f %.2f) right (%.2f %.2f %.2f), eye up (%.2f %.2f %.2f)",
                                      up_axis.mV[VX] / up_axis.magVec(), up_axis.mV[VY] / up_axis.magVec(), up_axis.mV[VZ] / up_axis.magVec(),
                                      right_axis.mV[VX] / right_axis.magVec(), right_axis.mV[VY] / right_axis.magVec(), right_axis.mV[VZ] / right_axis.magVec(),
                                      sEyeUp[0].mV[VX], sEyeUp[0].mV[VY], sEyeUp[0].mV[VZ]) << LL_ENDL;
    }

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(get_current_modelview() * plane));
    LLUI::pushMatrix();
    LLUI::loadIdentity();
    F32 right_x;
    font.render(text, 0, 0.f, 1.f, color, LLFontGL::LEFT, LLFontGL::BASELINE, style, (LLFontGL::ShadowType)shadow,
                static_cast<S32>(text.length()), 1000, &right_x, false, true);
    LLUI::popMatrix();
    gGL.popMatrix();
    return true;
}

bool PATXRStereo::drawDesktopMirror()
{
    if (sInEyes)
    {
        return false;
    }
    sMirrorShown = false;
    if (!sMirrorPending)
    {
        return false;
    }
    sMirrorPending = false;
    sMirrorShown = true;

    // Left eye, centre cropped to the aspect of the world view.
    const LLRect r = gViewerWindow->getWorldViewRectRaw();
    gGLViewport[0] = r.mLeft;
    gGLViewport[1] = r.mBottom;
    gGLViewport[2] = r.getWidth();
    gGLViewport[3] = r.getHeight();
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);

    const F32 dst_aspect = (F32)llmax(1, r.getWidth()) / (F32)llmax(1, r.getHeight());
    S32 src_w = sW, src_h = sH;
    if ((F32)sW / (F32)sH < dst_aspect)
    {
        src_h = llmax(1, ll_round((F32)sW / dst_aspect));
    }
    else
    {
        src_w = llmax(1, ll_round((F32)sH * dst_aspect));
    }
    const S32 src_x = (sW - src_w) / 2;
    const S32 src_y = (sH - src_h) / 2;

    GLint old_read = 0, old_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEyeRT[0].getTexture(), 0);
    glBlitFramebuffer(src_x, src_y, src_x + src_w, src_y + src_h,
                      r.mLeft, r.mBottom, r.mLeft + r.getWidth(), r.mBottom + r.getHeight(),
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
    if (scissor)
    {
        glEnable(GL_SCISSOR_TEST);
    }
    gGL.setSceneBlendType(LLRender::BT_ALPHA); // as renderFinalize leaves it
    return true;
}

bool PATXRStereo::drawOverlay(LLViewerFetchedTexture* image, const LLColor4& color)
{
    if (!sInEyes)
    {
        // Desktop window: when it shows the left eye, the overlay is already in
        // that image (head-locked); drawing it again would double it.
        return sMirrorShown;
    }
    if (sCurEye < 0 || !image)
    {
        return true; // never on the panel or flat over the eye
    }
    const F32* et = sEyeTan[sCurEye];
    const F32 uw = sUnionTan[1] - sUnionTan[0];
    const F32 vh = sUnionTan[3] - sUnionTan[2];
    // Eye edges in texture coordinates. Both eyes look the same way (parallel
    // views), so the head-frame tangent is linear across each eye image.
    const F32 u0 = (et[0] - sUnionTan[0]) / uw, u1 = (et[1] - sUnionTan[0]) / uw;
    const F32 v0 = (et[2] - sUnionTan[2]) / vh, v1 = (et[3] - sUnionTan[2]) / vh;

    image->addTextureStats((F32)(sW * sH));
    image->setKnownDrawSize(sW, sH);

    LLGLSLShader* prev = LLGLSLShader::sCurBoundShaderPtr;
    gUIProgram.bind();
    {
        LLGLSUIDefault gls_ui;
        LLGLDisable cull(GL_CULL_FACE);
        gGL.setColorMask(true, true); // alpha is forced to 1 afterwards (forceOpaqueAlpha)
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        eyeViewport();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, 1.f, 0.f, 1.f, -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();

        gGL.getTexUnit(0)->bind(image);
        gGL.color4fv(color.mV);
        gGL.begin(LLRender::TRIANGLES);
        gGL.texCoord2f(u1, v1); gGL.vertex2f(1.f, 1.f);
        gGL.texCoord2f(u0, v1); gGL.vertex2f(0.f, 1.f);
        gGL.texCoord2f(u0, v0); gGL.vertex2f(0.f, 0.f);
        gGL.texCoord2f(u1, v1); gGL.vertex2f(1.f, 1.f);
        gGL.texCoord2f(u0, v0); gGL.vertex2f(0.f, 0.f);
        gGL.texCoord2f(u1, v0); gGL.vertex2f(1.f, 0.f);
        gGL.end();
        gGL.flush();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
    }
    if (prev)
    {
        prev->bind();
    }
    else
    {
        gUIProgram.unbind();
    }

    static F64 last_log = 0.0;
    const F64 now = LLTimer::getTotalSeconds();
    if (now - last_log > 10.0)
    {
        last_log = now;
        LL_INFOS("PATXR") << llformat("RLV overlay in the eyes: eye %d u %.3f..%.3f v %.3f..%.3f, colour %.2f %.2f %.2f alpha %.2f",
                                      sCurEye, u0, u1, v0, v1, color.mV[0], color.mV[1], color.mV[2], color.mV[3]) << LL_ENDL;
    }
    return true;
}

bool PATXRStereo::overlayActive()
{
    return sInitialised && sHaveOverlayFrame;
}

bool PATXRStereo::overlayCoord(const LLVector3& dir_agent, LLVector2& uv)
{
    // Head frame of the last rendered frame (the overlay moves with it).
    const F32 f = dir_agent * sEyeAt[0];
    if (f <= 0.001f)
    {
        return false; // behind the head
    }
    const F32 tx = (dir_agent * -sEyeLeft[0]) / f; // right
    const F32 ty = (dir_agent * sEyeUp[0]) / f;
    const F32 u = (tx - sUnionTan[0]) / (sUnionTan[1] - sUnionTan[0]);
    const F32 v = (ty - sUnionTan[2]) / (sUnionTan[3] - sUnionTan[2]);
    if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f))
    {
        return false; // outside the view, covered by the overlay
    }
    uv.set(llclamp(u, 0.f, 0.999f), llclamp(v, 0.f, 0.999f));
    return true;
}

bool PATXRStereo::roomPointToAgent(const XrVector3f& p, LLVector3& out)
{
    if (!sHaveMap || !sHaveRef)
    {
        return false;
    }
    out = sMapBase + toWorld(unyaw({ p.x - sRefPos.x, p.y - sRefPos.y, p.z - sRefPos.z }), sMapFwd, sMapLeft, sMapUp);
    return true;
}

bool PATXRStereo::roomDirToAgent(const XrVector3f& d, LLVector3& out)
{
    if (!sHaveMap || !sHaveRef)
    {
        return false;
    }
    out = toWorld(unyaw({ d.x, d.y, d.z }), sMapFwd, sMapLeft, sMapUp);
    out.normVec();
    return true;
}

void PATXRStereo::setPointerCamera(const XrVector3f& origin, const XrVector3f& at, const XrVector3f& up, F32 vfov, F32 aspect,
                                   F32 ndc_x, F32 ndc_y)
{
    LLVector3 o, a, u;
    if (sInEyes || !roomPointToAgent(origin, o) || !roomDirToAgent(at, a) || !roomDirToAgent(up, u))
    {
        return;
    }
    LLViewerCamera& cam = LLViewerCamera::instance();
    if (!sPointerCamera)
    {
        sSavedCamera.origin = cam.getOrigin();
        sSavedCamera.at = cam.getAtAxis();
        sSavedCamera.left = cam.getLeftAxis();
        sSavedCamera.up = cam.getUpAxis();
        sSavedCamera.view = cam.getView();
        sSavedCamera.aspect = cam.getAspect();
        sSavedCamera.height = cam.getViewHeightInPixels();
        sPointerCamera = true;
    }
    LLVector3 left = u % a;
    left.normVec();
    u = a % left;
    u.normVec();
    cam.setOrigin(o);
    cam.setAxes(a, left, u);
    cam.setViewNoBroadcast(llclamp(vfov, 0.1f, 2.5f));
    cam.setAspect(llclamp(aspect, 0.2f, 5.f));
    sNdcOffset[0] = llclamp(ndc_x, -2.f, 2.f);
    sNdcOffset[1] = llclamp(ndc_y, -2.f, 2.f);
}

bool PATXRStereo::pointerCameraActive()
{
    return sPointerCamera;
}

void PATXRStereo::offsetProjection(LLMatrix4& proj)
{
    if (sPointerCamera)
    {
        // x_clip += ndc_x * w_clip with w_clip = -z_eye (row-vector matrices)
        proj.mMatrix[2][0] -= sNdcOffset[0];
        proj.mMatrix[2][1] -= sNdcOffset[1];
    }
}

void PATXRStereo::restoreCamera()
{
    if (!sPointerCamera)
    {
        return;
    }
    sPointerCamera = false;
    LLViewerCamera& cam = LLViewerCamera::instance();
    cam.setOrigin(sSavedCamera.origin);
    cam.setAxes(sSavedCamera.at, sSavedCamera.left, sSavedCamera.up);
    cam.setViewNoBroadcast(sSavedCamera.view);
    cam.setAspect(sSavedCamera.aspect);
    cam.setViewHeightInPixels(sSavedCamera.height);
}

namespace
{
    // Result of the samples issued a frame ago: first point along the ray that
    // lies behind the visible surface, refined between the two samples around it.
    void readDepthSlot(U32 slot)
    {
        if (!sDepthPending[slot])
        {
            return;
        }
        sDepthPending[slot] = false;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, sDepthPbo[slot]);
        const F32* d = (const F32*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, DEPTH_SAMPLES * sizeof(F32), GL_MAP_READ_BIT);
        if (!d)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return;
        }
        const DepthSample* smp = sDepthSamples[slot];
        F32 dist = -1.f; // nothing up to DEPTH_FAR
        bool have_prev = false;
        F32 prev_t = 0.f, prev_f = 0.f;
        for (U32 k = 0; k < DEPTH_SAMPLES; ++k)
        {
            if (!smp[k].valid)
            {
                have_prev = false;
                continue;
            }
            const F32 ndc = 2.f * d[k] - 1.f;
            const F32 surface = (d[k] >= 0.999999f || fabsf(ndc + sDepthA[slot]) < 1e-7f)
                                    ? 1e9f : sDepthB[slot] / (ndc + sDepthA[slot]);
            const F32 f = smp[k].z - surface; // > 0: the ray point is hidden behind the surface
            if (f >= 0.f)
            {
                dist = smp[k].t;
                if (have_prev && prev_f < 0.f)
                {
                    const F32 u = prev_f / (prev_f - f);
                    dist = prev_t + (smp[k].t - prev_t) * llclamp(u, 0.f, 1.f);
                }
                break;
            }
            have_prev = true;
            prev_t = smp[k].t;
            prev_f = f;
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        sDepthDist = dist;
        sDepthValid = true;
    }
}

void PATXRStereo::samplePointerDepth(const LLVector3& origin, const LLVector3& dir)
{
    if (!sInitialised || sW <= 0 || sH <= 0 || !sEyeRT[0].getDepth())
    {
        return;
    }
    GLint old_read_fbo = 0, old_pack = 0, old_align = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_align);
    if (!sDepthPbo[0])
    {
        glGenBuffers(2, sDepthPbo);
        for (GLuint pbo : sDepthPbo)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, DEPTH_SAMPLES * sizeof(F32), nullptr, GL_STREAM_READ);
        }
        glGenFramebuffers(1, &sDepthFbo);
    }
    // leftovers not read in between (older slot first, so the newer result wins)
    readDepthSlot(sDepthSlot);
    readDepthSlot(sDepthSlot ^ 1u);

    const U32 slot = sDepthSlot;
    sDepthSlot ^= 1u;
    DepthSample* smp = sDepthSamples[slot];
    const glm::mat4& proj = sProj[0];
    sDepthA[slot] = proj[2][2];
    sDepthB[slot] = proj[3][2];
    const F32* tan = sEyeTan[0];
    const F32 near_z = sDepthB[slot] / (-1.f + sDepthA[slot]);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sDepthFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sEyeRT[0].getDepth(), 0);
    glReadBuffer(GL_NONE);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sDepthPbo[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    for (U32 k = 0; k < DEPTH_SAMPLES; ++k)
    {
        DepthSample& sample = smp[k];
        sample.valid = false;
        sample.t = DEPTH_NEAR * powf(DEPTH_FAR / DEPTH_NEAR, (F32)k / (F32)(DEPTH_SAMPLES - 1));
        if (!complete)
        {
            continue;
        }
        const LLVector3 v = origin + dir * sample.t - sEyeOrigin[0];
        const F32 z = v * sEyeAt[0];
        if (z <= near_z * 1.01f)
        {
            continue;
        }
        const F32 tx = -(v * sEyeLeft[0]) / z;
        const F32 ty = (v * sEyeUp[0]) / z;
        const S32 px = (S32)floorf((tx - tan[0]) / (tan[1] - tan[0]) * (F32)sW);
        const S32 py = (S32)floorf((ty - tan[2]) / (tan[3] - tan[2]) * (F32)sH);
        if (px < 0 || py < 0 || px >= sW || py >= sH)
        {
            continue;
        }
        sample.z = z;
        sample.valid = true;
        glReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void*)(intptr_t)(k * sizeof(F32)));
    }
    sDepthPending[slot] = complete;
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, old_align);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
}

void PATXRStereo::skipPointerDepth()
{
    sDepthValid = false;
}

bool PATXRStereo::pointerDrawnInEyes()
{
    return sPointerInEyes;
}

bool PATXRStereo::pointerDepth(F32& dist)
{
    // between frames (idle): the samples of the last eye frame; the eye images
    // were finished and submitted since, so the read does not wait.
    if (sDepthPbo[0])
    {
        GLint old_pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
        readDepthSlot(sDepthSlot ^ 1u);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pack);
    }
    if (!sDepthValid)
    {
        return false;
    }
    dist = sDepthDist;
    return true;
}

void PATXRStereo::copyToSwapchain(U32 eye, GLuint texture, S32 width, S32 height)
{
    GLint old_read = 0, old_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Plain copy: the eye image is already gamma encoded (8 bit) and
    // GL_FRAMEBUFFER_SRGB stays off, so the bytes land unchanged in the sRGB
    // swapchain image, which the runtime then reads as sRGB.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEyeRT[eye].getTexture(), 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sDrawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBlitFramebuffer(0, 0, sW, sH, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor)
    {
        glEnable(GL_SCISSOR_TEST);
    }
}

void PATXRStereo::abort()
{
    if (sInEyes)
    {
        LL_WARNS("PATXR") << "Eye rendering did not finish this frame, restoring the desktop camera" << LL_ENDL;
        restoreDesktop();
    }
    sMirrorPending = false;
}

void PATXRStereo::release(bool viewer_shutdown)
{
    if (!viewer_shutdown)
    {
        abort();
        restoreCamera();
    }
    sPointerCamera = false;
    sHaveMap = false;
    sInEyes = false;
    sMirrorPending = false;
    sMirrorShown = false;
    sHaveOverlayFrame = false;
    sHaveTagFrame = false;
    sCurEye = -1;
    for (LLRenderTarget& rt : sEyeRT)
    {
        rt.release();
    }
    if (sReadFbo)
    {
        glDeleteFramebuffers(1, &sReadFbo);
        sReadFbo = 0;
    }
    if (sDrawFbo)
    {
        glDeleteFramebuffers(1, &sDrawFbo);
        sDrawFbo = 0;
    }
    if (sDepthFbo)
    {
        glDeleteFramebuffers(1, &sDepthFbo);
        sDepthFbo = 0;
    }
    if (sDepthPbo[0])
    {
        glDeleteBuffers(2, sDepthPbo);
        sDepthPbo[0] = sDepthPbo[1] = 0;
    }
    sDepthPending[0] = sDepthPending[1] = false;
    sDepthValid = false;
    if (!viewer_shutdown)
    {
        if (sEyeBuffers)
        {
            gPipeline.resizeScreenTexture(); // back to the window size
        }
        if (sVSyncOff && gViewerWindow && gViewerWindow->getWindow())
        {
            gViewerWindow->getWindow()->toggleVSync(gSavedSettings.getBOOL("RenderVSyncEnable"));
        }
        if (sInitialised)
        {
            LLVOAvatar::sVisibleInFirstPerson = gSavedSettings.getBOOL("FirstPersonAvatarVisible");
            if (isAgentAvatarValid())
            {
                gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
            }
        }
    }
    if (sInitialised)
    {
        LL_INFOS("PATXR") << "Stereo rendering stopped after " << sEyeFrames << " frames" << LL_ENDL;
    }
    sEyeBuffers = false;
    sVSyncOff = false;
    sInitialised = false;
    sHaveRef = false;
}

#endif // PAT_XR
