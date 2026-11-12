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
    // Defaults: LOCAL origin = eyes when the session started (seated).
    constexpr F32 DEFAULT_X = 0.f;
    constexpr F32 DEFAULT_Y = -0.50f;  // 50 cm below the eyes
    constexpr F32 DEFAULT_Z = -0.40f;  // 40 cm in front
    constexpr F32 DEFAULT_W = 0.45f;   // width
    constexpr F32 DEFAULT_D = 0.15f;   // depth
    constexpr F32 FRAME = 0.006f;      // white border, 6 mm
    constexpr F32 HAND_SIZE = 0.20f;   // passthrough square around a palm
    constexpr F32 MOVE_SPEED = 0.10f;  // m/s while a key is held
    constexpr F32 NEAR_Z = 0.05f;
    constexpr F32 FAR_Z = 100.f;

    F32 sX = DEFAULT_X, sY = DEFAULT_Y, sZ = DEFAULT_Z, sW = DEFAULT_W, sD = DEFAULT_D;
    bool sChanged = false;
    bool sWasHeld = false;

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

    struct Vec3 { F32 x, y, z; };

    // v rotated by the inverse of q (world -> eye).
    Vec3 rotateInverse(const XrQuaternionf& q, const Vec3& v)
    {
        const F32 qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w;
        // t = 2 * cross(q.xyz, v); v' = v + qw * t + cross(q.xyz, t)
        const F32 tx = 2.f * (qy * v.z - qz * v.y);
        const F32 ty = 2.f * (qz * v.x - qx * v.z);
        const F32 tz = 2.f * (qx * v.y - qy * v.x);
        return { v.x + qw * tx + (qy * tz - qz * ty),
                 v.y + qw * ty + (qz * tx - qx * tz),
                 v.z + qw * tz + (qx * ty - qy * tx) };
    }

    // LOCAL-space point -> clip space of this eye (OpenXR asymmetric frustum).
    void toClip(const XrView& view, const Vec3& p, F32* out)
    {
        const Vec3 rel = { p.x - view.pose.position.x, p.y - view.pose.position.y, p.z - view.pose.position.z };
        const Vec3 e = rotateInverse(view.pose.orientation, rel);
        const F32 l = tanf(view.fov.angleLeft), r = tanf(view.fov.angleRight);
        const F32 u = tanf(view.fov.angleUp), d = tanf(view.fov.angleDown);
        out[0] = (2.f / (r - l)) * e.x + ((r + l) / (r - l)) * e.z;
        out[1] = (2.f / (u - d)) * e.y + ((u + d) / (u - d)) * e.z;
        out[2] = -((FAR_Z + NEAR_Z) / (FAR_Z - NEAR_Z)) * e.z - (2.f * FAR_Z * NEAR_Z / (FAR_Z - NEAR_Z));
        out[3] = -e.z;
    }

    // Horizontal rectangle centred at (cx, cy, cz), width w (x), depth d (z).
    void drawRect(const XrView& view, F32 cx, F32 cy, F32 cz, F32 w, F32 d, F32 r, F32 g, F32 b, F32 a)
    {
        const Vec3 corners[4] = { { cx - w / 2, cy, cz + d / 2 }, { cx + w / 2, cy, cz + d / 2 },
                                  { cx - w / 2, cy, cz - d / 2 }, { cx + w / 2, cy, cz - d / 2 } };
        F32 verts[16];
        for (U32 i = 0; i < 4; ++i)
        {
            toClip(view, corners[i], verts + i * 4);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glUniform4f(sColorLoc, r, g, b, a);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void logWindow(const char* what)
    {
        LL_INFOS("PATXR") << what << llformat(" keyboard window: centre x=%.2f y=%.2f z=%.2f m, size %.0fx%.0f cm",
                                               sX, sY, sZ, sW * 100.f, sD * 100.f) << LL_ENDL;
    }
}

void PATXRKeyboard::update(F32 dt)
{
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
        return;
    }
    sWasHeld = true;
    const F32 step = MOVE_SPEED * llclamp(dt, 0.f, 0.1f);
    auto held = [](KEY k) { return gKeyboard->getKeyDown(k); };
    const F32 before = sX + sY * 3.f + sZ * 7.f + sW * 11.f + sD * 13.f;
    if (held(KEY_LEFT))      sX -= step;
    if (held(KEY_RIGHT))     sX += step;
    if (held(KEY_UP))        sZ -= step; // away from the user
    if (held(KEY_DOWN))      sZ += step;
    if (held(KEY_PAGE_UP))   sY += step;
    if (held(KEY_PAGE_DOWN)) sY -= step;
    if (held(KEY_HOME))      sW += step;
    if (held(KEY_END))       sW = llmax(0.05f, sW - step);
    if (held(KEY_INSERT))    sD += step;
    if (held(KEY_DELETE))    sD = llmax(0.03f, sD - step);
    if (held(KEY_BACKSPACE))
    {
        sX = DEFAULT_X; sY = DEFAULT_Y; sZ = DEFAULT_Z; sW = DEFAULT_W; sD = DEFAULT_D;
    }
    if (before != sX + sY * 3.f + sZ * 7.f + sW * 11.f + sD * 13.f)
    {
        sChanged = true;
    }
}

void PATXRKeyboard::draw(const XrView& view, const std::vector<XrVector3f>& palms)
{
    if (!initGL())
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
    drawRect(view, sX, sY, sZ, sW + 2 * FRAME, sD + 2 * FRAME, 1.f, 1.f, 1.f, 1.f);
    drawRect(view, sX, sY, sZ, sW, sD, 0.f, 0.f, 0.f, 0.f);
    // Hands near desk height get their own window (test of the automatic extension).
    for (const XrVector3f& p : palms)
    {
        if (p.y < sY + 0.25f)
        {
            drawRect(view, p.x, p.y, p.z, HAND_SIZE, HAND_SIZE, 0.f, 0.f, 0.f, 0.f);
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
}

#endif // PAT_XR
