// silicon - cpu tables, interrupts, timer
#pragma once
#include "../core.h"

struct __attribute__((packed)) IntFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no, err;
    u64 rip, cs, rflags, rsp, ss;
};

typedef void (*IrqHandler)(IntFrame*);

void gdt_init();
void idt_init();
void irq_set_handler(int irq, IrqHandler h);
void irq_unmask(int irq);
void pit_init(u32 hz);
u64  idle_ticks();                    // ms spent halted (for the cpu graph)
void cpu_idle();                      // hlt and account idle time
void cpu_rest();                      // spin ~1 ms (no hlt oversleep) and account idle time
u32  cpu_mhz();                       // measured TSC rate
const char* cpu_vendor();
const char* cpu_brand();
