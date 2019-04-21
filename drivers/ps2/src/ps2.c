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

static void ps2_kbd_irq(int int_num) {
    int_num = 0;

    char c = 0;
    do {
        if(inb(DATA_PORT) != c) {
            c = inb(DATA_PORT);
            if(c > 0)
                break;
        }
    } while(1);
    if(c == 0x1c)
        DEBUG_PRINT("Keyboard Interrupt\r\n");
}

static void ps2_mouse_irq(int int_num) {
    int_num = 0;

    DEBUG_PRINT("Mouse Interrupt\r\n");
}

uint8_t PS2_Initialize() {
    //Disable the ports
    outb(CMD_PORT, DISABLE_PORT1_CMD);
    WAIT_CMD_SENT;
    outb(CMD_PORT, DISABLE_PORT2_CMD);
    WAIT_CMD_SENT;

    //Flush the output buffer
    while(PS2_ReadStatus() & 1) inb(DATA_PORT);

    //Read the controller configuration byte
    uint8_t cfg = PS2_ReadConfig();

    cfg &= ~(1);
    cfg &= ~(1<<1);
    cfg &= ~(1<<6);

    uint8_t isDualChannel = (cfg & (1<<5)); //If clear, not dual channel

    //Write the controller config
    PS2_WriteConfig(cfg);

    outb(CMD_PORT, PERFORM_SELFTEST);
    WAIT_DATA_AVL;
    uint8_t test_result = inb(DATA_PORT);

    //If test didn't pass, return -1
    if(test_result != 0x55) return -1;

    if(isDualChannel) { //If test showed it was dual channel first, check properly
        outb(CMD_PORT, ENABLE_PORT2_CMD);
        cfg = PS2_ReadConfig();
        if(cfg & (1<<5)) isDualChannel = 0; //If bit is still set, not dual channel

        if(isDualChannel) outb(CMD_PORT, DISABLE_PORT2_CMD);
    }

    cfg |= (1 << 4);   //Disable the first port
    cfg |= (1 << 5);   //Disable the second port
    cfg &= ~(1 << 6);   //Disable translation
    cfg |= (1 << 2);    //Set the system flag
    PS2_WriteConfig(cfg);

    outb(CMD_PORT, PERFORM_PORT1TEST);
    WAIT_DATA_AVL;
    uint8_t port1_test_result = inb(DATA_PORT);
    uint8_t port2_test_result = 1;

    if(isDualChannel) {
        outb(CMD_PORT, PERFORM_PORT2TEST);
        WAIT_DATA_AVL;
        port2_test_result = inb(DATA_PORT);
    }

    //If both tests failed, return -1
    if(port1_test_result != 0 && port2_test_result != 0) return -1;

    //uint8_t cfg = 0;
    //uint8_t port1_test_result = 0;
    //uint8_t port2_test_result = 0;


    cfg = PS2_ReadConfig();
    if(port1_test_result == 0) {
        outb(CMD_PORT, ENABLE_PORT1_CMD);
        cfg |= 1;
        cfg &= ~(1 << 4);
        WAIT_CMD_SENT;
    }

    if(port2_test_result == 0) {
        outb(CMD_PORT, ENABLE_PORT2_CMD);
        cfg |= 2;
        cfg &= ~(1 << 5);
        WAIT_CMD_SENT;
    }
    PS2_WriteConfig(cfg);

    if(port1_test_result == 0) {
        PS2Keyboard_Initialize();
    }

    if(port2_test_result == 0) {
        PS2Mouse_Initialize();
    }
    DEBUG_PRINT("Devices inited\r\n");

    interrupt_registerhandler(33, ps2_kbd_irq);
    interrupt_registerhandler(44, ps2_mouse_irq);

    interrupt_setmask(1, false);
    interrupt_setmask(12, false);

    return 0;
}

uint8_t PS2_ReadStatus() {
    return inb(CMD_PORT);
}

uint8_t PS2_ReadConfig() {
    outb(CMD_PORT, READ_CFG_CMD);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    return inb(DATA_PORT);
}

void PS2_WriteConfig(uint8_t cfg) {
    outb(CMD_PORT, WRITE_CFG_CMD);
    WAIT_CMD_SENT;
    WAIT_DATA_SENT;
    outb(DATA_PORT, cfg);
    WAIT_CMD_SENT;
}
