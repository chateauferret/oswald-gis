#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aSelectionWeight;

uniform mat4 uModelViewProjection;
uniform float uDepthBias;

out float vSelectionWeight;

void main() {
    vSelectionWeight = aSelectionWeight;
    vec4 position = uModelViewProjection * vec4(aPos, 1.0);
    // Pull the lines slightly towards the viewer so they are not buried in the facets.
    position.z -= uDepthBias * position.w;
    gl_Position = position;
}
