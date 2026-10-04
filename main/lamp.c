/* See lamp.h. Ported from an earlier Python implementation; rounding uses
 * rint() (round-half-even, like Python's round()) to keep its exact results. */
#include "lamp.h"

#include <math.h>
#include <string.h>

/* ---- codec ----
 * Tuya BLE beacon frame codec, reverse-engineered from the Smart Life app
 * (classes14.dex: com.thingclips.sdk.bluetooth.pbddddb / ddbqqbd / bbdqqbd).
 * Frame = lead(1) | src(2) dst(2) sn(2) sub_cmd(1) crc(plaintext)(1) |
 * XXTEA(plaintext, local_key) XOR app_key (16) | crc(1). */

#define DELTA 0x9E3779B9u

static void to_words_be(const uint8_t *b, uint32_t *w, int n_words)
{
    for (int i = 0; i < n_words; i++) {
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    }
}

static void from_words_be(const uint32_t *w, uint8_t *b, int n_words)
{
    for (int i = 0; i < n_words; i++) {
        b[4 * i] = w[i] >> 24;
        b[4 * i + 1] = w[i] >> 16;
        b[4 * i + 2] = w[i] >> 8;
        b[4 * i + 3] = w[i];
    }
}

#define MX (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3) ^ e] ^ z))

/* 16-byte block only -- all this protocol ever encrypts. */
static void xxtea_encrypt16(uint8_t data[16], const uint8_t key[16])
{
    uint32_t v[4], k[4];
    to_words_be(data, v, 4);
    to_words_be(key, k, 4);
    const int n = 4;
    uint32_t sum = 0, y, z = v[n - 1], e;
    int p;
    for (int rounds = 6 + 52 / n; rounds > 0; rounds--) {
        sum += DELTA;
        e = (sum >> 2) & 3;
        for (p = 0; p < n - 1; p++) {
            y = v[p + 1];
            z = v[p] += MX;
        }
        y = v[0];
        z = v[n - 1] += MX;
    }
    from_words_be(v, data, 4);
}

static uint8_t crc16_high_byte(const uint8_t *data, int count)
{
    uint16_t s = 0;
    for (int i = 0; i < count; i++) {
        s ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) {
            if (s & 0x8000) {
                s ^= 33664;
            }
            s <<= 1;
        }
    }
    return s >> 8;
}

void lamp_encode_frame(const uint8_t *plaintext, int len, const lamp_keys_t *k, uint16_t sn,
                       uint8_t sub_cmd, uint8_t lead_byte, uint8_t out[LAMP_FRAME_LEN])
{
    uint8_t plain16[16] = {0};
    memcpy(plain16, plaintext, len);

    /* out = lead(1) | header8 | ciphertext(16) | trailer crc(1) */
    uint8_t *header8 = &out[1], *cipher = &out[9];
    memcpy(&header8[0], k->src_addr, 2);
    memcpy(&header8[2], k->dst_addr, 2);
    header8[4] = sn >> 8;
    header8[5] = sn & 0xFF;
    header8[6] = sub_cmd;
    header8[7] = crc16_high_byte(plain16, 16);

    memcpy(cipher, plain16, 16);
    xxtea_encrypt16(cipher, k->local_key);

    /* trailer CRC covers the PRE-xor inner frame, then the appKey XOR mutates it */
    out[0] = lead_byte & 0xFC;
    out[25] = crc16_high_byte(out, 25);
    out[0] = lead_byte;
    for (int i = 0; i < 16; i++) {
        cipher[i] ^= k->app_key[i];
    }
}

/* ---- frame builders ----
 * DP payload = [dpId][type<<4 | len][value...]. Byte layouts below are real
 * reverse-engineered protocol knowledge, each confirmed on the device:
 * - dp=11 (static color): [hue_hi, hue_lo, sat, bri]. Hue is a 16-bit degree
 *   value 0-359, not one clipped byte: a capture of the app's color wheel
 *   showed 01 2c = 300 for purple (red/green/blue all being < 256 had hidden
 *   the high byte). sat/bri 0-100.
 * - dp=73 (scene effects): [revision, 0, pattern, 1, speed, bri, colors].
 *   speed/bri order confirmed by an isolating test (static pattern, where
 *   speed can't show, came out full at bri=100). Speed is a full byte but the
 *   lamp only takes 0-100: 120 made it fall back to static red.
 * - sub_cmd 5 = "P2Dps" DP write; lead byte 0x0B matched a real capture. */

