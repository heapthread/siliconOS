; silicon - interrupt stubs (MS x64 ABI into isr_dispatch(IntFrame*))
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
    and rsp, -16
    sub rsp, 512
    fxsave [rsp]
    mov rcx, rbx
    sub rsp, 32
    call isr_dispatch
    add rsp, 32
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
