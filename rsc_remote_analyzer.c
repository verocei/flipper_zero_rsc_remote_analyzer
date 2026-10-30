/*
 * RSC Remote Analyzer - automatic Sub-GHz remote control analyzer for Flipper Zero (Momentum).
 *
 * One long press on the remote: the app finds the frequency (coarse sweep on the setting list,
 * then a narrow-band fine sweep), then tries the four receive presets one after another
 * (AM270, AM650, FM238, FM476) with every decoder of the firmware, and shows the result.
 * If the remote stops sending before a preset is tested, it asks for another press and goes on
 * with the results it already has.
 *
 * Receive only: the app never transmits.
 */

#include <stdarg.h>
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>
#include <datetime/datetime.h>

#include <math.h>

#include <lib/subghz/receiver.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/cc1101_configs.h>

#define TAG "RscRemoteAnalyzer"

#define APP_NAME    "RSC Remote Analyzer"
#define APP_VERSION "0.10"

#define DATA_DIR      EXT_PATH("apps_data/rsc_remote_analyzer")
#define REPORT_DIR    DATA_DIR "/reports"
#define SETTINGS_PATH DATA_DIR "/settings.txt"
#define TRACE_DIR     DATA_DIR "/debug_logs"
#define TRACE_OLD     DATA_DIR "/debug_log.txt" /* single log of 0.3-0.4 */
#define SUB_DIR       EXT_PATH("subghz") /* where the Sub-GHz app lists its own saves */

#define SETTINGS_FILE_TYPE "RSC Remote Analyzer Settings"
#define REPORT_FILE_TYPE   "RSC Remote Analyzer Report"

#define PRESET_COUNT 4
#define CAND_MAX     6
#define RAW_MAX      2048
#define RAW_LIMIT    32767 /* longer pulses and gaps are stored as this (a pause between packets) */
#define FREQ_MAX     255
#define NAME_LEN     32

/* Timing of the modulation search (ms) */
#define TICK_MS        5
#define SILENCE_MS     350 /* no signal this long = the remote stopped sending */
#define PROMPT_MS      400 /* waiting this long for a signal = ask for another press */
#define MIN_SPAN_MS    250 /* a preset counts as tested after this much signal */
#define MAX_ATTEMPTS   2 /* ... or after this many short presses */
#define REFIND_MS      5000 /* no signal at all on the found frequency: search again */
#define MODEL_EVERY_MS 50
#define ACTIVE_BELOW_DB 10.0f /* a press counts from this far below the found level ... */
#define ACTIVE_MIN_DB   10.0f /* ... but at most this far below the sensitivity setting */

/* Debug log: the last TRACE_LINES lines of a scan, written to TRACE_DIR when it ends; the last
 * TRACE_KEEP scans are kept, debug_log_1.txt is the newest */
#define TRACE_LINES    160
#define TRACE_LEN      48
#define TRACE_EVERY_MS 100
#define TRACE_KEEP     5u

/* Fine frequency search */
#define FINE_SPAN_HZ   300000
#define FINE_STEP_HZ   20000
#define FINE2_STEP_HZ  5000
#define SNAP_HZ        75000
#define SWEEP_DECAY_DB 1.0f /* the per-frequency peaks of the coarse sweep fade this much per pass */
#define HARMONIC_PPM   1000 /* 2 x f within 0.1 % = second harmonic of f */
#define FINE_STEPS_MAX 41 /* steps of one fine round (2 x FINE_SPAN_HZ / FINE_STEP_HZ + 1 fits) */
#define FINE_ROUNDS    8 /* the fine search repeats its round at most this many times ... */
#define FINE2_ROUNDS   5 /* ... the second, narrower one this many */
#define FINE_HITS      3 /* ... and stops after this many rounds that heard the remote */
#define FINE_HIT_DB    6.0f /* a round heard the remote: a step this far under the threshold or more */
#define CC1101_MDMCFG4 0x10

/* Automatic sensitivity: a signal is measured against the noise of its own frequency and preset */
#define AUTO_MIN_DBM         (-95.0f) /* never count anything weaker than this */
#define AUTO_SWEEP_MARGIN_DB 12.0f /* a list frequency's peak this far over its noise = a signal */
#define AUTO_FLOOR_RISE_DB   0.5f /* a frequency's noise estimate rises at most this much per pass */
#define AUTO_PRESS_MARGIN_DB 10.0f /* a press = this far over the preset's noise */
#define AUTO_NOISE_EMA       0.02f /* how fast a preset's noise estimate follows the quiet readings */
#define AUTO_NOISE_SPAN_DB   10.0f /* a preset's noise estimate stays this close to its typical level */
#define FIXED_NOISE_DB       6.0f /* a fixed sensitivity never goes closer than this to the measured noise */

/* UI language: index into the [LANG_COUNT] string arrays below; L() picks one of three texts */
#define LANG_EN    0
#define LANG_HU    1
#define LANG_DE    2
#define LANG_COUNT 3
static uint32_t g_lang = LANG_EN;
#define L(en, hu, de) (g_lang == LANG_HU ? (hu) : g_lang == LANG_DE ? (de) : (en))

#define FLAG_STOP   (1UL << 0)
#define FLAG_FINISH (1UL << 1)

/* ------------------------------------------------------------------------- */
/* Presets                                                                    */

typedef struct {
    const char* name;
    FuriHalSubGhzPreset preset;
    float noise; /* typical RSSI without a signal (measured on a Flipper, 433.92 MHz) */
} PresetDef;

static const PresetDef presets[PRESET_COUNT] = {
    {"AM270", FuriHalSubGhzPresetOok270Async, -96.0f},
    {"AM650", FuriHalSubGhzPresetOok650Async, -88.0f},
    {"FM238", FuriHalSubGhzPreset2FSKDev238Async, -100.0f},
    {"FM476", FuriHalSubGhzPreset2FSKDev476Async, -100.0f},
};

/* Tie-break between presets with equal results: the most common ones first */
static const uint8_t preset_rank[PRESET_COUNT] = {2, 4, 3, 1};

/* ------------------------------------------------------------------------- */
/* Settings                                                                   */

/* Index 0 is automatic (SENSITIVITY_AUTO): the threshold follows the measured noise */
#define SENSITIVITY_AUTO 0
static const int8_t sensitivity_dbm[] = {0, -65, -75, -85};
static const char* const sensitivity_names[LANG_COUNT][4] = {
    {"Auto", "Low", "Normal", "High"},
    {"Auto", "Alacsony", "Normal", "Magas"},
    {"Auto", "Niedrig", "Normal", "Hoch"},
};
static const uint16_t window_ms[] = {600, 1000, 1600};
static const char* const window_names[LANG_COUNT][3] = {
    {"Short", "Normal", "Long"},
    {"Rovid", "Normal", "Hosszu"},
    {"Kurz", "Normal", "Lang"},
};
static const char* const yes_no[LANG_COUNT][2] = {{"No", "Yes"}, {"Nem", "Igen"}, {"Nein", "Ja"}};
static const char* const on_off[LANG_COUNT][2] = {{"Off", "On"}, {"Ki", "Be"}, {"Aus", "An"}};
static const char* const language_names[LANG_COUNT] = {"English", "Magyar", "Deutsch"};
static const char* const manual_preset_names[] = {"Auto", "AM270", "AM650", "FM238", "FM476"};

typedef struct {
    uint32_t sensitivity; /* index into sensitivity_dbm, SENSITIVITY_AUTO = automatic */
    uint32_t window; /* index into window_ms */
    uint32_t all_presets; /* 1 = always try all four presets */
    uint32_t feedback; /* 1 = vibro/LED feedback */
    uint32_t debug_log; /* 1 = write the debug log of every scan */
    uint32_t language; /* LANG_EN, LANG_HU or LANG_DE */
} Settings;

/* ------------------------------------------------------------------------- */
/* Analysis state                                                             */

typedef struct {
    bool done;
    bool signal_seen;
    uint8_t attempts;
    uint16_t decodes;
    uint32_t span_ms; /* longest signal span seen while testing */
    float rssi_max;
    uint32_t pulses_ok;
    uint32_t pulses_glitch;
    uint32_t pulses_long;
} PresetResult;

typedef struct {
    bool used;
    char name[NAME_LEN];
    SubGhzProtocolType type;
    uint16_t count;
    uint16_t repeats; /* decodes identical to the previous one */
    uint16_t changes; /* decodes different from the previous one */
    uint32_t last_hash;
    uint16_t per_preset[PRESET_COUNT];
    uint8_t last_preset;
    FuriString* info;
    FlipperFormat* ff; /* the latest decode, serialized as a .sub key file */
} Candidate;

typedef enum {
    PhaseFreq,
    PhaseMod,
    PhaseDone,
} Phase;

typedef struct {
    Phase phase;
    bool manual;
    uint8_t only_preset; /* 0xFF = all */
    bool freq_found;
    uint32_t freq_nominal;
    uint32_t freq_measured;
    float rssi_peak;
    float found_level; /* the level the frequency search found the remote at */
    int8_t last_cand; /* candidate of the latest decode, -1 = none */
    bool any_activity; /* signal seen during the modulation search */
    uint8_t current;
    PresetResult pr[PRESET_COUNT];
    Candidate cand[CAND_MAX];
    int16_t* raw_cur; /* RAW capture in us, +/- = level; 16 bit to save RAM (see RAW_LIMIT) */
    size_t raw_cur_n;
    int16_t* raw_best;
    size_t raw_best_n;
    uint8_t raw_best_preset;
    uint32_t raw_best_score;
} Analysis;

typedef struct {
    bool started;
    uint32_t first;
    uint32_t last;
    uint32_t wait_since;
    bool prompted;
    uint16_t decodes_seen; /* decodes of the preset already counted as activity */
} Window;

/* ------------------------------------------------------------------------- */
/* Views                                                                      */

typedef enum {
    ViewMain,
    ViewScan,
    ViewResult,
    ViewDetails,
    ViewText,
    ViewTextInput,
    ViewSettings,
    ViewManual,
} ViewId;

typedef enum {
    EvMenuAuto = 1,
    EvMenuManual,
    EvMenuSaved,
    EvMenuSettings,
    EvScanDone,
    EvScanFinish,
    EvScanCancel,
    EvResultNew,
    EvResultSave,
    EvResultMore,
    EvDetailData,
    EvDetailModulation,
    EvDetailCompat,
    EvNameDone,
    EvManualStart,
    EvLanguage,
} EventId;

#define MAIN_ITEMS 4

typedef struct {
    uint8_t selected;
} MainModel;

typedef enum {
    MarkPending,
    MarkTested,
    MarkDecoded,
    MarkSkipped,
} Mark;

typedef struct {
    Phase phase;
    bool freq_known;
    uint32_t freq;
    float rssi;
    uint8_t current;
    uint8_t marks[PRESET_COUNT];
    char protocol[NAME_LEN];
    char status[40];
    bool prompt;
} ScanModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    NotificationApp* notif;
    Storage* storage;
    DialogsApp* dialogs;

    View* main_view;
    View* scan_view;
    Widget* result;
    Submenu* details;
    TextBox* text_box;
    TextInput* text_input;
    VariableItemList* settings_list;
    VariableItemList* manual_list;
    ViewId current_view;
    ViewId text_return;

    FuriString* text;
    FuriString* summary;
    char name_buf[NAME_LEN];

    Settings settings;
    uint8_t manual_freq;
    uint8_t manual_preset;

    const SubGhzDevice* device;
    SubGhzEnvironment* env;
    SubGhzReceiver* receiver;
    SubGhzWorker* sworker;
    SubGhzSetting* setting;
    uint32_t freqs[FREQ_MAX];
    size_t freq_count;
    uint8_t* narrow_preset;
    bool rx_running;

    FuriThread* thread;
    FuriMutex* mutex;
    Analysis an;
    Window win;
    volatile bool counting;
    float threshold;
    float sweep_peak[FREQ_MAX]; /* coarse sweep: fading peak per frequency of the list */
    float sweep_floor[FREQ_MAX]; /* coarse sweep: noise estimate per frequency (automatic) */
    bool auto_sens;
    float preset_floor; /* noise estimate of the preset being tested */
    float fixed_press; /* fixed sensitivity: the press threshold before the noise guard */
    float active_threshold; /* what counts as a press in the modulation search */
    uint32_t last_model;
    uint32_t scan_t0;
    DateTime scan_time;
    char (*trace)[TRACE_LEN];
    uint32_t trace_n;
    uint32_t last_trace;
    FuriString* preset_name;
    int8_t best;
} App;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */

