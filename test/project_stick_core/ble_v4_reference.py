#!/usr/bin/env python3
"""Independent BLE transfer protocol 4 generator (docs/product/BLE-TRANSFER-V4.md).
Rebuilds every stream in the mini program's vector file from the spec and compares.
Also builds full-size demo streams for the firmware host test."""
import hashlib, hmac, json, struct, sys, zlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

FRAME = 52272
def deflate(raw):
    c = zlib.compressobj(9, zlib.DEFLATED, -10, 9)
    return c.compress(raw) + c.flush()
def inflate(blob):
    return zlib.decompress(blob, -15)
def mac(key, msg):
    return hmac.new(key, msg.encode(), hashlib.sha256)
def ctr(key, nonce, offset, data):
    # AES-256-CTR, counter = nonce (128-bit big endian) + offset // 16
    n = (int.from_bytes(nonce, 'big') + offset // 16) % (1 << 128)
    enc = Cipher(algorithms.AES(key), modes.CTR(n.to_bytes(16, 'big'))).encryptor()
    skip = offset % 16
    return enc.update(b'\0' * skip + data)[skip:]
def need_hex(need):
    out = bytearray(max((len(need) + 7) // 8, 1))
    for i, n in enumerate(need):
        if n: out[i // 8] |= 1 << (i % 8)
    return out.hex()
def records(frames, need, compressed):
    out = b''
    for i, n in enumerate(need):
        if n:
            out += struct.pack('<HI', i, len(compressed[i])) + compressed[i]
    return out

def check(path):
    v = json.load(open(path))
    K, N = bytes.fromhex(v['K']), bytes.fromhex(v['N'])
    key = mac(K, 'enc|' + v['N']).digest()
    assert key.hex() == v['stream_key'], 'stream key'
    prog = v['program']
    header = bytes.fromhex(prog['header_hex'])
    assert len(header) == prog['header']
    raws = [inflate(bytes.fromhex(f['deflate'])) for f in prog['frames']]
    comp = [deflate(r) for r in raws]
    for f, r, c in zip(prog['frames'], raws, comp):
        assert len(r) == FRAME and hashlib.sha256(r).hexdigest() == f['digest'], 'frame digest'
        assert c.hex() == f['deflate'], 'deflate bytes differ from zlib wbits -10'
    full = header + b''.join(raws)
    assert len(full) == prog['size'] and hashlib.sha256(full).hexdigest() == prog['sha256'], 'program'
    single = v['single']
    errors = 0
    for case in v['cases']:
        is_single = case['begin4']['op']['header'] == 0
        frames = [raws[1]] if is_single else raws
        cmp_ = [comp[1]] if is_single else comp
        have = set(case['have'])
        need = [i not in have for i in range(len(frames))]
        assert need_hex(need) == case['need'], (case['name'], 'need')
        op = case['begin4']['op']
        msg = '|'.join(['studio4', v['device_id'], v['N'], str(v['epoch']), op['task'], op['hash'], str(op['expires']),
                        str(op['size']), str(op['header']), str(op['time'])])
        assert msg == case['begin4']['message'], case['name']
        assert mac(K, msg).hexdigest() == op['proof'], (case['name'], 'proof')
        if is_single:
            assert op['hash'] == single['sha256'] and hashlib.sha256(frames[0]).hexdigest() == op['hash']
            plain = records(frames, need, cmp_) if any(need) else b''
        else:
            plain = header + records(frames, need, cmp_)
        assert plain.hex() == case['plain'], (case['name'], 'plain')
        assert ctr(key, N, 0, plain).hex() == case['encrypted'], (case['name'], 'encrypted')
        for r in case['resumes']:
            rn = [False] * len(frames)
            ok = parse = r['need']
            # resume need: frames already done (recordwise) are removed
            got = bytes.fromhex(r['need'])
            rn = [bool(got[i // 8] >> (i % 8) & 1) for i in range(len(frames))]
            body = records(frames, rn, cmp_)
            assert ctr(key, N, r['received'], body).hex() == r['encrypted'], (case['name'], 'resume', r['received'])
    for p in v['progress']:
        b = struct.pack('<IBBHII', p['received'], p['state'], p['error'], p['needCount'], 0, p['seq'] % (1 << 32))
        assert b.hex() == p['hex'], 'progress'
    print('mini program vectors: all streams, proofs, need bitmaps and PROGRESS values match the spec')

def demo(path, out):
    b = open(path, 'rb').read()
    hl = struct.unpack('<I', b[4:8])[0]; header = b[:8 + hl]
    n = (len(b) - len(header)) // FRAME
    raws = [b[len(header) + i * FRAME: len(header) + (i + 1) * FRAME] for i in range(n)]
    comp = [deflate(r) for r in raws]
    full = header + records(raws, [True] * n, comp)
    cached = header + records(raws, [i == 3 for i in range(n)], comp)  # 16 of 17 frames cached
    open(out + '/demo-full.stream', 'wb').write(full)
    open(out + '/demo-one.stream', 'wb').write(cached)
    json.dump({'size': len(b), 'header': len(header), 'frames': n, 'sha256': hashlib.sha256(b).hexdigest(),
               'digests': [hashlib.sha256(r).hexdigest() for r in raws],
               'full_stream': len(full), 'one_stream': len(cached)}, open(out + '/demo.json', 'w'))
    print(f'demo plan: {len(b)} bytes, {n} frames -> full stream {len(full)} bytes '
          f'({len(b)/len(full):.1f}x), one changed frame {len(cached)} bytes')

if __name__ == '__main__':
    check(sys.argv[1])
    if len(sys.argv) > 3: demo(sys.argv[2], sys.argv[3])
