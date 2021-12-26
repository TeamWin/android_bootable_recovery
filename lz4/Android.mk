LOCAL_PATH := external/lz4/programs

include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
    datagen.c \
    bench.c \
    lz4io.c \
    lz4cli.c
LOCAL_MODULE := lz4_twrp
LOCAL_MODULE_STEM := lz4
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/system/bin
LOCAL_MODULE_TAGS := optional
LOCAL_STATIC_LIBRARIES := \
    liblz4 \
    libc++_static
include $(BUILD_EXECUTABLE)