static uint32_t now_ms(void) {
    return furi_get_tick() * 1000 / furi_kernel_get_tick_frequency();
}

/* One line of the debug log (worker thread only; saved after the worker stopped) */
static void trace(App* app, const char* fmt, ...) {
    if(!app->trace) return;
    char* line = app->trace[app->trace_n % TRACE_LINES];
    int n = snprintf(line, TRACE_LEN, "%6lu ", (unsigned long)(now_ms() - app->scan_t0));
    if(n < 0 || n >= TRACE_LEN) n = 0;
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, TRACE_LEN - n, fmt, args);
    va_end(args);
    app->trace_n++;
}

static void format_freq(char* out, size_t size, uint32_t f) {
    snprintf(
        out,
        size,
        "%lu.%02lu MHz",
        (unsigned long)(f / 1000000),
        (unsigned long)((f % 1000000) / 10000));
}

static void format_freq_long(FuriString* out, uint32_t f) {
    furi_string_cat_printf(
        out,
        "%lu.%03lu MHz",
        (unsigned long)(f / 1000000),
        (unsigned long)((f % 1000000) / 1000));
}

static void feedback(App* app, const NotificationSequence* seq) {
    if(app->settings.feedback) notification_message(app->notif, seq);
}

static const char* type_text(SubGhzProtocolType type) {
    switch(type) {
    case SubGhzProtocolTypeStatic:
        return L("Fixed code", "Fix kodos", "Festcode");
    case SubGhzProtocolTypeDynamic:
        return L("Rolling code", "Ugrokodos", "Rolling Code");
    default:
        return L("Not determined", "Nem meghatarozott", "Nicht bestimmt");
    }
}

static const char* type_key(SubGhzProtocolType type) {
    switch(type) {
    case SubGhzProtocolTypeStatic:
        return "Static";
    case SubGhzProtocolTypeDynamic:
        return "Dynamic";
    default:
        return "Other";
    }
}

/* ------------------------------------------------------------------------- */
/* Settings file                                                              */

static void settings_defaults(Settings* s) {
    s->sensitivity = SENSITIVITY_AUTO;
    s->window = 1;
    s->all_presets = 1;
    s->feedback = 1;
    s->language = LANG_EN;
    s->debug_log = 0;
}

static void settings_load(App* app) {
    settings_defaults(&app->settings);
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* header = furi_string_alloc();
    uint32_t version = 0;
    if(flipper_format_file_open_existing(ff, SETTINGS_PATH) &&
       flipper_format_read_header(ff, header, &version) &&
       furi_string_equal(header, SETTINGS_FILE_TYPE)) {
        uint32_t v;
        if(flipper_format_read_uint32(ff, "Sensitivity", &v, 1)) {
            /* Version 1 had Low / Normal / High at 0..2 with Normal as the default; the default
             * becomes Auto, the other two keep their meaning */
            if(version < 2) v = v == 1 ? SENSITIVITY_AUTO : v + 1;
            if(v < COUNT_OF(sensitivity_dbm)) app->settings.sensitivity = v;
        }
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Window", &v, 1) && v < COUNT_OF(window_ms))
            app->settings.window = v;
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "All_presets", &v, 1)) app->settings.all_presets = v ? 1 : 0;
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Feedback", &v, 1)) app->settings.feedback = v ? 1 : 0;
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Language", &v, 1) && v < LANG_COUNT)
            app->settings.language = v;
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Debug_log", &v, 1)) app->settings.debug_log = v ? 1 : 0;
    }
    g_lang = app->settings.language;
    furi_string_free(header);
    flipper_format_free(ff);
}

static void settings_save(App* app) {
    storage_simply_mkdir(app->storage, DATA_DIR);
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    if(flipper_format_file_open_always(ff, SETTINGS_PATH)) {
        flipper_format_write_header_cstr(ff, SETTINGS_FILE_TYPE, 2);
        flipper_format_write_uint32(ff, "Sensitivity", &app->settings.sensitivity, 1);
        flipper_format_write_uint32(ff, "Window", &app->settings.window, 1);
        flipper_format_write_uint32(ff, "All_presets", &app->settings.all_presets, 1);
        flipper_format_write_uint32(ff, "Feedback", &app->settings.feedback, 1);
        flipper_format_write_uint32(ff, "Language", &app->settings.language, 1);
        flipper_format_write_uint32(ff, "Debug_log", &app->settings.debug_log, 1);
    }
    flipper_format_free(ff);
}

/* ------------------------------------------------------------------------- */
/* Radio                                                                      */

/* The OOK650 preset with a 58 kHz channel filter, for the fine frequency search */
static uint8_t* build_narrow_preset(void) {
    const uint8_t* src = subghz_device_cc1101_preset_ook_650khz_async_regs;
    size_t pairs = 0;
    while(src[pairs]) pairs += 2;
    size_t len = pairs + 2 + 8; /* register pairs, 0 terminator pair, PATABLE */
    uint8_t* data = malloc(len);
    memcpy(data, src, len);
    for(size_t i = 0; i < pairs; i += 2) {
        if(data[i] == CC1101_MDMCFG4) data[i + 1] = (data[i + 1] & 0x0F) | 0xF0;
    }
    return data;
}

static void build_frequency_list(App* app) {
    app->freq_count = 0;
    size_t total = subghz_setting_get_frequency_count(app->setting);
    for(size_t i = 0; i < total && app->freq_count < FREQ_MAX; i++) {
        uint32_t f = subghz_setting_get_frequency(app->setting, i);
        /* Same exclusions as the firmware's Frequency Analyzer (self-noise of the device) */
        if(f == 464000000 || f == 467750000) continue;
        if(!subghz_devices_is_frequency_valid(app->device, f)) continue;
        bool dup = false;
        for(size_t k = 0; k < app->freq_count; k++)
            if(app->freqs[k] == f) dup = true;
        if(!dup) app->freqs[app->freq_count++] = f;
    }
    /* Keep the list sorted (insertion sort: qsort is disabled in the firmware API) */
    for(size_t i = 1; i < app->freq_count; i++) {
        uint32_t v = app->freqs[i];
        size_t k = i;
        while(k > 0 && app->freqs[k - 1] > v) {
            app->freqs[k] = app->freqs[k - 1];
            k--;
        }
        app->freqs[k] = v;
    }
}

static uint32_t snap_frequency(App* app, uint32_t measured) {
    uint32_t best = measured;
    uint32_t best_diff = SNAP_HZ + 1;
    for(size_t i = 0; i < app->freq_count; i++) {
        uint32_t f = app->freqs[i];
        uint32_t diff = f > measured ? f - measured : measured - f;
        if(diff < best_diff) {
            best_diff = diff;
            best = f;
        }
    }
    return best_diff <= SNAP_HZ ? best : measured;
}

static float measure(App* app, uint32_t f, uint8_t reads) {
    subghz_devices_idle(app->device);
    subghz_devices_set_frequency(app->device, f);
    subghz_devices_set_rx(app->device);
    furi_delay_ms(2);
    float best = -200.0f;
    for(uint8_t i = 0; i < reads; i++) {
        float r = subghz_devices_get_rssi(app->device);
        if(r > best) best = r;
        if(i + 1 < reads) furi_delay_ms(1);
    }
    return best;
}

static void rx_stop(App* app) {
    if(!app->rx_running) return;
    if(subghz_worker_is_running(app->sworker)) {
        subghz_worker_stop(app->sworker);
        subghz_devices_stop_async_rx(app->device);
    }
    subghz_devices_idle(app->device);
    app->rx_running = false;
}

static void rx_start(App* app, uint8_t preset, uint32_t freq) {
    rx_stop(app);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, presets[preset].preset, NULL);
    subghz_devices_set_frequency(app->device, freq);
    subghz_devices_flush_rx(app->device);
    subghz_devices_set_rx(app->device);
    subghz_receiver_reset(app->receiver);
    subghz_devices_start_async_rx(app->device, subghz_worker_rx_callback, app->sworker);
    subghz_worker_start(app->sworker);
    app->rx_running = true;
}

/* ------------------------------------------------------------------------- */
/* Decoder callbacks (run on the SubGhzWorker thread)                         */

static void pair_callback(void* context, bool level, uint32_t duration) {
    App* app = context;
    subghz_receiver_decode(app->receiver, level, duration);
    if(!app->counting) return;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    Analysis* an = &app->an;
    PresetResult* p = &an->pr[an->current];
    if(duration < 80)
        p->pulses_glitch++;
    else if(duration <= 20000)
        p->pulses_ok++;
    else
        p->pulses_long++;
    if(an->raw_cur_n < RAW_MAX) {
        int16_t d = duration > RAW_LIMIT ? RAW_LIMIT : (int16_t)duration;
        an->raw_cur[an->raw_cur_n++] = level ? d : (int16_t)-d;
    }
    furi_mutex_release(app->mutex);
}

static void overrun_callback(void* context) {
    App* app = context;
    subghz_receiver_reset(app->receiver);
}

static void receiver_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    App* app = context;
    const SubGhzProtocol* protocol = decoder_base->protocol;
    if(protocol->type != SubGhzProtocolTypeStatic && protocol->type != SubGhzProtocolTypeDynamic &&
       protocol->type != SubGhzProtocolCustom) {
        subghz_receiver_reset(receiver);
        return;
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    Analysis* an = &app->an;
    Candidate* c = NULL;
    for(size_t i = 0; i < CAND_MAX; i++) {
        if(an->cand[i].used && strcmp(an->cand[i].name, protocol->name) == 0) {
            c = &an->cand[i];
            break;
        }
    }
    if(!c) {
        for(size_t i = 0; i < CAND_MAX; i++) {
            if(!an->cand[i].used) {
                c = &an->cand[i];
                c->used = true;
                strlcpy(c->name, protocol->name, NAME_LEN);
                break;
            }
        }
    }
    if(c) {
        uint32_t hash = subghz_protocol_decoder_base_get_hash_data_long(decoder_base);
        if(c->count > 0) {
            if(hash == c->last_hash)
                c->repeats++;
            else
                c->changes++;
        }
        c->count++;
        c->last_hash = hash;
        c->type = protocol->type;
        c->per_preset[an->current]++;
        c->last_preset = an->current;
        an->last_cand = (int8_t)(c - an->cand);
        furi_string_reset(c->info);
        subghz_protocol_decoder_base_get_string(decoder_base, c->info);

        SubGhzRadioPreset rp = {
            .name = app->preset_name,
            .frequency = an->freq_nominal,
            .data = NULL,
            .data_size = 0,
            .latitude = NAN,
            .longitude = NAN,
        };
        furi_string_set(app->preset_name, presets[an->current].name);
        stream_clean(flipper_format_get_raw_stream(c->ff));
        subghz_protocol_decoder_base_serialize(decoder_base, c->ff, &rp);
        an->pr[an->current].decodes++;
    }
    furi_mutex_release(app->mutex);
    if(c) feedback(app, &sequence_blink_cyan_10);
    subghz_receiver_reset(receiver);
}

