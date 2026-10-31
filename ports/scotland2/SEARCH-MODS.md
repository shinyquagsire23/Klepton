The default compatible Beat Saber 1.40.8 profile adds original BetterSongSearch
2.2.9 and Airbuds Search 2.0.2 to the preserved ten core mods. Fake ID remains
excluded. The retained core-only selection is `profiles/ten-core.json`.

Both ports retain their upstream UI, search and download implementations. Each
provenance file records the upstream commit, original dependency declarations
and adaptations. The existing pinned shared dependency versions are retained;
compilation and real host import resolution check the API compatibility.
Airbuds uses the existing verified HTTPS WebUtils path. Its JWT payload decoder
is bounded and only reads claims; it does not authenticate tokens. The unused
OpenSSL encryption translation unit and Android logcat subprocess are excluded.
No token, game data or signing material belongs in a source package.

Airbuds Search needs the user's refresh token in its game settings. The package
does not obtain a token or log into Airbuds. Optional Kakasi is not bundled; the
original kana fallback remains. Headset menu behavior, playback and authenticated
Airbuds requests are not established by host build validation.

Build the original mods with `python3 ports/scotland2/tools/build_search_mods.py
bettersongsearch` and the same command with `airbuds-search`. Run
`python3 tests/t_search_selection.py` and compile/run `tests/t_search_port.cpp`
with a host C++20 compiler. After preparing the existing BSML host test fixture,
`python3 ports/scotland2/test-search-mods.py` runs the original constructors and
setup against real game IL2CPP, with strict import checks and isolated files.
The full source builder compiles the selected stack, prepares signed hook
catalogs, translates each module and validates the final visionOS app. An
unsigned build uses `--unsigned` and never registers or installs an app ID.

Source-only exports reconstruct the upstream trees from content-hashed public
inputs and reviewed patches. Airbuds' selected-file Git archive avoids its
unneeded vendored build products. The cache path is fully validated offline;
fresh-network Git fetch is a separate check and has not been exercised here.
