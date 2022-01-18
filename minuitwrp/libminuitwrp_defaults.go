package twrp

import (
	"android/soong/android"
	"android/soong/cc"
	"fmt"
	"path/filepath"
	"strings"
)

func globalFlags(ctx android.BaseContext) []string {
	var cflags []string

	if getMakeVars(ctx, "TW_SUPPORT_INPUT_1_2_HAPTICS") == "true" {
		cflags = append(cflags, "-DUSE_QTI_HAPTICS")
	}

	if getMakeVars(ctx, "TW_SUPPORT_INPUT_AIDL_HAPTICS") == "true" {
		cflags = append(cflags, "-DUSE_QTI_AIDL_HAPTICS")
	}

	if getMakeVars(ctx, "TW_USE_SAMSUNG_HAPTICS") == "true" {
                cflags = append(cflags, "-DUSE_SAMSUNG_HAPTICS")
        }

	if getMakeVars(ctx, "TW_TARGET_USES_QCOM_BSP") == "true" {
		cflags = append(cflags, "-DMSM_BSP")
	}

	if getMakeVars(ctx, "TW_NEW_ION_HEAP") == "true" {
		cflags = append(cflags, "-DNEW_ION_HEAP")
	}

	matches, err := filepath.Glob("external/libdrm/Android.*")
	_ = matches
	if err == nil {
		cflags = append(cflags, "-DHAS_DRM")
	}

	if getMakeVars(ctx, "TW_INCLUDE_JPEG") != "" {
		cflags = append(cflags, "-DTW_INCLUDE_JPEG")
	}

	if getMakeVars(ctx, "RECOVERY_TOUCHSCREEN_SWAP_XY") == "true" {
		cflags = append(cflags, "-DRECOVERY_TOUCHSCREEN_SWAP_XY")
	}

	if getMakeVars(ctx, "RECOVERY_TOUCHSCREEN_FLIP_X") == "true" {
		cflags = append(cflags, "-DRECOVERY_TOUCHSCREEN_FLIP_X")
	}

	if getMakeVars(ctx, "RECOVERY_TOUCHSCREEN_FLIP_Y") == "true" {
		cflags = append(cflags, "-DRECOVERY_TOUCHSCREEN_FLIP_Y")
	}

	if getMakeVars(ctx, "RECOVERY_GRAPHICS_FORCE_USE_LINELENGTH") == "true" {
		cflags = append(cflags, "-DRECOVERY_GRAPHICS_FORCE_USE_LINELENGTH")
	}

	if getMakeVars(ctx, "RECOVERY_GRAPHICS_FORCE_SINGLE_BUFFER") == "true" {
		cflags = append(cflags, "-DRECOVERY_GRAPHICS_FORCE_SINGLE_BUFFER")
	}

	if getMakeVars(ctx, "TWRP_EVENT_LOGGING") == "true" {
		cflags = append(cflags, "-D_EVENT_LOGGING")
	}

	var pixelFormat = strings.Replace(getMakeVars(ctx, "TARGET_RECOVERY_FORCE_PIXEL_FORMAT"), "\"", "", -1)

	switch pixelFormat {
	case "RGBA_8888":
		fmt.Println("****************************************************************************)")
		fmt.Println("* TARGET_RECOVERY_FORCE_PIXEL_FORMAT := RGBA_8888 not implemented yet      *)")
		fmt.Println("****************************************************************************)")
		cflags = append(cflags, "-DRECOVERY_RGBA")
		break

	case "RGBX_8888":
		fmt.Println("****************************************************************************)")
		fmt.Println("* TARGET_RECOVERY_FORCE_PIXEL_FORMAT := RGBX_8888 not implemented yet      *)")
		fmt.Println("****************************************************************************)")
		cflags = append(cflags, "-DRECOVERY_RGBX")
		break

	case "BGRA_8888":
		fmt.Println("****************************************************************************)")
		fmt.Println("* TARGET_RECOVERY_FORCE_PIXEL_FORMAT := BGRA_8888 not implemented yet      *)")
		fmt.Println("****************************************************************************)")
		cflags = append(cflags, "-DRECOVERY_BGRA")
		break

	case "RGB_565":
		cflags = append(cflags, "-DRECOVERY_FORCE_RGB_565")
		break
	}

	pixelFormat = strings.Replace(getMakeVars(ctx, "TARGET_RECOVERY_PIXEL_FORMAT"), "\"", "", -1)
	switch pixelFormat {
	case "ABGR_8888":
		cflags = append(cflags, "-DRECOVERY_ABGR")
		break

	case "RGBX_8888":
		cflags = append(cflags, "-DRECOVERY_RGBX")
		break

	case "BGRA_8888":
		cflags = append(cflags, "-DRECOVERY_BGRA")
		break
	}

	if getMakeVars(ctx, "TARGET_RECOVERY_OVERSCAN_PERCENT") != "" {
		cflags = append(cflags, "-DDOVERSCAN_PERCENT="+getMakeVars(ctx, "TARGET_RECOVERY_OVERSCAN_PERCENT"))
	} else {
		cflags = append(cflags, "-DOVERSCAN_PERCENT=0")
	}

	if getMakeVars(ctx, "TW_SCREEN_BLANK_ON_BOOT") == "true" {
		cflags = append(cflags, "-DTW_SCREEN_BLANK_ON_BOOT")
	}

	if getMakeVars(ctx, "TW_FBIOPAN") == "true" {
		cflags = append(cflags, "-DTW_FBIOPAN")
	}

	var tw_rotation = getMakeVars(ctx, "TW_ROTATION")
	switch tw_rotation {
	case "0":
		fallthrough
	case "90":
		fallthrough
	case "180":
		fallthrough
	case "270":
		cflags = append(cflags, "-DTW_ROTATION="+tw_rotation)
	default:
		if getMakeVars(ctx, "BOARD_HAS_FLIPPED_SCREEN") == "true" {
			cflags = append(cflags, "-DTW_ROTATION=180")
		} else {
			cflags = append(cflags, "-DTW_ROTATION=0")
		}
	}

	if getMakeVars(ctx, "TW_IGNORE_MAJOR_AXIS_0") == "true" {
		cflags = append(cflags, "-DTW_IGNORE_MAJOR_AXIS_0")
	}

	if getMakeVars(ctx, "TW_IGNORE_MT_POSITION_0") == "true" {
		cflags = append(cflags, "-DTW_IGNORE_MT_POSITION_0")
	}

	if getMakeVars(ctx, "TW_IGNORE_ABS_MT_TRACKING_ID") == "true" {
		cflags = append(cflags, "-DTW_IGNORE_ABS_MT_TRACKING_ID")
	}

	if getMakeVars(ctx, "TW_INPUT_BLACKLIST") != "" {
		cflags = append(cflags, "-DTW_INPUT_BLACKLIST="+getMakeVars(ctx, "TW_INPUT_BLACKLIST"))
	}

	if getMakeVars(ctx, "TW_WHITELIST_INPUT") != "" {
		cflags = append(cflags, "-DWHITELIST_INPUT="+getMakeVars(ctx, "TW_WHITELIST_INPUT"))
	}

	if getMakeVars(ctx, "TW_HAPTICS_TSPDRV") == "true" {
		cflags = append(cflags, "-DTW_HAPTICS_TSPDRV")
	}

	return cflags
}