/* ------------------------------------------------------------------------- */
/* Analysis helpers                                                           */

static void analysis_reset(App* app, bool manual, uint8_t only_preset, uint32_t freq) {
    Analysis* an = &app->an;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    for(size_t i = 0; i < CAND_MAX; i++) {
        an->cand[i].used = false;
        an->cand[i].name[0] = '\0';
        an->cand[i].count = 0;
        an->cand[i].repeats = 0;
        an->cand[i].changes = 0;
        an->cand[i].last_hash = 0;
        memset(an->cand[i].per_preset, 0, sizeof(an->cand[i].per_preset));
        furi_string_reset(an->cand[i].info);
        stream_clean(flipper_format_get_raw_stream(an->cand[i].ff));
    }
    memset(an->pr, 0, sizeof(an->pr));
    for(size_t i = 0; i < PRESET_COUNT; i++) an->pr[i].rssi_max = -200.0f;
    an->manual = manual;
    an->only_preset = only_preset;
    an->freq_found = manual;
    an->freq_nominal = freq;
    an->freq_measured = freq;
    an->rssi_peak = -200.0f;
    an->found_level = -200.0f;
    an->last_cand = -1;
    an->any_activity = false;
    an->current = 0;
    an->raw_cur_n = 0;
    an->raw_best_n = 0;
    an->raw_best_preset = 0;
    an->raw_best_score = 0;
    an->phase = manual ? PhaseMod : PhaseFreq;
    furi_mutex_release(app->mutex);
}

/* Best candidate: most decodes, ties by preset rank of its best preset. Call with the mutex held
 * or after the worker stopped. */
static int8_t best_candidate(Analysis* an) {
    int8_t best = -1;
    for(int8_t i = 0; i < CAND_MAX; i++) {
        if(!an->cand[i].used || an->cand[i].count == 0) continue;
        if(best < 0 || an->cand[i].count > an->cand[best].count) best = i;
    }
    return best;
}

static uint8_t best_preset_of(Candidate* c) {
    uint8_t best = 0;
    for(uint8_t i = 1; i < PRESET_COUNT; i++) {
        if(c->per_preset[i] > c->per_preset[best] ||
           (c->per_preset[i] == c->per_preset[best] && preset_rank[i] > preset_rank[best]))
            best = i;
    }
    return best;
}

static uint8_t presets_with_decode(Candidate* c) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < PRESET_COUNT; i++)
        if(c->per_preset[i]) n++;
    return n;
}

/* 0 = high, 1 = good, 2 = low (a single decode) */
static uint8_t confidence_level(Candidate* c) {
    if(c->count >= 3 || presets_with_decode(c) >= 2) return 0;
    if(c->count == 2) return 1;
    return 2;
}

static const char* confidence_text(Candidate* c) {
    static const char* const texts[LANG_COUNT][3] = {
        {"high", "good", "low (1x)"},
        {"magas", "jo", "gyenge (1x)"},
        {"hoch", "gut", "niedrig (1x)"},
    };
    return texts[g_lang][confidence_level(c)];
}

static const char* confidence_key(Candidate* c) {
    static const char* const keys[] = {"high", "good", "low"};
    return keys[confidence_level(c)];
}

static bool preset_wanted(Analysis* an, uint8_t i) {
    return an->only_preset == 0xFF || an->only_preset == i;
}

static int8_t next_pending(Analysis* an) {
    for(int8_t i = 0; i < PRESET_COUNT; i++)
        if(preset_wanted(an, i) && !an->pr[i].done) return i;
    return -1;
}

