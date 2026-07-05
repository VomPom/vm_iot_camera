// server.mjs
// ---------------------------------------------------------------------------
// vm_iot Web 控制台入口（Fastify + @fastify/websocket + @fastify/static）。
//
// 依赖关系：
//   - 不修改 daemon 任何代码；所有控制走 /tmp/vm_iot.ctl + /tmp/vm_iot.reply。
//   - WebRTC/HLS 实时画面由 mediamtx 独立服务承担；本进程只负责 REST/WS/静态。
//
// 默认参数（可通过环境变量覆盖）：
//   IOTCAM_CTL          /tmp/vm_iot.ctl
//   IOTCAM_REPLY        /tmp/vm_iot.reply
//   IOTCAM_EVENTS       /tmp/vm_iot.events   事件广播 FIFO（NDJSON），空则不启用
//   VM_IOT_WEB_HOST     0.0.0.0
//   VM_IOT_WEB_PORT     8080
//   MEDIAMTX_HOST       同请求 host（前端自动从 location.hostname 取）
//   MEDIAMTX_WHEP_PORT  8889
//   MEDIAMTX_HLS_PORT   8888
//   MEDIAMTX_PATH       live   （对应 RTSP mount 名）
// ---------------------------------------------------------------------------

import Fastify from 'fastify';
import fastifyStatic from '@fastify/static';
import fastifyWebsocket from '@fastify/websocket';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { FifoClient } from './fifo.mjs';
import { EventFifoReader } from './event_fifo.mjs';
import { registerRoutes } from './routes.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const CFG = {
  ctlPath:    process.env.IOTCAM_CTL    || '/tmp/vm_iot.ctl',
  replyPath:  process.env.IOTCAM_REPLY  || '/tmp/vm_iot.reply',
  eventsPath: process.env.IOTCAM_EVENTS || '/tmp/vm_iot.events',
  host:       process.env.VM_IOT_WEB_HOST || '0.0.0.0',
  port:       Number(process.env.VM_IOT_WEB_PORT || 8080),
  // 这些只是 /api/config 把出去给前端的默认推荐值，前端可以基于 location.hostname 覆盖
  media: {
    whepPort: Number(process.env.MEDIAMTX_WHEP_PORT || 8889),
    hlsPort:  Number(process.env.MEDIAMTX_HLS_PORT  || 8888),
    streamPath: process.env.MEDIAMTX_PATH || 'live',
  },
};

async function main() {
  const app = Fastify({
    logger: { level: process.env.LOG_LEVEL || 'info' },
    // 控制台是单页 + 少量 REST，body 不会大
    bodyLimit: 64 * 1024,
  });

  await app.register(fastifyWebsocket);
  await app.register(fastifyStatic, {
    root: path.join(__dirname, 'static'),
    prefix: '/',
    index: 'index.html',
  });

  // 暴露给前端的运行期参数
  app.get('/api/config', async () => ({
    ok: true,
    media: CFG.media,
  }));

  const fifo = new FifoClient({
    ctlPath:   CFG.ctlPath,
    replyPath: CFG.replyPath,
    timeoutMs: 2000,
  });

  try {
    await fifo.start();
    app.log.info({ ctl: CFG.ctlPath, reply: CFG.replyPath }, 'fifo client started');
  } catch (e) {
    app.log.error({ err: e.message }, 'fifo client failed to start; daemon is probably down');
    // 不退出：仍然把页面服起来，前端会显示 daemon offline，便于排查
  }

  /* 事件广播读端（daemon → web，单向）：
   * 仅当 IOTCAM_EVENTS 非空时启动；start() 内部已处理 open 失败重试，
   * 因此不需要 try/catch（与 fifo 风格一致：失败不阻止页面启动）。 */
  const events = new EventFifoReader({ path: CFG.eventsPath });
  if (CFG.eventsPath) {
    await events.start();
    app.log.info({ path: CFG.eventsPath }, 'event fifo reader started');
  }

  await registerRoutes(app, { fifo, events });

  // 优雅关闭
  const shutdown = async (sig) => {
    app.log.info({ sig }, 'shutting down');
    try { await events.stop(); } catch { /* ignore */ }
    try { await fifo.stop(); } catch { /* ignore */ }
    try { await app.close(); } catch { /* ignore */ }
    process.exit(0);
  };
  process.on('SIGINT',  () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));

  await app.listen({ host: CFG.host, port: CFG.port });
  app.log.info(`vm_iot web console listening on http://${CFG.host}:${CFG.port}`);
}

main().catch(err => {
  console.error('fatal:', err);
  process.exit(1);
});
