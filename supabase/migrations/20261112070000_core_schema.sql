-- eLingo Hub - core schema (Milestone 1)
--
-- Trust model
--   * Students never talk to Supabase. Their phones talk to the Hub over the Hub's own Wi-Fi.
--   * Each Hub is a Supabase Auth user linked to one row in public.hubs. The Hub calls the
--     hub_* RPCs below (security definer, ownership checked) and Storage (RLS on its folder).
--   * Edge Functions (worker, cleanup) use the secret key; worker_* RPCs are service_role only.
--   * anon has no access to anything in this schema.

create extension if not exists pgmq;
create extension if not exists pg_net with schema extensions;
create extension if not exists pg_cron with schema pg_catalog;

-- Supabase grants anon/authenticated access to new public objects by default; turn that off.
-- (PUBLIC's EXECUTE default is global, so it is removed by the explicit revoke at the end.)
alter default privileges in schema public revoke execute on functions from anon, authenticated;
alter default privileges in schema public revoke all on tables from anon, authenticated;

-- ---------------------------------------------------------------------------------------------
-- Tables
-- ---------------------------------------------------------------------------------------------

create table public.hubs (
  id               uuid primary key default gen_random_uuid(),
  auth_user_id     uuid not null unique references auth.users (id) on delete cascade,
  name             text not null check (char_length(name) between 1 and 64),
  created_at       timestamptz not null default now(),
  disabled_at      timestamptz,
  last_seen_at     timestamptz,
  firmware_version text,
  last_stats       jsonb not null default '{}'::jsonb
);

create table public.languages (
  code        text primary key check (code ~ '^[a-z]{2,3}$'),
  name        text not null,
  native_name text not null,
  enabled     boolean not null default true,
  sort_order  int not null default 100,
  tts_model   text,
  tts_voice   text,
  notes       text
);

create table public.sessions (
  id               uuid primary key default gen_random_uuid(),
  hub_id           uuid not null references public.hubs (id) on delete cascade,
  status           text not null default 'active' check (status in ('active', 'ended')),
  client_nonce     text not null check (client_nonce ~ '^[A-Za-z0-9_-]{4,64}$'),
  title            text check (title is null or char_length(title) <= 120),
  started_at       timestamptz not null default now(),
  ended_at         timestamptz,
  end_reason       text,
  last_activity_at timestamptz not null default now(),
  next_seq         int not null default 1,
  unique (hub_id, client_nonce)
);
create unique index sessions_one_active_per_hub on public.sessions (hub_id) where status = 'active';
create index sessions_activity_idx on public.sessions (last_activity_at) where status = 'active';

-- One row per (session, listener). The Hub is authoritative for presence and reports the full
-- listener set every few seconds; rows not reported are marked left.
create table public.listeners (
  session_id    uuid not null references public.sessions (id) on delete cascade,
  listener_id   text not null check (listener_id ~ '^[A-Za-z0-9_-]{8,64}$'),
  language_code text not null references public.languages (code),
  joined_at     timestamptz not null default now(),
  last_seen_at  timestamptz not null default now(),
  left_at       timestamptz,
  primary key (session_id, listener_id)
);
create index listeners_active_idx on public.listeners (session_id, language_code) where left_at is null;
create index listeners_stale_idx on public.listeners (last_seen_at) where left_at is null;

-- Demand state per (session, language). active = at least one current listener.
create table public.language_streams (
  session_id          uuid not null references public.sessions (id) on delete cascade,
  language_code       text not null references public.languages (code),
  listener_count      int not null default 0 check (listener_count >= 0),
  active              boolean not null default false,
  first_activated_at  timestamptz,
  last_activated_at   timestamptz,
  last_deactivated_at timestamptz,
  segments_produced   int not null default 0,
  primary key (session_id, language_code)
);

create table public.source_chunks (
  id                bigint generated always as identity primary key,
  session_id        uuid not null references public.sessions (id) on delete cascade,
  hub_id            uuid not null references public.hubs (id) on delete cascade,
  seq               int not null check (seq > 0),
  storage_path      text not null,
  bytes             int not null check (bytes between 44 and 262144),
  duration_ms       int not null check (duration_ms between 100 and 10000),
  rms               int,
  peak              int,
  silent            boolean not null default false,
  captured_at       timestamptz,
  uploaded_at       timestamptz not null default now(),
  status            text not null default 'queued'
                    check (status in ('queued', 'processing', 'done', 'skipped_silent',
                                      'skipped_no_listeners', 'failed')),
  target_languages  text[] not null default '{}',
  attempts          int not null default 0,
  processing_started_at timestamptz,
  processed_at      timestamptz,
  source_deleted_at timestamptz,
  last_error        text,
  unique (session_id, seq)
);
create index source_chunks_cleanup_idx on public.source_chunks (processed_at) where source_deleted_at is null;

-- Filled by Milestone 2 (Deepgram). One transcript per source chunk, shared by all languages.
create table public.transcripts (
  chunk_id          bigint primary key references public.source_chunks (id) on delete cascade,
  provider          text not null,
  model             text,
  text              text not null,
  confidence        real,
  detected_language text,
  created_at        timestamptz not null default now(),
  meta              jsonb not null default '{}'::jsonb
);

-- One generated audio segment per (source chunk, language), shared by every listener of that
-- language. id is a monotonic cursor the Hub polls with.
create table public.segments (
  id            bigint generated always as identity primary key,
  chunk_id      bigint not null references public.source_chunks (id) on delete cascade,
  session_id    uuid not null references public.sessions (id) on delete cascade,
  hub_id        uuid not null references public.hubs (id) on delete cascade,
  language_code text not null references public.languages (code),
  seq           int not null,
  status        text not null default 'pending' check (status in ('pending', 'ready', 'failed')),
  mode          text not null default 'loopback',
  text          text,
  storage_path  text,
  bytes         int,
  duration_ms   int,
  created_at    timestamptz not null default now(),
  ready_at      timestamptz,
  deleted_at    timestamptz,
  last_error    text,
  unique (chunk_id, language_code)
);
create index segments_poll_idx on public.segments (session_id, id) where status = 'ready' and deleted_at is null;
create index segments_cleanup_idx on public.segments (ready_at) where deleted_at is null;

create table public.processing_events (
  id            bigint generated always as identity primary key,
  created_at    timestamptz not null default now(),
  level         text not null check (level in ('info', 'warn', 'error')),
  stage         text not null,
  hub_id        uuid,
  session_id    uuid,
  chunk_id      bigint,
  language_code text,
  message       text not null,
  detail        jsonb not null default '{}'::jsonb
);
create index processing_events_session_idx on public.processing_events (session_id, created_at);

create table public.usage_events (
  id            bigint generated always as identity primary key,
  created_at    timestamptz not null default now(),
  hub_id        uuid,
  session_id    uuid,
  chunk_id      bigint,
  language_code text,
  provider      text not null,
  operation     text not null,
  model         text,
  units         numeric not null default 0,
  unit_type     text not null,
  est_cost_usd  numeric not null default 0,
  request_id    text,
  meta          jsonb not null default '{}'::jsonb
);
create index usage_events_session_idx on public.usage_events (session_id, language_code);

-- Published provider prices, used to estimate cost. Filled in Milestone 2.
create table public.rate_cards (
  provider     text not null,
  model        text not null,
  unit_type    text not null,
  usd_per_unit numeric not null,
  valid_from   date not null,
  valid_to     date,
  source_url   text,
  primary key (provider, model, unit_type, valid_from)
);

alter table public.hubs              enable row level security;
alter table public.languages         enable row level security;
alter table public.sessions          enable row level security;
alter table public.listeners         enable row level security;
alter table public.language_streams  enable row level security;
alter table public.source_chunks     enable row level security;
alter table public.transcripts       enable row level security;
alter table public.segments          enable row level security;
alter table public.processing_events enable row level security;
alter table public.usage_events      enable row level security;
alter table public.rate_cards        enable row level security;
-- No policies: anon/authenticated cannot read or write these tables directly. Hubs use the
-- hub_* RPCs; Edge Functions use the secret key (service_role bypasses RLS).
revoke all on all tables in schema public from anon, authenticated;

-- ---------------------------------------------------------------------------------------------
-- Seed: target languages supported by BOTH Deepgram Nova-3 (source, multilingual) and Gemini
-- 3.8 Flash TTS (output), checked 2026-11-12. isiZulu, isiXhosa and Setswana are deliberately
-- absent: Gemini TTS does not support them.
-- ---------------------------------------------------------------------------------------------
insert into public.languages (code, name, native_name, sort_order, tts_model, notes) values
  ('en',  'English',                 'English',   10, 'gemini-3.8-flash-tts', null),
  ('af',  'Afrikaans',               'Afrikaans', 20, 'gemini-3.8-flash-tts', null),
  ('st',  'Sesotho (Southern Sotho)', 'Sesotho',  30, 'gemini-3.8-flash-tts', 'Not supported by 3.8 Flash-Lite TTS'),
  ('nso', 'Sepedi (Northern Sotho)', 'Sepedi',    40, 'gemini-3.8-flash-tts', null),
  ('ss',  'siSwati',                 'siSwati',   50, 'gemini-3.8-flash-tts', 'Not supported by 3.8 Flash-Lite TTS');

-- ---------------------------------------------------------------------------------------------
-- Internal helpers
-- ---------------------------------------------------------------------------------------------

create or replace function public.current_hub_id()
returns uuid
language sql
stable
security definer
set search_path = ''
as $$
  select h.id from public.hubs h where h.auth_user_id = auth.uid() and h.disabled_at is null
$$;

create or replace function public._require_hub()
returns uuid
language plpgsql
stable
security definer
set search_path = ''
as $$
declare
  v_hub uuid := public.current_hub_id();
begin
  if v_hub is null then
    raise exception 'caller is not a registered hub' using errcode = '42501';
  end if;
  return v_hub;
end;
$$;

create or replace function public.hub_owns_session(p_session text)
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
  select exists (
    select 1 from public.sessions s
    where s.id::text = p_session and s.hub_id = public.current_hub_id()
  )
$$;

-- Realtime is best-effort: a failed broadcast must never roll back pipeline work.
create or replace function public._notify_hub(p_hub_id uuid, p_event text, p_payload jsonb)
returns void
language plpgsql
security definer
set search_path = ''
as $$
begin
  perform realtime.send(p_payload, p_event, 'hub:' || p_hub_id::text, true);
exception when others then
  null;
end;
$$;

-- Invokes an Edge Function through pg_net using credentials stored in Vault
-- (names: elingo_project_url, elingo_worker_key). No-op until those secrets exist.
create or replace function public._invoke_function(p_function text, p_body jsonb default '{}'::jsonb)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_url text;
  v_key text;
begin
  select decrypted_secret into v_url from vault.decrypted_secrets where name = 'elingo_project_url';
  select decrypted_secret into v_key from vault.decrypted_secrets where name = 'elingo_worker_key';
  if v_url is null or v_key is null then
    return;
  end if;
  perform net.http_post(
    url := v_url || '/functions/v1/' || p_function,
    body := p_body,
    headers := jsonb_build_object('Content-Type', 'application/json', 'apikey', v_key),
    timeout_milliseconds := 10000
  );
exception when others then
  null;
end;
$$;

create or replace function public._recompute_language_streams(p_session_id uuid)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub     uuid;
  v_changed int;
  v_result  jsonb;
begin
  select hub_id into v_hub from public.sessions where id = p_session_id;

  with counts as (
    select l.language_code, count(*)::int as n
    from public.listeners l
    where l.session_id = p_session_id and l.left_at is null
    group by l.language_code
  ),
  upserted as (
    insert into public.language_streams as ls
      (session_id, language_code, listener_count, active, first_activated_at, last_activated_at)
    select p_session_id, c.language_code, c.n, true, now(), now() from counts c
    on conflict (session_id, language_code) do update
      set listener_count     = excluded.listener_count,
          active             = true,
          first_activated_at = coalesce(ls.first_activated_at, now()),
          last_activated_at  = case when ls.active then ls.last_activated_at else now() end
      where ls.listener_count is distinct from excluded.listener_count or not ls.active
    returning 1
  ),
  deactivated as (
    update public.language_streams ls
       set listener_count = 0, active = false, last_deactivated_at = now()
     where ls.session_id = p_session_id
       and ls.active
       and not exists (select 1 from counts c where c.language_code = ls.language_code)
    returning 1
  )
  select (select count(*) from upserted) + (select count(*) from deactivated) into v_changed;

  select coalesce(jsonb_agg(jsonb_build_object('code', ls.language_code, 'listeners', ls.listener_count)
                            order by ls.language_code), '[]'::jsonb)
    into v_result
    from public.language_streams ls
   where ls.session_id = p_session_id and ls.active;

  if v_changed > 0 and v_hub is not null then
    perform public._notify_hub(v_hub, 'listener_counts',
                               jsonb_build_object('sessionId', p_session_id, 'languages', v_result));
  end if;
  return v_result;
end;
$$;

create or replace function public._end_session(p_session_id uuid, p_reason text)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub uuid;
begin
  update public.sessions
     set status = 'ended', ended_at = now(), end_reason = p_reason
   where id = p_session_id and status = 'active'
  returning hub_id into v_hub;
  if v_hub is null then
    return;
  end if;
  update public.listeners set left_at = now() where session_id = p_session_id and left_at is null;
  perform public._recompute_language_streams(p_session_id);
  perform public._notify_hub(v_hub, 'session_status',
                             jsonb_build_object('sessionId', p_session_id, 'status', 'ended', 'reason', p_reason));
end;
$$;

create or replace function public._session_json(p_session public.sessions)
returns jsonb
language sql
stable
set search_path = ''
as $$
  select jsonb_build_object(
    'id', p_session.id,
    'status', p_session.status,
    'nextSeq', p_session.next_seq,
    'startedAt', p_session.started_at,
    'endedAt', p_session.ended_at
  )
$$;

-- ---------------------------------------------------------------------------------------------
-- Hub API (called by the ESP32 with its own JWT)
-- ---------------------------------------------------------------------------------------------

-- Boot / heartbeat. Returns hub identity, the language catalog and a resumable session.
create or replace function public.hub_hello(p_firmware_version text default null,
                                            p_stats jsonb default '{}'::jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub     uuid := public._require_hub();
  v_name    text;
  v_session public.sessions;
begin
  if pg_column_size(p_stats) > 4096 then
    raise exception 'stats payload too large' using errcode = '22023';
  end if;

  update public.hubs
     set last_seen_at = now(),
         firmware_version = coalesce(left(p_firmware_version, 32), firmware_version),
         last_stats = coalesce(p_stats, '{}'::jsonb)
   where id = v_hub
  returning name into v_name;

  select * into v_session from public.sessions where hub_id = v_hub and status = 'active';
  -- A session the hub abandoned long ago (power loss) is closed instead of resumed.
  if v_session.id is not null and v_session.last_activity_at < now() - interval '2 minutes' then
    perform public._end_session(v_session.id, 'hub_restarted');
    v_session := null;
  end if;

  return jsonb_build_object(
    'hub', jsonb_build_object('id', v_hub, 'name', v_name),
    'languages', (select coalesce(jsonb_agg(jsonb_build_object('code', l.code, 'name', l.name,
                                                               'nativeName', l.native_name)
                                            order by l.sort_order, l.code), '[]'::jsonb)
                    from public.languages l where l.enabled),
    'session', case when v_session.id is null then null else public._session_json(v_session) end,
    'serverTime', now()
  );
end;
$$;

-- Start a lecture session. Idempotent per client nonce, so a retried request after a lost
-- response returns the same session instead of creating a second one.
create or replace function public.hub_start_session(p_client_nonce text, p_title text default null)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub     uuid := public._require_hub();
  v_session public.sessions;
  r         record;
begin
  perform 1 from public.hubs where id = v_hub for update;  -- serialize per hub

  select * into v_session from public.sessions where hub_id = v_hub and client_nonce = p_client_nonce;
  if found then
    return public._session_json(v_session);
  end if;

  for r in select id from public.sessions where hub_id = v_hub and status = 'active' loop
    perform public._end_session(r.id, 'superseded');
  end loop;

  insert into public.sessions (hub_id, client_nonce, title)
  values (v_hub, p_client_nonce, nullif(left(p_title, 120), ''))
  returning * into v_session;

  perform public._notify_hub(v_hub, 'session_status',
                             jsonb_build_object('sessionId', v_session.id, 'status', 'active'));
  return public._session_json(v_session);
end;
$$;

create or replace function public.hub_end_session(p_session_id uuid)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub     uuid := public._require_hub();
  v_session public.sessions;
begin
  select * into v_session from public.sessions where id = p_session_id and hub_id = v_hub for update;
  if not found then
    raise exception 'unknown session' using errcode = 'P0002';
  end if;
  perform public._end_session(p_session_id, 'button');
  select * into v_session from public.sessions where id = p_session_id;
  return public._session_json(v_session);
end;
$$;

-- Presence sync. p_listeners = [{"id": "...", "lang": "af"}, ...] is the Hub's complete current
-- listener set. Idempotent: the same input always yields the same state.
create or replace function public.hub_sync(p_session_id uuid,
                                           p_listeners jsonb,
                                           p_stats jsonb default '{}'::jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub      uuid := public._require_hub();
  v_session  public.sessions;
  v_ids      text[];
  v_langs    text[];
  v_rejected int;
  v_streams  jsonb;
begin
  if jsonb_typeof(p_listeners) is distinct from 'array' or jsonb_array_length(p_listeners) > 32 then
    raise exception 'p_listeners must be an array of at most 32 entries' using errcode = '22023';
  end if;
  if pg_column_size(p_stats) > 4096 then
    raise exception 'stats payload too large' using errcode = '22023';
  end if;

  select * into v_session from public.sessions where id = p_session_id and hub_id = v_hub for update;
  if not found then
    raise exception 'unknown session' using errcode = 'P0002';
  end if;

  update public.hubs set last_seen_at = now(), last_stats = coalesce(p_stats, '{}'::jsonb) where id = v_hub;

  if v_session.status <> 'active' then
    return jsonb_build_object('sessionStatus', v_session.status, 'languages', '[]'::jsonb, 'rejected', 0);
  end if;

  with incoming as (
    select distinct on (x ->> 'id') x ->> 'id' as listener_id, x ->> 'lang' as language_code
    from jsonb_array_elements(p_listeners) x
    order by x ->> 'id'
  ),
  valid as (
    select i.listener_id, i.language_code
    from incoming i
    join public.languages l on l.code = i.language_code and l.enabled
    where i.listener_id ~ '^[A-Za-z0-9_-]{8,64}$'
  )
  select coalesce(array_agg(listener_id), '{}'), coalesce(array_agg(language_code), '{}')
    into v_ids, v_langs
    from valid;

  v_rejected := jsonb_array_length(p_listeners) - coalesce(array_length(v_ids, 1), 0);

  insert into public.listeners as l (session_id, listener_id, language_code)
  select p_session_id, u.id, u.lang from unnest(v_ids, v_langs) as u(id, lang)
  on conflict (session_id, listener_id) do update
    set language_code = excluded.language_code,
        last_seen_at  = now(),
        left_at       = null,
        joined_at     = case when l.left_at is not null then now() else l.joined_at end;

  update public.listeners
     set left_at = now()
   where session_id = p_session_id and left_at is null and listener_id <> all (v_ids);

  update public.sessions set last_activity_at = now() where id = p_session_id;
  v_streams := public._recompute_language_streams(p_session_id);

  return jsonb_build_object('sessionStatus', 'active', 'languages', v_streams, 'rejected', v_rejected);
end;
$$;

-- Register an uploaded source chunk and queue it for processing (one transaction).
create or replace function public.hub_register_chunk(p_session_id uuid,
                                                     p_seq int,
                                                     p_duration_ms int,
                                                     p_rms int default null,
                                                     p_peak int default null,
                                                     p_silent boolean default false,
                                                     p_captured_at timestamptz default null)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub      uuid := public._require_hub();
  v_session  public.sessions;
  v_path     text;
  v_bytes    int;
  v_id       bigint;
  v_status   text;
  v_recent   int;
begin
  select * into v_session from public.sessions where id = p_session_id and hub_id = v_hub for update;
  if not found then
    raise exception 'unknown session' using errcode = 'P0002';
  end if;
  -- Chunks still uploading right after the button press are accepted for 2 minutes.
  if v_session.status = 'ended' and v_session.ended_at < now() - interval '2 minutes' then
    raise exception 'session has ended' using errcode = 'P0001';
  end if;
  if p_seq < 1 or p_seq > 100000 then
    raise exception 'seq out of range' using errcode = '22023';
  end if;

  v_path := v_hub::text || '/' || p_session_id::text || '/' || lpad(p_seq::text, 6, '0') || '.wav';

  select (o.metadata ->> 'size')::int into v_bytes
    from storage.objects o
   where o.bucket_id = 'source-audio' and o.name = v_path;
  if v_bytes is null then
    raise exception 'audio object not found: %', v_path using errcode = 'P0002';
  end if;

  select count(*) into v_recent
    from public.source_chunks
   where session_id = p_session_id and uploaded_at > now() - interval '60 seconds';
  if v_recent >= 30 then
    raise exception 'chunk rate limit exceeded' using errcode = '54000';
  end if;

  insert into public.source_chunks (session_id, hub_id, seq, storage_path, bytes, duration_ms,
                                    rms, peak, silent, captured_at)
  values (p_session_id, v_hub, p_seq, v_path, v_bytes, p_duration_ms, p_rms, p_peak,
          coalesce(p_silent, false), p_captured_at)
  on conflict (session_id, seq) do nothing
  returning id into v_id;

  if v_id is null then
    select id, status into v_id, v_status from public.source_chunks
     where session_id = p_session_id and seq = p_seq;
    return jsonb_build_object('chunkId', v_id, 'status', v_status, 'duplicate', true);
  end if;

  update public.sessions
     set next_seq = greatest(next_seq, p_seq + 1), last_activity_at = now()
   where id = p_session_id;

  perform pgmq.send('chunk_jobs', jsonb_build_object('chunk_id', v_id));
  -- Wake the worker now; the cron sweep is the fallback if this call is lost.
  perform public._invoke_function('process-chunk', jsonb_build_object('reason', 'chunk', 'chunkId', v_id));

  return jsonb_build_object('chunkId', v_id, 'status', 'queued', 'duplicate', false);
end;
$$;

-- New ready segments for the languages the Hub's listeners want.
create or replace function public.hub_poll(p_session_id uuid,
                                           p_after_segment_id bigint default 0,
                                           p_languages text[] default '{}')
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub     uuid := public._require_hub();
  v_session public.sessions;
begin
  select * into v_session from public.sessions where id = p_session_id and hub_id = v_hub;
  if not found then
    raise exception 'unknown session' using errcode = 'P0002';
  end if;

  return jsonb_build_object(
    'sessionStatus', v_session.status,
    'segments', (
      select coalesce(jsonb_agg(jsonb_build_object(
               'id', s.id, 'lang', s.language_code, 'seq', s.seq, 'path', s.storage_path,
               'bytes', s.bytes, 'durationMs', s.duration_ms) order by s.id), '[]'::jsonb)
      from (
        select * from public.segments
         where session_id = p_session_id
           and status = 'ready'
           and deleted_at is null
           and id > coalesce(p_after_segment_id, 0)
           and language_code = any (coalesce(p_languages, '{}'))
         order by id
         limit 24
      ) s
    ),
    'languages', (
      select coalesce(jsonb_agg(jsonb_build_object('code', ls.language_code,
                                                   'listeners', ls.listener_count)
                                order by ls.language_code), '[]'::jsonb)
      from public.language_streams ls
      where ls.session_id = p_session_id and ls.active
    )
  );
end;
$$;

-- ---------------------------------------------------------------------------------------------
-- Worker API (Edge Functions with the secret key only)
-- ---------------------------------------------------------------------------------------------

select pgmq.create('chunk_jobs');

create or replace function public.worker_claim(p_qty int default 4, p_vt int default 60)
returns table (msg_id bigint, read_ct int, chunk_id bigint)
language sql
security definer
set search_path = ''
as $$
  select m.msg_id, m.read_ct, (m.message ->> 'chunk_id')::bigint
  from pgmq.read('chunk_jobs', p_vt, least(greatest(p_qty, 1), 16)) m
$$;

-- Marks the chunk processing and snapshots the languages that have listeners right now.
create or replace function public.worker_begin_chunk(p_chunk_id bigint)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_chunk public.source_chunks;
  v_langs text[];
  v_ready text[];
begin
  select * into v_chunk from public.source_chunks where id = p_chunk_id for update;
  if not found then
    return null;
  end if;

  select coalesce(array_agg(language_code order by language_code), '{}') into v_langs
    from public.language_streams
   where session_id = v_chunk.session_id and active and listener_count > 0;

  select coalesce(array_agg(language_code), '{}') into v_ready
    from public.segments
   where chunk_id = p_chunk_id and status = 'ready';

  update public.source_chunks
     set status = case when status in ('done', 'skipped_silent', 'skipped_no_listeners', 'failed')
                       then status else 'processing' end,
         attempts = attempts + 1,
         processing_started_at = coalesce(processing_started_at, now()),
         target_languages = v_langs
   where id = p_chunk_id
  returning * into v_chunk;

  return jsonb_build_object(
    'chunk', jsonb_build_object('id', v_chunk.id, 'sessionId', v_chunk.session_id, 'hubId', v_chunk.hub_id,
                                'seq', v_chunk.seq, 'path', v_chunk.storage_path, 'bytes', v_chunk.bytes,
                                'durationMs', v_chunk.duration_ms, 'silent', v_chunk.silent,
                                'status', v_chunk.status, 'attempts', v_chunk.attempts),
    'languages', to_jsonb(v_langs),
    'readyLanguages', to_jsonb(v_ready)
  );
end;
$$;

create or replace function public.worker_language_active(p_session_id uuid, p_language text)
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
  select exists (select 1 from public.language_streams
                 where session_id = p_session_id and language_code = p_language
                   and active and listener_count > 0)
$$;

create or replace function public.worker_segment_ready(p_chunk_id bigint,
                                                       p_language text,
                                                       p_path text,
                                                       p_bytes int,
                                                       p_duration_ms int,
                                                       p_mode text,
                                                       p_text text default null)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_chunk public.source_chunks;
  v_id    bigint;
  v_new   boolean;
begin
  select * into v_chunk from public.source_chunks where id = p_chunk_id;
  if not found then
    raise exception 'unknown chunk %', p_chunk_id using errcode = 'P0002';
  end if;

  insert into public.segments as s (chunk_id, session_id, hub_id, language_code, seq, status, mode,
                                    text, storage_path, bytes, duration_ms, ready_at)
  values (p_chunk_id, v_chunk.session_id, v_chunk.hub_id, p_language, v_chunk.seq, 'ready', p_mode,
          p_text, p_path, p_bytes, p_duration_ms, now())
  on conflict (chunk_id, language_code) do update
    set status = 'ready', mode = excluded.mode, text = coalesce(excluded.text, s.text),
        storage_path = excluded.storage_path, bytes = excluded.bytes,
        duration_ms = excluded.duration_ms, ready_at = coalesce(s.ready_at, now()), last_error = null
    where s.status <> 'ready'
  returning id, (xmax = 0) into v_id, v_new;

  if v_id is null then  -- already ready: duplicate delivery, nothing to do
    select id into v_id from public.segments where chunk_id = p_chunk_id and language_code = p_language;
    return v_id;
  end if;

  update public.language_streams
     set segments_produced = segments_produced + 1
   where session_id = v_chunk.session_id and language_code = p_language;

  perform public._notify_hub(v_chunk.hub_id, 'segment_ready',
                             jsonb_build_object('sessionId', v_chunk.session_id, 'segmentId', v_id,
                                                'lang', p_language, 'seq', v_chunk.seq));
  return v_id;
end;
$$;

create or replace function public.worker_finish_chunk(p_chunk_id bigint,
                                                      p_msg_id bigint,
                                                      p_status text,
                                                      p_error text default null)
returns void
language plpgsql
security definer
set search_path = ''
as $$
begin
  if p_status not in ('done', 'skipped_silent', 'skipped_no_listeners', 'failed') then
    raise exception 'invalid final status %', p_status using errcode = '22023';
  end if;
  update public.source_chunks
     set status = p_status, processed_at = now(), last_error = left(p_error, 1000)
   where id = p_chunk_id;
  if p_msg_id is not null then
    perform pgmq.archive('chunk_jobs', p_msg_id);
  end if;
end;
$$;

create or replace function public.worker_log(p_level text,
                                             p_stage text,
                                             p_message text,
                                             p_chunk_id bigint default null,
                                             p_language text default null,
                                             p_detail jsonb default '{}'::jsonb)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_chunk public.source_chunks;
begin
  if p_chunk_id is not null then
    select * into v_chunk from public.source_chunks where id = p_chunk_id;
    if p_level = 'error' then
      update public.source_chunks set last_error = left(p_message, 1000) where id = p_chunk_id;
    end if;
  end if;
  insert into public.processing_events (level, stage, hub_id, session_id, chunk_id, language_code, message, detail)
  values (p_level, p_stage, v_chunk.hub_id, v_chunk.session_id, p_chunk_id, p_language,
          left(p_message, 2000), coalesce(p_detail, '{}'::jsonb));
  if p_level = 'error' and v_chunk.hub_id is not null then
    perform public._notify_hub(v_chunk.hub_id, 'processing_error',
                               jsonb_build_object('chunkId', p_chunk_id, 'stage', p_stage, 'lang', p_language));
  end if;
end;
$$;

create or replace function public.worker_record_usage(p_chunk_id bigint,
                                                      p_language text,
                                                      p_provider text,
                                                      p_operation text,
                                                      p_model text,
                                                      p_units numeric,
                                                      p_unit_type text,
                                                      p_request_id text default null,
                                                      p_meta jsonb default '{}'::jsonb)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_chunk public.source_chunks;
  v_rate  numeric;
begin
  select * into v_chunk from public.source_chunks where id = p_chunk_id;
  select r.usd_per_unit into v_rate
    from public.rate_cards r
   where r.provider = p_provider and r.model = coalesce(p_model, '') and r.unit_type = p_unit_type
     and r.valid_from <= current_date and (r.valid_to is null or r.valid_to >= current_date)
   order by r.valid_from desc
   limit 1;
  insert into public.usage_events (hub_id, session_id, chunk_id, language_code, provider, operation,
                                   model, units, unit_type, est_cost_usd, request_id, meta)
  values (v_chunk.hub_id, v_chunk.session_id, p_chunk_id, p_language, p_provider, p_operation,
          p_model, coalesce(p_units, 0), p_unit_type, coalesce(p_units, 0) * coalesce(v_rate, 0),
          p_request_id, coalesce(p_meta, '{}'::jsonb));
end;
$$;

-- Retention: source audio 10 min after processing (or 30 min if never processed),
-- translated audio 15 min after it became ready.
create or replace function public.worker_cleanup_candidates(p_limit int default 200)
returns table (kind text, row_id bigint, bucket text, path text)
language sql
security definer
set search_path = ''
as $$
  (select 'chunk', c.id, 'source-audio', c.storage_path
     from public.source_chunks c
    where c.source_deleted_at is null
      and (c.processed_at < now() - interval '10 minutes' or c.uploaded_at < now() - interval '30 minutes')
    order by c.id
    limit p_limit)
  union all
  (select 'segment', s.id, 'translated-audio', s.storage_path
     from public.segments s
    where s.deleted_at is null and s.storage_path is not null
      and s.ready_at < now() - interval '15 minutes'
    order by s.id
    limit p_limit)
$$;

create or replace function public.worker_mark_deleted(p_chunk_ids bigint[], p_segment_ids bigint[])
returns void
language sql
security definer
set search_path = ''
as $$
  update public.source_chunks set source_deleted_at = now() where id = any (coalesce(p_chunk_ids, '{}'));
  update public.segments set deleted_at = now() where id = any (coalesce(p_segment_ids, '{}'));
$$;

-- ---------------------------------------------------------------------------------------------
-- Scheduled jobs
-- ---------------------------------------------------------------------------------------------

-- Listeners the hub stopped reporting (hub offline/powered off) expire after 45 s, which
-- deactivates their languages and stops AI work. Sessions idle for 10 minutes are ended.
create or replace function public.expire_stale_listeners()
returns int
language plpgsql
security definer
set search_path = ''
as $$
declare
  r       record;
  v_count int := 0;
begin
  for r in
    select distinct session_id from public.listeners
     where left_at is null and last_seen_at < now() - interval '45 seconds'
  loop
    update public.listeners set left_at = now()
     where session_id = r.session_id and left_at is null and last_seen_at < now() - interval '45 seconds';
    perform public._recompute_language_streams(r.session_id);
    v_count := v_count + 1;
  end loop;

  for r in
    select id from public.sessions where status = 'active' and last_activity_at < now() - interval '10 minutes'
  loop
    perform public._end_session(r.id, 'hub_timeout');
  end loop;
  return v_count;
end;
$$;

-- Fallback for lost wake-ups and for redelivery after a visibility timeout. Only invokes the
-- worker when a message is actually visible, so an idle system costs no function calls.
create or replace function public.wake_worker_if_pending()
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
begin
  if exists (select 1 from pgmq.q_chunk_jobs where vt <= now() limit 1) then
    perform public._invoke_function('process-chunk', jsonb_build_object('reason', 'sweep'));
    return true;
  end if;
  return false;
end;
$$;

create or replace function public.cleanup_if_needed()
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
begin
  if exists (select 1 from public.worker_cleanup_candidates(1)) then
    perform public._invoke_function('cleanup', '{}'::jsonb);
    return true;
  end if;
  return false;
end;
$$;

select cron.schedule('elingo-process-sweep',    '10 seconds',  'select public.wake_worker_if_pending()');
select cron.schedule('elingo-expire-listeners', '15 seconds',  'select public.expire_stale_listeners()');
select cron.schedule('elingo-cleanup',          '*/5 * * * *', 'select public.cleanup_if_needed()');

-- ---------------------------------------------------------------------------------------------
-- Storage
-- ---------------------------------------------------------------------------------------------

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types) values
  ('source-audio',     'source-audio',     false, 262144,  array['audio/wav']),
  ('translated-audio', 'translated-audio', false, 1048576, array['audio/wav', 'audio/mpeg', 'audio/ogg'])
on conflict (id) do nothing;

-- Paths: source-audio/{hub_id}/{session_id}/{seq}.wav, translated-audio/{hub_id}/{session_id}/{lang}/{seq}.wav
create policy "hub inserts own source audio" on storage.objects
  for insert to authenticated
  with check (bucket_id = 'source-audio'
              and (storage.foldername(name))[1] = public.current_hub_id()::text
              and public.hub_owns_session((storage.foldername(name))[2]));

create policy "hub reads own source audio" on storage.objects
  for select to authenticated
  using (bucket_id = 'source-audio' and (storage.foldername(name))[1] = public.current_hub_id()::text);

create policy "hub overwrites own source audio" on storage.objects
  for update to authenticated
  using (bucket_id = 'source-audio' and (storage.foldername(name))[1] = public.current_hub_id()::text)
  with check (bucket_id = 'source-audio'
              and (storage.foldername(name))[1] = public.current_hub_id()::text
              and public.hub_owns_session((storage.foldername(name))[2]));

create policy "hub reads own translated audio" on storage.objects
  for select to authenticated
  using (bucket_id = 'translated-audio' and (storage.foldername(name))[1] = public.current_hub_id()::text);

-- ---------------------------------------------------------------------------------------------
-- Realtime: each hub may receive broadcasts on its own private topic "hub:<hub_id>".
-- ---------------------------------------------------------------------------------------------

create policy "hub receives own broadcasts" on realtime.messages
  for select to authenticated
  using (realtime.messages.extension = 'broadcast'
         and realtime.topic() = 'hub:' || public.current_hub_id()::text);

-- ---------------------------------------------------------------------------------------------
-- Grants (explicit allow-list)
-- ---------------------------------------------------------------------------------------------

revoke all on all functions in schema public from public, anon, authenticated;

grant execute on function public.current_hub_id()                                        to authenticated;
grant execute on function public.hub_owns_session(text)                                  to authenticated;
grant execute on function public.hub_hello(text, jsonb)                                  to authenticated;
grant execute on function public.hub_start_session(text, text)                           to authenticated;
grant execute on function public.hub_end_session(uuid)                                   to authenticated;
grant execute on function public.hub_sync(uuid, jsonb, jsonb)                            to authenticated;
grant execute on function public.hub_register_chunk(uuid, int, int, int, int, boolean, timestamptz) to authenticated;
grant execute on function public.hub_poll(uuid, bigint, text[])                          to authenticated;

grant execute on function public.worker_claim(int, int)                                  to service_role;
grant execute on function public.worker_begin_chunk(bigint)                              to service_role;
grant execute on function public.worker_language_active(uuid, text)                      to service_role;
grant execute on function public.worker_segment_ready(bigint, text, text, int, int, text, text) to service_role;
grant execute on function public.worker_finish_chunk(bigint, bigint, text, text)         to service_role;
grant execute on function public.worker_log(text, text, text, bigint, text, jsonb)       to service_role;
grant execute on function public.worker_record_usage(bigint, text, text, text, text, numeric, text, text, jsonb) to service_role;
grant execute on function public.worker_cleanup_candidates(int)                          to service_role;
grant execute on function public.worker_mark_deleted(bigint[], bigint[])                 to service_role;
