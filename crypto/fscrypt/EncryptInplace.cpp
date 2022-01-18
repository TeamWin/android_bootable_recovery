/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "EncryptInplace.h"

#include <ext4_utils/ext4.h>
#include <ext4_utils/ext4_utils.h>
#include <f2fs_sparseblock.h>
#include <fcntl.h>
#include <time.h>

#include <algorithm>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>

enum EncryptInPlaceError {
    kSuccess,
    kFailed,
    kFilesystemNotFound,
};

static uint64_t round_up(uint64_t val, size_t amount) {
    if (val % amount) val += amount - (val % amount);
    return val;
}

class InPlaceEncrypter {
  public:
    bool EncryptInPlace(const std::string& crypto_blkdev, const std::string& real_blkdev,
                        uint64_t nr_sec, bool set_progress_properties);
    bool ProcessUsedBlock(uint64_t block_num);

  private:
    // aligned 32K writes tends to make flash happy.
    // SD card association recommends it.
    static const size_t kIOBufferSize = 32768;

    // Avoid spamming the logs.  Print the "Encrypting blocks" log message once
    // every 10000 blocks (which is usually every 40 MB or so), and once at the end.
    static const int kLogInterval = 10000;

    std::string DescribeFilesystem();
    void InitFs(const std::string& fs_type, uint64_t blocks_to_encrypt, uint64_t total_blocks,
                unsigned int block_size);
    void UpdateProgress(size_t blocks, bool done);
    bool EncryptPendingData();
    bool DoEncryptInPlace();

    // ext4 methods
    bool ReadExt4BlockBitmap(uint32_t group, uint8_t* buf);
    uint64_t FirstBlockInGroup(uint32_t group);
    uint32_t NumBlocksInGroup(uint32_t group);
    uint32_t NumBaseMetaBlocksInGroup(uint64_t group);
    EncryptInPlaceError EncryptInPlaceExt4();

    // f2fs methods
    EncryptInPlaceError EncryptInPlaceF2fs();

    std::string real_blkdev_;
    std::string crypto_blkdev_;
    uint64_t nr_sec_;
    bool set_progress_properties_;

    android::base::unique_fd realfd_;
    android::base::unique_fd cryptofd_;

    time_t time_started_;
    int remaining_time_;

    std::string fs_type_;
    uint64_t blocks_done_;
    uint64_t blocks_to_encrypt_;
    unsigned int block_size_;
    unsigned int cur_pct_;

    std::vector<uint8_t> io_buffer_;
    uint64_t first_pending_block_;
    size_t blocks_pending_;
};

std::string InPlaceEncrypter::DescribeFilesystem() {
    if (fs_type_.empty())
        return "full block device " + real_blkdev_;
    else
        return fs_type_ + " filesystem on " + real_blkdev_;
}

// Finishes initializing the encrypter, now that the filesystem details are known.
void InPlaceEncrypter::InitFs(const std::string& fs_type, uint64_t blocks_to_encrypt,
                              uint64_t total_blocks, unsigned int block_size) {
    fs_type_ = fs_type;
    blocks_done_ = 0;
    blocks_to_encrypt_ = blocks_to_encrypt;
    block_size_ = block_size;
    cur_pct_ = 0;

    // Allocate the I/O buffer.  kIOBufferSize should always be a multiple of
    // the filesystem block size, but round it up just in case.
    io_buffer_.resize(round_up(kIOBufferSize, block_size));
    first_pending_block_ = 0;
    blocks_pending_ = 0;

    LOG(INFO) << "Encrypting " << DescribeFilesystem() << " in-place via " << crypto_blkdev_;
    LOG(INFO) << blocks_to_encrypt << " blocks (" << (blocks_to_encrypt * block_size) / 1000000
              << " MB) of " << total_blocks << " blocks are in-use";
}

void InPlaceEncrypter::UpdateProgress(size_t blocks, bool done) {
    // A log message already got printed for blocks_done_ if one was due, so the
    // next message will be due at the *next* block rounded up to kLogInterval.
    uint64_t blocks_next_msg = round_up(blocks_done_ + 1, kLogInterval);

    blocks_done_ += blocks;

    // Ensure that a log message gets printed at the end, but not if one was
    // already printed due to the block count being a multiple of kLogInterval.
    // E.g. we want to show "50000 of 50327" and then "50327 of "50327", but not
    // "50000 of 50000" and then redundantly "50000 of 50000" again.
    if (done && blocks_done_ % kLogInterval != 0) blocks_next_msg = blocks_done_;

    if (blocks_done_ >= blocks_next_msg)
        LOG(DEBUG) << "Encrypted " << blocks_next_msg << " of " << blocks_to_encrypt_ << " blocks";

    if (!set_progress_properties_) return;

    uint64_t new_pct;
    if (done) {
        new_pct = 100;
    } else {
        new_pct = (blocks_done_ * 100) / std::max<uint64_t>(blocks_to_encrypt_, 1);
        new_pct = std::min<uint64_t>(new_pct, 99);
    }
    if (new_pct > cur_pct_) {
        cur_pct_ = new_pct;
        android::base::SetProperty("vold.encrypt_progress", std::to_string(new_pct));
    }

    if (cur_pct_ >= 5) {
        struct timespec time_now;
        if (clock_gettime(CLOCK_MONOTONIC, &time_now)) {
            PLOG(WARNING) << "Error getting time while updating encryption progress";
        } else {
            double elapsed_time = difftime(time_now.tv_sec, time_started_);

            uint64_t remaining_blocks = 0;
            if (blocks_done_ < blocks_to_encrypt_)
                remaining_blocks = blocks_to_encrypt_ - blocks_done_;

            int remaining_time = 0;
            if (blocks_done_ != 0)
                remaining_time = (int)(elapsed_time * remaining_blocks / blocks_done_);

            // Change time only if not yet set, lower, or a lot higher for
            // best user experience
            if (remaining_time_ == -1 || remaining_time < remaining_time_ ||
                remaining_time > remaining_time_ + 60) {
                remaining_time_ = remaining_time;
                android::base::SetProperty("vold.encrypt_time_remaining",
                                           std::to_string(remaining_time));
            }
        }
    }
}

