/* main.c - raylib frontend: window, video, audio stream, input, CLI */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "core/md.h"
#include "core/core_internal.h"
#include "m68k.h"

#define OUT_RATE   48000
#define ABUF_FRAMES 800   /* 48000 / 60 */

/* Locate the ROM independently of the working directory (double-click safe):
 * next to the exe first (shipping layout), then the dev-tree locations. */
static const char *find_rom(void)
{
    static char buf[1024];
    const char *dir = GetApplicationDirectory();
    const char *rel[] = { "rom.bin", "roms/rom.bin", "../roms/rom.bin" };
    for (unsigned i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        snprintf(buf, sizeof buf, "%s%s", dir, rel[i]);
        if (FileExists(buf)) return buf;
    }
    if (FileExists("roms/rom.bin")) return "roms/rom.bin";
    if (FileExists("rom.bin")) return "rom.bin";
    return NULL;
}

static int error_window(const char *msg)
{
    InitWindow(720, 200, "Mega Drive - error");
    SetTargetFPS(30);
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground((Color){ 40, 8, 8, 255 });
        DrawText("Could not start:", 20, 40, 20, RAYWHITE);
        DrawText(msg, 20, 80, 20, (Color){ 255, 180, 120, 255 });
        DrawText("(press Esc to close)", 20, 140, 16, GRAY);
        EndDrawing();
    }
    CloseWindow();
    return 1;
}

static void usage(void)
{
    printf("usage: mdplayer [rom.bin] [options]\n"
           "  --frames N          run N frames headless, then exit\n"
           "  --screenshot PATH   with --frames: save final frame as PNG\n"
           "  --break-at HEX      print when 68k PC first reaches HEX (repeatable)\n"
           "  --scale N           window scale factor (default 3)\n");
}

static uint16_t poll_input(void)
{
    uint16_t b = 0;
    if (IsKeyDown(KEY_UP))    b |= MD_BTN_UP;
    if (IsKeyDown(KEY_DOWN))  b |= MD_BTN_DOWN;
    if (IsKeyDown(KEY_LEFT))  b |= MD_BTN_LEFT;
    if (IsKeyDown(KEY_RIGHT)) b |= MD_BTN_RIGHT;
    if (IsKeyDown(KEY_Z))     b |= MD_BTN_A;
    if (IsKeyDown(KEY_X))     b |= MD_BTN_B;
    if (IsKeyDown(KEY_C))     b |= MD_BTN_C;
    if (IsKeyDown(KEY_ENTER)) b |= MD_BTN_START;

    if (IsGamepadAvailable(0)) {
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP))     b |= MD_BTN_UP;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN))   b |= MD_BTN_DOWN;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))   b |= MD_BTN_LEFT;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))  b |= MD_BTN_RIGHT;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))  b |= MD_BTN_A;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))  b |= MD_BTN_B;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) b |= MD_BTN_C;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_RIGHT))     b |= MD_BTN_START;
        float ax = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        float ay = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (ax < -0.4f) b |= MD_BTN_LEFT;
        if (ax >  0.4f) b |= MD_BTN_RIGHT;
        if (ay < -0.4f) b |= MD_BTN_UP;
        if (ay >  0.4f) b |= MD_BTN_DOWN;
    }
    return b;
}

static void dump_cpu(void)
{
    printf("[cpu] D0=%08X D1=%08X D2=%08X D3=%08X\n",
           m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
           m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D3));
    printf("[cpu] A0=%08X A1=%08X A6=%08X SP=%08X SR=%04X\n",
           m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
           m68k_get_reg(NULL, M68K_REG_A6), m68k_get_reg(NULL, M68K_REG_SP),
           m68k_get_reg(NULL, M68K_REG_SR));
    uint32_t pc = md_get_pc();
    for (int i = 0; i < 8; i++) {
        char buf[128];
        int len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("[dasm] %06X: %s\n", pc, buf);
        pc += len;
    }
}

