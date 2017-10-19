
FILE(GLOB PLATFORM_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/pc/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/pc/*.S")

SET(PLATFORM_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/inc/platform/x86_64/pc/")

SET(PLATFORM_DEFINITIONS "-DMULTIBOOT2")
SET(PLATFORM_C_FLAGS "")
SET(PLATFORM_ASM_FLAGS "")

add_custom_target(image
COMMAND mkdir -p "ISO"
COMMAND mkdir -p "ISO/isodir"
COMMAND mkdir -p "ISO/isodir/boot"

COMMAND rm -rf "ISO/isodir/boot/*"
COMMAND rm -rf "ISO/isodir/boot/initrd"
COMMAND cp "${INITRD_CONTENTS}" "ISO/isodir/boot"
COMMAND tar -cvf "ISO/isodir/boot/initrd" -C "ISO/isodir/boot" .

COMMAND cp "kernel/kernel.bin" "ISO/isodir/boot/kernel.bin"
COMMAND mkdir -p "ISO/isodir/boot/grub"
COMMAND cp "${CMAKE_CURRENT_SOURCE_DIR}/platform/x86_64/pc/grub.cfg" "ISO/isodir/boot/grub/grub.cfg"
COMMAND grub2-mkrescue -o "ISO/os.iso" "ISO/isodir"
DEPENDS kernel.bin)

add_custom_target(disk.img
    COMMAND qemu-img create -f raw disk.img 32M)

add_custom_target(run 
    COMMAND qemu-system-x86_64 -m 1024M -machine q35, -cpu Haswell,+invtsc,+xsave,+aes -smp 2 -d int,cpu_reset,guest_errors -drive id=disk,file=disk.img,if=none -device ahci,id=ahci -device ide-drive,drive=disk,bus=ahci.0 -net nic,model=rtl8139, -net dump,file=../dump.pcap -net user -soundhw hda -device ich9-usb-uhci3 -device usb-mouse -device usb-kbd -vga vmware -cdrom "ISO/os.iso" -boot d
    DEPENDS image
    DEPENDS disk.img)