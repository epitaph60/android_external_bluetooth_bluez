LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	mainloop.c object.c watch.c

LOCAL_CFLAGS+=-O3 -DNEED_DBUS_WATCH_GET_UNIX_FD

LOCAL_C_INCLUDES:= \
	$(call include-path-for, glib) \
	$(call include-path-for, dbus)

LOCAL_MODULE:=libgdbus_static

include $(BUILD_STATIC_LIBRARY)
