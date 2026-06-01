; ============================================================================
; ENV - Environment Variable Manager for CP/M (MAC assembly port)
; ============================================================================
; Talks directly to the ESP32/host environment I/O driver on ports 71, 72,
; and 200. With a command tail, ENV forwards the tail to the host CLI parser.
; With no command tail, it opens an interactive editor that preserves mixed
; case values by reading characters through BDOS function 1.
;
; Build (CP/M):  mac env / load env  ->  ENV.COM
;
; MAC constraints: symbols unique in first 6 chars; ports in decimal.
; ============================================================================

BDOS    EQU     5               ; CP/M BDOS entry point
FCONI   EQU     1               ; console input with echo
FCONO   EQU     2               ; console output, char in E
FPRST   EQU     9               ; print '$'-terminated string at DE

TAILL   EQU     0080H           ; command tail length byte
TAILC   EQU     0081H           ; first command-tail char

MAXVR   EQU     32              ; variables cached locally
KEYSZ   EQU     17              ; 16 chars plus NUL
VALSZ   EQU     256             ; 255 chars plus NUL
LINSZ   EQU     144             ; command line buffer
PGSIZ   EQU     15              ; rows per page

ECPRT   EQU     71              ; ENV command/status port
EDPRT   EQU     72              ; ENV request data port
ERPRT   EQU     200             ; shared response port
RBPRT   EQU     49              ; guarded ESP32 reboot port

ECRST   EQU     0               ; reset request buffer
ECINI   EQU     1               ; initialize ENV storage
ECSET   EQU     3               ; set key/value
ECDEL   EQU     4               ; delete key
ECLST   EQU     5               ; list all key/value pairs
ECEXE   EQU     9               ; execute ENV CLI command

CR      EQU     0DH
LF      EQU     0AH
BS      EQU     08H
DEL     EQU     7FH
CTLC    EQU     03H
CTLU    EQU     15H
SPACE   EQU     ' '
TAB     EQU     09H

        ORG     0100H

START:  LXI     H,0             ; save CCP stack and use our own
        DAD     SP
        SHLD    OLDSP
        LXI     SP,STACK

        CALL    EINIT
        ORA     A
        JZ      STOK
        LXI     D,MINIT
        CALL    PSTR
        JMP     EXIT

STOK:   CALL    HARGS           ; command tail present?
        ORA     A
        JZ      IMODE
        CALL    EEXEC
        JMP     EXIT

EXIT:   LHLD    OLDSP
        SPHL
        RET

; -- command-tail helpers ---------------------------------------------------
HARGS:  LDA     TAILL           ; skip leading spaces and tabs
        MOV     B,A
        LXI     H,TAILC
HA1:    MOV     A,B
        ORA     A
        JZ      HA0
        MOV     A,M
        CPI     SPACE
        JZ      HA2
        CPI     TAB
        JNZ     HA3
HA2:    INX     H
        DCR     B
        JMP     HA1
HA3:    SHLD    ARGPT
        MOV     A,B
        STA     ARGLN
        MVI     A,1
        RET
HA0:    XRA     A
        STA     ARGLN
        RET

; -- ENV port wrappers ------------------------------------------------------
EINIT:  MVI     A,ECRST
        OUT     ECPRT
        MVI     A,ECINI
        OUT     ECPRT
        IN      ECPRT
        RET

EBEG:   MVI     A,ECRST
        OUT     ECPRT
        RET

ESSTR:  MOV     A,M             ; HL -> NUL-terminated string
        OUT     EDPRT
        ORA     A
        RZ
        INX     H
        JMP     ESSTR

ESVAL:  MVI     B,255           ; HL -> value, capped at 255 chars
ESV1:   MOV     A,M
        ORA     A
        JZ      ESV0
        OUT     EDPRT
        INX     H
        DCR     B
        JNZ     ESV1
ESV0:   XRA     A
        OUT     EDPRT
        RET

ESET:   PUSH    D               ; HL=key, DE=value, returns status in A
        CALL    EBEG
        CALL    ESSTR
        POP     H
        CALL    ESVAL
        MVI     A,ECSET
        OUT     ECPRT
        IN      ECPRT
        RET

