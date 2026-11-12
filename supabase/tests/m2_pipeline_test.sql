-- Milestone 2 database tests: demand-driven AI pipeline.
--
-- Run (everything is rolled back; nothing persists, no function is invoked):
--   supabase db query --remote --yes -f supabase/tests/m2_pipeline_test.sql
-- before the migration is applied, prepend it:  tools/run-sql-tests.mjs does both.
-- Output: TAP lines, one per assertion.

create or replace function pg_temp.tap(cond boolean, label text) returns text language sql as $$
  select case when coalesce(cond, false) then 'ok' else 'not ok' end || ' - ' || label
$$;

create or replace function pg_temp.as_hub(p_user uuid) returns void language plpgsql as $$
begin
  perform set_config('role', 'authenticated', true);
  perform set_config('request.jwt.claims', json_build_object('sub', p_user, 'role', 'authenticated')::text, true);
end;
$$;

create or replace function pg_temp.as_admin() returns void language plpgsql as $$
begin
  perform set_config('role', 'none', true);  -- back to the session user (a hub may not SET ROLE postgres)
  perform set_config('request.jwt.claims', '', true);
end;
$$;

-- The hub/anon roles must be able to call the helpers above while the tests run as them.
do $$ begin
  execute format('grant usage on schema %I to authenticated, anon', pg_my_temp_schema()::regnamespace::text);
end $$;

create or replace function pg_temp.m2_tests()
returns setof text
language plpgsql
as $$
declare
  t       text[] := '{}';
  user_a  uuid := gen_random_uuid();  hub_a uuid;
  user_b  uuid := gen_random_uuid();  hub_b uuid;
  sess    uuid;  sess_b uuid;
  r       jsonb;
  c1 bigint; c2 bigint; c3 bigint; c4 bigint; c5 bigint;
  j_af bigint; j_st bigint; j2 bigint; j3 bigint;
  u1 bigint;
  n int;
  ok_flag boolean;
  v_cost numeric;
  seg_path text;
  v_after bigint;
