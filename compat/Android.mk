BUILD_PAND:=1
BUILD_HIDD:=1

ifeq ($(BUILD_PAND),1)

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \
	$(LOCAL_PATH)/../gdbus \
	$(LOCAL_PATH)/../src \
	$(call include-path-for, glib) \
	$(call include-path-for, dbus)

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" \
	-DSTORAGEDIR=\"/data/misc/bluetoothd\" \
	-DCONFIGDIR=\"/etc/bluez\" \
	-DANDROID \
	-DNEED_PPOLL \
	-D__S_IFREG=0100000  # missing from bionic stat.h

LOCAL_SRC_FILES:= \
	pand.c    \
	bnep.c    \
	sdp.c

LOCAL_SHARED_LIBRARIES := \
	libbluetooth \
	libbluetoothd \
	libdbus

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE:=pand

include $(BUILD_EXECUTABLE)
endif


ifeq ($(BUILD_HIDD),1)

include $(CLEAR_VARS)

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/../common \
	$(LOCAL_PATH)/../gdbus \
	$(LOCAL_PATH)/../src \
	$(call include-path-for, glib) \
	$(call include-path-for, dbus)

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" \
	-DSTORAGEDIR=\"/data/misc/bluetoothd\" \
	-DCONFIGDIR=\"/etc/bluez\" \
	-DANDROID \
	-DNEED_PPOLL \
	-D__S_IFREG=0100000  # missing from bionic stat.h

LOCAL_SRC_FILES:= \
	hidd.c    \
	sdp.c    \
	fakehid.c

LOCAL_SHARED_LIBRARIES := \
	libbluetooth \
	libbluetoothd \
	libdbus

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
LOCAL_MODULE:=hidd

include $(BUILD_EXECUTABLE)
endif
