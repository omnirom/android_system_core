/// Copyright (C) 2024 The Android Open Source Project
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

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <fs_mgr/file_wait.h>
#include <libdm/dm.h>
#include <snapuserd/snapuserd_kernel.h>
#include <snapuserd/ublk_block_server.h>
#include <chrono>
#include <thread>
#include "snapuserd_logging.h"

#define UBLKSRV_TGT_TYPE_SNAPSHOT 0
namespace android {
namespace snapshot {

using android::base::unique_fd;
using fs_mgr::WaitForFile;

constexpr int kDefaultQueueDepth = 32;
constexpr int kDefaultMaxIoBufBytes = 64 * 1024;

int SnapshotInitTarget(struct ublksrv_dev* dev, int type, int argc, char** argv) {
    const struct ublksrv_ctrl_dev_info* info = ublksrv_ctrl_get_dev_info(ublksrv_get_ctrl_dev(dev));
    struct ublksrv_tgt_info* tgt = &dev->tgt;
    const char* device_name;

    if (type != UBLKSRV_TGT_TYPE_SNAPSHOT) {
        LOG(ERROR) << "SnapshotInitTarget: Invalid target type: " << type;
        return -EINVAL;
    }
    if (argc != 2) {
        LOG(ERROR) << "SnapshotInitTarget: Invalid argument count: " << argc << ", expected 2";
        return -EINVAL;
    }

    device_name = argv[0];
    uint64_t num_sectors = strtoull(argv[1], nullptr, 10);
    tgt->dev_size = num_sectors << SECTOR_SHIFT;
    LOG(DEBUG) << "Initializing snapshot: device name:" << device_name
               << " device size:" << tgt->dev_size;
    tgt->tgt_ring_depth = info->queue_depth;
    tgt->nr_fds = 0;
    return 0;
    //  TODOUBLK: b/414812023 : Save the config using ublksrv_json_write_target_base_info()
    //  This can be used in recovery time if snapuserd restarts
}

int SnapshotHandleIoAsync(const struct ublksrv_queue* q, const struct ublk_io_data* data) {
    const struct ublksrv_io_desc* iod = data->iod;
    // Retrieve UblkBlockServer object from queue and call ProcessRequest()

    UblkBlockServer* server = static_cast<UblkBlockServer*>(q->private_data);
    if (!server->ProcessRequest(data)) {
        // Something went wrong so complete with EIO
        LOG(ERROR) << "ERROR completing io " << data->tag << "size "
                   << (iod->nr_sectors << SECTOR_SHIFT);
        ublksrv_complete_io(q, data->tag, -EIO);
    } else {
        LOG(DEBUG) << "completing io " << data->tag << " size "
                   << (iod->nr_sectors << SECTOR_SHIFT);
        ublksrv_complete_io(q, data->tag, iod->nr_sectors << SECTOR_SHIFT);
    }

    return 0;
}
static struct ublksrv_tgt_type snapshot_tgt_type = {
        .handle_io_async = SnapshotHandleIoAsync,
        .init_tgt = SnapshotInitTarget,
        .type = UBLKSRV_TGT_TYPE_SNAPSHOT,
        .name = "android_snapshot",
        // TODOUBLK: b/414812023 : Probably we can recover the snapshot device config using
        // recovery_target
        //.recovery_tgt = snapshot_recovery_tgt,
};

UblkDeviceInfo::UblkDeviceInfo(const std::string& name, uint64_t num_sectors, int num_queues,
                               UeventHelperCallback callback)
    : data_{}, num_sectors_(num_sectors), name_(name), uevent_helper_(std::move(callback)) {
    argv_storage_ = {name_, std::to_string(num_sectors_)};
    argv_pointers_ = {argv_storage_[0].data(), argv_storage_[1].data()};

    data_.dev_id = -1;  // Let ublksrv assign
    data_.tgt_ops = &snapshot_tgt_type;
    data_.tgt_type = "android_snapshot";
    data_.queue_depth = kDefaultQueueDepth;
    data_.flags = 0;
    data_.nr_hw_queues = num_queues;
    data_.max_io_buf_bytes = kDefaultMaxIoBufBytes;
    data_.run_dir = nullptr;
    data_.tgt_argc = argv_pointers_.size();  // Will be 2
    data_.tgt_argv = argv_pointers_.data();

    ctrl_dev_ = ublksrv_ctrl_init(&data_);
    if (!ctrl_dev_) {
        PLOG(ERROR) << "ublksrv_ctrl_init failed for " << name_;
    } else {
        LOG(DEBUG) << "ublksrv_ctrl_init successful for " << name_
                   << ", dev_id: " << ublksrv_ctrl_get_dev_info(ctrl_dev_)->dev_id;
    }
}

bool UblkDeviceInfo::InitDev() {
    LOG(INFO) << "UblkDeviceInfo::InitDev() one-time initialization started for" << name_;
    if (uevent_helper_) {
        uevent_helper_(GetUblockCtrDeviceName());
    }
    auto ublk_char_dev = "/dev/ublkc" + std::to_string(GetDeviceId());
    if (!WaitForFile(ublk_char_dev, 1s)) {
        LOG(ERROR) << "InitDev: Failed waiting for " << ublk_char_dev;
        return false;
    }
    dev_ = const_cast<struct ublksrv_dev*>(ublksrv_dev_init(ctrl_dev_));
    if (!dev_) {
        LOG(ERROR) << "ublksrv_dev_init failed for " << name_;
        return false;
    }
    SetDevReady();
    LOG(INFO) << "InitDev done for " << name_;
    return true;
}

UblkBlockServer::UblkBlockServer(const std::string& misc_name,
                                 std::shared_ptr<UblkDeviceInfo> device_info, int q_id,
                                 UeventHelperCallback callback = nullptr)
    : misc_name_(misc_name),
      device_info_(device_info),
      qid_(q_id),
      uevent_helper_(std::move(callback)) {}

void UblkBlockServer::Open(Delegate* delegate, size_t buffer_size) {
    delegate_ = delegate;
    buffer_.Initialize(buffer_size);
}

bool UblkBlockServer::Initialize() {
    // always do the InitDev() in queue 0.
    if (qid_ == 0) {
        if (!device_info_->InitDev()) {
            SNAP_LOG(ERROR) << "Device initialization failed for queue " << qid_;
            return false;
        }
    } else if (!device_info_->WaitForDevReady()) {
        SNAP_LOG(ERROR) << "Device not ready for queue " << qid_;
        return false;
    }

    q_ = ublksrv_queue_init(device_info_->dev(), qid_, (void*)this);
    if (!q_) {
        SNAP_LOG(ERROR) << "ublksrv_queue_init failed for " << qid_;
        return false;
    }
    SNAP_LOG(DEBUG) << "ublksrv_queue_init success for " << qid_;
    q_inited_ = true;
    return true;
}

UblkBlockServer::~UblkBlockServer() {
    if (q_inited_ && q_) {
        SNAP_LOG(DEBUG) << "Deinitializing ublk queue " << qid_;
        ublksrv_queue_deinit(q_);
    }
}
bool UblkBlockServer::ProcessRequests() {
    // We need to actually create queue in the context of the thread which will be serving
    // io. This is the limitation of io_uring as ring is only mmaped to this process.
    // We won't be able to complete the IOs if the queues are created in different context.
    // So first time ProcessRequests() is called, create the queue before proceeding.
    // TODOUBLK: b/414812023, can use q_ itself as indicator instead of explicit bool
    if (!q_inited_) {
        Initialize();
    }
    // All we need is ublk_process_io call. This will call
    // handle_io_async() ops.
    // Also ProcessRequest() is called within a while(true) loop,
    // so we don't need a loop here.
    if (ublksrv_process_io(q_) < 0) {
        SNAP_LOG(INFO) << "ublk dev queue " << q_->q_id << " exiting";
        return false;
    }

    return true;
}

bool UblkBlockServer::ProcessRequest(const struct ublk_io_data* data) {
    // Reset the state and save the current request.
    ResetCurrentRequest(data);
    const struct ublksrv_io_desc* iod = current_->iod;
    bool io_done = false;
    // Reset the output buffer.
    buffer_.ResetBufferOffset();
    unsigned ublk_op = ublksrv_get_op(iod);
    SNAP_LOG(VERBOSE) << "UblkDaemon: request->op: " << ublk_op;
    SNAP_LOG(VERBOSE) << "UblkDaemon: request->flags: " << ublksrv_get_flags(iod);
    SNAP_LOG(VERBOSE) << "UblkDaemon: request->len: " << (iod->nr_sectors << SECTOR_SHIFT);
    SNAP_LOG(VERBOSE) << "UblkDaemon: request->sector: " << iod->start_sector;
    switch (ublk_op) {
        case UBLK_IO_OP_READ:
            io_done = delegate_->RequestSectors(iod->start_sector, iod->nr_sectors << SECTOR_SHIFT);
            SNAP_LOG(DEBUG) << "RequestSectors for sector" << iod->start_sector << "returned";
            break;

        case UBLK_IO_OP_WRITE:
            // We should not get any write request to ublk as we mount all
            // partitions as read-only.
            SNAP_LOG(ERROR) << "Unexpected write request from ublk";
            break;

        default:
            SNAP_LOG(ERROR) << "Unexpected request from ublk: " << ublk_op;
            break;
    }
    if (io_done) {
        // if we processed the request, check if the copied data matches the request size
        // this is unlikely, just a cautious check
        if (progress_ != (iod->nr_sectors << SECTOR_SHIFT)) {
            SNAP_LOG(ERROR) << "Progress (" << progress_ << ") does not match requested size ("
                            << (iod->nr_sectors << SECTOR_SHIFT) << ").";
        }
    }

    return io_done;
}

void* UblkBlockServer::GetResponseBuffer(size_t size, size_t to_write) {
    return buffer_.AcquireBuffer(size, to_write);
}

bool UblkBlockServer::SendBufferedIo() {
    size_t payload_size = buffer_.GetPayloadBytesWritten();
    void* src = buffer_.GetPayloadBufPtr();
    void* dest = (void*)(current_->iod->addr + progress_);
    // Stage the data in the current request's buffer
    memcpy(dest, src, payload_size);
    progress_ += payload_size;
    // Reset the buffer for the next part of the current request,
    // or for the next request if the current one is fully processed.
    buffer_.ResetBufferOffset();
    return true;
}

UblkBlockServerOpener::UblkBlockServerOpener(const std::string& misc_name,
                                             std::shared_ptr<UblkDeviceInfo> device_info,
                                             UeventHelperCallback callback = nullptr)
    : misc_name_(misc_name), device_info_(device_info), queues_(0) {
    SNAP_LOG(DEBUG) << "UblkBlockServerOpener created";
    if (callback) uevent_helper_ = std::move(callback);
}

UblkBlockServerOpener::~UblkBlockServerOpener() {
    SNAP_LOG(DEBUG) << "UblkBlockServerOpener destroyed";
}

std::unique_ptr<IBlockServer> UblkBlockServerOpener::Open(IBlockServer::Delegate* delegate,
                                                          size_t buffer_size) {
    auto server =
            std::make_unique<UblkBlockServer>(misc_name_, device_info_, queues_++, uevent_helper_);
    server->Open(delegate, buffer_size);
    servers_.push_back(std::move(server));
    return std::move(servers_.back());
}

std::shared_ptr<UblkBlockServerOpener> UblkDeviceManager::CreateUblkOpener(
        const std::string& device_name) {
    // 1. Check if an opener for this device already exists
    auto it = device_openers_.find(device_name);
    if (it != device_openers_.end()) {
        LOG(INFO) << "Returning previously created opener for " << device_name;
        return it->second;
    }
    // 2. Open a fresh one
    auto opener = std::make_shared<UblkBlockServerOpener>(device_name, GetDeviceInfo(device_name),
                                                          uevent_helper_);
    device_openers_.emplace(device_name, opener);
    return opener;
}

uint64_t UblkDeviceInfo::GetNumSectors() {
    return num_sectors_;
}

UblkDeviceInfo::~UblkDeviceInfo() {
    LOG(DEBUG) << "UblkDeviceInfo destructor for " << name_;
    if (dev_) {
        LOG(DEBUG) << "Deinitializing ublk dev for " << name_;
        ublksrv_dev_deinit(dev_);
        dev_ = nullptr;
    }
    if (ctrl_dev_) {
        LOG(DEBUG) << "Deleting ublk ctrl_dev (del_dev) for " << name_;
        if (ublksrv_ctrl_del_dev(ctrl_dev_) < 0) {
            PLOG(ERROR) << "ublksrv_ctrl_del_dev failed for " << name_;
            // Continue to deinit ctrl_dev anyway
        }

        LOG(DEBUG) << "Deinitializing ublk ctrl_dev (ctrl_deinit) for " << name_;
        ublksrv_ctrl_deinit(ctrl_dev_);
        ctrl_dev_ = nullptr;
    }
}

void UblkDeviceInfo::SetDevReady() {
    {
        std::lock_guard<std::mutex> lock(dev_ready_mutex_);
        dev_is_ready_ = true;
    }
    dev_ready_cv_.notify_all();
}

bool UblkDeviceInfo::WaitForDevReady() {
    std::unique_lock<std::mutex> lock(dev_ready_mutex_);
    if (!dev_ready_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return dev_is_ready_; })) {
        LOG(ERROR) << "Timed out waiting for device to be ready: " << name_;
        return false;
    }
    return true;
}

