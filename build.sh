#!/bin/bash

export ARCH=arm64 && export SUBARCH=arm64 && export CROSS_COMPILE=../../aarch64-linux-android-4.9/bin/aarch64-linux-androidkernel-

make ASUS_BUILD_PROJECT=ZE620KL TARGET_BUILD_VARIANT=user INSTALL_MOD_PATH=mods O=out ze620kl-user-sdm660-perf_defconfig && \
make ASUS_BUILD_PROJECT=ZE620KL TARGET_BUILD_VARIANT=user INSTALL_MOD_PATH=mods O=out -j$(nproc) all && \
make ASUS_BUILD_PROJECT=ZE620KL TARGET_BUILD_VARIANT=user INSTALL_MOD_PATH=mods O=out modules_install
