; ============================================================================
; MCPDONE - Print a completion banner plus the command-line arguments
; ============================================================================
; MAC (macro assembler) port of MCPDONE.C, feature for feature:
;
;   printf("MCP-TOOL-COMPLETED");
;   for (i = 1; i < argc; i++)
;       printf(" %s", argv[i]);
;   printf("\n");
;
; The CP/M CCP places the command tail at 0080H: a length byte followed by
; the raw characters typed after the program name (already upper-cased). We
; reproduce the C argv behaviour by walking that tail, collapsing runs of
; spaces, and emitting exactly one leading space before each argument token.
;
; This version uses Digital Research MAC macros for the BDOS calls. Build it
; with the macro assembler rather than ASM:
;
;   mac mcpdone        ->  MCPDONE.HEX (plus .PRN/.SYM)
;   load mcpdone       ->  MCPDONE.COM
;
; MAC constraints: symbols unique in first 6 chars; ports in decimal.
; ============================================================================

BDOS    EQU     5               ; CP/M BDOS entry point
FPRCHR  EQU     2               ; BDOS func 2: console output (char in E)
FPRSTR  EQU     9               ; BDOS func 9: print '$'-terminated string
TAILLN  EQU     0080H           ; command tail length byte
TAILCH  EQU     0081H           ; first character of the command tail
SPACE   EQU     ' '

; -- macro: invoke a BDOS function -----------------------------------------
DOS     MACRO   FUNC
        MVI     C,FUNC
        CALL    BDOS
        ENDM

; -- macro: print a '$'-terminated string at label STR ---------------------
PRINT   MACRO   STR
        LXI     D,STR
        DOS     FPRSTR
        ENDM

; -- macro: print the character in E (preserves HL and BC) -----------------
EMITCH  MACRO
        PUSH    H
        PUSH    B
        DOS     FPRCHR
        POP     B
        POP     H
        ENDM

        ORG     0100H           ; CP/M transient program area

START:  PRINT   BANNER          ; print the fixed banner

        LDA     TAILLN          ; B = number of bytes in the command tail
        MOV     B,A
        LXI     H,TAILCH        ; HL -> first character of the tail

; -- skip any spaces between arguments -------------------------------------
SKIPSP: MOV     A,B             ; characters remaining?
        ORA     A
        JZ      DONE            ; none -> finished
        MOV     A,M
        CPI     SPACE
        JNZ     ARGSTA          ; non-space -> start of an argument
        INX     H
        DCR     B
        JMP     SKIPSP

; -- emit one leading space, then the argument token -----------------------
ARGSTA: MVI     E,SPACE         ; one separating space before the token
        EMITCH

EMIT:   MOV     A,B             ; characters remaining?
        ORA     A
        JZ      DONE
        MOV     A,M
        CPI     SPACE
        JZ      SKIPSP          ; end of this token -> look for the next
        MOV     E,A             ; print this token character
        EMITCH
        INX     H
        DCR     B
        JMP     EMIT

DONE:   PRINT   CRLF            ; trailing newline (matches printf("\n"))
        RET                     ; return to CP/M

BANNER: DB      'MCP-TOOL-COMPLETED','$'
CRLF:   DB      0DH,0AH,'$'

        END
