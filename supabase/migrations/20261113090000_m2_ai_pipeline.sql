-- eLingo Hub - Milestone 2: AI translation pipeline
--
--   chunk (Hub upload) --chunk_jobs--> STT stage (process-chunk, Deepgram, one session at a time
--   in seq order) --> utterances (whole sentences) --> one translation_job per (utterance,
--   active language) --tts_jobs--> process-translation (Gemini translate + Gemini TTS, or the
--   lecturer's own audio when the student chose the lecturer's language) --> segments --> Hub.
--
-- Demand rules (database-backed, never in-memory):
--   * STT runs only if at least one language has listeners when the chunk is processed.
--   * Translation jobs are created only for languages with listeners, one per language (all
--     listeners of a language share it), and are skipped if the last listener left meanwhile.
--   * Audio too far behind live is skipped before any provider is called.
-- Reliability: per-session STT lease + cursor (ordering), unique constraints (idempotency),
-- provider attempts capped at 3 with exponential backoff, failures recorded, dead letters
-- archived; a failure in one language never blocks another (separate jobs).

-- ---------------------------------------------------------------------------------------------
-- Settings (tunable without redeploying functions)
-- ---------------------------------------------------------------------------------------------

create table public.app_settings (
  key   text primary key,
  value jsonb not null,
  note  text
);
alter table public.app_settings enable row level security;
revoke all on public.app_settings from anon, authenticated;

insert into public.app_settings (key, value, note) values
  ('stt_model',              '"nova-3"',           'Deepgram model'),
  ('stt_language',           '"multi"',            'Deepgram language: multi (code-switching) or a fixed code such as en'),
  ('translate_model',        '"gemini-3.8-flash"', 'Gemini text model for translation'),
  ('translate_thinking',     '"low"',              'thinking_level for the translation model'),
  ('max_chunk_lag_seconds',  '45',                 'chunks captured longer ago than this are not transcribed'),
  ('max_utterance_age_seconds', '60',              'utterances older than this are not translated'),
  ('session_cost_cap_usd',   '25',                 'provider work stops for a session above this estimated cost'),
  ('text_retention_hours',   '24',                 'transcript/translation text is erased this long after creation');

create or replace function public._setting(p_key text, p_default jsonb)
returns jsonb
language sql
stable
security definer
set search_path = ''
as $$
  select coalesce((select value from public.app_settings where key = p_key), p_default)
$$;

-- Per-language TTS model: Flash-Lite TTS (cheaper, faster) where it supports the language;
-- Southern Sotho and siSwati are only supported by Gemini 3.8 Flash TTS (docs, 2026-11-12).
update public.languages set tts_model = 'gemini-3.8-flash-lite-tts', tts_voice = 'Kore' where code in ('en', 'af', 'nso');
update public.languages set tts_model = 'gemini-3.8-flash-tts',      tts_voice = 'Kore' where code in ('st', 'ss');

-- ---------------------------------------------------------------------------------------------
-- Schema changes
-- ---------------------------------------------------------------------------------------------

alter table public.sessions
  add column stt_next_seq    int not null default 1,     -- next chunk seq the STT stage may take
  add column stt_lease_until timestamptz,                -- one STT worker per session at a time
  add column stt_lease_chunk bigint,
  add column stt_state       jsonb not null default '{}'::jsonb,  -- assembler state (held words, carry)
  add column utterance_seq   int not null default 0,
  add column source_language text,
  add column cost_usd        numeric not null default 0,
  add column flushed_at      timestamptz;

alter table public.source_chunks drop constraint source_chunks_status_check;
alter table public.source_chunks add constraint source_chunks_status_check
  check (status in ('queued', 'processing', 'done', 'skipped_silent', 'skipped_no_listeners',
                    'skipped_stale', 'failed'));

alter table public.transcripts
  add column words      jsonb not null default '[]'::jsonb,
  add column preroll_ms int not null default 0,
  add column request_id text,
  add column committed  text;

create table public.utterances (
  id          bigint generated always as identity primary key,
  session_id  uuid not null references public.sessions (id) on delete cascade,
  hub_id      uuid not null references public.hubs (id) on delete cascade,
  seq         int not null,
  text        text,
  language    text,
  start_seq   int not null,
  start_ms    int not null,
  end_seq     int not null,
  end_ms      int not null,
  word_count  int not null default 0,
  created_at  timestamptz not null default now(),
  text_purged_at timestamptz,
  unique (session_id, seq)
);

create table public.translation_jobs (
  id              bigint generated always as identity primary key,
  utterance_id    bigint not null references public.utterances (id) on delete cascade,
  session_id      uuid not null references public.sessions (id) on delete cascade,
  hub_id          uuid not null references public.hubs (id) on delete cascade,
  language_code   text not null references public.languages (code),
  status          text not null default 'queued'
                  check (status in ('queued', 'processing', 'ready', 'skipped_no_listeners',
                                    'skipped_stale', 'failed')),
  method          text check (method in ('tts', 'passthrough')),
  attempts        int not null default 0,
  translated_text text,
  last_error      text,
  created_at      timestamptz not null default now(),
  started_at      timestamptz,
  finished_at     timestamptz,
  unique (utterance_id, language_code)
);
create index translation_jobs_open_idx on public.translation_jobs (session_id, language_code)
  where status in ('queued', 'processing');

