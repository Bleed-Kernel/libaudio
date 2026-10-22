#include "audio_priv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bleed/syscalls.h>

#define PAGE_BYTES 4096

// TODO these belong in blibc, libcrime has its own copies
static long sys3(long n, long a, long b, long c) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}
static long sys_poll(long ms)                 { return sys3(7, 0, 0, ms); }
static long sys_shm_create(uint64_t pages)    { return sys3(522, (long)pages, 0, 0); }
static long sys_shm_map(long id)              { return sys3(523, id, 0, 0); }
static long sys_shm_destroy(long id)          { return sys3(524, id, 0, 0); }

static au_shared_t local_shared;
au_shared_t       *au_shared = &local_shared;

static long worker;
static long shared_shm;
static int  in_worker;
static int  closing;
static long parent_pid;

static uint8_t  slot_gen[AU_VOICES];
static uint32_t slot_seq[AU_VOICES];    // the play that last went into the slot
static uint32_t slot_age[AU_VOICES];
static uint32_t age_count;

// samples already handed over, found again by what the program passed
typedef struct {
    const void *data;
    uint32_t    frames;
    uint32_t    rate;
    uint8_t     format;
    uint8_t     channels;
} sample_key_t;

static sample_key_t *sent_samples;
static int           sent_count;
static int           sent_cap;

struct au_song {
    int id;
};
static uint8_t  song_used[AU_SONGS];
static uint32_t song_seq;   // last play or stop
static int      song_last;  // what that left playing

// the programs stream, its shm and mapping so it can be let go
static void    *stream_ring;
static long     stream_shm = -1;

// the workers side
static void        *stream_map;
static au_sample_t *samples;
static int          samples_cap;
static song_t      *songs[AU_SONGS];
static int          current_song;

static int seq_pending(uint32_t seq) {
    return (int32_t)(seq - au_shared->done) > 0;
}

static size_t pages_for(size_t bytes) {
    return (bytes + PAGE_BYTES - 1) / PAGE_BYTES;
}

static long shm_copy(const void *data, size_t len) {
    long id = sys_shm_create(pages_for(len));
    if (id < 0)
        return id;

    long at = sys_shm_map(id);
    if (at < 0) {
        sys_shm_destroy(id);
        return at;
    }
    memcpy((void *)at, data, len);
    _munmap((void *)at);
    return id;
}

static void *cmd_data(const au_cmd_t *c) {
    if (!in_worker)
        return (void *)(uintptr_t)c->x;
    long at = sys_shm_map((long)c->x);
    return at < 0 ? NULL : (void *)at;
}

static void cmd_data_done(void *data) {
    if (in_worker && data)
        _munmap(data);
}

static void publish_song(void) {
    au_shared->song_playing = eng_song_current() ? (uint32_t)current_song : 0;
}

static void apply(const au_cmd_t *c) {
    void *data;

    switch (c->op) {
        case CMD_SAMPLE:
            if (c->a >= samples_cap) {
                int cap = samples_cap ? samples_cap * 2 : 64;
                while (cap <= c->a)
                    cap *= 2;
                au_sample_t *bigger = realloc(samples, cap * sizeof(au_sample_t));
                if (!bigger)
                    break;
                memset(bigger + samples_cap, 0, (cap - samples_cap) * sizeof(au_sample_t));
                samples     = bigger;
                samples_cap = cap;
            }
            samples[c->a].data     = cmd_data(c);
            samples[c->a].frames   = (uint32_t)c->b;
            samples[c->a].rate     = (uint32_t)c->c;
            samples[c->a].format   = (uint8_t)c->d;
            samples[c->a].channels = (uint8_t)c->e;
            break;
        case CMD_PLAY:
            if (c->x < (uint64_t)samples_cap && samples[c->x].data)
                eng_play(c->a, c->b, &samples[c->x], c->c, c->d, c->e);
            break;
        case CMD_VOICE_SET:
            eng_voice_set(c->a, c->b, c->c, c->d);
            break;
        case CMD_VOICE_STOP:
            eng_voice_stop(c->a, c->b);
            break;
        case CMD_STOP_ALL:
            eng_stop_all();
            break;
        case CMD_SFX_VOLUME:
            eng_sfx_volume(c->a);
            break;
        case CMD_MUSIC_VOLUME:
            eng_music_volume(c->a);
            break;
        case CMD_MIDI:
            eng_synth_midi((uint8_t)c->a, (uint8_t)c->b, (uint8_t)c->c);
            break;
        case CMD_SYNTH_RESET:
            eng_synth_reset();
            break;
        case CMD_BANK_DEFAULT:
            au_shared->result = eng_bank_load_default();
            break;
        case CMD_BANK_SF2:
            data = cmd_data(c);
            au_shared->result = data ? eng_bank_load_sf2(data, (size_t)c->b) : -1;
            cmd_data_done(data);
            break;
        case CMD_SONG_LOAD:
            data = cmd_data(c);
            if (c->a > 0 && c->a < AU_SONGS && data)
                songs[c->a] = eng_song_load(data, (size_t)c->b);
            au_shared->result = c->a > 0 && c->a < AU_SONGS && songs[c->a] ? 0 : -1;
            cmd_data_done(data);
            break;
        case CMD_SONG_FREE:
            if (c->a > 0 && c->a < AU_SONGS && songs[c->a]) {
                eng_song_free(songs[c->a]);
                songs[c->a] = NULL;
            }
            break;
        case CMD_SONG_PLAY:
            if (c->a > 0 && c->a < AU_SONGS && songs[c->a]) {
                eng_song_play(songs[c->a], c->b);
                current_song = c->a;
            }
            break;
        case CMD_SONG_STOP:
            eng_song_stop();
            break;
        case CMD_SONG_PAUSE:
            eng_song_pause(c->a);
            break;
        case CMD_STREAM_OPEN:
            cmd_data_done(stream_map);
            stream_map = cmd_data(c);
            eng_stream_set(stream_map, (uint32_t)c->a);
            au_shared->result = stream_map ? 0 : -1;
            break;
        case CMD_STREAM_CLOSE:
            eng_stream_set(NULL, 0);
            cmd_data_done(stream_map);
            stream_map = NULL;
            break;
        case CMD_CLOSE:
            closing = 1;
            break;
        default:
            break;
    }
    publish_song();
}

