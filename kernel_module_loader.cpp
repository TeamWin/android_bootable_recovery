#include "kernel_module_loader.hpp"

bool KernelModuleLoader::Load_Vendor_Modules(BOOT_MODE mode) {
    // check /lib/modules (ramdisk vendor_boot)
    // check /lib/modules/N.N (ramdisk vendor_boot)
    // check /lib/modules/N.N-gki (ramdisk vendor_boot)
    // check /vendor/lib/modules (ramdisk)
    // check /vendor/lib/modules/1.1 (ramdisk prebuilt modules)
    // check /vendor/lib/modules/N.N (vendor mounted)
    // check /vendor/lib/modules/N.N-gki (vendor mounted)

    LOGINFO("Attempting to load modules\n");
    std::string vendor_base_dir(VENDOR_MODULE_DIR);
    std::string base_dir(VENDOR_BOOT_MODULE_DIR);
    std::vector<std::string> module_dirs;
    std::vector<std::string> vendor_module_dirs;

    TWPartition* ven = PartitionManager.Find_Partition_By_Path("/vendor");
    vendor_module_dirs.push_back(VENDOR_MODULE_DIR);
    vendor_module_dirs.push_back(vendor_base_dir + "/1.1");

    module_dirs.push_back(base_dir);

    struct utsname uts;
    if (uname(&uts)) {
        LOGERR("Unable to query kernel for version info\n");
    }

    std::string rls(uts.release);
    std::vector<std::string> release = TWFunc::split_string(rls, '.', true);
    module_dirs.push_back(base_dir + "/" + release[0] + "." + release[1]);

    for (auto&& module_dir:module_dirs) {
        Try_And_Load_Modules(module_dir);
    }

    for (auto&& module_dir:vendor_module_dirs) {
        Try_And_Load_Modules(module_dir);
    }

    if (ven) {
        LOGINFO("Checking mounted /vendor\n");
        ven->Mount(true);
    }

    for (auto&& module_dir:vendor_module_dirs) {
        Try_And_Load_Modules(module_dir);
    }

    if (ven)
        ven->UnMount(false);

	return true;
}

bool KernelModuleLoader::Try_And_Load_Modules(std::string module_dir) {
        LOGINFO("Checking directory: %s\n", module_dir.c_str());
        std::string dest_module_dir;
        dest_module_dir = "/tmp" + module_dir;
        TWFunc::Recursive_Mkdir(dest_module_dir);
        Copy_Modules_To_Tmpfs(module_dir);
        Write_Module_List(dest_module_dir);
        Modprobe m({dest_module_dir}, "modules.load.twrp");
        m.EnableVerbose(true);
        m.LoadListedModules(false);
        int modules_loaded = m.GetModuleCount();
        LOGINFO("Modules Loaded: %d\n", modules_loaded);
        return modules_loaded > 0 ? true : false;
}

bool KernelModuleLoader::Write_Module_List(std::string module_dir) {
	DIR* d;
	struct dirent* de;
	std::vector<std::string> kernel_modules;
	std::vector<std::string> kernel_modules_requested = TWFunc::split_string(EXPAND(TW_LOAD_VENDOR_MODULES), ' ', true);
	d = opendir(module_dir.c_str());
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
		TWFunc::write_to_file(module_file, kernel_modules);
        closedir(d);
	}
	return true;
}

bool KernelModuleLoader::Copy_Modules_To_Tmpfs(std::string module_dir) {
    std::string ramdisk_dir = "/tmp" + module_dir;
    DIR* d;
	struct dirent* de;
    d = opendir(module_dir.c_str());
    if (d != nullptr) {
        while ((de = readdir(d)) != nullptr) {
            std::string kernel_module = de->d_name;
            if (de->d_type == DT_REG) {
                std::string src =  module_dir + "/" + de->d_name;
                std::string dest = ramdisk_dir + "/" + de->d_name;
                if (TWFunc::copy_file(src, dest, 0700, false) != 0) {
                    return false;
                }
            }
        }
        closedir(d);
    } else {
        LOGINFO("Unable to open module directory: %s. Skipping\n", module_dir.c_str());
        return false;
    }
    return true;
}