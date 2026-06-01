; ============================================================================
; EDIT - Small nano-inspired text editor (8080 MAC assembler port of EDIT.C)
; ============================================================================
; Feature-for-feature port of Apps/EDIT/EDIT.C, built for CP/M on the Altair
; 8800 using VT100/xterm.js control sequences.  Terminal: 80 cols x 30 rows.
;
; Line storage uses a small heap allocator (ALLOC/FREE) so average-length
; lines use far less memory than a fixed 2D array.  Up to 2000 lines.
;
; Keys (identical to the C version):
;   Arrow keys   Move cursor          Ctrl-O   Write file
;   ESC/Ctrl-Q   Exit                 Ctrl-K   Cut current line
;   Ctrl-C       Copy current line    Ctrl-U   Paste (uncut) line
;   Ctrl-F       Find                 Ctrl-N   Find next
;   Ctrl-W       Help                 Ctrl-R/V Page up / Page down
;   Ctrl-T/B     Top / Bottom         Backspace/Delete  delete
;   Enter        Split line           Tab      toggle INS/OVR
;
; Build (CP/M):
;   mac edit        ->  EDIT.HEX (plus .PRN/.SYM)
;   load edit       ->  EDIT.COM
;
; MAC constraints: symbols unique in first 6 chars; ports in decimal.
; ============================================================================

; -- CP/M / BDOS ------------------------------------------------------------
BDOS    EQU     5               ; BDOS entry
FDIRIO  EQU     6               ; direct console I/O (E=0FFH read)
FOPENF  EQU     15              ; open file
FCLOSF  EQU     16              ; close file
FDELF   EQU     19              ; delete file
FREADF  EQU     20              ; read sequential
FWRITF  EQU     21              ; write sequential
FMAKEF  EQU     22              ; make file
FRENF   EQU     23              ; rename file
FDMAF   EQU     26              ; set DMA address
DEFFCB  EQU     5CH             ; default FCB parsed by the CCP

; -- editor geometry --------------------------------------------------------
COLS    EQU     80
TEXTY   EQU     2
TEXTN   EQU     27
STATY   EQU     29
HELPY   EQU     30
GUTW    EQU     4
TXTW    EQU     76              ; COLS-GUTW
MAXLN   EQU     2000
MAXCL   EQU     126
LOADRS  EQU     2048            ; keep this much heap free after loading
LOADMN  EQU     LOADRS+MAXCL+4  ; reserve plus one worst-case line block

; -- key / control codes ----------------------------------------------------
ESC     EQU     27
CTLF    EQU     6
CTLN    EQU     14
CTLW    EQU     23
CTLK    EQU     11
CTLO    EQU     15
CTLU    EQU     21
CTLC    EQU     3
CTLQ    EQU     17
BKSP    EQU     8
TABCH   EQU     9
DELCH7  EQU     127
CPMEOF  EQU     26
CRCH    EQU     13
LFCH    EQU     10
KUP     EQU     5
KDN     EQU     24
KRT     EQU     4
KLT     EQU     19
KDEL    EQU     7
KPGU    EQU     18
KPGD    EQU     22
KTOP    EQU     20
KBOT    EQU     2
KINS    EQU     9

; -- macro: invoke a BDOS function -----------------------------------------
DOS     MACRO   FUNC
        MVI     C,FUNC
        CALL    BDOS
        ENDM

        ORG     0100H

; ===========================================================================
; Startup
; ===========================================================================
START:  LXI     H,0             ; save CCP stack pointer
        DAD     SP
        SHLD    OLDSP
        LXI     SP,STKTOP       ; our own stack (below the heap)

        CALL    INIT            ; zero line table, resolve CONOUT, set heap

; getfcb(name): build filename from default FCB; usage+exit if none
        CALL    GETFCB
        ORA     A
        JNZ     ST1
        CALL    USAGE
        JMP     EXIT
ST1:
        CALL    LOADF           ; load file (renames original to .BAK)
        CALL    CLRS
        XRA     A
        STA     QUIT

MAINLP: LDA     QUIT
        ORA     A
        JNZ     MEND
        CALL    REDRW
        CALL    KEYWT           ; keyrd == keywt
        CALL    DOBIN
        JMP     MAINLP
MEND:
        LDA     EVSAV           ; if not evsav unbak()
        ORA     A
        JNZ     ME1
        CALL    UNBAK
ME1:
        CALL    CLRS
        MVI     A,CRCH
        CALL    CHOUT
        MVI     A,LFCH
        CALL    CHOUT
EXIT:
        LHLD    OLDSP           ; restore CCP stack and return
        SPHL
        RET

; ===========================================================================
; INIT - one-time setup
; ===========================================================================
INIT:
; resolve BIOS CONOUT vector (WBOOT+9)
        LHLD    1               ; HL -> WBOOT
        LXI     D,9
        DAD     D               ; HL -> CONOUT jump
        SHLD    CONOTA
; heap: BRK = HEAPB, FREEL = 0, HEAPND = mem[6] - 256
        LXI     H,HEAPB
        SHLD    BRKP
        LXI     H,0
        SHLD    FREEL
        LHLD    6               ; BDOS base = top of TPA
        LXI     D,0FF00H        ; -256
        DAD     D
        SHLD    HEAPND
        LXI     H,0
        SHLD    USED
; zero the LNPTR array (MAXLN*2 bytes) so zerob()'s xfree sees nulls
        LXI     H,LNPTR
        LXI     B,MAXLN*2
INZ1:   MVI     M,0
        INX     H
        DCX     B
        MOV     A,B
        ORA     C
        JNZ     INZ1
        RET

; ===========================================================================
; Low-level console
; ===========================================================================
; CHOUT - write char in A via BIOS CONOUT (char in C)
CHOUT:
        PUSH    H
        PUSH    D
        PUSH    B
        PUSH    PSW
        MOV     C,A
        CALL    CONOUV
        POP     PSW
        POP     B
        POP     D
        POP     H
        RET
CONOUV: LHLD    CONOTA
        PCHL                    ; jump to CONOUT, it RETs to CHOUT

; KEYWT - busy-wait for a key, return raw byte in A
KEYWT:
        PUSH    H
        PUSH    D
        PUSH    B
KW1:    MVI     E,0FFH
        DOS     FDIRIO
        ORA     A
        JZ      KW1
        POP     B
        POP     D
        POP     H
        RET

; CPUT - print NUL-terminated string at HL
CPUT:
CPT1:   MOV     A,M
        ORA     A
        RZ
        CALL    CHOUT
        INX     H
        JMP     CPT1

; PESC - emit ESC then the NUL-terminated string at HL
PESC:
        MVI     A,ESC
        CALL    CHOUT
        JMP     CPUT

; ===========================================================================
; Number / string helpers
; ===========================================================================
; DIV10 - HL = HL/10, remainder in A.  Preserves BC and DE.
DIV10:
        PUSH    D
        PUSH    B
        LXI     B,0
        MVI     E,16
        XRA     A
DV1:    DAD     H
        RAL
        PUSH    PSW
        MOV     A,C
        RAL
        MOV     C,A
        MOV     A,B
        RAL
        MOV     B,A
        POP     PSW
        CPI     10
        JC      DV2
        SUI     10
        INR     C
DV2:    DCR     E
        JNZ     DV1
        MOV     H,B
        MOV     L,C
        POP     B
        POP     D
        RET

; NUMP - print unsigned decimal value in HL
NUMP:
        MOV     A,H
        ORA     L
        JNZ     NMP1
        MVI     A,'0'
        JMP     CHOUT
NMP1:   MVI     C,0             ; digit count
NMP2:   MOV     A,H
        ORA     L
        JZ      NMP3
        CALL    DIV10
        ADI     '0'
        PUSH    PSW
        INR     C
        JMP     NMP2
NMP3:   MOV     A,C
        ORA     A
        RZ
NMP4:   POP     PSW
        CALL    CHOUT
        DCR     C
        JNZ     NMP4
        RET

; NDIG - count decimal digits of HL, returns count in A
NDIG:
        MVI     B,1
ND1:    MOV     A,H
        ORA     A
        JNZ     ND2
        MOV     A,L
        CPI     10
        JC      ND3
ND2:    CALL    DIV10
        INR     B
        JMP     ND1
ND3:    MOV     A,B
        RET

; STRLEN - DE=string, returns length in HL (DE preserved)
STRLEN:
        PUSH    D
        LXI     H,0
SLN1:   LDAX    D
        ORA     A
        JZ      SLN2
        INX     H
        INX     D
        JMP     SLN1
SLN2:   POP     D
        RET

; SCPY - strcpy: DE=dst, HL=src
SCPY:
        PUSH    D
SCP1:   MOV     A,M
        STAX    D
        ORA     A
        JZ      SCP2
        INX     H
        INX     D
        JMP     SCP1
SCP2:   POP     D
        RET

; SCAT - strcat: DE=dst, HL=src
SCAT:
SCT1:   LDAX    D
        ORA     A
        JZ      SCT2
        INX     D
        JMP     SCT1
SCT2:   JMP     SCPY            ; copy src(HL)->dst(DE), preserves D for return

; SCMP - signed compare HL vs DE.
;        Returns A=0FFH if HL<DE, A=0 if equal, A=1 if HL>DE.
SCMP:
        MOV     A,H
        XRA     D
        JM      SCDS            ; signs differ
        MOV     A,H
        CMP     D
        JC      SCLT
        JNZ     SCGT
        MOV     A,L
        CMP     E
        JC      SCLT
        JNZ     SCGT
        XRA     A
        RET
SCLT:   MVI     A,0FFH
        RET
SCGT:   MVI     A,1
        RET
SCDS:   MOV     A,H
        ORA     A
        JM      SCLT
        JMP     SCGT

; UCMP - unsigned compare HL vs DE.
;        Returns A=0FFH if HL<DE, A=0 if equal, A=1 if HL>DE.
UCMP:
        MOV     A,H
        CMP     D
        JC      SCLT
        JNZ     SCGT
        MOV     A,L
        CMP     E
        JC      SCLT
        JNZ     SCGT
        XRA     A
        RET

; ===========================================================================
; Line table access
; ===========================================================================
; GETLN - HL=row -> HL = ln[row]
GETLN:
        DAD     H
        LXI     D,LNPTR
        DAD     D
        MOV     E,M
        INX     H
        MOV     D,M
        XCHG
        RET

; PUTLN - HL=row, DE=ptr -> ln[row]=ptr
PUTLN:
        PUSH    D
        DAD     H
        LXI     D,LNPTR
        DAD     D
        POP     D
        MOV     M,E
        INX     H
        MOV     M,D
        RET

; CURLN - HL = ln[crow]
CURLN:
        LHLD    CROW
        CALL    GETLN
        RET

