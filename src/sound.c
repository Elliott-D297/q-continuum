/* sound.c -- the themes' sound cues (LCARS, Stardew Valley, Pokemon), synthesised here from partials and envelopes.
 *
 * Played only while the theme has sounds (not the classic ones) and Settings > Appearance > Sounds is on. On WSL they
 * play in Windows (see "Windows players": WSLg's audio stutters and echoes); elsewhere the audio device opens on the first
 * cue (Linux needs libpulse), so the classic themes never touch it. Tests: QC_TEST_SOUND_LOG=<file> appends each cue's
 * name to the file instead of playing it; scripted and screenshot runs without it stay silent. Sound files of your own in
 * ~/.config/q-continuum/sounds (LCARS), sounds/stardew, sounds/pokemon, sounds/jarvis replace the synthesised cues (see user_file).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pl_util.h"
#include "raylib.h"
#include "sound.h"

#define RATE 44100
#define CLICK_VARIANTS 6     /* the console keys play a different chirp each press, as on the show */
#define CLICK_MAX 16         /* your own key sounds: click.wav .. click-16.wav */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *const NAMES[SND__N] = { "click", "denied", "error", "done", "alert", "stopped" };
/* the device is probed in a child first: when the sound server hangs (WSLg's PulseServer after sleep or a long uptime),
   opening it blocks ~30 s and libasound prints a page of errors as it falls back to ALSA */
enum { PR_NONE = 0, PR_RUNNING, PR_OK, PR_NOLIB, PR_HUNG };
static struct { bool tried, ok; Sound s[SND__N], click[CLICK_MAX]; int probe, lastClick, nClick, pack; pid_t pid; double t0; } S = { .pid = -1 };

/* one tone added into buf. The pitch glides f0 -> f1 geometrically, shaped by glide (1 even, < 1 fast at first, the
   klaxon's swoop); partials {mult, amp}; attack, then held (tau 0) or decaying; am_hz chops the level like the show's
   buzzy alarm; release fades the tail so it never ends on a click. */
typedef struct {
    double t0, dur, f0, f1, glide, amp, attack, tau, release, am_hz, am_depth, vib_hz, vib_depth;
    const double (*parts)[2]; int np;
} Tone;

static void tone(float *buf, int n, Tone T)
{
    int a = (int)(T.t0 * RATE), len = (int)(T.dur * RATE);
    double phase[8] = { 0 }, g = T.glide > 0 ? T.glide : 1, rel = T.release > 0 ? T.release : 0.008;
    for (int i = 0; i < len && a + i < n; i++) {
        double t = (double)i / RATE, u = (double)i / len, left = T.dur - t;
        double f = T.f0 * pow(T.f1 / T.f0, pow(u, g));
        if (T.vib_hz > 0) f *= 1 + T.vib_depth * sin(2 * M_PI * T.vib_hz * t);
        double env = t < T.attack ? t / T.attack : T.tau > 0 ? exp(-(t - T.attack) / T.tau) : 1;
        if (left < rel) env *= left / rel;
        if (T.am_hz > 0) env *= 1 - T.am_depth * (0.5 + 0.5 * cos(2 * M_PI * T.am_hz * t));
        double v = 0;
        for (int k = 0; k < T.np && k < 8; k++) {
            if (f * T.parts[k][0] > RATE / 2.2) continue;  /* no aliasing on the klaxon's top */
            phase[k] += 2 * M_PI * f * T.parts[k][0] / RATE;
            v += T.parts[k][1] * sin(phase[k]);
        }
        buf[a + i] += (float)(T.amp * env * v);
    }
}

/* the panel timbre: a soft square (odd partials, rolled off) */
static const double PANEL[][2] = { { 1, 1 }, { 3, 0.22 }, { 5, 0.07 }, { 7, 0.03 } };
/* the alarm: a bright sawtooth */
static const double SAW[][2] = { { 1, 1 }, { 2, 0.5 }, { 3, 0.33 }, { 4, 0.25 }, { 5, 0.2 }, { 6, 0.17 }, { 7, 0.14 }, { 8, 0.12 } };

#define BLIP(at, d, fa, fb, A) tone(buf, n, (Tone){ .t0 = (at), .dur = (d), .f0 = (fa), .f1 = (fb), .amp = (A), \
                                            .attack = 0.003, .release = 0.012, .parts = PANEL, .np = 4 })

/* to a 16-bit wave at the cue's peak level */
static Wave finish(float *buf, int n, double peak, float gain)
{
    float mx = 0;
    for (int i = 0; i < n; i++) mx = fabsf(buf[i]) > mx ? fabsf(buf[i]) : mx;
    float k = mx > 0 ? (float)peak / mx : 0;
    short *pcm = malloc(sizeof(short) * (size_t)n);
    for (int i = 0; i < n; i++) pcm[i] = (short)(buf[i] * k * gain * 32000);
    free(buf);
    return (Wave){ .frameCount = (unsigned)n, .sampleRate = RATE, .sampleSize = 16, .channels = 1, .data = pcm };
}

