// sx1255_repeater.c
// Copyright (c) 2025 Karel ok1lbc <xkk@post.cz>
// SPDX-License-Identifier: MIT
//
// Duplex SX1255 repeater without the intermediate ZMQ stage.
//
// RX path:
//   RF ALSA capture S32_LE interleaved I/Q @125 kHz
//   -> shift -25 kHz -> LPF+decim 5 -> NBFM demod
//   -> 25 kHz to RX_AUDIO_SR resampler -> deemphasis
//   -> audio ALSA playback S16_LE mono
//
// TX path:
//   audio ALSA capture S16_LE mono @16 kHz
//   -> DC block -> LPF -> preemphasis -> FM modulator
//   -> simple 125/16 upsampler -> RF ALSA playback S32_LE interleaved I/Q @125 kHz
//
// Control over a tiny TCP server with lines: "SECRET PING|RX|TX|DUP|STOP".
//
// Build:
//   gcc -std=c11 -O2 -pthread -Wall -Wextra -o sx1255_repeater sx1255_repeater.c -lasound -lm

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -------- Config via ENV --------
static const char* EV(const char* k, const char* d){ const char* v=getenv(k); return v&&*v?v:d; }
static int EV_I(const char* k, int d){ const char* v=getenv(k); return v&&*v?atoi(v):d; }
static double EV_D(const char* k, double d){ const char* v=getenv(k); return v&&*v?strtod(v,NULL):d; }

static const char* SECRET;
static const char* SPI_DEV;
static uint32_t SPI_SPEED_HZ;
static double PRETX_S;
static double PRERX_S;

static const char* RF_PLAY_DEV;     // SX1255 TX I/Q playback
static const char* RF_CAPT_DEV;     // SX1255 RX I/Q capture
static int RF_SR;
static int RF_CH;
static int RF_PERIOD;
static int RF_BUF_PERIODS;

static const char* AUDIO_IN_DEV;    // TX audio source
static const char* AUDIO_OUT_DEV;   // RX audio sink
static int TX_AUDIO_SR;
static int RX_AUDIO_SR;
static int AUDIO_PERIOD;

static float RX_SHIFT_HZ;
static float RX_MAX_DEV_HZ;
static float RX_DEEMPH_TAU;
static float TX_GAIN;
static float TX_PREEMPH_TAU;

static const char* HOST;
static int PORT;

// -------- State --------
typedef enum { MODE_IDLE=0, MODE_RX=1, MODE_TX=2, MODE_DUP=3 } radio_mode_t;
static atomic_int want_run = 1;
static atomic_int cur_mode = MODE_IDLE;
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

static snd_pcm_t* pcm_rf_play = NULL;
static snd_pcm_t* pcm_rf_cap = NULL;
static snd_pcm_t* pcm_audio_in = NULL;
static snd_pcm_t* pcm_audio_out = NULL;

static pthread_t th_tx, th_rx;
static atomic_int tx_alive = 0;
static atomic_int rx_alive = 0;
static int tx_thread_started = 0;
static int rx_thread_started = 0;
static int srv_fd = -1;

// -------- DSP primitives --------
#define RX_IF_DECIM 5
#define RX_IF_TAPS 61
#define AUDIO_FIR_TAPS 81

typedef struct { float re, im; } cf32;

typedef struct {
    cf32 x_prev;
    cf32 y_prev;
    float r;
} dc_block_cc_t;

typedef struct {
    float x_prev;
    float y_prev;
    float r;
} dc_block_ff_t;

typedef struct {
    float taps[RX_IF_TAPS];
    cf32 delay[RX_IF_TAPS];
    int idx;
} fir_decim_cc_t;

typedef struct {
    float taps[AUDIO_FIR_TAPS];
    float delay[AUDIO_FIR_TAPS];
    int idx;
} fir_ff_t;

typedef struct {
    cf32 prev;
    bool has_prev;
} nbfm_demod_t;

typedef struct {
    float y_prev;
    float alpha;
} onepole_lp_t;

typedef struct {
    float x_prev;
    float a;
} preemph_t;

typedef struct {
    float phase;
    float phase_inc;
} rotator_t;

typedef struct {
    float phase;
    float sensitivity;
} fm_mod_t;

static void design_lowpass(float *taps, int ntaps, float fs, float fc)
{
    const float norm_cut = fc / fs;
    const int M = ntaps - 1;
    float sum = 0.0f;

    for(int n=0; n<ntaps; n++){
        float m = (float)n - (float)M / 2.0f;
        float h = fabsf(m) < 1e-6f ? 2.0f * norm_cut : sinf(2.0f * (float)M_PI * norm_cut * m) / ((float)M_PI * m);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * n / (float)M);
        taps[n] = h * w;
        sum += taps[n];
    }
    for(int n=0; n<ntaps; n++) taps[n] /= sum;
}

static void dc_block_cc_init(dc_block_cc_t *d, float r){ memset(d, 0, sizeof(*d)); d->r = r; }
static inline cf32 dc_block_cc_process(dc_block_cc_t *d, cf32 x)
{
    cf32 y = {
        .re = x.re - d->x_prev.re + d->r * d->y_prev.re,
        .im = x.im - d->x_prev.im + d->r * d->y_prev.im
    };
    d->x_prev = x;
    d->y_prev = y;
    return y;
}

static void dc_block_ff_init(dc_block_ff_t *d, float r){ memset(d, 0, sizeof(*d)); d->r = r; }
static inline float dc_block_ff_process(dc_block_ff_t *d, float x)
{
    float y = x - d->x_prev + d->r * d->y_prev;
    d->x_prev = x;
    d->y_prev = y;
    return y;
}