; ===========================================================================
; Heap allocator
; ===========================================================================
; ALLOC - HL = payload bytes -> HL = pointer (payload) or 0
ALLOC:
        LXI     D,2
        DAD     D
; freed blocks store a 2-byte next pointer in the payload
        MOV     A,H
        ORA     A
        JNZ     ALSZOK
        MOV     A,L
        CPI     4
        JNC     ALSZOK
        LXI     H,4
ALSZOK:
        SHLD    ANEED
        LXI     H,FREEL
        SHLD    APREV
        LHLD    FREEL
        SHLD    ACUR
ALOOP:
        LHLD    ACUR
        MOV     A,H
        ORA     L
        JZ      ALBUMP
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = cur.size
        LHLD    ANEED           ; HL = need
        MOV     A,E
        SUB     L
        MOV     A,D
        SBB     H
        JNC     ALFIT           ; size >= need
        LHLD    ACUR
        INX     H
        INX     H               ; &cur.next
        SHLD    APREV
        MOV     E,M
        INX     H
        MOV     D,M
        XCHG
        SHLD    ACUR
        JMP     ALOOP
ALFIT:
        LHLD    ACUR
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = size
        LHLD    ANEED
        MOV     A,E
        SUB     L
        MOV     C,A
        MOV     A,D
        SBB     H
        MOV     B,A             ; BC = size - need
        MOV     A,B
        ORA     A
        JNZ     ALSPL
        MOV     A,C
        CPI     4
        JC      ALWHOL
ALSPL:
        LHLD    ACUR
        XCHG
        LHLD    ANEED
        DAD     D
        SHLD    ANEW            ; ANEW = ACUR+need
        MOV     M,C
        INX     H
        MOV     M,B             ; ANEW.size = size-need
        LHLD    ACUR
        INX     H
        INX     H
        MOV     E,M
        INX     H
        MOV     D,M
        XCHG
        SHLD    ANEXT           ; ANEXT = cur.next
        LHLD    ANEXT
        XCHG
        LHLD    ANEW
        INX     H
        INX     H
        MOV     M,E
        INX     H
        MOV     M,D             ; ANEW.next = ANEXT
        LHLD    ANEED
        XCHG
        LHLD    ACUR
        MOV     M,E
        INX     H
        MOV     M,D             ; cur.size = need
        LHLD    ANEW
        XCHG
        LHLD    APREV
        MOV     M,E
        INX     H
        MOV     M,D             ; *APREV = ANEW
        LHLD    ACUR
        INX     H
        INX     H
        RET
ALWHOL:
        LHLD    ACUR
        INX     H
        INX     H
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = cur.next
        LHLD    APREV
        MOV     M,E
        INX     H
        MOV     M,D             ; *APREV = cur.next
        LHLD    ACUR
        INX     H
        INX     H
        RET
ALBUMP:
        LHLD    BRKP
        XCHG
        LHLD    ANEED
        DAD     D               ; HL = BRK+need
        XCHG                    ; DE = BRK+need
        LHLD    HEAPND
        MOV     A,L
        SUB     E
        MOV     A,H
        SBB     D
        JC      ALFAIL          ; HEAPND < BRK+need
        LHLD    BRKP
        SHLD    ATMP            ; blk = BRK
        XCHG
        LHLD    ANEED
        DAD     D
        SHLD    BRKP            ; BRK += need
        LHLD    ANEED
        XCHG
        LHLD    ATMP
        MOV     M,E
        INX     H
        MOV     M,D             ; blk.size = need
        LHLD    ATMP
        INX     H
        INX     H
        RET
ALFAIL:
        LXI     H,0
        RET

; FREE - HL = payload pointer; push block onto the free list
FREE:
        MOV     A,H
        ORA     L
        RZ
        DCX     H
        DCX     H
        SHLD    ATMP            ; block
        LHLD    FREEL
        XCHG                    ; DE = old head
        LHLD    ATMP
        INX     H
        INX     H
        MOV     M,E
        INX     H
        MOV     M,D             ; block.next = old head
        LHLD    ATMP
        SHLD    FREEL
        RET

; XALLOC - DE=src string -> HL=ptr (copy of src) or 0; tracks USED
XALLOC:
        CALL    STRLEN          ; HL=len (DE preserved)
        SHLD    XLEN
        INX     H               ; len+1
        PUSH    D
        CALL    ALLOC
        POP     D
        MOV     A,H
        ORA     L
        RZ                      ; failed -> HL=0
        XCHG                    ; HL=src, DE=dst
        PUSH    D
        CALL    SCPY
        POP     H               ; HL = ptr
        PUSH    H
        LHLD    XLEN
        LXI     D,3
        DAD     D
        XCHG
        LHLD    USED
        DAD     D
        SHLD    USED
        POP     H
        RET

; XFREE - HL=ptr; tracks USED then frees
XFREE:
        MOV     A,H
        ORA     L
        RZ
        PUSH    H
        XCHG
        CALL    STRLEN          ; HL=len, DE=ptr preserved
        LXI     D,3
        DAD     D               ; HL=len+3
        XCHG
        LHLD    USED
        MOV     A,L
        SUB     E
        MOV     L,A
        MOV     A,H
        SBB     D
        MOV     H,A
        SHLD    USED
        POP     H
        CALL    FREE
        RET

; ===========================================================================
; LNSET - HL=row, DE=src.  ln[row]=copy(src). Returns HL=0 ok, 0FFFFH fail.
; ===========================================================================
LNSET:
        SHLD    LSROW
        PUSH    D
        CALL    XALLOC
        POP     D
        MOV     A,H
        ORA     L
        JNZ     LNS1
        LXI     H,MOOM
        CALL    MSG
        LXI     H,0FFFFH
        RET
LNS1:   SHLD    LSPTR
        LHLD    LSROW
        CALL    GETLN
        MOV     A,H
        ORA     L
        JZ      LNS2
        CALL    XFREE
LNS2:   LHLD    LSPTR
        XCHG
        LHLD    LSROW
        CALL    PUTLN
        LXI     H,0
        RET

; ===========================================================================
; MSG - HL=src.  Set status message if changed.
; ===========================================================================
MSG:
        SHLD    MSGSRC
        LXI     D,MESG
        MVI     B,68
MSC1:   LDAX    D
        CMP     M
        JNZ     MSCD
        MOV     A,M
        ORA     A
        RZ
        INX     H
        INX     D
        DCR     B
        JNZ     MSC1
        RET
MSCD:   LHLD    MSGSRC
        LXI     D,MESG
        MVI     B,68
MSCP:   MOV     A,M
        ORA     A
        JZ      MSCE
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     MSCP
MSCE:   XRA     A
        STAX    D
        MVI     A,1
        STA     DSTAT
        RET

; SETDIR - A = v (0/1)
SETDIR:
        MOV     B,A
        LDA     DIRT
        CMP     B
        JZ      SDR1
        MVI     A,1
        STA     DTITLE
SDR1:   MOV     A,B
        STA     DIRT
        RET

; ===========================================================================
; ZEROB - reset the buffer to a single empty line
; ===========================================================================
ZEROB:
        LXI     H,0
        SHLD    ZBI
ZB1:    LHLD    ZBI
        LXI     D,MAXLN
        CALL    UCMP
        CPI     0FFH
        JNZ     ZB2             ; i >= MAXLN -> done
        LHLD    ZBI
        CALL    GETLN
        MOV     A,H
        ORA     L
        JZ      ZB1B
        CALL    XFREE
ZB1B:   LHLD    ZBI
        LXI     D,0
        CALL    PUTLN
        LHLD    ZBI
        INX     H
        SHLD    ZBI
        JMP     ZB1
ZB2:    XRA     A
        STA     EBUF
        LXI     H,0
        SHLD    USED
        LXI     D,MEMPTY
        CALL    XALLOC
        XCHG
        LXI     H,0
        CALL    PUTLN           ; ln[0]=xalloc("")
        LXI     H,1
        SHLD    LCNT
        LXI     H,0
        SHLD    CROW
        SHLD    CCOL
        SHLD    TOP
        SHLD    LFT
        LXI     H,1
        SHLD    RALL
        LXI     H,0FFFFH        ; -1
        SHLD    DLIN
        SHLD    SROW
        SHLD    SCOL
        SHLD    SLCNT
        XRA     A
        STA     DIRT
        STA     OVRM
        STA     EVSAV
        STA     EFLG
        MVI     A,1
        STA     DTITLE
        STA     DSTAT
        STA     DHELP
        XRA     A
        STA     CUTB
        STA     SBUF
        RET

; ===========================================================================
; VT100 helpers
; ===========================================================================
; CURMV - B=row, C=col (one based)
CURMV:
        MOV     A,B
        STA     CMROW
        MOV     A,C
        STA     CMCOL
        MVI     A,ESC
        CALL    CHOUT
        MVI     A,'['
        CALL    CHOUT
        LDA     CMROW
        MOV     L,A
        MVI     H,0
        CALL    NUMP
        MVI     A,';'
        CALL    CHOUT
        LDA     CMCOL
        MOV     L,A
        MVI     H,0
        CALL    NUMP
        MVI     A,'H'
        JMP     CHOUT

CLRS:
        LXI     H,E2J
        CALL    PESC
        MVI     B,1
        MVI     C,1
        JMP     CURMV
EOL:
        LXI     H,EK
        JMP     PESC
INVON:
        LXI     H,E7M
        JMP     PESC
INVOF:
        LXI     H,E0M
        JMP     PESC
SHWCR:
        LXI     H,ECSH
        JMP     PESC
HIDCR:
        LXI     H,ECSL
        JMP     PESC

; LNPAD - HL=n: print line number right-justified in 4-char gutter
LNPAD:
        PUSH    H
        LXI     D,10
        CALL    UCMP
        CPI     0FFH
        JNZ     LP1             ; n>=10
        LXI     H,TSP2
        CALL    CPUT
        JMP     LP3
LP1:    POP     H
        PUSH    H
        LXI     D,100
        CALL    UCMP
        CPI     0FFH
        JNZ     LP3             ; n>=100
        MVI     A,' '
        CALL    CHOUT
LP3:    POP     H
        CALL    NUMP
        MVI     A,' '
        JMP     CHOUT

; ===========================================================================
; Free-memory display
; ===========================================================================
; FREMEM - HL = approximate free heap bytes
FREMEM:
        LHLD    HEAPND
        LXI     D,HEAPB
        MOV     A,L
        SUB     E
        MOV     C,A
        MOV     A,H
        SBB     D
        MOV     B,A             ; BC = total
        LHLD    USED
        MOV     A,L
        SUB     C
        MOV     A,H
        SBB     B
        JC      FRM1            ; used < total
        LXI     H,0
        RET
FRM1:   LHLD    USED
        XCHG
        MOV     A,C
        SUB     E
        MOV     L,A
        MOV     A,B
        SBB     D
        MOV     H,A
        RET

