; ============================================================================
; FT - File Transfer "Get" client for the Altair 8800 (8080 macro assembler)
; ============================================================================
; MAC (Digital Research macro assembler) port of FT.C, feature for feature:
;
;   ft                     -> print help/usage banner
;   ft -g <filename>       -> download <filename> from the Remote FT Server
;   ft <anything else>     -> "Invalid arguments" message
;
; The download path is the original path sent to the server (e.g.
; SUBDIR/FOO.C); the file is saved locally under its basename (FOO.C). The
; transfer uses host I/O ports 60 (command/status) and 61 (data), exactly as
; the C version:
;
;   OUT 60,1   start a filename transfer (SETFN)
;   OUT 61,c   send each filename character, then OUT 61,0 (NUL terminator)
;   IN  60     poll status: 0=IDLE 1=DATA 2=EOF 3=BUSY 255=ERR
;   OUT 60,3   request the next data chunk (GETCH)
;   IN  61     read the chunk length byte (0 means 256), then that many bytes
;   OUT 60,4   close the transfer (CLOSE)
;
; Received bytes are written to a freshly created CP/M file via the BDOS
; sequential-write calls. fopen write mode is reproduced as delete-then-make.
; Two transfer modes, selected on the command line:
;   -g  <file>  text mode: incoming CR is stripped and LF is expanded to
;               CR LF, so the saved file always has canonical CP/M line
;               endings (works for LF-only and CRLF sources alike); a single
;               ^Z end-of-text marker is written and the final record is
;               NUL-padded. This is what SUBMIT/MAC/CC expect.
;   -gb <file>  binary mode: bytes are written verbatim with no ^Z; the final
;               partial record is padded with NUL (00H).
;
; One intentional deviation from FT.C: the byte counter is a 32-bit value
; here rather than the C version's 16-bit int, so the "Done (<n> bytes)"
; total does not wrap at 65536 for files larger than 64K.
;
; Build (CP/M):
;   mac  ft        ->  FT.HEX (plus .PRN/.SYM)
;   load ft        ->  FT.COM
;
; MAC constraints: symbols unique in first 6 chars; ports in decimal.
; ============================================================================

; -- BDOS entry and function numbers (decimal) ------------------------------
BDOS    EQU     5               ; CP/M BDOS entry point
FCONO   EQU     2               ; func 2:  console output (char in E)
FPRSTR  EQU     9               ; func 9:  print '$'-terminated string
FCLOSE  EQU     16              ; func 16: close file
FDEL    EQU     19              ; func 19: delete file
FWSEQ   EQU     21              ; func 21: write sequential
FMAKE   EQU     22              ; func 22: make (create) file
FSDMA   EQU     26              ; func 26: set DMA address

; -- host file-transfer ports (decimal) and protocol values -----------------
CPRT    EQU     60              ; command (OUT) / status (IN) port
DPRT    EQU     61              ; data (OUT filename / IN file bytes) port

CSETFN  EQU     1               ; command: start filename transfer
CGETCH  EQU     3               ; command: request next chunk
CCLOSE  EQU     4               ; command: close transfer

STDATA  EQU     1               ; status: data ready
STEOF   EQU     2               ; status: end of file
STBUSY  EQU     3               ; status: busy
STERR   EQU     255             ; status: error

; -- misc -------------------------------------------------------------------
SECSIZ  EQU     128             ; CP/M record size
CPMEOF  EQU     1AH             ; CP/M end-of-text marker (^Z)
FPAD    EQU     0               ; binary final-record pad byte (NUL)
TAILLN  EQU     0080H           ; command-tail length byte
TAILCH  EQU     0081H           ; first command-tail character

        ORG     0100H           ; CP/M transient program area

START:  LXI     H,0             ; save the CCP stack pointer
        DAD     SP
        SHLD    OLDSP
        LXI     SP,STACK        ; switch to our own stack

; --- copy the command tail to TBUF and NUL-terminate it ------------------
        LDA     TAILLN          ; B = tail length
        MOV     B,A
        LXI     H,TAILCH        ; HL -> first tail char
        LXI     D,TBUF          ; DE -> copy buffer
        ORA     A
        JZ      CPTLE           ; empty tail -> just terminate
