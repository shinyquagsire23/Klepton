import Foundation
#if canImport(Glibc)
import Glibc
#endif
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// One Auto-Cloud rule from the app's PICS info (`ufs/savefiles`), e.g. for Alyx:
/// root `gameinstall`, path `game/hlvr/save`, pattern `*`, recursive.
public struct CloudSaveRule: Sendable, Equatable {
    public var root: String
    public var path: String
    public var pattern: String
    public var recursive: Bool

    public init(root: String, path: String, pattern: String, recursive: Bool) {
        self.root = root; self.path = path; self.pattern = pattern; self.recursive = recursive
    }

    static func parse(ufs: KV?) -> [CloudSaveRule] {
        (ufs?["savefiles"]?.children ?? []).map { _, v in
            CloudSaveRule(root: v["root"]?.string ?? "", path: v["path"]?.string ?? "",
                          pattern: v["pattern"]?.string ?? "*", recursive: v["recursive"]?.string == "1")
        }
    }

    func matches(fileName: String) -> Bool {
        pattern == "*" || Glob.match(pattern.lowercased(), fileName.lowercased())
    }
}

enum Glob {
    static func match(_ p: String, _ s: String) -> Bool {
        let p = Array(p), s = Array(s)
        var memo = [[Bool?]](repeating: [Bool?](repeating: nil, count: s.count + 1), count: p.count + 1)
        func go(_ i: Int, _ j: Int) -> Bool {
            if let m = memo[i][j] { return m }
            let r: Bool
            if i == p.count { r = j == s.count }
            else if p[i] == "*" { r = go(i + 1, j) || (j < s.count && go(i, j + 1)) }
            else { r = j < s.count && (p[i] == "?" || p[i] == s[j]) && go(i + 1, j + 1) }
            memo[i][j] = r
            return r
        }
        return go(0, 0)
    }
}

/// What the sync engine needs from Steam Cloud. `SteamCloud` is the real one; tests use a fake.
public protocol CloudRemote {
    func list() async throws -> [CloudFile]
    func download(_ file: CloudFile) async throws -> Data
    /// Uploads files as one batch. Throws when a file did not go up; the ones before it, and the ones behind
    /// a file Steam itself refused, are in the cloud all the same.
    func upload(_ files: [(name: String, data: Data, time: Date)]) async throws
}

/// Steam Cloud over the CM connection.
public struct SteamCloud: CloudRemote {
    public let cm: CMClient
    public let appID: UInt32
    public let buildID: UInt64
    public init(cm: CMClient, appID: UInt32, buildID: UInt64 = 0) { self.cm = cm; self.appID = appID; self.buildID = buildID }
    /// Called after every file of an upload: how many are through, of how many, and what became of this one.
    /// An upload that is cut off — the game is left while it runs — otherwise leaves no account of how far it
    /// came. (Not in Glovebox's kit.)
    public var uploadStep: (@Sendable (_ done: Int, _ of: Int, _ what: String) -> Void)?
    /// Where the upload batches that are open are written down, so that one nobody closed — the game was
    /// left while it ran — is closed by the next sync. Steam's own word on it (ICloudService,
    /// CompleteAppUploadBatch): a batch that is never completed blocks new uploads for that user and app
    /// for several minutes, with "too many pending". Seen on a headset as EResult 108 at the start after a
    /// short one. (Not in Glovebox's kit.)
    public var batchLedger: URL?
    /// How often the opening of a batch is tried again when Steam answers "too many pending", and the pause
    /// before the first of those; each later pause is twice the one before. Far apart and few, because it is
    /// not known whether asking keeps the block alive.
    public var batchRetries = 4
    public var batchRetryPause: Double = 120

    private func ledger() -> [UInt64] {
        guard let batchLedger, let d = try? Data(contentsOf: batchLedger),
              let a = try? JSONDecoder().decode([String].self, from: d) else { return [] }
        return a.compactMap { UInt64($0) }
    }

    private func setLedger(_ ids: [UInt64]) {
        guard let batchLedger else { return }
        if ids.isEmpty { try? FileManager.default.removeItem(at: batchLedger); return }
        if let d = try? JSONEncoder().encode(ids.map(String.init)) { try? d.write(to: batchLedger, options: .atomic) }
    }

