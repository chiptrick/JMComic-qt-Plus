# -*- coding: utf-8 -*-
# 试用字典序插入 QNN 库的 APK（对比"追加到尾部"模式）
import zipfile, struct, os

BASE = r'C:\Users\WQ159\source\repos\JMComic-qt\cpp\dist\JMComic-ui-0.95-vk-arm64-v8a-debug.apk'
MAIN = r'C:\Users\WQ159\AppData\Local\Temp\main.so'
QNN_DIR = r'C:\tmp\qnn-libs'
OUT = r'C:\Users\WQ159\AppData\Local\Temp\test-sorted.apk'
ALIGN = 16384
ZIPALIGN_ID = 0xd935
ENTRY = 'lib/arm64-v8a/libjmcomic_ui_arm64-v8a.so'
QNN = ['libQnnHtp.so', 'libQnnHtpV79Skel.so', 'libQnnHtpV79Stub.so', 'libQnnSystem.so']

def align_extra(zout, zi, align):
    pos = zout.fp.tell()
    header_len = 30 + len(zi.filename.encode('utf-8'))
    pad = (-(pos + header_len)) % align
    if pad == 0:
        return b''
    if pad < 4:
        pad += align
    payload = pad - 4
    return struct.pack('<HH', ZIPALIGN_ID, payload) + b'\x00' * payload

zin = zipfile.ZipFile(BASE, 'r')
names = [i.filename for i in zin.infolist() if not i.filename.startswith('META-INF/')]
qnn_entries = ['lib/arm64-v8a/' + n for n in QNN]
# 字典序插入：全部条目重排序（lib 条目按名字排）
all_names = sorted(set(names) | set(qnn_entries))
main_data = open(MAIN, 'rb').read()

with zipfile.ZipFile(OUT, 'w', allowZip64=True) as zout:
    for name in all_names:
        if name == ENTRY:
            data = main_data
        elif name in qnn_entries:
            data = open(os.path.join(QNN_DIR, name.rsplit('/', 1)[-1]), 'rb').read()
        else:
            data = zin.read(name)
        src = zin.getinfo(name) if name in zin.namelist() else None
        zi = zipfile.ZipInfo(name, date_time=(2026, 11, 14, 0, 0, 0))
        if name.startswith('lib/') and name.endswith('.so'):
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            zi.create_system = 3
            zi.extra = align_extra(zout, zi, ALIGN)
        else:
            zi.compress_type = (src.compress_type if src else zipfile.ZIP_DEFLATED)
            zi.external_attr = (src.external_attr if src else (0o644 << 16))
            zi.create_system = (src.create_system if src else 3)
            zi.extra = align_extra(zout, zi, 4)
        zout.writestr(zi, data)
print('written', OUT, '%.1fMB' % (os.path.getsize(OUT)/1048576))