EDELK:  CALL    EBEG            ; HL=key, returns status in A
        CALL    ESSTR
        MVI     A,ECDEL
        OUT     ECPRT
        IN      ECPRT
        RET

EEXEC:  CALL    EBEG            ; forward raw command tail after leading ws
        LHLD    ARGPT
        LDA     ARGLN
        MOV     B,A
EE1:    MOV     A,B
        ORA     A
        JZ      EE2
        MOV     A,M
        OUT     EDPRT
        INX     H
        DCR     B
        JMP     EE1
EE2:    XRA     A
        OUT     EDPRT
        MVI     A,ECEXE
        OUT     ECPRT
EE3:    IN      ERPRT
        ORA     A
        JZ      EE4
        CALL    PCHR
        JMP     EE3
EE4:    IN      ECPRT
        RET

; -- load and parse the environment list -----------------------------------
LOADAL: XRA     A
        STA     NVAR
        STA     LSTAT
        STA     KANY
        STA     KIDX
        STA     VIDX
        CALL    SETPT
        CALL    EBEG
        MVI     A,ECLST
        OUT     ECPRT
        IN      ECPRT
        ORA     A
        RNZ
LLP:    IN      ERPRT
        ORA     A
        JZ      LEND
        MOV     C,A
        LDA     NVAR
        CPI     MAXVR
        JNC     LDRAN
        MOV     A,C
        CPI     CR
        JZ      LLP
        CPI     LF
        JZ      LNL
        CPI     '='
        JZ      LEQ
        LDA     LSTAT
        ORA     A
        JNZ     LVST
LKST:   LDA     KIDX
        CPI     KEYSZ-1
        JNC     LLP
        LHLD    KWR
        MOV     M,C
        INX     H
        SHLD    KWR
        LDA     KIDX
        INR     A
        STA     KIDX
        MVI     A,1
        STA     KANY
        JMP     LLP
LEQ:    LDA     LSTAT
        ORA     A
        JNZ     LVST
        LHLD    KWR
        MVI     M,0
        MVI     A,1
        STA     LSTAT
        JMP     LLP
LVST:   LDA     VIDX
        CPI     VALSZ-1
        JNC     LLP
        LHLD    VWR
        MOV     M,C
        INX     H
        SHLD    VWR
        LDA     VIDX
        INR     A
        STA     VIDX
        JMP     LLP
LNL:    CALL    LTERM
        LDA     KANY
        ORA     A
        JZ      LNEW
        LDA     NVAR
        INR     A
        STA     NVAR
        CPI     MAXVR
        JNC     LDRAN
LNEW:   XRA     A
        STA     LSTAT
        STA     KANY
        STA     KIDX
        STA     VIDX
        CALL    SETPT
        JMP     LLP
LEND:   CALL    LTERM
        LDA     KANY
        ORA     A
        JZ      LRET
        LDA     NVAR
        CPI     MAXVR
        JNC     LRET
        INR     A
        STA     NVAR
LRET:   LDA     NVAR
        RET
LDRAN:  IN      ERPRT           ; consume the rest of an oversized response
        ORA     A
        JNZ     LDRAN
        LDA     NVAR
        RET

LTERM:  LHLD    KWR
        MVI     M,0
        LHLD    VWR
        MVI     M,0
        RET

SETPT:  LDA     NVAR            ; current key/value write pointers
        MOV     B,A
        LXI     H,KEYS
SPT1:   MOV     A,B
        ORA     A
        JZ      SPT2
        LXI     D,KEYSZ
        DAD     D
        DCR     B
        JMP     SPT1
SPT2:   SHLD    KBASE
        SHLD    KWR
        LDA     NVAR
        LXI     H,VALS
        ADD     H
        MOV     H,A
        SHLD    VBASE
        SHLD    VWR
        RET

; -- interactive mode -------------------------------------------------------
IMODE:  LXI     D,MHEAD
        CALL    PSTR
        CALL    LOADAL
        XRA     A
        STA     CURPG
IMLP:   CALL    SHOW
        LXI     D,MPROM
        CALL    PSTR
        LXI     H,LBUF
        MVI     A,LINSZ-1
        CALL    READL
        JC      IQUIT
        ORA     A
        JZ      IMLP
        LDA     LBUF
        CPI     'a'
        JC      ICMD
        CPI     'z'+1
        JNC     ICMD
        SUI     'a'-'A'
