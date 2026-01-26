// Altair 8800 Disk Boot Loader
// This ROM loads the first sector (137 bytes) from disk into memory at 0x0000 and jumps to it
// Standard Altair disk boot process for CP/M
// Active-low status bits: 0 = active/true, 1 = inactive/false

// Boot loader code located at 0xFF00
0x31, 0x00, 0x00,       // LXI SP, 0000h     ; Set stack pointer
0x3E, 0x00,             // MVI A, 00h        ; Select drive 0
0xD3, 0x08,             // OUT 08h           ; Send to disk select port
0x3E, 0x04,             // MVI A, 04h        ; Head load command
0xD3, 0x09,             // OUT 09h           ; Send to disk control port
0xDB, 0x08,             // IN 08h            ; Read disk status (0xFF0C)
0xE6, 0x04,             // ANI 04h           ; Check head loaded bit (active-low)
0xC2, 0x0C, 0xFF,       // JNZ FF0Ch         ; Loop while bit=1 (not loaded)
0x06, 0x89,             // MVI B, 89h        ; 137 bytes to read (Altair sector size)
0x21, 0x00, 0x00,       // LXI H, 0000h      ; Destination address
0xDB, 0x09,             // IN 09h            ; Get sector position (0xFF19)
0xE6, 0x01,             // ANI 01h           ; Check sector true bit (bit 0)
0xC2, 0x19, 0xFF,       // JNZ FF19h         ; Wait for sector start (bit must be 0)
0xDB, 0x0A,             // IN 0Ah            ; Read data byte (0xFF20)
0x77,                   // MOV M, A          ; Store in memory
0x23,                   // INX H             ; Increment pointer
0x05,                   // DCR B             ; Decrement counter
0xC2, 0x20, 0xFF,       // JNZ FF20h         ; Loop until done
0xC3, 0x00, 0x00,       // JMP 0000h         ; Jump to loaded code
