// Copyright (C) 2025 The Android Open Source Project
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

#include <getopt.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <charconv>
#include <optional>
#include <string_view>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/unique_fd.h>
#include <liblp/builder.h>
#include <liblp/liblp.h>
#include <iostream>

using android::base::unique_fd;
using android::fs_mgr::LpMetadata;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::PartitionOpener;
using android::fs_mgr::ReadMetadata;
using android::fs_mgr::UpdatePartitionTable;

enum class SubCommand {
    kUnknown,
    kAdd,
    kRemove,
};

static int usage(const char* program, SubCommand cmd = SubCommand::kUnknown) {
    std::cerr << program
              << " - Modifies partitions in a super partition block device (in-place metadata "
                 "modification).\n";
    std::cerr << "\n";
    std::cerr << "Usage:\n";
    std::cerr << "  " << program << " add [options] SUPER_DEVICE PARTNAME PARTGROUP\n";
    std::cerr << "  " << program << " remove [options] SUPER_DEVICE PARTNAME\n";
    std::cerr << "\n";

    if (cmd == SubCommand::kUnknown || cmd == SubCommand::kAdd) {
        std::cerr << "Add sub-command options:\n";
        std::cerr << "  SUPER_DEVICE                  Path to the super partition block device to "
                     "modify (e.g., /dev/block/by-name/super).\n";
        std::cerr << "  PARTNAME                      Name of the partition to add or replace in "
                     "SUPER_DEVICE.\n";
        std::cerr
                << "  PARTGROUP                     Name of the partition group to use. If the\n"
                << "                                partition can be updated over OTA, the group\n"
                << "                                should match its updatable group.\n";
        std::cerr << "  --replace                     If PARTNAME already exists, it will be "
                     "replaced.\n";
        std::cerr << "  --size <bytes>                Size in bytes for the new empty partition. "
                     "Defaults to 0 if not specified.\n";
    }
    if (cmd == SubCommand::kUnknown || cmd == SubCommand::kRemove) {
        std::cerr << "\nRemove sub-command options:\n";
        std::cerr << "  SUPER_DEVICE                  Path to the super block device (e.g., "
                     "/dev/block/by-name/super).\n";
        std::cerr << "  PARTNAME                      Name of the partition to remove.\n";
    }
    std::cerr << "\nCommon options:\n";
    std::cerr << "  --slot <number>               Metadata slot number to use (default is 0).\n";
    std::cerr << "  -h, --help                    Show this help message.\n";
    std::cerr << "\n";
    return EX_USAGE;
}

class SuperModifyHelper final {
  public:
    explicit SuperModifyHelper(const std::string& super_device_path, uint32_t slot)
        : super_device_path_(super_device_path), slot_(slot) {}

    bool Open();
    bool AddOrReplaceEmptyPartition(const std::string& partition_name,
                                    const std::string& group_name, uint32_t attributes,
                                    uint64_t partition_size, bool replace);
    bool RemovePartition(const std::string& partition_name);
    bool Finalize();

  private:
    bool OpenSuperDevice();
    bool UpdateSuperMetadata();

    std::string super_device_path_;
    unique_fd super_device_fd_;
    std::unique_ptr<LpMetadata> metadata_;
    std::unique_ptr<MetadataBuilder> builder_;
    uint32_t slot_;
};

bool SuperModifyHelper::Open() {
    if (!OpenSuperDevice()) {
        return false;
    }
    metadata_ = android::fs_mgr::ReadMetadata(super_device_path_, slot_);
    if (!metadata_) {
        std::cerr << "Could not read metadata from super device: " << super_device_path_ << " slot "
                  << slot_ << "\n";
        return false;
    }
    builder_ = MetadataBuilder::New(*metadata_.get());
    if (!builder_) {
        std::cerr << "Could not create MetadataBuilder for super device: " << super_device_path_
                  << "\n";
        return false;
    }
    return true;
}