// back to running in here, the worker is gone or going
static void drop_worker(void) {
    au_shared_t *shared = au_shared;

    _waitpid(worker);
    worker = 0;
    local_shared = *shared;
    au_shared    = &local_shared;
    _munmap(shared);
    sys_shm_destroy(shared_shm);
}

// killed under us
static int worker_lost(void) {
    if (!worker || _kill(worker, 0) == 0)
        return 0;

    drop_worker();
    memset((void *)local_shared.voice_gen, 0, sizeof(local_shared.voice_gen));
    local_shared.song_playing = 0;
    local_shared.done         = local_shared.head;
    return 1;
}

static uint32_t send(uint32_t op, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, uint64_t x) {
    au_cmd_t cmd = { op, a, b, c, d, e, x };
    worker_lost();
    uint32_t head = au_shared->head;

    if (!worker) {
        apply(&cmd);
        au_shared->head = head + 1;
        au_shared->done = head + 1;
        return head + 1;
    }

    // full means the worker is way behind, it drains a ring every period so this is rare
    while (head - au_shared->done >= AU_CMD_RING)
        sys_poll(1);

    au_shared->ring[head % AU_CMD_RING] = cmd;
    __atomic_store_n(&au_shared->head, head + 1, __ATOMIC_RELEASE);
    return head + 1;
}

// for the few calls that return something
static int wait_result(uint32_t seq) {
    while (seq_pending(seq)) {
        if (worker_lost())
            return -1;
        sys_poll(1);
    }
    return au_shared->result;
}

// terminals hand ^C to the newest task, thats us and not the program. pass it on
static void worker_forward(int sig) {
    _kill(parent_pid, sig);
}

static void worker_main(long parent) {
    in_worker  = 1;
    parent_pid = parent;
    signal(SIGINT, worker_forward);

    int r = card_open();
    au_shared->rate   = au_out_rate;
    au_shared->opened = r < 0 ? r : 1;
    if (r < 0)
        _exit(0);

    while (!closing && _kill(parent, 0) == 0) {
        uint32_t head = __atomic_load_n(&au_shared->head, __ATOMIC_ACQUIRE);
        uint32_t done = au_shared->done;
        while (done != head && !closing) {
            apply(&au_shared->ring[done % AU_CMD_RING]);
            __atomic_store_n(&au_shared->done, ++done, __ATOMIC_RELEASE);
        }

        card_pump();
        publish_song();
        card_wait();
    }

    card_close();
    _exit(0);
}

int au_open(void) {
    if (worker)
        return 0;

    long id = sys_shm_create(pages_for(sizeof(au_shared_t)));
    if (id < 0)
        return (int)id;
    long at = sys_shm_map(id);
    if (at < 0) {
        sys_shm_destroy(id);
        return (int)at;
    }

    au_shared_t *shared = (au_shared_t *)at;
    *shared   = local_shared;
    shared->opened = 0;
    au_shared = shared;

    long parent = _getpid();
    long pid    = _fork();
    if (pid == 0)
        worker_main(parent);

    if (pid > 0) {
        while (!shared->opened && _kill(pid, 0) == 0)
            sys_poll(1);
        if (shared->opened > 0) {
            worker     = pid;
            shared_shm = id;
            return 0;
        }
        _waitpid(pid);
    }

    int why   = pid < 0 ? (int)pid : (shared->opened < 0 ? shared->opened : -1);
    au_shared = &local_shared;
    _munmap(shared);
    sys_shm_destroy(id);
    return why;
}

