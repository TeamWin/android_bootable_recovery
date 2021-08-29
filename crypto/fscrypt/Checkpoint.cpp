/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "Checkpoint"
#include "Checkpoint.h"
#include "VoldUtil.h"
#include "VolumeManager.h"

#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <android/hardware/boot/1.0/IBootControl.h>
#include <cutils/android_reboot.h>
#include <fcntl.h>
#include <fs_mgr.h>
#include <linux/fs.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using android::base::GetBoolProperty;
using android::base::GetUintProperty;
using android::base::SetProperty;
using android::binder::Status;
using android::fs_mgr::Fstab;
using android::fs_mgr::ReadDefaultFstab;
using android::fs_mgr::ReadFstabFromFile;
using android::hardware::hidl_string;
using android::hardware::boot::V1_0::BoolResult;
using android::hardware::boot::V1_0::CommandResult;
using android::hardware::boot::V1_0::IBootControl;
using android::hardware::boot::V1_0::Slot;


namespace {
const std::string kMetadataCPFile = "/metadata/vold/checkpoint";

android::binder::Status error(const std::string& msg) {
    PLOG(ERROR) << msg;
    return android::binder::Status::fromServiceSpecificError(errno, android::String8(msg.c_str()));
}

android::binder::Status error(int error, const std::string& msg) {
    LOG(ERROR) << msg;
    return android::binder::Status::fromServiceSpecificError(error, android::String8(msg.c_str()));
}

bool setBowState(std::string const& block_device, std::string const& state) {
    std::string bow_device = fs_mgr_find_bow_device(block_device);
    if (bow_device.empty()) return false;

    if (!android::base::WriteStringToFile(state, bow_device + "/bow/state")) {
        PLOG(ERROR) << "Failed to write to file " << bow_device + "/bow/state";
        return false;
    }

    return true;
}

}  // namespace

Status cp_supportsCheckpoint(bool& result) {
    result = false;

    for (const auto& entry : fstab_default) {
        if (entry.fs_mgr_flags.checkpoint_blk || entry.fs_mgr_flags.checkpoint_fs) {
            result = true;
            return Status::ok();
        }
    }
    return Status::ok();
}

Status cp_supportsBlockCheckpoint(bool& result) {
    result = false;

    for (const auto& entry : fstab_default) {
        if (entry.fs_mgr_flags.checkpoint_blk) {
            result = true;
            return Status::ok();
        }
    }
    return Status::ok();
}

Status cp_supportsFileCheckpoint(bool& result) {
    result = false;

    for (const auto& entry : fstab_default) {
        if (entry.fs_mgr_flags.checkpoint_fs) {
            result = true;
            return Status::ok();
        }
    }
    return Status::ok();
}

Status cp_startCheckpoint(int retry) {
    bool result;
    if (!cp_supportsCheckpoint(result).isOk() || !result)
        return error(ENOTSUP, "Checkpoints not supported");

    if (retry < -1) return error(EINVAL, "Retry count must be more than -1");
    std::string content = std::to_string(retry + 1);
    if (retry == -1) {
        android::sp<IBootControl> module = IBootControl::getService();
        if (module) {
            std::string suffix;
            auto cb = [&suffix](hidl_string s) { suffix = s; };
            if (module->getSuffix(module->getCurrentSlot(), cb).isOk()) content += " " + suffix;
        }
    }
    if (!android::base::WriteStringToFile(content, kMetadataCPFile))
        return error("Failed to write checkpoint file");
    return Status::ok();
}

namespace {

volatile bool isCheckpointing = false;

volatile bool needsCheckpointWasCalled = false;

// Protects isCheckpointing, needsCheckpointWasCalled and code that makes decisions based on status
// of isCheckpointing
std::mutex isCheckpointingLock;
}