static bool preset_confirmed(Analysis* an, uint8_t preset) {
    for(size_t i = 0; i < CAND_MAX; i++)
        if(an->cand[i].used && an->cand[i].per_preset[preset] >= 2) return true;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Scan screen model                                                          */

static void update_model(App* app, bool force, float rssi, const char* status, bool prompt) {
    uint32_t now = now_ms();
    if(!force && now - app->last_model < MODEL_EVERY_MS) return;
    app->last_model = now;
    Analysis* an = &app->an;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    int8_t best = best_candidate(an);
    char protocol[NAME_LEN];
    strlcpy(protocol, best >= 0 ? an->cand[best].name : "", NAME_LEN);
    uint8_t marks[PRESET_COUNT];
    for(uint8_t i = 0; i < PRESET_COUNT; i++) {
        if(!preset_wanted(an, i))
            marks[i] = MarkSkipped;
        else if(an->pr[i].decodes)
            marks[i] = MarkDecoded;
        else if(an->pr[i].done)
            marks[i] = MarkTested;
        else
            marks[i] = MarkPending;
    }
    furi_mutex_release(app->mutex);

    with_view_model(
        app->scan_view,
        ScanModel * m,
        {
            m->phase = an->phase;
            m->freq_known = an->freq_found;
            m->freq = an->freq_nominal;
            m->rssi = rssi;
            m->current = an->phase == PhaseMod ? an->current : 0xFF;
            memcpy(m->marks, marks, sizeof(marks));
            strlcpy(m->protocol, protocol, NAME_LEN);
            if(status) strlcpy(m->status, status, sizeof(m->status));
            m->prompt = prompt;
        },
        true);
}

/* ------------------------------------------------------------------------- */
/* Worker: frequency search                                                   */

static bool stop_requested(void) {
    return (furi_thread_flags_get() & (FLAG_STOP | FLAG_FINISH)) != 0;
}

static void sweep_forget(App* app) {
    for(size_t i = 0; i < FREQ_MAX; i++) {
        app->sweep_peak[i] = -200.0f;
        app->sweep_floor[i] = -200.0f;
    }
}

/* What counts as a signal on list frequency i: the setting, or in Auto its own noise + margin */
static float sweep_threshold(App* app, size_t i) {
    float floor = app->sweep_floor[i];
    if(!app->auto_sens) {
        /* A fixed setting under the noise (High in a noisy place) would take the noise for a remote */
        float guard = floor + FIXED_NOISE_DB;
        return floor > -199.0f && guard > app->threshold ? guard : app->threshold;
    }
    float t = floor + AUTO_SWEEP_MARGIN_DB;
    return t < AUTO_MIN_DBM ? AUTO_MIN_DBM : t;
}

/* A near remote is also heard on its second harmonic (433.92 MHz -> 867.84 MHz). When the strongest
 * frequency is in the upper band, the list frequencies at its half are measured right away (the
 * remote is sending now); if one of them carries a signal, that is the remote. */
static size_t fundamental_of(App* app, size_t best) {
    uint32_t f = app->freqs[best];
    if(f < 779000000) return best;
    size_t found = best;
    float found_level = -200.0f;
    for(size_t i = 0; i < app->freq_count; i++) {
        uint32_t twice = app->freqs[i] * 2;
        uint32_t diff = twice > f ? twice - f : f - twice;
        if(diff > f / 1000000 * HARMONIC_PPM) continue;
        float level = measure(app, app->freqs[i], 8);
        trace(app, "half %lu %d", (unsigned long)app->freqs[i], (int)level);
        if(level >= sweep_threshold(app, i) - 3.0f && level > found_level) {
            found = i;
            found_level = level;
        }
    }
    return found;
}

/* One fine search around centre (narrow filter). Like the coarse sweep it runs in quick rounds and
 * keeps the peak of every step: one read can fall into a gap of an OOK signal, and a short press may
 * end halfway - the peaks heard while it lasted stay valid. Returns the number of rounds that heard
 * the remote (0 = it was gone); out_f / out_level = the strongest step. */
static uint8_t fine_rounds(
    App* app,
    uint32_t centre,
    uint32_t span,
    uint32_t step,
    uint8_t rounds,
    float thr,
    uint32_t* out_f,
    float* out_level) {
    float peak[FINE_STEPS_MAX];
    uint32_t steps = 2 * span / step + 1;
    if(steps > FINE_STEPS_MAX) steps = FINE_STEPS_MAX;
    for(uint32_t k = 0; k < steps; k++) peak[k] = -200.0f;
    uint8_t hits = 0;
    uint8_t done = 0;
    while(done < rounds && hits < FINE_HITS) {
        float round_best = -200.0f;
        for(uint32_t k = 0; k < steps; k++) {
            if(stop_requested()) return 0;
            uint32_t f = centre - span + k * step;
            if(!subghz_devices_is_frequency_valid(app->device, f)) continue;
            float r = measure(app, f, 1);
            if(r > peak[k]) peak[k] = r;
            if(r > round_best) round_best = r;
        }
        done++;
        if(round_best >= thr - FINE_HIT_DB) hits++;
    }
    uint32_t best_k = steps / 2;
    for(uint32_t k = 0; k < steps; k++)
        if(peak[k] > peak[best_k]) best_k = k;
    *out_f = centre - span + best_k * step;
    *out_level = peak[best_k];
    trace(
        app,
        "fine %lu %d rounds %u hits %u",
        (unsigned long)*out_f,
        (int)*out_level,
        done,
        hits);
    return hits;
}

static bool frequency_sweep(App* app) {
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);

    /* One read per frequency can fall into a gap of an AM (OOK) signal, so the peaks of the passes
     * are kept, fading slowly, and the best frequency is chosen from them. */
    /* In Auto, each frequency also keeps its noise: the lowest reading, rising slowly, so a noisy
     * frequency needs a stronger peak. The best frequency is the one most over its threshold. */
    size_t best_i = 0;
    float best_excess = -1000.0f;
    for(size_t i = 0; i < app->freq_count; i++) {
        if(stop_requested()) return false;
        float r = measure(app, app->freqs[i], 1);
        float kept = app->sweep_peak[i] - SWEEP_DECAY_DB;
        app->sweep_peak[i] = r > kept ? r : kept;
        float rise = app->sweep_floor[i] + AUTO_FLOOR_RISE_DB;
        app->sweep_floor[i] = app->sweep_floor[i] < -199.0f || r < rise ? r : rise;
        float excess = app->sweep_peak[i] - sweep_threshold(app, i);
        if(excess > best_excess) {
            best_excess = excess;
            best_i = i;
        }
    }
    if(app->freq_count == 0) return false;
    float best = app->sweep_peak[best_i];
    update_model(app, true, best, L("Hold the remote button!", "Tartsd nyomva a gombot!", "Taste gedrueckt halten!"), false);
    uint32_t now = now_ms();
    if(now - app->last_trace >= 1000) {
        app->last_trace = now;
        trace(
            app,
            "pass %lu pk %d thr %d",
            (unsigned long)app->freqs[best_i],
            (int)best,
            (int)sweep_threshold(app, best_i));
    }
    if(best_excess < 0.0f) return false;

    size_t fund_i = fundamental_of(app, best_i);
    if(fund_i != best_i) {
        trace(
            app,
            "harmonic %lu -> %lu",
            (unsigned long)app->freqs[best_i],
            (unsigned long)app->freqs[fund_i]);
        best_i = fund_i;
    }
    uint32_t best_f = app->freqs[best_i];
    float thr = sweep_threshold(app, best_i);
    float confirm = measure(app, best_f, 10);

    trace(app, "sweep %lu %d conf %d thr %d", (unsigned long)best_f, (int)best, (int)confirm, (int)thr);
    if(confirm < thr - 3.0f) return false;

    update_model(app, true, confirm, L("Refining frequency", "Frekvencia pontositasa", "Frequenz verfeinern"), false);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetCustom, app->narrow_preset);

    /* First +-FINE_SPAN_HZ in FINE_STEP_HZ steps; if the remote was not heard there, it is gone
     * (the press ended) and the next press starts the search again - a guessed frequency would end
     * up in the saved .sub file. Then +-FINE_STEP_HZ in FINE2_STEP_HZ steps around the result; if the
     * press ended by then, the first result stands (the snap to the list makes up the rest). */
    uint32_t fine_f;
    float fine;
    if(!fine_rounds(app, best_f, FINE_SPAN_HZ, FINE_STEP_HZ, FINE_ROUNDS, thr, &fine_f, &fine))
        return false;
    uint32_t fine2_f;
    float fine2;
    if(fine_rounds(app, fine_f, FINE_STEP_HZ, FINE2_STEP_HZ, FINE2_ROUNDS, thr, &fine2_f, &fine2) &&
       fine2 >= fine - 3.0f) {
        fine_f = fine2_f;
        fine = fine2;
    }
    if(fine < thr - FINE_HIT_DB) return false;

    uint32_t nominal = snap_frequency(app, fine_f);
    uint32_t before = app->an.freq_nominal;
    if(before && (nominal > before ? nominal - before : before - nominal) > SNAP_HZ) {
        /* The search again found another frequency: what was tested on the old one (perhaps a
         * harmonic) does not count. Start the presets over. */
        trace(app, "restart %lu -> %lu", (unsigned long)before, (unsigned long)nominal);
        analysis_reset(app, false, app->an.only_preset, 0);
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->an.freq_measured = fine_f;
    app->an.freq_nominal = nominal;
    app->an.found_level = fine > confirm ? fine : confirm;
    if(app->an.found_level > app->an.rssi_peak) app->an.rssi_peak = app->an.found_level;
    app->an.freq_found = true;
    furi_mutex_release(app->mutex);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Worker: modulation search                                                  */

static void start_preset(App* app, uint8_t preset) {
    app->counting = false;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->an.current = preset;
    app->an.raw_cur_n = 0;
    furi_mutex_release(app->mutex);
    app->win.started = false;
    app->win.wait_since = now_ms();
    app->win.prompted = false;
    app->win.decodes_seen = app->an.pr[preset].decodes;
    /* Auto: a press is AUTO_PRESS_MARGIN_DB over the preset's noise, which modulation_tick keeps
     * learning from the quiet readings. Fixed setting: a press counts from ACTIVE_BELOW_DB under
     * the level the frequency search found, never stricter than the setting and at most
     * ACTIVE_MIN_DB looser (the presets measure the same remote lower than the narrow search). */
    float level;
    app->preset_floor = presets[preset].noise;
    if(app->auto_sens) {
        level = app->preset_floor + AUTO_PRESS_MARGIN_DB;
    } else {
        level = app->an.manual ? app->threshold : app->an.found_level - ACTIVE_BELOW_DB;
        if(level > app->threshold) level = app->threshold;
        if(level < app->threshold - ACTIVE_MIN_DB) level = app->threshold - ACTIVE_MIN_DB;
        app->fixed_press = level;
        float guard = app->preset_floor + FIXED_NOISE_DB;
        if(level < guard) level = guard;
    }
    app->active_threshold = level;
    trace(
        app,
        "preset %s %lu act %d found %d",
        presets[preset].name,
        (unsigned long)app->an.freq_nominal,
        (int)level,
        (int)app->an.found_level);
    rx_start(app, preset, app->an.freq_nominal);
}

/* Closes the current preset; returns true when the whole search is over */
static bool complete_preset(App* app) {
    app->counting = false;
    Analysis* an = &app->an;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    PresetResult* p = &an->pr[an->current];
    p->done = true;
    uint32_t pulses = p->pulses_ok + p->pulses_glitch + p->pulses_long;
    uint32_t score = p->pulses_ok >= 16 ? (p->pulses_ok * 1000) / pulses : 0;
    if(score > an->raw_best_score && an->raw_cur_n > 0) {
        int16_t* tmp = an->raw_best;
        an->raw_best = an->raw_cur;
        an->raw_best_n = an->raw_cur_n;
        an->raw_cur = tmp;
        an->raw_cur_n = 0;
        an->raw_best_score = score;
        an->raw_best_preset = an->current;
    }
    bool quick_done = !app->settings.all_presets && preset_confirmed(an, an->current);
    trace(app, "done %s dec %u span %lu", presets[an->current].name, p->decodes, (unsigned long)p->span_ms);
    int8_t next = quick_done ? -1 : next_pending(an);
    furi_mutex_release(app->mutex);

    if(next < 0) return true;
    start_preset(app, next);
    return false;
}

/* One tick of the modulation search; returns true when it is over */
static bool modulation_tick(App* app) {
    Analysis* an = &app->an;
    Window* w = &app->win;
    uint32_t now = now_ms();
    float rssi = subghz_devices_get_rssi(app->device);
    PresetResult* p = &an->pr[an->current];
    /* Learn the preset's noise from the quiet readings, kept near its typical level so a long
     * signal is never taken for noise. Auto measures a press against it; a fixed setting only keeps
     * FIXED_NOISE_DB above it. */
    float typical = presets[an->current].noise;
    if(rssi < app->active_threshold) app->preset_floor += (rssi - app->preset_floor) * AUTO_NOISE_EMA;
    if(app->preset_floor > typical + AUTO_NOISE_SPAN_DB) app->preset_floor = typical + AUTO_NOISE_SPAN_DB;
    if(app->preset_floor < typical - AUTO_NOISE_SPAN_DB) app->preset_floor = typical - AUTO_NOISE_SPAN_DB;
    if(app->auto_sens) {
        float level = app->preset_floor + AUTO_PRESS_MARGIN_DB;
        app->active_threshold = level < AUTO_MIN_DBM ? AUTO_MIN_DBM : level;
    } else {
        float guard = app->preset_floor + FIXED_NOISE_DB;
        app->active_threshold = app->fixed_press > guard ? app->fixed_press : guard;
    }

    /* A decode is always a signal, even one too weak for the RSSI threshold */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    uint16_t decodes = p->decodes;
    char dec_name[NAME_LEN] = "";
    uint32_t dec_hash = 0;
    if(an->last_cand >= 0) {
        strlcpy(dec_name, an->cand[an->last_cand].name, NAME_LEN);
        dec_hash = an->cand[an->last_cand].last_hash;
    }
    furi_mutex_release(app->mutex);
    bool decoded = decodes != w->decodes_seen;
    if(decoded) {
        w->decodes_seen = decodes;
        trace(app, "dec %s %08lX r %d", dec_name, (unsigned long)dec_hash, (int)rssi);
    }
    bool active = decoded || rssi > app->active_threshold;

    if(now - app->last_trace >= TRACE_EVERY_MS) {
        app->last_trace = now;
        trace(
            app,
            "%s r %d t %d s %u w %u a %u d %u",
            presets[an->current].name,
            (int)rssi,
            (int)app->active_threshold,
            w->started,
            w->prompted,
            p->attempts,
            p->decodes);
    }
    char status[40];
    snprintf(status, sizeof(status), L("Testing: %s", "Vizsgalat: %s", "Teste: %s"), presets[an->current].name);

    if(active) {
        if(!w->started) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            an->raw_cur_n = 0;
            furi_mutex_release(app->mutex);
            w->started = true;
            w->first = now;
            w->prompted = false;
            app->counting = true;
            trace(app, "start %d", (int)rssi);
        }
        w->last = now;
        p->signal_seen = true;
        an->any_activity = true;
        if(rssi > p->rssi_max) p->rssi_max = rssi;
        if(rssi > an->rssi_peak) an->rssi_peak = rssi;
    }

    if(!w->started) {
        bool prompt = now - w->wait_since >= PROMPT_MS;
        if(prompt && !w->prompted) {
            w->prompted = true;
            trace(app, "prompt");
            feedback(app, &sequence_single_vibro);
        }
        if(!an->manual && now - w->wait_since >= REFIND_MS) {
            /* Nothing for a while: the frequency was noise or is off. Search it again; the presets
             * already tested and the decodes are kept. */
            trace(app, "refind");
            sweep_forget(app);
            rx_stop(app);
            an->freq_found = false;
            an->phase = PhaseFreq;
            update_model(app, true, rssi, L("Searching frequency again", "Frekvencia ujrakeresese", "Frequenz neu suchen"), false);
            return false;
        }
        update_model(app, false, rssi, prompt ? NULL : status, prompt);
        return false;
    }

    uint32_t span = w->last - w->first;
    if(span > p->span_ms) p->span_ms = span;
    bool complete = false;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool confirmed = preset_confirmed(an, an->current);
    furi_mutex_release(app->mutex);

    if(confirmed || now - w->first >= window_ms[app->settings.window]) {
        complete = true;
    } else if(now - w->last >= SILENCE_MS) {
        /* The remote stopped sending */
        p->attempts++;
        trace(app, "silence span %lu try %u", (unsigned long)span, p->attempts);
        if(p->decodes > 0 || span >= MIN_SPAN_MS || p->attempts >= MAX_ATTEMPTS) {
            complete = true;
        } else {
            app->counting = false;
            w->started = false;
            w->wait_since = now - PROMPT_MS; /* ask for a new press right away */
            w->prompted = false;
        }
    }

    if(complete) {
        if(complete_preset(app)) return true;
        update_model(app, true, rssi, status, false);
        return false;
    }
    update_model(app, false, rssi, status, false);
    return false;
}

static int32_t worker_thread(void* context) {
    App* app = context;
    bool cancelled = false;
    subghz_devices_begin(app->device);
    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    app->last_model = 0;

    if(app->an.phase == PhaseMod) {
        int8_t first = next_pending(&app->an);
        if(first >= 0) start_preset(app, first);
    }
    update_model(
        app, true, -200.0f, app->an.phase == PhaseFreq ? L("Hold the remote button!", "Tartsd nyomva a gombot!", "Taste gedrueckt halten!") :
                                    L("Scanning", "Vizsgalat", "Analyse"),
        false);

    while(true) {
        uint32_t flags = furi_thread_flags_wait(
            FLAG_STOP | FLAG_FINISH, FuriFlagWaitAny, app->an.phase == PhaseMod ? TICK_MS : 0);
        if(!(flags & FuriFlagError)) {
            if(flags & FLAG_STOP) {
                cancelled = true;
                break;
            }
            if(flags & FLAG_FINISH) break;
        }
        if(app->an.phase == PhaseFreq) {
            if(frequency_sweep(app)) {
                app->an.phase = PhaseMod;
                feedback(app, &sequence_blink_cyan_10);
                int8_t first = next_pending(&app->an);
                if(first < 0) break;
                start_preset(app, first);
            }
        } else if(app->an.phase == PhaseMod) {
            if(modulation_tick(app)) break;
        } else {
            break;
        }
    }

    rx_stop(app);
    app->counting = false;
    subghz_devices_sleep(app->device);
    subghz_devices_end(app->device);
    app->an.phase = PhaseDone;
    if(!cancelled) view_dispatcher_send_custom_event(app->vd, EvScanDone);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Main screen                                                                */

static const char* const main_small_items[LANG_COUNT][3] = {
    {"Manual scan", "Saved scans", "Settings"},
    {"Kezi vizsgalat", "Mentett vizsgalatok", "Beallitasok"},
    {"Manuelle Analyse", "Gespeicherte Analysen", "Einstellungen"},
};

static void main_draw(Canvas* canvas, void* model) {
    MainModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "RSC Remote Analyzer");

    if(m->selected == 0) {
        canvas_draw_rbox(canvas, 3, 12, 122, 23, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, 3, 12, 122, 23, 3);
        canvas_draw_rframe(canvas, 4, 13, 120, 21, 2);
    }
    canvas_draw_str_aligned(canvas, 64, 15, AlignCenter, AlignTop, L("Automatic", "Automatikus", "Automatische"));
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, L("scan", "vizsgalat", "Analyse"));
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < COUNT_OF(main_small_items[0]); i++) {
        uint8_t y = 37 + i * 9;
        if(m->selected == i + 1) {
            canvas_draw_box(canvas, 0, y, 128, 9);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str_aligned(canvas, 64, y + 1, AlignCenter, AlignTop, main_small_items[g_lang][i]);
        canvas_set_color(canvas, ColorBlack);
    }
}

