#include "stdio.h"

/*
 * ATTN - "Paper Tape is All You Need"
 *
 * A 1-layer, 1-head transformer trained to reverse an 8-digit
 * sequence.  This is a BDS C 1.6 / CP/M port of the PDP-11 2.11BSD
 * assembly program attn.s (davepl/pdpsrc), itself a port of the
 * NN11 library by Damien Boureille (dbrll/ATTN-11).
 *
 * Numerics (faithful to the original):
 *   Q8  forward activations   (int, value = real * 256)
 *   Q15 backward gradients    (int)
 *   Q16 weight accumulators   (32-bit, via the BDS C LONG package)
 *
 * The 8080 has no hardware multiply/divide/shift, so all 32-bit
 * fixed-point math goes through LONG.C (the long() builtin).  The
 * training run is SLOW on a real/emulated 8080; lower NSTEP for a
 * quick demonstration.
 *
 * Usage:
 *   attn -t   train, save weights to ATTN.WTS on the current drive, then test
 *   attn      run inference using the saved weights (default)
 *   attn -h   help
 */

#define D 16            /* d_model            */
#define S 8             /* sequence length    */
#define V 10            /* vocab (digits 0-9) */

#define NSTEP 700       /* max training steps  */
#define RPRT 50         /* report interval     */
#define FSTEP 10        /* mix in IFILE every n steps */

/* Trained weights are saved here so inference can reload them.
 * The Q16 accumulator arrays are exact multiples of 128 bytes, so
 * they map onto whole CP/M sectors for raw read()/write().          */
#define WFILE "ATTN.WTS"
#define STKE 5          /* wtke  = V*D*4 = 640  bytes = 5 sectors */
#define SPSE 4          /* wpse  = S*D*4 = 512  bytes = 4 sectors */
#define SWQ  8          /* wwq/wwk/wwv = D*D*4 = 1024 bytes = 8 sectors */
#define SWOT 5          /* wwot  = D*V*4 = 640  bytes = 5 sectors */

/* Default inference input file: one S-digit sequence per line.       */
#define IFILE "ATTN.IN"

/* WORK layout: Q | K | V | A(scores), each S*D except A is S*S */
#define QB 0
#define KB (S*D)
#define VB (2*S*D)
#define AB (3*S*D)

/* --- Q16 weight accumulators (4 bytes per weight, big-endian) --- */
char wtke[V*D*4];       /* token embed  */
char wpse[S*D*4];       /* pos embed    */
char wwq[D*D*4];        /* Wq           */
char wwk[D*D*4];        /* Wk           */
char wwv[D*D*4];        /* Wv           */
char wwot[D*V*4];       /* Wout         */

/* --- Q8 weight copies (rebuilt from the Q16 accumulators) --- */
int qtke[V*D];
int qpse[S*D];
int qwq[D*D];
int qwk[D*D];
int qwv[D*D];
int qwot[D*V];

/* --- Q15 gradient accumulators --- */
int gtke[V*D];
int gpse[S*D];
int gwq[D*D];
int gwk[D*D];
int gwv[D*D];
int gwot[D*V];

/* --- forward state --- */
int xx[S*D];            /* embeddings (attn input)   */
int yy[S*D];            /* attn output (Y = O + X)   */
int logits[S*V];        /* output logits             */
int work[3*S*D + S*S];  /* attn workspace Q|K|V|A     */

/* --- backward workspace --- */
int dl[V];              /* dLogits (one position)    */
int dy[S*D];            /* dY                        */
int da[S*S];            /* dA, reused as dSc         */
int dqq[S*D];           /* dQ                        */
int dkk[S*D];           /* dK                        */
int dvv[S*D];           /* dV                        */
int dxx[S*D];           /* dX                        */
int dtmp[D];            /* temp column vector        */

/* --- lookup tables --- */
int exptbl[256];        /* exp(-i/32) in Q8          */
int logtbl[257];        /* -ln(x/256)*4096 in Q12    */

/* --- training data / state --- */
int tokens[S];
int target[S];
int teprd[S];
unsigned rseed;
int thit;
int ttot;
int tstep;
int fhits;
int vhits;

/* ============================================================ */
/* 32-bit fixed-point helpers (LONG package)                    */
/* ============================================================ */

/* clamp a 32-bit long to a signed 16-bit int */
int lci(a)
char *a;
{
    char hi[4], lo[4];

    itol(hi, 32767);
    if (lcomp(a, hi) > 0)
        return 32767;
    itol(lo, -32768);
    if (lcomp(a, lo) < 0)
        return -32768;
    return ltoi(a);
}

/* a (Q16 long) >> 8 -> clamped Q8 int */
int lq8(a)
char *a;
{
    char q[4], d[4];

    itol(d, 256);
    ldiv(q, a, d);
    return lci(q);
}

/* out (long) = a * b  (signed 16x16 -> 32) */
prod32(out, a, b)
char *out;
int a, b;
{
    char la[4], lb[4];

    itol(la, a);
    itol(lb, b);
    lmul(out, la, lb);
}

/* acc += a * b */
lmac(acc, a, b)
char *acc;
int a, b;
{
    char p[4];

    prod32(p, a, b);
    ladd(acc, acc, p);
}

