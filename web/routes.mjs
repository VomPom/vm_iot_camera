// routes.mjs
// ---------------------------------------------------------------------------
// REST + WebSocket 路由：把前端动作翻译成 daemon FIFO 命令行；并通过 WS
// 周期推送 status，给前端实时仪表板用。
//
// 安全约束：
//   1. 命令白名单（构造函数体内全部硬编码的 buildXxx），不接受任意字符串。
//   2. 唯一例外是 POST /api/raw：限长 256 字节、剥换行，仍然交给 FIFO 由
//      daemon 自己做语义校验。
//   3. 不接受路径穿越：snapshot/path、pag/file 都仅做基本校验，daemon 端
//      会再做一遍。
// ---------------------------------------------------------------------------

const RAW_MAX_LEN = 256;

// 把 daemon 应答转成 HTTP 响应
function reply(res, r) {
  if (!r.ok) {
    return res.code(400).send({
      ok: false,
      cmd: r.cmd,
      error: r.error || 'unknown',
      raw: r.raw,
    });
  }
  return res.send({ ok: true, cmd: r.cmd, body: r.body, raw: r.raw });
}

// 数字校验
function asInt(v, name) {
  if (v === undefined || v === null || v === '') {
    throw new Error(`${name} required`);
  }
  const n = Number(v);
  if (!Number.isInteger(n)) throw new Error(`${name} must be integer`);
  return n;
}

// 字符串校验：禁止换行 / NUL，避免协议注入
function asLine(v, name, maxLen = 512) {
  if (typeof v !== 'string') throw new Error(`${name} must be string`);
  if (v.length === 0) throw new Error(`${name} empty`);
  if (v.length > maxLen) throw new Error(`${name} too long (>${maxLen})`);
  if (/[\n\r\0]/.test(v)) throw new Error(`${name} contains forbidden char`);
  return v;
}

/**
 * @param {import('fastify').FastifyInstance} app
 * @param {{ fifo: import('./fifo.mjs').FifoClient }} ctx
 */
export async function registerRoutes(app, { fifo }) {
  /* ───────── 健康检查 ───────── */
  app.get('/api/health', async () => ({ ok: true, ts: Date.now() }));

  /* ───────── 状态 ───────── */
  app.get('/api/status', async (_req, res) => {
    const r = await fifo.send('status');
    return reply(res, r);
  });

  /* ───────── filter ───────── */
  app.post('/api/filter', async (req, res) => {
    const { type, action } = req.body ?? {};
    let line;
    if (action === 'next' || action === 'prev' || action === 'get') {
      line = `filter ${action}`;
    } else if (type !== undefined) {
      const n = asInt(type, 'type');
      line = `filter set ${n}`;
    } else {
      return res.code(400).send({ ok: false, error: 'usage: {type:N} or {action:next|prev|get}' });
    }
    return reply(res, await fifo.send(line));
  });

  app.post('/api/filter/reload', async (_req, res) => {
    return reply(res, await fifo.send('reload'));
  });

  /* ───────── snapshot ───────── */
  app.post('/api/snapshot', async (req, res) => {
    const { path } = req.body ?? {};
    let line = 'snapshot';
    if (path) {
      asLine(path, 'path', 1024);
      line = `snapshot ${path}`;
    }
    return reply(res, await fifo.send(line, 4000));  // 抓拍可能稍慢
  });

  /* ───────── record (daemon 协议预留位；不存在时会回 unknown_command) ───────── */
  app.post('/api/record', async (req, res) => {
    const { action, secs } = req.body ?? {};
    if (!['start', 'stop', 'status', 'auto'].includes(action)) {
      return res.code(400).send({ ok: false, error: 'action must be start|stop|status|auto' });
    }
    let line = `record ${action}`;
    if (action === 'auto') {
      const n = asInt(secs, 'secs');
      line += ` ${n}`;
    }
    return reply(res, await fifo.send(line));
  });

  /* ───────── PAG ───────── */
  app.get('/api/pag', async (_req, res) => {
    return reply(res, await fifo.send('pag get'));
  });

  app.post('/api/pag/file', async (req, res) => {
    const { path } = req.body ?? {};
    asLine(path, 'path', 1024);
    return reply(res, await fifo.send(`pag set-file ${path}`));
  });

  app.post('/api/pag/text', async (req, res) => {
    const { idx, text } = req.body ?? {};
    const i = asInt(idx, 'idx');
    asLine(text, 'text', 256);
    // 协议是 "pag set-text <IDX> <UTF8...>"，后面整段当文本
    return reply(res, await fifo.send(`pag set-text ${i} ${text}`));
  });

  app.post('/api/pag/replace', async (req, res) => {
    const { idx } = req.body ?? {};
    const i = asInt(idx, 'idx');
    return reply(res, await fifo.send(`pag set-replace-image ${i}`));
  });

  app.post('/api/pag/throttle', async (req, res) => {
    const { n } = req.body ?? {};
    const v = asInt(n, 'n');
    return reply(res, await fifo.send(`pag set-replace-image-every ${v}`));
  });

  /* ───────── raw（高级） ───────── */
  app.post('/api/raw', async (req, res) => {
    const { line } = req.body ?? {};
    if (typeof line !== 'string' || line.length === 0) {
      return res.code(400).send({ ok: false, error: 'line required' });
    }
    if (line.length > RAW_MAX_LEN) {
      return res.code(400).send({ ok: false, error: `line too long (>${RAW_MAX_LEN})` });
    }
    // 强行剥掉换行，防止注入多行命令
    const cleaned = line.replace(/[\r\n]/g, ' ').trim();
    if (!cleaned) return res.code(400).send({ ok: false, error: 'line empty after cleanup' });
    return reply(res, await fifo.send(cleaned));
  });

  /* ───────── WebSocket 事件流 ───────── */
  // 1Hz 主动推 status；命令完成事件通过 fastify hooks 旁路推（这里简化为
  // 仅 status 推送 + 客户端发命令时自己显示回执）。
  app.get('/ws/events', { websocket: true }, (socket /* WebSocket */, req) => {
    const send = (obj) => {
      try { socket.send(JSON.stringify(obj)); } catch { /* ignore */ }
    };
    send({ kind: 'hello', ts: Date.now() });

    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const r = await fifo.send('status', 1500);
        send({ kind: 'status', ts: Date.now(), ok: r.ok, body: r.body });
      } catch (e) {
        send({ kind: 'status_err', ts: Date.now(), error: String(e.message || e) });
      }
    };
    const timer = setInterval(tick, 1000);
    tick();

    socket.on('close', () => {
      alive = false;
      clearInterval(timer);
    });
    socket.on('error', () => {
      alive = false;
      clearInterval(timer);
    });
  });
}