static Wave synth_lcars(Snd which, int variant, float gain)
{
    static const double total[SND__N] = { 0.12, 0.42, 0.26, 0.42, 2.4, 0.62 };
    int n = (int)(total[which] * RATE);
    float *buf = calloc((size_t)n, sizeof *buf);
    double peak = 0.8;                                   /* each cue is normalised to this */
    switch (which) {
    case SND_CLICK:                                      /* the panel key: one or two quick chirps, high and clean */
        peak = 0.55;
        switch (variant % CLICK_VARIANTS) {
        case 0: BLIP(0, 0.055, 2093, 2093, 1); break;
        case 1: BLIP(0, 0.035, 1568, 1568, 1); BLIP(0.045, 0.045, 2349, 2349, 1); break;
        case 2: BLIP(0, 0.035, 2637, 2637, 1); BLIP(0.045, 0.05, 1976, 1976, 1); break;
        case 3: BLIP(0, 0.06, 1760, 2217, 1); break;
        case 4: BLIP(0, 0.028, 2349, 2349, 1); BLIP(0.034, 0.028, 2794, 2794, 1); BLIP(0.068, 0.04, 3136, 3136, 1); break;
        default: BLIP(0, 0.05, 1319, 1319, 1); BLIP(0.06, 0.045, 1760, 1760, 0.8); break;
        }
        break;
    case SND_DENIED:                                     /* "unable to comply": two low notes, falling */
        BLIP(0.00, 0.15, 659, 659, 1);
        BLIP(0.19, 0.22, 440, 440, 1);
        break;
    case SND_ERROR:                                      /* three quick beeps on one note */
        for (int k = 0; k < 3; k++) BLIP(k * 0.075, 0.05, 1397, 1397, 1);
        break;
    case SND_DONE:                                       /* the computer's "working ... complete" trill, up to a held top note */
        { static const double up[] = { 1319, 1568, 1976, 2349, 2637 };
          for (int k = 0; k < 5; k++) BLIP(k * 0.04, 0.034, up[k], up[k], 0.9);
          BLIP(0.22, 0.19, 3136, 3136, 1); }
        break;
    case SND_ALERT:                                      /* red alert: three swooping, buzzing whoops */
        for (int k = 0; k < 3; k++)
            tone(buf, n, (Tone){ .t0 = k * 0.78, .dur = 0.6, .f0 = 300, .f1 = 1050, .glide = 0.55, .amp = 1,
                                 .attack = 0.015, .release = 0.05, .am_hz = 26, .am_depth = 0.45, .parts = SAW, .np = 8 });
        break;
    case SND_STOPPED:                                    /* powering down: three falling chirps, then a falling slide */
        BLIP(0.00, 0.05, 2093, 2093, 1);
        BLIP(0.07, 0.05, 1568, 1568, 1);
        BLIP(0.14, 0.05, 1175, 1175, 1);
        tone(buf, n, (Tone){ .t0 = 0.22, .dur = 0.38, .f0 = 1175, .f1 = 330, .glide = 0.7, .amp = 0.8, .attack = 0.004,
                             .tau = 0.16, .release = 0.05, .parts = PANEL, .np = 4 });
        break;
    default: break;
    }
    return finish(buf, n, peak, gain);
}

/* Pokemon: the handheld's square-wave voice. Original jingles, the games' kinds of cue: menu blip, bonk, fanfare,
   the low-HP beeping. */
static const double SQUARE[][2] = { { 1, 1 }, { 3, 0.33 }, { 5, 0.2 }, { 7, 0.14 }, { 9, 0.11 }, { 11, 0.09 } };
#define SQ(at, d, fa, fb, A) tone(buf, n, (Tone){ .t0 = (at), .dur = (d), .f0 = (fa), .f1 = (fb), .amp = (A), \
                                          .attack = 0.002, .release = 0.006, .parts = SQUARE, .np = 6 })

static Wave synth_pokemon(Snd which, int variant, float gain)
{
    static const double total[SND__N] = { 0.07, 0.3, 0.32, 0.75, 1.7, 0.42 };
    int n = (int)(total[which] * RATE);
    float *buf = calloc((size_t)n, sizeof *buf);
    double peak = 0.7;
    switch (which) {
    case SND_CLICK:                                      /* the menu cursor: one short high blip */
        peak = 0.45;
        { static const double f[] = { 1976, 2093, 1865 }; SQ(0, 0.045, f[variant % 3], f[variant % 3], 1); }
        break;
    case SND_DENIED:                                     /* a bonk: two low notes, falling */
        SQ(0.00, 0.09, 392, 392, 1); SQ(0.11, 0.17, 262, 262, 1);
        break;
    case SND_ERROR:                                      /* the buzzer */
        tone(buf, n, (Tone){ .dur = 0.3, .f0 = 196, .f1 = 185, .amp = 1, .attack = 0.003, .release = 0.02, .am_hz = 30,
                             .am_depth = 0.6, .parts = SQUARE, .np = 6 });
        break;
    case SND_DONE:                                       /* a little fanfare up to a held top note */
        SQ(0.00, 0.075, 784, 784, 0.9); SQ(0.09, 0.075, 1047, 1047, 0.9); SQ(0.18, 0.075, 1319, 1319, 0.9);
        SQ(0.27, 0.075, 1568, 1568, 0.9); SQ(0.37, 0.36, 2093, 2093, 1);
        tone(buf, n, (Tone){ .t0 = 0.37, .dur = 0.36, .f0 = 1047, .f1 = 1047, .amp = 0.5, .attack = 0.002, .release = 0.08,
                             .parts = SQUARE, .np = 6 });                   /* the harmony an octave down */
        break;
    case SND_ALERT:                                      /* low HP: the two-note beeping that won't stop */
        for (int k = 0; k < 7; k++) { SQ(k * 0.24, 0.1, 1175, 1175, 1); SQ(k * 0.24 + 0.12, 0.1, 880, 880, 1); }
        break;
    case SND_STOPPED:                                    /* ran away: a quick slide down */
        tone(buf, n, (Tone){ .dur = 0.4, .f0 = 1600, .f1 = 260, .glide = 0.8, .amp = 1, .attack = 0.002, .release = 0.05,
                             .parts = SQUARE, .np = 6 });
        break;
    default: break;
    }
    return finish(buf, n, peak, gain);
}