alter table public.segments alter column chunk_id drop not null;
alter table public.segments
  add column utterance_id bigint references public.utterances (id) on delete cascade,
  add column chunk_seq    int,
  add column mime         text not null default 'audio/wav',
  add constraint segments_utterance_language_key unique (utterance_id, language_code),
  add constraint segments_has_source check (chunk_id is not null or utterance_id is not null);

alter table public.usage_events
  add column utterance_id bigint,
  add column status       text not null default 'ok' check (status in ('ok', 'error')),
  add column latency_ms   int,
  add column attempt      int,
  add column error        text;

alter table public.utterances       enable row level security;
alter table public.translation_jobs enable row level security;
revoke all on public.utterances, public.translation_jobs from anon, authenticated;

-- Official prices checked 2026-11-12 (USD). Deepgram: https://deepgram.com/pricing (pay as you go,
-- billed per second). Gemini: https://ai.google.dev/gemini-api/docs/pricing (paid tier; several
-- prices double on 2027-01-01). Thought tokens are billed as output tokens.
insert into public.rate_cards (provider, model, unit_type, usd_per_unit, valid_from, valid_to, source_url) values
  ('deepgram', 'nova-3/multi', 'audio_minutes', 0.0052, '2026-11-12', null, 'https://deepgram.com/pricing'),
  ('deepgram', 'nova-3',       'audio_minutes', 0.0043, '2026-11-12', null, 'https://deepgram.com/pricing'),
  ('gemini', 'gemini-3.8-flash', 'input_tokens',  0.75e-6, '2026-11-12', '2026-12-31', 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash', 'input_tokens',  1.50e-6, '2027-01-01', null,         'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash', 'output_tokens', 3.75e-6, '2026-11-12', '2026-12-31', 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash', 'output_tokens', 7.50e-6, '2027-01-01', null,         'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.5-flash-lite', 'input_tokens',  0.30e-6, '2026-11-12', null, 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.5-flash-lite', 'output_tokens', 2.50e-6, '2026-11-12', null, 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-tts', 'input_tokens',        0.50e-6, '2026-11-12', '2026-12-31', 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-tts', 'input_tokens',        1.00e-6, '2027-01-01', null,         'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-tts', 'audio_output_tokens', 9.00e-6, '2026-11-12', '2026-12-31', 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-tts', 'audio_output_tokens', 18.0e-6, '2027-01-01', null,         'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-lite-tts', 'input_tokens',        0.50e-6, '2026-11-12', '2026-12-31', 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-lite-tts', 'input_tokens',        1.00e-6, '2027-01-01', null,         'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-lite-tts', 'audio_output_tokens', 6.00e-6, '2026-11-12', '2026-12-31', 'https://ai.google.dev/gemini-api/docs/pricing'),
  ('gemini', 'gemini-3.8-flash-lite-tts', 'audio_output_tokens', 12.0e-6, '2027-01-01', null,         'https://ai.google.dev/gemini-api/docs/pricing')
on conflict do nothing;

select pgmq.create('tts_jobs');

-- ---------------------------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------------------------

create or replace function public._active_languages(p_session_id uuid)
returns text[]
language sql
stable
security definer
set search_path = ''
as $$
  select coalesce(array_agg(language_code order by language_code), '{}')
    from public.language_streams
   where session_id = p_session_id and active and listener_count > 0
$$;

create or replace function public._chunk_final(p_status text)
returns boolean
language sql
immutable
set search_path = ''
as $$
  select p_status in ('done', 'skipped_silent', 'skipped_no_listeners', 'skipped_stale', 'failed')
$$;

-- Ends a session; the STT stage then flushes any sentence still being assembled.
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
  perform pgmq.send('chunk_jobs', jsonb_build_object('flush_session', p_session_id), 3);
end;
$$;

-- ---------------------------------------------------------------------------------------------
-- Usage / cost ledger
-- ---------------------------------------------------------------------------------------------

-- One row per (call, unit type). Returns the estimated cost added to the session.
create or replace function public.worker_usage(p_session_id uuid,
                                               p_chunk_id bigint,
                                               p_utterance_id bigint,
                                               p_language text,
                                               p_provider text,
                                               p_operation text,
                                               p_model text,
                                               p_items jsonb,
                                               p_status text default 'ok',
                                               p_latency_ms int default null,
                                               p_attempt int default null,
                                               p_request_id text default null,
                                               p_error text default null,
                                               p_meta jsonb default '{}'::jsonb)
returns numeric
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_hub   uuid;
  v_total numeric := 0;
  v_cost  numeric;
  v_rate  numeric;
  item    jsonb;
begin
  select hub_id into v_hub from public.sessions where id = p_session_id;
  if jsonb_typeof(p_items) is distinct from 'array' or jsonb_array_length(p_items) = 0 then
    p_items := jsonb_build_array(jsonb_build_object('unitType', 'requests', 'units', 0));
  end if;
  for item in select * from jsonb_array_elements(p_items) loop
    select r.usd_per_unit into v_rate
      from public.rate_cards r
     where r.provider = p_provider and r.model = coalesce(p_model, '') and r.unit_type = item ->> 'unitType'
       and r.valid_from <= current_date and (r.valid_to is null or r.valid_to >= current_date)
     order by r.valid_from desc
     limit 1;
    v_cost := coalesce((item ->> 'units')::numeric, 0) * coalesce(v_rate, 0);
    v_total := v_total + v_cost;
    insert into public.usage_events (hub_id, session_id, chunk_id, utterance_id, language_code, provider, operation,
                                     model, units, unit_type, est_cost_usd, request_id, status, latency_ms, attempt,
                                     error, meta)
    values (v_hub, p_session_id, p_chunk_id, p_utterance_id, p_language, p_provider, p_operation, p_model,
            coalesce((item ->> 'units')::numeric, 0), item ->> 'unitType', v_cost, p_request_id,
            coalesce(p_status, 'ok'), p_latency_ms, p_attempt, left(p_error, 1000), coalesce(p_meta, '{}'::jsonb));
  end loop;
  update public.sessions set cost_usd = cost_usd + v_total where id = p_session_id;
  return v_total;
end;
$$;

create or replace function public._over_cost_cap(p_session public.sessions)
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
  select p_session.cost_usd >= (public._setting('session_cost_cap_usd', '25'))::text::numeric
$$;

-- ---------------------------------------------------------------------------------------------
-- STT stage (process-chunk, mode ai)
-- ---------------------------------------------------------------------------------------------

create or replace function public.worker_claim_stt(p_qty int default 4, p_vt int default 60)
returns table (msg_id bigint, read_ct int, chunk_id bigint, flush_session uuid)
language sql
security definer
set search_path = ''
as $$
  select m.msg_id, m.read_ct, (m.message ->> 'chunk_id')::bigint, (m.message ->> 'flush_session')::uuid
  from pgmq.read('chunk_jobs', p_vt, least(greatest(p_qty, 1), 16)) m
$$;

create or replace function public.worker_queue_delay(p_queue text, p_msg_id bigint, p_delay_sec int)
returns void
language plpgsql
security definer
set search_path = ''
as $$
begin
  if p_queue not in ('chunk_jobs', 'tts_jobs') then
    raise exception 'unknown queue %', p_queue using errcode = '22023';
  end if;
  perform pgmq.set_vt(p_queue, p_msg_id, greatest(1, least(p_delay_sec, 300)));
end;
$$;

create or replace function public.worker_queue_archive(p_queue text, p_msg_id bigint)
returns void
language plpgsql
security definer
set search_path = ''
as $$
begin
  if p_queue not in ('chunk_jobs', 'tts_jobs') then
    raise exception 'unknown queue %', p_queue using errcode = '22023';
  end if;
  perform pgmq.archive(p_queue, p_msg_id);
end;
$$;

-- Decides what the STT stage does with a chunk, and takes the session's STT lease for it.
-- action: process | skip (reason = final chunk status) | wait (predecessor or lease) | final
create or replace function public.worker_stt_begin(p_chunk_id bigint, p_lease_sec int default 90)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  c        public.source_chunks;
  s        public.sessions;
  prev     public.source_chunks;
  v_langs  text[];
  v_reason text;
  v_lag    numeric := (public._setting('max_chunk_lag_seconds', '45'))::text::numeric;
  v_prev   jsonb := null;
  v_last   boolean;
  v_late   boolean;
begin
  select * into c from public.source_chunks where id = p_chunk_id for update;
  if not found then
    return jsonb_build_object('action', 'final', 'reason', 'missing');
  end if;
  if public._chunk_final(c.status) then
    return jsonb_build_object('action', 'final', 'reason', c.status);
  end if;
  select * into s from public.sessions where id = c.session_id for update;

  -- Strict order per session. Chunks arrive in order (the Hub uploads FIFO); a seq that never
  -- arrives was dropped on the Hub, so only chunks that exist but are unfinished are waited for.
  if c.seq > s.stt_next_seq and exists (
       select 1 from public.source_chunks x
        where x.session_id = c.session_id and x.seq >= s.stt_next_seq and x.seq < c.seq
          and not public._chunk_final(x.status)) then
    return jsonb_build_object('action', 'wait', 'reason', 'predecessor');
  end if;
  if s.stt_lease_until > now() and s.stt_lease_chunk is distinct from c.id then
    return jsonb_build_object('action', 'wait', 'reason', 'lease');
  end if;

  v_langs := public._active_languages(c.session_id);
  v_last := s.status = 'ended' and c.seq >= s.next_seq - 1;
  v_late := c.seq < s.stt_next_seq;
  if v_late then
    v_reason := 'skipped_stale';  -- arrived after the cursor moved on
  elsif (c.captured_at is not null and c.captured_at < now() - make_interval(secs => v_lag + c.duration_ms / 1000.0))
     or (s.next_seq - 1 - c.seq) * 5 > v_lag then
    v_reason := 'skipped_stale';
  elsif c.silent then
    v_reason := 'skipped_silent';
  elsif cardinality(v_langs) = 0 then
    v_reason := 'skipped_no_listeners';
  elsif public._over_cost_cap(s) then
    v_reason := 'failed';
    perform public.worker_log('error', 'stt', 'session cost cap reached; no further provider calls', c.id);
  end if;

  update public.sessions
     set stt_lease_until = now() + make_interval(secs => greatest(p_lease_sec, 10)),
         stt_lease_chunk = c.id,
         stt_next_seq = greatest(stt_next_seq, c.seq)
   where id = s.id;
  update public.source_chunks
     set status = 'processing',
         attempts = attempts + case when v_reason is null then 1 else 0 end,
         processing_started_at = coalesce(processing_started_at, now()),
         target_languages = v_langs
   where id = c.id
  returning * into c;

  if v_reason is null then
    select * into prev from public.source_chunks
     where session_id = c.session_id and seq = c.seq - 1 and status = 'done' and source_deleted_at is null;
    if found and (
         (prev.captured_at is null and c.captured_at is null)
         or abs(extract(epoch from (c.captured_at - prev.captured_at)) * 1000 - prev.duration_ms) < 600) then
      v_prev := jsonb_build_object('seq', prev.seq, 'path', prev.storage_path, 'durationMs', prev.duration_ms);
    end if;
  end if;

  return jsonb_build_object(
    'action', case when v_reason is null then 'process' else 'skip' end,
    'reason', v_reason,
    'chunk', jsonb_build_object('id', c.id, 'sessionId', c.session_id, 'hubId', c.hub_id, 'seq', c.seq,
                                'path', c.storage_path, 'durationMs', c.duration_ms, 'attempts', c.attempts),
    'prev', v_prev,
    'isLast', v_last,
    'late', v_late,  -- the cursor already moved past it: commit without touching the assembler state
    'state', s.stt_state,
    'languages', to_jsonb(v_langs),
    'sttModel', public._setting('stt_model', '"nova-3"'),
    'sttLanguage', public._setting('stt_language', '"multi"')
  );
end;
$$;

-- Turns assembled utterances into rows and one translation job per active language.
create or replace function public._emit_utterances(p_session_id uuid, p_utterances jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  s       public.sessions;
  v_langs text[] := public._active_languages(p_session_id);
  u       jsonb;
  v_uid   bigint;
  v_job   bigint;
  v_lang  text;
  v_utts  int := 0;
  v_jobs  int := 0;
begin
  select * into s from public.sessions where id = p_session_id for update;
  if cardinality(v_langs) = 0 or jsonb_typeof(p_utterances) is distinct from 'array' then
    return jsonb_build_object('utterances', 0, 'jobs', 0);
  end if;
  for u in select * from jsonb_array_elements(p_utterances) loop
    if coalesce(btrim(u ->> 'text'), '') = '' then
      continue;
    end if;
    update public.sessions set utterance_seq = utterance_seq + 1,
                               source_language = coalesce(u ->> 'language', source_language)
     where id = p_session_id
    returning * into s;
    insert into public.utterances (session_id, hub_id, seq, text, language, start_seq, start_ms, end_seq, end_ms, word_count)
    values (p_session_id, s.hub_id, s.utterance_seq, left(u ->> 'text', 4000), nullif(u ->> 'language', ''),
            (u ->> 'startSeq')::int, (u ->> 'startMs')::int, (u ->> 'endSeq')::int, (u ->> 'endMs')::int,
            coalesce((u ->> 'words')::int, 0))
    returning id into v_uid;
    v_utts := v_utts + 1;
    foreach v_lang in array v_langs loop
      insert into public.translation_jobs (utterance_id, session_id, hub_id, language_code)
      values (v_uid, p_session_id, s.hub_id, v_lang)
      on conflict (utterance_id, language_code) do nothing
      returning id into v_job;
      if v_job is not null then
        perform pgmq.send('tts_jobs', jsonb_build_object('job_id', v_job));
        v_jobs := v_jobs + 1;
      end if;
    end loop;
  end loop;
  -- One worker invocation per new job so languages are produced in parallel.
  for i in 1 .. least(v_jobs, 8) loop
    perform public._invoke_function('process-translation', jsonb_build_object('reason', 'utterance'));
  end loop;
  return jsonb_build_object('utterances', v_utts, 'jobs', v_jobs);
end;
$$;

create or replace function public.worker_stt_commit(p_chunk_id bigint,
                                                    p_msg_id bigint,
                                                    p_status text,
                                                    p_state jsonb,
                                                    p_transcript jsonb default null,
                                                    p_utterances jsonb default '[]'::jsonb,
                                                    p_error text default null)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  c      public.source_chunks;
  v_emit jsonb := jsonb_build_object('utterances', 0, 'jobs', 0);
begin
  if not public._chunk_final(p_status) then
    raise exception 'invalid final status %', p_status using errcode = '22023';
  end if;
  select * into c from public.source_chunks where id = p_chunk_id for update;
  if not found then
    raise exception 'unknown chunk %', p_chunk_id using errcode = 'P0002';
  end if;
  perform 1 from public.sessions where id = c.session_id for update;

  if not public._chunk_final(c.status) then  -- a duplicate delivery commits nothing twice
    if p_transcript is not null then
      insert into public.transcripts (chunk_id, provider, model, text, confidence, detected_language, words,
                                      preroll_ms, request_id, committed, meta)
      values (c.id, coalesce(p_transcript ->> 'provider', 'deepgram'), p_transcript ->> 'model',
              coalesce(p_transcript ->> 'text', ''), (p_transcript ->> 'confidence')::real,
              p_transcript ->> 'language', coalesce(p_transcript -> 'words', '[]'::jsonb),
              coalesce((p_transcript ->> 'prerollMs')::int, 0), p_transcript ->> 'requestId',
              p_transcript ->> 'committed', coalesce(p_transcript -> 'meta', '{}'::jsonb))
      on conflict (chunk_id) do nothing;
    end if;
    v_emit := public._emit_utterances(c.session_id, p_utterances);
    update public.sessions
       set stt_state = coalesce(p_state, stt_state),
           stt_next_seq = greatest(stt_next_seq, c.seq + 1),
           stt_lease_until = null,
           stt_lease_chunk = null
     where id = c.session_id;
    update public.source_chunks
       set status = p_status, processed_at = now(), last_error = left(p_error, 1000)
     where id = c.id;
    -- Wake the worker for the next chunk of this session if it is already waiting.
    if exists (select 1 from public.source_chunks where session_id = c.session_id and seq > c.seq
                                                   and not public._chunk_final(status)) then
      perform public._invoke_function('process-chunk', jsonb_build_object('reason', 'next'));
    end if;
  end if;
  if p_msg_id is not null then
    perform pgmq.archive('chunk_jobs', p_msg_id);
  end if;
  return v_emit;
end;
$$;

-- A retryable provider failure: release the lease and redeliver after p_delay_sec.
create or replace function public.worker_stt_retry(p_chunk_id bigint, p_msg_id bigint, p_delay_sec int, p_error text)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  c public.source_chunks;
begin
  select * into c from public.source_chunks where id = p_chunk_id for update;
  if found then
    update public.source_chunks set last_error = left(p_error, 1000) where id = c.id;
    update public.sessions set stt_lease_until = null, stt_lease_chunk = null
     where id = c.session_id and stt_lease_chunk = c.id;
  end if;
  if p_msg_id is not null then
    perform pgmq.set_vt('chunk_jobs', p_msg_id, greatest(1, least(p_delay_sec, 300)));
  end if;
end;
$$;

-- Session end: emit the sentence still being assembled once every chunk is finished.
create or replace function public.worker_stt_flush_begin(p_session_id uuid, p_lease_sec int default 30)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  s public.sessions;
begin
  select * into s from public.sessions where id = p_session_id for update;
  if not found or s.flushed_at is not null then
    return jsonb_build_object('action', 'final');
  end if;
  if exists (select 1 from public.source_chunks where session_id = p_session_id and not public._chunk_final(status))
     or s.stt_lease_until > now() then
    return jsonb_build_object('action', 'wait');
  end if;
  update public.sessions set stt_lease_until = now() + make_interval(secs => p_lease_sec), stt_lease_chunk = null
   where id = p_session_id;
  return jsonb_build_object('action', 'process', 'state', s.stt_state, 'nextSeq', s.stt_next_seq,
                            'languages', to_jsonb(public._active_languages(p_session_id)));
end;
$$;

create or replace function public.worker_stt_flush_commit(p_session_id uuid, p_msg_id bigint, p_utterances jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_emit jsonb := jsonb_build_object('utterances', 0, 'jobs', 0);
  s      public.sessions;
begin
  select * into s from public.sessions where id = p_session_id for update;
  if found and s.flushed_at is null then
    v_emit := public._emit_utterances(p_session_id, p_utterances);
    update public.sessions set flushed_at = now(), stt_state = '{}'::jsonb, stt_lease_until = null
     where id = p_session_id;
  end if;
  if p_msg_id is not null then
    perform pgmq.archive('chunk_jobs', p_msg_id);
  end if;
  return v_emit;
end;
$$;

-- ---------------------------------------------------------------------------------------------
-- Translation + TTS stage (process-translation)
-- ---------------------------------------------------------------------------------------------

create or replace function public.worker_claim_tts(p_qty int default 2, p_vt int default 90)
returns table (msg_id bigint, read_ct int, job_id bigint)
language sql
security definer
set search_path = ''
as $$
  select m.msg_id, m.read_ct, (m.message ->> 'job_id')::bigint
  from pgmq.read('tts_jobs', p_vt, least(greatest(p_qty, 1), 8)) m
$$;

-- action: process | skip (reason = final job status) | final
create or replace function public.worker_tts_begin(p_job_id bigint)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  j        public.translation_jobs;
  u        public.utterances;
  s        public.sessions;
  l        public.languages;
  v_reason text;
  v_method text;
  v_age    numeric := (public._setting('max_utterance_age_seconds', '60'))::text::numeric;
  v_ctx    jsonb;
  v_chunks jsonb := '[]'::jsonb;
begin
  select * into j from public.translation_jobs where id = p_job_id for update;
  if not found then
    return jsonb_build_object('action', 'final', 'reason', 'missing');
  end if;
  if j.status in ('ready', 'skipped_no_listeners', 'skipped_stale', 'failed') then
    return jsonb_build_object('action', 'final', 'reason', j.status);
  end if;
  select * into u from public.utterances where id = j.utterance_id;
  select * into s from public.sessions where id = j.session_id;
  select * into l from public.languages where code = j.language_code;

  -- Demand is checked again right before any provider call: the last listener may have left.
  if not (j.language_code = any (public._active_languages(j.session_id))) then
    v_reason := 'skipped_no_listeners';
  elsif u.created_at < now() - make_interval(secs => v_age) then
    v_reason := 'skipped_stale';
  elsif public._over_cost_cap(s) then
    v_reason := 'failed';
  end if;

  -- The student chose the language the lecturer is speaking: send the lecturer's own voice.
  v_method := case when u.language is not null and u.language = j.language_code then 'passthrough' else 'tts' end;
  if v_reason is null and v_method = 'passthrough' then
    select coalesce(jsonb_agg(jsonb_build_object('seq', c.seq, 'path', c.storage_path, 'durationMs', c.duration_ms)
                              order by c.seq), '[]'::jsonb)
      into v_chunks
      from public.source_chunks c
     where c.session_id = u.session_id and c.seq between u.start_seq and u.end_seq and c.source_deleted_at is null;
    if jsonb_array_length(v_chunks) <> u.end_seq - u.start_seq + 1 then
      v_method := 'tts';  -- source audio incomplete: speak the transcript instead
      v_chunks := '[]'::jsonb;
    end if;
  end if;

  if v_reason is null then
    update public.translation_jobs
       set status = 'processing', attempts = attempts + 1, started_at = coalesce(started_at, now()), method = v_method
     where id = j.id
    returning * into j;
  end if;

  select coalesce(jsonb_agg(jsonb_build_object('source', p.text, 'target', pj.translated_text) order by p.seq), '[]'::jsonb)
    into v_ctx
    from (select * from public.utterances
           where session_id = u.session_id and seq < u.seq and seq >= u.seq - 3 and text is not null) p
    left join public.translation_jobs pj on pj.utterance_id = p.id and pj.language_code = j.language_code;

  return jsonb_build_object(
    'action', case when v_reason is null then 'process' else 'skip' end,
    'reason', v_reason,
    'method', v_method,
    'job', jsonb_build_object('id', j.id, 'attempts', j.attempts, 'translatedText', j.translated_text,
                              'language', j.language_code),
    'utterance', jsonb_build_object('id', u.id, 'seq', u.seq, 'text', u.text, 'language', u.language,
                                    'startSeq', u.start_seq, 'startMs', u.start_ms, 'endSeq', u.end_seq,
                                    'endMs', u.end_ms),
    'session', jsonb_build_object('id', s.id, 'hubId', s.hub_id),
    'language', jsonb_build_object('code', l.code, 'name', l.name, 'ttsModel', l.tts_model, 'ttsVoice', l.tts_voice),
    'sourceLanguageName', (select name from public.languages where code = u.language),
    'context', v_ctx,
    'chunks', v_chunks,
    'translateModel', public._setting('translate_model', '"gemini-3.8-flash"'),
    'translateThinking', public._setting('translate_thinking', '"low"')
  );
end;
$$;

-- Stored before TTS, so a TTS retry never pays for the translation again.
create or replace function public.worker_tts_save_translation(p_job_id bigint, p_text text)
returns void
language sql
security definer
set search_path = ''
as $$
  update public.translation_jobs set translated_text = left(p_text, 8000)
   where id = p_job_id and translated_text is null
$$;

create or replace function public.worker_tts_commit(p_job_id bigint,
                                                    p_msg_id bigint,
                                                    p_status text,
                                                    p_path text default null,
                                                    p_bytes int default null,
                                                    p_duration_ms int default null,
                                                    p_error text default null)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
  j     public.translation_jobs;
  u     public.utterances;
  v_seg bigint;
begin
  if p_status not in ('ready', 'skipped_no_listeners', 'skipped_stale', 'failed') then
    raise exception 'invalid final status %', p_status using errcode = '22023';
  end if;
  select * into j from public.translation_jobs where id = p_job_id for update;
  if not found then
    raise exception 'unknown job %', p_job_id using errcode = 'P0002';
  end if;
  if j.status not in ('ready', 'skipped_no_listeners', 'skipped_stale', 'failed') then
    select * into u from public.utterances where id = j.utterance_id;
    if p_status = 'ready' then
      if p_path is null or p_path not like j.hub_id::text || '/' || j.session_id::text || '/%' then
        raise exception 'segment path outside the session folder' using errcode = '22023';
      end if;
      insert into public.segments (utterance_id, session_id, hub_id, language_code, seq, chunk_seq, status, mode,
                                   text, storage_path, bytes, duration_ms, mime, ready_at)
      values (j.utterance_id, j.session_id, j.hub_id, j.language_code, u.seq, u.end_seq, 'ready', 'ai',
              j.translated_text, p_path, p_bytes, p_duration_ms, 'audio/wav', now())
      on conflict (utterance_id, language_code) do nothing
      returning id into v_seg;
      if v_seg is not null then
        update public.language_streams set segments_produced = segments_produced + 1
         where session_id = j.session_id and language_code = j.language_code;
        perform public._notify_hub(j.hub_id, 'segment_ready',
                                   jsonb_build_object('sessionId', j.session_id, 'segmentId', v_seg,
                                                      'lang', j.language_code, 'seq', u.seq));
      end if;
    end if;
    update public.translation_jobs
       set status = p_status, finished_at = now(), last_error = left(p_error, 1000)
     where id = j.id;
  end if;
  if p_msg_id is not null then
    perform pgmq.archive('tts_jobs', p_msg_id);
  end if;
  return v_seg;
end;
$$;

create or replace function public.worker_tts_retry(p_job_id bigint, p_msg_id bigint, p_delay_sec int, p_error text)
returns void
language plpgsql
security definer
set search_path = ''
as $$
begin
  update public.translation_jobs set status = 'queued', last_error = left(p_error, 1000)
   where id = p_job_id and status = 'processing';
  if p_msg_id is not null then
    perform pgmq.set_vt('tts_jobs', p_msg_id, greatest(1, least(p_delay_sec, 300)));
  end if;
end;
$$;

-- ---------------------------------------------------------------------------------------------
-- Delivery to the Hub: in order per language
-- ---------------------------------------------------------------------------------------------

-- Same contract as before, plus: segments are returned in speaking order, and a translated
-- segment is held back while an earlier utterance of the same language is still being produced
-- (at most 15 s, so one slow or failed job cannot stall a stream). The Hub's cursor is the
-- largest id returned, so only a gap-free prefix (by id) of the candidates is returned.
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
      with cand as (
        select s.id, s.language_code as lang, s.seq, coalesce(s.chunk_seq, s.seq) as chunk_seq, s.storage_path as path,
               s.bytes, s.duration_ms,
               s.utterance_id is not null and exists (
                 select 1 from public.translation_jobs j
                   join public.utterances u on u.id = j.utterance_id
                  where j.session_id = s.session_id and j.language_code = s.language_code
                    and j.status in ('queued', 'processing') and u.seq < s.seq
                    and j.created_at > now() - interval '15 seconds') as blocked
          from public.segments s
         where s.session_id = p_session_id
           and s.status = 'ready'
           and s.deleted_at is null
           and s.id > coalesce(p_after_segment_id, 0)
           and s.language_code = any (coalesce(p_languages, '{}'))
         order by s.id
         limit 24
      ), blk as (
        select min(id) as first_blocked from cand where blocked
      )
      select coalesce(jsonb_agg(jsonb_build_object(
               'id', c.id, 'lang', c.lang, 'seq', c.seq, 'chunkSeq', c.chunk_seq, 'path', c.path,
               'bytes', c.bytes, 'durationMs', c.duration_ms) order by c.seq, c.id), '[]'::jsonb)
        from cand c, blk
       where blk.first_blocked is null or c.id < blk.first_blocked
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
-- Scheduling, retention, reporting
-- ---------------------------------------------------------------------------------------------

create or replace function public.wake_worker_if_pending()
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_woke boolean := false;
begin
  if exists (select 1 from pgmq.q_chunk_jobs where vt <= now() limit 1) then
    perform public._invoke_function('process-chunk', jsonb_build_object('reason', 'sweep'));
    v_woke := true;
  end if;
  if exists (select 1 from pgmq.q_tts_jobs where vt <= now() limit 1) then
    perform public._invoke_function('process-translation', jsonb_build_object('reason', 'sweep'));
    v_woke := true;
  end if;
  return v_woke;
end;
$$;

-- Lecture text is temporary too: erased after text_retention_hours (audio has its own, shorter
-- retention in worker_cleanup_candidates). Usage/cost rows keep no text.
create or replace function public.purge_lecture_text()
returns int
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_cutoff timestamptz := now() - make_interval(hours => (public._setting('text_retention_hours', '24'))::text::int);
  n int;
begin
  update public.transcripts set text = '', words = '[]'::jsonb, committed = null
   where created_at < v_cutoff and (text <> '' or committed is not null);
  update public.utterances set text = null, text_purged_at = now()
   where created_at < v_cutoff and text_purged_at is null;
  get diagnostics n = row_count;
  update public.translation_jobs set translated_text = null
   where created_at < v_cutoff and translated_text is not null;
  update public.segments set text = null where created_at < v_cutoff and text is not null;
  update public.sessions set stt_state = '{}'::jsonb
   where status = 'ended' and ended_at < v_cutoff and stt_state <> '{}'::jsonb;
  return n;
end;
$$;

select cron.schedule('elingo-purge-text', '17 * * * *', 'select public.purge_lecture_text()');

-- Cost and usage per session and stage (service role / dashboard only).
create view public.session_usage with (security_invoker = true) as
select e.session_id,
       e.provider,
       e.operation,
       e.model,
       e.language_code,
       e.unit_type,
       count(*) filter (where e.status = 'ok')    as requests,
       count(*) filter (where e.status = 'error') as errors,
       count(*) filter (where e.attempt > 1)      as retries,
       sum(e.units)                               as units,
       sum(e.est_cost_usd)                        as est_cost_usd,
       round(avg(e.latency_ms))                   as avg_latency_ms
  from public.usage_events e
 group by 1, 2, 3, 4, 5, 6;
revoke all on public.session_usage from anon, authenticated;

create view public.session_summary with (security_invoker = true) as
select s.id as session_id,
       s.hub_id,
       s.status,
       s.started_at,
       s.ended_at,
       s.cost_usd,
       (select count(*) from public.source_chunks c where c.session_id = s.id)                         as chunks,
       (select count(*) from public.source_chunks c where c.session_id = s.id and c.status = 'done')   as chunks_transcribed,
       (select coalesce(sum(c.duration_ms), 0) / 1000.0 from public.source_chunks c
         where c.session_id = s.id and c.status = 'done')                                              as audio_seconds_transcribed,
       (select count(*) from public.utterances u where u.session_id = s.id)                            as utterances,
       (select count(distinct j.language_code) from public.translation_jobs j where j.session_id = s.id) as languages_served,
       (select count(*) from public.translation_jobs j where j.session_id = s.id and j.status = 'ready') as segments_ready,
       (select count(*) from public.translation_jobs j where j.session_id = s.id and j.status = 'failed') as segments_failed,
       (select coalesce(sum(g.duration_ms), 0) / 1000.0 from public.segments g
         where g.session_id = s.id and g.mode = 'ai')                                                  as generated_audio_seconds,
       (select round(avg(extract(epoch from (j.finished_at - u.created_at)) * 1000))
          from public.translation_jobs j join public.utterances u on u.id = j.utterance_id
         where j.session_id = s.id and j.status = 'ready')                                             as avg_utterance_to_audio_ms
  from public.sessions s;
revoke all on public.session_summary from anon, authenticated;

-- ---------------------------------------------------------------------------------------------
-- Grants
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

grant execute on function public.worker_claim_stt(int, int)                              to service_role;
grant execute on function public.worker_queue_delay(text, bigint, int)                   to service_role;
grant execute on function public.worker_queue_archive(text, bigint)                      to service_role;
grant execute on function public.worker_stt_begin(bigint, int)                           to service_role;
grant execute on function public.worker_stt_commit(bigint, bigint, text, jsonb, jsonb, jsonb, text) to service_role;
grant execute on function public.worker_stt_retry(bigint, bigint, int, text)             to service_role;
grant execute on function public.worker_stt_flush_begin(uuid, int)                       to service_role;
grant execute on function public.worker_stt_flush_commit(uuid, bigint, jsonb)            to service_role;
grant execute on function public.worker_claim_tts(int, int)                              to service_role;
grant execute on function public.worker_tts_begin(bigint)                                to service_role;
grant execute on function public.worker_tts_save_translation(bigint, text)               to service_role;
grant execute on function public.worker_tts_commit(bigint, bigint, text, text, int, int, text) to service_role;
grant execute on function public.worker_tts_retry(bigint, bigint, int, text)             to service_role;
grant execute on function public.worker_usage(uuid, bigint, bigint, text, text, text, text, jsonb, text, int, int, text, text, jsonb) to service_role;
grant execute on function public.purge_lecture_text()                                    to service_role;
-- Milestone 1 worker functions (loopback mode, cleanup) keep their grants.
grant execute on function public.worker_claim(int, int)                                  to service_role;
grant execute on function public.worker_begin_chunk(bigint)                              to service_role;
grant execute on function public.worker_language_active(uuid, text)                      to service_role;
grant execute on function public.worker_segment_ready(bigint, text, text, int, int, text, text) to service_role;
grant execute on function public.worker_finish_chunk(bigint, bigint, text, text)         to service_role;
grant execute on function public.worker_log(text, text, text, bigint, text, jsonb)       to service_role;
grant execute on function public.worker_record_usage(bigint, text, text, text, text, numeric, text, text, jsonb) to service_role;
grant execute on function public.worker_cleanup_candidates(int)                          to service_role;
grant execute on function public.worker_mark_deleted(bigint[], bigint[])                 to service_role;
grant select on public.session_usage, public.session_summary to service_role;
