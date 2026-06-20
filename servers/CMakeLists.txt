cmake_minimum_required(VERSION 3.20)

# The Core* OS services are now Lisp under lisp/servers/, bound by lisp/init.clp:
# CorePower, CoreDisplay, CoreInput, CoreAudio, CoreStorage (+ cardfs), CoreNetwork
# and CoreNetDebug were built-but-unloaded dead weight once boot transitioned
# straight to lisp_scheduler_enter, so their C modules were deleted.
#
# CoreUsb stays in C: it is the transfer/enumeration backend for the C USB drivers
# (uhci/ehci/xhci + usb_* class drivers), which have not been ported to Lisp.
# Its public header (servers/inc/CoreUsb) plus the CoreDisplay/CoreInput/CoreStorage
# headers are kept because the surviving C drivers (intel_gfx, usb_hid, usb_storage)
# still include them.
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/CoreUsb)
