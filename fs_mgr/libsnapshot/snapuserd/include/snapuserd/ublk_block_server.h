// Copyright (C) 2024 The Android Open Source Project
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

#pragma once

#include <android-base/unique_fd.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

#include <snapuserd/block_server.h>
#include <snapuserd/snapuserd_buffer.h>
#include <snapuserd/snapuserd_kernel.h>
#include <ublksrv.h>

namespace android {
namespace snapshot {

class UblkDeviceInfo;

class UblkBlockServer : public IBlockServer {
  public:
    UblkBlockServer(const std::string& misc_name, std::shared_ptr<UblkDeviceInfo> deviceInfo,
                    int q_id, UeventHelperCallback callback);

    bool ProcessRequests() override;
    void* GetResponseBuffer(size_t size, size_t to_write) override;
    bool SendBufferedIo() override;
    bool ProcessRequest(const struct ublk_io_data* data);
    void Open(Delegate* delegate, size_t buffer_size);
    void SetUeventHelper(UeventHelperCallback callback);
    ~UblkBlockServer();

  private:
    bool Initialize();
    void ResetCurrentRequest(const struct ublk_io_data* data) {
        current_ = data;
        progress_ = 0;
    }

    std::string misc_name_;
    Delegate* delegate_;
    BufferSink buffer_;
    const struct ublksrv_queue* q_;
    std::shared_ptr<UblkDeviceInfo> device_info_;
    int qid_;
    const struct ublk_io_data* current_;
    uint64_t progress_;
    bool q_inited_ = false;
    UeventHelperCallback uevent_helper_;
};

class UblkDeviceInfo {
  private:
    std::vector<std::string> argv_storage_;
    std::vector<char*> argv_pointers_;
    struct ublksrv_dev_data data_{};
    struct ublksrv_ctrl_dev* ctrl_dev_ = nullptr;
    struct ublksrv_dev* dev_ = nullptr;
    uint64_t num_sectors_;
    std::string name_;
    std::string linear_path_;
    UeventHelperCallback uevent_helper_;
    std::mutex dev_ready_mutex_;
    std::condition_variable dev_ready_cv_;
    bool dev_is_ready_ = false;

  public:
    UblkDeviceInfo(const std::string& name, uint64_t num_sectors, int num_queues,
                   UeventHelperCallback callback = nullptr);
    ~UblkDeviceInfo();
    uint64_t GetSize() const { return num_sectors_ << SECTOR_SHIFT; }
    uint64_t GetNumSectors();
    const std::string& name() const { return name_; }
    int GetDeviceId() const {
        auto dev_info = ublksrv_ctrl_get_dev_info(ctrl_dev_);
        return dev_info->dev_id;
    }
    std::string GetUblockDeviceName() { return "/dev/block/ublkb" + std::to_string(GetDeviceId()); }
    std::string GetUblockCtrDeviceName() { return "/dev/ublkc" + std::to_string(GetDeviceId()); }
    struct ublksrv_ctrl_dev* ctrl_dev() const { return ctrl_dev_; }
    const struct ublksrv_dev* dev() const { return dev_; }
    bool InitDev();
    void set_linear_path(const std::string& path) { linear_path_ = path; }
    const std::string& linear_path() const { return linear_path_; }
    bool WaitForDevReady();
    void SetDevReady();
};
class UblkBlockServerOpener : public IBlockServerOpener {
  public:
    UblkBlockServerOpener(const std::string& misc_name, std::shared_ptr<UblkDeviceInfo> deviceInfo,
                          UeventHelperCallback callback);
    ~UblkBlockServerOpener();
    // This is going to be a queue equivalent
    std::unique_ptr<IBlockServer> Open(IBlockServer::Delegate* delegate,
                                       size_t buffer_size) override;
    UeventHelperCallback GetUeventHelper() { return uevent_helper_; }

  private:
    // This can be multiple queues as defined by ro property.
    std::vector<std::unique_ptr<UblkBlockServer>> servers_;
    std::string misc_name_;
    std::shared_ptr<UblkDeviceInfo> device_info_;
    int queues_;
    UeventHelperCallback uevent_helper_;
};

class UblkDeviceManager {
  public:
    bool CreateDevice(const std::string& device_name, uint64_t num_sectors, int num_queues);
    std::shared_ptr<UblkDeviceInfo> GetDeviceInfo(const std::string& device_name) {
        auto it = devices_.find(device_name);
        if (it != devices_.end()) {
            return it->second;
        }
        return nullptr;
    }
    std::shared_ptr<UblkBlockServerOpener> CreateUblkOpener(const std::string& device_name);
    bool StartDevice(const std::string& device_name);
    bool StopDevice(const std::string& device_name);
    void SetUeventHelper(UeventHelperCallback callback) { uevent_helper_ = std::move(callback); }
    UeventHelperCallback GetUeventHelper() { return uevent_helper_; }

  private:
    // Store a map of device names to UblkDeviceInfo
    std::unordered_map<std::string, std::shared_ptr<UblkDeviceInfo>> devices_;
    // Store a map of device names to their corresponding openers
    std::unordered_map<std::string, std::shared_ptr<UblkBlockServerOpener>> device_openers_;

    uint64_t GetDeviceSize(const std::string& device_name);
    bool CreateDmLinearDevice(UblkDeviceInfo& device_info);
    bool DeleteDmLinearDevice(UblkDeviceInfo& device_name);
    UeventHelperCallback uevent_helper_;
};

class UblkBlockServerFactory : public IBlockServerFactory {
  public:
    std::shared_ptr<IBlockServerOpener> CreateOpener(const std::string& misc_name) override {
        return device_manager_.CreateUblkOpener(misc_name);
    }

    bool CreateDevice(const std::string& device_name, uint64_t num_sectors,
                      int num_queues) override {
        return device_manager_.CreateDevice(device_name, num_sectors, num_queues);
    }
    bool StartDevice(const std::string& device_name) {
        return device_manager_.StartDevice(device_name);
    }
    bool StopDevice(const std::string& device_name) {
        return device_manager_.StopDevice(device_name);
    }
    std::optional<std::string> GetDeviceName(const std::string& misc_name) override {
        auto deviceInfo = device_manager_.GetDeviceInfo(misc_name);
        if (deviceInfo) {
            return deviceInfo->GetUblockDeviceName();
        }
        return std::nullopt;
    };
    void SetUeventHelper(UeventHelperCallback callback) override {
        uevent_helper_ = std::move(callback);
        device_manager_.SetUeventHelper(uevent_helper_);
    }
    UeventHelperCallback GetUeventHelper() override { return uevent_helper_; }

  private:
    UblkDeviceManager device_manager_;  // Instance of the embedded manager
    UeventHelperCallback uevent_helper_;
};

}  // namespace snapshot
}  // namespace android
