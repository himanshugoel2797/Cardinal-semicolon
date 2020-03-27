/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdqueue.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

#include "ps2.h"
#include "priv_ps2.h"

#include "SysTimer/timer.h"
#include "SysInterrupts/interrupts.h"
#include "CoreInput/input.h"

static int kbd_lock = 0;
static queue_t kbd_q;
static int mouse_lock = 0;
static queue_t mouse_q;

struct ss3_translation_tbl
{
    uint8_t code;
    kbd_keys_t key;
};

static struct ss3_translation_tbl ss3[] = {
    {.code = 0x1C, .key = kbd_keys_A},
    {.code = 0x32, .key = kbd_keys_B},
    {.code = 0x21, .key = kbd_keys_C},
    {.code = 0x23, .key = kbd_keys_D},
    {.code = 0x24, .key = kbd_keys_E},
    {.code = 0x2B, .key = kbd_keys_F},
    {.code = 0x34, .key = kbd_keys_G},
    {.code = 0x33, .key = kbd_keys_H},
    {.code = 0x43, .key = kbd_keys_I},
    {.code = 0x3B, .key = kbd_keys_J},
    {.code = 0x42, .key = kbd_keys_K},
    {.code = 0x4B, .key = kbd_keys_L},
    {.code = 0x3A, .key = kbd_keys_M},
    {.code = 0x31, .key = kbd_keys_N},
    {.code = 0x44, .key = kbd_keys_O},
    {.code = 0x4D, .key = kbd_keys_P},
    {.code = 0x15, .key = kbd_keys_Q},
    {.code = 0x2D, .key = kbd_keys_R},
    {.code = 0x1B, .key = kbd_keys_S},
    {.code = 0x2C, .key = kbd_keys_T},
    {.code = 0x3C, .key = kbd_keys_U},
    {.code = 0x2A, .key = kbd_keys_V},
    {.code = 0x1D, .key = kbd_keys_W},
    {.code = 0x22, .key = kbd_keys_X},
    {.code = 0x35, .key = kbd_keys_Y},
    {.code = 0x1A, .key = kbd_keys_Z},
    {.code = 0x45, .key = kbd_keys_0},
    {.code = 0x16, .key = kbd_keys_1},
    {.code = 0x1E, .key = kbd_keys_2},
    {.code = 0x26, .key = kbd_keys_3},
    {.code = 0x25, .key = kbd_keys_4},
    {.code = 0x2E, .key = kbd_keys_5},
    {.code = 0x36, .key = kbd_keys_6},
    {.code = 0x3D, .key = kbd_keys_7},
    {.code = 0x3E, .key = kbd_keys_8},
    {.code = 0x46, .key = kbd_keys_9},
    {.code = 0x0E, .key = kbd_keys_tick},
    {.code = 0x4E, .key = kbd_keys_sub},
    {.code = 0x55, .key = kbd_keys_eq},
    {.code = 0x5C, .key = kbd_keys_bckslash},
    {.code = 0x66, .key = kbd_keys_bksp},
    {.code = 0x29, .key = kbd_keys_space},
    {.code = 0x0D, .key = kbd_keys_tab},
    {.code = 0x14, .key = kbd_keys_caps},
    {.code = 0x12, .key = kbd_keys_lshft},
    {.code = 0x11, .key = kbd_keys_lctrl},
    {.code = 0x8B, .key = kbd_keys_lwin},
    {.code = 0x19, .key = kbd_keys_lalt},
    {.code = 0x59, .key = kbd_keys_rshft},
    {.code = 0x58, .key = kbd_keys_rctrl},
    {.code = 0x8C, .key = kbd_keys_rwin},
    {.code = 0x39, .key = kbd_keys_ralt},
    {.code = 0x8D, .key = kbd_keys_apps},
    {.code = 0x5A, .key = kbd_keys_enter},
    {.code = 0x08, .key = kbd_keys_esc},
    {.code = 0x07, .key = kbd_keys_f1},
    {.code = 0x0F, .key = kbd_keys_f2},
    {.code = 0x17, .key = kbd_keys_f3},
    {.code = 0x1F, .key = kbd_keys_f4},
    {.code = 0x27, .key = kbd_keys_f5},
    {.code = 0x2F, .key = kbd_keys_f6},
    {.code = 0x37, .key = kbd_keys_f7},
    {.code = 0x3F, .key = kbd_keys_f8},
    {.code = 0x47, .key = kbd_keys_f9},
    {.code = 0x4F, .key = kbd_keys_f10},
    {.code = 0x56, .key = kbd_keys_f11},
    {.code = 0x5E, .key = kbd_keys_f12},
    {.code = 0x57, .key = kbd_keys_prntscrn},
    {.code = 0x5F, .key = kbd_keys_scroll},
    {.code = 0x62, .key = kbd_keys_pause},
    {.code = 0x54, .key = kbd_keys_lsqbrac},
    {.code = 0x67, .key = kbd_keys_insert},
    {.code = 0x6E, .key = kbd_keys_home},
    {.code = 0x6F, .key = kbd_keys_pgup},
    {.code = 0x64, .key = kbd_keys_del},
    {.code = 0x65, .key = kbd_keys_end},
    {.code = 0x6D, .key = kbd_keys_pgdn},
    {.code = 0x63, .key = kbd_keys_up},
    {.code = 0x61, .key = kbd_keys_left},
    {.code = 0x60, .key = kbd_keys_down},
    {.code = 0x6A, .key = kbd_keys_right},
    {.code = 0x76, .key = kbd_keys_num},
    {.code = 0x4A, .key = kbd_keys_kp_div},
    {.code = 0x7E, .key = kbd_keys_kp_mul},
    {.code = 0x4E, .key = kbd_keys_kp_sub},
    {.code = 0x7C, .key = kbd_keys_kp_add},
    {.code = 0x79, .key = kbd_keys_kp_en},
    {.code = 0x71, .key = kbd_keys_kp_dot},
    {.code = 0x70, .key = kbd_keys_kp_0},
    {.code = 0x69, .key = kbd_keys_kp_1},
    {.code = 0x72, .key = kbd_keys_kp_2},
    {.code = 0x7A, .key = kbd_keys_kp_3},
    {.code = 0x6B, .key = kbd_keys_kp_4},
    {.code = 0x73, .key = kbd_keys_kp_5},
    {.code = 0x74, .key = kbd_keys_kp_6},
    {.code = 0x6C, .key = kbd_keys_kp_7},
    {.code = 0x75, .key = kbd_keys_kp_8},
    {.code = 0x7D, .key = kbd_keys_kp_9},
    {.code = 0x5B, .key = kbd_keys_rsqbrac},
    {.code = 0x4C, .key = kbd_keys_semicolon},
    {.code = 0x52, .key = kbd_keys_apostrophe},
    {.code = 0x41, .key = kbd_keys_comma},
    {.code = 0x49, .key = kbd_keys_period},
    {.code = 0x4A, .key = kbd_keys_fwdslash},
    {.code = 0, .key = kbd_keys_unkn},
};