/* Stardew Valley: wooden, plucked and soft. Taps for keys, a marimba sparkle when a run is done, a low bell when one fails. */
static const double WOOD[][2] = { { 1, 1 }, { 2.76, 0.3 }, { 5.4, 0.08 } };            /* a struck bar */
static const double MARIMBA[][2] = { { 1, 1 }, { 4, 0.22 }, { 9.9, 0.05 } };
static const double TOLL[][2] = { { 1, 1 }, { 2.0, 0.5 }, { 2.4, 0.35 }, { 3.0, 0.25 }, { 4.2, 0.15 } };
#define PLUCK(at, d, f, A, T, P, NP) tone(buf, n, (Tone){ .t0 = (at), .dur = (d), .f0 = (f), .f1 = (f), .amp = (A), \
                                                  .attack = 0.002, .tau = (T), .release = 0.02, .parts = (P), .np = (NP) })

static Wave synth_stardew(Snd which, int variant, float gain)
{
    static const double total[SND__N] = { 0.09, 0.32, 0.36, 1.0, 2.3, 0.6 };
    int n = (int)(total[which] * RATE);
    float *buf = calloc((size_t)n, sizeof *buf);
    double peak = 0.75;
    switch (which) {
    case SND_CLICK:                                      /* a wooden tap, a slightly different block each time */
        peak = 0.5;
        { static const double f[] = { 880, 988, 784, 1047 }; PLUCK(0, 0.08, f[variant % 4], 1, 0.014, WOOD, 3); }
        break;
    case SND_DENIED:                                     /* two dull knocks */
        PLUCK(0.00, 0.13, 196, 1, 0.035, WOOD, 3); PLUCK(0.14, 0.16, 165, 1, 0.04, WOOD, 3);
        break;
    case SND_ERROR:                                      /* a bwomp, falling */
        tone(buf, n, (Tone){ .dur = 0.34, .f0 = 330, .f1 = 165, .glide = 0.6, .amp = 1, .attack = 0.004, .tau = 0.12,
                             .release = 0.05, .parts = WOOD, .np = 3 });
        break;
    case SND_DONE:                                       /* the marimba sparkle of something gathered */
        { static const double up[] = { 1047, 1319, 1568, 2093, 2637 };
          for (int k = 0; k < 5; k++) PLUCK(k * 0.075, 0.6, up[k], k == 4 ? 1 : 0.8, k == 4 ? 0.3 : 0.12, MARIMBA, 3); }
        break;
    case SND_ALERT:                                      /* the town bell: three low tolls, falling */
        { static const double f[] = { 440, 392, 330 };
          for (int k = 0; k < 3; k++) PLUCK(k * 0.55, 1.2, f[k], 1, 0.45, TOLL, 5); }
        break;
    case SND_STOPPED:                                    /* two soft plucks, down */
        PLUCK(0.00, 0.35, 784, 1, 0.12, MARIMBA, 3); PLUCK(0.16, 0.44, 523, 1, 0.16, MARIMBA, 3);
        break;
    default: break;
    }
    return finish(buf, n, peak, gain);
}

/* JARVIS: glassy and holographic. Near-pure sines with a faint detuned shimmer: soft ticks for keys, the arc reactor
   powering up into a chime when a run completes, a low pulsing alarm when one fails, a power-down sweep when stopped. */
static const double GLASS[][2] = { { 1, 1 }, { 1.005, 0.35 }, { 2, 0.12 }, { 3.01, 0.04 } };   /* the 1.005 beats slowly */
static const double HUM[][2] = { { 1, 1 }, { 2, 0.4 }, { 3, 0.2 } };
#define CHIME(at, d, fa, fb, A, T) tone(buf, n, (Tone){ .t0 = (at), .dur = (d), .f0 = (fa), .f1 = (fb), .amp = (A), \
                                                .attack = 0.004, .tau = (T), .release = 0.03, .parts = GLASS, .np = 4 })