static void fir_decim_cc_init(fir_decim_cc_t *f, float fs, float fc)
{
    memset(f, 0, sizeof(*f));
    design_lowpass(f->taps, RX_IF_TAPS, fs, fc);
}

static inline void fir_decim_cc_push(fir_decim_cc_t *f, cf32 x)
{
    f->delay[f->idx++] = x;
    if(f->idx >= RX_IF_TAPS) f->idx = 0;
}

static inline cf32 fir_decim_cc_compute(const fir_decim_cc_t *f)
{
    cf32 y = {0.0f, 0.0f};
    int idx = f->idx;
    for(int n=0; n<RX_IF_TAPS; n++){
        if(--idx < 0) idx = RX_IF_TAPS - 1;
        y.re += f->taps[n] * f->delay[idx].re;
        y.im += f->taps[n] * f->delay[idx].im;
    }
    return y;
}

static void fir_ff_init(fir_ff_t *f, float fs, float fc)
{
    memset(f, 0, sizeof(*f));
    design_lowpass(f->taps, AUDIO_FIR_TAPS, fs, fc);
}

static inline float fir_ff_process(fir_ff_t *f, float x)
{
    f->delay[f->idx++] = x;
    if(f->idx >= AUDIO_FIR_TAPS) f->idx = 0;

    float y = 0.0f;
    int idx = f->idx;
    for(int n=0; n<AUDIO_FIR_TAPS; n++){
        if(--idx < 0) idx = AUDIO_FIR_TAPS - 1;
        y += f->taps[n] * f->delay[idx];
    }
    return y;
}

static void nbfm_demod_init(nbfm_demod_t *d){ memset(d, 0, sizeof(*d)); }
static inline float nbfm_demod(nbfm_demod_t *d, cf32 x)
{
    if(!d->has_prev){
        d->prev = x;
        d->has_prev = true;
        return 0.0f;
    }
    float re = x.re * d->prev.re + x.im * d->prev.im;
    float im = x.im * d->prev.re - x.re * d->prev.im;
    d->prev = x;
    return atan2f(im, re);
}

static void onepole_lp_init(onepole_lp_t *d, float tau, float fs)
{
    memset(d, 0, sizeof(*d));
    float dt = 1.0f / fs;
    d->alpha = dt / (tau + dt);
}

static inline float onepole_lp_process(onepole_lp_t *d, float x)
{
    float y = d->y_prev + d->alpha * (x - d->y_prev);
    d->y_prev = y;
    return y;
}

static void preemph_init(preemph_t *p, float tau, float fs)
{
    memset(p, 0, sizeof(*p));
    p->a = expf(-1.0f / (tau * fs));
}

static inline float preemph_process(preemph_t *p, float x)
{
    float y = x - p->a * p->x_prev;
    p->x_prev = x;
    return y;
}

static void rotator_init(rotator_t *r, float fs, float f_shift)
{
    r->phase = 0.0f;
    r->phase_inc = 2.0f * (float)M_PI * (f_shift / fs);
}

static inline cf32 rotator_process(rotator_t *r, cf32 x)
{
    float c = cosf(r->phase);
    float s = sinf(r->phase);
    cf32 y = { .re = x.re * c - x.im * s, .im = x.re * s + x.im * c };
    r->phase += r->phase_inc;
    if(r->phase > (float)M_PI) r->phase -= 2.0f * (float)M_PI;
    else if(r->phase < -(float)M_PI) r->phase += 2.0f * (float)M_PI;
    return y;
}

static void fm_mod_init(fm_mod_t *m, float sensitivity)
{
    memset(m, 0, sizeof(*m));
    m->sensitivity = sensitivity;
}

static inline cf32 fm_mod_process(fm_mod_t *m, float x)
{
    m->phase += m->sensitivity * x;
    if(m->phase > (float)M_PI) m->phase -= 2.0f * (float)M_PI;
    else if(m->phase < -(float)M_PI) m->phase += 2.0f * (float)M_PI;
    cf32 y = { .re = cosf(m->phase), .im = sinf(m->phase) };
    return y;
}

// -------- Utils --------
static void msleep(double s)
{
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static int mode_has_tx(int mode){ return mode == MODE_TX || mode == MODE_DUP; }
static int mode_has_rx(int mode){ return mode == MODE_RX || mode == MODE_DUP; }

static inline float clampf(float x, float lo, float hi)
{
    if(x < lo) return lo;
    if(x > hi) return hi;
    return x;
}

static int32_t f_to_s32(float x)
{
    x = clampf(x, -1.0f, 1.0f);
    return (int32_t)lrintf(x * 2147483000.0f);
}

// -------- SX1255 mode control over SPI --------
#define SX1255_REG_MODE 0x00
#define SX1255_REG_STAT 0x11

static int sx1255_spi_setup(int fd)
{
    uint8_t mode = 0;
    uint8_t bits = 8;

    if(ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) return -1;
    if(ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) return -1;
    if(ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &SPI_SPEED_HZ) < 0) return -1;
    return 0;
}

static int sx1255_spi_xfer(int fd, const uint8_t *tx, uint8_t *rx, size_t n)
{
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = (uint32_t)n,
        .speed_hz = SPI_SPEED_HZ,
        .bits_per_word = 8,
    };
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

static uint8_t sx1255_rreg(int fd, uint8_t a)
{
    uint8_t tx[2] = { (uint8_t)(a & 0x7F), 0x00 };
    uint8_t rx[2] = {0};
    sx1255_spi_xfer(fd, tx, rx, 2);
    return rx[1];
}

static int sx1255_wreg(int fd, uint8_t a, uint8_t v)
{
    uint8_t tx[2] = { (uint8_t)(0x80 | (a & 0x7F)), v };
    uint8_t rx[2] = {0};
    return sx1255_spi_xfer(fd, tx, rx, 2);
}

