/* sound.h -- the themes' sound cues (see sound.c) */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>

typedef enum { SND_CLICK, SND_DENIED, SND_ERROR, SND_DONE, SND_ALERT, SND_STOPPED, SND__N } Snd;
/* the sets of cues: one per theme that has sounds */
enum { SOUND_LCARS = 0, SOUND_STARDEW, SOUND_POKEMON, SOUND_JARVIS,
       SOUND_SEVERANCE, SOUND_ALIEN, SOUND_PIPBOY, SOUND_NERV, SOUND_PORTAL, SOUND_SOULS, SOUND_FACTORIO, SOUND_WIN95, SOUND_STARCITIZEN, SOUND__N };

/* plays the cue when enabled (the caller decides: LCARS theme and the setting) at volume 0..1; testing = a scripted
   or screenshot run, which stays silent unless QC_TEST_SOUND_LOG records the cues */
void        sound_play(Snd which, bool enabled, float volume, bool testing);
const char *sound_name(Snd which);
int         sound_device_state(void);   /* 0 not opened yet, 1 playing, 2 no device (no libpulse), 3 the sound server doesn't answer */
void        sound_prepare(void);        /* start (or follow) the device probe in the background: call each frame while sounds are on */
void        sound_set_pack(int pack);  /* SOUND_*: the current theme's cues (reloaded when it changes) */
int         sound_play_all(int pack, float volume);   /* every cue of the pack for real, a second apart (--play-sounds) */
void        sound_shutdown(void);
const char *sound_pack_dir(int pack);    /* "/alien": where the pack's own files go under ~/.config/q-continuum/sounds */
/* writes <dir>/<pack>-<name>.wav for each cue as it plays (your own files or the synthesised ones), every pack; returns how many */
int         sound_export(const char *dir);

#endif
