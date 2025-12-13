//
// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#pragma once

#include <string>

#include <sparse/sparse.h>
#include "android-base/unique_fd.h"

class Transport;

namespace fastboot {

enum RetCode : int {
    SUCCESS = 0,
    BAD_ARG,
    IO_ERROR,
    BAD_DEV_RESP,
    DEVICE_FAIL,
    TIMEOUT,
};

class IFastBootDriver {
  public:
    RetCode virtual FlashPartition(const std::string& partition, const std::vector<char>& data) = 0;
    RetCode virtual FlashPartition(const std::string& partition, android::base::borrowed_fd fd,
                                   uint32_t sz) = 0;
    RetCode virtual DeletePartition(const std::string& partition) = 0;
    RetCode virtual WaitForDisconnect() = 0;
    RetCode virtual Reboot(std::string* response = nullptr,
                           std::vector<std::string>* info = nullptr) = 0;

    RetCode virtual RebootTo(std::string target, std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual GetVar(const std::string& key, std::string* val,
                           std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual FetchToFd(const std::string& partition, android::base::borrowed_fd fd,
                              int64_t offset = -1, int64_t size = -1,
                              std::string* response = nullptr,
                              std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual Download(const std::string& name, android::base::borrowed_fd fd, size_t size,
                             std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual Download(android::base::borrowed_fd fd, size_t size,
                             std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual Download(const std::string& name, const std::vector<char>& buf,
                             std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual Download(const std::vector<char>& buf, std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual Download(const std::string& partition, struct sparse_file* s, uint32_t sz,
                             size_t current, size_t total, bool use_crc,
                             std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual RawCommand(const std::string& cmd, const std::string& message,
                               std::string* response = nullptr,
                               std::vector<std::string>* info = nullptr, int* dsize = nullptr) = 0;
    virtual RetCode RawCommand(const std::string& cmd, std::string* response = nullptr,
                               std::vector<std::string>* info = nullptr, int* dsize = nullptr) = 0;
    RetCode virtual ResizePartition(const std::string& partition, const std::string& size) = 0;
    RetCode virtual Erase(const std::string& partition, std::string* response = nullptr,
                          std::vector<std::string>* info = nullptr) = 0;
    RetCode virtual Flash(const std::string& partition, std::string* response = nullptr,
                          std::vector<std::string>* info = nullptr) = 0;
    virtual ~IFastBootDriver() = default;
    virtual std::string Error() = 0;
    virtual RetCode SetActive(const std::string& slot, std::string* response = nullptr,
                              std::vector<std::string>* info = nullptr) = 0;
    virtual void set_transport(std::unique_ptr<Transport> transport) = 0;
    virtual RetCode SnapshotUpdateCommand(const std::string& command,
                                          std::string* response = nullptr,
                                          std::vector<std::string>* info = nullptr) = 0;
    virtual RetCode Boot(std::string* response = nullptr,
                         std::vector<std::string>* info = nullptr) = 0;
    virtual RetCode Continue(std::string* response = nullptr,
                             std::vector<std::string>* info = nullptr) = 0;
    virtual RetCode CreatePartition(const std::string& partition, const std::string& size) = 0;
    virtual RetCode Upload(const std::string& outfile, std::string* response = nullptr,
                           std::vector<std::string>* info = nullptr) = 0;
};
}  // namespace fastboot