Status cp_commitChanges() {
    std::lock_guard<std::mutex> lock(isCheckpointingLock);

    if (!isCheckpointing) {
        return Status::ok();
    }
    if (android::base::GetProperty("persist.vold.dont_commit_checkpoint", "0") == "1") {
        LOG(WARNING)
            << "NOT COMMITTING CHECKPOINT BECAUSE persist.vold.dont_commit_checkpoint IS 1";
        return Status::ok();
    }
    // TWRP should not mark errors on slots
    // android::sp<IBootControl> module = IBootControl::getService();
    // if (module) {
    //     CommandResult cr;
    //     module->markBootSuccessful([&cr](CommandResult result) { cr = result; });
    //     if (!cr.success)
    //         return error(EINVAL, "Error marking booted successfully: " + std::string(cr.errMsg));
    //     LOG(INFO) << "Marked slot as booted successfully.";
    //     // Clears the warm reset flag for next reboot.
    //     if (!SetProperty("ota.warm_reset", "0")) {
    //         LOG(WARNING) << "Failed to reset the warm reset flag";
    //     }
    // }
    // Must take action for list of mounted checkpointed things here
    // To do this, we walk the list of mounted file systems.
    // But we also need to get the matching fstab entries to see
    // the original flags
    std::string err_str;

    Fstab mounts;
    if (!ReadFstabFromFile("/proc/mounts", &mounts)) {
        return error(EINVAL, "Failed to get /proc/mounts");
    }

    // Walk mounted file systems
    for (const auto& mount_rec : mounts) {
        const auto fstab_rec = GetEntryForMountPoint(&fstab_default, mount_rec.mount_point);
        if (!fstab_rec) continue;

        if (fstab_rec->fs_mgr_flags.checkpoint_fs) {
            if (fstab_rec->fs_type == "f2fs") {
                std::string options = mount_rec.fs_options + ",checkpoint=enable";
                if (mount(mount_rec.blk_device.c_str(), mount_rec.mount_point.c_str(), "none",
                          MS_REMOUNT | fstab_rec->flags, options.c_str())) {
                    return error(EINVAL, "Failed to remount");
                }
            }
        } else if (fstab_rec->fs_mgr_flags.checkpoint_blk) {
            if (!setBowState(mount_rec.blk_device, "2"))
                return error(EINVAL, "Failed to set bow state");
        }
    }
    SetProperty("vold.checkpoint_committed", "1");
    LOG(INFO) << "Checkpoint has been committed.";
    isCheckpointing = false;
    if (!android::base::RemoveFileIfExists(kMetadataCPFile, &err_str))
        return error(err_str.c_str());

    return Status::ok();
}

namespace {
void abort_metadata_file() {
    std::string oldContent, newContent;
    int retry = 0;
    struct stat st;
    int result = stat(kMetadataCPFile.c_str(), &st);

    // If the file doesn't exist, we aren't managing a checkpoint retry counter
    if (result != 0) return;
    if (!android::base::ReadFileToString(kMetadataCPFile, &oldContent)) {
        PLOG(ERROR) << "Failed to read checkpoint file";
        return;
    }
    std::string retryContent = oldContent.substr(0, oldContent.find_first_of(" "));

    if (!android::base::ParseInt(retryContent, &retry)) {
        PLOG(ERROR) << "Could not parse retry count";
        return;
    }
    if (retry > 0) {
        newContent = "0";
        if (!android::base::WriteStringToFile(newContent, kMetadataCPFile))
            PLOG(ERROR) << "Could not write checkpoint file";
    }
}
}  // namespace

void cp_abortChanges(const std::string& message, bool retry) {
    if (!cp_needsCheckpoint()) return;
    if (!retry) abort_metadata_file();
    android_reboot(ANDROID_RB_RESTART2, 0, message.c_str());
}

bool cp_needsRollback() {
    std::string content;
    bool ret;

    ret = android::base::ReadFileToString(kMetadataCPFile, &content);
    if (ret) {
        if (content == "0") return true;
        if (content.substr(0, 3) == "-1 ") {
            std::string oldSuffix = content.substr(3);
            android::sp<IBootControl> module = IBootControl::getService();
            std::string newSuffix;

            if (module) {
                auto cb = [&newSuffix](hidl_string s) { newSuffix = s; };
                module->getSuffix(module->getCurrentSlot(), cb);
                if (oldSuffix == newSuffix) return true;
            }
        }
    }
    return false;
}

