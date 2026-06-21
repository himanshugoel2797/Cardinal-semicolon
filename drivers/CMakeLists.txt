cmake_minimum_required(VERSION 3.20)

# ps2 was retired: the i8042 keyboard driver is now Lisp (lisp/ps2.clp), driven by
# SysLisp over the generic ISA-IRQ + port-I/O primitives. No native module remains.
#
# The USB stack (uhci/xhci/ehci + the usb_* class drivers) and the rtl8169 /
# intel_wifi / intel_gfx / hdaudio drivers were removed: USB is now Lisp
# (lisp/drivers/{uhci,xhci}.clp + lisp/drivers/usb-* + lisp/servers/coreusb), and
# the others were orphaned C (no longer bound by any boot path -- CoreDriver and
# devices.txt are gone). Driver binding is policy in lisp/init.clp now.

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/rtl8139)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/virtio)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/lfb)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/ahci)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/tarfs)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/cardfs)