CPTL:   MOV     A,M
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     CPTL
CPTLE:  XRA     A
        STAX    D               ; NUL terminator

; --- split TBUF into tokens (collapse spaces); keep first two pointers ----
        XRA     A
        STA     TCNT
        LXI     H,TBUF
PSKSP:  MOV     A,M             ; skip leading spaces
        ORA     A
        JZ      PARSED
        CPI     ' '
        JNZ     PATOK
        INX     H
        JMP     PSKSP
PATOK:  LDA     TCNT            ; record token pointer for index 0 / 1
        CPI     0
        JZ      PST0
        CPI     1
        JZ      PST1
        JMP     PINCNT
PST0:   SHLD    TOK0
        JMP     PINCNT
PST1:   SHLD    TOK1
PINCNT: LDA     TCNT
        INR     A
        STA     TCNT
PINTOK: MOV     A,M             ; walk to the end of this token
        ORA     A
        JZ      PARSED
        CPI     ' '
        JZ      PENDT
        INX     H
        JMP     PINTOK
PENDT:  MVI     M,0             ; terminate token in place
        INX     H
        JMP     PSKSP
PARSED:

; --- dispatch on token count (argc-1) ------------------------------------
        LDA     TCNT
        ORA     A
        JZ      HELP            ; no args -> usage banner
        CPI     2
        JZ      TWOARG          ; exactly two tokens -> maybe -g <file>
        JMP     INVAL

; --- two tokens: first must be "-G" (text) or "-GB" (binary) -------------
TWOARG: LHLD    TOK0
        MOV     A,M
        CPI     '-'
        JNZ     INVAL
        INX     H
        MOV     A,M
        CPI     'G'             ; CCP upper-cases the tail
        JNZ     INVAL
        INX     H
        MOV     A,M
        ORA     A               ; "-G" alone -> text (normalize CRLF)
        JZ      TWTEXT
        CPI     'B'             ; "-GB" -> raw binary transfer
        JNZ     INVAL
        INX     H
        MOV     A,M
        ORA     A               ; must be end of token after the B
        JNZ     INVAL
        MVI     A,1
        STA     BINMOD          ; binary mode
        JMP     DOWNLD
TWTEXT: XRA     A
        STA     BINMOD          ; text mode (normalize line endings, ^Z)

; --- DOWNLOAD: filename = TOK1, save_fn = basename ------------------------
DOWNLD: LHLD    TOK1
        SHLD    FNAMEP          ; full path sent to the server
        SHLD    SAVEFN          ; default local name = full path
        LHLD    TOK1
BNLP:   MOV     A,M             ; scan for the last '/' or '\'
        ORA     A
        JZ      BNEND
        CPI     '/'
        JZ      BNSEP
        CPI     5CH             ; backslash
        JZ      BNSEP
        INX     H
        JMP     BNLP
BNSEP:  INX     H               ; HL -> char after the separator
        MOV     A,M
        ORA     A               ; trailing separator? leave save_fn alone
        JZ      BNLP
        SHLD    SAVEFN          ; basename starts after this separator
        JMP     BNLP
BNEND:

; --- print Downloading <file> from Remote FT Server -----------------------
        LXI     D,MDOWN1
        CALL    PSTR
        LHLD    FNAMEP
        CALL    PUTSTR
        LXI     D,MDOWN2
        CALL    PSTR

; --- print Saving as <basename> only when basename differs from full path -
        LHLD    SAVEFN
        XCHG                    ; DE = save_fn
        LHLD    FNAMEP
        MOV     A,H
        CMP     D
        JNZ     DOSAVE
        MOV     A,L
        CMP     E
        JZ      NOSAVE
DOSAVE: LXI     D,MSAVE1
        CALL    PSTR
        LHLD    SAVEFN
        CALL    PUTSTR
        LXI     D,MSAVE2
        CALL    PSTR
NOSAVE:

; --- create the output file (fopen write mode = delete then make) --------
        LHLD    SAVEFN
        CALL    MKFCBR          ; build FCB from the basename
        CALL    UNLINK          ; truncate any existing file
        LXI     D,FCB
        MVI     C,FMAKE
        CALL    BDOS
        CPI     0FFH            ; 255 -> make failed
        JZ      MKERR

; --- run the transfer, then close the file -------------------------------
        LXI     H,0
        SHLD    COUNT
        SHLD    COUNT+2
        XRA     A
        STA     WRIDX
        CALL    FTGET           ; A = 0 ok / nonzero failed
        STA     RESULT
        CALL    CLOSEF
        LDA     RESULT
        ORA     A
        JNZ     DOFAIL

; --- print Done (<count> bytes) ------------------------------------------
        LXI     D,MDONE1
        CALL    PSTR
        CALL    PR32
        LXI     D,MDONE2
        CALL    PSTR
        JMP     EXIT0

DOFAIL: LXI     D,MFAIL         ; transfer error: report and remove file
        CALL    PSTR
        CALL    UNLINK
        JMP     EXIT0

MKERR:  LXI     D,MERR1         ; could not create the local file
        CALL    PSTR
        LHLD    SAVEFN
        CALL    PUTSTR
        LXI     D,MERR2
        CALL    PSTR
        JMP     EXIT0

HELP:   LXI     D,MHELP
        CALL    PSTR
        JMP     EXIT0

INVAL:  LXI     D,MINVAL
        CALL    PSTR

EXIT0:  LXI     D,0080H          ; restore the default DMA buffer so the CCP
        MVI     C,FSDMA          ; reads the next SUBMIT ($$$.SUB) record into
        CALL    BDOS             ; 0080H instead of our WRBUF
        LHLD    OLDSP            ; restore the CCP stack and return
        SPHL
        RET

; ============================================================================
; FTGET - run the port 60/61 protocol, writing bytes via FPUTC.
;         Returns A = 0 on success, A = 1 on failure.
; ============================================================================
FTGET:  MVI     A,CSETFN        ; OUT 60,1  start filename transfer
        OUT     CPRT
        LHLD    FNAMEP          ; send each filename character on port 61
FGSFLP: MOV     A,M
        ORA     A
        JZ      FGSFEN
        OUT     DPRT
        INX     H
        JMP     FGSFLP
FGSFEN: XRA     A
        OUT     DPRT            ; NUL terminator

FGWB1:  IN      CPRT            ; wait while the server is BUSY
        CPI     STBUSY
        JZ      FGWB1
        CPI     STERR
        JZ      FGERR           ; never started -> fail, no close

FGCHKL: MVI     A,CGETCH        ; OUT 60,3  request the next chunk
        OUT     CPRT
FGWD1:  IN      CPRT            ; wait for DATA / EOF / ERR
        CPI     STDATA
        JZ      FGHVDT
        CPI     STEOF
        JZ      FGEOFL
        CPI     STERR
        JZ      FGERRC
        JMP     FGWD1
FGHVDT: IN      DPRT            ; chunk length (0 means 256)
        ORA     A
        JNZ     FGNZLN
        LXI     D,256
        JMP     FGGOTL
FGNZLN: MOV     E,A
        MVI     D,0
FGGOTL: MOV     A,D             ; read DE bytes from the data port
        ORA     E
        JZ      FGRDEN
        IN      DPRT
        CALL    FPUTC
        DCX     D
        JMP     FGGOTL
FGRDEN: IN      CPRT            ; status after the chunk
        CPI     STEOF
        JZ      FGEOFL
        CPI     STERR
        JZ      FGERRC
        JMP     FGCHKL

FGEOFL: MVI     A,CCLOSE        ; normal end: close transfer, success
        OUT     CPRT
        XRA     A
        RET
FGERRC: MVI     A,CCLOSE        ; mid-transfer error: close, then fail
        OUT     CPRT
FGERR:  MVI     A,1
        RET