bool cp_needsCheckpoint() {
    std::lock_guard<std::mutex> lock(isCheckpointingLock);

    // Make sure we only return true during boot. See b/138952436 for discussion
    if (needsCheckpointWasCalled) return isCheckpointing;
    needsCheckpointWasCalled = true;

    bool ret;
    std::string content;
    android::sp<IBootControl> module = IBootControl::getService();

    if (isCheckpointing) return isCheckpointing;

    if (module && module->isSlotMarkedSuccessful(module->getCurrentSlot()) == BoolResult::FALSE) {
        isCheckpointing = true;
        return true;
    }
    ret = android::base::ReadFileToString(kMetadataCPFile, &content);
    if (ret) {
        ret = content != "0";
        isCheckpointing = ret;
        return ret;
    }
    return false;
}

bool cp_isCheckpointing() {
    return isCheckpointing;
}

namespace {
const std::string kSleepTimeProp = "ro.sys.cp_msleeptime";
const uint32_t msleeptime_default = 1000;  // 1 s
const uint32_t max_msleeptime = 3600000;   // 1 h

const std::string kMinFreeBytesProp = "ro.sys.cp_min_free_bytes";
const uint64_t min_free_bytes_default = 100 * (1 << 20);  // 100 MiB

const std::string kCommitOnFullProp = "ro.sys.cp_commit_on_full";
const bool commit_on_full_default = true;

static void cp_healthDaemon(std::string mnt_pnt, std::string blk_device, bool is_fs_cp) {
    struct statvfs data;
    uint32_t msleeptime = GetUintProperty(kSleepTimeProp, msleeptime_default, max_msleeptime);
    uint64_t min_free_bytes =
        GetUintProperty(kMinFreeBytesProp, min_free_bytes_default, (uint64_t)-1);
    bool commit_on_full = GetBoolProperty(kCommitOnFullProp, commit_on_full_default);

    struct timespec req;
    req.tv_sec = msleeptime / 1000;
    msleeptime %= 1000;
    req.tv_nsec = msleeptime * 1000000;
    while (isCheckpointing) {
        uint64_t free_bytes = 0;
        if (is_fs_cp) {
            statvfs(mnt_pnt.c_str(), &data);
            free_bytes = ((uint64_t) data.f_bavail) * data.f_frsize;
        } else {
            std::string bow_device = fs_mgr_find_bow_device(blk_device);
            if (!bow_device.empty()) {
                std::string content;
                if (android::base::ReadFileToString(bow_device + "/bow/free", &content)) {
                    free_bytes = std::strtoull(content.c_str(), NULL, 10);
                }
            }
        }
        if (free_bytes < min_free_bytes) {
            if (commit_on_full) {
                LOG(INFO) << "Low space for checkpointing. Commiting changes";
                cp_commitChanges();
                break;
            } else {
                LOG(INFO) << "Low space for checkpointing. Rebooting";
                cp_abortChanges("checkpoint,low_space", false);
                break;
            }
        }
        nanosleep(&req, NULL);
    }
}

}  // namespace

Status cp_prepareCheckpoint() {
    // Log to notify CTS - see b/137924328 for context
    LOG(INFO) << "cp_prepareCheckpoint called";
    std::lock_guard<std::mutex> lock(isCheckpointingLock);
    if (!isCheckpointing) {
        return Status::ok();
    }

    Fstab mounts;
    if (!ReadFstabFromFile("/proc/mounts", &mounts)) {
        return error(EINVAL, "Failed to get /proc/mounts");
    }

    for (const auto& mount_rec : mounts) {
        const auto fstab_rec = GetEntryForMountPoint(&fstab_default, mount_rec.mount_point);
        if (!fstab_rec) continue;

        if (fstab_rec->fs_mgr_flags.checkpoint_blk) {
            android::base::unique_fd fd(
                TEMP_FAILURE_RETRY(open(mount_rec.mount_point.c_str(), O_RDONLY | O_CLOEXEC)));
            if (fd == -1) {
                PLOG(ERROR) << "Failed to open mount point" << mount_rec.mount_point;
                continue;
            }

            struct fstrim_range range = {};
            range.len = ULLONG_MAX;
            nsecs_t start = systemTime(SYSTEM_TIME_BOOTTIME);
            if (ioctl(fd, FITRIM, &range)) {
                PLOG(ERROR) << "Failed to trim " << mount_rec.mount_point;
                continue;
            }
            nsecs_t time = systemTime(SYSTEM_TIME_BOOTTIME) - start;
            LOG(INFO) << "Trimmed " << range.len << " bytes on " << mount_rec.mount_point << " in "
                      << nanoseconds_to_milliseconds(time) << "ms for checkpoint";

            setBowState(mount_rec.blk_device, "1");
        }
        if (fstab_rec->fs_mgr_flags.checkpoint_blk || fstab_rec->fs_mgr_flags.checkpoint_fs) {
            std::thread(cp_healthDaemon, std::string(mount_rec.mount_point),
                        std::string(mount_rec.blk_device),
                        fstab_rec->fs_mgr_flags.checkpoint_fs == 1)
                .detach();
        }
    }
    return Status::ok();
}

