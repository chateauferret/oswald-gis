#version 450 core

layout(triangles) in;
layout(triangle_strip, max_vertices = 9) out;

in float vElevation[];
in float vSelectionWeight[];
in vec3 vObjectPosition[];

out float gElevation;
out float gSelectionWeight;
out vec3 gObjectPosition;

uniform mat4 uModelViewProjection;
uniform bool uEquirectangular;

const float PI = 3.14159265358979323846;
const float TWO_PI = 6.28318530717958647692;

void emitVertex(int index, float longitude, float offset) {
    gElevation = vElevation[index];
    gSelectionWeight = vSelectionWeight[index];
    gObjectPosition = vObjectPosition[index];
    vec3 position = vObjectPosition[index];
    if (uEquirectangular) {
        float latitude = asin(clamp(position.z, -1.0, 1.0));
        position = vec3(longitude + offset, latitude, vElevation[index] * (0.025 / 32767.0));
    }
    gl_Position = uModelViewProjection * vec4(position, 1.0);
    EmitVertex();
}

void main() {
    float longitude[3];
    for (int i = 0; i < 3; ++i) {
        longitude[i] = atan(vObjectPosition[i].x, -vObjectPosition[i].y);
    }
    if (uEquirectangular) {
        for (int i = 1; i < 3; ++i) {
            float difference = longitude[i] - longitude[0];
            if (difference > PI) longitude[i] -= TWO_PI;
            if (difference < -PI) longitude[i] += TWO_PI;
        }
        for (int copy = -1; copy <= 1; ++copy) {
            float offset = float(copy) * TWO_PI;
            emitVertex(0, longitude[0], offset);
            emitVertex(1, longitude[1], offset);
            emitVertex(2, longitude[2], offset);
            EndPrimitive();
        }
    } else {
        emitVertex(0, longitude[0], 0.0);
        emitVertex(1, longitude[1], 0.0);
        emitVertex(2, longitude[2], 0.0);
        EndPrimitive();
    }
}
