#version 450 core

layout(lines) in;
layout(line_strip, max_vertices = 6) out;

in vec3 vPosition[];
in float vSelectionWeight[];
out float gSelectionWeight;

uniform mat4 uModelViewProjection;
uniform float uDepthBias;
uniform bool uEquirectangular;

const float PI = 3.14159265358979323846;
const float TWO_PI = 6.28318530717958647692;

void emitPoint(int index, vec3 position) {
    gSelectionWeight = vSelectionWeight[index];
    vec4 clip = uModelViewProjection * vec4(position, 1.0);
    clip.z -= uDepthBias * clip.w;
    gl_Position = clip;
    EmitVertex();
}

void main() {
    vec3 first = vPosition[0];
    vec3 second = vPosition[1];
    if (!uEquirectangular) {
        emitPoint(0, first);
        emitPoint(1, second);
        EndPrimitive();
        return;
    }

    float longitude0 = atan(first.x, -first.y);
    float longitude1 = atan(second.x, -second.y);
    float difference = longitude1 - longitude0;
    if (difference > PI) longitude1 -= TWO_PI;
    if (difference < -PI) longitude1 += TWO_PI;
    vec3 map0 = vec3(longitude0, asin(clamp(first.z, -1.0, 1.0)), 0.0);
    vec3 map1 = vec3(longitude1, asin(clamp(second.z, -1.0, 1.0)), 0.0);
    for (int copy = -1; copy <= 1; ++copy) {
        float offset = float(copy) * TWO_PI;
        emitPoint(0, map0 + vec3(offset, 0.0, 0.0));
        emitPoint(1, map1 + vec3(offset, 0.0, 0.0));
        EndPrimitive();
    }
}
