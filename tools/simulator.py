#!/usr/bin/env python3
"""
MeshCore Companion simulator (TCP, port 5000) - develop MeshCore Hub without hardware.

It imitates a module flashed with the "Companion Wi-Fi" firmware:
  app -> radio : '<' + len(u16 LE) + frame
  radio -> app : '>' + len(u16 LE) + frame
Frame layouts follow meshcore-dev/MeshCore examples/companion_radio/MyMesh.cpp.

Usage:
  python tools/simulator.py [--port 5000] [--chatty 30]
Then in the Hub settings choose "Wi-Fi TCP" with the IP of this PC and port 5000.

Console commands (type while running):
  dm <name> <text>      receive a private message from a contact
  ch <idx> <name> <txt> receive a channel message
  room <author> <text>  receive a room post (signed text)
  advert <name>         a contact advertises (PUSH_ADVERT)
  new <name>            a brand new node is heard (PUSH_NEW_ADVERT)
  noack                 toggle: stop/start acknowledging sent messages
  quit
"""
import argparse
import asyncio
import hashlib
import random
import struct
import sys
import time

CMD = dict(APP_START=1, SEND_TXT_MSG=2, SEND_CHANNEL_TXT_MSG=3, GET_CONTACTS=4, GET_DEVICE_TIME=5,
           SET_DEVICE_TIME=6, SEND_SELF_ADVERT=7, SET_ADVERT_NAME=8, ADD_UPDATE_CONTACT=9,
           SYNC_NEXT_MESSAGE=10, SET_RADIO_PARAMS=11, SET_RADIO_TX_POWER=12, RESET_PATH=13,
           SET_ADVERT_LATLON=14, REMOVE_CONTACT=15, SHARE_CONTACT=16, EXPORT_CONTACT=17,
           IMPORT_CONTACT=18, REBOOT=19, GET_BATT_AND_STORAGE=20, DEVICE_QUERY=22, SEND_LOGIN=26,
           SEND_STATUS_REQ=27, LOGOUT=29, GET_CONTACT_BY_KEY=30, GET_CHANNEL=31, SET_CHANNEL=32,
           SEND_TRACE_PATH=36, SET_DEVICE_PIN=37, SET_OTHER_PARAMS=38, SEND_TELEMETRY_REQ=39,
           SEND_PATH_DISCOVERY_REQ=52, GET_STATS=56, SET_AUTOADD_CONFIG=58, GET_AUTOADD_CONFIG=59,
           SET_PATH_HASH_MODE=61)
CMD_NAME = {v: k for k, v in CMD.items()}

ERR_UNSUPPORTED, ERR_NOT_FOUND, ERR_TABLE_FULL, ERR_BAD_STATE, ERR_FILE_IO, ERR_ILLEGAL_ARG = 1, 2, 3, 4, 5, 6


def key_for(name):
    return hashlib.sha256(name.encode()).digest()


def fixed(s, n):
    b = s.encode()[: n - 1]
    return b + bytes(n - len(b))


class Contact:
    def __init__(self, name, ctype, lat=0.0, lon=0.0, hops=1):
        self.name, self.type = name, ctype
        self.pub = key_for(name)
        self.lat, self.lon = lat, lon
        self.path_len = hops if hops >= 0 else 0xFF
        self.path = bytes(random.randrange(256) for _ in range(max(hops, 0))) + bytes(64)
        self.last_advert = int(time.time()) - random.randrange(60, 7200)
        self.lastmod = self.last_advert
        self.flags = 0

    def frame(self, code=3):
        return (bytes([code]) + self.pub + bytes([self.type, self.flags, self.path_len]) + self.path[:64]
                + fixed(self.name, 32)
                + struct.pack('<IiiI', self.last_advert, int(self.lat * 1e6), int(self.lon * 1e6), self.lastmod))


