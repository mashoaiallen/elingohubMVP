-- eLingo Hub database tests.
--
-- Run against a NON-PRODUCTION project:
--   supabase db query --remote -f supabase/tests/hub_api_test.sql
--
-- Everything runs inside one plpgsql block that always ends by raising ELINGO_TEST_ROLLBACK,
-- so every row it creates is rolled back. Output is TAP-style: one row per assertion.
-- (supabase db query only returns the last statement's rows, hence this structure.)

create or replace function pg_temp.elingo_db_tests()
returns setof text
language plpgsql
as $$
declare
  out_lines text[] := '{}';
  n         int := 0;
  hub_a     uuid;  user_a uuid := gen_random_uuid();
  hub_b     uuid;  user_b uuid := gen_random_uuid();
  user_x    uuid := gen_random_uuid();  -- signed-in user that is not a hub
  s1        jsonb; s2 jsonb; s3 jsonb; r jsonb;
  sess      uuid;  sess_b uuid;
  chunk     jsonb; v_chunk_id bigint;
  msg       record;
  begun     jsonb;
  seg1      bigint; seg2 bigint;
  cnt       int;
  ok_flag   boolean;
  err_state text;

begin
  begin
    -- ---------------------------------------------------------------- fixtures (as postgres)
    insert into auth.users (id, email, aud, role, created_at, updated_at)
    values (user_a, 'hub-a@test.elingo', 'authenticated', 'authenticated', now(), now()),
           (user_b, 'hub-b@test.elingo', 'authenticated', 'authenticated', now(), now()),
           (user_x, 'user-x@test.elingo', 'authenticated', 'authenticated', now(), now());
    insert into public.hubs (auth_user_id, name) values (user_a, 'Test Hub A') returning id into hub_a;
    insert into public.hubs (auth_user_id, name) values (user_b, 'Test Hub B') returning id into hub_b;

    -- ---------------------------------------------------------------- anon has no access
    set local role anon;
    begin
      perform public.hub_hello();
      ok_flag := false;
    exception when others then ok_flag := sqlstate = '42501'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - anon cannot call hub_hello', case when ok_flag then 'ok' else 'not ok' end, n);
    begin
      perform count(*) from public.sessions;
      ok_flag := false;
    exception when others then ok_flag := sqlstate = '42501'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - anon cannot read sessions table', case when ok_flag then 'ok' else 'not ok' end, n);
    reset role;

    -- ---------------------------------------------------------------- non-hub user is rejected
    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_x, 'role', 'authenticated')::text, true);
    begin
      perform public.hub_hello();
      ok_flag := false;
    exception when others then ok_flag := sqlstate = '42501'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - signed-in non-hub user is rejected', case when ok_flag then 'ok' else 'not ok' end, n);
    begin
      perform count(*) from public.hubs;
      ok_flag := false;
    exception when others then ok_flag := sqlstate = '42501'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - authenticated cannot read hubs table directly', case when ok_flag then 'ok' else 'not ok' end, n);

    -- ---------------------------------------------------------------- hub A: hello + sessions
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    r := public.hub_hello('test-fw', '{"heap": 1}'::jsonb);
    n := n + 1; out_lines := out_lines || format('%s %s - hello returns 5 enabled languages (got %s)',
      case when jsonb_array_length(r -> 'languages') = 5 then 'ok' else 'not ok' end, n, jsonb_array_length(r -> 'languages'));
    n := n + 1; out_lines := out_lines || format('%s %s - hello reports no active session',
      case when r -> 'session' = 'null'::jsonb then 'ok' else 'not ok' end, n);

    s1 := public.hub_start_session('nonce-0001');
    s2 := public.hub_start_session('nonce-0001');
    n := n + 1; out_lines := out_lines || format('%s %s - start_session is idempotent per nonce',
      case when s1 ->> 'id' = s2 ->> 'id' then 'ok' else 'not ok' end, n);
    s3 := public.hub_start_session('nonce-0002');
    reset role;
    select count(*) into cnt from public.sessions where hub_id = hub_a and status = 'active';
    n := n + 1; out_lines := out_lines || format('%s %s - new nonce supersedes: exactly one active session (got %s)',
      case when cnt = 1 and (select status from public.sessions where id = (s1 ->> 'id')::uuid) = 'ended' then 'ok' else 'not ok' end, n, cnt);
    sess := (s3 ->> 'id')::uuid;

    -- ---------------------------------------------------------------- presence and demand
    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    r := public.hub_sync(sess, '[{"id":"listener-0001","lang":"af"},{"id":"listener-0002","lang":"af"},
                                 {"id":"listener-0003","lang":"st"},{"id":"listener-0002","lang":"af"},
                                 {"id":"bad id!","lang":"af"},{"id":"listener-0009","lang":"zu"}]'::jsonb);
    n := n + 1; out_lines := out_lines || format('%s %s - sync counts af=2 st=1 (got %s)',
      case when r -> 'languages' = '[{"code":"af","listeners":2},{"code":"st","listeners":1}]'::jsonb then 'ok' else 'not ok' end, n, r -> 'languages');
    n := n + 1; out_lines := out_lines || format('%s %s - duplicate id counted once; invalid id and unsupported language rejected (rejected=%s)',
      case when (r ->> 'rejected')::int = 3 then 'ok' else 'not ok' end, n, r ->> 'rejected');

    r := public.hub_sync(sess, '[{"id":"listener-0001","lang":"af"},{"id":"listener-0002","lang":"af"},
                                 {"id":"listener-0003","lang":"st"}]'::jsonb);
    n := n + 1; out_lines := out_lines || format('%s %s - repeating the same sync is idempotent',
      case when r -> 'languages' = '[{"code":"af","listeners":2},{"code":"st","listeners":1}]'::jsonb then 'ok' else 'not ok' end, n);

    -- listener-0003 leaves (final st listener), listener-0001 switches af -> nso
    r := public.hub_sync(sess, '[{"id":"listener-0001","lang":"nso"},{"id":"listener-0002","lang":"af"}]'::jsonb);
    n := n + 1; out_lines := out_lines || format('%s %s - leave + language change: af=1 nso=1, st inactive (got %s)',
      case when r -> 'languages' = '[{"code":"af","listeners":1},{"code":"nso","listeners":1}]'::jsonb then 'ok' else 'not ok' end, n, r -> 'languages');
    reset role;
    select count(*) into cnt from public.language_streams where session_id = sess and language_code = 'st' and not active and last_deactivated_at is not null;
    n := n + 1; out_lines := out_lines || format('%s %s - st stream recorded as deactivated', case when cnt = 1 then 'ok' else 'not ok' end, n);

    -- ---------------------------------------------------------------- hub isolation
    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_b, 'role', 'authenticated')::text, true);
    begin
      perform public.hub_sync(sess, '[]'::jsonb);
      ok_flag := false;
    exception when others then ok_flag := sqlstate = 'P0002'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - hub B cannot sync hub A session', case when ok_flag then 'ok' else 'not ok' end, n);
    begin
      perform public.hub_poll(sess, 0, array['af']);
      ok_flag := false;
    exception when others then ok_flag := sqlstate = 'P0002'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - hub B cannot poll hub A session', case when ok_flag then 'ok' else 'not ok' end, n);
    begin
      insert into storage.objects (bucket_id, name, owner_id)
      values ('source-audio', hub_a::text || '/' || sess::text || '/000099.wav', user_b::text);
      ok_flag := false;
    exception when others then ok_flag := sqlstate = '42501'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - hub B cannot upload into hub A storage folder', case when ok_flag then 'ok' else 'not ok' end, n);

    -- ---------------------------------------------------------------- chunk registration
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    begin
      perform public.hub_register_chunk(sess, 1, 5000);
      ok_flag := false;
    exception when others then ok_flag := sqlstate = 'P0002'; end;
    n := n + 1; out_lines := out_lines || format('%s %s - register_chunk rejects a chunk that was never uploaded', case when ok_flag then 'ok' else 'not ok' end, n);

    begin
      insert into storage.objects (bucket_id, name, owner_id, metadata)
      values ('source-audio', hub_a::text || '/' || sess::text || '/000001.wav', user_a::text,
              '{"size": 160044, "mimetype": "audio/wav"}'::jsonb);
      ok_flag := true;
    exception when others then ok_flag := false; err_state := sqlerrm; end;
    n := n + 1; out_lines := out_lines || format('%s %s - hub A may write its own storage folder %s', case when ok_flag then 'ok' else 'not ok' end, n, coalesce(err_state, ''));

    chunk := public.hub_register_chunk(sess, 1, 5000, 900, 12000, false, now());
    r := public.hub_register_chunk(sess, 1, 5000, 900, 12000, false, now());
    reset role;
    v_chunk_id := (chunk ->> 'chunkId')::bigint;
    select count(*) into cnt from pgmq.q_chunk_jobs where (message ->> 'chunk_id')::bigint = v_chunk_id;
    n := n + 1; out_lines := out_lines || format('%s %s - duplicate registration: same chunk, one queue message (dup=%s, msgs=%s)',
      case when (r ->> 'duplicate')::boolean and (r ->> 'chunkId')::bigint = v_chunk_id and cnt = 1 then 'ok' else 'not ok' end, n, r ->> 'duplicate', cnt);
    n := n + 1; out_lines := out_lines || format('%s %s - registered size comes from storage, not the client (bytes=%s)',
      case when (select bytes from public.source_chunks where id = v_chunk_id) = 160044 then 'ok' else 'not ok' end, n,
      (select bytes from public.source_chunks where id = v_chunk_id));

    -- ---------------------------------------------------------------- worker (service role)
    set local role service_role;
    select * into msg from public.worker_claim(16, 30) m where m.chunk_id = v_chunk_id;
    n := n + 1; out_lines := out_lines || format('%s %s - worker claims the queued chunk', case when msg.msg_id is not null then 'ok' else 'not ok' end, n);
    begun := public.worker_begin_chunk(v_chunk_id);
    n := n + 1; out_lines := out_lines || format('%s %s - only languages with listeners are targeted (got %s)',
      case when begun -> 'languages' = '["af","nso"]'::jsonb then 'ok' else 'not ok' end, n, begun -> 'languages');
    seg1 := public.worker_segment_ready(v_chunk_id, 'af', 'x/af/000001.wav', 160044, 5000, 'loopback');
    seg2 := public.worker_segment_ready(v_chunk_id, 'af', 'x/af/000001.wav', 160044, 5000, 'loopback');
    n := n + 1; out_lines := out_lines || format('%s %s - duplicate segment delivery is idempotent', case when seg1 = seg2 then 'ok' else 'not ok' end, n);
    perform public.worker_segment_ready(v_chunk_id, 'nso', 'x/nso/000001.wav', 160044, 5000, 'loopback');
    perform public.worker_finish_chunk(v_chunk_id, msg.msg_id, 'done');
    reset role;
    n := n + 1; out_lines := out_lines || format('%s %s - af stream counts exactly one produced segment',
      case when (select segments_produced from public.language_streams where session_id = sess and language_code = 'af') = 1 then 'ok' else 'not ok' end, n);
    select count(*) into cnt from pgmq.q_chunk_jobs where msg_id = msg.msg_id;
    n := n + 1; out_lines := out_lines || format('%s %s - finished job is archived off the queue', case when cnt = 0 then 'ok' else 'not ok' end, n);

    -- ---------------------------------------------------------------- polling
    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    r := public.hub_poll(sess, 0, array['af']);
    n := n + 1; out_lines := out_lines || format('%s %s - poll returns only requested languages (got %s)',
      case when jsonb_array_length(r -> 'segments') = 1 and r -> 'segments' -> 0 ->> 'lang' = 'af' then 'ok' else 'not ok' end, n, jsonb_array_length(r -> 'segments'));
    r := public.hub_poll(sess, seg1, array['af', 'nso']);
    n := n + 1; out_lines := out_lines || format('%s %s - poll cursor skips already-seen segments',
      case when jsonb_array_length(r -> 'segments') = 1 and r -> 'segments' -> 0 ->> 'lang' = 'nso' then 'ok' else 'not ok' end, n);

    -- ---------------------------------------------------------------- zero listeners => zero targets
    perform public.hub_sync(sess, '[]'::jsonb);
    insert into storage.objects (bucket_id, name, owner_id, metadata)
    values ('source-audio', hub_a::text || '/' || sess::text || '/000002.wav', user_a::text, '{"size": 160044}'::jsonb);
    chunk := public.hub_register_chunk(sess, 2, 5000);
    reset role;
    set local role service_role;
    begun := public.worker_begin_chunk((chunk ->> 'chunkId')::bigint);
    reset role;
    n := n + 1; out_lines := out_lines || format('%s %s - no listeners: no target languages (got %s)',
      case when begun -> 'languages' = '[]'::jsonb then 'ok' else 'not ok' end, n, begun -> 'languages');

    -- ---------------------------------------------------------------- expiry and session end
    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    perform public.hub_sync(sess, '[{"id":"listener-0005","lang":"en"}]'::jsonb);
    reset role;
    update public.listeners set last_seen_at = now() - interval '1 minute' where session_id = sess and listener_id = 'listener-0005';
    perform public.expire_stale_listeners();
    n := n + 1; out_lines := out_lines || format('%s %s - stale listener (hub offline) expires and deactivates its language',
      case when not (select active from public.language_streams where session_id = sess and language_code = 'en') then 'ok' else 'not ok' end, n);

    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    perform public.hub_sync(sess, '[{"id":"listener-0006","lang":"af"}]'::jsonb);
    r := public.hub_end_session(sess);
    r := public.hub_end_session(sess);  -- idempotent
    reset role;
    n := n + 1; out_lines := out_lines || format('%s %s - ending a session removes listeners and deactivates every stream',
      case when (select count(*) from public.language_streams where session_id = sess and active) = 0
            and (select count(*) from public.listeners where session_id = sess and left_at is null) = 0
            and r ->> 'status' = 'ended' then 'ok' else 'not ok' end, n);

    update public.sessions set ended_at = now() - interval '5 minutes' where id = sess;
    insert into storage.objects (bucket_id, name, owner_id, metadata)
    values ('source-audio', hub_a::text || '/' || sess::text || '/000003.wav', user_a::text, '{"size": 160044}'::jsonb);
    set local role authenticated;
    perform set_config('request.jwt.claims', json_build_object('sub', user_a, 'role', 'authenticated')::text, true);
    begin
      perform public.hub_register_chunk(sess, 3, 5000);
      ok_flag := false;
    exception when others then ok_flag := sqlstate = 'P0001'; end;
    reset role;
    n := n + 1; out_lines := out_lines || format('%s %s - chunks for a long-ended session are refused', case when ok_flag then 'ok' else 'not ok' end, n);

    raise exception 'ELINGO_TEST_ROLLBACK';
  exception when others then
    if sqlerrm <> 'ELINGO_TEST_ROLLBACK' then
      n := n + 1;
      out_lines := out_lines || format('not ok %s - unexpected error after %s assertions: [%s] %s', n, n - 1, sqlstate, sqlerrm);
    end if;
  end;

  out_lines := array_prepend(format('1..%s', n), out_lines);
  return query select unnest(out_lines);
end;
$$;

select * from pg_temp.elingo_db_tests();
