#ifndef _KERNELMODULELOADER_HPP
#define _KERNELMODULELOADER_HPP

#include <dirent.h>
#include <string>
#include <vector>
#include <android-base/file.h>
#include <android-base/strings.h>
#include <modprobe/modprobe.h>
#include <sys/mount.h>
#include <sys/utsname.h>

#include "twcommon.h"
#include "twrp-functions.hpp"

#define VENDOR_MODULE_DIR "/vendor/lib/modules"           // Base path for vendor kernel modules to check by TWRP
#define VENDOR_BOOT_MODULE_DIR "/lib/modules"             // vendor_boot ramdisk GKI modules to check by TWRP
#define VENDOR_DLKM_MODULE_DIR "/vendor_dlkm/lib/modules" // vendor_dlkm placed modules to check by TWRP
typedef enum {
	RECOVERY_FASTBOOT_MODE = 0,
	RECOVERY_IN_BOOT_MODE,
	FASTBOOTD_MODE
} BOOT_MODE;

class KernelModuleLoader
{
public:
	static bool Load_Vendor_Modules(); // Load specific maintainer defined kernel modules in TWRP

private:
	static int Try_And_Load_Modules(std::string module_dir, bool vendor_is_mounted); // Use libmodprobe to attempt loading kernel modules
	static bool Write_Module_List(std::string module_dir); // Write list of modules to load from TW_LOAD_VENDOR_MODULES
	static bool Copy_Modules_To_Tmpfs(std::string module_dir); // Copy modules to ramdisk for loading
	static std::vector<string> Skip_Loaded_Kernel_Modules(); // return list of loaded kernel modules already done by init
	static BOOT_MODE Get_Boot_Mode(); // For getting the current boot mode
};

#endif // _KERNELMODULELOADER_HPP
