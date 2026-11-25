"""STRATA visual mockups: 128 x 128, 16 greys, 20 fps, Enaga's 5x7 font plus a 3x5 font.

    python tools/visuals_mockup.py third_party/enaga/firmware/hal/gfx.c OUTDIR

Needs numpy and Pillow, which nothing in the build does. The 3x5 font (TINY_SRC) is the one
app/paint.cpp carries.

Draws each proposed screen frame by frame from a simulated beat (130 BPM, 8 tracks) and writes
animated GIFs at 50 ms a frame (the unit's draw rate) plus PNG stills. Every pixel write is counted
so the draw cost per frame can be estimated. Mockups only: not app code.
"""
import math, os, re, sys
import numpy as np
from PIL import Image

W = H = 128
FPS = 20
NFRAMES = 56
SR = 24000
BPM = 130.0
STEP = 60.0 / BPM / 4

# ---------------------------------------------------------------- fonts
def load_font(gfx):
    src = open(gfx).read()
    table = re.search(r"static const u8 font\[\]\[5\] = \{(.*?)\};", src, re.S).group(1)
    table = re.sub(r"/\*.*?\*/", "", table, flags=re.S)
    glyphs = [[int(v, 0) for v in g.split(",") if v.strip()] for g in re.findall(r"G\(([^)]*)\)", table)]
    chars = re.search(r'font_chars\[\] = "([^"]*)"', src).group(1)
    return dict(zip(chars, glyphs))

TINY_SRC = {
 'A':"010101111101101",'B':"110101110101110",'C':"011100100100011",'D':"110101101101110",
 'E':"111100110100111",'F':"111100110100100",'G':"011100101101011",'H':"101101111101101",
 'I':"111010010010111",'J':"001001001101010",'K':"101101110101101",'L':"100100100100111",
 'M':"101111111101101",'N':"110101101101101",'O':"010101101101010",'P':"110101110100100",
 'Q':"010101101110011",'R':"110101110101101",'S':"011100010001110",'T':"111010010010010",
 'U':"101101101101111",'V':"101101101101010",'W':"101101111111101",'X':"101101010101101",
 'Y':"101101010010010",'Z':"111001010100111",'0':"111101101101111",'1':"010110010010111",
 '2':"110001010100111",'3':"110001010001110",'4':"101101111001001",'5':"111100110001110",
 '6':"011100111101111",'7':"111001010010010",'8':"111101111101111",'9':"111101111001110",
 '.':"000000000000010",'-':"000000111000000",'%':"101001010100101",':':"000010000010000",
 '/':"001001010100100",'+':"000010111010000",' ':"000000000000000",'>':"100010001010100",
 '<':"001010100010001",
}

