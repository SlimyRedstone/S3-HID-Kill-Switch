/*
 * Tone synthesis and playback. Codec setup mirrors Waveshare's BSP for the
 * ESP32-S3-CAM-OVxxxx (bsp_audio_codec_speaker_init), TX channel only.
 */

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "driver/i2s_std.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "melody.h"

static const char *TAG = "melody";

#define QUEUE_DEPTH     6
#define TASK_STACK      4096
#define TASK_PRIO       5

/*
 * DMA ring: 4 x 240 frames = 60 ms at 16 kHz. Audio handed to the driver is
 * still in this ring when esp_codec_dev_write() returns, so the tail of every
 * melody is pushed out with that much silence before the amplifier goes off.
 */
#define DMA_DESC_NUM    4
#define DMA_FRAME_NUM   240
#define TAIL_FRAMES     (DMA_DESC_NUM * DMA_FRAME_NUM + CHUNK_SAMPLES)

#define CHUNK_SAMPLES   256
#define AMPLITUDE       0.55f           /* of full scale, before codec volume */
#define ATTACK_MS       4
#define RELEASE_MS      12

#define WAVE_BITS       8
#define WAVE_SIZE       (1u << WAVE_BITS)

/* ---------------------------------------------------------- melodies ---- */

static const melody_note_t TUNE_BOOT[] = {
    { 1047, 70 }, { 1319, 70 }, { 1568, 70 }, { 2093, 120 },
};
static const melody_note_t TUNE_SCAN[] = {
    { 1760, 45 },
};
static const melody_note_t TUNE_SUCCESS[] = {
    { 1319, 90 }, { 0, 15 }, { 1568, 90 }, { 0, 15 }, { 2093, 220 },
};
static const melody_note_t TUNE_ERROR[] = {
    { 784, 130 }, { 0, 35 }, { 622, 130 }, { 0, 35 }, { 466, 320 },
};
static const melody_note_t TUNE_NO_HOST[] = {
    { 523, 110 }, { 0, 90 }, { 523, 110 },
};

typedef struct {
    const melody_note_t *notes;
    size_t               count;
} job_t;

#define TUNE(x) { (x), sizeof(x) / sizeof((x)[0]) }

static const job_t BUILTIN[MELODY_COUNT] = {
    [MELODY_BOOT]    = TUNE(TUNE_BOOT),
    [MELODY_SCAN]    = TUNE(TUNE_SCAN),
    [MELODY_SUCCESS] = TUNE(TUNE_SUCCESS),
    [MELODY_ERROR]   = TUNE(TUNE_ERROR),
    [MELODY_NO_HOST] = TUNE(TUNE_NO_HOST),
};

/* ------------------------------------------------------------- state ---- */

static melody_config_t        s_cfg;
static i2s_chan_handle_t      s_tx;
static esp_codec_dev_handle_t s_codec;
static QueueHandle_t          s_queue;

/* Jobs queued or rendering, and whether the amplifier is on. wait_idle
   needs both: the count drops to zero before the tail has played. */
static atomic_int  s_pending;
static atomic_bool s_playing;

static int16_t s_wave[WAVE_SIZE];
static int16_t s_buf[CHUNK_SAMPLES];

/* ------------------------------------------------------------ synth ----- */

static float wave_shape(float x)
{
    return sinf(x) + sinf(3.0f * x) / 3.0f;
}

static void build_wave(void)
{
    float peak = 0.0f;
    for (uint32_t i = 0; i < WAVE_SIZE; i++) {
        float v = fabsf(wave_shape(2.0f * (float)M_PI * i / WAVE_SIZE));
        if (v > peak) {
            peak = v;
        }
    }

    const float scale = 32767.0f * AMPLITUDE / peak;
    for (uint32_t i = 0; i < WAVE_SIZE; i++) {
        s_wave[i] = (int16_t)lrintf(wave_shape(2.0f * (float)M_PI * i / WAVE_SIZE) * scale);
    }
}

static void write_chunk(size_t samples)
{
    esp_codec_dev_write(s_codec, s_buf, (int)(samples * sizeof(int16_t)));
}

static void write_silence(uint32_t samples)
{
    memset(s_buf, 0, sizeof(s_buf));
    while (samples) {
        const size_t n = samples < CHUNK_SAMPLES ? samples : CHUNK_SAMPLES;
        write_chunk(n);
        samples -= n;
    }
}

static void render_note(const melody_note_t *note)
{
    const uint32_t sr    = s_cfg.sample_rate;
    const uint32_t total = sr * note->ms / 1000;

    if (note->freq_hz == 0 || note->freq_hz >= sr / 2) {
        write_silence(total);
        return;
    }

    uint32_t attack  = sr * ATTACK_MS / 1000;
    uint32_t release = sr * RELEASE_MS / 1000;
    if (attack + release > total) {
        attack  = total / 3;
        release = total / 3;
    }

    /* 32-bit phase accumulator; the top WAVE_BITS index the table. */
    const uint32_t step = (uint32_t)(((uint64_t)note->freq_hz << 32) / sr);
    uint32_t phase = 0;

    for (uint32_t done = 0; done < total;) {
        const size_t n = (total - done) < CHUNK_SAMPLES ? (total - done) : CHUNK_SAMPLES;

        for (size_t i = 0; i < n; i++) {
            const uint32_t t = done + (uint32_t)i;
            int32_t gain = 256;                             /* Q8 envelope */
            if (attack && t < attack) {
                gain = (int32_t)(t * 256 / attack);
            } else if (release && t + release >= total) {
                gain = (int32_t)((total - t) * 256 / release);
            }
            s_buf[i] = (int16_t)((s_wave[phase >> (32 - WAVE_BITS)] * gain) >> 8);
            phase += step;
        }

        write_chunk(n);
        done += (uint32_t)n;
    }
}

