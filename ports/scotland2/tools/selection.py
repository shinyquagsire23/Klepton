"""Validate the bounded core/search compositions; never infer arbitrary mods."""
CORE_MODS = frozenset(('custom-types','lapiz','bsml','songcore','playlistcore',
                      'paper2_scotland2','songdownloader','unicode','datakeeper','metacore'))
SEARCH_MODS = frozenset(('BetterSongSearch','airbuds-search'))

def validate_selection(selection):
    mods = selection['selectedMods']
    chosen = set(mods)
    if len(mods) != len(chosen) or chosen not in (CORE_MODS, CORE_MODS | SEARCH_MODS):
        raise ValueError('Unsupported composition: preserve all ten core mods and add the two search mods together')
    if 'FakeId' not in selection.get('excludedMods', []):
        raise ValueError('Fake ID must remain excluded')
    metadata = selection['metadata']
    if len(metadata) != len(set(metadata)) or any('fakeid' in name.lower() for name in metadata):
        raise ValueError('Duplicate or excluded metadata')
    expected = {'mods/libBetterSongSearch.so','mods/libairbuds-search.so'}
    actual = set(metadata) & expected
    if actual != (expected if SEARCH_MODS <= chosen else set()):
        raise ValueError('Search selection and packaged libraries differ')
    return chosen