static bool main_input(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        with_view_model(
            app->main_view,
            MainModel * m,
            {
                if(event->key == InputKeyUp)
                    m->selected = (m->selected + MAIN_ITEMS - 1) % MAIN_ITEMS;
                else
                    m->selected = (m->selected + 1) % MAIN_ITEMS;
            },
            true);
        return true;
    }
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        uint8_t selected = 0;
        with_view_model(app->main_view, MainModel * m, { selected = m->selected; }, false);
        view_dispatcher_send_custom_event(app->vd, EvMenuAuto + selected);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Scan screen                                                                */

static void scan_draw(Canvas* canvas, void* model) {
    ScanModel* m = model;
    char buf[40];
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    if(m->phase == PhaseFreq)
        canvas_draw_str(canvas, 0, 9, L("Frequency search", "Frekvencia keresese", "Frequenzsuche"));
    else if(m->current < PRESET_COUNT) {
        snprintf(buf, sizeof(buf), L("Modulation: %s", "Modulacio: %s", "Modulation: %s"), presets[m->current].name);
        canvas_draw_str(canvas, 0, 9, buf);
    } else
        canvas_draw_str(canvas, 0, 9, L("Evaluating", "Kiertekeles", "Auswertung"));

    canvas_set_font(canvas, FontSecondary);
    if(m->freq_known)
        format_freq(buf, sizeof(buf), m->freq);
    else
        strlcpy(buf, "--- MHz", sizeof(buf));
    canvas_draw_str(canvas, 0, 19, buf);

    /* Signal strength: -100 dBm .. -30 dBm */
    canvas_draw_frame(canvas, 66, 12, 38, 7);
    int32_t level = (int32_t)((m->rssi + 100.0f) * 36.0f / 70.0f);
    if(level < 0) level = 0;
    if(level > 36) level = 36;
    if(level > 0) canvas_draw_box(canvas, 67, 13, level, 5);
    if(m->rssi > -199.0f) {
        snprintf(buf, sizeof(buf), "%d", (int)m->rssi);
        canvas_draw_str_aligned(canvas, 128, 19, AlignRight, AlignBottom, buf);
    }

    for(uint8_t i = 0; i < PRESET_COUNT; i++) {
        uint8_t x = i * 32;
        if(m->current == i) {
            canvas_draw_rbox(canvas, x, 21, 31, 10, 2);
            canvas_set_color(canvas, ColorWhite);
        } else if(m->marks[i] != MarkSkipped) {
            canvas_draw_rframe(canvas, x, 21, 31, 10, 2);
        }
        canvas_draw_str_aligned(canvas, x + 16, 29, AlignCenter, AlignBottom, presets[i].name);
        canvas_set_color(canvas, ColorBlack);
        if(m->marks[i] == MarkDecoded) {
            canvas_draw_box(canvas, x + 2, 32, 27, 2);
        } else if(m->marks[i] == MarkTested) {
            for(uint8_t d = 2; d < 29; d += 3) canvas_draw_dot(canvas, x + d, 32);
        }
    }

    if(m->protocol[0]) {
        snprintf(buf, sizeof(buf), "Prot: %s", m->protocol);
    } else {
        strlcpy(buf, m->phase == PhaseFreq ? "Prot: -" : L("Prot: searching...", "Prot: kereses...", "Prot: suche..."), sizeof(buf));
    }
    canvas_draw_str(canvas, 0, 42, buf);

    if(m->prompt) {
        canvas_draw_rbox(canvas, 0, 44, 128, 20, 3);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignBottom, L("Press the remote", "Nyomd meg ujra a", "Bitte die Taste"));
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, L("button again!", "taviranyito gombjat!", "erneut druecken!"));
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_str(canvas, 0, 52, m->status);
        canvas_draw_str(canvas, 0, 62, L("OK=result  Back=exit", "OK=eredmeny  Back=kilepes", "OK=Ergebnis  Back=Ende"));
    }
}

static bool scan_input(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort) return event->key == InputKeyBack;
    if(event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(app->vd, EvScanFinish);
        return true;
    }
    if(event->key == InputKeyBack) {
        view_dispatcher_send_custom_event(app->vd, EvScanCancel);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Scan control                                                               */

static void switch_to(App* app, ViewId view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->vd, view);
}

static void scan_start(App* app, bool manual) {
    uint32_t freq = 0;
    uint8_t only = 0xFF;
    if(manual) {
        freq = app->freqs[app->manual_freq];
        if(app->manual_preset > 0) only = app->manual_preset - 1;
    }
    app->auto_sens = app->settings.sensitivity == SENSITIVITY_AUTO;
    app->threshold = app->auto_sens ? AUTO_MIN_DBM : (float)sensitivity_dbm[app->settings.sensitivity];
    app->active_threshold = app->threshold;
    sweep_forget(app);
    analysis_reset(app, manual, only, freq);
    if(app->trace) free(app->trace);
    app->trace = app->settings.debug_log ? malloc(TRACE_LINES * TRACE_LEN) : NULL;
    app->trace_n = 0;
    app->scan_t0 = now_ms();
    furi_hal_rtc_get_datetime(&app->scan_time);
    app->last_trace = 0;
    trace(
        app,
        "%s v%s sens %s freq %lu",
        manual ? "manual" : "auto",
        APP_VERSION,
        sensitivity_names[LANG_EN][app->settings.sensitivity],
        (unsigned long)freq);
    with_view_model(
        app->scan_view,
        ScanModel * m,
        {
            memset(m, 0, sizeof(ScanModel));
            m->phase = app->an.phase;
            m->rssi = -200.0f;
            m->current = 0xFF;
            m->freq_known = manual;
            m->freq = freq;
        },
        true);
    notification_message(app->notif, &sequence_display_backlight_enforce_on);
    switch_to(app, ViewScan);
    furi_thread_start(app->thread);
}

static void scan_stop(App* app, uint32_t flag) {
    if(furi_thread_get_state(app->thread) != FuriThreadStateStopped) {
        furi_thread_flags_set(furi_thread_get_id(app->thread), flag);
        furi_thread_join(app->thread);
    }
    notification_message(app->notif, &sequence_display_backlight_enforce_auto);
}

/* ------------------------------------------------------------------------- */
/* Result texts                                                               */

static void preset_line(FuriString* out, Analysis* an, uint8_t i) {
    PresetResult* p = &an->pr[i];
    furi_string_cat_printf(out, "%s: ", presets[i].name);
    if(!preset_wanted(an, i)) {
        furi_string_cat_str(out, L("not requested\n", "nem kert\n", "nicht gewaehlt\n"));
    } else if(!p->done && !p->signal_seen) {
        furi_string_cat_str(out, L("not tested\n", "nem vizsgalt\n", "nicht getestet\n"));
    } else if(p->decodes) {
        furi_string_cat_printf(out, L("%u decodes\n", "%u dekodolas\n", "%u Dekodierungen\n"), p->decodes);
    } else if(p->signal_seen) {
        furi_string_cat_printf(
            out, L("0 decodes, %lu ms\n", "0 dekodolas, %lu ms\n", "0 dekodiert, %lu ms\n"), (unsigned long)p->span_ms);
    } else {
        furi_string_cat_str(out, L("no signal\n", "nem kapott jelet\n", "kein Signal\n"));
    }
}

/* In automatic mode a found frequency means a signal; in manual mode only real activity counts */
static bool signal_detected(Analysis* an) {
    return an->any_activity || (!an->manual && an->freq_found);
}

static void build_result(App* app) {
    Analysis* an = &app->an;
    app->best = best_candidate(an);
    FuriString* s = app->summary;
    furi_string_reset(s);

    if(app->best >= 0) {
        Candidate* c = &an->cand[app->best];
        uint8_t bp = best_preset_of(c);
        furi_string_cat_printf(s, "\e#%s\n", c->name);
        furi_string_cat_printf(s, "%s\n", type_text(c->type));
        format_freq_long(s, an->freq_nominal);
        furi_string_cat_printf(s, "  %s\n", presets[bp].name);
        furi_string_cat_printf(s, L("Confidence: %s\n", "Megbizhatosag: %s\n", "Sicherheit: %s\n"), confidence_text(c));
        if(c->type == SubGhzProtocolTypeDynamic)
            furi_string_cat_str(s, L(
                "Must be paired with\nthe receiver.\n",
                "Parositas szukseges\na vevovel.\n",
                "Muss am Empfaenger\nangelernt werden.\n"));
        /* The first line of the decoder text usually holds the key */
        size_t nl = furi_string_search_char(c->info, '\n');
        if(nl != FURI_STRING_FAILURE) {
            FuriString* rest = furi_string_alloc_set(c->info);
            furi_string_right(rest, nl + 1);
            size_t nl2 = furi_string_search_char(rest, '\n');
            if(nl2 != FURI_STRING_FAILURE) furi_string_left(rest, nl2);
            furi_string_cat_printf(s, "%s\n", furi_string_get_cstr(rest));
            furi_string_free(rest);
        }
    } else if(signal_detected(an)) {
        furi_string_cat_str(s, L(
            "\e#Unknown protocol\n",
            "\e#Ismeretlen protokoll\n",
            "\e#Unbekanntes Protokoll\n"));
        furi_string_cat_str(s, L(
            "further analysis needed.\n",
            "tovabbi vizsgalat\nszukseges.\n",
            "weitere Analyse noetig.\n"));
        furi_string_cat_str(s, L("Signal: ", "Jel: ", "Signal: "));
        format_freq_long(s, an->freq_nominal);
        furi_string_cat_printf(s, ", %d dBm\n", (int)an->rssi_peak);
        if(an->raw_best_score > 0)
            furi_string_cat_printf(
                s, L(
                    "Likely: %s\n(not sure)\n",
                    "Valoszinu: %s\n(nem biztos)\n",
                    "Vermutlich: %s\n(unsicher)\n"), presets[an->raw_best_preset].name);
        furi_string_cat_str(s, L(
            "Fixed or rolling code:\nunknown.\n",
            "Fix vagy ugrokod:\nnem tudhato.\n",
            "Fest oder Rolling Code:\nunbekannt.\n"));
    } else {
        furi_string_cat_str(s, L(
            "\e#No signal received\n",
            "\e#Nem erkezett jel\n",
            "\e#Kein Signal empfangen\n"));
        furi_string_cat_str(
            s,
            L(
                "Hold the remote close to\nthe Flipper and press its\n"
                "button during the scan.\n",
                "Tartsd a taviranyitot a\nFlipper kozeleben, es nyomd\n"
                "meg a gombjat a vizsgalat\nalatt.\n",
                "Fernbedienung nah an den\nFlipper halten und die\n"
                "Taste waehrend der\nAnalyse druecken.\n"));
    }

    if(app->best >= 0)
        feedback(app, &sequence_success);
    else
        feedback(app, &sequence_double_vibro);
}

static void result_button(GuiButtonType button, InputType type, void* context);

static void show_result(App* app) {
    widget_reset(app->result);
    widget_add_text_scroll_element(app->result, 0, 0, 128, 51, furi_string_get_cstr(app->summary));
    widget_add_button_element(app->result, GuiButtonTypeLeft, L("New", "Uj", "Neu"), result_button, app);
    bool can_save = app->best >= 0 || signal_detected(&app->an);
    if(can_save) widget_add_button_element(app->result, GuiButtonTypeCenter, L("Save", "Mentes", "Speichern"), result_button, app);
    widget_add_button_element(app->result, GuiButtonTypeRight, L("More", "Tobb", "Mehr"), result_button, app);
    switch_to(app, ViewResult);
}