; ============================================================================
; FPUTC - consume one received byte in A. COUNT always counts received bytes.
;         In binary mode the byte is stored verbatim. In text mode incoming
;         CR (0DH) is dropped and LF (0AH) is expanded to CR LF, so the saved
;         file always has canonical CP/M line endings regardless of whether
;         the source used LF-only or CRLF. Preserves DE/HL/BC for the caller.
; ============================================================================
FPUTC:  PUSH    H
        PUSH    D
        PUSH    B
        MOV     B,A             ; B = received byte (survives INCCNT)
        CALL    INCCNT          ; COUNT = COUNT + 1 (received bytes)
        LDA     BINMOD
        ORA     A
        JZ      FPTEXT
        MOV     A,B             ; binary: store the byte unchanged
        CALL    PUTB
        JMP     FPDONE
FPTEXT: MOV     A,B
        CPI     0DH             ; strip incoming CR
        JZ      FPDONE
        CPI     0AH             ; LF -> CR LF
        JZ      FPLF
        CALL    PUTB            ; ordinary text byte
        JMP     FPDONE
FPLF:   MVI     A,0DH
        CALL    PUTB
        MVI     A,0AH
        CALL    PUTB
FPDONE: POP     B
        POP     D
        POP     H
        RET

; ============================================================================
; PUTB - store the byte in A into WRBUF and flush a full 128-byte record.
;        Preserves DE/HL/BC. Does NOT touch COUNT.
; ============================================================================
PUTB:   PUSH    H
        PUSH    D
        PUSH    B
        MOV     B,A             ; B = byte to store
        LXI     H,WRBUF
        LDA     WRIDX
        MOV     E,A
        MVI     D,0
        DAD     D               ; HL -> WRBUF + WRIDX
        MOV     M,B
        LDA     WRIDX
        INR     A
        STA     WRIDX
        CPI     SECSIZ
        JNZ     PBEND
        CALL    FLUSH
PBEND:  POP     B
        POP     D
        POP     H
        RET

; ============================================================================
; INCCNT - increment the 32-bit little-endian counter at COUNT (no wrap to
;          0 until 2^32). Clobbers A and HL only.
; ============================================================================
INCCNT: LXI     H,COUNT
        INR     M
        RNZ
        INX     H
        INR     M
        RNZ
        INX     H
        INR     M
        RNZ
        INX     H
        INR     M
        RET

; ============================================================================
; FLUSH - write the 128-byte WRBUF as one sequential record; reset WRIDX.
; ============================================================================
FLUSH:  PUSH    H
        PUSH    D
        PUSH    B
        LXI     D,WRBUF
        MVI     C,FSDMA
        CALL    BDOS
        LXI     D,FCB
        MVI     C,FWSEQ
        CALL    BDOS
        XRA     A
        STA     WRIDX
        POP     B
        POP     D
        POP     H
        RET

; ============================================================================
; CLOSEF - in text mode write a single ^Z EOF marker, then pad the final
;          partial record with NUL (00H) and close the file.
; ============================================================================
CLOSEF: LDA     BINMOD
        ORA     A
        JNZ     CLFCHK          ; binary: no ^Z marker
        MVI     A,CPMEOF        ; text: write one ^Z end-of-text marker
        CALL    PUTB
CLFCHK: LDA     WRIDX
        ORA     A
        JZ      CLFCLO          ; nothing buffered (record boundary)
        LXI     H,WRBUF
        LDA     WRIDX
        MOV     E,A
        MVI     D,0
        DAD     D               ; HL -> WRBUF + WRIDX
        LDA     WRIDX
        MOV     B,A
        MVI     A,SECSIZ
        SUB     B               ; B = 128 - WRIDX pad bytes
        MOV     B,A
CLFPAD: MVI     M,FPAD
        INX     H
        DCR     B
        JNZ     CLFPAD
        CALL    FLUSH
CLFCLO: LXI     D,FCB
        MVI     C,FCLOSE
        CALL    BDOS
        RET

; ============================================================================
; UNLINK - delete the file described by FCB (used for truncate and cleanup).
; ============================================================================
UNLINK: PUSH    H
        PUSH    D
        PUSH    B
        LXI     D,FCB
        MVI     C,FDEL
        CALL    BDOS
        POP     B
        POP     D
        POP     H
        RET

