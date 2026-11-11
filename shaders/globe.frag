#version 450 core

in float vElevation;
in float vSelectionWeight;
in vec3 vObjectPosition;

uniform sampler1D uColorLut;
uniform sampler1D uSelectionColorLut;
uniform bool uSelectionActive;
uniform bool uGraticuleEnabled;
uniform float uGraticuleSpacing;
uniform vec3 uViewDirectionObject;
uniform vec3 uGraticuleColor;
uniform vec3 uHighlightedGraticuleColor;

out vec4 FragColor;

vec4 GetStop(sampler1D lut, int index) {
    return texelFetch(lut, index, 0);
}

vec3 GetHypsometricColor(float elevation, sampler1D lut) {
    int count = textureSize(lut, 0);
    float t = clamp(elevation, GetStop(lut, 0).x, GetStop(lut, count - 1).x);

    int low = 0;
    int high = count - 1;
    while (high - low > 1) {
        int middle = low + (high - low) / 2;
        if (t < GetStop(lut, middle).x) {
            high = middle;
        } else {
            low = middle;
        }
    }
    vec4 lowStop = GetStop(lut, low);
    vec4 highStop = GetStop(lut, high);
    float blend = (t - lowStop.x) / (highStop.x - lowStop.x);
    return mix(lowStop.yzw, highStop.yzw, blend);
}

float gridLine(float coordinate) {
    float distanceToLine = abs(fract(coordinate + 0.5) - 0.5);
    float antialias = max(fwidth(coordinate), 0.0001);
    return 1.0 - smoothstep(0.5 * antialias, 1.5 * antialias, distanceToLine);
}

float highlightedLine(float angle) {
    float antialias = max(fwidth(angle), 0.0001);
    return 1.0 - smoothstep(0.5 * antialias, 1.5 * antialias, abs(angle));
}

void main() {
    vec3 color = GetHypsometricColor(vElevation, uColorLut);
    if (uSelectionActive) {
        color = mix(GetHypsometricColor(vElevation, uSelectionColorLut), color,
                    clamp(vSelectionWeight, 0.0, 1.0));
    }

    vec3 normal = normalize(vObjectPosition);
    float latitude = asin(clamp(normal.z, -1.0, 1.0));
    float longitude = atan(normal.x, -normal.y);
    float graticule = 0.0;
    float equator = 0.0;
    float primeMeridian = 0.0;
    if (uGraticuleEnabled) {
        graticule = max(gridLine(latitude / uGraticuleSpacing),
                        gridLine(longitude / uGraticuleSpacing));
        equator = highlightedLine(latitude);
        primeMeridian = highlightedLine(longitude);
    }
    color = mix(color, uGraticuleColor, graticule);
    color = mix(color, uHighlightedGraticuleColor, max(equator, primeMeridian));

    float facing = max(dot(normal, normalize(uViewDirectionObject)), 0.0);
    float rim = pow(1.0 - facing, 3.0);
    color = mix(color, vec3(0.24, 0.62, 0.95), rim * 0.35);
    FragColor = vec4(color, 1.0);
}
