LOCAL_PATH:= $(call my-dir)

#
# libplugin
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	hciops.c \

LOCAL_CFLAGS:= \
	-DVERSION=\"4.47\" \
	-DBLUETOOTH_PLUGIN_BUILTIN \

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../include \
        $(LOCAL_PATH)/../common \
        $(LOCAL_PATH)/../gdbus \
        $(LOCAL_PATH)/../src \
        $(call include-path-for, glib) \
        $(call include-path-for, dbus) \

LOCAL_SHARED_LIBRARIES := \
	libbluetoothd \
	libbluetooth \
	libcutils \
	libdbus

LOCAL_STATIC_LIBRARIES := \
	libbluez-common-static \
	libglib_static

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/bluez-plugin
LOCAL_UNSTRIPPED_PATH := $(TARGET_OUT_SHARED_LIBRARIES_UNSTRIPPED)/bluez-plugin
LOCAL_MODULE:=libbuiltinplugin

include $(BUILD_STATIC_LIBRARY)
