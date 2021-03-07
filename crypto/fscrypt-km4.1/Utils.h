/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_VOLD_UTILS_H
#define ANDROID_VOLD_UTILS_H

#include "KeyBuffer.h"

#include <android-base/macros.h>
#include <android-base/unique_fd.h>
#include <cutils/multiuser.h>
#include <selinux/selinux.h>
#include <utils/Errors.h>

#include <chrono>
#include <string>
#include <vector>

struct DIR;

static const char* kPropFuse = "persist.sys.fuse";
static const char* kVoldAppDataIsolationEnabled = "persist.sys.vold_app_data_isolation_enabled";
static const char* kExternalStorageSdcardfs = "external_storage.sdcardfs.enabled";

/* SELinux contexts used depending on the block device type */
extern security_context_t sBlkidContext;
extern security_context_t sBlkidUntrustedContext;
extern security_context_t sFsckContext;
extern security_context_t sFsckUntrustedContext;

// TODO remove this with better solution, b/64143519
extern bool sSleepOnUnmount;

std::string GetFuseMountPathForUser(userid_t user_id, const std::string& relative_upper_path);

android::status_t CreateDeviceNode(const std::string& path, dev_t dev);
android::status_t DestroyDeviceNode(const std::string& path);

android::status_t AbortFuseConnections();

int SetQuotaInherit(const std::string& path);
int SetQuotaProjectId(const std::string& path, long projectId);
/*
 * Creates and sets up an application-specific path on external
 * storage with the correct ACL and project ID (if needed).
 *
 * ONLY for use with app-specific data directories on external storage!
 * (eg, /Android/data/com.foo, /Android/obb/com.foo, etc.)
 */
int PrepareAppDirFromRoot(const std::string& path, const std::string& root, int appUid,
                          bool fixupExisting);

/* fs_prepare_dir wrapper that creates with SELinux context */
android::status_t PrepareDir(const std::string& path, mode_t mode, uid_t uid, gid_t gid);

/* Really unmounts the path, killing active processes along the way */
android::status_t ForceUnmount(const std::string& path);

/* Kills any processes using given path */
android::status_t KillProcessesUsingPath(const std::string& path);

/* Kills any processes using given mount prifix */
android::status_t KillProcessesWithMountPrefix(const std::string& path);

/* Creates bind mount from source to target */
android::status_t BindMount(const std::string& source, const std::string& target);

/** Creates a symbolic link to target */
android::status_t Symlink(const std::string& target, const std::string& linkpath);

/** Calls unlink(2) at linkpath */
android::status_t Unlink(const std::string& linkpath);

/** Creates the given directory if it is not already available */
android::status_t CreateDir(const std::string& dir, mode_t mode);

bool FindValue(const std::string& raw, const std::string& key, std::string* value);

/* Reads filesystem metadata from device at path */
android::status_t ReadMetadata(const std::string& path, std::string* fsType, std::string* fsUuid,
                      std::string* fsLabel);

/* Reads filesystem metadata from untrusted device at path */
android::status_t ReadMetadataUntrusted(const std::string& path, std::string* fsType, std::string* fsUuid,
                               std::string* fsLabel);

/* Returns either WEXITSTATUS() status, or a negative errno */
android::status_t ForkExecvp(const std::vector<std::string>& args, std::vector<std::string>* output = nullptr,
                    security_context_t context = nullptr);

pid_t ForkExecvpAsync(const std::vector<std::string>& args);

/* Gets block device size in bytes */
android::status_t GetBlockDevSize(int fd, uint64_t* size);
android::status_t GetBlockDevSize(const std::string& path, uint64_t* size);
/* Gets block device size in 512 byte sectors */
android::status_t GetBlockDev512Sectors(const std::string& path, uint64_t* nr_sec);

android::status_t ReadRandomBytes(size_t bytes, std::string& out);
android::status_t ReadRandomBytes(size_t bytes, char* buffer);
android::status_t GenerateRandomUuid(std::string& out);

/* Converts hex string to raw bytes, ignoring [ :-] */
android::status_t HexToStr(const std::string& hex, std::string& str);
/* Converts raw bytes to hex string */
android::status_t StrToHex(const std::string& str, std::string& hex);
/* Converts raw key bytes to hex string */
android::status_t StrToHex(const KeyBuffer& str, KeyBuffer& hex);
/* Normalize given hex string into consistent format */
android::status_t NormalizeHex(const std::string& in, std::string& out);

uint64_t GetFreeBytes(const std::string& path);
uint64_t GetTreeBytes(const std::string& path);

bool IsFilesystemSupported(const std::string& fsType);
bool IsSdcardfsUsed();
bool IsFuseDaemon(const pid_t pid);

/* Wipes contents of block device at given path */
android::status_t WipeBlockDevice(const std::string& path);

std::string BuildKeyPath(const std::string& partGuid);

std::string BuildDataSystemLegacyPath(userid_t userid);
std::string BuildDataSystemCePath(userid_t userid);
std::string BuildDataSystemDePath(userid_t userid);
std::string BuildDataMiscLegacyPath(userid_t userid);
std::string BuildDataMiscCePath(userid_t userid);
std::string BuildDataMiscDePath(userid_t userid);
std::string BuildDataProfilesDePath(userid_t userid);
std::string BuildDataVendorCePath(userid_t userid);
std::string BuildDataVendorDePath(userid_t userid);

std::string BuildDataPath(const std::string& volumeUuid);
std::string BuildDataMediaCePath(const std::string& volumeUuid, userid_t userid);
std::string BuildDataUserCePath(const std::string& volumeUuid, userid_t userid);
std::string BuildDataUserDePath(const std::string& volumeUuid, userid_t userid);

dev_t GetDevice(const std::string& path);

android::status_t EnsureDirExists(const std::string& path, mode_t mode, uid_t uid, gid_t gid);

android::status_t RestoreconRecursive(const std::string& path);

// TODO: promote to android::base
bool Readlinkat(int dirfd, const std::string& path, std::string* result);

// Handles dynamic major assignment for virtio-block
bool IsVirtioBlkDevice(unsigned int major);

android::status_t UnmountTreeWithPrefix(const std::string& prefix);
android::status_t UnmountTree(const std::string& mountPoint);

android::status_t DeleteDirContentsAndDir(const std::string& pathname);
android::status_t DeleteDirContents(const std::string& pathname);

android::status_t WaitForFile(const char* filename, std::chrono::nanoseconds timeout);

bool FsyncDirectory(const std::string& dirname);

bool writeStringToFile(const std::string& payload, const std::string& filename);

void ConfigureMaxDirtyRatioForFuse(const std::string& fuse_mount, unsigned int max_ratio);

void ConfigureReadAheadForFuse(const std::string& fuse_mount, size_t read_ahead_kb);

android::status_t MountUserFuse(userid_t user_id, const std::string& absolute_lower_path,
                       const std::string& relative_upper_path, android::base::unique_fd* fuse_fd);

android::status_t UnmountUserFuse(userid_t userId, const std::string& absolute_lower_path,
                         const std::string& relative_upper_path);

android::status_t PrepareAndroidDirs(const std::string& volumeRoot);

#endif
