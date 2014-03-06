
#DBG_SOC=msm8x25
DBG_SOC=msm8226
#DBG_PLATFORM=msm7627a
#DBG_PLATFORM=msm8625
DBG_PLATFORM=msm8226
BUILD_VERSION=rel


case "$DBG_PLATFORM" in
    msm7627a)
	BOARD_VERSION=v2
	DBG_SYSTEM_DIR=/home/leo/8x25_rel;;
    msm8625)
	BOARD_VERSION=v3
	DBG_SYSTEM_DIR=/home/leo/R8625QSOSKQLYA3060
	DBG_CROSS_COMPILE=$DBG_SYSTEM_DIR/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-;;
	 msm8226)
	BOARD_VERSION=v1
	DBG_SYSTEM_DIR=/home/leo/msm8226-1.8
	DBG_CROSS_COMPILE=$DBG_SYSTEM_DIR/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi-;;
    *) 
	echo "exit,not find your platform:  $DBG_PLATFORM"
	exit;;
esac

DBG_KERNEL_SRC_DIR=$DBG_SYSTEM_DIR/kernel
DBG_KERNEL_OBJ_DIR=$DBG_SYSTEM_DIR/out/target/product/$DBG_PLATFORM/obj/KERNEL_OBJ


echo $DBG_SYSTEM_DIR
echo $BOARD_VERSION

export DBG_SOC
export DBG_PLATFORM
export DBG_KERNEL_SRC_DIR
export DBG_KERNEL_OBJ_DIR
export DBG_SYSTEM_DIR
export DBG_CROSS_COMPILE
export BOARD_VERSION
export BUILD_VERSION