#!/usr/bin/env python3
"""Exercise the real project generator without game files or signing identities."""
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "visionos/gen_xcodeproj.py"


def generate(mode=None, sdk=False, env_extra=None):
    env = dict(os.environ, KLEPTON_TARGET="beatsaber", KLEPTON_TEAM="TESTTEAM00",
               KLEPTON_BUNDLE_ID="example.existing.beatsaber", KLEPTON_START_SCREEN="1")
    env.pop("KLEPTON_CONTROLLER_MODE", None)
    env.pop("KLEPTON_SURREAL_SDK", None)
    if mode is not None:
        env["KLEPTON_CONTROLLER_MODE"] = mode
    env.update(env_extra or {})
    # Surreal Interactive's SDK is present or absent as the test says, whatever this
    # checkout has fetched (a test load never fetches: only the generator's main does).
    fake_sdk = ("import os; _n='surreal_interactive_openxr_framework'; "
                "_f,_d,_l=os.path.isfile,os.path.isdir,os.listdir; "
                "os.path.isfile=lambda p:(%r if _n in str(p) else _f(p)); "
                "os.path.isdir=lambda p:(%r if _n in str(p) else _d(p)); "
                "os.listdir=lambda p:(['.klepton-fetched-test'] if _n in str(p) else _l(p)); " % (sdk, sdk))
    # Importing builds exactly the PBX string main writes, without requiring guest dylibs.
    code = fake_sdk + "import runpy,json,sys; sys.path.insert(0,%r); import targets; targets.libs_for=lambda _: 'libsynthetic'; m=runpy.run_path(%r); print(json.dumps({'pbx':m['PBX'],'swift':m['SWIFT'],'privacy':m['USAGE_DESCRIPTIONS'],'entitlements':m['ENTITLEMENTS_FILE']}))" % (str(GEN.parent), str(GEN))
    result = subprocess.run([sys.executable, "-c", code], env=env, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr)
    return json.loads(result.stdout)


class ProjectTests(unittest.TestCase):
    def test_modes_include_both_backends_and_keep_signing_identity(self):
        standard = generate()
        surreal = generate("surreal")
        self.assertIn('KL_CONTROLLER_MODE_DEFAULT = "standard";', standard["pbx"])
        self.assertIn('KL_CONTROLLER_MODE_DEFAULT = "surreal";', surreal["pbx"])
        self.assertEqual(standard["swift"], surreal["swift"])
        for name in ["KleptonControllers.swift", "KleptonControllerInput.swift", "KleptonControllerMode.swift",
                     "KleptonSurrealInput.swift", "KleptonSurrealBackend.swift", "KleptonSurrealView.swift",
                     "KleptonSurrealAim.swift", "KleptonSurrealAimView.swift", "KleptonSurrealSDK.swift"]:
            self.assertIn(name, surreal["swift"])
            self.assertTrue((ROOT / "visionos/Sources" / name).is_file())
        # Only the default changes: identity, entitlements, signing flags and framework list stay identical.
        self.assertEqual(standard["pbx"], surreal["pbx"].replace('KL_CONTROLLER_MODE_DEFAULT = "surreal";', 'KL_CONTROLLER_MODE_DEFAULT = "standard";'))
        self.assertIn("example.existing.beatsaber", surreal["pbx"])
        self.assertNotIn("OpenSurreal", surreal["pbx"])

    def test_surreal_sdk_in_every_build(self):
        # The mode is the player's choice at run time, so a game installed for standard
        # controllers links the SDK too and can be switched to Surreal Touch in its start window.
        sdk = "surreal_interactive_openxr_framework.xcframework"
        for mode in [None, "surreal"]:
            pbx = generate(mode, sdk=True)["pbx"]
            self.assertIn(f"{sdk} in Frameworks", pbx)
            self.assertNotIn(f"{sdk} in Embed", pbx, "a static framework is linked, never embedded")
            self.assertIn("KL_SURREAL_SDK", pbx)
            self.assertIn("-framework Accelerate", pbx)
        for mode in [None, "surreal"]:
            opted_out = generate(mode, sdk=True, env_extra={"KLEPTON_SURREAL_SDK": "0"})["pbx"]
            for absent in [sdk, "KL_SURREAL_SDK", "-framework Accelerate"]:
                self.assertNotIn(absent, opted_out)
        # Loaded by a test (never fetching), a build without the framework is the
        # backend-only project; run as the generator it would fetch it first (and a
        # Surreal build would stop if that failed).
        self.assertNotIn(sdk, generate(sdk=False)["pbx"])
        self.assertNotIn(sdk, generate("surreal", sdk=False)["pbx"])

    def test_surreal_support_off_builds_standard_only(self):
        # AVP Play's "Surreal Touch support" off: no SDK, Standard whatever was asked, and the
        # game told Surreal Touch is not there (KLSurrealAvailable NO hides its choice).
        sdk = "surreal_interactive_openxr_framework.xcframework"
        on = generate(sdk=True)["pbx"]
        self.assertIn("KL_SURREAL_AVAILABLE = YES;", on)
        for mode in [None, "surreal"]:
            off = generate(mode, sdk=True, env_extra={"KLEPTON_SURREAL_SUPPORT": "0"})["pbx"]
            self.assertIn("KL_SURREAL_AVAILABLE = NO;", off)
            self.assertIn('KL_CONTROLLER_MODE_DEFAULT = "standard";', off)
            for absent in [sdk, "KL_SURREAL_SDK", "-framework Accelerate"]:
                self.assertNotIn(absent, off)
        info = plistlib.loads((ROOT / "visionos/Info.plist").read_bytes())
        self.assertEqual(info["KLSurrealAvailable"], "$(KL_SURREAL_AVAILABLE)")

    def test_privacy_survives_generator_and_plist(self):
        generated = generate("surreal")
        info = plistlib.loads((ROOT / "visionos/Info.plist").read_bytes())
        self.assertEqual(info["KLControllerModeDefault"], "$(KL_CONTROLLER_MODE_DEFAULT)")
        for key in ["NSBluetoothAlwaysUsageDescription", "NSHandsTrackingUsageDescription", "NSWorldSensingUsageDescription"]:
            self.assertTrue(info[key])
            self.assertTrue(generated["privacy"][key])
            self.assertIn("INFOPLIST_KEY_" + key, generated["pbx"])

    def test_rejects_invalid_configuration(self):
        for value in ["unknown", 'surreal"; OTHER = 1;', ""]:
            with self.assertRaises(ValueError):
                generate(value)


if __name__ == "__main__":
    unittest.main()