static void ps2_irq(int int_num)
{
    int_num = 0;

    uint64_t stamp = timer_timestamp();
    //char tmp_buf[20];
    //DEBUG_PRINT("Timestamp: ");
    //DEBUG_PRINT(ltoa(stamp, tmp_buf, 16));
    //DEBUG_PRINT("\r\n");

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

        local_spinlock_lock(&mouse_lock);
        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (1ull << 32) | (uint64_t)x_off);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (2ull << 32) | (uint64_t)y_off);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (3ull << 32) | (uint64_t)scroll_off);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (4ull << 32) | (uint64_t)left_btn);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (5ull << 32) | (uint64_t)right_btn);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (6ull << 32) | (uint64_t)middle_btn);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (7ull << 32) | (uint64_t)fwd_btn);

        if (queue_full(&mouse_q))
        { //If we can't queue further, drop the oldest packet
            uint64_t dump = 0;
            queue_trydequeue(&mouse_q, &dump);
            queue_trydequeue(&mouse_q, &dump);
        }
        queue_tryenqueue(&mouse_q, stamp);
        queue_tryenqueue(&mouse_q, (8ull << 32) | (uint64_t)bck_btn);

        local_spinlock_unlock(&mouse_lock);

        /*DEBUG_PRINT("[PS/2] Mouse Interrupt: X: ");
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
        DEBUG_PRINT("\r\n");*/
    }
    else //Keyboard
    {
        uint8_t c = 0;
        //char tmp_buf[10];
        c = inb(DATA_PORT);
        if (c != 0xFA)
        {
            //Read and send translated scancodes to the Input manager
            if (PS2Keyboard_ActiveScancodeSet() == 3)
            {
                bool break_code = false;
                if (c == 0xf0)
                {
                    break_code = true;
                    c = inb(DATA_PORT);
                }

                local_spinlock_lock(&kbd_lock);
                if (queue_full(&kbd_q))
                {
                    uint64_t dummy = 0;
                    queue_trydequeue(&kbd_q, &dummy);
                    queue_trydequeue(&kbd_q, &dummy);
                }
                queue_tryenqueue(&kbd_q, stamp);
                queue_tryenqueue(&kbd_q, ((uint64_t)break_code << 32) | c);
                local_spinlock_unlock(&kbd_lock);
            }
            //DEBUG_PRINT("[PS/2] Keyboard Interrupt: ");
            //DEBUG_PRINT(itoa(c, tmp_buf, 16));
            //DEBUG_PRINT("\r\n");
        }
    }
}