; ============================================================================
; MKFCBR - build FCB (drive 0, name.ext) from the NUL-terminated name at HL.
; ============================================================================
MKFCBR: XRA     A
        STA     FCB             ; default drive
        PUSH    H
        LXI     H,FCB+1         ; blank-fill the 8+3 name field
        MVI     B,11
MFSPC:  MVI     M,' '
        INX     H
        DCR     B
        JNZ     MFSPC
        POP     H
        LXI     D,FCB+1         ; copy up to 8 name chars
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
MFSKN:  MOV     A,M             ; name overflow: skip to '.' or end
        ORA     A
        JZ      MFDONE
        CPI     '.'
        JZ      MFDOT
        INX     H
        JMP     MFSKN
MFDOT:  INX     H               ; step over the dot
        LXI     D,FCB+9         ; copy up to 3 extension chars
        MVI     B,3
MFEXLP: MOV     A,M
        ORA     A
        JZ      MFDONE
        STAX    D
        INX     H
        INX     D
        DCR     B
        JNZ     MFEXLP
MFDONE: XRA     A               ; clear ex,s1,s2,rc and the current record
        STA     FCB+12
        STA     FCB+13
        STA     FCB+14
        STA     FCB+15
        STA     FCB+32
        RET

; ============================================================================
; PUTSTR - output the NUL-terminated C string at HL via BDOS console out.
; ============================================================================
PUTSTR: MOV     A,M
        ORA     A
        RZ
        CALL    COUT
        INX     H
        JMP     PUTSTR

; ============================================================================
; PSTR - print the '$'-terminated string at DE via BDOS func 9.
; ============================================================================
PSTR:   PUSH    H
        PUSH    D
        PUSH    B
        MVI     C,FPRSTR
        CALL    BDOS
        POP     B
        POP     D
        POP     H
        RET

; ============================================================================
; COUT - output the character in A via BDOS func 2; preserves all registers.
; ============================================================================
COUT:   PUSH    H
        PUSH    D
        PUSH    B
        MOV     E,A
        MVI     C,FCONO
        CALL    BDOS
        POP     B
        POP     D
        POP     H
        RET

; ============================================================================
; PR32 - print the unsigned 32-bit COUNT as decimal (no leading zeros).
;        Copies COUNT into NUMWRK, then repeatedly divides by 10, storing
;        digits backwards into N32BUF.
; ============================================================================
PR32:   LXI     H,COUNT         ; NUMWRK = COUNT (4 bytes)
        LXI     D,NUMWRK
        MVI     C,4
PR32C:  MOV     A,M
        STAX    D
        INX     H
        INX     D
        DCR     C
        JNZ     PR32C
        LXI     H,N32END        ; build digits backwards from the buffer end
        SHLD    DPTR
PR32D:  CALL    DIV32           ; NUMWRK /= 10, A = remainder digit
        ADI     '0'
        LHLD    DPTR
        DCX     H
        MOV     M,A
        SHLD    DPTR
        CALL    ISZ32           ; Z set when NUMWRK has reached 0
        JNZ     PR32D
        LHLD    DPTR            ; print digits DPTR .. N32END-1
PR32P:  MOV     A,L
        LXI     D,N32END
        CMP     E
        JNZ     PR32E
        MOV     A,H
        CMP     D
        JZ      PR32X
PR32E:  MOV     A,M
        CALL    COUT
        INX     H
        JMP     PR32P
PR32X:  RET

; ============================================================================
; DIV32 - divide the 32-bit value at NUMWRK (little-endian) by 10 in place,
;         returning the remainder (0-9) in A. Reuses the 16-bit DIV10 by
;         processing one byte at a time from the most significant end.
; ============================================================================
DIV32:  MVI     B,0             ; B = carry/remainder feeding the next byte
        MVI     C,4             ; four bytes
        LXI     D,NUMWRK+3      ; DE -> most significant byte
DV32L:  LDAX    D               ; HL = remainder*256 + current byte
        MOV     L,A
        MOV     H,B
        CALL    DIV10           ; HL = quotient (0-255), A = remainder
        MOV     B,A             ; carry into the next lower byte
        MOV     A,L             ; quotient byte (H is 0 here)
        STAX    D
        DCX     D
        DCR     C
        JNZ     DV32L
        MOV     A,B             ; final remainder digit
        RET

