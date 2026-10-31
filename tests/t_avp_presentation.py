#!/usr/bin/env python3
import json,pathlib,sys,tempfile,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'ports/scotland2/tools'))
from build_release import stage_presentation

class PresentationTests(unittest.TestCase):
 def test_exact_art_and_hash_receipt(self):
  with tempfile.TemporaryDirectory() as folder:
   root=pathlib.Path(folder);art=root/'art';work=root/'work';work.mkdir()
   icon=art/'AppIcon-beatsaber140.solidimagestack';icon.mkdir(parents=True)
   (icon/'Contents.json').write_text('{"layers":[{"filename":"Back.solidimagestacklayer"}]}')
   (icon/'icon.png').write_bytes(b'owner image fixture')
   stage_presentation(art,work)
   self.assertEqual((work/'visionos/Assets.xcassets'/icon.name/'icon.png').read_bytes(),b'owner image fixture')
   self.assertEqual(len(json.loads((work/'presentation-receipt.json').read_text())['files']),2)
 def test_links_traversal_and_malformed_metadata_fail(self):
  for value in [[],{'images':{}},{'images':[4]},{'images':[{'filename':'../private.png'}]},{'layers':[{'filename':42}]}]:
   with self.subTest(value=value),tempfile.TemporaryDirectory() as folder:
    root=pathlib.Path(folder);art=root/'art';work=root/'work';work.mkdir()
    icon=art/'AppIcon-beatsaber140.solidimagestack';icon.mkdir(parents=True)
    (icon/'Contents.json').write_text(json.dumps(value))
    with self.assertRaises(ValueError):stage_presentation(art,work)
  with tempfile.TemporaryDirectory() as folder:
   root=pathlib.Path(folder);art=root/'art';art.mkdir();work=root/'work';work.mkdir()
   (art/'AppIcon-beatsaber140.solidimagestack').symlink_to(work,target_is_directory=True)
   with self.assertRaises(ValueError):stage_presentation(art,work)
if __name__=='__main__':unittest.main()
