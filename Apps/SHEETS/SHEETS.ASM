; ============================================================================
; SHEETS - Simple spreadsheet (8080 MAC assembler port of SHEETS.C)
; ============================================================================
; Feature-for-feature port of Apps/SHEETS/SHEETS.C, built for CP/M on the
; Altair 8800 using VT100/xterm.js control sequences.  Terminal: 80 cols.
;
; Grid:    26 columns (A..Z) by 99 rows
; Viewport: 7 columns x 26 rows, scrolls with cursor
; Cells:   sparse - a char* per slot, 0 means empty (small heap allocator)
; Values:  32-bit signed integers; text cells stored literally
; Formula: leading '='; supports + - * / unary minus, parens, cell refs
;          (A1..Z99), and SUM AVG MIN MAX COUNT range functions.
;
; Keys (identical to the C version):
;   Arrow keys    Move cursor
;   Enter         Edit current cell (preserve current content)
;   Any printable Start fresh edit with that character
;   ESC           (in edit) cancel ; (in nav) quit
;   Ctrl-K        Clear current cell      Ctrl-O   Write file
;   Ctrl-L        Reload file             Ctrl-W   Help
;   Ctrl-G        Go to cell              Ctrl-N   Insert row
;   Ctrl-T        Insert column           Ctrl-R/V Page up / Page down
;   Ctrl-Q        Quit
;
; Build (CP/M):
;   mac sheets       ->  SHEETS.HEX (plus .PRN/.SYM)
;   load sheets      ->  SHEETS.COM
;
; MAC constraints: symbols unique in first 6 chars; ports in decimal.
;
; 32-bit longs are stored big-endian (byte 0 = most significant, byte 3 =
; least significant, sign bit in byte 0) to match Apps/SDK/LONG.C semantics.
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
FDMAF   EQU     26              ; set DMA address
DEFFCB  EQU     5CH             ; default FCB parsed by the CCP

; -- grid geometry ----------------------------------------------------------
MAXCOL  EQU     26
MAXROW  EQU     99
CWID    EQU     10
VCOLS   EQU     7
VROWS   EQU     26
TITR    EQU     1
HDRR    EQU     2
DATR    EQU     3
EDR     EQU     29
STATR   EQU     29
HLPR    EQU     30
EBUFSZ  EQU     80

; -- key / control codes ----------------------------------------------------
ESC     EQU     27
CRCH    EQU     13
LFCH    EQU     10
BKSP    EQU     8
DELC7   EQU     127
CPMEOF  EQU     26
KUP     EQU     5
KDN     EQU     24
KRT     EQU     4
KLT     EQU     19
CTLG    EQU     7
CTLK    EQU     11
CTLL    EQU     12
CTLN    EQU     14
CTLO    EQU     15
CTLQ    EQU     17
CTLR    EQU     18
CTLT    EQU     20
CTLV    EQU     22
CTLW    EQU     23

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

        CALL    INIT            ; zero cell table, resolve CONOUT, set heap

; ename[0]=0 etc done in INIT; lzro/lten set up there too
        CALL    GETFCB          ; build ENAME from the default FCB
        ORA     A
        JNZ     ST1
        CALL    USAGE
        JMP     EXIT
ST1:    CALL    LOADF           ; load file (or "New file")
        CALL    CLRS
        CALL    DRWALL
        XRA     A
        STA     QUIT

MAINLP: LDA     QUIT
        ORA     A
        JNZ     MEND

; save old positions (orow,ocol,otrow,otcol)
        LHLD    CROW
        SHLD    OROW
        LHLD    CCOL
        SHLD    OCOL
        LHLD    TROW
        SHLD    OTROW
        LHLD    TCOL
        SHLD    OTCOL

        CALL    KEYWT
        STA     CKEY
        CALL    DOKEY           ; dispatch key

        CALL    FIXVW
; if rall: drwall; else if scrolled: drwgrd; else move highlight / status
        LDA     RALL
        ORA     A
        JZ      ML1
        CALL    DRWALL
        XRA     A
        STA     RALL
        JMP     MAINLP
ML1:
; trow<>otrow || tcol<>otcol ?
        LHLD    TROW
        XCHG
        LHLD    OTROW
        CALL    UCMP
        ORA     A
        JNZ     MLSCRL
        LHLD    TCOL
        XCHG
        LHLD    OTCOL
        CALL    UCMP
        ORA     A
        JNZ     MLSCRL
; crow<>orow || ccol<>ocol ?
        LHLD    CROW
        XCHG
        LHLD    OROW
        CALL    UCMP
        ORA     A
        JNZ     MLMOVE
        LHLD    CCOL
        XCHG
        LHLD    OCOL
        CALL    UCMP
        ORA     A
        JNZ     MLMOVE
; nothing moved: redraw status only
        CALL    STATLN
        JMP     MAINLP
MLSCRL: CALL    DRWGRD
        JMP     MAINLP
MLMOVE:
; drwrow(orow-trow); drwrow(crow-trow); statln
        LHLD    OROW
        XCHG
        LHLD    TROW
        MOV     A,E
        SUB     L
        MOV     L,A
        MVI     H,0
        CALL    DRWROW
        LHLD    CROW
        XCHG
        LHLD    TROW
        MOV     A,E
        SUB     L
        MOV     L,A
        MVI     H,0
        CALL    DRWROW
        CALL    STATLN
        JMP     MAINLP

MEND:   CALL    CLRS
        CALL    SHWCR
        LXI     H,TDONE
        CALL    CPUT
EXIT:   LHLD    OLDSP
        SPHL
        RET

; ===========================================================================
; INIT - one-time setup
; ===========================================================================
INIT:
; resolve BIOS CONOUT vector (WBOOT+9)
        LHLD    1
        LXI     D,9
        DAD     D
        SHLD    CONOTA
; heap: BRK = HEAPB, FREEL = 0, HEAPND = mem[6] - 256
        LXI     H,HEAPB
        SHLD    BRKP
        LXI     H,0
        SHLD    FREEL
        LHLD    6
        LXI     D,0FF00H        ; -256
        DAD     D
        SHLD    HEAPND
; zero cells[] table (MAXROW*MAXCOL*2 bytes)
        LXI     H,CELLS
        LXI     B,MAXROW*MAXCOL*2
INZ1:   MVI     M,0
        INX     H
        DCX     B
        MOV     A,B
        ORA     C
        JNZ     INZ1
; state
        LXI     H,0
        SHLD    CROW
        SHLD    CCOL
        SHLD    TROW
        SHLD    TCOL
        XRA     A
        STA     DIRT
        STA     ENAME
        STA     MESG
        MVI     A,1
        STA     RALL
; constant longs lzro=0, lten=10
        LXI     H,LZRO
        CALL    ZERO4
        LXI     H,10
        LXI     D,LTEN
        CALL    SETI4
        RET

; ===========================================================================
; Low-level console
; ===========================================================================
; CHOUT - write char in A via BIOS CONOUT (preserves all registers)
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
        PCHL

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
PESC:   MVI     A,ESC
        CALL    CHOUT
        JMP     CPUT

; ===========================================================================
; Number helpers
; ===========================================================================
; DIV10 - HL = HL/10, remainder in A.  Preserves BC and DE.
DIV10:  PUSH    D
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
NUMP:   MOV     A,H
        ORA     L
        JNZ     NMP1
        MVI     A,'0'
        JMP     CHOUT
NMP1:   MVI     C,0
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

; ===========================================================================
; String helpers
; ===========================================================================
; STRLEN - DE=string, returns length in HL (DE preserved)
STRLEN: PUSH    D
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
SCPY:   PUSH    D
SCP1:   MOV     A,M
        STAX    D
        ORA     A
        JZ      SCP2
        INX     H
        INX     D
        JMP     SCP1
SCP2:   POP     D
        RET

; UCMP - unsigned compare HL vs DE: A=0FFH if HL<DE, 0 if =, 1 if HL>DE
UCMP:   MOV     A,H
        CMP     D
        JC      UCLT
        JNZ     UCGT
        MOV     A,L
        CMP     E
        JC      UCLT
        JNZ     UCGT
        XRA     A
        RET
UCLT:   MVI     A,0FFH
        RET
UCGT:   MVI     A,1
        RET

; ===========================================================================
; Heap allocator (same design as EDIT.ASM)
; ===========================================================================
; ALLOC - HL = payload bytes -> HL = pointer (payload) or 0
ALLOC:  LXI     D,2
        DAD     D
        MOV     A,H
        ORA     A
        JNZ     ALSZOK
        MOV     A,L
        CPI     4
        JNC     ALSZOK
        LXI     H,4
ALSZOK: SHLD    ANEED
        LXI     H,FREEL
        SHLD    APREV
        LHLD    FREEL
        SHLD    ACUR
ALOOP:  LHLD    ACUR
        MOV     A,H
        ORA     L
        JZ      ALBUMP
        MOV     E,M
        INX     H
        MOV     D,M
        LHLD    ANEED
        MOV     A,E
        SUB     L
        MOV     A,D
        SBB     H
        JNC     ALFIT
        LHLD    ACUR
        INX     H
        INX     H
        SHLD    APREV
        MOV     E,M
        INX     H
        MOV     D,M
        XCHG
        SHLD    ACUR
        JMP     ALOOP
ALFIT:  LHLD    ACUR
        MOV     E,M
        INX     H
        MOV     D,M
        LHLD    ANEED
        MOV     A,E
        SUB     L
        MOV     C,A
        MOV     A,D
        SBB     H
        MOV     B,A
        MOV     A,B
        ORA     A
        JNZ     ALSPL
        MOV     A,C
        CPI     4
        JC      ALWHOL
ALSPL:  LHLD    ACUR
        XCHG
        LHLD    ANEED
        DAD     D
        SHLD    ANEW
        MOV     M,C
        INX     H
        MOV     M,B
        LHLD    ACUR
        INX     H
        INX     H
        MOV     E,M
        INX     H
        MOV     D,M
        XCHG
        SHLD    ANEXT
        LHLD    ANEXT
        XCHG
        LHLD    ANEW
        INX     H
        INX     H
        MOV     M,E
        INX     H
        MOV     M,D
        LHLD    ANEED
        XCHG
        LHLD    ACUR
        MOV     M,E
        INX     H
        MOV     M,D
        LHLD    ANEW
        XCHG
        LHLD    APREV
        MOV     M,E
        INX     H
        MOV     M,D
        LHLD    ACUR
        INX     H
        INX     H
        RET
ALWHOL: LHLD    ACUR
        INX     H
        INX     H
        MOV     E,M
        INX     H
        MOV     D,M
        LHLD    APREV
        MOV     M,E
        INX     H
        MOV     M,D
        LHLD    ACUR
        INX     H
        INX     H
        RET
ALBUMP: LHLD    BRKP
        XCHG
        LHLD    ANEED
        DAD     D
        XCHG
        LHLD    HEAPND
        MOV     A,L
        SUB     E
        MOV     A,H
        SBB     D
        JC      ALFAIL
        LHLD    BRKP
        SHLD    ATMP
        XCHG
        LHLD    ANEED
        DAD     D
        SHLD    BRKP
        LHLD    ANEED
        XCHG
        LHLD    ATMP
        MOV     M,E
        INX     H
        MOV     M,D
        LHLD    ATMP
        INX     H
        INX     H
        RET
ALFAIL: LXI     H,0
        RET

; FREE - HL = payload pointer; push block onto the free list
FREE:   MOV     A,H
        ORA     L
        RZ
        DCX     H
        DCX     H
        SHLD    ATMP
        LHLD    FREEL
        XCHG
        LHLD    ATMP
        INX     H
        INX     H
        MOV     M,E
        INX     H
        MOV     M,D
        LHLD    ATMP
        SHLD    FREEL
        RET

; ===========================================================================
; Cell table access
; ===========================================================================
; CELADR - HL=row, DE=col -> HL = &cells[row][col]  (2-byte slot address)
; index = (row*MAXCOL + col) * 2
CELADR: PUSH    D               ; save col
        LXI     B,MAXCOL
        CALL    MUL16           ; HL = row*MAXCOL
        POP     D
        DAD     D               ; + col
        DAD     H               ; *2
        LXI     D,CELLS
        DAD     D
        RET

; MUL16 - HL = HL * BC (small unsigned)
MUL16:  XCHG                    ; DE = multiplicand (row)
        LXI     H,0
ML16L:  MOV     A,B
        ORA     C
        RZ
        DAD     D               ; accumulate
        DCX     B
        JMP     ML16L

; GETCEL - HL=row, DE=col -> HL = cells[row][col] (the stored pointer)
GETCEL: CALL    CELADR
        MOV     E,M
        INX     H
        MOV     D,M
        XCHG
        RET

; PUTCEL - HL=row, DE=col, value in (CPVAL) -> cells[row][col]=CPVAL
PUTCEL: CALL    CELADR
        XCHG                    ; DE = slot addr
        LHLD    CPVAL           ; HL = value, DE = slot addr
        MOV     A,L
        STAX    D
        INX     D
        MOV     A,H
        STAX    D
        RET

