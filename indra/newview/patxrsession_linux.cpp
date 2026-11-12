/**
 * @file patxrsession_linux.cpp
 * @brief <PAT:XR> Linux graphics binding (SDL2 window, GLX context) for OpenXR.
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

#include "patxrsession_platform.h"

#if PAT_XR

#include "llgl.h" // GL + GLX (X11) headers on Linux

#include <SDL2/SDL.h>

#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace
{
    XrGraphicsBindingOpenGLXlibKHR sBinding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
}

void PATXRPlatform::prepareRuntime()
{
    // Respect an explicit choice or a registered active runtime.
    if (getenv("XR_RUNTIME_JSON"))
    {
        return;
    }
    const char* home = getenv("HOME");
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    std::string config = xdg_config && *xdg_config ? std::string(xdg_config) : std::string(home ? home : "") + "/.config";
    const std::string registered[] = { config + "/openxr/1/active_runtime.json", "/etc/xdg/openxr/1/active_runtime.json",
                                       "/etc/openxr/1/active_runtime.json" };
    for (const std::string& path : registered)
    {
        if (LLFile::isfile(path))
        {
            return;
        }
    }
    // WiVRn removes its active_runtime.json link when the server stops and may not
    // restore it; use the Flatpak runtime directly (system or user installation).
    const std::string candidates[] = {
        "/var/lib/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json",
        std::string(home ? home : "") + "/.local/share/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json" };
    for (const std::string& path : candidates)
    {
        if (LLFile::isfile(path))
        {
            setenv("XR_RUNTIME_JSON", path.c_str(), 0);
            LL_INFOS("PATXR") << "No active OpenXR runtime registered, using " << path << LL_ENDL;
            return;
        }
    }
}

const char* PATXRPlatform::graphicsExtensionName()
{
    return XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
}

const void* PATXRPlatform::graphicsBinding(std::string& error)
{
    // SDL2 creates a GLX context on X11; it is current on the main thread.
    Display* display = glXGetCurrentDisplay();
    GLXContext context = glXGetCurrentContext();
    GLXDrawable drawable = glXGetCurrentDrawable();
    if (!display || !context || !drawable)
    {
        error = "No current GLX context (only X11/GLX is supported, not EGL/Wayland)";
        return nullptr;
    }

    int fbconfig_id = 0;
    int screen = DefaultScreen(display);
    glXQueryContext(display, context, GLX_FBCONFIG_ID, &fbconfig_id);
    glXQueryContext(display, context, GLX_SCREEN, &screen);

    GLXFBConfig fbconfig = nullptr;
    uint32_t visual_id = 0;
    if (fbconfig_id)
    {
        int attribs[] = { GLX_FBCONFIG_ID, fbconfig_id, None };
        int count = 0;
        GLXFBConfig* configs = glXChooseFBConfig(display, screen, attribs, &count);
        if (configs && count > 0)
        {
            fbconfig = configs[0];
            if (XVisualInfo* vi = glXGetVisualFromFBConfig(display, fbconfig))
            {
                visual_id = (uint32_t)vi->visualid;
                XFree(vi);
            }
        }
        if (configs)
        {
            XFree(configs);
        }
    }

    sBinding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
    sBinding.xDisplay = display;
    sBinding.visualid = visual_id;
    sBinding.glxFBConfig = fbconfig;
    sBinding.glxDrawable = drawable;
    sBinding.glxContext = context;

    LL_INFOS("PATXR") << "GLX binding: screen " << screen << ", fbconfig 0x" << std::hex << fbconfig_id
                      << ", visual 0x" << visual_id << std::dec << LL_ENDL;
    return &sBinding;
}

void PATXRPlatform::confineMouse(bool confine)
{
    // The viewer's SDL window owns the GL context on the main thread.
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (window)
    {
        SDL_SetWindowGrab(window, confine ? SDL_TRUE : SDL_FALSE);
        LL_INFOS("PATXR") << "Mouse " << (confine ? "kept inside the window" : "free again") << LL_ENDL;
    }
}

#endif // PAT_XR