static Wave synth_jarvis(Snd which, int variant, float gain)
{
    static const double total[SND__N] = { 0.08, 0.36, 0.3, 1.1, 2.3, 0.75 };
    int n = (int)(total[which] * RATE);
    float *buf = calloc((size_t)n, sizeof *buf);
    double peak = 0.7;
    switch (which) {
    case SND_CLICK:                                      /* a soft, high tick that glides up a little */
        peak = 0.4;
        { static const double f[] = { 2637, 2794, 3136, 2349 }; double a = f[variant % 4];
          CHIME(0, 0.07, a, a * 1.12, 1, 0.018); }
        break;
    case SND_DENIED:                                     /* two soft tones, down a fourth */
        CHIME(0.00, 0.16, 988, 988, 1, 0.07); CHIME(0.16, 0.2, 740, 740, 1, 0.08);
        break;
    case SND_ERROR:                                      /* a glitch: three stuttered blips on a falling pitch */
        for (int k = 0; k < 3; k++) CHIME(k * 0.07, 0.06, 1480 - k * 180, 1400 - k * 180, 1, 0.025);
        tone(buf, n, (Tone){ .t0 = 0.2, .dur = 0.1, .f0 = 300, .f1 = 220, .amp = 0.5, .attack = 0.003, .tau = 0.04,
                             .release = 0.02, .parts = HUM, .np = 3 });
        break;
    case SND_DONE:                                       /* the reactor spins up, then a bright three-note chime that rings */
        tone(buf, n, (Tone){ .dur = 0.42, .f0 = 220, .f1 = 1320, .glide = 0.7, .amp = 0.45, .attack = 0.05, .release = 0.06,
                             .parts = HUM, .np = 3 });
        CHIME(0.36, 0.7, 1319, 1319, 0.8, 0.22); CHIME(0.45, 0.62, 1661, 1661, 0.8, 0.22); CHIME(0.54, 0.56, 1976, 1976, 1, 0.3);
        break;
    case SND_ALERT:                                      /* the warning: four pulses of a low two-tone hum */
        for (int k = 0; k < 4; k++) {
            tone(buf, n, (Tone){ .t0 = k * 0.56, .dur = 0.22, .f0 = 523, .f1 = 523, .amp = 1, .attack = 0.01, .release = 0.04,
                                 .am_hz = 18, .am_depth = 0.25, .parts = HUM, .np = 3 });
            tone(buf, n, (Tone){ .t0 = k * 0.56 + 0.24, .dur = 0.22, .f0 = 392, .f1 = 392, .amp = 1, .attack = 0.01, .release = 0.04,
                                 .am_hz = 18, .am_depth = 0.25, .parts = HUM, .np = 3 });
        }
        break;
    case SND_STOPPED:                                    /* powering down: one chime, then the hum sweeps away */
        CHIME(0.00, 0.3, 1568, 1568, 0.8, 0.1);
        tone(buf, n, (Tone){ .t0 = 0.1, .dur = 0.62, .f0 = 1200, .f1 = 140, .glide = 0.6, .amp = 0.7, .attack = 0.01,
                             .tau = 0.3, .release = 0.08, .parts = HUM, .np = 3 });
        break;
    default: break;
    }
    return finish(buf, n, peak, gain);
}

/* the screen and game themes: one parametric synth. Each pack: a key click (timbre, pitches, length), the done cue (notes
   of an arpeggio or chord, timbre, ring), the alert (two alternating pitches, repeats, chop) and a falling stop */
static const double SINE1[][2] = { { 1, 1 } };
static const double SQ3[][2] = { { 1, 1 }, { 3, 0.33 }, { 5, 0.2 } };
static const double BELL[][2] = { { 1, 1 }, { 2.76, 0.45 }, { 5.4, 0.25 }, { 8.9, 0.1 } };
static const double METAL[][2] = { { 1, 1 }, { 1.48, 0.6 }, { 2.3, 0.4 }, { 3.7, 0.2 } };
typedef struct {
    const double (*clickP)[2]; int clickNp; double click[4], clickDur, clickTau; bool noiseClick;
    const double (*doneP)[2]; int doneNp; double done[5]; int nDone; double doneStep, doneTau; bool chord;
    const double (*alertP)[2]; int alertNp; double alert[2]; int alertN; double alertGap, alertAm;
    double stop0, stop1;
} Pack;
static const Pack PACKS[] = {
    /* Severance: soft, polite office chimes */
    { SINE1, 1, { 1760, 1976, 1568, 2093 }, 0.06, 0.02, false, GLASS, 4, { 880, 1109, 1319, 1760 }, 4, 0.11, 0.5, false,
      SINE1, 1, { 659, 523 }, 4, 0.5, 0, 880, 440 },
    /* Alien: MU/TH/UR's teletype chirps, a square two-tone alarm */
    { SQ3, 3, { 2400, 2600, 2200, 2800 }, 0.03, 0.012, false, SQ3, 3, { 1200, 1600, 1200, 1600, 2400 }, 5, 0.06, 0.06, false,
      SQ3, 3, { 1000, 700 }, 8, 0.28, 0.3, 1600, 300 },
    /* Pip-Boy: Geiger clicks for keys, the level-up chirp, a crackling rads warning */
    { SQ3, 3, { 3200, 3600, 3000, 3400 }, 0.012, 0.004, true, SQ3, 3, { 784, 988, 1175, 1568 }, 4, 0.07, 0.12, false,
      SQ3, 3, { 1400, 1400 }, 12, 0.12, 0.6, 1200, 200 },
    /* NERV: hard beeps and the command centre's klaxon */
    { SQ3, 3, { 1500, 1500, 1500, 1500 }, 0.04, 0.02, false, SQ3, 3, { 880, 880, 1175 }, 3, 0.15, 0.2, false,
      SAW, 8, { 660, 440 }, 6, 0.38, 0.0, 900, 150 },
    /* Portal: Aperture's clean ding, a dinging test-complete, a polite alarm */
    { SINE1, 1, { 2093, 2349, 1976, 2637 }, 0.05, 0.015, false, BELL, 4, { 1047, 1568, 2093 }, 3, 0.16, 0.5, false,
      BELL, 4, { 1319, 988 }, 5, 0.42, 0, 1047, 523 },
    /* Dark Souls: a stone tap, a bonfire's deep bell, the death toll */
    { BELL, 4, { 330, 349, 294, 392 }, 0.08, 0.03, false, BELL, 4, { 196, 294, 392 }, 3, 0.35, 1.0, true,
      BELL, 4, { 110, 104 }, 3, 0.9, 0, 220, 110 },
    /* Factorio: mechanical clunks, research done, the biters alarm */
    { METAL, 4, { 180, 200, 160, 220 }, 0.05, 0.02, false, METAL, 4, { 523, 659, 784, 1047 }, 4, 0.09, 0.25, false,
      SQ3, 3, { 520, 390 }, 6, 0.3, 0.4, 600, 120 },
    /* Windows 95: a click, the chord, the critical stop */
    { SINE1, 1, { 1200, 1200, 1200, 1200 }, 0.02, 0.006, false, GLASS, 4, { 523, 659, 784, 1047 }, 4, 0.0, 1.1, true,
      SQ3, 3, { 330, 247 }, 1, 0.5, 0, 880, 330 },
    /* Star Citizen: soft MFD blips, the quantum drive spooling up to a chord, the shields-down two-tone */
    { SINE1, 1, { 2637, 2794, 2489, 2960 }, 0.035, 0.012, false, GLASS, 4, { 587, 880, 1175, 1760 }, 4, 0.09, 0.7, false,
      SQ3, 3, { 880, 622 }, 6, 0.36, 0.2, 1400, 180 },
};

