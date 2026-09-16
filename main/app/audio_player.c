#include "audio_player.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "max98357a.h"
#include "power.h"

static const char *TAG = "audio_player";

/*============================================================================
 * Tuning
 *============================================================================*/
/* Ring buffer of 16-bit stereo frames, already converted and volume-scaled.
 * 4096 frames ~ 93 ms at 44.1 kHz: deep enough to ride out a full-screen LCD
 * refresh (which blocks the LVGL task for ~30 ms) without an I2S underrun.
 * It lives in internal RAM (see ring_alloc()), so the size is capped by what
 * is free at playback time; 2048 frames is the accepted fallback. */
#define RING_FRAMES_MAX   4096
#define RING_FRAMES_MIN   2048
_Static_assert((RING_FRAMES_MAX & (RING_FRAMES_MAX - 1)) == 0 &&
                   (RING_FRAMES_MIN & (RING_FRAMES_MIN - 1)) == 0,
               "ring sizes must be powers of two");

#define READ_FRAMES       1024 /* max frames read from SD per fread() */
#define STAGE_FRAMES      1024 /* max frames handed to one I2S write */
#define MIN_FREE_TO_READ  256  /* skip the SD read if the ring is nearly full */
/* Frames the producer may pull from the SD card in a single LVGL tick.
 *
 * The audio poll runs from a "10 ms" LVGL timer, but that tick is only
 * nominal: one loop of the LVGL task adds an SD read (~5-12 ms here, the SD
 * card shares SPI3 with the LCD and each 4 KB fread spans FATFS sectors) plus
 * rendering. Measured effect of a single 1024-frame read per tick: the ring
 * never rose above empty and 11 s of playback produced 486 ring starvations
 * and 214 DMA underruns — i.e. constant, audible crackle. Allowing several
 * reads per tick lets the ring saturate (~93 ms of cushion) so the DMA stays
 * fed even when a tick takes 30+ ms. */
#define PRODUCE_BUDGET_FRAMES 2048
#define DRAIN_WAIT_MS     30   /* let the DMA play the tail before teardown */

#define AUDIO_TASK_STACK  3072
/* Above LVGL (5): the DMA backlog is only ~22 ms (4 x 240 frames), so a
 * 30 ms full-screen refresh on the LVGL task would starve the refill and
 * click. The consumer cannot hog the CPU — it blocks in the I2S write
 * waiting for DMA space, and waits 20 ms whenever the ring is empty.
 * The stack stays small because this task does no file I/O (the LVGL task
 * owns the SD card) — the high-water mark is logged when it exits. */
#define AUDIO_TASK_PRIO   6

#define WAV_MIN_RATE_HZ   8000
#define WAV_MAX_RATE_HZ   96000
#define WAV_MAX_CHUNKS    64 /* header walk limit: corrupt-file guard */

#define PCM_BYTES_PER_SAMPLE 2u

/* Sentinel for "the WAV header gave no usable data size: stream to EOF". */
#define DATA_UNKNOWN 0xFFFFFFFFu

/*============================================================================
 * State
 *============================================================================*/
typedef enum {
    PH_IDLE = 0,
    PH_PLAYING,  /* audio task running, ring being produced/consumed */
    PH_STOPPING, /* stop requested: waiting for the audio task to exit */
} phase_t;

typedef struct {
    uint16_t channels;    /* 1 or 2 */
    uint32_t sample_rate; /* Hz */
    uint32_t data_bytes;  /* PCM payload size, 0 = unknown */
} wav_info_t;

static bool s_initialized;

/* ---- filesystem / I2S state: written and read by the LVGL task only ---- */
static phase_t  s_phase = PH_IDLE;
static audio_player_state_t s_state = AUDIO_PLAYER_IDLE;
static FILE    *s_fp;
static uint16_t s_channels;
static uint32_t s_rate;
static uint32_t s_data_remaining; /* bytes of PCM left, or DATA_UNKNOWN */
static uint32_t s_total_frames;   /* PCM frames in the file (0 = unknown) */
static uint32_t s_frames_read;    /* PCM frames read so far */
static bool     s_eos;            /* no more data to read */
static TickType_t s_drain_tick;   /* when the ring ran dry after EOS */
static char     s_file[AUDIO_PLAYER_NAME_MAX + 1];
static uint8_t  s_volume = AUDIO_PLAYER_DEFAULT_VOLUME;
static esp_err_t s_last_error = ESP_OK;

