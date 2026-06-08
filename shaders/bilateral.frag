// 双边滤波 fragment shader
// 兼容写法：不写 #version / precision，由 glshader 根据当前 GL/GLES context 自动注入；
// sigma 与 texel 全部固化为常量或在 shader 内自取，避免依赖外部 uniform。
#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex;

// 与之前 yaml 默认值保持一致：sigma_space=4.0, sigma_color=0.10
const float SIGMA_S = 4.0;
const float SIGMA_C = 0.10;
const int   R       = 3;   // 7x7 窗口

void main() {
    // texel = 1 / 纹理尺寸（兼容写法：用 dFdx/dFdy 反推不可靠，
    // 故直接采样邻域时用一个固定 1/1280, 1/720 的常量；尺寸由 caps 锁定）
    vec2 texel = vec2(1.0 / 1280.0, 1.0 / 720.0);

    vec3 center = texture2D(tex, v_texcoord).rgb;
    vec3 sum    = vec3(0.0);
    float wsum  = 0.0;

    float inv_2ss = 1.0 / (2.0 * SIGMA_S * SIGMA_S);
    float inv_2sc = 1.0 / (2.0 * SIGMA_C * SIGMA_C);

    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            vec2  off = vec2(float(dx), float(dy)) * texel;
            vec3  s   = texture2D(tex, v_texcoord + off).rgb;
            float ws  = exp(-float(dx*dx + dy*dy) * inv_2ss);
            vec3  d   = s - center;
            float wc  = exp(-dot(d, d) * inv_2sc);
            float w   = ws * wc;
            sum  += s * w;
            wsum += w;
        }
    }
    gl_FragColor = vec4(sum / wsum, 1.0);
}