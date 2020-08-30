#ifndef STARTUPARGS_HPP
#define STARTUPARGS_HPP
#include <android-base/properties.h>

#include "data.hpp"
#include "gui/gui.hpp"
#include "openrecoveryscript.hpp"
#include "partitions.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "variables.h"
#include "bootloader_message/include/bootloader_message/bootloader_message.h"
#include "install/get_args.h"

class startupArgs {
public:
	static inline std::string const UPDATE_PACKAGE = "--update_package";
	static inline std::string const WIPE_CACHE = "--wipe_cache";
	static inline std::string const WIPE_DATA = "--wipe_data";
	static inline std::string const SEND_INTENT = "--send_intent";
	static inline std::string const SIDELOAD = "--sideload";
	static inline std::string const REASON = "--reason";
	static inline std::string const FASTBOOT = "--fastboot";
	static inline std::string const NANDROID = "--nandroid";
	void parse(int *argc, char ***argv);
	bool Should_Skip_Decryption();
	std::string Get_Intent();
	bool Get_Fastboot_Mode();

private:
	bool SkipDecryption = false;
	bool fastboot_mode = false;
	std::string Send_Intent;
};
#endif