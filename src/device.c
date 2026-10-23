#include "audio_priv.h"

#include <audio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int               dev_fd = -1;
static uint8_t          *ring;
static struct audio_info info;
static uint64_t          lead;
static uint64_t          peak;
static uint64_t          last_hw;
static uint64_t          last_xruns;
static uint64_t          xrun_extra;
static uint32_t          calm;
static uint32_t          floor_ms = AU_LEAD_MIN_MS;

static uint64_t ms_bytes(uint32_t ms) {
    return (uint64_t)ms * info.rate / 1000 * info.frame_bytes;
}

static void fill(uint64_t at, uint64_t bytes) {
    uint64_t first = info.ring_bytes - at;
    if (first > bytes)
        first = bytes;

    eng_render((int16_t *)(ring + at), (uint32_t)(first / info.frame_bytes));
    if (bytes > first)
        eng_render((int16_t *)ring, (uint32_t)((bytes - first) / info.frame_bytes));
}

int card_open(void) {
    int fd = open("/dev/audio0", O_WRONLY);
    if (fd < 0)
        return fd;

    struct audio_map map;
    if (ioctl(fd, AUDIO_IOC_GET_INFO, &info) < 0 || info.format != AUDIO_FORMAT_S16LE || info.channels != 2) {
        close(fd);
        return -1;
    }

    int r = ioctl(fd, AUDIO_IOC_ACQUIRE, &map);
    if (r < 0) {
        close(fd);
        return r;
    }

    dev_fd      = fd;
    ring        = (uint8_t *)(uintptr_t)map.addr;
    au_out_rate = info.rate;
    synth_rate_changed();

    lead       = ms_bytes(floor_ms);
    peak       = lead;
    last_hw    = 0;
    last_xruns = 0;
    xrun_extra = 0;

    // something queued before it starts 
    uint64_t prefill = lead - lead % info.frame_bytes;
    fill(0, prefill);
    ioctl(dev_fd, AUDIO_IOC_COMMIT, &prefill);
    ioctl(dev_fd, AUDIO_IOC_START, NULL);
    return 0;
}

void card_close(void) {
    if (dev_fd < 0)
        return;
    ioctl(dev_fd, AUDIO_IOC_RELEASE, NULL);
    close(dev_fd);
    dev_fd = -1;
    ring   = NULL;
}

void card_pump(void) {
    if (dev_fd < 0)
        return;

    struct audio_pos pos;
    if (ioctl(dev_fd, AUDIO_IOC_GET_POS, &pos) < 0)
        return;
    if (!pos.running)
        ioctl(dev_fd, AUDIO_IOC_START, NULL);

    uint64_t gap = pos.hw_pos > last_hw ? pos.hw_pos - last_hw : 0;
    last_hw = pos.hw_pos;
    if (gap > peak)
        peak = gap;
    else
        peak -= peak / 16384;

    // ran dry since last time, whatever we were doing wasnt enough
    if (pos.underruns != last_xruns) {
        last_xruns  = pos.underruns;
        xrun_extra += ms_bytes(AU_LEAD_XRUN_MS);
        calm        = 0;
    } else if (xrun_extra && ++calm >= 2000) {
        xrun_extra = xrun_extra > ms_bytes(1) ? xrun_extra - ms_bytes(1) : 0;
        calm       = 0;
    }

    uint64_t lo = ms_bytes(floor_ms);
    uint64_t hi = ms_bytes(AU_LEAD_MAX_MS);
    lead = peak + peak / 2 + info.period_bytes + xrun_extra;
    if (lead < lo) lead = lo;
    if (lead > hi) lead = hi;

    uint64_t queued = pos.write_pos > pos.hw_pos ? pos.write_pos - pos.hw_pos : 0;
    au_shared->queued = (uint32_t)(queued / info.frame_bytes);
    if (queued >= lead)
        return;

    uint64_t want = lead - queued;
    if (want > pos.writable)
        want = pos.writable;
    want -= want % info.frame_bytes;
    if (!want)
        return;

    fill(pos.write_pos % info.ring_bytes, want);
    ioctl(dev_fd, AUDIO_IOC_COMMIT, &want);
    au_shared->queued = (uint32_t)((queued + want) / info.frame_bytes);
}

void card_latency(uint32_t ms) {
    if (ms < AU_LEAD_MIN_MS) ms = AU_LEAD_MIN_MS;
    if (ms > AU_LEAD_MAX_MS) ms = AU_LEAD_MAX_MS;
    floor_ms = ms;
    card_pump();
}

// till the card finishes the period its on
void card_wait(void) {
    if (dev_fd >= 0)
        ioctl(dev_fd, AUDIO_IOC_WAIT, NULL);
}