ICMD:   CPI     'Q'
        JZ      IQUIT
        CPI     'L'
        JNZ     IC1
        CALL    LOADAL
        CALL    FIXPG
        JMP     IMLP
IC1:    CPI     'R'
        JNZ     IC2
        CALL    DBOOT
        JMP     IMLP
IC2:    CPI     'N'
        JNZ     IC3
        CALL    NUMPG
        MOV     B,A
        LDA     CURPG
        INR     A
        CMP     B
        JNC     INLST
        STA     CURPG
        JMP     IMLP
INLST:  LXI     D,MLAST
        CALL    PSTR
        JMP     IMLP
IC3:    CPI     'P'
        JNZ     IC4
        LDA     CURPG
        ORA     A
        JZ      INFST
        DCR     A
        STA     CURPG
        JMP     IMLP
INFST:  LXI     D,MFIRST
        CALL    PSTR
        JMP     IMLP
IC4:    CPI     'A'
        JNZ     IC5
        CALL    ADDVR
        JMP     IMLP
IC5:    CPI     'D'
        JNZ     ICNUM
        LXI     H,LBUF+1
        CALL    PARN
        JC      IDUSG
        CALL    DELVR
        JMP     IMLP
IDUSG:  LXI     D,MDUSG
        CALL    PSTR
        JMP     IMLP
ICNUM:  LXI     H,LBUF
        CALL    PARN
        JC      IINV
        ORA     A
        JZ      IINV
        MOV     B,A
        LDA     NVAR
        CMP     B
        JC      IINV
        MOV     A,B
        DCR     A
        CALL    EDITV
        JMP     IMLP
IINV:   LXI     D,MINV
        CALL    PSTR
        JMP     IMLP
IQUIT:  LXI     D,MBYE
        CALL    PSTR
        JMP     EXIT

; -- page display -----------------------------------------------------------
SHOW:   CALL    CRLF
        LDA     NVAR
        ORA     A
        JNZ     SH1
        LXI     D,MEMPTY
        CALL    PSTR
        RET
SH1:    CALL    FIXPG
        CALL    NUMPG
        STA     PAGES
        LDA     CURPG
        CALL    MUL15
        STA     STARTV
        ADI     PGSIZ
        MOV     B,A
        LDA     NVAR
        CMP     B
        JC      SH2
        MOV     A,B
SH2:    STA     STOPV
        LXI     D,MPG1
        CALL    PSTR
        LDA     CURPG
        INR     A
        CALL    PRN8
        LXI     D,MSLASH
        CALL    PSTR
        LDA     PAGES
        CALL    PRN8
        LXI     D,MPG2
        CALL    PSTR
        LDA     STARTV
        INR     A
        CALL    PRN8
        LXI     D,MDASH
        CALL    PSTR
        LDA     STOPV
        CALL    PRN8
        LXI     D,MOF
        CALL    PSTR
        LDA     NVAR
        CALL    PRN8
        LXI     D,MPG3
        CALL    PSTR
        LDA     STARTV
        STA     IVAR
SHLP:   LDA     IVAR
        MOV     B,A
        LDA     STOPV
        CMP     B
        JZ      SHDN
        MVI     A,SPACE
        CALL    PCHR
        LDA     IVAR
        INR     A
        CALL    PRN8
        LXI     D,MDOT
        CALL    PSTR
        LDA     IVAR
        CALL    GETKP
        CALL    PKEY
        LXI     D,MEQ
        CALL    PSTR
        LDA     IVAR
        CALL    GETVP
        CALL    PNUL
        CALL    CRLF
        LDA     IVAR
        INR     A
        STA     IVAR
        JMP     SHLP
SHDN:   RET

NUMPG:  LDA     NVAR            ; ceil(nvars / PGSIZ), minimum 1
        ADI     PGSIZ-1
        MVI     B,0
NP1:    CPI     PGSIZ
        JC      NP2
        SUI     PGSIZ
        INR     B
        JMP     NP1
NP2:    MOV     A,B
        ORA     A
        JNZ     NP3
        INR     A