; LDROOM - A=1 if LOADF may accept another full line and keep LOADRS free
LDROOM:
        CALL    FREMEM
        LXI     D,LOADMN
        CALL    UCMP
        CPI     0FFH
        JZ      LDRNO
        MVI     A,1
        RET
LDRNO:  XRA     A
        RET

; SHR10 - HL = HL >> 10 (unsigned)
SHR10:
        MVI     B,10
SR1:    ORA     A
        MOV     A,H
        RAR
        MOV     H,A
        MOV     A,L
        RAR
        MOV     L,A
        DCR     B
        JNZ     SR1
        RET

; FMSHOW - print " Free <n>K/B"
FMSHOW:
        LXI     H,TFREE
        CALL    CPUT
        CALL    FREMEM
        SHLD    FMVAL
        LXI     D,1024
        CALL    UCMP
        CPI     0FFH
        JZ      FM1             ; f < 1024
        LHLD    FMVAL
        CALL    SHR10
        CALL    NUMP
        LXI     H,TK
        JMP     CPUT
FM1:    LHLD    FMVAL
        CALL    NUMP
        LXI     H,TB
        JMP     CPUT

; ===========================================================================
; FIXCUR - clamp cursor and scroll state
; ===========================================================================
FIXCUR:
        LHLD    CROW
        LXI     D,0
        CALL    SCMP
        CPI     0FFH
        JNZ     FX1
        LXI     H,0
        SHLD    CROW
FX1:    LHLD    CROW
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP
        CPI     0FFH
        JZ      FX2
        LHLD    LCNT
        DCX     H
        SHLD    CROW
FX2:    CALL    CURLN
        XCHG
        CALL    STRLEN
        SHLD    FXLEN
        LHLD    CCOL
        LXI     D,0
        CALL    SCMP
        CPI     0FFH
        JNZ     FX3
        LXI     H,0
        SHLD    CCOL
FX3:    LHLD    CCOL
        XCHG
        LHLD    FXLEN
        XCHG
        CALL    SCMP
        CPI     1
        JNZ     FX4
        LHLD    FXLEN
        SHLD    CCOL
FX4:    LHLD    CROW
        XCHG
        LHLD    TOP
        XCHG
        CALL    SCMP
        CPI     0FFH
        JNZ     FX5
        LHLD    CROW
        SHLD    TOP
FX5:    LHLD    TOP
        LXI     D,TEXTN
        DAD     D
        XCHG
        LHLD    CROW
        CALL    SCMP
        CPI     0FFH
        JZ      FX6
        LHLD    CROW
        LXI     D,0FFE6H        ; -26
        DAD     D
        SHLD    TOP
FX6:    LHLD    TOP
        LXI     D,0
        CALL    SCMP
        CPI     0FFH
        JNZ     FX7
        LXI     H,0
        SHLD    TOP
FX7:    LHLD    CCOL
        XCHG
        LHLD    LFT
        XCHG
        CALL    SCMP
        CPI     0FFH
        JNZ     FX8
        LHLD    CCOL
        SHLD    LFT
FX8:    LHLD    LFT
        LXI     D,TXTW
        DAD     D
        XCHG
        LHLD    CCOL
        CALL    SCMP
        CPI     0FFH
        JZ      FX9
        LHLD    CCOL
        LXI     D,0FFB5H        ; -75
        DAD     D
        SHLD    LFT
FX9:    LHLD    LFT
        LXI     D,0
        CALL    SCMP
        CPI     0FFH
        JNZ     FXA
        LXI     H,0
        SHLD    LFT
FXA:    RET

; ===========================================================================
; TTLBAR - top title bar
; ===========================================================================
TTLBAR:
        MVI     B,1
        MVI     C,1
        CALL    CURMV
        CALL    INVON
        LXI     H,TTITLE
        CALL    CPUT
        LDA     FNAME
        ORA     A
        JZ      TT1
        LXI     H,FNAME
        CALL    CPUT
        JMP     TT2
TT1:    LXI     H,TNONAM
        CALL    CPUT
TT2:    LDA     DIRT
        ORA     A
        JZ      TT3
        LXI     H,TSTAR
        CALL    CPUT
TT3:    CALL    EOL
        JMP     INVOF

; ===========================================================================
; DRAWLN - HL=n: draw visible row n
; ===========================================================================
DRAWLN:
        SHLD    DLN
        LHLD    TOP
        XCHG
        LHLD    DLN
        DAD     D
        SHLD    DLL             ; l = top + n
        LHLD    DLN
        MOV     A,L
        ADI     TEXTY
        MOV     B,A
        MVI     C,1
        CALL    CURMV
        LHLD    DLL
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP            ; l vs lcnt
        CPI     0FFH
        JZ      DRW1            ; l < lcnt
        JMP     EOL             ; blank row
DRW1:   LHLD    DLL
        INX     H
        CALL    LNPAD
        LHLD    DLL
        CALL    GETLN
        XCHG
        LHLD    LFT
        DAD     D
        SHLD    DLP             ; p = ln[l] + lft
        LXI     H,0
        SHLD    DLX             ; x = 0
DRW2:   LHLD    DLP
        MOV     A,M
        ORA     A
        JZ      DRW9            ; end of line
        LHLD    DLX
        LXI     D,TXTW
        CALL    UCMP
        CPI     0FFH
        JNZ     DRW9            ; x >= TXTW
        LHLD    DLP
        MOV     A,M
        CPI     TABCH
        JNZ     DRW5
DRW3:   MVI     A,' '
        CALL    CHOUT
        LHLD    DLX
        INX     H
        SHLD    DLX
        MOV     A,L
        ANI     7
        JZ      DRW8            ; x%8==0
        LHLD    DLX
        LXI     D,TXTW
        CALL    UCMP
        CPI     0FFH
        JZ      DRW3            ; x < TXTW -> continue tab
        JMP     DRW8
DRW5:   LHLD    DLP
        MOV     A,M
        CALL    CHOUT
        LHLD    DLX
        INX     H
        SHLD    DLX
DRW8:   LHLD    DLP
        INX     H
        SHLD    DLP
        JMP     DRW2
DRW9:   JMP     EOL

; ===========================================================================
; STATLN - full status bar
; ===========================================================================
STATLN:
        MVI     B,STATY
        MVI     C,1
        CALL    CURMV
        CALL    INVON
        LXI     H,TSP1
        CALL    CPUT
        LXI     H,MESG
        CALL    CPUT
        LXI     H,TLNL
        CALL    CPUT
        LHLD    CROW
        INX     H
        CALL    NUMP
        LXI     H,TSLSH
        CALL    CPUT
        LHLD    LCNT
        CALL    NUMP
        LXI     H,TCOLL
        CALL    CPUT
        LHLD    CCOL
        INX     H
        CALL    NUMP
        CALL    STMODE
        CALL    FMSHOW
        CALL    EOL
        JMP     INVOF

; STMODE - print " OVR" or " INS"
STMODE:
        LDA     OVRM
        ORA     A
        JZ      STM1
        LXI     H,TOVR
        JMP     CPUT
STM1:   LXI     H,TINS
        JMP     CPUT

; STATCO - update just the column portion of the status bar
STATCO:
        LXI     D,MESG
        CALL    STRLEN          ; HL=strlen(mesg)
        LXI     D,13
        DAD     D
        PUSH    H
        LHLD    CROW
        INX     H
        CALL    NDIG
        POP     H
        MOV     E,A
        MVI     D,0
        DAD     D
        PUSH    H
        LHLD    LCNT
        CALL    NDIG
        POP     H
        MOV     E,A
        MVI     D,0
        DAD     D
        MOV     A,L
        MOV     C,A
        MVI     B,STATY
        CALL    CURMV
        CALL    INVON
        LHLD    CCOL
        INX     H
        CALL    NUMP
        CALL    STMODE
        CALL    FMSHOW
        CALL    EOL
        JMP     INVOF

; HELPBR - shortcut bar
HELPBR:
        MVI     B,HELPY
        MVI     C,1
        CALL    CURMV
        LXI     H,THELP
        CALL    CPUT
        JMP     EOL

; ===========================================================================
; REDRW - repaint
; ===========================================================================
REDRW:
        LHLD    TOP
        SHLD    RDOT
        LHLD    LFT
        SHLD    RDOL
        CALL    FIXCUR
; sm
        XRA     A
        STA     RDSM
        LDA     DSTAT
        ORA     A
        JNZ     RDS1
        LHLD    CROW
        XCHG
        LHLD    SROW
        CALL    UCMP
        ORA     A
        JNZ     RDS1            ; crow <> srow
        LHLD    LCNT
        XCHG
        LHLD    SLCNT
        CALL    UCMP
        ORA     A
        JNZ     RDS1            ; lcnt <> slcnt
        LHLD    CCOL
        XCHG
        LHLD    SCOL
        CALL    UCMP
        ORA     A
        JZ      RDS2
        MVI     A,2
        STA     RDSM
        JMP     RDS2
RDS1:   MVI     A,1
        STA     RDSM
RDS2:
; full vs partial
        LDA     RALL
        ORA     A
        JNZ     RDFULL
        LHLD    TOP
        XCHG
        LHLD    RDOT
        CALL    UCMP
        ORA     A
        JNZ     RDFULL          ; top <> ot
        LHLD    LFT
        XCHG
        LHLD    RDOL
        CALL    UCMP
        ORA     A
        JNZ     RDFULL          ; lft <> ol
        JMP     RDPART
RDFULL:
        CALL    HIDCR
        LDA     DTITLE
        ORA     A
        JZ      RDF1
        CALL    TTLBAR
        XRA     A
        STA     DTITLE
RDF1:   LXI     H,0
        SHLD    RDI
RDF2:   LHLD    RDI
        LXI     D,TEXTN
        CALL    UCMP
        CPI     0FFH
        JNZ     RDF3            ; i >= TEXTN
        LHLD    RDI
        CALL    DRAWLN
        LHLD    RDI
        INX     H
        SHLD    RDI
        JMP     RDF2
RDF3:   XRA     A
        STA     RALL
        LXI     H,0FFFFH        ; -1
        SHLD    DLIN
        JMP     RDSTAT
RDPART:
; if dlin in view, redraw it
        LHLD    DLIN
        XCHG
        LHLD    TOP
        XCHG
        CALL    SCMP            ; dlin vs top
        CPI     0FFH
        JZ      RDP2            ; dlin < top
        LHLD    TOP
        LXI     D,TEXTN
        DAD     D
        XCHG
        LHLD    DLIN
        CALL    SCMP            ; dlin vs top+TEXTN
        CPI     0FFH
        JNZ     RDP2            ; dlin >= top+TEXTN
        CALL    HIDCR
        LHLD    DLIN
        XCHG
        LHLD    TOP
        MOV     A,E
        SUB     L
        MOV     L,A
        MVI     H,0             ; dlin - top (small, fits)
        CALL    DRAWLN