/* (a * b) >> 8 -> clamped Q8 int */
int mq8(a, b)
int a, b;
{
    char p[4];

    prod32(p, a, b);
    return lq8(p);
}

/* Q8 divide: (a << 8) / b -> clamped Q8 int */
int fxdiv(a, b)
int a, b;
{
    char la[4], c[4], num[4], lb[4], q[4];

    itol(la, a);
    itol(c, 256);
    lmul(num, la, c);
    itol(lb, b);
    ldiv(q, num, lb);
    return lci(q);
}

/* arithmetic shift right by n (floor toward -inf), n small */
int asr(v, n)
int v, n;
{
    int d, q;

    d = 1;
    while (n-- > 0)
        d = d << 1;
    q = v / d;
    if (v < 0 && (v % d) != 0)
        q = q - 1;
    return q;
}

/* *dst += v, saturating to signed 16-bit */
addcl(dst, v)
int *dst;
int v;
{
    char a[4], b[4], s[4];

    itol(a, *dst);
    itol(b, v);
    ladd(s, a, b);
    *dst = lci(s);
}

/* a - b, saturating to signed 16-bit */
int subcl(a, b)
int a, b;
{
    char la[4], lb[4], s[4];

    itol(la, a);
    itol(lb, b);
    lsub(s, la, lb);
    return lci(s);
}

/* ============================================================ */
/* Vector primitives                                            */
/* ============================================================ */

int vmax(vec, n, pidx)
int *vec;
int n;
int *pidx;
{
    int mx, mi, i;

    mx = vec[0];
    mi = 0;
    for (i = 1; i < n; i++)
        if (vec[i] > mx) {
            mx = vec[i];
            mi = i;
        }
    *pidx = mi;
    return mx;
}

int vdot(x, y, n)
int *x, *y;
int n;
{
    char acc[4];
    int i;

    itol(acc, 0);
    for (i = 0; i < n; i++)
        lmac(acc, x[i], y[i]);
    return lq8(acc);
}

vcpy(src, dst, n)
int *src, *dst;
int n;
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

vclr(p, n)
int *p;
int n;
{
    int i;

    for (i = 0; i < n; i++)
        p[i] = 0;
}

/* dst[k] += (scalar * src[k]) >> 8, saturating */
vsadd(sc, src, dst, n)
int sc;
int *src, *dst;
int n;
{
    int k;

    for (k = 0; k < n; k++)
        addcl(&dst[k], mq8(sc, src[k]));
}

/* softmax in place (Q8), LUT-based */
sftmx(vec, n)
int *vec;
int n;
{
    int i, mx, d, idx, sum, dummy;

    mx = vmax(vec, n, &dummy);
    sum = 0;
    for (i = 0; i < n; i++) {
        d = mx - vec[i];
        if (d < 0)
            d = 0;
        idx = d >> 3;
        if (idx > 255)
            idx = 255;
        vec[i] = exptbl[idx];
        sum = sum + vec[i];
    }
    for (i = 0; i < n; i++)
        vec[i] = fxdiv(vec[i], sum);
}

/* ============================================================ */
/* Matrix primitives                                            */
/* ============================================================ */

/* vout[i] = sum_j mat[i][j] * vin[j]  (Q16 accum, >>8, clamp) */
mvmul(mat, vin, vout, rows, cols)
int *mat, *vin, *vout;
int rows, cols;
{
    char acc[4];
    int i, j, mi;

    mi = 0;
    for (i = 0; i < rows; i++) {
        itol(acc, 0);
        for (j = 0; j < cols; j++)
            lmac(acc, mat[mi + j], vin[j]);
        vout[i] = lq8(acc);
        mi = mi + cols;
    }
}

/* vout[i] += sum_j mat[i][j] * vin[j]  (saturating add) */
mvadd(mat, vin, vout, rows, cols)
int *mat, *vin, *vout;
int rows, cols;
{
    char acc[4];
    int i, j, mi;

    mi = 0;
    for (i = 0; i < rows; i++) {
        itol(acc, 0);
        for (j = 0; j < cols; j++)
            lmac(acc, mat[mi + j], vin[j]);
        addcl(&vout[i], lq8(acc));
        mi = mi + cols;
    }
}

/* vout[j] = sum_i (mat[i][j] * vin[i]) >> 8   (per-product Q8) */
vtmul(mat, vin, vout, rows, cols)
int *mat, *vin, *vout;
int rows, cols;
{
    int i, j, sc, mi;

    for (j = 0; j < cols; j++)
        vout[j] = 0;
    mi = 0;
    for (i = 0; i < rows; i++) {
        sc = vin[i];
        for (j = 0; j < cols; j++)
            addcl(&vout[j], mq8(mat[mi + j], sc));
        mi = mi + cols;
    }
}

/* mat[i][j] += (vx[i] * vy[j]) >> 8   (saturating) */
outer(mat, vx, vy, rows, cols)
int *mat, *vx, *vy;
int rows, cols;
{
    int i, j, sc, mi;

    mi = 0;
    for (i = 0; i < rows; i++) {
        sc = vx[i];
        for (j = 0; j < cols; j++)
            addcl(&mat[mi + j], mq8(sc, vy[j]));
        mi = mi + cols;
    }
}

/* ============================================================ */
/* Layer operations                                             */
/* ============================================================ */

