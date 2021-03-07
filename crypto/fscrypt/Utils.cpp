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

#include "Utils.h"

#include "Process.h"
#include "sehandle.h"

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/fs.h>
#include <logwrap/logwrap.h>
#include <private/android_filesystem_config.h>
#include <private/android_projectid_config.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <filesystem>
#include <list>
#include <mutex>
#include <regex>
#include <thread>

#ifndef UMOUNT_NOFOLLOW
#define UMOUNT_NOFOLLOW 0x00000008 /* Don't follow symlink on umount */
#endif

using namespace std::chrono_literals;
using android::base::EndsWith;
using android::base::ReadFileToString;
using android::base::StartsWith;
using android::base::StringPrintf;
using android::base::unique_fd;

struct selabel_handle* sehandle;

security_context_t sBlkidContext = nullptr;
security_context_t sBlkidUntrustedContext = nullptr;
security_context_t sFsckContext = nullptr;
security_context_t sFsckUntrustedContext = nullptr;

bool sSleepOnUnmount = true;

static const char* kBlkidPath = "/system/bin/blkid";
static const char* kKeyPath = "/data/misc/vold";

static const char* kProcDevices = "/proc/devices";
static const char* kProcFilesystems = "/proc/filesystems";

static const char* kAndroidDir = "/Android/";
static const char* kAppDataDir = "/Android/data/";
static const char* kAppMediaDir = "/Android/media/";
static const char* kAppObbDir = "/Android/obb/";

static const char* kMediaProviderCtx = "u:r:mediaprovider:";
static const char* kMediaProviderAppCtx = "u:r:mediaprovider_app:";

// Lock used to protect process-level SELinux changes from racing with each
// other between multiple threads.
static std::mutex kSecurityLock;

std::string GetFuseMountPathForUser(userid_t user_id, const std::string& relative_upper_path) {
    return StringPrintf("/mnt/user/%d/%s", user_id, relative_upper_path.c_str());
}

android::status_t CreateDeviceNode(const std::string& path, dev_t dev) {
    std::lock_guard<std::mutex> lock(kSecurityLock);
    const char* cpath = path.c_str();
    android::status_t res = 0;

    char* secontext = nullptr;
    if (sehandle) {
        if (!selabel_lookup(sehandle, &secontext, cpath, S_IFBLK)) {
            setfscreatecon(secontext);
        }
    }

    mode_t mode = 0660 | S_IFBLK;
    if (mknod(cpath, mode, dev) < 0) {
        if (errno != EEXIST) {
            PLOG(ERROR) << "Failed to create device node for " << major(dev) << ":" << minor(dev)
                        << " at " << path;
            res = -errno;
        }
    }

    if (secontext) {
        setfscreatecon(nullptr);
        freecon(secontext);
    }

    return res;
}

android::status_t DestroyDeviceNode(const std::string& path) {
    const char* cpath = path.c_str();
    if (TEMP_FAILURE_RETRY(unlink(cpath))) {
        return -errno;
    } else {
        return android::OK;
    }
}

// Sets a default ACL on the directory.
int SetDefaultAcl(const std::string& path, mode_t mode, uid_t uid, gid_t gid,
                  std::vector<gid_t> additionalGids) {
    if (IsSdcardfsUsed()) {
        // sdcardfs magically takes care of this
        return android::OK;
    }

    size_t num_entries = 3 + (additionalGids.size() > 0 ? additionalGids.size() + 1 : 0);
    size_t size = sizeof(posix_acl_xattr_header) + num_entries * sizeof(posix_acl_xattr_entry);
    auto buf = std::make_unique<uint8_t[]>(size);

    posix_acl_xattr_header* acl_header = reinterpret_cast<posix_acl_xattr_header*>(buf.get());
    acl_header->a_version = POSIX_ACL_XATTR_VERSION;

    posix_acl_xattr_entry* entry =
            reinterpret_cast<posix_acl_xattr_entry*>(buf.get() + sizeof(posix_acl_xattr_header));

    int tag_index = 0;

    entry[tag_index].e_tag = ACL_USER_OBJ;
    // The existing mode_t mask has the ACL in the lower 9 bits:
    // the lowest 3 for "other", the next 3 the group, the next 3 for the owner
    // Use the mode_t masks to get these bits out, and shift them to get the
    // correct value per entity.
    //
    // Eg if mode_t = 0700, rwx for the owner, then & S_IRWXU >> 6 results in 7
    entry[tag_index].e_perm = (mode & S_IRWXU) >> 6;
    entry[tag_index].e_id = uid;
    tag_index++;

    entry[tag_index].e_tag = ACL_GROUP_OBJ;
    entry[tag_index].e_perm = (mode & S_IRWXG) >> 3;
    entry[tag_index].e_id = gid;
    tag_index++;

    if (additionalGids.size() > 0) {
        for (gid_t additional_gid : additionalGids) {
            entry[tag_index].e_tag = ACL_GROUP;
            entry[tag_index].e_perm = (mode & S_IRWXG) >> 3;
            entry[tag_index].e_id = additional_gid;
            tag_index++;
        }

        entry[tag_index].e_tag = ACL_MASK;
        entry[tag_index].e_perm = (mode & S_IRWXG) >> 3;
        entry[tag_index].e_id = 0;
        tag_index++;
    }

    entry[tag_index].e_tag = ACL_OTHER;
    entry[tag_index].e_perm = mode & S_IRWXO;
    entry[tag_index].e_id = 0;

    int ret = setxattr(path.c_str(), XATTR_NAME_POSIX_ACL_DEFAULT, acl_header, size, 0);

    if (ret != 0) {
        PLOG(ERROR) << "Failed to set default ACL on " << path;
    }

    return ret;
}

int SetQuotaInherit(const std::string& path) {
    unsigned long flags;

    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << path << " to set project id inheritance.";
        return -1;
    }

    int ret = ioctl(fd, FS_IOC_GETFLAGS, &flags);
    if (ret == -1) {
        PLOG(ERROR) << "Failed to get flags for " << path << " to set project id inheritance.";
        return ret;
    }

    flags |= FS_PROJINHERIT_FL;

    ret = ioctl(fd, FS_IOC_SETFLAGS, &flags);
    if (ret == -1) {
        PLOG(ERROR) << "Failed to set flags for " << path << " to set project id inheritance.";
        return ret;
    }

    return 0;
}

int SetQuotaProjectId(const std::string& path, long projectId) {
    struct fsxattr fsx;

    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << path << " to set project id.";
        return -1;
    }

    int ret = ioctl(fd, FS_IOC_FSGETXATTR, &fsx);
    if (ret == -1) {
        PLOG(ERROR) << "Failed to get extended attributes for " << path << " to get project id.";
        return ret;
    }

    fsx.fsx_projid = projectId;
    return ioctl(fd, FS_IOC_FSSETXATTR, &fsx);
}