RDP2:   LXI     H,0FFFFH        ; -1
        SHLD    DLIN
        LDA     DTITLE
        ORA     A
        JZ      RDSTAT
        CALL    TTLBAR
        XRA     A
        STA     DTITLE
RDSTAT:
        LDA     RDSM
        CPI     1
        JNZ     RDS3
        CALL    STATLN
        XRA     A
        STA     DSTAT
        JMP     RDHLP
RDS3:   CPI     2
        JNZ     RDHLP
        CALL    STATCO
RDHLP:
        LDA     DHELP
        ORA     A
        JZ      RDCUR
        CALL    HELPBR
        XRA     A
        STA     DHELP
RDCUR:
        LHLD    CROW
        SHLD    SROW
        LHLD    CCOL
        SHLD    SCOL
        LHLD    LCNT
        SHLD    SLCNT
; curmv(TEXTY+crow-top, ccol-lft+1+GUTW)
        LHLD    CROW
        XCHG
        LHLD    TOP
        MOV     A,E
        SUB     L
        ADI     TEXTY
        MOV     B,A             ; row
        LHLD    CCOL
        XCHG
        LHLD    LFT
        MOV     A,E
        SUB     L
        ADI     1+GUTW
        MOV     C,A             ; col
        CALL    CURMV
        JMP     SHWCR

; ===========================================================================
; INSROW - HL=r: insert empty row before r
; ===========================================================================
INSROW:
        SHLD    IRR
        LHLD    LCNT
        LXI     D,MAXLN
        CALL    SCMP
        CPI     0FFH
        JZ      IR1
        LXI     H,0FFFFH
        RET
IR1:    LXI     D,MEMPTY
        CALL    XALLOC
        MOV     A,H
        ORA     L
        JNZ     IR2
        LXI     H,0FFFFH
        RET
IR2:    SHLD    IRPT
        LHLD    LCNT
        SHLD    IRI
IRL:    LHLD    IRI
        XCHG
        LHLD    IRR
        XCHG
        CALL    SCMP            ; i vs r
        CPI     1
        JNZ     IRDON           ; not i>r
        LHLD    IRI
        DCX     H
        CALL    GETLN
        XCHG                    ; DE = ln[i-1]
        LHLD    IRI
        CALL    PUTLN           ; ln[i] = ln[i-1]
        LHLD    IRI
        DCX     H
        SHLD    IRI
        JMP     IRL
IRDON:  LHLD    IRPT
        XCHG
        LHLD    IRR
        CALL    PUTLN           ; ln[r] = p
        LHLD    LCNT
        INX     H
        SHLD    LCNT
        LXI     H,0
        RET

; ===========================================================================
; DELROW - HL=r: delete row r
; ===========================================================================
DELROW:
        SHLD    DRR
        LHLD    LCNT
        LXI     D,1
        CALL    SCMP
        CPI     1
        JZ      DRMANY          ; lcnt > 1
; lcnt <= 1: reset ln[0] to empty
        LXI     D,MEMPTY
        CALL    XALLOC
        MOV     A,H
        ORA     L
        JNZ     DR0
        LXI     H,MOOM
        CALL    MSG
        LXI     H,0FFFFH
        RET
DR0:    SHLD    DRP
        LXI     H,0
        CALL    GETLN
        MOV     A,H
        ORA     L
        JZ      DR0B
        LXI     H,0
        CALL    GETLN
        CALL    XFREE
DR0B:   LHLD    DRP
        XCHG
        LXI     H,0
        CALL    PUTLN
        LXI     H,0
        SHLD    CCOL
        LXI     H,0
        RET
DRMANY:
        LHLD    DRR
        CALL    GETLN
        CALL    XFREE
        LHLD    DRR
        SHLD    DRI
DRL:    LHLD    LCNT
        DCX     H
        XCHG                    ; DE = lcnt-1
        LHLD    DRI
        CALL    SCMP            ; i vs lcnt-1
        CPI     0FFH
        JNZ     DRDON           ; i >= lcnt-1
        LHLD    DRI
        INX     H
        CALL    GETLN
        XCHG                    ; DE = ln[i+1]
        LHLD    DRI
        CALL    PUTLN           ; ln[i] = ln[i+1]
        LHLD    DRI
        INX     H
        SHLD    DRI
        JMP     DRL
DRDON:  LHLD    LCNT
        DCX     H
        XCHG
        LXI     H,0
        XCHG
        CALL    PUTLN           ; ln[lcnt-1] = 0
        LHLD    LCNT
        DCX     H
        SHLD    LCNT
        LHLD    CROW
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP            ; crow vs lcnt
        CPI     0FFH
        JZ      DRRET           ; crow < lcnt
        LHLD    LCNT
        DCX     H
        SHLD    CROW
DRRET:  LXI     H,0
        RET

; ===========================================================================
; INSCH - A=char: insert printable char at cursor
; ===========================================================================
INSCH:
        STA     CINS
        CALL    CURLN
        XCHG
        CALL    STRLEN
        SHLD    ICLEN
        LHLD    ICLEN
        LXI     D,MAXCL
        CALL    SCMP
        CPI     0FFH
        JZ      IC1             ; len < MAXCL
        LXI     H,MLLONG
        CALL    MSG
        LXI     H,0FFFFH
        RET
IC1:    CALL    CURLN
        LXI     D,EBUF
        CALL    SCPY
        LHLD    ICLEN
        XCHG
        LXI     H,EBUF
        DAD     D
        SHLD    ICP             ; ICP = EBUF+len
        LHLD    ICLEN
        XCHG
        LHLD    CCOL
        MOV     A,E
        SUB     L
        MOV     C,A
        MOV     A,D
        SBB     H
        MOV     B,A             ; BC = len-ccol
        INX     B               ; count = len-ccol+1
ICSH:   LHLD    ICP
        MOV     A,M
        INX     H
        MOV     M,A             ; *(p+1)=*p
        LHLD    ICP
        DCX     H
        SHLD    ICP
        DCX     B
        MOV     A,B
        ORA     C
        JNZ     ICSH
        LHLD    CCOL
        XCHG
        LXI     H,EBUF
        DAD     D
        LDA     CINS
        MOV     M,A             ; ebuf[ccol]=c
        LHLD    CROW
        LXI     D,EBUF
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      IC2
        LXI     H,0FFFFH
        RET
IC2:    LHLD    CCOL
        INX     H
        SHLD    CCOL
        MVI     A,1
        CALL    SETDIR
        LXI     H,MEMPTY
        CALL    MSG
        LXI     H,0
        RET

; ===========================================================================
; DELCH - delete char under cursor
; ===========================================================================
DELCH:
        CALL    CURLN
        XCHG
        CALL    STRLEN
        SHLD    DCLEN
        LHLD    CCOL
        XCHG
        LHLD    DCLEN
        XCHG
        CALL    SCMP            ; ccol vs len
        CPI     0FFH
        JNZ     DCJOIN          ; ccol >= len
; ccol < len: shift left
        CALL    CURLN
        LXI     D,EBUF
        CALL    SCPY
        LHLD    CCOL
        XCHG
        LXI     H,EBUF
        DAD     D
        SHLD    DCP             ; &ebuf[ccol]
        LHLD    DCLEN
        XCHG
        LHLD    CCOL
        MOV     A,E
        SUB     L
        MOV     C,A
        MOV     A,D
        SBB     H
        MOV     B,A             ; BC = len-ccol
DCSH:   LHLD    DCP
        INX     H
        MOV     A,M
        DCX     H
        MOV     M,A             ; ebuf[i]=ebuf[i+1]
        LHLD    DCP
        INX     H
        SHLD    DCP
        DCX     B
        MOV     A,B
        ORA     C
        JNZ     DCSH
        LHLD    CROW
        LXI     D,EBUF
        CALL    LNSET
        MVI     A,1
        CALL    SETDIR
        LXI     H,MEMPTY
        CALL    MSG
        LXI     H,0
        RET
DCJOIN:
        LHLD    CROW
        INX     H
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP            ; (crow+1) vs lcnt
        CPI     0FFH
        JNZ     DCRET           ; crow+1 >= lcnt
; len + strlen(ln[crow+1]) <= MAXCL ?
        LHLD    CROW
        INX     H
        CALL    GETLN
        XCHG
        CALL    STRLEN          ; HL = strlen(next)
        XCHG
        LHLD    DCLEN
        DAD     D               ; HL = len + next
        LXI     D,MAXCL
        CALL    SCMP            ; sum vs MAXCL
        CPI     1
        JZ      DCLONG          ; sum > MAXCL
        CALL    CURLN
        LXI     D,EBUF
        CALL    SCPY
        LHLD    CROW
        INX     H
        CALL    GETLN
        LXI     D,EBUF
        CALL    SCAT
        LHLD    CROW
        LXI     D,EBUF
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      DCJ2
        LXI     H,0FFFFH
        RET
DCJ2:   LHLD    CROW
        INX     H
        CALL    DELROW
        MVI     A,1
        CALL    SETDIR
        LXI     H,MEMPTY
        CALL    MSG
        LXI     H,0
        RET
DCLONG: LXI     H,MJLONG
        CALL    MSG
DCRET:  LXI     H,0
        RET

; ===========================================================================
; BAKSP - backspace / delete left
; ===========================================================================
BAKSP:
        LHLD    CCOL
        MOV     A,H
        ORA     L
        JZ      BK1             ; ccol == 0
        DCX     H
        SHLD    CCOL
        CALL    DELCH
        LXI     H,0
        RET
BK1:    LHLD    CROW
        MOV     A,H
        ORA     L
        JZ      BKRET           ; crow == 0
; old = strlen(ln[crow-1])
        LHLD    CROW
        DCX     H
        CALL    GETLN
        XCHG
        CALL    STRLEN
        SHLD    BKOLD
; old + strlen(ln[crow]) <= MAXCL ?
        CALL    CURLN
        XCHG
        CALL    STRLEN
        XCHG
        LHLD    BKOLD
        DAD     D
        LXI     D,MAXCL
        CALL    SCMP
        CPI     1
        JZ      BKLONG          ; sum > MAXCL
        LHLD    CROW
        DCX     H
        CALL    GETLN
        LXI     D,EBUF
        CALL    SCPY
        CALL    CURLN
        LXI     D,EBUF
        CALL    SCAT
        LHLD    CROW
        DCX     H
        LXI     D,EBUF
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      BK2
        LXI     H,0FFFFH
        RET
BK2:    LHLD    CROW
        DCX     H
        SHLD    CROW
        LHLD    CROW
        INX     H
        CALL    DELROW
        LHLD    BKOLD
        SHLD    CCOL
        MVI     A,1
        CALL    SETDIR
        LXI     H,MEMPTY
        CALL    MSG
        LXI     H,0
        RET
