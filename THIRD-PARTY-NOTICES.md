# Third-party sources and notices

Klepton retains its original license in `LICENSE`. The source-only builder fetches
original mod and SDK sources locally from the exact archive and SHA-256 recorded
in `source-inputs.json` and `build-inputs.json`, then applies the recorded patches.
Available original license/notice files are copied unchanged under
`THIRD-PARTY-LICENSES/` in the source package. Original archives are authoritative.

| Source | Immutable revision | Original archive | Available license in recorded tree |
| --- | --- | --- | --- |
| ports/scotland2/upstream | `432f67bca61e822a87b27c001bddcd1061d25bf4` | [pinned source](https://codeload.github.com/sc2ad/scotland2/zip/432f67bca61e822a87b27c001bddcd1061d25bf4) | LICENSE |
| ports/scotland2/flamingo/upstream | `461993fea629790bf499f41e807b00db25b0a148` | [pinned source](https://codeload.github.com/sc2ad/Flamingo/zip/461993fea629790bf499f41e807b00db25b0a148) | LICENSE |
| ports/scotland2/customtypes/upstream | `864211e6dd9ec9d82ddc4490668db02316672c38` | [pinned source](https://codeload.github.com/QuestPackageManager/Il2CppQuestTypePatching/zip/864211e6dd9ec9d82ddc4490668db02316672c38) | LICENSE |
| ports/scotland2/lapiz/upstream | `dce1d785d6aa7a8b61918c2210815ba25ec2391c` | [pinned source](https://codeload.github.com/raineaeternal/Lapiz/tar.gz/dce1d785d6aa7a8b61918c2210815ba25ec2391c) | LICENSE |
| ports/scotland2/bsml/upstream | `4987a88698becce955e44301fd4d573fc9180c2f` | [pinned source](https://codeload.github.com/bsq-ports/Quest-BSML/tar.gz/4987a88698becce955e44301fd4d573fc9180c2f) | No license file in the recorded archive/tree; redistribution permission not established |
| ports/scotland2/songcore/upstream | `4556891fb93a2a71549df11404074589daf8b31d` | [pinned source](https://codeload.github.com/raineaeternal/Quest-SongCore/tar.gz/4556891fb93a2a71549df11404074589daf8b31d) | LICENSE |
| ports/scotland2/songdownloader/upstream | `b2aacb11142a9f915c18a945760d98da1410fde4` | [pinned source](https://codeload.github.com/darknight1050/SongDownloader/tar.gz/b2aacb11142a9f915c18a945760d98da1410fde4) | LICENSE |
| ports/scotland2/playlistcore/upstream | `41f80c02bfe1c2e1edd9fcdee28a49e7c3aeda18` | [pinned source](https://codeload.github.com/Metalit/PlaylistCore/tar.gz/41f80c02bfe1c2e1edd9fcdee28a49e7c3aeda18) | LICENSE |
| ports/scotland2/metacore/upstream | `f54126c58b4604d5fe04c105a774aaa1bd81a593` | [pinned source](https://codeload.github.com/Metalit/MetaCore/tar.gz/f54126c58b4604d5fe04c105a774aaa1bd81a593) | No license file in the recorded archive/tree; redistribution permission not established |
| ports/scotland2/beatsaverplusplus/upstream | `d3efd36abb82bcbbef62f1c2b09b411cde547936` | [pinned source](https://codeload.github.com/bsq-ports/BeatSaverPlusPlus/tar.gz/d3efd36abb82bcbbef62f1c2b09b411cde547936) | LICENSE |
| ports/scotland2/web-utils/upstream | `d0ae1ef7f1557aa4055072a8326745ce303cf14c` | [pinned source](https://codeload.github.com/bsq-ports/WebUtils/tar.gz/d0ae1ef7f1557aa4055072a8326745ce303cf14c) | No license file in the recorded archive/tree; redistribution permission not established |
| ports/scotland2/datakeeper/upstream | `53a7e2d9f99a2e67ac325cadd40121196722fb0e` | [pinned source](https://codeload.github.com/Metalit/PlayerDataKeeper/tar.gz/53a7e2d9f99a2e67ac325cadd40121196722fb0e) | No license file in the recorded archive/tree; redistribution permission not established |
| ports/scotland2/unicode/upstream | `0cf6d1eefcf2175778e27261bd2fc1e6d604ea8a` | [pinned source](https://codeload.github.com/Metalit/UnicodeQuest/tar.gz/0cf6d1eefcf2175778e27261bd2fc1e6d604ea8a) | No license file in the recorded archive/tree; redistribution permission not established |
| ports/scotland2/song-details/upstream | `b13aa5d5cc07200a4873f7c0554a99ba0ba6d79f` | [pinned source](https://codeload.github.com/bsq-ports/SongDetails/tar.gz/b13aa5d5cc07200a4873f7c0554a99ba0ba6d79f) | No license file in the recorded archive/tree; redistribution permission not established |
| ports/scotland2/and64 | `dd9f4cf0565ff46f9c2844a1512bc1786a7f2446` | [pinned source](https://codeload.github.com/QuestPackageManager/beatsaber-hook/zip/dd9f4cf0565ff46f9c2844a1512bc1786a7f2446) | No license file in the recorded archive/tree; redistribution permission not established |

The selected released QMOD libraries are separately fetched from their original
release URLs and checked against `selected-mods.json` and the original inventory.
Their original licenses and attribution remain applicable. Dependency archive
locations and hashes are in `build-inputs.json`; each downloaded archive retains
its own notices. The public Mozilla CA bundle comes from curl and is MPL-2.0;
its provenance and full license are in `ports/scotland2/certificates/`.

This package provides source modifications and a local build workflow. It does
not grant rights to the game, maps, third-party mods, Apple signing credentials,
or other inputs. Missing license metadata remains unresolved; no blanket
redistribution clearance is asserted. Game and signing inputs are supplied by
each recipient and are never part of this archive.

## Released package inputs

The builder downloads these original public releases locally; the source archive
contains their pins and adaptation workflow, not the released binaries.

| Package | Version | Original release | Recorded license file |
| --- | --- | --- | --- |
| custom-types | 0.18.3 | [original input](https://github.com/QuestPackageManager/Il2CppQuestTypePatching/releases/download/v0.18.3/CustomTypes.qmod) | ports/scotland2/customtypes/upstream/LICENSE |
| lapiz | 0.2.23 | [original input](https://github.com/raineaeternal/Lapiz/releases/download/v0.2.23/Lapiz.qmod) | ports/scotland2/lapiz/upstream/LICENSE |
| bsml | 0.4.55 | [original input](https://github.com/bsq-ports/Quest-BSML/releases/download/v0.4.55/BSML.qmod) | No license file recorded for this package; consult the original release/source |
| songcore | 1.1.24 | [original input](https://github.com/raineaeternal/Quest-SongCore/releases/download/v1.1.24/SongCore.qmod) | ports/scotland2/songcore/upstream/LICENSE |
| playlistcore | 1.5.5 | [original input](https://github.com/Metalit/PlaylistCore/releases/download/v1.5.5/PlaylistCore.qmod) | ports/scotland2/playlistcore/upstream/LICENSE |
| paper2_scotland2 | 4.6.4 | [original input](https://github.com/Fernthedev/paperlog/releases/download/v4.6.4/paper2_scotland2.qmod) | No license file recorded for this package; consult the original release/source |
| songdownloader | 0.8.8 | [original input](https://github.com/darknight1050/SongDownloader/releases/download/v0.8.8/SongDownloader.qmod) | ports/scotland2/songdownloader/upstream/LICENSE |
| unicode | 0.1.7 | [original input](https://github.com/Metalit/UnicodeQuest/releases/download/v0.1.7/Unicode.qmod) | No license file recorded for this package; consult the original release/source |
| datakeeper | 0.2.5 | [original input](https://github.com/Metalit/PlayerDataKeeper/releases/download/v0.2.5/PlayerDataKeeper.qmod) | No license file recorded for this package; consult the original release/source |
| metacore | 1.6.2 | [original input](https://github.com/Metalit/MetaCore/releases/download/v1.6.2/MetaCore.qmod) | No license file recorded for this package; consult the original release/source |
| beatsaber-hook | 6.4.2 | [original input](https://github.com/QuestPackageManager/beatsaber-hook/releases/download/v6.4.2/beatsaber-hook.qmod) | No license file recorded for this package; consult the original release/source |
| tinyxml2 | 10.0.0 | [original input](https://github.com/MillzyDev/NDK-tinyxml2/releases/download/v10.0.0/tinyxml2.qmod) | No license file recorded for this package; consult the original release/source |
| web-utils | 0.6.8 | [original input](https://github.com/bsq-ports/WebUtils/releases/download/v0.6.8/WebUtils.qmod) | No license file recorded for this package; consult the original release/source |
| beatsaverplusplus | 0.2.1 | [original input](https://github.com/bsq-ports/BeatSaverPlusPlus/releases/download/v0.2.1/BeatSaverPlusPlus.qmod) | ports/scotland2/beatsaverplusplus/upstream/LICENSE |
| song-details | 1.0.4 | [original input](https://github.com/bsq-ports/SongDetails/releases/download/v1.0.4/SongDetails.qmod) | No license file recorded for this package; consult the original release/source |
