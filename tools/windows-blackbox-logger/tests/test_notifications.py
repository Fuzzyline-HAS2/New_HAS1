import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from notifications import NotificationSender, _allowed_url, _classify, _http_child, _transport, notion_database_id, resolve_notion_database, validate_config

SOURCE = '12345678-1234-1234-1234-123456789abc'
PAGE = 'abcdefab-1234-1234-1234-123456789abc'
# Deliberately synthetic, no live endpoint or credential is used by these tests.
CREDS = {'slack_webhook_url': 'https://hooks.slack.com/services/TEST/TEST/TEST', 'notion_token': 'synthetic_token_123'}
PAYLOAD = {'incident_id': 'incident-1', 'site_id': 'store-1', 'device_mac': 'AA:BB:CC:DD:EE:FF',
           'game_session_id': 'game-1', 'status': 'recovered', 'summary': '<!channel> test', 'occurred_at': '2026-10-24T00:00:00Z'}


class NotificationsTests(unittest.TestCase):
    def sender(self, responses):
        calls = []
        def transport(request, timeout):
            calls.append(request)
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 10)
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
        self.assertEqual(sender.send('slack', PAYLOAD).state, 'delivered')
        section = calls[0]['body']['blocks'][0]['text']
        self.assertEqual(section['type'], 'plain_text')
        self.assertIn('recovered', section['text'])
        self.assertNotIn('Authorization', calls[0])

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

    def test_notion_create_uses_datasource_and_incident_filter(self):
        sender, calls = self.sender([{'status': 200, 'body': '{"results":[],"has_more":false}'},
                                    {'status': 200, 'body': json.dumps({'id': PAGE})}])
        result = sender.send('notion', PAYLOAD)
        self.assertEqual(result.remote_id, PAGE)
        self.assertEqual(calls[0]['body']['filter']['rich_text']['equals'], 'incident-1')
        self.assertEqual(calls[1]['body']['parent'], {'type': 'data_source_id', 'data_source_id': SOURCE})
        self.assertEqual(calls[1]['body']['properties']['Status']['rich_text'][0]['text']['content'], 'recovered')

    def test_notion_update_does_not_create_duplicate(self):
        sender, calls = self.sender([{'status': 200, 'body': json.dumps({'results': [{'id': PAGE}]})},
                                    {'status': 200, 'body': json.dumps({'id': PAGE})}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'delivered')
        self.assertEqual(calls[1]['method'], 'PATCH')
        self.assertNotIn('parent', calls[1]['body'])

    def test_notion_duplicate_records_fail_closed(self):
        sender, calls = self.sender([{'status': 200, 'body': json.dumps({'results': [{'id': PAGE}, {'id': PAGE}]})}])
        self.assertEqual(sender.send('notion', PAYLOAD).code, 'duplicate_incident_records')
        self.assertEqual(len(calls), 1)

    def test_notion_unknown_create_and_safe_query_retry(self):
        sender, _ = self.sender([{'status': 200, 'body': '{"results":[]}'}, {'network_error': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'unknown')
        sender, _ = self.sender([{'network_error': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')

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
        sender, _ = self.sender([{'status': 200, 'body': json.dumps({'results': [{'id': PAGE}]})},
                                {'network_error': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'retryable')

    def test_oversized_query_does_not_create(self):
        sender, calls = self.sender([{'status': 200, 'oversized': True}])
        self.assertEqual(sender.send('notion', PAYLOAD).state, 'permanent')
        self.assertEqual(len(calls), 1)

    def test_notion_database_url_parser(self):
        self.assertEqual(notion_database_id('https://www.notion.so/team/My-DB-' + SOURCE.replace('-', '') + '?v=view'), SOURCE)
        self.assertEqual(notion_database_id('https://notion.so/' + SOURCE), SOURCE)
        for value in ('https://evil.test/' + SOURCE, 'http://notion.so/' + SOURCE,
                      'https://notion.so.evil.test/' + SOURCE, 'https://user@notion.so/' + SOURCE, 'https://notion.so/no-id'):
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

    def test_notion_blocked_rate_limit_is_permanent(self):
        response = {'status': 429, 'body': json.dumps({'additional_data': {'rate_limit_reason': 'public_api_request_blocked'}})}
        self.assertEqual(_classify(response).state, 'permanent')


if __name__ == '__main__': unittest.main()
