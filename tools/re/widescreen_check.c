// Runs the game's projection routine (0x3ec310) natively, original and with the --widescreen cave (bytes from jstars_loader.c).
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "cave_bytes.h" /* written by widescreen_check.py from runtime/jstars_loader.c */
enum { SITE = 0x3ec310, RESUME = 0x3ec319, CAVE = 0x730140, GOT_TANF = 0x538cf6 + 0x2ab45a, SIZE = 0x800000 };
static uint8_t* img0; static size_t img_len;
static float cell;
typedef void (*Proj)(float* out, float fov, float scale, float aspect, float n, float f);
static void run(int patched, float fov, float scale, float aspect, float k, float* m00, float* m11) {
    uint8_t* m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(m, img0, img_len < SIZE ? img_len : SIZE);
    float (*t)(float) = tanf; memcpy(m + GOT_TANF, &t, 8);
    if (patched) {
        uint8_t c[0x58]; memcpy(c, cave_code, sizeof c);
        uint64_t a = (uint64_t)(uintptr_t)&cell; memcpy(c + 2, &a, 8);
        int32_t back = RESUME - (CAVE + 0x36); memcpy(c + 0x32, &back, 4);
        memcpy(m + CAVE, c, sizeof c);
        m[SITE] = 0xe9; int32_t j = CAVE - (SITE + 5); memcpy(m + SITE + 1, &j, 4); memset(m + SITE + 5, 0x90, 4);
    }
    cell = k;
    float out[16] __attribute__((aligned(16)));
    ((Proj)(m + SITE))(out, fov, scale, aspect, 0.5f, 5000.0f);
    *m00 = out[0]; *m11 = out[5];
    munmap(m, SIZE);
}
static int fails;
static void check(const char* name, float a0, float a1, double w0, double w1) {
    int ok = fabs(a0 - w0) < 1e-4 * fmax(1, fabs(w0)) && fabs(a1 - w1) < 1e-4 * fmax(1, fabs(w1));
    fails += !ok; printf("%s %s: m00 %.5f m11 %.5f (want %.5f %.5f)\n", ok ? "PASS" : "FAIL", name, a0, a1, w0, w1);
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); img_len = ftell(f); rewind(f);
    img0 = malloc(img_len); if (fread(img0, 1, img_len, f) != img_len) return 2; fclose(f);
    const double k = (3440.0 / 1440) / (1920.0 / 1080), cot = 1 / tan(M_PI / 6);
    float a, b, c, d;
    run(0, 60, 1, 0.5625f, k, &a, &b); check("original 16:9", a, b, cot, cot / 0.5625);
    run(1, 60, 1, 0.5625f, k, &a, &b); check("widescreen 16:9 (3440x1440)", a, b, cot / k, cot / 0.5625);
    run(1, 60, 1, 0.5625f, 1, &c, &d); check("widescreen factor 1 = original", c, d, cot, cot / 0.5625);
    run(0, 90, 1, 1, k, &a, &b); run(1, 90, 1, 1, k, &c, &d); check("shadow (aspect 1) unchanged", c, d, a, b);
    run(0, 60, 1, 0.75f, k, &a, &b); run(1, 60, 1, 0.75f, k, &c, &d); check("4:3 unchanged", c, d, a, b);
    run(0, 45, 2, 0.5625f, k, &a, &b); run(1, 45, 2, 0.5625f, k, &c, &d); check("scale 2, 16:9 widened", c, d, a / k, b);
    run(1, 60, 1, 1080.0f / 1920.0f, 32.0 / 9 / (16.0 / 9), &c, &d); check("32:9", c, d, cot / 2, cot / 0.5625);
    printf("horizontal FOV at 3440x1440: %.1f degrees (16:9: 60.0); vertical unchanged\n", 2 * atan(k * tan(M_PI / 6)) * 180 / M_PI);
    return fails ? 1 : 0;
}
