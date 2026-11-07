"""List (and optionally dump) the files of a disk image with The Sleuth Kit (pip install pytsk3), as an independent check.

    python tools/tsk_ls.py image.img [offset_sectors]
"""
import sys
import pytsk3

img = pytsk3.Img_Info(sys.argv[1])
off = int(sys.argv[2]) * 512 if len(sys.argv) > 2 else 0
fs = pytsk3.FS_Info(img, offset=off)
print("filesystem:", fs.info.ftype)


def walk(d, prefix):
    for e in d:
        name = e.info.name.name.decode("utf-8", "replace")
        if name in (".", ".."):
            continue
        if not e.info.meta:
            continue
        if e.info.meta.type == pytsk3.TSK_FS_META_TYPE_DIR:
            print("%9s  %s/" % ("", prefix + name))
            walk(e.as_directory(), prefix + name + "/")
        else:
            size = e.info.meta.size
            data = e.read_random(0, size) if size else b""
            print("%9d  %s   %s" % (size, prefix + name, __import__("hashlib").md5(data).hexdigest()[:8]))


walk(fs.open_dir("/"), "/")
