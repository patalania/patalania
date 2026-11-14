/**
 * @file patxrui.cpp
 * @brief <PAT:XR> The desktop UI as a panel in VR, mouse pointer and pointer rays (platform neutral).
 *
 * Phase 8: the 2D UI (floaters, menus, chat, toolbars, HUD attachments) is drawn
 * into an own render target instead of straight onto the monitor, then shown in
 * the headset on a panel anchored in the room. The mouse pointer is drawn onto
 * the panel. See patxrui.h for the flow.
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

#include "patxrui.h"
#include "patxrstereo.h"

#include "llagentcamera.h"
#include "lldir.h"
#include "llgl.h"
#include "llimagebmp.h"
#include "llkeyboard.h"
#include "lltoolfocus.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "lltooltip.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"

#include <cmath>
#include <functional>

namespace
{
    constexpr F32 NEAR_Z = 0.05f;
    constexpr F32 FAR_Z = 100.f;
    constexpr F32 MIN_WIDTH = 0.4f;     // panel width limits (m)
    constexpr F32 MAX_WIDTH = 5.0f;
    constexpr F32 RESIZE_SPEED = 0.5f;  // relative size change per second while a key is held
    constexpr F32 RECENTRE_HOLD = 0.6f; // s: Ctrl+Shift+Space held this long = recentre, shorter = show/hide
    constexpr F32 FOLLOW_START = 25.f * DEG_TO_RAD; // mode "follow": start turning after the view left this far
    constexpr F32 FOLLOW_STOP = 3.f * DEG_TO_RAD;
    constexpr F32 FOLLOW_TIME = 0.4f;   // s, time constant of the follow movement
    constexpr F32 POINTER_MIN_DIST = 0.1f;  // m, nearest pointer depth in the world
    constexpr F32 POINTER_SKY_DIST = 20.f;  // m, depth without a hit (sky) and farthest depth
    constexpr F32 POINTER_IN_FRONT = 0.985f;   // hits: slightly in front of the surface, never behind it

    // PATVRUIMode
    enum EMode { MODE_ROOM = 0, MODE_HEAD = 1, MODE_FOLLOW = 2 };

    // ------------------------------------------------------------------ math
    XrVector3f v3(F32 x, F32 y, F32 z) { return { x, y, z }; }
    XrVector3f add(const XrVector3f& a, const XrVector3f& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    XrVector3f sub(const XrVector3f& a, const XrVector3f& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    XrVector3f mul(const XrVector3f& a, F32 s) { return { a.x * s, a.y * s, a.z * s }; }
    F32 dot(const XrVector3f& a, const XrVector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    XrVector3f cross(const XrVector3f& a, const XrVector3f& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    F32 norm(XrVector3f& a)
    {
        const F32 len = sqrtf(dot(a, a));
        if (len > 1e-6f)
        {
            a = mul(a, 1.f / len);
        }
        return len;
    }

    XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v, bool inverse = false)
    {
        const F32 sgn = inverse ? -1.f : 1.f;
        const F32 qx = sgn * q.x, qy = sgn * q.y, qz = sgn * q.z, qw = q.w;
        const F32 tx = 2.f * (qy * v.z - qz * v.y);
        const F32 ty = 2.f * (qz * v.x - qx * v.z);
        const F32 tz = 2.f * (qx * v.y - qy * v.x);
        return { v.x + qw * tx + (qy * tz - qz * ty),
                 v.y + qw * ty + (qz * tx - qx * tz),
                 v.z + qw * tz + (qx * ty - qy * tx) };
    }

    // Rotation whose columns are the axes x, y, z (orthonormal, right-handed).
    XrQuaternionf quatFromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z)
    {
        const F32 m00 = x.x, m01 = y.x, m02 = z.x;
        const F32 m10 = x.y, m11 = y.y, m12 = z.y;
        const F32 m20 = x.z, m21 = y.z, m22 = z.z;
        XrQuaternionf q;
        const F32 trace = m00 + m11 + m22;
        if (trace > 0.f)
        {
            const F32 s = 0.5f / sqrtf(trace + 1.f);
            q.w = 0.25f / s;
            q.x = (m21 - m12) * s;
            q.y = (m02 - m20) * s;
            q.z = (m10 - m01) * s;
        }
        else if (m00 > m11 && m00 > m22)
        {
            const F32 s = 2.f * sqrtf(1.f + m00 - m11 - m22);
            q.w = (m21 - m12) / s;
            q.x = 0.25f * s;
            q.y = (m01 + m10) / s;
            q.z = (m02 + m20) / s;
        }
        else if (m11 > m22)
        {
            const F32 s = 2.f * sqrtf(1.f + m11 - m00 - m22);
            q.w = (m02 - m20) / s;
            q.x = (m01 + m10) / s;
            q.y = 0.25f * s;
            q.z = (m12 + m21) / s;
        }
        else
        {
            const F32 s = 2.f * sqrtf(1.f + m22 - m00 - m11);
            q.w = (m10 - m01) / s;
            q.x = (m02 + m20) / s;
            q.y = (m12 + m21) / s;
            q.z = 0.25f * s;
        }
        return q;
    }

    // LOCAL-space point -> clip space of this eye (OpenXR asymmetric frustum).
    void toClip(const XrView& view, const XrVector3f& p, F32* out)
    {
        const XrVector3f e = rotate(view.pose.orientation, sub(p, view.pose.position), true);
        const F32 l = tanf(view.fov.angleLeft), r = tanf(view.fov.angleRight);
        const F32 u = tanf(view.fov.angleUp), d = tanf(view.fov.angleDown);
        out[0] = (2.f / (r - l)) * e.x + ((r + l) / (r - l)) * e.z;
        out[1] = (2.f / (u - d)) * e.y + ((u + d) / (u - d)) * e.z;
        out[2] = -((FAR_Z + NEAR_Z) / (FAR_Z - NEAR_Z)) * e.z - (2.f * FAR_Z * NEAR_Z / (FAR_Z - NEAR_Z));
        out[3] = -e.z;
    }

    // ------------------------------------------------------------------ state
    bool sActive = false;
    LLRenderTarget sCaptureRT;
    bool sCapturing = false;
    bool sNewCapture = false;   // captured since the last prepare()
    bool sHasImage = false;
    GLboolean sSavedMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };

    GLuint sPanelTex = 0;
    S32 sTexW = 0, sTexH = 0;
    S32 sTexLevels = 1;
    bool sMipsValid = false;
    GLuint sFbo = 0;

    GLuint sProgram = 0, sVAO = 0, sVBO = 0, sDepthSampler = 0;
    GLint sTexLoc = -1, sAlphaFixLoc = -1, sDepthLoc = -1, sTintLoc = -1;
    bool sGLFailed = false;

    // Panel in the LOCAL space: centre, right/up/normal (normal towards the user), size.
    bool sPlaced = false;
    bool sRecentre = false;
    XrVector3f sC{ 0.f, 0.f, 0.f }, sR{ 1.f, 0.f, 0.f }, sU{ 0.f, 1.f, 0.f }, sN{ 0.f, 0.f, 1.f };
    F32 sW = 2.3f, sH = 1.f;
    XrVector3f sEye{ 0.f, 0.f, 0.f };
    bool sEyeValid = false;
    bool sSpaceWasHeld = false;
    F32 sSpaceHeldTime = 0.f;
    bool sSpaceRecentred = false;
    bool sResized = false;
    F32 sPanelYaw = 0.f;        // modes room/follow: horizontal direction of the panel from the eyes
    bool sFollowing = false;
    // Ctrl+Shift+Space: whole UI (floaters, menus, HUDs) hidden; the pointer stays for world clicks.
    bool sHidden = false;

    // Pointer depth (PATXRUI::setPointerTarget)
    bool sPtrOnUI = true;
    F32 sPtrTarget = -1.f;   // world: metres along the ray, <= 0 = sky
    F32 sPtrDepth = -1.f;    // smoothed depth actually drawn, -1 = not yet known

    // Tooltip at the pointer while it is over the world: cut out of the panel
    // texture in prepare() and drawn at the pointer depth (drawWorldPointer).
    GLuint sTipTex = 0;
    S32 sTipTexW = 0, sTipTexH = 0;
    bool sTipValid = false;
    S32 sTipX = 0, sTipY = 0, sTipW = 0, sTipH = 0; // panel pixels, GL (y up)
    bool sSavedUIVisible = true;
    bool sSavedHUDs = true;

    struct CursorImage
    {
        GLuint tex = 0;
        S32 w = 0, h = 0;
        S32 hot_x = 0, hot_y = 0; // from the top-left corner, as in the image file
        bool tried = false;
    };
    CursorImage sCursors[UI_CURSOR_COUNT];

    // Same images and hot spots as the SDL window (llwindowsdl2.cpp); on other
    // platforms the files are missing and the plain arrow fallback is used.
    struct CursorFile { ECursorType type; const char* file; S32 hx, hy; };
    const CursorFile CURSOR_FILES[] = {
        { UI_CURSOR_ARROW, "llarrow.BMP", 0, 0 },
        { UI_CURSOR_WAIT, "wait.BMP", 12, 15 },
        { UI_CURSOR_HAND, "hand.BMP", 7, 10 },
        { UI_CURSOR_IBEAM, "ibeam.BMP", 15, 16 },
        { UI_CURSOR_CROSS, "cross.BMP", 16, 14 },
        { UI_CURSOR_SIZENWSE, "sizenwse.BMP", 14, 17 },
        { UI_CURSOR_SIZENESW, "sizenesw.BMP", 17, 17 },
        { UI_CURSOR_SIZEWE, "sizewe.BMP", 16, 14 },
        { UI_CURSOR_SIZENS, "sizens.BMP", 17, 16 },
        { UI_CURSOR_SIZEALL, "sizeall.BMP", 17, 17 },
        { UI_CURSOR_NO, "llno.BMP", 8, 8 },
        { UI_CURSOR_WORKING, "working.BMP", 12, 15 },
        { UI_CURSOR_TOOLGRAB, "lltoolgrab.BMP", 2, 13 },
        { UI_CURSOR_TOOLLAND, "lltoolland.BMP", 1, 6 },
        { UI_CURSOR_TOOLFOCUS, "lltoolfocus.BMP", 8, 5 },
        { UI_CURSOR_TOOLCREATE, "lltoolcreate.BMP", 7, 7 },
        { UI_CURSOR_ARROWDRAG, "arrowdrag.BMP", 0, 0 },
        { UI_CURSOR_ARROWCOPY, "arrowcop.BMP", 0, 0 },
        { UI_CURSOR_ARROWDRAGMULTI, "llarrowdragmulti.BMP", 0, 0 },
        { UI_CURSOR_ARROWCOPYMULTI, "arrowcopmulti.BMP", 0, 0 },
        { UI_CURSOR_NOLOCKED, "llnolocked.BMP", 8, 8 },
        { UI_CURSOR_ARROWLOCKED, "llarrowlocked.BMP", 0, 0 },
        { UI_CURSOR_GRABLOCKED, "llgrablocked.BMP", 2, 13 },
        { UI_CURSOR_TOOLTRANSLATE, "lltooltranslate.BMP", 0, 0 },
        { UI_CURSOR_TOOLROTATE, "lltoolrotate.BMP", 0, 0 },
        { UI_CURSOR_TOOLSCALE, "lltoolscale.BMP", 0, 0 },
        { UI_CURSOR_TOOLCAMERA, "lltoolcamera.BMP", 7, 5 },
        { UI_CURSOR_TOOLPAN, "lltoolpan.BMP", 7, 5 },
        { UI_CURSOR_TOOLZOOMIN, "lltoolzoomin.BMP", 7, 5 },
        { UI_CURSOR_TOOLZOOMOUT, "lltoolzoomout.BMP", 7, 5 },
        { UI_CURSOR_TOOLPICKOBJECT3, "toolpickobject3.BMP", 0, 0 },
        { UI_CURSOR_TOOLPLAY, "toolplay.BMP", 0, 0 },
        { UI_CURSOR_TOOLPAUSE, "toolpause.BMP", 0, 0 },
        { UI_CURSOR_TOOLMEDIAOPEN, "toolmediaopen.BMP", 0, 0 },
        { UI_CURSOR_PIPETTE, "lltoolpipette.BMP", 2, 28 },
        { UI_CURSOR_TOOLSIT, "toolsit.BMP", 20, 15 },
        { UI_CURSOR_TOOLBUY, "toolbuy.BMP", 20, 15 },
        { UI_CURSOR_TOOLOPEN, "toolopen.BMP", 20, 15 },
        { UI_CURSOR_TOOLPAY, "toolbuy.BMP", 20, 15 },
        { UI_CURSOR_TOOLPATHFINDING, "lltoolpathfinding.BMP", 16, 16 },
        { UI_CURSOR_TOOLPATHFINDING_PATH_START, "lltoolpathfindingpathstart.BMP", 16, 16 },
        { UI_CURSOR_TOOLPATHFINDING_PATH_START_ADD, "lltoolpathfindingpathstartadd.BMP", 16, 16 },
        { UI_CURSOR_TOOLPATHFINDING_PATH_END, "lltoolpathfindingpathend.BMP", 16, 16 },
        { UI_CURSOR_TOOLPATHFINDING_PATH_END_ADD, "lltoolpathfindingpathendadd.BMP", 16, 16 },
        { UI_CURSOR_TOOLNO, "llno.BMP", 8, 8 },
    };

    // ------------------------------------------------------------------ GL
    const char* VERTEX_SRC =
        "#version 330 core\n"
        "layout(location = 0) in vec4 clip_pos;\n"
        "layout(location = 1) in vec2 uv;\n"
        "out vec2 v_uv;\n"
        "void main() { v_uv = uv; gl_Position = clip_pos; }\n";
    // alpha_fix: the capture holds alpha^2 (see patxrui.h) -> premultiplied alpha.
    // HUD attachments are drawn with alpha writes off (Firestorm's HUD pass):
    // where the depth buffer was written (HUD geometry) the panel is opaque, and
    // colour without alpha counts as coverage (colour over black = premultiplied).
    const char* FRAGMENT_SRC =
        "#version 330 core\n"
        "uniform sampler2D tex;\n"
        "uniform sampler2D depth_tex;\n"
        "uniform int alpha_fix;\n"
        "uniform int tint;\n"
        "in vec2 v_uv;\n"
        "out vec4 frag_color;\n"
        "void main()\n"
        "{\n"
        "    vec4 c = texture(tex, v_uv);\n"
        "    if (alpha_fix != 0)\n"
        "    {\n"
        "        c.a = sqrt(clamp(c.a, 0.0, 1.0));\n"
        "        c.a = max(c.a, max(c.r, max(c.g, c.b)));\n"
        "        if (texture(depth_tex, v_uv).r < 1.0) c.a = 1.0;\n"
        "        c.rgb = min(c.rgb, vec3(c.a));\n"
        "    }\n"
        "    if (tint != 0)\n"
        "    {\n"
        "        float l = max(c.r, max(c.g, c.b));\n"
        "        c.rgb = tint == 1 ? vec3(l, 0.0, 0.0) : vec3(0.0, l, 0.0);\n"
        "    }\n"
        "    frag_color = c;\n"
        "}\n";

    GLuint compile(GLenum type, const char* src)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = { 0 };
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            LL_WARNS("PATXR") << "UI panel shader: " << log << LL_ENDL;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    bool initGL()
    {
        if (sProgram)
        {
            return true;
        }
        if (sGLFailed)
        {
            return false;
        }
        GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SRC);
        GLuint fs = compile(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
        if (!vs || !fs)
        {
            sGLFailed = true;
            return false;
        }
        sProgram = glCreateProgram();
        glAttachShader(sProgram, vs);
        glAttachShader(sProgram, fs);
        glLinkProgram(sProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(sProgram, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            LL_WARNS("PATXR") << "UI panel shader does not link" << LL_ENDL;
            glDeleteProgram(sProgram);
            sProgram = 0;
            sGLFailed = true;
            return false;
        }
        sTexLoc = glGetUniformLocation(sProgram, "tex");
        sAlphaFixLoc = glGetUniformLocation(sProgram, "alpha_fix");
        sDepthLoc = glGetUniformLocation(sProgram, "depth_tex");
        sTintLoc = glGetUniformLocation(sProgram, "tint");
        glGenSamplers(1, &sDepthSampler);
        glSamplerParameteri(sDepthSampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glSamplerParameteri(sDepthSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(sDepthSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(sDepthSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sDepthSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        GLint old_vao = 0, old_vbo = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
        glGenVertexArrays(1, &sVAO);
        glGenBuffers(1, &sVBO);
        glBindVertexArray(sVAO);
        glBindBuffer(GL_ARRAY_BUFFER, sVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(F32) * 24, nullptr, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(F32) * 6, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(F32) * 6, (void*)(sizeof(F32) * 4));
        glBindVertexArray(old_vao);
        glBindBuffer(GL_ARRAY_BUFFER, old_vbo);
        glGenFramebuffers(1, &sFbo);
        return true;
    }

    // Raw GL state that our draws touch; Firestorm's own state cache stays valid.
    struct SavedGL
    {
        GLint program = 0, vao = 0, vbo = 0, draw_fbo = 0, read_fbo = 0, active_tex = 0, tex0 = 0, sampler0 = 0;
        GLint tex1 = 0, sampler1 = 0;
        GLint viewport[4] = { 0, 0, 0, 0 };
        GLint blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_a = 0, blend_dst_a = 0;
        GLboolean blend = GL_FALSE, depth = GL_FALSE, cull = GL_FALSE, scissor = GL_FALSE, stencil = GL_FALSE, srgb = GL_FALSE;
        GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };

        SavedGL()
        {
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
            glActiveTexture(GL_TEXTURE1);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler1);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
            blend = glIsEnabled(GL_BLEND);
            depth = glIsEnabled(GL_DEPTH_TEST);
            cull = glIsEnabled(GL_CULL_FACE);
            scissor = glIsEnabled(GL_SCISSOR_TEST);
            stencil = glIsEnabled(GL_STENCIL_TEST);
            srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
            glGetBooleanv(GL_COLOR_WRITEMASK, mask);

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindSampler(0, 0);
            glUseProgram(sProgram);
            glUniform1i(sTexLoc, 0);
            glUniform1i(sDepthLoc, 1);
            glBindVertexArray(sVAO);
            glBindBuffer(GL_ARRAY_BUFFER, sVBO);
        }

        ~SavedGL()
        {
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glUseProgram(program);
            glBindTexture(GL_TEXTURE_2D, tex0);
            glBindSampler(0, sampler0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, tex1);
            glBindSampler(1, sampler1);
            glActiveTexture(active_tex);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
            blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
            depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
            cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
            scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
            stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
            srgb ? glEnable(GL_FRAMEBUFFER_SRGB) : glDisable(GL_FRAMEBUFFER_SRGB);
            glColorMask(mask[0], mask[1], mask[2], mask[3]);
        }
    };

    void drawQuad(const F32 clip[4][4], const F32 uv[4][2])
    {
        F32 verts[24];
        for (U32 i = 0; i < 4; ++i)
        {
            verts[i * 6 + 0] = clip[i][0];
            verts[i * 6 + 1] = clip[i][1];
            verts[i * 6 + 2] = clip[i][2];
            verts[i * 6 + 3] = clip[i][3];
            verts[i * 6 + 4] = uv[i][0];
            verts[i * 6 + 5] = uv[i][1];
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Depth of the capture on unit 1 (for the alpha fix), colour stays on unit 0.
    void bindCaptureDepth()
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sCaptureRT.getDepth());
        glBindSampler(1, sDepthSampler);
        glActiveTexture(GL_TEXTURE0);
    }

    // Order of the corners: bottom-left, bottom-right, top-left, top-right.
    const F32 FULL_UV[4][2] = { { 0.f, 0.f }, { 1.f, 0.f }, { 0.f, 1.f }, { 1.f, 1.f } };

    // Rectangle in pixels of a w x h target -> clip space.
    void pixelRect(F32 left, F32 bottom, F32 right, F32 top, S32 w, S32 h, F32 clip[4][4])
    {
        const F32 x0 = 2.f * left / (F32)w - 1.f, x1 = 2.f * right / (F32)w - 1.f;
        const F32 y0 = 2.f * bottom / (F32)h - 1.f, y1 = 2.f * top / (F32)h - 1.f;
        const F32 pts[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
        for (U32 i = 0; i < 4; ++i)
        {
            clip[i][0] = pts[i][0];
            clip[i][1] = pts[i][1];
            clip[i][2] = 0.f;
            clip[i][3] = 1.f;
        }
    }

    // ------------------------------------------------------------------ pointer
    CursorImage* cursorImage(ECursorType type)
    {
        if (type < 0 || type >= UI_CURSOR_COUNT)
        {
            type = UI_CURSOR_ARROW;
        }
        CursorImage& img = sCursors[type];
        if (img.tried)
        {
            return img.tex ? &img : (type != UI_CURSOR_ARROW ? cursorImage(UI_CURSOR_ARROW) : nullptr);
        }
        img.tried = true;
        const CursorFile* entry = nullptr;
        for (const CursorFile& f : CURSOR_FILES)
        {
            if (f.type == type)
            {
                entry = &f;
                break;
            }
        }
        if (entry)
        {
            const std::string path = gDirUtilp->getAppRODataDir() + gDirUtilp->getDirDelimiter() + "res-sdl" +
                                     gDirUtilp->getDirDelimiter() + entry->file;
            LLPointer<LLImageBMP> bmp = new LLImageBMP;
            LLPointer<LLImageRaw> raw = new LLImageRaw;
            if (bmp->load(path) && bmp->decode(raw, 0.f) && raw->getComponents() >= 3 && raw->getWidth() > 0)
            {
                const S32 w = raw->getWidth(), h = raw->getHeight(), comps = raw->getComponents();
                std::vector<U8> rgba((size_t)w * h * 4);
                const U8* src = raw->getData();
                for (S32 i = 0; i < w * h; ++i)
                {
                    const U8 r = src[i * comps], g = src[i * comps + 1], b = src[i * comps + 2];
                    // The SDL cursors use 200,200,200 as the transparent colour.
                    const bool transparent = r == 200 && g == 200 && b == 200;
                    rgba[i * 4 + 0] = transparent ? 0 : r;
                    rgba[i * 4 + 1] = transparent ? 0 : g;
                    rgba[i * 4 + 2] = transparent ? 0 : b;
                    rgba[i * 4 + 3] = transparent ? 0 : 255;
                }
                GLint old_tex = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
                glGenTextures(1, &img.tex);
                glBindTexture(GL_TEXTURE_2D, img.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, old_tex);
                img.w = w;
                img.h = h;
                img.hot_x = entry->hx;
                img.hot_y = entry->hy;
            }
            else
            {
                LL_WARNS("PATXR") << "Pointer image not found: " << path << LL_ENDL;
            }
        }
        if (!img.tex && type != UI_CURSOR_ARROW)
        {
            return cursorImage(UI_CURSOR_ARROW);
        }
        return img.tex ? &img : nullptr;
    }

    // Mouselook steers the view with the mouse and hides the cursor; holding
    // Alt shows it again (HUDs, menus, touch) - then the VR pointer is drawn too.
    bool pointerHidden()
    {
        // Also while the camera tool holds the mouse (left drag on the own avatar steers it; the
        // desktop hides the cursor then, too).
        return gViewerWindow->getWindow()->isCursorHidden() &&
               (gAgentCamera.cameraMouselook() || LLToolCamera::getInstance()->hasMouseCapture());
    }

    // Point on the panel plane under the mouse (LOCAL space); inside = on the panel itself.
    bool pointerPanelPoint(XrVector3f& p, bool& inside, F32* px = nullptr, F32* py = nullptr)
    {
        if (!gViewerWindow || sTexW <= 0 || sTexH <= 0)
        {
            return false;
        }
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        const F32 mx = (F32)mouse.mX * scale.mV[VX];
        const F32 my = (F32)mouse.mY * scale.mV[VY];
        inside = mx >= 0.f && my >= 0.f && mx < (F32)sTexW && my < (F32)sTexH;
        if (px && py)
        {
            *px = mx;
            *py = my;
        }
        p = add(add(sC, mul(sR, (mx / sTexW - 0.5f) * sW)), mul(sU, (my / sTexH - 0.5f) * sH));
        return true;
    }

    // Once per frame: the drawn depth = the hit under the pointer (over the UI: the panel).
    void updatePointerDepth(F32 /*dt*/)
    {
        XrVector3f p;
        bool inside = false;
        if (!sPlaced || !sEyeValid || !pointerPanelPoint(p, inside))
        {
            return;
        }
        XrVector3f to_p = sub(p, sEye);
        const F32 panel_dist = llmax(POINTER_MIN_DIST, norm(to_p));
        if (inside && sPtrOnUI)
        {
            sPtrDepth = panel_dist;
            return;
        }
        // Always the depth of the hit under the pointer, at once in both directions:
        // the eyes see a pointer at the wrong depth double, a depth jump is less of a bother.
        sPtrDepth = sPtrTarget > 0.f ? llclamp(sPtrTarget * POINTER_IN_FRONT, POINTER_MIN_DIST, POINTER_SKY_DIST)
                                     : POINTER_SKY_DIST;
        // Diagnosis: fixed depth in metres (0 = off).
        static LLCachedControl<F32> fixed_depth(gSavedSettings, "PATVRPointerFixedDepth", 0.f);
        if ((F32)fixed_depth > 0.f)
        {
            sPtrDepth = llclamp((F32)fixed_depth, POINTER_MIN_DIST, 100.f);
        }
    }

    // Panel texture bound as draw target on sFbo: while the pointer is over the
    // world, move the tooltip next to it out of the panel (it would sit in the
    // panel plane while the eyes look at the hit, and be seen double).
    void grabToolTip()
    {
        sTipValid = false;
        if (sPtrOnUI || !gViewerWindow || !LLToolTipMgr::instance().toolTipVisible())
        {
            return;
        }
        const LLRect r = LLToolTipMgr::instance().getToolTipRect();
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        const S32 x0 = llclamp(ll_round((F32)r.mLeft * scale.mV[VX]), 0, sTexW);
        const S32 x1 = llclamp(ll_round((F32)r.mRight * scale.mV[VX]), 0, sTexW);
        const S32 y0 = llclamp(ll_round((F32)r.mBottom * scale.mV[VY]), 0, sTexH);
        const S32 y1 = llclamp(ll_round((F32)r.mTop * scale.mV[VY]), 0, sTexH);
        if (x1 - x0 < 2 || y1 - y0 < 2)
        {
            return;
        }
        sTipX = x0;
        sTipY = y0;
        sTipW = x1 - x0;
        sTipH = y1 - y0;
        if (!sTipTex)
        {
            glGenTextures(1, &sTipTex);
        }
        glBindTexture(GL_TEXTURE_2D, sTipTex);
        if (sTipTexW != sTipW || sTipTexH != sTipH)
        {
            sTipTexW = sTipW;
            sTipTexH = sTipH;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sTipW, sTipH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sFbo);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sTipX, sTipY, sTipW, sTipH);
        glGenerateMipmap(GL_TEXTURE_2D);
        // cut it out of the panel: there the world shows through, the tooltip is drawn at the pointer
        GLfloat clear[4] = { 0.f, 0.f, 0.f, 0.f };
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glEnable(GL_SCISSOR_TEST);
        glScissor(sTipX, sTipY, sTipW, sTipH);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        sTipValid = true;
    }

    // Pointer onto the panel texture (bound as draw target, blending set up).
    void drawPointer()
    {
        if (!gViewerWindow || !gViewerWindow->getWindow() || pointerHidden())
        {
            return;
        }
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        const F32 mx = (F32)mouse.mX * scale.mV[VX];
        const F32 my = (F32)mouse.mY * scale.mV[VY];
        if (mx < 0.f || my < 0.f || mx >= (F32)sTexW || my >= (F32)sTexH)
        {
            return; // beyond the panel: drawn in the world (drawWorldPointer)
        }
        if (!sPtrOnUI)
        {
            return; // over the world seen through the panel: drawn at the depth of the hit (drawWorldPointer)
        }
        static LLCachedControl<F32> cursor_scale(gSavedSettings, "PATVRCursorScale", 1.f);
        const F32 s = llclamp((F32)cursor_scale, 0.25f, 4.f) * llmax(1.f, scale.mV[VX]);

        F32 clip[4][4];
        CursorImage* img = cursorImage(gViewerWindow->getWindow()->getCursor());
        if (img)
        {
            const F32 left = mx - img->hot_x * s;
            const F32 top = my + img->hot_y * s;
            pixelRect(left, top - img->h * s, left + img->w * s, top, sTexW, sTexH, clip);
            glBindTexture(GL_TEXTURE_2D, img->tex);
            glUniform1i(sAlphaFixLoc, 0);
            drawQuad(clip, FULL_UV);
        }
    }

    // ------------------------------------------------------------------ placement
    void logPanel(const char* what)
    {
        const F32 dist = sEyeValid ? dot(sub(sEye, sC), sN) : 0.f;
        LL_INFOS("PATXR") << what << llformat(" UI panel: centre x=%.2f y=%.2f z=%.2f m, %.2f x %.2f m, %.2f m from the eyes "
                                               "(%.0f deg wide), yaw %.0f deg, texture %dx%d, UI scale %.2f",
                                               sC.x, sC.y, sC.z, sW, sH, dist,
                                               dist > 0.01f ? 2.f * atanf(0.5f * sW / dist) * RAD_TO_DEG : 0.f,
                                               sPanelYaw * RAD_TO_DEG, sTexW, sTexH, PATXRUI::uiScale()) << LL_ENDL;
    }

    void updateHeight()
    {
        const S32 w = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 0;
        const S32 h = gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 0;
        sH = (w > 0 && h > 0) ? sW * (F32)h / (F32)w : sW * 0.5625f;
    }

    EMode mode()
    {
        static LLCachedControl<S32> ui_mode(gSavedSettings, "PATVRUIMode", MODE_ROOM);
        return (EMode)llclamp((S32)ui_mode, (S32)MODE_ROOM, (S32)MODE_FOLLOW);
    }

    F32 distance()
    {
        static LLCachedControl<F32> dist(gSavedSettings, "PATVRUIDistance", 1.2f);
        return llclamp((F32)dist, 0.3f, 5.f);
    }

    F32 dropBelowEyes()
    {
        static LLCachedControl<F32> drop(gSavedSettings, "PATVRUIDrop", 0.1f);
        return llclamp((F32)drop, -1.f, 1.f);
    }

    F32 headYaw(const XrPosef& eye)
    {
        XrVector3f fwd = rotate(eye.orientation, v3(0.f, 0.f, -1.f));
        fwd.y = 0.f;
        if (norm(fwd) < 0.1f)
        {
            return sPanelYaw;
        }
        return atan2f(-fwd.x, -fwd.z);
    }

    F32 angleDiff(F32 a, F32 b)
    {
        F32 d = fmodf(a - b + F_PI, 2.f * F_PI);
        if (d < 0.f)
        {
            d += 2.f * F_PI;
        }
        return d - F_PI;
    }

    // Settings the panel placement depends on, as used for the last placement.
    F32 sLayoutDist = 0.f, sLayoutDrop = 0.f, sLayoutWidth = 0.f;

    bool layoutChanged()
    {
        static LLCachedControl<F32> width(gSavedSettings, "PATVRUIWidth", 2.3f);
        const F32 w = llclamp((F32)width, MIN_WIDTH, MAX_WIDTH);
        return !sResized && (fabsf(distance() - sLayoutDist) > 0.001f || fabsf(dropBelowEyes() - sLayoutDrop) > 0.001f ||
                             fabsf(w - sLayoutWidth) > 0.001f);
    }

    // Panel in direction 'yaw' from 'eye': a bit below eye height, turned so
    // that it faces the eyes. Room mode: placed once (fixed in the real room,
    // never moved by the avatar or the SL camera); follow mode: every frame.
    void placeAt(const XrVector3f& eye, F32 yaw)
    {
        static LLCachedControl<F32> width(gSavedSettings, "PATVRUIWidth", 2.3f);
        if (!sResized)
        {
            sW = llclamp((F32)width, MIN_WIDTH, MAX_WIDTH);
        }
        updateHeight();
        sLayoutDist = distance();
        sLayoutDrop = dropBelowEyes();
        sLayoutWidth = sW;
        sPanelYaw = yaw;
        const XrVector3f fwd = v3(-sinf(yaw), 0.f, -cosf(yaw));
        sC = add(add(eye, mul(fwd, distance())), v3(0.f, -dropBelowEyes(), 0.f));
        sN = sub(eye, sC);
        norm(sN);
        sR = cross(v3(0.f, 1.f, 0.f), sN); // horizontal right
        norm(sR);
        sU = cross(sN, sR);
        norm(sU);
        sPlaced = true;
    }

    // Head-locked mode: fixed in front of the head, turns and tilts with it.
    void placeOnHead(const XrPosef& left_eye)
    {
        static LLCachedControl<F32> width(gSavedSettings, "PATVRUIWidth", 2.3f);
        if (!sResized)
        {
            sW = llclamp((F32)width, MIN_WIDTH, MAX_WIDTH);
        }
        updateHeight();
        sLayoutDist = distance();
        sLayoutDrop = dropBelowEyes();
        sLayoutWidth = sW;
        const XrQuaternionf& q = left_eye.orientation;
        sR = rotate(q, v3(1.f, 0.f, 0.f));
        sU = rotate(q, v3(0.f, 1.f, 0.f));
        sN = rotate(q, v3(0.f, 0.f, 1.f));
        sC = add(add(sEye, mul(sN, -distance())), mul(sU, -dropBelowEyes()));
        sPlaced = true;
    }

    void place(const XrPosef& left_eye)
    {
        placeAt(sEye, headYaw(left_eye));
        sRecentre = false;
        sFollowing = false;
        // The world's "straight ahead" (viewer camera direction) is taken from
        // the head at the same moment, so the panel centre and the camera
        // direction (e.g. an Alt-click focus) agree.
        PATXRStereo::resetReference();
        logPanel("Placed");
    }

    void setHidden(bool hidden)
    {
        if (hidden == sHidden || !gViewerWindow)
        {
            return;
        }
        sHidden = hidden;
        if (hidden)
        {
            sSavedUIVisible = gViewerWindow->getUIVisibility();
            sSavedHUDs = LLPipeline::sShowHUDAttachments;
            gViewerWindow->setUIVisibility(false);
            LLPipeline::sShowHUDAttachments = false;
        }
        else
        {
            gViewerWindow->setUIVisibility(sSavedUIVisible);
            LLPipeline::sShowHUDAttachments = sSavedHUDs;
        }
        LL_INFOS("PATXR") << "UI " << (hidden ? "hidden" : "shown again") << " (Ctrl+Shift+Space)" << LL_ENDL;
    }

    void handleKeys(F32 dt)
    {
        if (!gKeyboard)
        {
            return;
        }
        const MASK mask = gKeyboard->currentMask(false);
        const bool ctrl_shift = (mask & (MASK_CONTROL | MASK_SHIFT)) == (MASK_CONTROL | MASK_SHIFT);
        const bool alt = (mask & MASK_ALT) != 0;

        // Ctrl+Shift+Space: short = hide/show the UI, held = panel in front of the view.
        const bool space = ctrl_shift && !alt && gKeyboard->getKeyDown(' ');
        if (space)
        {
            sSpaceHeldTime = sSpaceWasHeld ? sSpaceHeldTime + dt : 0.f;
            if (!sSpaceRecentred && sSpaceHeldTime >= RECENTRE_HOLD)
            {
                LL_INFOS("PATXR") << "Ctrl+Shift+Space held: UI panel in front of the view" << LL_ENDL;
                sRecentre = true;
                sSpaceRecentred = true;
                setHidden(false);
            }
        }
        else if (sSpaceWasHeld)
        {
            if (!sSpaceRecentred)
            {
                setHidden(!sHidden);
            }
            sSpaceRecentred = false;
        }
        sSpaceWasHeld = space;

        // Ctrl+Shift+Alt+Home / End: larger / smaller (same distance).
        if (ctrl_shift && alt && sPlaced)
        {
            F32 factor = 1.f;
            if (gKeyboard->getKeyDown(KEY_HOME))
            {
                factor += RESIZE_SPEED * llclamp(dt, 0.f, 0.1f);
            }
            if (gKeyboard->getKeyDown(KEY_END))
            {
                factor -= RESIZE_SPEED * llclamp(dt, 0.f, 0.1f);
            }
            if (factor != 1.f)
            {
                sW = llclamp(sW * factor, MIN_WIDTH, MAX_WIDTH);
                updateHeight();
                sResized = true;
            }
        }
        if (sResized && !(ctrl_shift && alt))
        {
            sResized = false;
            gSavedSettings.setF32("PATVRUIWidth", sW);
            sLayoutWidth = sW;
            logPanel("Resized");
        }
    }
}