static Wave synth_pack(const Pack *P, Snd which, int variant, float gain)
{
    static const double total[SND__N] = { 0.12, 0.4, 0.4, 1.6, 3.0, 0.7 };
    int n = (int)(total[which] * RATE);
    float *buf = calloc((size_t)n, sizeof *buf);
    double peak = 0.7;
    switch (which) {
    case SND_CLICK:
        peak = 0.4;
        if (P->noiseClick) {                                   /* a Geiger tick: a burst of noise */
            unsigned r = 12345u + (unsigned)variant * 977u;
            int m = (int)(0.004 * RATE);
            for (int i = 0; i < m && i < n; i++) { r = r * 1103515245u + 12345u; buf[i] += (float)(((r >> 16) & 0x7FFF) / 16384.0 - 1.0) * (float)exp(-i / (0.001 * RATE)); }
        } else tone(buf, n, (Tone){ .dur = P->clickDur, .f0 = P->click[variant % 4], .f1 = P->click[variant % 4], .amp = 1, .attack = 0.002,
                                    .tau = P->clickTau, .release = 0.01, .parts = P->clickP, .np = P->clickNp });
        break;
    case SND_DENIED:
        tone(buf, n, (Tone){ .dur = 0.14, .f0 = P->alert[0], .f1 = P->alert[0], .amp = 1, .attack = 0.003, .tau = 0.08, .release = 0.02, .parts = P->alertP, .np = P->alertNp });
        tone(buf, n, (Tone){ .t0 = 0.16, .dur = 0.2, .f0 = P->alert[1], .f1 = P->alert[1], .amp = 1, .attack = 0.003, .tau = 0.1, .release = 0.02, .parts = P->alertP, .np = P->alertNp });
        break;
    case SND_ERROR:
        tone(buf, n, (Tone){ .dur = 0.3, .f0 = P->alert[0], .f1 = P->alert[1] * 0.8, .glide = 0.7, .amp = 1, .attack = 0.003, .tau = 0.15,
                             .release = 0.03, .parts = P->alertP, .np = P->alertNp });
        break;
    case SND_DONE:
        for (int k = 0; k < P->nDone; k++)
            tone(buf, n, (Tone){ .t0 = P->chord ? k * 0.012 : k * P->doneStep, .dur = 1.4 - (P->chord ? 0 : k * P->doneStep), .f0 = P->done[k],
                                 .f1 = P->done[k], .amp = k == P->nDone - 1 ? 1 : 0.8, .attack = 0.004, .tau = P->doneTau, .release = 0.06,
                                 .parts = P->doneP, .np = P->doneNp });
        break;
    case SND_ALERT:
        for (int k = 0; k < P->alertN; k++) {
            double t = k * P->alertGap;
            if (t > 2.8) break;
            tone(buf, n, (Tone){ .t0 = t, .dur = P->alertGap * 0.8, .f0 = P->alert[k % 2], .f1 = P->alert[k % 2], .amp = 1, .attack = 0.01,
                                 .tau = P->alertGap > 0.6 ? 0.6 : 0, .release = 0.04, .am_hz = P->alertAm > 0 ? 30 : 0, .am_depth = P->alertAm,
                                 .parts = P->alertP, .np = P->alertNp });
        }
        break;
    case SND_STOPPED:
        tone(buf, n, (Tone){ .dur = 0.6, .f0 = P->stop0, .f1 = P->stop1, .glide = 0.7, .amp = 1, .attack = 0.005, .tau = 0.3, .release = 0.06,
                             .parts = P->doneP, .np = P->doneNp });
        break;
    default: break;
    }
    return finish(buf, n, peak, gain);
}

