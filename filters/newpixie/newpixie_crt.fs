#version 330

// NewPixie CRT by Mattias Gustavsson. Adapted for GLSL 330/raylib from the
// libretro Slang port by hunterk. Distributed under the MIT alternative; see
// LICENSE in this directory.

in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;    // Accumulation buffer.
uniform sampler2D blur_texture;
uniform vec2 output_size;
uniform int frame_count;
uniform float curvature;
uniform float interference;
uniform float interference_strength;
uniform float interference_speed;
uniform float interference_frequency;
uniform float interference_line_strength;
uniform float scanline_roll;
uniform float vignette;
uniform float ghosting;
uniform float mask_strength;
uniform float brightness;
out vec4 finalColor;

vec3 tsample(sampler2D image, vec2 uv)
{
    uv = uv * vec2(1.025, 0.92) + vec2(-0.0125, 0.04);
    return pow(abs(texture(image, uv).rgb), vec3(2.2)) * 1.25;
}

vec3 filmic(vec3 linear_color)
{
    vec3 x = max(vec3(0.0), linear_color - 0.004);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

vec2 curve(vec2 uv)
{
    uv -= 0.5;
    uv *= vec2(0.925, 1.095);
    uv *= curvature;
    uv.x *= 1.0 + pow(abs(uv.y) / 4.0, 2.0);
    uv.y *= 1.0 + pow(abs(uv.x) / 3.0, 2.0);
    uv /= curvature;
    uv += 0.5;
    return uv * 0.92 + 0.04;
}

float random_value(vec2 seed)
{
    return fract(sin(dot(seed, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
    vec2 resolution = max(output_size, vec2(1.0));
    float time = float(frame_count % 849) * 36.0;
    vec2 curved_uv = mix(curve(fragTexCoord), fragTexCoord, 0.4);
    float scale = -0.101;
    vec2 screen_uv = curved_uv * (1.0 - scale) + scale / 2.0 +
                     vec2(0.003, -0.001);

    float interference_time = time * interference_speed;
    float interference_y = curved_uv.y * interference_frequency;
    float x = sin(0.1 * interference_time + interference_y * 13.0) *
              sin(0.23 * interference_time + interference_y * 19.0) *
              sin(0.3 + 0.11 * interference_time + interference_y * 23.0) *
              0.0012 * interference_strength;
    x += sin(gl_FragCoord.y * 1.5) / resolution.x * interference_line_strength;
    x *= interference;
    time = float(frame_count % 640);

    vec3 color;
    color.r = tsample(texture0, vec2(x + screen_uv.x + 0.0009,
                                     screen_uv.y + 0.0009)).r + 0.02;
    color.g = tsample(texture0, vec2(x + screen_uv.x,
                                     screen_uv.y - 0.0011)).g + 0.02;
    color.b = tsample(texture0, vec2(x + screen_uv.x - 0.0015,
                                     screen_uv.y)).b + 0.02;
    float intensity = clamp(dot(color, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
    intensity = intensity * intensity * 0.85 + 0.15;

    float ghost_strength = 0.15 * ghosting;
    vec3 red_ghost = tsample(blur_texture,
        vec2(x - 0.014, -0.027) * 0.85 +
        0.007 * vec2(0.35 * sin(1.0 / 7.0 + 15.0 * curved_uv.y + 0.9 * time),
                     0.35 * sin(2.0 / 7.0 + 10.0 * curved_uv.y + 1.37 * time)) +
        vec2(screen_uv.x + 0.001, screen_uv.y + 0.001)) * vec3(0.5, 0.25, 0.25);
    vec3 green_ghost = tsample(blur_texture,
        vec2(x - 0.019, -0.020) * 0.85 +
        0.007 * vec2(0.35 * cos(1.0 / 9.0 + 15.0 * curved_uv.y + 0.5 * time),
                     0.35 * sin(2.0 / 9.0 + 10.0 * curved_uv.y + 1.50 * time)) +
        vec2(screen_uv.x, screen_uv.y - 0.002)) * vec3(0.25, 0.5, 0.25);
    vec3 blue_ghost = tsample(blur_texture,
        vec2(x - 0.017, -0.003) * 0.85 +
        0.007 * vec2(0.35 * sin(2.0 / 3.0 + 15.0 * curved_uv.y + 0.7 * time),
                     0.35 * cos(2.0 / 3.0 + 10.0 * curved_uv.y + 1.63 * time)) +
        vec2(screen_uv.x - 0.002, screen_uv.y)) * vec3(0.25, 0.25, 0.5);

    color += ghost_strength * (1.0 - 0.299) *
             pow(clamp(3.0 * red_ghost, 0.0, 1.0), vec3(2.0)) * intensity;
    color += ghost_strength * (1.0 - 0.587) *
             pow(clamp(3.0 * green_ghost, 0.0, 1.0), vec3(2.0)) * intensity;
    color += ghost_strength * (1.0 - 0.114) *
             pow(clamp(3.0 * blue_ghost, 0.0, 1.0), vec3(2.0)) * intensity;

    color *= vec3(0.95, 1.05, 0.95) * brightness;
    color = clamp(color * 1.3 + 0.75 * color * color +
                  1.25 * color * color * color * color * color, 0.0, 10.0);

    float vig = (1.0 - 0.99 * vignette) +
                16.0 * curved_uv.x * curved_uv.y *
                (1.0 - curved_uv.x) * (1.0 - curved_uv.y);
    color *= 1.3 * pow(max(vig, 0.0), 0.5);

    time *= scanline_roll;
    float scans = clamp(0.35 + 0.18 *
                        sin(6.0 * time - curved_uv.y * resolution.y * 1.5),
                        0.0, 1.0);
    color *= pow(scans, 0.9);

    float mask = clamp(mod(gl_FragCoord.x, 3.0) / 2.0, 0.0, 1.0);
    color *= 1.0 - mask_strength * mask;
    color = filmic(color);

    vec2 seed = curved_uv * resolution;
    color -= 0.015 * pow(vec3(random_value(seed + time),
                              random_value(seed + time * 2.0),
                              random_value(seed + time * 3.0)), vec3(1.5));
    color *= 1.0 - 0.004 *
             (sin(50.0 * time + curved_uv.y * 2.0) * 0.5 + 0.5);

    if (screen_uv.x < 0.0 || screen_uv.x > 1.0 ||
        screen_uv.y < 0.0 || screen_uv.y > 1.0)
        color = vec3(0.0);

    finalColor = vec4(max(color, vec3(0.0)), 1.0) * fragColor;
}
