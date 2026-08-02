/**
 * schedule-worker.js
 * ----------------
 * "สมอง" ของระบบ: Cloudflare เป็นตัวตัดสินใจเรื่องเวลา
 *
 * - POST /config  : แอปส่ง {mode, schedules} มาเก็บใน KV
 * - GET  /status  : ดูสถานะ + ข้อมูลที่เก็บอยู่ (ไว้ debug)
 * - Cron ทุก 1 นาที: อ่านตาราง -> เทียบเวลาปัจจุบัน (UTC+7)
 *                    -> ถ้าสถานะเปลี่ยน ส่ง open/close ไป HiveMQ (MQTT over WebSocket)
 *
 * ESP32 จะยังมี autoCheck ใน NVS เป็น backup เผื่อ Cloudflare หลุด
 */

const CFG_KEY = 'config';
const STATE_KEY = 'state';

/* ===================================================================
 * MQTT v3.1.1 packet builder (เล็กพอรันใน Worker ไม่ต้องใช้ library)
 * =================================================================== */

function encodeRemainingLength(len) {
  const out = [];
  do {
    let digit = len % 128;
    len = Math.floor(len / 128);
    if (len > 0) digit |= 0x80;
    out.push(digit);
  } while (len > 0);
  return out;
}

function toBytes(arr) {
  const chunks = [];
  let total = 0;
  for (const a of arr) {
    const u = a instanceof Uint8Array ? a : new Uint8Array(a);
    chunks.push(u);
    total += u.length;
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const c of chunks) { out.set(c, offset); offset += c.length; }
  return out;
}

function encodeString(str) {
  const buf = new TextEncoder().encode(str);
  const out = new Uint8Array(2 + buf.length);
  out[0] = (buf.length >> 8) & 0xff;
  out[1] = buf.length & 0xff;
  out.set(buf, 2);
  return out;
}

function buildConnect(clientId, username, password) {
  const parts = [
    encodeString('MQTT'),
    [4],                                  // protocol level v3.1.1
    [0xC2],                               // flags: username(0x80) + password(0x40) + clean session(0x02)
    [0x00, 0x1e],                         // keepalive 30s
    encodeString(clientId),
  ];
  if (username) parts.push(encodeString(username));
  if (password) parts.push(encodeString(password));
  const payload = toBytes(parts);
  const header = toBytes([[0x10], encodeRemainingLength(payload.length)]);
  return toBytes([header, payload]);
}

function buildPublish(topic, payload, qos = 0) {
  const body = toBytes([encodeString(topic), new TextEncoder().encode(payload)]);
  const headerByte = 0x30 | (qos << 1);
  const header = toBytes([[headerByte], encodeRemainingLength(body.length)]);
  return toBytes([header, body]);
}

function buildPing() { return new Uint8Array([0xC0, 0x00]); }
function buildDisconnect() { return new Uint8Array([0xE0, 0x00]); }

/* ===================================================================
 * MQTT publish ผ่าน WebSocket -> HiveMQ (wss://host:8084/mqtt)
 * =================================================================== */
async function publishMqtt(topic, message, env) {
  const url = env.MQTT_WS_URL || 'wss://74bd39d864644eebb6efba20dadd4e93.s1.eu.hivemq.cloud:8084/mqtt';
  const clientId = env.MQTT_CLIENT_ID || ('cf_worker_' + Date.now());
  const username = env.MQTT_USER || 'test';
  const password = env.MQTT_PASS || 'test1234';

  return new Promise((resolve) => {
    let ws;
    let settled = false;
    const timeout = setTimeout(() => { if (!settled) { settled = true; try { ws && ws.close(); } catch (e) {} resolve(false); } }, 15000);

    try {
      ws = new WebSocket(url);
    } catch (e) {
      clearTimeout(timeout); resolve(false); return;
    }

    ws.addEventListener('open', () => {
      try { ws.send(buildConnect(clientId, username, password)); } catch (e) {}
    });

    ws.addEventListener('message', (ev) => {
      try {
        let bytes = ev.data;
        if (typeof bytes === 'string') bytes = new TextEncoder().encode(bytes);
        else if (Array.isArray(bytes)) bytes = new Uint8Array(bytes);
        else if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
        else if (ArrayBuffer.isView(bytes)) bytes = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        else return;

        // CONNACK = 0x20
        if (bytes[0] === 0x20) {
          if (bytes[2] !== 0) { // return code != 0 -> reject
            if (!settled) { settled = true; clearTimeout(timeout); try { ws.close(); } catch (e) {} resolve(false); }
            return;
          }
          // ส่ง PUBLISH แล้วปิดการเชื่อมต่อทันที (fire-and-forget)
          try {
            ws.send(buildPublish(topic, message, 0));
            ws.send(buildDisconnect());
          } catch (e) {}
        }
      } catch (e) {}
    });

    ws.addEventListener('close', () => {
      if (!settled) { settled = true; clearTimeout(timeout); resolve(true); }
    });
    ws.addEventListener('error', () => {
      if (!settled) { settled = true; clearTimeout(timeout); try { ws.close(); } catch (e) {} resolve(false); }
    });
  });
}

