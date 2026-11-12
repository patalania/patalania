/**
 * @file patxrhands.cpp
 * @brief <PAT:XR> Hand tracking (XR_EXT_hand_tracking), platform neutral.
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

#include "patxrhands.h"

namespace
{
    PFN_xrCreateHandTrackerEXT sCreate = nullptr;
    PFN_xrDestroyHandTrackerEXT sDestroy = nullptr;
    PFN_xrLocateHandJointsEXT sLocate = nullptr;
    XrHandTrackerEXT sTrackers[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    F64 sLastLog = 0.0;
    bool sLastActive[2] = { false, false };
    U32 sActiveFrames[2] = { 0, 0 };
}

void PATXRHands::init(XrInstance instance, XrSystemId system, XrSession session, bool extension_enabled)
{
    release();
    if (!extension_enabled)
    {
        LL_INFOS("PATXR") << "Hand tracking: extension " << XR_EXT_HAND_TRACKING_EXTENSION_NAME << " not offered by the runtime" << LL_ENDL;
        return;
    }
    XrSystemHandTrackingPropertiesEXT ht{ XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
    XrSystemProperties props{ XR_TYPE_SYSTEM_PROPERTIES };
    props.next = &ht;
    if (XR_FAILED(xrGetSystemProperties(instance, system, &props)) || !ht.supportsHandTracking)
    {
        LL_INFOS("PATXR") << "Hand tracking: not supported by this system" << LL_ENDL;
        return;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&sCreate)) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)&sDestroy)) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&sLocate)))
    {
        LL_WARNS("PATXR") << "Hand tracking: functions missing" << LL_ENDL;
        sCreate = nullptr;
        return;
    }
    for (U32 i = 0; i < 2; ++i)
    {
        XrHandTrackerCreateInfoEXT ci{ XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        ci.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrResult res = sCreate(session, &ci, &sTrackers[i]);
        if (XR_FAILED(res))
        {
            LL_WARNS("PATXR") << "Hand tracking: xrCreateHandTrackerEXT failed (" << (S32)res << ")" << LL_ENDL;
            sTrackers[i] = XR_NULL_HANDLE;
        }
    }
    LL_INFOS("PATXR") << "Hand tracking: supported, trackers created" << LL_ENDL;
}

void PATXRHands::locate(XrSpace space, XrTime time, PATXRPalm palms[2])
{
    palms[0] = PATXRPalm();
    palms[1] = PATXRPalm();
    if (!sLocate)
    {
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    const bool log_now = now - sLastLog > 2.0;
    for (U32 i = 0; i < 2; ++i)
    {
        if (sTrackers[i] == XR_NULL_HANDLE)
        {
            continue;
        }
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locations{ XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joints;
        XrHandJointsLocateInfoEXT li{ XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
        li.baseSpace = space;
        li.time = time;
        const bool ok = XR_SUCCEEDED(sLocate(sTrackers[i], &li, &locations));
        const XrHandJointLocationEXT& palm = joints[XR_HAND_JOINT_PALM_EXT];
        const bool active = ok && locations.isActive && (palm.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        if (active)
        {
            palms[i].valid = true;
            palms[i].pos = palm.pose.position;
            ++sActiveFrames[i];
        }
        if (active != sLastActive[i] || (log_now && active))
        {
            LL_INFOS("PATXR") << "Hand " << (i == 0 ? "left" : "right") << (active ? " tracked" : " lost");
            if (active)
            {
                LL_CONT << llformat(" palm x=%.2f y=%.2f z=%.2f m", palm.pose.position.x, palm.pose.position.y, palm.pose.position.z);
            }
            LL_CONT << " (" << sActiveFrames[i] << " tracked frames so far)" << LL_ENDL;
            sLastActive[i] = active;
        }
    }
    if (log_now)
    {
        sLastLog = now;
    }
}

void PATXRHands::release()
{
    for (XrHandTrackerEXT& t : sTrackers)
    {
        if (t != XR_NULL_HANDLE && sDestroy)
        {
            sDestroy(t);
        }
        t = XR_NULL_HANDLE;
    }
    sCreate = nullptr;
    sDestroy = nullptr;
    sLocate = nullptr;
    sLastActive[0] = sLastActive[1] = false;
}

#endif // PAT_XR
