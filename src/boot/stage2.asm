; silicon - stage 2 loader
; real mode: A20, E820, load kernel to 1 MiB (unreal mode), VBE framebuffer
; then protected mode -> long mode -> jump to kernel at 0x100000
;
; BootInfo @ 0x5000 (see src/kernel/bootinfo.h)
;   +0  u64 fb_addr     +8  u32 fb_width   +12 u32 fb_height
;   +16 u32 fb_pitch    +20 u32 fb_bpp     +24 u32 boot_drive
;   +28 u32 kernel_sectors                 +32 u32 e820_count
;   +40 e820 entries (24 bytes each, max 64)

bits 16
org 0x8000

BOOTINFO      equ 0x5000
E820_BUF      equ 0x5028
VBE_INFO      equ 0x6000
VBE_MODE      equ 0x6200
KSECT_PTR     equ 0x7C00 + 0x1B0
KERNEL_LBA    equ 64
KERNEL_DEST   equ 0x100000
BOUNCE_SEG    equ 0x2000          ; 0x20000
CHUNK         equ 64              ; sectors per read

PML4          equ 0x10000

stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    ; clear BootInfo
    mov di, BOOTINFO
    mov cx, 0x0800
    xor ax, ax
    rep stosw

    mov [BOOTINFO + 24], dl
    mov [drive], dl
    mov eax, [KSECT_PTR]
    mov [BOOTINFO + 28], eax

    mov si, msg_stage2
    call print

    call enable_a20
    call read_e820
    call load_kernel
    call setup_vbe
    jmp enter_long_mode

; ---------------------------------------------------------------- A20
enable_a20:
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE
    out 0x92, al
.done:
    ret

; ---------------------------------------------------------------- E820
read_e820:
    mov di, E820_BUF
    xor ebx, ebx
    xor bp, bp
.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [di + 20], 1
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    inc bp
    add di, 24
    cmp bp, 64
    jae .done
    test ebx, ebx
    jnz .loop
.done:
    mov [BOOTINFO + 32], bp
    ret

; ---------------------------------------------------------------- unreal mode
enter_unreal:
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm
.pm:
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    jmp short .rm
.rm:
    pop es
    pop ds
    sti
    ret

; ---------------------------------------------------------------- kernel load
load_kernel:
    mov eax, [KSECT_PTR]
    mov [remaining], eax
    test eax, eax
    jz .empty
    mov dword [lba], KERNEL_LBA
    mov dword [dest], KERNEL_DEST
.loop:
    mov eax, [remaining]
    test eax, eax
    jz .done
    cmp eax, CHUNK
    jbe .sz
    mov eax, CHUNK
.sz:
    mov [count], eax
    mov [dap.count], ax
    mov eax, [lba]
    mov [dap.lba], eax
    mov word [dap.off], 0
    mov word [dap.seg], BOUNCE_SEG
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc .fail

    call enter_unreal
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov esi, BOUNCE_SEG * 16
    mov edi, [dest]
    mov ecx, [count]
    shl ecx, 7                    ; sectors * 512 / 4
    cld
    a32 rep movsd

    mov eax, [count]
    add [lba], eax
    sub [remaining], eax
    shl eax, 9
    add [dest], eax

    mov al, '.'
    call putc
    jmp .loop
.done:
    mov si, msg_crlf
    call print
    ret
.empty:
    mov si, msg_nokernel
    jmp fatal
.fail:
    mov si, msg_diskerr
    jmp fatal

; ---------------------------------------------------------------- VBE
setup_vbe:
    mov dword [VBE_INFO], 'VBE2'
    mov ax, 0x4F00
    mov di, VBE_INFO
    int 0x10
    cmp ax, 0x004F
    jne .fail

    mov word [want_w], 1024
    mov word [want_h], 768
    call find_mode
    jnc .set
    mov word [want_w], 800
    mov word [want_h], 600
    call find_mode
    jnc .set
    jmp .fail
.set:
    mov ax, 0x4F02
    mov bx, [found_mode]
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne .fail
    ret
.fail:
    mov si, msg_vbe
    jmp fatal

; out: CF clear if found (mode info stored in BootInfo)
find_mode:
    mov si, [VBE_INFO + 14]
    mov ax, [VBE_INFO + 16]
    mov [list_seg], ax
