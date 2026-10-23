#pragma once

#include <bleed_audio.h>
#include <stdint.h>
#include <stddef.h>

#define AU_DEFAULT_RATE 48000
#define AU_CHUNK        256
#define AU_SONGS        32
#define AU_CMD_RING     512

// what the program asks for
enum {
    CMD_SAMPLE = 1,
    CMD_PLAY,
    CMD_VOICE_SET,
    CMD_VOICE_STOP,
    CMD_STOP_ALL,
    CMD_SFX_VOLUME,
    CMD_MUSIC_VOLUME,
    CMD_MIDI,           // a=status b c
    CMD_SYNTH_RESET,
    CMD_BANK_DEFAULT,
    CMD_BANK_SF2,
    CMD_SONG_LOAD,
    CMD_SONG_FREE,
    CMD_SONG_PLAY, 
    CMD_SONG_STOP,
    CMD_SONG_PAUSE,
    CMD_STREAM_OPEN,
    CMD_STREAM_CLOSE,
    CMD_LATENCY,
    CMD_CLOSE,
};

typedef struct {
    uint32_t op;
    int32_t  a, b, c, d, e;
    uint64_t x;
} au_cmd_t;

// lives in shm once theres a worker
typedef struct {
    volatile uint32_t head;
    volatile uint32_t done;
    volatile int32_t  result;
    volatile int32_t  opened;
    volatile uint32_t song_playing;
    volatile uint32_t rate;
    volatile uint64_t stream_pos;
    volatile uint32_t queued;       // frames on the card at the last pump
    volatile uint8_t  voice_gen[AU_VOICES];
    au_cmd_t          ring[AU_CMD_RING];
} au_shared_t;

extern au_shared_t *au_shared;
extern uint32_t     au_out_rate;

// mixer.c
void eng_play(int slot, int gen, const au_sample_t *sample, int vol, int pan, int loop);
void eng_voice_set(int slot, int gen, int vol, int pan);
void eng_voice_stop(int slot, int gen);
void eng_stop_all(void);
void eng_sfx_volume(int vol);
void eng_music_volume(int vol);
void eng_stream_set(int16_t *ring, uint32_t frames);
void eng_render(int16_t *out, uint32_t frames);

// synth.c, adds into a stereo mix at full scale
int  eng_bank_load_sf2(const void *data, size_t len);
int  eng_bank_load_file(const char *path);
int  eng_bank_load_default(void);
void eng_synth_reset(void);
void eng_synth_midi(uint8_t status, uint8_t data1, uint8_t data2);
void synth_render(int32_t *mix, uint32_t frames);
void synth_rate_changed(void);

// midi.c
typedef struct song song_t;
song_t  *eng_song_load(const void *data, size_t len);
void     eng_song_free(song_t *song);
void     eng_song_play(song_t *song, int loop);
void     eng_song_stop(void);
void     eng_song_pause(int paused);
song_t  *eng_song_current(void);
uint32_t song_frames_to_event(uint32_t max);
void     song_advance(uint32_t frames);

// device.c
int  card_open(void);
void card_close(void);
void card_pump(void);
void card_wait(void);
void card_latency(uint32_t ms);
