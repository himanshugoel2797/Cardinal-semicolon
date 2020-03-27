/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <types.h>

#include "ps2.h"
#include "priv_ps2.h"

#include "SysInterrupts/interrupts.h"

static void ps2_irq(int int_num)
{
    int_num = 0;

    if (PS2_ReadStatus() & 0x20) //Mouse
    {
        int x_off = 0, y_off = 0, scroll_off = 0;
        bool left_btn = false, right_btn = false, middle_btn = false, fwd_btn = false, bck_btn = false;

        if (PS2Mouse_IsFiveButton())
        {
            //Expect 4 bytes
            uint8_t b0 = inb(DATA_PORT);
            uint8_t b1 = inb(DATA_PORT);
            uint8_t b2 = inb(DATA_PORT);
            uint8_t b3 = inb(DATA_PORT);

            x_off += (int)b1 - (int)((b0 << 4) & 0x100);
            y_off += (int)b2 - (int)((b0 << 3) & 0x100);
            if ((b3 & 0xf) == 0x1)
                scroll_off += 1;
            else if ((b3 & 0xf) == 0xf)
                scroll_off -= 1;

            left_btn = b0 & 1;
            right_btn = (b0 & 2) >> 1;
            middle_btn = (b0 & 4) >> 2;
            fwd_btn = (b3 & 32) >> 5;
            bck_btn = (b3 & 16) >> 4;
        }
        else if (PS2Mouse_HasScrollWheel())
        {
            //Expect 4 bytes
            uint8_t b0 = inb(DATA_PORT);
            uint8_t b1 = inb(DATA_PORT);
            uint8_t b2 = inb(DATA_PORT);
            uint8_t b3 = inb(DATA_PORT);

            x_off += (int)b1 - (int)((b0 << 4) & 0x100);
            y_off += (int)b2 - (int)((b0 << 3) & 0x100);
            if ((b3 & 0xf) == 0x1)
                scroll_off += 1;
            else if ((b3 & 0xf) == 0xf)
                scroll_off -= 1;

            left_btn = b0 & 1;
            right_btn = (b0 & 2) >> 1;
            middle_btn = (b0 & 4) >> 2;
            fwd_btn = false;
            bck_btn = false;
        }
        else
        {
            //Expect 3 bytes
            uint8_t b0 = inb(DATA_PORT);
            uint8_t b1 = inb(DATA_PORT);
            uint8_t b2 = inb(DATA_PORT);

            x_off += (int)b1 - (int)((b0 << 4) & 0x100);
            y_off += (int)b2 - (int)((b0 << 3) & 0x100);
            scroll_off = 0;

            left_btn = b0 & 1;
            right_btn = (b0 & 2) >> 1;
            middle_btn = (b0 & 4) >> 2;
            fwd_btn = false;
            bck_btn = false;
        }

        DEBUG_PRINT("[PS/2] Mouse Interrupt: X: ");
        char tmp_buf[10];
        DEBUG_PRINT(itoa(x_off, tmp_buf, 10));
        DEBUG_PRINT(", Y: ");
        DEBUG_PRINT(itoa(y_off, tmp_buf, 10));
        DEBUG_PRINT(", Scroll: ");
        DEBUG_PRINT(itoa(scroll_off, tmp_buf, 10));
        DEBUG_PRINT(", L: ");
        DEBUG_PRINT(left_btn ? "Down" : "Up");
        DEBUG_PRINT(", R: ");
        DEBUG_PRINT(right_btn ? "Down" : "Up");
        DEBUG_PRINT(", M: ");
        DEBUG_PRINT(middle_btn ? "Down" : "Up");
        DEBUG_PRINT(", F: ");
        DEBUG_PRINT(fwd_btn ? "Down" : "Up");
        DEBUG_PRINT(", B: ");
        DEBUG_PRINT(bck_btn ? "Down" : "Up");
        DEBUG_PRINT("\r\n");
    }
    else //Keyboard
    {
        uint8_t c = 0;
        char tmp_buf[10];
        c = inb(DATA_PORT);
        if (c != 0xFA)
        {
            DEBUG_PRINT("[PS/2] Keyboard Interrupt: ");
            DEBUG_PRINT(itoa(c, tmp_buf, 16));
            DEBUG_PRINT("\r\n");
        }
    }
}

