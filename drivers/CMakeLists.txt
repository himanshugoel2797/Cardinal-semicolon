# Device drivers. Almost the entire driver tree is Lisp now (lisp/drivers/*.clp,
# bound by lisp/init.clp): ps2, rtl8139, virtio (gpu/net), lfb, ahci, hdaudio and
# the whole USB stack (uhci/xhci host controllers + usb-hid/hub/storage class
# drivers over lisp/servers/coreusb) are Lisp. The orphaned C drivers (rtl8169,
# intel_gfx, intel_wifi, ehci, and the C USB tree) were removed once the C driver
# loader (CoreDriver/devices.txt) was gone. Only tarfs remains in C.
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/tarfs)