/* ===================================================================
 * การคำนวณตารางเวลา (UTC+7, จันทร์=0 ... อาทิตย์=6 ตรงกับแอป)
 * =================================================================== */
function thaiMinutes(now) {
  const totalMin = Math.floor(now.getTime() / 60000) + 7 * 60;
  const minutesInDay = ((totalMin % 1440) + 1440) % 1440;
  // 1970-01-01 (epoch day 0) เป็นวันพฤหัสบดี -> index 3 ในระบบ จ=0
  const dayCount = Math.floor(totalMin / 1440);
  const dayOfWeek = ((dayCount + 3) % 7 + 7) % 7; // จันทร์=0 ... อาทิตย์=6
  return { minutesInDay, dayOfWeek };
}

function timeToMin(hhmm) {
  const parts = String(hhmm || '').split(':');
  if (parts.length < 2) return -1;
  return parseInt(parts[0], 10) * 60 + parseInt(parts[1], 10);
}

function scheduleActiveNow(schedules, now) {
  if (!Array.isArray(schedules) || schedules.length === 0) return false;
  const { minutesInDay, dayOfWeek } = thaiMinutes(now);

  for (const sch of schedules) {
    if (!sch.enabled) continue;
    const days = sch.days || [];
    if (!days[dayOfWeek]) continue;
    const startMin = timeToMin(sch.start);
    const endMin = timeToMin(sch.end);
    if (startMin < 0 || endMin < 0) continue;
    if (startMin <= endMin) {
      if (minutesInDay >= startMin && minutesInDay < endMin) return true;
    } else {
      // ข้ามเที่ยงคืน เช่น 22:00 - 06:00
      if (minutesInDay >= startMin || minutesInDay < endMin) return true;
    }
  }
  return false;
}

/* ===================================================================
 * Cron handler: ตรวจตาราง -> publish เมื่อสถานะเปลี่ยน
 * =================================================================== */
async function runCron(env) {
  const cfg = await env.SCHEDULE_KV.get(CFG_KEY, 'json');
  if (!cfg || cfg.mode !== 'auto') {
    return new Response('skip (no config or manual)', { status: 200 });
  }

  const now = new Date();
  const shouldOpen = scheduleActiveNow(cfg.schedules, now);

  const state = await env.SCHEDULE_KV.get(STATE_KEY, 'json');
  const prevOpen = state ? state.shouldOpen : null;

  // ครั้งแรกหรือสถานะเปลี่ยน -> publish
  if (prevOpen !== shouldOpen) {
    const topic = 'myhouse/valve/control';
    const payload = shouldOpen ? '{"cmd":"open"}' : '{"cmd":"close"}';
    const ok = await publishMqtt(topic, payload, env);
    await env.SCHEDULE_KV.put(STATE_KEY, JSON.stringify({
      shouldOpen,
      at: now.toISOString(),
      published: ok,
    }));
    return new Response(JSON.stringify({ action: shouldOpen ? 'open' : 'close', published: ok, time: now.toISOString() }), {
      headers: { 'content-type': 'application/json' },
    });
  }

  return new Response(JSON.stringify({ noChange: true, shouldOpen, time: now.toISOString() }), {
    headers: { 'content-type': 'application/json' },
  });
}

/* ===================================================================
 * HTTP handler
 * =================================================================== */
async function handleRequest(request, env) {
  const url = new URL(request.url);

  if (request.method === 'POST' && url.pathname === '/config') {
    try {
      const body = await request.json();
      if (body && typeof body === 'object') {
        // เก็บเฉพาะ mode + schedules (บังคับให้ schedules เป็น array)
        const cfg = {
          mode: body.mode === 'auto' ? 'auto' : 'manual',
          schedules: Array.isArray(body.schedules) ? body.schedules : [],
          updatedAt: new Date().toISOString(),
        };
        await env.SCHEDULE_KV.put(CFG_KEY, JSON.stringify(cfg));
        return new Response(JSON.stringify({ ok: true, stored: cfg }), { headers: { 'content-type': 'application/json' } });
      }
      return new Response('{"ok":false,"error":"bad body"}', { status: 400, headers: { 'content-type': 'application/json' } });
    } catch (e) {
      return new Response(JSON.stringify({ ok: false, error: e.message }), { status: 400, headers: { 'content-type': 'application/json' } });
    }
  }

  if (request.method === 'GET') {
    const cfg = await env.SCHEDULE_KV.get(CFG_KEY, 'json');
    const state = await env.SCHEDULE_KV.get(STATE_KEY, 'json');
    const { minutesInDay, dayOfWeek } = thaiMinutes(new Date());
    return new Response(JSON.stringify({ ok: true, nowMin: minutesInDay, dayOfWeek, config: cfg, state }), {
      headers: { 'content-type': 'application/json' },
    });
  }

  return new Response('not found', { status: 404 });
}

/* ===================================================================
 * Entry
 * =================================================================== */
export default {
  async fetch(request, env) {
    return handleRequest(request, env);
  },
  async scheduled(event, env, ctx) {
    ctx.waitUntil(runCron(env));
  },
};
