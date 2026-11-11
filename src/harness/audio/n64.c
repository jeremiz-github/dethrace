#define LIBDRAGON_PREVIEW 1
// libdragon mixer backend for the N64 port: 8-bit unsigned mono samples are converted to signed on the fly
// through a waveform read callback (the sample data is owned by S3 and never modified).
#include "harness/audio.h"
#include <libdragon.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define N64_MIX_CHANNELS 16
#define N64_MIX_FREQ 22050

typedef struct tN64_sample {
    waveform_t wave;
    const uint8_t* data;
    int size;
    int ch; // mixer channel, -1 when not started
    int init_volume;
    int init_pan;
    int init_new_rate;
    int rate;
    int has_init;
    int playing;
} tN64_sample;

static int audio_ready;
static tN64_sample* owner[N64_MIX_CHANNELS];

static void sample_read(void* ctx, samplebuffer_t* sbuf, int wpos, int wlen, bool seeking) {
    tN64_sample* s = (tN64_sample*)ctx;
    (void)seeking;
    while (wlen > 0) {
        int n = wlen < SAMPLEBUFFER_MARGIN_UNITS ? wlen : SAMPLEBUFFER_MARGIN_UNITS;
        int8_t* dst = (int8_t*)samplebuffer_append(sbuf, n);
        int i;
        for (i = 0; i < n; i++) {
            int p = wpos + i;
            dst[i] = (p >= 0 && p < s->size) ? (int8_t)(s->data[p] ^ 0x80) : 0;
        }
        wpos += n;
        wlen -= n;
    }
}

// the mixer is filled from the audio interrupt (libdragon's recommended pattern), so frame hitches do not cut the sound
static void fill_cb(short* buf, size_t n) {
    mixer_poll(buf, (int)n);
}

void N64_Audio_Poll(void) {
}

tAudioBackend_error_code AudioBackend_Init(void) {
    if (audio_ready) {
        return eAB_success;
    }
    audio_init(N64_MIX_FREQ, AUDIO_INIT_LATENCY_MS(200));
    mixer_init(N64_MIX_CHANNELS);
    mixer_set_vol(1.0f);
    audio_set_buffer_callback(fill_cb);
    audio_ready = 1;
    fprintf(stderr, "audio: libdragon mixer ready, %d channels at %d Hz\n", N64_MIX_CHANNELS, N64_MIX_FREQ);
    return eAB_success;
}

void AudioBackend_UnInit(void) {
    if (audio_ready) {
        audio_set_buffer_callback(NULL);
        mixer_close();
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
    if (s->ch < 0) {
        return;
    }
    vol = s->init_volume / 300.0f;
    if (vol > 1.0f) vol = 1.0f;
    if (vol < 0.0f) vol = 0.0f;
    pan = 0.5f + s->init_pan / 20000.0f;
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    mixer_ch_set_vol_pan(s->ch, vol, pan);
    if (s->init_new_rate > 0) {
        mixer_ch_set_freq(s->ch, (float)s->init_new_rate);
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
        mixer_ch_stop(s->ch);
        owner[s->ch] = NULL;
    }
    ch = -1;
    for (i = 0; i < N64_MIX_CHANNELS; i++) {
        if (owner[i] == NULL || !mixer_ch_playing(i)) {
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
    memset(&s->wave, 0, sizeof(s->wave));
    s->wave.name = "dethrace";
    s->wave.bits = 8;
    s->wave.channels = 1;
    s->wave.frequency = (float)rate;
    s->wave.len = size;
    s->wave.loop_len = loop ? size : 0;
    s->wave.read = sample_read;
    s->wave.ctx = s;
    s->data = (const uint8_t*)data;
    s->size = size;
    s->rate = rate;
    s->ch = ch;
    owner[ch] = s;
    if (plays++ < 10) fprintf(stderr, "audio: play ch %d size %d rate %d loop %d\n", ch, size, rate, loop);
    mixer_ch_play(ch, &s->wave);
    mixer_ch_set_priority(ch, MIXER_PRIORITY_SFX);
    apply_params(s);
    if (loop) {
        mixer_ch_set_loop(ch, true);
    }
    s->playing = 1;
    return eAB_success;
}

int AudioBackend_SoundIsPlaying(void* type_struct_sample) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    if (!audio_ready || s == NULL || s->ch < 0 || owner[s->ch] != s) {
        return 0;
    }
    return mixer_ch_playing(s->ch) ? 1 : 0;
}

tAudioBackend_error_code AudioBackend_StopSample(void* type_struct_sample) {
    tN64_sample* s = (tN64_sample*)type_struct_sample;
    if (audio_ready && s != NULL && s->ch >= 0 && owner[s->ch] == s) {
        mixer_ch_stop(s->ch);
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
        mixer_ch_set_freq(s->ch, (float)new_rate);
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
