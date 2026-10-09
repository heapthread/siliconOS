"""A tiny WireGuard server for testing silicon's VPN client (answers pings sent through the tunnel).

    python tools/wgpeer.py            listens on UDP 51820 and prints its public key
    then, inside silicon:  vpn setup 10.0.2.2:51820 <that key> 10.7.0.2 10.7.0.0/24
                           vpn on
                           ping 10.7.0.1

The handshake uses the `noiseprotocol` package (an independent Noise implementation), so a successful
handshake shows silicon follows the real WireGuard handshake; transport uses `cryptography`.
"""
import base64
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives import serialization
from noise.connection import NoiseConnection, Keypair
import hashlib

PORT = 51820
KEY_FILE = "wgpeer.key"


def load_key():
    try:
        raw = open(KEY_FILE, "rb").read()
    except OSError:
        k = X25519PrivateKey.generate()
        raw = k.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
        open(KEY_FILE, "wb").write(raw)
    return raw


def checksum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def main():
    priv = load_key()
    pub = X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    print("server public key:", base64.b64encode(pub).decode(), flush=True)
    mac1_key = hashlib.blake2s(b"mac1----" + pub).digest()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    sessions = {}
    my_index = 0x11223344
    while True:
        data, addr = sock.recvfrom(2048)
        t = struct.unpack("<I", data[:4])[0]
        if t == 1 and len(data) == 148:
            mac = hashlib.blake2s(data[:116], key=mac1_key, digest_size=16).digest()
            if mac != data[116:132]:
                print("bad mac1")
                continue
            sender = struct.unpack("<I", data[4:8])[0]
            n = NoiseConnection.from_name(b"Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s")
            n.set_as_responder()
            n.set_keypair_from_private_bytes(Keypair.STATIC, priv)
            n.set_psks(b"\0" * 32)
            n.set_prologue(b"WireGuard v1 zx2c4 Jason@zx2c4.com")
            n.start_handshake()
            try:
                ts = n.read_message(data[8:116])
            except Exception as e:
                print("handshake failed:", e)
                continue
            peer_pub = n.noise_protocol.handshake_state.rs.public_bytes
            reply = n.write_message(b"")
            assert n.handshake_finished
            resp = struct.pack("<III", 2, my_index, sender) + reply
            # the initiator's static key is the peer's mac1 key source
            pmac_key = hashlib.blake2s(b"mac1----" + peer_pub).digest()
            resp += hashlib.blake2s(resp, key=pmac_key, digest_size=16).digest() + b"\0" * 16
            sock.sendto(resp, addr)
            # initiator sends with the first cipher state, receives with the second
            recv_c, send_c = n.noise_protocol.cipher_state_decrypt, n.noise_protocol.cipher_state_encrypt
            sessions[my_index] = dict(
                peer_index=sender,
                recv=ChaCha20Poly1305(recv_c.k),
                send=ChaCha20Poly1305(send_c.k),
                counter=0,
            )
            print("handshake ok with", addr, "timestamp", ts.hex(), flush=True)
        elif t == 4 and len(data) >= 32:
            idx = struct.unpack("<I", data[4:8])[0]
            s = sessions.get(idx)
            if not s:
                continue
            ctr = struct.unpack("<Q", data[8:16])[0]
            nonce = b"\0" * 4 + struct.pack("<Q", ctr)
            try:
                plain = s["recv"].decrypt(nonce, data[16:], b"")
            except Exception:
                print("transport packet failed to authenticate")
                continue
            if not plain:
                print("keepalive")
                continue
            ihl = (plain[0] & 15) * 4
            proto = plain[9]
            src, dst = plain[12:16], plain[16:20]
            tot = struct.unpack("!H", plain[2:4])[0]
            if proto == 1 and plain[ihl] == 8:
                icmp = bytearray(plain[ihl:tot])
                icmp[0] = 0
                icmp[2:4] = b"\0\0"
                icmp[2:4] = struct.pack("!H", checksum(bytes(icmp)))
                ip = bytearray(20)
                ip[0] = 0x45
                ip[2:4] = struct.pack("!H", 20 + len(icmp))
                ip[8] = 64
                ip[9] = 1
                ip[12:16] = dst
                ip[16:20] = src
                ip[10:12] = struct.pack("!H", checksum(bytes(ip)))
                pkt = bytes(ip) + bytes(icmp)
                pad = pkt + b"\0" * ((-len(pkt)) % 16)
                c = s["counter"]
                s["counter"] += 1
                out = struct.pack("<IIQ", 4, s["peer_index"], c) + s["send"].encrypt(b"\0" * 4 + struct.pack("<Q", c), pad, b"")
                sock.sendto(out, addr)
                print("echo reply to", ".".join(map(str, src)), flush=True)
            else:
                print("packet proto", proto, "len", tot, flush=True)


if __name__ == "__main__":
    main()
