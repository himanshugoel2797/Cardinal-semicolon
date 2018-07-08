/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <types.h>

#include "ps2_keyboard.h"
#include "priv_ps2.h"

uint8_t PS2Keyboard_Initialize()
{
    //Reset the keyboard
    WAIT_DATA_SENT;
    outb(DATA_PORT, 0xFF);
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    WAIT_DATA_AVL;
    uint16_t reset_res = inb(DATA_PORT);
    if(reset_res != 0xaa) DEBUG_PRINT("PS/2 Keyboard initialization failed.\r\n");

    
    //Set scancode set 3
    outb(DATA_PORT, 0xF0);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    outb(DATA_PORT, 0x03);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);

    //Confirm that scancode set 3 was set
    outb(DATA_PORT, 0xF0);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    outb(DATA_PORT, 0x00);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    WAIT_DATA_AVL;

    if(inb(DATA_PORT) != 3)
        DEBUG_PRINT("Scancode set 3 could not be configured.\r\n");

    //Configure repeat rate
    outb(DATA_PORT, 0xF3);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    outb(DATA_PORT, 0x00);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);

    outb(DATA_PORT, 0xF8);	//Enable make/break/typematic/repeat codes for all keys
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    
    //Enable sending scancodes
    outb(DATA_PORT, 0xF4);
    WAIT_CMD_SENT;
    WAIT_DATA_AVL;
    inb(DATA_PORT);

    while(IS_DATA_AVL) {
        char c = inb(DATA_PORT);
        char tmp[10];
        DEBUG_PRINT(itoa(c, tmp, 16));
        DEBUG_PRINT("\r\n");
    }
        

    return 0;
}

void PS2Keyboard_SetLEDStatus(uint8_t led, uint8_t status)
{
    WAIT_DATA_SENT;
    outb(DATA_PORT, 0xED);
    WAIT_DATA_SENT;
    outb(DATA_PORT, status << led);
    WAIT_DATA_AVL;
    inb(DATA_PORT);
    return;
}