bool UblkDeviceManager::CreateDmLinearDevice(UblkDeviceInfo& device_info) {
    dm::DmTable table;
    table.Emplace<dm::DmTargetLinear>(0, device_info.GetNumSectors(),
                                      device_info.GetUblockDeviceName(), 0);
    auto& dm_ = dm::DeviceMapper::Instance();
    std::string name_for_linear = device_info.name();
    std::string linear_path;
    // TODOUBLK: b/414812023 : Fix up linear name in case they are being created in first_stage_init
    // Either this or snapshot_client should not send us -init when running with ublk
    if (name_for_linear.ends_with("-init")) {
        name_for_linear.resize(name_for_linear.size() - 5);
        LOG(DEBUG) << "nameForLinear trimmed : " << name_for_linear;
    }

    WaitForFile(device_info.GetUblockDeviceName(), 5s);
    if (dm_.GetState(name_for_linear) != dm::DmDeviceState::INVALID) {
        // this means device exists and we need to do LoadTableandActivate
        auto ret = dm_.LoadTableAndActivate(name_for_linear, table);
        if (!ret) {
            LOG(ERROR) << "Failed to do dm switch tables for " << name_for_linear;
            return false;
        }
        LOG(INFO) << "Replaced old ublk device with new one.";
        dm_.GetDmDevicePathByName(name_for_linear, &linear_path);

    } else {
        if (!dm_.CreateDevice(name_for_linear, table, &linear_path, 5s)) {
            LOG(ERROR) << "DM device creating failed for: " << name_for_linear;
            return false;
        }
    }
    LOG(INFO) << "DM device created for: " << name_for_linear << " at: " << linear_path;
    device_info.set_linear_path(linear_path);
    return true;
}

