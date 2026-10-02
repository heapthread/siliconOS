; silicon - stage 1 boot sector (MBR)
; Loads stage 2 (LBA 1..63) to 0x8000 and jumps to it.
; The build patches the kernel sector count at offset 0x1B0.

bits 16
org 0x7C00

STAGE2_SECTORS equ 63

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_boot
    call print

    ; require INT 13h extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc fail
    cmp bx, 0xAA55
    jne fail

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc fail

    mov dl, [boot_drive]
    jmp 0x0000:0x8000

fail:
    mov si, msg_fail
    call print
.hang:
    cli
    hlt
    jmp .hang

; si = zero-terminated string
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

boot_drive: db 0
msg_boot:   db "silicon: loading", 13, 10, 0
msg_fail:   db "silicon: disk read failed", 0

align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0x0000, 0x0800          ; offset, segment -> 0x8000
    dq 1                       ; LBA

    times 0x1B0 - ($ - $$) db 0
kernel_sectors: dd 0           ; patched by build.py
    times 510 - ($ - $$) db 0
    dw 0xAA55
