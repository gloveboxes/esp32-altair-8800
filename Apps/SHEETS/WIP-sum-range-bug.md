# WIP — SHEETS.ASM `sum()` range upper-row bug (PARKED)

Status: **parked**. `SHEETS.SUB` has been reverted to build the **C** version
(`SHEETS.C` + SDK `string`/`long`), so the shipped `SHEETS.COM` is the known-good
C build. The MAC/8080 port in `SHEETS.ASM` still has the open bug below.

## Symptom (reproduced live in docker browser terminal)

Scenario:
- A1=10, A2=20, A3=30, A5=60, A7=100, A8=100, A9=100
- A11 = `=sum(a1:a9)` displays **120**, should be **420**.

A fresh re-entry of `=sum(a1:a9)` (forcing re-evaluation — there is NO result
caching; `EVCELL` recomputes every redraw) still yields **120**. So this is a
genuine range-evaluation defect, not a stale-display / recalc-trigger problem.

## Isolation results (key findings)

| Formula        | Expected | Actual | Verdict |
|----------------|----------|--------|---------|
| `=a9`          | 100      | 100    | OK — direct high-row ref reads fine |
| `=sum(a7:a9)`  | 300      | 300    | OK — high rows scanned fine when start row is high |
| `=sum(a1:a9)`  | 420      | 120    | BUG — 120 == 10+20+30+60 == only a1:a5 |

So: the END ref (a9) parses & scans correctly (sum a7:a9 = 300). The bug only
appears when the START row is low and the range spans "far". The result 120 is
exactly the a1..a5 subtotal — the scan appears to STOP EARLY (around row 5),
dropping the later populated rows a7/a8/a9 even though they are individually
reachable.

NOTE: this is NOT the earlier insert-row BMPREF bug (that one is fixed & verified)
and NOT a simple off-by-one (the user confirmed: adding MORE numbers in-range did
not get added at all).

## Where to look next (in SHEETS.ASM)

- `FNRNG` (~line 1904): the range accumulate loop. Outer loop `RGIL` over rows
  RGI = RGR1..RGR2, inner `RGJL` over cols. Stop tests use `SCMP4` (signed 16-bit
  compare). Loop globals live in memory (RGI/RGJ/RGR1/RGR2/RGCNT/RSUM at ~4236).
- `EVCELL` (~line 1424) is invoked per cell via RGEVAL; it RECURSES into EXPR for
  formula cells and is guarded by `EDEPTH` (<25). SUSPECT: EVCELL / EXPR /
  PRSREF / FNRNG share GLOBAL scratch (EPOS, PRROW/PRCOL, RGI/RGJ, ACC, EVR/EVC).
  When the loop body re-enters the evaluator, a global used as the loop bound or
  index may get clobbered, truncating the scan. The "stops ~row 5" boundary hint:
  check whether something tied to the CURRENT cell position (crow≈A11/row 10) or a
  digit-accumulator wraps/limits the RGI walk.
- Compare against `SHEETS.C` `fnrng()` which keeps loop state in LOCAL stack vars,
  so the C version is immune — consistent with C build being correct.

## Reproduction harness

run_cpm CANNOT drive this (strips control bytes). Use docker browser terminal:
1. Build ASM: MCP `build_app` app:"sheets" (writes altair_mcp_server/disks/cpm63k.dsk).
2. `cp altair_mcp_server/disks/cpm63k.dsk disks/cpm63k.dsk && docker restart altair8800v2`
3. open http://localhost:8080/, `a:` then `sheets t1`.
4. Enter values; navigate with WordStar keys (Ctrl-E up / Ctrl-X down /
   Ctrl-S left / Ctrl-D right). Status line lags ~1 frame. `type_in_page` text
   does NOT commit a cell — press the Enter KEY separately. Type a leading `=`
   in its own send (fast typing can drop the first char).

## Secondary bug noticed (also parked)

`GOTOCL` (~line 3753): Goto `a10` reports "Out of range". Its `GODL` digit loop
has the SAME HL-clobber pattern that was fixed in PRSREF/BMPREF: it computes
`row*10` into HL then does `LHLD GOP` (clobbering HL) before adding the digit ->
GOROW becomes pointer+digit, fails bounds. Fix pattern: save digit in C (or row*10
in DE via XCHG) before reloading GOP. Not the user's reported sum bug, but worth
fixing when work resumes.
