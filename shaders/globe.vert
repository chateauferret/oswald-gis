#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aElevation;
layout(location = 2) in float aSelectionWeight;

uniform mat4 uModelViewProjection;

out float vElevation;
out float vSelectionWeight;

void main() {
    vElevation = aElevation;
    vSelectionWeight = aSelectionWeight;
    gl_Position = uModelViewProjection * vec4(aPos, 1.0);
}
