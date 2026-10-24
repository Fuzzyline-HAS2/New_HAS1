import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from notifications import MAX_BODY, NOTION_EVENT_PROPERTIES, NOTION_SESSION_PROPERTIES, NotificationSender, _allowed_url, _classify, _http_child, _transport, notion_database_id, resolve_notion_database, check_notion_schema, slack_message, slack_session_card, validate_config

SOURCE = '12345678-1234-1234-1234-123456789abc'
PAGE = 'abcdefab-1234-1234-1234-123456789abc'
# Deliberately synthetic, no live endpoint or credential is used by these tests.
CREDS = {'slack_webhook_url': 'https://hooks.slack.com/services/TEST/TEST/TEST', 'notion_token': 'synthetic_token_123'}
SESSION_SOURCE = '87654321-4321-4321-4321-cba987654321'
SESSION_PAGE = 'fedcbafe-4321-4321-4321-cba987654321'
PAYLOAD = {'incident_id': 'incident-1', 'site_id': 'store-1', 'device_mac': 'AA:BB:CC:DD:EE:FF', 'device_id': 'G2P5',
           'game_session_id': 'game-1', 'status': 'recovered', 'summary': '<!channel> test', 'occurred_at': '2026-10-24T00:00:00Z'}


def schema(names=('장애중', '복구됨', '미복구 종료'), relation=SESSION_SOURCE):
    properties = {name: {'type': kind, kind: {}} for name, kind in NOTION_EVENT_PROPERTIES.items()}
    properties['상태']['status']['options'] = [{'name': name} for name in names]
    properties['게임 세션']['relation'] = {'data_source_id': relation} if relation else {}
    return {'status': 200, 'body': json.dumps({'object': 'data_source', 'id': SOURCE, 'properties': properties})}


def session_schema(missing=None):
    properties = {name: {'type': kind, kind: {}} for name, kind in NOTION_SESSION_PROPERTIES.items() if name != missing}
    return {'status': 200, 'body': json.dumps({'object': 'data_source', 'id': SESSION_SOURCE, 'properties': properties})}


def found(*ids):
    return {'status': 200, 'body': json.dumps({'results': [{'id': page} for page in ids], 'has_more': False})}


def created(page):
    return {'status': 200, 'body': json.dumps({'id': page})}