BKLONG: LXI     H,MJLONG
        CALL    MSG
BKRET:  LXI     H,0
        RET

; ===========================================================================
; NEWLN - split the current line at the cursor
; ===========================================================================
NEWLN:
        LHLD    LCNT
        LXI     D,MAXLN
        CALL    SCMP
        CPI     0FFH
        JZ      NL1
        LXI     H,MBFULL
        CALL    MSG
        LXI     H,0FFFFH
        RET
NL1:    LHLD    CROW
        INX     H
        CALL    INSROW
        MOV     A,H
        ORA     L
        JZ      NL2
        LXI     H,MOOM
        CALL    MSG
        LXI     H,0FFFFH
        RET
NL2:
; lnset(crow+1, &ln[crow][ccol])
        CALL    CURLN
        XCHG
        LHLD    CCOL
        DAD     D               ; HL = ln[crow]+ccol
        XCHG                    ; DE = src
        LHLD    CROW
        INX     H
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      NL3
        LHLD    CROW
        INX     H
        CALL    DELROW
        LXI     H,0FFFFH
        RET
NL3:
; ebuf = copy(ln[crow]); ebuf[ccol]=0; lnset(crow,ebuf)
        CALL    CURLN
        LXI     D,EBUF
        CALL    SCPY
        LHLD    CCOL
        XCHG
        LXI     H,EBUF
        DAD     D
        MVI     M,0
        LHLD    CROW
        LXI     D,EBUF
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      NL4
        LHLD    CROW
        INX     H
        CALL    DELROW
        LXI     H,0FFFFH
        RET
NL4:    LHLD    CROW
        INX     H
        SHLD    CROW
        LXI     H,0
        SHLD    CCOL
        MVI     A,1
        CALL    SETDIR
        LXI     H,MEMPTY
        CALL    MSG
        LXI     H,0
        RET

; ===========================================================================
; CUTLN / CPYLN / UNCUT
; ===========================================================================
CUTLN:
        CALL    CURLN
        LXI     D,CUTB
        CALL    SCPY
        LHLD    CROW
        CALL    DELROW
        LXI     H,0
        SHLD    CCOL
        MVI     A,1
        CALL    SETDIR
        LXI     H,MCUT
        JMP     MSG

CPYLN:
        CALL    CURLN
        LXI     D,CUTB
        CALL    SCPY
        LXI     H,MCOPY
        JMP     MSG

UNCUT:
        LDA     CUTB
        ORA     A
        JNZ     UC1
        LXI     H,MCEMPT
        JMP     MSG
UC1:    LHLD    CROW
        CALL    INSROW
        MOV     A,H
        ORA     L
        JNZ     UC2             ; insrow <> 0 -> buffer full
        LHLD    CROW
        LXI     D,CUTB
        CALL    LNSET
        LXI     H,0
        SHLD    CCOL
        MVI     A,1
        CALL    SETDIR
        LXI     H,MUNCUT
        JMP     MSG
UC2:    LXI     H,MBFULL
        JMP     MSG

; ===========================================================================
; PROMPT - DE=dst, HL=prompt.  Returns A=1 (entered) or A=0 (cancel).
; ===========================================================================
PROMPT:
        SHLD    PRPR
        XCHG
        SHLD    PRDST
        MVI     B,STATY
        MVI     C,1
        CALL    CURMV
        CALL    INVON
        LXI     H,TSP1
        CALL    CPUT
        LHLD    PRPR
        CALL    CPUT
        CALL    EOL
        CALL    INVOF
        LXI     D,0
        LHLD    PRPR
        XCHG
        CALL    STRLEN
        MOV     A,L
        ADI     2
        MOV     C,A
        MVI     B,STATY
        CALL    CURMV
        CALL    SHWCR
        LHLD    PRDST
        XCHG
        CALL    STRLEN
        SHLD    PRP             ; p = strlen(dst)
        LHLD    PRDST
        CALL    CPUT
PRLP:   CALL    KEYWT
        STA     PRC
        CPI     CRCH
        JZ      PROK
        CPI     LFCH
        JZ      PROK
        CPI     ESC
        JZ      PRCAN
        CPI     CTLQ
        JZ      PRCAN
        CPI     BKSP
        JZ      PRBS
        CPI     DELCH7
        JZ      PRBS
        LDA     PRC
        CPI     32
        JC      PRLP
        CPI     127
        JNC     PRLP
        LHLD    PRP
        LXI     D,MAXCL
        CALL    UCMP
        CPI     0FFH
        JNZ     PRLP            ; p >= MAXCL
; dst[p]=c; p++; dst[p]=0; chout(c)
        LHLD    PRDST
        XCHG
        LHLD    PRP
        DAD     D
        LDA     PRC
        MOV     M,A
        INX     H
        MVI     M,0
        LHLD    PRP
        INX     H
        SHLD    PRP
        LDA     PRC
        CALL    CHOUT
        JMP     PRLP
PRBS:   LHLD    PRP
        MOV     A,H
        ORA     L
        JZ      PRLP            ; p == 0
        DCX     H
        SHLD    PRP
        LHLD    PRDST
        XCHG
        LHLD    PRP
        DAD     D
        MVI     M,0             ; dst[p]=0
        LHLD    PRPR
        XCHG
        CALL    STRLEN
        MOV     A,L
        ADI     2
        MOV     C,A
        MVI     B,STATY
        CALL    CURMV
        LHLD    PRDST
        CALL    CPUT
        CALL    EOL
        JMP     PRLP
PROK:   LHLD    PRDST
        XCHG
        LHLD    PRP
        DAD     D
        MVI     M,0             ; dst[p]=0
        MVI     A,1
        STA     DSTAT
        MVI     A,1
        RET
PRCAN:  MVI     A,1
        STA     DSTAT
        XRA     A
        RET

; UPCHR - A=char -> uppercase ASCII char for case-insensitive find
UPCHR:
        CPI     'a'
        RC
        CPI     123
        RNC
        SUI     32
        RET

; ===========================================================================
; STRMCH - HL=hay, DE=ndl.  Returns HL=offset, or 0FFFFH if not found.
; ===========================================================================
STRMCH:
        SHLD    SMHAY
        XCHG
        SHLD    SMNDL
        XCHG
        CALL    STRLEN          ; HL=nlen (DE=ndl preserved)
        MOV     A,H
        ORA     L
        JNZ     SM0
        LXI     H,0FFFFH
        RET
SM0:    LXI     H,0
        SHLD    SMI
SMOUT:  LHLD    SMHAY
        XCHG
        LHLD    SMI
        DAD     D
        SHLD    SMHP            ; hay+i
        MOV     A,M
        ORA     A
        JZ      SMNF
        LHLD    SMHP
        XCHG                    ; DE = hay+i
        LHLD    SMNDL           ; HL = ndl
SMIN:   MOV     A,M
        ORA     A
        JZ      SMMTCH          ; end of ndl -> match
        CALL    UPCHR
        MOV     C,A
        LDAX    D
        CALL    UPCHR
        CMP     C
        JNZ     SMNXT
        INX     H
        INX     D
        JMP     SMIN
SMMTCH: LHLD    SMI
        RET
SMNXT:  LHLD    SMI
        INX     H
        SHLD    SMI
        JMP     SMOUT
SMNF:   LXI     H,0FFFFH
        RET

; ===========================================================================
; FINDNX - find next occurrence of SBUF from the cursor, wrapping
; ===========================================================================
FINDNX:
        LDA     SBUF
        ORA     A
        RZ                      ; empty search term -> nothing to do
        JMP     FINDN2

FINDN2:
        LHLD    CROW
        SHLD    FNR
        LHLD    CCOL
        INX     H
        SHLD    FNC             ; c = ccol+1
        XRA     A
        STA     FNWR            ; wrap = 0
FNLP:
; if r >= lcnt
        LHLD    FNR
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP            ; r vs lcnt
        CPI     0FFH
        JZ      FNA             ; r < lcnt
        LDA     FNWR
        ORA     A
        JZ      FNWRAP
        LXI     H,MNOTFD
        CALL    MSG
        XRA     A
        RET
FNWRAP: LXI     H,0
        SHLD    FNR
        SHLD    FNC
        MVI     A,1
        STA     FNWR
FNA:
; if wrap && r > crow -> not found
        LDA     FNWR
        ORA     A
        JZ      FNB
        LHLD    FNR
        XCHG
        LHLD    CROW
        XCHG
        CALL    SCMP            ; r vs crow
        CPI     1
        JNZ     FNB
        LXI     H,MNOTFD
        CALL    MSG
        XRA     A
        RET
FNB:
; if wrap && r==crow && c>ccol -> not found
        LDA     FNWR
        ORA     A
        JZ      FNC2
        LHLD    FNR
        XCHG
        LHLD    CROW
        CALL    UCMP
        ORA     A
        JNZ     FNC2            ; r <> crow
        LHLD    FNC
        XCHG
        LHLD    CCOL
        XCHG
        CALL    SCMP            ; c vs ccol
        CPI     1
        JNZ     FNC2
        LXI     H,MNOTFD
        CALL    MSG
        XRA     A
        RET
FNC2:
; if c <= strlen(ln[r])
        LHLD    FNR
        CALL    GETLN
        XCHG
        CALL    STRLEN
        XCHG                    ; DE = len
        LHLD    FNC
        CALL    SCMP            ; c vs len
        CPI     1
        JZ      FNNEXT          ; c > len -> skip
; m = strmch(&ln[r][c], sbuf)
        LHLD    FNR
        CALL    GETLN
        XCHG
        LHLD    FNC
        DAD     D               ; HL = ln[r]+c
        LXI     D,SBUF
        CALL    STRMCH
        SHLD    FNM
        LXI     D,0FFFFH
        CALL    UCMP
        ORA     A
        JZ      FNNEXT          ; m == -1
; found
        LHLD    FNR
        SHLD    CROW
        LHLD    FNC
        XCHG
        LHLD    FNM
        DAD     D
        SHLD    CCOL            ; ccol = c + m
        LDA     FNWR
        ORA     A
        JZ      FNFND0
        LXI     H,MWRAP
        CALL    MSG
        JMP     FNFND1
FNFND0: LXI     H,MEMPTY
        CALL    MSG
FNFND1: MVI     A,1
        STA     RALL
        MVI     A,1
        RET
FNNEXT: LHLD    FNR
        INX     H
        SHLD    FNR
        LXI     H,0
        SHLD    FNC
        JMP     FNLP

; ===========================================================================
; DOFIND - prompt for a search string then search
; ===========================================================================
DOFIND:
        LXI     H,SBUF
        LXI     D,OLDSB
        CALL    SCPY            ; old = sbuf  (SCPY: DE=dst,HL=src)
        XRA     A
        STA     SBUF
        LXI     H,MFIND
        LXI     D,SBUF
        CALL    PROMPT
        ORA     A
        JNZ     DF1
        LXI     H,OLDSB
        LXI     D,SBUF
        CALL    SCPY
        LXI     H,MEMPTY
        JMP     MSG
