; ATTNINF.ASM - inference-only 8080 MAC port of ATTN.C
;
; "Paper Tape is All You Need" - a 1-layer, 1-head transformer that
; reverses an 8-digit sequence.  This is a hand-written Intel 8080
; assembly port of the inference path of Apps/ATTN/ATTN.C, itself a
; BDS C port of the PDP-11 attn.s (davepl/pdpsrc) and the NN11 library
; by Damien Boureille (dbrll/ATTN-11).
;
; Only inference is implemented here: it loads the trained Q16 weights
; from ATTN.WTS on the current drive (produced by the BDS C 'attn -t'),
; reads 8-digit lines
; from an input file (default ATTN.IN), runs the forward pass, prints
; the reversed prediction for each line, and reports accuracy.
;
; Numerics (faithful to ATTN.C):
;   Q8  activations          int16, value = real * 256
;   Q16 weights on disk      32-bit signed, big-endian (LONG package)
; The 8080 has no multiply/divide/shift, so all of that is open coded.
;
; Build:  mac attninf  ->  load attninf  ->  ATTNINFR.COM
;
; Usage:
;   attninfr           infer from ATTN.IN
;   attninfr <file>    infer from <file> instead
;   attninfr -h        help

DM      EQU     16              ; d_model
S       EQU     8               ; sequence length
V       EQU     10              ; vocab (digits 0-9)

BDOSE   EQU     5               ; BDOS entry point
FCONO   EQU     2               ; console output (char in E)
FPRSTR  EQU     9               ; print '$' string (DE)
FOPEN   EQU     15              ; open file (DE = FCB)
FRDSEQ  EQU     20              ; read sequential
FSDMA   EQU     26              ; set DMA address

CPMEOF  EQU     1AH             ; ^Z text terminator

        ORG     0100H

START:  LXI     H,0             ; save CCP stack
        DAD     SP
        SHLD    OLDSP
        LXI     SP,STACK        ; our stack

; --- parse the command tail for an optional filename or -H ----------------
        LDA     0080H           ; B = tail length
        MOV     B,A
        LXI     H,0081H         ; HL -> first tail char
TSKIP:  MOV     A,B
        ORA     A
        JZ      TDEF            ; nothing left -> default file
        MOV     A,M
        CPI     ' '
        JNZ     TGOT
        INX     H
        DCR     B
        JMP     TSKIP
TGOT:   LXI     D,FNAME         ; copy token -> FNAME
TCPY:   MOV     A,B
        ORA     A
        JZ      TCEND
        MOV     A,M
        CPI     ' '
        JZ      TCEND
        STAX    D
        INX     H
        INX     D
        DCR     B
        JMP     TCPY
TCEND:  XRA     A
        STAX    D               ; NUL terminate
        LDA     FNAME
        ORA     A
        JZ      TDEF            ; empty -> default
        CPI     '-'             ; option?
        JNZ     TDONE
        LDA     FNAME+1
        CPI     'H'             ; CCP upper-cases the tail
        JZ      DOHELP
        JMP     TDONE           ; '-' something else: treat as a name
TDEF:   LXI     H,DEFIN         ; FNAME = "ATTN.IN"
        LXI     D,FNAME
TDCP:   MOV     A,M
        STAX    D
        INX     H
        INX     D
        ORA     A
        JNZ     TDCP
TDONE:

; --- banner ---------------------------------------------------------------
        LXI     D,MBAN1
        CALL    PSTR
        LXI     D,MLOAD
        CALL    PSTR

; --- load and convert the weights -----------------------------------------
        CALL    LOADW
        ORA     A
        JZ      WTSOK
        LXI     D,MNOWTS
        CALL    PSTR
        JMP     EXIT
WTSOK:  CALL    CVT16

; --- open the input file --------------------------------------------------
        LXI     H,FNAME
        CALL    MKFCB
        LXI     D,FIN
        MVI     C,FOPEN
        CALL    BDOSE
        INR     A               ; 0FFH -> 0 on failure
        JNZ     INOK
        LXI     D,MNOIN
        CALL    PSTR
        LXI     H,FNAME
        CALL    PUTSTR
        LXI     D,MCRLF
        CALL    PSTR
        JMP     EXIT
INOK:

; --- run inference over the file ------------------------------------------
        CALL    RUNFIL

; --- report accuracy ------------------------------------------------------
        LXI     D,MACC
        CALL    PSTR
        LHLD    FHITS
        CALL    PRDEC
        MVI     A,'/'
        CALL    COUT
        LHLD    NSEQ
        CALL    PRDEC
        LXI     D,MCRLF
        CALL    PSTR

EXIT:   LXI     D,0080H         ; restore default DMA for the CCP
        MVI     C,FSDMA
        CALL    BDOSE
        LHLD    OLDSP
        SPHL
        RET

DOHELP: LXI     D,MHELP
        CALL    PSTR
        JMP     EXIT

; ==========================================================================
; LOADW - open ATTN.WTS on the current drive and read 38 records into WBUF.
;         Returns A = 0 on success, A = 1 on failure.
; ==========================================================================
LOADW:  LXI     D,FWTS
        MVI     C,FOPEN
        CALL    BDOSE
        INR     A               ; 0FFH -> 0 fail
        JZ      LWFAIL
        LXI     H,WBUF
        SHLD    DMAP
        MVI     A,38
        STA     WCNT