int PrepareDirWithProjectId(const std::string& path, mode_t mode, uid_t uid, gid_t gid,
                            long projectId) {
    int ret = fs_prepare_dir(path.c_str(), mode, uid, gid);

    if (ret != 0) {
        return ret;
    }

    if (!IsSdcardfsUsed()) {
        ret = SetQuotaProjectId(path, projectId);
    }

    return ret;
}

static int FixupAppDir(const std::string& path, mode_t mode, uid_t uid, gid_t gid, long projectId) {
    namespace fs = std::filesystem;

    // Setup the directory itself correctly
    int ret = PrepareDirWithProjectId(path, mode, uid, gid, projectId);
    if (ret != android::OK) {
        return ret;
    }

    // Fixup all of its file entries
    for (const auto& itEntry : fs::directory_iterator(path)) {
        ret = lchown(itEntry.path().c_str(), uid, gid);
        if (ret != 0) {
            return ret;
        }

        ret = chmod(itEntry.path().c_str(), mode);
        if (ret != 0) {
            return ret;
        }

        if (!IsSdcardfsUsed()) {
            ret = SetQuotaProjectId(itEntry.path(), projectId);
            if (ret != 0) {
                return ret;
            }
        }
    }

    return android::OK;
}

int PrepareAppDirFromRoot(const std::string& path, const std::string& root, int appUid,
                          bool fixupExisting) {
    long projectId;
    size_t pos;
    int ret = 0;
    bool sdcardfsSupport = IsSdcardfsUsed();

    // Make sure the Android/ directories exist and are setup correctly
    ret = PrepareAndroidDirs(root);
    if (ret != 0) {
        LOG(ERROR) << "Failed to prepare Android/ directories.";
        return ret;
    }

    // Now create the application-specific subdir(s)
    // path is something like /data/media/0/Android/data/com.foo/files
    // First, chop off the volume root, eg /data/media/0
    std::string pathFromRoot = path.substr(root.length());

    uid_t uid = appUid;
    gid_t gid = AID_MEDIA_RW;
    std::vector<gid_t> additionalGids;
    std::string appDir;

    // Check that the next part matches one of the allowed Android/ dirs
    if (StartsWith(pathFromRoot, kAppDataDir)) {
        appDir = kAppDataDir;
        if (!sdcardfsSupport) {
            gid = AID_EXT_DATA_RW;
            // Also add the app's own UID as a group; since apps belong to a group
            // that matches their UID, this ensures that they will always have access to
            // the files created in these dirs, even if they are created by other processes
            additionalGids.push_back(uid);
        }
    } else if (StartsWith(pathFromRoot, kAppMediaDir)) {
        appDir = kAppMediaDir;
        if (!sdcardfsSupport) {
            gid = AID_MEDIA_RW;
        }
    } else if (StartsWith(pathFromRoot, kAppObbDir)) {
        appDir = kAppObbDir;
        if (!sdcardfsSupport) {
            gid = AID_EXT_OBB_RW;
            // See comments for kAppDataDir above
            additionalGids.push_back(uid);
        }
    } else {
        LOG(ERROR) << "Invalid application directory: " << path;
        return -EINVAL;
    }

    // mode = 770, plus sticky bit on directory to inherit GID when apps
    // create subdirs
    mode_t mode = S_IRWXU | S_IRWXG | S_ISGID;
    // the project ID for application-specific directories is directly
    // derived from their uid

    // Chop off the generic application-specific part, eg /Android/data/
    // this leaves us with something like com.foo/files/
    std::string leftToCreate = pathFromRoot.substr(appDir.length());
    if (!EndsWith(leftToCreate, "/")) {
        leftToCreate += "/";
    }
    std::string pathToCreate = root + appDir;
    int depth = 0;
    // Derive initial project ID
    if (appDir == kAppDataDir || appDir == kAppMediaDir) {
        projectId = uid - AID_APP_START + PROJECT_ID_EXT_DATA_START;
    } else if (appDir == kAppObbDir) {
        projectId = uid - AID_APP_START + PROJECT_ID_EXT_OBB_START;
    }

    while ((pos = leftToCreate.find('/')) != std::string::npos) {
        std::string component = leftToCreate.substr(0, pos + 1);
        leftToCreate = leftToCreate.erase(0, pos + 1);
        pathToCreate = pathToCreate + component;

        if (appDir == kAppDataDir && depth == 1 && component == "cache/") {
            // All dirs use the "app" project ID, except for the cache dirs in
            // Android/data, eg Android/data/com.foo/cache
            // Note that this "sticks" - eg subdirs of this dir need the same
            // project ID.
            projectId = uid - AID_APP_START + PROJECT_ID_EXT_CACHE_START;
        }

        if (fixupExisting && access(pathToCreate.c_str(), F_OK) == 0) {
            // Fixup all files in this existing directory with the correct UID/GID
            // and project ID.
            ret = FixupAppDir(pathToCreate, mode, uid, gid, projectId);
        } else {
            ret = PrepareDirWithProjectId(pathToCreate, mode, uid, gid, projectId);
        }

        if (ret != 0) {
            return ret;
        }

        if (depth == 0) {
            // Set the default ACL on the top-level application-specific directories,
            // to ensure that even if applications run with a umask of 0077,
            // new directories within these directories will allow the GID
            // specified here to write; this is necessary for apps like
            // installers and MTP, that require access here.
            //
            // See man (5) acl for more details.
            ret = SetDefaultAcl(pathToCreate, mode, uid, gid, additionalGids);
            if (ret != 0) {
                return ret;
            }

            if (!sdcardfsSupport) {
                // Set project ID inheritance, so that future subdirectories inherit the
                // same project ID
                ret = SetQuotaInherit(pathToCreate);
                if (ret != 0) {
                    return ret;
                }
            }
        }

        depth++;
    }

    return android::OK;
}

android::status_t PrepareDir(const std::string& path, mode_t mode, uid_t uid, gid_t gid) {
    std::lock_guard<std::mutex> lock(kSecurityLock);
    const char* cpath = path.c_str();

    char* secontext = nullptr;
    if (sehandle) {
        if (!selabel_lookup(sehandle, &secontext, cpath, S_IFDIR)) {
            setfscreatecon(secontext);
        }
    }

    int res = fs_prepare_dir(cpath, mode, uid, gid);

    if (secontext) {
        setfscreatecon(nullptr);
        freecon(secontext);
    }

    if (res == 0) {
        return android::OK;
    } else {
        return -errno;
    }
}

