#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex;

const vec2  RES   = vec2(1280.0, 720.0);

// 马赛克块边长（像素）。越大越糊；想更细可改 8.0，想更糊可改 32.0。
const float BLOCK = 8.0;

void main() {
    // 1) uv → 像素坐标
    vec2 px = v_texcoord * RES;
    // 2) 量化到 BLOCK 网格中心
    px = (floor(px / BLOCK) + 0.5) * BLOCK;
    // 3) 回到 uv 空间采样
    vec2 uv = px / RES;
    gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0);
}