static int sx1255_mode_value(const char* mode, uint8_t *val)
{
    if(strcasecmp(mode, "idle") == 0){ *val = 0x01; return 0; } // REF
    if(strcasecmp(mode, "rx") == 0){ *val = 0x0B; return 0; }   // REF+RX
    if(strcasecmp(mode, "tx") == 0){ *val = 0x0D; return 0; }   // REF+TX
    if(strcasecmp(mode, "dup") == 0){ *val = 0x0F; return 0; }  // REF+RX+TX
    if(strcasecmp(mode, "off") == 0){ *val = 0x00; return 0; }  // all off
    return -1;
}

static int set_radio_mode(const char* mode)
{
    if(atomic_load(&tx_alive) || atomic_load(&rx_alive)){
        fprintf(stderr, "refusing setmode '%s' while I/O workers are alive\n", mode);
        return -1;
    }

    uint8_t val;
    if(sx1255_mode_value(mode, &val) != 0){
        fprintf(stderr, "unknown radio mode '%s'\n", mode);
        return -1;
    }

    int fd = open(SPI_DEV, O_RDWR);
    if(fd < 0){
        perror(SPI_DEV);
        return -1;
    }
    if(sx1255_spi_setup(fd) < 0){
        perror("spi_setup");
        close(fd);
        return -1;
    }

    int rc = 0;
    if(sx1255_wreg(fd, SX1255_REG_MODE, 0x01) < 0) rc = -1;
    usleep(2000);
    if(rc == 0 && sx1255_wreg(fd, SX1255_REG_MODE, val) < 0) rc = -1;
    usleep(2000);

    uint8_t m = sx1255_rreg(fd, SX1255_REG_MODE);
    uint8_t s = sx1255_rreg(fd, SX1255_REG_STAT);
    fprintf(stderr, "MODE=0x%02X (REF=%d RX=%d TX=%d) STAT=0x%02X\n",
            m, !!(m & 1), !!(m & 2), !!(m & 4), s);

    if(rc != 0) perror("sx1255 mode write");
    close(fd);
    return rc;
}

static ssize_t pcm_read_frames(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t frames)
{
    for(;;){
        if(!atomic_load(&want_run) || atomic_load(&cur_mode) == MODE_IDLE) return -EINTR;
        snd_pcm_sframes_t r = snd_pcm_readi(pcm, buf, frames);
        if(r == -EAGAIN){ msleep(0.001); continue; }
        if(r == -EPIPE){ snd_pcm_prepare(pcm); continue; }
        if(r == -EIO){
            /*
             * snd-aloop capture returns EIO while its paired playback side is
             * not open yet.  Merely preparing the existing handle is not
             * sufficient after playback connects; let the TX worker reopen
             * the capture side.
             */
            return -EIO;
        }
        if(r < 0){
            r = snd_pcm_recover(pcm, (int)r, 1);
            if(r < 0) return r;
            continue;
        }
        return r;
    }
}

static int pcm_write_all(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t frames, int channels, size_t sample_bytes)
{
    const uint8_t *p = (const uint8_t*)buf;
    snd_pcm_uframes_t left = frames;
    while(left > 0){
        if(!atomic_load(&want_run) || atomic_load(&cur_mode) == MODE_IDLE) return -EINTR;
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, left);
        if(w == -EPIPE){ snd_pcm_prepare(pcm); continue; }
        if(w < 0){
            w = snd_pcm_recover(pcm, (int)w, 1);
            if(w < 0) return (int)w;
            continue;
        }
        left -= (snd_pcm_uframes_t)w;
        p += (size_t)w * (size_t)channels * sample_bytes;
    }
    return 0;
}

// -------- ALSA --------
static int alsa_open_rf_play(void)
{
    int rc;
    snd_pcm_hw_params_t* hw = NULL;
    snd_pcm_sw_params_t* sw = NULL;

    if((rc = snd_pcm_open(&pcm_rf_play, RF_PLAY_DEV, SND_PCM_STREAM_PLAYBACK, 0)) < 0) return rc;
    snd_pcm_nonblock(pcm_rf_play, 0);
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_rf_play, hw);
    snd_pcm_hw_params_set_access(pcm_rf_play, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_rf_play, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm_rf_play, hw, RF_CH);
    unsigned int us = (unsigned int)RF_SR;
    snd_pcm_hw_params_set_rate_near(pcm_rf_play, hw, &us, 0);
    snd_pcm_uframes_t period = (snd_pcm_uframes_t)RF_PERIOD;
    snd_pcm_hw_params_set_period_size_near(pcm_rf_play, hw, &period, 0);
    snd_pcm_uframes_t bsz = period * (snd_pcm_uframes_t)RF_BUF_PERIODS;
    snd_pcm_hw_params_set_buffer_size_near(pcm_rf_play, hw, &bsz);
    if((rc = snd_pcm_hw_params(pcm_rf_play, hw)) < 0) goto fail;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm_rf_play, sw);
    snd_pcm_sw_params_set_start_threshold(pcm_rf_play, sw, period * 2);
    snd_pcm_sw_params_set_avail_min(pcm_rf_play, sw, period);
    if((rc = snd_pcm_sw_params(pcm_rf_play, sw)) < 0) goto fail;
    snd_pcm_prepare(pcm_rf_play);
    return 0;
fail:
    snd_pcm_close(pcm_rf_play);
    pcm_rf_play = NULL;
    return rc;
}

