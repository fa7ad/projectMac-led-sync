/* Host-side check of lamp.c (no ESP-IDF needed). From the repo root:
 *
 *   cc -Wall -Werror -o /tmp/test_lamp test/test_lamp.c main/lamp.c -lm && /tmp/test_lamp
 *
 * The golden frames came from the original Python codec (cross-checked by its
 * own decoder) with synthetic, non-device keys. */
#include <stdio.h>
#include <string.h>
#include "../main/lamp.h"

static int failures;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                          \
        }                                                        \
    } while (0)

static const lamp_keys_t KEYS = {
    .local_key = "0123456789abcdef",
    .app_key = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
    .src_addr = {0xaa, 0xbb},
    .dst_addr = {0xcc, 0xdd},
};

static lamp_scene_t scene(double h, double s, double v, double visual_bpm)
{
    lamp_scene_t sc;
    lamp_scene_init(&sc, 0);
    lamp_set_vibrant(&sc, h, s, v, 0);
    sc.visual_bpm = visual_bpm;
    sc.visual_scale_pct = 100; /* mapping cases below use the raw rate */
    return sc;
}

static bool target_is(lamp_target_t t, int kind, int a, int b, int c, int d)
{
    return t.kind == kind && t.a == a && t.b == b && t.c == c && t.d == d;
}

static void test_frames(void)
{
    static const uint8_t COLOR[LAMP_FRAME_LEN] = {
        0x0b, 0xaa, 0xbb, 0xcc, 0xdd, 0x04, 0xd2, 0x05, 0x3f, 0x05, 0x11, 0xf8, 0xf0,
        0xcb, 0xc9, 0x9a, 0x20, 0x0c, 0xa9, 0x84, 0xcb, 0xd3, 0x6d, 0x4e, 0x25, 0x8f};
    static const uint8_t PATTERN[LAMP_FRAME_LEN] = {
        0x0b, 0xaa, 0xbb, 0xcc, 0xdd, 0x9c, 0x40, 0x05, 0x8b, 0xd4, 0xc5, 0x59, 0x86,
        0x0a, 0x9b, 0x6a, 0x33, 0x0c, 0x96, 0xf0, 0x02, 0xf3, 0xd1, 0x61, 0xd9, 0x25};
    uint8_t out[LAMP_FRAME_LEN];

    /* hue 300 needs both bytes of dp=11's 16-bit hue field */
    lamp_build_frame(&(lamp_target_t){TARGET_COLOR, 300, 80, 90, 0}, &KEYS, 1234, 0, out);
    CHECK(memcmp(out, COLOR, LAMP_FRAME_LEN) == 0);
    lamp_build_frame(&(lamp_target_t){TARGET_PATTERN, PATTERN_JUMP, 0x07, 50, 100}, &KEYS, 40000, 7, out);
    CHECK(memcmp(out, PATTERN, LAMP_FRAME_LEN) == 0);
}

static void test_valid_colors(void)
{
    CHECK(lamp_colors_valid(PATTERN_STATIC, COLOR_RED));
    CHECK(!lamp_colors_valid(PATTERN_STATIC, 0x07)); /* static: solo colors only */
    CHECK(lamp_colors_valid(PATTERN_FLASH, 0x02));   /* flash+blue works, the app just hides it */
    CHECK(!lamp_colors_valid(PATTERN_JUMP, 0x11));   /* red+yellow: no cross-family pairs */
    CHECK(!lamp_colors_valid(-1, COLOR_RED));
}

