#pragma once

#include <stdint.h>
#include <stddef.h>


#define AU_VOICES       32
#define AU_SOUNDFONT_DIR "/initrd/soundfonts"
#define AU_LEAD_MIN_MS  40
#define AU_LEAD_MAX_MS  150
#define AU_LEAD_XRUN_MS 10

#define AU_U8   1
#define AU_S16  2

typedef struct {
    const void *data;
    uint32_t    frames;
    uint32_t    rate;
    uint8_t     format;         // AU_U8 or AU_S16
    uint8_t     channels;       // 1 or 2
} au_sample_t;

typedef struct au_song au_song_t;

typedef struct {
    int16_t  *ring;
    uint32_t  frames;
} au_stream_t;

int  au_open(void);
void au_close(void);
int  au_is_open(void);
uint32_t au_rate(void);

void au_latency(uint32_t ms);

// render up to the lead, cheap to call when theres nothing to do
void au_pump(void);

// sleep ms without letting the card run dry
void au_sleep_ms(uint32_t ms);

// render frames of s16 stereo by hand
void au_render(int16_t *out, uint32_t frames);

// sample voices. vol 0-127, pan 0-127 with 64 the middle
int  au_play(const au_sample_t *sample, int vol, int pan, int loop);
void au_voice_set(int voice, int vol, int pan);
void au_voice_stop(int voice);
int  au_voice_playing(int voice);
void au_stop_all(void);

// general midi on 16 channels through a soundfont .sf2
int  au_bank_load_sf2(const void *data, size_t len);
int  au_bank_load_file(const char *path);
int  au_bank_load_default(void);
void au_synth_reset(void);
void au_synth_midi(uint8_t status, uint8_t data1, uint8_t data2);

// standard midi files, type 0 and 1. the data is copied
au_song_t *au_song_load(const void *data, size_t len);
void au_song_free(au_song_t *song);
void au_song_play(au_song_t *song, int loop);
void au_song_stop(void);
void au_song_pause(int paused);
int  au_song_playing(void);

// one stream at a time, 0 or -1. pos is frames the card has taken from itß
int      au_stream_open(uint32_t frames, au_stream_t *out);
uint64_t au_stream_pos(void);
uint64_t au_stream_heard(void);     // pos minus whats still queued on the card
void     au_stream_close(void);

void au_music_volume(int vol);
void au_sfx_volume(int vol);

// raw .wav (pcm 8/16 bit) into a sample pointing inside data
int  au_wav_parse(const void *data, size_t len, au_sample_t *out);
