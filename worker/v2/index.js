/**
 * Water Valve API v2 — Cloudflare Worker
 * --------------------------------------
 * Central config + auth + commands + device sync.
 *
 * Bindings:
 *   DB           D1 database (see migrations/0001_init.sql)
 *   SETUP_TOKEN  secret used once by POST /v1/setup
 *   ALLOWED_ORIGINS comma-separated browser origins (optional)
 *
 * Design notes:
 * - Config is the source of truth; every write increments `revision`.
 * - Commands are short-lived (20 min, covering the device sync interval).
 * - ESP32 uses MQTT for instant control/status and this API for config sync.
 * - Passwords: PBKDF2-SHA256 (100k iterations) via WebCrypto.
 */

const PBKDF2_ITERATIONS = 100000;
const SESSION_DAYS = 30;
const COMMAND_TTL_SECONDS = 20 * 60; // must exceed the device's 15-minute sync interval
const AUTH_WINDOW_SECONDS = 60;
const AUTH_MAX_FAILURES = 5;
// The device only checks in over HTTPS every 15 minutes; a 5-minute window
// would label a healthy device offline most of the time.
const DEVICE_ONLINE_WINDOW_MS = 20 * 60 * 1000;

const DEFAULT_SCHEDULES = [
  {
    id: 1,
    name: 'รอบเช้า',
    start: '06:00',
    end: '06:30',
    days: [true, true, true, true, true, false, false],
    enabled: true,
  },
  {
    id: 2,
    name: 'รอบเย็น',
    start: '17:30',
    end: '18:00',
    days: [true, true, true, true, true, true, true],
    enabled: true,
  },
  {
    id: 3,
    name: 'รอบบ่าย',
    start: '12:00',
    end: '12:15',
    days: [false, false, true, true, true, false, false],
    enabled: false,
  },
];

/* ===================================================================
 * Small utilities
 * =================================================================== */

function nowIso() {
  return new Date().toISOString();
}

function addSecondsIso(seconds) {
  return new Date(Date.now() + seconds * 1000).toISOString();
}

function addDaysIso(days) {
  return new Date(Date.now() + days * 24 * 60 * 60 * 1000).toISOString();
}

function uuid() {
  return crypto.randomUUID();
}

function toB64(bytes) {
  let binary = '';
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
}

function fromB64(value) {
  const binary = atob(value);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
  return out;
}

function toB64Url(bytes) {
  return toB64(bytes).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');
}

function toHex(bytes) {
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
}

async function sha256Hex(value) {
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(value));
  return toHex(new Uint8Array(digest));
}

function timingSafeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i += 1) diff |= a[i] ^ b[i];
  return diff === 0;
}

function safeJson(value, fallback) {
  if (!value) return fallback;
  try {
    return JSON.parse(value);
  } catch {
    return fallback;
  }
}

function defaultSchedules() {
  return DEFAULT_SCHEDULES.map((schedule) => ({
    ...schedule,
    days: [...schedule.days],
  }));
}

function isHHMM(value) {
  return typeof value === 'string' && /^([01]\d|2[0-3]):[0-5]\d$/.test(value);
}