class Radio:
    def __init__(self, chatty):
        self.name = 'Sim-Hub'
        self.pub = key_for('self')
        self.lat, self.lon = 48.8566, 2.3522
        self.freq, self.bw, self.sf, self.cr, self.tx = 869618, 62500, 8, 8, 22
        self.pin = 123456
        self.manual_add, self.telem, self.loc_policy, self.multi_acks = 0, 0, 0, 0
        self.autoadd, self.autoadd_hops = 0x1E, 0
        self.clock = int(time.time()) - 3600
        self.contacts = [
            Contact('Alice', 1, 48.8606, 2.3376, 1),
            Contact('Bob 🚲', 1, 48.8530, 2.3499, -1),
            Contact('RPT-Colline', 2, 48.8800, 2.3300, 2),
            Contact('Salon Room', 3, 48.8700, 2.3600, 1),
            Contact('Capteur Jardin', 4, 48.8450, 2.3700, 3),
            Contact('Chloé', 1, 0, 0, 0),
        ]
        pub_secret = bytes.fromhex('8b3387e9c5cdea6ac9e5edbaa115cd72')
        self.channels = {0: ('Public', pub_secret), 1: ('#test', hashlib.sha256(b'#test').digest()[:16])}
        self.queue = []
        self.writer = None
        self.ack_enabled = True
        self.chatty = chatty
        self.logged_in = set()

    # ------------------------------------------------------------ io
    def send(self, frame):
        if not self.writer:
            return
        self.writer.write(b'>' + struct.pack('<H', len(frame)) + frame)

    def push_later(self, delay, frame):
        asyncio.get_running_loop().call_later(delay, self.send, frame)

    def queue_msg(self, frame):
        self.queue.append(frame)
        self.send(b'\x83')  # PUSH_CODE_MSG_WAITING

    def find(self, key):
        for c in self.contacts:
            if c.pub[: len(key)] == key:
                return c
        return None

    def by_name(self, name):
        for c in self.contacts:
            if c.name.lower().startswith(name.lower()):
                return c
        return None

    # ------------------------------------------------------------ incoming traffic
    def rx_dm(self, c, text, txt_type=0, extra=b''):
        snr = int(random.uniform(-8, 12) * 4) & 0xFF
        path = 0xFF if c.path_len in (0, 0xFF) else c.path_len
        self.queue_msg(bytes([16, snr, 0, 0]) + c.pub[:6] + bytes([path, txt_type])
                       + struct.pack('<I', int(time.time())) + extra + text.encode())

    def rx_channel(self, idx, sender, text):
        snr = int(random.uniform(-8, 12) * 4) & 0xFF
        self.queue_msg(bytes([17, snr, 0, 0, idx, random.randrange(0, 4), 0])
                       + struct.pack('<I', int(time.time())) + f'{sender}: {text}'.encode())

    def rx_room(self, author, text):
        room = self.by_name('Salon')
        a = self.by_name(author)
        prefix = a.pub[:4] if a else key_for(author)[:4]
        self.rx_dm(room, text, 2, prefix)

    # ------------------------------------------------------------ commands
    def handle(self, f):
        code = f[0]
        print(f'  <- {CMD_NAME.get(code, code)} ({len(f)} B)')
        ok = lambda: self.send(b'\x00')
        err = lambda e: self.send(bytes([1, e]))

        if code == CMD['DEVICE_QUERY']:
            self.send(bytes([13, 14, 175, 8]) + struct.pack('<I', self.pin) + fixed('03 Nov 2026', 12)
                      + fixed('Simulator', 40) + fixed('v1.12.0-sim', 20) + bytes([0, 0]))
        elif code == CMD['APP_START']:
            self.send(bytes([5, 1, self.tx, 22]) + self.pub
                      + struct.pack('<ii', int(self.lat * 1e6), int(self.lon * 1e6))
                      + bytes([self.multi_acks, self.loc_policy, self.telem, self.manual_add])
                      + struct.pack('<II', self.freq, self.bw) + bytes([self.sf, self.cr]) + self.name.encode())
        elif code == CMD['SET_DEVICE_TIME']:
            t = struct.unpack_from('<I', f, 1)[0]
            if t >= self.clock:
                self.clock = t
                ok()
            else:
                err(ERR_ILLEGAL_ARG)
        elif code == CMD['GET_DEVICE_TIME']:
            self.send(b'\x09' + struct.pack('<I', int(time.time())))
        elif code == CMD['GET_CONTACTS']:
            self.send(b'\x02' + struct.pack('<I', len(self.contacts)))
            for c in self.contacts:
                self.send(c.frame(3))
            self.send(b'\x04' + struct.pack('<I', max(c.lastmod for c in self.contacts)))
        elif code == CMD['GET_CONTACT_BY_KEY']:
            c = self.find(f[1:33])
            self.send(c.frame(3)) if c else err(ERR_NOT_FOUND)
        elif code == CMD['GET_CHANNEL']:
            ch = self.channels.get(f[1])
            if ch:
                self.send(bytes([18, f[1]]) + fixed(ch[0], 32) + ch[1])
            elif f[1] < 8:
                self.send(bytes([18, f[1]]) + bytes(48))  # empty slot
            else:
                err(ERR_NOT_FOUND)
        elif code == CMD['SET_CHANNEL']:
            name = f[2:34].split(b'\x00')[0].decode(errors='replace')
            if name:
                self.channels[f[1]] = (name, f[34:50])
            else:
                self.channels.pop(f[1], None)
            ok()
        elif code == CMD['GET_BATT_AND_STORAGE']:
            self.send(b'\x0c' + struct.pack('<HII', random.randint(3700, 4150), 120, 1024))
        elif code == CMD['SYNC_NEXT_MESSAGE']:
            self.send(self.queue.pop(0) if self.queue else b'\x0a')
        elif code == CMD['SEND_TXT_MSG']:
            txt_type, attempt = f[1], f[2]
            c = self.find(f[7:13])
            if not c:
                return err(ERR_NOT_FOUND)
            text = f[13:].decode(errors='replace')
            if txt_type == 1:  # CLI to repeater/room
                self.send(bytes([6, 0]) + struct.pack('<II', 0, 3000))
                reply = {'ver': 'v1.12.0 (sim)', 'clock': time.strftime('%H:%M - %d/%m/%Y'),
                         'get name': f'> {c.name}', 'neighbors': '-> 3 voisins: 1a(8.5dB) 2b(3dB) 4c(-2dB)'}
                asyncio.get_running_loop().call_later(
                    1.2, self.rx_dm, c, reply.get(text.strip(), 'OK - ' + text), 1)
                return
            flood = c.path_len == 0xFF
            ack = random.getrandbits(32)
            self.send(bytes([6, 1 if flood else 0]) + struct.pack('<II', ack, 4000))
            if self.ack_enabled and random.random() < 0.9:
                self.push_later(random.uniform(0.4, 2.5), b'\x82' + struct.pack('<II', ack, random.randint(300, 2500)))
            if c.type == 1 and attempt == 0 and random.random() < 0.6:
                asyncio.get_running_loop().call_later(random.uniform(2, 5), self.rx_dm, c, f'Bien reçu : « {text[:60]} » 👍')
        elif code == CMD['SEND_CHANNEL_TXT_MSG']:
            self.send(b'\x00') if f[2] in self.channels else err(ERR_NOT_FOUND)
        elif code == CMD['SEND_LOGIN']:
            c = self.find(f[1:33])
            if not c:
                return err(ERR_NOT_FOUND)
            pw = f[33:].decode(errors='replace')
            self.send(bytes([6, 0]) + c.pub[:4] + struct.pack('<I', 3000))
            if pw == 'bad':
                self.push_later(1.0, bytes([0x86, 0]) + c.pub[:6])
            else:
                self.logged_in.add(c.pub)
                self.push_later(1.0, bytes([0x85, 1 if pw == 'admin' else 0]) + c.pub[:6]
                                + struct.pack('<I', int(time.time())) + bytes([3, 14]))
                if c.type == 3:
                    asyncio.get_running_loop().call_later(2.0, self.rx_room, 'Alice', 'Bienvenue dans le salon !')
        elif code == CMD['LOGOUT']:
            self.logged_in.discard(f[1:33])
            ok()
        elif code == CMD['SEND_STATUS_REQ']:
            c = self.find(f[1:33])
            if not c:
                return err(ERR_NOT_FOUND)
            self.send(bytes([6, 0]) + struct.pack('<II', random.getrandbits(32), 3000))
            st = struct.pack('<HHhhIIIIIIIIHhHHII', 4012, 0, -112, -87, 15234, 9876, 3600, 86400 * 3 + 5000,
                             800, 1200, 5000, 4000, 0, 26, 12, 340, 4100, 3)
            self.push_later(1.5, bytes([0x87, 0]) + c.pub[:6] + st)
        elif code == CMD['SEND_TELEMETRY_REQ']:
            c = self.find(f[4:36])
            if not c:
                return err(ERR_NOT_FOUND)
            self.send(bytes([6, 0]) + struct.pack('<II', random.getrandbits(32), 3000))
            # Cayenne LPP: ch1 voltage(0x74, /100), ch1 temperature(0x67, /10), ch1 gps(0x88)
            lpp = bytes([1, 0x74]) + struct.pack('>H', 402) + bytes([1, 0x67]) + struct.pack('>h', 215)
            lpp += bytes([2, 0x68, 110])  # humidity 55%
            lat, lon = int(c.lat * 1e4), int(c.lon * 1e4)
            lpp += bytes([1, 0x88]) + lat.to_bytes(3, 'big', signed=True) + lon.to_bytes(3, 'big', signed=True) + (3500).to_bytes(3, 'big', signed=True)
            self.push_later(1.5, bytes([0x8B, 0]) + c.pub[:6] + lpp)
        elif code == CMD['SEND_TRACE_PATH']:
            tag = struct.unpack_from('<I', f, 1)[0]
            path = f[10:]
            self.send(bytes([6, 0]) + struct.pack('<II', tag, 4000))
            snrs = bytes(int(random.uniform(-5, 12) * 4) & 0xFF for _ in path)
            self.push_later(2.0, bytes([0x89, 0, len(path), f[9]]) + struct.pack('<II', tag, 0) + path + snrs
                            + bytes([int(random.uniform(0, 10) * 4)]))
        elif code == CMD['SEND_PATH_DISCOVERY_REQ']:
            c = self.find(f[2:34])
            if not c:
                return err(ERR_NOT_FOUND)
            self.send(bytes([6, 1]) + struct.pack('<II', random.getrandbits(32), 5000))
            self.push_later(2.5, bytes([0x8D, 0]) + c.pub[:6] + bytes([2, 0x1a, 0x2b, 2, 0x2b, 0x1a]))
        elif code == CMD['SEND_SELF_ADVERT']:
            ok()
        elif code == CMD['SET_ADVERT_NAME']:
            self.name = f[1:].decode(errors='replace')
            ok()
        elif code == CMD['SET_ADVERT_LATLON']:
            la, lo = struct.unpack_from('<ii', f, 1)
            self.lat, self.lon = la / 1e6, lo / 1e6
            ok()
        elif code == CMD['SET_RADIO_PARAMS']:
            self.freq, self.bw, self.sf, self.cr = struct.unpack_from('<IIBB', f, 1)
            ok()
        elif code == CMD['SET_RADIO_TX_POWER']:
            self.tx = f[1]
            ok()
        elif code == CMD['SET_DEVICE_PIN']:
            self.pin = struct.unpack_from('<I', f, 1)[0]
            ok()
        elif code == CMD['SET_OTHER_PARAMS']:
            self.manual_add = f[1]
            if len(f) > 2: self.telem = f[2]
            if len(f) > 3: self.loc_policy = f[3]
            if len(f) > 4: self.multi_acks = f[4]
            ok()
        elif code == CMD['SET_AUTOADD_CONFIG']:
            self.autoadd = f[1]
            if len(f) > 2: self.autoadd_hops = f[2]
            ok()
        elif code == CMD['GET_AUTOADD_CONFIG']:
            self.send(bytes([25, self.autoadd, self.autoadd_hops]))
        elif code in (CMD['RESET_PATH'], CMD['SHARE_CONTACT'], CMD['SET_PATH_HASH_MODE']):
            ok()
        elif code == CMD['REMOVE_CONTACT']:
            c = self.find(f[1:33])
            if c:
                self.contacts.remove(c)
                ok()
            else:
                err(ERR_NOT_FOUND)
        elif code == CMD['ADD_UPDATE_CONTACT']:
            ok()
        elif code == CMD['EXPORT_CONTACT']:
            # header(1) + path_len(1) + advert payload: pub + ts + sig(64) + flags/name (simplified)
            c = self.find(f[1:33]) if len(f) >= 33 else None
            pub = c.pub if c else self.pub
            name = (c.name if c else self.name).encode()
            self.send(b'\x0b' + bytes([0x11, 0]) + pub + struct.pack('<I', int(time.time())) + bytes(64) + bytes([0x81]) + name)
        elif code == CMD['IMPORT_CONTACT']:
            ok()
        elif code == CMD['GET_STATS']:
            t = f[1]
            if t == 0:
                self.send(bytes([24, 0]) + struct.pack('<HIHB', 4050, int(time.monotonic()), 0, 0))
            elif t == 1:
                self.send(bytes([24, 1]) + struct.pack('<hbbII', -114, -92, 28, 360, 1200))
            elif t == 2:
                self.send(bytes([24, 2]) + struct.pack('<7I', 2400, 310, 120, 190, 1500, 900, 4))
            else:
                err(ERR_ILLEGAL_ARG)
        elif code == CMD['REBOOT']:
            print('  (reboot requested - ignored)')
        else:
            err(ERR_UNSUPPORTED)

    # ------------------------------------------------------------ background chatter
    async def chatter(self):
        lines = ['Quelqu\'un sur le canal ?', 'Test de portée depuis la colline 📡', 'Météo : grand soleil ☀️',
                 'Le relais RPT-Colline répond bien', 'Bonne soirée à tous', 'Qui a un T-Echo à vendre ?']
        while True:
            await asyncio.sleep(self.chatty * random.uniform(0.6, 1.4))
            if not self.writer:
                continue
            r = random.random()
            if r < 0.5:
                self.rx_channel(0, random.choice(['Marc', 'Julie', 'Kevin', 'Sofia']), random.choice(lines))
            elif r < 0.7:
                self.rx_channel(1, random.choice(['Léo', 'Nina']), 'ping #test')
            elif r < 0.85:
                self.rx_dm(random.choice(self.contacts[:2]), random.choice(['Tu es dispo ?', 'Merci !', 'On se capte demain']))
            else:
                c = random.choice(self.contacts)
                c.last_advert = int(time.time())
                self.send(b'\x80' + c.pub)


