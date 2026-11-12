-- eLingo Hub - Milestone 2: original-audio fallback
--
-- When Gemini cannot translate or speak an utterance (rate limit, quota, outage, bad response),
-- every listener of that language gets the lecturer's original audio for it instead of
-- silence. A rate-limited model is put on a shared cool-down so no further requests are sent to
-- it until the provider's retry time has passed (free-tier prototype: 5 requests/min).

create table public.provider_cooldowns (
  key        text primary key,            -- e.g. 'gemini:gemini-3.8-flash'
  until      timestamptz not null,
  reason     text,
  updated_at timestamptz not null default now()
);
alter table public.provider_cooldowns enable row level security;
revoke all on public.provider_cooldowns from anon, authenticated;

create or replace function public.worker_provider_cooldown(p_key text, p_seconds int, p_reason text default null)
returns timestamptz
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_until timestamptz;
begin
  insert into public.provider_cooldowns as c (key, until, reason)
  values (left(p_key, 120), now() + make_interval(secs => greatest(1, least(p_seconds, 3600))), left(p_reason, 500))
  on conflict (key) do update
    set until = greatest(c.until, excluded.until), reason = excluded.reason, updated_at = now()
  returning until into v_until;
  return v_until;
end;
$$;

alter table public.translation_jobs drop constraint translation_jobs_method_check;
alter table public.translation_jobs add constraint translation_jobs_method_check
  check (method in ('tts', 'passthrough', 'fallback'));

-- worker_tts_begin: also returns the source chunks for every job (needed for the fallback) and
-- the cool-down state of the two Gemini models the job would use.
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
  v_chunks jsonb;
  v_complete boolean;
  v_tmodel text := public._setting('translate_model', '"gemini-3.8-flash"') #>> '{}';
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
  end if;

  select coalesce(jsonb_agg(jsonb_build_object('seq', c.seq, 'path', c.storage_path, 'durationMs', c.duration_ms)
                            order by c.seq), '[]'::jsonb)
    into v_chunks
    from public.source_chunks c
   where c.session_id = u.session_id and c.seq between u.start_seq and u.end_seq and c.source_deleted_at is null;
  v_complete := jsonb_array_length(v_chunks) = u.end_seq - u.start_seq + 1;

  -- The student chose the language the lecturer is speaking: send the lecturer's own voice.
  -- Over the session cost cap: no more provider calls, original audio only.
  v_method := case
    when u.language is not null and u.language = j.language_code and v_complete then 'passthrough'
    when public._over_cost_cap(s) then 'fallback'
    else 'tts' end;

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
    'chunksComplete', v_complete,
    'translateModel', v_tmodel,
    'translateThinking', public._setting('translate_thinking', '"low"'),
    'cooldowns', jsonb_build_object(
      'translate', (select until from public.provider_cooldowns where key = 'gemini:' || v_tmodel and until > now()),
      'tts',       (select until from public.provider_cooldowns where key = 'gemini:' || l.tts_model and until > now()))
  );
end;
$$;

-- worker_tts_commit gains p_mode: 'ai' (translated), 'passthrough' (lecturer's language) or
-- 'fallback' (original audio because the translation failed). The signature changes, so the
-- old function is dropped (an overload would make named calls ambiguous).
drop function public.worker_tts_commit(bigint, bigint, text, text, int, int, text);
create function public.worker_tts_commit(p_job_id bigint,
                                         p_msg_id bigint,
                                         p_status text,
                                         p_path text default null,
                                         p_bytes int default null,
                                         p_duration_ms int default null,
                                         p_error text default null,
                                         p_mode text default 'ai')
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
  if coalesce(p_mode, 'ai') not in ('ai', 'passthrough', 'fallback') then
    raise exception 'invalid mode %', p_mode using errcode = '22023';
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
      values (j.utterance_id, j.session_id, j.hub_id, j.language_code, u.seq, u.end_seq, 'ready', coalesce(p_mode, 'ai'),
              case when coalesce(p_mode, 'ai') = 'ai' then j.translated_text end,
              p_path, p_bytes, p_duration_ms, 'audio/wav', now())
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
       set status = p_status, finished_at = now(), last_error = left(p_error, 1000),
           method = case when p_status = 'ready' and p_mode = 'fallback' then 'fallback' else method end
     where id = j.id;
  end if;
  if p_msg_id is not null then
    perform pgmq.archive('tts_jobs', p_msg_id);
  end if;
  return v_seg;
end;
$$;

-- hub_poll: unchanged contract plus "original": true for fallback segments, so the student page
-- can say that the translation is unavailable for that part.
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
               s.bytes, s.duration_ms, s.mode = 'fallback' as original,
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
               'bytes', c.bytes, 'durationMs', c.duration_ms, 'original', c.original) order by c.seq, c.id), '[]'::jsonb)
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

-- Fallback share per session (how much of the lecture students heard untranslated).
create or replace view public.session_summary with (security_invoker = true) as
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
         where j.session_id = s.id and j.status = 'ready')                                             as avg_utterance_to_audio_ms,
       (select count(*) from public.segments g where g.session_id = s.id and g.mode = 'fallback')      as segments_original_fallback
  from public.sessions s;
revoke all on public.session_summary from anon, authenticated;
grant select on public.session_summary to service_role;

revoke all on function public.worker_provider_cooldown(text, int, text) from public, anon, authenticated;
revoke all on function public.worker_tts_commit(bigint, bigint, text, text, int, int, text, text) from public, anon, authenticated;
revoke all on function public.worker_tts_begin(bigint) from public, anon, authenticated;
revoke all on function public.hub_poll(uuid, bigint, text[]) from public, anon;
grant execute on function public.worker_provider_cooldown(text, int, text)                         to service_role;
grant execute on function public.worker_tts_commit(bigint, bigint, text, text, int, int, text, text) to service_role;
grant execute on function public.worker_tts_begin(bigint)                                          to service_role;
grant execute on function public.hub_poll(uuid, bigint, text[])                                    to authenticated;