android::status_t ForceUnmount(const std::string& path) {
    const char* cpath = path.c_str();
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return android::OK;
    }
    // Apps might still be handling eject request, so wait before
    // we start sending signals
    if (sSleepOnUnmount) sleep(5);

    android::vold::KillProcessesWithOpenFiles(path, SIGINT);
    if (sSleepOnUnmount) sleep(5);
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return android::OK;
    }

    android::vold::KillProcessesWithOpenFiles(path, SIGTERM);
    if (sSleepOnUnmount) sleep(5);
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return android::OK;
    }

    android::vold::KillProcessesWithOpenFiles(path, SIGKILL);
    if (sSleepOnUnmount) sleep(5);
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return android::OK;
    }
    PLOG(INFO) << "ForceUnmount failed";
    return -errno;
}

android::status_t KillProcessesWithMountPrefix(const std::string& path) {
    if (android::vold::KillProcessesWithMounts(path, SIGINT) == 0) {
        return android::OK;
    }
    if (sSleepOnUnmount) sleep(5);

    if (android::vold::KillProcessesWithMounts(path, SIGTERM) == 0) {
        return android::OK;
    }
    if (sSleepOnUnmount) sleep(5);

    if (android::vold::KillProcessesWithMounts(path, SIGKILL) == 0) {
        return android::OK;
    }
    if (sSleepOnUnmount) sleep(5);

    // Send SIGKILL a second time to determine if we've
    // actually killed everyone mount
    if (android::vold::KillProcessesWithMounts(path, SIGKILL) == 0) {
        return android::OK;
    }
    PLOG(ERROR) << "Failed to kill processes using " << path;
    return -EBUSY;
}

android::status_t KillProcessesUsingPath(const std::string& path) {
    if (android::vold::KillProcessesWithOpenFiles(path, SIGINT) == 0) {
        return android::OK;
    }
    if (sSleepOnUnmount) sleep(5);

    if (android::vold::KillProcessesWithOpenFiles(path, SIGTERM) == 0) {
        return android::OK;
    }
    if (sSleepOnUnmount) sleep(5);

    if (android::vold::KillProcessesWithOpenFiles(path, SIGKILL) == 0) {
        return android::OK;
    }
    if (sSleepOnUnmount) sleep(5);

    // Send SIGKILL a second time to determine if we've
    // actually killed everyone with open files
    if (android::vold::KillProcessesWithOpenFiles(path, SIGKILL) == 0) {
        return android::OK;
    }
    PLOG(ERROR) << "Failed to kill processes using " << path;
    return -EBUSY;
}

android::status_t BindMount(const std::string& source, const std::string& target) {
    if (UnmountTree(target) < 0) {
        return -errno;
    }
    if (TEMP_FAILURE_RETRY(mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr)) < 0) {
        PLOG(ERROR) << "Failed to bind mount " << source << " to " << target;
        return -errno;
    }
    return android::OK;
}

android::status_t Symlink(const std::string& target, const std::string& linkpath) {
    if (Unlink(linkpath) < 0) {
        return -errno;
    }
    if (TEMP_FAILURE_RETRY(symlink(target.c_str(), linkpath.c_str())) < 0) {
        PLOG(ERROR) << "Failed to create symlink " << linkpath << " to " << target;
        return -errno;
    }
    return android::OK;
}

android::status_t Unlink(const std::string& linkpath) {
    if (TEMP_FAILURE_RETRY(unlink(linkpath.c_str())) < 0 && errno != EINVAL && errno != ENOENT) {
        PLOG(ERROR) << "Failed to unlink " << linkpath;
        return -errno;
    }
    return android::OK;
}

android::status_t CreateDir(const std::string& dir, mode_t mode) {
    struct stat sb;
    if (TEMP_FAILURE_RETRY(stat(dir.c_str(), &sb)) == 0) {
        if (S_ISDIR(sb.st_mode)) {
            return android::OK;
        } else if (TEMP_FAILURE_RETRY(unlink(dir.c_str())) == -1) {
            PLOG(ERROR) << "Failed to unlink " << dir;
            return -errno;
        }
    } else if (errno != ENOENT) {
        PLOG(ERROR) << "Failed to stat " << dir;
        return -errno;
    }
    if (TEMP_FAILURE_RETRY(mkdir(dir.c_str(), mode)) == -1 && errno != EEXIST) {
        PLOG(ERROR) << "Failed to mkdir " << dir;
        return -errno;
    }
    return android::OK;
}

bool FindValue(const std::string& raw, const std::string& key, std::string* value) {
    auto qual = key + "=\"";
    size_t start = 0;
    while (true) {
        start = raw.find(qual, start);
        if (start == std::string::npos) return false;
        if (start == 0 || raw[start - 1] == ' ') {
            break;
        }
        start += 1;
    }
    start += qual.length();

    auto end = raw.find("\"", start);
    if (end == std::string::npos) return false;

    *value = raw.substr(start, end - start);
    return true;
}

static android::status_t readMetadata(const std::string& path, std::string* fsType, std::string* fsUuid,
                             std::string* fsLabel, bool untrusted) {
    fsType->clear();
    fsUuid->clear();
    fsLabel->clear();

    std::vector<std::string> cmd;
    cmd.push_back(kBlkidPath);
    cmd.push_back("-c");
    cmd.push_back("/dev/null");
    cmd.push_back("-s");
    cmd.push_back("TYPE");
    cmd.push_back("-s");
    cmd.push_back("UUID");
    cmd.push_back("-s");
    cmd.push_back("LABEL");
    cmd.push_back(path);

    std::vector<std::string> output;
    android::status_t res = ForkExecvp(cmd, &output, untrusted ? sBlkidUntrustedContext : sBlkidContext);
    if (res != android::OK) {
        LOG(WARNING) << "blkid failed to identify " << path;
        return res;
    }

    for (const auto& line : output) {
        // Extract values from blkid output, if defined
        FindValue(line, "TYPE", fsType);
        FindValue(line, "UUID", fsUuid);
        FindValue(line, "LABEL", fsLabel);
    }

    return android::OK;
}

android::status_t ReadMetadata(const std::string& path, std::string* fsType, std::string* fsUuid,
                      std::string* fsLabel) {
    return readMetadata(path, fsType, fsUuid, fsLabel, false);
}

android::status_t ReadMetadataUntrusted(const std::string& path, std::string* fsType, std::string* fsUuid,
                               std::string* fsLabel) {
    return readMetadata(path, fsType, fsUuid, fsLabel, true);
}

static std::vector<const char*> ConvertToArgv(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        if (argv.empty()) {
            LOG(DEBUG) << arg;
        } else {
            LOG(DEBUG) << "    " << arg;
        }
        argv.emplace_back(arg.data());
    }
    argv.emplace_back(nullptr);
    return argv;
}

static android::status_t ReadLinesFromFdAndLog(std::vector<std::string>* output,
                                      android::base::unique_fd ufd) {
    std::unique_ptr<FILE, int (*)(FILE*)> fp(android::base::Fdopen(std::move(ufd), "r"), fclose);
    if (!fp) {
        PLOG(ERROR) << "fdopen in ReadLinesFromFdAndLog";
        return -errno;
    }
    if (output) output->clear();
    char line[1024];
    while (fgets(line, sizeof(line), fp.get()) != nullptr) {
        LOG(DEBUG) << line;
        if (output) output->emplace_back(line);
    }
    return android::OK;
}

