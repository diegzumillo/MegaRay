/* video_filter.c - configurable multipass post-processing for the frontend */
#include "frontend/video_filter.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FilterConfig {
    bool enabled;
    char shader_dir[1024];
    float persistence;
    float blur_x;
    float blur_y;
    float curvature;
    bool interference;
    float interference_strength;
    float interference_speed;
    float interference_frequency;
    float interference_line_strength;
    bool rolling_scanlines;
    float vignette;
    float ghosting;
    float mask_strength;
    float brightness;
} FilterConfig;

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static bool parse_bool(const char *value, bool fallback)
{
    if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "yes") ||
        !strcmp(value, "on")) return true;
    if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "no") ||
        !strcmp(value, "off")) return false;
    return fallback;
}

static float parse_float(const char *value, float fallback)
{
    char *end;
    float result = strtof(value, &end);
    return end != value ? result : fallback;
}

static float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static bool find_config(char *path, size_t path_size)
{
    snprintf(path, path_size, "%sfilters.cfg", GetApplicationDirectory());
    if (FileExists(path)) return true;
    if (FileExists("filters.cfg")) {
        snprintf(path, path_size, "filters.cfg");
        return true;
    }
    return false;
}

static bool is_absolute_path(const char *path)
{
    return path[0] == '/' || path[0] == '\\' ||
           (isalpha((unsigned char)path[0]) && path[1] == ':');
}

static void config_directory(const char *path, char *directory, size_t size)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) {
        snprintf(directory, size, ".");
        return;
    }
    size_t length = (size_t)(slash - path);
    if (length >= size) length = size - 1;
    memcpy(directory, path, length);
    directory[length] = '\0';
}

static void load_config(const char *path, FilterConfig *config)
{
    FILE *file = fopen(path, "r");
    if (!file) return;

    char line[1200];
    while (fgets(line, sizeof line, file)) {
        char *text = trim(line);
        if (!*text || *text == '#' || *text == ';') continue;
        char *equals = strchr(text, '=');
        if (!equals) continue;
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);

        if (!strcmp(key, "enabled"))
            config->enabled = parse_bool(value, config->enabled);
        else if (!strcmp(key, "shader_dir"))
            snprintf(config->shader_dir, sizeof config->shader_dir, "%s", value);
        else if (!strcmp(key, "persistence"))
            config->persistence = parse_float(value, config->persistence);
        else if (!strcmp(key, "blur_x"))
            config->blur_x = parse_float(value, config->blur_x);
        else if (!strcmp(key, "blur_y"))
            config->blur_y = parse_float(value, config->blur_y);
        else if (!strcmp(key, "curvature"))
            config->curvature = parse_float(value, config->curvature);
        else if (!strcmp(key, "interference"))
            config->interference = parse_bool(value, config->interference);
        else if (!strcmp(key, "interference_strength"))
            config->interference_strength = parse_float(value, config->interference_strength);
        else if (!strcmp(key, "interference_speed"))
            config->interference_speed = parse_float(value, config->interference_speed);
        else if (!strcmp(key, "interference_frequency"))
            config->interference_frequency = parse_float(value, config->interference_frequency);
        else if (!strcmp(key, "interference_line_strength"))
            config->interference_line_strength =
                parse_float(value, config->interference_line_strength);
        else if (!strcmp(key, "rolling_scanlines"))
            config->rolling_scanlines = parse_bool(value, config->rolling_scanlines);
        else if (!strcmp(key, "vignette"))
            config->vignette = parse_float(value, config->vignette);
        else if (!strcmp(key, "ghosting"))
            config->ghosting = parse_float(value, config->ghosting);
        else if (!strcmp(key, "mask_strength"))
            config->mask_strength = parse_float(value, config->mask_strength);
        else if (!strcmp(key, "brightness"))
            config->brightness = parse_float(value, config->brightness);
    }
    fclose(file);
}

static void set_float(Shader shader, int location, float value)
{
    if (location >= 0) SetShaderValue(shader, location, &value, SHADER_UNIFORM_FLOAT);
}

static Shader load_shader_file(const char *directory, const char *name)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    if (!FileExists(path)) {
        TraceLog(LOG_WARNING, "FILTER: shader not found: %s", path);
        return (Shader){ 0 };
    }
    Shader shader = LoadShader(NULL, path);
    if (!IsShaderValid(shader))
        TraceLog(LOG_WARNING, "FILTER: could not load shader: %s", path);
    return shader;
}

static void clear_target(RenderTexture2D target)
{
    BeginTextureMode(target);
    ClearBackground(BLACK);
    EndTextureMode();
}

static void unload_resources(VideoFilter *filter)
{
    if (IsShaderValid(filter->accumulate_shader)) UnloadShader(filter->accumulate_shader);
    if (IsShaderValid(filter->blur_h_shader)) UnloadShader(filter->blur_h_shader);
    if (IsShaderValid(filter->blur_v_shader)) UnloadShader(filter->blur_v_shader);
    if (IsShaderValid(filter->crt_shader)) UnloadShader(filter->crt_shader);
    for (int i = 0; i < 2; i++) {
        if (IsRenderTextureValid(filter->history[i])) UnloadRenderTexture(filter->history[i]);
        if (IsRenderTextureValid(filter->blur[i])) UnloadRenderTexture(filter->blur[i]);
    }
}