static void test_mapping(void)
{
    lamp_scene_t sc = scene(0.5, 0.8, 0.9, 120);
    CHECK(target_is(lamp_map(&sc, MODE_COLOR, 0), TARGET_COLOR, 180, 80, 90, 0));
    sc = scene(300 / 360.0, 1, 1, 120);
    CHECK(target_is(lamp_map(&sc, MODE_COLOR, 0), TARGET_COLOR, 300, 100, 100, 0));

    /* pattern by bpm band, speed from the measured step timing; breath/gradient
     * pick the beat division that lands closest (70 bpm -> a step per 4 beats) */
    sc = scene(0, 1, 1, 70);
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 0), TARGET_PATTERN, PATTERN_BREATH, COLOR_RED, 78, 100));
    sc = scene(30 / 360.0, 1, 1, 100); /* orange brackets to red+green */
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 0), TARGET_PATTERN, PATTERN_GRADIENT, 0x05, 97, 100));
    sc = scene(60 / 360.0, 1, 1, 100); /* yellow arc falls back to four-color */
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 0), TARGET_PATTERN, PATTERN_GRADIENT, 0x78, 97, 100));
    sc = scene(0, 1, 1, 129); /* 129 steps/min = jump speed 70, measured on video */
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 0), TARGET_PATTERN, PATTERN_JUMP, 0x05, 70, 100));
    sc = scene(240 / 360.0, 1, 1, 155);
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 0), TARGET_PATTERN, PATTERN_FLASH, 0x02, 75, 100));

    /* combined: dp=11 only for hues the fixed palette can't show */
    sc = scene(0, 1, 1, 120);
    CHECK(lamp_map(&sc, MODE_COMBINED, 0).kind == TARGET_PATTERN);
    sc = scene(90 / 360.0, 1, 1, 120);
    CHECK(target_is(lamp_map(&sc, MODE_COMBINED, 0), TARGET_COLOR, 90, 100, 100, 0));
    sc = scene(90 / 360.0, 0.05, 1, 120); /* near-white counts as covered */
    CHECK(lamp_map(&sc, MODE_COMBINED, 0).kind == TARGET_PATTERN);

    sc = scene(0, 1, 1, 80);
    sc.tempo_bpm = 160; /* one beat = 0.375s: not held at 0.3, held at 0.4 */
    CHECK(lamp_map(&sc, MODE_PATTERN, 0.3).a != PATTERN_FLASH);
    CHECK(lamp_map(&sc, MODE_PATTERN, 0.4).a == PATTERN_FLASH);

    /* hue held past a beat: strobe the held color on the audio beat (flash,
     * 120 bpm -> speed 68), in pattern and combined mode alike -- even for a
     * gap hue combined would otherwise show as dp=11 (90 deg -> nearest yellow) */
    sc = scene(0, 1, 1, 80);
    sc.tempo_bpm = 120;
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 1.0), TARGET_PATTERN, PATTERN_FLASH, COLOR_RED, 68, 100));
    sc = scene(90 / 360.0, 1, 1, 80);
    sc.tempo_bpm = 120;
    CHECK(target_is(lamp_map(&sc, MODE_COMBINED, 1.0), TARGET_PATTERN, PATTERN_FLASH, 0x10, 68, 100));
    CHECK(lamp_map(&sc, MODE_COLOR, 1.0).kind == TARGET_COLOR); /* color mode stays a pure follow */
    sc.audio_scale_pct = 200; /* 2x: two flashes per beat (240 -> speed 84), hold still one real beat */
    CHECK(target_is(lamp_map(&sc, MODE_PATTERN, 1.0), TARGET_PATTERN, PATTERN_FLASH, 0x10, 84, 100));
}

static void test_rate_bpm_hold(void)
{
    lamp_scene_t sc = scene(0, 1, 1, 140);
    sc.tempo_bpm = 90;
    CHECK(lamp_rate_bpm(&sc, 0.3) == 140); /* visual wins while the hue moves */
    lamp_set_vibrant(&sc, 0.01, 1, 1, 0.5); /* 3.6 deg drift: hold not reset */
    CHECK(lamp_rate_bpm(&sc, 0.7) == 90);   /* held past one beat (0.67s at 90): audio tempo */
    lamp_set_vibrant(&sc, 0.5, 1, 1, 0.8);  /* real hue change resets the hold */
    CHECK(lamp_rate_bpm(&sc, 0.9) == 140);
    sc.visual_scale_pct = 50; /* scales visual only... */
    CHECK(lamp_rate_bpm(&sc, 0.9) == 70);
    CHECK(lamp_rate_bpm(&sc, 2.0) == 90); /* ...not the audio fallback */
    sc.audio_scale_pct = 200;              /* which has its own scale */
    CHECK(lamp_rate_bpm(&sc, 2.0) == 180);
    CHECK(lamp_rate_bpm(&sc, 0.9) == 70);
    lamp_scene_init(&sc, 0);
    CHECK(sc.visual_scale_pct == 50 && sc.audio_scale_pct == 100); /* defaults */
}

int main(void)
{
    test_frames();
    test_valid_colors();
    test_mapping();
    test_rate_bpm_hold();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