func globalSrcs(ctx android.BaseContext) []string {
	var srcs []string

	if getMakeVars(ctx, "TW_TARGET_USES_QCOM_BSP") == "true" {
		srcs = append(srcs, "graphics_overlay.cpp")
	}

	matches, err := filepath.Glob("external/libdrm/Android.*")
	_ = matches
	if err == nil {
		srcs = append(srcs, "graphics_drm.cpp")
	}

	if getMakeVars(ctx, "TW_HAPTICS_TSPDRV") == "true" {
		srcs = append(srcs, "tspdrv.cpp")
	}
	return srcs
}

func globalIncludes(ctx android.BaseContext) []string {
	var includes []string

	if getMakeVars(ctx, "TW_TARGET_USES_QCOM_BSP") == "true" {
		if getMakeVars(ctx, "TARGET_PREBUILT_KERNEL") != "" {
			includes = append(includes, getMakeVars(ctx, "TARGET_OUT_INTERMEDIATES")+"/KERNEL_OBJ/usr/include")
		} else {
			if getMakeVars(ctx, "TARGET_CUSTOM_KERNEL_HEADERS") != "" {
				includes = append(includes, "bootable/recovery/minuitwrp")
			} else {
				includes = append(includes, getMakeVars(ctx, "TARGET_CUSTOM_KERNEL_HEADERS"))
			}
		}
	} else {
		includes = append(includes, "bootable/recovery/minuitwrp")
	}

	if getMakeVars(ctx, "TW_INCLUDE_JPEG") != "" {
		includes = append(includes, "external/jpeg")
	}

	return includes
}