namespace {
const int kSectorSize = 512;

typedef uint64_t sector_t;

struct log_entry {
    sector_t source;  // in sectors of size kSectorSize
    sector_t dest;    // in sectors of size kSectorSize
    uint32_t size;    // in bytes
    uint32_t checksum;
} __attribute__((packed));

struct log_sector_v1_0 {
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;
    uint32_t block_size;
    uint32_t count;
    uint32_t sequence;
    uint64_t sector0;
} __attribute__((packed));

// MAGIC is BOW in ascii
const int kMagic = 0x00574f42;
// Partially restored MAGIC is WOB in ascii
const int kPartialRestoreMagic = 0x00424f57;

void crc32(const void* data, size_t n_bytes, uint32_t* crc) {
    static uint32_t table[0x100] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535,
        0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD,
        0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D,
        0x6DDDE4EB, 0xF4D4B551, 0x83D385C7, 0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
        0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4,
        0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
        0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59, 0x26D930AC,
        0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB,
        0xB6662D3D,

        0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5,
        0xE8B8D433, 0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
        0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED,
        0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
        0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65, 0x4DB26158, 0x3AB551CE, 0xA3BC0074,
        0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC,
        0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C,
        0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B,
        0xC0BA6CAD,

        0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615,
        0x73DC1683, 0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D,
        0x0A00AE27, 0x7D079EB1, 0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D,
        0x806567CB, 0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
        0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4,
        0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B, 0xD80D2BDA, 0xAF0A1B4C,
        0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79, 0xCB61B38C,
        0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
        0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B,
        0x5BDEAE1D,

        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785,
        0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D,
        0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD,
        0xF6B9265B, 0x6FB077E1, 0x18B74777, 0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
        0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354,
        0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
        0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9, 0xBDBDF21C,
        0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
        0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B,
        0x2D02EF8D};

    for (size_t i = 0; i < n_bytes; ++i) {
        *crc ^= ((uint8_t*)data)[i];
        *crc = table[(uint8_t)*crc] ^ *crc >> 8;
    }
}

// A map of relocations.
// The map must be initialized so that relocations[0] = 0
// During restore, we replay the log records in reverse, copying from dest to
// source
// To validate, we must be able to read the 'dest' sectors as though they had
// been copied but without actually copying. This map represents how the sectors
// would have been moved. To read a sector s, find the index <= s and read
// relocations[index] + s - index
typedef std::map<sector_t, sector_t> Relocations;

void relocate(Relocations& relocations, sector_t dest, sector_t source, int count) {
    // Find first one we're equal to or greater than
    auto s = --relocations.upper_bound(source);

    // Take slice
    Relocations slice;
    slice[dest] = source - s->first + s->second;
    ++s;

    // Add rest of elements
    for (; s != relocations.end() && s->first < source + count; ++s)
        slice[dest - source + s->first] = s->second;

    // Split range at end of dest
    auto dest_end = --relocations.upper_bound(dest + count);
    relocations[dest + count] = dest + count - dest_end->first + dest_end->second;

    // Remove all elements in [dest, dest + count)
    relocations.erase(relocations.lower_bound(dest), relocations.lower_bound(dest + count));

    // Add new elements
    relocations.insert(slice.begin(), slice.end());
}