/* Queued track (a tap on another file while playing) */
static char s_pending_path[AUDIO_PATH_MAX + 1];
static bool s_pending;

/* ---- shared with the audio task ---- */
static int16_t *s_ring;       /* s_ring_frames * 2 samples */
static uint32_t s_ring_frames; /* allocated capacity (power of two) */
static uint32_t s_ring_mask;   /* s_ring_frames - 1 */
static uint32_t s_head;     /* monotonic count of frames produced */
static uint32_t s_tail;     /* monotonic count of frames consumed */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_task_alive;
static volatile bool s_stop_req;
/* Streaming health, counted by the audio task and logged when a track ends.
 * Both should stay at 0 (the first DMA write of a track counts as 1 underrun
 * by design: the queue is empty when playback starts). */
static volatile uint32_t s_underrun_events;
static volatile uint32_t s_starve_events;
/* Wakes the consumer when the producer pushed data (or when a stop was
 * requested). Created on first playback and kept forever: the audio task is
 * created and deleted per track, so a task handle would go stale and must
 * never be used for notifications. */
static SemaphoreHandle_t s_ring_ready;

/* Consumer staging buffer: this is what gets handed to the I2S driver, so it
 * stays a static internal-RAM buffer. 4 KB. */
static int16_t s_stage[STAGE_FRAMES * 2];

/* Producer scratch (the SD read target): allocated together with the ring
 * instead of living in .bss. Internal RAM is the scarcest resource on this
 * board (the ring itself already falls back to PSRAM), and 4 KB of permanent
 * BSS is 4 KB the HTTP server, TLS and BLE cannot use. It is only ever
 * touched by the LVGL task, so PSRAM is fine for it. */
static int16_t *s_scratch; /* READ_FRAMES * 2 samples */

static esp_err_t player_start(const char *path);
static void request_stop(void);
static void player_teardown(void);

/*============================================================================
 * Helpers
 *============================================================================*/
static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/* Frames currently queued in the ring. */
static uint32_t ring_used(void)
{
    uint32_t used;
    portENTER_CRITICAL(&s_ring_mux);
    used = s_head - s_tail;
    portEXIT_CRITICAL(&s_ring_mux);
    return used;
}

/* Software volume: percent of full scale. 100% returns the sample untouched
 * (|s| <= 32767 and vol <= 100, so the int32 product cannot overflow). */
static inline int16_t apply_volume(int16_t s, uint8_t vol)
{
    if (vol >= 100) {
        return s;
    }
    return (int16_t)(((int32_t)s * (int32_t)vol) / 100);
}

/* Expand the raw PCM in s_scratch into 16-bit stereo frames and append them
 * to the ring (wrapping included). Mono is duplicated into both slots so the
 * MAX98357A plays it whichever slot it samples. */
static void ring_push_convert(uint32_t frames)
{
    const uint8_t vol = s_volume;
    uint32_t idx = s_head & s_ring_mask;

    for (uint32_t i = 0; i < frames; i++) {
        int16_t l;
        int16_t r;
        if (s_channels == 1) {
            l = apply_volume(s_scratch[i], vol);
            r = l;
        } else {
            l = apply_volume(s_scratch[2 * i], vol);
            r = apply_volume(s_scratch[2 * i + 1], vol);
        }
        s_ring[2 * idx]     = l;
        s_ring[2 * idx + 1] = r;
        idx = (idx + 1) & s_ring_mask;
    }

    portENTER_CRITICAL(&s_ring_mux);
    s_head += frames;
    portEXIT_CRITICAL(&s_ring_mux);
}

/*============================================================================
 * WAV header parsing (LVGL task; the file is positioned at the PCM data on
 * success)
 *============================================================================*/