// Tolerate weak validators and clients that strip the surrounding quotes.
function normalizeEtag(value) {
  if (!value) return '';
  return String(value).trim().replace(/^W\//i, '').replace(/^"(.*)"$/, '$1');
}

function validateConfig(body) {
  if (!body || typeof body !== 'object') return 'Body must be an object';
  if (body.mode !== 'auto' && body.mode !== 'manual') return 'mode must be "auto" or "manual"';
  if (!Array.isArray(body.schedules)) return 'schedules must be an array';
  if (body.schedules.length > 50) return 'Too many schedules (max 50)';

  const ids = new Set();
  for (const schedule of body.schedules) {
    if (!schedule || typeof schedule !== 'object') return 'Each schedule must be an object';
    if (!Number.isInteger(schedule.id) || schedule.id < 1) return 'schedule.id must be a positive integer';
    if (ids.has(schedule.id)) return 'schedule.id must be unique';
    ids.add(schedule.id);
    if (typeof schedule.name !== 'string' || schedule.name.length > 100) return 'schedule.name is invalid';
    if (!isHHMM(schedule.start) || !isHHMM(schedule.end)) return 'schedule.start/end must be HH:MM';
    if (!Array.isArray(schedule.days) || schedule.days.length !== 7 || !schedule.days.every((d) => typeof d === 'boolean')) {
      return 'schedule.days must be 7 booleans';
    }
    if (typeof schedule.enabled !== 'boolean') return 'schedule.enabled must be a boolean';
  }
  return null;
}

function normalizeInviteCode(value) {
  const raw = String(value || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
  if (raw.length !== 8) return '';
  return `${raw.slice(0, 4)}-${raw.slice(4)}`;
}

function generateInviteCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  const bytes = crypto.getRandomValues(new Uint8Array(8));
  let raw = '';
  for (const byte of bytes) raw += alphabet[byte % alphabet.length];
  return `${raw.slice(0, 4)}-${raw.slice(4)}`;
}

async function uniqueInviteCode(env) {
  for (let i = 0; i < 10; i += 1) {
    const code = generateInviteCode();
    const existing = await env.DB.prepare('SELECT id FROM households WHERE invite_code = ?').bind(code).first();
    if (!existing) return code;
  }
  throw new Error('Unable to generate a unique invite code');
}

/* ===================================================================
 * Password hashing (PBKDF2-SHA256)
 * =================================================================== */

async function derivePassword(password, salt, iterations, length) {
  const key = await crypto.subtle.importKey(
    'raw',
    new TextEncoder().encode(password),
    'PBKDF2',
    false,
    ['deriveBits'],
  );
  return crypto.subtle.deriveBits(
    { name: 'PBKDF2', salt, iterations, hash: 'SHA-256' },
    key,
    length * 8,
  );
}

async function hashPassword(password) {
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const bits = await derivePassword(password, salt, PBKDF2_ITERATIONS, 32);
  return `pbkdf2$${PBKDF2_ITERATIONS}$${toB64(salt)}$${toB64(new Uint8Array(bits))}`;
}

async function verifyPassword(password, stored) {
  const parts = String(stored || '').split('$');
  if (parts.length !== 4 || parts[0] !== 'pbkdf2') return false;
  const iterations = Number(parts[1]);
  if (!Number.isInteger(iterations) || iterations < 1) return false;
  const salt = fromB64(parts[2]);
  const expected = fromB64(parts[3]);
  const bits = await derivePassword(password, salt, iterations, expected.length);
  return timingSafeEqual(new Uint8Array(bits), expected);
}

/* ===================================================================
 * Sessions / auth
 * =================================================================== */

function parseBearer(request) {
  const header = request.headers.get('Authorization') || '';
  const match = header.match(/^Bearer\s+(.+)$/i);
  return match ? match[1].trim() : null;
}

async function createSession(env, memberId) {
  const token = `sess_${toB64Url(crypto.getRandomValues(new Uint8Array(32)))}`;
  const tokenHash = await sha256Hex(token);
  const id = uuid();
  const createdAt = nowIso();
  const expiresAt = addDaysIso(SESSION_DAYS);
  await env.DB.prepare(
    `INSERT INTO sessions (id, member_id, token_hash, created_at, expires_at, revoked_at)
     VALUES (?, ?, ?, ?, ?, NULL)`,
  ).bind(id, memberId, tokenHash, createdAt, expiresAt).run();
  return { id, token, expiresAt };
}

async function getSession(request, env) {
  const token = parseBearer(request);
  if (!token) return null;
  const tokenHash = await sha256Hex(token);
  const row = await env.DB.prepare(
    `SELECT s.id AS session_id, s.expires_at, s.revoked_at,
            m.id AS member_id, m.display_name, m.household_id,
            h.name AS household_name, h.timezone
     FROM sessions s
     JOIN members m ON m.id = s.member_id
     JOIN households h ON h.id = m.household_id
     WHERE s.token_hash = ?`,
  ).bind(tokenHash).first();
  if (!row || row.revoked_at) return null;
  if (new Date(row.expires_at).getTime() <= Date.now()) return null;
  return row;
}

async function getDevice(request, env) {
  const token = parseBearer(request);
  if (!token) return null;
  const tokenHash = await sha256Hex(token);
  const row = await env.DB.prepare(
    `SELECT d.*, h.timezone
     FROM devices d
     JOIN households h ON h.id = d.household_id
     WHERE d.token_hash = ?`,
  ).bind(tokenHash).first();
  return row || null;
}

async function audit(env, householdId, actorType, actorId, action, detail) {
  try {
    await env.DB.prepare(
      `INSERT INTO audit_log (id, household_id, actor_type, actor_id, action, detail_json, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?)`,
    ).bind(
      uuid(),
      householdId,
      actorType,
      actorId || null,
      action,
      detail === undefined ? null : JSON.stringify(detail),
      nowIso(),
    ).run();
  } catch (error) {
    console.error('audit failed', error);
  }
}

/* ===================================================================
 * Failed-auth throttling
 * =================================================================== */

function authAttemptKey(request, scope) {
  const ip = request.headers.get('cf-connecting-ip');
  return ip ? `${scope}:${ip}` : null;
}

async function isAuthRateLimited(env, request, scope) {
  const key = authAttemptKey(request, scope);
  if (!key) return { limited: false, key: null, retryAfter: 0 };

  const row = await env.DB.prepare(
    'SELECT window_start, failures FROM auth_attempts WHERE key = ?',
  ).bind(key).first();

  const nowSeconds = Math.floor(Date.now() / 1000);
  const elapsed = row ? nowSeconds - row.window_start : AUTH_WINDOW_SECONDS;
  if (row && elapsed < AUTH_WINDOW_SECONDS && row.failures >= AUTH_MAX_FAILURES) {
    return { limited: true, key, retryAfter: AUTH_WINDOW_SECONDS - elapsed };
  }
  return { limited: false, key, retryAfter: 0 };
}

async function recordAuthFailure(env, key) {
  if (!key) return;
  const nowSeconds = Math.floor(Date.now() / 1000);

  // Reset the counter when the previous window has elapsed, otherwise increment.
  await env.DB.prepare(
    `INSERT INTO auth_attempts (key, window_start, failures) VALUES (?, ?, 1)
     ON CONFLICT(key) DO UPDATE SET
       failures = CASE WHEN ? - window_start >= ? THEN 1 ELSE failures + 1 END,
       window_start = CASE WHEN ? - window_start >= ? THEN ? ELSE window_start END`,
  ).bind(
    key, nowSeconds,
    nowSeconds, AUTH_WINDOW_SECONDS,
    nowSeconds, AUTH_WINDOW_SECONDS, nowSeconds,
  ).run();

  // Opportunistic cleanup keeps the table small without a cron trigger.
  if (Math.random() < 0.05) {
    await env.DB.prepare('DELETE FROM auth_attempts WHERE window_start < ?')
      .bind(nowSeconds - 3600).run();
  }
}

async function clearAuthFailures(env, key) {
  if (!key) return;
  await env.DB.prepare('DELETE FROM auth_attempts WHERE key = ?').bind(key).run();
}

/* ===================================================================
 * Responses / CORS
 * =================================================================== */

function jsonResponse(body, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'content-type': 'application/json; charset=utf-8',
      ...extraHeaders,
    },
  });
}