bool UblkDeviceManager::DeleteDmLinearDevice(UblkDeviceInfo& device_info) {
    auto& dm = dm::DeviceMapper::Instance();
    auto deviceName = device_info.name();
    auto linear_path = device_info.linear_path();

    if (!dm.DeleteDeviceIfExists(device_info.name())) {
        LOG(WARNING) << "Unable to delete DM linear device " << deviceName << " at " << linear_path;
        return false;
    }
    LOG(INFO) << "Deleted DM linear device for " << deviceName;
    return true;
}

bool UblkDeviceManager::StartDevice(const std::string& device_name) {
    auto deviceInfo = GetDeviceInfo(device_name);
    if (!deviceInfo) {
        LOG(ERROR) << "Device not found: " << device_name;
        return false;
    }

    if (!deviceInfo->WaitForDevReady()) {
        LOG(ERROR) << "StartDevice: Timeout Device " << device_name << " not ready";
        return false;
    }

    // ublksrv_ctrl_start_dev tells ublk driver to expose /dev/ublkbN
    // That's why it is necessary to start the dev in-order to create
    // dm-linear device on top of the /dev/ublkbN.
    // start_dev cannot be done without initing queues.
    // So if ublksrv_ctrl_start_dev() appears stuck, its waiting for
    // all the queues to do init which posts the SQEs and in kernel
    // state machine kicks in.
    if (ublksrv_ctrl_start_dev(deviceInfo->ctrl_dev(), getpid()) < 0) {
        LOG(ERROR) << "ublksrv_ctrl_start_dev failed";
        return false;
    }
    if (uevent_helper_) {
        uevent_helper_(deviceInfo->GetUblockDeviceName());
    }

    LOG(INFO) << "ctrl start dev done for " << device_name;

    if (!device_name.ends_with("-init")) return CreateDmLinearDevice(*deviceInfo);
    return true;
}

