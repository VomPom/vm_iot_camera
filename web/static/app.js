// app.js — vm_iot console front-end
// ---------------------------------------------------------------------------
// 单文件、零依赖、纯 ESM。
// 四大模块：
//   1. media       — WebRTC (mediamtx WHEP) / HLS / RTSP-url 三档媒体源切换
//   2. faceOverlay — 人脸绘框：基于 /ws/events 广播的 kind:'faces' 消息，
//                    在 <video> 上叠一层 canvas 实时绘矩形。
//   3. control     — 命令分发：把按钮 / 输入框翻译成 fetch /api/...
//   4. status      — WebSocket /ws/events 拉 1Hz status，刷状态面板 + 顶栏
// ---------------------------------------------------------------------------

const $  = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

const log = {
  el: $('#log'),
  push(kind, text) {
    const t = new Date().toTimeString().slice(0, 8);
    const span = document.createElement('span');
    span.className = kind;
    span.textContent = text;
    this.el.append(`[${t}] `, span, '\n');
    this.el.scrollTop = this.el.scrollHeight;
  },
  send(line) { this.push('send', `→ ${line}`); },
  ok(text)   { this.push('ok',   text); },
  err(text)  { this.push('err',  text); },
  info(text) { this.push('',     text); },
};

/* ─────────────────────────── REST 工具 ─────────────────────────── */
async function api(method, path, body) {
  const opt = { method, headers: { 'Content-Type': 'application/json' } };
  if (body !== undefined) opt.body = JSON.stringify(body);
  const r = await fetch(path, opt);
  let json;
  try { json = await r.json(); } catch { json = { ok: false, error: 'bad_json' }; }
  return { http: r.status, ...json };
}

async function call(label, method, path, body) {
  log.send(`${label}`);
  try {
    const r = await api(method, path, body);
    if (r.ok) {
      const sum = formatBody(r.body) || r.cmd || 'ok';
      log.ok(`✓ ${label} ${sum}`);
      return r;
    } else {
      log.err(`✗ ${label} ${r.error || r.http} ${r.cmd || ''}`);
      return r;
    }
  } catch (e) {
    log.err(`✗ ${label} ${e.message}`);
    return { ok: false, error: e.message };
  }
}

function formatBody(body) {
  if (!body) return '';
  const keys = Object.keys(body);
  if (keys.length === 0) return '';
  return keys.slice(0, 3).map(k => `${k}=${body[k]}`).join(' ');
}

/* ─────────────────────────── 媒体接入 ─────────────────────────── *
 * 自动重连：所有触发源 → scheduleReconnect() 单一入口 → 指数退避（1s~10s）。
 * 详见 docs/reference/hotplug_recovery.md。 */

const RECONNECT_BASE_MS = 1000;
const RECONNECT_MAX_MS  = 10000;
const STALL_CHECK_MS    = 1000;
const STALL_LIMIT_SEC   = 5;      // 连续多少秒 currentTime 未推进视为卡死