#define DP_ID_SCENE 73
#define DP_ID_COLOR 11
#define SUB_CMD_P2DPS 5
#define LEAD_BYTE_P2DPS 0x0B

void lamp_build_frame(const lamp_target_t *t, const lamp_keys_t *k, uint16_t sn, uint8_t revision,
                      uint8_t out[LAMP_FRAME_LEN])
{
    /* [dpId][type<<4 | len][value...], type 0 */
    uint8_t plain[9];
    int len;
    if (t->kind == TARGET_COLOR) {
        uint8_t p[] = {DP_ID_COLOR, 4, t->a >> 8, t->a & 0xFF, t->b, t->c};
        len = sizeof(p);
        memcpy(plain, p, len);
    } else {
        uint8_t p[] = {DP_ID_SCENE, 7, revision, 0x00, t->a, 0x01, t->c, t->d, t->b};
        len = sizeof(p);
        memcpy(plain, p, len);
    }
    lamp_encode_frame(plain, len, k, sn, SUB_CMD_P2DPS, LEAD_BYTE_P2DPS, out);
}

#define COLOR_BLUE 0x02
#define COLOR_GREEN 0x04
#define COLOR_CYAN 0x08
#define COLOR_YELLOW 0x10
#define COLOR_PURPLE 0x20
#define COLORS_TRICOLOR (COLOR_RED | COLOR_GREEN | COLOR_BLUE)
#define COLORS_FOUR_COLOR (COLOR_YELLOW | COLOR_CYAN | COLOR_PURPLE | COLOR_WHITE)
#define COLORS_SEVEN_COLOR 0x7F
#define SOLO_COLORS COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, COLOR_CYAN, COLOR_PURPLE, COLOR_WHITE
#define COMBOS COLORS_TRICOLOR, COLORS_FOUR_COLOR, COLORS_SEVEN_COLOR
#define RGB_PAIRS (COLOR_RED | COLOR_GREEN), (COLOR_RED | COLOR_BLUE), (COLOR_GREEN | COLOR_BLUE)

/* Valid color presets per pattern, 0-terminated. The color field is NOT a
 * free bitmask: only these combinations work, re-verified against the Smart
 * Life app's preset menus (2026-09-10). Bits come from single-bit device
 * tests. The app's flash menu lacks solo blue, but the lamp flashes blue fine
 * (a UI restriction, not firmware), so it's included. */
static const uint8_t VALID_COLORS[5][11] = {
    [PATTERN_STATIC] = {SOLO_COLORS},
    [PATTERN_JUMP] = {COMBOS, RGB_PAIRS},
    [PATTERN_GRADIENT] = {COMBOS, RGB_PAIRS},
    [PATTERN_FLASH] = {COMBOS, SOLO_COLORS},
    [PATTERN_BREATH] = {COMBOS, SOLO_COLORS},
};

bool lamp_colors_valid(int pattern, int colors)
{
    if (pattern < 0 || pattern > PATTERN_BREATH) {
        return false;
    }
    for (const uint8_t *c = VALID_COLORS[pattern]; *c; c++) {
        if (*c == colors) {
            return true;
        }
    }
    return false;
}

/* ---- scene state ----
 * rate_bpm: projectMac's visual/bpm (on-screen strobe rate) normally wins,
 * but once the dominant hue has held still past the threshold there's no
 * visual rate to track, so it falls back to the audio tempo/bpm -- and the
 * mapping breaks the static scene up with a strobe of that color on the beat. */

#define HUE_HOLD_TOLERANCE_DEG 8.0

static double pymod(double a, double m) /* Python's float %: result takes the divisor's sign */
{
    double r = fmod(a, m);
    return r < 0 ? r + m : r;
}

static double circular_distance(double a, double b)
{
    double d = fmod(fabs(a - b), 360);
    return d < 360 - d ? d : 360 - d;
}

void lamp_scene_init(lamp_scene_t *s, double now)
{
    *s = (lamp_scene_t){.tempo_bpm = 120.0, .visual_bpm = 120.0, .visual_scale_pct = 50,
                        .audio_scale_pct = 100, .hue_hold_since = now};
}

