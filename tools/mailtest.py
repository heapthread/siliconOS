"""A tiny POP3S + SMTPS server for testing silicon's Mail app (self-signed certificate, any login works).

    python tools/mailtest.py            POP3 over TLS on 995, SMTP over TLS on 465
    then in silicon: write /home/.config/mail with  pop=10.0.2.2  smtp=10.0.2.2  user=u  pass=p
Mail sent from silicon is printed here and saved to mailtest_sent.txt.
"""
import base64
import datetime
import os
import socket
import ssl
import sys
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

MESSAGES = [
    "From: Ada Lovelace <ada@example.org>\r\nTo: user@silicon\r\nSubject: Hello from the test server\r\nDate: Sat, 07 Nov 2026 12:00:00 +0000\r\n\r\nThis message came over POP3 inside TLS.\r\nSecond line.\r\n",
    "From: Grace Hopper <grace@example.org>\r\nTo: user@silicon\r\nSubject: Second message\r\nDate: Sat, 07 Nov 2026 12:05:00 +0000\r\n\r\nA second, shorter message.\r\n",
]


def make_cert(kind):
    if kind == "ec":
        key = ec.generate_private_key(ec.SECP256R1())
    else:
        from cryptography.hazmat.primitives.asymmetric import rsa
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "silicon-test")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(1).not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
            .sign(key, hashes.SHA256()))
    open("mailtest.pem", "wb").write(cert.public_bytes(serialization.Encoding.PEM) +
                                     key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))


def line(f):
    return f.readline().decode("latin-1").rstrip("\r\n")


def pop3(conn):
    f = conn.makefile("rwb")
    def w(s): f.write((s + "\r\n").encode()); f.flush()
    w("+OK silicon test pop3 ready")
    while True:
        l = line(f)
        if not l:
            return
        cmd = l.split(" ")[0].upper()
        arg = l.split(" ")[1:]
        print("pop3 <", l if cmd != "PASS" else "PASS ***")
        if cmd == "USER": w("+OK")
        elif cmd == "PASS": w("+OK logged in")
        elif cmd == "STAT": w("+OK %d %d" % (len(MESSAGES), sum(len(m) for m in MESSAGES)))
        elif cmd == "LIST":
            w("+OK")
            for i, m in enumerate(MESSAGES): w("%d %d" % (i + 1, len(m)))
            w(".")
        elif cmd == "UIDL":
            w("+OK")
            for i, m in enumerate(MESSAGES): w("%d uid%d" % (i + 1, i + 1))
            w(".")
        elif cmd in ("RETR", "TOP"):
            i = int(arg[0]) - 1
            if not 0 <= i < len(MESSAGES): w("-ERR no such message"); continue
            m = MESSAGES[i]
            if cmd == "TOP":
                head, _, body = m.partition("\r\n\r\n")
                m = head + "\r\n\r\n" + "\r\n".join(body.split("\r\n")[:int(arg[1])])
            w("+OK")
            for ln in m.rstrip("\r\n").split("\r\n"):
                w(("." + ln) if ln.startswith(".") else ln)
            w(".")
        elif cmd == "DELE": w("+OK deleted")
        elif cmd == "CAPA": w("+OK"); w("USER"); w("UIDL"); w("TOP"); w(".")
        elif cmd == "QUIT": w("+OK bye"); return
        else: w("-ERR unknown command")


def smtp(conn):
    f = conn.makefile("rwb")
    def w(s): f.write((s + "\r\n").encode()); f.flush()
    w("220 silicon test smtp ready")
    data_mode = False
    body = []
    while True:
        l = line(f)
        if not l and not data_mode:
            try:
                if f.closed: return
            except Exception:
                return
        if data_mode:
            if l == ".":
                data_mode = False
                text = "\n".join(body)
                print("---- received mail ----\n" + text + "\n------------------------")
                open("mailtest_sent.txt", "a").write(text + "\n=====\n")
                w("250 queued")
                body = []
            else:
                body.append(l[1:] if l.startswith("..") else l)
            continue
        cmd = l.split(" ")[0].upper()
        print("smtp <", l[:60])
        if cmd == "EHLO": w("250-silicon.test"); w("250 AUTH LOGIN PLAIN")
        elif cmd == "AUTH":
            w("334 VXNlcm5hbWU6"); line(f); w("334 UGFzc3dvcmQ6"); line(f); w("235 ok")
        elif cmd in ("MAIL", "RCPT"): w("250 ok")
        elif cmd == "DATA": w("354 go ahead"); data_mode = True
        elif cmd == "QUIT": w("221 bye"); return
        elif cmd == "RSET" or cmd == "NOOP": w("250 ok")
        else: w("500 what?")


def serve(port, handler, ctx):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(4)
    while True:
        c, a = s.accept()
        def run(c=c):
            try:
                tc = ctx.wrap_socket(c, server_side=True)
                print("tls", port, tc.version(), tc.cipher()[0])
                handler(tc)
            except Exception as e:
                print("error", port, repr(e))
            finally:
                try: c.close()
                except Exception: pass
        threading.Thread(target=run, daemon=True).start()


if __name__ == "__main__":
    kind = sys.argv[1] if len(sys.argv) > 1 else "rsa"
    make_cert(kind)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain("mailtest.pem")
    if len(sys.argv) > 2 and sys.argv[2] == "tls12":
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    threading.Thread(target=serve, args=(465, smtp, ctx), daemon=True).start()
    serve(995, pop3, ctx)