android::status_t ForkExecvp(const std::vector<std::string>& args, std::vector<std::string>* output,
                    security_context_t context) {
    auto argv = ConvertToArgv(args);

    android::base::unique_fd pipe_read, pipe_write;
    if (!android::base::Pipe(&pipe_read, &pipe_write)) {
        PLOG(ERROR) << "Pipe in ForkExecvp";
        return -errno;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (context) {
            if (setexeccon(context)) {
                LOG(ERROR) << "Failed to setexeccon in ForkExecvp";
                abort();
            }
        }
        pipe_read.reset();
        if (dup2(pipe_write.get(), STDOUT_FILENO) == -1) {
            PLOG(ERROR) << "dup2 in ForkExecvp";
            _exit(EXIT_FAILURE);
        }
        pipe_write.reset();
        execvp(argv[0], const_cast<char**>(argv.data()));
        PLOG(ERROR) << "exec in ForkExecvp";
        _exit(EXIT_FAILURE);
    }
    if (pid == -1) {
        PLOG(ERROR) << "fork in ForkExecvp";
        return -errno;
    }

    pipe_write.reset();
    auto st = ReadLinesFromFdAndLog(output, std::move(pipe_read));
    if (st != 0) return st;

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        PLOG(ERROR) << "waitpid in ForkExecvp";
        return -errno;
    }
    if (!WIFEXITED(status)) {
        LOG(ERROR) << "Process did not exit normally, status: " << status;
        return -ECHILD;
    }
    if (WEXITSTATUS(status)) {
        LOG(ERROR) << "Process exited with code: " << WEXITSTATUS(status);
        return WEXITSTATUS(status);
    }
    return android::OK;
}

pid_t ForkExecvpAsync(const std::vector<std::string>& args) {
    auto argv = ConvertToArgv(args);

    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        execvp(argv[0], const_cast<char**>(argv.data()));
        PLOG(ERROR) << "exec in ForkExecvpAsync";
        _exit(EXIT_FAILURE);
    }
    if (pid == -1) {
        PLOG(ERROR) << "fork in ForkExecvpAsync";
        return -1;
    }
    return pid;
}

android::status_t ReadRandomBytes(size_t bytes, std::string& out) {
    out.resize(bytes);
    return ReadRandomBytes(bytes, &out[0]);
}

android::status_t ReadRandomBytes(size_t bytes, char* buf) {
    int fd = TEMP_FAILURE_RETRY(open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd == -1) {
        return -errno;
    }

    ssize_t n;
    while ((n = TEMP_FAILURE_RETRY(read(fd, &buf[0], bytes))) > 0) {
        bytes -= n;
        buf += n;
    }
    close(fd);

    if (bytes == 0) {
        return android::OK;
    } else {
        return -EIO;
    }
}

android::status_t GenerateRandomUuid(std::string& out) {
    android::status_t res = ReadRandomBytes(16, out);
    if (res == android::OK) {
        out[6] &= 0x0f; /* clear version        */
        out[6] |= 0x40; /* set to version 4     */
        out[8] &= 0x3f; /* clear variant        */
        out[8] |= 0x80; /* set to IETF variant  */
    }
    return res;
}

android::status_t HexToStr(const std::string& hex, std::string& str) {
    str.clear();
    bool even = true;
    char cur = 0;
    for (size_t i = 0; i < hex.size(); i++) {
        int val = 0;
        switch (hex[i]) {
            // clang-format off
            case ' ': case '-': case ':': continue;
            case 'f': case 'F': val = 15; break;
            case 'e': case 'E': val = 14; break;
            case 'd': case 'D': val = 13; break;
            case 'c': case 'C': val = 12; break;
            case 'b': case 'B': val = 11; break;
            case 'a': case 'A': val = 10; break;
            case '9': val = 9; break;
            case '8': val = 8; break;
            case '7': val = 7; break;
            case '6': val = 6; break;
            case '5': val = 5; break;
            case '4': val = 4; break;
            case '3': val = 3; break;
            case '2': val = 2; break;
            case '1': val = 1; break;
            case '0': val = 0; break;
            default: return -EINVAL;
                // clang-format on
        }

        if (even) {
            cur = val << 4;
        } else {
            cur += val;
            str.push_back(cur);
            cur = 0;
        }
        even = !even;
    }
    return even ? android::OK : -EINVAL;
}

static const char* kLookup = "0123456789abcdef";

android::status_t StrToHex(const std::string& str, std::string& hex) {
    hex.clear();
    for (size_t i = 0; i < str.size(); i++) {
        hex.push_back(kLookup[(str[i] & 0xF0) >> 4]);
        hex.push_back(kLookup[str[i] & 0x0F]);
    }
    return android::OK;
}

android::status_t StrToHex(const KeyBuffer& str, KeyBuffer& hex) {
    hex.clear();
    for (size_t i = 0; i < str.size(); i++) {
        hex.push_back(kLookup[(str.data()[i] & 0xF0) >> 4]);
        hex.push_back(kLookup[str.data()[i] & 0x0F]);
    }
    return android::OK;
}

android::status_t NormalizeHex(const std::string& in, std::string& out) {
    std::string tmp;
    if (HexToStr(in, tmp)) {
        return -EINVAL;
    }
    return StrToHex(tmp, out);
}

android::status_t GetBlockDevSize(int fd, uint64_t* size) {
    if (ioctl(fd, BLKGETSIZE64, size)) {
        return -errno;
    }

    return android::OK;
}

android::status_t GetBlockDevSize(const std::string& path, uint64_t* size) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    android::status_t res = android::OK;

    if (fd < 0) {
        return -errno;
    }

    res = GetBlockDevSize(fd, size);

    close(fd);

    return res;
}

android::status_t GetBlockDev512Sectors(const std::string& path, uint64_t* nr_sec) {
    uint64_t size;
    android::status_t res = GetBlockDevSize(path, &size);

    if (res != android::OK) {
        return res;
    }

    *nr_sec = size / 512;

    return android::OK;
}

uint64_t GetFreeBytes(const std::string& path) {
    struct statvfs sb;
    if (statvfs(path.c_str(), &sb) == 0) {
        return (uint64_t)sb.f_bavail * sb.f_frsize;
    } else {
        return -1;
    }
}

// TODO: borrowed from frameworks/native/libs/diskusage/ which should
// eventually be migrated into system/
static int64_t stat_size(struct stat* s) {
    int64_t blksize = s->st_blksize;
    // count actual blocks used instead of nominal file size
    int64_t size = s->st_blocks * 512;

    if (blksize) {
        /* round up to filesystem block size */
        size = (size + blksize - 1) & (~(blksize - 1));
    }

    return size;
}

