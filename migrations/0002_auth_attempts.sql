-- Failed authentication attempts per client IP and scope.
-- Only failures are counted; a successful login clears the counter for that IP.
-- The Worker also treats rows older than one hour as stale and cleans them up
-- opportunistically. No cron trigger is required.

CREATE TABLE IF NOT EXISTS auth_attempts (
  key          TEXT PRIMARY KEY,
  window_start INTEGER NOT NULL,
  failures     INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_auth_attempts_window ON auth_attempts(window_start);