// A map of sectors that have been written to.
// The final entry must always be False.
// When we restart the restore after an interruption, we must take care that
// when we copy from dest to source, that the block we copy to was not
// previously copied from.
// i e. A->B C->A; If we replay this sequence, we end up copying C->B
// We must save our partial result whenever we finish a page, or when we copy
// to a location that was copied from earlier (our source is an earlier dest)
typedef std::map<sector_t, bool> Used_Sectors;

bool checkCollision(Used_Sectors& used_sectors, sector_t start, sector_t end) {
    auto second_overlap = used_sectors.upper_bound(start);
    auto first_overlap = --second_overlap;

    if (first_overlap->second) {
        return true;
    } else if (second_overlap != used_sectors.end() && second_overlap->first < end) {
        return true;
    }
    return false;
}

void markUsed(Used_Sectors& used_sectors, sector_t start, sector_t end) {
    auto start_pos = used_sectors.insert_or_assign(start, true).first;
    auto end_pos = used_sectors.insert_or_assign(end, false).first;

    if (start_pos == used_sectors.begin() || !std::prev(start_pos)->second) {
        start_pos++;
    }
    if (std::next(end_pos) != used_sectors.end() && !std::next(end_pos)->second) {
        end_pos++;
    }
    if (start_pos->first < end_pos->first) {
        used_sectors.erase(start_pos, end_pos);
    }
}

// Restores the given log_entry's data from dest -> source
// If that entry is a log sector, set the magic to kPartialRestoreMagic and flush.
void restoreSector(int device_fd, Used_Sectors& used_sectors, std::vector<char>& ls_buffer,
                   log_entry* le, std::vector<char>& buffer) {
    log_sector_v1_0& ls = *reinterpret_cast<log_sector_v1_0*>(&ls_buffer[0]);
    uint32_t index = le - ((log_entry*)&ls_buffer[ls.header_size]);
    int count = (le->size - 1) / kSectorSize + 1;

    if (checkCollision(used_sectors, le->source, le->source + count)) {
        fsync(device_fd);
        lseek64(device_fd, 0, SEEK_SET);
        ls.count = index + 1;
        ls.magic = kPartialRestoreMagic;
        write(device_fd, &ls_buffer[0], ls.block_size);
        fsync(device_fd);
        used_sectors.clear();
        used_sectors[0] = false;
    }

    markUsed(used_sectors, le->dest, le->dest + count);

    if (index == 0 && ls.sequence != 0) {
        log_sector_v1_0* next = reinterpret_cast<log_sector_v1_0*>(&buffer[0]);
        if (next->magic == kMagic) {
            next->magic = kPartialRestoreMagic;
        }
    }

    lseek64(device_fd, le->source * kSectorSize, SEEK_SET);
    write(device_fd, &buffer[0], le->size);

    if (index == 0) {
        fsync(device_fd);
    }
}

// Read from the device
// If we are validating, the read occurs as though the relocations had happened
std::vector<char> relocatedRead(int device_fd, Relocations const& relocations, bool validating,
                                sector_t sector, uint32_t size, uint32_t block_size) {
    if (!validating) {
        std::vector<char> buffer(size);
        lseek64(device_fd, sector * kSectorSize, SEEK_SET);
        read(device_fd, &buffer[0], size);
        return buffer;
    }

    std::vector<char> buffer(size);
    for (uint32_t i = 0; i < size; i += block_size, sector += block_size / kSectorSize) {
        auto relocation = --relocations.upper_bound(sector);
        lseek64(device_fd, (sector + relocation->second - relocation->first) * kSectorSize,
                SEEK_SET);
        read(device_fd, &buffer[i], block_size);
    }

    return buffer;
}

}  // namespace

