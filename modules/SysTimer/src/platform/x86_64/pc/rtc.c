/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <types.h>

#include "priv_timers.h"
#include "timer.h"
#include "SysInterrupts/interrupts.h"

#define SECONDS 0x00
#define MINUTES 0x02
#define HOURS 0x04
#define WEEKDAY 0x06
#define DAYOFMONTH 0x07
#define MONTH 0x08
#define YEAR 0x09
#define CENTURY 0x32
#define STATUS_A 0x0A
#define STATUS_B 0x0B
#define BIN_FROM_BCD(bcd) (((bcd / 16) * 10) + (bcd & 0xf))

static uint8_t century = 0, year = 0, month = 0, dayofmonth = 0, weekday = 0, hours = 0, minutes = 0, seconds = 0;
static uint64_t ms_10000000x = 0, ms_sinceboot = 0;

static uint8_t rtc_read(uint8_t off) {

    int state = cli();
    outb(0x70, (1 << 7) | off);
    uint8_t val = inb(0x71);
    sti(state);

    return val;
}

static void rtc_write(uint8_t off, uint8_t val) {

    int state = cli();
    outb(0x70, (1 << 7) | off);
    outb(0x71, val);
    sti(state);
}

static void rtc_gettime(uint8_t *century, uint8_t *year, uint8_t *month, uint8_t *dayofmonth, uint8_t *weekday, uint8_t *hours, uint8_t *minutes, uint8_t *seconds){
    
    while((rtc_read(STATUS_A) & 0x80) != 0x80); //Wait for update in progress bit to be set
    while((rtc_read(STATUS_A) & 0x80) == 0x80); //Wait for update in progress bit to be clear
    
    //Read the time
    uint8_t rtc_data[64];
    for(uint8_t i = 0; i < 64; i++)
        rtc_data[i] = rtc_read(i);

    //Determine the format of the rtc
    bool _24hr = (rtc_data[STATUS_B] & 2);
    bool isBinary = (rtc_data[STATUS_B] & 4);

    bool pm = false;
    if(!_24hr){
        pm = rtc_data[HOURS] & 0x80;
        rtc_data[HOURS] &= ~0x80;
    }

    if(!isBinary){
        #define CONV_RTC(x) rtc_data[ x ] = BIN_FROM_BCD(rtc_data[ x ]);
        CONV_RTC(SECONDS)
        CONV_RTC(MINUTES)
        CONV_RTC(HOURS)
        CONV_RTC(WEEKDAY)
        CONV_RTC(DAYOFMONTH)
        CONV_RTC(MONTH)
        CONV_RTC(YEAR)
        CONV_RTC(CENTURY)
    }

    if(!_24hr && pm){
        rtc_data[HOURS] += 12;
    }

    *century = rtc_data[CENTURY];
    *year = rtc_data[YEAR];
    *month = rtc_data[MONTH];
    *dayofmonth = rtc_data[DAYOFMONTH];
    *weekday = rtc_data[WEEKDAY];
    *hours = rtc_data[HOURS];
    *minutes = rtc_data[MINUTES];
    *seconds = rtc_data[SECONDS];
}

static void rtc_interrupt(int int_num){
    int_num = 0;

    uint8_t int_mask = rtc_read(0x0C);
    if (int_mask & 0x40) ms_10000000x += 9765625;   //increment by the period of the interrupt
    if (ms_10000000x >= 10000000000){
        seconds ++;
        ms_10000000x -= 10000000000;
        ms_sinceboot ++;
    }
    if(seconds >= 60){
        seconds -= 60;
        minutes++;
    }
    if(minutes >= 60){
        minutes -= 60;
        hours++;
    }
    if(hours >= 24){
        hours -= 24;
        weekday = (weekday + 1) % 7;
        dayofmonth++;
    }
    if(dayofmonth >= 31){
        dayofmonth -= 31;
        month++;
    }
    if(month >= 12){
        month -= 12;
        year++;
    }
    if(year >= 100){
        year -= 100;
        century++;
    }
    //TODO: Fix the time update routine
}

static uint64_t rtc_readtime(timer_handlers_t *t){
    t = NULL;
    return ms_sinceboot;
}

PRIVATE int rtc_init() {
    //Read the start time and setup a periodic interrupt to maintain system time in ms
    rtc_gettime(&century, &year, &month, &dayofmonth, &weekday, &hours, &minutes, &seconds);
    century = atoi(CURRENT_YEAR, 10) / 100;

    //The read is guaranteed to start at ms = 0, thus start the periodic interrupt
    int irq = 8 + 32;
    if(interrupt_allocate(1, interrupt_flags_fixed, &irq) == 0){
        interrupt_registerhandler(irq, rtc_interrupt);
        rtc_write(STATUS_B, rtc_read(STATUS_B) | 0x40);
    
        //Register a read only realtime timer
        timer_handlers_t main_counter = { .name = "rtc" };
        timer_features_t main_features = timer_features_persistent | timer_features_counter | timer_features_read;
        //TODO: Compute and add timer_features_absolute for real world time

        main_counter.rate = 1000;
        main_counter.read = rtc_readtime;
        main_counter.write = NULL;
        main_counter.set_mode = NULL;
        main_counter.set_enable = NULL;
        main_counter.set_handler = NULL;

        timer_register(main_features, &main_counter);
        DEBUG_PRINT("RTC Initialized\r\n");
        return 0;
    }
    

    return -1;
}