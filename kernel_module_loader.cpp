#include "kernel_module_loader.hpp"

bool KernelModuleLoader::Load_Vendor_Modules(BOOT_MODE mode) {
    std::string base_dir(VENDOR_MODULE_DIR);
    std::vector<std::string> module_dirs;
    bool recovery_mode = false;

    module_dirs.push_back(VENDOR_MODULE_DIR);
    TWPartition* ven = PartitionManager.Find_Partition_By_Path("/vendor");

    switch (mode) {
        case RECOVERY_FASTBOOT_MODE:
            module_dirs.push_back(base_dir + "/1.1/");
            break;
        case RECOVERY_IN_BOOT_MODE:
        case FASTBOOTD_MODE:
            recovery_mode = true;
            struct utsname uts;
            if (uname(&uts)) {
                LOGERR("Unable to query kernel for version info\n");
            }
            std::string rls(uts.release);
            std::vector<std::string> release = TWFunc::split_string(rls, '.', true);
            module_dirs.push_back(base_dir + "/" + release[0] + "." + release[1] + "/");
            if (ven) {
                ven->Mount(true);
            } else {
                LOGERR("Unable to mount vendor partition to load kernel modules.\n");
                return false;
            }
            break;
    }

    for (auto&& module_dir:module_dirs) {
        LOGINFO("module_dir: %s\n", module_dir.c_str());
        std::string dest_module_dir;
        if (recovery_mode) {
            dest_module_dir = "/tmp" + module_dir;
            TWFunc::Recursive_Mkdir(dest_module_dir);
            Copy_Modules_To_Ramdisk(module_dir);
            Write_Module_List(dest_module_dir);
        } else {
            dest_module_dir = module_dir;
            Write_Module_List(module_dir);
        }
        Modprobe m({dest_module_dir}, "modules.load.twrp");
        m.EnableVerbose(true);
        m.LoadListedModules(false);
        int modules_loaded = m.GetModuleCount();
        LOGINFO("modules_loaded: %d\n", modules_loaded);
    }
    if (ven)
        ven->UnMount(false);

	return true;
}

bool KernelModuleLoader::Write_Module_List(std::string module_dir) {
	DIR* d;
	struct dirent* de;
	std::vector<std::string> kernel_modules;
	std::vector<std::string> kernel_modules_requested = TWFunc::split_string(EXPAND(TW_LOAD_VENDOR_MODULES), ' ', true);
	d = opendir(module_dir.c_str());
    LOGINFO("Opening module_dir: %s\n", module_dir.c_str());
	if (d != nullptr) {
		while ((de = readdir(d)) != nullptr) {
			std::string kernel_module = de->d_name;
			if (de->d_type == DT_REG) {
				if (android::base::EndsWith(kernel_module, ".ko")) {
					for (auto&& requested:kernel_modules_requested) {
						if (kernel_module == requested) {
							kernel_modules.push_back(kernel_module);
                            continue;
                        }
					}
					continue;
				}
			}
		} 
        std::string module_file = module_dir + "/modules.load.twrp";
        LOGINFO("module_file: %s\n", module_file.c_str());
		TWFunc::write_to_file(module_file, kernel_modules);
	}
	return true;
}

bool KernelModuleLoader::Copy_Modules_To_Ramdisk(std::string module_dir) {
    std::string ramdisk_dir = "/tmp/" + module_dir;
    DIR* d;
	struct dirent* de;
    d = opendir(module_dir.c_str());
    if (d != nullptr) {
        while ((de = readdir(d)) != nullptr) {
            std::string kernel_module = de->d_name;
            if (de->d_type == DT_REG) {
                std::string src =  module_dir + "/" + de->d_name;
                std::string dest = ramdisk_dir + "/" + de->d_name;
                if (TWFunc::copy_file(src, dest, 0700) != 0) {
                    return false;
                }
            }
        } 
    } else {
        LOGINFO("Unable to open module directory: %s. Skipping\n", module_dir.c_str());
        return false;
    }
    return true;
}