function ok(data, status = 200, extraHeaders = {}) {
  return jsonResponse({ ok: true, data }, status, extraHeaders);
}

function fail(code, message, status = 400, details) {
  const error = { code, message };
  if (details !== undefined) error.details = details;
  return jsonResponse({ ok: false, error }, status);
}

function corsHeaders(env, request) {
  const origin = request.headers.get('Origin');
  const allowed = String(env.ALLOWED_ORIGINS || '')
    .split(',')
    .map((value) => value.trim())
    .filter(Boolean);

  const headers = {
    'Access-Control-Allow-Methods': 'GET, POST, PUT, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type, Authorization, If-Match, X-Setup-Token',
    'Access-Control-Max-Age': '86400',
    Vary: 'Origin',
  };

  if (origin && (allowed.includes(origin) || allowed.includes('*'))) {
    headers['Access-Control-Allow-Origin'] = allowed.includes('*') ? '*' : origin;
  }
  return headers;
}

function withCors(response, env, request) {
  const headers = new Headers(response.headers);
  for (const [key, value] of Object.entries(corsHeaders(env, request))) {
    headers.set(key, value);
  }
  return new Response(response.body, { status: response.status, headers });
}

async function readJson(request) {
  try {
    return await request.json();
  } catch {
    return null;
  }
}

/* ===================================================================
 * Handlers
 * =================================================================== */

