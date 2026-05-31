100 REM ====================================================
110 REM  WEBSOCKET THROUGHPUT / TERMINAL PERF TEST
120 REM  Streams N lines and times the run with port 41
130 REM  (seconds-since-boot) read via the port-200 buffer.
140 REM ====================================================
150 WIDTH 255
152 ES$ = CHR$(27)
154 DIM CL$(5)
156 FOR J = 0 TO 5 : CL$(J) = ES$ + "[1;3" + CHR$(49 + J) + "m" : NEXT J
158 RST$ = ES$ + "[0m"
160 N = 10000
170 PRINT "WEBSOCKET THROUGHPUT TEST - ";N;" LOOPS"
180 REM --- capture start time ---
190 PORT = 41 : GOSUB 9000 : TS = VAL(RSTRING$)
200 REM --- the streaming workload ---
210 FOR I = 1 TO N
220 PRINT CL$(I - INT(I / 6) * 6);"The quick brown fox jumps over the lazy dog 012345678901234567890 # ";I;RST$
230 NEXT I
240 REM --- capture end time ---
250 PORT = 41 : GOSUB 9000 : TE = VAL(RSTRING$)
260 GOSUB 8000
270 END
280 REM
8000 REM ====================================================
8010 REM  REPORT: compute and print elapsed time and rate
8020 REM ====================================================
8030 D = TE - TS
8040 PRINT
8050 PRINT "LOOPS          : ";N
8060 PRINT "ELAPSED SECONDS: ";D
8070 IF D > 0 THEN PRINT "LINES/SEC      : ";N / D
8080 IF D <= 0 THEN PRINT "LINES/SEC      : (too fast - increase N)"
8090 RETURN
8100 REM
9000 REM ====================================================
9010 REM  READ STRING FROM PORT INTO RSTRING$ (until NULL)
9020 REM ====================================================
9030 RSTRING$ = ""
9040 OUT PORT, 0
9050 C = INP(200)
9060 WHILE C <> 0
9070 RSTRING$ = RSTRING$ + CHR$(C)
9080 C = INP(200)
9090 WEND
9100 RETURN