void PATXRUI::init()
{
    release();
    initGL();
}

void PATXRUI::release()
{
    setHidden(false);
    if (sCapturing)
    {
        sCaptureRT.flush();
        sCapturing = false;
    }
    sCaptureRT.release();
    for (CursorImage& img : sCursors)
    {
        if (img.tex)
        {
            glDeleteTextures(1, &img.tex);
        }
        img = CursorImage();
    }
    if (sPanelTex)
    {
        glDeleteTextures(1, &sPanelTex);
        sPanelTex = 0;
    }
    if (sTipTex)
    {
        glDeleteTextures(1, &sTipTex);
        sTipTex = 0;
    }
    sTipTexW = sTipTexH = 0;
    sTipValid = false;
    if (sFbo)
    {
        glDeleteFramebuffers(1, &sFbo);
        sFbo = 0;
    }
    if (sVBO)
    {
        glDeleteBuffers(1, &sVBO);
    }
    if (sVAO)
    {
        glDeleteVertexArrays(1, &sVAO);
    }
    if (sProgram)
    {
        glDeleteProgram(sProgram);
    }
    if (sDepthSampler)
    {
        glDeleteSamplers(1, &sDepthSampler);
    }
    sVBO = sVAO = sProgram = sDepthSampler = 0;
    sGLFailed = false;
    sTexW = sTexH = 0;
    sMipsValid = false;
    sActive = false;
    sHasImage = false;
    sNewCapture = false;
    sPlaced = false;
    sRecentre = false;
    sEyeValid = false;
}

