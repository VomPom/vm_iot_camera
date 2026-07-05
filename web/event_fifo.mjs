// SPDX-License-Identifier: MIT
//
// EventFifoReader —— daemon → web 单向事件流的读端。
//
// 与 fifo.mjs 的 FifoClient 定位差异：
//   - FifoClient 是"请求-应答"客户端，写 request、读 reply、维护命令队列；
//   - EventFifoReader 是"只读广播"消费者：daemon 单向写、web 单向读，
//     无请求、无 ACK、无重排。读到一行就 emit 一次事件对象。
//
// 设计要点：
//   1) O_RDONLY | O_NONBLOCK 打开 event_fifo；
//      借鉴 FifoClient 的思路，用 5 ms 轮询循环读，避免依赖 stream fd 的
//      readable 事件在 fifo 场景下的怪异行为（部分 kernel 版本 EAGAIN 后不再
//      唤醒 readable）。
//   2) NDJSON：按 '\n' 切行，每行 JSON.parse；解析失败仅 warn 一条，
//      不影响后续行。
//   3) 简易 EventEmitter：只暴露 on/off('event', handler)，加 close/error 回调。
//   4) 自动重连：daemon 重启会导致读 fd 得到 EOF；此时清 buffer、close、
//      3s 后重新 open，保证 web 侧无需重启即可继续订阅。
//
// 线程模型：单进程 Node，全部事件回调都在主 event loop 内串行执行。

import fs from 'node:fs';
import { EventEmitter } from 'node:events';

const POLL_INTERVAL_MS   = 5;
const RECONNECT_DELAY_MS = 3000;

export class EventFifoReader extends EventEmitter {
    /**
     * @param {{ path: string }} opts  path 为空则不启动。
     */
    constructor({ path }) {
        super();
        this.path       = path;
        this.fd         = -1;
        this.buffer     = Buffer.alloc(0);
        this.timer      = null;
        this.stopped    = false;
        this.readBuf    = Buffer.alloc(64 * 1024);   // 单次 read 上限，够 daemon 一批
    }

    async start() {
        if (!this.path) {
            console.warn('[event_fifo] path is empty, disabled');
            return;
        }
        this.stopped = false;
        this._openLoop();
    }

    async stop() {
        this.stopped = true;
        if (this.timer) {
            clearTimeout(this.timer);
            this.timer = null;
        }
        this._closeFd();
    }

    _openLoop() {
        if (this.stopped) return;
        try {
            /* 用 fs.openSync + O_NONBLOCK：daemon 未起时 open 也不阻塞（FIFO
             * 读端在没写端时会得到 ENXIO / 阻塞行为随实现不同，NONBLOCK 下
             * 恒为立即返回）。若 open 失败则 3s 后重试。 */
            this.fd = fs.openSync(this.path, fs.constants.O_RDONLY | fs.constants.O_NONBLOCK);
            console.log(`[event_fifo] opened path='${this.path}' fd=${this.fd}`);
            this._schedule();
        } catch (e) {
            console.warn(`[event_fifo] open('${this.path}') failed: ${e.message}, retry in ${RECONNECT_DELAY_MS}ms`);
            this.timer = setTimeout(() => this._openLoop(), RECONNECT_DELAY_MS);
        }
    }

    _schedule() {
        if (this.stopped) return;
        this.timer = setTimeout(() => this._pollOnce(), POLL_INTERVAL_MS);
    }

    _pollOnce() {
        if (this.stopped) return;
        if (this.fd < 0) { this._openLoop(); return; }

        try {
            const n = fs.readSync(this.fd, this.readBuf, 0, this.readBuf.length, null);
            if (n === 0) {
                /* daemon 关闭了写端。重连以恢复。 */
                console.warn('[event_fifo] EOF from writer, reconnecting');
                this._closeFd();
                this.timer = setTimeout(() => this._openLoop(), RECONNECT_DELAY_MS);
                return;
            }
            if (n > 0) {
                this.buffer = Buffer.concat([this.buffer, this.readBuf.subarray(0, n)]);
                this._drainLines();
            }
        } catch (e) {
            /* NONBLOCK read 无数据时抛 EAGAIN，视作正常轮询空转。 */
            if (e.code !== 'EAGAIN') {
                console.warn(`[event_fifo] read error: ${e.message}, reconnecting`);
                this._closeFd();
                this.timer = setTimeout(() => this._openLoop(), RECONNECT_DELAY_MS);
                return;
            }
        }
        this._schedule();
    }

    _drainLines() {
        while (true) {
            const nl = this.buffer.indexOf(0x0a);
            if (nl < 0) return;
            const line = this.buffer.subarray(0, nl).toString('utf8').trim();
            this.buffer = this.buffer.subarray(nl + 1);
            if (!line) continue;

            let obj;
            try { obj = JSON.parse(line); }
            catch (e) {
                console.warn(`[event_fifo] parse fail: ${e.message} line='${line.slice(0, 200)}'`);
                continue;
            }
            /* 顺带打条 debug 日志便于观察量级；生产可按 kind 过滤或去掉。 */
            this.emit('event', obj);
        }
    }

    _closeFd() {
        if (this.fd >= 0) {
            try { fs.closeSync(this.fd); } catch { /* ignore */ }
            this.fd = -1;
        }
        this.buffer = Buffer.alloc(0);
    }
}
