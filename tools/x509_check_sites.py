"""Fetch the certificate chain of some sites with openssl and check it with silicon's X.509 code (host build).

    g++ -O2 -DX509_HOST -x c++ tools/x509_host_test.cpp -o x509_test
    python tools/x509_check_sites.py ./x509_test host1 host2 ...
Sites that should fail (expired.badssl.com, wrong.host.badssl.com, self-signed.badssl.com, untrusted-root.badssl.com) are good tests.
"""
import os
import re
import subprocess
import sys
import tempfile

exe = sys.argv[1]
for host in sys.argv[2:]:
    try:
        out = subprocess.run(["openssl", "s_client", "-connect", host + ":443", "-servername", host, "-showcerts"],
                             input=b"", capture_output=True, timeout=20).stdout.decode("latin-1")
    except Exception as e:
        print("%-28s fetch failed: %s" % (host, e))
        continue
    pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", out, re.S)
    if not pems:
        print("%-28s no certificates" % host)
        continue
    d = tempfile.mkdtemp()
    files = []
    for i, p in enumerate(pems):
        der = subprocess.run(["openssl", "x509", "-outform", "DER"], input=p.encode(), capture_output=True).stdout
        f = os.path.join(d, "c%d.der" % i)
        open(f, "wb").write(der)
        files.append(f)
    r = subprocess.run([exe, "chain", host] + files, capture_output=True)
    print("%-28s (%d certs)  %s" % (host, len(files), r.stdout.decode().strip()))