/* X[i] = tok_emb[tokens[i]] + pos_emb[i] */
embed()
{
    int i, j, tok, oi;

    oi = 0;
    for (i = 0; i < S; i++) {
        tok = tokens[i];
        for (j = 0; j < D; j++)
            xx[oi + j] = qtke[tok * D + j] + qpse[oi + j];
        oi = oi + D;
    }
}

/* self-attention forward pass */
attn()
{
    int i, j;

    /* Step 1-3: Q = X.Wq, K = X.Wk, V = X.Wv */
    for (i = 0; i < S; i++) {
        vtmul(qwq, &xx[i * D], &work[QB + i * D], D, D);
        vtmul(qwk, &xx[i * D], &work[KB + i * D], D, D);
        vtmul(qwv, &xx[i * D], &work[VB + i * D], D, D);
    }
    /* Step 4: S[i][j] = (Q[i] . K[j]) / sqrt(d), sqrt(16)=4 -> >>2 */
    for (i = 0; i < S; i++)
        for (j = 0; j < S; j++)
            work[AB + i * S + j] =
                asr(vdot(&work[QB + i * D], &work[KB + j * D], D), 2);
    /* Step 5: softmax per row */
    for (i = 0; i < S; i++)
        sftmx(&work[AB + i * S], S);
    /* Step 6: Y[i] = V^T . A[i] */
    for (i = 0; i < S; i++)
        vtmul(&work[VB], &work[AB + i * S], &yy[i * D], S, D);
    /* Step 7: residual Y += X */
    for (i = 0; i < S * D; i++)
        yy[i] = yy[i] + xx[i];
}

/* logits[i] = Wout^T . Y[i] */
proj()
{
    int i;

    for (i = 0; i < S; i++)
        vtmul(qwot, &yy[i * D], &logits[i * V], D, V);
}

forwrd()
{
    embed();
    attn();
    proj();
}

/* ============================================================ */
/* Weight conversion / init / update                            */
/* ============================================================ */

/* convert one Q16 weight group to its Q8 copy */
cv1(w, q, n)
char *w;
int *q;
int n;
{
    int i;

    for (i = 0; i < n; i++)
        q[i] = lq8(w + (i << 2));
}

cvt16()
{
    cv1(wtke, qtke, V * D);
    cv1(wpse, qpse, S * D);
    cv1(wwq, qwq, D * D);
    cv1(wwk, qwk, D * D);
    cv1(wwv, qwv, D * D);
    cv1(wwot, qwot, D * V);
}

/* 15-bit LCG */
int rndnum()
{
    rseed = (rseed * 25173 + 13849) & 0x7FFF;
    return rseed;
}

/* fill n Q16 weights with random Q8 in [-128,127] */
in_fil(w, n)
char *w;
int n;
{
    int i, r;
    char t[4], c[4];

    for (i = 0; i < n; i++) {
        r = (rndnum() & 0x00FF) - 128;
        itol(t, r);
        itol(c, 256);
        lmul(w + (i << 2), t, c);
    }
}

initw()
{
    in_fil(wtke, V * D);
    in_fil(wpse, S * D);
    in_fil(wwq, D * D);
    in_fil(wwk, D * D);
    in_fil(wwv, D * D);
    in_fil(wwot, D * V);
}

/* w_q16 -= grad_q15 >> (shift-1); zero grad after read */
up_do(w, g, n, shift)
char *w;
int *g;
int n, shift;
{
    int i, delta;
    char d[4], nw[4];

    for (i = 0; i < n; i++) {
        delta = asr(g[i], shift - 1);
        g[i] = 0;
        itol(d, delta);
        lsub(nw, w + (i << 2), d);
        lassign(w + (i << 2), nw);
    }
}

updat()
{
    up_do(wtke, gtke, V * D, 4);
    up_do(wpse, gpse, S * D, 4);
    up_do(wwq, gwq, D * D, 1);
    up_do(wwk, gwk, D * D, 1);
    up_do(wwv, gwv, D * D, 1);
    up_do(wwot, gwot, D * V, 6);
}

zerog()
{
    vclr(gtke, V * D);
    vclr(gpse, S * D);
    vclr(gwq, D * D);
    vclr(gwk, D * D);
    vclr(gwv, D * D);
    vclr(gwot, D * V);
}

/* ============================================================ */
/* Backward pass                                                */
/* ============================================================ */

