; ============================================================================
; ESP32 - ESP32 Stats CLI Tool for CP/M (MAC assembly port)
; ============================================================================
; Prints system and network statistics from the emulator's ESP32 I/O ports.
; Plain CLI output: no screen layout, colors, or cursor control.
;
; Build (CP/M):  mac esp32 / load esp32  ->  ESP32.COM
;
; MAC constraints: symbols unique in first 6 characters; ports in decimal.
; ============================================================================

BDOS    EQU     5               ; CP/M BDOS entry point
FCONO   EQU     2               ; console output, char in E
FPRST   EQU     9               ; print '$'-terminated string at DE

INFOP   EQU     48              ; system info selector port
UPORT   EQU     41              ; uptime selector port
VPORT   EQU     70              ; emulator version selector port
RPORT   EQU     200             ; shared response port

CR      EQU     0DH
LF      EQU     0AH

        ORG     0100H

START:  LXI     H,0             ; save CCP stack and use our own
        DAD     SP
        SHLD    OLDSP
        LXI     SP,STACK

        LXI     D,MHEAD
        CALL    PSTR
        LXI     D,MSUB
        CALL    PSTR
        CALL    CRLF
        LXI     D,MSYS
        CALL    PSTR

        MVI     A,0             ; hostname
        OUT     INFOP
        CALL    RDSTR
        LXI     D,LHOST
        CALL    FIELD

        MVI     A,1             ; WiFi IP address
        OUT     INFOP
        CALL    RDSTR
        LXI     D,LWIFI
        CALL    FIELD

        MVI     A,2             ; device ID
        OUT     INFOP
        CALL    RDSTR
        LXI     D,LDEVID
        CALL    FIELD

        MVI     A,0             ; emulator version
        OUT     VPORT
        CALL    RDSTR
        LXI     D,LEMU
        CALL    FIELD

        MVI     A,1             ; uptime in seconds
        OUT     UPORT
        CALL    RDSTR
        CALL    CRLF
        LXI     D,MUPH
        CALL    PSTR
        LXI     D,LSECS
        CALL    FIELD

        CALL    DIV36           ; BUF / 3600 -> QBUF hours, REMV seconds
        CALL    DIV60           ; REMV / 60 -> MINV minutes
        LXI     D,LHM
        CALL    PSTR
        LXI     H,QBUF
        CALL    PNUL
        MVI     A,':'
        CALL    PCHR
        CALL    PR2D
        CALL    CRLF

EXIT:   LHLD    OLDSP
        SPHL
        RET

; -- ESP32 response helpers -------------------------------------------------
RDSTR:  LXI     H,BUF           ; read NUL-terminated response into BUF
        MVI     B,255
RDS1:   IN      RPORT
        ORA     A
        JZ      RDS0
        MOV     M,A
        INX     H
        DCR     B
        JNZ     RDS1
RDS2:   IN      RPORT           ; consume any oversized response tail
        ORA     A
        JNZ     RDS2
RDS0:   MVI     M,0
        RET

FIELD:  CALL    PSTR            ; DE -> padded field label, value is BUF
        LXI     H,BUF
        CALL    PNUL
        JMP     CRLF

; -- uptime formatting ------------------------------------------------------
DIV36:  LXI     H,0             ; decimal long divide BUF by 3600
        SHLD    REMV
        XRA     A
        STA     QANY
        LXI     H,QBUF
        SHLD    QWR
        LXI     H,BUF
        SHLD    SRCPT
D361:   LHLD    SRCPT
        MOV     A,M
        ORA     A
        JZ      D365
        INX     H
        SHLD    SRCPT
        CPI     '0'
        JC      D361
        CPI     '9'+1
        JNC     D361
        SUI     '0'
        MOV     C,A
        LHLD    REMV
        CALL    MUL10
        MOV     A,L
        ADD     C
        MOV     L,A
        MOV     A,H
        ACI     0
        MOV     H,A
        MVI     B,'0'
D362:   MOV     A,L             ; subtract 3600 while possible
        SUI     010H
        MOV     E,A
        MOV     A,H
        SBI     0EH
        JC      D363
        MOV     H,A
        MOV     L,E
        INR     B
        JMP     D362
D363:   SHLD    REMV
        LDA     QANY
        ORA     A
        JNZ     D364
        MOV     A,B
        CPI     '0'
        JZ      D361
        MVI     A,1
        STA     QANY
D364:   LHLD    QWR
        MOV     M,B
        INX     H
        SHLD    QWR
        JMP     D361
D365:   LDA     QANY
        ORA     A
        JNZ     D366
        LHLD    QWR
        MVI     M,'0'
        INX     H
        SHLD    QWR
D366:   LHLD    QWR
        MVI     M,0
        RET

MUL10:  PUSH    D               ; HL = HL * 10
        MOV     D,H
        MOV     E,L
        DAD     H
        DAD     H
        DAD     H
        DAD     D
        DAD     D
        POP     D
        RET

DIV60:  LHLD    REMV            ; REMV / 60 -> MINV
        MVI     B,0
D601:   MOV     A,L
        SUI     60
        MOV     E,A
        MOV     A,H
        SBI     0
        JC      D602
        MOV     H,A
        MOV     L,E
        INR     B
        JMP     D601
D602:   MOV     A,B
        STA     MINV
        RET

PR2D:   LDA     MINV            ; print minutes as two decimal digits
        MVI     B,'0'
P2D1:   CPI     10
        JC      P2D2
        SUI     10
        INR     B
        JMP     P2D1
P2D2:   MOV     C,A
        MOV     A,B
        CALL    PCHR
        MOV     A,C
        ADI     '0'
        JMP     PCHR

; -- console output ---------------------------------------------------------
PNUL:   MOV     A,M             ; print NUL-terminated string at HL
        ORA     A
        RZ
        CALL    PCHR
        INX     H
        JMP     PNUL

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
MHEAD:  DB      'ESP32 Stats - Altair 8800',CR,LF,'$'
MSUB:   DB      'Onboard system and network status',CR,LF,'$'
MSYS:   DB      'System',CR,LF,'$'
LHOST:  DB      '  Hostname        ','$'
LWIFI:  DB      '  WiFi IP         ','$'
LDEVID: DB      '  Device ID       ','$'
LEMU:   DB      '  Emulator        ','$'
MUPH:   DB      'Uptime',CR,LF,'$'
LSECS:  DB      '  Seconds         ','$'
LHM:    DB      '  Hours:mins      ','$'
MCRLF:  DB      CR,LF,'$'

; -- state ------------------------------------------------------------------
OLDSP:  DW      0
SRCPT:  DW      0
QWR:    DW      0
REMV:   DW      0
QANY:   DB      0
MINV:   DB      0
BUF:    DS      256
QBUF:   DS      12
        DS      128
STACK   EQU     $

        END