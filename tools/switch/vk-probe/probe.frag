#version 450
layout(location = 0) out vec4 color;
layout(push_constant) uniform Push { vec4 value; } push;
void main() { color = push.value; }
