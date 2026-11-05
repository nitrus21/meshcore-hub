#!/usr/bin/env python3
"""
Minimal mock of the Hub HTTP API to work on the web app without an ESP32.
Serves web/ and answers /api/* with fake data (no WebSocket: the app retries quietly).

  python tools/mock_hub.py [--port 8080] [--setup]
"""
import argparse
import json
import os
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'web')
NOW = lambda: int(time.time())
STATE = {'setup': False, 'tg_seq': 0}
SETTINGS = {'setupDone': True, 'lang': 'fr', 'theme': 'auto', 'accent': '#2f6fed', 'tz': 'CET-1CEST,M3.5.0,M10.5.0/3',
            'wifiSsid': 'Maison', 'wifiPass': '********', 'wifiAlt': [], 'apOnly': False, 'hostname': 'meshcore-hub', 'link': 'ble',
            'bleAddr': 'aa:bb:cc:dd:ee:ff', 'bleName': 'MeshCore-Hub Paris', 'tcpHost': '', 'tcpPort': 5000,
            'cellEnabled': False, 'cellAlways': False, 'cellApn': '', 'cellSimPin': '', 'cellModel': 'a7670',
            'cellTx': 43, 'cellRx': 44, 'cellPwr': -1,
            'tgEnabled': True, 'tgToken': '123:********', 'tgChats': [12345678], 'tgMode': 'single', 'tgRadioInfo': True,
            'tgRelayDm': True, 'tgRelayChannels': True, 'tgRelayRooms': True, 'tgRelayPublic': True, 'tgSkipOwn': True, 'tgInsecure': False,
            'tgMutedConvs': [], 'tgRouteOn': False, 'tgRouteKeep': False, 'tgRoutes': [], 'tgRouteFrom': [], 'historyDays': 0, 'historyMaxPct': 85, 'arduinoOta': False, 'hasPassword': True}
HOST_OK = __import__('re').compile(r'^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$')

CONTACTS = [
    {'key': 'a1' * 32, 'conv': 'c_a1a1a1a1a1a1', 'name': 'Alice', 'type': 1, 'flags': 1, 'hops': 1, 'hashSize': 1, 'path': '5e', 'lastAdvert': NOW() - 300, 'lat': 48.86, 'lon': 2.33, 'lastmod': 0},
    {'key': 'b2' * 32, 'conv': 'c_b2b2b2b2b2b2', 'name': 'Bob 🚲', 'type': 1, 'flags': 0, 'hops': -1, 'hashSize': 1, 'path': '', 'lastAdvert': NOW() - 7200, 'lastmod': 0},
    {'key': '5e' * 32, 'conv': 'c_5e5e5e5e5e5e', 'name': 'RPT-Colline', 'type': 2, 'flags': 0, 'hops': 0, 'hashSize': 1, 'path': '', 'lastAdvert': NOW() - 60, 'lat': 48.88, 'lon': 2.30, 'lastmod': 0},
    {'key': 'c3' * 32, 'conv': 'c_c3c3c3c3c3c3', 'name': 'Salon Room', 'type': 3, 'flags': 0, 'hops': 2, 'hashSize': 1, 'path': '5e7a', 'lastAdvert': NOW() - 900, 'lastmod': 0},
]
CHANNELS = [{'idx': 0, 'conv': 'ch_0', 'name': 'Public', 'secret': '8b3387e9c5cdea6ac9e5edbaa115cd72'},
            {'idx': 1, 'conv': 'ch_1', 'name': '#test', 'secret': '9cd8fcf22a47333b591d96a2b848b73f'}]