static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static esp_err_t wav_parse(FILE *fp, wav_info_t *out, const char *path)
{
    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), fp) != sizeof(riff)) {
        ESP_LOGE(TAG, "%s: file too short for a RIFF header", path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "%s: not a RIFF/WAVE file", path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool have_fmt = false;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;

    for (int i = 0; i < WAV_MAX_CHUNKS; i++) {
        uint8_t hdr[8];
        if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
            break; /* no data chunk before EOF */
        }

        const uint32_t size = rd_u32le(hdr + 4);
        if (size > 0x7FFFFFF0u) {
            ESP_LOGE(TAG, "%s: implausible chunk size %lu", path,
                     (unsigned long)size);
            return ESP_ERR_INVALID_SIZE;
        }
        /* Payload plus the pad byte that keeps chunks word-aligned. */
        long skip = (long)size + (long)(size & 1u);

        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            size_t want = (size < sizeof(fmt)) ? size : sizeof(fmt);
            if (want < 16 || fread(fmt, 1, want, fp) != want) {
                ESP_LOGE(TAG, "%s: truncated fmt chunk", path);
                return ESP_ERR_INVALID_SIZE;
            }
            format   = rd_u16le(fmt + 0);
            channels = rd_u16le(fmt + 2);
            rate     = rd_u32le(fmt + 4);
            bits     = rd_u16le(fmt + 14);
            /* WAVE_FORMAT_EXTENSIBLE (0xFFFE): the real format sits in the
             * first two bytes of the sub-format GUID at offset 24. */
            if (format == 0xFFFE && want >= 26) {
                format = rd_u16le(fmt + 24);
            }
            have_fmt = true;
            skip -= (long)want;
        } else if (memcmp(hdr, "data", 4) == 0) {
            if (!have_fmt) {
                ESP_LOGE(TAG, "%s: data chunk before fmt chunk", path);
                return ESP_ERR_INVALID_SIZE;
            }
            if (format != 1) {
                ESP_LOGE(TAG, "%s: not linear PCM (format %u; MP3/ADPCM/"
                              "float are not supported)", path, (unsigned)format);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (bits != 16) {
                ESP_LOGE(TAG, "%s: %u-bit samples (only 16-bit PCM)",
                         path, (unsigned)bits);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (channels < 1 || channels > 2) {
                ESP_LOGE(TAG, "%s: %u channels (only mono/stereo)",
                         path, (unsigned)channels);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (rate < WAV_MIN_RATE_HZ || rate > WAV_MAX_RATE_HZ) {
                ESP_LOGE(TAG, "%s: %lu Hz out of range (%u..%u)", path,
                         (unsigned long)rate, WAV_MIN_RATE_HZ,
                         WAV_MAX_RATE_HZ);
                return ESP_ERR_NOT_SUPPORTED;
            }

            out->channels    = channels;
            out->sample_rate = rate;
            out->data_bytes  = (size == DATA_UNKNOWN) ? 0 : size;
            return ESP_OK; /* file is now positioned at the first sample */
        }

        if (skip > 0 && fseek(fp, skip, SEEK_CUR) != 0) {
            break;
        }
    }

    ESP_LOGE(TAG, "%s: no usable data chunk found", path);
    return ESP_ERR_NOT_FOUND;
}

/*============================================================================
 * Audio task: ring buffer -> I2S (consumer). Touches no filesystem at all.
 *============================================================================*/
static void audio_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio task started (prio %d)", AUDIO_TASK_PRIO);

    while (!s_stop_req) {
        uint32_t used = ring_used();

        if (used == 0) {
            /* Nothing queued. If the track is still streaming, the producer
             * is behind and the DMA is about to run dry: count it, it is the
             * signature of a hiss/crackle caused by starvation rather than by
             * the amplifier. */
            if (!s_eos && !s_stop_req && s_frames_read > 0) {
                s_starve_events++;
            }
            /* Wait for the producer to push, or 20 ms, so a stop request is
             * noticed even with an empty ring. */
            xSemaphoreTake(s_ring_ready, pdMS_TO_TICKS(20));
            continue;
        }
        if (used > STAGE_FRAMES) {
            used = STAGE_FRAMES;
        }

        /* Gather the (possibly wrapped) span into the staging buffer, then
         * release the space: the producer may refill while this task blocks
         * in the I2S write below. */
        const uint32_t base = s_tail & s_ring_mask;
        uint32_t first = s_ring_frames - base;
        if (first > used) {
            first = used;
        }
        memcpy(&s_stage[0], &s_ring[2 * base],
               (size_t)first * 2 * sizeof(int16_t));
        if (used > first) {
            memcpy(&s_stage[2 * first], &s_ring[0],
                   (size_t)(used - first) * 2 * sizeof(int16_t));
        }

        portENTER_CRITICAL(&s_ring_mux);
        s_tail += used;
        portEXIT_CRITICAL(&s_ring_mux);

        /* The write blocks until the driver has copied the data into its DMA
         * descriptors, so it normally takes about as long as this chunk of
         * audio lasts. Coming back much sooner means the DMA queue was
         * already empty — i.e. the output starved. */
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t wret = max98357a_write(s_stage, used);
        const int64_t elapsed_us = esp_timer_get_time() - t0;

        if (wret != ESP_OK) {
            /* Driver already logged the reason; let poll() tear it down. */
            s_stop_req = true;
            break;
        }
        if (s_rate > 0 &&
            elapsed_us < ((int64_t)used * 1000000) / (int64_t)s_rate / 2) {
            s_underrun_events++;
        }
    }

    s_task_alive = false;
    ESP_LOGI(TAG, "audio task exiting (stack high water: %u bytes)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

/*============================================================================
 * Producer (LVGL task): SD -> ring
 *============================================================================*/
static void producer_refill(void)
{
    uint32_t budget = PRODUCE_BUDGET_FRAMES;
    const uint32_t bytes_per_frame = PCM_BYTES_PER_SAMPLE * s_channels;

    while (budget > 0) {
        const uint32_t free_frames = s_ring_frames - ring_used();
        if (free_frames < MIN_FREE_TO_READ) {
            return; /* ring is full enough: no SD traffic this tick */
        }

        uint32_t want = (free_frames > READ_FRAMES) ? READ_FRAMES : free_frames;
        if (s_data_remaining != DATA_UNKNOWN) {
            const uint32_t frames_left = s_data_remaining / bytes_per_frame;
            if (frames_left < want) {
                want = frames_left;
            }
        }
        if (want == 0) {
            s_eos = true;
            return;
        }

        const size_t got =
            fread(s_scratch, 1, (size_t)want * bytes_per_frame, s_fp);
        const uint32_t frames = (uint32_t)(got / bytes_per_frame);

        if (frames == 0) {
            /* 0 bytes with data still expected: the card was removed or the
             * read failed. A clean end-of-file is handled by `want == 0`. */
            if (s_data_remaining != DATA_UNKNOWN && s_data_remaining > 0) {
                ESP_LOGE(TAG, "SD read failed with %lu bytes left",
                         (unsigned long)s_data_remaining);
                s_last_error = ESP_FAIL;
                s_state = AUDIO_PLAYER_ERROR;
            }
            s_eos = true;
            return;
        }

        if (s_data_remaining != DATA_UNKNOWN) {
            s_data_remaining -= frames * bytes_per_frame;
        }

        ring_push_convert(frames); /* s_scratch -> ring, volume applied */
        s_frames_read += frames;
        xSemaphoreGive(s_ring_ready);

        budget = (frames >= budget) ? 0 : (budget - frames);

        if (frames < want) {
            s_eos = true; /* short read: end of file */
            return;
        }
    }
}

/*============================================================================
 * Lifecycle (all on the LVGL task)
 *============================================================================*/
/* Allocate the streaming ring and publish its capacity.
 *
 * Internal RAM is tried first and PSRAM only as a last resort: the ring is
 * written by the LVGL task and read by the audio task, which can run on
 * different cores, and internal RAM is the memory whose cross-core visibility
 * is guaranteed. External RAM goes through per-core caches, so a spinlock
 * around the indices alone would not make a shared PSRAM ring coherent. */
static bool ring_alloc(void)
{
    static const uint32_t sizes[] = { RING_FRAMES_MAX, RING_FRAMES_MIN };
    const size_t scratch_bytes = (size_t)READ_FRAMES * 2 * sizeof(int16_t);

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const size_t ring_bytes = (size_t)sizes[i] * 2 * sizeof(int16_t);
        int16_t *p = heap_caps_malloc(ring_bytes + scratch_bytes,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p != NULL) {
            s_ring = p;
            s_scratch = p + (size_t)sizes[i] * 2; /* second half of the block */
            s_ring_frames = sizes[i];
            s_ring_mask = sizes[i] - 1;
            ESP_LOGI(TAG, "ring buffer: %u frames (%u KB) in internal RAM",
                     (unsigned)sizes[i], (unsigned)(ring_bytes / 1024));
            return true;
        }
    }

    const size_t ring_bytes = (size_t)RING_FRAMES_MAX * 2 * sizeof(int16_t);
    int16_t *p = heap_caps_malloc(ring_bytes + scratch_bytes, MALLOC_CAP_SPIRAM);
    if (p != NULL) {
        s_ring = p;
        s_scratch = p + (size_t)RING_FRAMES_MAX * 2;
        s_ring_frames = RING_FRAMES_MAX;
        s_ring_mask = RING_FRAMES_MAX - 1;
        ESP_LOGW(TAG, "ring buffer: %u KB in PSRAM (internal RAM is exhausted)",
                 (unsigned)(ring_bytes / 1024));
        return true;
    }

    ESP_LOGE(TAG, "ring buffer allocation failed (%u KB)",
             (unsigned)(ring_bytes / 1024));
    return false;
}

static esp_err_t player_start(const char *path)
{
    wav_info_t w;
    esp_err_t ret;

    s_last_error = ESP_OK;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "cannot open %s", path);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ret = wav_parse(fp, &w, path);
    if (ret != ESP_OK) {
        fclose(fp);
        goto fail;
    }

    if (s_ring == NULL && !ring_alloc()) {
        fclose(fp);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (s_ring_ready == NULL) {
        s_ring_ready = xSemaphoreCreateBinary();
        if (s_ring_ready == NULL) {
            ESP_LOGE(TAG, "cannot create the ring semaphore");
            fclose(fp);
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }
    }

    /* The amp supply rail belongs to the caller of the driver (see
     * max98357a.h): switch it on before I2S comes up, and give the MAX98357A
     * a moment to leave its shutdown state so the first frames are not lost
     * (the DMA starts clocking out data as soon as I2S is enabled). */
    ret = power_on(POWER_ID_AUDIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "amp power on failed: %s", esp_err_to_name(ret));
        fclose(fp);
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = max98357a_init(w.sample_rate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        power_off(POWER_ID_AUDIO);
        fclose(fp);
        goto fail;
    }

    /* Reset the streaming state before the consumer starts. */
    s_head = 0;
    s_tail = 0;
    s_stop_req = false;
    s_eos = false;
    s_drain_tick = 0;
    s_frames_read = 0;
    s_underrun_events = 0;
    s_starve_events = 0;
    s_channels = w.channels;
    s_rate = w.sample_rate;
    s_data_remaining = (w.data_bytes == 0) ? DATA_UNKNOWN : w.data_bytes;
    s_total_frames = (w.data_bytes == 0)
                         ? 0
                         : (w.data_bytes / (PCM_BYTES_PER_SAMPLE * w.channels));
    snprintf(s_file, sizeof(s_file), "%s", path_basename(path));

    /* Claim "task alive" before it exists: it is only cleared once the task
     * has really stopped touching I2S, and poll() must not tear the channel
     * down underneath a task that is still starting up. */
    s_task_alive = true;
    if (xTaskCreate(audio_task, "audio_play", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create audio task (internal RAM exhausted?)");
        s_task_alive = false;
        max98357a_deinit();
        power_off(POWER_ID_AUDIO);
        fclose(fp);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_fp = fp; /* owned by the player from here on */
    s_phase = PH_PLAYING;
    s_state = AUDIO_PLAYER_PLAYING;

    ESP_LOGI(TAG, "playing %s: %lu Hz, %u ch, %lu PCM bytes, volume %u%%",
             s_file, (unsigned long)s_rate, (unsigned)s_channels,
             (unsigned long)w.data_bytes, (unsigned)s_volume);
    /* Reported on every track so the cost of the player is visible next to
     * the features that share the same tight internal RAM (HTTP server,
     * TLS, BLE). */
    ESP_LOGI(TAG, "internal heap free at start: %lu KB",
             (unsigned long)esp_get_free_internal_heap_size() / 1024);
    return ESP_OK;

fail:
    s_last_error = ret;
    s_state = AUDIO_PLAYER_ERROR;
    s_phase = PH_IDLE;
    return ret;
}

/* Full teardown. Only valid once the audio task has exited (poll() checks
 * s_task_alive first): the I2S channel must not be deleted under a writer. */
static void player_teardown(void)
{
    max98357a_deinit(); /* no-op (invalid state) when I2S is already down */
    power_off(POWER_ID_AUDIO);

    if (s_fp != NULL) {
        fclose(s_fp);
        s_fp = NULL;
    }
    if (s_ring != NULL) {
        heap_caps_free(s_ring); /* one block: ring + producer scratch */
        s_ring = NULL;
        s_scratch = NULL;
    }
    /* Drop a wakeup left over from the track that just ended. */
    if (s_ring_ready != NULL) {
        while (xSemaphoreTake(s_ring_ready, 0) == pdTRUE) {
        }
    }

    s_stop_req = false;
    s_eos = false;
    s_drain_tick = 0;
    s_data_remaining = 0;
    s_frames_read = 0; /* position resets; rate/duration keep the last track */
    s_head = 0;
    s_tail = 0;
    s_phase = PH_IDLE;
    if (s_state == AUDIO_PLAYER_PLAYING) {
        s_state = AUDIO_PLAYER_IDLE;
    }

    ESP_LOGI(TAG, "playback stopped (dma underruns: %lu, ring starvations: %lu)",
             (unsigned long)s_underrun_events, (unsigned long)s_starve_events);
    ESP_LOGI(TAG, "internal heap free at stop: %lu KB",
             (unsigned long)esp_get_free_internal_heap_size() / 1024);
}

static void request_stop(void)
{
    if (s_phase != PH_PLAYING) {
        return;
    }
    s_stop_req = true;
    if (s_ring_ready != NULL) {
        xSemaphoreGive(s_ring_ready); /* wake it even with an empty ring */
    }
    s_phase = PH_STOPPING;
    ESP_LOGI(TAG, "stopping playback");
}

/* Start the queued track, if any. Called from poll() with the player idle. */
static void start_pending(void)
{
    if (!s_pending) {
        return;
    }
    char path[AUDIO_PATH_MAX + 1];
    snprintf(path, sizeof(path), "%s", s_pending_path);
    s_pending = false;
    s_pending_path[0] = '\0';

    (void)player_start(path); /* failure is recorded in s_state/s_last_error */
}

/*============================================================================
 * Public API
 *============================================================================*/
esp_err_t audio_player_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_phase = PH_IDLE;
    s_state = AUDIO_PLAYER_IDLE;
    s_volume = AUDIO_PLAYER_DEFAULT_VOLUME;
    s_head = 0;
    s_tail = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "ready (default volume %u%%)", (unsigned)s_volume);
    return ESP_OK;
}

esp_err_t audio_player_play(const char *path)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_phase != PH_IDLE) {
        /* Busy: remember the newest request and stop the current track.
         * poll() starts it once the audio task has exited. */
        snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
        s_pending = true;
        if (s_phase == PH_PLAYING) {
            request_stop();
        }
        ESP_LOGI(TAG, "queued %s", path);
        return ESP_OK;
    }

    return player_start(path);
}

