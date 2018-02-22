/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "registry.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <types.h>

typedef enum {
    CPUID_FEAT_ECX_SSE3 = 1 << 0,
    CPUID_FEAT_ECX_PCLMUL = 1 << 1,
    CPUID_FEAT_ECX_DTES64 = 1 << 2,
    CPUID_FEAT_ECX_MONITOR = 1 << 3,
    CPUID_FEAT_ECX_DS_CPL = 1 << 4,
    CPUID_FEAT_ECX_VMX = 1 << 5,
    CPUID_FEAT_ECX_SMX = 1 << 6,
    CPUID_FEAT_ECX_EST = 1 << 7,
    CPUID_FEAT_ECX_TM2 = 1 << 8,
    CPUID_FEAT_ECX_SSSE3 = 1 << 9,
    CPUID_FEAT_ECX_CID = 1 << 10,
    CPUID_FEAT_ECX_FMA = 1 << 12,
    CPUID_FEAT_ECX_CX16 = 1 << 13,
    CPUID_FEAT_ECX_ETPRD = 1 << 14,
    CPUID_FEAT_ECX_PDCM = 1 << 15,
    CPUID_FEAT_ECX_DCA = 1 << 18,
    CPUID_FEAT_ECX_SSE4_1 = 1 << 19,
    CPUID_FEAT_ECX_SSE4_2 = 1 << 20,
    CPUID_FEAT_ECX_x2APIC = 1 << 21,
    CPUID_FEAT_ECX_MOVBE = 1 << 22,
    CPUID_FEAT_ECX_POPCNT = 1 << 23,
    CPUID_FEAT_ECX_AES = 1 << 25,
    CPUID_FEAT_ECX_XSAVE = 1 << 26,
    CPUID_FEAT_ECX_OSXSAVE = 1 << 27,
    CPUID_FEAT_ECX_AVX = 1 << 28,

    CPUID_FEAT_EDX_FPU = 1 << 0,
    CPUID_FEAT_EDX_VME = 1 << 1,
    CPUID_FEAT_EDX_DE = 1 << 2,
    CPUID_FEAT_EDX_PSE = 1 << 3,
    CPUID_FEAT_EDX_TSC = 1 << 4,
    CPUID_FEAT_EDX_MSR = 1 << 5,
    CPUID_FEAT_EDX_PAE = 1 << 6,
    CPUID_FEAT_EDX_MCE = 1 << 7,
    CPUID_FEAT_EDX_CX8 = 1 << 8,
    CPUID_FEAT_EDX_APIC = 1 << 9,
    CPUID_FEAT_EDX_SEP = 1 << 11,
    CPUID_FEAT_EDX_MTRR = 1 << 12,
    CPUID_FEAT_EDX_PGE = 1 << 13,
    CPUID_FEAT_EDX_MCA = 1 << 14,
    CPUID_FEAT_EDX_CMOV = 1 << 15,
    CPUID_FEAT_EDX_PAT = 1 << 16,
    CPUID_FEAT_EDX_PSE36 = 1 << 17,
    CPUID_FEAT_EDX_PSN = 1 << 18,
    CPUID_FEAT_EDX_CLF = 1 << 19,
    CPUID_FEAT_EDX_DTES = 1 << 21,
    CPUID_FEAT_EDX_ACPI = 1 << 22,
    CPUID_FEAT_EDX_MMX = 1 << 23,
    CPUID_FEAT_EDX_FXSR = 1 << 24,
    CPUID_FEAT_EDX_SSE = 1 << 25,
    CPUID_FEAT_EDX_SSE2 = 1 << 26,
    CPUID_FEAT_EDX_SS = 1 << 27,
    CPUID_FEAT_EDX_HTT = 1 << 28,
    CPUID_FEAT_EDX_TM1 = 1 << 29,
    CPUID_FEAT_EDX_IA64 = 1 << 30,
    CPUID_FEAT_EDX_PBE = 1 << 31
} CPUID_FEAT;

typedef enum { CPUID_EAX = 0, CPUID_EBX, CPUID_ECX, CPUID_EDX } CPUID_REG;

typedef enum { CPUID_ECX_IGNORE = 0, CPUID_EAX_FIRST_PAGE = 1 } CPUID_REQUESTS;

static uint32_t eax, ebx, ecx, edx;
static uint32_t cache_line_size = 0;

static void CPUID_RequestInfo(uint32_t page, uint32_t idx, uint32_t *eax,
                              uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {

    uint32_t ax = page, bx = *ebx, cx = idx, dx = *edx;

    __asm__ volatile("cpuid\n\t"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx)
                     : "a"(ax), "c"(cx));

    *eax = ax;
    *ebx = bx;
    *ecx = cx;
    *edx = dx;
}