static int alsa_open_rf_cap(void)
{
    int rc;
    snd_pcm_hw_params_t* hw = NULL;
    snd_pcm_sw_params_t* sw = NULL;

    if((rc = snd_pcm_open(&pcm_rf_cap, RF_CAPT_DEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) return rc;
    snd_pcm_nonblock(pcm_rf_cap, 1);
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_rf_cap, hw);
    snd_pcm_hw_params_set_access(pcm_rf_cap, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_rf_cap, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm_rf_cap, hw, RF_CH);
    unsigned int us = (unsigned int)RF_SR;
    snd_pcm_hw_params_set_rate_near(pcm_rf_cap, hw, &us, 0);
    snd_pcm_uframes_t period = (snd_pcm_uframes_t)RF_PERIOD;
    snd_pcm_hw_params_set_period_size_near(pcm_rf_cap, hw, &period, 0);
    snd_pcm_uframes_t bsz = period * (snd_pcm_uframes_t)RF_BUF_PERIODS;
    snd_pcm_hw_params_set_buffer_size_near(pcm_rf_cap, hw, &bsz);
    if((rc = snd_pcm_hw_params(pcm_rf_cap, hw)) < 0) goto fail;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm_rf_cap, sw);
    snd_pcm_sw_params_set_avail_min(pcm_rf_cap, sw, period);
    if((rc = snd_pcm_sw_params(pcm_rf_cap, sw)) < 0) goto fail;
    snd_pcm_prepare(pcm_rf_cap);
    return 0;
fail:
    snd_pcm_close(pcm_rf_cap);
    pcm_rf_cap = NULL;
    return rc;
}