LWLP:   LHLD    DMAP
        XCHG
        MVI     C,FSDMA
        CALL    BDOSE
        LXI     D,FWTS
        MVI     C,FRDSEQ
        CALL    BDOSE
        ORA     A               ; nonzero -> EOF/error (short file)
        JNZ     LWFAIL
        LHLD    DMAP
        LXI     D,128
        DAD     D
        SHLD    DMAP
        LDA     WCNT
        DCR     A
        STA     WCNT
        JNZ     LWLP
        XRA     A
        RET
LWFAIL: MVI     A,1
        RET

; ==========================================================================
; CVT16 - convert 1216 big-endian Q16 weights in WBUF to Q8 ints in QBUF.
;         Each weight = lq8(value) = signed value/256 toward zero, clamp16.
; ==========================================================================
CVT16:  LXI     H,WBUF
        SHLD    WBUFP
        LXI     H,QBUF
        SHLD    QBUFP
        LXI     H,1216
        SHLD    CVCNT
CVLP:   LHLD    WBUFP           ; HL -> b0 (MSB) .. b3 (LSB)
        MOV     A,M             ; b0
        STA     CVT4+3
        INX     H
        MOV     A,M             ; b1
        STA     CVT4+2
        INX     H
        MOV     A,M             ; b2
        STA     CVT4+1
        INX     H
        MOV     A,M             ; b3
        STA     CVT4
        INX     H
        SHLD    WBUFP           ; advanced by 4
        LXI     H,CVT4
        CALL    LQ8             ; HL = q8
        XCHG                    ; DE = q8
        LHLD    QBUFP
        MOV     M,E
        INX     H
        MOV     M,D
        INX     H
        SHLD    QBUFP
        LHLD    CVCNT
        DCX     H
        SHLD    CVCNT
        MOV     A,H
        ORA     L
        JNZ     CVLP
        RET

; ==========================================================================
; RUNFIL - read records from the open FIN file, gather 8-digit lines,
;          run infseq() on each, accumulate FHITS and NSEQ.
; ==========================================================================
RUNFIL: LXI     H,0
        SHLD    FHITS
        SHLD    NSEQ
        XRA     A
        STA     DIGI
RFREC:  LXI     D,SECBUF        ; set DMA to our record buffer
        MVI     C,FSDMA
        CALL    BDOSE
        LXI     D,FIN
        MVI     C,FRDSEQ
        CALL    BDOSE
        ORA     A
        JNZ     RFEOF           ; nonzero -> end of file
        LXI     H,SECBUF
        MVI     B,128
RFSCAN: MOV     A,M
        CPI     CPMEOF
        JZ      RFEOF
        CPI     '0'
        JC      RFNDIG
        CPI     ':'             ; '9'+1
        JNC     RFNDIG
        ; digit in A
        SUI     '0'             ; A = digit value (0..9)
        PUSH    H               ; save scan ptr
        PUSH    B               ; save scan count
        MOV     C,A             ; C = digit value
        LDA     DIGI
        CPI     S
        JNC     RFDADV          ; already have S digits: just count
        MOV     E,A             ; E = DIGI index
        MVI     D,0
        LXI     H,TOKENS
        DAD     D
        DAD     D               ; HL = TOKENS + DIGI*2
        MOV     M,C             ; low byte = digit
        INX     H
        MVI     M,0             ; high byte = 0
RFDADV: POP     B
        POP     H
        LDA     DIGI
        INR     A
        STA     DIGI
        JMP     RFNEXT
RFNDIG: CPI     0AH             ; newline (LF)?
        JNZ     RFNEXT
        ; end of line
        LDA     DIGI
        CPI     S
        JC      RFRST           ; fewer than S digits -> skip
        PUSH    H
        PUSH    B
        CALL    INFSEQ          ; A = ok (0/1)
        MOV     E,A
        MVI     D,0
        LHLD    FHITS
        DAD     D
        SHLD    FHITS
        LHLD    NSEQ
        INX     H
        SHLD    NSEQ
        POP     B
        POP     H
RFRST:  XRA     A
        STA     DIGI
RFNEXT: INX     H
        DCR     B
        JNZ     RFSCAN
        JMP     RFREC
RFEOF:  ; final line with no trailing newline
        LDA     DIGI
        CPI     S
        JC      RFDONE
        CALL    INFSEQ
        MOV     E,A
        MVI     D,0
        LHLD    FHITS
        DAD     D
        SHLD    FHITS
        LHLD    NSEQ
        INX     H
        SHLD    NSEQ
RFDONE: RET

; ==========================================================================
; INFSEQ - forward pass on TOKENS, print "in -> out ok/fail",
;          return A = 1 if every position matches the reversed input.
; ==========================================================================
INFSEQ: CALL    FORWRD
        MVI     A,' '
        CALL    COUT
        ; print tokens
        LXI     H,TOKENS
        SHLD    ISP
        MVI     A,S
        STA     ISI
ISPT:   LHLD    ISP
        MOV     A,M
        INX     H
        INX     H
        SHLD    ISP
        CALL    PRDIG
        LDA     ISI
        DCR     A
        STA     ISI
        JNZ     ISPT
        LXI     D,MARROW
        CALL    PSTR
        MVI     A,1
        STA     OKF
        ; for i = 0..S-1: argmax logits[i*V], compare to tokens[S-1-i]
        LXI     H,LOGB
        SHLD    ISP             ; reuse ISP as logits row pointer
        XRA     A
        STA     ISI             ; ISI = i
