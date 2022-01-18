package twrp

import (
	"android/soong/android"
	"android/soong/cc"
)

func globalFlags(ctx android.BaseContext) []string {
	var cflags []string

	if getMakeVars(ctx, "AB_OTA_UPDATER") == "true" {
		cflags = append(cflags, "-DAB_OTA_UPDATER=1")
	}

	if getMakeVars(ctx, "TW_USE_FSCRYPT_POLICY") == "1" {
		cflags = append(cflags, "-DUSE_FSCRYPT_POLICY_V1")
	} else {
		cflags = append(cflags, "-DUSE_FSCRYPT_POLICY_V2")
	}
	return cflags
}

func globalSrcs(ctx android.BaseContext) []string {
	var srcs []string

	if getMakeVars(ctx, "TWRP_CUSTOM_KEYBOARD") != "" {
		srcs = append(srcs, getMakeVars(ctx, "TWRP_CUSTOM_KEYBOARD"))
	}

	return srcs
}

func libAospRecoveryDefaults(ctx android.LoadHookContext) {
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
	ctx.AppendProperties(p)
}

func init() {
	android.RegisterModuleType("libaosprecovery_defaults", libAospRecoveryDefaultsFactory)
}

func libAospRecoveryDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, libAospRecoveryDefaults)

	return module
}