async function handleSetup(request, env) {
  if (!env.SETUP_TOKEN) return fail('NOT_FOUND', 'Not found', 404);
  if (request.headers.get('X-Setup-Token') !== env.SETUP_TOKEN) {
    return fail('UNAUTHORIZED', 'Invalid setup token', 401);
  }

  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);

  const householdName = String(body.householdName || '').trim();
  const password = String(body.password || '');
  const timezone = String(body.timezone || 'Asia/Bangkok').trim();
  const deviceName = String(body.deviceName || 'วาล์วน้ำ').trim();

  if (!householdName || householdName.length > 100) {
    return fail('VALIDATION', 'householdName is required (max 100 chars)', 422);
  }
  if (password.length < 8) {
    return fail('VALIDATION', 'password must be at least 8 characters', 422);
  }

  const householdId = uuid();
  const memberId = uuid();
  const deviceId = uuid();
  const inviteCode = await uniqueInviteCode(env);
  const passwordHash = await hashPassword(password);
  const deviceToken = `dev_${toB64Url(crypto.getRandomValues(new Uint8Array(32)))}`;
  const deviceHash = await sha256Hex(deviceToken);
  const createdAt = nowIso();
  const schedules = defaultSchedules();

  await env.DB.batch([
    env.DB.prepare(
      `INSERT INTO households (id, name, timezone, invite_code, password_hash, created_at)
       VALUES (?, ?, ?, ?, ?, ?)`,
    ).bind(householdId, householdName, timezone, inviteCode, passwordHash, createdAt),
    env.DB.prepare(
      `INSERT INTO members (id, household_id, display_name, created_at)
       VALUES (?, ?, ?, ?)`,
    ).bind(memberId, householdId, 'owner', createdAt),
    env.DB.prepare(
      `INSERT INTO devices (id, household_id, name, token_hash, firmware_version, last_seen_at, last_status_json, created_at)
       VALUES (?, ?, ?, ?, NULL, NULL, NULL, ?)`,
    ).bind(deviceId, householdId, deviceName, deviceHash, createdAt),
    env.DB.prepare(
      `INSERT INTO configs (household_id, mode, schedules_json, revision, updated_at, updated_by)
       VALUES (?, 'manual', ?, 1, ?, ?)`,
    ).bind(householdId, JSON.stringify(schedules), createdAt, memberId),
    env.DB.prepare(
      `INSERT INTO audit_log (id, household_id, actor_type, actor_id, action, detail_json, created_at)
       VALUES (?, ?, 'system', NULL, 'setup', ?, ?)`,
    ).bind(uuid(), householdId, JSON.stringify({ deviceId }), createdAt),
  ]);

  return ok({ householdId, inviteCode, deviceId, deviceToken, memberId }, 201);
}

async function handleJoin(request, env) {
  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);

  const inviteCode = normalizeInviteCode(body.inviteCode);
  const password = String(body.password || '');
  const displayName = String(body.displayName || '').trim();

  if (!inviteCode || !password || !displayName) {
    return fail('VALIDATION', 'inviteCode, password and displayName are required', 422);
  }
  if (displayName.length > 50) {
    return fail('VALIDATION', 'displayName must be at most 50 characters', 422);
  }

  const rate = await isAuthRateLimited(env, request, 'login');
  if (rate.limited) {
    return fail('RATE_LIMITED', 'Too many failed attempts, try again shortly', 429);
  }

  const household = await env.DB.prepare('SELECT * FROM households WHERE invite_code = ?').bind(inviteCode).first();
  if (!household) {
    await recordAuthFailure(env, rate.key);
    return fail('UNAUTHORIZED', 'Invalid invite code or password', 401);
  }

  const valid = await verifyPassword(password, household.password_hash);
  if (!valid) {
    await recordAuthFailure(env, rate.key);
    return fail('UNAUTHORIZED', 'Invalid invite code or password', 401);
  }
  await clearAuthFailures(env, rate.key);

  const memberId = uuid();
  const createdAt = nowIso();
  await env.DB.prepare(
    `INSERT INTO members (id, household_id, display_name, created_at)
     VALUES (?, ?, ?, ?)`,
  ).bind(memberId, household.id, displayName, createdAt).run();

  const session = await createSession(env, memberId);
  await audit(env, household.id, 'member', memberId, 'member.join', { displayName });

  return ok({
    token: session.token,
    expiresAt: session.expiresAt,
    member: { id: memberId, displayName },
    household: { id: household.id, name: household.name, timezone: household.timezone },
  }, 201);
}

async function handleLogin(request, env) {
  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);

  const inviteCode = normalizeInviteCode(body.inviteCode);
  const password = String(body.password || '');
  if (!inviteCode || !password) {
    return fail('VALIDATION', 'inviteCode and password are required', 422);
  }

  const rate = await isAuthRateLimited(env, request, 'login');
  if (rate.limited) {
    return fail('RATE_LIMITED', 'Too many failed attempts, try again shortly', 429);
  }

  const household = await env.DB.prepare('SELECT * FROM households WHERE invite_code = ?').bind(inviteCode).first();
  if (!household) {
    await recordAuthFailure(env, rate.key);
    return fail('UNAUTHORIZED', 'Invalid invite code or password', 401);
  }

  const valid = await verifyPassword(password, household.password_hash);
  if (!valid) {
    await recordAuthFailure(env, rate.key);
    return fail('UNAUTHORIZED', 'Invalid invite code or password', 401);
  }
  await clearAuthFailures(env, rate.key);

  let memberId = body.memberId ? String(body.memberId) : null;
  let member = null;
  if (memberId) {
    member = await env.DB.prepare('SELECT * FROM members WHERE id = ? AND household_id = ?')
      .bind(memberId, household.id).first();
  }

  if (!member) {
    const displayName = String(body.displayName || 'สมาชิก').trim() || 'สมาชิก';
    if (displayName.length > 50) {
      return fail('VALIDATION', 'displayName must be at most 50 characters', 422);
    }
    memberId = uuid();
    await env.DB.prepare(
      `INSERT INTO members (id, household_id, display_name, created_at)
       VALUES (?, ?, ?, ?)`,
    ).bind(memberId, household.id, displayName, nowIso()).run();
    member = { id: memberId, display_name: displayName };
  }

  const session = await createSession(env, member.id);
  await audit(env, household.id, 'member', member.id, 'member.login', {});

  return ok({
    token: session.token,
    expiresAt: session.expiresAt,
    member: { id: member.id, displayName: member.display_name },
    household: { id: household.id, name: household.name, timezone: household.timezone },
  });
}