ISPRD:  LHLD    ISP
        SHLD    VPTR
        MVI     A,V
        STA     VN
        CALL    VMAX            ; IDX = argmax
        LDA     IDX
        CALL    PRDIG
        ; tokens[S-1-i]
        LDA     ISI
        MOV     B,A
        MVI     A,S-1
        SUB     B               ; A = S-1-i
        MOV     L,A
        MVI     H,0
        DAD     H               ; *2
        LXI     D,TOKENS
        DAD     D
        MOV     A,M             ; tokens[S-1-i] low byte
        MOV     B,A
        LDA     IDX
        CMP     B
        JZ      ISPOK
        XRA     A
        STA     OKF             ; mismatch
ISPOK:  LHLD    ISP             ; advance logits row by V ints
        LXI     D,V+V
        DAD     D
        SHLD    ISP
        LDA     ISI
        INR     A
        STA     ISI
        CPI     S
        JNZ     ISPRD
        LDA     OKF
        ORA     A
        JZ      ISFAIL
        LXI     D,MOK
        CALL    PSTR
        JMP     ISRET
ISFAIL: LXI     D,MFAIL
        CALL    PSTR
ISRET:  LDA     OKF
        RET

; ==========================================================================
; FORWRD - embed -> attn -> proj
; ==========================================================================
FORWRD: CALL    EMBED
        CALL    ATTNF
        CALL    PROJ
        RET

; ==========================================================================
; EMBED - xx[i*D+j] = qtke[tokens[i]*D + j] + qpse[i*D + j]
; ==========================================================================
EMBED:  LXI     H,XX
        SHLD    EXXP
        LXI     H,QPSEB
        SHLD    EPSEP
        LXI     H,TOKENS
        SHLD    ETOKP
        MVI     A,S
        STA     EMBI
EMROW:  LHLD    ETOKP
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    ETOKP           ; DE = tok
        XCHG                    ; HL = tok
        DAD     H               ; tok*2
        DAD     H               ; *4
        DAD     H               ; *8
        DAD     H               ; *16
        DAD     H               ; *32  (= tok*D*2)
        LXI     D,QTKEB
        DAD     D
        SHLD    ETKEP
        MVI     A,DM
        STA     EMBJ
EMCOL:  LHLD    ETKEP
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    ETKEP           ; DE = tke
        LHLD    EPSEP
        MOV     C,M
        INX     H
        MOV     B,M
        INX     H
        SHLD    EPSEP           ; BC = pse
        MOV     A,E
        ADD     C
        MOV     L,A
        MOV     A,D
        ADC     B
        MOV     H,A             ; HL = tke + pse
        XCHG                    ; DE = sum
        LHLD    EXXP
        MOV     M,E
        INX     H
        MOV     M,D
        INX     H
        SHLD    EXXP
        LDA     EMBJ
        DCR     A
        STA     EMBJ
        JNZ     EMCOL
        LDA     EMBI
        DCR     A
        STA     EMBI
        JNZ     EMROW
        RET

; ==========================================================================
; ATTNF - self attention forward pass
; ==========================================================================
ATTNF:
; Step 1-3: Q = X.Wq, K = X.Wk, V = X.Wv  (per row vtmul)
        LXI     H,XX
        SHLD    AXIP
        LXI     H,WQB
        SHLD    AQP
        LXI     H,WKB
        SHLD    AKP
        LXI     H,WVB
        SHLD    AVP
        MVI     A,S
        STA     AAI
ATQKV:  LXI     H,QWQB
        SHLD    VTMAT
        LHLD    AXIP
        SHLD    VTVIN
        LHLD    AQP
        SHLD    VTVOUT
        MVI     A,DM
        STA     VTROWS
        MVI     A,DM
        STA     VTCOLS
        CALL    VTMUL
        LXI     H,QWKB
        SHLD    VTMAT
        LHLD    AXIP
        SHLD    VTVIN
        LHLD    AKP
        SHLD    VTVOUT
        MVI     A,DM
        STA     VTROWS
        MVI     A,DM
        STA     VTCOLS
        CALL    VTMUL
        LXI     H,QWVB
        SHLD    VTMAT
        LHLD    AXIP
        SHLD    VTVIN
        LHLD    AVP
        SHLD    VTVOUT
        MVI     A,DM
        STA     VTROWS
        MVI     A,DM
        STA     VTCOLS
        CALL    VTMUL
        ; advance row pointers by D*2 = 32
        LHLD    AXIP
        LXI     D,32
        DAD     D
        SHLD    AXIP
        LHLD    AQP
        LXI     D,32
        DAD     D
        SHLD    AQP
        LHLD    AKP
        LXI     D,32
        DAD     D
        SHLD    AKP
        LHLD    AVP
        LXI     D,32
        DAD     D
        SHLD    AVP
        LDA     AAI
        DCR     A
        STA     AAI
        JNZ     ATQKV
; Step 4: scores[i][j] = asr(vdot(Q[i],K[j],D),2)
        LXI     H,WQB
        SHLD    AQIP            ; Q[i]
        LXI     H,WAB
        SHLD    AAP             ; scores write ptr
        MVI     A,S
        STA     AAI
ATSCI:  LXI     H,WKB
        SHLD    AKJP            ; K[j]
        MVI     A,S
        STA     AAJ
ATSCJ:  LHLD    AQIP
        SHLD    XPTR
        LHLD    AKJP
        SHLD    YPTR
        MVI     A,DM
        STA     VDN
        CALL    VDOT            ; HL = dot (Q8, clamped)
        CALL    ASR2            ; HL = dot >> 2 (floor)
        XCHG                    ; DE = score
        LHLD    AAP
        MOV     M,E
        INX     H
        MOV     M,D
        INX     H
        SHLD    AAP
        LHLD    AKJP
        LXI     D,32            ; next K row (D*2)
        DAD     D
        SHLD    AKJP
        LDA     AAJ
        DCR     A
        STA     AAJ
        JNZ     ATSCJ
        LHLD    AQIP
        LXI     D,32            ; next Q row
        DAD     D
        SHLD    AQIP
        LDA     AAI
        DCR     A
        STA     AAI
        JNZ     ATSCI
