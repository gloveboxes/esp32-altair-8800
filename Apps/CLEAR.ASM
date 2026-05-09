        ORG     0100H           ; CP/M base of TPA (transient program area)
        MVI     A, 0            ; Set retro click display to bus mode 
        OUT     80              ; Call port to set display mode 
        MVI     C,09H           ; Print string function
        LXI     D,MESSAGE       ; Point to clear screen & home cursor sequence
        CALL    0005H           ; Call bdos
        RET                     ; To cp/m
MESSAGE:DB      027,'[2J',027,'[1;1H','$'  ; Clear screen then position cursor at row 1, col 1
        END