void PATXRUI::setActive(bool active)
{
    if (active == sActive)
    {
        return;
    }
    LL_INFOS("PATXR") << "UI " << (active ? "on the VR panel" : "back on the monitor only") << LL_ENDL;
    sActive = active;
    if (!active)
    {
        setHidden(false);
        sHasImage = false;
        sNewCapture = false;
    }
}

bool PATXRUI::isActive()
{
    return sActive;
}

bool PATXRUI::isHidden()
{
    return sHidden;
}

void PATXRUI::beginCapture()
{
    if (!sActive || sCapturing || !gViewerWindow || !initGL())
    {
        return;
    }
    const S32 w = gViewerWindow->getWindowWidthRaw();
    const S32 h = gViewerWindow->getWindowHeightRaw();
    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (!sCaptureRT.isComplete() || (S32)sCaptureRT.getWidth() != w || (S32)sCaptureRT.getHeight() != h)
    {
        sCaptureRT.release();
        if (!sCaptureRT.allocate(w, h, GL_RGBA, true))
        {
            LL_WARNS("PATXR") << "Could not allocate the UI render target (" << w << "x" << h << ")" << LL_ENDL;
            return;
        }
        LL_INFOS("PATXR") << "UI render target " << w << "x" << h << LL_ENDL;
    }
    gGL.flush();
    GLint viewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, sSavedMask);

    sCaptureRT.bindTarget();
    gGL.setColorMask(true, true); // the UI must write alpha into the target
    GLfloat clear[4] = { 0.f, 0.f, 0.f, 0.f };
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    sCaptureRT.clear();
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    if (scissor)
    {
        glEnable(GL_SCISSOR_TEST);
    }
    // Same size as the window: the viewport of the world view stays valid (HUDs).
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    sCapturing = true;
}

