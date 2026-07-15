#version 330

// NewPixie CRT by Mattias Gustavsson. Adapted for this raylib pipeline from
// the libretro Slang port by hunterk. Distributed under the MIT alternative;
// see LICENSE in this directory.

in vec2 fragTexCoord;
uniform sampler2D texture0;       // Previous accumulation render texture.
uniform sampler2D current_texture; // Current emulator framebuffer.
uniform float persistence;
out vec4 finalColor;

void main()
{
    vec3 previous = texture(texture0, fragTexCoord).rgb * persistence;

    // Render textures have their logical top at v=1; ordinary raylib textures
    // have it at v=0. The quad is oriented for the former.
    vec2 current_uv = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);
    vec3 current = texture(current_texture, current_uv).rgb * 0.96;
    finalColor = vec4(max(previous, current), 1.0);
}