bkwrd()
{
    int i, j, k, o, tok, dad, t;

    /* Step 1: dLogits, dWout, dY */
    vclr(dy, S * D);
    for (i = 0; i < S; i++) {
        for (k = 0; k < V; k++)
            dl[k] = logits[i * V + k];
        sftmx(dl, V);
        dl[target[i]] = dl[target[i]] - 256;
        for (k = 0; k < V; k++)
            dl[k] = dl[k] << 7;
        outer(gwot, &yy[i * D], dl, D, V);
        mvmul(qwot, dl, &dy[i * D], D, V);
    }

    /* Step 2: dA, dV */
    vclr(dvv, S * D);
    for (i = 0; i < S; i++)
        for (j = 0; j < S; j++) {
            da[i * S + j] = vdot(&work[VB + j * D], &dy[i * D], D);
            vsadd(work[AB + i * S + j], &dy[i * D], &dvv[j * D], D);
        }

    /* Step 3: backward softmax -> dSc (in da) */
    for (i = 0; i < S; i++) {
        dad = vdot(&work[AB + i * S], &da[i * S], S);
        for (j = 0; j < S; j++) {
            t = subcl(da[i * S + j], dad);
            t = mq8(work[AB + i * S + j], t);
            da[i * S + j] = asr(t, 2);
        }
    }

    /* Step 4: dQ, dK */
    for (i = 0; i < S; i++)
        vtmul(&work[KB], &da[i * S], &dqq[i * D], S, D);
    for (j = 0; j < S; j++) {
        for (i = 0; i < S; i++)
            dtmp[i] = da[i * S + j];
        vtmul(&work[QB], dtmp, &dkk[j * D], S, D);
    }

    /* Step 5: backward projections + dX */
    vcpy(dy, dxx, S * D);
    for (i = 0; i < S; i++) {
        o = i * D;
        mvadd(qwq, &dqq[o], &dxx[o], D, D);
        outer(gwq, &xx[o], &dqq[o], D, D);
        mvadd(qwk, &dkk[o], &dxx[o], D, D);
        outer(gwk, &xx[o], &dkk[o], D, D);
        mvadd(qwv, &dvv[o], &dxx[o], D, D);
        outer(gwv, &xx[o], &dvv[o], D, D);
    }

    /* Step 6: backward embedding */
    for (i = 0; i < S; i++) {
        o = i * D;
        tok = tokens[i];
        for (k = 0; k < D; k++) {
            addcl(&gtke[tok * D + k], dxx[o + k]);
            addcl(&gpse[i * D + k], dxx[o + k]);
        }
    }
}

/* ============================================================ */
/* Training driver                                              */
/* ============================================================ */

/* set reversal target for the current tokens */
mktarg()
{
    int i;

    for (i = 0; i < S; i++)
        target[i] = tokens[S - 1 - i];
}

/* generate a random reversal sample */
gensm()
{
    int i;

    for (i = 0; i < S; i++)
        tokens[i] = rndnum() % 10;
    mktarg();
}

/* train one current tokens/target sample */
trseq()
{
    cvt16();
    forwrd();
    bkwrd();
    updat();
    count();
}

/* quiet accuracy check for one current tokens/target sample */
int ckseq()
{
    int i, idx, ok;

    forwrd();
    ok = 1;
    for (i = 0; i < S; i++) {
        vmax(&logits[i * V], V, &idx);
        if (idx != target[i])
            ok = 0;
    }
    return ok;
}

/* read fixed samples from fname. trn=1 trains; trn=0 validates quietly. */
int filrun(fname, trn)
char *fname;
int trn;
{
    int ch, i, n;
    FILE *fp;

    fp = fopen(fname, "r");
    if (fp == NULL)
        return ERROR;

    n = 0;
    i = 0;
    if (!trn) {
        vhits = 0;
        cvt16();
    }
    while ((ch = fgetc(fp)) != EOF && ch != 26) {
        if (ch >= '0' && ch <= '9') {
            if (i < S)
                tokens[i] = ch - '0';
            i = i + 1;
        } else if (ch == '\n') {
            if (i >= S) {
                mktarg();
                if (trn)
                    trseq();
                else
                    vhits = vhits + ckseq();
                n = n + 1;
            }
            i = 0;
        }
    }
    if (i >= S) {
        mktarg();
        if (trn)
            trseq();
        else
            vhits = vhits + ckseq();
        n = n + 1;
    }
    fclose(fp);
    return n;
}

/* count correct argmax predictions */
count()
{
    int i, idx;

    for (i = 0; i < S; i++) {
        vmax(&logits[i * V], V, &idx);
        if (idx == target[i])
            thit = thit + 1;
        ttot = ttot + 1;
    }
}

/* average cross-entropy loss (Q12) for the current sample */
int closs()
{
    char acc[4], inc[4], d8[4], q[4];
    int i, k, p, t;

    itol(acc, 0);
    for (i = 0; i < S; i++) {
        for (k = 0; k < V; k++)
            dl[k] = logits[i * V + k];
        sftmx(dl, V);
        t = target[i];
        p = dl[t];
        if (p < 256) {
            itol(inc, logtbl[p]);
            ladd(acc, acc, inc);
        }
    }
    itol(d8, 8);
    ldiv(q, acc, d8);
    return ltoi(q);
}

/* fractional part (0-9999) of a Q12 value */
int lossfr(loss)
int loss;
{
    char a[4], b[4], p[4], d[4], q[4];
    int fr;

    fr = loss & 0x0FFF;
    itol(a, fr);
    itol(b, 10000);
    lmul(p, a, b);
    itol(d, 4096);
    ldiv(q, p, d);
    return ltoi(q);
}

/* print step / loss / accuracy, then reset counters */
report()
{
    char a[4], b[4], p[4], c[4], q[4];
    int loss, pm;

    loss = closs();
    printf("\n step %4d loss=%d.%04d", tstep, loss >> 12, lossfr(loss));

    itol(a, thit);
    itol(b, 1000);
    lmul(p, a, b);
    itol(c, ttot);
    ldiv(q, p, c);
    pm = ltoi(q);
    if (pm >= 1000)
        printf(" acc=1.000\n");
    else
        printf(" acc=0.%03d\n", pm);

    thit = 0;
    ttot = 0;
}

