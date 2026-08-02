/**
 * schedule-worker.js
 * ----------------
 * "แหล่งความจริง" ของตารางเวลา
 *
 * - POST /config  : แอปส่ง {mode, schedules} มาเก็บใน KV
 * - GET  /        : ESP32 ดึง config {mode, schedules} ไป execute เองในเครื่อง
 * - Cron ทุก 1 นาที: อ่านตาราง -> เทียบเวลาปัจจุบัน (UTC+7) -> บันทึก state ไว้ debug
 *
 * ESP32 ดึง config ผ่าน HTTPS ทุก 60 วิ แล้วรันตารางเองในเครื่อง
 * (Worker -> HiveMQ ถูก Cloudflare บล็อกพอร์ต 8884 จึงไม่ publish MQTT)
 */

const CFG_KEY = 'config';
const STATE_KEY = 'state';

// CORS: แอปวิ่งบน pages.dev ต้อง POST ข้าม origin มาที่ Worker ได้
const CORS_HEADERS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

function jsonResponse(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { 'content-type': 'application/json', ...CORS_HEADERS },
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
 * Cron handler: ตรวจตาราง -> บันทึกสถานะที่ควรเป็น (ESP32 ดึง config ไป execute เอง)
 * =================================================================== */
async function runCron(env) {
  const cfg = await env.SCHEDULE_KV.get(CFG_KEY, 'json');
  if (!cfg || cfg.mode !== 'auto') {
    return new Response('skip (no config or manual)', { status: 200 });
  }

  const now = new Date();
  const shouldOpen = scheduleActiveNow(cfg.schedules, now);
  await env.SCHEDULE_KV.put(STATE_KEY, JSON.stringify({
    shouldOpen,
    at: now.toISOString(),
  }));

  return new Response(JSON.stringify({ shouldOpen, time: now.toISOString() }), {
    headers: { 'content-type': 'application/json' },
  });
}

/* ===================================================================
 * HTTP handler
 * =================================================================== */
async function handleRequest(request, env) {
  const url = new URL(request.url);

  // OPTIONS preflight (POST + Content-Type: application/json ข้าม origin)
  if (request.method === 'OPTIONS') {
    return new Response(null, { status: 204, headers: CORS_HEADERS });
  }

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
        return jsonResponse({ ok: true, stored: cfg });
      }
      return jsonResponse({ ok: false, error: 'bad body' }, 400);
    } catch (e) {
      return jsonResponse({ ok: false, error: e.message }, 400);
    }
  }

  if (request.method === 'GET') {
    const cfg = await env.SCHEDULE_KV.get(CFG_KEY, 'json');
    const state = await env.SCHEDULE_KV.get(STATE_KEY, 'json');
    const { minutesInDay, dayOfWeek } = thaiMinutes(new Date());
    return jsonResponse({ ok: true, nowMin: minutesInDay, dayOfWeek, config: cfg, state });
  }

  return jsonResponse({ ok: false, error: 'not found' }, 404);
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
