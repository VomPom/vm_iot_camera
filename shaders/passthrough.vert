// passthrough vertex shader（兼容 GStreamer 自带 glshader：
// 不写 #version，由 glshader 根据当前 GL/GLES context 注入合适的 header
// 与 fragment 默认 attribute 名 a_position / a_texcoord 配套）
attribute vec4 a_position;
attribute vec2 a_texcoord;
varying   vec2 v_texcoord;

void main() {
    gl_Position = a_position;
    v_texcoord  = a_texcoord;
}