uint8_t PS2_Initialize()
{
    //Disable the ports
    outb(CMD_PORT, DISABLE_PORT1_CMD);
    WAIT_CMD_SENT;
    outb(CMD_PORT, DISABLE_PORT2_CMD);
    WAIT_CMD_SENT;

    //Flush the output buffer
    while (PS2_ReadStatus() & 1)
        inb(DATA_PORT);

    //Read the controller configuration byte
    uint8_t cfg = PS2_ReadConfig();

    cfg &= ~(1 << 0);
    cfg &= ~(1 << 1);
    cfg &= ~(1 << 6); //Disable translation

    uint8_t isDualChannel = (cfg & (1 << 5)); //If clear, not dual channel

    //Write the controller config
    PS2_WriteConfig(cfg);

    outb(CMD_PORT, PERFORM_SELFTEST);
    WAIT_DATA_AVL;
    uint8_t test_result = inb(DATA_PORT);

    //If test didn't pass, return -1
    if (test_result != 0x55)
        return -1;

    //Rewrite hte configuration byte in case of a reset
    PS2_WriteConfig(cfg);

    if (isDualChannel)
    {
        //If test showed it was dual channel first, check properly
        outb(CMD_PORT, ENABLE_PORT2_CMD);
        cfg = PS2_ReadConfig();
        if (cfg & (1 << 5))
            isDualChannel = 0; //If bit is still set, not dual channel

        if (isDualChannel)
            outb(CMD_PORT, DISABLE_PORT2_CMD);
    }

    cfg |= (1 << 4); //Disable the first port
    cfg |= (1 << 5); //Disable the second port
    cfg |= (1 << 2); //Set the system flag
    PS2_WriteConfig(cfg);

    outb(CMD_PORT, PERFORM_PORT1TEST);
    WAIT_DATA_AVL;
    uint8_t port1_test_result = inb(DATA_PORT);
    uint8_t port2_test_result = 1;

    if (isDualChannel)
    {
        outb(CMD_PORT, PERFORM_PORT2TEST);
        WAIT_DATA_AVL;
        port2_test_result = inb(DATA_PORT);
    }

    //If both tests failed, return -1
    if (port1_test_result != 0 && port2_test_result != 0)
        return -1;

    cfg = PS2_ReadConfig();
    if (port1_test_result == 0)
    {
        outb(CMD_PORT, ENABLE_PORT1_CMD);
        cfg |= (1 << 0);
        cfg &= ~(1 << 4);
        WAIT_CMD_SENT;
    }

    if (port2_test_result == 0)
    {
        outb(CMD_PORT, ENABLE_PORT2_CMD);
        cfg |= (1 << 1);
        cfg &= ~(1 << 5);
        WAIT_CMD_SENT;
    }
    PS2_WriteConfig(cfg);

    if (port1_test_result == 0)
    {
        PS2Keyboard_Initialize();
    }

    if (port2_test_result == 0)
    {
        PS2Mouse_Initialize();
    }
    DEBUG_PRINT("[PS/2] Devices inited\r\n");

    int kbd_irq = 33;
    int mouse_irq = 44;
    interrupt_allocate(1, interrupt_flags_exclusive | interrupt_flags_fixed, &kbd_irq);
    interrupt_allocate(1, interrupt_flags_exclusive | interrupt_flags_fixed, &mouse_irq);
    {
        char vector_str[10];
        DEBUG_PRINT("[PS/2] Allocated Interrupt Vector: ");
        DEBUG_PRINT(itoa(kbd_irq, vector_str, 10));
        DEBUG_PRINT("\r\n");
    }
    {
        char vector_str[10];
        DEBUG_PRINT("[PS/2] Allocated Interrupt Vector: ");
        DEBUG_PRINT(itoa(mouse_irq, vector_str, 10));
        DEBUG_PRINT("\r\n");
    }

    interrupt_registerhandler(kbd_irq, ps2_irq);
    interrupt_registerhandler(mouse_irq, ps2_irq);

    interrupt_setmask(1, false);
    interrupt_setmask(12, false);

    return 0;
}

uint8_t PS2_ReadStatus()
{
    return inb(CMD_PORT);
}

uint8_t PS2_ReadConfig()
{
    outb(CMD_PORT, READ_CFG_CMD);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    return inb(DATA_PORT);
}

void PS2_WriteConfig(uint8_t cfg)
{
    outb(CMD_PORT, WRITE_CFG_CMD);
    WAIT_CMD_SENT;
    WAIT_DATA_SENT;
    outb(DATA_PORT, cfg);
    WAIT_CMD_SENT;
}