void lamp_set_vibrant(lamp_scene_t *s, double h, double sat, double v, double now)
{
    double hue_deg = h * 360;
    if (circular_distance(hue_deg, s->hue_hold_ref_deg) > HUE_HOLD_TOLERANCE_DEG) {
        s->hue_hold_ref_deg = hue_deg;
        s->hue_hold_since = now;
    }
    s->vibrant_h = h;
    s->vibrant_s = sat;
    s->vibrant_v = v;
}

/* A hue that survives one full audio beat counts as held, so holds get broken
 * up sooner in faster music. 0.5s if there's no usable tempo. */
static bool hue_held(const lamp_scene_t *s, double now)
{
    double beat = s->tempo_bpm > 0 ? 60 / s->tempo_bpm : 0.5;
    return now - s->hue_hold_since > beat;
}

double lamp_rate_bpm(const lamp_scene_t *s, double now)
{
    return hue_held(s, now) ? s->tempo_bpm * s->audio_scale_pct / 100.0
                            : s->visual_bpm * s->visual_scale_pct / 100.0;
}

/* ---- mapping ----
 * Three modes: color-follow (dp=11 from projectMac's vibrant HSV), pattern
 * (dp=73, effect picked by bpm band and speed matched to the beat), and
 * combined (pattern, except dp=11 for hues the fixed palette can't show). */

#define BPM_MIN 70.0
#define BPM_MAX 160.0
#define SPEED_MIN 10
#define SPEED_MAX 100
#define WHITE_SATURATION_THRESHOLD 0.15
#define GAP_THRESHOLD_DEG 25

static const struct { double hue; int bit; } HUE_REFERENCE[] = {
    {0.0, COLOR_RED}, {60.0, COLOR_YELLOW}, {120.0, COLOR_GREEN},
    {180.0, COLOR_CYAN}, {240.0, COLOR_BLUE}, {300.0, COLOR_PURPLE},
};
#define N_HUE_REFERENCE 6
static const struct { double hue; int bit; } RGB_HUE_REFERENCE[] = {
    {0.0, COLOR_RED}, {120.0, COLOR_GREEN}, {240.0, COLOR_BLUE},
};

/* Seconds per color step = (101 - speed) * base, measured from 240fps
 * slow-motion video of the lamp (2026-10-04): held to 0.2% at speeds 70 and
 * 90 for jump and gradient alike, so 100 is exactly twice as fast as 99.
 * "bpm" here means color steps per minute, whatever the preset's color count.
 * ponytail: breath assumed = gradient and flash = jump (unmeasured); one
 * slow-mo clip each would confirm. */
static const double STEP_BASE_SECONDS[5] = {
    [PATTERN_BREATH] = 0.150,
    [PATTERN_GRADIENT] = 0.150,
    [PATTERN_JUMP] = 0.015,
    [PATTERN_FLASH] = 0.015,
};

static int py_round(double x)
{
    return (int)rint(x);
}

static int speed_for_rate(double steps_per_min, int pattern)
{
    double speed = 101 - 60 / (steps_per_min * STEP_BASE_SECONDS[pattern]);
    return py_round(fmin(fmax(speed, SPEED_MIN), SPEED_MAX));
}

/* Breath/gradient only reach a few rates near music tempos (speeds 97-99 at a
 * step per beat), so they may take a step every 2 or 4 beats, whichever lands
 * closest to the beat grid; ties keep fewer beats. Jump/flash already track
 * any 70-160 bpm within ~2% at a step per beat. */
static int speed_for_bpm(double bpm, int pattern)
{
    static const int BEATS[] = {1, 2, 4};
    int n_beats = (pattern == PATTERN_BREATH || pattern == PATTERN_GRADIENT) ? 3 : 1;
    if (bpm <= 0) {
        return SPEED_MIN;
    }
    int best_speed = SPEED_MIN;
    double best_error = INFINITY;
    for (int i = 0; i < n_beats; i++) {
        double target = bpm / BEATS[i];
        int speed = speed_for_rate(target, pattern);
        double error = fabs(60 / ((101 - speed) * STEP_BASE_SECONDS[pattern]) - target) / target;
        if (error < best_error) {
            best_speed = speed;
            best_error = error;
        }
    }
    return best_speed;
}

