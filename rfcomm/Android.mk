LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	kword.c \
	main.c \
	parser.c \
	lexer.c

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" \
	-DCONFIGDIR=\"/etc/bluez\" \
	-DNEED_PPOLL

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../common \
	$(LOCAL_PATH)/../include

LOCAL_SHARED_LIBRARIES := \
	libbluetooth

LOCAL_STATIC_LIBRARIES := \
	libbluez-common-static

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE_TAGS := eng
LOCAL_MODULE:=rfcomm

include $(BUILD_EXECUTABLE)
