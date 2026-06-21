cmake_minimum_required(VERSION 3.20)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CorePower)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreDisplay)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreInput)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreAudio)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreStorage)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreNetwork)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreNetDebug)

# CoreUsb was ported to Lisp (lisp/servers/coreusb); CoreDriver (the C PCI->driver
# binder reading devices.txt) is gone -- lisp/init.clp is the device binder now.
