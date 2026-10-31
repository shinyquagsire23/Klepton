# Beat Saber Airbuds Search

A Beat Saber mod for Quest that uses your Airbuds listening history to search for matching custom maps on BeatSaver.

Airbuds is a mobile app that supports viewing your playback records on the following music platforms: 
- `Spotify`
- `Apple Music`
- `SoundCloud`
- `Deezer`
- `Amazon Music`
- `Audiomack`

## Fork Notice

This project is forked from [Beat-Saber-Spotify-Search](https://github.com/TychoTheTaco/Beat-Saber-Spotify-Search).

Key differences:
- Rebranded as Airbuds Search (mod id `airbuds-search`).
- Romaji conversion is provided by an optional GPL Kakasi adapter mod [airbuds-search-kakasi](https://github.com/zilewang7/Beat-Saber-Airbuds-Search-Kakasi).
- Recently played list is grouped by day, cached to disk.
- View your friends' Airbuds history (PUBLIC only).
- Show maps from BeatSaver ordered by matching of name and difficulty (no more filter but only order).
- Register button into side menu of Solo and Multiplayer.


## Initial Setup

You only need an Airbuds refresh token. The mod does not use Spotify login anymore.

1. Open the Airbuds Search mod.
2. If no refresh token is saved, the mod shows a settings page. Paste your Airbuds app refresh token and press Save.
3. Relaunch the game if needed.

You can also edit the config file directly:

`/sdcard/ModData/com.beatgames.beatsaber/Mods/airbuds-search/airbuds-search.json`

Example:

```json
{
  "airbuds": {
    "refreshToken": "rt-your-token-here"
  },
  "filter": {
    "difficulty": "Normal"
  }
}
```

*In order to obtain refreshToken, the simplest way is to capture http packets on the app when logging in to the account. It is recommended to ask ai to get the most suitable way to obtain refresh_token for you.*


Search results are fetched online from BeatSaver. The Download button shows when a map is not installed locally.

For song titles and artists containing Japanese characters, the mod will attempt to generate romaji for additional searches. If the optional [Kakasi adapter mod](https://github.com/zilewang7/Beat-Saber-Airbuds-Search-Kakasi) is installed, it provides higher-quality conversion; otherwise the fallback is kana-only.

### Optional: Romaji Overrides

If some names romanize poorly, you can provide overrides.

Create `/sdcard/ModData/com.beatgames.beatsaber/Mods/airbuds-search/romaji_overrides.txt` with one entry per line:

```
# lines starting with # are ignored
<japanese>=<romaji>
```

The file should be UTF-8 encoded.

### Optional: Kakasi Adapter Mod (Better Romaji)
Install the separate GPL adapter mod `airbuds-search-kakasi` to enable Kakasi-based conversion.

### Developer Docs

See `README_DEV.md` for build, packaging, and adapter mod notes.