; ============================================================================
; ISZ32 - return with the Z flag set when the 32-bit NUMWRK is zero.
; ============================================================================
ISZ32:  LDA     NUMWRK
        ORA     A
        RNZ
        LDA     NUMWRK+1
        ORA     A
        RNZ
        LDA     NUMWRK+2
        ORA     A
        RNZ
        LDA     NUMWRK+3
        ORA     A
        RET

; ============================================================================
; DIV10 - unsigned HL / 10 -> HL quotient, A remainder (0-9).
; ============================================================================
DIV10:  PUSH    B
        PUSH    D
        LXI     D,0             ; DE = running remainder
        MVI     B,16
DIV10L: DAD     H               ; HL <<= 1, CY = old bit 15
        MOV     A,E
        RAL
        MOV     E,A
        MOV     A,D
        RAL
        MOV     D,A             ; DE = (DE << 1) | CY
        MOV     A,E
        SUI     10
        MOV     C,A
        MOV     A,D
        SBI     0
        JC      DIV10N
        MOV     D,A
        MOV     E,C
        INR     L               ; quotient bit 0 = 1
DIV10N: DCR     B
        JNZ     DIV10L
        MOV     A,E             ; remainder (0-9)
        POP     D
        POP     B
        RET

; ============================================================================
; Strings (apostrophe = 39, CR = 13, LF = 10 as raw byte values)
; ============================================================================
MHELP:  DB      'FT (File Transfer) - Remote File Transfer v1.00',13,10
        DB      'Transfer files from Remote FT Server',13,10,13,10
        DB      'Usage: ft [-g <filename>] [-gb <filename>]',13,10
        DB      13,10,'Options:',13,10
        DB      '  -g  <filename>  Get a text file (normalize line endings)',13,10
        DB      '  -gb <filename>  Get a binary file (verbatim bytes)',13,10
        DB      13,10,'Examples:',13,10
        DB      '  ft -g test.txt       Download text file from server',13,10
        DB      '  ft -gb prog.com      Download binary file from server',13,10,'$'

MDOWN1: DB      'Downloading ',39,'$'
MDOWN2: DB      39,' from Remote FT Server...',13,10,'$'
MSAVE1: DB      'Saving as ',39,'$'
MSAVE2: DB      39,13,10,'$'
MERR1:  DB      'Error: Failed to create output file ',39,'$'
MERR2:  DB      39,13,10,'$'
MDONE1: DB      'Done (','$'
MDONE2: DB      ' bytes)',13,10,'$'
MFAIL:  DB      'Failed - file not found or server error',13,10,'$'
MINVAL: DB      'Invalid arguments. Use ',39,'ft',39,' for help.',13,10,'$'

; ============================================================================
; Scratch RAM
; ============================================================================
OLDSP:  DS      2               ; saved CCP stack pointer
TCNT:   DS      1               ; command-tail token count
TOK0:   DS      2               ; pointer to first token
TOK1:   DS      2               ; pointer to second token
FNAMEP: DS      2               ; full path sent to the server
SAVEFN: DS      2               ; local basename pointer
COUNT:  DS      4               ; bytes received (32-bit, no wrap)
WRIDX:  DS      1               ; bytes buffered in WRBUF (0..128)
BINMOD: DS      1               ; 0 = text (normalize CRLF, ^Z), 1 = binary
RESULT: DS      1               ; FTGET result (0 ok / nonzero fail)
NUMWRK: DS      4               ; 32-bit scratch for decimal conversion
N32BUF: DS      10              ; up to 10 decimal digits
N32END  EQU     $               ; one past N32BUF
DPTR:   DS      2               ; current digit pointer for PR32
FCB:    DS      36              ; file control block
TBUF:   DS      130             ; command-tail copy + NUL
WRBUF:  DS      128             ; one sequential write record
        DS      64              ; stack space
STACK   EQU     $               ; top of our stack
        END