DF1:    LDA     SBUF
        ORA     A
        JNZ     DF2
        LXI     H,OLDSB
        LXI     D,SBUF
        CALL    SCPY
        LXI     H,MEMPTY
        JMP     MSG
DF2:    LXI     H,0FFFFH        ; -1
        SHLD    CCOL
        LDA     SBUF
        ORA     A
        RZ
        JMP     FINDN2

; ===========================================================================
; HELP - compact help page
; ===========================================================================
HELP:
        CALL    CLRS
        MVI     B,2
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP0
        CALL    CPUT
        MVI     B,4
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP1
        CALL    CPUT
        MVI     B,5
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP2
        CALL    CPUT
        MVI     B,6
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP3
        CALL    CPUT
        MVI     B,7
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP4
        CALL    CPUT
        MVI     B,8
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP5
        CALL    CPUT
        MVI     B,9
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP6
        CALL    CPUT
        MVI     B,10
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP7
        CALL    CPUT
        MVI     B,11
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP8
        CALL    CPUT
        MVI     B,12
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP9
        CALL    CPUT
        MVI     B,13
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP10
        CALL    CPUT
        MVI     B,14
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP11
        CALL    CPUT
        MVI     B,15
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP12
        CALL    CPUT
        MVI     B,16
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP13
        CALL    CPUT
        MVI     B,18
        MVI     C,4
        CALL    CURMV
        LXI     H,HLP14
        CALL    CPUT
        CALL    KEYWT
        CALL    CLRS
        MVI     A,1
        STA     DTITLE
        STA     DSTAT
        STA     DHELP
        RET

; ===========================================================================
; MOVEK - A=key: apply cursor motion
; ===========================================================================
MOVEK:
        CPI     KUP
        JZ      MVUP
        CPI     KDN
        JZ      MVDN
        CPI     KLT
        JZ      MVLT
        CPI     KRT
        JZ      MVRT
        RET
MVUP:   LHLD    CROW
        DCX     H
        SHLD    CROW
        RET
MVDN:   LHLD    CROW
        INX     H
        SHLD    CROW
        RET
MVLT:   LHLD    CCOL
        MOV     A,H
        ORA     L
        JZ      MVLT2
        DCX     H
        SHLD    CCOL
        RET
MVLT2:  LHLD    CROW
        MOV     A,H
        ORA     L
        RZ
        DCX     H
        SHLD    CROW
        CALL    CURLN
        XCHG
        CALL    STRLEN
        SHLD    CCOL
        RET
MVRT:   CALL    CURLN
        XCHG
        CALL    STRLEN
        SHLD    MVLEN
        LHLD    CCOL
        XCHG
        LHLD    MVLEN
        XCHG
        CALL    SCMP            ; ccol vs len
        CPI     0FFH
        JNZ     MVRT2           ; ccol >= len
        LHLD    CCOL
        INX     H
        SHLD    CCOL
        RET
MVRT2:  LHLD    CROW
        INX     H
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP            ; (crow+1) vs lcnt
        CPI     0FFH
        RNZ                     ; crow+1 >= lcnt
        LHLD    CROW
        INX     H
        SHLD    CROW
        LXI     H,0
        SHLD    CCOL
        RET

; ===========================================================================
; ASKXIT - confirm exit when modified.  Returns A=1 ok, A=0 cancel.
; ===========================================================================
ASKXIT:
        LDA     DIRT
        ORA     A
        JNZ     AX1
        MVI     A,1
        RET
AX1:    LXI     H,MMODQ
        CALL    MSG
        CALL    REDRW
        CALL    KEYWT
        CPI     'y'
        JZ      AXY
        CPI     'Y'
        JZ      AXY
        LXI     H,MEXITC
        CALL    MSG
        XRA     A
        RET
AXY:    MVI     A,1
        RET

; ===========================================================================
; DOBIN - A=key: dispatch one key
; ===========================================================================
DOBIN:
        STA     CKEY
; eflg == 2 ?
        LDA     EFLG
        CPI     2
        JNZ     DBE1
        XRA     A
        STA     EFLG
        LDA     CKEY
        CPI     'A'
        JNZ     DBE0A
        MVI     A,KUP
        JMP     MOVEK
DBE0A:  CPI     'B'
        JNZ     DBE0B
        MVI     A,KDN
        JMP     MOVEK
DBE0B:  CPI     'C'
        JNZ     DBE0C
        MVI     A,KRT
        JMP     MOVEK
DBE0C:  CPI     'D'
        JNZ     DBE1
        MVI     A,KLT
        JMP     MOVEK
DBE1:   LDA     EFLG
        CPI     1
        JNZ     DBDISP
        XRA     A
        STA     EFLG
        LDA     CKEY
        CPI     '['
        JNZ     DBDISP
        MVI     A,2
        STA     EFLG
        RET
DBDISP:
        LDA     CKEY
        CPI     CTLW
        JNZ     DB1
        CALL    HELP
        LXI     H,MEMPTY
        CALL    MSG
        MVI     A,1
        STA     RALL
        RET
DB1:    CPI     CTLF
        JNZ     DB2
        CALL    DOFIND
        MVI     A,1
        STA     RALL
        RET
DB2:    CPI     CTLN
        JNZ     DB3
        LDA     SBUF
        ORA     A
        JZ      DB2A
        CALL    FINDNX
        JMP     DB2B
DB2A:   LXI     H,MNOSRC
        CALL    MSG
DB2B:   MVI     A,1
        STA     RALL
        RET
DB3:    CPI     CTLO
        JNZ     DB4
        LXI     H,MWRITN
        CALL    MSG
        CALL    REDRW
        CALL    SAVEF
        MVI     A,1
        STA     RALL
        RET
DB4:    CPI     ESC
        JZ      DB4X
        CPI     CTLQ
        JNZ     DB5
DB4X:   CALL    ASKXIT
        ORA     A
        JZ      DB4B
        MVI     A,1
        STA     QUIT
DB4B:   MVI     A,1
        STA     RALL
        RET
DB5:    CPI     CTLK
        JNZ     DB6
        CALL    CUTLN
        MVI     A,1
        STA     RALL
        RET
DB6:    CPI     CTLC
        JNZ     DB7
        CALL    CPYLN
        RET
DB7:    CPI     CTLU
        JNZ     DB8
        CALL    UNCUT
        MVI     A,1
        STA     RALL
        RET
DB8:    CPI     BKSP
        JZ      DB8X
        CPI     DELCH7
        JNZ     DB9
DB8X:   LHLD    CCOL
        MOV     A,H
        ORA     L
        JZ      DB8B
        CALL    BAKSP
        LHLD    CROW
        SHLD    DLIN
        RET
DB8B:   CALL    BAKSP
        MVI     A,1
        STA     RALL
        RET
DB9:    CPI     KDEL
        JNZ     DB10
        CALL    CURLN
        XCHG
        CALL    STRLEN
        XCHG
        LHLD    CCOL
        CALL    SCMP            ; ccol vs len
        CPI     0FFH
        JNZ     DB9B            ; ccol >= len
        CALL    DELCH
        LHLD    CROW
        SHLD    DLIN
        RET
DB9B:   CALL    DELCH
        MVI     A,1
        STA     RALL
        RET
DB10:   CPI     CRCH
        JZ      DB10X
        CPI     LFCH
        JNZ     DB11
DB10X:  CALL    NEWLN
        MVI     A,1
        STA     RALL
        RET
DB11:   CPI     KUP
        JZ      DB11X
        CPI     KDN
        JZ      DB11X
        CPI     KLT
        JZ      DB11X
        CPI     KRT
        JNZ     DB12
DB11X:  LDA     CKEY
        JMP     MOVEK
DB12:   CPI     KPGU
        JNZ     DB13
        LHLD    CROW
        LXI     D,0FFE5H        ; -27
        DAD     D
        SHLD    CROW
        MVI     A,1
        STA     RALL
        RET
DB13:   CPI     KPGD
        JNZ     DB14
        LHLD    CROW
        LXI     D,TEXTN
        DAD     D
        SHLD    CROW
        MVI     A,1
        STA     RALL
        RET
DB14:   CPI     KTOP
        JNZ     DB15
        LXI     H,0
        SHLD    CROW
        SHLD    CCOL
        MVI     A,1
        STA     RALL
        RET
DB15:   CPI     KBOT
        JNZ     DB16
        LHLD    LCNT
        DCX     H
        SHLD    CROW
        LXI     H,0
        SHLD    CCOL
        MVI     A,1
        STA     RALL
        RET
DB16:   CPI     KINS
        JNZ     DB17
        LDA     OVRM
        XRI     1
        STA     OVRM
        MVI     A,1
        STA     DSTAT
        RET
DB17:   LDA     CKEY
        CPI     32
        JC      DBRET
        CPI     127
        JNC     DBRET
; printable
        LDA     OVRM
        ORA     A
        JZ      DBINS
; overwrite mode
        CALL    CURLN
        XCHG
        CALL    STRLEN
        XCHG
        LHLD    CCOL
        CALL    SCMP            ; ccol vs len
        CPI     0FFH
        JNZ     DBINS           ; ccol >= len -> insert
        CALL    CURLN
        LXI     D,EBUF
        CALL    SCPY
        LHLD    CCOL
        XCHG
        LXI     H,EBUF
        DAD     D
        LDA     CKEY
        MOV     M,A
        LHLD    CROW
        LXI     D,EBUF
        CALL    LNSET
        LHLD    CCOL
        INX     H
        SHLD    CCOL
        MVI     A,1
        CALL    SETDIR
        LXI     H,MEMPTY
        CALL    MSG
        JMP     DBPDL
DBINS:  LDA     CKEY
        CALL    INSCH
DBPDL:  LHLD    CROW
        SHLD    DLIN
DBRET:  RET

; ===========================================================================
; GETFCB - build FNAME from the default FCB (5CH).  Returns A=1 if a name.
; ===========================================================================
GETFCB:
        LXI     H,FNAME
        SHLD    GFD
; copy base chars (FCB+1..+8) skipping spaces
        LXI     H,DEFFCB+1
        LXI     D,FNAME
        MVI     B,8
GF1:    MOV     A,M
        CPI     ' '
        JZ      GF2
        STAX    D
        INX     D
GF2:    INX     H
        DCR     B
        JNZ     GF1
; if FCB[9] <> ' ' append '.' and ext
        LDA     DEFFCB+9
        CPI     ' '
        JZ      GF4
        MVI     A,'.'
        STAX    D
        INX     D
        LXI     H,DEFFCB+9
        MVI     B,3
