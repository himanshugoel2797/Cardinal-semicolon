cmake_minimum_required(VERSION 3.20)

ADD_LIBRARY(ubsan_handlers STATIC ubsan.c)

TARGET_INCLUDE_DIRECTORIES(ubsan_handlers SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")