bool SuperModifyHelper::AddOrReplaceEmptyPartition(const std::string& partition_name,
                                                   const std::string& group_name,
                                                   uint32_t attributes, uint64_t partition_size,
                                                   bool replace) {
    auto existing_partition = builder_->FindPartition(partition_name);
    if (existing_partition) {
        if (!replace) {
            std::cerr << "Partition " << partition_name
                      << " already exists and --replace not specified. Aborting.\n";
            return false;
        }
        std::cout << "Replacing existing partition: " << partition_name << std::endl;
        builder_->RemovePartition(partition_name);
    }

    auto new_partition = builder_->AddPartition(partition_name, group_name, attributes);
    if (!new_partition) {
        std::cerr << "Could not add partition metadata for: " << partition_name << "\n";
        return false;
    }

    if (!builder_->ResizePartition(new_partition, partition_size)) {
        std::cerr << "Failed to resize partition " << partition_name << " to " << partition_size
                  << " bytes.\n";
        return false;
    }
    std::cout << "Successfully configured partition " << partition_name << " with size "
              << partition_size << " bytes." << std::endl;

    return UpdateSuperMetadata();
}

bool SuperModifyHelper::RemovePartition(const std::string& partition_name) {
    auto partition = builder_->FindPartition(partition_name);
    if (!partition) {
        std::cerr << "Could not find partition to remove: " << partition_name << "\n";
        // This might not be an error if the goal is to ensure it's removed.
        // For now, let's treat it as a failure to make the operation explicit.
        return false;
    }
    builder_->RemovePartition(partition_name);
    std::cout << "Successfully marked partition " << partition_name << " for removal." << std::endl;
    return UpdateSuperMetadata();
}