GF3:    MOV     A,M
        CPI     ' '
        JZ      GF3B
        STAX    D
        INX     D
GF3B:   INX     H
        DCR     B
        JNZ     GF3
GF4:    XRA     A
        STAX    D               ; NUL terminate
; return (j>0): true if FNAME[0] <> 0
        LDA     FNAME
        ORA     A
        JZ      GF5
        MVI     A,1
        RET
GF5:    XRA     A
        RET

; ===========================================================================
; NAMEFC - DE=fcb, HL=name: parse "FNAME.EXT" into a 36-byte FCB
; ===========================================================================
NAMEFC:
        SHLD    NFNAME
        XCHG
        SHLD    NFFCB
        MVI     B,36
        XRA     A
NF0:    MOV     M,A
        INX     H
        DCR     B
        JNZ     NF0
        LHLD    NFFCB
        INX     H
        MVI     B,11
        MVI     A,' '
NF1:    MOV     M,A
        INX     H
        DCR     B
        JNZ     NF1
        LHLD    NFNAME
        XCHG
        LHLD    NFFCB
        INX     H               ; HL = fcb+1
        MVI     B,8
NF2:    LDAX    D
        ORA     A
        JZ      NFDONE
        CPI     '.'
        JZ      NFEXT
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     NF2
NF2S:   LDAX    D
        ORA     A
        JZ      NFDONE
        CPI     '.'
        JZ      NFEXT
        INX     D
        JMP     NF2S
NFEXT:  INX     D               ; skip '.'
        LHLD    NFFCB
        LXI     B,9
        DAD     B               ; HL = fcb+9
        MVI     B,3
NF3:    LDAX    D
        ORA     A
        JZ      NFDONE
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     NF3
NFDONE: RET

; ===========================================================================
; MKBAKN - DE=dst, HL=src: build "BASE.BAK" string
; ===========================================================================
MKBAKN:
        SHLD    MBNSRC
        XCHG
        SHLD    MBNDST
        LHLD    MBNSRC
        XCHG                    ; DE=src
        LHLD    MBNDST          ; HL=dst
        MVI     B,8
MBN1:   LDAX    D
        ORA     A
        JZ      MBN2
        CPI     '.'
        JZ      MBN2
        MOV     C,A
        MOV     A,B
        ORA     A
        JZ      MBN1B           ; j>=8: stop storing but keep scanning? C stops at 8
        MOV     A,C
        MOV     M,A
        INX     H
        DCR     B
MBN1B:  INX     D
        JMP     MBN1
MBN2:   MVI     M,'.'
        INX     H
        MVI     M,'B'
        INX     H
        MVI     M,'A'
        INX     H
        MVI     M,'K'
        INX     H
        MVI     M,0
        RET

; ===========================================================================
; MKBAK - rename FNAME -> FNAME.BAK (delete any old .BAK first)
; ===========================================================================
MKBAK:
        LXI     H,RFCB
        MVI     B,36
        XRA     A
MB0:    MOV     M,A
        INX     H
        DCR     B
        JNZ     MB0
; source name -> RFCB+1..+8 (padded), ext -> RFCB+9..+11
        LXI     D,FNAME
        LXI     H,RFCB+1
        MVI     B,8
MB1:    LDAX    D
        ORA     A
        JZ      MB1P
        CPI     '.'
        JZ      MB1P
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     MB1
; skip to '.' if name longer than 8
MB1S:   LDAX    D
        ORA     A
        JZ      MB1E
        CPI     '.'
        JZ      MB1E
        INX     D
        JMP     MB1S
MB1P:   MOV     A,B             ; pad remaining name field with spaces
        ORA     A
        JZ      MB1E
MB1PL:  MVI     M,' '
        INX     H
        DCR     B
        JNZ     MB1PL
MB1E:
; ext field RFCB+9: from name after '.'
        LXI     H,RFCB+9
        MVI     A,' '
        MOV     M,A
        INX     H
        MOV     M,A
        INX     H
        MOV     M,A             ; default 3 spaces
        LDAX    D
        CPI     '.'
        JNZ     MB2
        INX     D
        LXI     H,RFCB+9
        MVI     B,3
MB1X:   LDAX    D
        ORA     A
        JZ      MB2
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     MB1X
MB2:
; dest = base.BAK at RFCB+16: copy name field, set ext BAK
        LXI     H,RFCB+1
        LXI     D,RFCB+17
        MVI     B,8
MB2C:   MOV     A,M
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     MB2C
        LXI     H,RFCB+25
        MVI     M,'B'
        INX     H
        MVI     M,'A'
        INX     H
        MVI     M,'K'
; delete old .BAK (FCB at RFCB+16), then rename
        LXI     D,RFCB+16
        DOS     FDELF
        LXI     D,RFCB
        DOS     FRENF
        RET

; ===========================================================================
; UNBAK - rename FNAME.BAK -> FNAME
; ===========================================================================
UNBAK:
        LXI     H,RFCB
        MVI     B,36
        XRA     A
UB0:    MOV     M,A
        INX     H
        DCR     B
        JNZ     UB0
; source = base.BAK at RFCB
        LXI     D,FNAME
        LXI     H,RFCB+1
        MVI     B,8
UB1:    LDAX    D
        ORA     A
        JZ      UB1P
        CPI     '.'
        JZ      UB1P
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     UB1
UB1S:   LDAX    D
        ORA     A
        JZ      UB1E
        CPI     '.'
        JZ      UB1E
        INX     D
        JMP     UB1S
UB1P:   MOV     A,B
        ORA     A
        JZ      UB1E
UB1PL:  MVI     M,' '
        INX     H
        DCR     B
        JNZ     UB1PL
UB1E:   LXI     H,RFCB+9
        MVI     M,'B'
        INX     H
        MVI     M,'A'
        INX     H
        MVI     M,'K'
; dest = original name at RFCB+16
        LXI     D,FNAME
        LXI     H,RFCB+17
        MVI     B,8
UB2:    LDAX    D
        ORA     A
        JZ      UB2P
        CPI     '.'
        JZ      UB2P
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     UB2
UB2S:   LDAX    D
        ORA     A
        JZ      UB2E
        CPI     '.'
        JZ      UB2E
        INX     D
        JMP     UB2S
UB2P:   MOV     A,B
        ORA     A
        JZ      UB2E
UB2PL:  MVI     M,' '
        INX     H
        DCR     B
        JNZ     UB2PL
UB2E:
; ext of dest from name after '.'
        LXI     H,RFCB+25
        MVI     A,' '
        MOV     M,A
        INX     H
        MOV     M,A
        INX     H
        MOV     M,A
        LDAX    D
        CPI     '.'
        JNZ     UB3
        INX     D
        LXI     H,RFCB+25
        MVI     B,3
UB2X:   LDAX    D
        ORA     A
        JZ      UB3
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     UB2X
UB3:    LXI     D,RFCB
        DOS     FRENF
        RET

; ===========================================================================
; LOADF - load FNAME into the buffer (renaming the original to .BAK)
; ===========================================================================
LOADF:
        CALL    ZEROB
        CALL    MKBAK
        LXI     D,BFN
        LXI     H,FNAME
        CALL    MKBAKN          ; BFN = base.BAK
        LXI     D,RFCB
        LXI     H,BFN
        CALL    NAMEFC          ; RFCB = FCB for BFN
        LXI     D,RFCB
        DOS     FOPENF
        CPI     0FFH
        JNZ     LDOPEN
        LXI     H,MNEW
        CALL    MSG
        RET
LDOPEN:
        LXI     H,0
        SHLD    LDP             ; p = 0
        LXI     H,0
        SHLD    LCNT            ; lcnt = 0
        XRA     A
        STA     EBUF
LDREC:
        LXI     D,DMABUF
        DOS     FDMAF
        LXI     D,RFCB
        DOS     FREADF
        ORA     A
        JNZ     LDEND           ; nonzero -> EOF
        LXI     H,DMABUF
        SHLD    LDQ
        MVI     A,128
        STA     LDN
LDCH:   LHLD    LDQ
        MOV     A,M
        STA     LDC
; CPMEOF -> end
        CPI     CPMEOF
        JZ      LDFIN
        CPI     CRCH
        JZ      LDNEXT          ; skip CR
        CPI     LFCH
        JZ      LDLF
        CPI     TABCH
        JZ      LDTAB
; ordinary char
        LHLD    LDP
        LXI     D,MAXCL
        CALL    UCMP
        CPI     0FFH
        JNZ     LDNEXT          ; p >= MAXCL: drop
        LDA     LDC
        LHLD    LDP
        XCHG
        LXI     H,EBUF
        DAD     D
        MOV     M,A
        INX     H
        MVI     M,0             ; ebuf[p+1]=0
        LHLD    LDP
        INX     H
        SHLD    LDP
        JMP     LDNEXT
LDLF:
; terminate line, lnset(lcnt, ebuf)
        LHLD    LDP
        XCHG
        LXI     H,EBUF
        DAD     D
        MVI     M,0             ; ebuf[p]=0
        LHLD    LCNT
        LXI     D,MAXLN
        CALL    SCMP
        CPI     0FFH
        JNZ     LDLF2           ; lcnt >= MAXLN
        CALL    LDROOM
        ORA     A
        JZ      LDTRNC
        LHLD    LCNT
        LXI     D,EBUF
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      LDLF1
        LXI     H,MTRUNC
        CALL    MSG
        JMP     LDFIN
LDLF1:  LHLD    LCNT
        INX     H
        SHLD    LCNT
LDLF2:  LXI     H,0
        SHLD    LDP
        XRA     A
        STA     EBUF
        JMP     LDNEXT
LDTAB:
; expand tab to next multiple of 8
LDT1:   LHLD    LDP
        LXI     D,MAXCL
        CALL    UCMP
        CPI     0FFH
        JNZ     LDT2            ; p >= MAXCL
        LHLD    LDP
        XCHG
        LXI     H,EBUF
        DAD     D
        MVI     M,' '
        LHLD    LDP
        INX     H
        SHLD    LDP
LDT2:   LHLD    LDP
        MOV     A,L
        ANI     7
        JZ      LDT3            ; p%8==0
        LHLD    LDP
        LXI     D,MAXCL
        CALL    UCMP
        CPI     0FFH
        JZ      LDT1            ; p < MAXCL: keep filling
LDT3:   LHLD    LDP
        XCHG
        LXI     H,EBUF
        DAD     D
        MVI     M,0
        JMP     LDNEXT
LDNEXT:
        LHLD    LDQ
        INX     H
        SHLD    LDQ
        LDA     LDN
        DCR     A
        STA     LDN
        JNZ     LDCH
        JMP     LDREC
LDEND:
LDFIN:
; if p>0 or lcnt==0: finalize last line
        LHLD    LDP
        MOV     A,H
        ORA     L
        JNZ     LDF1
        LHLD    LCNT
        MOV     A,H
        ORA     L
        JNZ     LDCLOS