; Step 5: softmax each score row (S entries)
        LXI     H,WAB
        SHLD    AAP
        MVI     A,S
        STA     AAI
ATSFT:  LHLD    AAP
        SHLD    SFVEC
        MVI     A,S
        STA     SFN
        CALL    SFTMX
        LHLD    AAP
        LXI     D,S+S           ; next row (S ints = 16 bytes)
        DAD     D
        SHLD    AAP
        LDA     AAI
        DCR     A
        STA     AAI
        JNZ     ATSFT
; Step 6: Y[i] = V^T . A[i]
        LXI     H,WAB
        SHLD    AAP             ; A[i]
        LXI     H,YY
        SHLD    AQP             ; Y[i]
        MVI     A,S
        STA     AAI
ATYI:   LXI     H,WVB
        SHLD    VTMAT
        LHLD    AAP
        SHLD    VTVIN
        LHLD    AQP
        SHLD    VTVOUT
        MVI     A,S
        STA     VTROWS
        MVI     A,DM
        STA     VTCOLS
        CALL    VTMUL
        LHLD    AAP
        LXI     D,S+S
        DAD     D
        SHLD    AAP
        LHLD    AQP
        LXI     D,32
        DAD     D
        SHLD    AQP
        LDA     AAI
        DCR     A
        STA     AAI
        JNZ     ATYI
; Step 7: residual Y += X  (S*D ints)
        LXI     H,YY
        SHLD    AQP
        LXI     H,XX
        SHLD    AXIP
        LXI     H,S*DM
        SHLD    ARCNT
ATRES:  LHLD    AQP
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = yy
        LHLD    AXIP
        MOV     C,M
        INX     H
        MOV     B,M
        INX     H
        SHLD    AXIP            ; BC = xx
        MOV     A,E
        ADD     C
        MOV     E,A
        MOV     A,D
        ADC     B
        MOV     D,A             ; DE = yy+xx
        LHLD    AQP
        MOV     M,E
        INX     H
        MOV     M,D
        INX     H
        SHLD    AQP
        LHLD    ARCNT
        DCX     H
        SHLD    ARCNT
        MOV     A,H
        ORA     L
        JNZ     ATRES
        RET

; ==========================================================================
; PROJ - logits[i] = Wout^T . Y[i]
; ==========================================================================
PROJ:   LXI     H,YY
        SHLD    AQP             ; Y[i]
        LXI     H,LOGB
        SHLD    AAP             ; logits row
        MVI     A,S
        STA     AAI
PRJI:   LXI     H,QWOTB
        SHLD    VTMAT
        LHLD    AQP
        SHLD    VTVIN
        LHLD    AAP
        SHLD    VTVOUT
        MVI     A,DM
        STA     VTROWS
        MVI     A,V
        STA     VTCOLS
        CALL    VTMUL
        LHLD    AQP
        LXI     D,32            ; next Y row (D*2)
        DAD     D
        SHLD    AQP
        LHLD    AAP
        LXI     D,V+V           ; next logits row (V*2)
        DAD     D
        SHLD    AAP
        LDA     AAI
        DCR     A
        STA     AAI
        JNZ     PRJI
        RET

; ==========================================================================
; VTMUL - vout[j] = sum_i (mat[i][j] * vin[i]) >> 8, saturating.
;         Inputs: VTMAT, VTVIN, VTVOUT (ptrs), VTROWS, VTCOLS (counts).
; ==========================================================================
VTMUL:  LHLD    VTVOUT          ; clear vout (cols ints)
        XCHG
        LDA     VTCOLS
        MOV     B,A
VTCLR:  XRA     A
        STAX    D
        INX     D
        STAX    D
        INX     D
        DCR     B
        JNZ     VTCLR
        LHLD    VTMAT
        SHLD    MATP
        LHLD    VTVIN
        SHLD    VINP
        LDA     VTROWS
        STA     VTRI
VTROW:  LHLD    VINP            ; sc = vin[i]
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    VINP
        XCHG
        SHLD    VSC
        LHLD    VTVOUT
        SHLD    VOUTP
        LDA     VTCOLS
        STA     VTCJ
VTCOL:  LHLD    MATP            ; mat element
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    MATP
        XCHG
        SHLD    MULA
        LHLD    VSC
        SHLD    MULB
        CALL    PRODAB          ; PROD = mat * sc
        LXI     H,PROD
        CALL    LQ8             ; HL = (mat*sc)>>8 clamp
        XCHG                    ; DE = t
        LHLD    VOUTP
        CALL    ADDCL           ; vout[j] += t, saturating
        LHLD    VOUTP
        INX     H
        INX     H
        SHLD    VOUTP
        LDA     VTCJ
        DCR     A
        STA     VTCJ
        JNZ     VTCOL
        LDA     VTRI
        DCR     A
        STA     VTRI
        JNZ     VTROW
        RET

; ==========================================================================
; VDOT - HL = lq8(sum_i x[i]*y[i]).  Inputs XPTR, YPTR, VDN (count).
; ==========================================================================
VDOT:   LXI     H,0
        SHLD    ACC
        SHLD    ACC+2