void au_close(void) {
    if (!worker)
        return;

    send(CMD_CLOSE, 0, 0, 0, 0, 0, 0);
    if (worker)
        drop_worker();
}

int au_is_open(void) {
    return worker != 0;
}

uint32_t au_rate(void) {
    return worker ? au_shared->rate : au_out_rate;
}

void au_pump(void) {
}

void au_sleep_ms(uint32_t ms) {
    sys_poll(ms);
}

void au_render(int16_t *out, uint32_t frames) {
    if (worker) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    eng_render(out, frames);
    publish_song();
}

static int sample_id(const au_sample_t *s) {
    for (int i = 0; i < sent_count; i++) {
        sample_key_t *k = &sent_samples[i];
        if (k->data == s->data && k->frames == s->frames && k->rate == s->rate &&
            k->format == s->format && k->channels == s->channels)
            return i;
    }

    if (sent_count == sent_cap) {
        int           cap    = sent_cap ? sent_cap * 2 : 64;
        sample_key_t *bigger = realloc(sent_samples, cap * sizeof(sample_key_t));
        if (!bigger)
            return -1;
        sent_samples = bigger;
        sent_cap     = cap;
    }

    // the worker cant see our memory
    uint64_t x = (uint64_t)(uintptr_t)s->data;
    if (worker) {
        size_t bytes = (size_t)s->frames * s->channels * (s->format == AU_S16 ? 2 : 1);
        long   shm   = shm_copy(s->data, bytes);
        if (shm < 0)
            return -1;
        x = (uint64_t)shm;
    }

    int id = sent_count++;
    sent_samples[id] = (sample_key_t){ s->data, s->frames, s->rate, s->format, s->channels };
    send(CMD_SAMPLE, id, (int32_t)s->frames, (int32_t)s->rate, s->format, s->channels, x);
    return id;
}

int au_play(const au_sample_t *sample, int vol, int pan, int loop) {
    if (!sample || !sample->data || !sample->frames || !sample->rate)
        return -1;
    if ((sample->format != AU_U8 && sample->format != AU_S16) || (sample->channels != 1 && sample->channels != 2))
        return -1;

    int id = sample_id(sample);
    if (id < 0)
        return -1;

    // a free slot
    int pick = 0;
    for (int i = 0; i < AU_VOICES; i++) {
        if (!au_shared->voice_gen[i] && !seq_pending(slot_seq[i])) {
            pick = i;
            break;
        }
        if (slot_age[i] < slot_age[pick])
            pick = i;
    }

    uint8_t gen     = (uint8_t)(slot_gen[pick] % 255 + 1);
    slot_gen[pick]  = gen;
    slot_age[pick]  = ++age_count;
    slot_seq[pick]  = send(CMD_PLAY, pick, gen, vol, pan, loop, (uint64_t)id);
    return pick | (gen << 8);
}

static int voice_slot(int voice) {
    if (voice < 0)
        return -1;
    int slot = voice & 0xFF;
    if (slot >= AU_VOICES || slot_gen[slot] != ((voice >> 8) & 0xFF))
        return -1;
    return slot;
}

void au_voice_set(int voice, int vol, int pan) {
    int slot = voice_slot(voice);
    if (slot >= 0)
        send(CMD_VOICE_SET, slot, slot_gen[slot], vol, pan, 0, 0);
}

void au_voice_stop(int voice) {
    int slot = voice_slot(voice);
    if (slot >= 0)
        send(CMD_VOICE_STOP, slot, slot_gen[slot], 0, 0, 0, 0);
}

int au_voice_playing(int voice) {
    worker_lost();
    int slot = voice_slot(voice);
    if (slot < 0)
        return 0;
    // not got to the worker yet still counts, a program checking straight after playing
    // would think it finished instantly
    if (seq_pending(slot_seq[slot]))
        return 1;
    return au_shared->voice_gen[slot] == slot_gen[slot];
}

void au_stop_all(void) {
    send(CMD_STOP_ALL, 0, 0, 0, 0, 0, 0);
}

void au_sfx_volume(int vol) {
    send(CMD_SFX_VOLUME, vol, 0, 0, 0, 0, 0);
}

void au_music_volume(int vol) {
    send(CMD_MUSIC_VOLUME, vol, 0, 0, 0, 0, 0);
}

void au_synth_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    send(CMD_MIDI, status, data1, data2, 0, 0, 0);
}

void au_synth_reset(void) {
    send(CMD_SYNTH_RESET, 0, 0, 0, 0, 0, 0);
}

