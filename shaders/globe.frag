#version 450 core

in float vElevation;
in float vSelectionWeight;

layout(std430, binding = 1) readonly buffer ColorStops {
    vec4 stops[];
};

uniform int uStopCount;

layout(std430, binding = 2) readonly buffer SelectionColorStops {
    vec4 selectionStops[];
};

uniform int uSelectionStopCount;
uniform bool uSelectionActive;

out vec4 FragColor;

vec4 GetStop(int index, bool grayscale) {
    return grayscale ? selectionStops[index] : stops[index];
}

vec3 GetHypsometricColor(float elevation, bool grayscale) {
    int count = grayscale ? uSelectionStopCount : uStopCount;
    float t = clamp(elevation, GetStop(0, grayscale).x, GetStop(count - 1, grayscale).x);

    int low = 0;
    int high = count - 1;
    while (high - low > 1) {
        int middle = low + (high - low) / 2;
        if (t < GetStop(middle, grayscale).x) {
            high = middle;
        } else {
            low = middle;
        }
    }
    vec4 lowStop = GetStop(low, grayscale);
    vec4 highStop = GetStop(high, grayscale);
    float blend = (t - lowStop.x) / (highStop.x - lowStop.x);
    return mix(lowStop.yzw, highStop.yzw, blend);
}

void main() {
    // Full ambient illumination preserves map colours across the entire globe.
    vec3 color = GetHypsometricColor(vElevation, false);
    if (uSelectionActive) {
        color = mix(GetHypsometricColor(vElevation, true), color,
                    clamp(vSelectionWeight, 0.0, 1.0));
    }
    FragColor = vec4(color, 1.0);
}