/* ------------------------------------------------------------- task ----- */

static void amp(bool on)
{
    if (s_cfg.amp) {
        s_cfg.amp(on, s_cfg.amp_ctx);
    }
}

static void melody_task(void *arg)
{
    (void)arg;
    job_t job;

    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        atomic_store(&s_playing, true);
        amp(true);
        if (s_cfg.amp_settle_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_cfg.amp_settle_ms) ? pdMS_TO_TICKS(s_cfg.amp_settle_ms) : 1);
        }

        /* Back-to-back jobs share one amplifier cycle. */
        do {
            for (size_t i = 0; i < job.count; i++) {
                render_note(&job.notes[i]);
            }
            atomic_fetch_sub(&s_pending, 1);
        } while (xQueueReceive(s_queue, &job, 0) == pdTRUE);

        write_silence(TAIL_FRAMES);
        amp(false);
        atomic_store(&s_playing, false);
    }
}

/* -------------------------------------------------------------- API ----- */

esp_err_t melody_init(const melody_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->i2c_bus && cfg->sample_rate, ESP_ERR_INVALID_ARG,
                        TAG, "bad config");
    ESP_RETURN_ON_FALSE(s_queue == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_cfg = *cfg;
    build_wave();

    esp_err_t ret = ESP_FAIL;   /* written by the ESP_GOTO_ON_* macros */
    const audio_codec_data_if_t *data_if = NULL;
    const audio_codec_ctrl_if_t *ctrl_if = NULL;
    const audio_codec_if_t      *codec_if = NULL;

    /* -- I2S, master, TX only. The ES8311 takes MCLK from us. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(cfg->i2s_port, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num        = DMA_DESC_NUM;
    chan_cfg.dma_frame_num       = DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;    /* underrun sends zeros, not stale audio */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = cfg->mclk,
            .bclk = cfg->bclk,
            .ws   = cfg->ws,
            .dout = cfg->dout,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), fail, TAG, "i2s std mode");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_tx), fail, TAG, "i2s enable");

    /* -- ES8311: control on I2C, data on I2S, no PA pin (ours is on EXIO). */
    audio_codec_i2s_cfg_t i2s_if_cfg = {
        .port      = (uint8_t)cfg->i2s_port,
        .tx_handle = s_tx,
    };
    data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    ESP_GOTO_ON_FALSE(data_if, ESP_FAIL, fail, TAG, "codec data if");

    audio_codec_i2c_cfg_t i2c_if_cfg = {
        .addr       = ES8311_CODEC_DEFAULT_ADDR,    /* 8-bit form of 0x18 */
        .bus_handle = cfg->i2c_bus,
    };
    ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    ESP_GOTO_ON_FALSE(ctrl_if, ESP_FAIL, fail, TAG, "codec ctrl if (ES8311 not answering?)");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = -1,
        .master_mode = false,
        .use_mclk    = true,
        .hw_gain     = {
            .pa_voltage        = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    codec_if = es8311_codec_new(&es_cfg);
    ESP_GOTO_ON_FALSE(codec_if, ESP_FAIL, fail, TAG, "es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_GOTO_ON_FALSE(s_codec, ESP_FAIL, fail, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = cfg->sample_rate,
    };
    ESP_GOTO_ON_FALSE(esp_codec_dev_open(s_codec, &fs) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "codec open");
    esp_codec_dev_set_out_vol(s_codec, cfg->volume);

    /* -- playback task */
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(job_t));
    ESP_GOTO_ON_FALSE(s_queue, ESP_ERR_NO_MEM, fail_open, TAG, "queue");
    ESP_GOTO_ON_FALSE(xTaskCreate(melody_task, "melody", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                      ESP_ERR_NO_MEM, fail_queue, TAG, "task");

    ESP_LOGI(TAG, "ES8311 ready: %u Hz, volume %d", (unsigned)cfg->sample_rate, cfg->volume);
    return ESP_OK;

fail_queue:
    vQueueDelete(s_queue);
    s_queue = NULL;
fail_open:
    esp_codec_dev_close(s_codec);
fail:
    if (s_codec) {
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (codec_if) {
        audio_codec_delete_codec_if(codec_if);
    }
    if (ctrl_if) {
        audio_codec_delete_ctrl_if(ctrl_if);
    }
    if (data_if) {
        audio_codec_delete_data_if(data_if);
    }
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    return ret;
}

esp_err_t melody_play_notes(const melody_note_t *notes, size_t count)
{
    ESP_RETURN_ON_FALSE(notes && count, ESP_ERR_INVALID_ARG, TAG, "empty melody");
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_INVALID_STATE, TAG, "not started");

    const job_t job = { notes, count };
    atomic_fetch_add(&s_pending, 1);
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        atomic_fetch_sub(&s_pending, 1);
        ESP_LOGW(TAG, "queue full, melody dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t melody_play(melody_id_t id)
{
    ESP_RETURN_ON_FALSE((unsigned)id < MELODY_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad id %d", id);
    return melody_play_notes(BUILTIN[id].notes, BUILTIN[id].count);
}

void melody_flush(void)
{
    if (s_queue == NULL) {
        return;
    }
    job_t job;
    while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
        atomic_fetch_sub(&s_pending, 1);
    }
}

esp_err_t melody_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "not started");
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return esp_codec_dev_set_out_vol(s_codec, volume) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t melody_wait_idle(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    while (atomic_load(&s_pending) > 0 || atomic_load(&s_playing)) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}