async def console(radio):
    loop = asyncio.get_running_loop()
    while True:
        line = (await loop.run_in_executor(None, sys.stdin.readline)).strip()
        if not line:
            continue
        cmd, *rest = line.split(' ', 1)
        arg = rest[0] if rest else ''
        try:
            if cmd == 'dm':
                name, text = arg.split(' ', 1)
                c = radio.by_name(name)
                radio.rx_dm(c, text) if c else print('unknown contact')
            elif cmd == 'ch':
                idx, name, text = arg.split(' ', 2)
                radio.rx_channel(int(idx), name, text)
            elif cmd == 'room':
                author, text = arg.split(' ', 1)
                radio.rx_room(author, text)
            elif cmd == 'advert':
                c = radio.by_name(arg)
                c and radio.send(b'\x80' + c.pub)
            elif cmd == 'new':
                c = Contact(arg or f'Node-{random.randrange(999)}', 1, 48.85 + random.random() / 20, 2.35, 1)
                radio.contacts.append(c)
                radio.send(c.frame(0x8A))
            elif cmd == 'noack':
                radio.ack_enabled = not radio.ack_enabled
                print('ACKs', 'on' if radio.ack_enabled else 'off')
            elif cmd == 'quit':
                loop.stop()
            else:
                print(__doc__)
        except ValueError:
            print('bad arguments')


