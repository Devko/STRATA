#!/usr/bin/env python3
"""Offline kick + rumble lab (sound design only, nothing here runs on the unit).

Renders STRATA's own KICK presets with kick_render (the PR #269 machine and insert chain, built
for the host), then builds the rumble recipes from the 2026-11-28 research on top in numpy, and
writes level-matched WAVs for listening.

    python3 tools/kick_lab/lab.py KICK_RENDER_BINARY OUTDIR
"""
import os, subprocess, sys
import numpy as np
from scipy import signal
from scipy.io import wavfile

SR = 48000
BPM = 132.0
BEAT = SR * 60.0 / BPM
BEATS = 16                       # four bars
N = int(BEAT * BEATS)
t_beat = np.arange(int(BEAT)) / SR
RENDER = sys.argv[1]
OUT = sys.argv[2]
rng = np.random.default_rng(1)


def strata_kick(preset, *overrides):
    raw = subprocess.run([RENDER, preset, str(BPM), str(BEATS), *overrides],
                         check=True, capture_output=True).stdout
    x = np.frombuffer(raw, np.float32).astype(np.float64)
    return x[:N]


def tile(one):
    out = np.zeros(N)
    for b in range(BEATS):
        s = int(round(b * BEAT))
        e = min(N, s + len(one))
        out[s:e] += one[:e - s]
    return out


# --- building blocks ---------------------------------------------------------------------------

def tanh_adaa(x, drive):
    """First-order antiderivative-antialiased tanh (Parker et al. 2016)."""
    u = x * drive
    F = np.log(np.cosh(np.clip(u, -30, 30)))
    up = np.concatenate([[0.0], u[:-1]])
    Fp = np.concatenate([[0.0], F[:-1]])
    d = u - up
    small = np.abs(d) < 1e-5
    y = np.where(small, np.tanh(0.5 * (u + up)), (F - Fp) / np.where(small, 1, d))
    return y / np.tanh(drive)


def soft(x, drive):
    return np.tanh(x * drive) / np.tanh(drive)


def hard(x, drive):
    return np.clip(x * drive, -1, 1)


def lp(x, hz, order=4, q=None):
    if q is None:
        sos = signal.butter(order, hz, 'low', fs=SR, output='sos')
        return signal.sosfilt(sos, x)
    # resonant 24 dB: two biquads, the second with Q
    b, a = signal.iirfilter(2, hz, btype='low', ftype='butter', fs=SR)
    y = signal.lfilter(b, a, x)
    w0 = 2 * np.pi * hz / SR
    alpha = np.sin(w0) / (2 * q)
    bb = np.array([(1 - np.cos(w0)) / 2, 1 - np.cos(w0), (1 - np.cos(w0)) / 2])
    aa = np.array([1 + alpha, -2 * np.cos(w0), 1 - alpha])
    return signal.lfilter(bb / aa[0], aa / aa[0], y)


def hp(x, hz, order=2):
    return signal.sosfilt(signal.butter(order, hz, 'high', fs=SR, output='sos'), x)


def bell(x, hz, db, q=1.2):
    A = 10 ** (db / 40)
    w0 = 2 * np.pi * hz / SR
    al = np.sin(w0) / (2 * q)
    b = [1 + al * A, -2 * np.cos(w0), 1 - al * A]
    a = [1 + al / A, -2 * np.cos(w0), 1 - al / A]
    return signal.lfilter(np.array(b) / a[0], np.array(a) / a[0], x)


def comb(x, d, g, damp):
    # y[n] = x[n-d] + g * lp(y)[n-d], lp one-pole with coefficient damp
    b = np.zeros(d + 2); b[d] = 1.0; b[d + 1] = -damp
    a = np.zeros(d + 2); a[0] = 1.0; a[1] = -damp; a[d] -= g * (1 - damp)
    return signal.lfilter(b, a, x)


def allpass(x, d, g=0.5):
    b = np.zeros(d + 1); b[0] = -g; b[d] = 1.0
    a = np.zeros(d + 1); a[0] = 1.0; a[d] = -g
    return signal.lfilter(b, a, x)


