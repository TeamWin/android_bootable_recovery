package twrp

import (
	"android/soong/android"
	"android/soong/cc"
	"fmt"
	"io/ioutil"
	"os"
	"path"
	"strconv"
	"strings"
)

func printThemeWarning(theme string) {
	if theme == "" {
		theme = "not set"
	}
	themeWarning := "***************************************************************************\n"
	themeWarning += "Could not find ui.xml for TW_THEME: "
	themeWarning += theme
	themeWarning += "\nSet TARGET_SCREEN_WIDTH and TARGET_SCREEN_HEIGHT to automatically select\n"
	themeWarning += "an appropriate theme, or set TW_THEME to one of the following:\n"
	themeWarning += "landscape_hdpi landscape_mdpi portrait_hdpi portrait_mdpi watch_mdpi\n"
	themeWarning += "****************************************************************************\n"
	themeWarning += "(theme selection failed; exiting)\n"

	fmt.Printf(themeWarning)
}

func printCustomThemeWarning(theme string, location string) {
	customThemeWarning := "****************************************************************************\n"
	customThemeWarning += "Could not find ui.xml for TW_CUSTOM_THEME: "
	customThemeWarning += theme + "\n"
	customThemeWarning += "Expected to find custom theme's ui.xml at: "
	customThemeWarning += location
	customThemeWarning += "Please fix this or set TW_THEME to one of the following:\n"
	customThemeWarning += "landscape_hdpi landscape_mdpi portrait_hdpi portrait_mdpi watch_mdpi\n"
	customThemeWarning += "****************************************************************************\n"
	customThemeWarning += "(theme selection failed; exiting)\n"
	fmt.Printf(customThemeWarning)
}