static void result_button(GuiButtonType button, InputType type, void* context) {
    App* app = context;
    if(type != InputTypeShort) return;
    if(button == GuiButtonTypeLeft)
        view_dispatcher_send_custom_event(app->vd, EvResultNew);
    else if(button == GuiButtonTypeCenter)
        view_dispatcher_send_custom_event(app->vd, EvResultSave);
    else if(button == GuiButtonTypeRight)
        view_dispatcher_send_custom_event(app->vd, EvResultMore);
}

static void detail_data_text(App* app, FuriString* out) {
    Analysis* an = &app->an;
    furi_string_reset(out);
    if(app->best < 0) {
        furi_string_cat_str(out, L("No decoded data.\n\n", "Nincs dekodolt adat.\n\n", "Keine dekodierten Daten.\n\n"));
        if(an->freq_found) {
            furi_string_cat_str(out, L("Measured freq: ", "Mert frekvencia: ", "Gemessen: "));
            format_freq_long(out, an->freq_measured);
            furi_string_cat_str(out, L("\nUsed freq: ", "\nHasznalt frekvencia: ", "\nVerwendet: "));
            format_freq_long(out, an->freq_nominal);
            furi_string_cat_printf(out, L("\nPeak level: %d dBm\n", "\nCsucs jelszint: %d dBm\n", "\nSpitzenpegel: %d dBm\n"), (int)an->rssi_peak);
        }
        if(an->raw_best_n)
            furi_string_cat_printf(
                out,
                L(
                    "RAW capture: %u values\n(%s), can be saved and\n"
                    "studied in Sub-GHz or\nProtoView.\n",
                    "RAW felvetel: %u ertek\n(%s). Mentheto, es a\n"
                    "Sub-GHz appban vagy\nProtoView-ban vizsgalhato.\n",
                    "RAW-Aufnahme: %u Werte\n(%s). Speicherbar, in\n"
                    "Sub-GHz oder ProtoView\nweiter pruefbar.\n"),
                (unsigned)an->raw_best_n,
                presets[an->raw_best_preset].name);
        return;
    }
    Candidate* c = &an->cand[app->best];
    furi_string_cat_printf(out, "%s (%s)\n", c->name, type_text(c->type));
    furi_string_cat(out, c->info);
    furi_string_cat_str(out, L("\n\nMeasured freq: ", "\n\nMert frekvencia: ", "\n\nGemessen: "));
    format_freq_long(out, an->freq_measured);
    furi_string_cat_printf(out, L("\nPeak level: %d dBm\n", "\nCsucs jelszint: %d dBm\n", "\nSpitzenpegel: %d dBm\n"), (int)an->rssi_peak);
    furi_string_cat_printf(out, L("Decodes: %u\n", "Dekodolasok: %u\n", "Dekodierungen: %u\n"), c->count);
    furi_string_cat_printf(out, L("Same repeats: %u\n", "Azonos ismetles: %u\n", "Gleicher Code: %u\n"), c->repeats);
    furi_string_cat_printf(out, L("Code changes: %u\n", "Elterokod: %u\n", "Codewechsel: %u\n"), c->changes);
    for(int8_t i = 0; i < CAND_MAX; i++) {
        if(i == app->best || !an->cand[i].used) continue;
        furi_string_cat_printf(
            out, L("Also seen: %s (%u)\n", "Mas protokoll is: %s (%u)\n", "Auch gesehen: %s (%u)\n"), an->cand[i].name, an->cand[i].count);
    }
}

static void detail_modulation_text(App* app, FuriString* out) {
    Analysis* an = &app->an;
    furi_string_reset(out);
    furi_string_cat_str(out, L("Receive presets:\n", "Vetelei beallitasok:\n", "Empfangs-Presets:\n"));
    for(uint8_t i = 0; i < PRESET_COUNT; i++) preset_line(out, an, i);
    if(app->best >= 0) {
        furi_string_cat_printf(
            out,
            L("\nMost likely: %s\n(the most decodes)\n", "\nLegvaloszinubb: %s\n(a legtobb dekodolas)\n", "\nWahrscheinlich: %s\n(meiste Dekodierungen)\n"),
            presets[best_preset_of(&an->cand[app->best])].name);
    } else if(an->raw_best_score > 0) {
        furi_string_cat_printf(
            out,
            L("\nLikely: %s\n(from the pulse pattern,\nnot sure)\n",
              "\nValoszinu: %s\n(impulzusminta alapjan,\nnem biztos)\n",
              "\nVermutlich: %s\n(aus dem Pulsmuster,\n"
              "unsicher)\n"),
            presets[an->raw_best_preset].name);
    }
    furi_string_cat_str(
        out,
        L("\nScreen marks:\nsolid bar = decoded,\ndotted = tested, no decode.\n",
          "\nJelek a kepernyon:\nteli csik = dekodolt,\npontozott = vizsgalt, nincs\ndekodolas.\n",
          "\nAnzeige:\nvoller Balken = dekodiert,\n"
          "punktiert = getestet, nicht\ndekodiert.\n"));
}

static void detail_compat_text(App* app, FuriString* out) {
    Analysis* an = &app->an;
    furi_string_reset(out);
    if(app->best < 0) {
        furi_string_cat_str(
            out,
            L(
                "Unknown protocol - further\nanalysis needed.\n\n"
                "Whether it is fixed or\nrolling code is unknown.\n"
                "Receiving a signal is not\nthe same as identifying\n"
                "its protocol.\n\nThe RAW capture can be\n"
                "studied further in Sub-GHz\n(Read RAW) or ProtoView.\n",
                "Ismeretlen protokoll -\ntovabbi vizsgalat\n"
                "szukseges.\n\nNem tudhato, hogy fix vagy\n"
                "ugrokodos. A jel fogasa\nnem azonos a protokoll\n"
                "felismeresevel.\n\nA RAW felvetel a Sub-GHz\n"
                "appban (Read RAW) vagy\nProtoView-ban tovabb\n"
                "vizsgalhato.\n",
                "Unbekanntes Protokoll -\nweitere Analyse noetig.\n\n"
                "Ob Fest- oder Rolling\nCode, ist nicht\n"
                "feststellbar. Ein\nempfangenes Signal ist\n"
                "noch kein erkanntes\nProtokoll.\n\n"
                "Die RAW-Aufnahme kann in\nSub-GHz (Read RAW) oder\n"
                "ProtoView weiter geprueft\nwerden.\n"));
        return;
    }
    Candidate* c = &an->cand[app->best];
    if(c->type == SubGhzProtocolTypeDynamic) {
        furi_string_cat_str(
            out,
            L(
                "Rolling code protocol.\n\nIt cannot be copied by\n"
                "simple recording and\nreplay: every press sends\n"
                "a new code.\n\nA new remote has to be\n"
                "paired with the receiver\nas its manual describes.\n\n"
                "Saving keeps the analysis\nonly (no .sub file).\n",
                "Ugrokodos protokoll.\n\nEgyszeru rogzitessel es\n"
                "visszajatszassal nem\nmasolhato: minden\n"
                "gombnyomas uj kodot kuld.\n\nUj taviranyitot a\n"
                "vevoegyseg kezikonyve\nszerint, a vevovel\n"
                "parositva lehet hasznalni.\n\n"
                "A mentes csak az elemzest\nrogziti (.sub nem keszul).\n",
                "Rolling-Code-Protokoll.\n\nEinfaches Aufnehmen und\n"
                "Abspielen kopiert es\nnicht: jeder Druck sendet\n"
                "einen neuen Code.\n\nEine neue Fernbedienung\n"
                "muss laut Anleitung am\nEmpfaenger angelernt\n"
                "werden.\n\nGespeichert wird nur die\n"
                "Analyse (ohne .sub-Datei).\n"));
    } else if(c->type == SubGhzProtocolTypeStatic) {
        furi_string_cat_str(
            out,
            L(
                "Fixed code protocol.\n\nEvery press sends the same\n"
                "code. After saving, the\n.sub file opens in the\n"
                "Sub-GHz app (Saved).\n\nTransmit only on your own\n"
                "system and within the\nlocal rules.\n",
                "Fix kodos protokoll.\n\nA kod minden gombnyomasnal\n"
                "ugyanaz. Mentes utan a\n.sub fajl a Sub-GHz appban\n"
                "(Saved) megnyithato.\n\nKuldeni csak sajat\n"
                "rendszeren, a helyi\nszabalyok szerint szabad.\n",
                "Festcode-Protokoll.\n\nJeder Druck sendet\n"
                "denselben Code. Nach dem\nSpeichern oeffnet sich die\n"
                ".sub-Datei in der Sub-GHz-\nApp (Saved).\n\n"
                "Senden nur am eigenen\nSystem und nach den\n"
                "oertlichen Regeln.\n"));
    } else {
        furi_string_cat_str(
            out,
            L(
                "Whether the protocol uses\na fixed or rolling code\n"
                "cannot be told for sure.\n",
                "A protokoll mukodese (fix\nvagy ugrokod) nem\n"
                "allapithato meg biztosan.\n",
                "Ob das Protokoll Fest-\noder Rolling Code nutzt,\n"
                "ist nicht sicher\nfeststellbar.\n"));
    }
    furi_string_cat_printf(out, L("\nConfidence: %s\n", "\nMegbizhatosag: %s\n", "\nSicherheit: %s\n"), confidence_text(c));
}

static void show_text(App* app, ViewId back) {
    text_box_reset(app->text_box);
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->text));
    app->text_return = back;
    switch_to(app, ViewText);
}

/* ------------------------------------------------------------------------- */
/* Saving                                                                     */

static void sanitize_name(char* name) {
    for(char* p = name; *p; p++) {
        if(*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' ||
           *p == '<' || *p == '>' || *p == '|')
            *p = '_';
    }
    if(!name[0]) strlcpy(name, "remote", NAME_LEN);
}

static void default_name(App* app) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        app->name_buf,
        NAME_LEN,
        "RA_%02u%02u_%02u%02u",
        dt.month,
        dt.day,
        dt.hour,
        dt.minute);
}

static bool write_text_file(App* app, const char* path, FuriString* text) {
    Stream* stream = file_stream_alloc(app->storage);
    bool ok = file_stream_open(stream, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) ok = stream_write_string(stream, text) == furi_string_size(text);
    file_stream_close(stream);
    stream_free(stream);
    return ok;
}

static bool write_raw_sub(App* app, const char* path) {
    Analysis* an = &app->an;
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* preset = furi_string_alloc();
    subghz_block_generic_get_preset_name(presets[an->raw_best_preset].name, preset);
    bool ok = flipper_format_file_open_always(ff, path) &&
              flipper_format_write_header_cstr(ff, SUBGHZ_RAW_FILE_TYPE, SUBGHZ_RAW_FILE_VERSION) &&
              flipper_format_write_uint32(ff, "Frequency", &an->freq_nominal, 1) &&
              flipper_format_write_string(ff, "Preset", preset) &&
              flipper_format_write_string_cstr(ff, "Protocol", "RAW");
    for(size_t i = 0; ok && i < an->raw_best_n; i += 256) {
        size_t n = an->raw_best_n - i;
        if(n > 256) n = 256;
        int32_t line[256];
        for(size_t k = 0; k < n; k++) line[k] = an->raw_best[i + k];
        ok = flipper_format_write_int32(ff, "RAW_Data", line, n);
    }
    furi_string_free(preset);
    flipper_format_free(ff);
    return ok;
}