const media = {
  cfg: null,
  mode: 'webrtc',
  pc: null,
  hls: null,
  el: $('#player'),
  overlay: $('#videoOverlay'),
  overlayMsg: $('#overlayMsg'),
  rtspBox: $('#rtspBox'),

  // 重连状态
  reconnectAttempt: 0,
  reconnectTimer:   0,
  reconnecting:     false,
  connected:        false,
  // 停滞看门狗
  stallTimer:       0,
  lastMediaTime:    -1,
  stallSeconds:     0,

  async init() {
    try {
      const r = await fetch('/api/config').then(x => x.json());
      this.cfg = r.media || { whepPort: 8889, hlsPort: 8888, streamPath: 'live' };
    } catch {
      this.cfg = { whepPort: 8889, hlsPort: 8888, streamPath: 'live' };
    }
    $$('#modeSeg button').forEach(b => b.addEventListener('click', () => {
      this.switchMode(b.dataset.mode, /*userInitiated=*/true);
    }));

    /* 页面切回来立即抢一次重连，抵消 hidden 时 timer throttle 造成的延误。 */
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible' && !this.connected &&
          (this.mode === 'webrtc' || this.mode === 'hls')) {
        this.scheduleReconnect('visibility', /*immediate=*/true);
      }
    });

    this.switchMode('webrtc', /*userInitiated=*/true);
  },

  setOverlay(msg) {
    if (!msg) {
      this.overlay.classList.add('hidden');
    } else {
      this.overlayMsg.textContent = msg;
      this.overlay.classList.remove('hidden');
    }
  },

  async switchMode(mode, userInitiated = false) {
    if (userInitiated) {
      this.cancelPendingReconnect();
      this.reconnectAttempt = 0;
    }
    this.teardown();
    this.mode = mode;
    this.connected = false;
    $$('#modeSeg button').forEach(b => b.classList.toggle('on', b.dataset.mode === mode));
    this.rtspBox.classList.add('hidden');
    this.el.classList.remove('hidden');

    if (mode === 'webrtc' || mode === 'hls') {
      await this.connectCurrentMode();
    } else if (mode === 'rtsp') {
      this.el.classList.add('hidden');
      this.setOverlay('');
      $('#rtspUrl').textContent = `rtsp://${location.hostname}:8554/live`;
      this.rtspBox.classList.remove('hidden');
      this.stopStallWatchdog();
    }
  },

  async connectCurrentMode() {
    if (this.reconnecting) return;
    this.reconnecting = true;
    const host = location.hostname;
    const attempt = this.reconnectAttempt;
    try {
      if (this.mode === 'webrtc') {
        this.setOverlay(attempt > 0
          ? `reconnecting webrtc… (attempt ${attempt + 1})`
          : 'negotiating webrtc…');
        await this.startWHEP(`http://${host}:${this.cfg.whepPort}/${this.cfg.streamPath}/whep`);
      } else if (this.mode === 'hls') {
        this.setOverlay(attempt > 0
          ? `reconnecting hls… (attempt ${attempt + 1})`
          : 'loading hls…');
        await this.startHLS(`http://${host}:${this.cfg.hlsPort}/${this.cfg.streamPath}/index.m3u8`);
      }
    } catch (e) {
      log.err(`${this.mode} connect failed: ${e.message}`);
      this.reconnecting = false;
      this.scheduleReconnect(`${this.mode}-connect-error`);
      return;
    }
    this.reconnecting = false;
  },

  /* 单一重连调度入口：任何触发源都走这里，防并发。 */
  scheduleReconnect(reason, immediate = false) {
    if (this.mode !== 'webrtc' && this.mode !== 'hls') return;
    if (this.reconnectTimer || this.reconnecting) return;

    this.connected = false;
    this.teardown();

    if (immediate) {
      log.info(`reconnect: ${reason} (immediate)`);
      this.reconnectTimer = setTimeout(() => {
        this.reconnectTimer = 0;
        this.connectCurrentMode();
      }, 0);
      return;
    }

    const backoff = Math.min(
      RECONNECT_BASE_MS * Math.pow(2, this.reconnectAttempt),
      RECONNECT_MAX_MS,
    );
    this.reconnectAttempt++;
    log.err(`reconnect: ${reason}, retry in ${backoff}ms (attempt ${this.reconnectAttempt})`);
    this.setOverlay(`disconnected — retry in ${(backoff / 1000).toFixed(1)}s`);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = 0;
      this.connectCurrentMode();
    }, backoff);
  },

  cancelPendingReconnect() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = 0;
    }
  },

  onConnected(kind) {
    this.connected = true;
    this.reconnectAttempt = 0;
    this.setOverlay('');
    if (kind) log.ok(`${kind} connected`);
    this.startStallWatchdog();
  },

  teardown() {
    this.stopStallWatchdog();
    if (this.pc)  { try { this.pc.close();  } catch {} this.pc  = null; }
    if (this.hls) { try { this.hls.destroy(); } catch {} this.hls = null; }
    this.el.srcObject = null;
    this.el.removeAttribute('src');
    try { this.el.load(); } catch {}
  },

  /* 停滞看门狗：兜底"连接假活但流已停"——pc.connectionState 仍是 connected
   * 但 currentTime 停在原地。连续 STALL_LIMIT_SEC 秒未推进即触发重连。 */
  startStallWatchdog() {
    this.stopStallWatchdog();
    this.lastMediaTime = -1;
    this.stallSeconds  = 0;
    this.stallTimer = setInterval(() => this.checkStall(), STALL_CHECK_MS);
  },
  stopStallWatchdog() {
    if (this.stallTimer) { clearInterval(this.stallTimer); this.stallTimer = 0; }
    this.lastMediaTime = -1;
    this.stallSeconds  = 0;
  },
  checkStall() {
    if (!this.connected || this.el.paused) return;
    const notReady = this.el.readyState < 2;   // < HAVE_CURRENT_DATA
    const t = this.el.currentTime;
    const stuck = notReady || (this.lastMediaTime >= 0 && t === this.lastMediaTime);
    this.lastMediaTime = t;
    if (stuck) {
      if (++this.stallSeconds >= STALL_LIMIT_SEC) {
        log.err(`stream stalled for ${this.stallSeconds}s (t=${t.toFixed(2)}, rs=${this.el.readyState})`);
        this.scheduleReconnect('stall');
      }
    } else {
      this.stallSeconds = 0;
    }
  },

  // mediamtx WHEP：POST SDP offer，得到 answer
  async startWHEP(url) {
    const pc = new RTCPeerConnection({ iceServers: [] });
    this.pc = pc;
    pc.addTransceiver('video', { direction: 'recvonly' });
    pc.addTransceiver('audio', { direction: 'recvonly' });

    pc.ontrack = (ev) => {
      if (this.el.srcObject !== ev.streams[0]) {
        this.el.srcObject = ev.streams[0];
        this.onConnected('webrtc');
      }
      const trk = ev.track;
      if (!trk) return;
      /* track mute 常见于短暂丢包，1.5s 缓冲后仍 mute 才重连。 */
      trk.addEventListener('mute', () => {
        setTimeout(() => {
          if (trk.muted && this.pc === pc) this.scheduleReconnect('track-mute');
        }, 1500);
      });
      trk.addEventListener('ended', () => {
        if (this.pc === pc) this.scheduleReconnect('track-ended');
      });
    };
    /* pc 身份守卫（this.pc !== pc）：忽略被替换掉的旧 pc 的迟到事件，
     * 否则旧 pc 的 closed 会把新连接又拆掉。 */
    pc.onconnectionstatechange = () => {
      if (this.pc !== pc) return;
      const st = pc.connectionState;
      if (st === 'failed' || st === 'disconnected' || st === 'closed') {
        this.scheduleReconnect(`webrtc-${st}`);
      }
    };
    pc.oniceconnectionstatechange = () => {
      /* 某些 Chromium 版本只发 iceConnectionState，兜一层。 */
      if (this.pc !== pc) return;
      if (pc.iceConnectionState === 'failed') this.scheduleReconnect('webrtc-ice-failed');
    };

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    const resp = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/sdp' },
      body: offer.sdp,
    });
    if (!resp.ok) throw new Error(`whep http ${resp.status}`);
    const answer = await resp.text();
    await pc.setRemoteDescription({ type: 'answer', sdp: answer });
  },

  async startHLS(url) {
    // Safari 原生支持
    if (this.el.canPlayType('application/vnd.apple.mpegurl')) {
      this.el.src = url;
      this.el.addEventListener('loadeddata', () => this.onConnected('hls'), { once: true });
      this.el.addEventListener('error', () => this.scheduleReconnect('hls-video-error'), { once: true });
      return;
    }
    // 其他浏览器：动态拉 hls.js（CDN，仅这一处）
    const Hls = await import('https://cdn.jsdelivr.net/npm/hls.js@1.5.13/dist/hls.mjs')
      .then(m => m.default).catch(() => null);
    if (!Hls || !Hls.isSupported()) throw new Error('hls.js unavailable');
    const hls = new Hls({ liveDurationInfinity: true, lowLatencyMode: true });
    this.hls = hls;
    hls.loadSource(url);
    hls.attachMedia(this.el);
    hls.on(Hls.Events.MANIFEST_PARSED, () => this.onConnected('hls'));
    hls.on(Hls.Events.ERROR, (_e, data) => {
      if (!data.fatal) return;
      /* fatal 先让 hls.js 自愈一次，不行才走完整重连。 */
      if (data.type === Hls.ErrorTypes.NETWORK_ERROR) {
        log.err(`hls fatal network: ${data.details}, recover`);
        try { hls.startLoad(); return; } catch {}
      } else if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
        log.err(`hls fatal media: ${data.details}, recover`);
        try { hls.recoverMediaError(); return; } catch {}
      }
      this.scheduleReconnect(`hls-fatal-${data.type}`);
    });
  },
};

