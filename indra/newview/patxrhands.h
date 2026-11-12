/**
 * @file patxrhands.h
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

#ifndef PAT_XRHANDS_H
#define PAT_XRHANDS_H

#include <openxr/openxr.h>
#include <vector>

namespace PATXRHands
{
    // Creates hand trackers if the extension was enabled and the system supports them.
    void init(XrInstance instance, XrSystemId system, XrSession session, bool extension_enabled);

    // Palm positions (in 'space') of all currently tracked hands; logs the state periodically.
    void locate(XrSpace space, XrTime time, std::vector<XrVector3f>& palms);

    void release();
}

#endif // PAT_XRHANDS_H