static void fb_stats(void)
{
    const uint32_t *fb = md_framebuffer();
    uint32_t first = fb[0];
    int diff = 0;
    for (int i = 0; i < MD_SCREEN_W * MD_SCREEN_H; i++)
        if (fb[i] != first) diff++;
    printf("[headless] PC=%06X instructions=%llu non-uniform pixels=%d/%d\n",
           md_get_pc(), (unsigned long long)md_instr_count(),
           diff, MD_SCREEN_W * MD_SCREEN_H);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL;
    const char *shot_path = NULL;
    const char *wav_path = NULL;
    int frames = 0, scale = 3, auto_start = 0, ym_test = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (!strcmp(argv[i], "--auto-start")) {
            auto_start = 1;
        } else if (!strcmp(argv[i], "--ym-test")) {
            ym_test = 1;
        } else if (!strcmp(argv[i], "--break-at") && i + 1 < argc) {
            md_add_break_pc((uint32_t)strtoul(argv[++i], NULL, 16), argv[i]);
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            rom_path = argv[i];
        }
    }

    if (ym_test) {
        md_reset();
        printf("[ym-test] peak=%d (expect ~5400: one channel at 9-bit DAC max, x7 mix gain)\n",
               z80bus_ym_selftest());
        return 0;
    }

    SetTraceLogLevel(LOG_WARNING);
    if (!rom_path)
        rom_path = find_rom();
    if (!rom_path || !md_load_rom(rom_path)) {
        char msg[1200];
        snprintf(msg, sizeof msg, "ROM not found or unreadable: %s",
                 rom_path ? rom_path : "no rom.bin next to the exe or in roms/");
        fprintf(stderr, "%s\n", msg);
        if (frames > 0) return 1;
        return error_window(msg);
    }
    md_reset();

    if (frames > 0) {   /* headless verification mode */
        int16_t *wav = NULL;
        int wav_frames = 0;
        if (wav_path)
            wav = malloc((size_t)frames * 810 * 2 * sizeof(int16_t));
        for (int f = 0; f < frames; f++) {
            if (auto_start)   /* tap Start for 10 frames out of every 60 */
                md_set_pad(0, (f % 60) < 10 ? MD_BTN_START : 0);
            md_run_frame();
            if (wav)
                wav_frames += md_read_audio(wav + wav_frames * 2, 810, OUT_RATE);
            if (f % 60 == 59) {
                char zb[128];
                z80bus_debug(zb, sizeof zb);
                printf("[headless] frame %d PC=%06X | z80 %s\n", f + 1, md_get_pc(), zb);
            }
        }
        if (wav) {
            Wave w = { .frameCount = (unsigned int)wav_frames, .sampleRate = OUT_RATE,
                       .sampleSize = 16, .channels = 2, .data = wav };
            ExportWave(w, wav_path);
            printf("[headless] wav: %s (%d frames)\n", wav_path, wav_frames);
            free(wav);
        }
        md_flush_sram();
        fb_stats();
        dump_cpu();
        if (shot_path) {
            Image img = {
                .data = (void *)md_framebuffer(),
                .width = MD_SCREEN_W, .height = MD_SCREEN_H,
                .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
            };
            ExportImage(img, shot_path);
            printf("[headless] screenshot: %s\n", shot_path);
        }
        return 0;
    }

    SetTraceLogLevel(LOG_WARNING);
    InitWindow(MD_SCREEN_W * scale, MD_SCREEN_H * scale, "Mega Drive");
    SetTargetFPS(60);

    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(ABUF_FRAMES);
    AudioStream stream = LoadAudioStream(OUT_RATE, 16, 2);
    PlayAudioStream(stream);
    static int16_t abuf[ABUF_FRAMES * 2];

    Image blank = GenImageColor(MD_SCREEN_W, MD_SCREEN_H, BLACK);
    ImageFormat(&blank, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Texture2D tex = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(tex, TEXTURE_FILTER_POINT);

    int frame_no = 0;
    while (!WindowShouldClose()) {
        md_set_pad(0, poll_input());
        md_run_frame();
        if (++frame_no % 60 == 0)
            md_flush_sram();

        if (IsAudioStreamProcessed(stream)) {
            int n = md_read_audio(abuf, ABUF_FRAMES, OUT_RATE);
            for (int i = n; i < ABUF_FRAMES; i++) {   /* pad underrun */
                abuf[i * 2] = n ? abuf[(n - 1) * 2] : 0;
                abuf[i * 2 + 1] = n ? abuf[(n - 1) * 2 + 1] : 0;
            }
            UpdateAudioStream(stream, abuf, ABUF_FRAMES);
        }

        UpdateTexture(tex, md_framebuffer());

        int sw = GetScreenWidth(), sh = GetScreenHeight();
        int s = sw / MD_SCREEN_W < sh / MD_SCREEN_H ? sw / MD_SCREEN_W : sh / MD_SCREEN_H;
        if (s < 1) s = 1;
        int dw = MD_SCREEN_W * s, dh = MD_SCREEN_H * s;

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(tex,
                       (Rectangle){ 0, 0, MD_SCREEN_W, MD_SCREEN_H },
                       (Rectangle){ (float)((sw - dw) / 2), (float)((sh - dh) / 2),
                                    (float)dw, (float)dh },
                       (Vector2){ 0, 0 }, 0.0f, WHITE);
        if (IsKeyDown(KEY_F3)) DrawFPS(8, 8);
        EndDrawing();
    }

    md_flush_sram();
    UnloadTexture(tex);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
