// main.js —— WebSocket 连接、JPEG 帧渲染、本地延时统计
//
// 服务端二进制帧布局（大端）：
//   magic 'RTVJ'(4) | wall_ms double(8) | width u32(4) | height u32(4) | jpeg...
// E2E 延时（同机回环可信）：Date.now() - wall_ms

(() => {
  const videoCanvas = document.getElementById('video');
  const vctx = videoCanvas.getContext('2d');
  const frameInfo = document.getElementById('frame-info');

  const wsProto = location.protocol === 'https:' ? 'wss' : 'ws';
  const wsUrl = `${wsProto}://${location.host}/ws`;
  let ws = null;
  let retryMs = 1000;
  let frameCount = 0;

  function connect() {
    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { Dashboard.badge(true); retryMs = 1000; };
    ws.onclose = () => {
      Dashboard.badge(false);
      setTimeout(connect, retryMs);
      retryMs = Math.min(retryMs * 2, 8000);
    };
    ws.onerror = () => ws.close();
    ws.onmessage = onMessage;
  }

  let lastPing = 0;
  function pingLoop() {
    if (ws && ws.readyState === 1) {
      lastPing = performance.now();
      ws.send('ping ' + lastPing);
    }
    setTimeout(pingLoop, 1000);
  }

  function onMessage(ev) {
    if (typeof ev.data === 'string') {
      if (ev.data.startsWith('pong ')) {
        const rtt = performance.now() - parseFloat(ev.data.slice(5));
        Dashboard.set('m-ws_rtt', rtt.toFixed(1) + ' ms');
        return;
      }
      try {
        Dashboard.renderServer(JSON.parse(ev.data));
      } catch (e) { /* 忽略非 JSON */ }
      return;
    }
    const buf = new DataView(ev.data);
    if (buf.byteLength < 20) return;
    if (buf.getUint8(0) !== 0x52 || buf.getUint8(1) !== 0x54 ||
        buf.getUint8(2) !== 0x56 || buf.getUint8(3) !== 0x4A) return;

    const wallMs = buf.getFloat64(4, false);   // big-endian double
    const width = buf.getUint32(12, false);
    const height = buf.getUint32(16, false);
    const jpeg = new Uint8Array(ev.data, 20);

    const blob = new Blob([jpeg], { type: 'image/jpeg' });
    createImageBitmap(blob).then((bmp) => {
      if (videoCanvas.width !== width) videoCanvas.width = width;
      if (videoCanvas.height !== height) videoCanvas.height = height;
      vctx.drawImage(bmp, 0, 0, width, height);
      bmp.close && bmp.close();

      const e2e = Date.now() - wallMs;   // 同机回环：墙钟一致，差值即 E2E
      if (e2e >= 0 && e2e < 10000) Dashboard.addLatency(e2e);
      Dashboard.countFrame();
      frameInfo.textContent = `frame: ${++frameCount}  ${width}x${height}  e2e≈${e2e.toFixed(0)}ms`;
    }).catch(() => {});
  }

  Dashboard.badge(false);
  connect();
  pingLoop();
})();