VDLP:   LHLD    XPTR
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    XPTR
        XCHG
        SHLD    MULA
        LHLD    YPTR
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    YPTR
        XCHG
        SHLD    MULB
        CALL    PRODAB
        LXI     H,ACC
        LXI     D,PROD
        CALL    ADD44
        LDA     VDN
        DCR     A
        STA     VDN
        JNZ     VDLP
        LXI     H,ACC
        CALL    LQ8
        RET

; ==========================================================================
; SFTMX - softmax in place over SFVEC (SFN entries), LUT based.
; ==========================================================================
SFTMX:  LHLD    SFVEC           ; mx = vmax(vec,n)
        SHLD    VPTR
        LDA     SFN
        STA     VN
        CALL    VMAX            ; MXVAL = max
        LHLD    SFVEC
        SHLD    SFP
        LDA     SFN
        STA     SFI
        LXI     H,0
        SHLD    SUM
SFP1:   LHLD    SFP
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = vec[i]
        LHLD    MXVAL
        MOV     A,L
        SUB     E
        MOV     C,A
        MOV     A,H
        SBB     D
        MOV     B,A             ; BC = mx - vec[i]
        MOV     A,B
        ANI     80H
        JZ      SFDOK
        LXI     B,0             ; d < 0 -> 0
SFDOK:  MOV     H,B
        MOV     L,C
        CALL    SHR3            ; idx = d >> 3
        MOV     A,H
        ORA     A
        JNZ     SFCAP
        MOV     A,L
        JMP     SFIDX
SFCAP:  MVI     A,255
SFIDX:  MOV     L,A             ; A = idx (0..255)
        MVI     H,0
        DAD     H               ; *2
        LXI     D,EXPTBL
        DAD     D
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = exptbl[idx]
        LHLD    SFP
        MOV     M,E
        INX     H
        MOV     M,D             ; vec[i] = exptbl[idx]
        LHLD    SUM
        DAD     D
        SHLD    SUM             ; sum += value
        LHLD    SFP
        INX     H
        INX     H
        SHLD    SFP
        LDA     SFI
        DCR     A
        STA     SFI
        JNZ     SFP1
        ; pass 2: vec[i] = fxdiv(vec[i], sum)
        LHLD    SFVEC
        SHLD    SFP
        LDA     SFN
        STA     SFI
SFP2:   LHLD    SFP
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = vec[i]
        LHLD    SUM             ; HL = sum
        XCHG                    ; HL = vec[i], DE = sum
        CALL    FXDIV           ; HL = (vec[i]<<8)/sum, clamp
        XCHG                    ; DE = result
        LHLD    SFP
        MOV     M,E
        INX     H
        MOV     M,D
        INX     H
        SHLD    SFP
        LDA     SFI
        DCR     A
        STA     SFI
        JNZ     SFP2
        RET

; ==========================================================================
; VMAX - find max of VPTR[0..VN-1]; MXVAL = max value, IDX = max index.
; ==========================================================================
VMAX:   LHLD    VPTR
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    VMP             ; VMP -> vec[1]
        XCHG
        SHLD    MXVAL           ; mx = vec[0]
        XRA     A
        STA     IDX
        MVI     A,1
        STA     VMI
VMLP:   LDA     VMI
        MOV     B,A
        LDA     VN
        CMP     B
        JZ      VMDONE          ; i == n
        LHLD    VMP
        MOV     E,M
        INX     H
        MOV     D,M
        INX     H
        SHLD    VMP             ; DE = vec[i]
        LHLD    MXVAL
        XCHG                    ; HL = vec[i], DE = mx
        CALL    SGT             ; A = 1 if vec[i] > mx
        ORA     A
        JZ      VMNX
        ; new max
        ; HL still = vec[i]? SGT preserves? recompute from VMP-2
        LHLD    VMP
        DCX     H
        DCX     H
        MOV     E,M
        INX     H
        MOV     D,M             ; DE = vec[i]
        XCHG
        SHLD    MXVAL
        LDA     VMI
        STA     IDX
VMNX:   LDA     VMI
        INR     A
        STA     VMI
        JMP     VMLP
VMDONE: LHLD    MXVAL
        RET

; ==========================================================================
; SGT - A = 1 if (HL > DE) signed, else 0.  Clobbers A only.
; ==========================================================================
SGT:    MOV     A,H
        XRA     D
        JM      SGTDIF
        MOV     A,H             ; same sign: unsigned compare
        CMP     D
        JC      SGTNO
        JNZ     SGTYES
        MOV     A,L
        CMP     E
        JC      SGTNO
        JNZ     SGTYES
        JMP     SGTNO
SGTDIF: MOV     A,H             ; signs differ: HL>DE iff HL positive
        ORA     A
        JP      SGTYES
SGTNO:  XRA     A
        RET
SGTYES: MVI     A,1
        RET

; ==========================================================================
; PRODAB - PROD(4 LE) = MULA * MULB, signed 16x16 -> 32.
; ==========================================================================
PRODAB: XRA     A
        STA     PSIGN
        LHLD    MULA
        MOV     A,H
        ANI     80H
        JZ      PAAP
        CALL    NEGHL
        LDA     PSIGN
        XRI     1
        STA     PSIGN
PAAP:   SHLD    UA
        LHLD    MULB
        MOV     A,H
        ANI     80H
        JZ      PABP
        CALL    NEGHL
        LDA     PSIGN
        XRI     1
        STA     PSIGN
PABP:   SHLD    UB
        CALL    UMUL
        LDA     PSIGN
        ORA     A
        RZ
        LXI     H,PROD
        CALL    NEG32
        RET

