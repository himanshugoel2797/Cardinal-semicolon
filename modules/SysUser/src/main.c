/**
 * Copyright (c) 2017 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "plat_defs.h"
#include "boot_information.h"

int module_init(){

    /*CardinalBootInfo *bInfo = GetBootInfo();
    uint32_t* v = (uint32_t*)(0xffff800000000000ull + bInfo->FramebufferAddress);
    //__asm__("cli\n\thlt" :: "a"(bInfo->FramebufferHeight), "b"(bInfo->FramebufferAddress));
    for(uint32_t y = 0; y < bInfo->FramebufferHeight; y++)
        for(uint32_t x = 0; x < bInfo->FramebufferWidth; x++)
        {
            v[y * bInfo->FramebufferPitch/sizeof(uint32_t) + x] = 0xaaaaaaaa;
        }
*/
    //Setup syscall system
    syscall_plat_init();
    return 0;
}

int user_mp_init() {
    syscall_plat_init();
    return 0;
}