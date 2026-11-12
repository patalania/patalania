/**
 * @file patxrkeyboard.cpp
 * @brief <PAT:XR> Passthrough window for the real keyboard (and hands) in VR.
 *
 * A horizontal rectangle in the LOCAL space (on the desk, where the real
 * keyboard is) is drawn with alpha 0; WiVRn shows the headset camera image
 * there. Phase 6b: feasibility; phase 8: placement from the resting hands
 * (position, height, direction, width), soft following, manual adjustment by
 * shortcuts on top, calibration saved in the settings.
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
#include "llviewercontrol.h"

#include <cmath>

namespace
{
    // Default window relative to the head pose at VR start (metres, head frame),
    // used until the hands have been seen resting on the keyboard.
    constexpr F32 DEFAULT_BELOW = 0.50f;   // below the eyes
    constexpr F32 DEFAULT_AHEAD = 0.40f;   // in front
    constexpr F32 DEFAULT_W = 0.46f;       // width (full-size keyboard)
    constexpr F32 DEFAULT_D = 0.18f;       // depth
    constexpr F32 FRAME = 0.006f;          // white border, 6 mm
    constexpr F32 HAND_SIZE = 0.20f;       // passthrough square around a palm
    constexpr F32 MOVE_SPEED = 0.10f;      // m/s while a key is held
    constexpr F32 TURN_SPEED = 20.f * DEG_TO_RAD; // rad/s while a key is held
    constexpr F32 NEAR_Z = 0.05f;
    constexpr F32 FAR_Z = 100.f;
    // Hands resting on the keyboard
    constexpr F32 HANDS_STILL_SECONDS = 1.0f;
    constexpr F32 HANDS_STILL_RADIUS = 0.02f;  // max movement while "resting"
    constexpr F32 HANDS_MAX_DY = 0.07f;        // similar height
    constexpr F32 HANDS_MIN_DIST = 0.12f;      // palm centres, horizontal
    constexpr F32 HANDS_MAX_DIST = 0.45f;      // farther apart: one hand is on the mouse
    constexpr F32 HANDS_MAX_TURN = 30.f * DEG_TO_RAD; // line between the hands vs. finger direction
    constexpr F32 HANDS_BELOW_HEAD = 0.20f;    // resting hands are at least this far below the eyes
    constexpr F32 PALM_ABOVE_KEYS = 0.03f;     // palm centre is a bit above the key tops
    constexpr F32 KEYS_BEYOND_PALMS = 0.03f;   // keyboard centre a bit beyond the palms (away from the user)
    constexpr F32 HAND_MARGIN = 0.22f;         // added to the hand distance for the width
    constexpr F32 MIN_W = 0.30f, MAX_W = 0.60f;
    // Following the hands later on: only near the current window, blended in softly.
    constexpr F32 FOLLOW_MAX_DY = 0.10f;
    constexpr F32 FOLLOW_MAX_DIST = 0.25f;
    constexpr F32 FOLLOW_WEIGHT = 0.35f;       // share of the new hand estimate per resting period
    constexpr F32 SHOW_SMOOTHING = 0.25f;      // s, time constant of the shown window
    constexpr F64 SAVE_INTERVAL = 1.0;         // s between writes of the calibration settings

    struct Vec3 { F32 x, y, z; };
    struct Pose { Vec3 c{ 0.f, 0.f, 0.f }; F32 yaw = 0.f; F32 w = DEFAULT_W; F32 d = DEFAULT_D; };

    // Window state (LOCAL space). yaw rotates the window's x axis around +Y.
    // shown = smoothed (base from the hands + manual adjustment in the window frame).
    bool sPlaced = false;        // something to show (head known)
    bool sHaveBase = false;      // base from resting hands (or loaded from the settings)
    Pose sBase;
    Vec3 sAdj = { 0.f, 0.f, 0.f };  // x along the keyboard, y up, z towards the user
    F32 sAdjYaw = 0.f, sAdjW = 0.f, sAdjD = 0.f;
    Pose sShown;
    bool sChanged = false;
    bool sWasHeld = false;
    bool sBackspaceHeld = false;
    bool sDirty = false;
    F64 sLastSave = 0.0;
    Vec3 sStillRef[2];
    F32 sStillTime = 0.f;
    F64 sLastRejectLog = 0.0;

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

    // Yaw whose axisZ points along -dir (dir = horizontal direction away from the user).
    F32 yawFacing(const Vec3& dir) { return atan2f(-dir.x, -dir.z); }

    F32 angleDiff(F32 a, F32 b)
    {
        F32 d = fmodf(a - b + F_PI, 2.f * F_PI);
        if (d < 0.f)
        {
            d += 2.f * F_PI;
        }
        return d - F_PI;
    }

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

    void logPose(const char* what, const Pose& p)
    {
        LL_INFOS("PATXR") << what << llformat(" keyboard window: centre x=%.2f y=%.2f z=%.2f m, yaw %.0f deg, size %.0fx%.0f cm",
                                               p.c.x, p.c.y, p.c.z, p.yaw * RAD_TO_DEG, p.w * 100.f, p.d * 100.f) << LL_ENDL;
    }

    // Base + manual adjustment.
    Pose target()
    {
        Pose t;
        t.yaw = sBase.yaw + sAdjYaw;
        const Vec3 ax = axisX(t.yaw), az = axisZ(t.yaw);
        t.c = { sBase.c.x + ax.x * sAdj.x + az.x * sAdj.z, sBase.c.y + sAdj.y, sBase.c.z + ax.z * sAdj.x + az.z * sAdj.z };
        t.w = llclamp(sBase.w + sAdjW, 0.10f, 1.20f);
        t.d = llclamp(sBase.d + sAdjD, 0.05f, 0.60f);
        return t;
    }

    void smoothShown(F32 dt)
    {
        const Pose t = target();
        const F32 k = 1.f - expf(-llclamp(dt, 0.f, 0.1f) / SHOW_SMOOTHING);
        sShown.c = { sShown.c.x + (t.c.x - sShown.c.x) * k, sShown.c.y + (t.c.y - sShown.c.y) * k,
                     sShown.c.z + (t.c.z - sShown.c.z) * k };
        sShown.yaw += angleDiff(t.yaw, sShown.yaw) * k;
        sShown.w += (t.w - sShown.w) * k;
        sShown.d += (t.d - sShown.d) * k;
    }

    void save(bool force)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (!sDirty || (!force && now - sLastSave < SAVE_INTERVAL))
        {
            return;
        }
        sDirty = false;
        sLastSave = now;
        gSavedSettings.setBOOL("PATVRKeyboardCalibrated", sHaveBase);
        gSavedSettings.setVector3("PATVRKeyboardCenter", LLVector3(sBase.c.x, sBase.c.y, sBase.c.z));
        gSavedSettings.setF32("PATVRKeyboardYaw", sBase.yaw * RAD_TO_DEG);
        gSavedSettings.setVector3("PATVRKeyboardSize", LLVector3(sBase.w, sBase.d, 0.f));
        gSavedSettings.setVector3("PATVRKeyboardAdjustPos", LLVector3(sAdj.x, sAdj.y, sAdj.z));
        gSavedSettings.setVector3("PATVRKeyboardAdjustYawSize", LLVector3(sAdjYaw * RAD_TO_DEG, sAdjW, sAdjD));
    }

    // Default: below and in front of the head, turned to the head's yaw.
    void placeFromHead(const Vec3& head, const Vec3& fwd)
    {
        sBase.yaw = yawFacing(fwd);
        const Vec3 az = axisZ(sBase.yaw);
        sBase.c = { head.x - az.x * DEFAULT_AHEAD, head.y - DEFAULT_BELOW, head.z - az.z * DEFAULT_AHEAD };
        sBase.w = DEFAULT_W;
        sBase.d = DEFAULT_D;
        sAdj = { 0.f, 0.f, 0.f };
        sAdjYaw = sAdjW = sAdjD = 0.f;
        sHaveBase = false;
        sShown = target();
        sPlaced = true;
        sStillTime = 0.f;
        logPose("Default", sShown);
    }

    // Saved calibration (LOCAL space), if it is plausible for the current head
    // position (WiVRn's LOCAL origin may change between sessions).
    bool loadSaved(const Vec3& head)
    {
        if (!gSavedSettings.getBOOL("PATVRKeyboardCalibrated"))
        {
            return false;
        }
        const LLVector3 c = gSavedSettings.getVector3("PATVRKeyboardCenter");
        const LLVector3 size = gSavedSettings.getVector3("PATVRKeyboardSize");
        const LLVector3 adj = gSavedSettings.getVector3("PATVRKeyboardAdjustPos");
        const LLVector3 adj2 = gSavedSettings.getVector3("PATVRKeyboardAdjustYawSize");
        const F32 horiz = sqrtf((c.mV[VX] - head.x) * (c.mV[VX] - head.x) + (c.mV[VZ] - head.z) * (c.mV[VZ] - head.z));
        const F32 below = head.y - c.mV[VY];
        if (horiz > 1.0f || below < 0.15f || below > 1.0f)
        {
            LL_INFOS("PATXR") << llformat("Saved keyboard window not used: %.2f m from the head, %.2f m below the eyes "
                                          "(the room origin changed?)", horiz, below) << LL_ENDL;
            return false;
        }
        sBase.c = { c.mV[VX], c.mV[VY], c.mV[VZ] };
        sBase.yaw = gSavedSettings.getF32("PATVRKeyboardYaw") * DEG_TO_RAD;
        sBase.w = llclamp(size.mV[VX], MIN_W, MAX_W);
        sBase.d = llclamp(size.mV[VY], 0.05f, 0.60f);
        sAdj = { adj.mV[VX], adj.mV[VY], adj.mV[VZ] };
        sAdjYaw = adj2.mV[VX] * DEG_TO_RAD;
        sAdjW = adj2.mV[VY];
        sAdjD = adj2.mV[VZ];
        sHaveBase = true;
        sShown = target();
        sPlaced = true;
        logPose("Saved", sShown);
        return true;
    }

    F32 dist(const Vec3& a, const Vec3& b)
    {
        return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    void logRejected(const char* why)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now - sLastRejectLog > 10.0)
        {
            sLastRejectLog = now;
            LL_INFOS("PATXR") << "Resting hands ignored for the keyboard window: " << why << LL_ENDL;
        }
    }

    // Keyboard estimate from both hands resting on it: direction from the line
    // between the hands, checked against (and averaged with) the direction of the
    // fingers. A hand on the mouse gives a line that does not fit and is ignored.
    bool estimateFromHands(const PATXRPalm palms[2], const Vec3& head, Pose& est)
    {
        const Vec3 l = { palms[0].pos.x, palms[0].pos.y, palms[0].pos.z };
        const Vec3 r = { palms[1].pos.x, palms[1].pos.y, palms[1].pos.z };
        const F32 horiz = sqrtf((r.x - l.x) * (r.x - l.x) + (r.z - l.z) * (r.z - l.z));
        if ((l.y + r.y) * 0.5f > head.y - HANDS_BELOW_HEAD)
        {
            logRejected("hands not below the head");
            return false;
        }
        if (horiz < HANDS_MIN_DIST || horiz > HANDS_MAX_DIST)
        {
            logRejected(horiz > HANDS_MAX_DIST ? "hands too far apart (one hand on the mouse?)" : "hands too close together");
            return false;
        }
        F32 yaw = atan2f(-(r.z - l.z), r.x - l.x); // axisX along left -> right hand
        if (palms[0].orientationValid && palms[1].orientationValid)
        {
            Vec3 f = { 0.f, 0.f, 0.f };
            for (U32 i = 0; i < 2; ++i)
            {
                Vec3 fi = rotate(palms[i].orientation, { 0.f, 0.f, -1.f }, false); // towards the fingers
                const F32 len = sqrtf(fi.x * fi.x + fi.z * fi.z);
                if (len > 0.2f)
                {
                    f.x += fi.x / len;
                    f.z += fi.z / len;
                }
            }
            if (f.x * f.x + f.z * f.z > 0.25f)
            {
                const F32 yaw_fingers = yawFacing(f);
                const F32 diff = angleDiff(yaw_fingers, yaw);
                if (fabsf(diff) > HANDS_MAX_TURN)
                {
                    logRejected(llformat("fingers point %.0f deg away from the line between the hands", diff * RAD_TO_DEG).c_str());
                    return false;
                }
                yaw += 0.5f * diff;
            }
        }
        const Vec3 az = axisZ(yaw);
        est.yaw = yaw;
        est.c = { (l.x + r.x) * 0.5f - az.x * KEYS_BEYOND_PALMS, (l.y + r.y) * 0.5f - PALM_ABOVE_KEYS,
                  (l.z + r.z) * 0.5f - az.z * KEYS_BEYOND_PALMS };
        est.w = llclamp(horiz + HAND_MARGIN, MIN_W, MAX_W);
        est.d = DEFAULT_D;
        return true;
    }

    // Both hands resting ~1 s at a similar height: first time the window jumps
    // onto them, afterwards it follows softly (only when they rest near it).
    void followHands(F32 dt, const PATXRPalm palms[2], const Vec3& head)
    {
        if (!palms[0].valid || !palms[1].valid)
        {
            sStillTime = 0.f;
            return;
        }
        const Vec3 l = { palms[0].pos.x, palms[0].pos.y, palms[0].pos.z };
        const Vec3 r = { palms[1].pos.x, palms[1].pos.y, palms[1].pos.z };
        if (fabsf(l.y - r.y) > HANDS_MAX_DY)
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
        sStillTime = 0.f; // next resting period starts now
        Pose est;
        if (!estimateFromHands(palms, head, est))
        {
            return;
        }
        if (!sHaveBase)
        {
            sBase = est;
            sAdj = { 0.f, 0.f, 0.f };
            sAdjYaw = sAdjW = sAdjD = 0.f;
            sHaveBase = true;
            sDirty = true;
            logPose("Placed on the hands:", target());
            return;
        }
        const F32 dy = fabsf(est.c.y - sBase.c.y);
        const F32 dh = sqrtf((est.c.x - sBase.c.x) * (est.c.x - sBase.c.x) + (est.c.z - sBase.c.z) * (est.c.z - sBase.c.z));
        const F32 dyaw = angleDiff(est.yaw, sBase.yaw);
        if (dy > FOLLOW_MAX_DY || dh > FOLLOW_MAX_DIST || fabsf(dyaw) > HANDS_MAX_TURN)
        {
            logRejected("hands resting away from the keyboard window (Ctrl+Shift+Backspace measures again)");
            return;
        }
        sBase.c = { sBase.c.x + (est.c.x - sBase.c.x) * FOLLOW_WEIGHT, sBase.c.y + (est.c.y - sBase.c.y) * FOLLOW_WEIGHT,
                    sBase.c.z + (est.c.z - sBase.c.z) * FOLLOW_WEIGHT };
        sBase.yaw += dyaw * FOLLOW_WEIGHT;
        sBase.w += (est.w - sBase.w) * FOLLOW_WEIGHT;
        sDirty = true;
        if (dh > 0.02f || dy > 0.02f || fabsf(dyaw) > 3.f * DEG_TO_RAD)
        {
            LL_INFOS("PATXR") << llformat("Keyboard window follows the hands: %.0f cm sideways, %.0f cm height, %.0f deg "
                                          "(%.0f%% of it)", dh * 100.f, dy * 100.f, dyaw * RAD_TO_DEG, FOLLOW_WEIGHT * 100.f) << LL_ENDL;
        }
    }
}

void PATXRKeyboard::update(F32 dt, const XrPosef& left_eye, const XrPosef& right_eye, const PATXRPalm palms[2])
{
    const Vec3 head = { (left_eye.position.x + right_eye.position.x) * 0.5f,
                        (left_eye.position.y + right_eye.position.y) * 0.5f,
                        (left_eye.position.z + right_eye.position.z) * 0.5f };
    const Vec3 fwd = rotate(left_eye.orientation, { 0.f, 0.f, -1.f }, false);
    if (!sPlaced)
    {
        LL_INFOS("PATXR") << llformat("Head at VR start: x=%.2f y=%.2f z=%.2f m", head.x, head.y, head.z) << LL_ENDL;
        if (!loadSaved(head))
        {
            placeFromHead(head, fwd);
        }
    }
    followHands(dt, palms, head);

    bool manual = false;
    if (gKeyboard)
    {
        const MASK mask = gKeyboard->currentMask(false);
        const bool ctrl_shift = (mask & (MASK_CONTROL | MASK_SHIFT)) == (MASK_CONTROL | MASK_SHIFT);
        const bool alt = (mask & MASK_ALT) != 0;
        if (!ctrl_shift)
        {
            if (sWasHeld && sChanged)
            {
                logPose("Adjusted", target());
                sChanged = false;
                save(true);
            }
            sWasHeld = false;
            sBackspaceHeld = false;
        }
        else
        {
            sWasHeld = true;
            auto held = [](KEY k) { return gKeyboard->getKeyDown(k); };

            // Measure again: default below the head, then wait for resting hands.
            if (held(KEY_BACKSPACE) && !alt)
            {
                if (!sBackspaceHeld)
                {
                    LL_INFOS("PATXR") << "Keyboard window: measuring again" << LL_ENDL;
                    placeFromHead(head, fwd);
                    sDirty = true;
                    save(true);
                }
                sBackspaceHeld = true;
            }
            else
            {
                sBackspaceHeld = false;
                // Fine adjustment in the window's own frame, kept on top of the hand placement.
                const F32 step = MOVE_SPEED * llclamp(dt, 0.f, 0.1f);
                const F32 turn = TURN_SPEED * llclamp(dt, 0.f, 0.1f);
                const F32 before = sAdj.x + sAdj.y + sAdj.z + sAdjYaw + sAdjW + sAdjD;
                if (!alt)
                {
                    if (held(KEY_LEFT))      sAdj.x -= step;
                    if (held(KEY_RIGHT))     sAdj.x += step;
                    if (held(KEY_UP))        sAdj.z -= step; // away from the user
                    if (held(KEY_DOWN))      sAdj.z += step;
                    if (held(KEY_PAGE_UP))   sAdj.y += step;
                    if (held(KEY_PAGE_DOWN)) sAdj.y -= step;
                    if (held(KEY_HOME))      sAdjYaw += turn; // counter-clockwise seen from above
                    if (held(KEY_END))       sAdjYaw -= turn;
                    if (held(KEY_INSERT))    sAdjW += step;
                    if (held(KEY_DELETE))    sAdjW -= step;
                }
                else
                {
                    // Ctrl+Shift+Alt+PgUp/PgDn: depth (Home/End with Alt = UI panel size)
                    if (held(KEY_PAGE_UP))   sAdjD += step;
                    if (held(KEY_PAGE_DOWN)) sAdjD -= step;
                }
                if (sAdj.x + sAdj.y + sAdj.z + sAdjYaw + sAdjW + sAdjD != before)
                {
                    manual = true;
                    sChanged = true;
                    sDirty = true;
                }
            }
        }
    }

    if (manual)
    {
        sShown = target(); // keys act at once
    }
    else
    {
        smoothShown(dt);
    }
    if (!sWasHeld)
    {
        save(false);
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
    drawRect(view, sShown.c, sShown.yaw, sShown.w + 2 * FRAME, sShown.d + 2 * FRAME, 1.f, 1.f, 1.f, 1.f);
    drawRect(view, sShown.c, sShown.yaw, sShown.w, sShown.d, 0.f, 0.f, 0.f, 0.f);
    // Hands near desk height get their own window (unchanged from the first test).
    for (U32 i = 0; i < 2; ++i)
    {
        if (palms[i].valid && palms[i].pos.y < sShown.c.y + 0.25f)
        {
            drawRect(view, { palms[i].pos.x, palms[i].pos.y, palms[i].pos.z }, sShown.yaw, HAND_SIZE, HAND_SIZE, 0.f, 0.f, 0.f, 0.f);
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
    save(true);
    sPlaced = false;
    sHaveBase = false;
    sStillTime = 0.f;
}

#endif // PAT_XR
