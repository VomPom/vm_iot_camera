// fifo.mjs
// ---------------------------------------------------------------------------
// vm_iot daemon FIFO 客户端（Node.js 复刻自 src/cli/iotcamctl.cpp）。
//
// 设计要点：
//   1. 长持有 reply FIFO 读端（O_RDONLY | O_NONBLOCK），daemon 端用 O_RDWR
//      持有读写端，因此本端 open 后不会因 ENXIO 失败、也不会读到 EOF。
//      持有期间所有命令都从同一个 fd 顺序读应答 → 不会出现 fd 切换导致
//      应答漂到下一个命令的情况。
//   2. 命令在内部互斥队列里串行执行：同一时刻只有一条命令在等应答；
//      新命令排队等待。这等价于 iotcamctl 的 flock（LOCK_EX）。
//   3. 协议状态机：daemon 应答以单独一行 "." 收尾。我们持续 read 累积，
//      直到 buffer 末尾匹配 "\n.\n"（或开头就是 ".\n"），一次性切出帧。
//   4. 超时：每条命令独立计时；超时后 reject 当前命令并把后续帧丢弃到
//      下一个 ".\n"，避免污染下一个命令。
//   5. 不依赖任何 native 模块，纯 fs + 事件循环。
// ---------------------------------------------------------------------------

import { open, stat } from 'node:fs/promises';
import { constants as FS } from 'node:fs';

/* ─────────────────────────── 应答解析 ─────────────────────────── */
// payload 不含结尾的 ".\n"。返回 { ok, cmd, error, body, raw }
function parseReply(payload) {
  const lines = payload.split('\n');
  // 末尾若是 split 多出来的空串则丢掉
  if (lines.length && lines[lines.length - 1] === '') lines.pop();

  const head = lines.shift() ?? '';
  let ok = false;
  let cmd = '';
  let error = '';

  if (head.startsWith('ok ')) {
    ok = true;
    cmd = head.slice(3);
  } else if (head.startsWith('err ')) {
    ok = false;
    const rest = head.slice(4);
    const sp = rest.lastIndexOf(' ');
    if (sp < 0) {
      cmd = rest;
    } else {
      cmd = rest.slice(0, sp);
      error = rest.slice(sp + 1);
    }
  } else {
    ok = false;
    cmd = head;
    error = 'protocol_error';
  }

  // body 行尝试按 key=value 解析为对象，未命中的丢进 raw[]
  const body = {};
  const raw = [];
  for (const line of lines) {
    if (!line) continue;
    const eq = line.indexOf('=');
    if (eq <= 0) {
      raw.push(line);
      continue;
    }
    const k = line.slice(0, eq).trim();
    const v = line.slice(eq + 1).trim();
    if (!k) {
      raw.push(line);
      continue;
    }
    body[k] = v;
  }
  return { ok, cmd, error, body, raw };
}

/* ─────────────────────────── FIFO 客户端 ─────────────────────────── */
export class FifoClient {
  /**
   * @param {object} opts
   * @param {string} opts.ctlPath   请求 FIFO 路径
   * @param {string} opts.replyPath 应答 FIFO 路径
   * @param {number} [opts.timeoutMs=2000] 单条命令默认超时
   */
  constructor({ ctlPath, replyPath, timeoutMs = 2000 }) {
    this.ctlPath = ctlPath;
    this.replyPath = replyPath;
    this.timeoutMs = timeoutMs;

    /** @type {import('node:fs/promises').FileHandle | null} */
    this._replyFh = null;
    /** @type {Buffer} */
    this._buf = Buffer.alloc(0);

    // 命令队列：{ line, resolve, reject, timeoutMs }
    this._queue = [];
    this._busy = false;

    this._closed = false;
    this._readLoopPromise = null;

    // 当前正在等待的命令（用于读循环把帧交给它）
    this._pending = null;
  }

  async start() {
    // 校验两个 FIFO 都存在且确实是 FIFO
    await this._checkFifo(this.ctlPath, 'control');
    await this._checkFifo(this.replyPath, 'reply');

    // 长持有 reply FIFO（O_RDONLY | O_NONBLOCK），并启动读循环
    this._replyFh = await open(this.replyPath, FS.O_RDONLY | FS.O_NONBLOCK);
    this._readLoopPromise = this._readLoop().catch(err => {
      // 读循环异常 = FIFO 通道断了，所有 pending 命令立刻失败
      this._failAllPending(new Error(`reply fifo read loop exited: ${err.message}`));
    });
  }

  async stop() {
    this._closed = true;
    this._failAllPending(new Error('fifo client stopped'));
    if (this._replyFh) {
      try { await this._replyFh.close(); } catch { /* ignore */ }
      this._replyFh = null;
    }
  }

  /**
   * 发送一行命令（不含 \n），等待 daemon 应答。
   * @param {string} line       例如 "filter set 2"
   * @param {number} [timeoutMs] 覆盖默认超时
   * @returns {Promise<{ok, cmd, error, body, raw}>}
   */
  send(line, timeoutMs) {
    if (this._closed) {
      return Promise.reject(new Error('fifo client closed'));
    }
    return new Promise((resolve, reject) => {
      this._queue.push({
        line,
        resolve,
        reject,
        timeoutMs: timeoutMs ?? this.timeoutMs,
      });
      this._drain();
    });
  }