static int alsa_open_audio_capture(void)
{
    int rc;
    if((rc = snd_pcm_open(&pcm_audio_in, AUDIO_IN_DEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) return rc;
    rc = snd_pcm_set_params(pcm_audio_in, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                            1, (unsigned int)TX_AUDIO_SR, 1, 100000);
    if(rc < 0){
        snd_pcm_close(pcm_audio_in);
        pcm_audio_in = NULL;
        return rc;
    }
    /*
     * Never let a missing snd-aloop playback client keep the TX worker stuck
     * in snd_pcm_readi().  Mode changes must remain responsive even when no
     * audio source is connected.
     */
    if((rc = snd_pcm_nonblock(pcm_audio_in, 1)) < 0){
        snd_pcm_close(pcm_audio_in);
        pcm_audio_in = NULL;
        return rc;
    }
    /*
     * In non-blocking mode snd-aloop capture can remain PREPARED forever:
     * snd_pcm_readi() then returns EIO even while the paired playback PCM is
     * RUNNING.  Start capture explicitly.  EIO is expected when no playback
     * client exists yet; the TX worker will reopen this handle and try again.
     */
    rc = snd_pcm_start(pcm_audio_in);
    if(rc < 0 && rc != -EIO){
        snd_pcm_close(pcm_audio_in);
        pcm_audio_in = NULL;
        return rc;
    }
    return 0;
}

static int alsa_open_audio_playback(void)
{
    int rc;
    if((rc = snd_pcm_open(&pcm_audio_out, AUDIO_OUT_DEV, SND_PCM_STREAM_PLAYBACK, 0)) < 0) return rc;
    rc = snd_pcm_set_params(pcm_audio_out, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                            1, (unsigned int)RX_AUDIO_SR, 1, 100000);
    if(rc < 0){
        snd_pcm_close(pcm_audio_out);
        pcm_audio_out = NULL;
        return rc;
    }
    return 0;
}

static void alsa_drop_all(void)
{
    if(pcm_rf_play) snd_pcm_drop(pcm_rf_play);
    /*
     * snd_pcm_drop() can deadlock against a blocking snd_pcm_readi() in
     * another thread.  abort() explicitly wakes capture readers so workers
     * can observe MODE_IDLE and terminate before the handles are closed.
     */
    if(pcm_rf_cap) snd_pcm_abort(pcm_rf_cap);
    if(pcm_audio_in) snd_pcm_abort(pcm_audio_in);
    if(pcm_audio_out) snd_pcm_drop(pcm_audio_out);
}

static void alsa_close_all(void)
{
    if(pcm_rf_play){ snd_pcm_close(pcm_rf_play); pcm_rf_play = NULL; }
    if(pcm_rf_cap){ snd_pcm_close(pcm_rf_cap); pcm_rf_cap = NULL; }
    if(pcm_audio_in){ snd_pcm_close(pcm_audio_in); pcm_audio_in = NULL; }
    if(pcm_audio_out){ snd_pcm_close(pcm_audio_out); pcm_audio_out = NULL; }
}

// -------- Threads --------
static void* tx_worker(void* _)
{
    (void)_;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    tx_alive = 1;

    const int audio_block = AUDIO_PERIOD > 0 ? AUDIO_PERIOD : 256;
    const int max_rf_frames = (audio_block * 125 + 15) / 16 + 8;

    int16_t *audio = calloc((size_t)audio_block, sizeof(int16_t));
    int32_t *rf = calloc((size_t)max_rf_frames * (size_t)RF_CH, sizeof(int32_t));
    if(!audio || !rf){
        free(audio);
        free(rf);
        tx_alive = 0;
        return NULL;
    }

    dc_block_ff_t dc;
    fir_ff_t lpf;
    preemph_t pre;
    fm_mod_t mod;
    dc_block_ff_init(&dc, 0.995f);
    fir_ff_init(&lpf, (float)TX_AUDIO_SR, 5000.0f);
    preemph_init(&pre, TX_PREEMPH_TAU, (float)TX_AUDIO_SR);
    // GNU Radio flow modulates at TX_AUDIO_SR and then resamples to RF_SR.
    // Here we generate at RF_SR directly, so scale the per-sample phase step.
    fm_mod_init(&mod, TX_GAIN * (float)TX_AUDIO_SR / (float)RF_SR);

    int up_acc = 0;
    unsigned int source_retries = 0;
    uint64_t dbg_samples = 0;
    double dbg_sum_sq = 0.0;
    int dbg_peak = 0;
    uint64_t dbg_clipped = 0;
    fprintf(stderr,
            "TX worker started: audio=%s format=S16_LE channels=1 rate=%d "
            "period=%d rf_rate=%d rf_channels=%d gain=%.3f\n",
            AUDIO_IN_DEV, TX_AUDIO_SR, audio_block, RF_SR, RF_CH, TX_GAIN);

    while(mode_has_tx(atomic_load(&cur_mode))){
        if(!pcm_audio_in || !pcm_rf_play){ msleep(0.001); continue; }

        ssize_t got = pcm_read_frames(pcm_audio_in, audio, (snd_pcm_uframes_t)audio_block);
        if(got == -EIO){
            source_retries++;
            if(source_retries == 1 || source_retries % 10 == 0){
                fprintf(stderr,
                        "TX audio source not ready (EIO), reopening capture"
                        " [retry=%u]\n",
                        source_retries);
            }

            int rc = 0;
            pthread_mutex_lock(&io_lock);
            if(mode_has_tx(atomic_load(&cur_mode))){
                if(pcm_audio_in){
                    snd_pcm_close(pcm_audio_in);
                    pcm_audio_in = NULL;
                }
                rc = alsa_open_audio_capture();
            }
            pthread_mutex_unlock(&io_lock);

            if(rc < 0){
                fprintf(stderr, "TX audio capture reopen failed: %s\n",
                        snd_strerror(rc));
            }
            msleep(0.100);
            continue;
        }
        if(got == -EINTR && !mode_has_tx(atomic_load(&cur_mode))) continue;
        if(got < 0){
            fprintf(stderr, "TX audio read failed: %s (%zd)\n",
                    snd_strerror((int)got), got);
            continue;
        }
        if(got == 0){
            fprintf(stderr, "TX audio read returned zero frames\n");
            continue;
        }
        source_retries = 0;

        for(ssize_t i=0; i<got; i++){
            int sample = audio[i];
            int magnitude = sample < 0 ? -sample : sample;
            if(magnitude > dbg_peak) dbg_peak = magnitude;
            if(magnitude >= 32760) dbg_clipped++;
            double normalized = (double)sample / 32768.0;
            dbg_sum_sq += normalized * normalized;
        }
        dbg_samples += (uint64_t)got;
        if(dbg_samples >= (uint64_t)TX_AUDIO_SR){
            double rms = sqrt(dbg_sum_sq / (double)dbg_samples);
            double rms_dbfs = rms > 0.0 ? 20.0 * log10(rms) : -INFINITY;
            double peak_dbfs = dbg_peak > 0
                ? 20.0 * log10((double)dbg_peak / 32768.0)
                : -INFINITY;
            fprintf(stderr,
                    "TX AUDIO: samples=%llu peak=%d (%.1f dBFS) "
                    "rms=%.6f (%.1f dBFS) clipped=%llu\n",
                    (unsigned long long)dbg_samples, dbg_peak, peak_dbfs,
                    rms, rms_dbfs, (unsigned long long)dbg_clipped);
            dbg_samples = 0;
            dbg_sum_sq = 0.0;
            dbg_peak = 0;
            dbg_clipped = 0;
        }

        int rf_frames = 0;
        for(ssize_t i=0; i<got; i++){
            float x = (float)audio[i] / 32768.0f;
            x = dc_block_ff_process(&dc, x);
            x = fir_ff_process(&lpf, x);
            x = preemph_process(&pre, x);
            x = clampf(x, -1.0f, 1.0f);

            up_acc += RF_SR;
            while(up_acc >= TX_AUDIO_SR && rf_frames < max_rf_frames){
                up_acc -= TX_AUDIO_SR;
                cf32 s = fm_mod_process(&mod, x);
                rf[rf_frames * RF_CH + 0] = f_to_s32(s.re);
                if(RF_CH > 1) rf[rf_frames * RF_CH + 1] = f_to_s32(s.im);
                for(int c=2; c<RF_CH; c++) rf[rf_frames * RF_CH + c] = 0;
                rf_frames++;
            }
        }

        if(rf_frames > 0){
            pcm_write_all(pcm_rf_play, rf, (snd_pcm_uframes_t)rf_frames, RF_CH, sizeof(int32_t));
        }
    }

    free(audio);
    free(rf);
    tx_alive = 0;
    return NULL;
}

static void* rx_worker(void* _)
{
    (void)_;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    rx_alive = 1;

    const int rf_frames = RF_PERIOD > 0 ? RF_PERIOD : 4096;
    const int audio_frames = AUDIO_PERIOD > 0 ? AUDIO_PERIOD : 512;

    int32_t *rf = calloc((size_t)rf_frames * (size_t)RF_CH, sizeof(int32_t));
    int16_t *audio = calloc((size_t)audio_frames, sizeof(int16_t));
    if(!rf || !audio){
        free(rf);
        free(audio);
        rx_alive = 0;
        return NULL;
    }

    dc_block_cc_t dc;
    rotator_t rot;
    fir_decim_cc_t if_lpf;
    nbfm_demod_t demod;
    onepole_lp_t deemp;
    fir_ff_t audio_lpf;
    dc_block_cc_init(&dc, 0.999f);
    rotator_init(&rot, (float)RF_SR, RX_SHIFT_HZ);
    fir_decim_cc_init(&if_lpf, (float)RF_SR, 7500.0f);
    nbfm_demod_init(&demod);
    onepole_lp_init(&deemp, RX_DEEMPH_TAU, (float)RX_AUDIO_SR);
    fir_ff_init(&audio_lpf, (float)RX_AUDIO_SR, 2600.0f);

    int dec_if_phase = 0;
    int audio_pos = 0;
    float fs_if = (float)RF_SR / (float)RX_IF_DECIM;
    float demod_scale = fs_if / (2.0f * (float)M_PI * RX_MAX_DEV_HZ);
    double rx_resamp_step = fs_if / (double)RX_AUDIO_SR;
    double rx_next_out_t = 0.0;
    double rx_in_t = 0.0;
    float rx_prev_if = 0.0f;
    bool rx_have_prev = false;

    while(mode_has_rx(atomic_load(&cur_mode))){
        if(!pcm_rf_cap || !pcm_audio_out){ msleep(0.001); continue; }

        ssize_t got = pcm_read_frames(pcm_rf_cap, rf, (snd_pcm_uframes_t)rf_frames);
        if(got <= 0) continue;

        for(ssize_t i=0; i<got; i++){
            cf32 x = {
                .re = (float)rf[i * RF_CH + 0] / 2147483648.0f,
                .im = RF_CH > 1 ? (float)rf[i * RF_CH + 1] / 2147483648.0f : 0.0f
            };
            x = dc_block_cc_process(&dc, x);
            x = rotator_process(&rot, x);

            fir_decim_cc_push(&if_lpf, x);
            if(++dec_if_phase < RX_IF_DECIM) continue;
            dec_if_phase = 0;

            cf32 x_if = fir_decim_cc_compute(&if_lpf);
            float a_if = nbfm_demod(&demod, x_if) * demod_scale;

            if(!rx_have_prev){
                rx_prev_if = a_if;
                rx_have_prev = true;
                continue;
            }

            rx_in_t += 1.0;
            while(rx_next_out_t <= rx_in_t){
                double frac = rx_next_out_t - (rx_in_t - 1.0);
                if(frac < 0.0){
                    rx_next_out_t += rx_resamp_step;
                    continue;
                }
                if(frac > 1.0) break;

                float a = rx_prev_if + (float)frac * (a_if - rx_prev_if);
                a = onepole_lp_process(&deemp, a);
                a = fir_ff_process(&audio_lpf, a);
                a = clampf(a, -1.0f, 1.0f);

                audio[audio_pos++] = (int16_t)lrintf(a * 30000.0f);
                if(audio_pos >= audio_frames){
                    pcm_write_all(pcm_audio_out, audio, (snd_pcm_uframes_t)audio_pos, 1, sizeof(int16_t));
                    audio_pos = 0;
                }

                rx_next_out_t += rx_resamp_step;
            }
            rx_prev_if = a_if;
        }
    }

    if(audio_pos > 0 && pcm_audio_out){
        pcm_write_all(pcm_audio_out, audio, (snd_pcm_uframes_t)audio_pos, 1, sizeof(int16_t));
    }

    free(rf);
    free(audio);
    rx_alive = 0;
    return NULL;
}

// -------- Mode transitions --------
static void stop_workers_and_close_io(void)
{
    atomic_store(&cur_mode, MODE_IDLE);

    pthread_mutex_lock(&io_lock);
    alsa_drop_all();
    pthread_mutex_unlock(&io_lock);

    if(tx_thread_started){
        pthread_join(th_tx, NULL);
        tx_thread_started = 0;
    }
    if(rx_thread_started){
        pthread_join(th_rx, NULL);
        rx_thread_started = 0;
    }

    pthread_mutex_lock(&io_lock);
    alsa_close_all();
    pthread_mutex_unlock(&io_lock);
}

static int stop_radio(void)
{
    stop_workers_and_close_io();

    const char* stoparg = EV("CMD_ARG_STOP", "off");
    int rc = set_radio_mode(stoparg);
    if(rc != 0){
        fprintf(stderr, "setmode STOP failed using '%s'\n", stoparg);
    }
    return rc;
}

static int start_tx(void)
{
    stop_workers_and_close_io();

    const char* txarg = EV("CMD_ARG_TX", "tx");
    if(set_radio_mode(txarg) != 0){
        fprintf(stderr, "setmode TX failed using '%s'\n", txarg);
        return -1;
    }
    msleep(PRETX_S);

    pthread_mutex_lock(&io_lock);
    int rc = alsa_open_audio_capture();
    if(rc == 0) rc = alsa_open_rf_play();
    if(rc != 0){
        fprintf(stderr, "ALSA TX open failed: %s\n", snd_strerror(rc));
        alsa_close_all();
        pthread_mutex_unlock(&io_lock);
        return -3;
    }
    pthread_mutex_unlock(&io_lock);

    atomic_store(&cur_mode, MODE_TX);
    if(pthread_create(&th_tx, NULL, tx_worker, NULL) != 0){
        atomic_store(&cur_mode, MODE_IDLE);
        pthread_mutex_lock(&io_lock);
        alsa_drop_all();
        alsa_close_all();
        pthread_mutex_unlock(&io_lock);
        return -4;
    }
    tx_thread_started = 1;
    return 0;
}

static int start_rx(void)
{
    stop_workers_and_close_io();

    const char* rxarg = EV("CMD_ARG_RX", "rx");
    if(set_radio_mode(rxarg) != 0){
        fprintf(stderr, "setmode RX failed using '%s'\n", rxarg);
        return -1;
    }
    msleep(PRERX_S);

    pthread_mutex_lock(&io_lock);
    int rc = alsa_open_audio_playback();
    if(rc == 0) rc = alsa_open_rf_cap();
    if(rc != 0){
        fprintf(stderr, "ALSA RX open failed: %s\n", snd_strerror(rc));
        alsa_close_all();
        pthread_mutex_unlock(&io_lock);
        return -3;
    }
    pthread_mutex_unlock(&io_lock);

    atomic_store(&cur_mode, MODE_RX);
    if(pthread_create(&th_rx, NULL, rx_worker, NULL) != 0){
        atomic_store(&cur_mode, MODE_IDLE);
        pthread_mutex_lock(&io_lock);
        alsa_drop_all();
        alsa_close_all();
        pthread_mutex_unlock(&io_lock);
        return -4;
    }
    rx_thread_started = 1;
    return 0;
}

static int start_duplex(void)
{
    stop_workers_and_close_io();

    const char* duparg = EV("CMD_ARG_DUP", "dup");
    if(set_radio_mode(duparg) != 0){
        fprintf(stderr, "setmode DUP failed using '%s'\n", duparg);
        return -1;
    }
    double predup_s = EV_D("PREDUP", PRETX_S > PRERX_S ? PRETX_S : PRERX_S);
    msleep(predup_s);

    pthread_mutex_lock(&io_lock);
    int rc = alsa_open_audio_playback();
    if(rc == 0) rc = alsa_open_rf_cap();
    if(rc == 0) rc = alsa_open_audio_capture();
    if(rc == 0) rc = alsa_open_rf_play();
    if(rc != 0){
        fprintf(stderr, "ALSA DUP open failed: %s\n", snd_strerror(rc));
        alsa_close_all();
        pthread_mutex_unlock(&io_lock);
        return -3;
    }
    pthread_mutex_unlock(&io_lock);

    atomic_store(&cur_mode, MODE_DUP);
    if(pthread_create(&th_rx, NULL, rx_worker, NULL) != 0){
        atomic_store(&cur_mode, MODE_IDLE);
        pthread_mutex_lock(&io_lock);
        alsa_drop_all();
        alsa_close_all();
        pthread_mutex_unlock(&io_lock);
        return -4;
    }
    rx_thread_started = 1;
    if(pthread_create(&th_tx, NULL, tx_worker, NULL) != 0){
        stop_workers_and_close_io();
        return -4;
    }
    tx_thread_started = 1;
    return 0;
}

// -------- TCP control --------
static int startswith_ci(const char* s, const char* p)
{
    for(; *p && *s; s++, p++){
        if(toupper((unsigned char)*s) != toupper((unsigned char)*p)) return 0;
    }
    return *p == 0;
}

static void control_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){ perror("socket"); return; }
    srv_fd = fd;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)PORT);
    a.sin_addr.s_addr = inet_addr(HOST);
    if(bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0){
        perror("bind");
        close(fd);
        srv_fd = -1;
        return;
    }
    listen(fd, 8);

    char line[256];
    while(atomic_load(&want_run)){
        int c = accept(fd, NULL, NULL);
        if(c < 0){
            if(!atomic_load(&want_run)) break;
            if(errno == EINTR) continue;
            if(errno == EBADF) break;
            continue;
        }

        ssize_t n = recv(c, line, sizeof(line) - 1, 0);
        if(n <= 0){ close(c); continue; }
        line[n] = 0;

        char *sp = strchr(line, ' ');
        if(!sp){ send(c, "ERR\n", 4, 0); close(c); continue; }
        *sp = 0;
        char* tok = sp + 1;

        if(strcmp(line, SECRET) != 0){
            send(c, "ERR\n", 4, 0);
            close(c);
            continue;
        }

        if(startswith_ci(tok, "PING")){
            send(c, "PONG\n", 5, 0);
        } else if(startswith_ci(tok, "TX")){
            int rc = start_tx(); send(c, rc == 0 ? "OK\n" : "ERR\n", rc == 0 ? 3 : 4, 0);
        } else if(startswith_ci(tok, "RX")){
            int rc = start_rx(); send(c, rc == 0 ? "OK\n" : "ERR\n", rc == 0 ? 3 : 4, 0);
        } else if(startswith_ci(tok, "DUP")){
            int rc = start_duplex(); send(c, rc == 0 ? "OK\n" : "ERR\n", rc == 0 ? 3 : 4, 0);
        } else if(startswith_ci(tok, "STOP")){
            int rc = stop_radio(); send(c, rc == 0 ? "OK\n" : "ERR\n", rc == 0 ? 3 : 4, 0);
        } else {
            send(c, "ERR\n", 4, 0);
        }
        close(c);
    }

    close(fd);
    srv_fd = -1;
}

