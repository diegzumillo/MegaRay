#ifndef VIDEO_FILTER_H
#define VIDEO_FILTER_H

#include <stdbool.h>
#include "raylib.h"

typedef struct VideoFilter {
    bool enabled;
    int width;
    int height;
    int history_index;
    int frame_count;

    Shader accumulate_shader;
    Shader blur_h_shader;
    Shader blur_v_shader;
    Shader crt_shader;
    RenderTexture2D history[2];
    RenderTexture2D blur[2];

    int current_texture_loc;
    int persistence_loc;
    int blur_h_texel_loc;
    int blur_h_amount_loc;
    int blur_v_texel_loc;
    int blur_v_amount_loc;
    int output_size_loc;
    int frame_count_loc;
    int blur_texture_loc;
    int curvature_loc;
    int interference_loc;
    int interference_strength_loc;
    int interference_speed_loc;
    int interference_frequency_loc;
    int interference_line_strength_loc;
    int scanline_roll_loc;
    int vignette_loc;
    int ghosting_loc;
    int mask_strength_loc;
    int brightness_loc;
} VideoFilter;

/* Loads filters.cfg and the NewPixie shader pipeline from beside the
 * executable. Missing or invalid resources leave the unfiltered renderer
 * active. Intermediate buffers remain at the emulated source resolution. */
void video_filter_init(VideoFilter *filter, int width, int height);
void video_filter_draw(VideoFilter *filter, Texture2D texture,
                       Rectangle source, Rectangle destination);
void video_filter_unload(VideoFilter *filter);

#endif