    /// Closes, as failed, every batch the ledger still holds. Returns how many there were.
    @discardableResult
    public func closeLeftOverBatches() async -> Int {
        let open = ledger()
        var left: [UInt64] = []
        for id in open {
            do {
                _ = try await cm.call("Cloud.CompleteAppUploadBatchBlocking#1",
                                      CloudCompleteAppUploadBatchRequest(appID: appID, batchID: id, result: .fail))
            } catch SteamError.result {
                // Steam knows no such open batch any more: it lapsed by itself.
            } catch {
                left.append(id)                    // no answer: asked again next time
            }
        }
        setLedger(left)
        return open.count
    }

    public func list() async throws -> [CloudFile] {
        var out: [CloudFile] = []
        var start: UInt32 = 0
        while true {
            let data = try await cm.call("Cloud.EnumerateUserFiles#1", CloudEnumerateUserFilesRequest(appID: appID, startIndex: start))
            let r = try CloudEnumerateUserFilesResponse(proto: data)
            out += r.files
            start += UInt32(r.files.count)
            if r.files.isEmpty || start >= r.total { break }
        }
        return out
    }

    public func download(_ file: CloudFile) async throws -> Data {
        let data = try await cm.call("Cloud.ClientFileDownload#1", CloudClientFileDownloadRequest(appID: appID, filename: file.filename))
        let r = try CloudClientFileDownloadResponse(proto: data)
        let urlString = r.host.isEmpty ? file.url : "\(r.https ? "https" : "http")://\(r.host)\(r.path)"
        guard let url = URL(string: urlString) else { throw SteamError.badData("cloud file URL") }
        var req = URLRequest(url: url)
        for (k, v) in r.headers { req.setValue(v, forHTTPHeaderField: k) }
        let (body, resp) = try await URLSession.shared.data(for: req)
        guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw SteamError.badData("cloud download for \(file.filename)") }
        var content = body
        if r.rawFileSize != 0, r.rawFileSize != r.fileSize, body.starts(with: [0x50, 0x4B, 0x03, 0x04]) {
            content = try Codec.unzipSingle(body)
        }
        let expected = r.sha.isEmpty ? Data(hex: file.sha1Hex) : r.sha
        if let expected, !expected.isEmpty, Codec.sha1(content) != expected {
            throw SteamError.badData("cloud file \(file.filename) failed its hash check")
        }
        return content
    }

    public func upload(_ files: [(name: String, data: Data, time: Date)]) async throws {
        guard !files.isEmpty else { return }
        await closeLeftOverBatches()
        // Too many pending (108): a batch nobody closed, and not one on the ledger, is still open at Steam,
        // which lets it lapse by itself — after more than ten minutes on a headset, whatever "several"
        // means. The upload runs behind the game, so it can wait for that instead of giving up until the
        // next start. (Not in Glovebox's kit.)
        var batchData = Data()
        for attempt in 0... {
            do {
                batchData = try await cm.call("Cloud.BeginAppUploadBatch#1",
                                              CloudBeginAppUploadBatchRequest(appID: appID, machineName: SteamAuth.deviceName(),
                                                                              files: files.map(\.name), buildID: buildID))
                break
            } catch SteamError.result(let r, _) where r.rawValue == 108 && attempt < batchRetries {
                let pause = batchRetryPause * Double(1 << attempt)
                uploadStep?(0, files.count, "Steam still has an earlier upload open; trying again in \(Int(pause)) s (\(attempt + 1)/\(batchRetries))")
                try await Task.sleep(nanoseconds: UInt64(pause * 1_000_000_000))
            }
        }
        let batch = try CloudBeginAppUploadBatchResponse(proto: batchData).batchID
        // Off the ledger only once Steam has taken the closing of it.
        setLedger(ledger() + [batch])
        func close(_ result: EResult) async throws {
            _ = try await cm.call("Cloud.CompleteAppUploadBatchBlocking#1",
                                  CloudCompleteAppUploadBatchRequest(appID: appID, batchID: batch, result: result))
            setLedger(ledger().filter { $0 != batch })
        }
        var result = EResult.ok
        // A file Steam itself refuses does not hold back the ones behind it; it is reported once they are
        // through, by name. Anything else (no connection, no answer) ends the batch as before. (Glovebox's
        // kit stops at the first file either way.)
        var refused: SteamError?
        do {
            for (i, f) in files.enumerated() {
                do {
                    let again = try await uploadOne(f, batch: batch)
                    uploadStep?(i + 1, files.count, "\(f.name): sent, \(f.data.count) bytes\(again ? ", after calling off an unfinished upload of it" : "")")
                } catch SteamError.result(let r, let what) {
                    if refused == nil { refused = .result(r, "\(what) (\(f.name))") }
                    uploadStep?(i + 1, files.count, "\(f.name): refused, \(what) \(r)")
                }
            }
        } catch {
            try? await close(.fail)
            throw error
        }
        if refused != nil { result = .fail }
        try await close(result)
        if let refused { throw refused }
    }

    /// True when the upload had to be begun a second time.
    @discardableResult
    private func uploadOne(_ f: (name: String, data: Data, time: Date), batch: UInt64) async throws -> Bool {
        let sha = Codec.sha1(f.data)
        let request = CloudClientBeginFileUploadRequest(appID: appID, size: UInt32(f.data.count), sha: sha,
                                                        timestamp: UInt64(f.time.timeIntervalSince1970),
                                                        filename: f.name, batchID: batch)
        let begin: Data
        var again = false
        do {
            begin = try await cm.call("Cloud.ClientBeginFileUpload#1", request)
        } catch SteamError.result(let r, _) where r.rawValue == 10 {
            // Busy: an upload of this very file was begun before and never finished or called off — a sync
            // that was ended in the middle of it (seen on a headset: the one file in flight when a start cut
            // the sync off was refused at every start after it, and nothing behind it in the batch went up).
            // Call that one off, and begin again. (Not in Glovebox's kit.)
            _ = try? await cm.call("Cloud.ClientCommitFileUpload#1",
                                   CloudClientCommitFileUploadRequest(succeeded: false, appID: appID, sha: sha, filename: f.name))
            begin = try await cm.call("Cloud.ClientBeginFileUpload#1", request)
            again = true
        }
        let blocks = try CloudClientBeginFileUploadResponse(proto: begin).blocks
        var ok = true
        for b in blocks {
            guard let url = URL(string: "\(b.https ? "https" : "http")://\(b.host)\(b.path)") else { ok = false; break }
            var req = URLRequest(url: url)
            req.httpMethod = b.method
            for (k, v) in b.headers { req.setValue(v, forHTTPHeaderField: k) }
            if let explicit = b.explicitBody {
                req.httpBody = explicit
            } else {
                let start = Int(b.offset), end = min(f.data.count, start + Int(b.length))
                req.httpBody = f.data.subdata(in: start..<end)
            }
            let (_, resp) = try await URLSession.shared.data(for: req)
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if !(200..<300).contains(code) { ok = false; break }
        }
        let commit = try await cm.call("Cloud.ClientCommitFileUpload#1",
                                       CloudClientCommitFileUploadRequest(succeeded: ok, appID: appID, sha: sha, filename: f.name))
        guard ok, try CloudClientCommitFileUploadResponse(proto: commit).committed else {
            throw SteamError.badData("Steam Cloud didn't accept \(f.name)")
        }
        return again
    }
}

