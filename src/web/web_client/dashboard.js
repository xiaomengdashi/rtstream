// dashboard.js —— 指标渲染与曲线图（ImGui 风格暗色面板）

const Dashboard = (() => {
  const $ = (id) => document.getElementById(id);

  const latency = [];       // e2e ms 序列（最多 300）
  const fpsHistory = [];    // 每秒客户端 fps（最多 60）
  let framesThisSecond = 0;
  let lastSecondTs = performance.now();
  let lastServerJson = null;

  function fmtBytes(n) {
    if (n > 1024 * 1024 * 1024) return (n / 1024 / 1024 / 1024).toFixed(2) + ' GiB';
    if (n > 1024 * 1024) return (n / 1024 / 1024).toFixed(2) + ' MiB';
    if (n > 1024) return (n / 1024).toFixed(1) + ' KiB';
    return n + ' B';
  }

  function percentile(arr, p) {
    if (!arr.length) return 0;
    const s = [...arr].sort((a, b) => a - b);
    const i = Math.min(s.length - 1, Math.floor(p * (s.length - 1)));
    return s[i];
  }

  function set(id, text) { const el = $(id); if (el) el.textContent = text; }

  function renderServer(json) {
    const S = (id, v) => set('m-' + id, typeof v === 'number' && !Number.isInteger(v) ? v.toFixed(1) : String(v));
    S('capture_frames', json.capture_frames);
    S('encoded_frames', json.encoded_frames);
    // 码率：用两次快照差分计算
    const now = performance.now();
    if (lastServerJson) {
      const dt = (now - lastServerJson._t) / 1000;
      const dBytes = json.net_bytes_sent - lastServerJson.net_bytes_sent;
      if (dt > 0 && dBytes >= 0) set('m-send_bitrate', Math.round(dBytes * 8 / dt / 1000) + ' kbps');
    }
    lastServerJson = { ...json, _t: now };
    S('net_packets_sent', json.net_packets_sent);
    S('net_bytes_sent', fmtBytes(json.net_bytes_sent));
    S('net_fec_sent', json.net_fec_sent);
    S('sim_dropped', json.sim_dropped);
    S('nack_received', json.nack_received);
    S('retransmit_sent', json.retransmit_sent);
    S('net_packets_recv', json.net_packets_recv);
    S('net_lost', json.net_lost);
    S('fec_recovered', json.fec_recovered);
    S('nack_hit', json.nack_hit);
    S('rtt', json.rtt_ms ? json.rtt_ms.toFixed(1) + ' ms' : '- ms');
    S('jitter', json.jitter_ms ? json.jitter_ms.toFixed(1) + ' ms' : '- ms');
    S('lat_p50', json.lat_p50 ? json.lat_p50.toFixed(1) + ' ms' : '- ms');
    S('lat_p95', json.lat_p95 ? json.lat_p95.toFixed(1) + ' ms' : '- ms');
    S('lat_max', json.lat_max ? json.lat_max.toFixed(1) + ' ms' : '- ms');
  }

  function addLatency(ms) {
    latency.push(ms);
    if (latency.length > 300) latency.shift();
    set('m-lat_p50', percentile(latency, 0.5).toFixed(1) + ' ms');
    set('m-lat_p95', percentile(latency, 0.95).toFixed(1) + ' ms');
    set('m-lat_max', Math.max(...latency).toFixed(1) + ' ms');
  }

  function tickSecond() {
    const now = performance.now();
    if (now - lastSecondTs >= 1000) {
      const fps = framesThisSecond * 1000 / (now - lastSecondTs);
      fpsHistory.push(fps);
      if (fpsHistory.length > 60) fpsHistory.shift();
      set('m-local_fps', fps.toFixed(1));
      framesThisSecond = 0;
      lastSecondTs = now;
    }
    requestAnimationFrame(tickSecond);
  }

  // ---- 曲线绘制 ----
  function drawChart(canvas, series, color, opts = {}) {
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = '#1f232b';
    ctx.fillRect(0, 0, w, h);
    if (!series.length) return;
    const max = Math.max(...series, opts.minMax || 1);
    // 网格
    ctx.strokeStyle = '#2b313c';
    ctx.lineWidth = 1;
    for (let i = 1; i < 4; i++) {
      const y = (h / 4) * i;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    series.forEach((v, i) => {
      const x = (i / (series.length - 1 || 1)) * (w - 8) + 4;
      const y = h - 6 - (v / max) * (h - 12);
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
    // 峰值标注
    ctx.fillStyle = '#8a94a6';
    ctx.font = '10px monospace';
    ctx.fillText(max.toFixed(opts.unit ? 0 : 1) + (opts.unit || ''), 6, 12);
  }

  function renderCharts() {
    drawChart($('latency-chart'), latency, '#4fa3ff', { unit: 'ms' });
    drawChart($('fps-chart'), fpsHistory, '#67d17c', { unit: ' fps' });
    requestAnimationFrame(renderCharts);
  }

  function badge(on) {
    const el = $('conn-badge');
    el.textContent = on ? '已连接' : '未连接';
    el.className = 'badge ' + (on ? 'on' : 'off');
  }

  requestAnimationFrame(tickSecond);
  requestAnimationFrame(renderCharts);

  return { addLatency, renderServer, badge, countFrame: () => framesThisSecond++, set };
})();
