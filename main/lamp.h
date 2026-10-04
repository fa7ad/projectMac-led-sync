/* Pure lamp logic -- Tuya beacon codec, DP frame builders, scene-stream
 * mapping. The reverse-engineering notes behind the constants live next to
 * them in lamp.c.
 *
 * No ESP-IDF headers on purpose: test/test_lamp.c compiles this on the host. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAMP_FRAME_LEN 26

enum { PATTERN_STATIC, PATTERN_JUMP, PATTERN_GRADIENT, PATTERN_FLASH, PATTERN_BREATH };
enum { MODE_COLOR, MODE_PATTERN, MODE_COMBINED };
enum { TARGET_COLOR, TARGET_PATTERN };

#define COLOR_RED 0x01
#define COLOR_WHITE 0x40

typedef struct {
    uint8_t local_key[16];
    uint8_t app_key[16];
    uint8_t src_addr[2];
    uint8_t dst_addr[2];
} lamp_keys_t;

/* Only the scene-stream fields something actually reads. */
typedef struct {
    double vibrant_h, vibrant_s, vibrant_v; /* 0.0-1.0 each */
    double brightness;
    double tempo_bpm, visual_bpm;
    double hue_hold_ref_deg, hue_hold_since; /* seconds, caller's monotonic clock */
} lamp_scene_t;

/* kind=TARGET_COLOR: a,b,c = hue 0-359, saturation 0-100, brightness 0-100.
 * kind=TARGET_PATTERN: a,b,c,d = pattern, colors, speed, brightness. */
typedef struct {
    int kind, a, b, c, d;
} lamp_target_t;

void lamp_scene_init(lamp_scene_t *s, double now);
void lamp_set_vibrant(lamp_scene_t *s, double h, double sat, double v, double now);
double lamp_rate_bpm(const lamp_scene_t *s, double now);
lamp_target_t lamp_map(const lamp_scene_t *s, int mode, double now);
lamp_target_t lamp_color_follow(const lamp_scene_t *s);
bool lamp_colors_valid(int pattern, int colors);

void lamp_encode_frame(const uint8_t *plaintext, int len, const lamp_keys_t *k, uint16_t sn,
                       uint8_t sub_cmd, uint8_t lead_byte, uint8_t out[LAMP_FRAME_LEN]);
void lamp_build_frame(const lamp_target_t *t, const lamp_keys_t *k, uint16_t sn, uint8_t revision,
                      uint8_t out[LAMP_FRAME_LEN]);