bool PATXRUI::capturing()
{
    return sCapturing;
}

void PATXRUI::endCapture()
{
    if (!sCapturing)
    {
        return;
    }
    gGL.flush();
    sCaptureRT.flush();
    gGL.setColorMask(sSavedMask[0], sSavedMask[3]);
    sCapturing = false;
    sNewCapture = true;
    if (!sHasImage)
    {
        sHasImage = true;
        LL_INFOS("PATXR") << "First UI image captured" << LL_ENDL;
    }

    // The monitor shows the UI as before: target over the mirrored eye image.
    SavedGL saved;
    glViewport(0, 0, sCaptureRT.getWidth(), sCaptureRT.getHeight());
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, sCaptureRT.getTexture());
    bindCaptureDepth();
    glUniform1i(sAlphaFixLoc, 1);
    F32 clip[4][4];
    pixelRect(0.f, 0.f, (F32)sCaptureRT.getWidth(), (F32)sCaptureRT.getHeight(),
              sCaptureRT.getWidth(), sCaptureRT.getHeight(), clip);
    drawQuad(clip, FULL_UV);
}

void PATXRUI::update(F32 dt, const XrPosef& left_eye, const XrPosef& right_eye)
{
    sEye = mul(add(left_eye.position, right_eye.position), 0.5f);
    sEyeValid = true;
    updatePointerDepth(dt);
    const EMode m = mode();
    if (m == MODE_HEAD)
    {
        placeOnHead(left_eye);
        if (sRecentre)
        {
            sRecentre = false;
            logPanel("Head-locked");
        }
    }
    else if (!sPlaced || sRecentre)
    {
        place(left_eye);
    }
    else if (m == MODE_ROOM && layoutChanged())
    {
        // Distance / height / width changed on the preferences page: move the
        // panel along its direction, without touching the world's "straight ahead".
        placeAt(sEye, sPanelYaw);
        logPanel("Re-placed (settings)");
    }
    else if (m == MODE_FOLLOW)
    {
        // Follows the view with a delay: starts when the view has left the
        // panel far enough, then glides until it is in front again.
        const F32 diff = angleDiff(headYaw(left_eye), sPanelYaw);
        if (!sFollowing && fabsf(diff) > FOLLOW_START)
        {
            sFollowing = true;
        }
        F32 yaw = sPanelYaw;
        if (sFollowing)
        {
            yaw += diff * (1.f - expf(-llclamp(dt, 0.f, 0.1f) / FOLLOW_TIME));
            if (fabsf(diff) < FOLLOW_STOP)
            {
                sFollowing = false;
            }
        }
        placeAt(sEye, yaw);
    }
    handleKeys(dt);
    // Leaving mouselook shows toolbars, bars and chat again (Firestorm); with the UI
    // hidden (Ctrl+Shift+Space) it has to stay completely hidden.
    static bool was_mouselook = false;
    const bool mouselook = gAgentCamera.cameraMouselook();
    if (sHidden && was_mouselook && !mouselook && gViewerWindow)
    {
        gViewerWindow->setUIVisibility(false);
    }
    was_mouselook = mouselook;
    updateHeight();
}

