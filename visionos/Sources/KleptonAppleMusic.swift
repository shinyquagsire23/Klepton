// Apple Music listening history for the Beat Saber mods' song search.
//
// The search mod (airbuds-search, with Klepton's applemusic patch) lists what
// was played recently and finds BeatSaver maps for it. On a headset that
// history comes from here, written in the mod's own history cache format into
// its data folder: the library's songs by their real last-played date, then
// what Apple lists as recently played but the library does not hold (only
// streamed; no date, so the mod files it under its undated group). Beside it
// two lists of its own: the favourites (Apple's "Favorite Songs" playlist) and
// the most played songs of the library. Only titles, artists
// and cover addresses leave MusicKit; no song is played or downloaded from
// Apple. The mod reads the file when its view opens; this rewrites it at every
// start of the app.
//
// Only in a build with the mods (KL_SCOTLAND2). Apple answers only with two
// tokens: the player's own Music User Token, which MusicKit gets on this
// headset once they allow it and which stays here, and a developer token. That
// one comes from the project's server (GET /api/v1/musickit/token, signed with
// the project's MusicKit key), so no App ID needs a MusicKit service of its
// own; it opens Apple's public catalogue and nobody's library. Without it the
// file says why, which the mod shows. There is deliberately no fallback to
// MusicKit's own token (Marcel, 2026-10-30).
#if KL_SCOTLAND2
import Foundation
import MusicKit

enum KleptonAppleMusic {
    /// The mod's data folder under the guest's /sdcard.
    static var folder: URL {
        URL(fileURLWithPath: Paths.container)
            .appendingPathComponent("android-files/ModData/com.beatgames.beatsaber/Mods/airbuds-search")
    }
    static var historyURL: URL { folder.appendingPathComponent("applemusic_recently_played.json") }
    static var favoritesURL: URL { folder.appendingPathComponent("applemusic_favorites.json") }
    static var mostPlayedURL: URL { folder.appendingPathComponent("applemusic_most_played.json") }

    /// At most this many songs a list.
    static let limit = 100

    /// One song as the mod lists it; `played` only where Apple knows when.
    typealias Entry = (id: String, title: String, artist: String, cover: String, played: Date?)

    /// The names Apple gives the favourites playlist, by language (it has no other mark).
    static let favoriteNames: Set<String> = ["favorite songs", "favourite songs", "lieblingstitel", "lieblingssongs", "morceaux favoris",
                                             "canciones favoritas", "brani preferiti", "favoriete nummers",
                                             "músicas favoritas", "favoritlåtar"]

    static func refreshInBackground() {
        Task.detached(priority: .utility) { await refresh() }
    }

    /// The project's token service; KL_MUSICKIT_TOKEN_URL points a test build at another one.
    static var tokenURL: URL {
        if let custom = ProcessInfo.processInfo.environment["KL_MUSICKIT_TOKEN_URL"], let url = URL(string: custom) { return url }
        return URL(string: "https://avpplay.martek.de/api/v1/musickit/token")!
    }

    static func refresh() async {
        let now = Int64(Date().timeIntervalSince1970 * 1000)
        switch await MusicAuthorization.request() {
        case .authorized:
            break
        case .denied, .restricted:
            write(historyURL, status: "denied", message: "Apple Music access is off for this app (Settings › Privacy & Security › Media & Apple Music).", entries: [], now: now)
            return
        default:
            write(historyURL, status: "notDetermined", message: "Apple Music access was not granted.", entries: [], now: now)
            return
        }
        MusicDataRequest.tokenProvider = ProjectTokens()
        var dated: [Entry] = []
        var recent: [Entry] = []
        var problems: [String] = []
        do { dated = try await libraryHistory() } catch { problems.append("library: \(describe(error))") }
        do { recent = try await recentlyPlayed() } catch { problems.append("recently played: \(describe(error))") }
        for p in problems { print("[applemusic] \(p)") }
        if dated.isEmpty && recent.isEmpty && !problems.isEmpty {
            write(historyURL, status: "error", message: "Apple Music did not answer: \(problems.joined(separator: "; ")).", entries: [], now: now)
            return
        }
        let inLibrary = Set(dated.map(key))
        let undated = recent.filter { !inLibrary.contains(key($0)) }
        write(historyURL, status: "ok", message: problems.isEmpty ? "" : problems.joined(separator: "; "),
              entries: Array((dated + undated).prefix(limit)), now: now)
        do { write(favoritesURL, status: "ok", message: "", entries: try await favorites(), now: now) }
        catch { print("[applemusic] favorites: \(describe(error))") }
        do { write(mostPlayedURL, status: "ok", message: "", entries: try await mostPlayed(), now: now) }
        catch { print("[applemusic] most played: \(describe(error))") }
    }

