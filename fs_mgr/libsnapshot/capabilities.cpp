/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "libsnapshot/capabilities.h"

#include <sys/utsname.h>

#include <android-base/properties.h>
#include <com_android_libsnapshot.h>

#include <optional>

namespace android {
namespace snapshot {

static bool KernelSupportsUblk() {
    struct utsname uts{};
    unsigned int major, minor;

    uname(&uts);
    if (sscanf(uts.release, "%u.%u", &major, &minor) != 2) {
        return false;
    }
    // We will only support kernels from 6.1 onwards
    return major > 6 || (major == 6 && minor >= 1);
}

static bool IsVabcWithUblkSupportEnabledByFlag() {
    return com::android::libsnapshot::vabc_with_ublk_support();
}

bool IsUblkEnabled() {
    // Allow override for testing ublk mode
    std::string test_mode = android::base::GetProperty("snapuserd.test.ublk.force_mode", "");
    bool property_enabled = android::base::GetBoolProperty("ro.virtual_ab.ublk.enabled", false);
    if (test_mode == "enabled") {
        property_enabled = true;
    } else if (test_mode == "disabled") {
        property_enabled = false;
    }

    bool flag_enabled = IsVabcWithUblkSupportEnabledByFlag();
    bool kernel_support = KernelSupportsUblk();

    return (property_enabled && flag_enabled && kernel_support);
}

}  // namespace snapshot
}  // namespace android