NP3:    RET

FIXPG:  CALL    NUMPG
        DCR     A
        MOV     B,A
        LDA     CURPG
        CMP     B
        RC
        MOV     A,B
        STA     CURPG
        RET

MUL15:  MOV     B,A
        XRA     A
ML1:    MOV     C,A
        MOV     A,B
        ORA     A
        MOV     A,C
        RZ
        ADI     PGSIZ
        DCR     B
        JMP     ML1

; -- edit/add/delete/reboot -------------------------------------------------
EDITV:  STA     CURIX           ; A = zero-based index
        LXI     D,MKEY
        CALL    PSTR
        LDA     CURIX
        CALL    GETKP
        CALL    PNUL
        CALL    CRLF
        LXI     D,MVAL
        CALL    PSTR
        LDA     CURIX
        CALL    GETVP
        CALL    PNUL
        CALL    CRLF
        LXI     D,MNEW
        CALL    PSTR
        LXI     H,EBUF
        MVI     A,VALSZ-1
        CALL    READL
        JC      ECAN
        ORA     A
        JZ      EUNCH
        LDA     CURIX
        CALL    GETKP
        LXI     D,EBUF
        CALL    ESET
        ORA     A
        JNZ     EFAIL
        LDA     CURIX
        CALL    GETVP
        XCHG
        LXI     H,EBUF
        CALL    SCPY
        LXI     D,MSAVE
        CALL    PSTR
        RET
ECAN:   LXI     D,MCAN
        CALL    PSTR
        RET
EUNCH:  LXI     D,MUNCH
        CALL    PSTR
        RET
EFAIL:  LXI     D,MESET
        CALL    PSTR
        RET

ADDVR:  LXI     D,MKEY
        CALL    PSTR
        LXI     H,KBUF
        MVI     A,KEYSZ-1
        CALL    READL
        JC      ADCAN
        ORA     A
        JZ      ADCAN
        LXI     D,MVAL
        CALL    PSTR
        LXI     H,EBUF
        MVI     A,VALSZ-1
        CALL    READL
        JC      ADCAN
        LXI     H,KBUF
        LXI     D,EBUF
        CALL    ESET
        ORA     A
        JNZ     ADERR
        LXI     D,MSAVE
        CALL    PSTR
        CALL    LOADAL
        RET
ADCAN:  LXI     D,MCAN
        CALL    PSTR
        RET
ADERR:  LXI     D,MESET
        CALL    PSTR
        RET

DELVR:  ORA     A               ; A = one-based index
        JZ      DVINV
        MOV     B,A
        LDA     NVAR
        CMP     B
        JC      DVINV
        MOV     A,B
        DCR     A
        STA     CURIX
        CALL    GETKP
        CALL    EDELK
        ORA     A
        JNZ     DVERR
        LXI     D,MDEL1
        CALL    PSTR
        LDA     CURIX
        CALL    GETKP
        CALL    PNUL
        LXI     D,MDEL2
        CALL    PSTR
        CALL    LOADAL
        RET
DVINV:  LXI     D,MINV
        CALL    PSTR
        RET
DVERR:  LXI     D,MEDEL
        CALL    PSTR
        RET

DBOOT:  LXI     D,MBOOT
        CALL    PSTR
        LXI     H,SBUF
        MVI     A,7
        CALL    READL
        JC      BCAN
        LDA     SBUF
        CPI     'y'
        JZ      BYES
        CPI     'Y'
        JZ      BYES
BCAN:   LXI     D,MCAN
        CALL    PSTR
        RET
BYES:   LXI     D,MRBOT
        CALL    PSTR
        MVI     A,0A5H
        OUT     RBPRT
        RET

; -- input and parsing ------------------------------------------------------
READL:  STA     RMAX            ; HL=buffer, A=max-1, returns A=len or CY
        SHLD    RBUF
        SHLD    RWR
        XRA     A
        STA     RLEN