; UMUL - PROD(4 LE) = UA * UB, unsigned 16x16 -> 32.
UMUL:   LHLD    UA
        SHLD    M32
        LXI     H,0
        SHLD    M32+2
        SHLD    PROD
        SHLD    PROD+2
        LHLD    UB
        MOV     B,H
        MOV     C,L             ; BC = multiplier
        MVI     A,16
        STA     UCNT
UMLP:   MOV     A,C
        ANI     1
        JZ      UMNOAD
        PUSH    B               ; ADD44 uses B as its byte counter
        LXI     H,PROD
        LXI     D,M32
        CALL    ADD44
        POP     B
UMNOAD: ORA     A               ; clear carry (A nonzero ok, just clears CY)
        MOV     A,B
        RAR
        MOV     B,A
        MOV     A,C
        RAR
        MOV     C,A             ; UB >>= 1 (logical)
        PUSH    B               ; SHL32 uses B as its byte counter
        LXI     H,M32
        CALL    SHL32           ; M32 <<= 1
        POP     B
        LDA     UCNT
        DCR     A
        STA     UCNT
        JNZ     UMLP
        RET

; NEGHL - HL = -HL
NEGHL:  MOV     A,H
        CMA
        MOV     H,A
        MOV     A,L
        CMA
        MOV     L,A
        INX     H
        RET

; ADD44 - 4-byte LE: (HL) += (DE)
ADD44:  MVI     B,4
        XRA     A               ; clear carry
AD4LP:  LDAX    D
        ADC     M
        MOV     M,A
        INX     H
        INX     D
        DCR     B
        JNZ     AD4LP
        RET

; SHL32 - 4-byte LE at (HL) <<= 1
SHL32:  MVI     B,4
        XRA     A               ; clear carry
SH4LP:  MOV     A,M
        RAL
        MOV     M,A
        INX     H
        DCR     B
        JNZ     SH4LP
        RET

; NEG32 - 4-byte LE at (HL) = -value
NEG32:  PUSH    H
        MVI     B,4
NG4C:   MOV     A,M
        CMA
        MOV     M,A
        INX     H
        DCR     B
        JNZ     NG4C
        POP     H
        MVI     B,4
        STC                     ; carry = 1 -> add 1
NG4A:   MOV     A,M
        ACI     0
        MOV     M,A
        INX     H
        DCR     B
        JNZ     NG4A
        RET

; ==========================================================================
; LQ8 - HL = signed (src 4-byte LE)/256 toward zero, clamped to int16.
;       Input: HL -> 4-byte LE value.
; ==========================================================================
LQ8:    LXI     D,LWORK         ; copy src -> LWORK
        MVI     B,4
LQCP:   MOV     A,M
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     LQCP
        LDA     LWORK+3
        ANI     80H
        STA     LQSGN
        JZ      LQPOS
        LXI     H,LWORK
        CALL    NEG32           ; magnitude
LQPOS:  LDA     LWORK+3         ; m2 (after >>8: byte3 is top)
        ORA     A
        JNZ     LQOVF
        LDA     LWORK+1
        MOV     L,A
        LDA     LWORK+2
        MOV     H,A             ; HL = low 16 of magnitude>>8
        MOV     A,H
        ANI     80H
        JNZ     LQBIG
        ; 0..32767
        LDA     LQSGN
        ORA     A
        RZ
        CALL    NEGHL
        RET
LQBIG:  LDA     LQSGN
        ORA     A
        JZ      LQOVFP          ; positive -> clamp 32767
        MOV     A,H             ; negative: val > 0x8000 ?
        CPI     80H
        JNZ     LQOVFN
        MOV     A,L
        ORA     A
        JNZ     LQOVFN
        LXI     H,8000H         ; val == 0x8000 -> -32768
        RET
LQOVF:  LDA     LQSGN
        ORA     A
        JZ      LQOVFP
LQOVFN: LXI     H,8000H
        RET
LQOVFP: LXI     H,7FFFH
        RET

; ==========================================================================
; ADDCL - saturating 16-bit add: (HL) += DE, clamp to int16.
;         Input: HL -> int16 dst, DE = addend.
; ==========================================================================
ADDCL:  PUSH    H               ; save dst ptr
        MOV     C,M
        INX     H
        MOV     B,M             ; BC = dst value
        MOV     A,C
        ADD     E
        MOV     L,A
        MOV     A,B
        ADC     D
        MOV     H,A             ; HL = sum
        MOV     A,B
        XRA     D
        JM      ADCST           ; operands differ in sign -> no overflow
        MOV     A,B
        XRA     H
        JP      ADCST           ; result keeps sign -> ok
        MOV     A,B
        ANI     80H
        JZ      ADCPOS
        LXI     H,8000H
        JMP     ADCST
ADCPOS: LXI     H,7FFFH
ADCST:  POP     D               ; DE = dst ptr
        MOV     A,L
        STAX    D
        INX     D
        MOV     A,H
        STAX    D
        RET

; ==========================================================================
; ASR2 - HL = floor(HL / 4), arithmetic (sign preserving).
; ==========================================================================
ASR2:   CALL    ASR1
ASR1:   MOV     A,H             ; carry = sign bit of H
        RAL
        MOV     A,H
        RAR
        MOV     H,A
        MOV     A,L
        RAR
        MOV     L,A
        RET

; ==========================================================================
; SHR3 - HL = HL >> 3 (logical).
; ==========================================================================
SHR3:   MVI     B,3
SHR3L:  XRA     A               ; clear carry
        MOV     A,H
        RAR
        MOV     H,A
        MOV     A,L
        RAR
        MOV     L,A
        DCR     B
        JNZ     SHR3L
        RET