bool UblkDeviceManager::StopDevice(const std::string& device_name) {
    auto device_info = GetDeviceInfo(device_name);
    if (!device_info) {
        LOG(ERROR) << "Device not found: " << device_name;
        return false;
    }

    struct ublksrv_ctrl_dev* ctrlDev = device_info->ctrl_dev();
    if (!ctrlDev) {
        // This case should ideally not happen if CreateDevice succeeded and deviceInfo is valid.
        LOG(ERROR) << "Cannot stop device " << device_name
                   << ", ctrl_dev is null in UblkDeviceInfo.";
        // Still attempt to remove from maps to clean up manager's state.
        devices_.erase(device_name);
        device_openers_.erase(device_name);
        return false;  // Indicate failure as the device wasn't properly initialized.
    }

    // Signal the ublk device to stop processing I/O.
    if (ublksrv_ctrl_stop_dev(ctrlDev) < 0) {
        PLOG(ERROR) << "ublksrv_ctrl_stop_dev failed for " << device_name;
        // Proceed with cleanup, but the device might not have stopped cleanly.
    } else {
        LOG(INFO) << "ublk device stopped (stop_dev) for " << device_name;
    }

    // Remove the UblkDeviceInfo from the manager.
    LOG(INFO) << "Removing UblkDeviceInfo and UblkBlockServerOpener for " << device_name;
    devices_.erase(device_name);
    device_openers_.erase(device_name);

    return true;
}

