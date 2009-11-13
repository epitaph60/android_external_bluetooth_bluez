LOCAL_PATH:= $(call my-dir)

# A2DP plugin

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	a2dp.c \
	avdtp.c \
	control.c \
	device.c \
	gateway.c \
	headset.c \
	ipc.c \
	main.c \
	manager.c \
	module-bluetooth-sink.c \
	sink.c \
	source.c \
	telephony-dummy.c \
	unix.c

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" \
	-DSTORAGEDIR=\"/data/misc/bluetoothd\" \
	-DCONFIGDIR=\"/etc/bluez\" \
	-DANDROID \
	-D__S_IFREG=0100000  # missing from bionic stat.h

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \
	$(LOCAL_PATH)/../gdbus \
	$(LOCAL_PATH)/../src \
	$(call include-path-for, glib) \
	$(call include-path-for, dbus)

LOCAL_SHARED_LIBRARIES := \
	libbluetooth \
	libbluetoothd \
	libdbus


LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/bluez-plugin
LOCAL_UNSTRIPPED_PATH := $(TARGET_OUT_SHARED_LIBRARIES_UNSTRIPPED)/bluez-plugin
LOCAL_MODULE := audio

include $(BUILD_SHARED_LIBRARY)

#
# liba2dp
# This is linked to Audioflinger so **LGPL only**

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	liba2dp.c \
	ipc.c \
	../sbc/sbc.c.arm \
	../sbc/sbc_primitives.c \
	../sbc/sbc_primitives_neon.c

# to improve SBC performance
LOCAL_CFLAGS:= -funroll-loops

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../sbc \

LOCAL_SHARED_LIBRARIES := \
	libcutils

LOCAL_MODULE := liba2dp

include $(BUILD_SHARED_LIBRARY)
