"""Offline MCP bridge tests: no compositor, screenshot tool or input daemon needed."""
import importlib.util
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / 'agent-pointer-mcp.py'
spec = importlib.util.spec_from_file_location('agent_pointer_mcp', MODULE_PATH)
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)


class FakeDaemon:
    def __init__(self, response=b'OK\n'):
        self.temp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.temp.name) / 'agent.sock')
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.path)
        self.server.listen(4)
        self.response = response
        self.received = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while True:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            with conn:
                data = conn.recv(8192)
                self.received.append(data)
                conn.sendall(self.response)

    def close(self):
        self.server.close()
        self.temp.cleanup()


class ProtocolTests(unittest.TestCase):
    def test_initialize(self):
        response = mcp.handle({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                               'params': {'protocolVersion': '2025-03-26'}})
        self.assertEqual(response['result']['protocolVersion'], '2025-03-26')
        self.assertEqual(response['result']['serverInfo']['name'], 'agent-pointer')

    def test_tools_list(self):
        response = mcp.handle({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'})
        tools = response['result']['tools']
        self.assertEqual(len(tools), 15)
        self.assertEqual(len({t['name'] for t in tools}), 15)

    def test_notification(self):
        self.assertIsNone(mcp.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'}))

    def test_invalid_params(self):
        response = mcp.handle({'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call', 'params': []})
        self.assertEqual(response['error']['code'], -32602)

    def test_unknown_method(self):
        response = mcp.handle({'jsonrpc': '2.0', 'id': 4, 'method': 'not-real'})
        self.assertEqual(response['error']['code'], -32601)


class ValidationTests(unittest.TestCase):
    def test_move_command(self):
        self.assertEqual(mcp.command('move_pointer', {'output': 'eDP-1', 'x': 720, 'y': 450}),
                         'move-to eDP-1 720 450')

    def test_reject_shell_injection_in_output(self):
        with self.assertRaises(ValueError):
            mcp.command('move_pointer', {'output': 'eDP-1;touch /tmp/bad', 'x': 1, 'y': 2})

    def test_reject_bool_integer(self):
        with self.assertRaises(ValueError):
            mcp.command('move_pointer', {'output': 'eDP-1', 'x': True, 'y': 2})

    def test_reject_newline_in_text(self):
        with self.assertRaises(ValueError):
            mcp.command('type_text', {'text': 'hello\nquit'})

    def test_utf8_limit(self):
        with self.assertRaises(ValueError):
            mcp.command('type_text', {'text': 'é' * 2100})

    def test_text_size_boundary(self):
        self.assertEqual(mcp.command('type_text', {'text': 'a' * 4089}),
                         'type ' + 'a' * 4089)
        with self.assertRaises(ValueError):
            mcp.command('type_text', {'text': 'a' * 4090})

    def test_missing_arguments(self):
        self.assertTrue(mcp.call('move_pointer', {'output': 'eDP-1'})['isError'])

    def test_unknown_arguments(self):
        self.assertTrue(mcp.call('status', {'unexpected': 1})['isError'])

    def test_scroll_range(self):
        self.assertEqual(mcp.command('scroll', {'axis': 'vertical', 'steps': -3}),
                         'scroll vertical -3')
        with self.assertRaises(ValueError):
            mcp.command('scroll', {'axis': 'vertical', 'steps': 101})


class SocketTests(unittest.TestCase):
    def test_status_round_trip(self):
        daemon = FakeDaemon(b'OK seat=agent\n')
        try:
            with patch.object(mcp, 'SOCKET_PATH', daemon.path):
                result = mcp.call('status', {})
            self.assertFalse(result['isError'])
            self.assertEqual(result['content'][0]['text'], 'OK seat=agent')
            self.assertEqual(daemon.received, [b'status\n'])
        finally:
            daemon.close()

    def test_error_reply(self):
        daemon = FakeDaemon(b'ERR no such output\n')
        try:
            with patch.object(mcp, 'SOCKET_PATH', daemon.path):
                result = mcp.call('list_outputs', {})
            self.assertTrue(result['isError'])
        finally:
            daemon.close()

    def test_daemon_command_rejects_okay(self):
        daemon = FakeDaemon(b'OKAY invalid\n')
        try:
            with patch.object(mcp, 'SOCKET_PATH', daemon.path):
                with self.assertRaises(ValueError):
                    mcp.daemon_command('status')
        finally:
            daemon.close()


class SwayToolTests(unittest.TestCase):
    def test_list_windows_parsing(self):
        tree = {'type': 'root', 'nodes': [{'type': 'output', 'name': 'eDP-1',
                'nodes': [{'type': 'workspace', 'name': '1', 'nodes': [
                    {'type': 'con', 'id': 42, 'name': 'Terminal', 'app_id': 'terminal',
                     'rect': {'x': 10, 'y': 20, 'width': 800, 'height': 600},
                     'focused': True, 'visible': True, 'nodes': [], 'floating_nodes': []}
                ], 'floating_nodes': []}], 'floating_nodes': []}], 'floating_nodes': []}
        with patch.object(mcp, 'sway_json', return_value=tree):
            windows = mcp.window_records()
        self.assertEqual(windows[0]['id'], 42)
        self.assertEqual(windows[0]['output'], 'eDP-1')
        self.assertEqual(windows[0]['workspace'], '1')

    def test_focus_window_does_not_click_by_default(self):
        window = {'id': 42, 'output': 'eDP-1', 'visible': True, 'rect': {'x': 100, 'y': 200,
                  'width': 400, 'height': 300}}
        output = {'eDP-1': {'rect': {'x': 0, 'y': 0, 'width': 1440, 'height': 900}}}
        with patch.object(mcp, 'window_records', return_value=[window]), \
             patch.object(mcp, 'outputs_by_name', return_value=output), \
             patch.object(mcp, 'daemon_command', return_value='OK') as send:
            result = mcp.focus_window({'window_id': 42})
        self.assertFalse(result['isError'])
        send.assert_called_once_with('move-to eDP-1 299 349')

    def test_focus_window_rejects_hidden_or_unknown_visibility(self):
        for visible in (False, None):
            for click in (False, True):
                with self.subTest(visible=visible, click=click):
                    window = {'id': 42, 'output': 'eDP-1', 'visible': visible,
                              'rect': {'x': 100, 'y': 200, 'width': 400, 'height': 300}}
                    with patch.object(mcp, 'window_records', return_value=[window]), \
                         patch.object(mcp, 'daemon_command') as send:
                        result = mcp.call('focus_window', {'window_id': 42, 'click': click})
                    self.assertTrue(result['isError'])
                    self.assertIn('not visible', result['content'][0]['text'])
                    send.assert_not_called()

    def test_focus_window_clicks_visible_window_when_requested(self):
        window = {'id': 42, 'output': 'eDP-1', 'visible': True,
                  'rect': {'x': 100, 'y': 200, 'width': 400, 'height': 300}}
        output = {'eDP-1': {'rect': {'x': 0, 'y': 0, 'width': 1440, 'height': 900}}}
        with patch.object(mcp, 'window_records', return_value=[window]), \
             patch.object(mcp, 'outputs_by_name', return_value=output), \
             patch.object(mcp, 'daemon_command', return_value='OK') as send:
            result = mcp.focus_window({'window_id': 42, 'click': True})
        self.assertTrue(json.loads(result['content'][0]['text'])['clicked'])
        self.assertEqual([c.args[0] for c in send.call_args_list],
                         ['move-to eDP-1 299 349', 'click left'])

    def test_screenshot_region_geometry(self):
        output = {'DP-7': {'rect': {'x': 1440, 'y': 0, 'width': 2560, 'height': 1440}}}
        png = b'\x89PNG\r\n\x1a\n' + b'dummy'
        with patch.object(mcp, 'outputs_by_name', return_value=output), \
             patch.object(mcp, 'run_checked', return_value=png) as run:
            result = mcp.screenshot_region({'output': 'DP-7', 'x': 10, 'y': 20,
                                            'width': 100, 'height': 80})
        self.assertFalse(result['isError'])
        self.assertEqual(result['content'][1]['type'], 'image')
        self.assertEqual(run.call_args.args[0], ['grim', '-g', '1450,20 100x80', '-t', 'png', '-'])


if __name__ == '__main__':
    unittest.main()
