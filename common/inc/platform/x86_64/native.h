// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_X86_64_NATIVE_H
#define CARDINAL_X86_64_NATIVE_H

#define PAT_MSR (0x00000277)
#define EFER_MSR (0xC0000080)
#define IA32_APIC_BASE (0x0000001B)

__attribute__((always_inline)) static __inline void outb(const uint16_t port, const uint8_t val)
{
    __asm__ volatile("outb %1, %0"
                     :
                     : "dN"(port), "a"(val));
}

__attribute__((always_inline)) static __inline uint8_t inb(const uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0"
                     : "=a"(ret)
                     : "dN"(port));
    return ret;
}

__attribute__((always_inline)) static __inline void outw(const uint16_t port, const uint16_t val)
{
    __asm__ volatile("outw %1, %0"
                     :
                     : "dN"(port), "a"(val));
}

__attribute__((always_inline)) static __inline uint16_t inw(const uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0"
                     : "=a"(ret)
                     : "dN"(port));
    return ret;
}

__attribute__((always_inline)) static __inline void outl(const uint16_t port, const uint32_t val)
{
    __asm__ volatile("outl %1, %0"
                     :
                     : "dN"(port), "a"(val));
}

__attribute__((always_inline)) static __inline uint32_t inl(const uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0"
                     : "=a"(ret)
                     : "dN"(port));
    return ret;
}

__attribute__((always_inline)) static __inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = val;
    uint32_t hi = (val >> 32);
    __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(msr));
}

__attribute__((always_inline)) static __inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdmsr"
                     : "=a"(lo), "=d"(hi)
                     : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

__attribute__((always_inline)) static __inline void halt(void)
{
    __asm__ volatile("hlt");
}

__attribute__((always_inline)) static __inline int cli(void)
{
    uint64_t flags = 0;
    __asm__ volatile(
        "pushf\n\t"
        "popq %0\n\t"
        "cli\n\t"
        : "=r"(flags));

    return (flags & 0x200);
}

__attribute__((always_inline)) static __inline void sti(int state)
{
    if (state)
        __asm__ volatile("sti");
}

__attribute__((always_inline)) static __inline uint32_t popcnt64(uint64_t a)
{
    uint32_t r = 0;
    __asm__ volatile("popcntq %1,%0"
                     : "=r"(r)
                     : "rm"(a)
                     : "cc");
    return r;
}

__attribute__((always_inline)) static __inline uint32_t popcnt32(uint32_t a)
{
    uint32_t r = 0;
    __asm__ volatile("popcntd %1,%0"
                     : "=r"(r)
                     : "rm"(a)
                     : "cc");
    return r;
}
#endif