bool InPlaceEncrypter::EncryptPendingData() {
    if (blocks_pending_ == 0) return true;

    ssize_t bytes = blocks_pending_ * block_size_;
    uint64_t offset = first_pending_block_ * block_size_;

    if (pread64(realfd_, &io_buffer_[0], bytes, offset) != bytes) {
        PLOG(ERROR) << "Error reading real_blkdev " << real_blkdev_ << " for inplace encrypt";
        return false;
    }

    if (pwrite64(cryptofd_, &io_buffer_[0], bytes, offset) != bytes) {
        PLOG(ERROR) << "Error writing crypto_blkdev " << crypto_blkdev_ << " for inplace encrypt";
        return false;
    }

    UpdateProgress(blocks_pending_, false);

    blocks_pending_ = 0;
    return true;
}

bool InPlaceEncrypter::ProcessUsedBlock(uint64_t block_num) {
    // Flush if the amount of pending data has reached the I/O buffer size, if
    // there's a gap between the pending blocks and the next block (due to
    // block(s) not being used by the filesystem and thus not needing
    // encryption), or if the next block will be aligned to the I/O buffer size.
    if (blocks_pending_ * block_size_ == io_buffer_.size() ||
        block_num != first_pending_block_ + blocks_pending_ ||
        (block_num * block_size_) % io_buffer_.size() == 0) {
        if (!EncryptPendingData()) return false;
        first_pending_block_ = block_num;
    }
    blocks_pending_++;
    return true;
}

// Reads the block bitmap for block group |group| into |buf|.
bool InPlaceEncrypter::ReadExt4BlockBitmap(uint32_t group, uint8_t* buf) {
    uint64_t offset = (uint64_t)aux_info.bg_desc[group].bg_block_bitmap * info.block_size;
    if (pread64(realfd_, buf, info.block_size, offset) != (ssize_t)info.block_size) {
        PLOG(ERROR) << "Failed to read block bitmap for block group " << group;
        return false;
    }
    return true;
}

uint64_t InPlaceEncrypter::FirstBlockInGroup(uint32_t group) {
    return aux_info.first_data_block + (group * (uint64_t)info.blocks_per_group);
}

uint32_t InPlaceEncrypter::NumBlocksInGroup(uint32_t group) {
    uint64_t remaining = aux_info.len_blocks - FirstBlockInGroup(group);
    return std::min<uint64_t>(info.blocks_per_group, remaining);
}

// In block groups with an uninitialized block bitmap, we only need to encrypt
// the backup superblock and the block group descriptors (if they are present).
uint32_t InPlaceEncrypter::NumBaseMetaBlocksInGroup(uint64_t group) {
    if (!ext4_bg_has_super_block(group)) return 0;
    return 1 + aux_info.bg_desc_blocks;
}

EncryptInPlaceError InPlaceEncrypter::EncryptInPlaceExt4() {
    if (setjmp(setjmp_env))  // NOLINT
        return kFilesystemNotFound;

    if (read_ext(realfd_, 0) != 0) return kFilesystemNotFound;

    LOG(DEBUG) << "ext4 filesystem has " << aux_info.groups << " block groups";

    uint64_t blocks_to_encrypt = 0;
    for (uint32_t group = 0; group < aux_info.groups; group++) {
        if (aux_info.bg_desc[group].bg_flags & EXT4_BG_BLOCK_UNINIT)
            blocks_to_encrypt += NumBaseMetaBlocksInGroup(group);
        else
            blocks_to_encrypt +=
                    (NumBlocksInGroup(group) - aux_info.bg_desc[group].bg_free_blocks_count);
    }

    InitFs("ext4", blocks_to_encrypt, aux_info.len_blocks, info.block_size);

    // Encrypt each block group.
    std::vector<uint8_t> block_bitmap(info.block_size);
    for (uint32_t group = 0; group < aux_info.groups; group++) {
        if (!ReadExt4BlockBitmap(group, &block_bitmap[0])) return kFailed;

        uint64_t first_block_num = FirstBlockInGroup(group);
        bool uninit = (aux_info.bg_desc[group].bg_flags & EXT4_BG_BLOCK_UNINIT);
        uint32_t block_count = uninit ? NumBaseMetaBlocksInGroup(group) : NumBlocksInGroup(group);

        // Encrypt each used block in the block group.
        for (uint32_t i = 0; i < block_count; i++) {
            if (uninit || bitmap_get_bit(&block_bitmap[0], i))
                ProcessUsedBlock(first_block_num + i);
        }
    }
    return kSuccess;
}