RL1:    MVI     C,FCONI
        CALL    BDOS
        ANI     7FH
        MOV     B,A
        CPI     CR
        JZ      RLDON
        CPI     LF
        JZ      RLDON
        CPI     BS
        JZ      RLBS
        CPI     DEL
        JZ      RLBS
        CPI     CTLC
        JZ      RLCC
        CPI     CTLU
        JZ      RLCU
        CPI     SPACE
        JC      RL1
        CPI     DEL
        JNC     RL1
        LDA     RLEN
        MOV     C,A
        LDA     RMAX
        CMP     C
        JZ      RL1
        LHLD    RWR
        MOV     M,B
        INX     H
        SHLD    RWR
        LDA     RLEN
        INR     A
        STA     RLEN
        JMP     RL1
RLDON:  MVI     A,LF
        CALL    PCHR
        LHLD    RWR
        MVI     M,0
        LDA     RLEN
        ORA     A
        RET
RLBS:   LDA     RLEN
        ORA     A
        JZ      RL1
        DCR     A
        STA     RLEN
        LHLD    RWR
        DCX     H
        SHLD    RWR
        MVI     A,SPACE
        CALL    PCHR
        MVI     A,BS
        CALL    PCHR
        JMP     RL1
RLCC:   MVI     A,LF
        CALL    PCHR
        LHLD    RBUF
        MVI     M,0
        STC
        RET
RLCU:   LDA     RLEN
        ORA     A
        JZ      RL1
        MVI     A,BS
        CALL    PCHR
        MVI     A,SPACE
        CALL    PCHR
        MVI     A,BS
        CALL    PCHR
        LDA     RLEN
        DCR     A
        STA     RLEN
        LHLD    RWR
        DCX     H
        SHLD    RWR
        JMP     RLCU

PARN:   MVI     B,0             ; HL -> text, returns A=value or CY on error
        MVI     C,0
PN1:    MOV     A,M
        CPI     SPACE
        JZ      PN2
        CPI     TAB
        JNZ     PN3
PN2:    INX     H
        JMP     PN1
PN3:    MOV     A,M
        CPI     '0'
        JC      PN4
        CPI     '9'+1
        JNC     PN4
        SUI     '0'
        MOV     E,A
        MOV     A,B
        CPI     10
        JNC     PNERR
        ADD     A
        MOV     D,A
        ADD     A
        ADD     A
        ADD     D
        ADD     E
        MOV     B,A
        MVI     C,1
        INX     H
        JMP     PN3
PN4:    MOV     A,C
        ORA     A
        JZ      PNERR
        MOV     A,B
        ORA     A
        RET
PNERR:  STC
        RET

; -- string, number, and pointer helpers -----------------------------------
GETKP:  MOV     B,A             ; A=index -> HL=keys[index]
        LXI     H,KEYS
GK1:    MOV     A,B
        ORA     A
        RZ
        LXI     D,KEYSZ
        DAD     D
        DCR     B
        JMP     GK1

GETVP:  LXI     H,VALS          ; A=index -> HL=vals[index]
        ADD     H
        MOV     H,A
        RET

SCPY:   MOV     A,M             ; DE=dst, HL=src
        STAX    D
        ORA     A
        RZ
        INX     H
        INX     D
        JMP     SCPY

PNUL:   MOV     A,M             ; print NUL-terminated string at HL
        ORA     A
        RZ
        CALL    PCHR
        INX     H
        JMP     PNUL

PKEY:   MVI     B,0             ; print key at HL, pad to 16 chars
PK1:    MOV     A,M
        ORA     A
        JZ      PK2
        CALL    PCHR
        INX     H
        INR     B
        JMP     PK1
PK2:    MOV     A,B
        CPI     KEYSZ-1
        RNC
        MVI     A,SPACE
        CALL    PCHR
        INR     B
        JMP     PK2

PRN8:   MVI     H,0             ; print unsigned byte in A
        MOV     L,A
        JMP     PRDEC

DIV10:  PUSH    B               ; unsigned HL / 10 -> HL quotient, A rem
        PUSH    D
        LXI     D,0
        MVI     B,16
DVL1:   DAD     H
        MOV     A,E
        RAL
        MOV     E,A
        MOV     A,D
        RAL
        MOV     D,A
        MOV     A,E
        SUI     10
        MOV     C,A
        MOV     A,D
        SBI     0
        JC      DVL2
        MOV     D,A
        MOV     E,C
        INR     L
DVL2:   DCR     B
        JNZ     DVL1
        MOV     A,E
        POP     D
        POP     B
        RET

