/**
 * @file patxrkeyboard.cpp
 * @brief <PAT:XR> Passthrough window for the real keyboard (and hands) in VR.
 *
 * Phase 6b feasibility test: a horizontal rectangle in the LOCAL space (desk
 * height, in front of the user) is drawn with alpha 0; WiVRn shows the headset
 * camera image there. Position and size can be changed with shortcuts.
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

#include "patxrkeyboard.h"

#include "llgl.h"
#include "llkeyboard.h"

#include <cmath>

namespace
{
    // Default window relative to the head pose at VR start (metres, head frame).
    constexpr F32 DEFAULT_BELOW = 0.50f;   // below the eyes
    constexpr F32 DEFAULT_AHEAD = 0.40f;   // in front
    constexpr F32 DEFAULT_W = 0.50f;       // width
    constexpr F32 DEFAULT_D = 0.20f;       // depth
    constexpr F32 FRAME = 0.006f;          // white border, 6 mm
    constexpr F32 HAND_SIZE = 0.20f;       // passthrough square around a palm
    constexpr F32 MOVE_SPEED = 0.10f;      // m/s while a key is held
    constexpr F32 NEAR_Z = 0.05f;
    constexpr F32 FAR_Z = 100.f;
    // Hand calibration
    constexpr F32 HANDS_STILL_SECONDS = 1.0f;
    constexpr F32 HANDS_STILL_RADIUS = 0.02f;  // max movement while "resting"
    constexpr F32 HANDS_MAX_DY = 0.07f;        // similar height
    constexpr F32 HANDS_MIN_DIST = 0.10f;
    constexpr F32 HANDS_MAX_DIST = 0.70f;
    constexpr F32 PALM_ABOVE_KEYS = 0.03f;     // palm centre is a bit above the key tops
    constexpr F32 HAND_MARGIN = 0.20f;         // added to the hand distance for the width

    struct Vec3 { F32 x, y, z; };

    // Window state (LOCAL space). yaw rotates the window's x axis around +Y.
    bool sPlaced = false;        // default placement done (head known)
    bool sHandPlaced = false;    // placed on the resting hands
    Vec3 sC = { 0.f, 0.f, 0.f };
    F32 sYaw = 0.f;
    F32 sW = DEFAULT_W, sD = DEFAULT_D;
    bool sChanged = false;
    bool sWasHeld = false;
    bool sBackspaceHeld = false;
    Vec3 sStillRef[2];
    F32 sStillTime = 0.f;

    GLuint sProgram = 0;
    GLuint sVAO = 0;
    GLuint sVBO = 0;
    GLint sColorLoc = -1;
    bool sGLFailed = false;

    const char* VERTEX_SRC =
        "#version 330 core\n"
        "layout(location = 0) in vec4 clip_pos;\n"
        "void main() { gl_Position = clip_pos; }\n";
    const char* FRAGMENT_SRC =
        "#version 330 core\n"
        "uniform vec4 color;\n"
        "out vec4 frag_color;\n"
        "void main() { frag_color = color; }\n";

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
            LL_WARNS("PATXR") << "Keyboard window shader: " << log << LL_ENDL;
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
            LL_WARNS("PATXR") << "Keyboard window shader does not link" << LL_ENDL;
            glDeleteProgram(sProgram);
            sProgram = 0;
            sGLFailed = true;
            return false;
        }
        sColorLoc = glGetUniformLocation(sProgram, "color");

        GLint old_vao = 0, old_vbo = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
        glGenVertexArrays(1, &sVAO);
        glGenBuffers(1, &sVBO);
        glBindVertexArray(sVAO);
        glBindBuffer(GL_ARRAY_BUFFER, sVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(F32) * 16, nullptr, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(old_vao);
        glBindBuffer(GL_ARRAY_BUFFER, old_vbo);
        return true;
    }

    Vec3 rotate(const XrQuaternionf& q, const Vec3& v, bool inverse)
    {
        const F32 sgn = inverse ? -1.f : 1.f;
        const F32 qx = sgn * q.x, qy = sgn * q.y, qz = sgn * q.z, qw = q.w;
        // t = 2 * cross(q.xyz, v); v' = v + qw * t + cross(q.xyz, t)
        const F32 tx = 2.f * (qy * v.z - qz * v.y);
        const F32 ty = 2.f * (qz * v.x - qx * v.z);
        const F32 tz = 2.f * (qx * v.y - qy * v.x);
        return { v.x + qw * tx + (qy * tz - qz * ty),
                 v.y + qw * ty + (qz * tx - qx * tz),
                 v.z + qw * tz + (qx * ty - qy * tx) };
    }

    // Window frame axes from yaw: x = right along the keyboard, z = towards the user.
    Vec3 axisX(F32 yaw) { return { cosf(yaw), 0.f, -sinf(yaw) }; }
    Vec3 axisZ(F32 yaw) { return { sinf(yaw), 0.f, cosf(yaw) }; }

    // LOCAL-space point -> clip space of this eye (OpenXR asymmetric frustum).
    void toClip(const XrView& view, const Vec3& p, F32* out)
    {
        const Vec3 rel = { p.x - view.pose.position.x, p.y - view.pose.position.y, p.z - view.pose.position.z };
        const Vec3 e = rotate(view.pose.orientation, rel, true);
        const F32 l = tanf(view.fov.angleLeft), r = tanf(view.fov.angleRight);
        const F32 u = tanf(view.fov.angleUp), d = tanf(view.fov.angleDown);
        out[0] = (2.f / (r - l)) * e.x + ((r + l) / (r - l)) * e.z;
        out[1] = (2.f / (u - d)) * e.y + ((u + d) / (u - d)) * e.z;
        out[2] = -((FAR_Z + NEAR_Z) / (FAR_Z - NEAR_Z)) * e.z - (2.f * FAR_Z * NEAR_Z / (FAR_Z - NEAR_Z));
        out[3] = -e.z;
    }

    // Horizontal rectangle centred at c, width w along axisX(yaw), depth d along axisZ(yaw).
    void drawRect(const XrView& view, const Vec3& c, F32 yaw, F32 w, F32 d, F32 r, F32 g, F32 b, F32 a)
    {
        const Vec3 ax = axisX(yaw), az = axisZ(yaw);
        const F32 sx[4] = { -0.5f, 0.5f, -0.5f, 0.5f };
        const F32 sz[4] = { 0.5f, 0.5f, -0.5f, -0.5f };
        F32 verts[16];
        for (U32 i = 0; i < 4; ++i)
        {
            const Vec3 p = { c.x + ax.x * sx[i] * w + az.x * sz[i] * d,
                             c.y,
                             c.z + ax.z * sx[i] * w + az.z * sz[i] * d };
            toClip(view, p, verts + i * 4);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glUniform4f(sColorLoc, r, g, b, a);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void logWindow(const char* what)
    {
        LL_INFOS("PATXR") << what << llformat(" keyboard window: centre x=%.2f y=%.2f z=%.2f m, yaw %.0f deg, size %.0fx%.0f cm",
                                               sC.x, sC.y, sC.z, sYaw * RAD_TO_DEG, sW * 100.f, sD * 100.f) << LL_ENDL;
    }

    // Default: below and in front of the head, turned to the head's yaw.
    void placeFromHead(const XrPosef& left_eye, const XrPosef& right_eye)
    {
        const Vec3 head = { (left_eye.position.x + right_eye.position.x) * 0.5f,
                            (left_eye.position.y + right_eye.position.y) * 0.5f,
                            (left_eye.position.z + right_eye.position.z) * 0.5f };
        const Vec3 fwd = rotate(left_eye.orientation, { 0.f, 0.f, -1.f }, false);
        // yaw so that axisZ(yaw) points from the window towards the user (= -forward, horizontal)
        sYaw = atan2f(-fwd.x, -fwd.z);
        const Vec3 az = axisZ(sYaw);
        sC = { head.x - az.x * DEFAULT_AHEAD, head.y - DEFAULT_BELOW, head.z - az.z * DEFAULT_AHEAD };
        sW = DEFAULT_W;
        sD = DEFAULT_D;
        sPlaced = true;
        sHandPlaced = false;
        sStillTime = 0.f;
        LL_INFOS("PATXR") << llformat("Head at VR start: x=%.2f y=%.2f z=%.2f m", head.x, head.y, head.z) << LL_ENDL;
        logWindow("Default");
    }

    F32 dist(const Vec3& a, const Vec3& b)
    {
        return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    // Both hands resting ~1 s at a similar height -> window onto the hands.
    void placeFromHands(F32 dt, const PATXRPalm palms[2])
    {
        if (!palms[0].valid || !palms[1].valid)
        {
            sStillTime = 0.f;
            return;
        }
        const Vec3 l = { palms[0].pos.x, palms[0].pos.y, palms[0].pos.z };
        const Vec3 r = { palms[1].pos.x, palms[1].pos.y, palms[1].pos.z };
        const F32 d = dist(l, r);
        if (fabsf(l.y - r.y) > HANDS_MAX_DY || d < HANDS_MIN_DIST || d > HANDS_MAX_DIST)
        {
            sStillTime = 0.f;
            return;
        }
        if (sStillTime == 0.f || dist(l, sStillRef[0]) > HANDS_STILL_RADIUS || dist(r, sStillRef[1]) > HANDS_STILL_RADIUS)
        {
            sStillRef[0] = l;
            sStillRef[1] = r;
            sStillTime = 0.0001f;
            return;
        }
        sStillTime += dt;
        if (sStillTime < HANDS_STILL_SECONDS)
        {
            return;
        }
        // Keyboard direction = left -> right hand, horizontal.
        sYaw = atan2f(-(r.z - l.z), r.x - l.x);
        sC = { (l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f - PALM_ABOVE_KEYS, (l.z + r.z) * 0.5f };
        const F32 horiz = sqrtf((r.x - l.x) * (r.x - l.x) + (r.z - l.z) * (r.z - l.z));
        sW = llclamp(horiz + HAND_MARGIN, 0.35f, 0.90f);
        sD = DEFAULT_D;
        sHandPlaced = true;
        logWindow("Placed on the hands:");
    }
}

void PATXRKeyboard::update(F32 dt, const XrPosef& left_eye, const XrPosef& right_eye, const PATXRPalm palms[2])
{
    if (!sPlaced)
    {
        placeFromHead(left_eye, right_eye);
    }
    if (!sHandPlaced)
    {
        placeFromHands(dt, palms);
    }

    if (!gKeyboard)
    {
        return;
    }
    const MASK mask = gKeyboard->currentMask(false);
    if ((mask & (MASK_CONTROL | MASK_SHIFT)) != (MASK_CONTROL | MASK_SHIFT))
    {
        if (sWasHeld && sChanged)
        {
            logWindow("Moved");
            sChanged = false;
        }
        sWasHeld = false;
        sBackspaceHeld = false;
        return;
    }
    sWasHeld = true;
    auto held = [](KEY k) { return gKeyboard->getKeyDown(k); };

    // Measure again: default below the head, then wait for resting hands.
    if (held(KEY_BACKSPACE))
    {
        if (!sBackspaceHeld)
        {
            LL_INFOS("PATXR") << "Keyboard window: measuring again" << LL_ENDL;
            placeFromHead(left_eye, right_eye);
        }
        sBackspaceHeld = true;
        return;
    }
    sBackspaceHeld = false;

    // Fine adjustment in the window's own frame.
    const F32 step = MOVE_SPEED * llclamp(dt, 0.f, 0.1f);
    const Vec3 ax = axisX(sYaw), az = axisZ(sYaw);
    F32 mx = 0.f, mz = 0.f;
    if (held(KEY_LEFT))      mx -= step;
    if (held(KEY_RIGHT))     mx += step;
    if (held(KEY_UP))        mz -= step; // away from the user
    if (held(KEY_DOWN))      mz += step;
    if (held(KEY_PAGE_UP))   { sC.y += step; sChanged = true; }
    if (held(KEY_PAGE_DOWN)) { sC.y -= step; sChanged = true; }
    if (held(KEY_HOME))      { sW += step; sChanged = true; }
    if (held(KEY_END))       { sW = llmax(0.05f, sW - step); sChanged = true; }
    if (held(KEY_INSERT))    { sD += step; sChanged = true; }
    if (held(KEY_DELETE))    { sD = llmax(0.03f, sD - step); sChanged = true; }
    if (mx != 0.f || mz != 0.f)
    {
        sC.x += ax.x * mx + az.x * mz;
        sC.z += ax.z * mx + az.z * mz;
        sChanged = true;
    }
}

void PATXRKeyboard::draw(const XrView& view, const PATXRPalm palms[2])
{
    if (!sPlaced || !initGL())
    {
        return;
    }
    GLint old_program = 0, old_vao = 0, old_vbo = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    const GLboolean stencil = glIsEnabled(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);

    glUseProgram(sProgram);
    glBindVertexArray(sVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sVBO);

    // White frame, then the transparent window inside it.
    drawRect(view, sC, sYaw, sW + 2 * FRAME, sD + 2 * FRAME, 1.f, 1.f, 1.f, 1.f);
    drawRect(view, sC, sYaw, sW, sD, 0.f, 0.f, 0.f, 0.f);
    // Hands near desk height get their own window (unchanged from the first test).
    for (U32 i = 0; i < 2; ++i)
    {
        if (palms[i].valid && palms[i].pos.y < sC.y + 0.25f)
        {
            drawRect(view, { palms[i].pos.x, palms[i].pos.y, palms[i].pos.z }, sYaw, HAND_SIZE, HAND_SIZE, 0.f, 0.f, 0.f, 0.f);
        }
    }

    glBindVertexArray(old_vao);
    glBindBuffer(GL_ARRAY_BUFFER, old_vbo);
    glUseProgram(old_program);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (stencil) glEnable(GL_STENCIL_TEST);
}

void PATXRKeyboard::release()
{
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
    sVBO = sVAO = sProgram = 0;
    sGLFailed = false;
    sPlaced = false;
    sHandPlaced = false;
    sStillTime = 0.f;
}

#endif // PAT_XR
