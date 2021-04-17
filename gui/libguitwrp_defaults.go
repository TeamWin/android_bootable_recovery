package libgui_defaults

import (
	"android/soong/android"
	"android/soong/cc"
)

func globalFlags(ctx android.BaseContext) []string {
	var cflags []string

	if ctx.AConfig().Getenv("TW_DELAY_TOUCH_INIT_MS") != "" {
		cflags = append(cflags, "-DTW_DELAY_TOUCH_INIT_MS="+ctx.AConfig().Getenv("TW_DELAY_TOUCH_INIT_MS"))
	}

	if ctx.AConfig().Getenv("TW_EVENT_LOGGING") == "true" {
		cflags = append(cflags, "-D_EVENT_LOGGING")
	}

	if ctx.AConfig().Getenv("TW_USE_KEY_CODE_TOUCH_SYNC") != "" {
		cflags = append(cflags, "-DTW_USE_KEY_CODE_TOUCH_SYNC="+ctx.AConfig().Getenv("TW_USE_KEY_CODE_TOUCH_SYNC"))
	}

	if ctx.AConfig().Getenv("TW_SCREEN_BLANK_ON_BOOT") != "" {
		cflags = append(cflags, "-DTW_SCREEN_BLANK_ON_BOOT")
	}

	if ctx.AConfig().Getenv("TW_OZIP_DECRYPT_KEY") != "" {
		cflags = append(cflags, "-DTW_OZIP_DECRYPT_KEY=\""+ctx.AConfig().Getenv("TW_OZIP_DECRYPT_KEY")+"\"")
	} else {
		cflags = append(cflags, "-DTW_OZIP_DECRYPT_KEY=0")
	}

	if ctx.AConfig().Getenv("TW_NO_SCREEN_BLANK") != "" {
		cflags = append(cflags, "-DTW_NO_SCREEN_BLANK")
	}

	if ctx.AConfig().Getenv("TW_NO_SCREEN_TIMEOUT") != "" {
		cflags = append(cflags, "-DTW_NO_SCREEN_TIMEOUT")
	}

	if ctx.AConfig().Getenv("TW_OEM_BUILD") != "" {
		cflags = append(cflags, "-DTW_OEM_BUILD")
	}

	if ctx.AConfig().Getenv("TW_X_OFFSET") != "" {
		cflags = append(cflags, "-DTW_X_OFFSET="+ctx.AConfig().Getenv("TW_X_OFFSET"))
	}

	if ctx.AConfig().Getenv("TW_Y_OFFSET") != "" {
		cflags = append(cflags, "-DTW_Y_OFFSET="+ctx.AConfig().Getenv("TW_Y_OFFSET"))
	}

	if ctx.AConfig().Getenv("TW_W_OFFSET") != "" {
		cflags = append(cflags, "-DTW_W_OFFSET="+ctx.AConfig().Getenv("TW_W_OFFSET"))
	}

	if ctx.AConfig().Getenv("TW_H_OFFSET") != "" {
		cflags = append(cflags, "-DTW_H_OFFSET="+ctx.AConfig().Getenv("TW_H_OFFSET"))
	}

	if ctx.AConfig().Getenv("TW_ROUND_SCREEN") == "true" {
		cflags = append(cflags, "-DTW_ROUND_SCREEN")
	}

	cflags = append(cflags, "-DTWRES=\""+ctx.AConfig().Getenv("TWRES_PATH")+"\"")

	return cflags
}

func globalSrcs(ctx android.BaseContext) []string {
	var srcs []string

	if ctx.AConfig().Getenv("TWRP_CUSTOM_KEYBOARD") != "" {
		srcs = append(srcs, ctx.AConfig().Getenv("TWRP_CUSTOM_KEYBOARD"))
	} else {
		srcs = append(srcs, "hardwarekeyboard.cpp")
	}
	return srcs
}

func globalIncludes(ctx android.BaseContext) []string {
	var includes []string

	if ctx.AConfig().Getenv("TW_INCLUDE_CRYPTO") != "" {
		includes = append(includes, "bootable/recovery/crypto/fscrypt")
	}

	return includes
}

func libGuiDefaults(ctx android.LoadHookContext) {
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
	}

	p := &props{}
	p.Cflags = globalFlags(ctx)
	s := globalSrcs(ctx)
	p.Srcs = s
	i := globalIncludes(ctx)
	p.Include_dirs = i
	ctx.AppendProperties(p)
}

func init() {
	android.RegisterModuleType("libguitwrp_defaults", libGuiDefaultsFactory)
}

func libGuiDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, libGuiDefaults)

	return module
}