static void build_report(App* app, FuriString* r, const char* name, const char* sub_path) {
    Analysis* an = &app->an;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    furi_string_reset(r);
    furi_string_cat_printf(r, "Filetype: %s\nVersion: 1\n", REPORT_FILE_TYPE);
    furi_string_cat_printf(
        r,
        "Date: %04u-%02u-%02u %02u:%02u:%02u\n",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
    furi_string_cat_printf(r, "Name: %s\n", name);
    furi_string_cat_printf(r, "Mode: %s\n", an->manual ? "Manual" : "Automatic");
    furi_string_cat_printf(r, "Frequency: %lu\n", (unsigned long)an->freq_nominal);
    furi_string_cat_printf(r, "Measured_frequency: %lu\n", (unsigned long)an->freq_measured);
    furi_string_cat_printf(r, "RSSI_peak: %d\n", (int)an->rssi_peak);
    if(app->best >= 0) {
        Candidate* c = &an->cand[app->best];
        furi_string_cat_printf(r, "Identification: Decoded\n");
        furi_string_cat_printf(r, "Protocol: %s\n", c->name);
        furi_string_cat_printf(r, "Protocol_type: %s\n", type_key(c->type));
        furi_string_cat_printf(r, "Modulation: %s\n", presets[best_preset_of(c)].name);
        furi_string_cat_printf(r, "Confidence: %s\n", confidence_key(c));
        furi_string_cat_printf(
            r, "Decodes: %u\nRepeats: %u\nChanges: %u\n", c->count, c->repeats, c->changes);
    } else {
        furi_string_cat_printf(r, "Identification: Unknown\n");
        if(an->raw_best_score > 0)
            furi_string_cat_printf(
                r, "Modulation_guess: %s\n", presets[an->raw_best_preset].name);
    }
    for(uint8_t i = 0; i < PRESET_COUNT; i++) {
        PresetResult* p = &an->pr[i];
        furi_string_cat_printf(
            r,
            "Preset_%s: tested=%u decodes=%u signal_ms=%lu rssi=%d\n",
            presets[i].name,
            p->done ? 1 : 0,
            p->decodes,
            (unsigned long)p->span_ms,
            p->signal_seen ? (int)p->rssi_max : -200);
    }
    furi_string_cat_printf(r, "Sub_file: %s\n", sub_path[0] ? sub_path : "-");
    if(app->best >= 0) {
        furi_string_cat_str(r, "# Decoded data:\n");
        FuriString* info = furi_string_alloc_set(an->cand[app->best].info);
        furi_string_replace_all(info, "\r", "");
        while(furi_string_size(info) &&
              furi_string_get_char(info, furi_string_size(info) - 1) == '\n')
            furi_string_left(info, furi_string_size(info) - 1);
        furi_string_cat_str(r, "# ");
        for(size_t i = 0; i < furi_string_size(info); i++) {
            char ch = furi_string_get_char(info, i);
            if(ch == '\n')
                furi_string_cat_str(r, "\n# ");
            else
                furi_string_push_back(r, ch);
        }
        furi_string_cat_str(r, "\n");
        furi_string_free(info);
    }
}

static void do_save(App* app) {
    sanitize_name(app->name_buf);
    storage_simply_mkdir(app->storage, DATA_DIR);
    storage_simply_mkdir(app->storage, REPORT_DIR);

    /* Find a free name for both files */
    char name[NAME_LEN + 4];
    FuriString* report_path = furi_string_alloc();
    FuriString* sub_path = furi_string_alloc();
    strlcpy(name, app->name_buf, sizeof(name));
    for(uint8_t n = 2; n < 100; n++) {
        furi_string_printf(report_path, "%s/%s.txt", REPORT_DIR, name);
        furi_string_printf(sub_path, "%s/%s.sub", SUB_DIR, name);
        if(!storage_file_exists(app->storage, furi_string_get_cstr(report_path)) &&
           !storage_file_exists(app->storage, furi_string_get_cstr(sub_path)))
            break;
        snprintf(name, sizeof(name), "%s_%u", app->name_buf, n);
    }

    Analysis* an = &app->an;
    bool sub_written = false;
    bool sub_ok = true;
    const char* sub_note = "";
    if(app->best >= 0) {
        Candidate* c = &an->cand[app->best];
        if(c->type == SubGhzProtocolTypeDynamic) {
            sub_note = L(
                "Rolling code: no .sub file.",
                "Ugrokodos: nincs .sub fajl.",
                "Rolling Code:\nkeine .sub-Datei.");
        } else {
            storage_simply_mkdir(app->storage, EXT_PATH("subghz"));
            storage_simply_mkdir(app->storage, SUB_DIR);
            Stream* src = flipper_format_get_raw_stream(c->ff);
            stream_rewind(src);
            sub_ok = stream_save_to_file(
                         src, app->storage, furi_string_get_cstr(sub_path), FSOM_CREATE_ALWAYS) > 0;
            sub_written = sub_ok;
        }
    } else if(an->raw_best_n > 0) {
        storage_simply_mkdir(app->storage, EXT_PATH("subghz"));
        storage_simply_mkdir(app->storage, SUB_DIR);
        sub_ok = write_raw_sub(app, furi_string_get_cstr(sub_path));
        sub_written = sub_ok;
        sub_note = L("RAW capture (for analysis).", "RAW felvetel (elemzeshez).", "RAW-Aufnahme (zur Analyse).");
    } else {
        sub_note = L("No signal to save.", "Nincs mentheto jel.", "Kein Signal zum Speichern.");
    }

    FuriString* report = furi_string_alloc();
    build_report(app, report, name, sub_written ? furi_string_get_cstr(sub_path) : "");
    bool report_ok = write_text_file(app, furi_string_get_cstr(report_path), report);
    furi_string_free(report);

    furi_string_reset(app->text);
    if(report_ok && sub_ok) {
        furi_string_cat_printf(
            app->text,
            L("Saved: %s\n\nReport:\n%s\n", "Mentve: %s\n\nJelentes:\n%s\n", "Gespeichert: %s\n\nBericht:\n%s\n"),
            name,
            furi_string_get_cstr(report_path));
        if(sub_written)
            furi_string_cat_printf(app->text, L("\nSignal:\n%s\n", "\nJel:\n%s\n", "\nSignal:\n%s\n"), furi_string_get_cstr(sub_path));
        if(sub_note[0]) furi_string_cat_printf(app->text, "\n%s\n", sub_note);
        if(sub_written && app->best >= 0)
            furi_string_cat_str(
                app->text,
                L("\nAlso listed in the Sub-GHz\napp under Saved.\n",
                  "\nA Sub-GHz app Saved\nmenujeben is megtalalhato.\n",
                  "\nAuch in der Sub-GHz-App\nunter Saved zu finden.\n"));
    } else {
        furi_string_cat_str(
            app->text, L("Save error!\nCheck the SD card.\n", "Mentesi hiba!\nEllenorizd az SD-kartyat.\n", "Speicherfehler!\nSD-Karte pruefen.\n"));
    }
    furi_string_free(report_path);
    furi_string_free(sub_path);
    show_text(app, ViewResult);
}

static void show_saved(App* app) {
    storage_simply_mkdir(app->storage, DATA_DIR);
    storage_simply_mkdir(app->storage, REPORT_DIR);
    FuriString* path = furi_string_alloc_set(REPORT_DIR);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, ".txt", NULL);
    opts.base_path = REPORT_DIR;
    opts.hide_ext = true;
    bool selected = dialog_file_browser_show(app->dialogs, path, path, &opts);
    if(selected) {
        furi_string_reset(app->text);
        Stream* stream = file_stream_alloc(app->storage);
        if(file_stream_open(stream, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
            FuriString* line = furi_string_alloc();
            while(stream_read_line(stream, line) && furi_string_size(app->text) < 4096) {
                furi_string_cat(app->text, line);
            }
            furi_string_free(line);
        } else {
            furi_string_set(app->text, L("The file cannot be read.", "A fajl nem olvashato.", "Die Datei ist nicht lesbar."));
        }
        file_stream_close(stream);
        stream_free(stream);
        show_text(app, ViewMain);
    }
    furi_string_free(path);
}

/* ------------------------------------------------------------------------- */
/* Settings and manual screens                                                */

static void set_sensitivity(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->settings.sensitivity = i;
    variable_item_set_current_value_text(item, sensitivity_names[g_lang][i]);
    settings_save(app);
}

static void set_window(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->settings.window = i;
    variable_item_set_current_value_text(item, window_names[g_lang][i]);
    settings_save(app);
}

static void set_all_presets(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->settings.all_presets = i;
    variable_item_set_current_value_text(item, yes_no[g_lang][i]);
    settings_save(app);
}

static void set_feedback(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->settings.feedback = i;
    variable_item_set_current_value_text(item, on_off[g_lang][i]);
    settings_save(app);
}

static void set_debug_log(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->settings.debug_log = i;
    variable_item_set_current_value_text(item, on_off[g_lang][i]);
    settings_save(app);
}

static void set_language(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->settings.language = i;
    g_lang = i;
    variable_item_set_current_value_text(item, language_names[i]);
    settings_save(app);
    /* The other labels change too: rebuild the list outside this callback */
    view_dispatcher_send_custom_event(app->vd, EvLanguage);
}

static void build_settings_list(App* app) {
    VariableItemList* list = app->settings_list;
    variable_item_list_reset(list);
    VariableItem* item;
    /* The same label in every language, so the item can always be found */
    item = variable_item_list_add(list, "Language", LANG_COUNT, set_language, app);
    variable_item_set_current_value_index(item, g_lang);
    variable_item_set_current_value_text(item, language_names[g_lang]);
    item = variable_item_list_add(
        list, L("Sensitivity", "Erzekenyseg", "Empfindlichk."), COUNT_OF(sensitivity_names[0]), set_sensitivity, app);
    variable_item_set_current_value_index(item, app->settings.sensitivity);
    variable_item_set_current_value_text(item, sensitivity_names[g_lang][app->settings.sensitivity]);
    item = variable_item_list_add(
        list, L("Window", "Ablak", "Fenster"), COUNT_OF(window_names[0]), set_window, app);
    variable_item_set_current_value_index(item, app->settings.window);
    variable_item_set_current_value_text(item, window_names[g_lang][app->settings.window]);
    item = variable_item_list_add(
        list, L("All 4 presets", "Mind a 4 preset", "Alle 4 Presets"), COUNT_OF(yes_no[0]), set_all_presets, app);
    variable_item_set_current_value_index(item, app->settings.all_presets);
    variable_item_set_current_value_text(item, yes_no[g_lang][app->settings.all_presets]);
    item = variable_item_list_add(
        list, L("Feedback", "Visszajelzes", "Rueckmeldung"), COUNT_OF(on_off[0]), set_feedback, app);
    variable_item_set_current_value_index(item, app->settings.feedback);
    variable_item_set_current_value_text(item, on_off[g_lang][app->settings.feedback]);
    item = variable_item_list_add(
        list, L("Debug log", "Hibanaplo", "Debug-Log"), COUNT_OF(on_off[0]), set_debug_log, app);
    variable_item_set_current_value_index(item, app->settings.debug_log);
    variable_item_set_current_value_text(item, on_off[g_lang][app->settings.debug_log]);
    item = variable_item_list_add(list, L("Version", "Verzio", "Version"), 1, NULL, app);
    variable_item_set_current_value_text(item, APP_VERSION);
}

static void set_manual_freq(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->manual_freq = variable_item_get_current_value_index(item);
    char buf[20];
    format_freq(buf, sizeof(buf), app->freqs[app->manual_freq]);
    variable_item_set_current_value_text(item, buf);
}

static void set_manual_preset(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->manual_preset = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, manual_preset_names[app->manual_preset]);
}

static void manual_enter(void* context, uint32_t index) {
    App* app = context;
    if(index == 2) view_dispatcher_send_custom_event(app->vd, EvManualStart);
}

/* Writes the debug log of the last scan, oldest line first (after the worker stopped). The older
 * logs move one number up (debug_log_1 -> debug_log_2 ...), the one past TRACE_KEEP is deleted. */
