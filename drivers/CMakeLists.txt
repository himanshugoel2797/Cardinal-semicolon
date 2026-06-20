cmake_minimum_required(VERSION 3.20)

# ps2 was retired: the i8042 keyboard driver is now Lisp (lisp/ps2.clp), driven by
# SysLisp over the generic ISA-IRQ + port-I/O primitives. No native module remains.
#
# rtl8139, rtl8169, virtio (gpu/net), lfb, hdaudio and ahci were likewise retired:
# their drivers are now Lisp (lisp/drivers/*.clp), bound by lisp/init.clp. The native
# modules were built-but-unloaded dead weight once the C driver loader was removed.

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/intel_wifi)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/intel_gfx)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/uhci)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/ehci)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/xhci)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/usb_hid)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/usb_storage)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/usb_hub)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/usb_serial)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/tarfs)