static Wave synth(Snd which, int variant, float gain)
{
    if (S.pack >= SOUND_SEVERANCE) return synth_pack(&PACKS[S.pack - SOUND_SEVERANCE], which, variant, gain);
    return S.pack == SOUND_STARDEW ? synth_stardew(which, variant, gain)
         : S.pack == SOUND_POKEMON ? synth_pokemon(which, variant, gain)
         : S.pack == SOUND_JARVIS ? synth_jarvis(which, variant, gain) : synth_lcars(which, variant, gain);
}
static int synth_clicks(void) { return S.pack >= SOUND_SEVERANCE ? 4 : S.pack == SOUND_STARDEW ? 4 : S.pack == SOUND_POKEMON ? 3 : S.pack == SOUND_JARVIS ? 4 : CLICK_VARIANTS; }
static const char *const PACK_DIR[SOUND__N] = { "", "/stardew", "/pokemon", "/jarvis", "/severance", "/alien", "/pipboy", "/nerv",
                                                 "/portal", "/souls", "/factorio", "/win95", "/starcitizen" };
static const char *const PACK_NAME[SOUND__N] = { "lcars", "stardew", "pokemon", "jarvis", "severance", "alien", "pipboy", "nerv",
                                                  "portal", "souls", "factorio", "win95", "starcitizen" };

/* your own recordings override the synthesised cues: <config>/q-continuum/sounds/<name>.wav (or .ogg, .mp3), and
   click.wav, click-2.wav, ... for the keys (a different one each press) */
static bool user_file(Snd which, int variant, char *out, size_t cap)
{
    static const char *const EXT[] = { "wav", "ogg", "mp3" };
    const char *x = getenv("XDG_CONFIG_HOME");
    char dir[1024];
    if (x && *x) snprintf(dir, sizeof dir, "%s/q-continuum/sounds%s", x, PACK_DIR[S.pack]);
    else snprintf(dir, sizeof dir, "%s/.config/q-continuum/sounds%s", pl_home(), PACK_DIR[S.pack]);
    char suf[16] = "";
    if (variant) snprintf(suf, sizeof suf, "-%d", variant + 1);
    for (int e = 0; e < 3; e++) {
        snprintf(out, cap, "%s/%s%s.%s", dir, NAMES[which], suf, EXT[e]);
        if (access(out, R_OK) == 0) return true;
    }
    return false;
}

static int user_clicks(void)
{
    char p[1100];
    int n = 0;
    while (n < CLICK_MAX && user_file(SND_CLICK, n, p, sizeof p)) n++;
    return n;
}

static Wave cue_wave(Snd which, int variant, float gain)
{
    char p[1100];
    if (user_file(which, variant, p, sizeof p)) {
        Wave w = LoadWave(p);
        if (w.data && w.frameCount) {
            WaveFormat(&w, (int)w.sampleRate, 16, (int)w.channels);
            short *d = w.data;                         /* to one peak level: clips from different sources vary 10x */
            unsigned m = w.frameCount * w.channels;
            int mx = 0;
            for (unsigned i = 0; i < m; i++) mx = abs(d[i]) > mx ? abs(d[i]) : mx;
            float k = mx ? 0.75f * 32000 / mx * gain : gain;
            for (unsigned i = 0; i < m; i++) d[i] = (short)(d[i] * k);
            return w;
        }
        if (w.data) UnloadWave(w);
    }
    return synth(which, variant, gain);
}

static Sound make(Snd which, int variant)
{
    Wave w = cue_wave(which, variant, 1.0f);
    Sound s = LoadSoundFromWave(w);
    free(w.data);
    return s;
}

static int export_pack(const char *dir)
{
    int n = 0;
    for (int i = 0; i < SND__N; i++) {
        bool ok = true;
        int nv = i != SND_CLICK ? 1 : user_clicks() ? user_clicks() : synth_clicks();
        for (int v = 0; v < nv; v++) {
            Wave w = cue_wave((Snd)i, v, 1.0f);
            char p[1024];
            if (v) snprintf(p, sizeof p, "%s/%s-%s-%d.wav", dir, PACK_NAME[S.pack], NAMES[i], v + 1);
            else snprintf(p, sizeof p, "%s/%s-%s.wav", dir, PACK_NAME[S.pack], NAMES[i]);
            ok = ExportWave(w, p) && ok;
            free(w.data);
        }
        n += ok;
    }
    return n;
}

int sound_export(const char *dir)
{
    int keep = S.pack, n = 0;
    pl_mkdirs(dir);                                    /* the folder may not exist yet */
    for (S.pack = 0; S.pack < SOUND__N; S.pack++) n += export_pack(dir);
    S.pack = keep;
    return n;
}

static void quiet_stderr(int *saved)
{
    fflush(stderr);
    *saved = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, 2); close(dn); }
}
static void restore_stderr(int saved) { if (saved >= 0) { fflush(stderr); dup2(saved, 2); close(saved); } }

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static void load_pack(void)
{
    for (int i = 0; i < SND__N; i++) S.s[i] = make((Snd)i, 0);
    S.nClick = user_clicks();
    if (!S.nClick) S.nClick = synth_clicks();
    for (int v = 0; v < S.nClick; v++) S.click[v] = make(SND_CLICK, v);
    S.lastClick = 0;
}
static void unload_pack(void)
{
    for (int i = 0; i < SND__N; i++) UnloadSound(S.s[i]);
    for (int v = 0; v < S.nClick; v++) UnloadSound(S.click[v]);
}