static int pattern_for_bpm(double bpm)
{
    const double width = (BPM_MAX - BPM_MIN) / 4;
    if (bpm < BPM_MIN + 1 * width) return PATTERN_BREATH;
    if (bpm < BPM_MIN + 2 * width) return PATTERN_GRADIENT;
    if (bpm < BPM_MIN + 3 * width) return PATTERN_JUMP;
    return PATTERN_FLASH;
}

static int nearest_reference_bit(double hue_deg, int pattern /* -1 = any */)
{
    int best = -1;
    double best_d = 0;
    for (int i = 0; i < N_HUE_REFERENCE; i++) {
        if (pattern >= 0 && !lamp_colors_valid(pattern, HUE_REFERENCE[i].bit)) {
            continue;
        }
        double d = circular_distance(hue_deg, HUE_REFERENCE[i].hue);
        if (best < 0 || d < best_d) { /* strict < keeps the first of ties, like min() */
            best = HUE_REFERENCE[i].bit;
            best_d = d;
        }
    }
    return best;
}

static int hue_sat_to_color_bits(double hue_deg, double sat, int pattern)
{
    if (sat < WHITE_SATURATION_THRESHOLD) {
        return COLOR_WHITE;
    }
    return nearest_reference_bit(hue_deg, pattern);
}

static int hue_sat_to_color_combo(double hue_deg, double sat)
{
    if (sat < WHITE_SATURATION_THRESHOLD) {
        return COLORS_SEVEN_COLOR;
    }
    int nearest = nearest_reference_bit(hue_deg, -1);
    if (nearest != COLOR_RED && nearest != COLOR_GREEN && nearest != COLOR_BLUE) {
        return COLORS_FOUR_COLOR;
    }
    for (int i = 0; i < 3; i++) {
        double lo = RGB_HUE_REFERENCE[i].hue, hi = RGB_HUE_REFERENCE[(i + 1) % 3].hue;
        if (pymod(hue_deg - lo, 360) < pymod(hi - lo, 360)) {
            return RGB_HUE_REFERENCE[i].bit | RGB_HUE_REFERENCE[(i + 1) % 3].bit;
        }
    }
    return COLORS_TRICOLOR; /* unreachable */
}

static bool covered_by_pattern_palette(double hue_deg, double sat)
{
    if (sat < WHITE_SATURATION_THRESHOLD) {
        return true;
    }
    for (int i = 0; i < N_HUE_REFERENCE; i++) {
        if (circular_distance(hue_deg, HUE_REFERENCE[i].hue) < GAP_THRESHOLD_DEG) {
            return true;
        }
    }
    return false;
}

lamp_target_t lamp_color_follow(const lamp_scene_t *s)
{
    return (lamp_target_t){TARGET_COLOR, py_round(s->vibrant_h * 360) % 360, py_round(s->vibrant_s * 100),
                           py_round(s->vibrant_v * 100), 0};
}

static lamp_target_t pattern_follow(const lamp_scene_t *s, double now)
{
    double bpm = lamp_rate_bpm(s, now);
    /* a held hue (only brightness drifting, if anything) is boring as-is:
     * strobe that color on the audio beat instead of the tempo band's effect */
    int pattern = hue_held(s, now) ? PATTERN_FLASH : pattern_for_bpm(bpm);
    double hue_deg = s->vibrant_h * 360;
    int colors = (pattern == PATTERN_JUMP || pattern == PATTERN_GRADIENT)
                     ? hue_sat_to_color_combo(hue_deg, s->vibrant_s)
                     : hue_sat_to_color_bits(hue_deg, s->vibrant_s, pattern);
    int brightness = s->brightness != 0 ? py_round(s->brightness * 100) : 100;
    return (lamp_target_t){TARGET_PATTERN, pattern, colors, speed_for_bpm(bpm, pattern), brightness};
}

lamp_target_t lamp_map(const lamp_scene_t *s, int mode, double now)
{
    if (mode == MODE_COLOR) {
        return lamp_color_follow(s);
    }
    if (mode == MODE_PATTERN || hue_held(s, now) || covered_by_pattern_palette(s->vibrant_h * 360, s->vibrant_s)) {
        return pattern_follow(s, now);
    }
    return lamp_color_follow(s);
}
