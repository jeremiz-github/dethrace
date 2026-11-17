#define LIBDRAGON_PREVIEW 1
// CPU mixer backend (libdragon audio driver only; the RSP mixer stalls behind long RDP jobs and crashed) for the N64 port: 8-bit unsigned mono samples are converted to signed on the fly
// through a waveform read callback (the sample data is owned by S3 and never modified).
#include "harness/audio.h"
#include <libdragon.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define N64_MIX_CHANNELS 12
#define N64_MIX_FREQ 22050
#define N64_MIX_MAXFRAMES 2048

typedef struct tN64_sample {
    const uint8_t* data;
    int size;
    int ch; // mixer channel, -1 when not started
    int init_volume;
    int init_pan;
    int init_new_rate;
    int rate;
    int playing;
} tN64_sample;

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos; // 16.16
    uint32_t step; // 16.16 per output frame
    int vl, vr; // 0..256
    volatile int active;
    int loop;
} tChan;

static tChan chans[N64_MIX_CHANNELS];
static int audio_ready;
static tN64_sample* owner[N64_MIX_CHANNELS];
static int32_t acc[N64_MIX_MAXFRAMES * 2];

#define N64_MAX_PITCH_HZ 44100.0f
static float clamp_pitch(float f) { return f > N64_MAX_PITCH_HZ ? N64_MAX_PITCH_HZ : (f < 100.0f ? 100.0f : f); }
static uint32_t step_for(float hz) { return (uint32_t)(clamp_pitch(hz) * 65536.0f / N64_MIX_FREQ); }

volatile unsigned gN64_audio_cb;
volatile int gN64_audio_peak;

// runs from the audio interrupt: plain CPU mixing, no RSP involved
static void fill_cb(short* buf, size_t n) {
    int c, peak = 0;
    size_t i;
    if (n > N64_MIX_MAXFRAMES) n = N64_MIX_MAXFRAMES;
    memset(acc, 0, n * 2 * sizeof(int32_t));
    for (c = 0; c < N64_MIX_CHANNELS; c++) {
        tChan* ch = &chans[c];
        uint32_t pos, step, size;
        const uint8_t* d;
        int vl, vr;
        if (!ch->active) continue;
        pos = ch->pos; step = ch->step; size = ch->size; d = ch->data; vl = ch->vl; vr = ch->vr;
        for (i = 0; i < n; i++) {
            int smp;
            uint32_t idx = pos >> 16;
            if (idx >= size) {
                if (ch->loop) { pos -= size << 16; idx = pos >> 16; if (idx >= size) { pos = 0; idx = 0; } }
                else { ch->active = 0; break; }
            }
            smp = (int)d[idx] - 128;
            acc[i * 2] += smp * vl;
            acc[i * 2 + 1] += smp * vr;
            pos += step;
        }
        ch->pos = pos;
    }
    for (i = 0; i < n * 2; i++) {
        int v = acc[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[i] = (short)v;
        if (((v < 0) ? -v : v) > peak) peak = (v < 0) ? -v : v;
    }
    if (n * 2 < 1) peak = 0;
    if (peak > gN64_audio_peak) gN64_audio_peak = peak;
    gN64_audio_cb++;
}

static uint8_t beep_data[50];
static unsigned long beep_end_ms;

// The AI interrupt (which calls fill_cb) only fires when a queued buffer finishes: nothing runs until a first buffer is queued,
// and the chain would stay dead if it ever stalls. Called every frame: re-prime it when no callback ran for a while.
void N64_Audio_Poll(void) {
    static unsigned last_cb;
    static unsigned long last_ms;
    unsigned long now;
    if (!audio_ready) return;
    now = get_ticks_ms();
    if (beep_end_ms && now > beep_end_ms) {
        chans[N64_MIX_CHANNELS - 1].active = 0;
        beep_end_ms = 0;
    }
    if (gN64_audio_cb != last_cb) {
        last_cb = gN64_audio_cb;
        last_ms = now;
    } else if (now - last_ms > 300) {
        last_ms = now;
        if (audio_can_write()) audio_write_silence();
    }
}

tAudioBackend_error_code AudioBackend_Init(void) {
    int i;
    if (audio_ready) {
        return eAB_success;
    }
    audio_init(N64_MIX_FREQ, 4);
    memset(chans, 0, sizeof(chans));
    audio_set_buffer_callback(fill_cb);
    audio_ready = 1;
    if (audio_can_write()) audio_write_silence();
    for (i = 0; i < 50; i++) beep_data[i] = (i < 25) ? 188 : 68; // ~440 Hz square wave
    {
        tChan* b = &chans[N64_MIX_CHANNELS - 1];
        b->data = beep_data; b->size = 50; b->pos = 0; b->step = 65536; b->vl = b->vr = 100; b->loop = 1;
        b->active = 1;
        beep_end_ms = get_ticks_ms() + 300;
    }
    fprintf(stderr, "audio: CPU mixer ready, %d channels at %d Hz\n", N64_MIX_CHANNELS, N64_MIX_FREQ);
    return eAB_success;
}

void AudioBackend_UnInit(void) {
    if (audio_ready) {
        audio_set_buffer_callback(NULL);
        audio_close();
        audio_ready = 0;
    }
}

tAudioBackend_error_code AudioBackend_InitCDA(void) {
    return eAB_error; // no CD audio / music files on the N64
}

void AudioBackend_UnInitCDA(void) {
}

void* AudioBackend_AllocateSampleTypeStruct(void) {
    tN64_sample* s = (tN64_sample*)calloc(1, sizeof(tN64_sample));
    if (s != NULL) {
        s->ch = -1;
    }
    return s;
}

static void apply_params(tN64_sample* s) {
    float vol, pan;
    tChan* c;
    if (s->ch < 0) {
        return;
    }
    c = &chans[s->ch];
    vol = s->init_volume / 300.0f;
    if (vol > 1.0f) vol = 1.0f;
    if (vol < 0.0f) vol = 0.0f;
    pan = 0.5f + s->init_pan / 20000.0f;
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    c->vl = (int)(vol * 110.0f * (pan < 0.5f ? 1.0f : 2.0f * (1.0f - pan)));
    c->vr = (int)(vol * 110.0f * (pan > 0.5f ? 1.0f : 2.0f * pan));
    if (s->init_new_rate > 0) {
        c->step = step_for((float)s->init_new_rate);
    }
}

tAudioBackend_error_code AudioBackend_PlaySample(void* type_struct_sample, int channels, void* data, int size, int rate, int loop) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    int ch, i;
    static int plays, fails;
    if (!audio_ready || s == NULL || channels != 1 || data == NULL || size <= 0 || rate <= 0) {
        if (fails++ < 10) fprintf(stderr, "audio: PlaySample rejected (ready %d ch %d size %d rate %d)\n", audio_ready, channels, size, rate);
        return eAB_error;
    }
    if (s->ch >= 0 && owner[s->ch] == s) {
        chans[s->ch].active = 0;
        owner[s->ch] = NULL;
    }
    ch = -1;
    for (i = 0; i < N64_MIX_CHANNELS - 1; i++) {
        if (owner[i] == NULL || !chans[i].active) {
            ch = i;
            break;
        }
    }
    if (ch < 0) {
        return eAB_error;
    }
    if (owner[ch] != NULL && owner[ch] != s) {
        owner[ch]->ch = -1;
        owner[ch]->playing = 0;
    }
    s->data = (const uint8_t*)data;
    s->size = size;
    s->rate = rate;
    s->ch = ch;
    owner[ch] = s;
    if (plays++ < 10) fprintf(stderr, "audio: play ch %d size %d rate %d loop %d\n", ch, size, rate, loop);
    {
        tChan* c = &chans[ch];
        c->active = 0;
        c->data = s->data;
        c->size = (uint32_t)size;
        c->pos = 0;
        c->step = step_for((float)rate);
        c->loop = loop ? 1 : 0;
        c->vl = c->vr = 128;
        apply_params(s);
        c->active = 1;
    }
    s->playing = 1;
    return eAB_success;
}

