#!/usr/bin/env python3
"""Local MCP stdio bridge for agent-pointer (JSON-RPC 2.0)."""
import base64
import json
import subprocess
import os
import re
import socket
import sys

PROTOCOL = '2025-03-26'
SOCKET_PATH = os.environ.get('AGENT_POINTER_SOCKET') or os.path.join(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'), 'agent-pointer.sock')


def tool(name, description, properties=None, required=None):
    return {'name': name, 'description': description, 'inputSchema': {'type': 'object', 'properties': properties or {}, 'required': required or [], 'additionalProperties': False}}

INT = {'type': 'integer'}
BUTTON = {'type': 'string', 'enum': ['left', 'right', 'middle']}
TOOLS = [
    tool('status', 'Check agent-pointer daemon connection and keymap.'),
    tool('list_windows', 'List Sway windows, including hidden windows, with title, app_id, output, workspace, rect, visibility, focus and floating state.'),
    tool('screenshot', 'Capture one named active Sway output as a PNG image (read-only).', {'output': {'type': 'string'}}, ['output']),
    tool('screenshot_region', 'Capture a rectangular region in local logical coordinates of a named Sway output.', {'output': {'type': 'string'}, 'x': INT, 'y': INT, 'width': INT, 'height': INT}, ['output', 'x', 'y', 'width', 'height']),
    tool('focus_window', 'Target a visible Sway window by ID using only the agent virtual pointer. Moves pointer inside the window; optional click may activate UI and is disabled by default. Never changes seat0 focus via Sway IPC.', {'window_id': INT, 'click': {'type': 'boolean', 'default': False}}, ['window_id']),
    tool('list_outputs', 'List active Sway outputs and logical dimensions/positions.'),
    tool('move_pointer', 'Move pointer to local logical coordinates on a named output.', {'output': {'type': 'string'}, 'x': INT, 'y': INT}, ['output', 'x', 'y']),
    tool('click', 'Click a mouse button at the current agent pointer position.', {'button': BUTTON}, ['button']),
    tool('button_down', 'Hold a mouse button for dragging.', {'button': BUTTON}, ['button']),
    tool('button_up', 'Release a held mouse button.', {'button': BUTTON}, ['button']),
    tool('release_buttons', 'Release all held mouse buttons.'),
    tool('scroll', 'Scroll on the current output; signed steps, -100 to 100.', {'axis': {'type': 'string', 'enum': ['vertical', 'horizontal']}, 'steps': INT}, ['axis', 'steps']),
    tool('press_key', 'Press a named XKB keysym, e.g. Return, Escape, Tab.', {'key': {'type': 'string'}}, ['key']),
    tool('key_chord', 'Press shortcut such as Ctrl+A or Ctrl+Shift+Tab.', {'chord': {'type': 'string'}}, ['chord']),
    tool('type_text', 'Type single-line text using the startup seat0 XKB keymap snapshot. Max 4089 UTF-8 bytes.', {'text': {'type': 'string'}}, ['text']),
]
TOOL_MAP = {t['name']: t for t in TOOLS}


def send(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False, separators=(',', ':')) + '\n')
    sys.stdout.flush()


def fail(message):
    return {'content': [{'type': 'text', 'text': message}], 'isError': True}


def string(value, label, pattern=None):
    if not isinstance(value, str) or not value or '\n' in value or '\r' in value or '\x00' in value:
        raise ValueError(f'{label}: non-empty single-line string required')
    if pattern and not re.fullmatch(pattern, value, re.ASCII):
        raise ValueError(f'{label}: invalid characters')
    return value


def integer(value, label, lo, hi):
    if type(value) is not int or not lo <= value <= hi:
        raise ValueError(f'{label}: integer in [{lo}, {hi}] required')
    return value


def command(name, a):
    if name == 'status': return 'status'
    if name == 'list_outputs': return 'outputs'
    if name == 'move_pointer':
        return f"move-to {string(a['output'], 'output', r'[A-Za-z0-9_.:-]{1,127}')} {integer(a['x'], 'x', 0, 100000)} {integer(a['y'], 'y', 0, 100000)}"
    if name in ('click', 'button_down', 'button_up'):
        b = a['button']
        if b not in ('left', 'right', 'middle'): raise ValueError('invalid button')
        return f"{'click' if name == 'click' else 'down' if name == 'button_down' else 'up'} {b}"
    if name == 'release_buttons': return 'release'
    if name == 'scroll':
        axis = a['axis']
        if axis not in ('vertical', 'horizontal'): raise ValueError('invalid axis')
        return f"scroll {axis} {integer(a['steps'], 'steps', -100, 100)}"
    if name == 'press_key': return 'key ' + string(a['key'], 'key', r'[A-Za-z0-9_+-]{1,100}')
    if name == 'key_chord': return 'chord ' + string(a['chord'], 'chord', r'[A-Za-z0-9_+]{1,120}')
    if name == 'type_text':
        text = string(a['text'], 'text')
        if len(('type ' + text + '\n').encode('utf-8')) > 4095:
            raise ValueError('text too long for daemon socket command (4095-byte limit)')
        return 'type ' + text
    raise ValueError('unknown tool')


