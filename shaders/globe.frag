#version 450 core

in float vElevation;

layout(std430, binding = 1) readonly buffer ColorStops {
    vec4 stops[];
};

uniform int uStopCount;

out vec4 FragColor;

vec3 GetHypsometricColor(float elevation) {
    float t = clamp(elevation, stops[0].x, stops[uStopCount - 1].x);

    int low = 0;
    int high = uStopCount - 1;
    while (high - low > 1) {
        int middle = low + (high - low) / 2;
        if (t < stops[middle].x) {
            high = middle;
        } else {
            low = middle;
        }
    }
    float blend = (t - stops[low].x) / (stops[high].x - stops[low].x);
    return mix(stops[low].yzw, stops[high].yzw, blend);
}

void main() {
    // Full ambient illumination preserves map colours across the entire globe.
    FragColor = vec4(GetHypsometricColor(vElevation), 1.0);
}
