# SteamCloudKit

Steam's connection, sign-in and Steam Cloud, in Swift: the parts of Glovebox's `SteamLauncherKit`
(github.com/glovebox-vr/glovebox, `launcher/Sources/SteamLauncherKit`, MIT, © 2026 the Glovebox contributors —
`LICENSE-Glovebox.txt`) that Steam Cloud saves need, as of Glovebox 0.1.0 (`bf5a142`).

Taken unchanged: `CMClient.swift` (without its C import), `Transport.swift`, `Messages.swift`, `ProtoWire.swift`,
`KeyValues.swift`, `SteamCloud.swift`, `SteamAuth.swift`.

Changed:

- `Codec.swift` is new: SHA-1, CRC-32 and inflate from CommonCrypto and the system zlib, and Glovebox's reader for
  a one-file zip with bounds checks added. Glovebox has these in a C target beside its LZMA and Zstandard decoders.
- `SteamApps.swift` has the product-info part only (an app's build and its Auto-Cloud rules); ownership, depot
  keys, manifests and content servers are the depot downloader's and are left out with it.

- `SteamCloud.swift` has seven additions: `CloudSync.localSettled`, called when the downloads are in place and only
  uploads remain, so that a game can start while they go on; an upload that Steam refuses as busy (one begun
  earlier and never finished) is called off and begun again; an empty file that the cloud does not have is not
  sent. A file Steam refuses no longer stops the files behind it in the batch. Every file through is
  reported (`uploadStep`), and open upload batches are written down (`batchLedger`) so that one left open is
  closed by the next sync. When Steam answers the opening of a batch with "too many pending", it is tried again
  after a pause, a few times.
- `CloudGame.swift` is new: what Glovebox keeps in its depot downloader's game profile.

The sources are compiled into the game app (`visionos/gen_xcodeproj.py`); `Package.swift` is only for their tests
on a Mac (`swift test` here).