LDF1:   LHLD    LDP
        XCHG
        LXI     H,EBUF
        DAD     D
        MVI     M,0
        LHLD    LCNT
        LXI     D,MAXLN
        CALL    SCMP
        CPI     0FFH
        JNZ     LDCLOS          ; lcnt >= MAXLN
        CALL    LDROOM
        ORA     A
        JZ      LDTRNC
        LHLD    LCNT
        LXI     D,EBUF
        CALL    LNSET
        MOV     A,H
        ORA     L
        JZ      LDF2
        LXI     D,RFCB
        DOS     FCLOSF
        XRA     A
        CALL    SETDIR
        LXI     H,MTRUNC
        JMP     MSG
LDF2:   LHLD    LCNT
        INX     H
        SHLD    LCNT
LDCLOS:
        LXI     D,RFCB
        DOS     FCLOSF
        XRA     A
        CALL    SETDIR
        LXI     H,MLOAD
        JMP     MSG
LDTRNC: LXI     D,RFCB
        DOS     FCLOSF
        XRA     A
        CALL    SETDIR
        LXI     H,MTRUNC
        JMP     MSG

; ===========================================================================
; SAVEF - write the buffer to FNAME
; ===========================================================================
SAVEF:
        LDA     FNAME
        ORA     A
        JNZ     SV1
        LXI     H,MNOFN
        CALL    MSG
        LXI     H,0FFFFH
        RET
SV1:    LXI     D,RFCB
        LXI     H,FNAME
        CALL    NAMEFC
        LXI     D,RFCB
        DOS     FDELF           ; remove any existing
        LXI     D,RFCB
        DOS     FMAKEF
        CPI     0FFH
        JNZ     SV2
        LXI     H,MCWRIT
        CALL    MSG
        LXI     H,0FFFFH
        RET
SV2:    LXI     D,DMABUF
        DOS     FDMAF
        XRA     A
        STA     WRIDX
        LXI     H,0
        SHLD    SVR             ; r = 0
SVRL:   LHLD    SVR
        XCHG
        LHLD    LCNT
        XCHG
        CALL    SCMP            ; r vs lcnt
        CPI     0FFH
        JNZ     SVDONE          ; r >= lcnt
        LHLD    SVR
        CALL    GETLN
        SHLD    SVP             ; p = ln[r]
SVCL:   LHLD    SVP
        MOV     A,M
        ORA     A
        JZ      SVEOL
        CALL    PUTBYT
        ORA     A
        JNZ     SVFULL          ; disk full
        LHLD    SVP
        INX     H
        SHLD    SVP
        JMP     SVCL
SVEOL:  MVI     A,CRCH
        CALL    PUTBYT
        MVI     A,LFCH
        CALL    PUTBYT
        LHLD    SVR
        INX     H
        SHLD    SVR
        JMP     SVRL
SVDONE:
; append ^Z then flush partial record
        MVI     A,CPMEOF
        CALL    PUTBYT
        LDA     WRIDX
        ORA     A
        JZ      SVCLOS
; pad to 128 with zeros
SVPAD:  LDA     WRIDX
        CPI     128
        JZ      SVPADW
        MOV     C,A
        MVI     B,0
        LXI     H,DMABUF
        DAD     B
        MVI     M,0
        LDA     WRIDX
        INR     A
        STA     WRIDX
        JMP     SVPAD
SVPADW: LXI     D,RFCB
        DOS     FWRITF
SVCLOS: LXI     D,RFCB
        DOS     FCLOSF
        XRA     A
        CALL    SETDIR
        MVI     A,1
        STA     EVSAV
        LXI     H,MWROTE
        JMP     MSG
SVFULL: LXI     D,RFCB
        DOS     FCLOSF
        LXI     H,MDFULL
        CALL    MSG
        LXI     H,0FFFFH
        RET

; PUTBYT - A=byte: append to the write record, flushing at 128.
;          Returns A=0 ok, A<>0 on disk-full.
PUTBYT:
        PUSH    PSW
        LDA     WRIDX
        MOV     C,A
        MVI     B,0
        LXI     H,DMABUF
        DAD     B
        POP     PSW
        MOV     M,A
        LDA     WRIDX
        INR     A
        STA     WRIDX
        CPI     128
        JNZ     PBOK
        LXI     D,RFCB
        DOS     FWRITF
        PUSH    PSW
        XRA     A
        STA     WRIDX
        POP     PSW
        RET                     ; A = write status
PBOK:   XRA     A
        RET

; ===========================================================================
; USAGE - command line help
; ===========================================================================
USAGE:
        LXI     H,USG1
        CALL    CPUT
        LXI     H,USG2
        CALL    CPUT
        LXI     H,USG3
        JMP     CPUT

; ===========================================================================
; Constant strings
; ===========================================================================
E2J:    DB      '[2J',0
EK:     DB      '[K',0
E7M:    DB      '[7m',0
E0M:    DB      '[0m',0
ECSH:   DB      '[?25h',0
ECSL:   DB      '[?25l',0

TTITLE: DB      ' EDIT v4 ',0
TNONAM: DB      '[No Name]',0
TSTAR:  DB      ' *',0
TSP1:   DB      ' ',0
TSP2:   DB      '  ',0
TLNL:   DB      '  Ln ',0
TSLSH:  DB      '/',0
TCOLL:  DB      ' Col ',0
TOVR:   DB      ' OVR',0
TINS:   DB      ' INS',0
TFREE:  DB      ' Free ',0
TK:     DB      'K',0
TB:     DB      'B',0
THELP:  DB      '^W Help ^O Write ^Q Exit ^F Find ^N Next ^K Cut ^C Copy ^U Paste',0

MEMPTY: DB      0
MOOM:   DB      'Out of memory',0
MNEW:   DB      'New file',0
MLOAD:  DB      'File loaded',0
MTRUNC: DB      'Truncated - out of memory',0
MNOFN:  DB      'No filename',0
MCWRIT: DB      'Cannot write file',0
MDFULL: DB      'Disk full - save failed',0
MWROTE: DB      'Wrote file',0
MCUT:   DB      'Cut line',0
MCOPY:  DB      'Copied line',0
MUNCUT: DB      'Uncut line',0
MCEMPT: DB      'Cut buffer empty',0
MBFULL: DB      'Buffer full',0
MLLONG: DB      'Line too long',0
MJLONG: DB      'Join would be too long',0
MNOTFD: DB      'Not found',0
MWRAP:  DB      'Wrapped',0
MNOSRC: DB      'No search term - use Ctrl-F',0
MWRITN: DB      'Writing...',0
MEXITC: DB      'Exit cancelled',0
MMODQ:  DB      'Modified buffer. Exit without saving? y/N',0
MFIND:  DB      'Find:',0

HLP0:   DB      'EDIT help',0
HLP1:   DB      'Arrow keys   Move cursor',0
HLP2:   DB      'Ctrl-R/V     Page up / Page down',0
HLP3:   DB      'Ctrl-T/B     Go to top / bottom',0
HLP4:   DB      'Ctrl-O       Write file to disk',0
HLP5:   DB      'ESC/Ctrl-Q   Exit editor',0
HLP6:   DB      'Ctrl-K       Cut current line',0
HLP7:   DB      'Ctrl-C       Copy current line',0
HLP8:   DB      'Ctrl-U       Paste (uncut) line',0
HLP9:   DB      'Ctrl-F       Find',0
HLP10:  DB      'Ctrl-N       Find next',0
HLP11:  DB      'Backspace    Delete char left',0
HLP12:  DB      'Delete       Delete char right',0
HLP13:  DB      'Enter        Split line',0
HLP14:  DB      'Press any key to return.',0

USG1:   DB      13,10,'EDIT - nano-inspired BDS C editor',13,10,0
USG2:   DB      'Usage: edit filename',13,10,0
USG3:   DB      'Use Ctrl-W inside the editor for help.',13,10,0

; ===========================================================================
; Variables (uninitialized at load; set at runtime)
; ===========================================================================
OLDSP:  DS      2
CONOTA: DS      2

; heap state
FREEL:  DS      2
BRKP:   DS      2
HEAPND: DS      2
USED:   DS      2
ANEED:  DS      2
ACUR:   DS      2
APREV:  DS      2
ANEW:   DS      2
ANEXT:  DS      2
ATMP:   DS      2
XLEN:   DS      2

; editor state
LCNT:   DS      2
CROW:   DS      2
CCOL:   DS      2
TOP:    DS      2
LFT:    DS      2
DLIN:   DS      2
SROW:   DS      2
SCOL:   DS      2
SLCNT:  DS      2
DIRT:   DS      1
QUIT:   DS      1
RALL:   DS      1
DTITLE: DS      1
DSTAT:  DS      1
DHELP:  DS      1
OVRM:   DS      1
EVSAV:  DS      1
EFLG:   DS      1

; scratch / temporaries
CMROW:  DS      1
CMCOL:  DS      1
CKEY:   DS      1
CINS:   DS      1
MSGSRC: DS      2
LSROW:  DS      2
LSPTR:  DS      2
ZBI:    DS      2
FXLEN:  DS      2
FMVAL:  DS      2
DLN:    DS      2
DLL:    DS      2
DLP:    DS      2
DLX:    DS      2
RDOT:   DS      2
RDOL:   DS      2
RDSM:   DS      1
RDI:    DS      2
IRR:    DS      2
IRPT:   DS      2
IRI:    DS      2
DRR:    DS      2
DRP:    DS      2
DRI:    DS      2
ICLEN:  DS      2
ICP:    DS      2
DCLEN:  DS      2
DCP:    DS      2
BKOLD:  DS      2
MVLEN:  DS      2
PRPR:   DS      2
PRDST:  DS      2
PRP:    DS      2
PRC:    DS      1
SMHAY:  DS      2
SMNDL:  DS      2
SMI:    DS      2
SMHP:   DS      2
FNR:    DS      2
FNC:    DS      2
FNWR:   DS      1
FNM:    DS      2
GFD:    DS      2
NFNAME: DS      2
NFFCB:  DS      2
MBNSRC: DS      2
MBNDST: DS      2
LDP:    DS      2
LDQ:    DS      2
LDN:    DS      1
LDC:    DS      1
SVR:    DS      2
SVP:    DS      2
WRIDX:  DS      1

; buffers
FNAME:   DS      36
MESG:   DS      72
CUTB:   DS      MAXCL+2
SBUF:   DS      MAXCL+2
OLDSB:  DS      MAXCL+2
EBUF:   DS      MAXCL+4
BFN:    DS      16
RFCB:   DS      36
DMABUF: DS      128

; line pointer table and stack
LNPTR:  DS      MAXLN*2
        DS      256
STKTOP  EQU     $
HEAPB   EQU     $

        END     START
