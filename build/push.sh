
source ./env_entry.sh
adb  wait-for-device remount
adb push $DBG_OUT_PATH /system/lib/modules/out
adb push $DBG_ANDROID_OUT_PATH/lidbg_servicer /system/bin/lidbg_servicer
adb shell chmod 0644 /system/lib/modules/out/*.ko
adb shell chmod 777 /system/bin/lidbg_servicer
adb shell rm -r /flysystem/lib/out