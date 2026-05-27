        ORG     0100H           ; CP/M base of TPA (transient program area)
        MVI     C,09H           ; BDOS function 9: print '$'-terminated string
        LXI     D,MESSAGE       ; DE -> VT100 escape sequence
        CALL    0005H           ; Call BDOS
        RET                     ; Return to CP/M
;
; VT100 control sequences:
;   ESC [ 2 J  -> erase entire screen
;   ESC [ H    -> move cursor home (row 1, column 1)
;
MESSAGE:DB      1BH,'[2J',1BH,'[H','$'
        END