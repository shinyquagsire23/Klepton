"""Exercise the AVP adapter's install boundary without a device or signing keys."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('avp_adapter', Path(__file__).resolve().parents[1] / 'ports/scotland2/tools/avp_play.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)

class MetadataInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        game = self.root / 'beatsaber140'
        game.mkdir()
        (game / 'apktool.yml').write_text('versionInfo:\n  versionCode: 1716\n  versionName: 1.40.8_7379\n')
        (game / 'AndroidManifest.xml').write_text('<manifest package="com.beatgames.beatsaber"/>')
        output = self.root / 'output'
        app = output / 'visionos/build/release-dd/Build/Products/Release-xros/Klepton.app'
        app.mkdir(parents=True)
        (app / 'fixture').write_text('signed app stand-in')
        (self.root / '.scotland2-avp-local.json').write_text('{"output":"output"}')
        self.env = {'KLEPTON_TARGET':'beatsaber140', 'KLEPTON_TEAM':'existing-team',
                    'KLEPTON_BUNDLE_ID':'existing.test', 'KLEPTON_DEVICE':'connected-device'}

    def run_main(self, no_install=False, stage_fails=False):
        calls = []
        def run(command, **kwargs):
            calls.append((command, os.environ.get('KLEPTON_BUNDLE_ID'), os.environ.get('KLEPTON_TARGET')))
            if stage_fails and '--meta' in command:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)
        output = io.StringIO()
        with patch.object(adapter, 'ROOT', self.root), patch.object(adapter.subprocess, 'run', side_effect=run), patch.dict(os.environ, self.env), patch('sys.argv', ['avp_play.py'] + (['--no-install'] if no_install else [])), contextlib.redirect_stdout(output):
            try:
                adapter.main()
            except Exception as error:
                return calls, output.getvalue(), error
        return calls, output.getvalue(), None

    def test_install_stages_metadata_for_same_identity_before_success(self):
        calls, output, error = self.run_main()
        self.assertIsNone(error)
        install = next(i for i, (cmd, _, _) in enumerate(calls) if 'devicectl' in cmd)
        self.assertEqual(calls[install + 1], (['bash', str(self.root/'visionos/stage_assets.sh'), '--meta', 'connected-device'], 'existing.test', 'beatsaber140'))
        self.assertIn('and game metadata installed', output)

    def test_metadata_copy_failure_is_not_reported_installed(self):
        _, output, error = self.run_main(stage_fails=True)
        self.assertIsInstance(error, subprocess.CalledProcessError)
        self.assertNotIn('and game metadata installed', output)

    def test_build_only_does_not_contact_device(self):
        calls, output, error = self.run_main(no_install=True)
        self.assertIsNone(error)
        self.assertFalse(any('devicectl' in cmd or '--meta' in cmd for cmd, _, _ in calls))
        self.assertIn('NOT installed', output)

    def test_wrong_version_or_missing_manifest_fails_before_build(self):
        for invalid in ('wrong-version', 'missing-manifest'):
            with self.subTest(invalid=invalid):
                if invalid == 'wrong-version':
                    (self.root/'beatsaber140/apktool.yml').write_text('versionCode: 545\n')
                else:
                    (self.root/'beatsaber140/apktool.yml').write_text('versionCode: 1716\n')
                    (self.root/'beatsaber140/AndroidManifest.xml').unlink()
                calls, output, error = self.run_main()
                self.assertIsNotNone(error)
                self.assertEqual(calls, [])

    def test_unity63_dispatches_its_own_profile_without_legacy_build_or_device_calls(self):
        (self.root/'beatsaber140/apktool.yml').write_text('versionCode: 3237\n')
        profile=self.root/'ports/scotland2/port1453';profile.mkdir(parents=True)
        (profile/'selected-mods.json').write_text(json.dumps({'game':{'versionCode':3237},'runtimeProfile':'1.45.3'}))
        calls,output,error=self.run_main(no_install=True)
        self.assertIsNone(error)
        self.assertEqual(len(calls),1)
        self.assertEqual(calls[0][0][1:], [str(self.root/'ports/scotland2/tools/avp_play_1453.py'),'--no-install'])

    def test_unity63_without_a_matching_profile_fails_before_any_build(self):
        (self.root/'beatsaber140/apktool.yml').write_text('versionCode: 3237\n')
        calls,output,error=self.run_main(no_install=True)
        self.assertIsNotNone(error)
        self.assertEqual(calls,[])

class AVPBuildStampTests(unittest.TestCase):
    def test_actual_avp_dotted_stamp_and_invalid_values(self):
        import sys
        tools=Path(__file__).resolve().parents[1]/'ports/scotland2/tools'
        with patch.object(sys, 'path', [str(tools), *sys.path]):
            from assemble_1453_app import build_stamp
            self.assertEqual(build_stamp('3237.351.1'), '3237.351.1')
            self.assertEqual(build_stamp(349), '349')
            for value in ('', '-1', '1.2.3.4', '1..2', '351 release', '1\n', '1000000000'):
                with self.assertRaises(Exception): build_stamp(value)

if __name__ == '__main__':
    unittest.main()