void PATXRUI::recentre()
{
    sRecentre = true;
}

bool PATXRUI::hasImage()
{
    return sActive && sHasImage && sPlaced && sProgram;
}

void PATXRUI::prepare()
{
    if (!hasImage())
    {
        return;
    }
    const S32 w = sCaptureRT.getWidth(), h = sCaptureRT.getHeight();
    if (!sPanelTex || w != sTexW || h != sTexH)
    {
        if (sPanelTex)
        {
            glDeleteTextures(1, &sPanelTex);
        }
        sTexW = w;
        sTexH = h;
        sTexLevels = 1;
        for (S32 s = llmax(w, h); s > 1; s >>= 1)
        {
            ++sTexLevels;
        }
        GLint old_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
        glGenTextures(1, &sPanelTex);
        glBindTexture(GL_TEXTURE_2D, sPanelTex);
        glTexStorage2D(GL_TEXTURE_2D, sTexLevels, GL_RGBA8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, old_tex);
        updateHeight();
        logPanel("Texture");
    }

    SavedGL saved;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sPanelTex, 0);
    glViewport(0, 0, sTexW, sTexH);
    F32 clip[4][4];
    pixelRect(0.f, 0.f, (F32)sTexW, (F32)sTexH, sTexW, sTexH, clip);

    // UI with premultiplied alpha, then the pointer on top.
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, sCaptureRT.getTexture());
    bindCaptureDepth();
    glUniform1i(sAlphaFixLoc, 1);
    drawQuad(clip, FULL_UV);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    grabToolTip();
    drawPointer();

    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    sMipsValid = false;
    if (!useQuadLayer())
    {
        // drawn into the eye images: minified, trilinear filtering keeps text calm
        glBindTexture(GL_TEXTURE_2D, sPanelTex);
        glGenerateMipmap(GL_TEXTURE_2D);
        sMipsValid = true;
    }
    sNewCapture = false;
}

