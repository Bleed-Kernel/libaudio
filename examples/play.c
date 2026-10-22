#include <bleed_audio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, 2);
    long size = ftell(f);
    fseek(f, 0, 0);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }

    void *buf = malloc((size_t)size);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcasecmp(s + a - b, tail);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    const char *bank = NULL;
    int         loop = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l"))
            loop = 1;
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            bank = argv[++i];
        else
            path = argv[i];
    }
    if (!path) {
        printf("usage: play file.wav|file.mid [-b bank.sf2] [-l]\n");
        return 1;
    }

    size_t len  = 0;
    void  *data = slurp(path, &len);
    if (!data) {
        printf("play: cant read %s\n", path);
        return 1;
    }

    if (bank && au_bank_load_file(bank) < 0)
        printf("play: %s isnt a soundfont, trying the default one\n", bank);

    int r = au_open();
    if (r < 0) {
        printf("play: no sound card (%d)\n", r);
        return 1;
    }

    au_sample_t sample;
    au_song_t  *song  = NULL;
    int         voice = -1;

    if (ends_with(path, ".wav") && au_wav_parse(data, len, &sample) == 0) {
        printf("play: %u frames, %u Hz, %u channel%s\n", sample.frames, sample.rate, sample.channels,
               sample.channels == 1 ? "" : "s");
        voice = au_play(&sample, 127, 64, loop);
    } else if ((song = au_song_load(data, len))) {
        au_song_play(song, loop);
    } else {
        printf("play: %s isnt a wav or midi file\n", path);
        au_close();
        return 1;
    }

    while (au_voice_playing(voice) || au_song_playing())
        au_sleep_ms(20);

    // whats still queued plays out after the release
    au_close();
    au_song_free(song);
    free(data);
    return 0;
}