bool SuperModifyHelper::OpenSuperDevice() {
    super_device_fd_.reset(open(super_device_path_.c_str(), O_RDWR | O_CLOEXEC));
    if (super_device_fd_ < 0) {
        std::cerr << "Open failed for super device: " << super_device_path_ << ": "
                  << strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool SuperModifyHelper::UpdateSuperMetadata() {
    metadata_ = builder_->Export();
    if (!metadata_) {
        std::cerr << "Failed to export new metadata for super device.\n";
        return false;
    }
    PartitionOpener opener;  // Default opener for block devices
    for (uint32_t i = 0; i < metadata_->geometry.metadata_slot_count; i++) {
        if (!UpdatePartitionTable(opener, super_device_path_, *metadata_.get(), i)) {
            std::cerr << "Could not write metadata to super device slot " << i << ".\n";
            return false;
        }
    }
    std::cout << "Successfully updated super partition metadata in all slots." << std::endl;
    return true;
}

bool SuperModifyHelper::Finalize() {
    if (super_device_fd_ >= 0 && fsync(super_device_fd_.get()) < 0) {
        std::cerr << "fsync on super device failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

enum class AddOptionCode : int {
    kReplace = 1,
    kSize = 2,
    kSlot = 3,  // Common option, but needs to be in specific subcommand options for getopt_long
    kHelp = 'h',
};

enum class RemoveOptionCode : int {
    kSlot = 1,  // Common option
    kHelp = 'h',
};

int HandleAddCommand(int argc, char* argv[], const char* program_name) {
    struct option add_options[] = {
            {"replace", no_argument, nullptr, static_cast<int>(AddOptionCode::kReplace)},
            {"size", required_argument, nullptr, static_cast<int>(AddOptionCode::kSize)},
            {"slot", required_argument, nullptr, static_cast<int>(AddOptionCode::kSlot)},
            {"help", no_argument, nullptr, static_cast<int>(AddOptionCode::kHelp)},
            {nullptr, 0, nullptr, 0},
    };

    bool replace_flag = false;
    uint64_t specified_size = 0;
    uint32_t slot = 0;

    int rv;
    // Reset optind for subcommand parsing
    optind = 1;
    while ((rv = getopt_long(argc, argv, "h", add_options, nullptr)) != -1) {
        switch (rv) {
            case static_cast<int>(AddOptionCode::kHelp):
                usage(program_name, SubCommand::kAdd);
                return EX_OK;
            case static_cast<int>(AddOptionCode::kReplace):
                replace_flag = true;
                break;
            case static_cast<int>(AddOptionCode::kSize): {
                if (!android::base::ParseUint(optarg, &specified_size)) {
                    std::cerr << "Error: Invalid argument for --size: " << optarg
                              << ". Must be a non-negative integer.\n";
                    return usage(program_name, SubCommand::kAdd);
                }
            } break;
            case static_cast<int>(AddOptionCode::kSlot): {
                if (!android::base::ParseUint(optarg, &slot)) {
                    std::cerr << "Error: Invalid argument for --slot: " << optarg
                              << ". Must be a non-negative integer.\n";
                    return usage(program_name, SubCommand::kAdd);
                }
            } break;
            default:
                return usage(program_name, SubCommand::kAdd);
        }
    }

    if (argc - optind != 3) {
        std::cerr << "Error: Incorrect number of arguments for 'add'. Expected SUPER_DEVICE "
                     "PARTNAME PARTGROUP.\n\n";
        return usage(program_name, SubCommand::kAdd);
    }

    std::string super_device_arg = argv[optind++];
    std::string partition_name_arg = argv[optind++];
    std::string group_name_arg = argv[optind++];

    SuperModifyHelper super_modifier(super_device_arg, slot);
    if (!super_modifier.Open()) {
        return EX_SOFTWARE;
    }

    uint32_t attributes = LP_PARTITION_ATTR_NONE;

    if (!super_modifier.AddOrReplaceEmptyPartition(partition_name_arg, group_name_arg, attributes,
                                                   specified_size, replace_flag)) {
        return EX_SOFTWARE;
    }

    if (!super_modifier.Finalize()) {
        return EX_SOFTWARE;
    }

    std::cout << "lpmodify add: Successfully processed partition " << partition_name_arg << " on "
              << super_device_arg << ".\n";
    return EX_OK;
}

int HandleRemoveCommand(int argc, char* argv[], const char* program_name) {
    struct option remove_options[] = {
            {"slot", required_argument, nullptr, static_cast<int>(RemoveOptionCode::kSlot)},
            {"help", no_argument, nullptr, static_cast<int>(RemoveOptionCode::kHelp)},
            {nullptr, 0, nullptr, 0},
    };

    uint32_t slot = 0;

    int rv;
    // Reset optind for subcommand parsing
    optind = 1;
    while ((rv = getopt_long(argc, argv, "h", remove_options, nullptr)) != -1) {
        switch (rv) {
            case static_cast<int>(RemoveOptionCode::kHelp):
                return usage(program_name, SubCommand::kRemove);
            case static_cast<int>(RemoveOptionCode::kSlot): {
                if (!android::base::ParseUint(optarg, &slot)) {
                    std::cerr << "Error: Invalid argument for --slot: " << optarg
                              << ". Must be a non-negative integer.\n";
                    return usage(program_name, SubCommand::kRemove);
                }
            } break;
            default:
                return usage(program_name, SubCommand::kRemove);
        }
    }

    if (argc - optind != 2) {
        std::cerr << "Error: Incorrect number of arguments for 'remove'. Expected SUPER_DEVICE "
                     "PARTNAME.\n\n";
        return usage(program_name, SubCommand::kRemove);
    }

    std::string super_device_path = argv[optind++];
    std::string partition_name = argv[optind++];

    SuperModifyHelper remover(super_device_path, slot);
    if (!remover.Open()) {
        return EX_SOFTWARE;
    }

    if (!remover.RemovePartition(partition_name)) {
        remover.Finalize();
        return EX_SOFTWARE;
    }

    if (!remover.Finalize()) {
        return EX_SOFTWARE;
    }

    std::cout << "lpmodify remove: Partition " << partition_name << " removed successfully from "
              << super_device_path << ".\n";
    return EX_OK;
}

int main(int argc, char* argv[]) {
    const char* program_name = argv[0];

    if (argc < 2) {
        std::cerr << "Error: No subcommand specified.\n";
        return usage(program_name);
    }

    std::string subcommand_str = argv[1];

    if (subcommand_str == "add") {
        // Shift argv and adjust argc for subcommand handlers
        return HandleAddCommand(argc - 1, argv + 1, program_name);
    } else if (subcommand_str == "remove") {
        return HandleRemoveCommand(argc - 1, argv + 1, program_name);
    } else if (subcommand_str == "--help" || subcommand_str == "-h") {
        return usage(program_name);
    } else {
        std::cerr << "Error: Unknown subcommand: " << subcommand_str << "\n";
        return usage(program_name);
    }
}