def reverb(x, t60, damp=0.6, predelay_ms=5):
    """A mono Schroeder/Freeverb-style hall: 8 damped combs, 4 all-passes."""
    pd = int(predelay_ms * SR / 1000)
    x = np.concatenate([np.zeros(pd), x])[:len(x)]
    out = np.zeros_like(x)
    for ms in (29.7, 37.1, 41.1, 43.7, 47.3, 53.9, 59.3, 61.7):
        d = int(ms * SR / 1000)
        g = 10 ** (-3 * d / (t60 * SR))
        out += comb(x, d, g, damp)
    for ms in (5.0, 1.7, 12.6, 9.1):
        out = allpass(out, int(ms * SR / 1000))
    return out / 8


def duck_env(depth=1.0, attack_ms=1.0, hold_ms=40.0, release_ms=180.0, shape=1.0):
    """STRATA's Duck (dsp/chain.hpp): linear attack/hold/release, one per beat. shape > 1 bends the
    release so the rumble swells late, towards the offbeat."""
    one = np.ones(int(BEAT))
    a = int(attack_ms * SR / 1000); h = int(hold_ms * SR / 1000); r = int(release_ms * SR / 1000)
    amt = np.zeros(int(BEAT))
    amt[:a] = np.linspace(0, 1, a)
    amt[a:a + h] = 1
    amt[a + h:a + h + r] = (1 - np.linspace(0, 1, r)) ** (1 / shape)
    one = 1 - depth * amt
    out = np.ones(N)
    for b in range(BEATS):
        s = int(round(b * BEAT))
        e = min(N, s + len(one))
        out[s:e] = one[:e - s]
    return out


def taps(x, sixteenths=(1, 2, 3), gains=(0.75, 0.55, 0.4), invert=False):
    """Omega's TAPS idea: feedback-free FIR, delayed copies of the kick on the 16th grid."""
    out = np.zeros_like(x)
    six = BEAT / 4
    for i, (k, g) in enumerate(zip(sixteenths, gains)):
        d = int(round(k * six))
        sgn = -1 if (invert and i % 2) else 1
        out[d:] += sgn * g * x[:-d]
    return out


def tempo_delay(x, division=0.25, fb=0.35, tone_hz=2000):
    d = int(round(BEAT * division))
    # feedback delay with a low-pass in the loop, via lfilter on blocks of d
    y = np.zeros_like(x)
    state = 0.0
    a = np.exp(-2 * np.pi * tone_hz / SR)
    buf = np.zeros_like(x)
    for n0 in range(0, len(x), d):
        seg = x[n0:n0 + d].copy()
        if n0 >= d:
            prev = y[n0 - d:n0 - d + len(seg)]
            seg[:len(prev)] += fb * prev
        y[n0:n0 + len(seg)] = seg
    # wet only: shift by d, darkened
    wet = np.concatenate([np.zeros(d), y])[:len(x)]
    return signal.lfilter([1 - a], [1, -a], wet)


def midi_rumble(root_hz, vels=(65, 90, 115), decay_ms=90, cutoff=140):
    """16ths 2-4 of each beat: a low-passed saw at the root, velocities ramping up."""
    six = int(BEAT / 4)
    t = np.arange(six) / SR
    ph = (root_hz * t) % 1.0
    saw = 2 * ph - 1
    env = np.exp(-t / (decay_ms / 1000)) * np.minimum(1, t / 0.002)
    out = np.zeros(N)
    for b in range(BEATS):
        for k, v in zip((1, 2, 3), vels):
            s = int(round(b * BEAT + k * BEAT / 4))
            e = min(N, s + six)
            out[s:e] += (v / 127) * (saw * env)[:e - s]
    return lp(soft(lp(out, cutoff * 2, 2), 2.0), cutoff, 4)