MSGS = {
    'ch_0': [{'i': 1, 't': NOW() - 4000, 'd': 0, 'a': 'Marc', 'x': 'Quelqu\'un sur le canal ?', 's': 6.5, 'h': 2},
             {'i': 2, 't': NOW() - 3900, 'd': 1, 'a': 'Hub', 'x': 'Oui, reçu 5/5 depuis Paris', 'k': 1},
             {'i': 3, 't': NOW() - 100, 'd': 0, 'a': 'Julie', 'x': '@[Marc] test de portée OK 📡', 's': -3.25, 'h': 4}],
    'c_a1a1a1a1a1a1': [{'i': 4, 't': NOW() - 600, 'd': 0, 'x': 'Salut ! Tu es dispo ce soir ?', 's': 9.0, 'h': -1},
                       {'i': 5, 't': NOW() - 500, 'd': 1, 'x': 'Oui vers 19h', 'k': 2, 'r': 1450},
                       {'i': 6, 't': NOW() - 30, 'd': 1, 'x': 'Tu as reçu ?', 'k': 3}],
}
NODE = {'name': 'Hub Paris', 'key': 'ee' * 32, 'lat': 48.8566, 'lon': 2.3522, 'txPower': 20, 'maxTxPower': 22, 'freq': 869.618, 'bw': 62.5,
        'sf': 8, 'cr': 8, 'multiAcks': 0, 'advertLocPolicy': 0, 'telemetryModes': 0, 'manualAdd': 0, 'fwVer': 14, 'fwVersion': 'v1.12.0',
        'buildDate': '15 Oct 2026', 'manufacturer': 'Heltec V3', 'maxContacts': 350, 'maxChannels': 8, 'blePin': 123456, 'repeat': False, 'pathHashMode': 0}


def status():
    return {'auth': True, 'setupDone': not STATE['setup'], 'version': '1.0.0-mock', 'uptime': 3600, 'heap': 120000, 'minHeap': 90000,
            'time': NOW(), 'timeValid': True, 'hostname': 'meshcore-hub', 'lang': 'fr', 'theme': 'auto', 'accent': '#2f6fed', 'hasPassword': True,
            'wifi': {'mode': 1, 'connected': True, 'ssid': 'Maison', 'rssi': -58, 'ip': '192.168.1.42', 'apIp': '0.0.0.0', 'apSsid': ''},
            'link': {'type': 'ble', 'state': 'ready', 'phase': 'up', 'error': '', 'name': 'MeshCore-Hub Paris', 'addr': 'aa:bb:cc:dd:ee:ff', 'rssi': -61, 'lastRx': 2},
            'lastSync': NOW() - 20, 'node': NODE, 'battery': {'mv': 4012, 'pct': 84, 'usedKb': 120, 'totalKb': 1024},
            'telegram': {'enabled': SETTINGS['tgEnabled'], 'online': True, 'bot': 'Hub', 'user': 'monhub_bot', 'error': '', 'claiming': False,
                         'queued': 0, 'chats': len(SETTINGS['tgChats']), 'topicsEnabled': False,
                         'test': {'seq': STATE['tg_seq'], 'pending': False, 'ok': True, 'name': 'Hub', 'user': 'monhub_bot', 'err': ''}},
            'cell': {'supported': True, 'online': False, 'state': 'waiting_wifi' if SETTINGS['cellEnabled'] else 'off'},
            'unread': 2, 'fsUsed': 210000, 'fsTotal': 720896}


class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def log_message(self, *a):
        pass

    def send_json(self, obj, code=200):
        b = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        p = self.path.split('?')[0]
        q = dict(x.split('=', 1) for x in self.path.split('?', 1)[1].split('&')) if '?' in self.path else {}
        routes = {
            '/api/status': status, '/api/contacts': lambda: CONTACTS,
            '/api/channels': lambda: {'channels': CHANNELS, 'max': 8},
            '/api/conversations': lambda: [{'id': k, 'name': k, 'kind': 1 if k.startswith('ch') else 0, 'lastTs': v[-1]['t'], 'lastId': v[-1]['i'],
                                            'preview': v[-1]['x'], 'lastAuthor': v[-1].get('a', ''), 'lastOut': bool(v[-1]['d']), 'unread': 1,
                                            'pinned': False, 'muted': False, 'archived': False} for k, v in MSGS.items()],
            '/api/messages': lambda: {'messages': MSGS.get(q.get('conv'), []), 'max': 140 if q.get('conv', '').startswith('ch') else 160},
            '/api/settings': lambda: {'settings': SETTINGS, 'savedRooms': [], 'net': {}},
            '/api/wifi': lambda: {'done': True, 'networks': [{'ssid': 'Maison', 'rssi': -52, 'secure': True}, {'ssid': 'Boitier_4G', 'rssi': -67, 'secure': True}, {'ssid': 'Gite_invites', 'rssi': -74, 'secure': False}, {'ssid': 'Voisin', 'rssi': -80, 'secure': True}], 'net': {'test': 'ok', 'ip': '192.168.1.42'}},
            '/api/ble': lambda: {'scanning': False, 'devices': []},
            '/api/logs': lambda: {'lines': ['+1 I MeshCore Hub mock', '+2 W exemple d\'avertissement']},
        }
        if p in routes:
            return self.send_json(routes[p]())
        if p.startswith('/api/'):
            return self.send_json({'ok': False, 'err': 'not_found'}, 404)
        return super().do_GET()

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = json.loads(self.rfile.read(n) or b'{}') if n else {}
        op = body.get('op') if self.path == '/api/do' else None
        if op == 'send':
            conv = body['conv']
            MSGS.setdefault(conv, []).append({'i': NOW(), 't': NOW(), 'd': 1, 'x': body['text'], 'k': 1})
            return self.send_json({'ok': True, 'id': NOW()})
        if op == 'settings':
            patch = body.get('settings', {})
            if 'hostname' in patch:
                hn = str(patch['hostname']).strip().lower().removesuffix('.local')
                if not HOST_OK.match(hn):
                    return self.send_json({'ok': False, 'err': 'bad_hostname'})
                patch['hostname'] = hn
            SETTINGS.update({k: v for k, v in patch.items() if not (isinstance(v, str) and '****' in v)})
            return self.send_json({'ok': True, 'settings': SETTINGS})
        if op == 'tg_test':
            STATE['tg_seq'] += 1  # resolved through /api/status (no WebSocket in the mock)
            return self.send_json({'ok': True})
        if op == 'tg_route_invite':
            name = str(body.get('name', '')).strip().lower()
            if not name or len(name) > 20 or not all(c.isascii() and (c.isalnum() or c in '-_') for c in name):
                return self.send_json({'ok': False, 'err': 'bad_route_name'})
            return self.send_json({'ok': True, 'code': '0123456789abcdef', 'user': 'monhub_bot'})
        if op == 'tg_route_del':
            SETTINGS['tgRoutes'] = [r for r in SETTINGS['tgRoutes'] if r['name'] != body.get('name')]
            return self.send_json({'ok': True, 'settings': SETTINGS})
        if op == 'tg_route_from':
            v = [c for c in SETTINGS['tgRouteFrom'] if c != body.get('conv')]
            if body.get('on'):
                if len(v) >= 32:
                    return self.send_json({'ok': False, 'err': 'route_senders_full'})
                v.append(body.get('conv'))
            SETTINGS['tgRouteFrom'] = v
            return self.send_json({'ok': True, 'settings': SETTINGS})
        if op == 'link_restore':
            return self.send_json({'ok': True, 'restored': False})
        if op == 'password':
            return self.send_json({'ok': True, 'reboot': SETTINGS['arduinoOta']})
        self.send_json({'ok': True})

    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache')  # like the Hub: never run a stale app.js
        super().end_headers()


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--setup', action='store_true', help='start in setup-wizard mode')
    a = ap.parse_args()
    STATE['setup'] = a.setup
    print(f'mock hub on http://localhost:{a.port}  (setup={a.setup})')
    ThreadingHTTPServer(('127.0.0.1', a.port), H).serve_forever()