func copyThemeResources(ctx android.BaseContext, dirs []string, files []string) {
	outDir := ctx.Config().Getenv("OUT")
	twRes := outDir + "/recovery/root/twres/"
	os.MkdirAll(twRes, os.ModePerm)
	recoveryDir := getRecoveryAbsDir(ctx)
	theme := determineTheme(ctx)
	for idx, dir := range dirs {
		_ = idx
		dirToCopy := ""
		destDir := twRes + path.Base(dir)
		baseDir := path.Base(dir)
		if baseDir == theme {
			destDir = twRes
			dirToCopy = recoveryDir + dir
		} else {
			dirToCopy = recoveryDir + dir
		}
		copyDir(dirToCopy, destDir)
	}
	for idx, file := range files {
		_ = idx
		fileToCopy := recoveryDir + file
		fileDest := twRes + path.Base(file)
		copyFile(fileToCopy, fileDest)
	}
	data, err := ioutil.ReadFile(recoveryDir + "variables.h")
	if err != nil {
		fmt.Println(err)
		return
	}
	version := "0"
	for _, line := range strings.Split(string(data), "\n") {
		if strings.Contains(line, "TW_THEME_VERSION") {
			version = strings.Split(line, " ")[2]
		}
	}

	_props := [3]string{"TW_CUSTOM_BATTERY_POS", "TW_CUSTOM_CPU_POS", "TW_CUSTOM_CLOCK_POS" }
	props := [3]string{"0", "0", "0"}
	for i, item := range _props {
		if getMakeVars(ctx, item) != "" {
			props[i] = strings.Trim(getMakeVars(ctx, item), "\"")
		}
	}

	_files := [2]string{"splash.xml", "ui.xml"}
	for _, i := range _files {
		var fontsize int = 0
		var width int = 0

		data, err = ioutil.ReadFile(twRes + i)
		if err != nil {
			fmt.Println(err)
			return
		}

		newFile := strings.Replace(string(data), "{themeversion}", version, -1)

		// Custom position for status bar items - start
		if i == "ui.xml" {

			for _, line := range strings.Split(string(data), "\n") {
		                if strings.Contains(line, "name=\"font_m\"") {
		                      fontsize, err = strconv.Atoi(strings.Split(line, "\"")[5])
		                       if err != nil {
		                               fmt.Println(err)
		                               return
		                       }
				}
				if strings.Contains(line, "resolution") {
					width, err = strconv.Atoi(strings.Split(line, "\"")[1])
					if err != nil {
						fmt.Println(err)
						return
					}
				}
		        }

			var cpusize int = (fontsize * 5) + (width/100)
			var clocksize int = (fontsize * 4) + (width/100)
			var batterysize int = (fontsize * 6) - (width/100)
			var pos_clock_24 string = props[2]
			for j := 0; j < len(props); j++ {
				if props[j] == "left" {
					props[j] = strconv.Itoa(width/50)
					if _props[j] == "TW_CUSTOM_CLOCK_POS" {
						pos_clock_24 = props[j]
					}
				} else if props[j] == "center" {
					if _props[j] == "TW_CUSTOM_BATTERY_POS" {
						props[j] = strconv.Itoa( (width/2) - (batterysize*43/100) )
					} else if _props[j] == "TW_CUSTOM_CLOCK_POS" {
						pos := (width/2) - (clocksize*45/100)
						props[j] = strconv.Itoa(pos)
						pos_clock_24 = strconv.Itoa( pos * 31/30 )
					} else if _props[j] == "TW_CUSTOM_CPU_POS" {
						props[j] = strconv.Itoa( (width/2) - (cpusize*41/100) )
					}
				} else if props[j] == "right" {
					if _props[j] == "TW_CUSTOM_BATTERY_POS" {
						props[j] = strconv.Itoa( width - batterysize )
					} else if _props[j] == "TW_CUSTOM_CLOCK_POS" {
						props[j] = strconv.Itoa(width - clocksize)
						pos_clock_24 = props[j]
					} else if _props[j] == "TW_CUSTOM_CPU_POS" {
						props[j] = strconv.Itoa( width - cpusize )
					}
				}
			}

			alignProp := "%status_topalign_header_y%"
			if strings.Trim(getMakeVars(ctx, "TW_STATUS_ICONS_ALIGN"), "\"") == "center" || strings.Trim(getMakeVars(ctx, "TW_STATUS_ICONS_ALIGN"), "\"") == "2" {
				alignProp = "%status_centeralign_header_y%"
			} else if strings.Trim(getMakeVars(ctx, "TW_STATUS_ICONS_ALIGN"), "\"") == "bottom" || strings.Trim(getMakeVars(ctx, "TW_STATUS_ICONS_ALIGN"), "\"") == "3" {
				alignProp = "%status_bottomalign_header_y%"
			}
			newFile = strings.Replace(newFile, "{battery_pos}", props[0], -1)
			newFile = strings.Replace(newFile, "{cpu_pos}", props[1], -1)
			newFile = strings.Replace(newFile, "{clock_12_pos}", props[2], -1)
			newFile = strings.Replace(newFile, "{clock_24_pos}", pos_clock_24, -1)
			newFile = strings.Replace(newFile, "{statusicons_align}", alignProp, -1)
		}
		// Custom position for status bar items - end

		err = ioutil.WriteFile(twRes + i, []byte(newFile), 0)
		if err != nil {
			fmt.Println(err)
			return
		}
	}
}

func copyCustomTheme(ctx android.BaseContext, customTheme string) {
	outDir := ctx.Config().Getenv("OUT")
	twRes := outDir + "/recovery/root/twres/"
	os.MkdirAll(twRes, os.ModePerm)
	fileDest := twRes + path.Base(customTheme)
	fileToCopy := fmt.Sprintf("%s%s", getBuildAbsDir(ctx), customTheme)
	copyFile(fileToCopy, fileDest)
}

