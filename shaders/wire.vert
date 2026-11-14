#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aSelectionWeight;

out vec3 vPosition;
out float vSelectionWeight;

void main() {
    vSelectionWeight = aSelectionWeight;
    vPosition = aPos;
}