/* ─────────────────────────── face 绘框叠加 ─────────────────────────── *
 * 数据链路：daemon (facedetect) → events FIFO (NDJSON) → web /ws/events
 *   → statusMod.ws.onmessage → faceOverlay.onFacesEvent → rAF 循环重绘。
 *
 * 坐标系归一化：上报的 rects 在 daemon 主线采集帧尺寸（frame_w/h）下。
 * <video> 元素重用了 CSS `object-fit: contain`，这意味着实际显示区可能
 * 小于 canvas 尺寸（letterbox）。每帧重绘时仅根据视频固有尺寸与当前 canvas
 * 尺寸计算 letterbox 偏移量，转换后再 cv::rectangle 样的 strokeRect。
 *
 * 限时性：仅保留最新一帧，且若距上一次上报超过 500ms 就清除（避免"人已走
 * 但框还在"）；cooldown 与频率控制已在 daemon 侧完成，前端不再叠加频率限制。 */
const faceOverlay = {
  canvas:  null,
  ctx:     null,
  toggle:  null,
  video:   null,
  enabled: true,
  latest:  null,        // { rects, frame_w, frame_h, arriveTs }
  rafId:   0,

  init() {
    this.canvas = $('#faceOverlay');
    this.video  = $('#player');
    this.toggle = $('#faceOverlayToggle');
    if (!this.canvas || !this.video) return;

    this.ctx = this.canvas.getContext('2d');
    this.enabled = this.toggle ? this.toggle.checked : true;

    if (this.toggle) {
      this.toggle.addEventListener('change', () => {
        this.enabled = this.toggle.checked;
        if (!this.enabled) this.clear();
      });
    }

    /* HiDPI 适配：把 canvas 的位图尺寸换算为 devicePixelRatio，
     * strokeRect 在 4K 屏上仍保持锐利的 1px 线重。 */
    const resize = () => {
      const dpr  = window.devicePixelRatio || 1;
      const rect = this.canvas.getBoundingClientRect();
      this.canvas.width  = Math.max(1, Math.round(rect.width  * dpr));
      this.canvas.height = Math.max(1, Math.round(rect.height * dpr));
      this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);   // 后续直接以 CSS 像素下笔
    };
    resize();
    window.addEventListener('resize', resize);

    const loop = () => {
      this.rafId = requestAnimationFrame(loop);
      this.render();
    };
    this.rafId = requestAnimationFrame(loop);
  },

  onFacesEvent(msg) {
    if (!msg || msg.kind !== 'faces') return;
    if (!Array.isArray(msg.rects)) return;
    this.latest = {
      rects:    msg.rects,
      frame_w:  msg.frame_w  || 0,
      frame_h:  msg.frame_h  || 0,
      arriveTs: performance.now(),
    };
  },

  clear() {
    if (!this.ctx) return;
    const rect = this.canvas.getBoundingClientRect();
    this.ctx.clearRect(0, 0, rect.width, rect.height);
  },

  render() {
    if (!this.ctx || !this.enabled) return;

    const rectC = this.canvas.getBoundingClientRect();
    this.ctx.clearRect(0, 0, rectC.width, rectC.height);

    const l = this.latest;
    if (!l || !l.rects.length) return;
    /* 500ms 之后不再保留，避免人走后框遗留。daemon fps=5、cooldown=200ms，
     * 500ms 足够接住相邻两帧之间的空隔。 */
    if (performance.now() - l.arriveTs > 500) return;

    /* 计算 <video> 内部真实显示区（object-fit: contain letterbox）。 */
    const v = this.video;
    const vw = v.videoWidth  || l.frame_w;
    const vh = v.videoHeight || l.frame_h;
    if (!vw || !vh || !l.frame_w || !l.frame_h) return;

    const cw = rectC.width;
    const ch = rectC.height;
    const stageAspect = cw / ch;
    const videoAspect = vw / vh;
    let dispW, dispH, dispX, dispY;
    if (videoAspect > stageAspect) {
      dispW = cw; dispH = cw / videoAspect;
      dispX = 0;  dispY = (ch - dispH) / 2;
    } else {
      dispH = ch; dispW = ch * videoAspect;
      dispY = 0;  dispX = (cw - dispW) / 2;
    }

    const sx = dispW / l.frame_w;
    const sy = dispH / l.frame_h;

    this.ctx.save();
    this.ctx.strokeStyle = '#7FE7B5';
    this.ctx.lineWidth   = 2;
    this.ctx.shadowColor = 'rgba(0,0,0,0.6)';
    this.ctx.shadowBlur  = 2;
    for (const r of l.rects) {
      const x = dispX + r.x * sx;
      const y = dispY + r.y * sy;
      const w = r.w * sx;
      const h = r.h * sy;
      this.ctx.strokeRect(x, y, w, h);
    }
    this.ctx.restore();
  },
};