static void trace_save(App* app) {
    if(!app->trace) return;
    storage_simply_mkdir(app->storage, DATA_DIR);
    storage_simply_mkdir(app->storage, TRACE_DIR);
    storage_simply_remove(app->storage, TRACE_OLD);
    char from[96];
    char to[96];
    snprintf(to, sizeof(to), "%s/debug_log_%u.txt", TRACE_DIR, TRACE_KEEP);
    storage_simply_remove(app->storage, to);
    for(unsigned n = TRACE_KEEP - 1; n >= 1; n--) {
        snprintf(from, sizeof(from), "%s/debug_log_%u.txt", TRACE_DIR, n);
        snprintf(to, sizeof(to), "%s/debug_log_%u.txt", TRACE_DIR, n + 1);
        if(storage_common_exists(app->storage, from)) storage_common_rename(app->storage, from, to);
    }
    snprintf(to, sizeof(to), "%s/debug_log_1.txt", TRACE_DIR);
    /* Line by line straight into the file: a full log built as one string first needed up to twice
     * its size again and ran the Flipper out of memory (0.7, High sensitivity in noise). */
    Stream* stream = file_stream_alloc(app->storage);
    if(file_stream_open(stream, to, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        DateTime* dt = &app->scan_time;
        char head[40];
        snprintf(
            head,
            sizeof(head),
            "Scan: %04u-%02u-%02u %02u:%02u:%02u\n",
            dt->year,
            dt->month,
            dt->day,
            dt->hour,
            dt->minute,
            dt->second);
        stream_write_cstring(stream, head);
        uint32_t first = app->trace_n > TRACE_LINES ? app->trace_n - TRACE_LINES : 0;
        for(uint32_t i = first; i < app->trace_n; i++) {
            stream_write_cstring(stream, app->trace[i % TRACE_LINES]);
            stream_write_char(stream, '\n');
        }
    }
    file_stream_close(stream);
    stream_free(stream);
    free(app->trace);
    app->trace = NULL;
}

static void build_manual_list(App* app) {
    VariableItemList* list = app->manual_list;
    variable_item_list_reset(list);
    char buf[20];
    VariableItem* item = variable_item_list_add(list, L("Frequency", "Frekvencia", "Frequenz"), app->freq_count, set_manual_freq, app);
    variable_item_set_current_value_index(item, app->manual_freq);
    format_freq(buf, sizeof(buf), app->freqs[app->manual_freq]);
    variable_item_set_current_value_text(item, buf);
    item = variable_item_list_add(
        list, L("Modulation", "Modulacio", "Modulation"), COUNT_OF(manual_preset_names), set_manual_preset, app);
    variable_item_set_current_value_index(item, app->manual_preset);
    variable_item_set_current_value_text(item, manual_preset_names[app->manual_preset]);
    item = variable_item_list_add(list, L("Start (OK)", "Inditas (OK)", "Start (OK)"), 1, NULL, app);
    variable_item_set_current_value_text(item, ">");
    variable_item_list_set_enter_callback(list, manual_enter, app);
}

/* ------------------------------------------------------------------------- */
/* Navigation and events                                                      */

static void details_callback(void* context, uint32_t index) {
    App* app = context;
    view_dispatcher_send_custom_event(app->vd, index);
}

static void name_done(void* context) {
    App* app = context;
    view_dispatcher_send_custom_event(app->vd, EvNameDone);
}

static bool navigation_callback(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewMain:
        return false; /* exit the app */
    case ViewScan:
        view_dispatcher_send_custom_event(app->vd, EvScanCancel);
        return true;
    case ViewResult:
    case ViewSettings:
    case ViewManual:
        switch_to(app, ViewMain);
        return true;
    case ViewDetails:
    case ViewTextInput:
        switch_to(app, ViewResult);
        return true;
    case ViewText:
        switch_to(app, app->text_return);
        return true;
    }
    return false;
}

static bool custom_event_callback(void* context, uint32_t event) {
    App* app = context;
    switch(event) {
    case EvMenuAuto:
        scan_start(app, false);
        return true;
    case EvMenuManual:
        if(app->freq_count == 0) return true;
        build_manual_list(app);
        switch_to(app, ViewManual);
        return true;
    case EvManualStart:
        scan_start(app, true);
        return true;
    case EvMenuSaved:
        show_saved(app);
        return true;
    case EvMenuSettings:
        build_settings_list(app);
        switch_to(app, ViewSettings);
        return true;
    case EvLanguage:
        build_settings_list(app);
        variable_item_list_set_selected_item(app->settings_list, 0);
        return true;
    case EvScanDone:
        furi_thread_join(app->thread);
        trace_save(app);
        notification_message(app->notif, &sequence_display_backlight_enforce_auto);
        build_result(app);
        show_result(app);
        return true;
    case EvScanFinish:
        scan_stop(app, FLAG_FINISH); /* the worker sends EvScanDone */
        return true;
    case EvScanCancel:
        scan_stop(app, FLAG_STOP);
        trace_save(app);
        switch_to(app, ViewMain);
        return true;
    case EvResultNew:
        scan_start(app, app->an.manual);
        return true;
    case EvResultSave:
        default_name(app);
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, L("Name for saving", "Nev a menteshez", "Name zum Speichern"));
        text_input_set_result_callback(
            app->text_input, name_done, app, app->name_buf, NAME_LEN, true); /* typing replaces the offered name */
        switch_to(app, ViewTextInput);
        return true;
    case EvNameDone:
        do_save(app);
        return true;
    case EvResultMore:
        submenu_reset(app->details);
        submenu_set_header(app->details, L("Details", "Reszletek", "Details"));
        submenu_add_item(app->details, L("Detailed data", "Reszletes adatok", "Detaildaten"), EvDetailData, details_callback, app);
        submenu_add_item(app->details, L("Modulations", "Modulaciok", "Modulationen"), EvDetailModulation, details_callback, app);
        submenu_add_item(app->details, L("Compatibility", "Kompatibilitas", "Kompatibilitaet"), EvDetailCompat, details_callback, app);
        switch_to(app, ViewDetails);
        return true;
    case EvDetailData:
        detail_data_text(app, app->text);
        show_text(app, ViewDetails);
        return true;
    case EvDetailModulation:
        detail_modulation_text(app, app->text);
        show_text(app, ViewDetails);
        return true;
    case EvDetailCompat:
        detail_compat_text(app, app->text);
        show_text(app, ViewDetails);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* App lifecycle                                                              */

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->text = furi_string_alloc();
    app->summary = furi_string_alloc();
    app->preset_name = furi_string_alloc();
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->best = -1;
    settings_load(app);

    /* Radio and decoders */
    subghz_devices_init();
    app->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    app->setting = subghz_setting_alloc();
    subghz_setting_load(app->setting, EXT_PATH("subghz/assets/setting_user"));
    build_frequency_list(app);
    for(size_t i = 0; i < app->freq_count; i++)
        if(app->freqs[i] == 433920000) app->manual_freq = i;
    app->narrow_preset = build_narrow_preset();

    app->env = subghz_environment_alloc();
    /* The KeeLoq manufacturer keystore is not loaded: it takes about 9 KB, and with it the app ran
     * the Flipper out of memory when a scan started. KeeLoq is still decoded and shown as rolling
     * code; only the manufacturer name is missing (the Sub-GHz app shows it). */
    subghz_environment_set_came_atomo_rainbow_table_file_name(app->env, SUBGHZ_CAME_ATOMO_DIR_NAME);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        app->env, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(
        app->env, SUBGHZ_NICE_FLOR_S_DIR_NAME);
    subghz_environment_set_protocol_registry(app->env, (void*)&subghz_protocol_registry);
    app->receiver = subghz_receiver_alloc_init(app->env);
    subghz_receiver_set_filter(app->receiver, SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_ignore_filter(
        app->receiver, SubGhzProtocolFilter_Weather | SubGhzProtocolFilter_TPMS);
    subghz_receiver_set_rx_callback(app->receiver, receiver_callback, app);

    app->sworker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(app->sworker, overrun_callback);
    subghz_worker_set_pair_callback(app->sworker, pair_callback);
    subghz_worker_set_context(app->sworker, app);

    for(size_t i = 0; i < CAND_MAX; i++) {
        app->an.cand[i].info = furi_string_alloc();
        app->an.cand[i].ff = flipper_format_string_alloc();
    }
    app->an.raw_cur = malloc(RAW_MAX * sizeof(int16_t));
    app->an.raw_best = malloc(RAW_MAX * sizeof(int16_t));

    app->thread = furi_thread_alloc_ex("RscRemoteAnalyzerWorker", 4096, worker_thread, app);

    /* GUI */
    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->vd, navigation_callback);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLocking, sizeof(MainModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, main_draw);
    view_set_input_callback(app->main_view, main_input);
    view_dispatcher_add_view(app->vd, ViewMain, app->main_view);

    app->scan_view = view_alloc();
    view_allocate_model(app->scan_view, ViewModelTypeLocking, sizeof(ScanModel));
    view_set_context(app->scan_view, app);
    view_set_draw_callback(app->scan_view, scan_draw);
    view_set_input_callback(app->scan_view, scan_input);
    view_dispatcher_add_view(app->vd, ViewScan, app->scan_view);

    app->result = widget_alloc();
    view_dispatcher_add_view(app->vd, ViewResult, widget_get_view(app->result));
    app->details = submenu_alloc();
    view_dispatcher_add_view(app->vd, ViewDetails, submenu_get_view(app->details));
    app->text_box = text_box_alloc();
    view_dispatcher_add_view(app->vd, ViewText, text_box_get_view(app->text_box));
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(app->vd, ViewTextInput, text_input_get_view(app->text_input));
    app->settings_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->vd, ViewSettings, variable_item_list_get_view(app->settings_list));
    app->manual_list = variable_item_list_alloc();
    view_dispatcher_add_view(app->vd, ViewManual, variable_item_list_get_view(app->manual_list));

    return app;
}

static void app_free(App* app) {
    scan_stop(app, FLAG_STOP);
    furi_thread_free(app->thread);

    view_dispatcher_remove_view(app->vd, ViewMain);
    view_dispatcher_remove_view(app->vd, ViewScan);
    view_dispatcher_remove_view(app->vd, ViewResult);
    view_dispatcher_remove_view(app->vd, ViewDetails);
    view_dispatcher_remove_view(app->vd, ViewText);
    view_dispatcher_remove_view(app->vd, ViewTextInput);
    view_dispatcher_remove_view(app->vd, ViewSettings);
    view_dispatcher_remove_view(app->vd, ViewManual);
    view_free(app->main_view);
    view_free(app->scan_view);
    widget_free(app->result);
    submenu_free(app->details);
    text_box_free(app->text_box);
    text_input_free(app->text_input);
    variable_item_list_free(app->settings_list);
    variable_item_list_free(app->manual_list);
    view_dispatcher_free(app->vd);

    subghz_worker_free(app->sworker);
    subghz_receiver_free(app->receiver);
    subghz_environment_free(app->env);
    subghz_setting_free(app->setting);
    subghz_devices_deinit();
    free(app->narrow_preset);
    for(size_t i = 0; i < CAND_MAX; i++) {
        furi_string_free(app->an.cand[i].info);
        flipper_format_free(app->an.cand[i].ff);
    }
    free(app->an.raw_cur);
    free(app->an.raw_best);
    if(app->trace) free(app->trace);

    furi_mutex_free(app->mutex);
    furi_string_free(app->text);
    furi_string_free(app->summary);
    furi_string_free(app->preset_name);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t rsc_remote_analyzer_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    switch_to(app, ViewMain);
    view_dispatcher_run(app->vd);
    app_free(app);
    return 0;
}