void audio_player_stop(void)
{
    s_pending = false;
    s_pending_path[0] = '\0';
    if (s_phase == PH_PLAYING) {
        request_stop();
    }
}

bool audio_player_is_active(void)
{
    return (s_phase != PH_IDLE) || s_pending;
}

void audio_player_set_volume(uint8_t percent)
{
    s_volume = (percent > 100) ? 100 : percent;
}

esp_err_t audio_player_get_status(audio_player_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->state = s_state;
    memcpy(out->file, s_file, sizeof(out->file));
    out->sample_rate = s_rate;
    out->channels = s_channels;
    out->volume = s_volume;
    out->last_error = s_last_error;

    if (s_rate > 0) {
        if (s_total_frames > 0) {
            out->duration_ms =
                (uint32_t)(((uint64_t)s_total_frames * 1000u) / s_rate);
        }
        const uint32_t played = (s_frames_read > ring_used())
                                    ? (s_frames_read - ring_used())
                                    : 0;
        out->position_ms = (uint32_t)(((uint64_t)played * 1000u) / s_rate);
    }
    return ESP_OK;
}

void audio_player_poll(void)
{
    if (!s_initialized) {
        return;
    }

    /* Waiting for the audio task to exit: only then may the I2S channel be
     * deleted and the file be closed. */
    if (s_phase == PH_STOPPING) {
        if (!s_task_alive) {
            player_teardown();
            start_pending();
        }
        return;
    }

    if (s_phase == PH_IDLE) {
        start_pending();
        return;
    }

    /* PH_PLAYING */
    if (s_stop_req) {
        /* Set by the audio task after an I2S write error. */
        ESP_LOGW(TAG, "stopping after an I2S error");
        s_last_error = ESP_FAIL;
        s_state = AUDIO_PLAYER_ERROR;
        request_stop();
        return;
    }

    producer_refill();

    if (s_eos && ring_used() == 0) {
        /* Everything has been handed to the DMA: give it DRAIN_WAIT_MS to
         * play the tail before the amp is switched off. */
        if (s_drain_tick == 0) {
            s_drain_tick = xTaskGetTickCount();
        } else if ((xTaskGetTickCount() - s_drain_tick) >=
                   pdMS_TO_TICKS(DRAIN_WAIT_MS)) {
            ESP_LOGI(TAG, "end of file: %s", s_file);
            request_stop();
        }
    } else {
        s_drain_tick = 0;
    }
}