int add_cpuid() {
    uint32_t eax, ebx, ecx, edx;

    #define MANUFACT_AMD 1
    #define MANUFACT_INTEL 2
    int cpu_manufacturer = 0;

    // Processor manufacturer identification string
    {
        CPUID_RequestInfo(0, 0, &eax, &ebx, &ecx, &edx);

        char proc_str[13];
        proc_str[12] = 0;
        memcpy(proc_str, &ebx, sizeof(ebx));
        memcpy(proc_str + 4, &edx, sizeof(edx));
        memcpy(proc_str + 8, &ecx, sizeof(ecx));

        if(strcmp(proc_str, "GenuineIntel") == 0)
            cpu_manufacturer = MANUFACT_INTEL;
        else if(strcmp(proc_str, "AuthenticAMD") == 0)
            cpu_manufacturer = MANUFACT_AMD;

        if (registry_addkey_str("HW/PROC", "IDENT_STR", proc_str) !=
                registry_err_ok)
            return -1;
    }

    // Processor model identification
    {
        CPUID_RequestInfo(1, 0, &eax, &ebx, &ecx, &edx);
        uint32_t stepping = eax & 0x0F;
        uint32_t model = (eax & 0xF0) >> 4;
        uint32_t family = (eax & 0xF00) >> 8;
        uint32_t processor_type = (eax & 0xF000) >> 12;

        if (family == 15) {
            family += (eax & 0xFF00000) >> 20;
            model = model | ((eax & 0xF0000) >> 12);
        }

        if (registry_addkey_uint("HW/PROC", "STEPPING", stepping) !=
                registry_err_ok)
            return -1;

        if (registry_addkey_uint("HW/PROC", "MODEL", model) != registry_err_ok)
            return -1;

        if (registry_addkey_uint("HW/PROC", "FAMILY", family) != registry_err_ok)
            return -1;

        if (registry_addkey_uint("HW/PROC", "TYPE", processor_type) !=
                registry_err_ok)
            return -1;

        bool tsc_avail = (edx & (1 << 4));
        bool tsc_deadline = (ecx & (1 << 24));

        if(registry_addkey_bool("HW/PROC", "TSC_AVAIL", tsc_avail) != registry_err_ok)
            return -1;

        if(registry_addkey_bool("HW/PROC", "TSC_DEADLINE", tsc_deadline) != registry_err_ok)
            return -1;

        {

            //APIC frequency is the Bus frequency by default
            CPUID_RequestInfo(0x16, 0, &eax, &ebx, &ecx, &edx);
            uint32_t apic_rate = ecx & 0xFFFF;

            //Use the processor identification to configure special information, like APIC clock rates
            switch(cpu_manufacturer) {
                case MANUFACT_AMD:
                {
                    //This method works for Zen only
                    uint32_t tsc_freq = (rdmsr(0xc0010064) & 0xff) * 25 * (1000 * 1000);

                    if(registry_addkey_uint("HW/PROC", "TSC_FREQ", 100 * 1000 * 1000) != registry_err_ok)
                        return -1;

                    //Default to 100MHz
                    if(apic_rate == 0)
                        apic_rate = 100;

                    if(registry_addkey_uint("HW/PROC", "APIC_FREQ", apic_rate * 1000 * 1000) != registry_err_ok)
                        return -1;
                }
                break;
                case MANUFACT_INTEL:
                    {
                        CPUID_RequestInfo(0x15, 0, &eax, &ebx, &ecx, &edx);

                        uint32_t ratio_denom = eax;
                        uint32_t ratio_numer = ebx;
                        uint32_t clock_freq = ecx;

                        if(ratio_denom != 0) {
                            if(registry_addkey_uint("HW/PROC", "TSC_FREQ", clock_freq * ratio_numer / ratio_denom) != registry_err_ok)
                                return -1;
                        }else{
                            if(registry_addkey_uint("HW/PROC", "TSC_FREQ", 0) != registry_err_ok)
                                return -1;
                        }

                        if(apic_rate == 0)
                        switch(model){
                            //Nehalem
                            case 0x1a:
                            case 0x1e:
                            case 0x1f:
                            case 0x2e:

                            //Westmere
                            case 0x25:
                            case 0x2c:
                            case 0x2f:  //CPUID holds, else must callibrate
                            break;

                            case 0x2a: //Sandy Bridge
                            case 0x2d: //Sandy Bridge EP
                            case 0x3a: //Ivy Bridge
                            case 0x3e: //Ivy Bridge EP
                            case 0x3c: //Haswell DT
                            case 0x3f: //Haswell MB
                            case 0x45: //Haswell ULT
                            case 0x46: //Haswell ULX
                            case 0x3d: //Broadwell
                            case 0x47: //Broadwell H
                            case 0x56: //Broadwell EP
                            case 0x4f: //Broadwell EX
                            //MSR_PLATFORM_INFO gives the apic rate
                            apic_rate = ((rdmsr(0xce) >> 8) & 0xFF) * 100;
                            break;
                            
                            case 0x4e: //Skylake Y/U
                            case 0x5e: //Skylake H/S
                            case 0x55: //Skylake E

                            case 0x8e: //Kabylake Y/U
                            case 0x9e: //Kabylake H/S
                                apic_rate = 24;
                            break;
                        }
                        if(registry_addkey_uint("HW/PROC", "APIC_FREQ", apic_rate * 1000 * 1000) != registry_err_ok)
                            return -1;
                    }
                break;
            }
        }

    }

    // Memory information
    {
        CPUID_RequestInfo(2, 0, &eax, &ebx, &ecx, &edx);
        uint32_t phys_bits = (eax & 0xFF);
        uint32_t virt_bits = (eax >> 8) & 0xFF;

        if (registry_addkey_uint("HW/PHYS_MEM", "BITS", phys_bits) !=
                registry_err_ok)
            return -1;

        if (registry_addkey_uint("HW/VIRT_MEM", "BITS", virt_bits) !=
                registry_err_ok)
            return -1;
    }

    // TODO: Determine initial cache parameters to configure the page allocator
    {
        uint32_t cache_idx = 0, cache_type, cache_level, apic_id_cnt, thread_cnt,
                 line_size, associativity, partition_cnt, set_cnt;
        bool fully_associative;
        do {
            char idx_str[10];
            char dir_str[256] = "HW/CACHE/";
            CPUID_RequestInfo(4, cache_idx, &eax, &ebx, &ecx, &edx);

            cache_type = (eax & 0x1F);
            if (cache_type == 0)
                break;

            apic_id_cnt = ((eax >> 26) & 0x3F) + 1;
            thread_cnt = ((eax >> 14) & 0xFFF) + 1;
            fully_associative = (eax >> 9) & 1;
            cache_level = (eax >> 5) & 0x7;

            associativity = ((ebx >> 22) + 1);
            partition_cnt = ((ebx >> 12) & 0x3FF) + 1;
            line_size = (ebx & 0xFFF) + 1;
            set_cnt = ecx + 1;

            strncat(dir_str, itoa(cache_idx, idx_str, 16), 255);

            if (registry_createdirectory("HW/CACHE", idx_str) != registry_err_ok)
                return -1;

            if (registry_addkey_uint(dir_str, "ASSOCIATIVITY", associativity) !=
                    registry_err_ok)
                return -1;

            if (registry_addkey_uint(dir_str, "PARITITIONS", partition_cnt) !=
                    registry_err_ok)
                return -1;

            if (registry_addkey_uint(dir_str, "LINE_SZ", line_size) !=
                    registry_err_ok)
                return -1;

            if (registry_addkey_uint(dir_str, "SET_CNT", set_cnt) != registry_err_ok)
                return -1;

            cache_idx++;
        } while (true);
    }

    {
        //Get descriptors
        CPUID_RequestInfo(2, 0, &eax, &ebx, &ecx, &edx);
        //__asm__("hlt" :: "a"(eax), "b"(ebx), "c"(ecx), "d"(edx));

    }

    {
        CPUID_RequestInfo(7, 0, &eax, &ebx, &ecx, &edx);

        //SMEP
        bool smep = (ebx >> 7) & 1;
        if (registry_addkey_bool("HW/PROC", "SMEP", smep) !=
                registry_err_ok)
            return -1;

        //SMAP
        bool smap = (ebx >> 20) & 1;
        if (registry_addkey_bool("HW/PROC", "SMAP", smap) !=
                registry_err_ok)
            return -1;
    }

    {
        CPUID_RequestInfo(0x80000001, 0, &eax, &ebx, &ecx, &edx);

        //1GB pages
        bool gibibyte_pages = (edx >> 26) & 1;
        if (registry_addkey_bool("HW/PROC", "HUGEPAGE", gibibyte_pages) !=
                registry_err_ok)
            return -1;
    }

    {
        CPUID_RequestInfo(0x80000007, 0, &eax, &ebx, &ecx, &edx);

        bool tsc_invar = (edx & (1 << 8));
        if (registry_addkey_bool("HW/PROC", "TSC_INVARIANT", tsc_invar) != registry_err_ok)
            return -1;
    }

    {
        CPUID_RequestInfo(0x1, 0, &eax, &ebx, &ecx, &edx);

        //x2APIC
        bool x2apic = (ecx >> 21) & 1;
        if (registry_addkey_bool("HW/PROC", "X2APIC", x2apic) !=
                registry_err_ok)
            return -1;
    }

    // TODO: Setup proper IDT and GDT

    // TODO: Come back after virtual memory and physical memory have been
    // initialized

    // TODO: Parse ACPI tables

    // TODO: Parse PCI device tree
    return 0;
}