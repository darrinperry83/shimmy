"""Exercise real Mach-O relocations/aliases as well as malformed input guards."""
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from namespace_macho import rewrite


class NamespaceTests(unittest.TestCase):
    def test_rejects_bad_objects(self):
        for data in (b'', b'\0' * 32, struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 1, 1, 8, 0, 0) + b'\0' * 8):
            with self.assertRaises(ValueError):
                rewrite(data)

    def test_alias_relocation_and_host_import(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'fixture.s'
            source.write_text('''.text
.p2align 2
.globl _entry, _alias
_entry:
    bl _other
    b _bzero
_alias = _entry
.data
.quad _alias
.quad section$start$__DATA$__test
.section __DATA,__test
.quad 0
''')
            original = root / 'fixture.o'
            subprocess.run(['xcrun','clang','-arch','arm64e','-c',str(source),'-o',str(original)],check=True)
            data, count = rewrite(original.read_bytes())
            self.assertGreaterEqual(count, 3)
            namespaced = root / 'namespaced.o'
            namespaced.write_bytes(data)
            symbols = subprocess.check_output(['xcrun','nm',str(namespaced)],text=True)
            self.assertIn('_rdl_entry', symbols)
            self.assertIn('_rdl_alias', symbols)
            self.assertIn('_rdl_other', symbols)
            self.assertNotIn(' _entry\n', symbols)
            self.assertIn(' _bzero\n', symbols)
            self.assertIn('section$start$__DATA$__test', symbols)
            host = root / 'host.s'
            host.write_text('.text\n.p2align 2\n.globl _rdl_other, _bzero, _entry\n_rdl_other:\n_bzero:\n_entry:\nret\n')
            host_object = root / 'host.o'
            subprocess.run(['xcrun','clang','-arch','arm64e','-c',str(host),'-o',str(host_object)],check=True)
            linked = root / 'combined.o'
            subprocess.run(['xcrun','ld','-r','-arch','arm64e',str(namespaced),str(host_object),'-o',str(linked)],check=True)
            undefined = subprocess.check_output(['xcrun','nm','-u',str(linked)],text=True)
            self.assertEqual(undefined.strip(), 'section$start$__DATA$__test')
            with self.assertRaises(ValueError):
                rewrite(original.read_bytes() + b'bad trailing data')

    def test_indirect_alias_target_is_namespaced(self):
        # An N_INDR target uses a string offset, not a symbol-table index.
        strings = b'\0_alias\0_entry\0'
        original = (struct.pack('<8I', 0xfeedfacf, 0x100000c, 2, 1, 1, 24, 0, 0)
                    + struct.pack('<6I', 2, 24, 56, 1, 72, len(strings))
                    + struct.pack('<IBBHQ', 1, 0x0b, 0, 0, 8) + strings)
        data, _ = rewrite(original)
        name, kind, section, desc, target = struct.unpack_from('<IBBHQ', data, 56)
        self.assertEqual(data[72 + name:].split(b'\0')[0], b'_rdl_alias')
        self.assertEqual(data[72 + target:].split(b'\0')[0], b'_rdl_entry')


if __name__ == '__main__':
    unittest.main()