def glue(x, thresh_db=-14, ratio=2.5, att_ms=30, rel_ms=80):
    """A feed-forward RMS-ish compressor, 1-2 dB of glue."""
    a_a = np.exp(-1 / (att_ms * SR / 1000)); a_r = np.exp(-1 / (rel_ms * SR / 1000))
    lvl = np.abs(x)
    env = np.zeros_like(x)
    e = 0.0
    for i in range(0, len(x), 32):                      # control rate, 32 samples
        v = lvl[i:i + 32].max()
        e = a_a ** 32 * e + (1 - a_a ** 32) * v if v > e else a_r ** 32 * e + (1 - a_r ** 32) * v
        env[i:i + 32] = e
    db = 20 * np.log10(env + 1e-9)
    over = np.maximum(0, db - thresh_db)
    g = 10 ** (-(over - over / ratio) / 20)
    return x * g


def low_end_bus(x):
    return tanh_adaa(glue(hp(x, 27, 4)), 1.3)


# --- the variants ------------------------------------------------------------------------------

out_files = []


def write(name, x, note):
    out_files.append((name, x, note))


print('rendering STRATA kicks...')
techno = strata_kick('TECHNO')
heavy = strata_kick('HEAVY')
fat = strata_kick('FAT')
f0 = 25.0 * 2 ** (2.2572390 * 55 / 127)        # TUN 55, TECHNO's: about 49 Hz

# A: dry kicks
write('A1_STRATA_TECHNO_dry', techno, 'PR #269 TECHNO, machine + insert chain (host render)')
write('A2_STRATA_HEAVY_dry', heavy, 'PR #269 HEAVY')
# TECHNO with existing knobs only: a held, shorter body and a choke (HLD 13 = 41 ms, DEC 72, CHK 90)
tight = strata_kick('TECHNO', '8=72', '31=13', '28=90')
write('A3_STRATA_TECHNO_held_short', tight,
      'TECHNO with existing knobs: HLD 41 ms, DEC 72, CHK 90 (tight dry kick for a rumble)')
# KNK reaching higher: today 15 x f0 (~1 kHz start); the paste's recipe starts 1.5-4 kHz.
# Emulated offline by adding the missing top sweep: 3 ms sweep 2.5 kHz -> 300 Hz, phase-locked.
t = t_beat
sweep_f = 300 + 2200 * np.exp(-t / 0.0025)
sweep = np.sin(2 * np.pi * np.cumsum(sweep_f) / SR) * np.exp(-t / 0.006) * 0.35
write('A4_STRATA_TECHNO_plus_top_sweep', tanh_adaa(tight + tile(sweep), 1.2),
      'A3 plus a 2.5 kHz to 300 Hz click sweep (what a wider KNK range would add)')
# the paste's own python kick, verbatim maths
f_inst = 49.0 + 2200.0 * np.exp(-t / 0.006) + 160.0 * np.exp(-t / 0.035)
ph = 2 * np.pi * np.cumsum(f_inst) / SR
amp = np.exp(-t / 0.11) * np.clip(1 - (t / 0.26) ** 4, 0, 1)
click = rng.normal(0, 0.18, len(t)) * np.exp(-t / 0.005)
paste_kick = tile(np.tanh(1.4 * (np.sin(ph) * amp + click)))
write('A5_paste_reference_kick', paste_kick, "the other LLM's numpy kick, as given")
# the recipe kick, done properly: two-stage pitch, hold, ADAA clip, 280 Hz scoop
f_inst = f0 + 2600 * np.exp(-t / 0.003) + 170 * np.exp(-t / 0.030)
ph = 2 * np.pi * np.cumsum(f_inst) / SR
hold = 0.045; L = 0.26
amp = np.where(t < hold, 1.0, np.clip(1 - (np.maximum(t - hold, 0) / (L - hold)) ** 1.6, 0, 1))
tick = hp(rng.normal(0, 1, len(t)), 3000) * np.exp(-t / 0.0015) * 0.25
recipe = bell(tanh_adaa(np.sin(ph) * amp + tick, 2.2), 280, -4)
recipe_kick = tile(recipe)
write('A6_recipe_kick', recipe_kick, 'recipe: phase-locked sine, 2.6 kHz/3 ms + 170 Hz/30 ms sweeps, 45 ms hold, gone by 260 ms, ADAA tanh, -4 dB at 280 Hz')