/// Two-way save sync like the Steam client's Auto-Cloud, tuned so a save is never replaced by
/// something it didn't come from:
/// - Saves live where the game looks for them, in lowercase (the game's own spelling on a
///   case-sensitive disk), whatever case the cloud's names use. Steam treats names case-insensitively.
/// - A headset save goes up only over the exact cloud version it was last in sync with, and that
///   cloud version is backed up first.
/// - When the cloud changed (alone or as well as the headset), or there's no history, the cloud's
///   copy wins and the headset's is set aside in `.cloud-backups/`.
/// - A save slot the game started here (e.g. `s0`) that shares its name with a different slot in
///   the cloud moves to a free slot number, so both games are kept and both show in the game.
/// - Every file is written to a temp file, hash-checked, then moved into place; a save is never
///   half-written.
/// - Deletions are never propagated in either direction.
/// - Files in `neverUpload` (the game's own settings, scratch files) only ever come down.
/// - It only runs while the game is not running, so files never change under it.
public struct CloudSync {
    public struct Report: Sendable, Equatable {
        public var downloaded: [String] = []
        public var uploaded: [String] = []
        public var backedUp: [String] = []
        /// Slots moved to a free number, e.g. "s0 → s5".
        public var relocated: [String] = []
        /// With uploads off: what would have gone up.
        public var wouldUpload: [String] = []
        public var unchanged = 0
        public init() {}
    }