bool PATXRUI::useQuadLayer()
{
    static LLCachedControl<bool> quad(gSavedSettings, "PATVRUIQuadLayer", true);
    return quad;
}

S32 PATXRUI::imageWidth()
{
    return sTexW;
}

S32 PATXRUI::imageHeight()
{
    return sTexH;
}

void PATXRUI::copyToSwapchain(GLuint texture, S32 width, S32 height)
{
    if (!sPanelTex)
    {
        return;
    }
    // Plain copy (8 bit, already gamma encoded; GL_FRAMEBUFFER_SRGB off), as the eyes.
    SavedGL saved;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sPanelTex, 0);
    GLuint draw_fbo = 0;
    glGenFramebuffers(1, &draw_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBlitFramebuffer(0, 0, sTexW, sTexH, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &draw_fbo);
}

void PATXRUI::fillQuadLayer(XrCompositionLayerQuad& layer, XrSpace space, XrSwapchain swapchain)
{
    layer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied
    layer.space = space;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = swapchain;
    layer.subImage.imageRect.offset = { 0, 0 };
    layer.subImage.imageRect.extent = { sTexW, sTexH };
    layer.subImage.imageArrayIndex = 0;
    layer.pose.orientation = quatFromAxes(sR, sU, sN); // quad faces +Z = towards the user
    layer.pose.position = sC;
    layer.size = { sW, sH };
}