# B: rumbles under the tight STRATA kick (A3). Every rumble mono, ducked by the kick.
dry = tight
duck = duck_env(1.0, 1, 30, 200, shape=0.6)


def mix(dry, rum, level):
    r = rum / (np.sqrt(np.mean(rum ** 2)) + 1e-12) * np.sqrt(np.mean(dry ** 2)) * level
    return dry + r


write('B0_no_rumble', dry, 'A3 alone, for reference')
# B1: STRATA today, rumble in the machine: RMB 100 RLN 100 on the tight kick
b1 = strata_kick('TECHNO', '8=72', '31=13', '28=90', '29=100', '30=100')
write('B1_STRATA_RMB_today', b1, "PR #269's RMB/RLN (noise + sine swell inside the kick) on A3")
# B2: STRATA today, reverb send with RDK: reverb (HP 30 Hz), no drive, no low-pass, ducked
rv = reverb(hp(dry, 30), 1.0, damp=0.7)
write('B2_STRATA_reverb_RDK_today', mix(dry, rv * duck, 0.8),
      'what STRATA can do today: kick into the reverb, return ducked (RDK), no drive or low-pass')
# B3: the canonical chain: reverb -> drive -> LP24 110 Hz res -> mono -> duck
r3 = lp(soft(reverb(dry, 0.9, damp=0.7), 6.0), 110, q=1.1) * duck
write('B3_reverb_drive_LP_duck', mix(dry, r3, 0.9),
      'canonical: reverb 0.9 s -> tanh drive -> LP24 110 Hz (Q 1.1) -> mono -> duck')
# B4: Omega-style TAPS: kick copies on 16ths 2-4 -> drive -> LP -> duck
r4 = lp(soft(lp(taps(dry), 400, 2), 4.0), 120, q=1.0) * duck_env(1.0, 1, 20, 150, 0.8)
write('B4_TAPS_16th_FIR', mix(dry, r4, 0.9),
      'TAPS: feedback-free 16th copies (0.75/0.55/0.4) -> drive -> LP24 120 Hz -> duck')
# B5: hypnotic: 1/16 delay into a short room, drive, LP
r5 = lp(soft(reverb(tempo_delay(dry, 0.25, 0.3, 800), 0.5, damp=0.6), 5.0), 120, q=1.0) * duck
write('B5_16th_delay_into_room', mix(dry, r5, 0.9),
      'hypnotic: 1/16 delay (fb 0.3) -> 0.5 s room -> drive -> LP24 120 Hz -> duck')
# B6: industrial: long reverb, hard clip, LP 170
r6 = lp(hard(reverb(dry, 1.4, damp=0.5), 8.0), 170, q=0.9) * duck_env(1.0, 1, 25, 180, 0.7)
write('B6_industrial_hardclip', mix(dry, r6, 0.9),
      'industrial: 1.4 s reverb -> hard clip -> LP24 170 Hz -> duck')
# B7: MIDI rumble (synth on 16ths 2-4, velocities 65/90/115)
r7 = midi_rumble(f0)
write('B7_midi_rumble_synth', mix(dry, r7, 0.8),
      'synth rumble: saw at the root on 16ths 2-4, velocities 65/90/115, LP 140 Hz')
# B8: the paste's prime-tap rumble on the STRATA kick
rr = np.zeros_like(dry)
for i, ms in enumerate([37, 53, 79, 107, 139, 173, 214, 263, 317, 379]):
    d = int(SR * ms / 1000)
    rr[d:] += 0.65 * 0.84 ** i * (-1 if i % 2 else 1) * dry[:-d]
r8 = signal.lfilter([1 - np.exp(-2 * np.pi * 110 / SR)], [1, -np.exp(-2 * np.pi * 110 / SR)],
                    np.tanh(4.5 * rr))
