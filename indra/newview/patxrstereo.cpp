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
#include "patxrkeyboard.h"

#include "llgl.h"
#include "llglstates.h"
#include "llhudobject.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"

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
    LLRect sWindowWorldRect;
    glm::mat4 sProj[2];
    glm::mat4 sView[2];
    LLVector3 sEyeOrigin[2], sEyeAt[2], sEyeLeft[2], sEyeUp[2];
    U64 sEyeFrames = 0;

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
    LLVector3 toWorld(const Vec3& v, const LLVector3& fwd, const LLVector3& left)
    {
        return fwd * (-v.z) + left * (-v.x) + LLVector3::z_axis * v.y;
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
        render_hud_elements();
        {
            LLGLSUIDefault gls_ui;
            gPipeline.disableLights();
            LLHUDObject::renderAll();
        }
        gGL.flush();

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
    sInitialised = true;
    sHaveRef = false;
    sEyeFrames = 0;
    LL_INFOS("PATXR") << "Stereo rendering ready: " << sW << "x" << sH << " per eye" << LL_ENDL;
    return true;
}

void PATXRStereo::resetReference()
{
    sHaveRef = false;
}

bool PATXRStereo::beginEyes(const XrView views[2])
{
    if (!sInitialised || sInEyes || gSnapshot || gCubeSnapshot || !gViewerWindow)
    {
        return false;
    }
    LLViewerCamera& cam = LLViewerCamera::instance();

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

    // Eye poses in the agent frame; frusta from the runtime's (asymmetric) FOV.
    const F32 z_near = cam.getNear();
    const F32 z_far = MAX_FAR_CLIP * 2.f; // as LLViewerWindow::setup3DRender
    F32 tl = 0.f, tr = 0.f, td = 0.f, tu = 0.f;
    for (U32 i = 0; i < 2; ++i)
    {
        const XrPosef& pose = views[i].pose;
        const Vec3 rel = unyaw({ pose.position.x - sRefPos.x, pose.position.y - sRefPos.y, pose.position.z - sRefPos.z });
        sEyeOrigin[i] = sBase.origin + toWorld(rel, fwd, left);
        sEyeAt[i] = toWorld(unyaw(rotate(pose.orientation, { 0.f, 0.f, -1.f })), fwd, left);
        sEyeLeft[i] = toWorld(unyaw(rotate(pose.orientation, { -1.f, 0.f, 0.f })), fwd, left);
        sEyeUp[i] = toWorld(unyaw(rotate(pose.orientation, { 0.f, 1.f, 0.f })), fwd, left);
        sEyeAt[i].normVec();
        sEyeLeft[i].normVec();
        sEyeUp[i].normVec();

        const XrFovf& fov = views[i].fov;
        sProj[i] = frustumOf(tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleDown), tanf(fov.angleUp), z_near, z_far);
        tl = llmin(tl, tanf(fov.angleLeft));
        tr = llmax(tr, tanf(fov.angleRight));
        td = llmin(td, tanf(fov.angleDown));
        tu = llmax(tu, tanf(fov.angleUp));
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

bool PATXRStereo::drawDesktopMirror()
{
    if (!sMirrorPending || sInEyes)
    {
        return false;
    }
    sMirrorPending = false;

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
    }
    sInEyes = false;
    sMirrorPending = false;
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
