#!/usr/bin/env python3
"""
haptic-patterns.py: give MediaExperience's SystemSoundVibrationPatterns.plist legacy patterns the AW8697 path can play.

  tools/haptic-patterns.py IN.plist OUT.plist [--dump]

Most entries (Taptic Engine patterns: UISwitch_On_Haptic, OrbPop_Haptic, Ringtone_US_Haptic, ...) only have
"Atoms" (Tap / Minitap / Microtap / Sine / BoostSine... at StartTimes). With no VibeSynth hardware, FigVibrator uses
its legacy IOKit parser, which reads only Intensity + VibePattern (or On/Off/TotalDuration) and ignores Atoms; an
entry it cannot parse becomes a 1 s default buzz (research/haptics/ios-h/RESULTS.md). This adds to every Atoms-only
entry:
  VibePattern = [False, gap_ms, True, on_ms, False, gap_ms, ...]   Intensity = 0..1
Taps become short on-segments, which SweetVibrator (0249/0250) plays as RAM waveforms: on <= 40 ms is a tap whose
waveform follows intensity (1 light .. 5; 20 heavy for >= 25 ms at full intensity); longer on-segments buzz.
  Microtap  10 ms, strength 0.35        Minitap  18 ms, 0.65        Tap  30 ms, 1.0
  Sine / BoostSine / ...Brake / RampSine: their Duration, 1.0       Silence: nothing (a gap)
  Cycles + Period: the atom repeated Cycles times, Period apart
One Intensity per pattern (the legacy format has no per-segment strength): the strongest atom's
strength x atom Amplitude x pattern Amplitude. Atoms are kept. Entries that already have a legacy form are untouched.
"""
import plistlib
import sys

KIND = [('Microtap', 10, 0.35), ('Minitap', 18, 0.65), ('Tap', 30, 1.0)]
CONT = ('Sine', 'BoostSine', 'BoostSineBrake', 'SineBrake', 'RampSine')


def atom_events(a):
    t = a.get('Type', '')
    amp = float(a.get('Amplitude', a.get('Ampliude', 1.0)))
    if t == 'Silence' or amp <= 0:
        return []
    for name, ms, strength in KIND:
        if t.startswith(name) and (t == name or t[len(name):].isdigit()):
            dur, s = ms, strength
            break
    else:
        if t in CONT:
            dur, s = max(1, round(float(a.get('Duration', 0.1)) * 1000)), 1.0
        else:
            return []
    start = float(a.get('StartTime', 0.0)) * 1000
    cycles = int(a.get('Cycles', 1) or 1)
    period = float(a.get('Period', 0)) * 1000
    return [(start + i * period, dur, s * amp) for i in range(max(1, cycles))]


def convert(entry):
    ev = sorted(e for a in entry.get('Atoms', []) for e in atom_events(a))
    if not ev:
        return None
    pat, now = [], 0
    for start, dur, _ in ev:
        start = round(start)
        if start < now:                         # overlaps the previous one: start right after it
            start = now
        if start > now:
            pat += [False, start - now]
        pat += [True, dur]
        now = start + dur
    level = max(s for _, _, s in ev) * float(entry.get('Amplitude', 1.0))
    return pat, round(min(1.0, max(0.05, level)), 3)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    d = plistlib.load(open(sys.argv[1], 'rb'))
    n = 0
    for k, v in d.items():
        if not isinstance(v, dict) or 'Atoms' not in v or 'VibePattern' in v or 'OnDuration' in v:
            continue
        r = convert(v)
        if r is None:
            continue
        v['VibePattern'], v['Intensity'] = r
        n += 1
        if '--dump' in sys.argv:
            print(f'{k:40s} {r[1]:5.2f} {r[0][:12]}{" ..." if len(r[0]) > 12 else ""}')
    plistlib.dump(d, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
    print(f'{n} pattern(s) given a VibePattern -> {sys.argv[2]}')


if __name__ == '__main__':
    main()