static bool ps2_has_pending(void *state)
{
    if (state == NULL)
    {
        //keyboard
        int cli_state = cli();
        local_spinlock_lock(&kbd_lock);
        bool retVal = queue_entcnt(&kbd_q) != 0;
        local_spinlock_unlock(&kbd_lock);
        sti(cli_state);
        return retVal;
    }
    else
    {
        //mouse
        int cli_state = cli();
        local_spinlock_lock(&mouse_lock);
        bool retVal = queue_entcnt(&mouse_q) != 0;
        local_spinlock_unlock(&mouse_lock);
        sti(cli_state);
        return retVal;
    }
}

static void ps2_read(void *state, input_device_event_t *event)
{
    if (state == NULL)
    {
        //keyboard
        int cli_state = cli();
        local_spinlock_lock(&kbd_lock);
        uint64_t stamp = 0;
        uint64_t inpt = 0;
        queue_trydequeue(&kbd_q, &stamp);
        queue_trydequeue(&kbd_q, &inpt);
        local_spinlock_unlock(&kbd_lock);
        sti(cli_state);

        uint32_t input_type = inpt & 0xffffffff;

        event->timestamp = stamp;

        int idx = 0;
        for (; ss3[idx].code != 0; idx++)
            if (ss3[idx].code == input_type)
            {
                event->is_btn_event = true;
                event->index = (uint32_t)ss3[idx].key;
                event->state = (bool)(inpt >> 32);
                break;
            }
        if (ss3[idx].code == 0)
        {
            event->is_btn_event = true;
            event->index = (uint32_t)kbd_keys_unkn;
            event->state = false;
        }
    }
    else
    {
        //mouse
        int cli_state = cli();
        local_spinlock_lock(&mouse_lock);
        uint64_t stamp = 0;
        uint64_t inpt = 0;
        queue_trydequeue(&mouse_q, &stamp);
        queue_trydequeue(&mouse_q, &inpt);
        local_spinlock_unlock(&mouse_lock);
        sti(cli_state);

        uint32_t input_type = inpt >> 32;

        event->timestamp = stamp;
        if (input_type == 4 || input_type == 5 || input_type == 6 || input_type == 7 || input_type == 8)
        {
            event->is_btn_event = true;
            event->index = input_type;
            event->state = inpt & 1;
        }
        else
        {
            event->is_btn_event = false;
            event->index = input_type;
            event->position = (int)(inpt & 0xffffffff);
        }
    }
}

static input_device_desc_t kbd_desc = {
    .name = "PS/2 Keyboard",
    .state = NULL,
    .features = input_device_features_none,
    .type = input_device_type_keyboard,
    .handlers = {
        .has_pending = ps2_has_pending,
        .read = ps2_read,
    },
};

static input_device_desc_t mouse_desc = {
    .name = "PS/2 Mouse",
    .state = (void *)1,
    .features = input_device_features_none,
    .type = input_device_type_mouse,
    .handlers = {
        .has_pending = ps2_has_pending,
        .read = ps2_read,
    },
};

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

    //Rewrite the configuration byte in case of a reset
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

    queue_init(&kbd_q, BUF_LEN * ENT_SIZE);
    queue_init(&mouse_q, BUF_LEN * ENT_SIZE);

    input_device_register(&kbd_desc);
    input_device_register(&mouse_desc);

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