// TODO: borrowed from frameworks/native/libs/diskusage/ which should
// eventually be migrated into system/
int64_t calculate_dir_size(int dfd) {
    int64_t size = 0;
    struct stat s;
    DIR* d;
    struct dirent* de;

    d = fdopendir(dfd);
    if (d == NULL) {
        close(dfd);
        return 0;
    }

    while ((de = readdir(d))) {
        const char* name = de->d_name;
        if (fstatat(dfd, name, &s, AT_SYMLINK_NOFOLLOW) == 0) {
            size += stat_size(&s);
        }
        if (de->d_type == DT_DIR) {
            int subfd;

            /* always skip "." and ".." */
            if (name[0] == '.') {
                if (name[1] == 0) continue;
                if ((name[1] == '.') && (name[2] == 0)) continue;
            }

            subfd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (subfd >= 0) {
                size += calculate_dir_size(subfd);
            }
        }
    }
    closedir(d);
    return size;
}

uint64_t GetTreeBytes(const std::string& path) {
    int dirfd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        PLOG(WARNING) << "Failed to open " << path;
        return -1;
    } else {
        return calculate_dir_size(dirfd);
    }
}

// TODO: Use a better way to determine if it's media provider app.
bool IsFuseDaemon(const pid_t pid) {
    auto path = StringPrintf("/proc/%d/mounts", pid);
    char* tmp;
    if (lgetfilecon(path.c_str(), &tmp) < 0) {
        return false;
    }
    bool result = android::base::StartsWith(tmp, kMediaProviderAppCtx)
            || android::base::StartsWith(tmp, kMediaProviderCtx);
    freecon(tmp);
    return result;
}

bool IsFilesystemSupported(const std::string& fsType) {
    std::string supported;
    if (!ReadFileToString(kProcFilesystems, &supported)) {
        PLOG(ERROR) << "Failed to read supported filesystems";
        return false;
    }
    return supported.find(fsType + "\n") != std::string::npos;
}

bool IsSdcardfsUsed() {
    return IsFilesystemSupported("sdcardfs") &&
           android::base::GetBoolProperty(kExternalStorageSdcardfs, true);
}

android::status_t WipeBlockDevice(const std::string& path) {
    android::status_t res = -1;
    const char* c_path = path.c_str();
    uint64_t range[2] = {0, 0};

    int fd = TEMP_FAILURE_RETRY(open(c_path, O_RDWR | O_CLOEXEC));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << path;
        goto done;
    }

    if (GetBlockDevSize(fd, &range[1]) != android::OK) {
        PLOG(ERROR) << "Failed to determine size of " << path;
        goto done;
    }

    LOG(INFO) << "About to discard " << range[1] << " on " << path;
    if (ioctl(fd, BLKDISCARD, &range) == 0) {
        LOG(INFO) << "Discard success on " << path;
        res = 0;
    } else {
        PLOG(ERROR) << "Discard failure on " << path;
    }

done:
    close(fd);
    return res;
}

static bool isValidFilename(const std::string& name) {
    if (name.empty() || (name == ".") || (name == "..") || (name.find('/') != std::string::npos)) {
        return false;
    } else {
        return true;
    }
}

std::string BuildKeyPath(const std::string& partGuid) {
    return StringPrintf("%s/expand_%s.key", kKeyPath, partGuid.c_str());
}

