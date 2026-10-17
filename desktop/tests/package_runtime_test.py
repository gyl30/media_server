#!/usr/bin/env python3
"""Offline candidate: set INTERNAL_RUNTIME_DIR to a newly generated internal package.

No Qt, HTTP, media, proxy URL lookup or D-Bus request is started. The child only
constructs/frees the real packaged libproxy factory and inspects its own maps.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CHILD = r'''
import ctypes, json
from pathlib import Path
import sys
runtime = Path(sys.argv[1]).resolve()
library = ctypes.CDLL(str(runtime / 'lib/libproxy.so.1'))
library.px_proxy_factory_new.argtypes = []
library.px_proxy_factory_new.restype = ctypes.c_void_p
library.px_proxy_factory_free.argtypes = [ctypes.c_void_p]
factory = library.px_proxy_factory_new()
if not factory:
    raise RuntimeError('real libproxy factory construction failed')
try:
    paths = sorted({line.split(maxsplit=5)[5] for line in Path('/proc/self/maps').read_text().splitlines()
                    if len(line.split(maxsplit=5)) == 6 and line.split(maxsplit=5)[5].startswith('/')})
    print(json.dumps(paths))
finally:
    library.px_proxy_factory_free(factory)
'''


class LibproxyRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get('INTERNAL_RUNTIME_DIR')
        if not configured:
            raise RuntimeError('INTERNAL_RUNTIME_DIR must name the new package; no implicit skip')
        cls.runtime = Path(configured).resolve(strict=True)

    def factory_maps(self, module_directory):
        env = dict(os.environ)
        # Match the planned headless target, without causing the GNOME helper to
        # read host desktop settings. This does not disable the network module.
        for name in ('LD_PRELOAD', 'LD_AUDIT', 'GNOME_DESKTOP_SESSION_ID', 'DESKTOP_SESSION'):
            env.pop(name, None)
        env.update(LD_LIBRARY_PATH=str(self.runtime / 'lib'), PX_MODULE_PATH=str(module_directory),
                   PX_GSETTINGS=str(self.runtime / 'lib/libproxy/0.4.17/pxgsettings'))
        child = subprocess.run([sys.executable, '-c', CHILD, str(self.runtime)], env=env,
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, timeout=10)
        self.assertEqual(child.returncode, 0, child.stderr)
        return [Path(path) for path in json.loads(child.stdout)]

    def test_resource_metadata_and_real_module_mapping(self):
        manifest = json.loads((self.runtime / 'manifest.json').read_text())
        expected = {'lib/libproxy/0.4.17/modules/network_networkmanager.so',
                    'lib/libproxy/0.4.17/modules/config_gnome3.so', 'lib/libproxy/0.4.17/pxgsettings'}
        entries = {entry['bundle_path']: entry for entry in manifest['libproxy_resources']}
        self.assertEqual(set(entries), expected)
        for relative in expected:
            data = (self.runtime / relative).read_bytes()
            self.assertEqual(data[:4], b'\x7fELF')
            digest = hashlib.sha256(data).hexdigest()
            self.assertEqual(digest, entries[relative]['sha256'])
            self.assertEqual(digest, manifest['files'][relative])
            self.assertIsNotNone(entries[relative]['copyright'])
        maps = self.factory_maps(self.runtime / 'lib/libproxy/0.4.17/modules')
        expected_module = self.runtime / 'lib/libproxy/0.4.17/modules/network_networkmanager.so'
        self.assertIn(expected_module, maps)
        self.assertIn((self.runtime / 'lib/libproxy.so.1').resolve(), maps)
        for path in maps:
            if '/libproxy/' in str(path) and '/modules/' in str(path):
                self.assertTrue(path.is_relative_to(self.runtime), str(path))

    def test_module_path_is_replacement_not_system_fallback(self):
        with tempfile.TemporaryDirectory(prefix='qt-libproxy-empty-') as directory:
            maps = self.factory_maps(directory)
        self.assertFalse(any('/libproxy/' in str(path) and '/modules/' in str(path) for path in maps))

    def test_wrapper_overrides_inherited_absolute_paths(self):
        with tempfile.TemporaryDirectory(prefix='qt-libproxy-wrapper-') as directory:
            scratch = Path(directory)
            shutil.copy2(self.runtime / 'run.sh', scratch / 'run.sh')
            (scratch / 'bin').mkdir()
            stub = scratch / 'bin/gb_monitor'
            stub.write_text('#!/bin/sh\nprintf "%s\\n" "$PX_MODULE_PATH" "$PX_GSETTINGS" "$LD_LIBRARY_PATH"\n')
            stub.chmod(0o700)
            env = dict(os.environ, PX_MODULE_PATH='/not-the-runtime', PX_GSETTINGS='/not-the-helper')
            env.pop('GNOME_DESKTOP_SESSION_ID', None)
            env.pop('DESKTOP_SESSION', None)
            child = subprocess.run(['/bin/sh', str(scratch / 'run.sh')], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
            self.assertEqual(child.returncode, 0, child.stderr)
            self.assertEqual(child.stdout.splitlines(), [str(scratch / 'lib/libproxy/0.4.17/modules'),
                str(scratch / 'lib/libproxy/0.4.17/pxgsettings'), str(scratch / 'lib')])

    def test_wrapper_refuses_unsafe_gnome_helper_path(self):
        with tempfile.TemporaryDirectory(prefix='qt-libproxy wrapper-') as directory:
            scratch = Path(directory)
            shutil.copy2(self.runtime / 'run.sh', scratch / 'run.sh')
            (scratch / 'bin').mkdir()
            stub = scratch / 'bin/gb_monitor'
            stub.write_text('#!/bin/sh\nprintf "unexpected application start\\n"\n')
            stub.chmod(0o700)
            for setting in ({'DESKTOP_SESSION': 'gnome'}, {'DESKTOP_SESSION': 'mate'}, {'GNOME_DESKTOP_SESSION_ID': ''}):
                with self.subTest(setting=setting):
                    child = subprocess.run(['/bin/sh', str(scratch / 'run.sh')],
                        env=dict(os.environ, **setting), stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, timeout=5)
                    self.assertEqual(child.returncode, 1)
                    self.assertEqual(child.stdout, '')
                    self.assertIn('libproxy 0.4.17', child.stderr)


if __name__ == '__main__':
    unittest.main()