async def main():
    ap = argparse.ArgumentParser(description='MeshCore Companion TCP simulator')
    ap.add_argument('--port', type=int, default=5000)
    ap.add_argument('--chatty', type=float, default=30, help='mean seconds between random incoming traffic (0 = off)')
    args = ap.parse_args()
    radio = Radio(args.chatty)

    async def client(reader, writer):
        if radio.writer:  # like the real firmware: one client at a time
            print('second client refused')
            writer.close()
            return
        peer = writer.get_extra_info('peername')
        print(f'client connected: {peer}')
        radio.writer = writer
        buf = bytearray()
        try:
            while True:
                data = await reader.read(512)
                if not data:
                    break
                buf += data
                while True:
                    i = buf.find(b'<')
                    if i < 0:
                        buf.clear()
                        break
                    del buf[:i]
                    if len(buf) < 3:
                        break
                    n = struct.unpack_from('<H', buf, 1)[0]
                    if len(buf) < 3 + n:
                        break
                    frame = bytes(buf[3:3 + n])
                    del buf[:3 + n]
                    if frame:
                        radio.handle(frame)
                await writer.drain()
        except ConnectionError:
            pass
        finally:
            print('client disconnected')
            radio.writer = None
            writer.close()

    server = await asyncio.start_server(client, '0.0.0.0', args.port)
    print(f'MeshCore Companion simulator listening on TCP {args.port}  (type "help")')
    tasks = [console(radio)]
    if args.chatty > 0:
        tasks.append(radio.chatter())
    async with server:
        await asyncio.gather(server.serve_forever(), *tasks)


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
