#ifndef _KERNELMODULELOADER_HPP
#define _KERNELMODULELOADER_HPP

#include <dirent.h>
#include <string>
#include <vector>
#include <android-base/strings.h>
#include <modprobe/modprobe.h>
#include <sys/utsname.h>

#include "twcommon.h"
#include "twrp-functions.hpp"

#define VENDOR_MODULE_DIR "/vendor/lib/modules"    // Base path for vendor kernel modules to load by TWRP
typedef enum {
	RECOVERY_FASTBOOT_MODE = 0,
	RECOVERY_IN_BOOT_MODE,
	FASTBOOTD_MODE
} BOOT_MODE;

class KernelModuleLoader
{
public:
    static bool Load_Vendor_Modules(BOOT_MODE mode); // Load specific maintainer defined kernel modules in TWRP

private:
	static bool Write_Module_List(std::string module_dir); // Write list of modules to load from TW_LOAD_VENDOR_MODULES
    static bool Copy_Modules_To_Ramdisk(std::string module_dir); // Copy modules to ramdisk for loading
};

#endif // _KERNELMODULELOADER_HPP