int au_bank_load_default(void) {
    return wait_result(send(CMD_BANK_DEFAULT, 0, 0, 0, 0, 0, 0));
}

int au_bank_load_sf2(const void *data, size_t len) {
    if (!data || !len || len > 0x7FFFFFFF)
        return -1;

    uint64_t x   = (uint64_t)(uintptr_t)data;
    long     shm = -1;
    if (worker) {
        shm = shm_copy(data, len);
        if (shm < 0)
            return -1;
        x = (uint64_t)shm;
    }

    int r = wait_result(send(CMD_BANK_SF2, 0, (int32_t)len, 0, 0, 0, x));
    if (shm >= 0)
        sys_shm_destroy(shm);
    return r;
}

int au_bank_load_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    fseek(fp, 0, 2);
    long len = ftell(fp);
    fseek(fp, 0, 0);

    void *data = len > 0 ? malloc((size_t)len) : NULL;
    int   r    = -1;
    if (data && fread(data, 1, (size_t)len, fp) == (size_t)len)
        r = au_bank_load_sf2(data, (size_t)len);

    free(data);
    fclose(fp);
    return r;
}

au_song_t *au_song_load(const void *data, size_t len) {
    if (!data || len < 14 || len > 0x7FFFFFFF)
        return NULL;

    int id = 1;
    while (id < AU_SONGS && song_used[id])
        id++;
    if (id == AU_SONGS)
        return NULL;

    au_song_t *song = malloc(sizeof(au_song_t));
    if (!song)
        return NULL;

    uint64_t x   = (uint64_t)(uintptr_t)data;
    long     shm = -1;
    if (worker) {
        shm = shm_copy(data, len);
        if (shm < 0) {
            free(song);
            return NULL;
        }
        x = (uint64_t)shm;
    }

    // the worker parses its own copy, ours goes as soon as its done
    int r = wait_result(send(CMD_SONG_LOAD, id, (int32_t)len, 0, 0, 0, x));
    if (shm >= 0)
        sys_shm_destroy(shm);
    if (r < 0) {
        free(song);
        return NULL;
    }

    song_used[id] = 1;
    song->id      = id;
    return song;
}

void au_song_free(au_song_t *song) {
    if (!song)
        return;
    if (song_last == song->id)
        song_last = 0;
    send(CMD_SONG_FREE, song->id, 0, 0, 0, 0, 0);
    song_used[song->id] = 0;
    free(song);
}

void au_song_play(au_song_t *song, int loop) {
    if (!song)
        return;
    song_seq  = send(CMD_SONG_PLAY, song->id, loop, 0, 0, 0, 0);
    song_last = song->id;
}

void au_song_stop(void) {
    song_seq  = send(CMD_SONG_STOP, 0, 0, 0, 0, 0, 0);
    song_last = 0;
}

void au_song_pause(int paused) {
    send(CMD_SONG_PAUSE, paused, 0, 0, 0, 0, 0);
}

int au_song_playing(void) {
    worker_lost();
    if (seq_pending(song_seq))
        return song_last != 0;
    return au_shared->song_playing != 0;
}

int au_stream_open(uint32_t frames, au_stream_t *out) {
    if (!frames || !out || stream_ring)
        return -1;

    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
    void  *ring  = NULL;
    long   shm   = -1;

    // the worker reads it as we write it, so its shared rather than copied like a sample
    if (worker) {
        shm = sys_shm_create(pages_for(bytes));
        if (shm < 0)
            return -1;
        long at = sys_shm_map(shm);
        if (at < 0) {
            sys_shm_destroy(shm);
            return -1;
        }
        ring = (void *)at;
    } else {
        ring = calloc(1, bytes);
        if (!ring)
            return -1;
    }

    uint64_t x = worker ? (uint64_t)shm : (uint64_t)(uintptr_t)ring;
    if (wait_result(send(CMD_STREAM_OPEN, (int32_t)frames, 0, 0, 0, 0, x)) < 0) {
        if (worker) {
            _munmap(ring);
            sys_shm_destroy(shm);
        } else {
            free(ring);
        }
        return -1;
    }

    stream_ring = ring;
    stream_shm  = shm;
    out->ring   = ring;
    out->frames = frames;
    return 0;
}

uint64_t au_stream_pos(void) {
    return au_shared->stream_pos;
}

void au_stream_close(void) {
    if (!stream_ring)
        return;

    wait_result(send(CMD_STREAM_CLOSE, 0, 0, 0, 0, 0, 0));
    if (stream_shm >= 0) {
        _munmap(stream_ring);
        sys_shm_destroy(stream_shm);
    } else {
        free(stream_ring);
    }
    stream_ring = NULL;
    stream_shm  = -1;
}