int AudioBackend_SoundIsPlaying(void* type_struct_sample) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    if (!audio_ready || s == NULL || s->ch < 0 || owner[s->ch] != s) {
        return 0;
    }
    return chans[s->ch].active ? 1 : 0;
}

tAudioBackend_error_code AudioBackend_StopSample(void* type_struct_sample) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    if (audio_ready && s != NULL && s->ch >= 0 && owner[s->ch] == s) {
        chans[s->ch].active = 0;
        owner[s->ch] = NULL;
    }
    if (s != NULL) {
        s->ch = -1;
        s->playing = 0;
    }
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetVolume(void* type_struct_sample, int volume) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    if (s == NULL) {
        return eAB_error;
    }
    s->init_volume = volume;
    if (s->ch >= 0 && owner[s->ch] == s) {
        apply_params(s);
    }
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetPan(void* type_struct_sample, int pan) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    if (s == NULL) {
        return eAB_error;
    }
    s->init_pan = pan;
    if (s->ch >= 0 && owner[s->ch] == s) {
        apply_params(s);
    }
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetFrequency(void* type_struct_sample, int original_rate, int new_rate) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    (void)original_rate;
    if (s == NULL) {
        return eAB_error;
    }
    s->init_new_rate = new_rate;
    if (s->ch >= 0 && owner[s->ch] == s && new_rate > 0) {
        chans[s->ch].step = step_for((float)new_rate);
    }
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetVolumeSeparate(void* type_struct_sample, int left_volume, int right_volume) {
    (void)type_struct_sample;
    (void)left_volume;
    (void)right_volume;
    return eAB_error;
}

tAudioBackend_error_code AudioBackend_PlayCDA(int track) {
    (void)track;
    return eAB_error;
}

tAudioBackend_error_code AudioBackend_StopCDA(void) {
    return eAB_success;
}

int AudioBackend_CDAIsPlaying(void) {
    return 0;
}

tAudioBackend_error_code AudioBackend_SetCDAVolume(int volume) {
    (void)volume;
    return eAB_error;
}

tAudioBackend_stream* AudioBackend_StreamOpen(int bitdepth, int channels, unsigned int sample_rate) {
    (void)bitdepth;
    (void)channels;
    (void)sample_rate;
    return NULL;
}

tAudioBackend_error_code AudioBackend_StreamWrite(tAudioBackend_stream* stream_handle, const unsigned char* data, unsigned long size) {
    (void)stream_handle;
    (void)data;
    (void)size;
    return eAB_error;
}

tAudioBackend_error_code AudioBackend_StreamClose(tAudioBackend_stream* stream_handle) {
    (void)stream_handle;
    return eAB_error;
}