    public let remote: CloudRemote
    public let rules: [CloudSaveRule]
    /// The folder a rule's root token maps to (e.g. `gameinstall` → the depot root), plus a path mapper
    /// for installs that don't keep the depot layout.
    public let rootFolder: (String) -> URL?
    public let localPath: (String) -> String?
    public let stateURL: URL
    public let backupRoot: URL
    public var now: () -> Date = Date.init
    /// Sync runs whose backups of cloud copies are kept (headset copies set aside are always kept).
    public var keptCloudBackups = 10
    /// Off: download-only. Nothing is sent to the cloud; the report lists what would have gone up,
    /// and the sync history isn't advanced for it, so it goes up once uploads are turned on.
    public var uploads = true
    /// File names (globs, any case) that never go up, e.g. the game's own settings. They still come
    /// down from the cloud like any other save.
    public var neverUpload: [String] = []
    /// Called once when nothing more will be written here: every download is in place and what
    /// remains is sending the uploads, whose bytes were read before. A game may start from then on
    /// while the uploads go on behind it. (Not in Glovebox's kit; added for that.)
    public var localSettled: (@Sendable () -> Void)?

    public init(remote: CloudRemote, rules: [CloudSaveRule], rootFolder: @escaping (String) -> URL?,
                localPath: @escaping (String) -> String? = { $0 }, stateURL: URL, backupRoot: URL) {
        self.remote = remote; self.rules = rules; self.rootFolder = rootFolder; self.localPath = localPath
        self.stateURL = stateURL; self.backupRoot = backupRoot
    }

    struct Local { let url: URL; let data: Data; let sha: String; let time: Date }

    /// `%GameInstall%game/hlvr/save/x.sav` → (gameinstall, game/hlvr/save/x.sav)
    public static func split(_ name: String) -> (root: String?, path: String) {
        guard name.hasPrefix("%"), let end = name.dropFirst().firstIndex(of: "%") else { return (nil, name) }
        return (String(name[name.index(after: name.startIndex)..<end]), String(name[name.index(after: end)...]))
    }

    /// Where a cloud file lives here: always lowercase below the install folder.
    private func url(forCloudName name: String) -> URL? {
        let (root, path) = Self.split(name)
        guard let root, let base = rootFolder(root.lowercased()), let rel = localPath(path.lowercased()) else { return nil }
        let rule = rules.first { $0.root.lowercased() == root.lowercased() && path.lowercased().hasPrefix($0.path.lowercased() + "/") }
        guard let rule, rule.matches(fileName: (path as NSString).lastPathComponent) else { return nil }
        return base.appendingPathComponent(rel)
    }

    /// A rule's folder as the game spells it.
    private func folder(for rule: CloudSaveRule) -> (base: URL, rel: String)? {
        guard let base = rootFolder(rule.root.lowercased()), let rel = localPath(rule.path.lowercased()) else { return nil }
        return (base, rel)
    }

    /// The cloud key for a path below a rule's folder: the lowercased cloud name.
    private static func key(_ rule: CloudSaveRule, _ sub: String) -> String {
        "%\(tokenName(rule.root))%\(rule.path)/\(sub)".lowercased()
    }

    /// Every local file the rules cover in the game's own folder, keyed by lowercased cloud name.
    /// (`prepareLocal` has already folded every other spelling into it.)
    private func scanLocal() -> [String: (name: String, url: URL)] {
        var out: [String: (String, URL)] = [:]
        for rule in rules {
            guard let (base, rel) = folder(for: rule) else { continue }
            for f in Self.files(in: base.appendingPathComponent(rel), rule: rule) {
                let name = "%\(Self.tokenName(rule.root))%\(rule.path)/\(f.sub.lowercased())"
                out[name.lowercased()] = (name, f.url)
            }
        }
        return out
    }