void PATXRUI::drawInEye(const XrView& view, GLuint texture, S32 width, S32 height)
{
    if (!sPanelTex || !sMipsValid)
    {
        return;
    }
    SavedGL saved;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glViewport(0, 0, width, height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, sPanelTex);
    glUniform1i(sAlphaFixLoc, 0);
    const XrVector3f hr = mul(sR, 0.5f * sW), hu = mul(sU, 0.5f * sH);
    const XrVector3f corners[4] = { sub(sub(sC, hr), hu), sub(add(sC, hr), hu), add(sub(sC, hr), hu), add(add(sC, hr), hu) };
    F32 clip[4][4];
    for (U32 i = 0; i < 4; ++i)
    {
        toClip(view, corners[i], clip[i]);
    }
    drawQuad(clip, FULL_UV);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

bool PATXRUI::rayRoom(F32 u, F32 v, XrVector3f& origin, XrVector3f& dir)
{
    if (!sActive || !sPlaced || !sEyeValid)
    {
        return false;
    }
    const XrVector3f p = add(add(sC, mul(sR, (u - 0.5f) * sW)), mul(sU, (v - 0.5f) * sH));
    origin = sEye;
    dir = sub(p, sEye);
    return norm(dir) > 0.01f;
}

bool PATXRUI::pointerCamera(XrVector3f& origin, XrVector3f& at, XrVector3f& up, F32& vfov, F32& aspect, F32& ndc_x, F32& ndc_y)
{
    if (!sActive || !sPlaced || !sEyeValid)
    {
        return false;
    }
    const F32 t = dot(sub(sEye, sC), sN);
    if (t < 0.05f)
    {
        return false; // head at or behind the panel
    }
    const XrVector3f foot = sub(sEye, mul(sN, t)); // eye projected onto the panel plane
    origin = sEye;
    at = mul(sN, -1.f);
    up = sU;
    vfov = 2.f * atanf(0.5f * sH / t);
    aspect = sW / llmax(0.01f, sH);
    ndc_x = 2.f * dot(sub(foot, sC), sR) / sW;
    ndc_y = 2.f * dot(sub(foot, sC), sU) / llmax(0.01f, sH);
    return true;
}

bool PATXRUI::drawWorldPointerWith(const std::function<bool(const XrVector3f&, F32*)>& to_clip, GLuint texture, S32 width,
                                   S32 height, S32 eye)
{
    if (!hasImage() || !gViewerWindow || !gViewerWindow->getWindow() || pointerHidden() || sTexW <= 0)
    {
        return false;
    }
    XrVector3f p;
    bool inside = false;
    F32 mx = 0.f, my = 0.f;
    if (!pointerPanelPoint(p, inside, &mx, &my) || (inside && sPtrOnUI))
    {
        return false; // over the UI: drawn into the panel texture
    }
    CursorImage* img = cursorImage(gViewerWindow->getWindow()->getCursor());
    if (!img)
    {
        return false;
    }
    // Along the ray from the eye centre through the panel point (the click ray),
    // at the depth of what it points at; parallel to the panel, same angular size.
    const LLVector2& scale = gViewerWindow->getDisplayScale();
    const F32 t = llmax(0.05f, dot(sub(sEye, sC), sN));
    XrVector3f dir = sub(p, sEye);
    const F32 panel_dist = norm(dir);
    const F32 depth = sPtrDepth > 0.f ? sPtrDepth : panel_dist;
    const XrVector3f q = add(sEye, mul(dir, depth));
    const F32 k = depth / t;
    static LLCachedControl<F32> cursor_scale(gSavedSettings, "PATVRCursorScale", 1.f);
    const F32 m = (sW / (F32)sTexW) * k * llclamp((F32)cursor_scale, 0.25f, 4.f) * llmax(1.f, scale.mV[VX]);
    // image pixel (x right, y down) relative to the hot spot -> plane parallel to the panel
    auto corner = [&](F32 ix, F32 iy) { return add(add(q, mul(sR, (ix - img->hot_x) * m)), mul(sU, -(iy - img->hot_y) * m)); };
    const XrVector3f corners[4] = { corner(0.f, (F32)img->h), corner((F32)img->w, (F32)img->h), corner(0.f, 0.f),
                                    corner((F32)img->w, 0.f) };
    SavedGL saved;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glViewport(0, 0, width, height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1i(sAlphaFixLoc, 0);
    F32 clip[4][4];
    F32 cursor_clip[4][4];
    for (U32 i = 0; i < 4; ++i)
    {
        if (!to_clip(corners[i], cursor_clip[i]))
        {
            return false;
        }
    }
    if (sTipValid && sTipTex)
    {
        // tooltip: same place relative to the pointer as on the panel, at the pointer depth
        const F32 mp = (sW / (F32)sTexW) * k;
        auto tip = [&](F32 px, F32 py) { return add(add(q, mul(sR, (px - mx) * mp)), mul(sU, (py - my) * mp)); };
        const F32 l = (F32)sTipX, b = (F32)sTipY, r = (F32)(sTipX + sTipW), t2 = (F32)(sTipY + sTipH);
        const XrVector3f tc[4] = { tip(l, b), tip(r, b), tip(l, t2), tip(r, t2) };
        bool ok = true;
        for (U32 i = 0; i < 4 && ok; ++i)
        {
            ok = to_clip(tc[i], clip[i]);
        }
        if (ok)
        {
            glBindTexture(GL_TEXTURE_2D, sTipTex);
            drawQuad(clip, FULL_UV);
        }
    }
    glBindTexture(GL_TEXTURE_2D, img->tex);
    // Diagnosis (PATVRPointerStereoColors): left eye red, right eye green.
    static LLCachedControl<bool> stereo_colors(gSavedSettings, "PATVRPointerStereoColors", false);
    glUniform1i(sTintLoc, (stereo_colors && eye >= 0) ? (eye == 0 ? 1 : 2) : 0);
    drawQuad(cursor_clip, FULL_UV);
    glUniform1i(sTintLoc, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    return true;
}

void PATXRUI::drawWorldPointer(const XrView& view, GLuint texture, S32 width, S32 height, S32 eye)
{
    drawWorldPointerWith([&](const XrVector3f& p, F32* out) { toClip(view, p, out); return true; }, texture, width, height,
                         eye);
}

void PATXRUI::setPointerTarget(bool on_ui, F32 dist)
{
    sPtrOnUI = on_ui;
    sPtrTarget = dist;
}

void PATXRUI::logPointerPath(const XrView views[2])
{
    static F64 s_last = 0.0;
    const F64 now = LLTimer::getTotalSeconds();
    if (now - s_last < 0.5 || !hasImage() || !gViewerWindow || !gViewerWindow->getWindow())
    {
        return;
    }
    s_last = now;
    if (pointerHidden())
    {
        LL_INFOS("PATXRPointer") << "pointer drawn: hidden" << LL_ENDL;
        return;
    }
    XrVector3f p;
    bool inside = false;
    if (!pointerPanelPoint(p, inside))
    {
        return;
    }
    XrVector3f dir = sub(p, sEye);
    const F32 panel_dist = norm(dir);
    const bool panel = inside && sPtrOnUI;
    const F32 depth = panel ? panel_dist : (sPtrDepth > 0.f ? sPtrDepth : panel_dist);
    const XrVector3f q = add(sEye, mul(dir, depth));
    F32 ang[2];
    for (U32 i = 0; i < 2; ++i)
    {
        const XrVector3f e = rotate(views[i].pose.orientation, sub(q, views[i].pose.position), true);
        ang[i] = atan2f(e.x, -e.z) * RAD_TO_DEG;
    }
    LL_INFOS("PATXRPointer") << llformat("pointer drawn (%s): %s @ %.2f m (target %.2f m, panel %.2f m, inside %d, on_ui %d), "
                                         "angle L %.3f R %.3f deg, disparity %.3f deg",
                                         PATXRStereo::pointerDrawnInEyes() ? "world pass" : "XR overlay",
                                         panel ? "panel" : "world", depth, sPtrTarget, panel_dist, inside ? 1 : 0,
                                         sPtrOnUI ? 1 : 0, ang[0], ang[1], ang[0] - ang[1]) << LL_ENDL;
}

bool PATXRUI::pointerVisible()
{
    return gViewerWindow && gViewerWindow->getWindow() && !pointerHidden();
}

F32 PATXRUI::uiScale()
{
    static LLCachedControl<F32> scale(gSavedSettings, "PATVRUIScale", 1.75f);
    return sActive ? llclamp((F32)scale, 0.75f, 4.f) : 1.f;
}

#endif // PAT_XR