; ==========================================================================
; FXDIV - HL = (a << 8) / b, unsigned, clamp to int16.
;         Input: HL = a (>=0), DE = b (>0).
; ==========================================================================
FXDIV:  XRA     A
        STA     NUM
        STA     NUM+3
        MOV     A,L
        STA     NUM+1
        MOV     A,H
        STA     NUM+2           ; NUM = a << 8
        XCHG
        SHLD    DVS             ; DVS = b
        LXI     B,0             ; REM = 0
        MVI     A,32
        STA     FXCNT
FXLP:   LXI     H,NUM           ; NUM <<= 1, MSB -> carry
        PUSH    B               ; preserve REM across SHL32
        CALL    SHL32
        POP     B
        MOV     A,C             ; REM = (REM<<1) | carry
        RAL
        MOV     C,A
        MOV     A,B
        RAL
        MOV     B,A
        LHLD    DVS
        XCHG                    ; DE = DVS
        MOV     A,C             ; REM - DVS
        SUB     E
        MOV     L,A
        MOV     A,B
        SBB     D
        MOV     H,A
        JC      FXSKIP          ; REM < DVS
        MOV     C,L
        MOV     B,H             ; REM = REM - DVS
        LXI     H,NUM
        MOV     A,M
        ORI     1
        MOV     M,A             ; quotient bit
FXSKIP: LDA     FXCNT
        DCR     A
        STA     FXCNT
        JNZ     FXLP
        LDA     NUM+2           ; clamp quotient to int16
        MOV     B,A
        LDA     NUM+3
        ORA     B
        JNZ     FXBIG
        LHLD    NUM
        MOV     A,H
        ANI     80H
        JNZ     FXBIG
        RET
FXBIG:  LXI     H,7FFFH
        RET

; ==========================================================================
; MKFCB - build FIN (drive 0, name.ext) from NUL-terminated name at HL.
; ==========================================================================
MKFCB:  XRA     A
        STA     FIN
        PUSH    H
        LXI     H,FIN+1
        MVI     B,11
MFSPC:  MVI     M,' '
        INX     H
        DCR     B
        JNZ     MFSPC
        POP     H
        LXI     D,FIN+1
        MVI     B,8
MFNMLP: MOV     A,M
        ORA     A
        JZ      MFDONE
        CPI     '.'
        JZ      MFDOT
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     MFNMLP
MFSKN:  MOV     A,M
        ORA     A
        JZ      MFDONE
        CPI     '.'
        JZ      MFDOT
        INX     H
        JMP     MFSKN
MFDOT:  INX     H
        LXI     D,FIN+9
        MVI     B,3
MFEXLP: MOV     A,M
        ORA     A
        JZ      MFDONE
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     MFEXLP
MFDONE: XRA     A
        STA     FIN+12
        STA     FIN+13
        STA     FIN+14
        STA     FIN+15
        STA     FIN+32
        RET

; ==========================================================================
; PRDIG - print digit (A = 0..9) followed by a space.
; ==========================================================================
PRDIG:  ADI     '0'
        CALL    COUT
        MVI     A,' '
        CALL    COUT
        RET

; ==========================================================================
; PRDEC - print HL as unsigned decimal (no leading zeros).
; ==========================================================================
PRDEC:  MOV     A,H
        ORA     L
        JNZ     PRD1
        MVI     A,'0'           ; value 0
        CALL    COUT
        RET
PRD1:   MVI     C,0             ; digit count
        PUSH    H               ; keep value while setting digit pointer
        LXI     H,DBUF+8        ; build digits backwards
        SHLD    DBPTR
        POP     H
PRDLP:  MOV     A,H
        ORA     L
        JZ      PRDPR
        CALL    UDV10           ; HL/=10, A=remainder
        ADI     '0'
        MOV     B,A
        PUSH    H               ; save quotient; UDV10 uses DE
        LHLD    DBPTR
        DCX     H
        SHLD    DBPTR
        MOV     M,B
        POP     H
        INR     C
        JMP     PRDLP
PRDPR:  LHLD    DBPTR           ; HL -> first digit, C = count
PRDP1:  MOV     A,C
        ORA     A
        RZ
        MOV     A,M
        CALL    COUT
        INX     H
        DCR     C
        JMP     PRDP1

; UDV10 - HL = HL/10, A = remainder (repeated subtraction).
UDV10:  LXI     D,0
UDVLP:  MOV     A,H
        ORA     A
        JNZ     UDVSUB
        MOV     A,L
        CPI     10
        JC      UDVDN
UDVSUB: MOV     A,L
        SUI     10
        MOV     L,A
        MOV     A,H
        SBI     0
        MOV     H,A
        INX     D
        JMP     UDVLP
UDVDN:  MOV     A,L             ; remainder
        XCHG                    ; HL = quotient
        RET

; ==========================================================================
; PUTSTR - print NUL-terminated string at HL via console out.
; ==========================================================================
PUTSTR: MOV     A,M
        ORA     A
        RZ
        CALL    COUT
        INX     H
        JMP     PUTSTR

; ==========================================================================
; PSTR - print '$'-terminated string at DE via BDOS func 9.
; ==========================================================================
PSTR:   PUSH    H
        PUSH    D
        PUSH    B
        MVI     C,FPRSTR
        CALL    BDOSE
        POP     B
        POP     D
        POP     H
        RET