    private static func key(_ e: Entry) -> String { e.title.lowercased() + "|" + e.artist.lowercased() }

    private static func cover(_ artwork: Artwork?) -> String { artwork?.url(width: 300, height: 300)?.absoluteString ?? "" }

    /// The library's songs, last played first, each with that date (it travels with the iCloud library).
    private static func libraryHistory() async throws -> [Entry] {
        var request = MusicLibraryRequest<Song>()
        request.sort(by: \.lastPlayedDate, ascending: false)
        request.limit = limit
        return try await request.response().items.compactMap { song in
            guard let played = song.lastPlayedDate else { return nil }
            return (song.id.rawValue, song.title, song.artistName, cover(song.artwork), played)
        }
    }

    /// The library's most played songs.
    private static func mostPlayed() async throws -> [Entry] {
        var request = MusicLibraryRequest<Song>()
        request.sort(by: \.playCount, ascending: false)
        request.limit = 50
        return try await request.response().items.filter { ($0.playCount ?? 0) > 0 }
            .map { ($0.id.rawValue, $0.title, $0.artistName, cover($0.artwork), nil) }
    }

    /// The songs of Apple's favourites playlist; empty where the library has none.
    private static func favorites() async throws -> [Entry] {
        var request = MusicLibraryRequest<Playlist>()
        request.limit = 500
        guard let playlist = try await request.response().items.first(where: { favoriteNames.contains($0.name.lowercased()) })
        else { return [] }
        let tracks = try await playlist.with([.tracks]).tracks ?? []
        return tracks.prefix(limit).map { ($0.id.rawValue, $0.title, $0.artistName, cover($0.artwork), nil) }
    }

    /// An error as it can be acted on: MusicKit's case and Apple's own text, where there is one.
    private static func describe(_ error: Error) -> String {
        if let data = error as? MusicDataRequest.Error {
            return "\(data.status) \(data.code) \(data.title): \(data.detailText)"
        }
        return "\(String(describing: error)) – \(error.localizedDescription)"
    }

    /// The player's recently played songs, straight from Apple (`/v1/me/recent/played/tracks`, 30 a page).
    private static func recentlyPlayed() async throws -> [Entry] {
        var songs: [Entry] = []
        var offset = 0
        while songs.count < limit {
            var url = URLComponents(string: "https://api.music.apple.com/v1/me/recent/played/tracks")!
            url.queryItems = [URLQueryItem(name: "limit", value: "30"), URLQueryItem(name: "offset", value: String(offset))]
            let response = try await MusicDataRequest(urlRequest: URLRequest(url: url.url!)).response()
            let object = try JSONSerialization.jsonObject(with: response.data) as? [String: Any]
            let page = (object?["data"] as? [[String: Any]]) ?? []
            for item in page {
                guard let id = item["id"] as? String, let a = item["attributes"] as? [String: Any],
                      let title = a["name"] as? String else { continue }
                let template = (a["artwork"] as? [String: Any])?["url"] as? String ?? ""
                songs.append((id, title, a["artistName"] as? String ?? "",
                              template.replacingOccurrences(of: "{w}", with: "300").replacingOccurrences(of: "{h}", with: "300"), nil))
            }
            guard page.count == 30, object?["next"] != nil else { break }
            offset += 30
        }
        return songs
    }

    /// The developer token from the project's server; the user token is MusicKit's own, for that token.
    final class ProjectTokens: MusicUserTokenProvider, MusicDeveloperTokenProvider, @unchecked Sendable {
        func developerToken(options: MusicTokenRequestOptions) async throws -> String {
            let (data, response) = try await URLSession.shared.data(from: KleptonAppleMusic.tokenURL)
            guard (response as? HTTPURLResponse)?.statusCode == 200 || { print("[applemusic] token service answered \((response as? HTTPURLResponse)?.statusCode ?? 0)"); return false }(),
                  let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let token = object["token"] as? String, !token.isEmpty else {
                throw MusicTokenRequestError.developerTokenRequestFailed
            }
            return token
        }
    }

    /// One list in the mod's history cache format. A song without a date gets 0, which the mod files as undated.
    private static func write(_ url: URL, status: String, message: String, entries: [Entry], now: Int64) {
        let iso = ISO8601DateFormatter()
        let tracks: [[String: Any]] = entries.map { e in
            ["id": e.id, "name": e.title, "artists": [e.artist], "albumUrl": e.cover,
             "playedAtMs": e.played.map { Int64($0.timeIntervalSince1970 * 1000) } ?? 0,
             "playedAt": e.played.map { iso.string(from: $0) } ?? ""]
        }
        let object: [String: Any] = ["source": "applemusic", "status": status, "message": message,
                                     "fetchedAtMs": now, "tracks": tracks]
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: url, options: .atomic)
    }
}
#endif