PRDEC:  PUSH    H               ; print unsigned HL as decimal
        PUSH    D
        PUSH    B
        LXI     D,NBEND
PD1:    CALL    DIV10
        ADI     '0'
        DCX     D
        STAX    D
        MOV     A,H
        ORA     L
        JNZ     PD1
PD2:    LDAX    D
        CALL    PCHR
        INX     D
        MOV     A,E
        LXI     H,NBEND
        CMP     L
        JNZ     PD2
        MOV     A,D
        CMP     H
        JNZ     PD2
        POP     B
        POP     D
        POP     H
        RET

PSTR:   PUSH    B               ; print '$'-terminated string at DE
        PUSH    H
        MVI     C,FPRST
        CALL    BDOS
        POP     H
        POP     B
        RET

PCHR:   PUSH    B               ; print char in A
        PUSH    D
        PUSH    H
        MOV     E,A
        MVI     C,FCONO
        CALL    BDOS
        POP     H
        POP     D
        POP     B
        RET

CRLF:   LXI     D,MCRLF
        JMP     PSTR

; -- messages ---------------------------------------------------------------
MINIT:  DB      'Error: Cannot init ENV storage',CR,LF,'$'
MHEAD:  DB      'ENV Interactive Mode',CR,LF
        DB      '====================',CR,LF,'$'
MPROM:  DB      CR,LF
        DB      '[number]=edit  A=add  D <n>=delete  N=next  P=prev'
        DB      '  L=reload  R=reboot  Q=quit',CR,LF
        DB      '> ','$'
MEMPTY: DB      '(no environment variables set)',CR,LF,'$'
MPG1:   DB      '-- Page ','$'
MSLASH: DB      '/','$'
MPG2:   DB      '  (entries ','$'
MDASH:  DB      '-','$'
MOF:    DB      ' of ','$'
MPG3:   DB      ') --',CR,LF,'$'
MDOT:   DB      '. ','$'
MEQ:    DB      ' = ','$'
MLAST:  DB      '(already on last page)',CR,LF,'$'
MFIRST: DB      '(already on first page)',CR,LF,'$'
MDUSG:  DB      'Usage: D <number>',CR,LF,'$'
MINV:   DB      'Invalid number.',CR,LF,'$'
MBYE:   DB      'Bye.',CR,LF,'$'
MKEY:   DB      CR,LF,'Key   : ','$'
MVAL:   DB      'Value : ','$'
MNEW:   DB      'New   : ','$'
MCAN:   DB      'Cancelled.',CR,LF,'$'
MUNCH:  DB      '(unchanged)',CR,LF,'$'
MSAVE:  DB      'Saved.',CR,LF,'$'
MESET:  DB      'Error: e_set failed',CR,LF,'$'
MEDEL:  DB      'Error: e_del failed',CR,LF,'$'
MDEL1:  DB      'Deleted ','$'
MDEL2:  DB      '.',CR,LF,'$'
MBOOT:  DB      CR,LF,'Reboot ESP32? (y/N) ','$'
MRBOT:  DB      'Rebooting ESP32...',CR,LF,'$'
MCRLF:  DB      CR,LF,'$'

; -- state ------------------------------------------------------------------
OLDSP:  DW      0
ARGPT:  DW      0
ARGLN:  DB      0
NVAR:   DB      0
CURPG:  DB      0
PAGES:  DB      0
STARTV: DB      0
STOPV:  DB      0
IVAR:   DB      0
CURIX:  DB      0
LSTAT:  DB      0
KANY:   DB      0
KIDX:   DB      0
VIDX:   DB      0
KBASE:  DW      0
VBASE:  DW      0
KWR:    DW      0
VWR:    DW      0
RBUF:   DW      0
RWR:    DW      0
RLEN:   DB      0
RMAX:   DB      0
NBUF:   DS      6
NBEND   EQU     $
SBUF:   DS      8
KBUF:   DS      KEYSZ
LBUF:   DS      LINSZ
EBUF:   DS      VALSZ
KEYS:   DS      MAXVR*KEYSZ
VALS:   DS      MAXVR*VALSZ
        DS      256
STACK   EQU     $

        END
