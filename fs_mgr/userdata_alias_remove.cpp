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

/*
 * This removes a userdata aliasing file, which maps storage space for
 * a specific purpose to a special-purpose partition, allowing for its
 * later release.
 */

#include <errno.h>
#include <error.h>
#include <stdio.h>

#include <android-base/file.h>
#include <android-base/properties.h>

#include <fstab/fstab.h>

static constexpr const char* ALIAS_REMOVE_PROP_NAME = "userdata.alias.remove";

int main(void) {
    android::fs_mgr::Fstab fstab;
    if (!android::fs_mgr::ReadDefaultFstab(&fstab)) {
        error(1, 0, "no valid fstab");
    }

    android::fs_mgr::FstabEntry* dataEntry =
            android::fs_mgr::GetEntryForMountPoint(&fstab, "/data");
    if (!dataEntry) {
        error(1, 0, "/data is not mounted yet");
    }

    /* Only F2FS supports device aliasing file */
    if (dataEntry->fs_type != "f2fs") {
        return 0;
    }

    std::string target = android::base::GetProperty(ALIAS_REMOVE_PROP_NAME, "");
    for (size_t i = 0; i < dataEntry->user_devices.size(); ++i) {
        if (dataEntry->device_aliased[i]) {
            std::string deviceName = android::base::Basename(dataEntry->user_devices[i]);
            if (target == deviceName) {
                std::string filename = "/data/" + target;
                if (unlink(filename.c_str())) {
                    error(1, errno, "Failed to remove file: %s", filename.c_str());
                }
                return 0;
            }
        }
    }

    error(1, 0, "%s is not a device aliasing file", target.c_str());
}