void video_filter_init(VideoFilter *filter, int width, int height)
{
    memset(filter, 0, sizeof *filter);
    FilterConfig config = {
        .enabled = true,
        .shader_dir = "filters/newpixie",
        .persistence = 0.65f,
        .blur_x = 1.0f,
        .blur_y = 1.0f,
        .curvature = 2.0f,
        .interference = false,
        .interference_strength = 0.35f,
        .interference_speed = 1.0f,
        .interference_frequency = 1.0f,
        .interference_line_strength = 0.10f,
        .rolling_scanlines = true,
        .vignette = 1.0f,
        .ghosting = 1.0f,
        .mask_strength = 0.23f,
        .brightness = 1.0f,
    };

    char config_path[1024];
    if (!find_config(config_path, sizeof config_path)) return;
    load_config(config_path, &config);
    if (!config.enabled || !config.shader_dir[0] || width <= 0 || height <= 0) return;

    char shader_directory[2048];
    if (is_absolute_path(config.shader_dir)) {
        snprintf(shader_directory, sizeof shader_directory, "%s", config.shader_dir);
    } else {
        char directory[1024];
        config_directory(config_path, directory, sizeof directory);
        snprintf(shader_directory, sizeof shader_directory, "%s/%s",
                 directory, config.shader_dir);
    }

    filter->accumulate_shader = load_shader_file(shader_directory, "accumulate.fs");
    filter->blur_h_shader = load_shader_file(shader_directory, "blur_h.fs");
    filter->blur_v_shader = load_shader_file(shader_directory, "blur_v.fs");
    filter->crt_shader = load_shader_file(shader_directory, "newpixie_crt.fs");
    if (!IsShaderValid(filter->accumulate_shader) ||
        !IsShaderValid(filter->blur_h_shader) ||
        !IsShaderValid(filter->blur_v_shader) ||
        !IsShaderValid(filter->crt_shader)) {
        unload_resources(filter);
        memset(filter, 0, sizeof *filter);
        return;
    }

    for (int i = 0; i < 2; i++) {
        filter->history[i] = LoadRenderTexture(width, height);
        filter->blur[i] = LoadRenderTexture(width, height);
    }
    if (!IsRenderTextureValid(filter->history[0]) ||
        !IsRenderTextureValid(filter->history[1]) ||
        !IsRenderTextureValid(filter->blur[0]) ||
        !IsRenderTextureValid(filter->blur[1])) {
        TraceLog(LOG_WARNING, "FILTER: could not allocate NewPixie render textures");
        unload_resources(filter);
        memset(filter, 0, sizeof *filter);
        return;
    }

    for (int i = 0; i < 2; i++) {
        SetTextureFilter(filter->history[i].texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(filter->history[i].texture, TEXTURE_WRAP_CLAMP);
        SetTextureFilter(filter->blur[i].texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(filter->blur[i].texture, TEXTURE_WRAP_CLAMP);
        clear_target(filter->history[i]);
        clear_target(filter->blur[i]);
    }

    filter->width = width;
    filter->height = height;
    filter->current_texture_loc = GetShaderLocation(filter->accumulate_shader, "current_texture");
    filter->persistence_loc = GetShaderLocation(filter->accumulate_shader, "persistence");
    filter->blur_h_texel_loc = GetShaderLocation(filter->blur_h_shader, "texel_size");
    filter->blur_h_amount_loc = GetShaderLocation(filter->blur_h_shader, "blur_amount");
    filter->blur_v_texel_loc = GetShaderLocation(filter->blur_v_shader, "texel_size");
    filter->blur_v_amount_loc = GetShaderLocation(filter->blur_v_shader, "blur_amount");
    filter->output_size_loc = GetShaderLocation(filter->crt_shader, "output_size");
    filter->frame_count_loc = GetShaderLocation(filter->crt_shader, "frame_count");
    filter->blur_texture_loc = GetShaderLocation(filter->crt_shader, "blur_texture");
    filter->curvature_loc = GetShaderLocation(filter->crt_shader, "curvature");
    filter->interference_loc = GetShaderLocation(filter->crt_shader, "interference");
    filter->interference_strength_loc =
        GetShaderLocation(filter->crt_shader, "interference_strength");
    filter->interference_speed_loc =
        GetShaderLocation(filter->crt_shader, "interference_speed");
    filter->interference_frequency_loc =
        GetShaderLocation(filter->crt_shader, "interference_frequency");
    filter->interference_line_strength_loc =
        GetShaderLocation(filter->crt_shader, "interference_line_strength");
    filter->scanline_roll_loc = GetShaderLocation(filter->crt_shader, "scanline_roll");
    filter->vignette_loc = GetShaderLocation(filter->crt_shader, "vignette");
    filter->ghosting_loc = GetShaderLocation(filter->crt_shader, "ghosting");
    filter->mask_strength_loc = GetShaderLocation(filter->crt_shader, "mask_strength");
    filter->brightness_loc = GetShaderLocation(filter->crt_shader, "brightness");

    float texel_size[2] = { 1.0f / width, 1.0f / height };
    if (filter->blur_h_texel_loc >= 0)
        SetShaderValue(filter->blur_h_shader, filter->blur_h_texel_loc,
                       texel_size, SHADER_UNIFORM_VEC2);
    if (filter->blur_v_texel_loc >= 0)
        SetShaderValue(filter->blur_v_shader, filter->blur_v_texel_loc,
                       texel_size, SHADER_UNIFORM_VEC2);

    set_float(filter->accumulate_shader, filter->persistence_loc,
              clampf(config.persistence, 0.0f, 0.99f));
    set_float(filter->blur_h_shader, filter->blur_h_amount_loc,
              clampf(config.blur_x, 0.0f, 5.0f));
    set_float(filter->blur_v_shader, filter->blur_v_amount_loc,
              clampf(config.blur_y, 0.0f, 5.0f));
    set_float(filter->crt_shader, filter->curvature_loc,
              clampf(config.curvature, 0.0001f, 4.0f));
    set_float(filter->crt_shader, filter->interference_loc,
              config.interference ? 1.0f : 0.0f);
    set_float(filter->crt_shader, filter->interference_strength_loc,
              clampf(config.interference_strength, 0.0f, 2.0f));
    set_float(filter->crt_shader, filter->interference_speed_loc,
              clampf(config.interference_speed, 0.0f, 4.0f));
    set_float(filter->crt_shader, filter->interference_frequency_loc,
              clampf(config.interference_frequency, 0.25f, 4.0f));
    set_float(filter->crt_shader, filter->interference_line_strength_loc,
              clampf(config.interference_line_strength, 0.0f, 1.0f));
    set_float(filter->crt_shader, filter->scanline_roll_loc,
              config.rolling_scanlines ? 1.0f : 0.0f);
    set_float(filter->crt_shader, filter->vignette_loc,
              clampf(config.vignette, 0.0f, 1.0f));
    set_float(filter->crt_shader, filter->ghosting_loc,
              clampf(config.ghosting, 0.0f, 2.0f));
    set_float(filter->crt_shader, filter->mask_strength_loc,
              clampf(config.mask_strength, 0.0f, 1.0f));
    set_float(filter->crt_shader, filter->brightness_loc,
              clampf(config.brightness, 0.25f, 2.0f));
    filter->enabled = true;
}

static void draw_render_texture(RenderTexture2D target)
{
    DrawTexturePro(target.texture,
                   (Rectangle){ 0, 0, (float)target.texture.width,
                                (float)-target.texture.height },
                   (Rectangle){ 0, 0, (float)target.texture.width,
                                (float)target.texture.height },
                   (Vector2){ 0, 0 }, 0.0f, WHITE);
}

void video_filter_draw(VideoFilter *filter, Texture2D texture,
                       Rectangle source, Rectangle destination)
{
    if (!filter->enabled) {
        DrawTexturePro(texture, source, destination, (Vector2){ 0, 0 }, 0.0f, WHITE);
        return;
    }

    int previous = filter->history_index;
    int current = previous ^ 1;

    /* The source image and OpenGL render textures have opposite vertical
     * orientations. The accumulation shader samples each with the appropriate
     * coordinates while writing into the other history texture. */
    BeginTextureMode(filter->history[current]);
    BeginShaderMode(filter->accumulate_shader);
    SetShaderValueTexture(filter->accumulate_shader,
                          filter->current_texture_loc, texture);
    draw_render_texture(filter->history[previous]);
    EndShaderMode();
    EndTextureMode();
    filter->history_index = current;

    BeginTextureMode(filter->blur[0]);
    BeginShaderMode(filter->blur_h_shader);
    draw_render_texture(filter->history[current]);
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(filter->blur[1]);
    BeginShaderMode(filter->blur_v_shader);
    draw_render_texture(filter->blur[0]);
    EndShaderMode();
    EndTextureMode();

    float output_size[2] = { destination.width, destination.height };
    if (filter->output_size_loc >= 0)
        SetShaderValue(filter->crt_shader, filter->output_size_loc,
                       output_size, SHADER_UNIFORM_VEC2);
    if (filter->frame_count_loc >= 0)
        SetShaderValue(filter->crt_shader, filter->frame_count_loc,
                       &filter->frame_count, SHADER_UNIFORM_INT);
    BeginShaderMode(filter->crt_shader);
    SetShaderValueTexture(filter->crt_shader, filter->blur_texture_loc,
                          filter->blur[1].texture);
    DrawTexturePro(filter->history[current].texture,
                   (Rectangle){ 0, 0, (float)filter->width, (float)-filter->height },
                   destination, (Vector2){ 0, 0 }, 0.0f, WHITE);
    EndShaderMode();
    if (++filter->frame_count >= 1000000) filter->frame_count = 0;
}

void video_filter_unload(VideoFilter *filter)
{
    unload_resources(filter);
    memset(filter, 0, sizeof *filter);
}