async function handleLogout(request, env) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);

  await env.DB.prepare('UPDATE sessions SET revoked_at = ? WHERE id = ?')
    .bind(nowIso(), session.session_id).run();
  return ok({ revoked: true });
}

async function handleChangePassword(request, env, householdId) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  if (session.household_id !== householdId) return fail('FORBIDDEN', 'Not a member of this household', 403);

  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);

  const currentPassword = String(body.currentPassword || '');
  const newPassword = String(body.newPassword || '');
  if (!currentPassword || !newPassword) {
    return fail('VALIDATION', 'currentPassword and newPassword are required', 422);
  }
  if (newPassword.length < 8) {
    return fail('VALIDATION', 'newPassword must be at least 8 characters', 422);
  }
  if (newPassword.length > 200) {
    return fail('VALIDATION', 'newPassword must be at most 200 characters', 422);
  }
  if (newPassword === currentPassword) {
    return fail('VALIDATION', 'newPassword must differ from currentPassword', 422);
  }

  const rate = await isAuthRateLimited(env, request, 'password');
  if (rate.limited) {
    return fail('RATE_LIMITED', 'Too many failed attempts, try again shortly', 429);
  }

  const household = await env.DB.prepare('SELECT * FROM households WHERE id = ?').bind(householdId).first();
  if (!household) return fail('NOT_FOUND', 'Household not found', 404);

  const valid = await verifyPassword(currentPassword, household.password_hash);
  if (!valid) {
    await recordAuthFailure(env, rate.key);
    return fail('INVALID_PASSWORD', 'Current password is incorrect', 401);
  }
  await clearAuthFailures(env, rate.key);

  const passwordHash = await hashPassword(newPassword);
  const revokedAt = nowIso();

  // Keep the caller signed in; every other session for this household must
  // re-authenticate with the new password.
  await env.DB.batch([
    env.DB.prepare('UPDATE households SET password_hash = ? WHERE id = ?')
      .bind(passwordHash, householdId),
    env.DB.prepare(
      `UPDATE sessions SET revoked_at = ?
       WHERE revoked_at IS NULL AND id != ?
         AND member_id IN (SELECT id FROM members WHERE household_id = ?)`,
    ).bind(revokedAt, session.session_id, householdId),
  ]);

  await audit(env, householdId, 'member', session.member_id, 'household.password.change', {});
  return ok({ updated: true, revokedAt });
}

async function handleRotateDeviceToken(request, env, householdId, deviceId) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  if (session.household_id !== householdId) return fail('FORBIDDEN', 'Not a member of this household', 403);

  const device = await env.DB.prepare('SELECT id FROM devices WHERE id = ? AND household_id = ?')
    .bind(deviceId, householdId).first();
  if (!device) return fail('NOT_FOUND', 'Device not found', 404);

  // The plaintext token is returned exactly once; D1 keeps only its hash.
  const deviceToken = `dev_${toB64Url(crypto.getRandomValues(new Uint8Array(32)))}`;
  const tokenHash = await sha256Hex(deviceToken);
  await env.DB.prepare('UPDATE devices SET token_hash = ? WHERE id = ?')
    .bind(tokenHash, deviceId).run();

  await audit(env, householdId, 'member', session.member_id, 'device.token.rotate', { deviceId });
  return ok({ deviceId, deviceToken });
}

async function handleMe(request, env) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  return ok({
    member: { id: session.member_id, displayName: session.display_name },
    household: { id: session.household_id, name: session.household_name, timezone: session.timezone },
  });
}

