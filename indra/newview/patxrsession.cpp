/**
 * @file patxrsession.cpp
 * @brief <PAT:XR> OpenXR session for the Patalania VR mode (platform neutral).
 *
 * Instance, system, session, LOCAL space, one swapchain per eye, event loop and
 * the frame loop. Phase 7: the world is rendered per eye inside display()
 * (patxrstereo.cpp); before login or without a world image the frame shows the
 * headset camera (passthrough, alpha 0).
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

#include "patxrsession.h"

#if PAT_XR

#include "patxrsession_platform.h"
#include "patxrhands.h"
#include "patxrkeyboard.h"
#include "patxrstereo.h"
#include "patxrui.h"

#include "llagent.h"
#include "llgl.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr const char* SETTING_ENABLED = "PATVREnabled";
    constexpr const char* SETTING_RES_SCALE = "PATVRResolutionScale";
    constexpr const char* SETTING_HALF_RATE = "PATVRHalfRate";
    constexpr F64 STOP_TIMEOUT_SECONDS = 3.0;
    constexpr F64 STATS_INTERVAL_SECONDS = 10.0;
    constexpr XrDuration SWAPCHAIN_WAIT_TIMEOUT = 100000000; // 100 ms, never block the viewer for good
    constexpr F32 ESCAPE_HOLD_SECONDS = 1.5f;                // hold Escape = leave VR

    // Diagnosis: a helper thread reports when the main thread is stuck inside a
    // runtime call (WiVRn answers over a socket; if it never does, the viewer
    // hangs there). Only logs, it cannot unblock the call.
    std::atomic<const char*> sCallName{ nullptr };
    std::atomic<F64> sCallStart{ 0.0 };
    std::atomic<bool> sMonitorRunning{ false };

    struct RuntimeCall
    {
        explicit RuntimeCall(const char* name)
        {
            sCallStart = LLTimer::getTotalSeconds();
            sCallName = name;
        }
        ~RuntimeCall() { sCallName = nullptr; }
    };

    void startStallMonitor()
    {
        bool expected = false;
        if (!sMonitorRunning.compare_exchange_strong(expected, true))
        {
            return;
        }
        std::thread([]()
        {
            const char* reported = nullptr;
            while (sMonitorRunning)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const char* name = sCallName;
                if (name && name != reported && LLTimer::getTotalSeconds() - sCallStart.load() > 2.0)
                {
                    LL_WARNS("PATXR") << "Main thread blocked in " << name << " for more than 2 s (OpenXR runtime not answering)" << LL_ENDL;
                    reported = name;
                }
                else if (!name && reported)
                {
                    LL_INFOS("PATXR") << "Runtime answered again (" << reported << ")" << LL_ENDL;
                    reported = nullptr;
                }
            }
        }).detach();
    }

    struct EyeSwapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLKHR> images;
        S32 pending = -1; // acquired, wait timed out
    };

    struct XRState
    {
        XrInstance instance = XR_NULL_HANDLE;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        XrSession session = XR_NULL_HANDLE;
        XrSpace space = XR_NULL_HANDLE;
        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        EyeSwapchain eyes[2];
        EyeSwapchain ui;             // phase 8: UI panel as quad layer
        int64_t format = 0;
        bool uiActive = false;       // UI goes into the panel
        F32 uiScale = 1.f;
        bool lastQuadValid = false;
        XrCompositionLayerQuad lastQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        GLuint fbo = 0;
        bool running = false;       // between xrBeginSession and xrEndSession
        bool stopRequested = false; // we asked the runtime to end the session
        F64 stopRequestTime = 0.0;
        U64 frames = 0;
        bool handExt = false;
        F64 lastFrameTime = 0.0;
        PATXRPalm palms[2];
        F32 lastDt = 0.f;

        // Frame begun at the end of the previous main-loop iteration, rendered by
        // display() in between, submitted at the end of this iteration.
        bool frameOpen = false;
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        bool viewsLocated = false;
        bool viewsValid = false;
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        bool eyesActive = false;    // between the two display() hooks
        bool eyesRendered = false;  // world image of this frame is ready
        F64 eyesStart = 0.0;

        // Last submitted world layer (re-submitted by the half-rate mode).
        bool lastLayerValid = false;
        XrCompositionLayerProjectionView lastViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                          { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };

        // Statistics (logged every STATS_INTERVAL_SECONDS)
        F64 statsStart = 0.0;
        U32 statsSubmitted = 0;
        U32 statsRendered = 0;
        U32 statsRepeated = 0;
        U32 statsMissed = 0;
        F64 statsEyeSeconds = 0.0;
        F64 statsEyeMax = 0.0;
        XrTime lastDisplayTime = 0;
        U32 swapchainTimeouts = 0;
    };

    XRState sXR;

    std::string resultString(XrResult result)
    {
        char buf[XR_MAX_RESULT_STRING_SIZE] = { 0 };
        if (sXR.instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(sXR.instance, result, buf)))
        {
            return buf;
        }
        return llformat("XrResult %d", (S32)result);
    }

    // Release everything in reverse order. Safe to call in any state.
    void destroyAll(bool viewer_shutdown = false)
    {
        const bool ui_was_active = sXR.uiActive;
        PATXRUI::release();
        PATXRPlatform::confineMouse(false);
        PATXRStereo::release(viewer_shutdown);
        PATXRKeyboard::release();
        PATXRHands::release();
        if (sXR.fbo)
        {
            glDeleteFramebuffers(1, &sXR.fbo);
            sXR.fbo = 0;
        }
        for (EyeSwapchain& eye : sXR.eyes)
        {
            if (eye.handle != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(eye.handle);
            }
            eye = EyeSwapchain();
        }
        if (sXR.ui.handle != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(sXR.ui.handle);
        }
        if (sXR.space != XR_NULL_HANDLE)
        {
            xrDestroySpace(sXR.space);
        }
        if (sXR.session != XR_NULL_HANDLE)
        {
            xrDestroySession(sXR.session);
        }
        if (sXR.instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(sXR.instance);
            LL_INFOS("PATXR") << "OpenXR stopped after " << sXR.frames << " frames" << LL_ENDL;
        }
        sXR = XRState();
        if (ui_was_active && !viewer_shutdown && gViewerWindow)
        {
            gViewerWindow->requestResolutionUpdate(); // UI scale back to the desktop value
        }
    }

    // Leave VR: release OpenXR, switch the setting off and tell the user why.
    void fallBackToDesktop(const std::string& notification, const std::string& reason)
    {
        LL_WARNS("PATXR") << notification << ": " << reason << LL_ENDL;
        destroyAll();
        gSavedSettings.setBOOL(SETTING_ENABLED, false);
        LLSD args;
        args["REASON"] = reason;
        LLNotificationsUtil::add(notification, args);
    }

    bool failed(XrResult result, const char* what, std::string& error)
    {
        if (XR_SUCCEEDED(result))
        {
            return false;
        }
        error = std::string(what) + " failed: " + resultString(result);
        return true;
    }

    bool hasExtension(const char* name)
    {
        uint32_t count = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr)))
        {
            return false;
        }
        std::vector<XrExtensionProperties> props(count, { XR_TYPE_EXTENSION_PROPERTIES });
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data())))
        {
            return false;
        }
        for (const XrExtensionProperties& p : props)
        {
            if (!strcmp(p.extensionName, name))
            {
                return true;
            }
        }
        return false;
    }

    int64_t chooseSwapchainFormat(std::string& error)
    {
        uint32_t count = 0;
        if (failed(xrEnumerateSwapchainFormats(sXR.session, 0, &count, nullptr), "xrEnumerateSwapchainFormats", error))
        {
            return 0;
        }
        std::vector<int64_t> formats(count);
        if (failed(xrEnumerateSwapchainFormats(sXR.session, count, &count, formats.data()), "xrEnumerateSwapchainFormats", error))
        {
            return 0;
        }
        for (int64_t wanted : { (int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8 })
        {
            for (int64_t f : formats)
            {
                if (f == wanted)
                {
                    return f;
                }
            }
        }
        if (!formats.empty())
        {
            return formats[0];
        }
        error = "Runtime offers no swapchain format";
        return 0;
    }

    // Connection to the runtime (instance + headset). Runs on a worker thread:
    // WiVRn blocks xrCreateInstance while its server has no headset connected,
    // which must not freeze the viewer. Uses no shared state except its arguments.
    struct PendingConnect
    {
        std::mutex mutex;
        bool done = false;       // worker finished (guarded by mutex)
        bool abandoned = false;  // VR switched off meanwhile: worker cleans up (guarded)
        XrInstance instance = XR_NULL_HANDLE;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        bool handExt = false;
        std::string error;
    };
    std::shared_ptr<PendingConnect> sPending;
    F64 sPendingSince = 0.0;
    bool sWaitNotified = false;

    std::string connect(XrInstance& instance, XrSystemId& system, bool& hand_ext)
    {
        const char* gfx_ext = PATXRPlatform::graphicsExtensionName();

        // 1. Runtime + graphics extension (fails cleanly when no runtime is installed).
        uint32_t ext_count = 0;
        XrResult res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
        if (XR_FAILED(res))
        {
            return llformat("No OpenXR runtime available (XrResult %d). Is the WiVRn server running?", (S32)res);
        }
        if (!hasExtension(gfx_ext))
        {
            return std::string("The OpenXR runtime does not support ") + gfx_ext;
        }

        // 2. Instance (WiVRn: returns only once a headset is connected).
        XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
        LLStringUtil::copy(ici.applicationInfo.applicationName, "Patalania", XR_MAX_APPLICATION_NAME_SIZE);
        ici.applicationInfo.applicationVersion = 1;
        LLStringUtil::copy(ici.applicationInfo.engineName, "Patalania", XR_MAX_ENGINE_NAME_SIZE);
        ici.applicationInfo.engineVersion = 1;
        ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        hand_ext = hasExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        const char* extensions[2] = { gfx_ext, XR_EXT_HAND_TRACKING_EXTENSION_NAME };
        ici.enabledExtensionCount = hand_ext ? 2 : 1;
        ici.enabledExtensionNames = extensions;
        res = xrCreateInstance(&ici, &instance);
        if (XR_FAILED(res))
        {
            instance = XR_NULL_HANDLE;
            return llformat("Could not connect to the OpenXR runtime (XrResult %d). Is the WiVRn server running?", (S32)res);
        }

        XrInstanceProperties iprops{ XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &iprops)))
        {
            LL_INFOS("PATXR") << "OpenXR runtime: " << iprops.runtimeName << " "
                              << XR_VERSION_MAJOR(iprops.runtimeVersion) << "."
                              << XR_VERSION_MINOR(iprops.runtimeVersion) << "."
                              << XR_VERSION_PATCH(iprops.runtimeVersion) << LL_ENDL;
        }

        // 3. Headset.
        XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        res = xrGetSystem(instance, &sgi, &system);
        if (res == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
        {
            return "No headset connected. Connect the headset to the WiVRn server and try again.";
        }
        if (XR_FAILED(res))
        {
            return llformat("xrGetSystem failed (XrResult %d)", (S32)res);
        }
        XrSystemProperties sprops{ XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sprops)))
        {
            LL_INFOS("PATXR") << "Headset: " << sprops.systemName << ", max swapchain "
                              << sprops.graphicsProperties.maxSwapchainImageWidth << "x"
                              << sprops.graphicsProperties.maxSwapchainImageHeight << LL_ENDL;
        }
        return std::string();
    }

    void startConnect()
    {
        PATXRPlatform::prepareRuntime(); // main thread: may set the loader environment
        auto pending = std::make_shared<PendingConnect>();
        sPending = pending;
        sPendingSince = LLTimer::getTotalSeconds();
        sWaitNotified = false;
        std::thread([pending]()
        {
            XrInstance instance = XR_NULL_HANDLE;
            XrSystemId system = XR_NULL_SYSTEM_ID;
            bool hand_ext = false;
            std::string error = connect(instance, system, hand_ext);
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->done = true;
            if (pending->abandoned || !error.empty())
            {
                if (instance != XR_NULL_HANDLE)
                {
                    xrDestroyInstance(instance);
                }
                instance = XR_NULL_HANDLE;
            }
            pending->instance = instance;
            pending->system = system;
            pending->handExt = hand_ext;
            pending->error = error;
        }).detach();
    }

    // Returns an empty string on success, otherwise a readable reason.
    // Needs sXR.instance/system from connect(); runs on the main thread (GL).
    std::string start()
    {
        std::string error;
        const bool hand_ext = sXR.handExt;

        // 4. Stereo view configuration.
        uint32_t view_count = 0;
        if (failed(xrEnumerateViewConfigurationViews(sXR.instance, sXR.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                     0, &view_count, nullptr), "xrEnumerateViewConfigurationViews", error))
        {
            return error;
        }
        if (view_count != 2)
        {
            return llformat("Unexpected number of views: %u", view_count);
        }
        std::vector<XrViewConfigurationView> views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        if (failed(xrEnumerateViewConfigurationViews(sXR.instance, sXR.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                     view_count, &view_count, views.data()), "xrEnumerateViewConfigurationViews", error))
        {
            return error;
        }

        uint32_t blend_count = 0;
        XrEnvironmentBlendMode blend_modes[8];
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(sXR.instance, sXR.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                          8, &blend_count, blend_modes)) && blend_count > 0)
        {
            // Phase 6b: ALPHA_BLEND lets WiVRn show the headset camera where alpha is 0
            // (keyboard window). Without it the runtime offers no passthrough.
            sXR.blendMode = blend_modes[0];
            std::string modes;
            for (uint32_t i = 0; i < blend_count; ++i)
            {
                modes += llformat(" %d", (S32)blend_modes[i]);
                if (blend_modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
                {
                    sXR.blendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                }
            }
            LL_INFOS("PATXR") << "Blend modes offered:" << modes << LL_ENDL;
            if (sXR.blendMode != XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
            {
                LL_WARNS("PATXR") << "No ALPHA_BLEND: keyboard passthrough window not possible "
                                     "(headset without passthrough, or WiVRn passthrough disabled)" << LL_ENDL;
            }
        }

        // 5. OpenGL requirements (mandatory call before xrCreateSession).
        PFN_xrGetOpenGLGraphicsRequirementsKHR get_gl_reqs = nullptr;
        if (failed(xrGetInstanceProcAddr(sXR.instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                         (PFN_xrVoidFunction*)&get_gl_reqs), "xrGetInstanceProcAddr", error))
        {
            return error;
        }
        XrGraphicsRequirementsOpenGLKHR gl_reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        if (failed(get_gl_reqs(sXR.instance, sXR.system, &gl_reqs), "xrGetOpenGLGraphicsRequirementsKHR", error))
        {
            return error;
        }
        LL_INFOS("PATXR") << "Runtime needs OpenGL " << XR_VERSION_MAJOR(gl_reqs.minApiVersionSupported) << "."
                          << XR_VERSION_MINOR(gl_reqs.minApiVersionSupported) << ", viewer has "
                          << gGLManager.mGLVersion << LL_ENDL;

        // 6. Session bound to the viewer's GL context.
        const void* binding = PATXRPlatform::graphicsBinding(error);
        if (!binding)
        {
            return error;
        }
        XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
        sci.next = binding;
        sci.systemId = sXR.system;
        if (failed(xrCreateSession(sXR.instance, &sci, &sXR.session), "xrCreateSession", error))
        {
            sXR.session = XR_NULL_HANDLE;
            return error;
        }

        // 7. Seated reference space.
        XrReferenceSpaceCreateInfo rci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rci.poseInReferenceSpace.orientation.w = 1.f;
        if (failed(xrCreateReferenceSpace(sXR.session, &rci, &sXR.space), "xrCreateReferenceSpace", error))
        {
            sXR.space = XR_NULL_HANDLE;
            return error;
        }

        // 8. One swapchain per eye: recommended size x PATVRResolutionScale.
        const F32 res_scale = llclamp(gSavedSettings.getF32(SETTING_RES_SCALE), 0.3f, 2.f);
        int64_t format = chooseSwapchainFormat(error);
        if (!format)
        {
            return error;
        }
        sXR.format = format;
        for (uint32_t i = 0; i < 2; ++i)
        {
            EyeSwapchain& eye = sXR.eyes[i];
            XrSwapchainCreateInfo swci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            swci.format = format;
            swci.sampleCount = 1;
            swci.width = llclamp((uint32_t)ll_round(views[i].recommendedImageRectWidth * res_scale), 256u,
                                 views[i].maxImageRectWidth);
            swci.height = llclamp((uint32_t)ll_round(views[i].recommendedImageRectHeight * res_scale), 256u,
                                  views[i].maxImageRectHeight);
            swci.faceCount = 1;
            swci.arraySize = 1;
            swci.mipCount = 1;
            if (failed(xrCreateSwapchain(sXR.session, &swci, &eye.handle), "xrCreateSwapchain", error))
            {
                eye.handle = XR_NULL_HANDLE;
                return error;
            }
            eye.width = (int32_t)swci.width;
            eye.height = (int32_t)swci.height;

            uint32_t image_count = 0;
            if (failed(xrEnumerateSwapchainImages(eye.handle, 0, &image_count, nullptr), "xrEnumerateSwapchainImages", error))
            {
                return error;
            }
            eye.images.assign(image_count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
            if (failed(xrEnumerateSwapchainImages(eye.handle, image_count, &image_count,
                                                  (XrSwapchainImageBaseHeader*)eye.images.data()), "xrEnumerateSwapchainImages", error))
            {
                return error;
            }
        }
        LL_INFOS("PATXR") << "Swapchains: " << sXR.eyes[0].width << "x" << sXR.eyes[0].height << " per eye (recommended "
                          << views[0].recommendedImageRectWidth << "x" << views[0].recommendedImageRectHeight
                          << ", scale " << res_scale << "), format 0x"
                          << std::hex << format << std::dec << ", " << sXR.eyes[0].images.size() << " images, blend mode "
                          << (S32)sXR.blendMode << LL_ENDL;
        if (sXR.eyes[0].width != sXR.eyes[1].width || sXR.eyes[0].height != sXR.eyes[1].height)
        {
            return "Both eyes need the same image size";
        }

        PATXRHands::init(sXR.instance, sXR.system, sXR.session, hand_ext);

        glGenFramebuffers(1, &sXR.fbo);
        PATXRUI::init();
        if (!PATXRStereo::init(sXR.eyes[0].width, sXR.eyes[0].height))
        {
            return "Not enough video memory for the eye images (try a lower PATVRResolutionScale)";
        }
        sXR.statsStart = LLTimer::getTotalSeconds();
        startStallMonitor();
        return std::string();
    }

    // Ask the runtime to end the session; the event loop finishes the job.
    void requestStop()
    {
        if (sXR.running && !sXR.stopRequested)
        {
            LL_INFOS("PATXR") << "Leaving VR mode" << LL_ENDL;
            sXR.stopRequested = true;
            sXR.stopRequestTime = LLTimer::getTotalSeconds();
            if (XR_FAILED(xrRequestExitSession(sXR.session)))
            {
                destroyAll();
            }
        }
        else if (!sXR.running)
        {
            LL_INFOS("PATXR") << "Leaving VR mode (session was not running)" << LL_ENDL;
            destroyAll();
        }
        else if (LLTimer::getTotalSeconds() - sXR.stopRequestTime > STOP_TIMEOUT_SECONDS)
        {
            LL_WARNS("PATXR") << "Runtime did not end the session in time, releasing it" << LL_ENDL;
            destroyAll();
        }
    }

    // Returns false when OpenXR was torn down while handling events.
    bool pollEvents()
    {
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        while (sXR.instance != XR_NULL_HANDLE)
        {
            event = { XR_TYPE_EVENT_DATA_BUFFER };
            XrResult res = xrPollEvent(sXR.instance, &event);
            if (res == XR_EVENT_UNAVAILABLE)
            {
                break;
            }
            if (XR_FAILED(res))
            {
                fallBackToDesktop("PatVRStopped", "xrPollEvent failed: " + resultString(res));
                return false;
            }

            switch (event.type)
            {
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                // Recentred in the headset: the head reference must be taken again.
                LL_INFOS("PATXR") << "Reference space changed (recentred)" << LL_ENDL;
                PATXRStereo::resetReference();
                PATXRUI::recentre();
                break;

            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                fallBackToDesktop("PatVRStopped", "The OpenXR runtime is shutting down (WiVRn server stopped?)");
                return false;

            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                const XrEventDataSessionStateChanged& sc = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                sXR.state = sc.state;
                LL_INFOS("PATXR") << "Session state " << (S32)sc.state << LL_ENDL;
                switch (sc.state)
                {
                case XR_SESSION_STATE_READY:
                {
                    XrSessionBeginInfo sbi{ XR_TYPE_SESSION_BEGIN_INFO };
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    res = xrBeginSession(sXR.session, &sbi);
                    if (XR_FAILED(res))
                    {
                        fallBackToDesktop("PatVRStopped", "xrBeginSession failed: " + resultString(res));
                        return false;
                    }
                    sXR.running = true;
                    LL_INFOS("PATXR") << "Session running" << LL_ENDL;
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(sXR.session);
                    sXR.running = false;
                    if (sXR.stopRequested)
                    {
                        destroyAll();
                        return false;
                    }
                    // Runtime paused us (e.g. headset disconnected): stay ready
                    // for a new READY, the desktop keeps running meanwhile.
                    LL_INFOS("PATXR") << "Session stopped by the runtime, waiting for the headset" << LL_ENDL;
                    break;
                case XR_SESSION_STATE_EXITING:
                    if (sXR.stopRequested)
                    {
                        destroyAll();
                    }
                    else
                    {
                        fallBackToDesktop("PatVRStopped", "The OpenXR runtime ended the VR session");
                    }
                    return false;
                case XR_SESSION_STATE_LOSS_PENDING:
                    fallBackToDesktop("PatVRStopped", "Connection to the headset was lost");
                    return false;
                default:
                    break;
                }
                break;
            }
            default:
                break;
            }
        }
        return sXR.instance != XR_NULL_HANDLE;
    }

    // No world image (before login, window minimized, ...): show the headset
    // camera everywhere (alpha 0), so the room and the monitor stay visible.
    void drawPassthrough(const EyeSwapchain& eye, GLuint texture)
    {
        GLint old_fbo = 0;
        GLint old_viewport[4] = { 0 };
        GLfloat old_clear[4] = { 0.f };
        GLboolean old_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
        glGetIntegerv(GL_VIEWPORT, old_viewport);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
        glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
        const GLboolean old_scissor_test = glIsEnabled(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sXR.fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        glViewport(0, 0, eye.width, eye.height);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);
        glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
        if (old_scissor_test)
        {
            glEnable(GL_SCISSOR_TEST);
        }
        glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
        glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    }

    // xrWaitFrame + xrBeginFrame. Called at the end of a main-loop iteration, so
    // the headset paces the viewer; display() of the next iteration renders it.
    bool beginFrame()
    {
        XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
        const F64 wait_start = LLTimer::getTotalSeconds();
        XrResult res;
        {
            RuntimeCall call("xrWaitFrame");
            res = xrWaitFrame(sXR.session, &fwi, &frame_state);
        }
        // WiVRn does not pace frames that are not shown (headset off the face,
        // session SYNCHRONIZED): xrWaitFrame returned at ~170/s. Keep the pace.
        if (XR_SUCCEEDED(res) && !frame_state.shouldRender && frame_state.predictedDisplayPeriod > 0)
        {
            const F64 period = frame_state.predictedDisplayPeriod / 1.0e9;
            const F64 left = period - (LLTimer::getTotalSeconds() - wait_start);
            if (left > 0.001)
            {
                ms_sleep((U32)(left * 1000.0));
            }
        }
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrWaitFrame failed: " + resultString(res));
            return false;
        }
        XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
        res = xrBeginFrame(sXR.session, &fbi);
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrBeginFrame failed: " + resultString(res));
            return false;
        }
        if (sXR.lastDisplayTime && frame_state.predictedDisplayPeriod > 0 &&
            frame_state.predictedDisplayTime - sXR.lastDisplayTime > frame_state.predictedDisplayPeriod * 3 / 2)
        {
            sXR.statsMissed += (U32)((frame_state.predictedDisplayTime - sXR.lastDisplayTime) /
                                     frame_state.predictedDisplayPeriod) - 1;
        }
        sXR.lastDisplayTime = frame_state.predictedDisplayTime;
        sXR.frameState = frame_state;
        sXR.frameOpen = true;
        sXR.viewsLocated = false;
        sXR.viewsValid = false;
        sXR.eyesActive = false;
        sXR.eyesRendered = false;
        return true;
    }

    // Eye poses for the predicted display time, as late as possible (right
    // before the world is rendered). Also feeds hand tracking and the keyboard window.
    void locateViews()
    {
        sXR.viewsLocated = true;
        sXR.viewsValid = false;
        XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = sXR.frameState.predictedDisplayTime;
        vli.space = sXR.space;
        XrViewState view_state{ XR_TYPE_VIEW_STATE };
        sXR.views[0] = { XR_TYPE_VIEW };
        sXR.views[1] = { XR_TYPE_VIEW };
        uint32_t view_count = 0;
        XrResult res;
        {
            RuntimeCall call("xrLocateViews");
            res = xrLocateViews(sXR.session, &vli, &view_state, 2, &view_count, sXR.views);
        }
        sXR.viewsValid = XR_SUCCEEDED(res) && view_count == 2 &&
                         (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (sXR.viewsValid)
        {
            PATXRHands::locate(sXR.space, sXR.frameState.predictedDisplayTime, sXR.palms);
            PATXRKeyboard::update(sXR.lastDt, sXR.views[0].pose, sXR.views[1].pose, sXR.palms);
            PATXRUI::update(sXR.lastDt, sXR.views[0].pose, sXR.views[1].pose);
        }
    }

    // World (projection layer) and UI panel (quad layer, phase 8), both optional.
    bool endFrame(const XrCompositionLayerProjectionView* proj_views, XrTime display_time,
                  const XrCompositionLayerQuad* quad = nullptr)
    {
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        const XrCompositionLayerBaseHeader* layers[2] = { nullptr, nullptr };
        uint32_t layer_count = 0;
        if (proj_views)
        {
            layer.space = sXR.space;
            layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            layer.viewCount = 2;
            layer.views = proj_views;
            layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
        }
        if (quad)
        {
            layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(quad);
        }
        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = display_time;
        fei.environmentBlendMode = sXR.blendMode;
        fei.layerCount = layer_count;
        fei.layers = layer_count ? layers : nullptr;
        XrResult res;
        {
            RuntimeCall call("xrEndFrame");
            res = xrEndFrame(sXR.session, &fei);
        }
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrEndFrame failed: " + resultString(res));
            return false;
        }
        if (sXR.frames++ == 0)
        {
            LL_INFOS("PATXR") << "First frame submitted" << LL_ENDL;
        }
        return true;
    }

    void logStats()
    {
        const F64 now = LLTimer::getTotalSeconds();
        const F64 span = now - sXR.statsStart;
        if (span < STATS_INTERVAL_SECONDS)
        {
            return;
        }
        const F64 period_ms = sXR.frameState.predictedDisplayPeriod / 1.0e6;
        LL_INFOS("PATXR") << llformat("Stats: headset %.1f Hz, submitted %.1f/s, world rendered %.1f/s, repeated %.1f/s, "
                                      "missed %u, eye render avg %.1f ms max %.1f ms (CPU, both eyes)",
                                      period_ms > 0.0 ? 1000.0 / period_ms : 0.0,
                                      sXR.statsSubmitted / span, sXR.statsRendered / span, sXR.statsRepeated / span,
                                      sXR.statsMissed,
                                      sXR.statsRendered ? sXR.statsEyeSeconds * 1000.0 / sXR.statsRendered : 0.0,
                                      sXR.statsEyeMax * 1000.0) << LL_ENDL;
        sXR.statsStart = now;
        sXR.statsSubmitted = sXR.statsRendered = sXR.statsRepeated = sXR.statsMissed = 0;
        sXR.statsEyeSeconds = sXR.statsEyeMax = 0.0;
    }

    // Acquire + wait for a swapchain image. 1 = image ready (index), 0 = the
    // runtime still holds it (try again next frame), -1 = failed (VR left).
    S32 acquireImage(EyeSwapchain& sc, uint32_t& index)
    {
        // An image acquired earlier whose wait timed out is waited for again
        // instead of acquiring another one.
        if (sc.pending < 0)
        {
            uint32_t acquired = 0;
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            XrResult res;
            {
                RuntimeCall call("xrAcquireSwapchainImage");
                res = xrAcquireSwapchainImage(sc.handle, &ai, &acquired);
            }
            if (XR_FAILED(res))
            {
                fallBackToDesktop("PatVRStopped", "xrAcquireSwapchainImage failed: " + resultString(res));
                return -1;
            }
            sc.pending = (S32)acquired;
        }
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = SWAPCHAIN_WAIT_TIMEOUT;
        XrResult res;
        {
            RuntimeCall call("xrWaitSwapchainImage");
            res = xrWaitSwapchainImage(sc.handle, &wi);
        }
        if (res == XR_TIMEOUT_EXPIRED)
        {
            if (sXR.swapchainTimeouts++ % 50 == 0)
            {
                LL_WARNS("PATXR") << "Swapchain image not available in time (" << sXR.swapchainTimeouts << "x)" << LL_ENDL;
            }
            return 0;
        }
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrWaitSwapchainImage failed: " + resultString(res));
            return -1;
        }
        index = (uint32_t)sc.pending;
        sc.pending = -1;
        return 1;
    }

    void releaseImage(EyeSwapchain& sc)
    {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(sc.handle, &ri);
    }

    // Quad-layer swapchain of the UI panel's size (recreated when the window size changes).
    bool ensureUISwapchain(S32 width, S32 height)
    {
        EyeSwapchain& ui = sXR.ui;
        if (ui.handle != XR_NULL_HANDLE && ui.width == width && ui.height == height)
        {
            return true;
        }
        if (ui.handle != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(ui.handle);
        }
        ui = EyeSwapchain();
        sXR.lastQuadValid = false;
        XrSwapchainCreateInfo swci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swci.format = sXR.format;
        swci.sampleCount = 1;
        swci.width = (uint32_t)width;
        swci.height = (uint32_t)height;
        swci.faceCount = 1;
        swci.arraySize = 1;
        swci.mipCount = 1;
        XrResult res = xrCreateSwapchain(sXR.session, &swci, &ui.handle);
        if (XR_FAILED(res))
        {
            LL_WARNS("PATXR") << "UI panel swapchain " << width << "x" << height << " failed: " << resultString(res) << LL_ENDL;
            ui.handle = XR_NULL_HANDLE;
            return false;
        }
        ui.width = width;
        ui.height = height;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(ui.handle, 0, &count, nullptr);
        ui.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
        if (XR_FAILED(xrEnumerateSwapchainImages(ui.handle, count, &count, (XrSwapchainImageBaseHeader*)ui.images.data())))
        {
            xrDestroySwapchain(ui.handle);
            ui = EyeSwapchain();
            return false;
        }
        LL_INFOS("PATXR") << "UI panel quad layer swapchain " << width << "x" << height << ", " << count << " images" << LL_ENDL;
        return true;
    }

    // Submit the frame begun at the end of the previous iteration: the world
    // image from display() if there is one, otherwise passthrough; the UI panel
    // on top (quad layer, or drawn into both eye images).
    void submitFrame()
    {
        if (!sXR.frameOpen)
        {
            return;
        }
        sXR.frameOpen = false;
        if (sXR.eyesActive)
        {
            PATXRStereo::abort();
            sXR.eyesActive = false;
        }

        XrCompositionLayerProjectionView proj_views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                           { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        bool have_layer = false;
        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        bool have_quad = false;
        if (sXR.frameState.shouldRender)
        {
            if (!sXR.viewsLocated)
            {
                locateViews();
            }
            if (sXR.viewsValid)
            {
                const bool ui = PATXRUI::hasImage();
                const bool ui_quad = ui && PATXRUI::useQuadLayer();
                if (ui)
                {
                    PATXRUI::prepare();
                }

                have_layer = true;
                for (U32 i = 0; i < 2 && have_layer; ++i)
                {
                    EyeSwapchain& eye = sXR.eyes[i];
                    uint32_t index = 0;
                    const S32 got = acquireImage(eye, index);
                    if (got < 0)
                    {
                        return;
                    }
                    if (got == 0)
                    {
                        have_layer = false; // runtime still holds the image: nothing new this frame
                        break;
                    }

                    if (sXR.eyesRendered)
                    {
                        PATXRStereo::copyToSwapchain(i, eye.images[index].image, eye.width, eye.height);
                    }
                    else
                    {
                        drawPassthrough(eye, eye.images[index].image);
                    }
                    if (ui && !ui_quad)
                    {
                        PATXRUI::drawInEye(sXR.views[i], eye.images[index].image, eye.width, eye.height);
                    }
                    releaseImage(eye);

                    // The pose the image was rendered with: the compositor reprojects
                    // from it to the real head pose at display time.
                    proj_views[i].pose = sXR.views[i].pose;
                    proj_views[i].fov = sXR.views[i].fov;
                    proj_views[i].subImage.swapchain = eye.handle;
                    proj_views[i].subImage.imageRect.offset = { 0, 0 };
                    proj_views[i].subImage.imageRect.extent = { eye.width, eye.height };
                    proj_views[i].subImage.imageArrayIndex = 0;
                }

                if (ui_quad && ensureUISwapchain(PATXRUI::imageWidth(), PATXRUI::imageHeight()))
                {
                    uint32_t index = 0;
                    const S32 got = acquireImage(sXR.ui, index);
                    if (got < 0)
                    {
                        return;
                    }
                    if (got > 0)
                    {
                        PATXRUI::copyToSwapchain(sXR.ui.images[index].image, sXR.ui.width, sXR.ui.height);
                        releaseImage(sXR.ui);
                        PATXRUI::fillQuadLayer(quad, sXR.space, sXR.ui.handle);
                        have_quad = true;
                    }
                }
            }
        }

        if (!endFrame(have_layer ? proj_views : nullptr, sXR.frameState.predictedDisplayTime, have_quad ? &quad : nullptr))
        {
            return;
        }
        ++sXR.statsSubmitted;
        if (sXR.eyesRendered)
        {
            ++sXR.statsRendered;
        }
        sXR.lastLayerValid = have_layer;
        if (have_layer)
        {
            sXR.lastViews[0] = proj_views[0];
            sXR.lastViews[1] = proj_views[1];
        }
        sXR.lastQuadValid = have_quad;
        if (have_quad)
        {
            sXR.lastQuad = quad;
        }
        logStats();
    }

    // Half-rate mode: one extra headset frame that shows the last image again
    // (old pose, the compositor reprojects it), so the viewer renders only every
    // second headset frame with an even pace.
    bool repeatFrame()
    {
        if (!beginFrame())
        {
            return false;
        }
        sXR.frameOpen = false;
        const bool layer = sXR.lastLayerValid && sXR.frameState.shouldRender;
        const bool quad = sXR.lastQuadValid && sXR.frameState.shouldRender;
        if (!endFrame(layer ? sXR.lastViews : nullptr, sXR.frameState.predictedDisplayTime, quad ? &sXR.lastQuad : nullptr))
        {
            return false;
        }
        ++sXR.statsSubmitted;
        ++sXR.statsRepeated;
        return true;
    }
}

namespace
{
    void updateUIState(bool want)
    {
        const bool active = want && sXR.running && LLStartUp::getStartupState() >= STATE_STARTED;
        if (active != sXR.uiActive)
        {
            sXR.uiActive = active;
            PATXRUI::setActive(active);
            static LLCachedControl<bool> confine(gSavedSettings, "PATVRConfineMouse", true);
            PATXRPlatform::confineMouse(active && confine);
            sXR.uiScale = PATXRUI::uiScale();
            if (gViewerWindow)
            {
                gViewerWindow->requestResolutionUpdate(); // UI scale of the panel
            }
            LL_INFOS("PATXR") << "UI scale " << sXR.uiScale << (PATXRUI::useQuadLayer() ? ", panel as quad layer" : ", panel drawn into the eye images") << LL_ENDL;
        }
        else if (active && PATXRUI::uiScale() != sXR.uiScale)
        {
            sXR.uiScale = PATXRUI::uiScale();
            if (gViewerWindow)
            {
                gViewerWindow->requestResolutionUpdate();
            }
        }
    }

    // Ctrl+Shift+V and "hold Escape" read the raw key state, so they work with
    // the keyboard focus anywhere (chat bar, text fields, mouselook). The menu
    // accelerator for Ctrl+Shift+V only fires when no widget takes the key
    // first; if it did fire this frame, the setting has already changed and we
    // do not toggle a second time.
    void handleKeys()
    {
        static bool v_was_down = false;
        static bool escape_done = false;
        static bool last_enabled = false;
        const bool enabled = gSavedSettings.getBOOL(SETTING_ENABLED);
        if (!gKeyboard)
        {
            last_enabled = enabled;
            return;
        }
        const MASK mask = gKeyboard->currentMask(false);
        const bool v_down = gKeyboard->getKeyDown('V') &&
                            (mask & (MASK_CONTROL | MASK_SHIFT | MASK_ALT)) == (MASK_CONTROL | MASK_SHIFT);
        if (v_down && !v_was_down && enabled == last_enabled)
        {
            LL_INFOS("PATXR") << "Ctrl+Shift+V: VR mode " << (enabled ? "off" : "on") << LL_ENDL;
            gSavedSettings.setBOOL(SETTING_ENABLED, !enabled);
        }
        v_was_down = v_down;

        if (gKeyboard->getKeyDown(KEY_ESCAPE))
        {
            if (!escape_done && gKeyboard->getKeyElapsedTime(KEY_ESCAPE) >= ESCAPE_HOLD_SECONDS &&
                gSavedSettings.getBOOL(SETTING_ENABLED))
            {
                LL_INFOS("PATXR") << "Escape held: leaving VR mode" << LL_ENDL;
                gSavedSettings.setBOOL(SETTING_ENABLED, false);
                escape_done = true;
            }
        }
        else
        {
            escape_done = false;
        }
        last_enabled = gSavedSettings.getBOOL(SETTING_ENABLED);
    }
}

void PATXRSession::frame()
{
    if (!gGLManager.mInited)
    {
        return;
    }

    static LLCachedControl<bool> vr_enabled(gSavedSettings, SETTING_ENABLED, false);
    handleKeys();
    const bool want = vr_enabled;

    if (sPending)
    {
        // Connection attempt in progress on the worker thread.
        std::unique_lock<std::mutex> lock(sPending->mutex);
        if (!sPending->done)
        {
            sPending->abandoned = !want;
            if (!want)
            {
                LL_INFOS("PATXR") << "VR mode switched off while waiting for the runtime" << LL_ENDL;
                lock.unlock();
                sPending.reset();
                return;
            }
            if (!sWaitNotified && LLTimer::getTotalSeconds() - sPendingSince > 2.0)
            {
                sWaitNotified = true;
                LL_INFOS("PATXR") << "Waiting for the runtime / headset" << LL_ENDL;
                lock.unlock();
                LLNotificationsUtil::add("PatVRWaiting");
            }
            return;
        }
        auto pending = sPending;
        sPending.reset();
        lock.unlock();
        if (!pending->error.empty())
        {
            if (want)
            {
                fallBackToDesktop("PatVRUnavailable", pending->error);
            }
            return;
        }
        if (!want)
        {
            xrDestroyInstance(pending->instance);
            return;
        }
        sXR.instance = pending->instance;
        sXR.system = pending->system;
        sXR.handExt = pending->handExt;
        std::string error = start();
        if (!error.empty())
        {
            fallBackToDesktop("PatVRUnavailable", error);
            return;
        }
    }
    else if (want && sXR.instance == XR_NULL_HANDLE)
    {
        LL_INFOS("PATXR") << "Entering VR mode" << LL_ENDL;
        startConnect();
        return;
    }

    if (sXR.instance == XR_NULL_HANDLE)
    {
        return;
    }

    // 1. Finish the frame rendered by display() in this iteration.
    submitFrame();
    if (sXR.instance == XR_NULL_HANDLE)
    {
        return;
    }

    if (!want)
    {
        requestStop();
        if (sXR.instance == XR_NULL_HANDLE)
        {
            return;
        }
    }

    // Phase 8: the UI goes onto the panel while the session runs in the world;
    // between frames the camera follows the panel (tools' screen-space logic).
    updateUIState(want);
    {
        XrVector3f origin, at, up;
        F32 vfov = 0.f, aspect = 1.f;
        if (sXR.uiActive && sXR.eyesRendered && PATXRUI::pointerCamera(origin, at, up, vfov, aspect))
        {
            PATXRStereo::setPointerCamera(origin, at, up, vfov, aspect);
        }
    }

    // 2. Session state changes (only between frames).
    if (!pollEvents())
    {
        return;
    }

    // 3. Wait for the headset and begin the next frame.
    if (sXR.running)
    {
        const F64 now = LLTimer::getTotalSeconds();
        const F32 dt = sXR.lastFrameTime > 0.0 ? (F32)(now - sXR.lastFrameTime) : 0.f;
        sXR.lastFrameTime = now;
        sXR.lastDt = dt;
        static LLCachedControl<bool> half_rate(gSavedSettings, SETTING_HALF_RATE, false);
        if (half_rate && sXR.lastLayerValid && !repeatFrame())
        {
            return;
        }
        beginFrame();
    }
}

void PATXRSession::beginEyes()
{
    if (!sXR.running || !sXR.frameOpen || !sXR.frameState.shouldRender || sXR.eyesRendered || sXR.eyesActive)
    {
        return;
    }
    locateViews();
    if (!sXR.viewsValid)
    {
        return;
    }
    sXR.eyesStart = LLTimer::getTotalSeconds();
    sXR.eyesActive = PATXRStereo::beginEyes(sXR.views);
}

void PATXRSession::renderEyes()
{
    if (!sXR.eyesActive)
    {
        return;
    }
    PATXRStereo::renderEyes(sXR.views, sXR.palms);
    sXR.eyesActive = false;
    sXR.eyesRendered = true;
    const F64 spent = LLTimer::getTotalSeconds() - sXR.eyesStart;
    sXR.statsEyeSeconds += spent;
    sXR.statsEyeMax = llmax(sXR.statsEyeMax, spent);
}

bool PATXRSession::drawDesktopMirror()
{
    return PATXRStereo::drawDesktopMirror();
}

bool PATXRSession::inEyes()
{
    return PATXRStereo::inEyes();
}

bool PATXRSession::pixelVectors(const LLVector3& pos_agent, LLVector3& up, LLVector3& right)
{
    return PATXRStereo::pixelVectors(pos_agent, up, right);
}

bool PATXRSession::drawHudText(const LLWString& text, const LLVector3& pos_agent, const LLVector3& right_axis,
                               const LLVector3& up_axis, const LLFontGL& font, U8 style, S32 shadow, const LLColor4& color)
{
    return PATXRStereo::drawHudText(text, pos_agent, right_axis, up_axis, font, style, shadow, color);
}

void PATXRSession::beforeDisplay()
{
    PATXRStereo::restoreCamera();
}

void PATXRSession::beginUI()
{
    PATXRUI::beginCapture();
}

void PATXRSession::endUI()
{
    PATXRUI::endCapture();
}

bool PATXRSession::uiOnPanel()
{
    return PATXRUI::capturing();
}

bool PATXRSession::mouseDirection(S32 x, S32 y, LLVector3& dir)
{
    if (!sXR.uiActive || !gViewerWindow)
    {
        return false;
    }
    const F32 u = (F32)x / (F32)llmax(1, gViewerWindow->getWindowWidthScaled());
    const F32 v = (F32)y / (F32)llmax(1, gViewerWindow->getWindowHeightScaled());
    XrVector3f origin, d;
    return PATXRUI::rayRoom(u, v, origin, d) && PATXRStereo::roomDirToAgent(d, dir);
}

void PATXRSession::mouseRayStart(LLVector3& origin, LLVector3& near_normal, const LLVector3& dir)
{
    if (!sXR.uiActive)
    {
        return;
    }
    XrVector3f o, d;
    LLVector3 eye;
    if (PATXRUI::rayRoom(0.5f, 0.5f, o, d) && PATXRStereo::roomPointToAgent(o, eye))
    {
        origin = eye;
        near_normal = dir;
    }
}

void PATXRSession::mouseRayOriginGlobal(LLVector3d& origin)
{
    if (!sXR.uiActive)
    {
        return;
    }
    XrVector3f o, d;
    LLVector3 eye;
    if (PATXRUI::rayRoom(0.5f, 0.5f, o, d) && PATXRStereo::roomPointToAgent(o, eye))
    {
        origin = gAgent.getPosGlobalFromAgent(eye);
    }
}

F32 PATXRSession::uiScale()
{
    return PATXRUI::uiScale();
}

void PATXRSession::shutdown()
{
    sMonitorRunning = false;
    if (sPending)
    {
        // A worker may still wait for the runtime: let it clean up on its own.
        std::lock_guard<std::mutex> lock(sPending->mutex);
        sPending->abandoned = true;
    }
    sPending.reset();
    if (sXR.instance != XR_NULL_HANDLE)
    {
        LL_INFOS("PATXR") << "Viewer shutdown, releasing OpenXR" << LL_ENDL;
        if (sXR.running)
        {
            xrEndSession(sXR.session); // may fail outside STOPPING; destroy follows anyway
        }
        destroyAll(true);
    }
}

bool PATXRSession::isRunning()
{
    return sXR.running;
}

#else // !PAT_XR: VR mode not built for this platform

void PATXRSession::frame()
{
}

void PATXRSession::shutdown()
{
}

bool PATXRSession::isRunning()
{
    return false;
}

void PATXRSession::beginEyes()
{
}

void PATXRSession::renderEyes()
{
}

bool PATXRSession::drawDesktopMirror()
{
    return false;
}

bool PATXRSession::inEyes()
{
    return false;
}

bool PATXRSession::pixelVectors(const LLVector3&, LLVector3&, LLVector3&)
{
    return false;
}

bool PATXRSession::drawHudText(const LLWString&, const LLVector3&, const LLVector3&, const LLVector3&,
                               const LLFontGL&, U8, S32, const LLColor4&)
{
    return false;
}

void PATXRSession::beforeDisplay()
{
}

void PATXRSession::beginUI()
{
}

void PATXRSession::endUI()
{
}

bool PATXRSession::uiOnPanel()
{
    return false;
}

bool PATXRSession::mouseDirection(S32, S32, LLVector3&)
{
    return false;
}

void PATXRSession::mouseRayStart(LLVector3&, LLVector3&, const LLVector3&)
{
}

void PATXRSession::mouseRayOriginGlobal(LLVector3d&)
{
}

F32 PATXRSession::uiScale()
{
    return 1.f;
}

#endif // PAT_XR
