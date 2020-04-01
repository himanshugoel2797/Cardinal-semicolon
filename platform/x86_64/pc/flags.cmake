
FILE(GLOB PLATFORM_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/pc/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/platform/x86_64/pc/*.S")

SET(PLATFORM_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/inc/platform/x86_64/pc/")

SET(PLATFORM_DEFINITIONS "-DMULTIBOOT2")
SET(PLATFORM_C_FLAGS "")
SET(PLATFORM_ASM_FLAGS "")

SET(PLATFORM_CELF_DIR "${CMAKE_CURRENT_BINARY_DIR}/ISO/isodir/boot/")

add_custom_target(image
COMMAND mkdir -p "ISO/isodir/boot"
COMMAND rm -rf "ISO/isodir/boot/initrd"
COMMAND rm -rf "ISO/isodir/boot/kernel.bin"
COMMAND rm -rf "ISO/isodir/boot/grub"
COMMAND rm -rf "ISO/isodir/boot/loadscript.txt"
COMMAND rm -rf "ISO/isodir/boot/apscript.txt"
COMMAND rm -rf "ISO/isodir/boot/servicescript.txt"

COMMAND cp "${LOAD_SCRIPT}" "ISO/isodir/boot/loadscript.txt"
COMMAND cp "${DEVICE_FILE}" "ISO/isodir/boot/devices.txt"
COMMAND cp "${AP_SCRIPT}" "ISO/isodir/boot/apscript.txt"
COMMAND cp "${SERVICE_SCRIPT}" "ISO/isodir/boot/servicescript.txt"
COMMAND tar -cvf "ISO/isodir/boot/initrd" -C "ISO/isodir/boot" .

COMMAND rm -rf "ISO/isodir/boot/loadscript.txt"
COMMAND rm -rf "ISO/isodir/boot/apscript.txt"
COMMAND rm -rf "ISO/isodir/boot/servicescript.txt"
COMMAND rm -rf "ISO/isodir/boot/devices.txt"
COMMAND rm -rf "ISO/isodir/boot/*.celf"

COMMAND cp "kernel/kernel.bin" "ISO/isodir/boot/kernel.bin"
COMMAND mkdir -p "ISO/isodir/boot/grub"
COMMAND mkdir -p "ISO/isodir/EFI"
COMMAND mkdir -p "ISO/isodir/EFI/BOOT"
COMMAND cp "${CMAKE_CURRENT_SOURCE_DIR}/platform/x86_64/pc/grub.cfg" "ISO/isodir/boot/grub/grub.cfg"
COMMAND grub-mkstandalone -O x86_64-efi -o "ISO/isodir/EFI/BOOT/BOOTX64.EFI" "boot/grub/grub.cfg=${CMAKE_CURRENT_SOURCE_DIR}/platform/x86_64/pc/grub_efi.cfg" "boot/initrd=ISO/isodir/boot/initrd" "boot/kernel.bin=kernel/kernel.bin"
COMMAND grub-mkrescue -d /usr/lib/grub/i386-pc -o "ISO/os.iso" "ISO/isodir"
DEPENDS kernel.bin)

add_custom_target(disk.img
    COMMAND qemu-img create -f raw disk.img 128M)

add_custom_target(run 
    COMMAND sudo qemu-virgil --enable-kvm -m 2G -machine q35, -cpu host -smp 3 -d int,cpu_reset,guest_errors -D log.txt -drive id=disk,file=disk.img,if=none -device ahci,id=ahci -device ide-drive,drive=disk,bus=ahci.0 -net nic,model=rtl8139 -net user,id=u1 -object filter-dump,id=f1,netdev=u1,file=dump.dat -device ich9-intel-hda -device hda-output -device ich9-usb-uhci3 -cdrom "ISO/os.iso" -boot d -device virtio-vga,virgl=on -display gtk,gl=on
    DEPENDS image
    DEPENDS disk.img)