/* final test: 10 samples */
test()
{
    int n, i, idx, allok, ok;

    ok = 0;
    for (n = 0; n < 10; n++) {
        gensm();
        cvt16();
        forwrd();
        for (i = 0; i < S; i++) {
            vmax(&logits[i * V], V, &idx);
            teprd[i] = idx;
        }
        printf(" ");
        for (i = 0; i < S; i++)
            printf("%d ", tokens[i]);
        printf("-> ");
        for (i = 0; i < S; i++)
            printf("%d ", teprd[i]);
        allok = 1;
        for (i = 0; i < S; i++)
            if (teprd[i] != target[i])
                allok = 0;
        if (allok) {
            ok = ok + 1;
            printf(" ok\n");
        } else
            printf(" fail\n");
    }
    printf("\naccuracy  %2d/%d\n", ok, 10);
}

/* run inference on the current tokens[] and print "in -> out".
 * The task is to reverse the sequence, so the expected output is the
 * input read backwards; score the prediction against it.
 * Assumes cvt16() has already built the Q8 weight copies.
 * Returns 1 if every position is correct, else 0.                    */
int infseq()
{
    int i, idx, ok;

    forwrd();
    printf(" ");
    for (i = 0; i < S; i++)
        printf("%d ", tokens[i]);
    printf("-> ");
    ok = 1;
    for (i = 0; i < S; i++) {
        vmax(&logits[i * V], V, &idx);
        printf("%d ", idx);
        if (idx != tokens[S - 1 - i])
            ok = 0;
    }
    if (ok)
        printf(" ok\n");
    else
        printf(" fail\n");
    return ok;
}

/* read S-digit sequences from a text file and run inference on each.
 * Digits are 0-9; any non-digit (space, newline) separates sequences.
 * A line must supply at least S digits; the first S are used.
 * Returns the count processed, or ERROR if the file cannot be opened. */
int runfil(fname)
char *fname;
{
    int ch, i, n;
    FILE *fp;

    fp = fopen(fname, "r");
    if (fp == NULL)
        return ERROR;

    n = 0;
    i = 0;
    fhits = 0;
    while ((ch = fgetc(fp)) != EOF && ch != 26) {
        if (ch >= '0' && ch <= '9') {
            if (i < S)
                tokens[i] = ch - '0';
            i = i + 1;
        } else if (ch == '\n') {
            if (i >= S) {
                fhits = fhits + infseq();
                n = n + 1;
            } else if (i > 0)
                printf(" (skipped line: %d digits, need %d)\n", i, S);
            i = 0;
        }
        /* other chars are in-line separators and are ignored */
    }
    /* handle a final line with no trailing newline */
    if (i >= S) {
        fhits = fhits + infseq();
        n = n + 1;
    }
    fclose(fp);
    return n;
}

/* ============================================================ */
/* Lookup-table initialisers (no aggregate init in BDS C)       */
/* ============================================================ */

