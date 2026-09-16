#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*============================================================================
 * SD-card WAV player: streams a 16-bit PCM WAV file to the MAX98357A.
 *
 * SPI / task constraints (see upload_server.h, lcd_driver.h, sd_card.h):
 * the SD card shares the SPI bus with the LCD, so ALL filesystem access must
 * happen on the LVGL task. This module therefore splits the work in two:
 *
 *   producer (LVGL task)   audio_player_play() + audio_player_poll()
 *                          fopen/fread the WAV, convert to 16-bit stereo,
 *                          apply the software volume, push into a ring
 *                          buffer (~93 ms, internal RAM: it is shared with
 *                          the audio task, which can run on the other core).
 *   consumer (audio task)  pops the ring into an internal staging buffer and
 *                          writes it to I2S. Never touches the filesystem.
 *
 * Call audio_player_poll() from an LVGL timer (~10 ms) on the LVGL task: it
 * refills the ring, detects end-of-file, and performs the teardown /
 * queued-track switch. ui_sd.c already drives it from its upload timer.
 *
 * Supported input: RIFF/WAVE, PCM (format 1, or WAVE_FORMAT_EXTENSIBLE whose
 * sub-format is PCM), 16-bit, mono or stereo, 8 kHz .. 96 kHz. Anything else
 * (MP3, 24/32-bit, float, ADPCM) is rejected with a log line and an error.
 *
 * The MAX98357A is a mono amplifier, so a stereo file plays through one of
 * the two I2S slots (whichever the module samples) — both slots carry the
 * real left/right data, no downmix is applied.
 *============================================================================*/

/* Longest absolute path accepted (matches ui_files.c's buffer: mount path +
 * entry name + '/' + NUL). */
#define AUDIO_PATH_MAX 320

/* Software volume applied while converting to the I2S stream, as a
 * percentage of full scale. Comes from CONFIG_AUDIO_PLAYER_VOLUME (menuconfig
 * -> "Audio Player Configuration"), default 40%: the MAX98357A module runs
 * 9 dB of gain, so a full-scale file at 100% clips on a small speaker. Set it
 * to 0 to mute the stream while the pipeline keeps running.
 * The audio page (step 2) will expose audio_player_set_volume() to the user. */
#ifndef CONFIG_AUDIO_PLAYER_VOLUME
#define CONFIG_AUDIO_PLAYER_VOLUME 40
#endif
#define AUDIO_PLAYER_DEFAULT_VOLUME CONFIG_AUDIO_PLAYER_VOLUME

/* Name (basename) buffer in audio_player_status_t */
#define AUDIO_PLAYER_NAME_MAX 63

typedef enum {
    AUDIO_PLAYER_IDLE = 0, /* nothing playing */
    AUDIO_PLAYER_PLAYING,  /* file open, I2S running */
    AUDIO_PLAYER_ERROR,    /* the last play attempt failed (see last_error) */
} audio_player_state_t;

typedef struct {
    audio_player_state_t state;
    char     file[AUDIO_PLAYER_NAME_MAX + 1]; /* basename of the track */
    uint32_t sample_rate;  /* Hz, from the WAV header */
    uint16_t channels;     /* 1 or 2 */
    uint32_t duration_ms;  /* 0 when the header carries no usable size */
    uint32_t position_ms;  /* approximate playback position */
    uint8_t  volume;       /* 0..100 */
    esp_err_t last_error;  /* ESP_OK unless state == AUDIO_PLAYER_ERROR */
} audio_player_status_t;

/**
 * @brief Initialize the player state. Takes no I2S/DMA/heap resources: the
 *        amp supply, the I2S channel and the ring buffer are set up when
 *        playback actually starts.
 * @note  Call once from app_main() before the first play request.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if called twice.
 */
esp_err_t audio_player_init(void);

/**
 * @brief Start playing a WAV file (non-blocking after the header is parsed).
 *
 * Must be called from the LVGL task: it opens the file and parses the WAV
 * header, both of which touch the shared SD SPI bus. On failure the player
 * keeps working and nothing is left open.
 *
 * If a track is already playing this one is queued and the current track is
 * stopped first; the queued file starts as soon as the audio task has exited
 * (typically within ~50 ms). Only the most recent queued path is kept.
 *
 * @param[in] path Absolute VFS path, e.g. "/sdcard/music/song.wav".
 * @return ESP_OK if playback started (or was queued), otherwise:
 *         ESP_ERR_NOT_FOUND      file missing / no SD card mounted
 *         ESP_ERR_NOT_SUPPORTED  not PCM 16-bit mono/stereo 8k..96k
 *         ESP_ERR_INVALID_SIZE   truncated or malformed header
 *         ESP_ERR_NO_MEM         ring buffer or audio task could not start
 */
esp_err_t audio_player_play(const char *path);

/**
 * @brief Request playback stop (asynchronous).
 *
 * Also drops any queued track. The audio task exits within one chunk
 * (<= ~25 ms) and audio_player_poll() then closes the file, releases I2S and
 * switches the amplifier rail off. Safe to call when nothing is playing.
 * @note LVGL task only.
 */
void audio_player_stop(void);

/**
 * @brief Clean up pending SD/audio work. MUST be called from the LVGL task,
 *        regularly (~10 ms), e.g. from an LVGL timer. ui_sd.c drives it.
 */
void audio_player_poll(void);

/**
 * @brief True while a file is open, playing or being torn down — i.e. while
 *        the player owns an SD file handle and the I2S channel.
 *
 * ui_sd.c uses this to postpone SD unmount/probe (which would reconfigure
 * the SPI bus or pull the FATFS volume out from under the open file).
 * @note LVGL task only (reads player state written by the same task).
 */
bool audio_player_is_active(void);

/**
 * @brief Set the software volume (applied to newly streamed samples).
 * @param[in] percent 0..100; values above 100 are clamped.
 * @note LVGL task only. Not exposed in the UI yet (step 2: audio page).
 */
void audio_player_set_volume(uint8_t percent);

/**
 * @brief Snapshot of the player state for the UI.
 * @param[out] out Filled on success.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if out is NULL.
 * @note LVGL task only.
 */
esp_err_t audio_player_get_status(audio_player_status_t *out);