/* ─────────────────────────── 命令分发 ─────────────────────────── */
const ctrl = {
  filterMax: 6,
  init() {
    this.renderFilterSeg();
    document.addEventListener('click', (e) => {
      const t = e.target.closest('[data-cmd]');
      if (!t) return;
      this.handle(t.dataset.cmd, t);
    });
    // 输入框回车 = 触发对应按钮
    $('#rawInput').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') this.handle('raw-send');
    });
    $('#pagText').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') this.handle('pag-set-text');
    });
    // 状态/PAG 刷新
    $('#btnStatusRefresh').addEventListener('click', () => statusMod.pull());
    $('#btnPagRefresh').addEventListener('click', () => this.handle('pag-get'));
    $('#btnLogClear').addEventListener('click', () => { log.el.textContent = ''; });
    // 快捷键
    document.addEventListener('keydown', (e) => {
      if (e.target.matches('input, textarea')) return;
      if (e.code === 'Space')      { e.preventDefault(); this.handle('snapshot'); }
      else if (e.key === 'r' || e.key === 'R') { this.handle('record-toggle'); }
      else if (e.key === '/')      { e.preventDefault(); $('#rawInput').focus(); }
      else if (/^[1-9]$/.test(e.key)) {
        const n = parseInt(e.key, 10);
        if (n <= this.filterMax + 1) {  // +1 因为 0 也算一个
          this.setFilter(n - 1);
        }
      }
    });
  },

  renderFilterSeg() {
    const seg = $('#filterSeg');
    seg.innerHTML = '';
    for (let i = 0; i <= this.filterMax; i++) {
      const b = document.createElement('button');
      b.textContent = String(i);
      b.dataset.cmd = 'filter-set';
      b.dataset.type = String(i);
      seg.appendChild(b);
    }
  },

  markFilter(n) {
    $$('#filterSeg button').forEach(b => {
      b.classList.toggle('on', Number(b.dataset.type) === Number(n));
    });
  },

  async setFilter(n) {
    await call(`filter set ${n}`, 'POST', '/api/filter', { type: n });
    this.markFilter(n);
  },

  async handle(cmd, el) {
    switch (cmd) {
      case 'filter-set': {
        const n = Number(el.dataset.type);
        return this.setFilter(n);
      }
      case 'filter-next':
        await call('filter next', 'POST', '/api/filter', { action: 'next' });
        statusMod.pull();
        break;
      case 'filter-prev':
        await call('filter prev', 'POST', '/api/filter', { action: 'prev' });
        statusMod.pull();
        break;
      case 'filter-reload':
        await call('reload shader', 'POST', '/api/filter/reload');
        break;
      case 'snapshot': {
        const r = await call('snapshot', 'POST', '/api/snapshot', {});
        if (r.ok && r.body && r.body.path) {
          $('#snapResult').textContent = `last: ${r.body.path}`;
        }
        break;
      }
      case 'record-start':
        await call('record start', 'POST', '/api/record', { action: 'start' });
        $('#recState').textContent = 'recording';
        $('#recState').classList.add('on'); $('#recState').classList.remove('ok');
        break;
      case 'record-stop':
        await call('record stop', 'POST', '/api/record', { action: 'stop' });
        $('#recState').textContent = 'idle';
        $('#recState').classList.remove('on');
        break;
      case 'record-toggle': {
        const cls = $('#recState').classList.contains('on');
        return this.handle(cls ? 'record-stop' : 'record-start');
      }
      case 'record-auto': {
        const secs = Number($('#recAuto').value);
        await call(`record auto ${secs}`, 'POST', '/api/record', { action: 'auto', secs });
        break;
      }
      case 'pag-get':
        await call('pag get', 'GET', '/api/pag');
        break;
      case 'pag-set-file': {
        const path = $('#pagFile').value.trim();
        if (!path) return log.err('pag file: empty');
        await call(`pag set-file ${path}`, 'POST', '/api/pag/file', { path });
        break;
      }
      case 'pag-set-text': {
        const idx  = Number($('#pagTextIdx').value);
        const text = $('#pagText').value;
        if (text === '') return log.err('pag text: empty');
        await call(`pag set-text ${idx} ${text}`, 'POST', '/api/pag/text', { idx, text });
        break;
      }
      case 'pag-set-replace': {
        const idx = Number($('#pagReplaceIdx').value);
        await call(`pag set-replace-image ${idx}`, 'POST', '/api/pag/replace', { idx });
        break;
      }
      case 'pag-set-throttle': {
        const n = Number($('#pagThrottle').value);
        await call(`pag set-replace-image-every ${n}`, 'POST', '/api/pag/throttle', { n });
        break;
      }
      case 'raw-send': {
        const line = $('#rawInput').value.trim();
        if (!line) return;
        await call(`raw: ${line}`, 'POST', '/api/raw', { line });
        $('#rawInput').value = '';
        break;
      }
      default:
        log.err(`unknown cmd: ${cmd}`);
    }
  },
};

