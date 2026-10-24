#!/usr/bin/env python3
"""Write an APK's resources.arsc with the app label (app_name) from strings.xml.

usage: app_label_arsc.py base.apk strings.xml out.arsc
       app_label_arsc.py --check app.apk strings.xml

The headset's library tile shows the app label, the string resource app_name
in resources.arsc. The release APK is a repack of an earlier APK, so its
resources.arsc still carries that APK's label; this rebuilds it with the
app_name of packaging/quest/res/values/strings.xml: Apktool (APKTOOL, default
/root/android/apktool_2.12.1.jar) decodes the base's resources, the app_name
text is replaced, aapt2 (in Apktool) builds the table again.

Stops with an error when anything is off, and checks the result: parsed
again, the new table has the same resources, ids and values as the base's
except app_name, which is the label from strings.xml (Android escapes
resolved, e.g. Neitey\\'s -> Neitey's).

--check only tests that app.apk's label is the one in strings.xml (the
release zip script runs it on the finished APK).
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

APP_NAME = re.compile(r'(<string name="app_name">)(.*?)(</string>)', re.S)


def android_unescape(raw):
    """The text aapt2 stores for a simple string resource (no markup)."""
    s = raw.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        s = s[1:-1]
    elif re.search(r"(?<!\\)'", s):
        sys.exit('strings.xml: app_name %r has an unescaped apostrophe (write \\\')' % raw)
    elif re.search(r'(?<!\\)"', s):
        sys.exit('strings.xml: app_name %r has an unescaped double quote' % raw)
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append({'n': '\n', 't': '\t'}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    if '&' in s or '<' in s:
        sys.exit('strings.xml: app_name %r: XML entities/markup are not handled here' % raw)
    return ''.join(out)


def string_pool(b, off):
    """The strings of the ResStringPool chunk at off."""
    _, hs, size, count, _, flags, start, _ = struct.unpack_from('<HHIIIIII', b, off)
    utf8 = flags & 0x100
    offsets = struct.unpack_from('<%dI' % count, b, off + hs)
    strings = []
    for o in offsets:
        p = off + start + o
        if utf8:
            for _ in range(2):  # UTF-16 length, then UTF-8 byte length
                n = b[p]
                p += 1
                if n & 0x80:
                    n = (n & 0x7f) << 8 | b[p]
                    p += 1
            strings.append(b[p:p + n].decode('utf-8'))
        else:
            n = struct.unpack_from('<H', b, p)[0]
            p += 2
            if n & 0x8000:
                n = (n & 0x7fff) << 16 | struct.unpack_from('<H', b, p)[0]
                p += 2
            strings.append(b[p:p + 2 * n].decode('utf-16-le'))
    return strings, off + size


def table(b):
    """{(package, type, key, config): value} of a resources.arsc; strings resolved."""
    t, hs, size, packages = struct.unpack_from('<HHII', b, 0)
    if t != 0x0002 or size != len(b):
        sys.exit('not a resource table')
    glob, p = string_pool(b, hs)
    res = {}
    for _ in range(packages):
        ct, chs, csize, pid = struct.unpack_from('<HHII', b, p)
        if ct != 0x0200:
            sys.exit('unexpected chunk 0x%x' % ct)
        name = b[p + 12:p + 12 + 256].decode('utf-16-le').split('\0')[0]
        type_off, _, key_off = struct.unpack_from('<III', b, p + 268)
        types, _ = string_pool(b, p + type_off)
        keys, _ = string_pool(b, p + key_off)
        q = p + chs
        while q < p + csize:
            kt, khs, ksize = struct.unpack_from('<HHI', b, q)
            if kt == 0x0201:  # ResTable_type
                tid, tflags, _, count, entries_start = struct.unpack_from('<BBHII', b, q + 8)
                if tflags:
                    sys.exit('sparse/offset16 type chunks are not handled')
                config = b[q + 20:q + khs]
                offs = struct.unpack_from('<%dI' % count, b, q + khs)
                for i, o in enumerate(offs):
                    if o == 0xffffffff:
                        continue
                    e = q + entries_start + o
                    esize, eflags, key = struct.unpack_from('<HHI', b, e)
                    if eflags & 0x0009:
                        sys.exit('complex (bag) or compact entries are not handled')
                    _, _, dtype, data = struct.unpack_from('<HBBI', b, e + esize)
                    value = ('str', glob[data]) if dtype == 0x03 else (dtype, data)
                    rid = pid << 24 | tid << 16 | i
                    res[(rid, name, types[tid - 1], keys[key], config)] = value
            q += ksize
        p += csize
    return res


def apktool(jar, *args):
    env = dict(os.environ, JAVA_TOOL_OPTIONS='')
    r = subprocess.run(['java', '-jar', jar] + list(args), env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.exit('apktool %s failed:\n%s' % (args[0], r.stdout))


def read_label(strings_xml):
    """(raw, resolved) app_name of strings.xml."""
    m = APP_NAME.findall(open(strings_xml, encoding='utf-8').read())
    if len(m) != 1:
        sys.exit('%s: expected one app_name string, found %d' % (strings_xml, len(m)))
    return m[0][1], android_unescape(m[0][1])


def check(apk, strings_xml):
    _, label = read_label(strings_xml)
    with zipfile.ZipFile(apk) as z:
        t = table(z.read('resources.arsc'))
    found = [v for k, v in t.items() if k[2] == 'string' and k[3] == 'app_name']
    if found != [('str', label)]:
        sys.exit('%s: app label is %r, expected %r (package it with app_label_arsc.py)' % (apk, found, label))
    print('%s: app label %r' % (apk, label))


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--check':
        return check(sys.argv[2], sys.argv[3])
    if len(sys.argv) != 4:
        sys.exit(__doc__.split('\n\n')[1])
    base, strings_xml, out = sys.argv[1:4]
    jar = os.environ.get('APKTOOL', '/root/android/apktool_2.12.1.jar')
    if not os.path.isfile(jar):
        sys.exit('Apktool not found: %s (set APKTOOL)' % jar)
    raw, label = read_label(strings_xml)

    work = tempfile.mkdtemp(prefix='app-label-arsc-')
    try:
        # Only the manifest and the resources: Apktool wants an APK, not the dex/libs.
        small = os.path.join(work, 'res.apk')
        with zipfile.ZipFile(base) as z, zipfile.ZipFile(small, 'w') as s:
            old = z.read('resources.arsc')
            for n in z.namelist():
                if n in ('AndroidManifest.xml', 'resources.arsc') or n.startswith('res/'):
                    s.writestr(n, z.read(n))
        dec = os.path.join(work, 'dec')
        apktool(jar, 'd', '-s', '-f', '-o', dec, small)
        path = os.path.join(dec, 'res', 'values', 'strings.xml')
        text = open(path, encoding='utf-8').read()
        text, n = APP_NAME.subn(lambda g: g.group(1) + raw + g.group(3), text)
        if n != 1:
            sys.exit('decoded strings.xml: expected one app_name, found %d' % n)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
        built = os.path.join(work, 'built.apk')
        apktool(jar, 'b', '-o', built, dec)
        with zipfile.ZipFile(built) as z:
            new = z.read('resources.arsc')

        a, b = table(old), table(new)
        label_keys = [k for k in a if k[2] == 'string' and k[3] == 'app_name']
        if len(label_keys) != 1:
            sys.exit('check failed: the base has %d app_name entries' % len(label_keys))
        expect = dict(a)
        expect[label_keys[0]] = ('str', label)
        if b != expect:
            diff = sorted(set(a) ^ set(b)) + [k for k in a if k in b and k != label_keys[0] and a[k] != b[k]]
            sys.exit('check failed: the rebuilt table differs beyond app_name: %r' % diff[:8])
        with open(out, 'wb') as f:
            f.write(new)
        print('%s: %d bytes, app_name %r -> %r, %d other resources unchanged'
              % (out, len(new), a[label_keys[0]][1], label, len(a) - 1))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