/* never blocks: the first call starts the probe, later ones see how it went (cues before that stay silent) */
static bool ready(void)
{
    if (S.tried) return S.ok;
    if (S.probe == PR_NONE) {
        pid_t p = fork();
        if (p == 0) {
            int sv; quiet_stderr(&sv);
            SetTraceLogLevel(LOG_NONE);
            InitAudioDevice();
            _exit(IsAudioDeviceReady() ? 0 : 1);
        }
        if (p < 0) { S.tried = true; S.probe = PR_NOLIB; return false; }
        S.pid = p; S.t0 = mono(); S.probe = PR_RUNNING;
        return false;
    }
    if (S.probe == PR_RUNNING) {
        int st = 0;
        pid_t r = waitpid(S.pid, &st, WNOHANG);
        if (r == 0) {
            if (mono() - S.t0 < 3.0) return false;
            kill(S.pid, SIGKILL); waitpid(S.pid, NULL, 0);
            S.probe = PR_HUNG; S.tried = true;
            fprintf(stderr, "Q-Continuum: the sound server didn't answer in 3 s; LCARS sounds are off (see Settings > Appearance)\n");
            return false;
        }
        S.probe = r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? PR_OK : PR_NOLIB;
        S.tried = true;
        if (S.probe != PR_OK) { fprintf(stderr, "Q-Continuum: no sound device; LCARS key beeps are off (see Settings > Appearance)\n"); return false; }
        int sv; quiet_stderr(&sv);
        SetTraceLogLevel(LOG_WARNING);
        InitAudioDevice();
        restore_stderr(sv);
        if (!IsAudioDeviceReady()) { S.probe = PR_NOLIB; return false; }
        S.ok = true;
        load_pack();
        return true;
    }
    return false;
}


void sound_set_pack(int pack)
{
    if (pack < 0 || pack >= SOUND__N || pack == S.pack) return;
    if (S.ok) unload_pack();
    S.pack = pack;
    if (S.ok) load_pack();
}

/* ============================================================================ Windows players (WSL)
 * WSLg carries Linux audio to Windows over its RDP channel, which stutters, repeats buffers and echoes (worst after the
 * sink has been idle), whatever our buffer size. So on WSL the cues play in Windows itself: two PowerShell processes,
 * started once, read WAV paths on stdin and play them with Media.SoundPlayer (loaded once, ~150 ms; then a cue starts in under 10 ms).
 * One plays the keys, the other the run cues: a SoundPlayer cuts off its own previous sound, so the two can overlap.
 * The WAVs are written under ~/.cache/q-continuum/sounds and read by Windows as \\wsl$\<distro>\... (no slow /mnt/c
 * writes, no cmd.exe calls). QC_AUDIO=pulse keeps WSLg audio instead. */
#define PS_EXE "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
static struct { int fd[2]; pid_t pid[2]; double startedAt[2]; int nClick[SOUND__N]; bool counted[SOUND__N]; } W = { .fd = { -1, -1 } };

static bool win_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *a = getenv("QC_AUDIO");
        m = getenv("WSL_DISTRO_NAME") && access(PS_EXE, X_OK) == 0 && !(a && !strcmp(a, "pulse"));
    }
    return m == 1;
}

static void win_cache_dir(char *out, size_t cap)
{
    const char *x = getenv("XDG_CACHE_HOME");
    if (x && *x) snprintf(out, cap, "%s/q-continuum/sounds", x);
    else snprintf(out, cap, "%s/.cache/q-continuum/sounds", pl_home());
}

