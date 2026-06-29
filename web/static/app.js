// app.js — vm_iot console front-end
// ---------------------------------------------------------------------------
// 单文件、零依赖、纯 ESM。
// 三大模块：
//   1. media   — WebRTC (mediamtx WHEP) / HLS / RTSP-url 三档媒体源切换
//   2. control — 命令分发：把按钮 / 输入框翻译成 fetch /api/...
//   3. status  — WebSocket /ws/events 拉 1Hz status，刷状态面板 + 顶栏
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

/* ─────────────────────────── 媒体接入 ─────────────────────────── */
const media = {
  cfg: null,                   // /api/config 返回的 mediamtx 端口
  mode: 'webrtc',
  pc: null,                    // RTCPeerConnection
  hls: null,                   // hls.js 实例（按需 import）
  el: $('#player'),
  overlay: $('#videoOverlay'),
  overlayMsg: $('#overlayMsg'),
  rtspBox: $('#rtspBox'),

  async init() {
    try {
      const r = await fetch('/api/config').then(x => x.json());
      this.cfg = r.media || { whepPort: 8889, hlsPort: 8888, streamPath: 'live' };
    } catch {
      this.cfg = { whepPort: 8889, hlsPort: 8888, streamPath: 'live' };
    }
    $$('#modeSeg button').forEach(b => b.addEventListener('click', () => {
      this.switchMode(b.dataset.mode);
    }));
    this.switchMode('webrtc');
  },

  setOverlay(msg) {
    if (!msg) {
      this.overlay.classList.add('hidden');
    } else {
      this.overlayMsg.textContent = msg;
      this.overlay.classList.remove('hidden');
    }
  },

  async switchMode(mode) {
    this.teardown();
    this.mode = mode;
    $$('#modeSeg button').forEach(b => b.classList.toggle('on', b.dataset.mode === mode));
    this.rtspBox.classList.add('hidden');
    this.el.classList.remove('hidden');

    const host = location.hostname;
    if (mode === 'webrtc') {
      this.setOverlay('negotiating webrtc…');
      try {
        await this.startWHEP(`http://${host}:${this.cfg.whepPort}/${this.cfg.streamPath}/whep`);
      } catch (e) {
        log.err(`webrtc failed: ${e.message}; try HLS`);
        this.setOverlay('webrtc failed — try HLS');
      }
    } else if (mode === 'hls') {
      this.setOverlay('loading hls…');
      try {
        await this.startHLS(`http://${host}:${this.cfg.hlsPort}/${this.cfg.streamPath}/index.m3u8`);
      } catch (e) {
        log.err(`hls failed: ${e.message}`);
        this.setOverlay('hls failed');
      }
    } else if (mode === 'rtsp') {
      this.el.classList.add('hidden');
      this.setOverlay('');
      $('#rtspUrl').textContent = `rtsp://${host}:8554/live`;
      this.rtspBox.classList.remove('hidden');
    }
  },

  teardown() {
    if (this.pc) { try { this.pc.close(); } catch {} this.pc = null; }
    if (this.hls) { try { this.hls.destroy(); } catch {} this.hls = null; }
    this.el.srcObject = null;
    this.el.removeAttribute('src');
  },

  // mediamtx WHEP：POST SDP offer，得到 answer
  async startWHEP(url) {
    const pc = new RTCPeerConnection({
      iceServers: [],  // 局域网内不需要 STUN
    });
    this.pc = pc;
    pc.addTransceiver('video', { direction: 'recvonly' });
    pc.addTransceiver('audio', { direction: 'recvonly' });

    pc.ontrack = (ev) => {
      if (this.el.srcObject !== ev.streams[0]) {
        this.el.srcObject = ev.streams[0];
        this.setOverlay('');
        log.ok('webrtc track received');
      }
    };
    pc.onconnectionstatechange = () => {
      if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) {
        this.setOverlay(`webrtc ${pc.connectionState}`);
      }
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
      this.el.addEventListener('loadeddata', () => this.setOverlay(''), { once: true });
      this.el.addEventListener('error', () => this.setOverlay('hls error'), { once: true });
      return;
    }
    // 其他浏览器：动态拉 hls.js（CDN，仅这一处）
    const Hls = await import('https://cdn.jsdelivr.net/npm/hls.js@1.5.13/dist/hls.mjs')
      .then(m => m.default).catch(() => null);
    if (!Hls || !Hls.isSupported()) {
      throw new Error('hls.js unavailable');
    }
    const hls = new Hls({ liveDurationInfinity: true, lowLatencyMode: true });
    this.hls = hls;
    hls.loadSource(url);
    hls.attachMedia(this.el);
    hls.on(Hls.Events.MANIFEST_PARSED, () => this.setOverlay(''));
    hls.on(Hls.Events.ERROR, (_e, data) => {
      if (data.fatal) this.setOverlay(`hls fatal: ${data.type}`);
    });
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
  await media.init();
  statusMod.init();
});
