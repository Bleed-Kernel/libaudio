#include "audio_priv.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int au_wav_parse(const void *data, size_t len, au_sample_t *out) {
    const uint8_t *p = data;
    if (!p || !out || len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4))
        return -1;

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *at  = p + 12;
    const uint8_t *end = p + len;

    while (at + 8 <= end) {
        uint32_t       clen = le32(at + 4);
        const uint8_t *body = at + 8;
        if (clen > (size_t)(end - body))
            clen = (uint32_t)(end - body);

        if (!memcmp(at, "fmt ", 4) && clen >= 16) {
            fmt      = le16(body);
            channels = le16(body + 2);
            rate     = le32(body + 4);
            bits     = le16(body + 14);
        } else if (!memcmp(at, "data", 4)) {
            // only plain pcm, anything compressed is someone elses problem
            if (fmt != 1 || (bits != 8 && bits != 16) || (channels != 1 && channels != 2) || !rate)
                return -1;

            out->data     = body;
            out->rate     = rate;
            out->channels = (uint8_t)channels;
            out->format   = bits == 8 ? AU_U8 : AU_S16;
            out->frames   = clen / (channels * (bits / 8));
            return out->frames ? 0 : -1;
        }

        at = body + clen + (clen & 1);
    }
    return -1;
}
