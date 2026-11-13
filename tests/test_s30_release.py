"""Native S30 publication must preserve payloads and source identity."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
from s30_assets import collect


class S30ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name) / 'run'
        self.out = Path(self.temp.name) / 'out'
        (self.run / 'firmware').mkdir(parents=True)
        self.commit = 'abc123' + '0' * 34
        self.base = '1.0.0-rc.4'
        self.version = self.base + '-abc123'
        self.manifest = dict(version=self.version, project_head=self.commit, dirty=False,
            version_metadata=dict(version=self.version, base_version=self.base,
                git_commit=self.commit, git_tag='v' + self.base, dirty=False),
            flash_layout=dict(name='s30-v1b-stock-v1', bootloader_included=False))
        self.write_manifest()
        (self.run / 'firmware-validation.json').write_text(json.dumps(dict(status='PASS')))
        checks = []
        self.bodies = {}
        for role, suffix in (('OTA', 'rbl'), ('STOCK_UA', 'bin'), ('app', 'bin'), ('UG', 'bin')):
            filename = f'BL2028N_{role}_{self.version}.{suffix}'
            body = ('native-payload-' + role).encode()
            (self.run / 'firmware' / filename).write_bytes(body)
            checks.append(hashlib.sha256(body).hexdigest() + '  ' + filename + '\n')
            self.bodies[f'BL2028N_{role}_{self.base}.{suffix}'] = body
        (self.run / 'firmware/SHA256SUMS').write_text(''.join(checks))

    def write_manifest(self):
        (self.run / 'build-manifest.json').write_text(json.dumps(self.manifest))

    def test_native_release_aliases_preserve_bytes_and_git_version(self):
        collect(self.run, self.out)
        for name, body in self.bodies.items():
            self.assertEqual((self.out / name).read_bytes(), body)
        manifest = json.loads((self.out / 'build-s30.json').read_text())
        self.assertEqual(manifest['version'], self.version)
        self.assertEqual(manifest['project_head'], self.commit)
        self.assertEqual(set(manifest['release_files']), set(self.bodies))
        for line in (self.out / 'SHA256SUMS-S30').read_text().splitlines():
            checksum, name = line.split('  ', 1)
            self.assertEqual(hashlib.sha256((self.out / name).read_bytes()).hexdigest(), checksum)
        collect(self.run, self.out)  # exact retries are safe

    def test_rejects_dirty_mismatched_identity_and_wrong_layout(self):
        original = copy.deepcopy(self.manifest)
        changes = [('dirty', True), ('project_head', 'f' * 40), ('version', self.version + '-dirty'),
                   ('flash_layout', dict(name='openbeken', bootloader_included=True))]
        for key, value in changes:
            self.manifest = dict(original, **{key: value})
            self.write_manifest()
            with self.subTest(key=key), self.assertRaises(ValueError):
                collect(self.run, self.out)
        self.manifest = copy.deepcopy(original)
        self.manifest['version_metadata']['base_version'] = '../../outside'
        self.write_manifest()
        with self.assertRaises(ValueError):
            collect(self.run, self.out)
        self.assertFalse(self.out.exists())

    def test_native_catalog_and_online_manifests_select_rbl_without_signed_wrapper(self):
        import catalog
        from archive_pages import write_candidate_latest, write_latest
        from device_manifest import verify_manifests
        collect(self.run, self.out)
        with patch.object(catalog, 'STEMS', {}):
            release = catalog.make('v' + self.base, self.commit, self.out, 'S30 update')
        image = release['models']['s30']['downloads']['rbl']
        self.assertEqual(image['path'], '/releases/v' + self.base + '/BL2028N_OTA_' + self.base + '.rbl')
        self.assertEqual(image['sha256'], hashlib.sha256(self.bodies[Path(image['path']).name]).hexdigest())
        verify_manifests(release, lambda path: (self.out / Path(path).name).read_bytes())
        write_candidate_latest([release], self.out)
        latest = self.out / 'ota/candidate/s30/full/latest.msgpack'
        self.assertEqual(latest.read_bytes(), (self.out / 'BL2028N.latest.msgpack').read_bytes())
        self.assertFalse((self.out / 'ota/v1/s30/full/latest.msgpack').exists())
        write_latest({'stable': None, 'releases': []}, self.out)
        self.assertTrue(latest.exists())
        write_candidate_latest([], self.out)
        self.assertFalse(latest.exists())

    def test_rejects_corrupt_or_unverified_native_image(self):
        validation = self.run / 'firmware-validation.json'
        validation.write_text(json.dumps(dict(status='FAIL')))
        with self.assertRaises(ValueError):
            collect(self.run, self.out)
        validation.write_text(json.dumps(dict(status='PASS')))
        image = next((self.run / 'firmware').glob('*.rbl'))
        image.write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'checksums differ'):
            collect(self.run, self.out)
        self.assertFalse(self.out.exists())

    def test_refuses_replacement_before_writing_any_asset(self):
        self.out.mkdir()
        existing = self.out / 'validation-s30.json'
        existing.write_bytes(b'other release')
        with self.assertRaisesRegex(ValueError, 'Refuse to replace'):
            collect(self.run, self.out)
        self.assertEqual(list(self.out.iterdir()), [existing])
        self.assertEqual(existing.read_bytes(), b'other release')

    def test_reviewed_notes_accept_native_s30_without_accepting_unsigned_phi_models(self):
        import catalog
        import user_notes as notes
        collect(self.run, self.out)
        original = (notes.DATA / 'sources' / ('v' + self.base + '.md')).read_text()
        with patch.object(catalog, 'STEMS', {}):
            release = catalog.make('v' + self.base, self.commit, self.out, original)
        archive = Path(self.temp.name) / 'archive'
        (archive / 'releases').mkdir(parents=True)
        document = notes.read(notes.DATA / 'catalog.legacy-v0.9.json')
        document['releases'].append(release)
        data = Path(self.temp.name) / 'data'
        shutil.copytree(notes.DATA, data)
        order = notes.read(data / 'release-order.json')
        next(r for r in order['releases'] if r['version'] == self.base)['previous'] = '0.9.27'
        (data / 'release-order.json').write_bytes(notes.encoded(order))
        lock = (ROOT / 'frontend.lock.json').read_bytes()
        process = subprocess.CompletedProcess([], 0, stdout=lock)
        with patch.object(notes.subprocess, 'run', return_value=process), patch.object(notes, 'verify_archive') as verify:
            def compile_notes():
                (archive / 'releases/index.json').write_bytes(notes.encoded(document))
                return notes.compile_manifest(data, archive=archive, formal=True)[0]
            manifest = compile_notes()
            target = next(r for r in manifest['releases'] if r['version'] == self.base)['targets'][0]
            self.assertEqual(target['kind'], 'rbl')
            self.assertEqual(target['sha256'], release['models']['s30']['downloads']['rbl']['sha256'])
            verify.assert_called_once()
            native = release['models'].pop('s30')
            release['models']['tc1'] = native
            with self.assertRaisesRegex(ValueError, 'require signed firmware'):
                compile_notes()
            release['models'] = {'s30': dict(native, downloads={'raw': native['downloads']['rbl']})}
            with self.assertRaisesRegex(ValueError, 'require signed firmware'):
                compile_notes()

    def test_reviewed_native_archive_checks_catalog_and_payload(self):
        import catalog
        import user_notes as notes
        collect(self.run, self.out)
        original = (notes.DATA / 'sources' / ('v' + self.base + '.md')).read_bytes()
        with patch.object(catalog, 'STEMS', {}):
            release = catalog.make('v' + self.base, self.commit, self.out, original.decode())
        archive = Path(self.temp.name) / 'archive'
        folder = archive / 'releases' / release['tag']
        shutil.copytree(self.out, folder)
        lock = (ROOT / 'frontend.lock.json').read_bytes()
        identity = {'source_commit': self.commit, 'frontend': {
            'kind': 'lock', 'sha256': notes.digest(lock), 'lock': json.loads(lock)}}
        identities = {'releases': {release['tag']: {'targets': {'s30:full': identity}}}}
        process = subprocess.CompletedProcess([], 0, stdout=lock)
        with patch.object(notes.subprocess, 'run', return_value=process):
            notes.verify_archive({'releases': [release]}, identities, archive)
            asset = release['models']['s30']['downloads']['rbl']
            old_digest = asset['sha256']
            asset['sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'catalog differs from verified build'):
                notes.verify_archive({'releases': [release]}, identities, archive)
            asset['sha256'] = old_digest
            (folder / Path(asset['path']).name).write_bytes(b'corrupt native image')
            with self.assertRaisesRegex(ValueError, 'Native release checksum mismatch'):
                notes.verify_archive({'releases': [release]}, identities, archive)


if __name__ == '__main__':
    unittest.main()
