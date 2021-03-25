package twrp

import (
	"android/soong/android"
	// "github.com/google/blueprint"
	// "github.com/google/blueprint/proptools"
	"fmt"
	// "os"
	// "path"
	// "strconv"
	"strings"
)

var pctx = android.NewPackageContext("android/soong/twrp")

type prebuiltProperties struct {
	Src *string
	System_library bool
	Vendor_library bool
	System_binary bool
	Vendor_binary bool
	// Filename *string `android:"arch_variant"`
	// Sub_dir *string `android:"arch_variant"`
}


type PrebuiltTwrpModule interface {
	android.Module
	SubDir() string
	OutputFile() string
}

type PrebuiltTwrp struct {
	android.ModuleBase

	properties prebuiltProperties
	sourcePrebuiltFile 	string
	// installDir      	android.InstallPath
	postInstallCommands []string
	outputFilePath 		android.OutputPath
	installDirPath      android.InstallPath
	installFilename 	string
	system_library 		bool
	vendor_library 		bool
	system_binary 		bool
	vendor_binary 		bool
}

func init() {
	pctx.Import("android/soong/android")
	android.RegisterModuleType("prebuilt_twrp", PrebuiltFactory)
}

func InitPrebuiltTwrpModule(p *PrebuiltTwrp) {
	p.AddProperties(&p.properties)
}

func  (p *PrebuiltTwrp) copyPrebuilt(ctx android.ModuleContext, filename string) {
	fmt.Printf("copying file: %s to %s\n", filename)
	p.postInstallCommands = append(p.postInstallCommands, filename)
}

func (p *PrebuiltTwrp) getLocationFromType(ctx android.ModuleContext) string {
	fileLocation := ""
	library_dir := ""
	fmt.Printf("called getLocationFromType\n");
	arch := ctx.Config().Targets[android.Android][0].Arch.ArchType
	if (arch == android.Arm64) {
		library_dir = "lib64"
	} else if (arch == android.Arm) {
		library_dir = "lib"
	}

	if (p.system_library) {
		fileLocation = fmt.Sprintf("system/%s/", library_dir)
	}

	if (p.vendor_library) {
		fileLocation = fmt.Sprintf("vendor/%s/", library_dir)
	}
	if (p.vendor_binary) {
		fileLocation = "vendor/bin/"
	}

	if (p.system_binary) {
		fileLocation = "system/bin/"
	}

	return fileLocation
}

func (p *PrebuiltTwrp) AndroidMkEntries() []android.AndroidMkEntries {
	return []android.AndroidMkEntries{android.AndroidMkEntries{
		Class:         "DATA",
		OutputFile: android.OptionalPathForPath(p.outputFilePath),
		Include:       "$(BUILD_PREBUILT)",
		ExtraEntries: []android.AndroidMkExtraEntriesFunc{
				func(entries *android.AndroidMkEntries) {
					entries.SetString("LOCAL_MODULE_TAGS", "optional")
					entries.SetString("LOCAL_MODULE_PATH", p.installDirPath.ToMakePath().String())
					entries.SetPath("LOCAL_PREBUILT_MODULE_FILE", p.outputFilePath)
					entries.SetString("LOCAL_MODULE_STEM", p.installFilename)
					postInstallCommands := append([]string{}, p.postInstallCommands...)
						if len(postInstallCommands) > 0 {
								entries.SetString("LOCAL_POST_INSTALL_CMD", strings.Join(postInstallCommands, " && "))
						}
				},
		},
}}
}

func (p *PrebuiltTwrp) GenerateAndroidBuildActions(ctx android.ModuleContext) {
	p.sourcePrebuiltFile = android.String(p.properties.Src)
	if (p.properties.System_library) {
		p.system_library = true;
	}
	if (p.properties.System_binary) {
		p.system_binary = true;
	}
	if (p.properties.Vendor_binary) {
		p.vendor_binary = true;
	}
	// p.installDir = android.PathForModuleInstall(ctx, "test")
	// p.installDirPath = android.PathForModuleInstall(ctx, "test")
	p.installDirPath = android.PathForModuleInstall(ctx, ctx.ModuleName())
	fmt.Printf("test: %s\n", p.installDirPath)
	p.outputFilePath = android.PathForModuleOut(ctx, ctx.ModuleName()).OutputPath
	p.installFilename = "test"
	// location := getLocationFromType(ctx)
	// outDir := ctx.Config().Getenv("OUT")
	// dst := fmt.Sprintf("%s/%s/", outDir, location)
	// fileToCopy := fmt.Sprintf("%s/%s", outDir, p.sourcePrebuiltFile)
	// copyPrebuilt(ctx, fileToCopy, dst)
}

func PrebuiltFactory() android.Module {
	module := &PrebuiltTwrp{}
	InitPrebuiltTwrpModule(module)
	android.InitAndroidArchModule(module, android.HostSupported, android.MultilibCommon)
	return module
}
