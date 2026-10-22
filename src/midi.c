#include "audio_priv.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const uint8_t *pos;
    uint64_t       next_tick;
    uint8_t        running;
    uint8_t        done;
} track_t;

struct song {
    uint8_t  *data;
    uint16_t  division;
    int       ntracks;
    track_t  *tracks;
    uint32_t  tempo;
    uint64_t  tick;
    double    wait;
    int       loop;
};

static song_t *current;
static int        paused;

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t read_var(track_t *t) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && t->pos < t->end; i++) {
        uint8_t b = *t->pos++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return v;
    }
    return v;
}

static double frames_per_tick(song_t *s) {
    if (s->division & 0x8000) {
        int fps = -(int8_t)(s->division >> 8);
        int tpf = s->division & 0xFF;
        return fps && tpf ? (double)au_out_rate / (fps * tpf) : 0;
    }
    return (double)s->tempo * au_out_rate / (1000000.0 * (s->division ? s->division : 96));
}

static void song_rewind(song_t *s) {
    s->tempo = 500000;
    s->tick  = 0;
    for (int i = 0; i < s->ntracks; i++) {
        track_t *t   = &s->tracks[i];
        t->pos       = t->start;
        t->running   = 0;
        t->done      = t->pos >= t->end;
        t->next_tick = t->done ? 0 : read_var(t);
    }
}

// earliest tick any track 
static int song_next_tick(song_t *s, uint64_t *out) {
    int found = 0;
    for (int i = 0; i < s->ntracks; i++) {
        track_t *t = &s->tracks[i];
        if (t->done)
            continue;
        if (!found || t->next_tick < *out)
            *out = t->next_tick;
        found = 1;
    }
    return found;
}

static void track_event(song_t *s, track_t *t) {
    uint8_t status = *t->pos;
    if (status & 0x80)
        t->pos++;
    else
        status = t->running;

    if (status == 0xFF) {
        if (t->pos + 1 > t->end) {
            t->done = 1;
            return;
        }
        uint8_t  type = *t->pos++;
        uint32_t len  = read_var(t);
        if (t->pos + len > t->end) {
            t->done = 1;
            return;
        }
        if (type == 0x51 && len == 3) {
            uint32_t tempo = ((uint32_t)t->pos[0] << 16) | (t->pos[1] << 8) | t->pos[2];
            if (tempo)
                s->tempo = tempo;
        }
        if (type == 0x2F)
            t->done = 1;
        t->pos += len;
        return;
    }

    if (status == 0xF0 || status == 0xF7) {
        uint32_t len = read_var(t);
        t->pos = t->pos + len > t->end ? t->end : t->pos + len;
        return;
    }

    if (!(status & 0x80)) {
        t->done = 1;
        return;
    }

    t->running = status;
    int     two = (status & 0xF0) != 0xC0 && (status & 0xF0) != 0xD0;
    uint8_t d1  = t->pos < t->end ? *t->pos++ : 0;
    uint8_t d2  = two && t->pos < t->end ? *t->pos++ : 0;
    eng_synth_midi(status, d1, d2);
}

static void all_notes_off(void) {
    for (uint8_t ch = 0; ch < 16; ch++)
        eng_synth_midi(0xB0 | ch, 123, 0);
}

song_t *eng_song_load(const void *data, size_t len) {
    const uint8_t *p = data;
    if (!p || len < 14 || memcmp(p, "MThd", 4) != 0 || be32(p + 4) < 6)
        return NULL;

    song_t *s = calloc(1, sizeof(song_t));
    if (!s)
        return NULL;
    s->data = malloc(len);
    if (!s->data) {
        free(s);
        return NULL;
    }
    memcpy(s->data, data, len);

    uint32_t hlen   = be32(s->data + 4);
    uint16_t ntrks  = be16(s->data + 10);
    s->division     = be16(s->data + 12);
    s->tracks       = calloc(ntrks ? ntrks : 1, sizeof(track_t));
    if (!s->tracks) {
        eng_song_free(s);
        return NULL;
    }

    // anything that isnt MTrk gets skipped, the spec says to
    const uint8_t *at  = s->data + 8 + hlen;
    const uint8_t *end = s->data + len;
    while (at + 8 <= end && s->ntracks < ntrks) {
        uint32_t clen = be32(at + 4);
        const uint8_t *body = at + 8;
        if (clen > (size_t)(end - body))
            clen = (uint32_t)(end - body);

        if (!memcmp(at, "MTrk", 4)) {
            s->tracks[s->ntracks].start = body;
            s->tracks[s->ntracks].end   = body + clen;
            s->ntracks++;
        }
        at = body + clen;
    }

    song_rewind(s);
    if (!s->ntracks || frames_per_tick(s) <= 0) {
        eng_song_free(s);
        return NULL;
    }
    return s;
}

void eng_song_free(song_t *song) {
    if (!song)
        return;
    if (song == current)
        eng_song_stop();
    free(song->tracks);
    free(song->data);
    free(song);
}

void eng_song_play(song_t *song, int loop) {
    if (current)
        eng_song_stop();
    if (!song)
        return;

    eng_synth_reset();
    song_rewind(song);
    song->loop = loop;
    song->wait = 0;
    current    = song;
    paused  = 0;
}

void eng_song_stop(void) {
    if (!current)
        return;
    current = NULL;
    all_notes_off();
}

void eng_song_pause(int p) {
    if (p && !paused)
        all_notes_off();
    paused = p ? 1 : 0;
}

song_t *eng_song_current(void) {
    return current;
}

uint32_t song_frames_to_event(uint32_t max) {
    if (!current || paused)
        return max;
    if (current->wait <= 0)
        return 0;

    double n = current->wait;
    if (n >= max)
        return max;
    uint32_t whole = (uint32_t)n;
    return whole < n ? whole + 1 : whole;
}

void song_advance(uint32_t frames) {
    song_t *s = current;
    if (!s || paused)
        return;

    s->wait -= frames;
    while (current == s && s->wait <= 0) {
        for (int i = 0; i < s->ntracks; i++) {
            track_t *t = &s->tracks[i];
            while (!t->done && t->next_tick == s->tick) {
                if (t->pos >= t->end) {
                    t->done = 1;
                    break;
                }
                track_event(s, t);
                if (!t->done && t->pos < t->end)
                    t->next_tick += read_var(t);
                else
                    t->done = 1;
            }
        }

        uint64_t next;
        if (!song_next_tick(s, &next)) {
            if (!s->loop || !s->tick) {
                eng_song_stop();
                return;
            }
            all_notes_off();
            song_rewind(s);
            if (!song_next_tick(s, &next)) {
                eng_song_stop();
                return;
            }
        }

        s->wait += (double)(next - s->tick) * frames_per_tick(s);
        s->tick  = next;
    }
}