Status cp_restoreCheckpoint(const std::string& blockDevice, int restore_limit) {
    bool validating = true;
    std::string action = "Validating";
    int restore_count = 0;

    for (;;) {
        Relocations relocations;
        relocations[0] = 0;
        Status status = Status::ok();

        LOG(INFO) << action << " checkpoint on " << blockDevice;
        android::base::unique_fd device_fd(open(blockDevice.c_str(), O_RDWR | O_CLOEXEC));
        if (device_fd < 0) return error("Cannot open " + blockDevice);

        log_sector_v1_0 original_ls;
        read(device_fd, reinterpret_cast<char*>(&original_ls), sizeof(original_ls));
        if (original_ls.magic == kPartialRestoreMagic) {
            validating = false;
            action = "Restoring";
        } else if (original_ls.magic != kMagic) {
            return error(EINVAL, "No magic");
        }

        LOG(INFO) << action << " " << original_ls.sequence << " log sectors";

        for (int sequence = original_ls.sequence; sequence >= 0 && status.isOk(); sequence--) {
            auto ls_buffer = relocatedRead(device_fd, relocations, validating, 0,
                                           original_ls.block_size, original_ls.block_size);
            log_sector_v1_0& ls = *reinterpret_cast<log_sector_v1_0*>(&ls_buffer[0]);

            Used_Sectors used_sectors;
            used_sectors[0] = false;

            if (ls.magic != kMagic && (ls.magic != kPartialRestoreMagic || validating)) {
                status = error(EINVAL, "No magic");
                break;
            }

            if (ls.block_size != original_ls.block_size) {
                status = error(EINVAL, "Block size mismatch");
                break;
            }

            if ((int)ls.sequence != sequence) {
                status = error(EINVAL, "Expecting log sector " + std::to_string(sequence) +
                                           " but got " + std::to_string(ls.sequence));
                break;
            }

            LOG(INFO) << action << " from log sector " << ls.sequence;
            for (log_entry* le =
                     reinterpret_cast<log_entry*>(&ls_buffer[ls.header_size]) + ls.count - 1;
                 le >= reinterpret_cast<log_entry*>(&ls_buffer[ls.header_size]); --le) {
                // This is very noisy - limit to DEBUG only
                LOG(VERBOSE) << action << " " << le->size << " bytes from sector " << le->dest
                             << " to " << le->source << " with checksum " << std::hex
                             << le->checksum;

                auto buffer = relocatedRead(device_fd, relocations, validating, le->dest, le->size,
                                            ls.block_size);
                uint32_t checksum = le->source / (ls.block_size / kSectorSize);
                for (size_t i = 0; i < le->size; i += ls.block_size) {
                    crc32(&buffer[i], ls.block_size, &checksum);
                }

                if (le->checksum && checksum != le->checksum) {
                    status = error(EINVAL, "Checksums don't match");
                    break;
                }

                if (validating) {
                    relocate(relocations, le->source, le->dest, (le->size - 1) / kSectorSize + 1);
                } else {
                    restoreSector(device_fd, used_sectors, ls_buffer, le, buffer);
                    restore_count++;
                    if (restore_limit && restore_count >= restore_limit) {
                        status = error(EAGAIN, "Hit the test limit");
                        break;
                    }
                }
            }
        }

        if (!status.isOk()) {
            if (!validating) {
                LOG(ERROR) << "Checkpoint restore failed even though checkpoint validation passed";
                return status;
            }

            LOG(WARNING) << "Checkpoint validation failed - attempting to roll forward";
            auto buffer = relocatedRead(device_fd, relocations, false, original_ls.sector0,
                                        original_ls.block_size, original_ls.block_size);
            lseek64(device_fd, 0, SEEK_SET);
            write(device_fd, &buffer[0], original_ls.block_size);
            return Status::ok();
        }

        if (!validating) break;

        validating = false;
        action = "Restoring";
    }

    return Status::ok();
}

Status cp_markBootAttempt() {
    std::string oldContent, newContent;
    int retry = 0;
    struct stat st;
    int result = stat(kMetadataCPFile.c_str(), &st);

    // If the file doesn't exist, we aren't managing a checkpoint retry counter
    if (result != 0) return Status::ok();
    if (!android::base::ReadFileToString(kMetadataCPFile, &oldContent))
        return error("Failed to read checkpoint file");
    std::string retryContent = oldContent.substr(0, oldContent.find_first_of(" "));

    if (!android::base::ParseInt(retryContent, &retry))
        return error(EINVAL, "Could not parse retry count");
    if (retry > 0) {
        retry--;

        newContent = std::to_string(retry);
        if (!android::base::WriteStringToFile(newContent, kMetadataCPFile))
            return error("Could not write checkpoint file");
    }
    return Status::ok();
}

void cp_resetCheckpoint() {
    std::lock_guard<std::mutex> lock(isCheckpointingLock);
    needsCheckpointWasCalled = false;
}
