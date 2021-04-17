LOCAL_PATH := $(call my-dir)
# Transfer in the resources for the device
include $(CLEAR_VARS)
LOCAL_MODULE := twrp
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := DATA
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)$(TWRES_PATH)

# The extra blank line before *** is intentional to ensure it ends up on its own line
define TW_THEME_WARNING_MSG

****************************************************************************
  Could not find ui.xml for TW_THEME: $(TW_THEME)
  Set TARGET_SCREEN_WIDTH and TARGET_SCREEN_HEIGHT to automatically select
  an appropriate theme, or set TW_THEME to one of the following:
    $(notdir $(wildcard $(LOCAL_PATH)/theme/*_*))
****************************************************************************
endef
define TW_CUSTOM_THEME_WARNING_MSG

****************************************************************************
  Could not find ui.xml for TW_CUSTOM_THEME: $(TW_CUSTOM_THEME)
  Expected to find custom theme's ui.xml at:
    $(TWRP_THEME_LOC)/ui.xml
  Please fix this or set TW_THEME to one of the following:
    $(notdir $(wildcard $(LOCAL_PATH)/theme/*_*))
****************************************************************************
endef

TWRP_RES := $(LOCAL_PATH)/theme/common/fonts
TWRP_RES += $(LOCAL_PATH)/theme/common/languages
ifeq ($(TW_EXTRA_LANGUAGES),true)
    TWRP_RES += $(LOCAL_PATH)/theme/extra-languages/fonts
    TWRP_RES += $(LOCAL_PATH)/theme/extra-languages/languages
endif

ifeq ($(TW_CUSTOM_THEME),)
    ifeq ($(TW_THEME),)
        ifeq ($(DEVICE_RESOLUTION),)
            GUI_WIDTH := $(TARGET_SCREEN_WIDTH)
            GUI_HEIGHT := $(TARGET_SCREEN_HEIGHT)
        else
            SPLIT_DEVICE_RESOLUTION := $(subst x, ,$(DEVICE_RESOLUTION))
            GUI_WIDTH := $(word 1, $(SPLIT_DEVICE_RESOLUTION))
            GUI_HEIGHT := $(word 2, $(SPLIT_DEVICE_RESOLUTION))
        endif

        # Minimum resolution of 100x100
        # This also ensures GUI_WIDTH and GUI_HEIGHT are numbers
        ifeq ($(shell test $(GUI_WIDTH) -ge 100; echo $$?),0)
        ifeq ($(shell test $(GUI_HEIGHT) -ge 100; echo $$?),0)
            ifeq ($(shell test $(GUI_WIDTH) -gt $(GUI_HEIGHT); echo $$?),0)
                ifeq ($(shell test $(GUI_WIDTH) -ge 1280; echo $$?),0)
                    TW_THEME := landscape_hdpi
                else
                    TW_THEME := landscape_mdpi
                endif
            else ifeq ($(shell test $(GUI_WIDTH) -lt $(GUI_HEIGHT); echo $$?),0)
                ifeq ($(shell test $(GUI_WIDTH) -ge 720; echo $$?),0)
                    TW_THEME := portrait_hdpi
                else
                    TW_THEME := portrait_mdpi
                endif
            else ifeq ($(shell test $(GUI_WIDTH) -eq $(GUI_HEIGHT); echo $$?),0)
                # watch_hdpi does not yet exist
                TW_THEME := watch_mdpi
            endif
        endif
        endif
    endif

    TWRP_THEME_LOC := $(LOCAL_PATH)/theme/$(TW_THEME)
    ifeq ($(wildcard $(TWRP_THEME_LOC)/ui.xml),)
        $(warning $(TW_THEME_WARNING_MSG))
        $(error Theme selection failed; exiting)
    endif

    TWRP_RES += $(LOCAL_PATH)/theme/common/$(word 1,$(subst _, ,$(TW_THEME))).xml
    # for future copying of used include xmls and fonts:
    # UI_XML := $(TWRP_THEME_LOC)/ui.xml
    # TWRP_INCLUDE_XMLS := $(shell xmllint --xpath '/recovery/include/xmlfile/@name' $(UI_XML)|sed -n 's/[^\"]*\"\([^\"]*\)\"[^\"]*/\1\n/gp'|sort|uniq)
    # TWRP_FONTS_TTF := $(shell xmllint --xpath '/recovery/resources/font/@filename' $(UI_XML)|sed -n 's/[^\"]*\"\([^\"]*\)\"[^\"]*/\1\n/gp'|sort|uniq)niq)
else
    TWRP_THEME_LOC := $(TW_CUSTOM_THEME)
    ifeq ($(wildcard $(TWRP_THEME_LOC)/ui.xml),)
        $(warning $(TW_CUSTOM_THEME_WARNING_MSG))
        $(error Theme selection failed; exiting)
    endif
endif

TWRP_RES += $(TW_ADDITIONAL_RES)

TWRP_RES_GEN := $(intermediates)/twrp
$(TWRP_RES_GEN):
	mkdir -p $(TARGET_RECOVERY_ROOT_OUT)$(TWRES_PATH)
	cp -fr $(TWRP_RES) $(TARGET_RECOVERY_ROOT_OUT)$(TWRES_PATH)
	cp -fr $(TWRP_THEME_LOC)/* $(TARGET_RECOVERY_ROOT_OUT)$(TWRES_PATH)

LOCAL_GENERATED_SOURCES := $(TWRP_RES_GEN)
#LOCAL_SRC_FILES := twrp
LOCAL_SRC_FILES := $(TWRP_RES_GEN)
$(warning LOCAL_SRC_FILES: $(LOCAL_SRC_FILES))
include $(BUILD_PREBUILT)