def run_checked(argv, timeout=5, binary=False):
    try:
        proc = subprocess.run(argv, capture_output=True, timeout=timeout,
                              text=not binary, check=False)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise ValueError(f'{argv[0]} failed: {e}') from e
    if proc.returncode:
        stderr = proc.stderr
        if isinstance(stderr, bytes):
            stderr = stderr.decode('utf-8', errors='replace')
        raise ValueError(f'{argv[0]} exited {proc.returncode}: {(stderr or "no stderr").strip()[:1000]}')
    return proc.stdout


def sway_json(kind):
    data = run_checked(['swaymsg', '-r', '-t', kind])
    try:
        return json.loads(data)
    except ValueError as e:
        raise ValueError(f'swaymsg {kind}: invalid JSON: {e}') from e


def outputs_by_name():
    data = sway_json('get_outputs')
    if not isinstance(data, list):
        raise ValueError('Unexpected get_outputs response')
    return {o['name']: o for o in data if isinstance(o, dict)
            and isinstance(o.get('name'), str) and o.get('active') is True}


def window_records():
    root = sway_json('get_tree')
    found = []

    def visit(node, output=None, workspace=None, floating=False):
        if not isinstance(node, dict):
            return
        kind = node.get('type')
        if kind == 'output':
            output = node.get('name')
        if kind == 'workspace':
            workspace = node.get('name')
        if kind in ('con', 'floating_con') and (node.get('app_id') or node.get('window')):
            rect = node.get('rect') or {}
            found.append({
                'id': node.get('id'), 'title': node.get('name'),
                'app_id': node.get('app_id'), 'pid': node.get('pid'),
                'output': output, 'workspace': workspace,
                'rect': {k: rect.get(k) for k in ('x', 'y', 'width', 'height')},
                'focused': bool(node.get('focused')),
                'visible': bool(node.get('visible')),
                'floating': floating or kind == 'floating_con',
            })
        for child in node.get('nodes', []):
            visit(child, output, workspace, floating)
        for child in node.get('floating_nodes', []):
            visit(child, output, workspace, True)

    visit(root)
    return found


def list_windows():
    return {'content': [{'type': 'text', 'text': json.dumps({'windows': window_records()}, ensure_ascii=False)}], 'isError': False}


def daemon_command(cmd):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(3)
        sock.connect(SOCKET_PATH)
        sock.sendall((cmd + '\n').encode('utf-8'))
        response = bytearray()
        while len(response) < 16384:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
            if b'\n' in response:
                break
    if not response:
        raise ValueError('Daemon returned no response')
    result = response.decode('utf-8', errors='replace').strip()
    if not (result == 'OK' or result.startswith('OK ')):
        raise ValueError('Daemon: ' + result)
    return result


def focus_window(args):
    window_id = integer(args['window_id'], 'window_id', 1, 2**63 - 1)
    click = args.get('click', False)
    if type(click) is not bool:
        raise ValueError('click: boolean required')
    matches = [w for w in window_records() if w['id'] == window_id]
    if len(matches) != 1:
        raise ValueError('Window ID not found')
    w = matches[0]
    if w.get('visible') is not True:
        raise ValueError('Window is not visible; refusing to target a hidden window')
    output = w['output']
    outs = outputs_by_name()
    if output not in outs:
        raise ValueError('Window is not on an active output')
    o = outs[output]
    r = w['rect']
    orc = o.get('rect') or {}
    for k in ('x', 'y', 'width', 'height'):
        if type(r.get(k)) is not int or type(orc.get(k)) is not int:
            raise ValueError('Missing window/output logical geometry')
    if r['width'] <= 0 or r['height'] <= 0:
        raise ValueError('Window has empty geometry')
    # Intersection ensures target lies on the named output, including partially
    # off-screen windows. This never sends a compositor-level focus command.
    left = max(r['x'], orc['x'])
    top = max(r['y'], orc['y'])
    right = min(r['x'] + r['width'], orc['x'] + orc['width'])
    bottom = min(r['y'] + r['height'], orc['y'] + orc['height'])
    if right <= left or bottom <= top:
        raise ValueError('Window does not intersect its active output')
    gx = (left + right - 1) // 2
    gy = (top + bottom - 1) // 2
    x, y = gx - orc['x'], gy - orc['y']
    moved = daemon_command(f'move-to {output} {x} {y}')
    result = {'window_id': window_id, 'output': output, 'local': [x, y],
              'pointer': moved, 'clicked': False,
              'note': 'Pointer targeted; keyboard focus is compositor-policy dependent.'}
    if click:
        result['click_result'] = daemon_command('click left')
        result['clicked'] = True
        result['note'] = 'Agent click sent; may activate application UI. Seat0 not focused through IPC.'
    return {'content': [{'type': 'text', 'text': json.dumps(result, ensure_ascii=False)}], 'isError': False}


