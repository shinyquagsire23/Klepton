#!/usr/bin/env python3
import copy,json,pathlib,sys,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'ports/scotland2/tools'))
from selection import validate_selection,CORE_MODS,SEARCH_MODS

class SelectionTests(unittest.TestCase):
 def setUp(self):self.selection=json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text())
 def test_twelve_modules_and_retained_ten_core_profile(self):
  self.assertEqual(validate_selection(self.selection),CORE_MODS|SEARCH_MODS)
  ten=json.loads((ROOT/'ports/scotland2/profiles/ten-core.json').read_text())
  self.assertEqual(validate_selection(ten),CORE_MODS)
 def test_partial_missing_or_unknown_mod_selection_is_rejected(self):
  for name in ['BetterSongSearch','songcore']:
   selection=copy.deepcopy(self.selection);selection['selectedMods'].remove(name)
   with self.assertRaises(ValueError):validate_selection(selection)
  selection=copy.deepcopy(self.selection);selection['selectedMods'].append('unknown')
  with self.assertRaises(ValueError):validate_selection(selection)
 def test_fake_id_duplicates_and_missing_payload_are_rejected(self):
  for mutation in [lambda s:s['selectedMods'].append('FakeId'),lambda s:s['metadata'].append('mods/libFakeId.so'),
                   lambda s:s['metadata'].append(s['metadata'][0]),lambda s:s['metadata'].remove('mods/libairbuds-search.so')]:
   selection=copy.deepcopy(self.selection);mutation(selection)
   with self.assertRaises(ValueError):validate_selection(selection)
 def test_original_dependencies_stay_pinned_and_new_sources_are_explicit(self):
  packages={p['id']:p for p in self.selection['packages']}
  self.assertEqual(packages['songcore']['version'],'1.1.24')
  self.assertEqual(packages['custom-types']['version'],'0.18.3')
  for mod in SEARCH_MODS:
   self.assertTrue(packages[mod]['sourceBuilt'])
   self.assertTrue(set(packages[mod]['dependencies'])<=set(packages))
if __name__=='__main__':unittest.main()
