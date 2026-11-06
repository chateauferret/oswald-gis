#version 450 core

in float vElevation;
in vec3 vNormal;

layout(std430, binding = 1) readonly buffer ColorStops {
    vec4 stops[];
};

uniform int uOceanStopCount;
uniform int uLandStopCount;
uniform vec3 uLightDir;

out vec4 FragColor;

vec3 GetHypsometricColor(float elevation) {
    bool ocean = elevation < 0.0;
    float t = ocean ? clamp((elevation + 32767.0) / 32767.0, 0.0, 1.0)
                    : clamp(elevation / 32767.0, 0.0, 1.0);
    int first = ocean ? 0 : uOceanStopCount;
    int count = ocean ? uOceanStopCount : uLandStopCount;

    int low = first;
    int high = first + count - 1;
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
    float diffuse = max(dot(normalize(vNormal), normalize(uLightDir)), 0.15);
    FragColor = vec4(GetHypsometricColor(vElevation) * diffuse, 1.0);
}