t_ = (np.arange(int(BEAT)) / SR)
dk = 1 - 0.92 * np.exp(-((t_ / (0.38 * BEAT / SR)) ** 2))
r8 = r8 * tile(dk)[:N]
write('B8_paste_prime_taps', mix(dry, r8, 0.8),
      "the other LLM's rumble: 10 prime-spaced taps -> tanh -> one-pole 110 Hz -> gaussian duck")
# B9: decimated rumble reverb: the same as B3 but the reverb runs at 6 kHz (1/8 rate)
dec = signal.decimate(lp(dry, 400, 4), 8, ftype='fir', zero_phase=False)
SR_lo = SR // 8


def reverb_lo(x, t60, damp):
    out = np.zeros_like(x)
    for ms in (29.7, 37.1, 41.1, 43.7):
        d = int(ms * SR_lo / 1000)
        g = 10 ** (-3 * d / (t60 * SR_lo))
        out += comb(x, d, g, damp)
    for ms in (5.0, 12.6):
        out = allpass(out, int(ms * SR_lo / 1000))
    return out / 4


rl = soft(reverb_lo(dec, 0.9, 0.3), 6.0)
up = signal.resample_poly(rl, 8, 1)[:N]
r9 = lp(up, 110, q=1.1) * duck
write('B9_decimated_reverb_rumble', mix(dry, r9, 0.9),
      'B3 with a 4-comb reverb at 6 kHz (1/8 rate): the cheap in-kick rumble candidate')

# C: the low-end bus on the best-guess pair
pair = mix(dry, r3, 0.9)
write('C1_B3_through_low_end_bus', low_end_bus(pair),
      'B3 -> HP 27 Hz -> glue comp (2.5:1, 30/80 ms) -> ADAA soft clip')
write('C2_recipe_kick_B3_rumble_bus',
      low_end_bus(mix(recipe_kick, lp(soft(reverb(recipe_kick, 0.9, 0.7), 6.0), 110, q=1.1) * duck, 0.9)),
      'A6 recipe kick with the B3 rumble through the low-end bus')
# C3: clipper A/B: hard vs ADAA on HEAVY (aliasing check)
write('C3_HEAVY_naive_clip_x4', soft(heavy * 0.5, 4.0), 'HEAVY into a naive tanh at x4')
write('C4_HEAVY_ADAA_clip_x4', tanh_adaa(heavy * 0.5, 4.0), 'HEAVY into an ADAA tanh at x4')

# --- write, level-matched ----------------------------------------------------------------------
os.makedirs(OUT, exist_ok=True)
target = 10 ** (-14 / 20)
gains = [target / (np.sqrt(np.mean(x ** 2)) + 1e-12) for _, x, _ in out_files]
peak = max(np.max(np.abs(x)) * g for (_, x, _), g in zip(out_files, gains))
trim = min(1.0, 0.97 / peak)
lines = []
tour = []
gap = np.zeros(int(0.6 * SR))
for (name, x, note), g in zip(out_files, gains):
    y = x * g * trim
    pcm = (np.clip(y, -1, 1) * 32767).astype(np.int16)
    wavfile.write(os.path.join(OUT, name + '.wav'), SR, np.stack([pcm, pcm], 1))
    tour += [y[:int(BEAT * 8)], gap]
    lines.append(f'{name}: {note}')
y = np.concatenate(tour)
pcm = (np.clip(y, -1, 1) * 32767).astype(np.int16)
wavfile.write(os.path.join(OUT, '00_TOUR_all_variants_2bars_each.wav'), SR, np.stack([pcm, pcm], 1))
with open(os.path.join(OUT, 'INDEX.txt'), 'w') as f:
    f.write(f'All files 132 BPM, 4 bars, 48 kHz, level-matched to the same RMS ({20*np.log10(target*trim):.1f} dBFS).\n')
    f.write('00_TOUR plays the first 2 bars of each, in this order:\n\n')
    f.write('\n'.join(lines) + '\n')
print('\n'.join(lines))
