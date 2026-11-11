#version 450 core

in float vSelectionWeight;

uniform bool uSelectionActive;
uniform vec3 uUnselectedColor;
uniform vec3 uSelectedColor;

out vec4 FragColor;

void main() {
    vec3 color = uUnselectedColor;
    if (uSelectionActive) {
        // Same blend direction as the elevation colour maps in globe.frag.
        color = mix(uSelectedColor, uUnselectedColor, clamp(vSelectionWeight, 0.0, 1.0));
    }
    FragColor = vec4(color, 1.0);
}
