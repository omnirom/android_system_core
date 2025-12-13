/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <android-base/file.h>
#include <gtest/gtest.h>
#include <liblp/builder.h>
#include <liblp/liblp.h>
#include <storage_literals/storage_literals.h>

using android::fs_mgr::LpMetadata;
using android::fs_mgr::MetadataBuilder;
using namespace android::storage_literals;

TEST(Writer, WriteValidation) {
    auto builder = MetadataBuilder::New(2_GiB, 16_KiB, 2);
    ASSERT_NE(builder, nullptr);

    auto p = builder->AddPartition("system", LP_PARTITION_ATTR_NONE);
    ASSERT_NE(p, nullptr);

    // Add too many extents.
    for (size_t i = 0; i < 32000; i += 2) {
        ASSERT_TRUE(builder->AddLinearExtent(p, "super", 1, i));
    }

    auto exported = builder->Export();
    ASSERT_NE(exported, nullptr);

    TemporaryFile temp;
    ASSERT_GE(temp.fd, 0);
    ASSERT_FALSE(WriteToImageFile(temp.fd, *exported.get()));
}
