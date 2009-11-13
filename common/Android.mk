LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	btio.c \
	glib-helper.c \
	logging.c \
	oui.c \
	sdp-xml.c \
	textfile.c \
	test_textfile.c \
	android_bluez.c

LOCAL_CFLAGS+= \
	-O3 \
	-DNEED_DBUS_WATCH_GET_UNIX_FD

ifeq ($(BOARD_HAVE_BLUETOOTH_BCM),true)
LOCAL_CFLAGS += \
	-DBOARD_HAVE_BLUETOOTH_BCM
endif

LOCAL_C_INCLUDES:= \
	$(LOCAL_PATH)/../include \
	$(call include-path-for, glib) \
	$(call include-path-for, glib)/glib \
	$(call include-path-for, dbus)

LOCAL_MODULE:=libbluez-common-static

LOCAL_STATIC_LIBRARY:= \
	libglib_static

include $(BUILD_STATIC_LIBRARY)