std::string BuildDataSystemLegacyPath(userid_t userId) {
    return StringPrintf("%s/system/users/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataSystemCePath(userid_t userId) {
    return StringPrintf("%s/system_ce/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataSystemDePath(userid_t userId) {
    return StringPrintf("%s/system_de/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataMiscLegacyPath(userid_t userId) {
    return StringPrintf("%s/misc/user/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataMiscCePath(userid_t userId) {
    return StringPrintf("%s/misc_ce/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataMiscDePath(userid_t userId) {
    return StringPrintf("%s/misc_de/%u", BuildDataPath("").c_str(), userId);
}

// Keep in sync with installd (frameworks/native/cmds/installd/utils.h)
std::string BuildDataProfilesDePath(userid_t userId) {
    return StringPrintf("%s/misc/profiles/cur/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataVendorCePath(userid_t userId) {
    return StringPrintf("%s/vendor_ce/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataVendorDePath(userid_t userId) {
    return StringPrintf("%s/vendor_de/%u", BuildDataPath("").c_str(), userId);
}

std::string BuildDataPath(const std::string& volumeUuid) {
    // TODO: unify with installd path generation logic
    if (volumeUuid.empty()) {
        return "/data";
    } else {
        CHECK(isValidFilename(volumeUuid));
        return StringPrintf("/mnt/expand/%s", volumeUuid.c_str());
    }
}

std::string BuildDataMediaCePath(const std::string& volumeUuid, userid_t userId) {
    // TODO: unify with installd path generation logic
    std::string data(BuildDataPath(volumeUuid));
    return StringPrintf("%s/media/%u", data.c_str(), userId);
}

std::string BuildDataUserCePath(const std::string& volumeUuid, userid_t userId) {
    // TODO: unify with installd path generation logic
    std::string data(BuildDataPath(volumeUuid));
    if (volumeUuid.empty() && userId == 0) {
        std::string legacy = StringPrintf("%s/data", data.c_str());
        struct stat sb;
        if (lstat(legacy.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
            /* /data/data is dir, return /data/data for legacy system */
            return legacy;
        }
    }
    return StringPrintf("%s/user/%u", data.c_str(), userId);
}

std::string BuildDataUserDePath(const std::string& volumeUuid, userid_t userId) {
    // TODO: unify with installd path generation logic
    std::string data(BuildDataPath(volumeUuid));
    return StringPrintf("%s/user_de/%u", data.c_str(), userId);
}

dev_t GetDevice(const std::string& path) {
    struct stat sb;
    if (stat(path.c_str(), &sb)) {
        PLOG(WARNING) << "Failed to stat " << path;
        return 0;
    } else {
        return sb.st_dev;
    }
}

android::status_t RestoreconRecursive(const std::string& path) {
    LOG(DEBUG) << "Starting restorecon of " << path;

    static constexpr const char* kRestoreconString = "selinux.restorecon_recursive";

    android::base::SetProperty(kRestoreconString, "");
    android::base::SetProperty(kRestoreconString, path);

    android::base::WaitForProperty(kRestoreconString, path);

    LOG(DEBUG) << "Finished restorecon of " << path;
    return android::OK;
}

bool Readlinkat(int dirfd, const std::string& path, std::string* result) {
    // Shamelessly borrowed from android::base::Readlink()
    result->clear();

    // Most Linux file systems (ext2 and ext4, say) limit symbolic links to
    // 4095 bytes. Since we'll copy out into the string anyway, it doesn't
    // waste memory to just start there. We add 1 so that we can recognize
    // whether it actually fit (rather than being truncated to 4095).
    std::vector<char> buf(4095 + 1);
    while (true) {
        ssize_t size = readlinkat(dirfd, path.c_str(), &buf[0], buf.size());
        // Unrecoverable error?
        if (size == -1) return false;
        // It fit! (If size == buf.size(), it may have been truncated.)
        if (static_cast<size_t>(size) < buf.size()) {
            result->assign(&buf[0], size);
            return true;
        }
        // Double our buffer and try again.
        buf.resize(buf.size() * 2);
    }
}

static unsigned int GetMajorBlockVirtioBlk() {
    std::string devices;
    if (!ReadFileToString(kProcDevices, &devices)) {
        PLOG(ERROR) << "Unable to open /proc/devices";
        return 0;
    }

    bool blockSection = false;
    for (auto line : android::base::Split(devices, "\n")) {
        if (line == "Block devices:") {
            blockSection = true;
        } else if (line == "Character devices:") {
            blockSection = false;
        } else if (blockSection) {
            auto tokens = android::base::Split(line, " ");
            if (tokens.size() == 2 && tokens[1] == "virtblk") {
                return std::stoul(tokens[0]);
            }
        }
    }

    return 0;
}

bool IsVirtioBlkDevice(unsigned int major) {
    // Most virtualized platforms expose block devices with the virtio-blk
    // block device driver. Unfortunately, this driver does not use a fixed
    // major number, but relies on the kernel to assign one from a specific
    // range of block majors, which are allocated for "LOCAL/EXPERIMENAL USE"
    // per Documentation/devices.txt. This is true even for the latest Linux
    // kernel (4.4; see init() in drivers/block/virtio_blk.c).
    static unsigned int kMajorBlockVirtioBlk = GetMajorBlockVirtioBlk();
    return kMajorBlockVirtioBlk && major == kMajorBlockVirtioBlk;
}

static android::status_t findMountPointsWithPrefix(const std::string& prefix,
                                          std::list<std::string>& mountPoints) {
    // Add a trailing slash if the client didn't provide one so that we don't match /foo/barbaz
    // when the prefix is /foo/bar
    std::string prefixWithSlash(prefix);
    if (prefix.back() != '/') {
        android::base::StringAppendF(&prefixWithSlash, "/");
    }

    std::unique_ptr<FILE, int (*)(FILE*)> mnts(setmntent("/proc/mounts", "re"), endmntent);
    if (!mnts) {
        PLOG(ERROR) << "Unable to open /proc/mounts";
        return -errno;
    }

    // Some volumes can be stacked on each other, so force unmount in
    // reverse order to give us the best chance of success.
    struct mntent* mnt;  // getmntent returns a thread local, so it's safe.
    while ((mnt = getmntent(mnts.get())) != nullptr) {
        auto mountPoint = std::string(mnt->mnt_dir) + "/";
        if (android::base::StartsWith(mountPoint, prefixWithSlash)) {
            mountPoints.push_front(mountPoint);
        }
    }
    return android::OK;
}

// Unmount all mountpoints that start with prefix. prefix itself doesn't need to be a mountpoint.
android::status_t UnmountTreeWithPrefix(const std::string& prefix) {
    std::list<std::string> toUnmount;
    android::status_t result = findMountPointsWithPrefix(prefix, toUnmount);
    if (result < 0) {
        return result;
    }
    for (const auto& path : toUnmount) {
        if (umount2(path.c_str(), MNT_DETACH)) {
            PLOG(ERROR) << "Failed to unmount " << path;
            result = -errno;
        }
    }
    return result;
}

android::status_t UnmountTree(const std::string& mountPoint) {
    if (TEMP_FAILURE_RETRY(umount2(mountPoint.c_str(), MNT_DETACH)) < 0 && errno != EINVAL &&
        errno != ENOENT) {
        PLOG(ERROR) << "Failed to unmount " << mountPoint;
        return -errno;
    }
    return android::OK;
}

static android::status_t delete_dir_contents(DIR* dir) {
    // Shamelessly borrowed from android::installd
    int dfd = dirfd(dir);
    if (dfd < 0) {
        return -errno;
    }

    android::status_t result = android::OK;
    struct dirent* de;
    while ((de = readdir(dir))) {
        const char* name = de->d_name;
        if (de->d_type == DT_DIR) {
            /* always skip "." and ".." */
            if (name[0] == '.') {
                if (name[1] == 0) continue;
                if ((name[1] == '.') && (name[2] == 0)) continue;
            }

            android::base::unique_fd subfd(
                openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (subfd.get() == -1) {
                PLOG(ERROR) << "Couldn't openat " << name;
                result = -errno;
                continue;
            }
            std::unique_ptr<DIR, decltype(&closedir)> subdirp(
                android::base::Fdopendir(std::move(subfd)), closedir);
            if (!subdirp) {
                PLOG(ERROR) << "Couldn't fdopendir " << name;
                result = -errno;
                continue;
            }
            result = delete_dir_contents(subdirp.get());
            if (unlinkat(dfd, name, AT_REMOVEDIR) < 0) {
                PLOG(ERROR) << "Couldn't unlinkat " << name;
                result = -errno;
            }
        } else {
            if (unlinkat(dfd, name, 0) < 0) {
                PLOG(ERROR) << "Couldn't unlinkat " << name;
                result = -errno;
            }
        }
    }
    return result;
}

android::status_t DeleteDirContentsAndDir(const std::string& pathname) {
    android::status_t res = DeleteDirContents(pathname);
    if (res < 0) {
        return res;
    }
    if (TEMP_FAILURE_RETRY(rmdir(pathname.c_str())) < 0 && errno != ENOENT) {
        PLOG(ERROR) << "rmdir failed on " << pathname;
        return -errno;
    }
    LOG(VERBOSE) << "Success: rmdir on " << pathname;
    return android::OK;
}

android::status_t DeleteDirContents(const std::string& pathname) {
    // Shamelessly borrowed from android::installd
    std::unique_ptr<DIR, decltype(&closedir)> dirp(opendir(pathname.c_str()), closedir);
    if (!dirp) {
        if (errno == ENOENT) {
            return android::OK;
        }
        PLOG(ERROR) << "Failed to opendir " << pathname;
        return -errno;
    }
    return delete_dir_contents(dirp.get());
}

// TODO(118708649): fix duplication with init/util.h
android::status_t WaitForFile(const char* filename, std::chrono::nanoseconds timeout) {
    android::base::Timer t;
    while (t.duration() < timeout) {
        struct stat sb;
        if (stat(filename, &sb) != -1) {
            LOG(INFO) << "wait for '" << filename << "' took " << t;
            return 0;
        }
        std::this_thread::sleep_for(10ms);
    }
    LOG(WARNING) << "wait for '" << filename << "' timed out and took " << t;
    return -1;
}

bool FsyncDirectory(const std::string& dirname) {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(dirname.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << dirname;
        return false;
    }
    if (fsync(fd) == -1) {
        if (errno == EROFS || errno == EINVAL) {
            PLOG(WARNING) << "Skip fsync " << dirname
                          << " on a file system does not support synchronization";
        } else {
            PLOG(ERROR) << "Failed to fsync " << dirname;
            return false;
        }
    }
    return true;
}

bool writeStringToFile(const std::string& payload, const std::string& filename) {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(
        open(filename.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0666)));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << filename;
        return false;
    }
    if (!android::base::WriteStringToFd(payload, fd)) {
        PLOG(ERROR) << "Failed to write to " << filename;
        unlink(filename.c_str());
        return false;
    }
    // fsync as close won't guarantee flush data
    // see close(2), fsync(2) and b/68901441
    if (fsync(fd) == -1) {
        if (errno == EROFS || errno == EINVAL) {
            PLOG(WARNING) << "Skip fsync " << filename
                          << " on a file system does not support synchronization";
        } else {
            PLOG(ERROR) << "Failed to fsync " << filename;
            unlink(filename.c_str());
            return false;
        }
    }
    return true;
}

android::status_t AbortFuseConnections() {
    namespace fs = std::filesystem;

    for (const auto& itEntry : fs::directory_iterator("/sys/fs/fuse/connections")) {
        std::string abortPath = itEntry.path().string() + "/abort";
        LOG(DEBUG) << "Aborting fuse connection entry " << abortPath;
        bool ret = writeStringToFile("1", abortPath);
        if (!ret) {
            LOG(WARNING) << "Failed to write to " << abortPath;
        }
    }

    return android::OK;
}

android::status_t EnsureDirExists(const std::string& path, mode_t mode, uid_t uid, gid_t gid) {
    if (access(path.c_str(), F_OK) != 0) {
        PLOG(WARNING) << "Dir does not exist: " << path;
        if (fs_prepare_dir(path.c_str(), mode, uid, gid) != 0) {
            return -errno;
        }
    }
    return android::OK;
}

// Gets the sysfs path for parameters of the backing device info (bdi)
static std::string getBdiPathForMount(const std::string& mount) {
    // First figure out MAJOR:MINOR of mount. Simplest way is to stat the path.
    struct stat info;
    if (stat(mount.c_str(), &info) != 0) {
        PLOG(ERROR) << "Failed to stat " << mount;
        return "";
    }
    unsigned int maj = major(info.st_dev);
    unsigned int min = minor(info.st_dev);

    return StringPrintf("/sys/class/bdi/%u:%u", maj, min);
}

// Configures max_ratio for the FUSE filesystem.
void ConfigureMaxDirtyRatioForFuse(const std::string& fuse_mount, unsigned int max_ratio) {
    LOG(INFO) << "Configuring max_ratio of " << fuse_mount << " fuse filesystem to " << max_ratio;
    if (max_ratio > 100) {
        LOG(ERROR) << "Invalid max_ratio: " << max_ratio;
        return;
    }
    std::string fuseBdiPath = getBdiPathForMount(fuse_mount);
    if (fuseBdiPath == "") {
        return;
    }
    std::string max_ratio_file = StringPrintf("%s/max_ratio", fuseBdiPath.c_str());
    unique_fd fd(TEMP_FAILURE_RETRY(open(max_ratio_file.c_str(), O_WRONLY | O_CLOEXEC)));
    if (fd.get() == -1) {
        PLOG(ERROR) << "Failed to open " << max_ratio_file;
        return;
    }
    LOG(INFO) << "Writing " << max_ratio << " to " << max_ratio_file;
    if (!WriteStringToFd(std::to_string(max_ratio), fd)) {
        PLOG(ERROR) << "Failed to write to " << max_ratio_file;
    }
}

// Configures read ahead property of the fuse filesystem with the mount point |fuse_mount| by
// writing |read_ahead_kb| to the /sys/class/bdi/MAJOR:MINOR/read_ahead_kb.
void ConfigureReadAheadForFuse(const std::string& fuse_mount, size_t read_ahead_kb) {
    LOG(INFO) << "Configuring read_ahead of " << fuse_mount << " fuse filesystem to "
              << read_ahead_kb << "kb";
    std::string fuseBdiPath = getBdiPathForMount(fuse_mount);
    if (fuseBdiPath == "") {
        return;
    }
    // We found the bdi path for our filesystem, time to configure read ahead!
    std::string read_ahead_file = StringPrintf("%s/read_ahead_kb", fuseBdiPath.c_str());
    unique_fd fd(TEMP_FAILURE_RETRY(open(read_ahead_file.c_str(), O_WRONLY | O_CLOEXEC)));
    if (fd.get() == -1) {
        PLOG(ERROR) << "Failed to open " << read_ahead_file;
        return;
    }
    LOG(INFO) << "Writing " << read_ahead_kb << " to " << read_ahead_file;
    if (!WriteStringToFd(std::to_string(read_ahead_kb), fd)) {
        PLOG(ERROR) << "Failed to write to " << read_ahead_file;
    }
}

android::status_t MountUserFuse(userid_t user_id, const std::string& absolute_lower_path,
                       const std::string& relative_upper_path, android::base::unique_fd* fuse_fd) {
    std::string pre_fuse_path(StringPrintf("/mnt/user/%d", user_id));
    std::string fuse_path(
            StringPrintf("%s/%s", pre_fuse_path.c_str(), relative_upper_path.c_str()));

    std::string pre_pass_through_path(StringPrintf("/mnt/pass_through/%d", user_id));
    std::string pass_through_path(
            StringPrintf("%s/%s", pre_pass_through_path.c_str(), relative_upper_path.c_str()));

    // Ensure that /mnt/user is 0700. With FUSE, apps don't need access to /mnt/user paths directly.
    // Without FUSE however, apps need /mnt/user access so /mnt/user in init.rc is 0755 until here
    auto result = PrepareDir("/mnt/user", 0750, AID_ROOT, AID_MEDIA_RW);
    if (result != android::OK) {
        PLOG(ERROR) << "Failed to prepare directory /mnt/user";
        return -1;
    }

    // Shell is neither AID_ROOT nor AID_EVERYBODY. Since it equally needs 'execute' access to
    // /mnt/user/0 to 'adb shell ls /sdcard' for instance, we set the uid bit of /mnt/user/0 to
    // AID_SHELL. This gives shell access along with apps running as group everybody (user 0 apps)
    // These bits should be consistent with what is set in zygote in
    // com_android_internal_os_Zygote#MountEmulatedStorage on volume bind mount during app fork
    result = PrepareDir(pre_fuse_path, 0710, user_id ? AID_ROOT : AID_SHELL,
                             multiuser_get_uid(user_id, AID_EVERYBODY));
    if (result != android::OK) {
        PLOG(ERROR) << "Failed to prepare directory " << pre_fuse_path;
        return -1;
    }

    result = PrepareDir(fuse_path, 0700, AID_ROOT, AID_ROOT);
    if (result != android::OK) {
        PLOG(ERROR) << "Failed to prepare directory " << fuse_path;
        return -1;
    }

    result = PrepareDir(pre_pass_through_path, 0710, AID_ROOT, AID_MEDIA_RW);
    if (result != android::OK) {
        PLOG(ERROR) << "Failed to prepare directory " << pre_pass_through_path;
        return -1;
    }

    result = PrepareDir(pass_through_path, 0710, AID_ROOT, AID_MEDIA_RW);
    if (result != android::OK) {
        PLOG(ERROR) << "Failed to prepare directory " << pass_through_path;
        return -1;
    }

    if (relative_upper_path == "emulated") {
        std::string linkpath(StringPrintf("/mnt/user/%d/self", user_id));
        result = PrepareDir(linkpath, 0755, AID_ROOT, AID_ROOT);
        if (result != android::OK) {
            PLOG(ERROR) << "Failed to prepare directory " << linkpath;
            return -1;
        }
        linkpath += "/primary";
        Symlink("/storage/emulated/" + std::to_string(user_id), linkpath);

        std::string pass_through_linkpath(StringPrintf("/mnt/pass_through/%d/self", user_id));
        result = PrepareDir(pass_through_linkpath, 0710, AID_ROOT, AID_MEDIA_RW);
        if (result != android::OK) {
            PLOG(ERROR) << "Failed to prepare directory " << pass_through_linkpath;
            return -1;
        }
        pass_through_linkpath += "/primary";
        Symlink("/storage/emulated/" + std::to_string(user_id), pass_through_linkpath);
    }

    // Open fuse fd.
    fuse_fd->reset(open("/dev/fuse", O_RDWR | O_CLOEXEC));
    if (fuse_fd->get() == -1) {
        PLOG(ERROR) << "Failed to open /dev/fuse";
        return -1;
    }

    // Note: leaving out default_permissions since we don't want kernel to do lower filesystem
    // permission checks before routing to FUSE daemon.
    const auto opts = StringPrintf(
        "fd=%i,"
        "rootmode=40000,"
        "allow_other,"
        "user_id=0,group_id=0,",
        fuse_fd->get());

    result = TEMP_FAILURE_RETRY(mount("/dev/fuse", fuse_path.c_str(), "fuse",
                                      MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_LAZYTIME,
                                      opts.c_str()));
    if (result != 0) {
        PLOG(ERROR) << "Failed to mount " << fuse_path;
        return -errno;
    }

    if (IsSdcardfsUsed()) {
        std::string sdcardfs_path(
                StringPrintf("/mnt/runtime/full/%s", relative_upper_path.c_str()));

        LOG(INFO) << "Bind mounting " << sdcardfs_path << " to " << pass_through_path;
        return BindMount(sdcardfs_path, pass_through_path);
    } else {
        LOG(INFO) << "Bind mounting " << absolute_lower_path << " to " << pass_through_path;
        return BindMount(absolute_lower_path, pass_through_path);
    }
}

android::status_t UnmountUserFuse(userid_t user_id, const std::string& absolute_lower_path,
                         const std::string& relative_upper_path) {
    std::string fuse_path(StringPrintf("/mnt/user/%d/%s", user_id, relative_upper_path.c_str()));
    std::string pass_through_path(
            StringPrintf("/mnt/pass_through/%d/%s", user_id, relative_upper_path.c_str()));

    // Best effort unmount pass_through path
    sSleepOnUnmount = false;
    LOG(INFO) << "Unmounting pass_through_path " << pass_through_path;
    auto status = ForceUnmount(pass_through_path);
    if (status != android::OK) {
        LOG(ERROR) << "Failed to unmount " << pass_through_path;
    }
    rmdir(pass_through_path.c_str());

    LOG(INFO) << "Unmounting fuse path " << fuse_path;
    android::status_t result = ForceUnmount(fuse_path);
    sSleepOnUnmount = true;
    if (result != android::OK) {
        // TODO(b/135341433): MNT_DETACH is needed for fuse because umount2 can fail with EBUSY.
        // Figure out why we get EBUSY and remove this special casing if possible.
        PLOG(ERROR) << "Failed to unmount. Trying MNT_DETACH " << fuse_path << " ...";
        if (umount2(fuse_path.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) && errno != EINVAL &&
            errno != ENOENT) {
            PLOG(ERROR) << "Failed to unmount with MNT_DETACH " << fuse_path;
            return -errno;
        }
        result = android::OK;
    }
    rmdir(fuse_path.c_str());

    return result;
}

android::status_t PrepareAndroidDirs(const std::string& volumeRoot) {
    std::string androidDir = volumeRoot + kAndroidDir;
    std::string androidDataDir = volumeRoot + kAppDataDir;
    std::string androidObbDir = volumeRoot + kAppObbDir;
    std::string androidMediaDir = volumeRoot + kAppMediaDir;

    bool useSdcardFs = IsSdcardfsUsed();

    // mode 0771 + sticky bit for inheriting GIDs
    mode_t mode = S_IRWXU | S_IRWXG | S_IXOTH | S_ISGID;
    if (fs_prepare_dir(androidDir.c_str(), mode, AID_MEDIA_RW, AID_MEDIA_RW) != 0) {
        PLOG(ERROR) << "Failed to create " << androidDir;
        return -errno;
    }

    gid_t dataGid = useSdcardFs ? AID_MEDIA_RW : AID_EXT_DATA_RW;
    if (fs_prepare_dir(androidDataDir.c_str(), mode, AID_MEDIA_RW, dataGid) != 0) {
        PLOG(ERROR) << "Failed to create " << androidDataDir;
        return -errno;
    }

    gid_t obbGid = useSdcardFs ? AID_MEDIA_RW : AID_EXT_OBB_RW;
    if (fs_prepare_dir(androidObbDir.c_str(), mode, AID_MEDIA_RW, obbGid) != 0) {
        PLOG(ERROR) << "Failed to create " << androidObbDir;
        return -errno;
    }
    // Some other apps, like installers, have write access to the OBB directory
    // to pre-download them. To make sure newly created folders in this directory
    // have the right permissions, set a default ACL.
    SetDefaultAcl(androidObbDir, mode, AID_MEDIA_RW, obbGid, {});

    if (fs_prepare_dir(androidMediaDir.c_str(), mode, AID_MEDIA_RW, AID_MEDIA_RW) != 0) {
        PLOG(ERROR) << "Failed to create " << androidMediaDir;
        return -errno;
    }

    return android::OK;
}