static bool win_channel(int ch)
{
    if (W.fd[ch] >= 0) {
        int st;
        if (waitpid(W.pid[ch], &st, WNOHANG) == 0) return true;   /* still there */
        close(W.fd[ch]); W.fd[ch] = -1;
    }
    if (mono() - W.startedAt[ch] < 5) return false;               /* it just died: not in a loop */
    W.startedAt[ch] = mono();
    int p[2];
    if (pipe(p) != 0) return false;
    /* QC_SOUND_DEBUG=<Windows path>: each cue played, or why not, goes to that file */
    const char *dbg = getenv("QC_SOUND_DEBUG");
    char log[600] = "";
    if (dbg && *dbg && !strchr(dbg, '\'')) snprintf(log, sizeof log, "Add-Content -LiteralPath '%s' -Value", dbg);
    char script[2600];
    snprintf(script, sizeof script,
        "$c = @{}; while (($l = [Console]::In.ReadLine()) -ne $null) { try { "
        "if (-not $c.ContainsKey($l)) { $s = New-Object Media.SoundPlayer (New-Object IO.MemoryStream (,[IO.File]::ReadAllBytes($l))); "
        "$s.Load(); $c[$l] = $s }; $c[$l].Play(); %s%s } catch { %s%s } }",   /* from memory: SoundPlayer won't take \\wsl$ paths */
        log[0] ? log : "", log[0] ? " \"played $l\"" : "", log[0] ? log : "", log[0] ? " \"ERROR $l : $_\"" : "");
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return false; }
    if (pid == 0) {
        dup2(p[0], 0); close(p[0]); close(p[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
        if (chdir("/mnt/c") != 0) _exit(1);           /* powershell warns about a UNC working directory otherwise */
        execl(PS_EXE, "powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script, (char *)NULL);   // NOLINT(clang-analyzer-optin.taint.GenericTaint): QC_SOUND_DEBUG is a developer path, quotes refused
        _exit(127);
    }
    close(p[0]);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    signal(SIGPIPE, SIG_IGN);                            /* a player that died: write() fails, the app lives */
    W.fd[ch] = p[1]; W.pid[ch] = pid;
    return true;
}

static void win_cue(Snd which, float volume)
{
    int ch = which == SND_DONE || which == SND_ALERT || which == SND_STOPPED ? 1 : 0;
    if (!win_channel(ch)) return;
    int v = 0;
    if (which == SND_CLICK) {
        if (!W.counted[S.pack]) { int u = user_clicks(); W.nClick[S.pack] = u ? u : synth_clicks(); W.counted[S.pack] = true; }
        int n = W.nClick[S.pack], k = n > 1 ? GetRandomValue(0, n - 2) : 0;
        v = S.lastClick = n > 1 && k >= S.lastClick ? k + 1 : k;
    }
    char dir[1024], file[1300], own[1100];
    win_cache_dir(dir, sizeof dir);
    struct stat os = { 0 };
    long tag = user_file(which, v, own, sizeof own) && stat(own, &os) == 0 ? (long)os.st_mtime : 0;   /* a new file of yours, a new copy */
    snprintf(file, sizeof file, "%s/%s-%s-%d-%d-%ld.wav", dir, PACK_NAME[S.pack], NAMES[which], v + 1, (int)(volume * 100 + 0.5f), tag);
    if (access(file, R_OK) != 0) {
        pl_mkdirs(dir);
        SetTraceLogLevel(LOG_WARNING);
        Wave w = cue_wave(which, v, volume);
        bool ok = ExportWave(w, file);
        free(w.data);
        if (!ok) return;
    }
    /* \\wsl$\<distro>\home\... */
    char line[1500];
    int k = snprintf(line, sizeof line, "\\\\wsl$\\%s", getenv("WSL_DISTRO_NAME"));
    for (const char *q = file; *q && k < (int)sizeof line - 2; q++) line[k++] = *q == '/' ? '\\' : *q;
    line[k++] = '\n'; line[k] = 0;
    if (write(W.fd[ch], line, (size_t)k) != k) { close(W.fd[ch]); W.fd[ch] = -1; }   /* full or gone: next cue restarts it */
}

static void win_stop(void)
{
    for (int ch = 0; ch < 2; ch++) if (W.fd[ch] >= 0) { close(W.fd[ch]); W.fd[ch] = -1; waitpid(W.pid[ch], NULL, WNOHANG); }   /* EOF ends its loop */
}

/* WSL: start the key player now, so the first key isn't silent while PowerShell starts; elsewhere the device probe */
void sound_prepare(void)
{
    if (win_mode()) { if (W.fd[0] < 0 && W.startedAt[0] == 0) win_channel(0); return; }
    if (!S.tried) ready();
}

static const char *test_log(void) { return getenv("QC_TEST_SOUND_LOG"); }

void sound_play(Snd which, bool enabled, float volume, bool testing)
{
    if (which < 0 || which >= SND__N || !enabled || volume <= 0) return;
    const char *log = test_log();
    if (log) {
        FILE *f = fopen(log, "a");
        if (f) { fprintf(f, "%s\n", NAMES[which]); fclose(f); }
        return;
    }
    if (testing) return;
    if (win_mode()) { win_cue(which, volume); return; }
    if (!ready()) return;                                  /* still finding out, or no device */
    Sound *s = &S.s[which];
    if (which == SND_CLICK) {                              /* a different chirp from the last one */
        int v = S.nClick > 1 ? GetRandomValue(0, S.nClick - 2) : 0;
        S.lastClick = S.nClick > 1 && v >= S.lastClick ? v + 1 : v;
        s = &S.click[S.lastClick];
    }
    SetSoundVolume(*s, volume);
    PlaySound(*s);
}

int sound_play_all(int pack, float volume)
{
    sound_set_pack(pack);
    for (int i = 0; i < 40 && !win_mode() && !ready(); i++) usleep(100000);   /* the device probe, elsewhere */
    int n = 0;
    for (int i = 0; i < SND__N; i++) {
        sound_play((Snd)i, true, volume, false);
        n++;
        usleep(i == SND_ALERT ? 2600000 : 1200000);
    }
    usleep(500000);
    sound_shutdown();
    return n;
}

int sound_device_state(void)
{
    if (win_mode()) return W.fd[0] >= 0 || W.fd[1] >= 0 ? 1 : 0;
    return !S.tried ? 0 : S.ok ? 1 : S.probe == PR_HUNG ? 3 : 2;
}

const char *sound_name(Snd which) { return which >= 0 && which < SND__N ? NAMES[which] : "?"; }

void sound_shutdown(void)
{
    win_stop();
    if (!S.ok) return;
    unload_pack();
    CloseAudioDevice();
    S.ok = false;
}

const char *sound_pack_dir(int pack) { return pack >= 0 && pack < SOUND__N ? PACK_DIR[pack] : ""; }