async function handleState(request, env, householdId) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  if (session.household_id !== householdId) return fail('FORBIDDEN', 'Not a member of this household', 403);

  const config = await env.DB.prepare('SELECT * FROM configs WHERE household_id = ?').bind(householdId).first();
  if (!config) return fail('NOT_FOUND', 'Config not found', 404);

  const devices = await env.DB.prepare(
    'SELECT id, name, firmware_version, last_seen_at, last_status_json FROM devices WHERE household_id = ? ORDER BY created_at',
  ).bind(householdId).all();

  const updatedBy = config.updated_by
    ? await env.DB.prepare('SELECT id, display_name FROM members WHERE id = ?').bind(config.updated_by).first()
    : null;

  const etag = `"rev-${config.revision}"`;
  const ifNoneMatch = request.headers.get('If-None-Match');
  if (ifNoneMatch && normalizeEtag(ifNoneMatch) === normalizeEtag(etag)) {
    return new Response(null, { status: 304, headers: { ETag: etag } });
  }

  return ok({
    revision: config.revision,
    mode: config.mode,
    schedules: safeJson(config.schedules_json, []),
    updatedAt: config.updated_at,
    updatedBy: updatedBy ? { id: updatedBy.id, displayName: updatedBy.display_name } : null,
    devices: (devices.results || []).map((device) => {
      const status = safeJson(device.last_status_json, {});
      return {
        ...status,
        id: device.id,
        name: device.name,
        firmwareVersion: device.firmware_version,
        lastSeenAt: device.last_seen_at,
        online: device.last_seen_at
          ? Date.now() - new Date(device.last_seen_at).getTime() < DEVICE_ONLINE_WINDOW_MS
          : false,
      };
    }),
  }, 200, { ETag: etag });
}

async function handlePutConfig(request, env, householdId) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  if (session.household_id !== householdId) return fail('FORBIDDEN', 'Not a member of this household', 403);

  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);

  const validationError = validateConfig(body);
  if (validationError) return fail('VALIDATION', validationError, 422);

  let baseRevision = body.baseRevision;
  const ifMatch = request.headers.get('If-Match');
  if (ifMatch) {
    const match = ifMatch.match(/(\d+)/);
    if (match) baseRevision = Number(match[1]);
  }
  if (!Number.isInteger(baseRevision)) {
    return fail('PRECONDITION_REQUIRED', 'baseRevision or If-Match is required', 428);
  }

  const updatedAt = nowIso();
  const result = await env.DB.prepare(
    `UPDATE configs
     SET mode = ?, schedules_json = ?, revision = revision + 1, updated_at = ?, updated_by = ?
     WHERE household_id = ? AND revision = ?`,
  ).bind(
    body.mode,
    JSON.stringify(body.schedules),
    updatedAt,
    session.member_id,
    householdId,
    baseRevision,
  ).run();

  if (!result.meta || result.meta.changes === 0) {
    const latest = await env.DB.prepare('SELECT * FROM configs WHERE household_id = ?').bind(householdId).first();
    if (!latest) return fail('NOT_FOUND', 'Config not found', 404);
    return fail('CONFLICT', 'Config was modified by another member', 409, {
      current: {
        revision: latest.revision,
        mode: latest.mode,
        schedules: safeJson(latest.schedules_json, []),
        updatedAt: latest.updated_at,
      },
    });
  }

  const newRevision = baseRevision + 1;
  await audit(env, householdId, 'member', session.member_id, 'config.update', {
    revision: newRevision,
    mode: body.mode,
    scheduleCount: body.schedules.length,
  });

  return ok({ revision: newRevision, updatedAt }, 200, { ETag: `"rev-${newRevision}"` });
}

function commandDto(row) {
  return {
    commandId: row.id,
    action: row.action,
    status: row.status,
    expiresAt: row.expires_at,
    createdAt: row.created_at,
    ackedAt: row.acked_at,
    error: row.error,
  };
}

async function handleCreateCommand(request, env, householdId) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  if (session.household_id !== householdId) return fail('FORBIDDEN', 'Not a member of this household', 403);

  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);

  const deviceId = String(body.deviceId || '');
  const action = body.action;
  const idempotencyKey = body.idempotencyKey ? String(body.idempotencyKey) : null;

  if (!deviceId || (action !== 'open' && action !== 'close')) {
    return fail('VALIDATION', 'deviceId and action (open|close) are required', 422);
  }

  const device = await env.DB.prepare('SELECT id FROM devices WHERE id = ? AND household_id = ?')
    .bind(deviceId, householdId).first();
  if (!device) return fail('NOT_FOUND', 'Device not found', 404);

  if (idempotencyKey) {
    const existing = await env.DB.prepare(
      'SELECT * FROM commands WHERE household_id = ? AND idempotency_key = ?',
    ).bind(householdId, idempotencyKey).first();
    if (existing) return ok(commandDto(existing));
  }

  const createdAt = nowIso();
  const expiresAt = addSecondsIso(COMMAND_TTL_SECONDS);

  // Latest command wins. Without this, an older pending "open" could be
  // delivered on the sync after a newer "close" was acked, opening the valve
  // unexpectedly. Idempotent replays return above and do not supersede.
  await env.DB.prepare(
    `UPDATE commands SET status = 'expired', error = 'superseded'
     WHERE device_id = ? AND status = 'pending'`,
  ).bind(deviceId).run();

  const id = uuid();
  await env.DB.prepare(
    `INSERT INTO commands
       (id, household_id, device_id, action, status, idempotency_key, created_by, created_at, expires_at, acked_at, error)
     VALUES (?, ?, ?, ?, 'pending', ?, ?, ?, ?, NULL, NULL)`,
  ).bind(id, householdId, deviceId, action, idempotencyKey, session.member_id, createdAt, expiresAt).run();

  await audit(env, householdId, 'member', session.member_id, 'command.create', {
    commandId: id,
    deviceId,
    action,
  });

  const row = await env.DB.prepare('SELECT * FROM commands WHERE id = ?').bind(id).first();
  return ok(commandDto(row), 201);
}