static void on_sig(int s)
{
    (void)s;
    atomic_store(&want_run, 0);
    atomic_store(&cur_mode, MODE_IDLE);
    if(srv_fd >= 0){
        close(srv_fd);
        srv_fd = -1;
    }
}

static void print_help(const char *prog)
{
    printf(
        "Usage: %s [--help|-h]\n"
        "\n"
        "SX1255 duplex repeater without ZMQ. Runtime configuration is done with environment variables.\n"
        "\n"
        "Startup:\n"
        "  START=RX|TX|DUP|STOP        Initial mode, default RX. STOP only starts the TCP control server.\n"
        "  SECRET=mytoken              TCP control shared secret, default mytoken.\n"
        "  HOST=0.0.0.0                TCP control listen address, default 0.0.0.0.\n"
        "  PORT=17020                  TCP control listen port, default 17020.\n"
        "\n"
        "TCP control commands:\n"
        "  Send one line: SECRET PING|RX|TX|DUP|STOP\n"
        "  Example: echo 'mytoken RX' | nc 127.0.0.1 17020\n"
        "\n"
        "SX1255 SPI mode control:\n"
        "  SPI_DEV=/dev/spidev0.0      SPI device, default /dev/spidev0.0.\n"
        "  SPI_SPEED_HZ=1000000        SPI speed, default 1000000.\n"
        "  CMD_ARG_RX=rx               Internal mode name for RX, default rx.\n"
        "  CMD_ARG_TX=tx               Internal mode name for TX, default tx.\n"
        "  CMD_ARG_DUP=dup             Internal mode name for duplex, default dup.\n"
        "  CMD_ARG_STOP=off            Internal mode name for STOP, default off.\n"
        "                              Accepted mode names: rx, tx, dup, idle, off.\n"
        "  PRERX=0.002                 Delay after RX setmode before opening ALSA, seconds.\n"
        "  PRETX=0.002                 Delay after TX setmode before opening ALSA, seconds.\n"
        "  PREDUP=max(PRETX,PRERX)     Delay after DUP setmode before opening ALSA, seconds.\n"
        "\n"
        "RF ALSA I/Q path:\n"
        "  PLAYBACK_DEV=hw:GenericStereoAu,0,0   SX1255 TX I/Q playback device.\n"
        "  CAPTURE_DEV=hw:GenericStereoAu,1,0    SX1255 RX I/Q capture device.\n"
        "  SR=125000                   RF I/Q sample rate, default 125000.\n"
        "  CH=2                        RF I/Q channels, default 2.\n"
        "  PERIOD=4096                 RF ALSA period frames, default 4096.\n"
        "  BUF_PERIODS=16              RF ALSA buffer periods, default 16.\n"
        "\n"
        "Audio ALSA path:\n"
        "  AUDIO_IN_DEV=plughw:Loopback,1,0      TX audio capture (from playback 0,0).\n"
        "  AUDIO_OUT_DEV=plughw:Loopback,0,1     RX audio playback (to capture 1,1).\n"
        "  TX_AUDIO_SR=16000           TX audio sample rate, default 16000.\n"
        "  RX_AUDIO_SR=16000           RX audio sample rate, default 16000.\n"
        "  AUDIO_PERIOD=512            Audio block/period frames, default 512.\n"
        "\n"
        "RX DSP:\n"
        "  RX_SHIFT_HZ=-25000          RX frequency shift before demod, default -25000.\n"
        "  RX_MAX_DEV_HZ=2500          RX FM deviation scaling, default 2500.\n"
        "  RX_DEEMPH_TAU=0.0001        RX deemphasis tau, default 100 us.\n"
        "\n"
        "TX DSP:\n"
        "  TX_GAIN=1.75                FM modulator gain, default 1.75.\n"
        "  TX_PREEMPH_TAU=0.000075     TX preemphasis tau, default 75 us.\n"
        "\n"
        "Examples:\n"
        "  %s\n"
        "  START=DUP PORT=17020 %s\n"
        "  START=STOP SECRET=abc %s\n",
        prog, prog, prog, prog
    );
}