func determineTheme(ctx android.BaseContext) string {
	guiWidth := 0
	guiHeight := 0
	if getMakeVars(ctx, "TW_CUSTOM_THEME") == "" {
		if getMakeVars(ctx, "TW_THEME") == "" {
			if getMakeVars(ctx, "DEVICE_RESOLUTION") == "" {
				width, err := strconv.Atoi(getMakeVars(ctx, "TARGET_SCREEN_WIDTH"))
				if err == nil {
					guiWidth = width
				}
				height, err := strconv.Atoi(getMakeVars(ctx, "TARGET_SCREEN_HEIGHT"))
				if err == nil {
					guiHeight = height
				}
			} else {
				deviceRes := getMakeVars(ctx, "DEVICE_RESOLUTION")
				width, err := strconv.Atoi(strings.Split(deviceRes, "x")[0])
				if err == nil {
					guiWidth = width
				}
				height, err := strconv.Atoi(strings.Split(deviceRes, "x")[1])
				if err == nil {
					guiHeight = height
				}
			}
		}
		if guiWidth > 100 {
			if guiHeight > 100 {
				if guiWidth > guiHeight {
					if guiWidth > 1280 {
						return "landscape_hdpi"
					} else {
						return "landscape_mdpi"
					}
				} else if guiWidth < guiHeight {
					if guiWidth > 720 {
						return "portrait_hdpi"
					} else {
						return "portrait_mdpi"
					}
				} else if guiWidth == guiHeight {
					return "watch_mdpi"
				}
			}
		}
	}

	return getMakeVars(ctx, "TW_THEME")
}

func copyTheme(ctx android.BaseContext) bool {
	var directories []string
	var files []string
	var customThemeLoc string
	localPath := ctx.ModuleDir()
	directories = append(directories, "gui/theme/common/fonts/")
	directories = append(directories, "gui/theme/common/languages/")
	if getMakeVars(ctx, "TW_EXTRA_LANGUAGES") == "true" {
		directories = append(directories, "gui/theme/extra-languages/fonts/")
		directories = append(directories, "gui/theme/extra-languages/languages/")
	}
	var theme = determineTheme(ctx)
	directories = append(directories, "gui/theme/"+theme)
	themeXML := fmt.Sprintf("gui/theme/common/%s.xml", strings.Split(theme, "_")[0])
	files = append(files, themeXML)
	if getMakeVars(ctx, "TW_CUSTOM_THEME") == "" {
		defaultTheme := fmt.Sprintf("%s/theme/%s/ui.xml", localPath, theme)
		if android.ExistentPathForSource(ctx, defaultTheme).Valid() {
			fullDefaultThemePath := fmt.Sprintf("gui/theme/%s/ui.xml", theme)
			files = append(files, fullDefaultThemePath)
		} else {
			printThemeWarning(theme)
			return false
		}
	} else {
		customThemeLoc = getMakeVars(ctx, "TW_CUSTOM_THEME")
		if android.ExistentPathForSource(ctx, customThemeLoc).Valid() {
		} else {
			printCustomThemeWarning(customThemeLoc, getMakeVars(ctx, "TW_CUSTOM_THEME"))
			return false
		}
	}
	copyThemeResources(ctx, directories, files)
	if customThemeLoc != "" {
		copyCustomTheme(ctx, customThemeLoc)
	}
	return true
}

func globalFlags(ctx android.BaseContext) []string {
	var cflags []string


	if getMakeVars(ctx, "AB_OTA_UPDATER") == "true" {
		cflags = append(cflags, "-DAB_OTA_UPDATER=1")
	}
	return cflags
}

func globalSrcs(ctx android.BaseContext) []string {
	var srcs []string

	if getMakeVars(ctx, "TWRP_CUSTOM_KEYBOARD") != "" {
		srcs = append(srcs, getMakeVars(ctx, "TWRP_CUSTOM_KEYBOARD"))
	} else {
		srcs = append(srcs, "hardwarekeyboard.cpp")
	}
	return srcs
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
	ctx.AppendProperties(p)
	if copyTheme(ctx) == false {
		os.Exit(-1)
	}
}

func init() {
	android.RegisterModuleType("libguitwrp_defaults", libGuiDefaultsFactory)
}

func libGuiDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, libGuiDefaults)

	return module
}
