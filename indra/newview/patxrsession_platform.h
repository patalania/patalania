/**
 * @file patxrsession_platform.h
 * @brief <PAT:XR> Platform part of the OpenXR session: graphics binding only.
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

#ifndef PAT_XRSESSION_PLATFORM_H
#define PAT_XRSESSION_PLATFORM_H

#include <string>

// One implementation per platform:
//   patxrsession_linux.cpp  -> XrGraphicsBindingOpenGLXlibKHR (GLX)
//   patxrsession_win.cpp    -> XrGraphicsBindingOpenGLWin32KHR (WGL, later)
namespace PATXRPlatform
{
    // Called before the first OpenXR call: lets the loader find a runtime even
    // when no active runtime is registered (Linux: WiVRn Flatpak fallback).
    void prepareRuntime();

    // Name of the OpenXR graphics extension this platform needs.
    const char* graphicsExtensionName();

    // Builds the graphics binding for the GL context that is current on this
    // thread. Returns a pointer for XrSessionCreateInfo::next (valid until the
    // next call), or nullptr with a readable reason in 'error'.
    const void* graphicsBinding(std::string& error);
}

#endif // PAT_XRSESSION_PLATFORM_H
