; silicon - kernel entry (64-bit, MS x64 ABI)
; rcx = BootInfo* from stage 2

bits 64
default rel

extern kmain
extern __bss_start
extern __bss_end
extern __ctors_start
extern __ctors_end

section .entry code

global kentry
kentry:
    cli
    mov r12, rcx                    ; keep BootInfo*

    ; zero .bss (no stack use before this completes)
    lea rdi, [__bss_start]
    lea rcx, [__bss_end]
    sub rcx, rdi
    xor eax, eax
    cld
    rep stosb

    lea rsp, [kernel_stack_top]
    and rsp, -16

    ; global constructors
    lea rbx, [__ctors_start]
.ctors:
    lea rax, [__ctors_end]
    cmp rbx, rax
    jae .ctors_done
    mov rax, [rbx]
    add rbx, 8
    test rax, rax
    jz .ctors
    cmp rax, -1
    je .ctors
    sub rsp, 32
    call rax
    add rsp, 32
    jmp .ctors
.ctors_done:

    mov rcx, r12
    sub rsp, 32
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
kernel_stack:
    resb 256 * 1024
kernel_stack_top:
