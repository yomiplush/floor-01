"""Exercise the real HTTP server and audio state, without a physical audio device."""
import http.client
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import time
from urllib.parse import urlsplit

root = Path(__file__).resolve().parents[1]
with socket.socket() as reserve:
    reserve.bind(('127.0.0.1', 0))
    port = reserve.getsockname()[1]
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='generic', SDL_AUDIODRIVER='dummy')
process = subprocess.Popen([str(root/'build/floor01'), '--lan', '--lan-quiet', '--lan-address', '127.0.0.1', '--lan-port', str(port)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    assert select.select([process.stdout], [], [], 10)[0], 'Server did not start'
    line = process.stdout.readline().strip()
    assert line.startswith('FLOOR_LAN_URL='), (line, process.poll())
    url = urlsplit(line.split('=', 1)[1])

    def request(path='/state', body=None, token=url.fragment, extra=None):
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
        headers = {'Authorization': 'Bearer '+token}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        headers.update(extra or {})
        c.request('POST' if body is not None else 'GET', path, json.dumps(body) if body is not None else None, headers)
        response = c.getresponse()
        status, data = response.status, response.read()
        c.close()
        return status, data

    def state():
        status, data = request()
        assert status == 200
        return json.loads(data)

    def command(op, **args):
        assert request('/command', {'op': op, 'client': 'test', **args})[0] == 200, op

    assert request('/')[0] == 200
    assert b'FLOOR' in request('/')[1]
    assert request(token='incorrect')[0] == 403
    assert request(extra={'Origin': 'http://untrusted.invalid'})[0] == 403
    assert request(extra={'Host': 'untrusted.invalid'})[0] == 403
    assert request('/command', {'op':'playing', 'value':True}, token='incorrect')[0] == 403
    assert not state()['playing']
    for payload in [dict(op='step', track=8, step=0, scene=0, value=1), dict(op='bpm', value=999), dict(op='bpm', value=126.5), dict(op='playing', value='true'), dict(op='xy', x=-1, y=0), dict(op='fill', value=True), dict(op='unknown')]:
        assert request('/command', payload)[0] == 400, payload
    command('bpm', value=140)
    command('scene', value=2)
    command('step', track=0, step=1, scene=2, value=2)
    command('level', track=3, value=23)
    command('mute', track=4, value=True)
    command('swing', value=50)
    command('drive', value=38)
    command('volume', value=47)
    command('xy', x=.3, y=.7)
    command('kill', value=True)
    s = state()
    assert s['bpm']==140 and s['scene']==2 and s['pattern'][0][1]==2
    assert abs(s['levels'][3]-.23)<.001 and s['mute'][4]
    assert abs(s['swing']-.225)<.001 and abs(s['drive']-.38)<.001
    assert abs(s['volume']-.47)<.001 and abs(s['cutoff']-.3)<.001 and abs(s['echo']-.7)<.001 and s['kill']
    command('reset')
    assert abs(state()['echo']-.12)<.001 and state()['cutoff']==1
    command('fill', value=True)
    assert state()['fill']
    time.sleep(1.45)
    assert not state()['fill'], 'Disconnected fill must expire'
    command('playing', value=True)
    command('scene', value=1)
    time.sleep(2)
    assert state()['scene']==1 and state()['playing']
    command('fill', value=True)
    command('playing', value=False)
    assert not state()['playing'] and not state()['fill'] and state()['step']==-1
    # Partial requests are reassembled; duplicate headers are rejected.
    with socket.create_connection(('127.0.0.1', port), timeout=3) as c:
        raw = f'GET /state HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {url.fragment}\r\n\r\n'.encode()
        c.sendall(raw[:25]); time.sleep(.02); c.sendall(raw[25:])
        assert c.recv(4096).startswith(b'HTTP/1.1 200')
    with socket.create_connection(('127.0.0.1', port), timeout=3) as c:
        c.sendall(f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nHost: evil\r\n\r\n'.encode())
        assert c.recv(4096).startswith(b'HTTP/1.1 400')
    print('PASS: LAN authentication, origin/host checks, validation, controls, scene quantization, fill expiry, stop, HTTP framing')
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