begin
  -- ------------------------------------------------------------------ fixtures
  insert into auth.users (id, email, aud, role, created_at, updated_at)
  values (user_a, 'm2-hub-a@test.elingo', 'authenticated', 'authenticated', now(), now()),
         (user_b, 'm2-hub-b@test.elingo', 'authenticated', 'authenticated', now(), now());
  insert into public.hubs (auth_user_id, name) values (user_a, 'M2 Hub A') returning id into hub_a;
  insert into public.hubs (auth_user_id, name) values (user_b, 'M2 Hub B') returning id into hub_b;

  perform pg_temp.as_hub(user_a);
  sess := (public.hub_start_session('m2nonce-a') ->> 'id')::uuid;
  -- Students: A and B choose Afrikaans, C chooses Sesotho.
  r := public.hub_sync(sess, '[{"id":"listenerA001","lang":"af"},{"id":"listenerB001","lang":"af"},{"id":"listenerC001","lang":"st"}]');
  perform pg_temp.as_hub(user_b);
  sess_b := (public.hub_start_session('m2nonce-b') ->> 'id')::uuid;
  perform pg_temp.as_admin();

  t := t || pg_temp.tap((select listener_count from public.language_streams where session_id = sess and language_code = 'af') = 2
                        and (select listener_count from public.language_streams where session_id = sess and language_code = 'st') = 1,
                        'listener counts: af=2, st=1');

  -- Upload + register chunks 1..5 (chunk 4 silent, chunk 5 captured long ago).
  insert into storage.objects (bucket_id, name, owner_id, metadata)
  select 'source-audio', hub_a::text || '/' || sess::text || '/' || lpad(g::text, 6, '0') || '.wav', user_a::text, '{"size": 40060}'::jsonb
    from generate_series(1, 5) g;
  perform pg_temp.as_hub(user_a);
  c1 := (public.hub_register_chunk(sess, 1, 5000, 900, 12000, false, now()) ->> 'chunkId')::bigint;
  -- captured_at 5 s after chunk 1: a contiguous recording (now() is fixed inside a transaction)
  c2 := (public.hub_register_chunk(sess, 2, 5000, 900, 12000, false, now() + interval '5 seconds') ->> 'chunkId')::bigint;
  r := public.hub_register_chunk(sess, 2, 5000, 900, 12000, false, now() + interval '5 seconds');  -- duplicate upload
  t := t || pg_temp.tap((r ->> 'duplicate')::boolean and (r ->> 'chunkId')::bigint = c2, 'duplicate chunk registration returns the same chunk');
  perform pg_temp.as_admin();
  t := t || pg_temp.tap((select count(*) from pgmq.q_chunk_jobs where (message ->> 'chunk_id')::bigint = c2) = 1,
                        'duplicate chunk creates no second processing job');

  -- ------------------------------------------------------------------ STT ordering + demand
  r := public.worker_stt_begin(c1);
  t := t || pg_temp.tap(r ->> 'action' = 'process' and r -> 'languages' = '["af", "st"]'::jsonb, 'chunk 1: transcribe for af and st');
  r := public.worker_stt_begin(c2);
  t := t || pg_temp.tap(r ->> 'action' = 'wait' and r ->> 'reason' = 'predecessor', 'chunk 2 waits for chunk 1 (order)');

  r := public.worker_stt_commit(c1, null, 'done', '{"prevSeq":1,"held":[],"carry":[]}'::jsonb,
         '{"provider":"deepgram","model":"nova-3","text":"Energy is conserved.","words":[]}'::jsonb,
         '[{"text":"Energy is conserved.","language":"en","startSeq":1,"startMs":200,"endSeq":1,"endMs":1500,"words":3}]'::jsonb);
  t := t || pg_temp.tap((r ->> 'utterances')::int = 1 and (r ->> 'jobs')::int = 2, 'one utterance -> exactly one job per active language');
  t := t || pg_temp.tap((select count(*) from public.translation_jobs where session_id = sess and language_code = 'af') = 1,
                        'two Afrikaans listeners share one Afrikaans job');
  select id into u1 from public.utterances where session_id = sess and seq = 1;
  select id into j_af from public.translation_jobs where utterance_id = u1 and language_code = 'af';
  select id into j_st from public.translation_jobs where utterance_id = u1 and language_code = 'st';
  t := t || pg_temp.tap((select count(*) from pgmq.q_tts_jobs where (message ->> 'job_id')::bigint in (j_af, j_st)) = 2, 'two TTS messages queued');

  r := public.worker_stt_commit(c1, null, 'done', '{}'::jsonb, null,
         '[{"text":"Energy is conserved.","language":"en","startSeq":1,"startMs":200,"endSeq":1,"endMs":1500}]'::jsonb);
  t := t || pg_temp.tap((r ->> 'jobs')::int = 0 and (select count(*) from public.utterances where session_id = sess) = 1,
                        'duplicate STT commit creates nothing');
  t := t || pg_temp.tap((select stt_next_seq from public.sessions where id = sess) = 2
                        and (select stt_lease_chunk from public.sessions where id = sess) is null, 'cursor advanced, lease released');
  r := public.worker_stt_begin(c2);
  t := t || pg_temp.tap(r ->> 'action' = 'process' and r -> 'prev' ->> 'seq' = '1', 'chunk 2 runs next, with chunk 1 as pre-roll');
  r := public.worker_stt_commit(c2, null, 'done', '{"prevSeq":2}'::jsonb, null, '[]'::jsonb);

  -- ------------------------------------------------------------------ TTS demand + idempotency
  r := public.worker_tts_begin(j_af);
  t := t || pg_temp.tap(r ->> 'action' = 'process' and r ->> 'method' = 'tts' and r -> 'language' ->> 'ttsModel' = 'gemini-3.8-flash-lite-tts',
                        'af job: translate + Flash-Lite TTS');
  t := t || pg_temp.tap((select tts_model from public.languages where code = 'st') = 'gemini-3.8-flash-tts', 'Sesotho uses 3.8 Flash TTS (Lite lacks it)');

  -- The Sesotho student leaves before the st job runs.
  perform pg_temp.as_hub(user_a);
  r := public.hub_sync(sess, '[{"id":"listenerA001","lang":"af"},{"id":"listenerB001","lang":"af"}]');
  perform pg_temp.as_admin();
  r := public.worker_tts_begin(j_st);
  t := t || pg_temp.tap(r ->> 'action' = 'skip' and r ->> 'reason' = 'skipped_no_listeners', 'final listener left: st job skipped before any provider call');
  perform public.worker_tts_commit(j_st, null, 'skipped_no_listeners');

  seg_path := hub_a::text || '/' || sess::text || '/af/u000001.wav';
  begin
    perform public.worker_tts_commit(j_af, null, 'ready', hub_b::text || '/x/af/u1.wav', 1000, 1500);
    ok_flag := false;
  exception when others then ok_flag := sqlstate = '22023'; end;
  t := t || pg_temp.tap(ok_flag, 'a segment path outside the session folder is refused');
  perform public.worker_tts_save_translation(j_af, 'Energie word behou.');
  perform public.worker_tts_save_translation(j_af, 'overwritten?');
  t := t || pg_temp.tap((select translated_text from public.translation_jobs where id = j_af) = 'Energie word behou.', 'stored translation is never overwritten (no re-billing)');
  perform public.worker_tts_commit(j_af, null, 'ready', seg_path, 9000, 1500);
  perform public.worker_tts_commit(j_af, null, 'ready', seg_path, 9000, 1500);
  t := t || pg_temp.tap((select count(*) from public.segments where utterance_id = u1) = 1, 'duplicate TTS commit -> one segment');

  -- ------------------------------------------------------------------ delivery
  perform pg_temp.as_hub(user_a);
  r := public.hub_poll(sess, 0, '{af}');
  t := t || pg_temp.tap(jsonb_array_length(r -> 'segments') = 1 and r -> 'segments' -> 0 ->> 'chunkSeq' = '1'
                        and r -> 'segments' -> 0 ->> 'path' = seg_path, 'Hub receives the Afrikaans segment');
  r := public.hub_poll(sess, 0, '{st}');
  t := t || pg_temp.tap(jsonb_array_length(r -> 'segments') = 0, 'nothing produced for Sesotho');
  perform pg_temp.as_hub(user_b);
  begin
    perform public.hub_poll(sess, 0, '{af}');
    ok_flag := false;
  exception when others then ok_flag := sqlstate = 'P0002'; end;
  t := t || pg_temp.tap(ok_flag, 'another hub cannot poll this session (isolation)');
  t := t || pg_temp.tap((select count(*) from storage.objects where bucket_id = 'source-audio' and name like hub_a::text || '/%') = 0,
                        'another hub cannot see this hub''s audio objects');
  begin
    perform public.worker_stt_begin(c1);
    ok_flag := false;
  exception when others then ok_flag := sqlstate = '42501'; end;
  t := t || pg_temp.tap(ok_flag, 'a hub cannot call worker functions');
  perform pg_temp.as_admin();

  -- Ordering gate: utterance 3 is ready before utterance 2.
  perform public._emit_utterances(sess, '[{"text":"Two.","language":"en","startSeq":2,"startMs":0,"endSeq":2,"endMs":900},
                                         {"text":"Three.","language":"en","startSeq":2,"startMs":1000,"endSeq":2,"endMs":1900}]'::jsonb);
  select j.id into j2 from public.translation_jobs j join public.utterances u on u.id = j.utterance_id where u.session_id = sess and u.seq = 2;
  select j.id into j3 from public.translation_jobs j join public.utterances u on u.id = j.utterance_id where u.session_id = sess and u.seq = 3;
  t := t || pg_temp.tap(j2 is not null and j3 is not null and (select count(*) from public.translation_jobs where utterance_id in
                        (select id from public.utterances where session_id = sess and seq in (2, 3))) = 2,
                        'after the st listener left, new utterances get af jobs only');
  perform public.worker_tts_begin(j3);
  perform public.worker_tts_commit(j3, null, 'ready', hub_a::text || '/' || sess::text || '/af/u000003.wav', 9000, 900);
  select max(id) into v_after from public.segments where utterance_id = u1;
  perform pg_temp.as_hub(user_a);
  r := public.hub_poll(sess, v_after, '{af}');
  t := t || pg_temp.tap(jsonb_array_length(r -> 'segments') = 0, 'segment 3 is held back while segment 2 is in production');
  perform pg_temp.as_admin();
  perform public.worker_tts_begin(j2);
  perform public.worker_tts_commit(j2, null, 'ready', hub_a::text || '/' || sess::text || '/af/u000002.wav', 9000, 900);
  perform pg_temp.as_hub(user_a);
  r := public.hub_poll(sess, v_after, '{af}');
  t := t || pg_temp.tap(r -> 'segments' -> 0 ->> 'seq' = '2' and r -> 'segments' -> 1 ->> 'seq' = '3', 'then 2 and 3 arrive in speaking order');
  perform pg_temp.as_admin();

  -- ------------------------------------------------------------------ skips before any provider call
  perform pg_temp.as_hub(user_a);
  c3 := (public.hub_register_chunk(sess, 3, 5000, 900, 12000, false, now()) ->> 'chunkId')::bigint;
  c4 := (public.hub_register_chunk(sess, 4, 5000, 5, 30, true, now()) ->> 'chunkId')::bigint;
  c5 := (public.hub_register_chunk(sess, 5, 5000, 900, 12000, false, now() - interval '3 minutes') ->> 'chunkId')::bigint;
  r := public.hub_sync(sess, '[]');  -- everyone left
  perform pg_temp.as_admin();
  r := public.worker_stt_begin(c3);
  t := t || pg_temp.tap(r ->> 'action' = 'skip' and r ->> 'reason' = 'skipped_no_listeners', 'no listeners: chunk not transcribed');
  perform public.worker_stt_commit(c3, null, 'skipped_no_listeners', '{}'::jsonb, null, '[{"text":"lost.","startSeq":3,"startMs":0,"endSeq":3,"endMs":100}]'::jsonb);
  t := t || pg_temp.tap((select count(*) from public.utterances where session_id = sess and text = 'lost.') = 0, 'no listeners: no utterance, no job');
  perform pg_temp.as_hub(user_a);
  r := public.hub_sync(sess, '[{"id":"listenerA001","lang":"af"}]');
  perform pg_temp.as_admin();
  r := public.worker_stt_begin(c4);
  t := t || pg_temp.tap(r ->> 'reason' = 'skipped_silent', 'silent chunk: not transcribed');
  perform public.worker_stt_commit(c4, null, 'skipped_silent', '{}'::jsonb);
  r := public.worker_stt_begin(c5);
  t := t || pg_temp.tap(r ->> 'reason' = 'skipped_stale', 'chunk captured 3 min ago: not transcribed');
  perform public.worker_stt_commit(c5, null, 'skipped_stale', '{}'::jsonb);

  -- ------------------------------------------------------------------ cost tracking + cap
  v_cost := public.worker_usage(sess, c1, null, null, 'deepgram', 'transcribe', 'nova-3/multi',
                                '[{"unitType":"audio_minutes","units":1}]'::jsonb, 'ok', 800, 1, 'req-1');
  t := t || pg_temp.tap(v_cost = 0.0052 and (select cost_usd from public.sessions where id = sess) >= 0.0052, 'Deepgram minute priced at the rate card');
  v_cost := public.worker_usage(sess, null, u1, 'af', 'gemini', 'tts', 'gemini-3.8-flash-lite-tts',
                                '[{"unitType":"input_tokens","units":20},{"unitType":"audio_output_tokens","units":250}]'::jsonb);
  t := t || pg_temp.tap(round(v_cost, 8) = round(20 * 0.50e-6 + 250 * 6.00e-6, 8), 'TTS priced from audio tokens (10 s Flash-Lite = $0.0015)');
  t := t || pg_temp.tap((select requests from public.session_usage where session_id = sess and operation = 'tts' and unit_type = 'audio_output_tokens') = 1,
                        'usage visible per session and stage');
  update public.sessions set cost_usd = 1000 where id = sess;
  perform public._emit_utterances(sess, '[{"text":"Four.","language":"en","startSeq":5,"startMs":0,"endSeq":5,"endMs":500}]'::jsonb);
  r := public.worker_tts_begin((select j.id from public.translation_jobs j join public.utterances u on u.id = j.utterance_id
                                where u.session_id = sess and u.text = 'Four.'));
  t := t || pg_temp.tap(r ->> 'action' = 'process' and r ->> 'method' = 'fallback', 'session over the cost cap: original audio, no further provider calls');
  update public.sessions set cost_usd = 0 where id = sess;

  -- ------------------------------------------------------------------ original-audio fallback
  perform public.worker_provider_cooldown('gemini:gemini-3.8-flash', 30, 'free tier 5 rpm');
  perform public.worker_provider_cooldown('gemini:gemini-3.8-flash', 5, 'shorter hint');
  t := t || pg_temp.tap((select until from public.provider_cooldowns where key = 'gemini:gemini-3.8-flash') > now() + interval '25 seconds',
                        'a cool-down is never shortened by a later, shorter hint');
  perform public._emit_utterances(sess, '[{"text":"Five.","language":"en","startSeq":1,"startMs":0,"endSeq":1,"endMs":900}]'::jsonb);
  select j.id into j2 from public.translation_jobs j join public.utterances u on u.id = j.utterance_id
   where u.session_id = sess and u.text = 'Five.' and j.language_code = 'af';
  r := public.worker_tts_begin(j2);
  t := t || pg_temp.tap(r ->> 'method' = 'tts' and r -> 'cooldowns' ->> 'translate' is not null and r ->> 'chunksComplete' = 'true'
                        and jsonb_array_length(r -> 'chunks') = 1, 'job sees the cool-down and has the source audio for a fallback');
  perform public.worker_tts_commit(j2, null, 'ready', hub_a::text || '/' || sess::text || '/af/u000099.wav', 5000, 1200, 'original audio: rate-limited', 'fallback');
  t := t || pg_temp.tap((select mode from public.segments where utterance_id = (select utterance_id from public.translation_jobs where id = j2)) = 'fallback'
                        and (select method from public.translation_jobs where id = j2) = 'fallback', 'fallback segment recorded as original audio');
  -- Earlier jobs left open by the tests above would (correctly) hold this later segment back.
  update public.translation_jobs set status = 'failed' where session_id = sess and status in ('queued', 'processing');
  perform pg_temp.as_hub(user_a);
  r := public.hub_poll(sess, 0, '{af}');
  t := t || pg_temp.tap(exists (select 1 from jsonb_array_elements(r -> 'segments') e where (e ->> 'original')::boolean)
                        and exists (select 1 from jsonb_array_elements(r -> 'segments') e where not (e ->> 'original')::boolean),
                        'Hub is told which segments are original audio');
  perform pg_temp.as_admin();
  t := t || pg_temp.tap((select segments_original_fallback from public.session_summary where session_id = sess) = 1, 'fallback share reported per session');

  -- ------------------------------------------------------------------ same language as the lecturer
  perform pg_temp.as_hub(user_a);
  r := public.hub_sync(sess, '[{"id":"listenerA001","lang":"af"},{"id":"listenerE001","lang":"en"}]');
  perform pg_temp.as_admin();
  perform public._emit_utterances(sess, '[{"text":"Hello.","language":"en","startSeq":1,"startMs":0,"endSeq":2,"endMs":500}]'::jsonb);
  r := public.worker_tts_begin((select j.id from public.translation_jobs j join public.utterances u on u.id = j.utterance_id
                                where u.session_id = sess and u.text = 'Hello.' and j.language_code = 'en'));
  t := t || pg_temp.tap(r ->> 'method' = 'passthrough' and jsonb_array_length(r -> 'chunks') = 2, 'English listener of an English lecture: lecturer''s own audio, no TTS');

  -- ------------------------------------------------------------------ end, flush, retention
  perform pg_temp.as_hub(user_a);
  perform public.hub_end_session(sess);
  perform pg_temp.as_admin();
  t := t || pg_temp.tap((select count(*) from pgmq.q_chunk_jobs where message ->> 'flush_session' = sess::text) = 1, 'session end queues one flush');
  r := public.worker_stt_flush_begin(sess);
  t := t || pg_temp.tap(r ->> 'action' = 'process', 'flush runs once all chunks are finished');
  perform public.worker_stt_flush_commit(sess, null, '[]'::jsonb);
  t := t || pg_temp.tap(public.worker_stt_flush_begin(sess) ->> 'action' = 'final', 'flush happens only once');

  update public.segments set ready_at = now() - interval '20 minutes' where session_id = sess;
  t := t || pg_temp.tap((select count(*) from public.worker_cleanup_candidates(500) c where c.kind = 'segment' and c.path like hub_a::text || '/' || sess::text || '/af/u%') = 4,
                        'translated segments expire after 15 minutes');
  update public.utterances set created_at = now() - interval '2 days' where session_id = sess;
  update public.transcripts set created_at = now() - interval '2 days' where chunk_id in (c1, c2);
  update public.translation_jobs set created_at = now() - interval '2 days' where session_id = sess;
  perform public.purge_lecture_text();
  t := t || pg_temp.tap((select count(*) from public.utterances where session_id = sess and text is not null) = 0
                        and (select count(*) from public.translation_jobs where session_id = sess and translated_text is not null) = 0
                        and (select count(*) from public.transcripts where chunk_id in (c1, c2) and text <> '') = 0,
                        'lecture text erased after the retention period');

  perform set_config('role', 'anon', true);
  begin
    perform count(*) from public.utterances;
    ok_flag := false;
  exception when others then ok_flag := sqlstate = '42501'; end;
  t := t || pg_temp.tap(ok_flag, 'anon cannot read utterances');
  begin
    perform count(*) from public.session_usage;
    ok_flag := false;
  exception when others then ok_flag := sqlstate = '42501'; end;
  t := t || pg_temp.tap(ok_flag, 'anon cannot read usage');
  perform pg_temp.as_admin();

  return query
    select line from (
      select 0 as i, '1..' || cardinality(t) as line
      union all
      select u.i, format('%s %s - %s', split_part(u.x, ' - ', 1), u.i, substr(u.x, position(' - ' in u.x) + 3))
        from unnest(t) with ordinality as u(x, i)
    ) q order by i;
end;
$$;

select * from pg_temp.m2_tests();
