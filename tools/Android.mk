LOCAL_PATH:= $(call my-dir)

#
# avinfo
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	avinfo.c

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\"

LOCAL_C_INCLUDES:=\
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE_TAGS := eng
LOCAL_MODULE:=avinfo

include $(BUILD_EXECUTABLE)

#
# sdptool
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	sdptool.c

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" -fpermissive

LOCAL_C_INCLUDES:=\
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_STATIC_LIBRARIES := \
	libbluez-common-static

LOCAL_MODULE:=sdptool

include $(BUILD_EXECUTABLE)

#
# hciconfig
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	csr.c \
	csr_h4.c \
	hciconfig.c

LOCAL_CFLAGS:= \
	-DSTORAGEDIR=\"/tmp\" \
	-DVERSION=\"4.47\"

LOCAL_C_INCLUDES:=\
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_STATIC_LIBRARIES := \
	libbluez-common-static

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE_TAGS := eng
LOCAL_MODULE:=hciconfig

include $(BUILD_EXECUTABLE)

#
# hcitool
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	hcitool.c

LOCAL_CFLAGS:= \
	-DSTORAGEDIR=\"/tmp\" \
	-DVERSION=\"4.47\"

LOCAL_C_INCLUDES:=\
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_STATIC_LIBRARIES := \
	libbluez-common-static

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE_TAGS := eng
LOCAL_MODULE:=hcitool

include $(BUILD_EXECUTABLE)

#
# l2ping
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	l2ping.c

LOCAL_C_INCLUDES:=\
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE_TAGS := eng
LOCAL_MODULE:=l2ping

include $(BUILD_EXECUTABLE)

#
# hciattach
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	hciattach.c \
	hciattach_st.c \
	hciattach_ti.c \
	hciattach_tialt.c

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" \
	-D__BSD_VISIBLE=1

LOCAL_C_INCLUDES:=\
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_STATIC_LIBRARIES := \
	libbluez-common-static

LOCAL_MODULE:=hciattach

include $(BUILD_EXECUTABLE)