; ===========================================================================
; SETCEL - row=SCR, col=SCC, string DE.  Free old, alloc+copy new.
;          Returns A=0 ok, A=0FFH fail (out of range / oom).
; ===========================================================================
SETCEL: SHLD    SCRSAV          ; HL had row on entry? - no; use globals
SETCL2:
; bounds checked by caller; free old cell
        LHLD    SCR
        XCHG
        LHLD    SCC
        XCHG                    ; HL=row, DE=col
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      SCNOFR
        CALL    FREE
SCNOFR:
; null out slot first
        LXI     H,0
        SHLD    CPVAL
        LHLD    SCR
        XCHG
        LHLD    SCC
        XCHG
        CALL    PUTCEL
; empty string? -> done
        LHLD    SCSTR
        MOV     A,M
        ORA     A
        JZ      SCOK
; n = strlen; alloc n+1
        XCHG
        LHLD    SCSTR
        XCHG
        CALL    STRLEN          ; HL=len (DE=str preserved? STRLEN preserves DE)
        SHLD    SCLEN
        INX     H
        CALL    ALLOC
        MOV     A,H
        ORA     L
        JZ      SCFAIL
        SHLD    CPVAL
        XCHG                    ; DE=dst
        LHLD    SCSTR           ; HL=src
        CALL    SCPY
        LHLD    SCR
        XCHG
        LHLD    SCC
        XCHG
        CALL    PUTCEL
SCOK:   XRA     A
        RET
SCFAIL: MVI     A,0FFH
        RET

; helper to set SCR/SCC/SCSTR then call SETCL2
; CLRCEL - clear cells[crow][ccol], set dirty if it had content
CLRCEL: LHLD    CROW
        XCHG
        LHLD    CCOL
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        RZ
        CALL    FREE
        LXI     H,0
        SHLD    CPVAL
        LHLD    CROW
        XCHG
        LHLD    CCOL
        XCHG
        CALL    PUTCEL
        MVI     A,1
        STA     DIRT
        RET

; ===========================================================================
; Character classifiers (operate on A)
; ===========================================================================
; ISDIGF - returns A=1 if digit else 0
ISDIGF: CPI     '0'
        JC      ISDF0
        CPI     '9'+1
        JNC     ISDF0
        MVI     A,1
        RET
ISDF0:  XRA     A
        RET

; ISALF - returns A=1 if A-Z or a-z else 0
ISALF:  CPI     'A'
        JC      ISAL0
        CPI     'Z'+1
        JC      ISAL1
        CPI     'a'
        JC      ISAL0
        CPI     'z'+1
        JNC     ISAL0
ISAL1:  MVI     A,1
        RET
ISAL0:  XRA     A
        RET

; UPR - A -> uppercase
UPR:    CPI     'a'
        RC
        CPI     'z'+1
        RNC
        SUI     32
        RET

; ===========================================================================
; 32-bit signed integer math (big-endian: byte0=MSB .. byte3=LSB)
; ===========================================================================
; ZERO4 - HL points to a 4-byte long; set it to 0
ZERO4:  XRA     A
        MOV     M,A
        INX     H
        MOV     M,A
        INX     H
        MOV     M,A
        INX     H
        MOV     M,A
        RET

; COPY4 - copy long at HL(src) to DE(dst)
COPY4:  MVI     B,4
CP4L:   MOV     A,M
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     CP4L
        RET

; SETI4 - HL = signed 16-bit value, DE = dst long -> dst = sign-extend(HL)
SETI4:  MOV     A,H
        RLC                     ; sign bit -> CY
        SBB     A               ; A = 0 if positive, 0FFH if negative
        STAX    D               ; byte0
        INX     D
        STAX    D               ; byte1 (sign extension)
        INX     D
        MOV     A,H
        STAX    D               ; byte2
        INX     D
        MOV     A,L
        STAX    D               ; byte3
        RET