/* ─────────────────────────── status / WebSocket ─────────────────────────── */
const statusMod = {
  ws: null,
  init() {
    this.connect();
    this.pull();
  },
  connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const url = `${proto}://${location.host}/ws/events`;
    let ws;
    try { ws = new WebSocket(url); } catch (e) {
      log.err(`ws connect: ${e.message}`); return;
    }
    this.ws = ws;
    ws.onopen = () => log.info('events stream connected');
    ws.onclose = () => {
      log.err('events stream closed; retry in 3s');
      this.markOffline();
      setTimeout(() => this.connect(), 3000);
    };
    ws.onerror = () => { /* close handler 会接力 */ };
    ws.onmessage = (ev) => {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.kind === 'status' && m.ok && m.body) {
        this.apply(m.body);
        this.markOnline();
      } else if (m.kind === 'status_err') {
        this.markOffline();
      } else if (m.kind === 'faces') {
        faceOverlay.onFacesEvent(m);
      }
    };
  },
  async pull() {
    try {
      const r = await fetch('/api/status').then(x => x.json());
      if (r.ok && r.body) { this.apply(r.body); this.markOnline(); }
      else this.markOffline();
    } catch { this.markOffline(); }
  },
  markOnline() {
    $('#liveDot').className = 'dot dot-live';
    $('#liveText').textContent = 'live';
    $('#liveText').classList.remove('muted');
  },
  markOffline() {
    $('#liveDot').className = 'dot dot-err';
    $('#liveText').textContent = 'daemon offline';
    $('#liveText').classList.add('muted');
  },
  apply(body) {
    const set = (id, key, fmt) => {
      const v = body[key];
      if (v === undefined) return;
      $('#' + id).textContent = fmt ? fmt(v) : v;
    };
    // 顶栏精简版
    set('hUptime', 'uptime');
    set('hEncoder','encoder');
    set('hClients','clients');
    set('hFilter', 'filter_type');
    // 状态卡完整版
    set('sUptime', 'uptime');
    set('sClients','clients');
    set('sBitrate','bitrate_kbps', v => `${v} kbps`);
    set('sEncoder','encoder');
    set('sFilter', 'filter_type');
    set('sPag',    'pag_file');
    set('sDevice', 'device');
    // 分辨率：daemon 通常给 width/height/framerate
    if (body.width && body.height) {
      $('#sRes').textContent = `${body.width}x${body.height}@${body.framerate || '?'}`;
    }
    // 同步 filter 当前选中
    if (body.filter_type !== undefined) {
      ctrl.markFilter(body.filter_type);
    }
    if (body.max_type !== undefined) {
      const m = Number(body.max_type);
      if (m !== ctrl.filterMax) {
        ctrl.filterMax = m;
        ctrl.renderFilterSeg();
        ctrl.markFilter(body.filter_type);
      }
    }
  },
};

/* ─────────────────────────── boot ─────────────────────────── */
window.addEventListener('DOMContentLoaded', async () => {
  log.info('vm_iot console ready');
  ctrl.init();
  faceOverlay.init();
  await media.init();
  statusMod.init();
});