    /// The files a rule covers under one folder, with paths relative to it.
    private static func files(in dir: URL, rule: CloudSaveRule) -> [(url: URL, sub: String)] {
        // Relative paths from the enumerator itself: comparing absolute paths breaks when part of
        // the path is a symlink (macOS /var → /private/var).
        guard let e = FileManager.default.enumerator(atPath: dir.path) else { return [] }
        var out: [(URL, String)] = []
        for case let sub as String in e {
            let u = dir.appendingPathComponent(sub)
            guard isFile(u) else { continue }
            if (sub as NSString).lastPathComponent.hasPrefix(".") { continue }  // our temp files
            if !rule.recursive, sub.contains("/") { continue }
            guard rule.matches(fileName: u.lastPathComponent) else { continue }
            out.append((u, sub))
        }
        return out
    }

    private static func isFile(_ u: URL) -> Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: u.path, isDirectory: &isDir) && !isDir.boolValue
    }

    /// Every spelling of `rel` under `base` (hlvr/save, hlvr/SAVE, …), on a case-sensitive disk.
    static func spellings(of rel: String, in base: URL) -> [URL] {
        var dirs = [base]
        for part in rel.split(separator: "/") {
            dirs = dirs.flatMap { d -> [URL] in
                let names = (try? FileManager.default.contentsOfDirectory(atPath: d.path)) ?? []
                return names.filter { $0.lowercased() == part.lowercased() }.map { d.appendingPathComponent($0) }
                    .filter { var isDir: ObjCBool = false
                              return FileManager.default.fileExists(atPath: $0.path, isDirectory: &isDir) && isDir.boolValue }
            }
        }
        return dirs
    }

    /// A save slot folder, e.g. `s0` in `s0/autosave.sav`: letters then a number.
    static func slot(of sub: String) -> String? {
        guard let first = sub.split(separator: "/", omittingEmptySubsequences: false).first, sub.contains("/") else { return nil }
        let s = String(first)
        let digits = s.reversed().prefix { $0.isNumber }.count
        guard digits > 0, s.dropLast(digits).allSatisfy({ $0.isLetter || $0 == "_" }) else { return nil }
        return s
    }

    /// The lowest slot number with the same prefix that nobody uses: `s0` with s0…s4 taken → `s5`.
    /// Lowest rather than next-after-highest, so a game stays within the slots the game lists.
    static func freeSlot(after slot: String, taken: Set<String>) -> String {
        let digits = slot.reversed().prefix { $0.isNumber }.count
        let prefix = String(slot.dropLast(digits))
        let used = Set(taken.compactMap { t -> Int? in
            guard t.hasPrefix(prefix) else { return nil }
            return Int(t.dropFirst(prefix.count))
        })
        var n = 0
        while used.contains(n) { n += 1 }
        return prefix + String(n)
    }

    /// Whether any file is left anywhere under a folder.
    private static func hasFiles(_ d: URL) -> Bool {
        let e = FileManager.default.enumerator(atPath: d.path)
        while let sub = e?.nextObject() as? String { if isFile(d.appendingPathComponent(sub)) { return true } }
        return false
    }

    /// Two paths to one file, as on a case-insensitive disk (`SAVE/a` and `save/a`).
    private static func sameFile(_ a: URL, _ b: URL) -> Bool {
        let fm = FileManager.default
        guard let x = try? fm.attributesOfItem(atPath: a.path), let y = try? fm.attributesOfItem(atPath: b.path),
              let ix = x[.systemFileNumber] as? NSNumber, let iy = y[.systemFileNumber] as? NSNumber else { return false }
        return ix == iy && (x[.systemNumber] as? NSNumber) == (y[.systemNumber] as? NSNumber)
    }

    private func neverGoesUp(_ path: String) -> Bool {
        let file = (path.lowercased() as NSString).lastPathComponent
        return neverUpload.contains { Glob.match($0.lowercased(), file) }
    }

    private static func sha(_ u: URL) -> String? { (try? Data(contentsOf: u)).map { Codec.sha1($0).hex } }

    /// Before anything is compared, every save goes where the game looks for it:
    /// 1. A slot the game started here that collides with a different slot of the same name in the
    ///    cloud moves to a free number. It's a collision when one of its files differs from the cloud's
    ///    and either there's no sync history for it, or the version last synced sits under another
    ///    spelling (an earlier launcher kept the cloud's, e.g. `SAVE/S0`, which the game never sees on
    ///    a case-sensitive disk, so it started a new game in `save/s0`).
    /// 2. A loose file (in no slot) that collides the same way is set aside; the cloud's copy comes next.
    /// 3. Every other spelling is folded into the lowercase one. Two different copies of one file keep
    ///    the newer in place and the other in the backups.
    private func prepareLocal(remote: [String: CloudFile], state: inout [String: String], stamp: String,
                              report: inout Report) throws {
        let fm = FileManager.default
        for rule in rules {
            guard let (base, rel) = folder(for: rule) else { continue }
            let dir = base.appendingPathComponent(rel)
            func found() -> [(url: URL, sub: String, canonical: Bool)] {
                Self.spellings(of: rel, in: base).flatMap { d in
                    Self.files(in: d, rule: rule).map { f in
                        (f.url, f.sub.lowercased(), d.path == dir.path && f.sub == f.sub.lowercased())
                    }
                }
            }
            let all = found()
            guard all.contains(where: { !$0.canonical }) || all.contains(where: { remote[Self.key(rule, $0.sub)] != nil }) else { continue }
            var shas: [String: String] = [:]
            for f in all { shas[f.url.path] = Self.sha(f.url) }

            var foreignSlots = Set<String>()
            var looseAside: [(url: URL, sub: String)] = []
            // A file that never goes up has no history from this side, so it says nothing about whose slot it is.
            for f in all where f.canonical && !neverGoesUp(f.sub) {
                let k = Self.key(rule, f.sub)
                guard let r = remote[k], let s = shas[f.url.path], s != r.sha1Hex.lowercased() else { continue }
                let syncedElsewhere = all.contains { !$0.canonical && $0.sub == f.sub && shas[$0.url.path] == state[k] }
                guard state[k] == nil || syncedElsewhere else { continue }
                if let slot = Self.slot(of: f.sub) { foreignSlots.insert(slot) } else { looseAside.append((f.url, f.sub)) }
            }

            var taken = Set(all.compactMap { Self.slot(of: $0.sub) })
            let prefix = Self.key(rule, "")
            for k in remote.keys where k.hasPrefix(prefix) {
                if let s = Self.slot(of: String(k.dropFirst(prefix.count))) { taken.insert(s) }
            }
            for slot in foreignSlots.sorted() {
                // A slot is free when no file is in it, here or in the cloud. The game makes empty
                // folders for its unused slots (s5…s9 next to s0…s4); an empty one is replaced.
                var fresh = Self.freeSlot(after: slot, taken: taken)
                while Self.hasFiles(dir.appendingPathComponent(fresh)) {
                    taken.insert(fresh)
                    fresh = Self.freeSlot(after: slot, taken: taken)
                }
                taken.insert(fresh)
                if fm.fileExists(atPath: dir.appendingPathComponent(fresh).path) {
                    try fm.removeItem(at: dir.appendingPathComponent(fresh))
                }
                try fm.moveItem(at: dir.appendingPathComponent(slot), to: dir.appendingPathComponent(fresh))
                report.relocated.append("\(slot) → \(fresh)")
                // What was synced in this slot left with it: the cloud's copies of the slot come down.
                for k in state.keys where k.hasPrefix(Self.key(rule, slot + "/")) { state[k] = nil }
            }
            for f in looseAside {
                report.backedUp.append(try backup(fm.contents(atPath: f.url.path) ?? Data(), as: "headset/\(f.sub)", stamp: stamp))
                try fm.removeItem(at: f.url)
                state[Self.key(rule, f.sub)] = nil
            }

            for f in found() where !f.canonical {
                let dest = dir.appendingPathComponent(f.sub)
                if Self.sameFile(f.url, dest) { continue }
                guard fm.fileExists(atPath: dest.path) else {
                    try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try fm.moveItem(at: f.url, to: dest)
                    continue
                }
                if Self.sha(f.url) == Self.sha(dest) { try fm.removeItem(at: f.url); continue }
                func time(_ u: URL) -> Date { (try? u.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast }
                let older = time(f.url) > time(dest) ? dest : f.url
                report.backedUp.append(try backup(fm.contents(atPath: older.path) ?? Data(), as: "headset/\(f.sub)", stamp: stamp))
                if older == dest {
                    try fm.removeItem(at: dest)
                    try fm.moveItem(at: f.url, to: dest)
                } else {
                    try fm.removeItem(at: f.url)
                }
            }
            for d in Self.spellings(of: rel, in: base) where d.path != dir.path { Self.removeIfNoFiles(d) }
            Self.removeEmptyMisspelled(in: dir)
        }
    }

    /// Removes a folder whose tree holds no files any more.
    private static func removeIfNoFiles(_ d: URL) {
        let e = FileManager.default.enumerator(atPath: d.path)
        while let sub = e?.nextObject() as? String { if isFile(d.appendingPathComponent(sub)) { return } }
        try? FileManager.default.removeItem(at: d)
    }

    /// Empty folders left behind under the game's folder by other spellings (e.g. `save/S0`).
    private static func removeEmptyMisspelled(in dir: URL) {
        let subs = (FileManager.default.enumerator(atPath: dir.path)?.allObjects as? [String]) ?? []
        for sub in subs.sorted(by: { $0.count > $1.count }) where sub != sub.lowercased() {
            let u = dir.appendingPathComponent(sub)
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: u.path, isDirectory: &isDir), isDir.boolValue { removeIfNoFiles(u) }
        }
    }

    /// Steam writes root tokens in their canonical case, e.g. `GameInstall`.
    static func tokenName(_ root: String) -> String {
        ["gameinstall": "GameInstall", "winmydocuments": "WinMyDocuments", "winappdataroaming": "WinAppDataRoaming",
         "winappdatalocal": "WinAppDataLocal", "macappsupport": "MacAppSupport", "linuxhome": "LinuxHome"][root.lowercased()] ?? root
    }

    private func read(_ url: URL) -> Local? {
        guard let d = try? Data(contentsOf: url) else { return nil }
        let t = (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
        return Local(url: url, data: d, sha: Codec.sha1(d).hex, time: t)
    }

    /// Keeps a copy under `.cloud-backups/<run>/headset|cloud/<path>`; returns that relative path.
    private func backup(_ data: Data, as rel: String, stamp: String) throws -> String {
        let dest = backupRoot.appendingPathComponent(stamp).appendingPathComponent(rel)
        try FileManager.default.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: dest, options: .atomic)
        return "\(stamp)/\(rel)"
    }

    /// The path below the root token, lowercased, for backups: `game/hlvr/save/s0/autosave.sav`.
    private static func backupPath(_ key: String) -> String { split(key).path }

    /// Old cloud copies beyond the last `keptCloudBackups` runs go; headset copies stay.
    private func pruneCloudBackups() {
        let fm = FileManager.default
        let runs = ((try? fm.contentsOfDirectory(atPath: backupRoot.path)) ?? []).filter { !$0.hasPrefix(".") }.sorted()
        let withCloud = runs.filter { fm.fileExists(atPath: backupRoot.appendingPathComponent($0).appendingPathComponent("cloud").path) }
        for run in withCloud.dropLast(keptCloudBackups) {
            let d = backupRoot.appendingPathComponent(run)
            try? fm.removeItem(at: d.appendingPathComponent("cloud"))
            if ((try? fm.contentsOfDirectory(atPath: d.path)) ?? []).isEmpty { try? fm.removeItem(at: d) }
        }
    }

    private func writeSafely(_ data: Data, sha: String, to url: URL, time: Date) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let tmp = url.deletingLastPathComponent().appendingPathComponent(".\(url.lastPathComponent).cloud-tmp")
        try data.write(to: tmp)
        guard let check = try? Data(contentsOf: tmp), Codec.sha1(check).hex == sha else {
            try? fm.removeItem(at: tmp)
            throw SteamError.badData("couldn't write \(url.lastPathComponent) intact")
        }
        // rename(2) swaps the file in atomically: readers see the old save or the new one, never a mix.
        guard rename(tmp.path, url.path) == 0 else {
            try? fm.removeItem(at: tmp)
            throw SteamError.badData("couldn't replace \(url.lastPathComponent)")
        }
        try? fm.setAttributes([.modificationDate: time], ofItemAtPath: url.path)
    }

    public func run() async throws -> Report {
        var state = (try? JSONDecoder().decode([String: String].self, from: Data(contentsOf: stateURL))) ?? [:]
        func saveState() throws { try JSONEncoder().encode(state).write(to: stateURL, options: .atomic) }
        var report = Report()
        let stamp = ISO8601DateFormatter().string(from: now()).replacingOccurrences(of: ":", with: "-")

        // One entry per name, however the cloud spells it; if it has two spellings, the newer one.
        var remoteByKey: [String: CloudFile] = [:]
        for f in try await remote.list() {
            let k = f.filename.lowercased()
            if let seen = remoteByKey[k], seen.timestamp >= f.timestamp { continue }
            remoteByKey[k] = f
        }
        try prepareLocal(remote: remoteByKey, state: &state, stamp: stamp, report: &report)
        let localByKey = scanLocal()
        var uploads: [(key: String, name: String, data: Data, time: Date, sha: String)] = []

        for key in Set(remoteByKey.keys).union(localByKey.keys).sorted() {
            let r = remoteByKey[key]
            let l = localByKey[key].flatMap { read($0.url) }
            let last = state[key]
            let rSHA = r?.sha1Hex.lowercased()

            if r != nil, let l, l.sha == rSHA {
                state[key] = l.sha
                report.unchanged += 1
                continue
            }
            if let r, l == nil {
                // We synced this exact version before and it's gone here: the game deleted it.
                // Leave the cloud copy alone rather than bring it back.
                if let last, last == rSHA { continue }
                guard let dest = url(forCloudName: r.filename) else { continue }
                let data = try await remote.download(r)
                try writeSafely(data, sha: Codec.sha1(data).hex, to: dest, time: Date(timeIntervalSince1970: TimeInterval(r.timestamp)))
                state[key] = Codec.sha1(data).hex
                report.downloaded.append(r.filename)
                try saveState()
                continue
            }
            guard let l else { continue }
            let staysHere = neverGoesUp(key)
            guard let r else {
                if last == l.sha { continue }  // deleted from the cloud elsewhere; don't resurrect it
                if staysHere { continue }
                if !self.uploads { report.wouldUpload.append(localByKey[key]!.name); continue }
                // An empty file is not a save and is not sent (not in Glovebox's kit; the game leaves empty
                // pictures and markers beside its saves).
                if l.data.isEmpty { continue }
                uploads.append((key, localByKey[key]!.name, l.data, l.time, l.sha))
                continue
            }
            let remoteTime = Date(timeIntervalSince1970: TimeInterval(r.timestamp))
            if let last, rSHA == last {
                // Only the headset changed since the last sync: it goes up under the cloud's own name,
                // over the very version it came from, and that version is kept first.
                if staysHere { continue }
                if !self.uploads { report.wouldUpload.append(r.filename); continue }
                let old = try await remote.download(r)
                report.backedUp.append(try backup(old, as: "cloud/" + Self.backupPath(key), stamp: stamp))
                uploads.append((key, r.filename, l.data, l.time, l.sha))
            } else {
                // The cloud changed (alone or as well), or there's no history: the cloud's copy wins,
                // and the headset's is set aside.
                let data = try await remote.download(r)
                report.backedUp.append(try backup(l.data, as: "headset/" + Self.backupPath(key), stamp: stamp))
                try writeSafely(data, sha: Codec.sha1(data).hex, to: l.url, time: remoteTime)
                state[key] = Codec.sha1(data).hex
                report.downloaded.append(r.filename)
                try saveState()
            }
        }

        try saveState()
        localSettled?()
        if !uploads.isEmpty {
            try await remote.upload(uploads.map { ($0.name, $0.data, $0.time) })
            for u in uploads { state[u.key] = u.sha; report.uploaded.append(u.name) }
        }
        try saveState()
        pruneCloudBackups()
        return report
    }
}
