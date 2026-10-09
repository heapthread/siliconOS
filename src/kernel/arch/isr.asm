; silicon - interrupt stubs (MS x64 ABI into isr_dispatch(IntFrame*))
;
; isr_dispatch returns the frame to resume. Returning a different frame than it was given is how
; the scheduler switches threads: every thread's context is an IntFrame on its own kernel stack,
; with its FPU/SSE state saved just below it.
bits 64
default rel
extern isr_dispatch

section .text

%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push %1
    jmp isr_common
%endmacro

%assign i 0
%rep 48
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep

; int 0x80: system call from a user process, int 0x81: the kernel's main thread hands the cpu to the
; user threads for a slice
global isr128
isr128:
    push 0
    push 0x80
    jmp isr_common
global isr129
isr129:
    push 0
    push 0x81
    jmp isr_common

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rbx, rsp                    ; frame pointer (callee-saved)
    lea rsp, [rbx - 528]            ; fxsave area just below the frame
    and rsp, -16
    fxsave [rsp]
    mov rcx, rbx
    sub rsp, 32
    call isr_dispatch               ; rax = the frame to resume (maybe another thread's)
    mov rbx, rax
    lea rsp, [rbx - 528]
    and rsp, -16
    fxrstor [rsp]
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .rdata
global isr_table
isr_table:
%assign i 0
%rep 48
    dq isr %+ i
  %assign i i+1
%endrep
global isr_sys_table
isr_sys_table:
    dq isr128
    dq isr129

section .text
; void gdt_load(void* desc, u16 code, u16 data)
global gdt_load
gdt_load:
    lgdt [rcx]
    mov ax, r8w
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    movzx rdx, dx
    lea rax, [.reload]
    push rdx
    push rax
    retfq
.reload:
    ret

; void tss_load(u16 selector)
global tss_load
tss_load:
    ltr cx
    ret

; u64 read_cr3(), void write_cr3(u64)
global read_cr3
read_cr3:
    mov rax, cr3
    ret
global write_cr3
write_cr3:
    mov cr3, rcx
    ret

; void fx_init_image(void* area): a valid default FPU/SSE state for a new thread
global fx_default_state
fx_default_state:
    fxsave [rcx]
    ret