static int encrypt_f2fs_block(uint64_t block_num, void* _encrypter) {
    InPlaceEncrypter* encrypter = reinterpret_cast<InPlaceEncrypter*>(_encrypter);
    if (!encrypter->ProcessUsedBlock(block_num)) return -1;
    return 0;
}

EncryptInPlaceError InPlaceEncrypter::EncryptInPlaceF2fs() {
    std::unique_ptr<struct f2fs_info, void (*)(struct f2fs_info*)> fs_info(
            generate_f2fs_info(realfd_), free_f2fs_info);
    if (!fs_info) return kFilesystemNotFound;

    InitFs("f2fs", get_num_blocks_used(fs_info.get()), fs_info->total_blocks, fs_info->block_size);
    if (run_on_used_blocks(0, fs_info.get(), encrypt_f2fs_block, this) != 0) return kFailed;
    return kSuccess;
}

bool InPlaceEncrypter::DoEncryptInPlace() {
    EncryptInPlaceError rc;

    rc = EncryptInPlaceExt4();
    if (rc != kFilesystemNotFound) return rc == kSuccess;

    rc = EncryptInPlaceF2fs();
    if (rc != kFilesystemNotFound) return rc == kSuccess;

    LOG(WARNING) << "No recognized filesystem found on " << real_blkdev_
                 << ".  Falling back to encrypting the full block device.";
    InitFs("", nr_sec_, nr_sec_, 512);
    for (uint64_t i = 0; i < nr_sec_; i++) {
        if (!ProcessUsedBlock(i)) return false;
    }
    return true;
}

bool InPlaceEncrypter::EncryptInPlace(const std::string& crypto_blkdev,
                                      const std::string& real_blkdev, uint64_t nr_sec,
                                      bool set_progress_properties) {
    struct timespec time_started = {0};

    real_blkdev_ = real_blkdev;
    crypto_blkdev_ = crypto_blkdev;
    nr_sec_ = nr_sec;
    set_progress_properties_ = set_progress_properties;

    realfd_.reset(open64(real_blkdev.c_str(), O_RDONLY | O_CLOEXEC));
    if (realfd_ < 0) {
        PLOG(ERROR) << "Error opening real_blkdev " << real_blkdev << " for inplace encrypt";
        return false;
    }

    cryptofd_.reset(open64(crypto_blkdev.c_str(), O_WRONLY | O_CLOEXEC));
    if (cryptofd_ < 0) {
        PLOG(ERROR) << "Error opening crypto_blkdev " << crypto_blkdev << " for inplace encrypt";
        return false;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &time_started)) {
        PLOG(WARNING) << "Error getting time at start of in-place encryption";
        // Note - continue anyway - we'll run with 0
    }
    time_started_ = time_started.tv_sec;
    remaining_time_ = -1;

    bool success = DoEncryptInPlace();

    if (success) success &= EncryptPendingData();

    if (success && fsync(cryptofd_) != 0) {
        PLOG(ERROR) << "Error syncing " << crypto_blkdev_;
        success = false;
    }

    if (!success) {
        LOG(ERROR) << "In-place encryption of " << DescribeFilesystem() << " failed";
        return false;
    }
    if (blocks_done_ != blocks_to_encrypt_) {
        LOG(WARNING) << "blocks_to_encrypt (" << blocks_to_encrypt_
                     << ") was incorrect; we actually encrypted " << blocks_done_
                     << " blocks.  Encryption progress was inaccurate";
    }
    // Make sure vold.encrypt_progress gets set to 100.
    UpdateProgress(0, true);
    LOG(INFO) << "Successfully encrypted " << DescribeFilesystem();
    return true;
}

// Encrypts |real_blkdev| in-place by reading the data from |real_blkdev| and
// writing it to |crypto_blkdev|, which should be a dm-crypt or dm-default-key
// device backed by |real_blkdev|.  The size to encrypt is |nr_sec| 512-byte
// sectors; however, if a filesystem is detected, then its size will be used
// instead, and only the in-use blocks of the filesystem will be encrypted.
bool encrypt_inplace(const std::string& crypto_blkdev, const std::string& real_blkdev,
                     uint64_t nr_sec, bool set_progress_properties) {
    LOG(DEBUG) << "encrypt_inplace(" << crypto_blkdev << ", " << real_blkdev << ", " << nr_sec
               << ", " << (set_progress_properties ? "true" : "false") << ")";

    InPlaceEncrypter encrypter;
    return encrypter.EncryptInPlace(crypto_blkdev, real_blkdev, nr_sec, set_progress_properties);
}