initex()
{
    exptbl[0]=256; exptbl[1]=248; exptbl[2]=240; exptbl[3]=233; exptbl[4]=226; exptbl[5]=219; exptbl[6]=212; exptbl[7]=206; exptbl[8]=199; exptbl[9]=193;
    exptbl[10]=187; exptbl[11]=182; exptbl[12]=176; exptbl[13]=171; exptbl[14]=165; exptbl[15]=160; exptbl[16]=155; exptbl[17]=150; exptbl[18]=146; exptbl[19]=141;
    exptbl[20]=137; exptbl[21]=133; exptbl[22]=129; exptbl[23]=125; exptbl[24]=121; exptbl[25]=117; exptbl[26]=114; exptbl[27]=110; exptbl[28]=107; exptbl[29]=103;
    exptbl[30]=100; exptbl[31]=97; exptbl[32]=94; exptbl[33]=91; exptbl[34]=88; exptbl[35]=86; exptbl[36]=83; exptbl[37]=81; exptbl[38]=78; exptbl[39]=76;
    exptbl[40]=73; exptbl[41]=71; exptbl[42]=69; exptbl[43]=67; exptbl[44]=65; exptbl[45]=63; exptbl[46]=61; exptbl[47]=59; exptbl[48]=57; exptbl[49]=55;
    exptbl[50]=54; exptbl[51]=52; exptbl[52]=50; exptbl[53]=49; exptbl[54]=47; exptbl[55]=46; exptbl[56]=44; exptbl[57]=43; exptbl[58]=42; exptbl[59]=41;
    exptbl[60]=39; exptbl[61]=38; exptbl[62]=37; exptbl[63]=36; exptbl[64]=35; exptbl[65]=34; exptbl[66]=33; exptbl[67]=32; exptbl[68]=31; exptbl[69]=30;
    exptbl[70]=29; exptbl[71]=28; exptbl[72]=27; exptbl[73]=26; exptbl[74]=25; exptbl[75]=25; exptbl[76]=24; exptbl[77]=23; exptbl[78]=22; exptbl[79]=22;
    exptbl[80]=21; exptbl[81]=20; exptbl[82]=20; exptbl[83]=19; exptbl[84]=19; exptbl[85]=18; exptbl[86]=17; exptbl[87]=17; exptbl[88]=16; exptbl[89]=16;
    exptbl[90]=15; exptbl[91]=15; exptbl[92]=14; exptbl[93]=14; exptbl[94]=14; exptbl[95]=13; exptbl[96]=13; exptbl[97]=12; exptbl[98]=12; exptbl[99]=12;
    exptbl[100]=11; exptbl[101]=11; exptbl[102]=11; exptbl[103]=10; exptbl[104]=10; exptbl[105]=10; exptbl[106]=9; exptbl[107]=9; exptbl[108]=9; exptbl[109]=8;
    exptbl[110]=8; exptbl[111]=8; exptbl[112]=8; exptbl[113]=7; exptbl[114]=7; exptbl[115]=7; exptbl[116]=7; exptbl[117]=7; exptbl[118]=6; exptbl[119]=6;
    exptbl[120]=6; exptbl[121]=6; exptbl[122]=6; exptbl[123]=5; exptbl[124]=5; exptbl[125]=5; exptbl[126]=5; exptbl[127]=5; exptbl[128]=5; exptbl[129]=5;
    exptbl[130]=4; exptbl[131]=4; exptbl[132]=4; exptbl[133]=4; exptbl[134]=4; exptbl[135]=4; exptbl[136]=4; exptbl[137]=4; exptbl[138]=3; exptbl[139]=3;
    exptbl[140]=3; exptbl[141]=3; exptbl[142]=3; exptbl[143]=3; exptbl[144]=3; exptbl[145]=3; exptbl[146]=3; exptbl[147]=3; exptbl[148]=3; exptbl[149]=2;
    exptbl[150]=2; exptbl[151]=2; exptbl[152]=2; exptbl[153]=2; exptbl[154]=2; exptbl[155]=2; exptbl[156]=2; exptbl[157]=2; exptbl[158]=2; exptbl[159]=2;
    exptbl[160]=2; exptbl[161]=2; exptbl[162]=2; exptbl[163]=2; exptbl[164]=2; exptbl[165]=1; exptbl[166]=1; exptbl[167]=1; exptbl[168]=1; exptbl[169]=1;
    exptbl[170]=1; exptbl[171]=1; exptbl[172]=1; exptbl[173]=1; exptbl[174]=1; exptbl[175]=1; exptbl[176]=1; exptbl[177]=1; exptbl[178]=1; exptbl[179]=1;
    exptbl[180]=1; exptbl[181]=1; exptbl[182]=1; exptbl[183]=1; exptbl[184]=1; exptbl[185]=1; exptbl[186]=1; exptbl[187]=1; exptbl[188]=1; exptbl[189]=1;
    exptbl[190]=1; exptbl[191]=1; exptbl[192]=1; exptbl[193]=1; exptbl[194]=1; exptbl[195]=1; exptbl[196]=1; exptbl[197]=1; exptbl[198]=1; exptbl[199]=1;
    exptbl[200]=0; exptbl[201]=0; exptbl[202]=0; exptbl[203]=0; exptbl[204]=0; exptbl[205]=0; exptbl[206]=0; exptbl[207]=0; exptbl[208]=0; exptbl[209]=0;
    exptbl[210]=0; exptbl[211]=0; exptbl[212]=0; exptbl[213]=0; exptbl[214]=0; exptbl[215]=0; exptbl[216]=0; exptbl[217]=0; exptbl[218]=0; exptbl[219]=0;
    exptbl[220]=0; exptbl[221]=0; exptbl[222]=0; exptbl[223]=0; exptbl[224]=0; exptbl[225]=0; exptbl[226]=0; exptbl[227]=0; exptbl[228]=0; exptbl[229]=0;
    exptbl[230]=0; exptbl[231]=0; exptbl[232]=0; exptbl[233]=0; exptbl[234]=0; exptbl[235]=0; exptbl[236]=0; exptbl[237]=0; exptbl[238]=0; exptbl[239]=0;
    exptbl[240]=0; exptbl[241]=0; exptbl[242]=0; exptbl[243]=0; exptbl[244]=0; exptbl[245]=0; exptbl[246]=0; exptbl[247]=0; exptbl[248]=0; exptbl[249]=0;
    exptbl[250]=0; exptbl[251]=0; exptbl[252]=0; exptbl[253]=0; exptbl[254]=0; exptbl[255]=0;
}

