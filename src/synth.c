#include "audio_priv.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "vendor/tsf.h"

#define DRUM_CHANNEL    9
#define MAX_VOICES      64

static tsf  *font;
static float fbuf[AU_CHUNK * 2];
static int   tried_default;

static void channels_reset(void) {
    tsf_reset(font);
    for (int ch = 0; ch < 16; ch++)
        tsf_channel_set_presetnumber(font, ch, 0, ch == DRUM_CHANNEL);
}

int eng_bank_load_sf2(const void *data, size_t len) {
    if (!data || len < 12 || len > 0x7FFFFFFF)
        return -1;

    tsf *f = tsf_load_memory(data, (int)len);
    if (!f)
        return -1;

    if (font)
        tsf_close(font);
    font = f;
    tsf_set_max_voices(font, MAX_VOICES);
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, (int)au_out_rate, 0);
    channels_reset();
    return 0;
}

int eng_bank_load_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    fseek(fp, 0, 2);
    long len = ftell(fp);
    fseek(fp, 0, 0);

    void *data = len > 0 ? malloc((size_t)len) : NULL;
    int   r    = -1;
    if (data && fread(data, 1, (size_t)len, fp) == (size_t)len)
        r = eng_bank_load_sf2(data, (size_t)len);

    // tsf keeps its own copy of everything, the file isnt needed after
    free(data);
    fclose(fp);
    return r;
}

int eng_bank_load_default(void) {
    if (font)
        return 0;
    tried_default = 1;

    DIR *dir = opendir(AU_SOUNDFONT_DIR);
    if (!dir)
        return -1;

    int r = -1;
    struct dirent *e;
    while (r < 0 && (e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".sf2"))
            continue;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", AU_SOUNDFONT_DIR, e->d_name);
        r = eng_bank_load_file(path);
    }
    closedir(dir);
    return r;
}

void synth_rate_changed(void) {
    if (font)
        tsf_set_output(font, TSF_STEREO_INTERLEAVED, (int)au_out_rate, 0);
}

void eng_synth_reset(void) {
    if (!font && !tried_default)
        eng_bank_load_default();
    if (font)
        channels_reset();
}

void eng_synth_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    if (!font)
        return;

    int ch = status & 0x0F;
    data1 &= 0x7F;
    data2 &= 0x7F;

    switch (status & 0xF0) {
        case 0x80:
            tsf_channel_note_off(font, ch, data1);
            break;
        case 0x90:
            if (data2)
                tsf_channel_note_on(font, ch, data1, data2 / 127.0f);
            else
                tsf_channel_note_off(font, ch, data1);
            break;
        case 0xB0:
            tsf_channel_midi_control(font, ch, data1, data2);
            break;
        case 0xC0:
            tsf_channel_set_presetnumber(font, ch, data1, ch == DRUM_CHANNEL);
            break;
        case 0xE0:
            tsf_channel_set_pitchwheel(font, ch, (data2 << 7) | data1);
            break;
        default:
            break;
    }
}

void synth_render(int32_t *mix, uint32_t frames) {
    if (!font || !tsf_active_voice_count(font))
        return;

    while (frames) {
        uint32_t n = frames < AU_CHUNK ? frames : AU_CHUNK;
        tsf_render_float(font, fbuf, (int)n, 0);
        for (uint32_t i = 0; i < n * 2; i++)
            mix[i] += (int32_t)(fbuf[i] * 32767.0f);
        mix    += n * 2;
        frames -= n;
    }
}
