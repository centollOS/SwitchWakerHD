"""Synthetic PE resource fixtures, never game or real executable bytes."""
import struct
import tempfile
from pathlib import Path
import unittest
import zipfile

import guard

MANIFEST = b'''<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
<requestedExecutionLevel level="asInvoker" uiAccess="false"/>
</requestedPrivileges></security></trustInfo>
<application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
<longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
</windowsSettings></application></assembly>'''


def pe(manifest):
    raw_size = (0x60+len(manifest)+511)//512*512
    data = bytearray(0x200+raw_size)
    data[:2] = b'MZ'
    def put(fmt, offset, *values): struct.pack_into(fmt, data, offset, *values)
    put('<I', 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    put('<HH', 0x84, 0x8664, 1)
    put('<H', 0x94, 240)
    put('<H', 0x98, 0x20b)
    put('<I', 0x98+108, 16)
    put('<II', 0x98+112+16, 0x1000, 0x60+len(manifest))
    section = 0x98+240
    data[section:section+8] = b'.rsrc\0\0\0'
    put('<IIII', section+8, raw_size, 0x1000, raw_size, 0x200)
    root = 0x200
    for directory in (0, 0x18, 0x30): put('<H', root+directory+14, 1)
    put('<II', root+16, 24, 0x80000018)
    put('<II', root+0x28, 1, 0x80000030)
    put('<II', root+0x40, 1033, 0x48)
    put('<IIII', root+0x48, 0x1060, len(manifest), 0, 0)
    data[root+0x60:root+0x60+len(manifest)] = manifest
    return bytes(data)


class WindowsManifest(unittest.TestCase):
    def check(self, data, name='Wind Waker HD.exe'):
        problems = []
        guard.check_entry(name, data, problems)
        return problems

    def test_actual_utf8_resource_is_required_for_both_executables(self):
        for name in ('wwhd.exe', 'Release/Wind Waker HD.exe', 'release/WWHD.EXE'):
            self.assertEqual(self.check(pe(MANIFEST), name), [])
            for manifest in (MANIFEST.replace(b'UTF-8', b'Windows-1252'),
                             MANIFEST.replace(b'activeCodePage', b'unrelated'),
                             MANIFEST.replace(b'2019/WindowsSettings', b'2005/WindowsSettings'),
                             MANIFEST.replace(b'windowsSettings', b'unrelatedSettings')):
                self.assertTrue(self.check(pe(manifest), name))

    def test_long_path_awareness_is_required(self):  # issue #144
        for manifest in (MANIFEST.replace(b'>true<', b'>false<'),
                         MANIFEST.replace(b'longPathAware', b'unrelated'),
                         MANIFEST.replace(b'2016/WindowsSettings', b'2005/WindowsSettings')):
            self.assertTrue(self.check(pe(manifest), 'wwhd.exe'))

    def test_appended_utf8_decoy_cannot_replace_missing_manifest_setting(self):
        bad = pe(MANIFEST.replace(b'activeCodePage', b'unrelated'))+MANIFEST
        self.assertTrue(self.check(bad))

    def test_missing_resource_and_malformed_pe_fail_closed(self):
        data = bytearray(pe(MANIFEST))
        struct.pack_into('<I', data, 0x210, 99) # resource type is not RT_MANIFEST
        for bad in (b'not PE'+MANIFEST, pe(MANIFEST)[:300], bytes(data), pe(b'<invalid')):
            self.assertTrue(self.check(bad))

    def test_archive_and_directory_scans_enforce_the_resource(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'release'; root.mkdir()
            (root/'Wind Waker HD.exe').write_bytes(pe(MANIFEST))
            self.assertEqual(guard.scan(str(root))[0], [])
            archive = Path(temporary)/'bad.zip'
            with zipfile.ZipFile(archive,'w') as output:
                output.writestr('Release/wwhd.exe', pe(MANIFEST.replace(b'UTF-8',b'ACP')))
            self.assertTrue(guard.scan(str(archive))[0])


if __name__ == '__main__': unittest.main()