; ==========================================================================
; COUT - print char in A via BDOS func 2; preserves all registers.
; ==========================================================================
COUT:   PUSH    H
        PUSH    D
        PUSH    B
        PUSH    PSW
        MOV     E,A
        MVI     C,FCONO
        CALL    BDOSE
        POP     PSW
        POP     B
        POP     D
        POP     H
        RET

; ==========================================================================
; Messages
; ==========================================================================
MBAN1:  DB      'attn/11 inference - paper tape is all you need',13,10,'$'
MLOAD:  DB      'loading weights from ATTN.WTS ...',13,10,13,10,'$'
MNOWTS: DB      'no weights file found - run ',27H,'attn -t',27H,' first',13,10,'$'
MNOIN:  DB      'cannot open input file ','$'
MHELP:  DB      'attninfr - tiny transformer that reverses 8 digits',13,10,13,10
        DB      'usage:',13,10
        DB      '  attninfr          infer from ATTN.IN',13,10
        DB      '  attninfr <file>   infer from <file>',13,10
        DB      '  attninfr -h       this help',13,10,'$'
MARROW: DB      '-> ','$'
MOK:    DB      ' ok',13,10,'$'
MFAIL:  DB      ' fail',13,10,'$'
MACC:   DB      13,10,'accuracy  ','$'
MCRLF:  DB      13,10,'$'
DEFIN:  DB      'ATTN.IN',0

; ==========================================================================
; exptbl[256] - exp(-i/32) in Q8 (identical to ATTN.C)
; ==========================================================================
EXPTBL: DW      256,248,240,233,226,219,212,206,199,193,187,182,176,171,165,160
        DW      155,150,146,141,137,133,129,125,121,117,114,110,107,103,100,97
        DW      94,91,88,86,83,81,78,76,73,71,69,67,65,63,61,59
        DW      57,55,54,52,50,49,47,46,44,43,42,41,39,38,37,36
        DW      35,34,33,32,31,30,29,28,27,26,25,25,24,23,22,22
        DW      21,20,20,19,19,18,17,17,16,16,15,15,14,14,14,13
        DW      13,12,12,12,11,11,11,10,10,10,9,9,9,8,8,8
        DW      8,7,7,7,7,7,6,6,6,6,6,5,5,5,5,5
        DW      5,5,4,4,4,4,4,4,4,4,3,3,3,3,3,3
        DW      3,3,3,3,3,2,2,2,2,2,2,2,2,2,2,2
        DW      2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1
        DW      1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
        DW      1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0
        DW      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
        DW      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
        DW      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

; ==========================================================================
; Weights FCB (ATTN.WTS on current drive), pre-built
; ==========================================================================
FWTS:   DB      0,'ATTN    ','WTS',0,0,0,0
        DB      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
        DB      0

; ==========================================================================
; Scratch / state
; ==========================================================================
OLDSP:  DS      2
FIN:    DS      36              ; input file FCB
FNAME:  DS      20

MULA:   DS      2
MULB:   DS      2
UA:     DS      2
UB:     DS      2
PSIGN:  DS      1
PROD:   DS      4
M32:    DS      4
ACC:    DS      4
LWORK:  DS      4
LQSGN:  DS      1
UCNT:   DS      1
CVT4:   DS      4
NUM:    DS      4
DVS:    DS      2
FXCNT:  DS      1

VPTR:   DS      2
VN:     DS      1
MXVAL:  DS      2
IDX:    DS      1
VMP:    DS      2
VMI:    DS      1

XPTR:   DS      2
YPTR:   DS      2
VDN:    DS      1

VTMAT:  DS      2
VTVIN:  DS      2
VTVOUT: DS      2
VTROWS: DS      1
VTCOLS: DS      1
MATP:   DS      2
VINP:   DS      2
VOUTP:  DS      2
VSC:    DS      2
VTRI:   DS      1
VTCJ:   DS      1

EXXP:   DS      2
EPSEP:  DS      2
ETOKP:  DS      2
ETKEP:  DS      2
EMBI:   DS      1
EMBJ:   DS      1

AXIP:   DS      2
AQP:    DS      2
AKP:    DS      2
AVP:    DS      2
AQIP:   DS      2
AKJP:   DS      2
AAP:    DS      2
AAI:    DS      1
AAJ:    DS      1
ARCNT:  DS      2

SFVEC:  DS      2
SFN:    DS      1
SFP:    DS      2
SFI:    DS      1
SUM:    DS      2

WBUFP:  DS      2
QBUFP:  DS      2
CVCNT:  DS      2
DMAP:   DS      2
WCNT:   DS      1

ISP:    DS      2
ISI:    DS      1
OKF:    DS      1
FHITS:  DS      2
NSEQ:   DS      2
DIGI:   DS      1

DBUF:   DS      8
DBPTR:  DS      2

SECBUF: DS      128

; --- big arrays -----------------------------------------------------------
WBUF:   DS      4864            ; Q16 weights (38 sectors)
QBUF:   DS      2432            ; Q8 weights (1216 ints)
WORKB:  DS      896            ; attn workspace Q|K|V|A
XX:     DS      256             ; embeddings
YY:     DS      256             ; attn output
LOGB:   DS      160             ; logits (S*V ints)
TOKENS: DS      16              ; current 8 tokens

QTKEB   EQU     QBUF
QPSEB   EQU     QBUF+320
QWQB    EQU     QBUF+576
QWKB    EQU     QBUF+1088
QWVB    EQU     QBUF+1600
QWOTB   EQU     QBUF+2112

WQB     EQU     WORKB
WKB     EQU     WORKB+256
WVB     EQU     WORKB+512
WAB     EQU     WORKB+768

        DS      512
STACK:

        END     START