def png_result(data, description):
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('grim did not return a valid PNG header')
    if len(data) > 16 * 1024 * 1024:
        raise ValueError('Screenshot exceeds 16 MiB limit')
    return {'content': [
        {'type': 'text', 'text': f'{description} ({len(data)} bytes).'},
        {'type': 'image', 'data': base64.b64encode(data).decode('ascii'), 'mimeType': 'image/png'}
    ], 'isError': False}


def screenshot_region(args):
    name = string(args['output'], 'output', r'[A-Za-z0-9_.:-]{1,127}')
    outs = outputs_by_name()
    if name not in outs:
        raise ValueError(f'Unknown or inactive output: {name}')
    rect = outs[name].get('rect') or {}
    for key in ('x', 'y', 'width', 'height'):
        if type(rect.get(key)) is not int:
            raise ValueError(f'Output {name} missing logical {key}')
    x = integer(args['x'], 'x', 0, 100000)
    y = integer(args['y'], 'y', 0, 100000)
    width = integer(args['width'], 'width', 1, 100000)
    height = integer(args['height'], 'height', 1, 100000)
    if x + width > rect['width'] or y + height > rect['height']:
        raise ValueError('Region exceeds output logical bounds')
    gx, gy = rect['x'] + x, rect['y'] + y
    geometry = f'{gx},{gy} {width}x{height}'
    data = run_checked(['grim', '-g', geometry, '-t', 'png', '-'], timeout=12, binary=True)
    return png_result(data, f'PNG region {name} local={x},{y} size={width}x{height}')


def screenshot(args):
    name = string(args['output'], 'output', r'[A-Za-z0-9_.:-]{1,127}')
    if name not in outputs_by_name():
        raise ValueError(f'Unknown or inactive output: {name}')
    data = run_checked(['grim', '-o', name, '-t', 'png', '-'], timeout=12, binary=True)
    return png_result(data, f'PNG screenshot of {name}')


def call(name, args):
    if name not in TOOL_MAP: return fail('Unknown tool: ' + str(name))
    if not isinstance(args, dict): return fail('Arguments must be an object')
    spec = TOOL_MAP[name]['inputSchema']
    unknown = set(args) - set(spec['properties'])
    missing = set(spec['required']) - set(args)
    if unknown or missing: return fail(f'Invalid arguments: missing={sorted(missing)}, unknown={sorted(unknown)}')
    try:
        if name == 'list_windows': return list_windows()
        if name == 'screenshot': return screenshot(args)
        if name == 'screenshot_region': return screenshot_region(args)
        if name == 'focus_window': return focus_window(args)
        cmd = command(name, args)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(3)
            s.connect(SOCKET_PATH)
            s.sendall((cmd + '\n').encode('utf-8'))
            response = bytearray()
            while len(response) < 16384:
                chunk = s.recv(4096)
                if not chunk: break
                response.extend(chunk)
                if b'\n' in response: break
        if not response: return fail('Daemon returned no response')
        result = response.decode('utf-8', errors='replace').strip()
        return {'content': [{'type': 'text', 'text': result}], 'isError': not (result == 'OK' or result.startswith('OK '))}
    except (OSError, TimeoutError, ValueError, KeyError) as e:
        return fail(f'{type(e).__name__}: {e} (socket: {SOCKET_PATH})')


def rpc_error(id_, code, message):
    return {'jsonrpc': '2.0', 'id': id_, 'error': {'code': code, 'message': message}}


def handle(req):
    if not isinstance(req, dict): return rpc_error(None, -32600, 'Invalid Request')
    method = req.get('method')
    id_ = req.get('id')
    if method == 'notifications/initialized' or (isinstance(method, str) and method.startswith('notifications/')):
        return None
    if 'id' not in req: return None
    if method == 'initialize':
        params = req.get('params', {})
        if params is None: params = {}
        if not isinstance(params, dict): return rpc_error(id_, -32602, 'Invalid params')
        requested = params.get('protocolVersion', PROTOCOL)
        return {'jsonrpc': '2.0', 'id': id_, 'result': {'protocolVersion': requested if requested in ('2024-11-05', '2025-03-26', '2025-06-18') else PROTOCOL, 'capabilities': {'tools': {'listChanged': False}}, 'serverInfo': {'name': 'agent-pointer', 'version': '5.2.0'}}}
    if method == 'ping': return {'jsonrpc': '2.0', 'id': id_, 'result': {}}
    if method == 'tools/list': return {'jsonrpc': '2.0', 'id': id_, 'result': {'tools': TOOLS}}
    if method == 'tools/call':
        p = req.get('params', {})
        if p is None: p = {}
        if not isinstance(p, dict): return rpc_error(id_, -32602, 'Invalid params')
        return {'jsonrpc': '2.0', 'id': id_, 'result': call(p.get('name'), p.get('arguments') or {})}
    return rpc_error(id_, -32601, 'Method not found')


def main():
    for raw in sys.stdin.buffer:
        try:
            req = json.loads(raw)
            response = handle(req)
            if response is not None: send(response)
        except Exception as e:
            print(f'agent-pointer MCP: {e}', file=sys.stderr)
            send(rpc_error(None, -32603, 'Internal error'))


if __name__ == '__main__':
    main()
