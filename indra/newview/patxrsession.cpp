/**
 * @file patxrsession.cpp
 * @brief <PAT:XR> OpenXR session for the Patalania VR mode (platform neutral).
 *
 * Phase 6 (skeleton): instance, system, session, LOCAL space, one swapchain per
 * eye, event loop and a test image (left eye red, right eye blue, white square
 * in the centre). Stereo rendering of the world follows in phase 7.
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

#include "llgl.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr const char* SETTING_ENABLED = "PATVREnabled";
    constexpr F64 STOP_TIMEOUT_SECONDS = 3.0;

    struct EyeSwapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLKHR> images;
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
        GLuint fbo = 0;
        bool running = false;       // between xrBeginSession and xrEndSession
        bool stopRequested = false; // we asked the runtime to end the session
        F64 stopRequestTime = 0.0;
        U64 frames = 0;
        bool handExt = false;
        F64 lastFrameTime = 0.0;
        std::vector<XrVector3f> palms;
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
    void destroyAll()
    {
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

        // 8. One swapchain per eye at the recommended size.
        int64_t format = chooseSwapchainFormat(error);
        if (!format)
        {
            return error;
        }
        for (uint32_t i = 0; i < 2; ++i)
        {
            EyeSwapchain& eye = sXR.eyes[i];
            XrSwapchainCreateInfo swci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            swci.format = format;
            swci.sampleCount = 1;
            swci.width = views[i].recommendedImageRectWidth;
            swci.height = views[i].recommendedImageRectHeight;
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
        LL_INFOS("PATXR") << "Swapchains: " << sXR.eyes[0].width << "x" << sXR.eyes[0].height << " per eye, format 0x"
                          << std::hex << format << std::dec << ", " << sXR.eyes[0].images.size() << " images, blend mode "
                          << (S32)sXR.blendMode << LL_ENDL;

        PATXRHands::init(sXR.instance, sXR.system, sXR.session, hand_ext);

        glGenFramebuffers(1, &sXR.fbo);
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

    // Phase 6 test image: one colour per eye, white square in the centre.
    void drawTestImage(const EyeSwapchain& eye, GLuint texture, U32 eye_index, const XrView& view)
    {
        GLint old_fbo = 0;
        GLint old_viewport[4] = { 0 };
        GLint old_scissor[4] = { 0 };
        GLfloat old_clear[4] = { 0.f };
        GLboolean old_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
        glGetIntegerv(GL_VIEWPORT, old_viewport);
        glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
        glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
        const GLboolean old_scissor_test = glIsEnabled(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sXR.fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (sXR.frames == 0 && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            LL_WARNS("PATXR") << "Swapchain framebuffer incomplete" << LL_ENDL;
        }

        glViewport(0, 0, eye.width, eye.height);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
        if (eye_index == 0)
        {
            glClearColor(0.45f, 0.f, 0.f, 1.f); // left: red
        }
        else
        {
            glClearColor(0.f, 0.f, 0.45f, 1.f); // right: blue
        }
        glClear(GL_COLOR_BUFFER_BIT);

        const GLsizei size = llmax(eye.width, eye.height) / 10;
        glEnable(GL_SCISSOR_TEST);
        glScissor((eye.width - size) / 2, (eye.height - size) / 2, size, size);
        glClearColor(1.f, 1.f, 1.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);

        // Phase 6b: transparent keyboard window (passthrough) in the LOCAL space.
        PATXRKeyboard::draw(view, sXR.palms);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);
        glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
        glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
        if (!old_scissor_test)
        {
            glDisable(GL_SCISSOR_TEST);
        }
        glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
        glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    }

    void renderFrame()
    {
        XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
        XrResult res = xrWaitFrame(sXR.session, &fwi, &frame_state);
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrWaitFrame failed: " + resultString(res));
            return;
        }
        XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
        res = xrBeginFrame(sXR.session, &fbi);
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrBeginFrame failed: " + resultString(res));
            return;
        }

        XrCompositionLayerProjectionView proj_views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                           { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        const XrCompositionLayerBaseHeader* layers[1] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
        uint32_t layer_count = 0;

        if (frame_state.shouldRender)
        {
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = frame_state.predictedDisplayTime;
            vli.space = sXR.space;
            XrViewState view_state{ XR_TYPE_VIEW_STATE };
            XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            uint32_t view_count = 0;
            res = xrLocateViews(sXR.session, &vli, &view_state, 2, &view_count, views);
            const bool views_valid = XR_SUCCEEDED(res) && view_count == 2 &&
                                     (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
            if (views_valid)
            {
                PATXRHands::locate(sXR.space, frame_state.predictedDisplayTime, sXR.palms);
                for (U32 i = 0; i < 2; ++i)
                {
                    EyeSwapchain& eye = sXR.eyes[i];
                    uint32_t index = 0;
                    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                    res = xrAcquireSwapchainImage(eye.handle, &ai, &index);
                    if (XR_FAILED(res))
                    {
                        fallBackToDesktop("PatVRStopped", "xrAcquireSwapchainImage failed: " + resultString(res));
                        return;
                    }
                    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                    wi.timeout = XR_INFINITE_DURATION;
                    res = xrWaitSwapchainImage(eye.handle, &wi);
                    if (XR_FAILED(res))
                    {
                        fallBackToDesktop("PatVRStopped", "xrWaitSwapchainImage failed: " + resultString(res));
                        return;
                    }

                    drawTestImage(eye, eye.images[index].image, i, views[i]);

                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(eye.handle, &ri);

                    proj_views[i].pose = views[i].pose;
                    proj_views[i].fov = views[i].fov;
                    proj_views[i].subImage.swapchain = eye.handle;
                    proj_views[i].subImage.imageRect.offset = { 0, 0 };
                    proj_views[i].subImage.imageRect.extent = { eye.width, eye.height };
                    proj_views[i].subImage.imageArrayIndex = 0;
                }
                layer.space = sXR.space;
                layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                   XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                layer.viewCount = 2;
                layer.views = proj_views;
                layer_count = 1;
            }
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = frame_state.predictedDisplayTime;
        fei.environmentBlendMode = sXR.blendMode;
        fei.layerCount = layer_count;
        fei.layers = layer_count ? layers : nullptr;
        res = xrEndFrame(sXR.session, &fei);
        if (XR_FAILED(res))
        {
            fallBackToDesktop("PatVRStopped", "xrEndFrame failed: " + resultString(res));
            return;
        }
        if (sXR.frames++ == 0)
        {
            LL_INFOS("PATXR") << "First frame submitted" << LL_ENDL;
        }
    }
}

void PATXRSession::frame()
{
    if (!gGLManager.mInited)
    {
        return;
    }

    static LLCachedControl<bool> vr_enabled(gSavedSettings, SETTING_ENABLED, false);
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

    if (!want)
    {
        requestStop();
        if (sXR.instance == XR_NULL_HANDLE)
        {
            return;
        }
    }

    if (!pollEvents())
    {
        return;
    }

    if (sXR.running)
    {
        const F64 now = LLTimer::getTotalSeconds();
        const F32 dt = sXR.lastFrameTime > 0.0 ? (F32)(now - sXR.lastFrameTime) : 0.f;
        sXR.lastFrameTime = now;
        PATXRKeyboard::update(dt);
        renderFrame();
    }
}

void PATXRSession::shutdown()
{
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
        destroyAll();
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

#endif // PAT_XR