  /* ─────────────────────────── 内部 ─────────────────────────── */

  async _checkFifo(path, role) {
    let st;
    try {
      st = await stat(path);
    } catch (e) {
      throw new Error(
        `${role} FIFO '${path}' not found: ${e.message}. ` +
        `is vm_iot daemon running?`
      );
    }
    if (!st.isFIFO()) {
      throw new Error(`${role} path '${path}' is not a FIFO`);
    }
  }

  // 串行驱动队列：同一时刻仅一条命令在飞
  async _drain() {
    if (this._busy) return;
    const job = this._queue.shift();
    if (!job) return;
    this._busy = true;

    let timer = null;
    this._pending = {
      resolve: (frame) => {
        if (timer) clearTimeout(timer);
        this._pending = null;
        this._busy = false;
        try { job.resolve(parseReply(frame)); } finally { this._drain(); }
      },
      reject: (err) => {
        if (timer) clearTimeout(timer);
        this._pending = null;
        this._busy = false;
        // 超时/失败时，把当前累积 buffer 中尚未消费的内容丢到下一个 ".\n"
        // 防止下一条命令读到上一条的残骸
        this._discardOneFrame();
        try { job.reject(err); } finally { this._drain(); }
      },
    };

    timer = setTimeout(() => {
      const p = this._pending;
      if (p) p.reject(new Error(`timeout waiting daemon reply (${job.timeoutMs}ms)`));
    }, job.timeoutMs);

    // 写请求：每次新开 fd（daemon 端是 O_RDWR 持有，O_WRONLY|O_NONBLOCK 不会 ENXIO）
    try {
      const wfh = await open(this.ctlPath, FS.O_WRONLY | FS.O_NONBLOCK);
      try {
        await wfh.write(job.line + '\n');
      } finally {
        await wfh.close();
      }
    } catch (e) {
      // 写失败：直接 reject 当前命令并继续
      const p = this._pending;
      if (p) p.reject(new Error(`write request failed: ${e.message}`));
    }
  }

  // 读循环：把 reply FIFO 累积到 buf，按 "\n.\n" 切帧
  async _readLoop() {
    const tmp = Buffer.allocUnsafe(4096);
    while (!this._closed && this._replyFh) {
      let n = 0;
      try {
        const r = await this._replyFh.read(tmp, 0, tmp.length, null);
        n = r.bytesRead;
      } catch (e) {
        if (e.code === 'EAGAIN') {
          // 非阻塞读暂时无数据：等 5ms 再试。
          // (Node 没有 epoll 直通；此处简单轮询足够，FIFO 流量很低)
          await sleep(5);
          continue;
        }
        throw e;
      }
      if (n === 0) {
        // 理论上 daemon 持 O_RDWR，不会到这；保险起见短暂 sleep
        await sleep(5);
        continue;
      }
      this._buf = Buffer.concat([this._buf, tmp.subarray(0, n)]);

      // 尝试切出尽可能多的完整帧
      while (true) {
        const frame = this._takeFrame();
        if (frame === null) break;
        const p = this._pending;
        if (p) {
          p.resolve(frame);
        }
        // 没人等：丢弃（极少出现，daemon 不会主动推送）
      }
    }
  }

  // 从 _buf 头部切一帧（不含结尾的 ".\n"）。返回 null 表示帧未完整。
  _takeFrame() {
    const buf = this._buf;
    // 寻找单独一行的 "."：要么 buf 起始就是 ".\n"，要么前缀 "...\n.\n"
    // 用 indexOf 扫 ".\n" 序列，逐个验证前一字节是 '\n' 或位置=0
    let from = 0;
    while (from < buf.length) {
      const idx = buf.indexOf('.\n', from);
      if (idx < 0) return null;
      const isLineStart = idx === 0 || buf[idx - 1] === 0x0a; // '\n'
      if (isLineStart) {
        const frame = buf.subarray(0, idx).toString('utf8');
        // 去掉帧末尾可能的 '\n'（即 head/body 部分的最后一行换行）
        const cleaned = frame.endsWith('\n') ? frame.slice(0, -1) : frame;
        this._buf = buf.subarray(idx + 2);
        return cleaned;
      }
      from = idx + 1;
    }
    return null;
  }

  // 超时清理：把 _buf 推进到下一帧之后
  _discardOneFrame() {
    const idx = this._findFrameEnd();
    if (idx >= 0) {
      this._buf = this._buf.subarray(idx + 2);
    }
  }

  _findFrameEnd() {
    const buf = this._buf;
    let from = 0;
    while (from < buf.length) {
      const idx = buf.indexOf('.\n', from);
      if (idx < 0) return -1;
      if (idx === 0 || buf[idx - 1] === 0x0a) return idx;
      from = idx + 1;
    }
    return -1;
  }

  _failAllPending(err) {
    if (this._pending) {
      try { this._pending.reject(err); } catch { /* ignore */ }
    }
    const q = this._queue.splice(0);
    for (const job of q) {
      try { job.reject(err); } catch { /* ignore */ }
    }
  }
}

function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}
