#ifdef GL_ES
precision mediump float;
#endif

// gstglshader 标准输入：
//   v_texcoord：纹理坐标
//   tex       ：上游 GL 纹理（RGBA）
varying vec2 v_texcoord;
uniform sampler2D tex;

// 由宿主程序通过 GstGLShader 的 "uniforms" 属性下发：
//   0 = passthrough（不处理）
//   1 = mosaic     （马赛克）
//   2 = invert     （反相）
uniform int filter_type;

// 输入分辨率（用于像素级特效，如马赛克）。
// 默认 1280x720，可按需通过 uniforms 覆盖。
uniform vec2 resolution;

// 马赛克块边长（像素）。也可后续通过 uniforms 在线调节。
const float MOSAIC_BLOCK = 16.0;

void main() {
    vec2 uv = v_texcoord;

    if (filter_type == 1) {
        // ---- 马赛克 ----
        vec2 res = resolution;
        // 防止未设置 resolution 时除 0
        if (res.x < 1.0 || res.y < 1.0) res = vec2(1280.0, 720.0);
        vec2 px = uv * res;
        px = (floor(px / MOSAIC_BLOCK) + 0.5) * MOSAIC_BLOCK;
        uv = px / res;
        gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0);
    } else if (filter_type == 2) {
        // ---- 反相 ----
        vec3 c = texture2D(tex, uv).rgb;
        gl_FragColor = vec4(vec3(1.0) - c, 1.0);
    } else {
        // ---- passthrough ----
        gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0);
    }
}