initlg()
{
    logtbl[0]=22713; logtbl[1]=22713; logtbl[2]=19874; logtbl[3]=18213; logtbl[4]=17035; logtbl[5]=16121; logtbl[6]=15374; logtbl[7]=14743; logtbl[8]=14196; logtbl[9]=13713;
    logtbl[10]=13282; logtbl[11]=12891; logtbl[12]=12535; logtbl[13]=12207; logtbl[14]=11903; logtbl[15]=11621; logtbl[16]=11357; logtbl[17]=11108; logtbl[18]=10874; logtbl[19]=10653;
    logtbl[20]=10443; logtbl[21]=10243; logtbl[22]=10052; logtbl[23]=9870; logtbl[24]=9696; logtbl[25]=9529; logtbl[26]=9368; logtbl[27]=9213; logtbl[28]=9064; logtbl[29]=8921;
    logtbl[30]=8782; logtbl[31]=8647; logtbl[32]=8517; logtbl[33]=8391; logtbl[34]=8269; logtbl[35]=8150; logtbl[36]=8035; logtbl[37]=7923; logtbl[38]=7813; logtbl[39]=7707;
    logtbl[40]=7603; logtbl[41]=7502; logtbl[42]=7404; logtbl[43]=7307; logtbl[44]=7213; logtbl[45]=7121; logtbl[46]=7031; logtbl[47]=6943; logtbl[48]=6857; logtbl[49]=6772;
    logtbl[50]=6689; logtbl[51]=6608; logtbl[52]=6529; logtbl[53]=6451; logtbl[54]=6374; logtbl[55]=6299; logtbl[56]=6225; logtbl[57]=6153; logtbl[58]=6081; logtbl[59]=6011;
    logtbl[60]=5943; logtbl[61]=5875; logtbl[62]=5808; logtbl[63]=5743; logtbl[64]=5678; logtbl[65]=5615; logtbl[66]=5552; logtbl[67]=5491; logtbl[68]=5430; logtbl[69]=5370;
    logtbl[70]=5311; logtbl[71]=5253; logtbl[72]=5196; logtbl[73]=5139; logtbl[74]=5084; logtbl[75]=5029; logtbl[76]=4974; logtbl[77]=4921; logtbl[78]=4868; logtbl[79]=4816;
    logtbl[80]=4764; logtbl[81]=4713; logtbl[82]=4663; logtbl[83]=4613; logtbl[84]=4564; logtbl[85]=4516; logtbl[86]=4468; logtbl[87]=4421; logtbl[88]=4374; logtbl[89]=4328;
    logtbl[90]=4282; logtbl[91]=4237; logtbl[92]=4192; logtbl[93]=4148; logtbl[94]=4104; logtbl[95]=4060; logtbl[96]=4017; logtbl[97]=3975; logtbl[98]=3933; logtbl[99]=3891;
    logtbl[100]=3850; logtbl[101]=3810; logtbl[102]=3769; logtbl[103]=3729; logtbl[104]=3690; logtbl[105]=3650; logtbl[106]=3612; logtbl[107]=3573; logtbl[108]=3535; logtbl[109]=3497;
    logtbl[110]=3460; logtbl[111]=3423; logtbl[112]=3386; logtbl[113]=3350; logtbl[114]=3314; logtbl[115]=3278; logtbl[116]=3242; logtbl[117]=3207; logtbl[118]=3172; logtbl[119]=3138;
    logtbl[120]=3103; logtbl[121]=3069; logtbl[122]=3036; logtbl[123]=3002; logtbl[124]=2969; logtbl[125]=2936; logtbl[126]=2904; logtbl[127]=2871; logtbl[128]=2839; logtbl[129]=2807;
    logtbl[130]=2776; logtbl[131]=2744; logtbl[132]=2713; logtbl[133]=2682; logtbl[134]=2651; logtbl[135]=2621; logtbl[136]=2591; logtbl[137]=2561; logtbl[138]=2531; logtbl[139]=2501;
    logtbl[140]=2472; logtbl[141]=2443; logtbl[142]=2414; logtbl[143]=2385; logtbl[144]=2357; logtbl[145]=2328; logtbl[146]=2300; logtbl[147]=2272; logtbl[148]=2244; logtbl[149]=2217;
    logtbl[150]=2189; logtbl[151]=2162; logtbl[152]=2135; logtbl[153]=2108; logtbl[154]=2082; logtbl[155]=2055; logtbl[156]=2029; logtbl[157]=2003; logtbl[158]=1977; logtbl[159]=1951;
    logtbl[160]=1925; logtbl[161]=1900; logtbl[162]=1874; logtbl[163]=1849; logtbl[164]=1824; logtbl[165]=1799; logtbl[166]=1774; logtbl[167]=1750; logtbl[168]=1725; logtbl[169]=1701;
    logtbl[170]=1677; logtbl[171]=1653; logtbl[172]=1629; logtbl[173]=1605; logtbl[174]=1582; logtbl[175]=1558; logtbl[176]=1535; logtbl[177]=1512; logtbl[178]=1488; logtbl[179]=1466;
    logtbl[180]=1443; logtbl[181]=1420; logtbl[182]=1397; logtbl[183]=1375; logtbl[184]=1353; logtbl[185]=1330; logtbl[186]=1308; logtbl[187]=1286; logtbl[188]=1265; logtbl[189]=1243;
    logtbl[190]=1221; logtbl[191]=1200; logtbl[192]=1178; logtbl[193]=1157; logtbl[194]=1136; logtbl[195]=1115; logtbl[196]=1094; logtbl[197]=1073; logtbl[198]=1052; logtbl[199]=1032;
    logtbl[200]=1011; logtbl[201]=991; logtbl[202]=970; logtbl[203]=950; logtbl[204]=930; logtbl[205]=910; logtbl[206]=890; logtbl[207]=870; logtbl[208]=850; logtbl[209]=831;
    logtbl[210]=811; logtbl[211]=792; logtbl[212]=772; logtbl[213]=753; logtbl[214]=734; logtbl[215]=715; logtbl[216]=696; logtbl[217]=677; logtbl[218]=658; logtbl[219]=639;
    logtbl[220]=621; logtbl[221]=602; logtbl[222]=584; logtbl[223]=565; logtbl[224]=547; logtbl[225]=529; logtbl[226]=511; logtbl[227]=492; logtbl[228]=474; logtbl[229]=457;
    logtbl[230]=439; logtbl[231]=421; logtbl[232]=403; logtbl[233]=386; logtbl[234]=368; logtbl[235]=351; logtbl[236]=333; logtbl[237]=316; logtbl[238]=299; logtbl[239]=281;
    logtbl[240]=264; logtbl[241]=247; logtbl[242]=230; logtbl[243]=213; logtbl[244]=197; logtbl[245]=180; logtbl[246]=163; logtbl[247]=147; logtbl[248]=130; logtbl[249]=114;
    logtbl[250]=97; logtbl[251]=81; logtbl[252]=65; logtbl[253]=48; logtbl[254]=32; logtbl[255]=16; logtbl[256]=0;
}