func globalStaticLibs(ctx android.BaseContext) []string {
	var staticLibs []string

	matches, err := filepath.Glob("external/libdrm/Android.*")
	_ = matches
	if err == nil {
		matches, err = filepath.Glob("external/libdrm/Android.common.mk")
		if err != nil {
			staticLibs = append(staticLibs, "libdrm_platform")
		} else {
			staticLibs = append(staticLibs, "libdrm")
		}
	}

	return staticLibs
}

func globalSharedLibs(ctx android.BaseContext) []string {
	var sharedLibs []string

	if getMakeVars(ctx, "TW_SUPPORT_INPUT_1_2_HAPTICS") == "true" {
		sharedLibs = append(sharedLibs, "android.hardware.vibrator@1.2")
		sharedLibs = append(sharedLibs, "libhidlbase")
	}

	if getMakeVars(ctx, "TW_SUPPORT_INPUT_AIDL_HAPTICS") == "true" {
		sharedLibs = append(sharedLibs, "android.hardware.vibrator-V1-ndk_platform")
		sharedLibs = append(sharedLibs, "android.hardware.vibrator-V1-cpp")
	}

	if getMakeVars(ctx, "TW_INCLUDE_JPEG") != "" {
		sharedLibs = append(sharedLibs, "libjpeg")
	}
	return sharedLibs
}

func globalRequiredModules(ctx android.BaseContext) []string {
	var requiredModules []string

	if getMakeVars(ctx, "TARGET_PREBUILT_KERNEL") != "" {
		var kernelDir = getMakeVars(ctx, "TARGET_OUT_INTERMEDIATES") + ")/KERNEL_OBJ/usr"
		requiredModules = append(requiredModules, kernelDir)
	}
	return requiredModules
}

func libMinuiTwrpDefaults(ctx android.LoadHookContext) {
	type props struct {
		Target struct {
			Android struct {
				Cflags  []string
				Enabled *bool
			}
		}
		Cflags       []string
		Srcs         []string
		Include_dirs []string
		Static_libs  []string
		Shared_libs  []string
		Required     []string
	}

	p := &props{}
	p.Cflags = globalFlags(ctx)
	s := globalSrcs(ctx)
	p.Srcs = s
	i := globalIncludes(ctx)
	p.Include_dirs = i
	staticLibs := globalStaticLibs(ctx)
	p.Static_libs = staticLibs
	sharedLibs := globalSharedLibs(ctx)
	p.Shared_libs = sharedLibs
	requiredModules := globalRequiredModules(ctx)
	p.Required = requiredModules
	ctx.AppendProperties(p)
}

func init() {
	android.RegisterModuleType("libminuitwrp_defaults", libMinuiTwrpDefaultsFactory)
}

func libMinuiTwrpDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, libMinuiTwrpDefaults)

	return module
}