async function handleListCommands(request, env, householdId) {
  const session = await getSession(request, env);
  if (!session) return fail('UNAUTHORIZED', 'Invalid or expired session', 401);
  if (session.household_id !== householdId) return fail('FORBIDDEN', 'Not a member of this household', 403);

  const url = new URL(request.url);
  const limit = Math.min(Math.max(Number(url.searchParams.get('limit')) || 20, 1), 100);
  const now = nowIso();

  // There is no cron trigger; make history accurate when the app asks for it.
  // The device itself already ignores commands whose expires_at has passed.
  await env.DB.prepare(
    `UPDATE commands SET status = 'expired', error = 'ttl'
     WHERE household_id = ? AND status = 'pending' AND expires_at <= ?`,
  ).bind(householdId, now).run();

  const rows = await env.DB.prepare(
    'SELECT * FROM commands WHERE household_id = ? ORDER BY created_at DESC LIMIT ?',
  ).bind(householdId, limit).all();

  return ok({ commands: (rows.results || []).map(commandDto) });
}

async function updateDeviceStatus(env, device, body) {
  const now = nowIso();
  const status = {
    valve: body.valve === true,
    mode: body.mode === 'auto' ? 'auto' : 'manual',
    rssi: typeof body.rssi === 'number' ? body.rssi : null,
    uptimeSeconds: typeof body.uptimeSeconds === 'number' ? body.uptimeSeconds : null,
    firmwareVersion: typeof body.firmwareVersion === 'string' ? body.firmwareVersion : null,
    fault: typeof body.fault === 'string' ? body.fault : null,
    lastCommandId: typeof body.lastCommandId === 'string' ? body.lastCommandId : null,
  };

  await env.DB.prepare(
    `UPDATE devices
     SET last_seen_at = ?, last_status_json = ?, firmware_version = COALESCE(?, firmware_version)
     WHERE id = ?`,
  ).bind(now, JSON.stringify(status), status.firmwareVersion, device.id).run();

  if (status.lastCommandId) {
    await env.DB.prepare(
      `UPDATE commands SET status = 'acked', acked_at = ?
       WHERE id = ? AND device_id = ? AND status = 'pending'`,
    ).bind(now, status.lastCommandId, device.id).run();
  }

  await env.DB.prepare(
    `UPDATE commands SET status = 'expired'
     WHERE device_id = ? AND status = 'pending' AND expires_at <= ?`,
  ).bind(device.id, now).run();

  return { now, status };
}

async function pendingCommand(env, deviceId, now) {
  return env.DB.prepare(
    `SELECT * FROM commands
     WHERE device_id = ? AND status = 'pending' AND expires_at > ?
     ORDER BY created_at DESC LIMIT 1`,
  ).bind(deviceId, now).first();
}

async function handleDeviceSync(request, env) {
  const device = await getDevice(request, env);
  if (!device) return fail('UNAUTHORIZED', 'Invalid device token', 401);

  const body = (await readJson(request)) || {};
  const { now } = await updateDeviceStatus(env, device, body);

  const config = await env.DB.prepare('SELECT * FROM configs WHERE household_id = ?').bind(device.household_id).first();
  if (!config) return fail('NOT_FOUND', 'Config not found', 404);

  const command = await pendingCommand(env, device.id, now);

  return ok({
    deviceId: device.id,
    revision: config.revision,
    mode: config.mode,
    schedules: safeJson(config.schedules_json, []),
    command: command
      ? {
          id: command.id,
          action: command.action,
          expiresAt: command.expires_at,
          createdAt: command.created_at,
        }
      : null,
    serverTime: now,
  });
}

async function handleDeviceStatus(request, env) {
  const device = await getDevice(request, env);
  if (!device) return fail('UNAUTHORIZED', 'Invalid device token', 401);
  const body = (await readJson(request)) || {};
  const { now } = await updateDeviceStatus(env, device, body);
  return ok({ received: true, serverTime: now });
}