; ---------------------------------------------------------------------------
; ADDL - LARGR = LARGA + LARGB (signed two's complement add)
;        Big-endian: byte index 3 is LSB.  LCARRY holds the running carry.
; ---------------------------------------------------------------------------
ADDL:   LXI     H,3
        SHLD    LIDX
        XRA     A
        STA     LCARRY
ADDLL:  LHLD    LIDX
        LXI     D,LARGB
        DAD     D
        MOV     B,M             ; B = b[idx]
        LHLD    LIDX
        LXI     D,LARGA
        DAD     D
        MOV     A,M             ; A = a[idx]
        MOV     C,A             ; C = a[idx] (save across carry restore)
        LDA     LCARRY          ; restore carry into CY
        RAR                     ; CY = saved carry
        MOV     A,C             ; A = a[idx] again (MOV preserves CY)
        ADC     B               ; A = a + b + carry
        MOV     C,A             ; result byte
        MVI     A,0
        RAL                     ; A = new carry (0/1)
        STA     LCARRY
        LHLD    LIDX
        LXI     D,LARGR
        DAD     D
        MOV     M,C
        LHLD    LIDX
        MOV     A,L
        ORA     A
        RZ
        DCX     H
        SHLD    LIDX
        JMP     ADDLL

; NEGL - LARGR = -LARGA  (two's complement: invert then add 1)
NEGL:   LXI     H,3
        SHLD    LIDX
        MVI     A,1
        STA     LCARRY          ; +1 enters at LSB
NEGLL:  LHLD    LIDX
        LXI     D,LARGA
        DAD     D
        MOV     A,M
        CMA
        MOV     B,A             ; B = ~a[idx]
        LDA     LCARRY
        RAR                     ; CY = carry
        MOV     A,B
        ACI     0               ; A = ~a + carry
        MOV     C,A
        MVI     A,0
        RAL
        STA     LCARRY
        LHLD    LIDX
        LXI     D,LARGR
        DAD     D
        MOV     M,C
        LHLD    LIDX
        MOV     A,L
        ORA     A
        RZ
        DCX     H
        SHLD    LIDX
        JMP     NEGLL

; SUBL - LARGR = LARGA - LARGB.  Implemented as A + (-B).
SUBL:   LXI     H,LARGA         ; preserve original A in LARGA2
        LXI     D,LARGA2
        CALL    COPY4
        LXI     H,LARGB         ; LARGA = B, negate -> LARGR = -B
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR         ; LARGB = -B
        LXI     D,LARGB
        CALL    COPY4
        LXI     H,LARGA2        ; restore LARGA = original A
        LXI     D,LARGA
        CALL    COPY4
        JMP     ADDL            ; LARGR = A + (-B)

; CMPL - signed compare LARGA vs LARGB -> A=0FFH if A<B, 0 if =, 1 if A>B
CMPL:
; high bytes for sign
        LDA     LARGA
        MOV     C,A             ; C = a[0]
        LDA     LARGB
        MOV     B,A             ; B = b[0]
        MOV     A,C
        XRA     B
        JM      CMSDIF          ; signs differ (bit7 set)
; same sign: unsigned compare byte0..3
        LXI     H,0
        SHLD    LIDX
CMPLL:  LHLD    LIDX
        LXI     D,LARGA
        DAD     D
        MOV     C,M             ; C = a[idx]
        LHLD    LIDX
        LXI     D,LARGB
        DAD     D               ; M = b[idx]
        MOV     A,C
        SUB     M               ; A = a[idx] - b[idx]
        JC      CMALT           ; a<b
        JNZ     CMAGT           ; a>b
        LHLD    LIDX
        INX     H
        SHLD    LIDX
        MOV     A,L
        CPI     4
        JNZ     CMPLL
        XRA     A               ; equal
        RET
CMALT:  MVI     A,0FFH
        RET
CMAGT:  MVI     A,1
        RET
CMSDIF:
; signs differ: A negative (bit7 set) -> A<B
        LDA     LARGA
        ANI     80H
        JNZ     CMALT
        JMP     CMAGT

; ===========================================================================
; Arithmetic uses ADDL/SUBL/MULL/DIVL with the LARGA/LARGB/LARGR convention;
; wrappers move operands in and out.
; ===========================================================================
; MULL - LARGR = LARGA * LARGB (signed).  Shift-add on magnitudes.
MULL:
; sign = signA xor signB
        LDA     LARGA
        ANI     80H
        MOV     B,A
        LDA     LARGB
        ANI     80H
        XRA     B
        STA     LMSGN
; magnitude of A -> MCAND
        LDA     LARGA
        ANI     80H
        JZ      MLA0
        LXI     H,LARGA
        LXI     D,MARGA
        CALL    COPY4
        LXI     H,MARGA
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,MCAND
        CALL    COPY4
        LXI     H,MARGA
        LXI     D,LARGA
        CALL    COPY4
        JMP     MLB
MLA0:   LXI     H,LARGA
        LXI     D,MCAND
        CALL    COPY4
MLB:    LDA     LARGB
        ANI     80H
        JZ      MLB0
        LXI     H,LARGB
        LXI     D,MARGB
        CALL    COPY4
        LXI     H,LARGB
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,MPLR
        CALL    COPY4
        LXI     H,MARGB
        LXI     D,LARGB
        CALL    COPY4
        JMP     MLGO
MLB0:   LXI     H,LARGB
        LXI     D,MPLR
        CALL    COPY4
MLGO:
; PROD = 0
        LXI     H,PROD
        CALL    ZERO4
; 32 iterations: if MPLR LSB set, PROD += MCAND; MCAND <<=1; MPLR >>=1
        MVI     A,32
        STA     MITER
MLIT:   LDA     MPLR+3
        ANI     1
        JZ      MLNOAD
; PROD += MCAND  (use ADDL with PROD,MCAND -> PROD)
        LXI     H,PROD
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,MCAND
        LXI     D,LARGB
        CALL    COPY4
        CALL    ADDL
        LXI     H,LARGR
        LXI     D,PROD
        CALL    COPY4
MLNOAD:
; MCAND <<= 1
        LXI     H,MCAND
        CALL    SHL4
; MPLR >>= 1 (logical)
        LXI     H,MPLR
        CALL    SHR4
        LDA     MITER
        DCR     A
        STA     MITER
        JNZ     MLIT
; apply sign
        LDA     LMSGN
        ORA     A
        JZ      MLPOS
        LXI     H,PROD
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        RET
MLPOS:  LXI     H,PROD
        LXI     D,LARGR
        CALL    COPY4
        RET

; SHL4 - long at HL <<= 1 (HL points to byte0=MSB).  Shift LSB->MSB.
SHL4:   PUSH    H
        LXI     D,3
        DAD     D               ; HL -> byte3 (LSB)
        XRA     A               ; carry=0
        MVI     B,4
SHL4L:  MOV     A,M
        RAL
        MOV     M,A
        DCX     H
        DCR     B
        JNZ     SHL4L
        POP     H
        RET

; SHR4 - long at HL >>= 1 logical (HL points to byte0=MSB).
SHR4:   PUSH    H
        ORA     A               ; carry=0
        MVI     B,4
SHR4L:  MOV     A,M
        RAR
        MOV     M,A
        INX     H
        DCR     B
        JNZ     SHR4L
        POP     H
        RET

; DIVL - LARGR = LARGA / LARGB (signed, truncating).  LREM = remainder.
; Caller must ensure LARGB <> 0.
DIVL:
        LDA     LARGA
        ANI     80H
        MOV     B,A
        LDA     LARGB
        ANI     80H
        XRA     B
        STA     LDSGN           ; quotient sign
; |A| -> DDVD
        LDA     LARGA
        ANI     80H
        JZ      DVA0
        LXI     H,LARGA
        LXI     D,DARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,DDVD
        CALL    COPY4
        LXI     H,DARGA
        LXI     D,LARGA
        CALL    COPY4
        JMP     DVB
DVA0:   LXI     H,LARGA
        LXI     D,DDVD
        CALL    COPY4
DVB:    LDA     LARGB
        ANI     80H
        JZ      DVB0
        LXI     H,LARGB
        LXI     D,DARGB
        CALL    COPY4
        LXI     H,LARGB
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,DDVS
        CALL    COPY4
        LXI     H,DARGB
        LXI     D,LARGB
        CALL    COPY4
        JMP     DVGO
DVB0:   LXI     H,LARGB
        LXI     D,DDVS
        CALL    COPY4
DVGO:
; restoring division: REM=0, QUOT=0, 32 bits
        LXI     H,DREM
        CALL    ZERO4
        LXI     H,DQUO
        CALL    ZERO4
        MVI     A,32
        STA     DITER
DVIT:
; REM <<= 1
        LXI     H,DREM
        CALL    SHL4
; shift DDVD left; its old MSB falls out into CY
        LXI     H,DDVD
        CALL    SHL4
; CY (preserved across SHL4's POP/RET) is the bit shifted out of DDVD's MSB.
; Pull it into REM's LSB (REM LSB is 0 after the shift above).
        JNC     DVNOC
        LDA     DREM+3
        ORI     1
        STA     DREM+3
DVNOC:
; QUOT <<= 1
        LXI     H,DQUO
        CALL    SHL4
; if REM >= DDVS: REM -= DDVS; QUOT |= 1
        LXI     H,DREM
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,DDVS
        LXI     D,LARGB
        CALL    COPY4
        CALL    UCMP4           ; A=0FFH if REM<DDVS, else >=
        CPI     0FFH
        JZ      DVNS
; REM -= DDVS
        LXI     H,DREM
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,DDVS
        LXI     D,LARGB
        CALL    COPY4
        CALL    SUBL
        LXI     H,LARGR
        LXI     D,DREM
        CALL    COPY4
        LDA     DQUO+3
        ORI     1
        STA     DQUO+3
DVNS:   LDA     DITER
        DCR     A
        STA     DITER
        JNZ     DVIT
; remainder sign = sign of dividend (A); apply quotient sign
        LXI     H,DREM
        LXI     D,LREM
        CALL    COPY4
        LDA     LARGA           ; note LARGA may have been restored to orig
        LDA     LDSGN
        ORA     A
        JZ      DVPOS
        LXI     H,DQUO
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        RET                     ; LARGR = -QUOT
DVPOS:  LXI     H,DQUO
        LXI     D,LARGR
        CALL    COPY4
        RET

; UCMP4 - unsigned compare LARGA vs LARGB -> A=0FFH if A<B, 0 if =, 1 if A>B
UCMP4:  LXI     H,0
        SHLD    LIDX
UC4L:   LHLD    LIDX
        LXI     D,LARGA
        DAD     D
        MOV     C,M             ; a[idx]
        LHLD    LIDX
        LXI     D,LARGB
        DAD     D
        MOV     A,M             ; b[idx]
        MOV     B,A
        MOV     A,C
        SUB     B
        JC      UC4LT
        JNZ     UC4GT
        LHLD    LIDX
        INX     H
        SHLD    LIDX
        MOV     A,L
        CPI     4
        JNZ     UC4L
        XRA     A
        RET
UC4LT:  MVI     A,0FFH
        RET
UC4GT:  MVI     A,1
        RET

; ===========================================================================
; ATOL - DE=string -> ACC long.  Reads optional '-' then digits.
; ===========================================================================
ATOL:   SHLD    ATSAV
        XCHG
        SHLD    ATPTR
        LXI     H,ACC
        CALL    ZERO4
        XRA     A
        STA     ATNEG
        LHLD    ATPTR
        MOV     A,M
        CPI     '-'
        JNZ     ATL
        MVI     A,1
        STA     ATNEG
        LHLD    ATPTR
        INX     H
        SHLD    ATPTR
ATL:    LHLD    ATPTR
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      ATEND
; ACC = ACC*10 + digit
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,LTEN
        LXI     D,LARGB
        CALL    COPY4
        CALL    MULL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        LHLD    ATPTR
        MOV     A,M
        SUI     '0'
        MOV     L,A
        MVI     H,0
        LXI     D,DIGT
        CALL    SETI4
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,DIGT
        LXI     D,LARGB
        CALL    COPY4
        CALL    ADDL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        LHLD    ATPTR
        INX     H
        SHLD    ATPTR
        JMP     ATL
ATEND:  LDA     ATNEG
        ORA     A
        RZ
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        RET

; ===========================================================================
; LTOA - convert ACC long to ASCII in LTBUF (NUL terminated).
; ===========================================================================
LTOA:   LXI     H,LTBUF
        SHLD    LTOUT
; sign
        LDA     ACC
        ANI     80H
        JZ      LTPOS
        LHLD    LTOUT
        MVI     M,'-'
        INX     H
        SHLD    LTOUT
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,LABS
        CALL    COPY4
        JMP     LTGEN
LTPOS:  LXI     H,ACC
        LXI     D,LABS
        CALL    COPY4
LTGEN:
; generate digits least-significant first into LWORK, then reverse
        LXI     H,LWORK
        SHLD    LWP
        MVI     A,0
        STA     LDCNT
LTLP:
; digit = LABS mod 10 ; LABS = LABS / 10
        LXI     H,LABS
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,LTEN
        LXI     D,LARGB
        CALL    COPY4
        CALL    DIVL            ; LARGR=quot, LREM=rem
        LDA     LREM+3
        ADI     '0'
        LHLD    LWP
        MOV     M,A
        INX     H
        SHLD    LWP
        LDA     LDCNT
        INR     A
        STA     LDCNT
        LXI     H,LARGR
        LXI     D,LABS
        CALL    COPY4
; if LABS <> 0 continue
        LXI     H,LABS
        CALL    ISZ4
        ORA     A
        JZ      LTLP
; reverse digits from LWORK into LTOUT
LTREV:  LDA     LDCNT
        ORA     A
        JZ      LTDON
        LHLD    LWP
        DCX     H
        SHLD    LWP
        MOV     A,M
        LHLD    LTOUT
        MOV     M,A
        INX     H
        SHLD    LTOUT
        LDA     LDCNT
        DCR     A
        STA     LDCNT
        JMP     LTREV
LTDON:  LHLD    LTOUT
        MVI     M,0
        RET

; ISZ4 - HL points to long; A=1 if zero else 0
ISZ4:   MOV     A,M
        INX     H
        ORA     M
        INX     H
        ORA     M
        INX     H
        ORA     M
        JZ      ISZ4Y
        XRA     A
        RET
ISZ4Y:  MVI     A,1
        RET

; ===========================================================================
; Expression evaluator.  Parses from (EPOS); result long in ACC.
; EOK=1 ok, EOK=0 error.  Uses recursion via stack for left operands.
; ===========================================================================
; ESKP - skip spaces/tabs at EPOS
ESKP:   LHLD    EPOS
        MOV     A,M
        CPI     ' '
        JZ      ESK1
        CPI     9
        RNZ
ESK1:   LHLD    EPOS
        INX     H
        SHLD    EPOS
        JMP     ESKP

; PRSREF - parse cell ref at EPOS -> PRROW,PRCOL (0-based). A=1 ok, 0 fail.
PRSREF: CALL    ESKP
        LHLD    EPOS
        MOV     A,M
        CALL    ISALF
        ORA     A
        JZ      PRF0
        LHLD    EPOS
        MOV     A,M
        CALL    UPR
        SUI     'A'
        MOV     L,A
        MVI     H,0
        SHLD    PRCOL
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      PRF0
        LXI     H,0
        SHLD    PRROW
PRFD:   LHLD    EPOS
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      PRFDN
; row = row*10 + digit
        LHLD    EPOS
        MOV     A,M
        SUI     '0'
        MOV     C,A             ; C = digit (save before HL is reused)
        LHLD    PRROW
        MOV     D,H
        MOV     E,L
        DAD     H               ; *2
        DAD     H               ; *4
        DAD     D               ; *5
        DAD     H               ; *10
        MOV     A,C             ; digit
        MOV     E,A
        MVI     D,0
        DAD     D
        SHLD    PRROW
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        JMP     PRFD
PRFDN:  LHLD    PRROW
        DCX     H
        SHLD    PRROW           ; 0-based
; bounds: row 0..MAXROW-1, col 0..MAXCOL-1
        LHLD    PRROW
        LXI     D,MAXROW
        CALL    SCMP4
        CPI     0FFH
        JNZ     PRF0
        LHLD    PRROW
        LXI     D,0
        CALL    SCMP4
        CPI     0FFH
        JZ      PRF0
        LHLD    PRCOL
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     PRF0
        MVI     A,1
        RET
PRF0:   XRA     A
        RET

; SCMP4 - signed 16-bit compare HL vs DE -> 0FFH if HL<DE,0 if =,1 if HL>DE
SCMP4:  MOV     A,H
        XRA     D
        JM      SC4DIF
        MOV     A,H
        CMP     D
        JC      SC4LT
        JNZ     SC4GT
        MOV     A,L
        CMP     E
        JC      SC4LT
        JNZ     SC4GT
        XRA     A
        RET
SC4LT:  MVI     A,0FFH
        RET
SC4GT:  MVI     A,1
        RET
SC4DIF: MOV     A,H
        ORA     A
        JM      SC4LT
        JMP     SC4GT

; ===========================================================================
; EVCELL - row=EVR, col=EVC -> ACC = value of cell.  A/EOK status.
; Recursion guarded by EDEPTH.  Saves EPOS across formula evaluation.
; ===========================================================================
EVCELL: LXI     H,ACC
        CALL    ZERO4
; bounds
        LHLD    EVR
        LXI     D,MAXROW
        CALL    SCMP4
        CPI     0FFH
        JNZ     EVBAD
        LHLD    EVR
        LXI     D,0
        CALL    SCMP4
        CPI     0FFH
        JZ      EVBAD
        LHLD    EVC
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     EVBAD
        LHLD    EVR
        XCHG
        LHLD    EVC
        XCHG
        CALL    GETCEL          ; HL = cell ptr
        MOV     A,H
        ORA     L
        JZ      EVEMP           ; empty -> ACC=0 ok
        SHLD    EVS             ; save ptr
        MOV     A,M
        ORA     A
        JZ      EVEMP
        CPI     '='
        JZ      EVFRM
; plain text/number: take leading signed integer
        LHLD    EVS
        MOV     A,M
        CPI     '-'
        JZ      EVNUM
        CALL    ISDIGF
        ORA     A
        JZ      EVOK            ; not numeric -> ACC stays 0
EVNUM:  LHLD    EVS
        XCHG
        CALL    ATOL            ; ACC = value
        JMP     EVOK
EVFRM:  LDA     EDEPTH
        CPI     25
        JNC     EVBAD           ; > 24 -> error
        LDA     EDEPTH
        INR     A
        STA     EDEPTH
        LHLD    EPOS
        SHLD    EVSEP           ; save EPOS
        LHLD    EVS
        INX     H               ; skip '='
        SHLD    EPOS
        MVI     A,1
        STA     EOK
        CALL    EXPR            ; ACC = expr value
        LHLD    EVSEP
        SHLD    EPOS
        LDA     EDEPTH
        DCR     A
        STA     EDEPTH
        LDA     EOK
        ORA     A
        JZ      EVBAD
        JMP     EVOK
EVEMP:  LXI     H,ACC
        CALL    ZERO4
EVOK:   MVI     A,1
        STA     EOK
        RET
EVBAD:  XRA     A
        STA     EOK
        RET

; ===========================================================================
; EXPR - parse additive expr at EPOS into ACC.  Left operand kept on stack.
; ===========================================================================
EXPR:   CALL    TERM
        LDA     EOK
        ORA     A
        RZ
EXPRL:  CALL    ESKP
        LHLD    EPOS
        MOV     A,M
        CPI     '+'
        JZ      EXADD
        CPI     '-'
        JZ      EXSUB
        RET
EXADD:  MVI     A,1
        STA     EXOP
        JMP     EXGO
EXSUB:  XRA     A
        STA     EXOP
EXGO:
; save ACC (left) on stack, advance, eval TERM into ACC (right)
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        LXI     H,ACC
        LXI     D,EXSAV
        CALL    COPY4           ; EXSAV = left
        CALL    TERM
        LDA     EOK
        ORA     A
        RZ
; LARGA=left(EXSAV), LARGB=ACC(right)
        LXI     H,EXSAV
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,ACC
        LXI     D,LARGB
        CALL    COPY4
        LDA     EXOP
        ORA     A
        JZ      EXDS
        CALL    ADDL
        JMP     EXST
EXDS:   CALL    SUBL
EXST:   LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        JMP     EXPRL

; ===========================================================================
; TERM - parse multiplicative term at EPOS into ACC.
; ===========================================================================
TERM:   CALL    FACTOR
        LDA     EOK
        ORA     A
        RZ
TERML:  CALL    ESKP
        LHLD    EPOS
        MOV     A,M
        CPI     '*'
        JZ      TMUL
        CPI     '/'
        JZ      TDIV
        RET
TMUL:   MVI     A,1
        STA     TMOP
        JMP     TMGO
TDIV:   XRA     A
        STA     TMOP
TMGO:   LHLD    EPOS
        INX     H
        SHLD    EPOS
        LXI     H,ACC
        LXI     D,TMSAV
        CALL    COPY4           ; TMSAV = left
        CALL    FACTOR
        LDA     EOK
        ORA     A
        RZ
        LXI     H,TMSAV
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,ACC
        LXI     D,LARGB
        CALL    COPY4
        LDA     TMOP
        ORA     A
        JZ      TMDIV
        CALL    MULL
        JMP     TMST
TMDIV:
; check divisor (LARGB) <> 0
        LXI     H,LARGB
        CALL    ISZ4
        ORA     A
        JNZ     TMDZ
        CALL    DIVL
        JMP     TMST
TMDZ:   XRA     A
        STA     EOK
        RET
TMST:   LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        JMP     TERML

; ===========================================================================
; FACTOR - parse a factor at EPOS into ACC.
; ===========================================================================
FACTOR: CALL    ESKP
        XRA     A
        STA     FNEG
FNGL:   LHLD    EPOS
        MOV     A,M
        CPI     '-'
        JZ      FNMINUS
        CPI     '+'
        JZ      FNPLUS
        JMP     FNDONE
FNMINUS:LDA     FNEG
        XRI     1
        STA     FNEG
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        CALL    ESKP
        JMP     FNGL
FNPLUS: LHLD    EPOS
        INX     H
        SHLD    EPOS
        CALL    ESKP
        JMP     FNGL
FNDONE:
        LHLD    EPOS
        MOV     A,M
        CPI     '('
        JZ      FPAREN
        CALL    ISDIGF
        ORA     A
        JNZ     FNUMB
        CALL    FNFUNC          ; try SUM/AVG/MIN/MAX/COUNT; A=1 if matched
        ORA     A
        JZ      FNNOMT          ; A=0 -> not a function, try cell ref
        LDA     EOK             ; matched: honor EOK before FAPPLY (FAPPLY
        ORA     A               ; would reset EOK=1 and swallow range errors,
        RZ                      ; e.g. a circular ref inside the range)
        JMP     FAPPLY
FNNOMT: LHLD    EPOS
        MOV     A,M
        CALL    ISALF
        ORA     A
        JNZ     FREF
        XRA     A               ; error
        STA     EOK
        RET

FPAREN: LHLD    EPOS
        INX     H
        SHLD    EPOS
        CALL    EXPR
        LDA     EOK
        ORA     A
        RZ
        CALL    ESKP
        LHLD    EPOS
        MOV     A,M
        CPI     ')'
        JNZ     FERR
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        JMP     FAPPLY

FNUMB:  LXI     H,ACC
        CALL    ZERO4
FNUML:  LHLD    EPOS
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      FAPPLY
; ACC = ACC*10 + digit
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,LTEN
        LXI     D,LARGB
        CALL    COPY4
        CALL    MULL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        LHLD    EPOS
        MOV     A,M
        SUI     '0'
        MOV     L,A
        MVI     H,0
        LXI     D,DIGT
        CALL    SETI4
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,DIGT
        LXI     D,LARGB
        CALL    COPY4
        CALL    ADDL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        JMP     FNUML

FREF:   CALL    PRSREF
        ORA     A
        JZ      FERR
        LHLD    PRROW
        SHLD    EVR
        LHLD    PRCOL
        SHLD    EVC
        CALL    EVCELL
        LDA     EOK
        ORA     A
        RZ
        JMP     FAPPLY

FERR:   XRA     A
        STA     EOK
        RET

FAPPLY: LDA     FNEG
        ORA     A
        JZ      FRET
; ACC = -ACC
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        CALL    NEGL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
FRET:   MVI     A,1
        STA     EOK
        RET

; ===========================================================================
; FNFUNC - if EPOS starts with SUM/AVG/MIN/MAX/COUNT '(' parse range fn.
;          Returns A=1 (handled, ACC set) or A=0 (no match, EPOS unmoved).
;          On error sets EOK=0 and returns A=1.
; ===========================================================================
FNFUNC:
; check 3-letter names with '(' at [3]; COUNT is 5 letters with '(' at [5]
        LHLD    EPOS
        MOV     A,M
        CALL    UPR
        MOV     B,A             ; B = upper(c0)
; SUM
        CPI     'S'
        JNZ     FNCKA
        CALL    CK3
        CPI     'U'
        JNZ     FNNO
        INX     H
        MOV     A,M
        CALL    UPR
        CPI     'M'
        JNZ     FNNO
        INX     H
        MOV     A,M
        CPI     '('
        JNZ     FNNO
        MVI     A,0
        STA     FTAG
        LHLD    EPOS
        LXI     D,4
        DAD     D
        SHLD    EPOS
        JMP     FNRNG
FNCKA:  CPI     'A'
        JNZ     FNCKI
        CALL    CK3
        CPI     'V'
        JNZ     FNNO
        INX     H
        MOV     A,M
        CALL    UPR
        CPI     'G'
        JNZ     FNNO
        INX     H
        MOV     A,M
        CPI     '('
        JNZ     FNNO
        MVI     A,1
        STA     FTAG
        LHLD    EPOS
        LXI     D,4
        DAD     D
        SHLD    EPOS
        JMP     FNRNG
FNCKI:  CPI     'M'
        JNZ     FNCKC
; MIN or MAX
        LHLD    EPOS
        INX     H
        MOV     A,M
        CALL    UPR
        CPI     'I'
        JZ      FNMIN
        CPI     'A'
        JZ      FNMAX
        JMP     FNNO
FNMIN:  LHLD    EPOS
        LXI     D,2
        DAD     D
        MOV     A,M
        CALL    UPR
        CPI     'N'
        JNZ     FNNO
        INX     H
        MOV     A,M
        CPI     '('
        JNZ     FNNO
        MVI     A,2
        STA     FTAG
        LHLD    EPOS
        LXI     D,4
        DAD     D
        SHLD    EPOS
        JMP     FNRNG
FNMAX:  LHLD    EPOS
        LXI     D,2
        DAD     D
        MOV     A,M
        CALL    UPR
        CPI     'X'
        JNZ     FNNO
        INX     H
        MOV     A,M
        CPI     '('
        JNZ     FNNO
        MVI     A,3
        STA     FTAG
        LHLD    EPOS
        LXI     D,4
        DAD     D
        SHLD    EPOS
        JMP     FNRNG
FNCKC:  CPI     'C'
        JNZ     FNNO
; COUNT
        LHLD    EPOS
        INX     H
        MOV     A,M
        CALL    UPR
        CPI     'O'
        JNZ     FNNO
        LHLD    EPOS
        LXI     D,2
        DAD     D
        MOV     A,M
        CALL    UPR
        CPI     'U'
        JNZ     FNNO
        LHLD    EPOS
        LXI     D,3
        DAD     D
        MOV     A,M
        CALL    UPR
        CPI     'N'
        JNZ     FNNO
        LHLD    EPOS
        LXI     D,4
        DAD     D
        MOV     A,M
        CALL    UPR
        CPI     'T'
        JNZ     FNNO
        LHLD    EPOS
        LXI     D,5
        DAD     D
        MOV     A,M
        CPI     '('
        JNZ     FNNO
        MVI     A,4
        STA     FTAG
        LHLD    EPOS
        LXI     D,6
        DAD     D
        SHLD    EPOS
        JMP     FNRNG
FNNO:   XRA     A
        RET

; CK3 - HL=EPOS+1, A = upper char at EPOS+1 (helper to shorten code)
CK3:    LHLD    EPOS
        INX     H
        MOV     A,M
        CALL    UPR
        RET

; FNRNG - parse "ref:ref)" and apply FTAG aggregate into ACC.
FNRNG:  CALL    PRSREF
        ORA     A
        JZ      FNRER
        LHLD    PRROW
        SHLD    RGR1
        LHLD    PRCOL
        SHLD    RGC1
        CALL    ESKP
        LHLD    EPOS
        MOV     A,M
        CPI     ':'
        JNZ     FNRER
        LHLD    EPOS
        INX     H
        SHLD    EPOS
        CALL    PRSREF
        ORA     A
        JZ      FNRER
        LHLD    PRROW
        SHLD    RGR2
        LHLD    PRCOL
        SHLD    RGC2
        CALL    ESKP
        LHLD    EPOS
        MOV     A,M
        CPI     ')'
        JNZ     FNRER
        LHLD    EPOS
        INX     H
        SHLD    EPOS
; accumulate
        LXI     H,ACC
        CALL    ZERO4
        LXI     H,RSUM
        CALL    ZERO4
        LXI     H,0
        SHLD    RGCNT
        XRA     A
        STA     RGGOT
        LHLD    RGR1
        SHLD    RGI
RGIL:   LHLD    RGR2
        XCHG
        LHLD    RGI
        CALL    SCMP4           ; i vs r2 : stop if i>r2
        CPI     1
        JZ      RGDONE
        LHLD    RGC1
        SHLD    RGJ
RGJL:   LHLD    RGC2
        XCHG
        LHLD    RGJ
        CALL    SCMP4
        CPI     1
        JZ      RGNEXTI
; process cell (RGI,RGJ)
        LDA     FTAG
        CPI     4
        JNZ     RGEVAL
; COUNT: if non-empty bump cnt
        LHLD    RGI
        XCHG
        LHLD    RGJ
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      RGJN
        MOV     A,M
        ORA     A
        JZ      RGJN
        LHLD    RGCNT
        INX     H
        SHLD    RGCNT
        JMP     RGJN
RGEVAL: LHLD    RGI
        SHLD    EVR
        LHLD    RGJ
        SHLD    EVC
        CALL    EVCELL
        LDA     EOK
        ORA     A
        JZ      FNRE2
        LHLD    RGCNT
        INX     H
        SHLD    RGCNT
        LDA     FTAG
        CPI     2
        JZ      RGMIN
        CPI     3
        JZ      RGMAX
; SUM or AVG: ACC += value
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
; current cell value is also in ACC after EVCELL... need separate.
; EVCELL wrote ACC; save it then re-add. We must add cell value to running.
; Fix: keep running sum in RSUM.
        JMP     RGADD
RGMIN:  CALL    RGMM
        JMP     RGJN
RGMAX:  CALL    RGMM
        JMP     RGJN
RGADD:
; running sum lives in RSUM; ACC currently holds cell value
        LXI     H,RSUM
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,ACC
        LXI     D,LARGB
        CALL    COPY4
        CALL    ADDL
        LXI     H,LARGR
        LXI     D,RSUM
        CALL    COPY4
RGJN:   LHLD    RGJ
        INX     H
        SHLD    RGJ
        JMP     RGJL
RGNEXTI:LHLD    RGI
        INX     H
        SHLD    RGI
        JMP     RGIL
RGDONE:
; finalize per tag
        LDA     FTAG
        CPI     4
        JZ      RGCNTV
        CPI     2
        JZ      RGMRES
        CPI     3
        JZ      RGMRES
; SUM or AVG: ACC = RSUM (then divide for AVG)
        LXI     H,RSUM
        LXI     D,ACC
        CALL    COPY4
        LDA     FTAG
        CPI     1
        JNZ     RGOK
; AVG: cnt==0 -> error
        LHLD    RGCNT
        MOV     A,H
        ORA     L
        JZ      FNRER
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        LHLD    RGCNT
        LXI     D,DIVI
        CALL    SETI4
        LXI     H,DIVI
        LXI     D,LARGB
        CALL    COPY4
        CALL    DIVL
        LXI     H,LARGR
        LXI     D,ACC
        CALL    COPY4
        JMP     RGOK
RGCNTV: LHLD    RGCNT
        LXI     D,ACC
        CALL    SETI4
        JMP     RGOK
RGMRES: LXI     H,RMM
        LXI     D,ACC
        CALL    COPY4
RGOK:   MVI     A,1
        STA     EOK
        RET
FNRER:  XRA     A
        STA     EOK
        MVI     A,1
        RET
FNRE2:  XRA     A
        STA     EOK
        MVI     A,1
        RET

; RGMM - MIN/MAX helper: cell value in ACC, FTAG 2=min 3=max, RMM holds best
RGMM:   LDA     RGGOT
        ORA     A
        JZ      RGMSET
; compare ACC vs RMM
        LXI     H,ACC
        LXI     D,LARGA
        CALL    COPY4
        LXI     H,RMM
        LXI     D,LARGB
        CALL    COPY4
        CALL    CMPL            ; A=0FFH if ACC<RMM, 1 if ACC>RMM
        MOV     B,A
        LDA     FTAG
        CPI     2
        JZ      RGMNCK
; MAX: replace if ACC>RMM
        MOV     A,B
        CPI     1
        JNZ     RGMRET
        JMP     RGMSET
RGMNCK: MOV     A,B
        CPI     0FFH
        JNZ     RGMRET
RGMSET: LXI     H,ACC
        LXI     D,RMM
        CALL    COPY4
        MVI     A,1
        STA     RGGOT
RGMRET: RET

; ===========================================================================
; RNDCEL - render cell row=RNR,col=RNCOL into RBUF (CWID chars + NUL).
; ===========================================================================
RNDCEL:
; fill RBUF with spaces
        LXI     H,RBUF
        MVI     B,CWID
RNCSP:  MVI     M,' '
        INX     H
        DCR     B
        JNZ     RNCSP
        MVI     M,0             ; RBUF[CWID]=0
        LHLD    RNR
        XCHG
        LHLD    RNCOL
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        RZ                      ; empty -> blank
        SHLD    RNS
        MOV     A,M
        ORA     A
        RZ
        CPI     '='
        JZ      RNFRM
; plain: numeric right-align if starts - or digit, else left
        LHLD    RNS
        MOV     A,M
        CPI     '-'
        JZ      RNNUM
        CALL    ISDIGF
        ORA     A
        JNZ     RNNUM
; text: left align, truncate to CWID
        LHLD    RNS
        XCHG
        CALL    STRLEN
        SHLD    RNLEN
        LXI     D,CWID
        CALL    UCMP
        CPI     1
        JNZ     RNTXOK
        LXI     H,CWID
        SHLD    RNLEN
RNTXOK: LHLD    RNS
        XCHG                    ; DE=src
        LXI     H,RBUF
        LHLD    RNLEN
        MOV     B,L
        LXI     H,RBUF
RNTXL:  MOV     A,B
        ORA     A
        RZ
        LDAX    D
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JMP     RNTXL
RNNUM:  LHLD    RNS
        XCHG
        CALL    STRLEN
        SHLD    RNLEN
        LXI     D,CWID
        CALL    UCMP
        CPI     1
        JNZ     RNNROK
        JMP     RNHASH
RNNROK:
; p = CWID - n ; copy s into RBUF[p..]
        LXI     H,CWID
        XCHG
        LHLD    RNLEN
        MOV     A,E
        SUB     L
        MOV     C,A             ; C = p
        LHLD    RNS
        XCHG                    ; DE=src
        LXI     H,RBUF
        MVI     B,0
        MOV     A,C
        ADD     L
        MOV     L,A             ; HL = RBUF+p
RNNL:   LDAX    D
        ORA     A
        RZ
        MOV     M,A
        INX     H
        INX     D
        JMP     RNNL
RNFRM:  XRA     A
        STA     EDEPTH
        LHLD    RNR
        SHLD    EVR
        LHLD    RNCOL
        SHLD    EVC
        CALL    EVCELL
        LDA     EOK
        ORA     A
        JZ      RNERR
        CALL    LTOA            ; LTBUF = ascii
        LXI     D,LTBUF
        CALL    STRLEN
        SHLD    RNLEN
        LXI     D,CWID
        CALL    UCMP
        CPI     1
        JZ      RNHASH          ; n>CWID
; right align LTBUF into RBUF
        LXI     H,CWID
        XCHG
        LHLD    RNLEN
        MOV     A,E
        SUB     L
        MOV     C,A
        LXI     D,LTBUF
        LXI     H,RBUF
        MVI     B,0
        MOV     A,C
        ADD     L
        MOV     L,A
RNFL:   LDAX    D
        ORA     A
        RZ
        MOV     M,A
        INX     H
        INX     D
        JMP     RNFL
RNERR:  LXI     H,TERRC
        LXI     D,RBUF
        CALL    SCPY
        RET
RNHASH: LXI     H,RBUF
        MVI     B,CWID
RNHL:   MVI     M,'#'
        INX     H
        DCR     B
        JNZ     RNHL
        MVI     M,0
        RET

; ===========================================================================
; VT100 helpers
; ===========================================================================
; CURMV - B=row, C=col
CURMV:  MOV     A,B
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

CLRS:   LXI     H,E2J
        CALL    PESC
        MVI     B,1
        MVI     C,1
        JMP     CURMV
EOL:    LXI     H,EK
        JMP     PESC
INVON:  LXI     H,E7M
        JMP     PESC
INVOF:  LXI     H,E0M
        JMP     PESC
SHWCR:  LXI     H,ECSH
        JMP     PESC
HIDCR:  LXI     H,ECSL
        JMP     PESC

; ===========================================================================
; MSG - HL=src: copy to MESG (max 62 chars)
; ===========================================================================
MSG:    LXI     D,MESG
        MVI     B,62
MSGL:   MOV     A,M
        ORA     A
        JZ      MSGE
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     MSGL
MSGE:   XRA     A
        STAX    D
        RET

; ===========================================================================
; Title bar
; ===========================================================================
TITLN:  MVI     B,TITR
        MVI     C,1
        CALL    CURMV
        LXI     H,TTLCOL
        CALL    CPUT
        LDA     ENAME
        ORA     A
        JZ      TIT1
        LXI     H,ENAME
        CALL    CPUT
        JMP     TIT2
TIT1:   LXI     H,TNOF
        CALL    CPUT
TIT2:   LDA     DIRT
        ORA     A
        JZ      TIT3
        LXI     H,TSTAR
        CALL    CPUT
TIT3:   CALL    EOL
        LXI     H,E0M
        JMP     PESC

; ===========================================================================
; DRWHDR - column header row
; ===========================================================================
DRWHDR: MVI     B,HDRR
        MVI     C,1
        CALL    CURMV
        LXI     H,HDRCOL
        CALL    CPUT
        LXI     H,T4SP
        CALL    CPUT
        LXI     H,0
        SHLD    DHI
DHL:    LHLD    DHI
        LXI     D,VCOLS
        CALL    UCMP
        CPI     0FFH
        JNZ     DHEND
; col = tcol + i
        LHLD    TCOL
        XCHG
        LHLD    DHI
        DAD     D
        SHLD    DHCOL
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JZ      DHOK
        LXI     H,T10SP
        CALL    CPUT
        JMP     DHNXT
DHOK:
; pad = (CWID-1)/2 = 4 ; print 4 spaces, letter, 5 spaces
        MVI     B,4
DHPL:   MVI     A,' '
        CALL    CHOUT
        DCR     B
        JNZ     DHPL
        LHLD    DHCOL
        MOV     A,L
        ADI     'A'
        CALL    CHOUT
        MVI     B,5
DHPR:   MVI     A,' '
        CALL    CHOUT
        DCR     B
        JNZ     DHPR
DHNXT:  LHLD    DHI
        INX     H
        SHLD    DHI
        JMP     DHL
DHEND:  CALL    EOL
        LXI     H,E0M
        JMP     PESC

; ===========================================================================
; DRWROW - HL = visual row (0..VROWS-1)
; ===========================================================================
DRWROW: SHLD    DRVR
; r = trow + vr
        LHLD    TROW
        XCHG
        LHLD    DRVR
        DAD     D
        SHLD    DRR
; curmv(DATR+vr, 1)
        LHLD    DRVR
        MOV     A,L
        ADI     DATR
        MOV     B,A
        MVI     C,1
        CALL    CURMV
        LHLD    DRR
        LXI     D,MAXROW
        CALL    UCMP
        CPI     0FFH
        JZ      DRROK
        LXI     H,E0M
        CALL    PESC
        JMP     EOL
DRROK:
; gutter: amber, 3-digit row number
        LXI     H,HDRCOL
        CALL    CPUT
        LHLD    DRR
        INX     H               ; r+1
        SHLD    DRRN
        LXI     D,10
        CALL    UCMP
        CPI     0FFH
        JNZ     DRG2            ; r+1 >= 10
        LXI     H,T2SP
        CALL    CPUT
        JMP     DRGN
DRG2:   LHLD    DRRN
        LXI     D,100
        CALL    UCMP
        CPI     0FFH
        JNZ     DRGN            ; >=100
        MVI     A,' '
        CALL    CHOUT
DRGN:   LHLD    DRRN
        CALL    NUMP
        MVI     A,' '
        CALL    CHOUT
        LXI     H,E0M
        CALL    PESC
; columns
        LXI     H,0
        SHLD    DRI
DRCL:   LHLD    DRI
        LXI     D,VCOLS
        CALL    UCMP
        CPI     0FFH
        JNZ     DRCEND
        LHLD    TCOL
        XCHG
        LHLD    DRI
        DAD     D
        SHLD    DRC
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JZ      DRCOK
        LXI     H,T10SP
        CALL    CPUT
        JMP     DRCNX
DRCOK:  LHLD    DRR
        SHLD    RNR
        LHLD    DRC
        SHLD    RNCOL
        CALL    RNDCEL
; cursor cell? r==crow && c==ccol
        LHLD    DRR
        XCHG
        LHLD    CROW
        CALL    UCMP
        ORA     A
        JNZ     DRCNORM
        LHLD    DRC
        XCHG
        LHLD    CCOL
        CALL    UCMP
        ORA     A
        JNZ     DRCNORM
        LXI     H,CURCOL
        CALL    CPUT
        JMP     DRCPUT
DRCNORM:LXI     H,CELCOL
        CALL    CPUT
DRCPUT: LXI     H,RBUF
        CALL    CPUT
        LXI     H,E0M
        CALL    PESC
DRCNX:  LHLD    DRI
        INX     H
        SHLD    DRI
        JMP     DRCL
DRCEND: JMP     EOL

; ===========================================================================
; STATLN - status line
; ===========================================================================
STATLN: MVI     B,STATR
        MVI     C,1
        CALL    CURMV
        LXI     H,STCOL
        CALL    CPUT
        MVI     A,' '
        CALL    CHOUT
        LHLD    CCOL
        MOV     A,L
        ADI     'A'
        CALL    CHOUT
        LHLD    CROW
        INX     H
        CALL    NUMP
        LXI     H,T2SP
        CALL    CPUT
; cell content preview (up to 50 chars), "..." if longer
        LHLD    CROW
        XCHG
        LHLD    CCOL
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      STNOC
        SHLD    STS
        MVI     B,50
STSL:   LHLD    STS
        MOV     A,M
        ORA     A
        JZ      STNOC
        CALL    CHOUT
        LHLD    STS
        INX     H
        SHLD    STS
        DCR     B
        JNZ     STSL
; B exhausted; if more chars print "..."
        LHLD    STS
        MOV     A,M
        ORA     A
        JZ      STNOC
        LXI     H,TDOTS
        CALL    CPUT
STNOC:  LXI     H,T4SP
        CALL    CPUT
        LXI     H,MESG
        CALL    CPUT
        CALL    EOL
        LXI     H,E0M
        JMP     PESC

; ===========================================================================
; HELPBR - help bar
; ===========================================================================
HELPBR: MVI     B,HLPR
        MVI     C,1
        CALL    CURMV
        LXI     H,HLPCOL
        CALL    CPUT
        LXI     H,THBAR
        CALL    CPUT
        CALL    EOL
        LXI     H,E0M
        JMP     PESC

; ===========================================================================
; DRWALL / DRWGRD
; ===========================================================================
DRWALL: CALL    HIDCR
        CALL    TITLN
        CALL    DRWHDR
        LXI     H,0
        SHLD    DAI
DAL:    LHLD    DAI
        LXI     D,VROWS
        CALL    UCMP
        CPI     0FFH
        JNZ     DAEND
        LHLD    DAI
        CALL    DRWROW
        LHLD    DAI
        INX     H
        SHLD    DAI
        JMP     DAL
DAEND:  CALL    STATLN
        JMP     HELPBR

DRWGRD: CALL    HIDCR
        CALL    DRWHDR
        LXI     H,0
        SHLD    DAI
DGL:    LHLD    DAI
        LXI     D,VROWS
        CALL    UCMP
        CPI     0FFH
        JNZ     DGEND
        LHLD    DAI
        CALL    DRWROW
        LHLD    DAI
        INX     H
        SHLD    DAI
        JMP     DGL
DGEND:  JMP     STATLN

; ===========================================================================
; FIXVW - clamp viewport to keep cursor visible
; ===========================================================================
FIXVW:
; if crow<trow: trow=crow
        LHLD    CROW
        XCHG
        LHLD    TROW
        XCHG
        CALL    SCMP4           ; crow vs trow
        CPI     0FFH
        JNZ     FV1
        LHLD    CROW
        SHLD    TROW
FV1:
; if crow>=trow+VROWS: trow=crow-VROWS+1
        LHLD    TROW
        LXI     D,VROWS
        DAD     D
        XCHG
        LHLD    CROW
        CALL    SCMP4           ; crow vs trow+VROWS
        CPI     0FFH
        JZ      FV2             ; crow < that
        LHLD    CROW
        LXI     D,0FFFFH-VROWS+2 ; -(VROWS-1)
        DAD     D
        SHLD    TROW
FV2:
; if ccol<tcol: tcol=ccol
        LHLD    CCOL
        XCHG
        LHLD    TCOL
        XCHG
        CALL    SCMP4
        CPI     0FFH
        JNZ     FV3
        LHLD    CCOL
        SHLD    TCOL
FV3:
; if ccol>=tcol+VCOLS: tcol=ccol-VCOLS+1
        LHLD    TCOL
        LXI     D,VCOLS
        DAD     D
        XCHG
        LHLD    CCOL
        CALL    SCMP4
        CPI     0FFH
        JZ      FV4
        LHLD    CCOL
        LXI     D,0FFFFH-VCOLS+2
        DAD     D
        SHLD    TCOL
FV4:    RET

; ===========================================================================
; PROMPT - DE=dst buffer, HL=prompt str, B=maxlen. Returns A=1 ok / 0 cancel.
; ===========================================================================
PROMPT: SHLD    PRPR
        XCHG
        SHLD    PRDST
        MOV     A,B
        STA     PRMAX
        MVI     B,EDR
        MVI     C,1
        CALL    CURMV
        CALL    INVON
        MVI     A,' '
        CALL    CHOUT
        LHLD    PRPR
        CALL    CPUT
        MVI     A,' '
        CALL    CHOUT
        CALL    INVOF
        LHLD    PRDST
        CALL    CPUT
        CALL    EOL
        CALL    SHWCR
        LHLD    PRDST
        XCHG
        CALL    STRLEN
        SHLD    PRP
PRLP:   CALL    KEYWT
        STA     PRC
        CPI     CRCH
        JZ      PROK
        CPI     LFCH
        JZ      PROK
        CPI     ESC
        JZ      PRCAN
        CPI     BKSP
        JZ      PRBS
        CPI     DELC7
        JZ      PRBS
        LDA     PRC
        CPI     32
        JC      PRLP
        CPI     127
        JNC     PRLP
; if p < max-1: store
        LHLD    PRP
        XCHG
        LDA     PRMAX
        DCR     A
        MOV     L,A
        MVI     H,0
        CALL    UCMP            ; (max-1) vs p
        CPI     1
        JNZ     PRLP            ; not (max-1 > p) -> full
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
        JZ      PRLP
        DCX     H
        SHLD    PRP
        MVI     A,BKSP
        CALL    CHOUT
        MVI     A,' '
        CALL    CHOUT
        MVI     A,BKSP
        CALL    CHOUT
        LHLD    PRDST
        XCHG
        LHLD    PRP
        DAD     D
        MVI     M,0
        JMP     PRLP
PROK:   LHLD    PRDST
        XCHG
        LHLD    PRP
        DAD     D
        MVI     M,0
        CALL    HIDCR
        MVI     B,EDR
        MVI     C,1
        CALL    CURMV
        CALL    EOL
        MVI     A,1
        RET
PRCAN:  CALL    HIDCR
        MVI     B,EDR
        MVI     C,1
        CALL    CURMV
        CALL    EOL
        XRA     A
        RET

; ===========================================================================
; EDITCL - edit current cell.  A on entry: 0 = keep content, else init char.
; ===========================================================================
EDITCL: STA     ECINIT
        XRA     A
        STA     EBUF
        LDA     ECINIT
        ORA     A
        JNZ     ECFRESH
; keep existing content
        LHLD    CROW
        XCHG
        LHLD    CCOL
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      ECPROMPT
        XCHG
        LXI     H,EBUF
        XCHG                    ; DE=EBUF dst, HL=src
        CALL    SCPYT           ; copy with 77-char cap
        JMP     ECPROMPT
ECFRESH:LDA     ECINIT
        STA     EBUF
        XRA     A
        STA     EBUF+1
ECPROMPT:
        LXI     H,TCELL
        LXI     D,EBUF
        MVI     B,EBUFSZ-1
        CALL    PROMPT
        ORA     A
        JZ      ECCANC
; setcel(crow,ccol,EBUF)
        LHLD    CROW
        SHLD    SCR
        LHLD    CCOL
        SHLD    SCC
        LXI     H,EBUF
        SHLD    SCSTR
        CALL    SETCL2
        MVI     A,1
        STA     DIRT
        LXI     H,MEMPTY
        CALL    MSG
; if crow<MAXROW-1 crow++
        LHLD    CROW
        LXI     D,MAXROW-1
        CALL    SCMP4
        CPI     0FFH
        JNZ     ECDONE
        LHLD    CROW
        INX     H
        SHLD    CROW
        JMP     ECDONE
ECCANC: LXI     H,MCANC
        CALL    MSG
ECDONE: MVI     A,1
        STA     RALL
        RET

; SCPYT - copy src(HL) to dst(DE) capping length at 77 (EDITCL helper)
SCPYT:  MVI     B,77
SCPTL:  MOV     A,B
        ORA     A
        JZ      SCPTE
        MOV     A,M
        ORA     A
        JZ      SCPTE
        STAX    D
        INX     H
        INX     D
        DCR     B
        JMP     SCPTL
SCPTE:  XRA     A
        STAX    D
        RET

; ===========================================================================
; GETFCB - build ENAME from the default FCB.  Returns A=1 if a name present.
; ===========================================================================
GETFCB: LXI     H,DEFFCB+1
        LXI     D,ENAME
        MVI     B,8
GF1:    MOV     A,M
        CPI     ' '
        JZ      GF2
        STAX    D
        INX     D
GF2:    INX     H
        DCR     B
        JNZ     GF1
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
        STAX    D
        LDA     ENAME
        ORA     A
        JZ      GF5
        MVI     A,1
        RET
GF5:    XRA     A
        RET

; NAMEFC - DE=fcb, HL=name: parse "NAME.EXT" into a 36-byte FCB
NAMEFC: SHLD    NFNAME
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
        INX     H
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
NFEXT:  INX     D
        LHLD    NFFCB
        LXI     B,9
        DAD     B
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
; SAVEF - write grid to ENAME.  One line per non-empty cell: "A1=text".
; ===========================================================================
SAVEF:  LDA     ENAME
        ORA     A
        JNZ     SV1
        LXI     H,MNOFN
        JMP     MSG
SV1:    LXI     D,RFCB
        LXI     H,ENAME
        CALL    NAMEFC
        LXI     D,RFCB
        DOS     FDELF
        LXI     D,RFCB
        DOS     FMAKEF
        CPI     0FFH
        JNZ     SV2
        LXI     H,MCWRIT
        JMP     MSG
SV2:    LXI     D,DMABUF
        DOS     FDMAF
        XRA     A
        STA     WRIDX
        LXI     H,0
        SHLD    SVR
SVRL:   LHLD    SVR
        LXI     D,MAXROW
        CALL    UCMP
        CPI     0FFH
        JNZ     SVDONE
        LXI     H,0
        SHLD    SVC
SVCL:   LHLD    SVC
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     SVNXR
        LHLD    SVR
        XCHG
        LHLD    SVC
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      SVNXC
        SHLD    SVP
        MOV     A,M
        ORA     A
        JZ      SVNXC
; write column letter
        LHLD    SVC
        MOV     A,L
        ADI     'A'
        CALL    PUTBYT
; row number (r+1), 1 or 2 digits
        LHLD    SVR
        INX     H
        SHLD    SVRN
        LXI     D,10
        CALL    UCMP
        CPI     0FFH
        JZ      SVONE
; tens digit
        LHLD    SVRN
        CALL    DIV10
        MOV     A,L             ; quotient low (0..9)
        ADI     '0'
        CALL    PUTBYT
SVONE:  LHLD    SVRN
        CALL    DIV10           ; remainder (ones digit) returned in A
        ADI     '0'
        CALL    PUTBYT
        MVI     A,'='
        CALL    PUTBYT
; cell text
SVTX:   LHLD    SVP
        MOV     A,M
        ORA     A
        JZ      SVTXE
        CALL    PUTBYT
        LHLD    SVP
        INX     H
        SHLD    SVP
        JMP     SVTX
SVTXE:  MVI     A,CRCH
        CALL    PUTBYT
        MVI     A,LFCH
        CALL    PUTBYT
SVNXC:  LHLD    SVC
        INX     H
        SHLD    SVC
        JMP     SVCL
SVNXR:  LHLD    SVR
        INX     H
        SHLD    SVR
        JMP     SVRL
SVDONE: MVI     A,CPMEOF
        CALL    PUTBYT
        LDA     WRIDX
        ORA     A
        JZ      SVCLOS
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
        STA     DIRT
        LXI     H,MSAVED
        JMP     MSG

; PUTBYT - A=byte: append to write record, flush at 128.
PUTBYT: PUSH    PSW
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
        RNZ
        PUSH    PSW
        LXI     D,RFCB
        DOS     FWRITF
        XRA     A
        STA     WRIDX
        POP     PSW
        RET

; ===========================================================================
; LOADF - load ENAME into the grid (clears existing cells first).
; ===========================================================================
LOADF:  CALL    CLRALL
        LDA     ENAME
        ORA     A
        JNZ     LD1
        LXI     H,MNOFN
        JMP     MSG
LD1:    LXI     D,RFCB
        LXI     H,ENAME
        CALL    NAMEFC
        LXI     D,RFCB
        DOS     FOPENF
        CPI     0FFH
        JNZ     LDOPEN
        LXI     H,MNEW
        JMP     MSG
LDOPEN: LXI     H,0
        SHLD    LDP
        XRA     A
        STA     LNBUF
LDREC:  LXI     D,DMABUF
        DOS     FDMAF
        LXI     D,RFCB
        DOS     FREADF
        ORA     A
        JNZ     LDEND
        LXI     H,DMABUF
        SHLD    LDQ
        MVI     A,128
        STA     LDN
LDCH:   LHLD    LDQ
        MOV     A,M
        STA     LDC
        CPI     CPMEOF
        JZ      LDFIN
        CPI     CRCH
        JZ      LDNEXT
        CPI     LFCH
        JZ      LDLF
; ordinary char -> LNBUF[p++]=c (cap ~118)
        LHLD    LDP
        LXI     D,118
        CALL    UCMP
        CPI     0FFH
        JNZ     LDNEXT
        LDA     LDC
        LHLD    LDP
        XCHG
        LXI     H,LNBUF
        DAD     D
        MOV     M,A
        LHLD    LDP
        INX     H
        SHLD    LDP
        JMP     LDNEXT
LDLF:
; terminate LNBUF[p]=0, parse "A1=..."
        LHLD    LDP
        XCHG
        LXI     H,LNBUF
        DAD     D
        MVI     M,0
        CALL    PARSLN
        LXI     H,0
        SHLD    LDP
        JMP     LDNEXT
LDNEXT: LHLD    LDQ
        INX     H
        SHLD    LDQ
        LDA     LDN
        DCR     A
        STA     LDN
        JNZ     LDCH
        JMP     LDREC
LDEND:
LDFIN:
; process any pending partial line (no trailing LF)
        LHLD    LDP
        MOV     A,H
        ORA     L
        JZ      LDCLOS
        LHLD    LDP
        XCHG
        LXI     H,LNBUF
        DAD     D
        MVI     M,0
        CALL    PARSLN
LDCLOS: LXI     D,RFCB
        DOS     FCLOSF
        XRA     A
        STA     DIRT
        MVI     A,1
        STA     RALL
        LXI     H,MLOAD
        JMP     MSG

; PARSLN - parse LNBUF "<col><row>=<text>" and store into the grid.
PARSLN: LDA     LNBUF
        ORA     A
        RZ
        CALL    ISALF
        ORA     A
        RZ
; col = upr(LNBUF[0]) - 'A'
        LDA     LNBUF
        CALL    UPR
        SUI     'A'
        MOV     L,A
        MVI     H,0
        SHLD    PLCOL
; parse row digits from LNBUF[1]
        LXI     H,LNBUF+1
        SHLD    PLP
        LXI     H,0
        SHLD    PLROW
PLDL:   LHLD    PLP
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      PLDN
        LHLD    PLROW
        MOV     D,H
        MOV     E,L
        DAD     H
        DAD     H
        DAD     D
        DAD     H               ; *10
        LHLD    PLP
        MOV     A,M
        SUI     '0'
        MOV     E,A
        MVI     D,0
        DAD     D
        SHLD    PLROW
        LHLD    PLP
        INX     H
        SHLD    PLP
        JMP     PLDL
PLDN:   LHLD    PLROW
        DCX     H
        SHLD    PLROW           ; 0-based
; require '=' at PLP
        LHLD    PLP
        MOV     A,M
        CPI     '='
        RNZ
        LHLD    PLP
        INX     H
        SHLD    PLEQ            ; text ptr
; bounds check row/col
        LHLD    PLROW
        LXI     D,MAXROW
        CALL    SCMP4
        CPI     0FFH
        RNZ
        LHLD    PLROW
        LXI     D,0
        CALL    SCMP4
        CPI     0FFH
        JZ      PLBAD
        LHLD    PLCOL
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        RNZ
        LHLD    PLROW
        SHLD    SCR
        LHLD    PLCOL
        SHLD    SCC
        LHLD    PLEQ
        SHLD    SCSTR
        CALL    SETCL2
        RET
PLBAD:  RET

; CLRALL - free every cell pointer and null the table
CLRALL: LXI     H,0
        SHLD    CARI
CARL:   LHLD    CARI
        LXI     D,MAXROW*MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     CAREND
; ptr at CELLS + i*2
        LHLD    CARI
        DAD     H
        LXI     D,CELLS
        DAD     D
        SHLD    CARP
        MOV     E,M
        INX     H
        MOV     D,M
        MOV     A,D
        ORA     E
        JZ      CARNX
        XCHG                    ; HL = ptr
        CALL    FREE
        LHLD    CARP
        MVI     M,0
        INX     H
        MVI     M,0
CARNX:  LHLD    CARI
        INX     H
        SHLD    CARI
        JMP     CARL
CAREND: RET

; ===========================================================================
; INSROW - insert a blank row at CROW; bottom row dropped; refs bumped.
; ===========================================================================
INSROW:
; free bottom row cells
        LXI     H,0
        SHLD    IRC
IRFL:   LHLD    IRC
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     IRFD
        LXI     H,MAXROW-1
        XCHG
        LHLD    IRC
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      IRFN
        CALL    FREE
        LXI     H,0
        SHLD    CPVAL
        LXI     H,MAXROW-1
        XCHG
        LHLD    IRC
        XCHG
        CALL    PUTCEL
IRFN:   LHLD    IRC
        INX     H
        SHLD    IRC
        JMP     IRFL
IRFD:
; shift rows down: for r=MAXROW-1; r>crow; r-- : ln[r]=ln[r-1]
        LXI     H,MAXROW-1
        SHLD    IRR
IRSL:   LHLD    IRR
        XCHG
        LHLD    CROW
        XCHG
        CALL    SCMP4           ; r vs crow
        CPI     1
        JNZ     IRSD            ; not r>crow
        LXI     H,0
        SHLD    IRC
IRSCL:  LHLD    IRC
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     IRSCD
; ln[r][c] = ln[r-1][c]
        LHLD    IRR
        DCX     H
        XCHG
        LHLD    IRC
        XCHG
        CALL    GETCEL
        SHLD    CPVAL
        LHLD    IRR
        XCHG
        LHLD    IRC
        XCHG
        CALL    PUTCEL
        LHLD    IRC
        INX     H
        SHLD    IRC
        JMP     IRSCL
IRSCD:  LHLD    IRR
        DCX     H
        SHLD    IRR
        JMP     IRSL
IRSD:
; clear crow row
        LXI     H,0
        SHLD    IRC
IRCL:   LHLD    IRC
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     IRCD
        LXI     H,0
        SHLD    CPVAL
        LHLD    CROW
        XCHG
        LHLD    IRC
        XCHG
        CALL    PUTCEL
        LHLD    IRC
        INX     H
        SHLD    IRC
        JMP     IRCL
IRCD:
; bmpall(crow, -1)
        LHLD    CROW
        SHLD    BMDR
        LXI     H,0FFFFH
        SHLD    BMDC
        CALL    BMPALL
        MVI     A,1
        STA     DIRT
        STA     RALL
        LXI     H,MROWI
        JMP     MSG

; ===========================================================================
; INSCOL - insert a blank column at CCOL; rightmost dropped; refs bumped.
; ===========================================================================
INSCOL:
; free rightmost column cells
        LXI     H,0
        SHLD    ICR
ICFL:   LHLD    ICR
        LXI     D,MAXROW
        CALL    UCMP
        CPI     0FFH
        JNZ     ICFD
        LHLD    ICR
        XCHG
        LXI     H,MAXCOL-1
        XCHG
        CALL    GETCEL
        MOV     A,H
        ORA     L
        JZ      ICFN
        CALL    FREE
        LXI     H,0
        SHLD    CPVAL
        LHLD    ICR
        XCHG
        LXI     H,MAXCOL-1
        XCHG
        CALL    PUTCEL
ICFN:   LHLD    ICR
        INX     H
        SHLD    ICR
        JMP     ICFL
ICFD:
; for each row: shift cols right c=MAXCOL-1; c>ccol; c-- : ln[r][c]=ln[r][c-1]
        LXI     H,0
        SHLD    ICR
ICRL:   LHLD    ICR
        LXI     D,MAXROW
        CALL    UCMP
        CPI     0FFH
        JNZ     ICRD
        LXI     H,MAXCOL-1
        SHLD    ICC
ICCL:   LHLD    ICC
        XCHG
        LHLD    CCOL
        XCHG
        CALL    SCMP4           ; c vs ccol
        CPI     1
        JNZ     ICCD            ; not c>ccol
        LHLD    ICR
        XCHG
        LHLD    ICC
        DCX     H
        XCHG
        CALL    GETCEL
        SHLD    CPVAL
        LHLD    ICR
        XCHG
        LHLD    ICC
        XCHG
        CALL    PUTCEL
        LHLD    ICC
        DCX     H
        SHLD    ICC
        JMP     ICCL
ICCD:
; clear ln[r][ccol]
        LXI     H,0
        SHLD    CPVAL
        LHLD    ICR
        XCHG
        LHLD    CCOL
        XCHG
        CALL    PUTCEL
        LHLD    ICR
        INX     H
        SHLD    ICR
        JMP     ICRL
ICRD:
; bmpall(-1, ccol)
        LXI     H,0FFFFH
        SHLD    BMDR
        LHLD    CCOL
        SHLD    BMDC
        CALL    BMPALL
        MVI     A,1
        STA     DIRT
        STA     RALL
        LXI     H,MCOLI
        JMP     MSG

; ===========================================================================
; BMPALL - apply BMPREF to every formula cell.  BMDR/BMDC set by caller.
; ===========================================================================
BMPALL: LXI     H,0
        SHLD    BAI
BAL:    LHLD    BAI
        LXI     D,MAXROW*MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     BAEND
        LHLD    BAI
        DAD     H
        LXI     D,CELLS
        DAD     D
        SHLD    BAP             ; &cells[i]
        MOV     E,M
        INX     H
        MOV     D,M
        MOV     A,D
        ORA     E
        JZ      BANX
        XCHG                    ; HL = ptr
        MOV     A,M
        CPI     '='
        JNZ     BANX
        SHLD    BMSRC
        CALL    BMPREF          ; -> BMOUT ptr (or 0)
        MOV     A,H
        ORA     L
        JZ      BANX
        SHLD    BMNEW
; free old, store new
        LHLD    BMSRC
        CALL    FREE
        LHLD    BAP
        XCHG
        LHLD    BMNEW
        MOV     A,L
        STAX    D
        INX     D
        MOV     A,H
        STAX    D
BANX:   LHLD    BAI
        INX     H
        SHLD    BAI
        JMP     BAL
BAEND:  RET

; ===========================================================================
; BMPREF - renumber refs in formula at BMSRC after an insertion.
;          BMDR>=0: row inserted at BMDR; BMDC>=0: col inserted at BMDC.
;          Builds into BMBUF, allocs a copy -> HL (ptr) or 0.
; ===========================================================================
BMPREF: LHLD    BMSRC
        SHLD    BRP
        LXI     H,BMBUF
        SHLD    BRO
        LXI     H,0
        SHLD    BRI
BRL:    LHLD    BRI
        LXI     D,155
        CALL    UCMP
        CPI     0FFH
        JNZ     BREND           ; i>=155
        LHLD    BRP
        MOV     A,M
        ORA     A
        JZ      BREND
; is alpha followed by digit?
        CALL    ISALF
        ORA     A
        JZ      BRCOPY
        LHLD    BRP
        INX     H
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      BRCOPY
; parse ref: col = upr(*p)-'A'
        LHLD    BRP
        MOV     A,M
        CALL    UPR
        SUI     'A'
        MOV     L,A
        MVI     H,0
        SHLD    BRCOL
        LHLD    BRP
        INX     H
        SHLD    BRP
        LXI     H,0
        SHLD    BRROW
        XRA     A
        STA     BRN
BRDL:   LHLD    BRP
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      BRDN
        LDA     BRN
        CPI     2
        JZ      BRDN
        LHLD    BRROW
        MOV     D,H
        MOV     E,L
        DAD     H
        DAD     H
        DAD     D
        DAD     H               ; HL = row*10
        XCHG                    ; DE = row*10 (LHLD BRP below clobbers HL)
        LHLD    BRP
        MOV     A,M
        SUI     '0'
        MOV     L,A
        MVI     H,0             ; HL = digit
        DAD     D               ; HL = row*10 + digit
        SHLD    BRROW
        LHLD    BRP
        INX     H
        SHLD    BRP
        LDA     BRN
        INR     A
        STA     BRN
        JMP     BRDL
BRDN:
; if dc>=0 && col>=dc && col<MAXCOL-1: col++
        LHLD    BMDC
        MOV     A,H
        ANI     80H
        JNZ     BRROWA          ; dc < 0 -> skip col bump
        LHLD    BRCOL
        XCHG
        LHLD    BMDC
        XCHG
        CALL    SCMP4           ; col vs dc
        CPI     0FFH
        JZ      BRROWA          ; col<dc
        LHLD    BRCOL
        LXI     D,MAXCOL-1
        CALL    SCMP4
        CPI     0FFH
        JNZ     BRROWA          ; col>=MAXCOL-1
        LHLD    BRCOL
        INX     H
        SHLD    BRCOL
BRROWA:
; if dr>=0 && row>=dr+1 && row<MAXROW: row++
        LHLD    BMDR
        MOV     A,H
        ANI     80H
        JNZ     BRCLMP          ; dr<0
        LHLD    BMDR
        INX     H
        XCHG
        LHLD    BRROW
        CALL    SCMP4           ; row vs dr+1
        CPI     0FFH
        JZ      BRCLMP          ; row<dr+1
        LHLD    BRROW
        LXI     D,MAXROW
        CALL    SCMP4
        CPI     0FFH
        JNZ     BRCLMP          ; row>=MAXROW
        LHLD    BRROW
        INX     H
        SHLD    BRROW
BRCLMP:
; clamp col 0..MAXCOL-1
        LHLD    BRCOL
        MOV     A,H
        ANI     80H
        JZ      BRCC1
        LXI     H,0
        SHLD    BRCOL
BRCC1:  LHLD    BRCOL
        LXI     D,MAXCOL-1
        CALL    SCMP4
        CPI     1
        JNZ     BRCC2
        LXI     H,MAXCOL-1
        SHLD    BRCOL
BRCC2:
; clamp row 1..MAXROW (1-based as written)
        LHLD    BRROW
        LXI     D,1
        CALL    SCMP4
        CPI     0FFH
        JNZ     BRRC1
        LXI     H,1
        SHLD    BRROW
BRRC1:  LHLD    BRROW
        LXI     D,MAXROW
        CALL    SCMP4
        CPI     1
        JNZ     BRRC2
        LXI     H,MAXROW
        SHLD    BRROW
BRRC2:
; emit: 'A'+col
        LHLD    BRCOL
        MOV     A,L
        ADI     'A'
        CALL    BREMIT
; emit row (1 or 2 digits)
        LHLD    BRROW
        LXI     D,10
        CALL    SCMP4
        CPI     0FFH
        JZ      BR1DIG
        LHLD    BRROW
        CALL    DIV10
        MOV     A,L
        ADI     '0'
        CALL    BREMIT
        LHLD    BRROW
        CALL    DIV10           ; remainder (ones digit) returned in A
        ADI     '0'
        CALL    BREMIT
        JMP     BRL
BR1DIG: LHLD    BRROW
        MOV     A,L
        ADI     '0'
        CALL    BREMIT
        JMP     BRL
BRCOPY: LHLD    BRP
        MOV     A,M
        CALL    BREMIT
        LHLD    BRP
        INX     H
        SHLD    BRP
        LHLD    BRI
        INX     H
        SHLD    BRI
        JMP     BRL
BREND:  LHLD    BRO
        MVI     M,0
; alloc copy of BMBUF
        LXI     D,BMBUF
        CALL    STRLEN
        INX     H
        CALL    ALLOC
        MOV     A,H
        ORA     L
        RZ
        SHLD    BMRES
        XCHG
        LXI     H,BMBUF
        CALL    SCPY
        LHLD    BMRES
        RET

; BREMIT - store A into BMBUF (via BRO), bump BRI
BREMIT: PUSH    PSW
        LHLD    BRO
        POP     PSW
        MOV     M,A
        LHLD    BRO
        INX     H
        SHLD    BRO
        LHLD    BRI
        INX     H
        SHLD    BRI
        RET

; ===========================================================================
; GOTOCL - prompt for a cell, jump to it.
; ===========================================================================
GOTOCL: LXI     H,GBUF
        MVI     M,0
        LXI     H,TGOTO
        LXI     D,GBUF
        MVI     B,7
        CALL    PROMPT
        ORA     A
        JNZ     GO1
        LXI     H,MEMPTY
        JMP     MSG
GO1:    LDA     GBUF
        CALL    ISALF
        ORA     A
        JNZ     GO2
        LXI     H,MBADC
        JMP     MSG
GO2:    LDA     GBUF
        CALL    UPR
        SUI     'A'
        MOV     L,A
        MVI     H,0
        SHLD    GOCOL
        LXI     H,GBUF+1
        SHLD    GOP
        LXI     H,0
        SHLD    GOROW
GODL:   LHLD    GOP
        MOV     A,M
        CALL    ISDIGF
        ORA     A
        JZ      GODN
        LHLD    GOROW
        MOV     D,H
        MOV     E,L
        DAD     H
        DAD     H
        DAD     D
        DAD     H
        LHLD    GOP
        MOV     A,M
        SUI     '0'
        MOV     E,A
        MVI     D,0
        DAD     D
        SHLD    GOROW
        LHLD    GOP
        INX     H
        SHLD    GOP
        JMP     GODL
GODN:   LHLD    GOROW
        DCX     H
        SHLD    GOROW
; bounds
        LHLD    GOROW
        LXI     D,MAXROW
        CALL    SCMP4
        CPI     0FFH
        JNZ     GOBAD
        LHLD    GOROW
        LXI     D,0
        CALL    SCMP4
        CPI     0FFH
        JZ      GOBAD
        LHLD    GOCOL
        LXI     D,MAXCOL
        CALL    UCMP
        CPI     0FFH
        JNZ     GOBAD
        LHLD    GOROW
        SHLD    CROW
        LHLD    GOCOL
        SHLD    CCOL
        LXI     H,MEMPTY
        JMP     MSG
GOBAD:  LXI     H,MOOR
        JMP     MSG

; ===========================================================================
; HELPSC - full-screen help page.
; ===========================================================================
HELPSC: CALL    CLRS
        MVI     B,2
        MVI     C,4
        CALL    CURMV
        LXI     H,HS0
        CALL    CPUT
        MVI     B,4
        MVI     C,4
        CALL    CURMV
        LXI     H,HS1
        CALL    CPUT
        MVI     B,5
        MVI     C,4
        CALL    CURMV
        LXI     H,HS2
        CALL    CPUT
        MVI     B,7
        MVI     C,4
        CALL    CURMV
        LXI     H,HS3
        CALL    CPUT
        MVI     B,8
        MVI     C,4
        CALL    CURMV
        LXI     H,HS4
        CALL    CPUT
        MVI     B,9
        MVI     C,4
        CALL    CURMV
        LXI     H,HS5
        CALL    CPUT
        MVI     B,10
        MVI     C,4
        CALL    CURMV
        LXI     H,HS6
        CALL    CPUT
        MVI     B,11
        MVI     C,4
        CALL    CURMV
        LXI     H,HS7
        CALL    CPUT
        MVI     B,12
        MVI     C,4
        CALL    CURMV
        LXI     H,HS8
        CALL    CPUT
        MVI     B,13
        MVI     C,4
        CALL    CURMV
        LXI     H,HS9
        CALL    CPUT
        MVI     B,14
        MVI     C,4
        CALL    CURMV
        LXI     H,HS10
        CALL    CPUT
        MVI     B,15
        MVI     C,4
        CALL    CURMV
        LXI     H,HS11
        CALL    CPUT
        MVI     B,16
        MVI     C,4
        CALL    CURMV
        LXI     H,HS12
        CALL    CPUT
        MVI     B,17
        MVI     C,4
        CALL    CURMV
        LXI     H,HS13
        CALL    CPUT
        MVI     B,18
        MVI     C,4
        CALL    CURMV
        LXI     H,HS14
        CALL    CPUT
        MVI     B,20
        MVI     C,4
        CALL    CURMV
        LXI     H,HS15
        CALL    CPUT
        MVI     B,21
        MVI     C,4
        CALL    CURMV
        LXI     H,HS16
        CALL    CPUT
        MVI     B,23
        MVI     C,4
        CALL    CURMV
        LXI     H,HS17
        CALL    CPUT
        CALL    KEYWT
        MVI     A,1
        STA     RALL
        RET

; ===========================================================================
; ASKQT - confirm quit when dirty.  A=1 ok to quit, A=0 cancel.
; ===========================================================================
ASKQT:  LDA     DIRT
        ORA     A
        JNZ     AQ1
        MVI     A,1
        RET
AQ1:    LXI     H,MQUIT
        CALL    MSG
        CALL    STATLN
        CALL    KEYWT
        CPI     'y'
        JZ      AQY
        CPI     'Y'
        JZ      AQY
        LXI     H,MEMPTY
        CALL    MSG
        XRA     A
        RET
AQY:    MVI     A,1
        RET

; ===========================================================================
; DOKEY - dispatch the key in CKEY.
; ===========================================================================
DOKEY:  LDA     CKEY
        CPI     KUP
        JNZ     DK1
        LHLD    CROW
        MOV     A,H
        ORA     L
        RZ
        DCX     H
        SHLD    CROW
        RET
DK1:    CPI     KDN
        JNZ     DK2
        LHLD    CROW
        LXI     D,MAXROW-1
        CALL    SCMP4
        CPI     0FFH
        RNZ
        LHLD    CROW
        INX     H
        SHLD    CROW
        RET
DK2:    CPI     KLT
        JNZ     DK3
        LHLD    CCOL
        MOV     A,H
        ORA     L
        RZ
        DCX     H
        SHLD    CCOL
        RET
DK3:    CPI     KRT
        JNZ     DK4
        LHLD    CCOL
        LXI     D,MAXCOL-1
        CALL    SCMP4
        CPI     0FFH
        RNZ
        LHLD    CCOL
        INX     H
        SHLD    CCOL
        RET
DK4:    CPI     CTLR
        JNZ     DK5
; crow -= VROWS-1; if <0 0; trow=crow
        LHLD    CROW
        LXI     D,0FFFFH-VROWS+2
        DAD     D
        SHLD    CROW
        MOV     A,H
        ANI     80H
        JZ      DK4A
        LXI     H,0
        SHLD    CROW
DK4A:   LHLD    CROW
        SHLD    TROW
        MVI     A,1
        STA     RALL
        RET
DK5:    CPI     CTLV
        JNZ     DK6
        LHLD    CROW
        LXI     D,VROWS-1
        DAD     D
        SHLD    CROW
        LXI     D,MAXROW-1
        CALL    SCMP4
        CPI     1
        JNZ     DK5A
        LXI     H,MAXROW-1
        SHLD    CROW
DK5A:   LHLD    CROW
        LXI     D,0FFFFH-VROWS+2
        DAD     D
        SHLD    TROW
        MOV     A,H
        ANI     80H
        JZ      DK5B
        LXI     H,0
        SHLD    TROW
DK5B:   MVI     A,1
        STA     RALL
        RET
DK6:    CPI     CRCH
        JZ      DK6X
        CPI     LFCH
        JNZ     DK7
DK6X:   XRA     A
        CALL    EDITCL
        RET
DK7:    CPI     CTLK
        JNZ     DK8
        CALL    CLRCEL
        LXI     H,MCLRD
        CALL    MSG
        MVI     A,1
        STA     RALL
        RET
DK8:    CPI     CTLN
        JNZ     DK9
        CALL    INSROW
        RET
DK9:    CPI     CTLT
        JNZ     DK10
        CALL    INSCOL
        RET
DK10:   CPI     CTLO
        JNZ     DK11
        CALL    SAVEF
        MVI     A,1
        STA     RALL
        RET
DK11:   CPI     CTLL
        JNZ     DK12
        CALL    LOADF
        RET
DK12:   CPI     CTLG
        JNZ     DK13
        CALL    GOTOCL
        RET
DK13:   CPI     CTLW
        JNZ     DK14
        CALL    HELPSC
        RET
DK14:   CPI     CTLQ
        JZ      DK14X
        CPI     ESC
        JNZ     DK15
DK14X:  CALL    ASKQT
        ORA     A
        RZ
        MVI     A,1
        STA     QUIT
        RET
DK15:   LDA     CKEY
        CPI     32
        RC
        CPI     127
        RNC
; printable -> editcl(k)
        LDA     CKEY
        CALL    EDITCL
        RET

; ===========================================================================
; USAGE
; ===========================================================================
USAGE:  LXI     H,USG1
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

TTLCOL: DB      27,'[1;97;44m ALTAIR SHEETS  ',0
HDRCOL: DB      27,'[30;103m',0
CURCOL: DB      27,'[1;97;41m',0
CELCOL: DB      27,'[92m',0
STCOL:  DB      27,'[1;93;44m',0
HLPCOL: DB      27,'[2;33m',0

TNOF:   DB      '[no file]',0
TSTAR:  DB      ' *',0
T2SP:   DB      '  ',0
T4SP:   DB      '    ',0
T10SP:  DB      '          ',0
TDOTS:  DB      '...',0
TERRC:  DB      '  #ERR    ',0
TDONE:  DB      13,10,'SHEETS done.',13,10,0
TCELL:  DB      'Cell:',0
TGOTO:  DB      'Goto:',0
THBAR:  DB      '^W Help ^O Save ^L Load ^G Goto '
        DB      '^N InsRow ^T InsCol ^R/V Page ^Q Quit',0

MEMPTY: DB      0
MNOFN:  DB      'No filename',0
MNEW:   DB      'New file',0
MLOAD:  DB      'Loaded',0
MSAVED: DB      'Saved',0
MCWRIT: DB      'Cannot write',0
MCLRD:  DB      'Cleared',0
MCANC:  DB      'Edit cancelled',0
MROWI:  DB      'Row inserted',0
MCOLI:  DB      'Column inserted',0
MBADC:  DB      'Bad cell',0
MOOR:   DB      'Out of range',0
MQUIT:  DB      'Unsaved changes. Quit anyway? y/N',0

HS0:    DB      'SHEETS - simple spreadsheet',0
HS1:    DB      'Grid: A..Z columns by 1..99 rows.',0
HS2:    DB      'Cells hold text, integers, or =formulas.',0
HS3:    DB      'Movement:   Arrow keys',0
HS4:    DB      'Edit cell:  Enter (keep)  or any printable (replace)',0
HS5:    DB      'Clear cell: Ctrl-K',0
HS6:    DB      'Insert row: Ctrl-N  (shifts rows down, drops bottom)',0
HS7:    DB      'Insert col: Ctrl-T  (shifts cols right, drops rightmost)',0
HS8:    DB      'Save file:  Ctrl-O',0
HS9:    DB      'Load file:  Ctrl-L',0
HS10:   DB      'Goto cell:  Ctrl-G  (e.g. C12)',0
HS11:   DB      'Page up/dn: Ctrl-R / Ctrl-V',0
HS12:   DB      'Quit:       Ctrl-Q  or ESC',0
HS13:   DB      'Formulas: =A1+B2*3   =-A1   =(A1+A2)/2',0
HS14:   DB      '          =SUM(A1:A5)  =AVG(A1:C3)  =MIN/MAX/COUNT(A1:A9)',0
HS15:   DB      'Integers only (32-bit, -2147483648..2147483647).  Refs are',0
HS16:   DB      'renumbered automatically when rows/columns are inserted.',0
HS17:   DB      'Press any key to return.',0

USG1:   DB      13,10,'SHEETS - simple spreadsheet',13,10,0
USG2:   DB      'Usage: SHEETS filename',13,10,0
USG3:   DB      'A filename is required so Ctrl-O can save.',13,10,0

; ===========================================================================
; Variables
; ===========================================================================
OLDSP:  DS      2
CONOTA: DS      2

; heap state
FREEL:  DS      2
BRKP:   DS      2
HEAPND: DS      2
ANEED:  DS      2
ACUR:   DS      2
APREV:  DS      2
ANEW:   DS      2
ANEXT:  DS      2
ATMP:   DS      2

; grid state
CROW:   DS      2
CCOL:   DS      2
TROW:   DS      2
TCOL:   DS      2
DIRT:   DS      1
QUIT:   DS      1
RALL:   DS      1
OROW:   DS      2
OCOL:   DS      2
OTROW:  DS      2
OTCOL:  DS      2
CKEY:   DS      1
CMROW:  DS      1
CMCOL:  DS      1

; cell pointer to store
CPVAL:  DS      2

; setcel
SCR:    DS      2
SCC:    DS      2
SCSTR:  DS      2
SCLEN:  DS      2
SCRSAV: DS      2

; evaluator
EPOS:   DS      2
EOK:    DS      1
EDEPTH: DS      1
EVR:    DS      2
EVC:    DS      2
EVS:    DS      2
EVSEP:  DS      2
PRROW:  DS      2
PRCOL:  DS      2
EXOP:   DS      1
EXSAV:  DS      4
TMOP:   DS      1
TMSAV:  DS      4
FNEG:   DS      1
FTAG:   DS      1
RGR1:   DS      2
RGC1:   DS      2
RGR2:   DS      2
RGC2:   DS      2
RGI:    DS      2
RGJ:    DS      2
RGCNT:  DS      2
RGGOT:  DS      1
RSUM:   DS      4
RMM:    DS      4
DIGT:   DS      4
DIVI:   DS      4

; 32-bit math scratch
LZRO:   DS      4
LTEN:   DS      4
ACC:    DS      4
LARGA:  DS      4
LARGB:  DS      4
LARGR:  DS      4
LARGA2: DS      4
NTMP:   DS      4
LREM:   DS      4
LIDX:   DS      2
LCARRY: DS      1
LMSGN:  DS      1
LDSGN:  DS      1
MITER:  DS      1
MCAND:  DS      4
MPLR:   DS      4
PROD:   DS      4
MARGA:  DS      4
MARGB:  DS      4
DITER:  DS      1
DDVD:   DS      4
DDVS:   DS      4
DREM:   DS      4
DQUO:   DS      4
DARGA:  DS      4
DARGB:  DS      4

; atol / ltoa
ATPTR:  DS      2
ATSAV:  DS      2
ATNEG:  DS      1
LTOUT:  DS      2
LABS:   DS      4
LWORK:  DS      16
LWP:    DS      2
LDCNT:  DS      1
LTBUF:  DS      16

; render
RNR:    DS      2
RNCOL:  DS      2
RNS:    DS      2
RNLEN:  DS      2
RBUF:   DS      CWID+2

; draw header / rows
DHI:    DS      2
DHCOL:  DS      2
DRVR:   DS      2
DRR:    DS      2
DRRN:   DS      2
DRI:    DS      2
DRC:    DS      2
DAI:    DS      2

; status
STS:    DS      2

; prompt
PRPR:   DS      2
PRDST:  DS      2
PRP:    DS      2
PRC:    DS      1
PRMAX:  DS      1

; editcl
ECINIT: DS      1

; file name parse
NFNAME: DS      2
NFFCB:  DS      2

; save
SVR:    DS      2
SVC:    DS      2
SVP:    DS      2
SVRN:   DS      2
WRIDX:  DS      1

; load
LDP:    DS      2
LDQ:    DS      2
LDN:    DS      1
LDC:    DS      1
PLP:    DS      2
PLROW:  DS      2
PLCOL:  DS      2
PLEQ:   DS      2

; clrall
CARI:   DS      2
CARP:   DS      2

; insert row/col
IRR:    DS      2
IRC:    DS      2
ICR:    DS      2
ICC:    DS      2

; bmpall / bmpref
BMDR:   DS      2
BMDC:   DS      2
BAI:    DS      2
BAP:    DS      2
BMSRC:  DS      2
BMNEW:  DS      2
BMRES:  DS      2
BRP:    DS      2
BRO:    DS      2
BRI:    DS      2
BRCOL:  DS      2
BRROW:  DS      2
BRN:    DS      1

; goto
GOCOL:  DS      2
GOROW:  DS      2
GOP:    DS      2

; buffers
ENAME:  DS      20
MESG:   DS      72
EBUF:   DS      EBUFSZ+2
LNBUF:  DS      128
GBUF:   DS      10
BMBUF:  DS      160
RFCB:   DS      36
DMABUF: DS      128

; cells table and stack
CELLS:  DS      MAXROW*MAXCOL*2
        DS      768
STKTOP  EQU     $
HEAPB   EQU     $

        END     START
