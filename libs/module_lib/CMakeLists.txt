cmake_minimum_required(VERSION 3.20)

ADD_LIBRARY(cardinal_module STATIC module.c)
TARGET_INCLUDE_DIRECTORIES(cardinal_module PUBLIC "${LIBS_DIR}/crypto")
TARGET_INCLUDE_DIRECTORIES(cardinal_module SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")