bool UblkDeviceManager::CreateDevice(const std::string& device_name, uint64_t num_sectors,
                                     int num_queues) {
    auto device_info =
            std::make_shared<UblkDeviceInfo>(device_name, num_sectors, num_queues, uevent_helper_);

    if (!device_info->ctrl_dev()) {
        LOG(ERROR) << "Failed to initialize UblkDeviceInfo for " << device_name;
        return false;
    }

    struct ublksrv_ctrl_dev* ctrl_dev_ = device_info->ctrl_dev();

    if (ublksrv_ctrl_add_dev(ctrl_dev_) < 0) {
        LOG(ERROR) << "ublksrv_ctrl_add_dev failed for " << device_name;
        return false;
    }
    LOG(DEBUG) << "ublksrv_ctrl_add_dev successful for " << device_name;
    if (ublksrv_ctrl_get_affinity(ctrl_dev_) < 0) {
        // This is not fatal
        LOG(WARNING) << "ublksrv_ctrl_get_affinity failed for " << device_name;
    }
    struct ublk_params p = {.types = UBLK_PARAM_TYPE_BASIC,
                            .basic = {
                                    .logical_bs_shift = SECTOR_SHIFT,
                                    .physical_bs_shift = 12,
                                    .io_opt_shift = 12,
                                    .io_min_shift = 9,
                                    .max_sectors = kDefaultMaxIoBufBytes >> SECTOR_SHIFT,
                                    .dev_sectors = num_sectors,
                            }};
    if (ublksrv_ctrl_set_params(ctrl_dev_, &p)) {
        LOG(ERROR) << "ublksrv_ctrl_set_params failed for " << device_name;
        return false;
    }
    LOG(DEBUG) << "ublksrv_ctrl_set_params done for " << device_name;

    devices_.emplace(device_name, device_info);
    return true;
}

uint64_t UblkDeviceManager::GetDeviceSize(const std::string& device_name) {
    auto it = devices_.find(device_name);
    if (it != devices_.end()) {
        return it->second->GetSize();
    }
    LOG(ERROR) << "Device not found: " << device_name;
    return 0;
}
}  // namespace snapshot
}  // namespace android
