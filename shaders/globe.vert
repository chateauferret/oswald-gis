#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aElevation;

uniform mat4 uModelViewProjection;

out float vElevation;

void main() {
    vElevation = aElevation;
    gl_Position = uModelViewProjection * vec4(aPos, 1.0);
}