int main(int argc, char **argv)
{
    if(argc > 1){
        if(strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0){
            print_help(argv[0]);
            return 0;
        }
        fprintf(stderr, "Unknown argument '%s'. Use --help.\n", argv[1]);
        return 2;
    }

    SECRET = EV("SECRET", "mytoken");
    SPI_DEV = EV("SPI_DEV", "/dev/spidev0.0");
    SPI_SPEED_HZ = (uint32_t)EV_I("SPI_SPEED_HZ", 1000000);
    PRETX_S = EV_D("PRETX", 0.002);
    PRERX_S = EV_D("PRERX", 0.002);

    RF_PLAY_DEV = EV("PLAYBACK_DEV", "hw:GenericStereoAu,0,0");
    RF_CAPT_DEV = EV("CAPTURE_DEV", "hw:GenericStereoAu,1,0");
    RF_SR = EV_I("SR", 125000);
    RF_CH = EV_I("CH", 2);
    RF_PERIOD = EV_I("PERIOD", 4096);
    RF_BUF_PERIODS = EV_I("BUF_PERIODS", 16);

    /*
     * Keep TX injection and RX monitoring on separate snd-aloop cables:
     *
     *   TX source playback  Loopback,0,0 -> capture Loopback,1,0 (this process)
     *   RX output playback  Loopback,0,1 -> capture Loopback,1,1 (consumer)
     *
     * Sharing Loopback,0,0 for both directions prevents RX from opening while
     * a persistent TX source such as ffmpeg owns that playback subdevice.
     */
    AUDIO_IN_DEV = EV("AUDIO_IN_DEV", "plughw:Loopback,1,0");
    AUDIO_OUT_DEV = EV("AUDIO_OUT_DEV", "plughw:Loopback,0,1");
    TX_AUDIO_SR = EV_I("TX_AUDIO_SR", 16000);
    RX_AUDIO_SR = EV_I("RX_AUDIO_SR", 16000);
    AUDIO_PERIOD = EV_I("AUDIO_PERIOD", 512);

    RX_SHIFT_HZ = (float)EV_D("RX_SHIFT_HZ", -25000.0);
    RX_MAX_DEV_HZ = (float)EV_D("RX_MAX_DEV_HZ", 2500.0);
    RX_DEEMPH_TAU = (float)EV_D("RX_DEEMPH_TAU", 100e-6);
    TX_GAIN = (float)EV_D("TX_GAIN", 1.75);
    TX_PREEMPH_TAU = (float)EV_D("TX_PREEMPH_TAU", 75e-6);

    HOST = EV("HOST", "0.0.0.0");
    PORT = EV_I("PORT", 17020);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    const char* START = EV("START", "RX");
    if(strcasecmp(START, "DUP") == 0) start_duplex();
    else if(strcasecmp(START, "RX") == 0) start_rx();
    else if(strcasecmp(START, "TX") == 0) start_tx();
    else if(strcasecmp(START, "STOP") == 0 || strcasecmp(START, "IDLE") == 0) {
        stop_radio();
    } else {
        fprintf(stderr, "Unknown START='%s' (use RX, TX, DUP or STOP)\n", START);
        return 2;
    }

    control_server();

    stop_radio();
    return 0;
}
