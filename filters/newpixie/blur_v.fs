#version 330

// NewPixie CRT by Mattias Gustavsson. Adapted from the libretro Slang port by
// hunterk. Distributed under the MIT alternative; see LICENSE.

in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 texel_size;
uniform float blur_amount;
out vec4 finalColor;

void main()
{
    vec2 blur = vec2(0.0, blur_amount * texel_size.y);
    vec4 sum = texture(texture0, fragTexCoord) * 0.2270270270;
    sum += texture(texture0, fragTexCoord - 4.0 * blur) * 0.0162162162;
    sum += texture(texture0, fragTexCoord - 3.0 * blur) * 0.0540540541;
    sum += texture(texture0, fragTexCoord - 2.0 * blur) * 0.1216216216;
    sum += texture(texture0, fragTexCoord - 1.0 * blur) * 0.1945945946;
    sum += texture(texture0, fragTexCoord + 1.0 * blur) * 0.1945945946;
    sum += texture(texture0, fragTexCoord + 2.0 * blur) * 0.1216216216;
    sum += texture(texture0, fragTexCoord + 3.0 * blur) * 0.0540540541;
    sum += texture(texture0, fragTexCoord + 4.0 * blur) * 0.0162162162;
    finalColor = sum;
}