class NotificationsTests(unittest.TestCase):
    def sender(self, responses, schema_response=None, session_response=None):
        calls = []
        def transport(request, timeout):
            calls.append(request)
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 10)
            if request['method'] == 'GET' and request['url'].endswith('/v1/data_sources/' + SESSION_SOURCE):
                return session_schema() if session_response is None else session_response
            if request['method'] == 'GET' and '/v1/data_sources/' in request['url']:
                return schema() if schema_response is None else schema_response
            return responses.pop(0)
        config = validate_config({'enabled': True, 'credential_file': 'test.dpapi',
            'slack': {'enabled': True}, 'notion': {'enabled': True, 'data_source_id': SOURCE}}, '.')
        return NotificationSender(config, credential_reader=lambda _: json.dumps(CREDS), transport=transport), calls

    def test_disabled_no_credential_read_or_network(self):
        sender = NotificationSender(validate_config({}, '.'), credential_reader=lambda _: self.fail())
        self.assertEqual(sender.send('slack', PAYLOAD).code, 'channel_disabled')

    def test_validation_rejects_inline_secrets_and_bad_flags(self):
        for config in ({'webhook_url': 'secret'}, {'enabled': 'false'}, {'slack': {'enabled': 1}},
                       {'enabled': True, 'credential_file': 'x', 'notion': {'enabled': True, 'data_source_id': '../bad'}}):
            with self.assertRaises(ValueError): validate_config(config, '.')

    def test_destinations_no_redirect_or_arbitrary_host(self):
        for url in ('http://hooks.slack.com/services/A/B/C', 'https://evil.test/services/A/B/C',
                    'https://hooks.slack.com.evil.test/services/A/B/C', 'https://x@hooks.slack.com/services/A/B/C',
                    'https://hooks.slack.com/services/A/B/C?secret=x', 'https://api.notion.com/v1/users'):
            with self.assertRaises(ValueError): _allowed_url(url, 'POST')
        self.assertEqual(_classify({'status': 302}).state, 'permanent')

    def test_slack_plain_text_and_recovery(self):
        sender, calls = self.sender([{'status': 200, 'body': 'ok'}])
        payload = dict(PAYLOAD, device_id='<!channel>', opened_at='2026-10-23T23:57:30Z', close_reason='reconnected')
        self.assertEqual(sender.send('slack', payload).state, 'delivered')
        blocks = calls[0]['body']['blocks']
        texts = [blocks[0]['text']] + blocks[1]['fields'] + blocks[2]['elements']
        self.assertTrue(all(text['type'] == 'plain_text' for text in texts))
        self.assertIn('복구', blocks[0]['text']['text'])
        rendered = '\n'.join(text['text'] for text in texts)
        self.assertIn('10/24 09:00:00', rendered)  # UTC shown in store-local KST
        self.assertIn('2분 30초', rendered)
        self.assertIn('장치 재연결', rendered)
        self.assertNotIn('Authorization', calls[0])

    def test_slack_offline_card_skips_unknown_and_legacy_payloads(self):
        sender, calls = self.sender([{'status': 200, 'body': 'ok'}, {'status': 200, 'body': 'ok'}])
        payload = dict(PAYLOAD, status='offline', device_id='glove-3', role='unknown', gap_ms=12500,
                       last_seen_utc='2026-10-23T23:59:47Z')
        self.assertEqual(sender.send('slack', payload).state, 'delivered')
        blocks = calls[0]['body']['blocks']
        rendered = '\n'.join(field['text'] for field in blocks[1]['fields'])
        self.assertIn('연결 끊김', blocks[0]['text']['text'])
        self.assertIn('12초', rendered)
        self.assertNotIn('역할', rendered)
        self.assertIn('원인', blocks[2]['elements'][0]['text'])
        # Outbox rows queued before this format still render without the new keys.
        self.assertEqual(sender.send('slack', dict(PAYLOAD, status='offline')).state, 'delivered')
        self.assertTrue(calls[1]['body']['blocks'][1]['fields'])

    def bot_sender(self, responses):
        calls = []
        def transport(request, timeout):
            calls.append(request)
            return responses.pop(0)
        config = validate_config({'enabled': True, 'credential_file': 'test.dpapi',
            'slack': {'enabled': True, 'channel_id': 'C0123456789'}}, '.')
        creds = {'slack_bot_token': 'xoxb-synthetic-test-token'}
        return NotificationSender(config, credential_reader=lambda _: json.dumps(creds), transport=transport), calls

    @staticmethod
    def slack_ok(**extra):
        return {'status': 200, 'body': json.dumps(dict({'ok': True, 'channel': 'C0123456789', 'ts': '1700000000.000100'}, **extra))}

    CARD = '1700000000.000100'

    @staticmethod
    def session(ts=None, state='open', **extra):
        incidents = [{'device_id': 'G1P1', 'state': state, 'opened_utc': '2026-10-24T22:25:29Z', 'updated_utc': '2026-10-24T22:26:04Z'},
                     {'device_id': 'G1P5', 'state': 'recovered', 'opened_utc': '2026-10-24T22:25:29Z', 'updated_utc': '2026-10-24T22:25:47Z'}]
        return dict({'site_id': 'store-1', 'session_id': '1657', 'ts': ts, 'notion_page': SESSION_PAGE,
                     'event_page': PAGE, 'incidents': incidents}, **extra)

    @staticmethod
    def texts(body):
        found = []
        for block in body['blocks']:
            for item in [block.get('text')] + block.get('fields', []) + block.get('elements', []):
                if item:
                    found.append((item['type'], item['text']))
        return found

    def test_slack_bot_first_event_opens_one_card_for_the_game_session(self):
        sender, calls = self.bot_sender([self.slack_ok()])
        result = sender.send('slack', dict(PAYLOAD, device_id='G1P1', status='offline', slack_session=self.session()))
        self.assertEqual((result.state, result.remote_id, result.parent_id), ('delivered', self.CARD, self.CARD))
        self.assertEqual(len(calls), 1)  # one card; nothing in its thread yet
        card = calls[0]['body']
        self.assertEqual((calls[0]['url'], card['channel']), ('https://slack.com/api/chat.postMessage', 'C0123456789'))
        self.assertNotIn('thread_ts', card)
        self.assertNotIn('xoxb', json.dumps(card))
        self.assertEqual(card['blocks'][0]['text']['text'], '🔴 장애 진행 중 1건 · 세션 1657 · store-1')
        texts = self.texts(card)
        self.assertIn(('plain_text', '장애\n2건 (복구 1 · 미복구 0 · 진행 중 1)'), texts)
        self.assertIn(('plain_text', '심각도\nSEV3 · 자동 판정'), texts)
        self.assertIn(('plain_text', '🔴 G1P1 · 10/25 07:25:29 끊김 → 장애중\n🟢 G1P5 · 10/25 07:25:29 끊김 → 18초 만에 복구'), texts)
        link = 'https://www.notion.so/' + SESSION_PAGE.replace('-', '')
        self.assertIn(('mrkdwn', f'<{link}|📒 노션 장애 페이지>'), texts)
        # mrkdwn carries only the collector-built link; every payload value stays plain_text.
        self.assertEqual([kind for kind, _ in texts].count('mrkdwn'), 1)

    def test_slack_bot_recovery_edits_the_card_and_keeps_the_thread_for_people(self):
        sender, calls = self.bot_sender([self.slack_ok()])
        payload = dict(PAYLOAD, device_id='G1P1', status='recovered', slack_session=self.session(self.CARD, 'recovered'))
        result = sender.send('slack', payload)
        self.assertEqual((result.state, result.remote_id), ('delivered', self.CARD))
        self.assertEqual([call['url'] for call in calls], ['https://slack.com/api/chat.update'])
        self.assertEqual(calls[0]['body']['ts'], self.CARD)
        self.assertEqual(calls[0]['body']['blocks'][0]['text']['text'], '🟢 모두 복구 · 세션 1657 · store-1')
        self.assertIn(('plain_text', '🟢 G1P1 · 10/25 07:25:29 끊김 → 35초 만에 복구\n🟢 G1P5 · 10/25 07:25:29 끊김 → 18초 만에 복구'),
                      self.texts(calls[0]['body']))

    def test_slack_bot_unrecovered_end_threads_and_broadcasts(self):
        sender, calls = self.bot_sender([self.slack_ok(), self.slack_ok(ts='1700000300.000200')])
        payload = dict(PAYLOAD, device_id='G1P1', status='monitoring_ended', close_reason='snapshot_no_longer_active',
                       opened_at='2026-10-24T22:25:29Z', slack_session=self.session(self.CARD, 'closed'))
        self.assertEqual(sender.send('slack', payload).state, 'delivered')
        update, reply = calls
        self.assertEqual(update['body']['blocks'][0]['text']['text'], '🟠 미복구 종료 포함 · 세션 1657 · store-1')
        self.assertIn(('plain_text', '심각도\nSEV2 · 미복구 있음'), self.texts(update['body']))
        self.assertEqual((reply['body']['thread_ts'], reply['body']['reply_broadcast']), (self.CARD, True))
        self.assertEqual(reply['body']['text'], '⚪ G1P1 미복구 종료 · 게임 종료 또는 서버 목록에서 제외 · 끊긴 시각 10/25 07:25:29')
        self.assertIn(('mrkdwn', f"<https://www.notion.so/{PAGE.replace('-', '')}|📒 노션 이벤트>"), self.texts(reply['body']))

    def test_slack_bot_failures_never_duplicate_and_keep_the_card(self):
        ended = dict(PAYLOAD, status='monitoring_ended', slack_session=self.session(self.CARD, 'closed'))
        # A failed edit is safely retryable and posts no reply.
        sender, calls = self.bot_sender([{'status': 503}])
        self.assertEqual(sender.send('slack', ended).state, 'retryable')
        self.assertEqual(len(calls), 1)
        # An ambiguous reply is not retried automatically, but the card is remembered.
        sender, _ = self.bot_sender([self.slack_ok(), {'network_error': True}])
        result = sender.send('slack', ended)
        self.assertEqual((result.state, result.parent_id), ('unknown', self.CARD))
        # A new card whose reply is rate limited: the retry must thread under that card.
        sender, _ = self.bot_sender([self.slack_ok(ts='1700000500.000500'), {'status': 429, 'retry_after': '5'}])
        result = sender.send('slack', dict(ended, slack_session=self.session(None, 'closed')))
        self.assertEqual((result.state, result.parent_id), ('retryable', '1700000500.000500'))
        # Slack API errors are permanent and carry only the fixed error name.
        sender, _ = self.bot_sender([{'status': 200, 'body': '{"ok":false,"error":"not_in_channel"}'}])
        result = sender.send('slack', dict(PAYLOAD, status='offline'))
        self.assertEqual((result.state, result.code), ('permanent', 'slack_not_in_channel'))
        sender, _ = self.bot_sender([{'status': 200, 'body': '{"ok":true}'}])
        self.assertEqual(sender.send('slack', dict(PAYLOAD, status='offline')).state, 'unknown')

    def test_slack_bot_transient_errors_are_not_dropped(self):
        def error(name):
            return {'status': 200, 'body': json.dumps({'ok': False, 'error': name})}
        edit = dict(PAYLOAD, slack_session=self.session(self.CARD, 'recovered'))
        new_card = dict(PAYLOAD, status='offline')
        for name in ('internal_error', 'fatal_error', 'request_timeout', 'service_unavailable'):
            sender, _ = self.bot_sender([error(name)])
            self.assertEqual(sender.send('slack', edit).state, 'retryable')  # edits are idempotent
            sender, _ = self.bot_sender([error(name)])
            expected = 'retryable' if name == 'service_unavailable' else 'unknown'
            self.assertEqual(sender.send('slack', new_card).state, expected)

    def test_slack_bot_deleted_card_opens_a_new_one(self):
        gone = {'status': 200, 'body': '{"ok":false,"error":"message_not_found"}'}
        sender, calls = self.bot_sender([gone, self.slack_ok(ts='1700000400.000300')])
        result = sender.send('slack', dict(PAYLOAD, slack_session=self.session(self.CARD, 'recovered')))
        self.assertEqual((result.state, result.remote_id, result.parent_id), ('delivered', '1700000400.000300', '1700000400.000300'))
        self.assertNotIn('thread_ts', calls[1]['body'])
        self.assertIn('모두 복구', calls[1]['body']['blocks'][0]['text']['text'])

    def test_slack_session_card_keeps_device_names_inert(self):
        session = self.session(site_id='<!here>', incidents=[{'device_id': '<!channel>', 'state': 'open', 'opened_utc': 'bad', 'updated_utc': None}])
        card = slack_session_card(PAYLOAD, session)
        self.assertNotIn('<!here>', card['text'])
        self.assertIn('&lt;!here&gt;', card['text'])
        self.assertIn(('plain_text', '🔴 <!channel> · ? 끊김 → 장애중'), self.texts(card))

    def test_slack_webhook_skips_bot_queued_monitoring_end(self):
        sender, calls = self.sender([])
        result = sender.send('slack', dict(PAYLOAD, status='monitoring_ended', slack_ts='1700000000.000100'))
        self.assertEqual(result.state, 'permanent')
        self.assertEqual(calls, [])

    def test_slack_card_tolerates_garbage_and_escapes_fallback(self):
        message = slack_message(dict(PAYLOAD, status='offline', device_id='<!channel>', gap_ms=10 ** 400,
                                     last_seen_utc='0001-01-01T00:00:00+05:00'))
        self.assertNotIn('<!channel>', message['text'])
        self.assertIn('&lt;!channel&gt;', message['text'])
        self.assertIn('<!channel>', message['blocks'][0]['text']['text'])  # plain_text renders it inert

    EVIDENCE = {'incident_id': 'incident-1', 'notion_page_id': PAGE, 'evidence': {
        'rows': [['장치', '부팅 12 · 켜진 뒤 0:01:07 (수신 12:03:30)', '와이파이 이벤트 · 끊김 원인 200 BEACON_TIMEOUT']],
        'truncated': False, 'device_pending': True, 'collected_utc': '2026-10-24T03:10:00+00:00', 'incident_id': 'incident-1'}}

    @staticmethod
    def children(*headings):
        blocks = [{'type': 'heading_3', 'heading_3': {'rich_text': [{'plain_text': text}]}} for text in headings]
        return {'status': 200, 'body': json.dumps({'results': blocks, 'has_more': False})}

    def test_notion_evidence_appends_table_to_event_page_once(self):
        sender, calls = self.sender([self.children('사람이 쓴 메모'), {'status': 200, 'body': '{"results":[]}'}])
        result = sender.send('notion', self.EVIDENCE)
        self.assertEqual((result.state, result.code, result.remote_id), ('delivered', 'ok', PAGE))
        get, patch = calls
        self.assertEqual((get['method'], get['url']), ('GET', f'https://api.notion.com/v1/blocks/{PAGE}/children'))
        self.assertEqual((patch['method'], patch['url']), ('PATCH', get['url']))
        heading, note, table = patch['body']['children']
        self.assertIn('원인 미확정', heading['heading_3']['rich_text'][0]['text']['content'])
        text = note['paragraph']['rich_text'][0]['text']['content']
        self.assertIn('incidents.py --incident-id incident-1', text)
        self.assertIn('장치 기록이 아직 올라오지 않았습니다', text)
        rows = table['table']['children']
        self.assertEqual(table['table']['table_width'], 3)
        self.assertEqual([cell[0]['text']['content'] for cell in rows[0]['table_row']['cells']], ['출처', '시각', '내용'])
        self.assertEqual(len(rows), 2)
        # A retry after an unknown append finds the heading and does not append again.
        sender, calls = self.sender([self.children('📦 블랙박스 증거 (후보 · 원인 미확정)')])
        self.assertEqual(sender.send('notion', self.EVIDENCE).code, 'already_attached')
        self.assertEqual(len(calls), 1)

    def test_notion_evidence_failures_are_retryable_and_never_unbuilt(self):
        for responses in ([{'network_error': True}], [self.children(), {'network_error': True}], [self.children(), {'status': 503}]):
            sender, _ = self.sender(responses)
            self.assertEqual(sender.send('notion', self.EVIDENCE).state, 'retryable')
        sender, calls = self.sender([])
        self.assertEqual(sender.send('notion', dict(self.EVIDENCE, evidence=True)).code, 'evidence_not_built')
        self.assertEqual(calls, [])
        _allowed_url(f'https://api.notion.com/v1/blocks/{PAGE}/children', 'GET')
        for url, method in ((f'https://api.notion.com/v1/blocks/{PAGE}', 'DELETE'), (f'https://api.notion.com/v1/blocks/{PAGE}', 'PATCH')):
            with self.assertRaises(ValueError): _allowed_url(url, method)

    def test_slack_bot_config_and_destinations(self):
        for channel_id in ('#has1-device-alerts', 'c0123456789', 7):
            with self.assertRaises(ValueError):
                validate_config({'slack': {'enabled': True, 'channel_id': channel_id}}, '.')
        _allowed_url('https://slack.com/api/chat.update', 'POST')
        for url in ('https://slack.com/api/chat.delete', 'https://slack.com/api/users.list'):
            with self.assertRaises(ValueError): _allowed_url(url, 'POST')
        with self.assertRaises(ValueError):
            _http_child({'url': 'https://slack.com/api/chat.postMessage', 'method': 'POST', 'body': {}, 'token': 'xoxp-user-token-123'})

    def test_rate_limit_and_auth_are_distinct(self):
        sender, _ = self.sender([{'status': 429, 'retry_after': '125'}, {'status': 401, 'body': 'secret echo'}])
        result = sender.send('slack', PAYLOAD)
        self.assertEqual((result.state, result.retry_after), ('retryable', 125))
        result = sender.send('slack', PAYLOAD)
        self.assertEqual(result.state, 'permanent')
        self.assertNotIn('secret', repr(result))
        self.assertEqual(_classify({'status': 429, 'retry_after': '9999999999999'}).retry_after, 86400)

    def test_ambiguous_slack_write_not_retried(self):
        for response in ({'network_error': True}, {'status': 503}, {'status': 200, 'body': 'unexpected'}):
            sender, calls = self.sender([response])
            self.assertEqual(sender.send('slack', PAYLOAD).state, 'unknown')
            self.assertEqual(len(calls), 1)

    def test_transport_hard_timeout_redacts_input(self):
        with patch('notifications.subprocess.run', side_effect=subprocess.TimeoutExpired('secret', 10)):
            self.assertEqual(_transport({'token': 'secret'}, 0.1), {'network_error': True})

    def test_credential_error_redacted(self):
        sender, calls = self.sender([])
        def bad(_): raise RuntimeError('secret token and webhook')
        sender.credential_reader = bad
        result = sender.send('slack', PAYLOAD)
        self.assertEqual(result.code, 'configuration_or_credentials')
        self.assertEqual(calls, [])
        self.assertNotIn('secret token', repr(result))

    def test_notion_create_makes_session_then_links_event(self):
        payload = dict(PAYLOAD, status='offline', opened_at='2026-10-24T19:58:00Z', occurred_at='2026-10-24T19:58:00Z', role='player')
        sender, calls = self.sender([found(), found(), created(SESSION_PAGE), created(PAGE)])
        result = sender.send('notion', payload)
        self.assertEqual((result.state, result.remote_id), ('delivered', PAGE))
        self.assertEqual([(c['method'], c['url'].rsplit('/v1/', 1)[1]) for c in calls], [
            ('GET', 'data_sources/' + SOURCE), ('POST', f'data_sources/{SOURCE}/query'),
            ('GET', 'data_sources/' + SESSION_SOURCE), ('POST', f'data_sources/{SESSION_SOURCE}/query'),
            ('POST', 'pages'), ('POST', 'pages')])
        self.assertEqual(calls[1]['body']['filter'], {'property': '사건 ID', 'rich_text': {'equals': 'incident-1'}})
        lookup = calls[3]['body']
        self.assertEqual(lookup['filter'], {'property': '세션 키', 'rich_text': {'equals': 'store-1/game-1'}})
        self.assertEqual(lookup['page_size'], 100)
        session = calls[4]['body']
        self.assertEqual(session['parent'], {'type': 'data_source_id', 'data_source_id': SESSION_SOURCE})
        self.assertEqual(session['properties']['장애']['title'][0]['text']['content'], 'store-1 · 10/25 04:58 · 세션 game-1')
        self.assertEqual(session['properties']['처음 발생'], {'date': {'start': '2026-10-24T19:58:00+00:00'}})
        event = calls[5]['body']
        self.assertEqual(event['parent'], {'type': 'data_source_id', 'data_source_id': SOURCE})
        props = event['properties']
        self.assertEqual(props['게임 세션'], {'relation': [{'id': SESSION_PAGE}]})
        self.assertEqual(props['상태'], {'status': {'name': '장애중'}})
        self.assertEqual(props['이벤트']['title'][0]['text']['content'], 'G2P5 연결 끊김')
        self.assertEqual((props['장치'], props['역할']), ({'select': {'name': 'G2P5'}}, {'select': {'name': 'player'}}))
        self.assertNotIn('종료 시각', props)
        self.assertEqual(props['요약']['rich_text'][0]['text']['content'], '<!channel> test')

    def test_notion_session_resolved_once_per_process(self):
        # Two devices of one session drop together: the second must not query (and miss) the new page.
        sender, calls = self.sender([found(), found(), created(SESSION_PAGE), created(PAGE), found(), created(PAGE)])
        for incident in ('incident-1', 'incident-2'):
            self.assertEqual(sender.send('notion', dict(PAYLOAD, incident_id=incident)).state, 'delivered')
        session_writes = [c for c in calls if c['url'].endswith(f'{SESSION_SOURCE}/query') or
                          (c['method'] == 'POST' and c['body'].get('parent', {}).get('data_source_id') == SESSION_SOURCE)]
        self.assertEqual(len(session_writes), 2)
        self.assertEqual(calls[-1]['body']['properties']['게임 세션'], {'relation': [{'id': SESSION_PAGE}]})

    def test_notion_duplicate_sessions_resolve_deterministically(self):
        older, newer = SESSION_PAGE, 'aaaaaaaa-4321-4321-4321-cba987654321'
        rows = {'status': 200, 'body': json.dumps({'results': [
            {'id': newer, 'created_time': '2026-10-24T19:59:00.000Z'},
            {'id': older, 'created_time': '2026-10-24T19:58:00.000Z'}]})}
        sender, calls = self.sender([found(), rows, created(PAGE)])
        sender.send('notion', PAYLOAD)
        self.assertEqual(calls[-1]['body']['properties']['게임 세션'], {'relation': [{'id': older}]})
        tie = {'status': 200, 'body': json.dumps({'results': [
            {'id': older, 'created_time': '2026-10-24T19:58:00.000Z'},
            {'id': newer, 'created_time': '2026-10-24T19:58:00.000Z'}]})}
        sender, calls = self.sender([found(), tie, created(PAGE)])
        sender.send('notion', PAYLOAD)
        self.assertEqual(calls[-1]['body']['properties']['게임 세션'], {'relation': [{'id': newer}]})

    def test_notion_failed_event_create_forgets_cached_session(self):
        sender, calls = self.sender([found(), found(SESSION_PAGE), {'status': 400}, found(), found(SESSION_PAGE), created(PAGE)])
        self.assertEqual(sender.send('notion', PAYLOAD).code, 'http_400')
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'delivered')
        self.assertEqual(sum(c['url'].endswith(f'{SESSION_SOURCE}/query') for c in calls), 2)

    def test_notion_followup_patches_linked_page_without_query(self):
        sender, calls = self.sender([created(PAGE)])
        self.assertEqual(sender.send('notion', dict(PAYLOAD, notion_page_id=PAGE)).state, 'delivered')
        self.assertEqual([(c['method'], c['url'].rsplit('/v1/', 1)[1]) for c in calls],
                         [('GET', 'data_sources/' + SOURCE), ('PATCH', 'pages/' + PAGE)])
        sender, calls = self.sender([found(PAGE), created(PAGE)])
        self.assertEqual(sender.send('notion', dict(PAYLOAD, notion_page_id='not-a-uuid')).state, 'delivered')
        self.assertEqual([c['method'] for c in calls], ['GET', 'POST', 'PATCH'])

    def test_notion_create_needs_budget_reserve(self):
        sender, calls = self.sender([found(), found(SESSION_PAGE)])
        # Five calls at 1 s each leave 4 s, under the 5 s reserve, so the event create is never attempted.
        with patch('notifications.time.monotonic', side_effect=[100, 101, 102, 103, 104, 106]):
            result = sender.send('notion', PAYLOAD)
        self.assertEqual((result.state, result.code), ('retryable', 'budget_exhausted'))
        self.assertFalse(any(c['method'] == 'POST' and c['url'].endswith('/v1/pages') for c in calls))

    def test_notion_conflict_is_retried(self):
        sender, _ = self.sender([found(PAGE), {'status': 409}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')
        sender, _ = self.sender([found(), found(), {'status': 409}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')

    def test_notion_existing_session_is_reused(self):
        sender, calls = self.sender([found(), found(SESSION_PAGE), created(PAGE)])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'delivered')
        self.assertEqual([c['method'] for c in calls], ['GET', 'POST', 'GET', 'POST', 'POST'])
        self.assertEqual(calls[4]['body']['properties']['게임 세션'], {'relation': [{'id': SESSION_PAGE}]})

    def test_notion_update_does_not_create_duplicate_or_touch_session(self):
        sender, calls = self.sender([found(PAGE), created(PAGE)])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'delivered')
        self.assertEqual([c['method'] for c in calls], ['GET', 'POST', 'PATCH'])
        self.assertNotIn('parent', calls[2]['body'])
        self.assertNotIn('게임 세션', calls[2]['body']['properties'])

    def test_notion_duplicate_records_fail_closed(self):
        sender, calls = self.sender([found(PAGE, PAGE)])
        self.assertEqual(sender.send('notion', PAYLOAD).code, 'duplicate_incident_records')
        self.assertEqual(len(calls), 2)

    def test_notion_unknown_create_and_safe_query_retry(self):
        sender, _ = self.sender([found(), found(SESSION_PAGE), {'network_error': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'unknown')
        sender, _ = self.sender([{'network_error': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')

    def test_notion_lost_session_create_retries_without_event_write(self):
        for response in ({'network_error': True}, {'status': 503}, {'status': 200, 'body': 'not-json'}):
            with self.subTest(response=response):
                sender, calls = self.sender([found(), found(), response])
                self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')
                self.assertEqual(len(calls), 5)

    def test_notion_session_schema_checked_before_any_create(self):
        for response in (session_schema(missing='세션 키'), {'status': 403}):
            with self.subTest(response=response):
                sender, calls = self.sender([found()], session_response=response)
                self.assertEqual(sender.send('notion', PAYLOAD).state, 'permanent')
                self.assertEqual([c['method'] for c in calls], ['GET', 'POST', 'GET'])

    def test_notion_requires_game_session_and_sanitizes_select_names(self):
        for missing in ('site_id', 'game_session_id'):
            sender, calls = self.sender([])
            self.assertEqual(sender.send('notion', dict(PAYLOAD, **{missing: ''})).code, 'missing_game_session')
            self.assertEqual(calls, [])
        sender, calls = self.sender([found(PAGE), created(PAGE)])
        sender.send('notion', dict(PAYLOAD, device_id='G2,P5'))
        self.assertEqual(calls[2]['body']['properties']['장치'], {'select': {'name': 'G2 P5'}})

    def test_http_child_fixed_host_headers_and_no_redirect(self):
        class Response:
            status = 302
            def read(self, maximum):
                self.maximum = maximum
                return b'ignored'
            def getheader(self, name, default): return default
        with patch('notifications.http.client.HTTPSConnection') as factory:
            connection = factory.return_value
            connection.getresponse.return_value = Response()
            response = _http_child({'url': 'https://api.notion.com/v1/pages', 'method': 'POST',
                                    'body': {}, 'token': CREDS['notion_token']})
            factory.assert_called_once_with('api.notion.com', timeout=10.0)
            headers = connection.request.call_args.args[3]
            self.assertEqual(headers['Notion-Version'], '2025-09-03')
            self.assertEqual(headers['Authorization'], 'Bearer ' + CREDS['notion_token'])
            self.assertEqual(response['status'], 302)
            self.assertEqual(connection.request.call_count, 1)
            connection.close.assert_called_once()

    def test_notion_update_timeout_is_retryable_but_create_is_unknown(self):
        sender, _ = self.sender([found(PAGE), {'network_error': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')

    def test_oversized_query_does_not_create(self):
        sender, calls = self.sender([{'status': 200, 'oversized': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'permanent')
        self.assertEqual(len(calls), 2)

    def test_notion_database_url_parser(self):
        self.assertEqual(notion_database_id('https://www.notion.so/team/My-DB-' + SOURCE.replace('-', '') + '?v=view'), SOURCE)
        self.assertEqual(notion_database_id('https://notion.so/' + SOURCE), SOURCE)
        self.assertEqual(notion_database_id('https://app.notion.com/p/' + SOURCE.replace('-', '') + '?v=abc&source=copy_link'), SOURCE)
        for value in ('https://evil.test/' + SOURCE, 'http://notion.so/' + SOURCE,
                      'https://notion.so.evil.test/' + SOURCE, 'https://app.notion.com.evil.test/p/' + SOURCE,
                      'https://app.notion.com:444/p/' + SOURCE, 'https://notion.com/p/' + SOURCE, 'https://user@notion.so/' + SOURCE, 'https://notion.so/no-id'):
            with self.assertRaises(ValueError): notion_database_id(value)

    def test_read_only_source_resolution(self):
        calls = []
        def transport(request, timeout):
            calls.append(request)
            return {'status': 200, 'body': json.dumps({'data_sources': [{'id': PAGE}]})}
        result = resolve_notion_database('https://notion.so/' + SOURCE, CREDS['notion_token'], transport=transport)
        self.assertEqual(result.remote_id, PAGE)
        self.assertEqual(calls[0]['method'], 'GET')
        self.assertEqual(calls[0]['url'], 'https://api.notion.com/v1/databases/' + SOURCE)
        self.assertIsNone(calls[0]['body'])

    def test_source_resolution_no_guess_no_secret_errors(self):
        for sources, code in (([], 'no_data_sources'), ([{'id': PAGE}, {'id': SOURCE}], 'multiple_data_sources_choose_explicit_id')):
            result = resolve_notion_database('https://notion.so/' + SOURCE, CREDS['notion_token'],
                transport=lambda *_: {'status': 200, 'body': json.dumps({'data_sources': sources})})
            self.assertEqual(result.code, code)
            self.assertIsNone(result.remote_id)
        result = resolve_notion_database('https://notion.so/' + SOURCE, CREDS['notion_token'],
            transport=lambda *_: {'status': 401, 'body': CREDS['notion_token']})
        self.assertEqual(result.state, 'permanent')
        self.assertNotIn(CREDS['notion_token'], repr(result))

    def test_all_transitions_set_status_and_outage_times(self):
        payload = dict(PAYLOAD, opened_at='2026-10-24T00:00:00Z', occurred_at='2026-10-24T00:05:07.600Z', close_reason='reconnected')
        for status, name in (('offline', '장애중'), ('recovered', '복구됨'), ('monitoring_ended', '미복구 종료')):
            with self.subTest(status=status):
                sender, calls = self.sender([found(PAGE), created(PAGE)])
                self.assertEqual(sender.send('notion', dict(payload, status=status)).state, 'delivered')
                props = calls[2]['body']['properties']
                self.assertEqual(props['상태'], {'status': {'name': name}})
                self.assertEqual(props['발생 시각'], {'date': {'start': '2026-10-24T00:00:00+00:00'}})
                if status == 'offline':
                    self.assertNotIn('종료 시각', props)
                    self.assertNotIn('종료 사유', props)
                    self.assertNotIn('지속(초)', props)
                else:
                    self.assertEqual(props['종료 시각'], {'date': {'start': '2026-10-24T00:05:07+00:00'}})
                    self.assertEqual(props['종료 사유'], {'select': {'name': '장치 재연결'}})
                    self.assertEqual(props['지속(초)'], {'number': 308})

    def test_legacy_payload_without_structured_fields(self):
        # Queued before opened_at/role existed: only the offline event knows its start time.
        sender, calls = self.sender([found(PAGE), created(PAGE)])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'delivered')
        props = calls[2]['body']['properties']
        self.assertNotIn('발생 시각', props)
        self.assertNotIn('지속(초)', props)
        self.assertNotIn('역할', props)
        self.assertEqual(props['종료 시각'], {'date': {'start': '2026-10-24T00:00:00+00:00'}})

    def test_invalid_schema_never_queries_or_writes(self):
        cases = [schema(('장애중', '복구됨')), schema(('장애중', '복구됨', '미복구 종료', '복구됨')),
                 schema(('장애중', '복구됨', '미복구  종료')), schema(relation=None), schema(relation='not-a-uuid')]
        for name in NOTION_EVENT_PROPERTIES:
            for missing in (False, True):
                data = json.loads(schema()['body'])
                if missing:
                    del data['properties'][name]
                else:
                    data['properties'][name] = {'type': 'checkbox', 'checkbox': {}}
                cases.append({'status': 200, 'body': json.dumps(data)})
        for response in cases:
            with self.subTest(response=response):
                sender, calls = self.sender([], schema_response=response)
                result = sender.send('notion', PAYLOAD)
                self.assertEqual(result.state, 'permanent')
                self.assertIn(result.code, ('notion_schema_mismatch', 'notion_status_options_mismatch'))
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0]['method'], 'GET')

    def test_schema_transient_failures_retry_and_errors_redact(self):
        cases = [({'network_error': True}, 'retryable'), ({'status': 503}, 'retryable'),
                 ({'status': 429, 'retry_after': '9'}, 'retryable'),
                 ({'status': 403, 'body': CREDS['notion_token']}, 'permanent'),
                 ({'status': 200, 'oversized': True}, 'permanent')]
        for body in ('not-json-secret', '[]', '{}', '{"object":"data_source","id":"bad"}'):
            cases.append(({'status': 200, 'body': body}, 'retryable'))
        malformed = json.loads(schema()['body'])
        malformed['properties']['상태']['status']['options'] = [None]
        cases.append(({'status': 200, 'body': json.dumps(malformed)}, 'retryable'))
        for response, expected in cases:
            with self.subTest(response=response):
                sender, calls = self.sender([], schema_response=response)
                result = sender.send('notion', PAYLOAD)
                self.assertEqual(result.state, expected)
                self.assertEqual(len(calls), 1)
                self.assertNotIn(CREDS['notion_token'], repr(result))

    def test_unknown_status_never_writes(self):
        for status in ('other', None, {}):
            sender, calls = self.sender([])
            self.assertEqual(sender.send('notion', dict(PAYLOAD, status=status)).code, 'unsupported_incident_status')
            self.assertEqual(calls, [])

    def test_schema_read_query_write_share_total_budget(self):
        sender, calls = self.sender([found(PAGE), created(PAGE)])
        timeouts = []
        transport = sender.transport
        def timed(request, timeout):
            timeouts.append(timeout)
            return transport(request, timeout)
        sender.transport = timed
        with patch('notifications.time.monotonic', side_effect=[100, 101, 104, 108]):
            self.assertEqual(sender.send('notion', PAYLOAD).state, 'delivered')
        self.assertEqual(timeouts, [9, 6, 2])
        sender, calls = self.sender([])
        with patch('notifications.time.monotonic', side_effect=[100, 101, 111]):
            self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')
        self.assertEqual(len(calls), 1)

    def test_read_only_schema_preflight_all_mapping_options_required(self):
        def preflight(event=None, session=None):
            calls = []
            def transport(request, timeout):
                calls.append(request)
                if request['url'].endswith(SESSION_SOURCE):
                    return session or session_schema()
                return event or schema()
            return check_notion_schema(SOURCE, CREDS['notion_token'], transport=transport), calls
        result, calls = preflight()
        self.assertEqual(result.code, 'schema_valid_status')
        self.assertEqual([(c['method'], c['url'][-36:], c['body']) for c in calls],
                         [('GET', SOURCE, None), ('GET', SESSION_SOURCE, None)])
        self.assertEqual(preflight(event=schema(('장애중', '복구됨')))[0].code, 'notion_status_options_mismatch')
        self.assertEqual(preflight(session=session_schema(missing='처음 발생'))[0].code, 'notion_schema_mismatch')
        result = check_notion_schema(SOURCE, CREDS['notion_token'],
                                    transport=lambda *_: {'status': 403, 'body': CREDS['notion_token']})
        self.assertEqual(result.code, 'http_403')
        self.assertNotIn(CREDS['notion_token'], repr(result))

    def test_schema_helper_cli_rejects_secret_without_network(self):
        request = json.dumps({'data_source_id': SOURCE, 'token': 'bad secret'}).encode('utf-8')
        for prefix in (b'', b'\xef\xbb\xbf'):
            with self.subTest(bom=bool(prefix)):
                completed = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'notifications.py'),
                                            '--check-notion-schema'], input=prefix + request,
                                           capture_output=True, check=True, timeout=5)
                self.assertEqual(json.loads(completed.stdout)['code'], 'invalid_credentials')
                self.assertNotIn(b'bad secret', completed.stdout + completed.stderr)

    def test_schema_helper_cli_rejects_malformed_or_oversized_bytes(self):
        oversized = json.dumps({'data_source_id': SOURCE, 'token': 'bad secret', 'note': '\uac00' * 22000},
                               ensure_ascii=False).encode('utf-8')
        self.assertGreater(len(oversized), MAX_BODY)
        for request in (b'{', b'\xff', b'\xef\xbb\xbf{', oversized):
            with self.subTest(size=len(request)):
                completed = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'notifications.py'),
                                            '--check-notion-schema'], input=request,
                                           capture_output=True, check=True, timeout=5)
                self.assertEqual(json.loads(completed.stdout), {'network_error': True})
                self.assertEqual(completed.stderr, b'')
                self.assertNotIn(b'bad secret', completed.stdout)

    def test_notion_blocked_rate_limit_is_permanent(self):
        response = {'status': 429, 'body': json.dumps({'additional_data': {'rate_limit_reason': 'public_api_request_blocked'}})}
        self.assertEqual(_classify(response).state, 'permanent')


if __name__ == '__main__': unittest.main()