.next:
    push ds
    mov ax, [list_seg]
    mov ds, ax
    mov cx, [si]
    pop ds
    cmp cx, 0xFFFF
    je .none
    add si, 2
    mov [list_off], si
    mov [cur_mode], cx

    mov ax, 0x4F01
    mov di, VBE_MODE
    int 0x10
    mov si, [list_off]
    cmp ax, 0x004F
    jne .next
    mov ax, [VBE_MODE]
    and ax, 0x0091
    cmp ax, 0x0091                ; supported + graphics + LFB
    jne .next
    mov ax, [VBE_MODE + 0x12]
    cmp ax, [want_w]
    jne .next
    mov ax, [VBE_MODE + 0x14]
    cmp ax, [want_h]
    jne .next
    cmp byte [VBE_MODE + 0x19], 32
    jne .next

    mov cx, [cur_mode]
    mov [found_mode], cx
    mov eax, [VBE_MODE + 0x28]
    mov [BOOTINFO + 0], eax
    mov dword [BOOTINFO + 4], 0
    movzx eax, word [VBE_MODE + 0x12]
    mov [BOOTINFO + 8], eax
    movzx eax, word [VBE_MODE + 0x14]
    mov [BOOTINFO + 12], eax
    movzx eax, word [VBE_MODE + 0x10]
    mov [BOOTINFO + 16], eax
    movzx eax, byte [VBE_MODE + 0x19]
    mov [BOOTINFO + 20], eax
    clc
    ret
.none:
    stc
    ret

; ---------------------------------------------------------------- text helpers
putc:
    pusha
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    popa
    ret

print:
    lodsb
    test al, al
    jz .done
    call putc
    jmp print
.done:
    ret

fatal:
    call print
.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------- long mode
enter_long_mode:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm32

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00

    ; zero PML4, PDPT, 4 PDs (0x10000 .. 0x16000)
    mov edi, PML4
    mov ecx, 0x6000 / 4
    xor eax, eax
    rep stosd

    mov dword [PML4], PML4 + 0x1000 + 3
    mov edi, PML4 + 0x1000           ; PDPT
    mov eax, PML4 + 0x2000 + 3
    mov ecx, 4
.pdpt:
    mov [edi], eax
    add eax, 0x1000
    add edi, 8
    loop .pdpt

    ; 2048 x 2 MiB pages identity mapping 0..4 GiB
    mov edi, PML4 + 0x2000
    xor eax, eax
    xor edx, edx
    mov ecx, 2048
.pd:
    mov ebx, eax
    or ebx, 0x83                     ; present | rw | huge
    mov [edi], ebx
    mov [edi + 4], edx
    add eax, 0x200000
    add edi, 8
    loop .pd

    mov eax, PML4
    mov cr3, eax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)   ; PAE, OSFXSR, OSXMMEXCPT
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)                 ; LME
    wrmsr

    mov eax, cr0
    and eax, ~(1 << 2)               ; clear EM
    or eax, (1 << 31) | (1 << 1)     ; PG, MP
    mov cr0, eax

    jmp 0x18:lm64

bits 64
lm64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x7C00
    mov rcx, BOOTINFO                ; MS x64 ABI: first argument
    mov rax, KERNEL_DEST
    jmp rax

; ---------------------------------------------------------------- data
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF            ; 0x08 code32
    dq 0x00CF92000000FFFF            ; 0x10 data32
    dq 0x00AF9A000000FFFF            ; 0x18 code64
    dq 0x00CF92000000FFFF            ; 0x20 data64
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

align 4
dap:
    db 0x10, 0
.count: dw 0
.off:   dw 0
.seg:   dw 0
.lba:   dd 0
        dd 0

drive:      db 0
align 4
remaining:  dd 0
lba:        dd 0
dest:       dd 0
count:      dd 0
want_w:     dw 0
want_h:     dw 0
found_mode: dw 0
cur_mode:   dw 0
list_seg:   dw 0
list_off:   dw 0

msg_stage2:   db "silicon: stage 2", 13, 10, 0
msg_crlf:     db 13, 10, 0
msg_nokernel: db "silicon: no kernel on disk", 0
msg_diskerr:  db "silicon: kernel read error", 0
msg_vbe:      db "silicon: no 1024x768x32 VBE mode", 0