class Scr:
    writes = 0
    def __init__(s, font):
        s.p = np.zeros((H, W), np.uint8); s.font = font
    def clear(s): s.p[:] = 0; Scr.writes += W * H
    def px(s, x, y, g):
        x = int(x); y = int(y)
        if 0 <= x < W and 0 <= y < H:
            s.p[y, x] = g; Scr.writes += 1
    def pxmax(s, x, y, g):
        x = int(x); y = int(y)
        if 0 <= x < W and 0 <= y < H and g > s.p[y, x]:
            s.p[y, x] = g; Scr.writes += 1
    def rect(s, x, y, w, h, g):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
        if x1 > x0 and y1 > y0:
            s.p[y0:y1, x0:x1] = g; Scr.writes += (x1 - x0) * (y1 - y0)
    def frame(s, x, y, w, h, g):
        s.rect(x, y, w, 1, g); s.rect(x, y + h - 1, w, 1, g); s.rect(x, y, 1, h, g); s.rect(x + w - 1, y, 1, h, g)
    def text(s, x, y, txt, g=15):
        for ch in txt:
            gl = s.font.get(ch, s.font['?'])
            for cx in range(5):
                for cy in range(7):
                    if gl[cx] >> cy & 1: s.px(x + cx, y + cy, g)
            x += 6
    def tiny(s, x, y, txt, g=15):
        for ch in txt:
            b = TINY_SRC.get(ch, TINY_SRC[' '])
            for i, c in enumerate(b):
                if c == '1': s.px(x + i % 3, y + i // 3, g)
            x += 4
    def big(s, x, y, txt, g=15):          # the 5x7 font doubled: 10 x 14 digits
        for ch in txt:
            gl = s.font.get(ch, s.font['?'])
            for cx in range(5):
                for cy in range(7):
                    if gl[cx] >> cy & 1: s.rect(x + 2 * cx, y + 2 * cy, 2, 2, g)
            x += 12
    def inv(s, x, y, w, txt, small=False):   # a badge: grey 15 box, black text
        h = 7 if small else 9
        s.rect(x, y, w, h, 15)
        (s.tiny if small else s.text)(x + 2, y + 1, txt, 0)
    def line(s, x0, y0, x1, y1, g):          # Wu anti-aliased line, max-blended
        steep = abs(y1 - y0) > abs(x1 - x0)
        if steep: x0, y0, x1, y1 = y0, x0, y1, x1
        if x0 > x1: x0, x1, y0, y1 = x1, x0, y1, y0
        dx = x1 - x0; dy = y1 - y0
        grad = dy / dx if dx else 1.0
        n = int(round(x1)) - int(round(x0))
        x = int(round(x0)); y = y0 + grad * (x - x0)
        for _ in range(n + 1):
            f = y - math.floor(y); yi = int(math.floor(y))
            a, b = int(round(g * (1 - f))), int(round(g * f))
            if steep: s.pxmax(yi, x, a); s.pxmax(yi + 1, x, b)
            else: s.pxmax(x, yi, a); s.pxmax(x, yi + 1, b)
            x += 1; y += grad
    def poly(s, pts, g):
        for (a, b), (c, d) in zip(pts, pts[1:]): s.line(a, b, c, d, g)
    def hdots(s, x0, x1, y, g, step=2):
        for x in range(x0, x1, step): s.px(x, y, g)
    def vdots(s, x, y0, y1, g, step=2):
        for y in range(y0, y1, step): s.px(x, y, g)
    def dot(s, x, y, g):                       # a 3x3 soft dot
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                s.pxmax(x + dx, y + dy, g if dx == 0 and dy == 0 else (g * 2 // 3 if dx == 0 or dy == 0 else g // 3))

# ---------------------------------------------------------------- the simulated beat
TRK = ["KCK", "SNR", "HHC", "HHO", "BAS", "PAD", "SMP", "GRN"]
PAT = [
    [1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,1,0],
    [0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,1],
    [1,0,1,0, 1,0,1,0, 1,0,1,1, 1,0,1,0],
    [0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,1,0],
    [0,0,1,0, 0,1,0,1, 0,0,1,0, 0,1,1,0],
    [1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0],
    [0,0,0,1, 0,0,0,0, 0,0,0,1, 0,0,0,0],
    [1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0],
]
LOCKS = {(0, 14), (1, 15), (2, 11), (4, 5), (4, 13)}
DUR = NFRAMES / FPS + 0.2
N = int(DUR * SR)
rng = np.random.default_rng(7)

def synth():
    t = np.arange(N) / SR
    tracks = np.zeros((8, 2, N)); envs = np.zeros((8, N))
    def place(tr, fn, length):
        L = int(length * SR)
        for st in range(int(DUR / STEP) + 1):
            if PAT[tr][st % 16]:
                i0 = int(st * STEP * SR); i1 = min(N, i0 + L)
                if i0 >= N: break
                l, r, e = fn(np.arange(i1 - i0) / SR)
                tracks[tr, 0, i0:i1] += l; tracks[tr, 1, i0:i1] += r; envs[tr, i0:i1] = np.maximum(envs[tr, i0:i1], e)
    def kick(tt):
        e = np.exp(-tt * 9); ph = 2 * np.pi * (48 * tt + 90 * (1 - np.exp(-tt * 30)) / 30)
        v = np.sin(ph) * e; return v, v, e
    def snare(tt):
        e = np.exp(-tt * 18); n = rng.standard_normal(len(tt)) * 0.5
        v = (n + np.sin(2 * np.pi * 190 * tt)) * e * 0.6; return v, v * 0.9, e
    def hat(dec, pan):
        def f(tt):
            e = np.exp(-tt * dec); n = rng.standard_normal(len(tt))
            n = np.diff(n, prepend=0) * 0.25 * e; return n * (1 - pan), n * (1 + pan), e
        return f
    def bass(tt):
        e = np.exp(-tt * 6); f = 55 * 1.5
        v = (2 * ((f * tt) % 1) - 1) * e * 0.5; return v, v, e
    def pad(tt):
        e = np.minimum(1, tt * 3) * np.exp(-tt * 0.8)
        l = np.sin(2 * np.pi * 220 * tt) + 0.5 * np.sin(2 * np.pi * 277.2 * tt)
        r = np.sin(2 * np.pi * 220.8 * tt + 0.7) + 0.5 * np.sin(2 * np.pi * 330 * tt)
        return l * e * 0.25, r * e * 0.25, e
    def smp(tt):
        e = np.exp(-tt * 4); v = np.sin(2 * np.pi * 440 * tt * (1 + tt)) * e * 0.3; return v, v * 0.7, e
    def grn(tt):
        e = np.minimum(1, tt * 2) * np.exp(-tt * 0.5)
        v = np.sin(2 * np.pi * 330 * tt + 3 * np.sin(2 * np.pi * 3 * tt)) * e * 0.2
        return v * 0.6, v * 1.2, e
    place(0, kick, 0.5); place(1, snare, 0.3); place(2, hat(40, -0.3), 0.08); place(3, hat(8, 0.3), 0.4)
    place(4, bass, 0.3); place(5, pad, 2.0); place(6, smp, 0.6); place(7, grn, 3.0)
    master = np.tanh(tracks.sum(0) * 0.8)
    return tracks, envs, master

TRACKS, ENVS, MASTER = synth()

def idx(fr): return min(N - 1, int(fr / FPS * SR))
def level(sig, fr):                        # block peak over the last 50 ms
    i = idx(fr); return float(np.abs(sig[..., max(0, i - SR // 20):i + 1]).max()) if i > 0 else 0.0
def step_at(fr): return int(fr / FPS / STEP)
def step_frac(fr): return fr / FPS / STEP - step_at(fr)

# ---------------------------------------------------------------- shared frame (V1)
class Meter:
    def __init__(s): s.v = 0.0; s.pk = 0.0; s.hold = 0
    def feed(s, x):
        s.v = max(x, s.v * 0.7)
        if x >= s.pk: s.pk, s.hold = x, 20
        elif s.hold: s.hold -= 1
        else: s.pk *= 0.9

def db01(x, floor=-48.0):
    return max(0.0, min(1.0, (20 * math.log10(max(x, 1e-6)) - floor) / -floor))

def chrome(s, fr, track, page, tabs, meter):
    st = step_at(fr); bar = st // 16 + 1; beat = st % 16 // 4 + 1
    s.inv(0, 0, 21, "A01")
    s.text(24, 1, "%d.%d" % (bar, beat), 15)
    s.tiny(46, 2, "130.0", 9)
    s.tiny(70, 2, "26%", 5)
    s.inv(106, 0, 22, track)
    # progress: 16 ticks, the played part bright, the current step pulsing
    pos = (st % 16 + step_frac(fr)) / 16
    s.rect(0, 10, 120, 1, 2)
    s.rect(0, 10, int(pos * 120), 1, 10)
    for k in range(0, 16, 4): s.px(k * 120 // 16, 11, 5)
    # master meter, x 123-127, rows 12-118, segmented, peak dot held
    meter.feed(level(MASTER, fr))
    top, bot = 13, 118
    n = int(db01(meter.v) * (bot - top)); pk = int(db01(meter.pk) * (bot - top))
    for y in range(bot - n, bot, 2):
        g = 15 if y < top + 10 else (11 if y < top + 35 else 7)
        s.rect(124, y, 3, 1, g)
    s.rect(124, bot - pk, 3, 1, 15)
    s.vdots(122, top, bot, 2, 6)
    # page tabs
    x = 0
    for t in tabs:
        w = len(t) * 4 + 4
        if t == page: s.rect(x, 121, w, 7, 15); s.tiny(x + 2, 122, t, 0)
        else: s.tiny(x + 2, 122, t, 5)
        x += w + 2

# ---------------------------------------------------------------- screens
class Perform:
    name = "perform"
    def __init__(s): s.m = Meter(); s.trail = np.zeros((H, W)); s.lv = [0.0] * 8
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T1", "PERF", ["PERF", "MIX", "FX", "LOOP"], s.m)
        vals = [0.5 + 0.45 * math.sin(fr / 9), 0.3 + 0.2 * math.sin(fr / 5.3), 0.65, 0.2 + 0.15 * math.sin(fr / 13)]
        labels = ["CUT", "RES", "DLY", "VRB"]
        for k in range(4):
            cx, cy, r = 14 + k * 30, 30, 11
            for a in range(0, 271, 6):                 # dim track, 270 degrees
                ang = math.radians(135 + a); sc.px(cx + r * math.cos(ang), cy + r * math.sin(ang), 3)
            end = 270 * vals[k]; prev = None
            for a in np.arange(0, end + 0.1, 4):       # bright value arc, 2 px thick
                ang = math.radians(135 + a)
                for rr in (r, r - 1):
                    sc.pxmax(cx + rr * math.cos(ang), cy + rr * math.sin(ang), 15)
            ang = math.radians(135 + end); sc.line(cx, cy, cx + (r - 3) * math.cos(ang), cy + (r - 3) * math.sin(ang), 12)
            sc.tiny(cx - 5, cy + 6, "%03d" % int(vals[k] * 127), 15)
            sc.inv(cx - 10, 43, 5, chr(65 + k), small=True)
            sc.tiny(cx - 3, 44, labels[k], 8)
        # master scope with phosphor persistence
        s.trail *= 0.55
        i = idx(fr); seg = MASTER[0, max(0, i - 360):i]
        if len(seg) == 360:
            z = np.where((seg[:-121] < 0) & (seg[1:-120] >= 0))[0]    # trigger on a rising zero crossing
            o = z[-1] if len(z) else 0
            w = seg[o:o + 120]
            for x in range(119):
                y0, y1 = 71 - w[x] * 14, 71 - w[x + 1] * 14
                for y in range(int(min(y0, y1)), int(max(y0, y1)) + 1): s.trail[y, x] = 15
        sc.hdots(0, 120, 71, 3, 4)
        g = s.trail.astype(np.uint8)
        sc.p[54:89, 0:120] = np.maximum(sc.p[54:89, 0:120], g[54:89, 0:120]); Scr.writes += 35 * 120
        sc.tiny(0, 52, "OUT", 6)
        # 8 track meters with names, mute on T6
        for k in range(8):
            x = k * 15
            s.lv[k] = max(level(TRACKS[k], fr), s.lv[k] * 0.72)
            h = int(db01(s.lv[k], -36) * 20)
            muted = k == 5
            for y in range(112 - h, 112, 2): sc.rect(x + 2, y, 10, 1, 4 if muted else (15 if y < 96 else 9))
            sc.rect(x + 2, 112, 10, 1, 3)
            sc.tiny(x + 1, 114, TRK[k], 3 if muted else 10)

class Grid:
    name = "grid"
    def __init__(s): s.m = Meter(); s.flash = np.zeros((8, 16)); s.last = -1; s.lv = [0.0] * 8
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T1", "GRID", ["GRID", "STEP", "PAT", "SONG"], s.m)
        st = step_at(fr)
        if st != s.last:
            s.last = st
            for t in range(8):
                if PAT[t][st % 16]: s.flash[t, st % 16] = 1.0
        x0, y0, cw, ch = 9, 15, 7, 11
        col = st % 16
        sc.rect(x0 + col * cw, y0 - 1, cw - 1, 8 * ch + 1, 2)            # playhead column
        for t in range(8):
            s.lv[t] = max(level(TRACKS[t], fr), s.lv[t] * 0.7)
            sc.tiny(0, y0 + t * ch + 3, str(t + 1), 15 if t == 0 else 6)
            lv = int(db01(s.lv[t], -36) * 9)
            sc.rect(5, y0 + t * ch + 9 - lv, 2, lv, 8)
            for k in range(16):
                x, y = x0 + k * cw, y0 + t * ch + 2
                if PAT[t][k]:
                    f = s.flash[t, k]
                    g = int(7 + 8 * f)
                    grow = 1 if f > 0.6 else 0
                    sc.rect(x - grow, y - grow, 5 + 2 * grow, 7 + 2 * grow, g)
                    if (t, k) in LOCKS: sc.rect(x + 3, y, 2, 2, 0); sc.px(x + 4, y, 15)
                else:
                    sc.px(x + 2, y + 3, 4 if k % 4 == 0 else 2)
        s.flash *= 0.6
        sc.tiny(0, 107, "PG1 L16 X1", 8); sc.tiny(56, 107, "SWG 54", 8); sc.tiny(88, 107, "PROB 100", 5)
        sc.rect(0, 114, 120, 1, 2)
        sc.tiny(0, 116, "T1 KCK  ACTIVE STEPS 5", 6)

ENV_Y0, ENV_Y1 = 34, 100
class Amp:
    name = "amp"
    def __init__(s): s.m = Meter(); s.trig = -99
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T1", "AMP", ["OSC", "FLT", "AMP", "LFO", "FX"], s.m)
        a = 4; d = 40 + 30 * (0.5 + 0.5 * math.sin(fr / 10)); sus = 0.45; r = 50
        sc.tiny(0, 15, "A ATK %03d" % a, 15); sc.tiny(42, 15, "B DEC %03d" % int(d), 15); sc.tiny(84, 15, "C SUS 057", 9)
        # x axis: time, 120 px; y: level
        X = lambda f: 2 + f * 116
        Y = lambda v: ENV_Y1 - v * (ENV_Y1 - ENV_Y0)
        ta, td, tr = a / 127 * 0.25, d / 127 * 0.45, r / 127 * 0.3
        hold = 1 - ta - td - tr
        pts = [(0, 0), (ta, 1), (ta + td, sus), (ta + td + hold * 0.9, sus), (1, 0)]
        def curve():
            out = []
            for i in range(121):
                f = i / 120
                if f < ta: v = f / ta
                elif f < ta + td: u = (f - ta) / td; v = sus + (1 - sus) * (1 - u) ** 2.2
                elif f < pts[3][0]: v = sus
                else: u = (f - pts[3][0]) / (1 - pts[3][0]); v = sus * (1 - u) ** 2.2
                out.append((X(f), Y(v)))
            return out
        c = curve()
        for x in range(0, 120, 8): sc.vdots(x, ENV_Y0, ENV_Y1 + 1, 2, 4)
        for v in (0.25, 0.5, 0.75): sc.hdots(0, 120, int(Y(v)), 2, 4)
        for (x, y) in c:                                     # dim fill under the curve
            for yy in range(int(y) + 2, ENV_Y1, 2): sc.pxmax(x, yy, 2)
        sc.poly(c, 9)
        # the voice: a bright head riding the curve after each kick, the travelled part lit
        st = step_at(fr)
        if PAT[0][st % 16] and fr - s.trig > 3 and step_frac(fr) < 0.45: s.trig = fr
        el = (fr - s.trig) / FPS / 1.0
        if 0 <= el < 1:
            k = int(el * 120)
            sc.poly(c[:k + 1], 15)
            for (x, y) in c[:k + 1]:
                for yy in range(int(y) + 2, ENV_Y1, 2): sc.pxmax(x, yy, 4)
            sc.dot(int(c[k][0]), int(c[k][1]), 15)
        for i, (f, v) in enumerate(pts[1:4]):
            x, y = int(X(f)), int(Y(v))
            sc.inv(x - 3, y - 10 if i < 2 else y - 10, 5, "ABC"[i], small=True)
        sc.rect(0, ENV_Y1, 120, 1, 6)
        sc.tiny(0, 104, "D REL %03d" % r, 9); sc.tiny(48, 104, "CRV EXP", 6)
        sc.tiny(0, 112, "VEL>AMP 64", 6)

class Filter:
    name = "filter"
    def __init__(s): s.m = Meter(); s.spec = np.zeros(60); s.trail = None
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T5", "FLT", ["OSC", "FLT", "AMP", "LFO", "FX"], s.m)
        cut = 300 * 2 ** (3.2 * (0.5 + 0.5 * math.sin(fr / 8))); res = 0.75
        sc.tiny(0, 15, "A CUT %5s" % (("%.1fK" % (cut / 1000)) if cut >= 1000 else "%dHZ" % cut), 15)
        sc.tiny(48, 15, "B RES 096", 15); sc.tiny(90, 15, "LP24", 9)
        fx = lambda f: 1 + (math.log10(f) - math.log10(30)) / (math.log10(12000) - math.log10(30)) * 118
        top, bot = 24, 100
        for f in (100, 1000, 10000):
            sc.vdots(int(fx(f)), top, bot, 2, 4)
        sc.tiny(int(fx(100)) - 5, 102, "100", 5); sc.tiny(int(fx(1000)) - 3, 102, "1K", 5); sc.tiny(int(fx(10000)) - 5, 102, "10K", 5)
        # live spectrum of the track behind the curve (bass, the filter's input)
        i = idx(fr); seg = TRACKS[4, 0, max(0, i - 1024):i]
        if len(seg) == 1024:
            sp = np.abs(np.fft.rfft(seg * np.hanning(1024)))
            freqs = np.fft.rfftfreq(1024, 1 / SR)
            for b in range(60):
                f0 = 30 * (12000 / 30) ** (b / 60); f1 = 30 * (12000 / 30) ** ((b + 1) / 60)
                m = (freqs >= f0) & (freqs < f1)
                v = sp[m].max() if m.any() else sp[np.argmin(np.abs(freqs - f0))]
                s.spec[b] = max(db01(v / 60, -60), s.spec[b] * 0.8)
        for b in range(60):
            x = 1 + b * 2; h = int(s.spec[b] * (bot - top) * 0.9)
            sc.rect(x, bot - h, 1, h, 3)
        # SVF 24 dB response with resonance
        q = 0.5 + res * 6; pts = []
        for x in range(1, 120):
            f = 30 * (12000 / 30) ** ((x - 1) / 118); w = f / cut
            mag = 1 / math.sqrt((1 - w * w) ** 2 + (w / q) ** 2)
            db = 2 * 20 * math.log10(mag) * 0.5
            y = top + 22 - db * 1.1
            pts.append((x, max(top, min(bot, y))))
        for (x, y) in pts:
            for yy in range(int(y) + 2, bot, 3): sc.pxmax(x, yy, 1)
        sc.poly(pts, 15)
        sc.rect(int(fx(cut)), top, 1, bot - top, 6)
        sc.inv(int(fx(cut)) - 3, bot - 8, 5, "A", small=True)
        sc.rect(0, bot, 120, 1, 6)
        sc.tiny(0, 110, "ENV>CUT +32  LFO>CUT 20", 7)

class Lfo:
    name = "lfo"
    def __init__(s): s.m = Meter(); s.hist = []
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T5", "LFO", ["OSC", "FLT", "AMP", "LFO", "FX"], s.m)
        sc.tiny(0, 15, "A WAV TRI", 15); sc.tiny(42, 15, "B RAT 1/4", 15); sc.tiny(84, 15, "C DEP 080", 15)
        shape = lambda u: 1 - 4 * abs(u - 0.5) if True else 0
        x0, x1, yc, amp = 2, 76, 58, 26
        sc.hdots(x0, x1, yc, 4, 3)
        sc.frame(x0 - 2, yc - amp - 4, x1 - x0 + 4, 2 * amp + 9, 3)
        pts = [(x0 + i * (x1 - x0) / 64, yc - shape(i / 64) * amp) for i in range(65)]
        sc.poly(pts, 8)
        ph = (fr / FPS / (STEP * 4)) % 1                     # 1/4: a beat
        k = int(ph * 64)
        for j in range(1, 10):                               # a short fading trail behind the head
            kk = (k - j * 2) % 64
            sc.dot(int(pts[kk][0]), int(pts[kk][1]), max(2, 12 - j))
        sc.dot(int(pts[k][0]), int(pts[k][1]), 15)
        sc.vdots(int(pts[k][0]), yc - amp - 3, yc + amp + 4, 2, 6)
        # the output over time, scrolling (a strip chart), and the destination bar
        v = shape(ph); s.hist.append(v); s.hist = s.hist[-38:]
        sc.frame(82, yc - amp - 4, 38, 2 * amp + 9, 3)
        for i in range(1, len(s.hist)):
            sc.line(119 - (len(s.hist) - i), yc - s.hist[i - 1] * amp, 120 - (len(s.hist) - i), yc - s.hist[i] * amp, 12)
        sc.tiny(84, 91, "OUT", 6)
        sc.tiny(0, 92, "D DST CUT", 15)
        cut = 0.5 + 0.4 * v
        sc.rect(0, 100, 120, 5, 2); sc.rect(0, 100, int(cut * 120), 5, 12); sc.rect(int(0.5 * 120), 99, 1, 7, 15)
        sc.tiny(0, 108, "SYNC ON  TRIG FREE", 7)

class Wave:
    name = "wave"
    def __init__(s): s.m = Meter()
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T7", "OSC", ["OSC", "FLT", "AMP", "LFO", "FX"], s.m)
        pos = 0.5 + 0.48 * math.sin(fr / 7)
        sc.tiny(0, 15, "A TBL DIGI3", 15); sc.tiny(52, 15, "B POS %03d" % int(pos * 127), 15); sc.tiny(96, 15, "MOT+", 9)
        NF, NP = 16, 40
        def frame_wave(fi, u):
            m = fi / (NF - 1)
            sine = math.sin(2 * math.pi * u)
            saw = 2 * ((u + 0.5) % 1) - 1
            sq = math.tanh(6 * math.sin(2 * math.pi * u))
            return (1 - m) * sine + m * (saw if m < 0.6 else sq) * 0.9 + 0.15 * m * math.sin(6 * math.pi * u)
        cur = pos * (NF - 1)
        for fi in range(NF - 1, -1, -1):                    # back to front, hidden lines by blacking out below
            ox, oy = 6 + fi * 3.4, 80 - fi * 2.6
            pts = [(ox + i * 1.6, oy - frame_wave(fi, i / (NP - 1)) * 11) for i in range(NP)]
            for (x, y) in pts:
                sc.rect(int(x), int(y) + 1, 2, max(0, int(oy + 12 - y)), 0)
            d = abs(fi - cur)
            g = 15 if d < 0.5 else (8 if d < 1.5 else 4)
            sc.poly(pts, g)
        # the live frame big, below
        sc.rect(0, 94, 120, 1, 3)
        pts = [(2 + i * 116 / 63, 106 - (frame_wave(int(cur), i / 63) * (1 - (cur % 1)) + frame_wave(min(NF - 1, int(cur) + 1), i / 63) * (cur % 1)) * 9) for i in range(64)]
        sc.poly(pts, 13)
        sc.tiny(0, 97, "LIVE", 6)

class Grain:
    name = "grain"
    def __init__(s):
        s.m = Meter(); r = np.random.default_rng(3)
        n = 118; t = np.linspace(0, 1, n)
        s.env = np.abs(np.sin(t * 9) * np.exp(-t * 1.2) + 0.35 * r.standard_normal(n) * np.exp(-t)) * 0.9 + 0.05
        s.grains = []; s.r = np.random.default_rng(11); s.trail = np.zeros((H, W))
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "T8", "SRC", ["SRC", "GRN", "AMP", "LFO", "FX"], s.m)
        pos = 0.38 + 0.25 * math.sin(fr / 14); size = 0.12; spray = 0.08
        sc.tiny(0, 15, "A POS %03d" % int(pos * 127), 15); sc.tiny(42, 15, "B SIZ 040", 15); sc.tiny(84, 15, "C SPR 020", 15)
        yc, amp = 46, 20
        a, b = int(pos * 118) + 1, int((pos + size) * 118) + 1
        for x in range(118):                                 # min/max overview, the window brighter
            h = int(s.env[x] * amp)
            g = 9 if a <= x + 1 <= b else 4
            sc.rect(x + 1, yc - h, 1, 2 * h + 1, g)
        sc.rect(a, yc - amp - 3, 1, 2 * amp + 7, 15); sc.rect(b, yc - amp - 3, 1, 2 * amp + 7, 15)
        sc.rect(a, yc - amp - 3, b - a, 1, 15); sc.rect(a, yc + amp + 3, b - a, 1, 15)
        for x in (30, 64, 96): sc.vdots(x, yc - amp, yc + amp, 3, 6)  # slice marks
        # grain cloud: each grain a particle at its read position, rising by pitch, fading with its envelope
        for _ in range(4):
            p = pos + size * s.r.random() + spray * s.r.standard_normal() * 0.5
            s.grains.append([p, 0.0, s.r.choice([-12, 0, 0, 7, 12]), s.r.uniform(0.008, 0.02)])
        s.trail *= 0.72
        alive = []
        for g in s.grains:
            g[1] += 0.12
            if g[1] < 1:
                alive.append(g)
                e = math.sin(math.pi * g[1])
                x = 1 + (g[0] + g[3] * g[1] * 4) * 118
                y = 100 - (g[2] + 12) / 24 * 26 - g[1] * 3
                if 0 <= x < 120 and 70 <= y < 104: s.trail[int(y), int(x)] = max(s.trail[int(y), int(x)], 15 * e)
                gx = int(x)                                   # its playhead on the waveform
                if 0 <= gx < 120: sc.pxmax(gx, yc - amp - 1, int(15 * e)); sc.pxmax(gx, yc + amp + 1, int(15 * e))
        s.grains = alive
        sc.rect(0, 72, 120, 1, 2)
        sc.tiny(0, 74, "GRAINS 8/8", 6)
        for y in range(76, 104):
            for x in range(120):
                v = s.trail[y, x]
                if v > 1: sc.dot(x, y, int(v)) if v > 10 else sc.pxmax(x, y, int(v))
        sc.tiny(100, 74, "+12", 4); sc.tiny(100, 98, "-12", 4)
        sc.tiny(0, 108, "LOOP.WAV  2.41S  44.1K", 7)

class Master:
    name = "master"
    def __init__(s): s.m = Meter(); s.bands = np.zeros(30); s.peaks = np.zeros(30); s.hold = np.zeros(30); s.trail = np.zeros((40, 40)); s.gr = 0.0
    def draw(s, sc, fr):
        sc.clear(); chrome(sc, fr, "MST", "MST", ["EQ", "DCK", "DLY", "MST"], s.m)
        i = idx(fr); seg = MASTER[:, max(0, i - 1024):i]
        top, bot = 15, 58
        if seg.shape[1] == 1024:
            mono = seg.mean(0)
            sp = np.abs(np.fft.rfft(mono * np.hanning(1024))); freqs = np.fft.rfftfreq(1024, 1 / SR)
            for b in range(30):
                f0 = 35 * (11000 / 35) ** (b / 30); f1 = 35 * (11000 / 35) ** ((b + 1) / 30)
                m = (freqs >= f0) & (freqs < f1)
                v = sp[m].max() if m.any() else sp[np.argmin(np.abs(freqs - f0))]
                v = db01(v / 80 * (1 + b / 10), -54)
                s.bands[b] = max(v, s.bands[b] - 0.08)
                if v >= s.peaks[b]: s.peaks[b] = v; s.hold[b] = 12
                elif s.hold[b] > 0: s.hold[b] -= 1
                else: s.peaks[b] = max(0, s.peaks[b] - 0.03)
        for b in range(30):
            x = b * 4; h = int(s.bands[b] * (bot - top))
            for y in range(bot - h, bot, 2):
                sc.rect(x, y, 3, 1, 15 if y < top + 12 else (11 if y < top + 28 else 7))
            sc.rect(x, bot - int(s.peaks[b] * (bot - top)) - 1, 3, 1, 15)
        sc.rect(0, bot + 1, 120, 1, 4)
        for f, lab in ((100, "100"), (1000, "1K"), (10000, "10K")):
            x = int(math.log(f / 35) / math.log(11000 / 35) * 120); sc.tiny(x - 4, bot + 3, lab, 5)
        # vectorscope (goniometer), 40 x 40, with persistence
        s.trail *= 0.5
        L, R = MASTER[0, max(0, i - 1200):i:4], MASTER[1, max(0, i - 1200):i:4]
        for l, r in zip(L, R):
            m, sd = (l + r) * 0.7, (l - r) * 0.7
            x, y = int(20 + sd * 18), int(20 - m * 18)
            if 0 <= x < 40 and 0 <= y < 40: s.trail[y, x] = 15
        ox, oy = 0, 68
        sc.frame(ox, oy, 42, 42, 3); sc.vdots(ox + 21, oy + 1, oy + 41, 3, 4); sc.hdots(ox + 1, ox + 41, oy + 21, 3, 4)
        g = s.trail.astype(np.uint8); sub = sc.p[oy + 1:oy + 41, ox + 1:ox + 41]
        sc.p[oy + 1:oy + 41, ox + 1:ox + 41] = np.maximum(sub, g); Scr.writes += 1600
        sc.tiny(ox + 1, oy + 43, "M/S", 5)
        # compressor and limiter: gain reduction falls from the top, numbers beside
        lvl = level(MASTER, fr); s.gr = max(db01(lvl, -12) * 0.8, s.gr * 0.8)
        for k, (lab, v) in enumerate((("CMP", s.gr), ("LIM", s.gr * 0.4))):
            x = 50 + k * 18
            sc.rect(x, 70, 8, 38, 2); sc.rect(x, 70, 8, int(v * 38), 13)
            sc.tiny(x - 2, 111, lab, 8)
        sc.tiny(88, 78, "GR", 6); sc.tiny(88, 86, "-%.1f" % (s.gr * 6), 15)
        sc.tiny(88, 98, "LUFS", 6); sc.tiny(88, 106, "-9.4", 15)

# ---------------------------------------------------------------- output
PAL = []
for g in range(16):
    if g == 0: PAL += [10, 12, 16]
    else:
        t = g / 15.0; PAL += [int(30 + 215 * t), int(36 + 214 * t), int(44 + 211 * t)]
PAL += [24, 26, 30, 6, 8, 10]                                   # 16 bezel, 17 grid line
PAL += [0] * (768 - len(PAL))
SCALE, BORDER = 3, 6

def to_img(p):
    big = np.kron(p, np.ones((SCALE, SCALE), np.uint8))
    big[SCALE - 1::SCALE, :] = np.where(big[SCALE - 1::SCALE, :] > 0, big[SCALE - 1::SCALE, :] * 3 // 4, 17)
    size = (W + 2 * BORDER) * SCALE
    out = np.full((size, size), 16, np.uint8)
    o = BORDER * SCALE; out[o:o + W * SCALE, o:o + W * SCALE] = big
    im = Image.fromarray(out, "P"); im.putpalette(PAL); return im

def main():
    font = load_font(sys.argv[1]); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
    report = []
    stills = []
    for cls in (Perform, Grid, Amp, Filter, Lfo, Wave, Grain, Master):
        scr = Scr(font); view = cls(); frames = []; costs = []
        for fr in range(NFRAMES + 8):
            Scr.writes = 0
            view.draw(scr, fr + 8)
            if fr >= 8: frames.append(to_img(scr.p.copy())); costs.append(Scr.writes)
        frames[0].save(os.path.join(out, view.name + ".gif"), save_all=True, append_images=frames[1:],
                       duration=1000 // FPS, loop=0, optimize=False, disposal=1)
        frames[NFRAMES // 2].save(os.path.join(out, view.name + ".png"))
        stills.append(frames[NFRAMES // 2])
        report.append("%-8s pixel writes per frame: mean %6d  max %6d" % (view.name, sum(costs) / len(costs), max(costs)))
    w, h = stills[0].size
    sheet = Image.new("P", (4 * w, 2 * h), 16); sheet.putpalette(PAL)
    for k, im in enumerate(stills): sheet.paste(im, ((k % 4) * w, (k // 4) * h))
    sheet.save(os.path.join(out, "overview.png"))
    print("\n".join(report))

if __name__ == "__main__":
    main()