async function handleDeviceDesired(request, env) {
  const device = await getDevice(request, env);
  if (!device) return fail('UNAUTHORIZED', 'Invalid device token', 401);

  const now = nowIso();
  await env.DB.prepare('UPDATE devices SET last_seen_at = ? WHERE id = ?').bind(now, device.id).run();

  const config = await env.DB.prepare('SELECT * FROM configs WHERE household_id = ?').bind(device.household_id).first();
  if (!config) return fail('NOT_FOUND', 'Config not found', 404);

  const command = await pendingCommand(env, device.id, now);

  return ok({
    deviceId: device.id,
    revision: config.revision,
    mode: config.mode,
    schedules: safeJson(config.schedules_json, []),
    command: command
      ? {
          id: command.id,
          action: command.action,
          expiresAt: command.expires_at,
          createdAt: command.created_at,
        }
      : null,
    serverTime: now,
  });
}

async function handleDeviceAck(request, env, commandId) {
  const device = await getDevice(request, env);
  if (!device) return fail('UNAUTHORIZED', 'Invalid device token', 401);

  const body = await readJson(request);
  if (!body) return fail('BAD_REQUEST', 'Invalid JSON body', 400);
  if (!['acked', 'failed', 'expired'].includes(body.status)) {
    return fail('VALIDATION', 'status must be acked|failed|expired', 422);
  }

  const now = nowIso();
  const result = await env.DB.prepare(
    `UPDATE commands SET status = ?, acked_at = ?, error = ?
     WHERE id = ? AND device_id = ?`,
  ).bind(body.status, now, body.error ? String(body.error) : null, commandId, device.id).run();

  if (!result.meta || result.meta.changes === 0) return fail('NOT_FOUND', 'Command not found', 404);

  await audit(env, device.household_id, 'device', device.id, 'command.ack', {
    commandId,
    status: body.status,
  });

  return ok({ commandId, status: body.status, ackedAt: now });
}

/* ===================================================================
 * Router
 * =================================================================== */

async function handleRequest(request, env) {
  const url = new URL(request.url);
  const path = url.pathname;

  if (request.method === 'OPTIONS') {
    return new Response(null, { status: 204, headers: corsHeaders(env, request) });
  }

  if (path === '/health' && request.method === 'GET') {
    return ok({ version: '2.0.0', time: nowIso() });
  }

  if (path === '/v1/setup' && request.method === 'POST') return handleSetup(request, env);
  if (path === '/v1/auth/join' && request.method === 'POST') return handleJoin(request, env);
  if (path === '/v1/auth/login' && request.method === 'POST') return handleLogin(request, env);
  if (path === '/v1/auth/logout' && request.method === 'POST') return handleLogout(request, env);
  if (path === '/v1/me' && request.method === 'GET') return handleMe(request, env);

  let match = path.match(/^\/v1\/households\/([^/]+)\/state$/);
  if (match && request.method === 'GET') return handleState(request, env, match[1]);

  match = path.match(/^\/v1\/households\/([^/]+)\/config$/);
  if (match && request.method === 'PUT') return handlePutConfig(request, env, match[1]);

  match = path.match(/^\/v1\/households\/([^/]+)\/password$/);
  if (match && request.method === 'POST') return handleChangePassword(request, env, match[1]);

  match = path.match(/^\/v1\/households\/([^/]+)\/devices\/([^/]+)\/rotate-token$/);
  if (match && request.method === 'POST') return handleRotateDeviceToken(request, env, match[1], match[2]);

  match = path.match(/^\/v1\/households\/([^/]+)\/commands$/);
  if (match && request.method === 'POST') return handleCreateCommand(request, env, match[1]);
  if (match && request.method === 'GET') return handleListCommands(request, env, match[1]);

  if (path === '/v1/devices/sync' && request.method === 'POST') return handleDeviceSync(request, env);
  if (path === '/v1/devices/status' && request.method === 'POST') return handleDeviceStatus(request, env);
  if (path === '/v1/devices/desired' && request.method === 'GET') return handleDeviceDesired(request, env);

  match = path.match(/^\/v1\/devices\/commands\/([^/]+)\/ack$/);
  if (match && request.method === 'POST') return handleDeviceAck(request, env, match[1]);

  return fail('NOT_FOUND', 'Not found', 404);
}

export default {
  async fetch(request, env) {
    try {
      const response = await handleRequest(request, env);
      return withCors(response, env, request);
    } catch (error) {
      console.error('unhandled error', error);
      return withCors(fail('INTERNAL', 'Internal server error', 500), env, request);
    }
  },
};
