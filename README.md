# libaudio

Sound for Bleed programs, on top of the kernel's `/dev/audio0`.

- a mixer: 32 sample voices, any rate, 8 or 16 bit, mono or stereo, resampled to the card
- general midi on 16 channels through a soundfont (.sf2), synthesised by [TinySoundFont](https://github.com/schellingb/TinySoundFont) (MIT, vendored in src/vendor)
- the first .sf2 in /initrd/soundfonts is the default bank, Bleed ships TimGM6mb there
- a midi file player, type 0 and 1
- `play`, a little command line player for .wav and .mid

Nothing runs by itself, theres no threads. Call `au_pump()` often, or `au_sleep_ms()` instead of sleeping,
and it renders enough to keep the card going till the next call. How far ahead it renders follows how
long the gaps between calls are, between `AU_LEAD_MIN_MS` and `AU_LEAD_MAX_MS`.

```c
#include <bleed_audio.h>

au_open();
au_bank_load_default();

au_song_t *song = au_song_load(mid, mid_len);
au_song_play(song, 1);

au_sample_t boom = { data, frames, 11025, AU_U8, 1 };
au_play(&boom, 127, 64, 0);

for (;;) {
    game_frame();
    au_pump();
}
```

## Building

```
make BLIBC_DIR=../blibc
make install        # copies bin/play into ../Bleed-Kernel/initrd/bin
```

Programs take `bin/libaudio.a` and `include/bleed_audio.h` the way they take libcrime, Bleed-Doom's
Makefile has an `audio` target that does it.
