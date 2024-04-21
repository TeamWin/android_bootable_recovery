/*
	Copyright 2012 to 2017 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string.h>
#include <stdio.h>
#include <cutils/properties.h>

#include <android-base/unique_fd.h>

#include "twcommon.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"

#include "otautil/sysutil.h"
#include <ziparchive/zip_archive.h>
#include "twinstall/install.h"
#include "twinstall/verifier.h"
#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrpDigestDriver.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrp-functions.hpp"
#include "gui/gui.hpp"
#include "gui/pages.hpp"
#include "twinstall.h"
#include "installcommand.h"
#include "../twrpRepacker.hpp"
extern "C" {
	#include "gui/gui.h"
}

#define AB_OTA "payload_properties.txt"

enum zip_type {
	UNKNOWN_ZIP_TYPE = 0,
	UPDATE_BINARY_ZIP_TYPE,
	AB_OTA_ZIP_TYPE,
	TWRP_THEME_ZIP_TYPE
};

static int Install_Theme(const char* path, ZipArchiveHandle Zip) {
#ifdef TW_OEM_BUILD // We don't do custom themes in OEM builds
	return INSTALL_CORRUPT;
#else
	std::string binary_name("ui.xml");
	ZipEntry64 binary_entry;
	if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
		return INSTALL_CORRUPT;
	}
	if (!PartitionManager.Mount_Current_Storage(true))
		return INSTALL_ERROR;
	string theme_path = DataManager::GetCurrentStoragePath();
	theme_path += DataManager::GetStrValue(TW_RECOVERY_FOLDER_VAR) + "/theme";
	if (!TWFunc::Path_Exists(theme_path)) {
		if (!TWFunc::Recursive_Mkdir(theme_path)) {
			return INSTALL_ERROR;
		}
	}
	theme_path += "/ui.zip";
	if (TWFunc::copy_file(path, theme_path, 0644) != 0) {
		return INSTALL_ERROR;
	}
	LOGINFO("Installing custom theme '%s' to '%s'\n", path, theme_path.c_str());
	PageManager::RequestReload();
	return INSTALL_SUCCESS;
#endif
}

static int Prepare_Update_Binary(ZipArchiveHandle Zip) {
	char arches[PATH_MAX];
	property_get("ro.product.cpu.abilist", arches, "error");
	if (strcmp(arches, "error") == 0)
		property_get("ro.product.cpu.abi", arches, "error");
	vector<string> split = TWFunc::split_string(arches, ',', true);
	std::vector<string>::iterator arch;
	std::string base_name = UPDATE_BINARY_NAME;
	base_name += "-";
	ZipEntry64 binary_entry;
	std::string update_binary_string(UPDATE_BINARY_NAME);
	if (FindEntry(Zip, update_binary_string, &binary_entry) != 0) {
		for (arch = split.begin(); arch != split.end(); arch++) {
			std::string temp = base_name + *arch;
			std::string binary_name(temp.c_str());
			if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
				std::string binary_name(temp.c_str());
				break;
			}
		}
	}
	LOGINFO("Extracting updater binary '%s'\n", UPDATE_BINARY_NAME);
	unlink(TMP_UPDATER_BINARY_PATH);
	android::base::unique_fd fd(
		open(TMP_UPDATER_BINARY_PATH, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755));
	if (fd == -1) {
		return INSTALL_ERROR;
	}
	int32_t err = ExtractEntryToFile(Zip, &binary_entry, fd);
	if (err != 0) {
		LOGERR("Could not extract '%s'\n", UPDATE_BINARY_NAME);
		return INSTALL_ERROR;
	}

	// If exists, extract file_contexts from the zip file
	std::string file_contexts("file_contexts");
	ZipEntry64 file_contexts_entry;
	if (FindEntry(Zip, file_contexts, &file_contexts_entry) != 0) {
		LOGINFO("Zip does not contain SELinux file_contexts file in its root.\n");
	} else {
		const string output_filename = "/file_contexts";
		LOGINFO("Zip contains SELinux file_contexts file in its root. Extracting to %s\n", output_filename.c_str());
		android::base::unique_fd fd(
			open(output_filename.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
		if (fd == -1) {
			return INSTALL_ERROR;
		}
		if (ExtractEntryToFile(Zip, &file_contexts_entry, fd)) {
			LOGERR("Could not extract '%s'\n", output_filename.c_str());
			return INSTALL_ERROR;
		}
	}
	return INSTALL_SUCCESS;
}


static int Run_Update_Binary(const char *path, int* wipe_cache, zip_type ztype) {
	int ret_val, pipe_fd[2], status, zip_verify;
	char buffer[1024];
	FILE* child_data;
	pipe(pipe_fd);

	std::vector<std::string> args;
    if (ztype == UPDATE_BINARY_ZIP_TYPE) {
		ret_val = update_binary_command(path, 0, pipe_fd[1], &args);
    } else if (ztype == AB_OTA_ZIP_TYPE) {
		ret_val = abupdate_binary_command(path, 0, pipe_fd[1], &args);
	} else {
		LOGERR("Unknown zip type %i\n", ztype);
		ret_val = INSTALL_CORRUPT;
	}
    if (ret_val) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return ret_val;
    }

	// Convert the vector to a NULL-terminated char* array suitable for execv.
	const char* chr_args[args.size() + 1];
	chr_args[args.size()] = NULL;
	for (size_t i = 0; i < args.size(); i++)
		chr_args[i] = args[i].c_str();

	pid_t pid = fork();
	if (pid == 0) {
		close(pipe_fd[0]);
		execve(chr_args[0], const_cast<char**>(chr_args), environ);
		printf("E:Can't execute '%s': %s\n", chr_args[0], strerror(errno));
		_exit(-1);
	}
	close(pipe_fd[1]);

	*wipe_cache = 0;

	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
	child_data = fdopen(pipe_fd[0], "r");
	while (fgets(buffer, sizeof(buffer), child_data) != NULL) {
		char* command = strtok(buffer, " \n");
		if (command == NULL) {
			continue;
		} else if (strcmp(command, "progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			char* seconds_char = strtok(NULL, " \n");

			float fraction_float = strtof(fraction_char, NULL);
			int seconds_float = strtol(seconds_char, NULL, 10);

			if (zip_verify)
				DataManager::ShowProgress(fraction_float * (1 - VERIFICATION_PROGRESS_FRACTION), seconds_float);
			else
				DataManager::ShowProgress(fraction_float, seconds_float);
		} else if (strcmp(command, "set_progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			float fraction_float = strtof(fraction_char, NULL);
			DataManager::_SetProgress(fraction_float);
		} else if (strcmp(command, "ui_print") == 0) {
			char* display_value = strtok(NULL, "\n");
			if (display_value) {
				gui_print("%s", display_value);
			} else {
				gui_print("\n");
			}
		} else if (strcmp(command, "wipe_cache") == 0) {
			*wipe_cache = 1;
		} else if (strcmp(command, "clear_display") == 0) {
			// Do nothing, not supported by TWRP
		} else if (strcmp(command, "log") == 0) {
			printf("%s\n", strtok(NULL, "\n"));
		} else {
			LOGERR("unknown command [%s]\n", command);
		}
	}
	fclose(child_data);

	int waitrc = TWFunc::Wait_For_Child(pid, &status, "Updater");
	if (waitrc != 0)
		return INSTALL_ERROR;

	return INSTALL_SUCCESS;
}

int TWinstall_zip(const char* path, int* wipe_cache, bool check_for_digest) {
	int ret_val, zip_verify = 1, unmount_system = 1, reflashtwrp = 0;

	gui_msg(Msg("installing_zip=Installing zip file '{1}'")(path));
	if (strlen(path) < 9 || strncmp(path, "/sideload", 9) != 0) {
		string digest_str;
		string Full_Filename = path;

		if (check_for_digest) {
			gui_msg("check_for_digest=Checking for Digest file...");
			if (*path != '@' && !twrpDigestDriver::Check_File_Digest(Full_Filename)) {
				LOGERR("Aborting zip install: Digest verification failed\n");
				return INSTALL_CORRUPT;
			}
		}
	}

	DataManager::GetValue(TW_UNMOUNT_SYSTEM, unmount_system);

#ifndef TW_OEM_BUILD
	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
#endif
	DataManager::SetProgress(0);

	auto package = Package::CreateMemoryPackage(path);
	if (!package) {
		return INSTALL_CORRUPT;
	}

	if (zip_verify) {
		gui_msg("verify_zip_sig=Verifying zip signature...");
		static constexpr const char* CERTIFICATE_ZIP_FILE = "/system/etc/security/otacerts.zip";
		std::vector<Certificate> loaded_keys = LoadKeysFromZipfile(CERTIFICATE_ZIP_FILE);
		if (loaded_keys.empty()) {
			LOGERR("Failed to load keys\n");
			return -1;
		}
		LOGINFO("%zu key(s) loaded from %s\n", loaded_keys.size(), CERTIFICATE_ZIP_FILE);

		ret_val = verify_file(package.get(), loaded_keys, std::bind(&DataManager::SetProgress, std::placeholders::_1));
		if (ret_val != VERIFY_SUCCESS) {
			LOGINFO("Zip signature verification failed: %i\n", ret_val);
			gui_err("verify_zip_fail=Zip signature verification failed!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			return -1;
		} else {
			gui_msg("verify_zip_done=Zip signature verified successfully.");
		}
	}

	ZipArchiveHandle Zip = package->GetZipArchiveHandle();
	if (!Zip) {
		return INSTALL_CORRUPT;
	}

	if (unmount_system) {
		gui_msg("unmount_system=Unmounting System...");
		if(!PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), true)) {
			gui_err("unmount_system_err=Failed unmounting System");
			return -1;
		}
		unlink("/system");
		mkdir("/system", 0755);
	}

	time_t start, stop;
	time(&start);

	std::string update_binary_name(UPDATE_BINARY_NAME);
	ZipEntry64 update_binary_entry;
	if (FindEntry(Zip, update_binary_name, &update_binary_entry) == 0) {
		LOGINFO("Update binary zip\n");
		// Additionally verify the compatibility of the package.
		if (!verify_package_compatibility(Zip)) {
			gui_err("zip_compatible_err=Zip Treble compatibility error!");
			ret_val = INSTALL_CORRUPT;
		} else {
			ret_val = Prepare_Update_Binary(Zip);
			if (ret_val == INSTALL_SUCCESS)
				ret_val = Run_Update_Binary(path, wipe_cache, UPDATE_BINARY_ZIP_TYPE);
		}
	} else {
		std::string ab_binary_name(AB_OTA);
		ZipEntry64 ab_binary_entry;
		if (FindEntry(Zip, ab_binary_name, &ab_binary_entry) == 0) {
			LOGINFO("AB zip\n");
			gui_msg(Msg(msg::kHighlight, "flash_ab_inactive=Flashing A/B zip to inactive slot: {1}")(PartitionManager.Get_Active_Slot_Display()=="A"?"B":"A"));
			// We need this so backuptool can do its magic
			bool system_mount_state = PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path());
			bool vendor_mount_state = PartitionManager.Is_Mounted_By_Path("/vendor");
			PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
			PartitionManager.Mount_By_Path("/vendor", false);
			TWFunc::copy_file("/system/bin/sh", "/tmp/sh", 0755);
			mount("/tmp/sh", "/system/bin/sh", "auto", MS_BIND, NULL);
			ret_val = Run_Update_Binary(path, wipe_cache, AB_OTA_ZIP_TYPE);
			umount("/system/bin/sh");
			unlink("/tmp/sh");
			if (!vendor_mount_state)
				PartitionManager.UnMount_By_Path("/vendor", false);
			if (!system_mount_state)
				PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
			if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false)) {
				PartitionManager.Unlock_Block_Partitions();
				PartitionManager.Prepare_All_Super_Volumes();
				gui_warn("mount_vab_partitions=Devices on super may not mount until rebooting recovery.");
			}
			gui_warn("flash_ab_reboot=To flash additional zips, please reboot recovery to switch to the updated slot.");
			DataManager::GetValue(TW_AUTO_REFLASHTWRP_VAR, reflashtwrp);
			if (reflashtwrp) {
			twrpRepacker repacker;
			repacker.Flash_Current_Twrp();
			}
		} else {
			std::string binary_name("ui.xml");
			ZipEntry64 binary_entry;
			if (FindEntry(Zip, binary_name, &binary_entry) == 0) {
				LOGINFO("TWRP theme zip\n");
				ret_val = Install_Theme(path, Zip);
			} else {
				ret_val = INSTALL_CORRUPT;
			}
		}
	}
	time(&stop);
	int total_time = (int) difftime(stop, start);
	if (ret_val == INSTALL_CORRUPT) {
		gui_err("invalid_zip_format=Invalid zip file format!");
	} else {
		LOGINFO("Install took %i second(s).\n", total_time);
	}
	return ret_val;
}
