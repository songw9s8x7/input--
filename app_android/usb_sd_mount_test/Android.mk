
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_SHARED_LIBRARIES += libcutils libutils 
LOCAL_LDLIBS += -llog
LOCAL_SRC_FILES:= \/par    lidbg_servicer.c
LOCAL_MODULE_TAGS:= optional
LOCAL_MODULE := lidbg_servicer
include $(BUILD_EXECUTABLE)