/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <mntent.h>
#include <unordered_set>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "Process.h"
#include "Utils.h"

using android::base::StringPrintf;

static bool checkMaps(const std::string& path, const std::string& prefix) {
    bool found = false;
    auto file = std::unique_ptr<FILE, decltype(&fclose)>{fopen(path.c_str(), "re"), fclose};
    if (!file) {
        return false;
    }

    char* buf = nullptr;
    size_t len = 0;
    while (getline(&buf, &len, file.get()) != -1) {
        std::string line(buf);
        std::string::size_type pos = line.find('/');
        if (pos != std::string::npos) {
            line = line.substr(pos);
            if (android::base::StartsWith(line, prefix)) {
                LOG(WARNING) << "Found map " << path << " referencing " << line;
                found = true;
                break;
            }
        }
    }
    free(buf);

    return found;
}

static bool checkSymlink(const std::string& path, const std::string& prefix) {
    std::string res;
    if (android::base::Readlink(path, &res)) {
        if (android::base::StartsWith(res, prefix)) {
            LOG(WARNING) << "Found symlink " << path << " referencing " << res;
            return true;
        }
    }
    return false;
}

// TODO: Refactor the code with KillProcessesWithOpenFiles().
int KillProcessesWithTmpfsMounts(const std::string& prefix, int signal) {
    std::unordered_set<pid_t> pids;

    auto proc_d = std::unique_ptr<DIR, int (*)(DIR*)>(opendir("/proc"), closedir);
    if (!proc_d) {
        PLOG(ERROR) << "Failed to open proc";
        return -1;
    }

    struct dirent* proc_de;
    while ((proc_de = readdir(proc_d.get())) != nullptr) {
        // We only care about valid PIDs
        pid_t pid;
        if (proc_de->d_type != DT_DIR) continue;
        if (!android::base::ParseInt(proc_de->d_name, &pid)) continue;

        // Look for references to prefix
        std::string mounts_file(StringPrintf("/proc/%d/mounts", pid));
        auto fp = std::unique_ptr<FILE, int (*)(FILE*)>(
                setmntent(mounts_file.c_str(), "r"), endmntent);
        if (!fp) {
            PLOG(WARNING) << "Failed to open " << mounts_file;
            continue;
        }

        // Check if obb directory is mounted, and get all packages of mounted app data directory.
        mntent* mentry;
        while ((mentry = getmntent(fp.get())) != nullptr) {
            if (mentry->mnt_fsname != nullptr && strncmp(mentry->mnt_fsname, "tmpfs", 5) == 0
                    && android::base::StartsWith(mentry->mnt_dir, prefix)) {
                pids.insert(pid);
                break;
            }
        }
    }
    if (signal != 0) {
        for (const auto& pid : pids) {
            LOG(WARNING) << "Killing pid "<< pid << " with signal " << strsignal(signal) <<
                    " because it has a mount with prefix " << prefix;
            kill(pid, signal);
        }
    }
    return pids.size();
}

int KillProcessesWithOpenFiles(const std::string& prefix, int signal, bool killFuseDaemon) {
    std::unordered_set<pid_t> pids;

    auto proc_d = std::unique_ptr<DIR, int (*)(DIR*)>(opendir("/proc"), closedir);
    if (!proc_d) {
        PLOG(ERROR) << "Failed to open proc";
        return -1;
    }

    struct dirent* proc_de;
    while ((proc_de = readdir(proc_d.get())) != nullptr) {
        // We only care about valid PIDs
        pid_t pid;
        if (proc_de->d_type != DT_DIR) continue;
        if (!android::base::ParseInt(proc_de->d_name, &pid)) continue;

        // Look for references to prefix
        bool found = false;
        auto path = StringPrintf("/proc/%d", pid);
        found |= checkMaps(path + "/maps", prefix);
        found |= checkSymlink(path + "/cwd", prefix);
        found |= checkSymlink(path + "/root", prefix);
        found |= checkSymlink(path + "/exe", prefix);

        auto fd_path = path + "/fd";
        auto fd_d = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(fd_path.c_str()), closedir);
        if (!fd_d) {
            PLOG(WARNING) << "Failed to open " << fd_path;
        } else {
            struct dirent* fd_de;
            while ((fd_de = readdir(fd_d.get())) != nullptr) {
                if (fd_de->d_type != DT_LNK) continue;
                found |= checkSymlink(fd_path + "/" + fd_de->d_name, prefix);
            }
        }

        if (found) {
            if (!IsFuseDaemon(pid) || killFuseDaemon) {
                pids.insert(pid);
            } else {
                LOG(WARNING) << "Found FUSE daemon with open file. Skipping...";
            }
        }
    }
    if (signal != 0) {
        for (const auto& pid : pids) {
            std::string comm;
            android::base::ReadFileToString(StringPrintf("/proc/%d/comm", pid), &comm);
            comm = android::base::Trim(comm);

            std::string exe;
            android::base::Readlink(StringPrintf("/proc/%d/exe", pid), &exe);

            LOG(WARNING) << "Sending " << strsignal(signal) << " to pid " << pid << " (" << comm
                         << ", " << exe << ")";
            kill(pid, signal);
        }
    }
    return pids.size();
}