/* ============================================================ */
/* Weight persistence (raw CP/M sector I/O)                     */
/* ============================================================ */

/* save the six Q16 weight arrays to WFILE; 0 ok, ERROR on fail */
int savew()
{
    int fd;

    fd = creat(WFILE);
    if (fd == ERROR)
        return ERROR;
    close(fd);
    fd = open(WFILE, 2);
    if (fd == ERROR)
        return ERROR;
    if (write(fd, wtke, STKE) != STKE) { close(fd); return ERROR; }
    if (write(fd, wpse, SPSE) != SPSE) { close(fd); return ERROR; }
    if (write(fd, wwq, SWQ) != SWQ) { close(fd); return ERROR; }
    if (write(fd, wwk, SWQ) != SWQ) { close(fd); return ERROR; }
    if (write(fd, wwv, SWQ) != SWQ) { close(fd); return ERROR; }
    if (write(fd, wwot, SWOT) != SWOT) { close(fd); return ERROR; }
    close(fd);
    return 0;
}

/* load the six Q16 weight arrays from WFILE; 0 ok, ERROR on fail */
int loadw()
{
    int fd;

    fd = open(WFILE, 0);
    if (fd == ERROR)
        return ERROR;
    if (read(fd, wtke, STKE) != STKE) { close(fd); return ERROR; }
    if (read(fd, wpse, SPSE) != SPSE) { close(fd); return ERROR; }
    if (read(fd, wwq, SWQ) != SWQ) { close(fd); return ERROR; }
    if (read(fd, wwk, SWQ) != SWQ) { close(fd); return ERROR; }
    if (read(fd, wwv, SWQ) != SWQ) { close(fd); return ERROR; }
    if (read(fd, wwot, SWOT) != SWOT) { close(fd); return ERROR; }
    close(fd);
    return 0;
}

/* ============================================================ */
/* Entry point                                                  */
/* ============================================================ */

main(argc, argv)
int argc;
char *argv[];
{
    int step, train, ns, vs;
    char *fname;

    initex();
    initlg();

    train = 0;
    fname = IFILE;
    if (argc > 1) {
        if (strcmp(argv[1], "-t") == 0 || strcmp(argv[1], "-T") == 0)
            train = 1;
        else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "-H") == 0 ||
                 strcmp(argv[1], "/?") == 0) {
            printf("attn - tiny transformer that reverses 8 digits\n\n");
            printf("usage:\n");
            printf("  attn          infer from %s (one 8-digit line each)\n", IFILE);
            printf("  attn <file>   infer from <file> instead\n");
            printf("  attn -t       train, save weights to %s, then test\n", WFILE);
            printf("  attn -h       this help\n");
            return 0;
        } else
            fname = argv[1];        /* explicit input file */
    }

    printf("attn/11 - paper tape is all you need\n");
    printf("d=16 seq=8 v=10 params=1216 q8/q15/q16\n\n");

    if (train) {
        printf("training...\n");
        rseed = 887;
        initw();
        zerog();
        thit = 0;
        ttot = 0;
        for (step = 1; step <= NSTEP; step++) {
            tstep = step;
            putchar('.');       /* heartbeat: one dot per completed step */
            gensm();
            trseq();
            if ((step % FSTEP) == 0)
                filrun(IFILE, 1);
            if ((step % RPRT) == 0) {
                report();
                vs = filrun(IFILE, 0);
                if (vs != ERROR) {
                    printf(" validation %2d/%d on %s\n", vhits, vs, IFILE);
                    if (vhits == vs && vs > 0) {
                        printf("validation passed; stopping early\n");
                        break;
                    }
                }
            }
        }
        printf("\nsaving weights to %s ...\n", WFILE);
        if (savew() != 0)
            printf("WARNING: could not save weights\n");
        printf("\n");
        test();
        ns = runfil(IFILE);
        if (ns != ERROR)
            printf("\naccuracy  %2d/%d on %s\n", fhits, ns, IFILE);
        return 0;
    }

    /* inference */
    printf("loading weights from %s ...\n", WFILE);
    if (loadw() != 0) {
        printf("no weights file found - run 'attn -t' first\n");
        return 1;
    }
    cvt16();                        /* build Q8 weight copies once */

    ns = runfil(fname);
    if (ns == ERROR) {
        if (argc > 1 && fname != IFILE) {
            printf("cannot open input file %s\n", fname);
            return 1;
        }
        /* no input file present: fall back to a random demo */
        printf("no %s found - running random demo\n\n", IFILE);
        rseed = 1;
        test();
        return 0;
    }

    printf("\naccuracy  %2d/%d\n", fhits, ns);
    return 0;
}
