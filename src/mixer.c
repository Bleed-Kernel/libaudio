#include "audio_priv.h"

#include <string.h>

typedef struct {
    const uint8_t *data;
    uint32_t       frames;
    uint8_t        format;
    uint8_t        channels;
    uint8_t        active;
    uint8_t        loop;
    uint8_t        gen;
    uint64_t       pos;
    uint64_t       step;
    int32_t        vol_l;
    int32_t        vol_r;
} voice_t;

uint32_t au_out_rate = AU_DEFAULT_RATE;

static voice_t  voices[AU_VOICES];
static int32_t  sfx_gain   = 32767;
static int32_t  music_gain = 32767;

static int16_t *stream;
static uint32_t stream_frames;
static uint64_t stream_pos;

// a soundfont mix goes past full scale on a loud bar
#define MUSIC_HEADROOM 16384

static void voice_levels(voice_t *v, int vol, int pan) {
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    if (pan < 0)   pan = 0;
    if (pan > 127) pan = 127;

    int32_t l = pan <= 64 ? 32767 : (127 - pan) * 32767 / 63;
    int32_t r = pan >= 64 ? 32767 : pan * 32767 / 64;
    v->vol_l = l * vol / 127;
    v->vol_r = r * vol / 127;
}

static voice_t *voice_at(int slot, int gen) {
    if (slot < 0 || slot >= AU_VOICES)
        return NULL;
    voice_t *v = &voices[slot];
    return v->active && v->gen == gen ? v : NULL;
}

static void voice_end(voice_t *v) {
    v->active = 0;
    au_shared->voice_gen[v - voices] = 0;
}

// the program picked the slot, whatever was in it goes
void eng_play(int slot, int gen, const au_sample_t *sample, int vol, int pan, int loop) {
    if (slot < 0 || slot >= AU_VOICES || !sample || !sample->data || !sample->frames || !sample->rate)
        return;

    voice_t *v  = &voices[slot];
    v->data     = sample->data;
    v->frames   = sample->frames;
    v->format   = sample->format;
    v->channels = sample->channels;
    v->loop     = loop ? 1 : 0;
    v->gen      = (uint8_t)gen;
    v->pos      = 0;
    v->step     = ((uint64_t)sample->rate << 32) / au_out_rate;
    voice_levels(v, vol, pan);
    v->active   = 1;
    au_shared->voice_gen[slot] = (uint8_t)gen;
}

void eng_voice_set(int slot, int gen, int vol, int pan) {
    voice_t *v = voice_at(slot, gen);
    if (v)
        voice_levels(v, vol, pan);
}

void eng_voice_stop(int slot, int gen) {
    voice_t *v = voice_at(slot, gen);
    if (v)
        voice_end(v);
}

void eng_stop_all(void) {
    for (int i = 0; i < AU_VOICES; i++)
        voice_end(&voices[i]);
}

static int32_t volume_gain(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    return vol * 32767 / 127;
}

void eng_sfx_volume(int vol) {
    sfx_gain = volume_gain(vol);
}

void eng_music_volume(int vol) {
    music_gain = volume_gain(vol);
}

static inline int32_t sample_at(const voice_t *v, uint32_t frame, int ch) {
    uint32_t i = frame * v->channels + (v->channels == 2 ? ch : 0);
    if (v->format == AU_U8)
        return ((int32_t)v->data[i] - 128) << 8;
    return ((const int16_t *)v->data)[i];
}

static void mixer_render_voices(int32_t *mix, uint32_t frames) {
    for (int n = 0; n < AU_VOICES; n++) {
        voice_t *v = &voices[n];
        if (!v->active)
            continue;

        int32_t l = (int32_t)(((int64_t)v->vol_l * sfx_gain) >> 15);
        int32_t r = (int32_t)(((int64_t)v->vol_r * sfx_gain) >> 15);

        for (uint32_t i = 0; i < frames; i++) {
            uint32_t at = (uint32_t)(v->pos >> 32);
            if (at >= v->frames) {
                if (!v->loop) {
                    voice_end(v);
                    break;
                }
                v->pos -= (uint64_t)v->frames << 32;
                at = (uint32_t)(v->pos >> 32);
            }

            // linear between this frame and the next, the last one just holds
            uint32_t next = at + 1 < v->frames ? at + 1 : (v->loop ? 0 : at);
            int32_t  frac = (int32_t)((v->pos >> 17) & 0x7FFF);

            int32_t a = sample_at(v, at, 0);
            int32_t b = sample_at(v, next, 0);
            int32_t sl = a + (((b - a) * frac) >> 15);
            int32_t sr = sl;
            if (v->channels == 2) {
                a  = sample_at(v, at, 1);
                b  = sample_at(v, next, 1);
                sr = a + (((b - a) * frac) >> 15);
            }

            mix[i * 2]     += (sl * l) >> 15;
            mix[i * 2 + 1] += (sr * r) >> 15;
            v->pos += v->step;
        }
    }
}

void eng_stream_set(int16_t *ring, uint32_t frames) {
    stream        = ring;
    stream_frames = ring ? frames : 0;
    stream_pos    = 0;
    au_shared->stream_pos = 0;
}

// taken straight, the program did its own mixing. zeroed behind us so a stall is quiet
static void mixer_render_stream(int32_t *mix, uint32_t frames) {
    if (!stream)
        return;

    for (uint32_t i = 0; i < frames; i++) {
        int16_t *at = stream + (stream_pos % stream_frames) * 2;
        mix[i * 2]     += at[0];
        mix[i * 2 + 1] += at[1];
        at[0] = at[1] = 0;
        stream_pos++;
    }
    au_shared->stream_pos = stream_pos;
}

void eng_render(int16_t *out, uint32_t frames) {
    int32_t sfx[AU_CHUNK * 2];
    int32_t music[AU_CHUNK * 2];

    while (frames) {
        uint32_t n = frames < AU_CHUNK ? frames : AU_CHUNK;

        // stop short of the next midi event so it lands on the right sample
        n = song_frames_to_event(n);
        if (n) {
            memset(sfx, 0, n * 2 * sizeof(int32_t));
            memset(music, 0, n * 2 * sizeof(int32_t));
            mixer_render_voices(sfx, n);
            mixer_render_stream(sfx, n);
            synth_render(music, n);

            int32_t mg = (int32_t)(((int64_t)music_gain * MUSIC_HEADROOM) >> 15);
            for (uint32_t i = 0; i < n * 2; i++) {
                int32_t s = sfx[i] + (int32_t)(((int64_t)music[i] * mg) >> 15);
                if (s > 32767)  s = 32767;
                if (s < -32768) s = -32768;
                out[i] = (int16_t)s;
            }
            out    += n * 2;
            frames -= n;
        }
        song_advance(n);
    }
}
