// Getting the guest's DATA onto the headset without a Mac in the loop.
//
// The guest's code is in the bundle — it has to be, it is signed — but its data
// is not: an APK the engine opens as a zip, the assets/ tree unpacked beside
// it, and for a split-binary title an OBB. stage_assets.sh pushes those over
// devicectl, which needs the Mac, the cable and the build tree every time the
// container is lost. This is the same three things, fetched by the app itself:
//
//   1. see what is missing (KLGameFiles),
//   2. download the APK and the OBB from the store's CDN with the OWNER's
//      session — they sign in to their own account in a web view here, and the
//      CDN decides whether that account may have the files,
//   3. unpack assets/ out of the APK and put the two metadata files beside it.
//
// Step 3 alone is also the no-network path: copy the APK (and OBB) into this
// app's folder in Files and press Unpack.
//
// Which files, under which names and ids, is a property of the BUILD: the
// translated libraries in this bundle match exactly one APK, so the generator
// bakes the answers into Info.plist (targets.py) rather than asking for them.
//
// The session token never leaves this file except as an HTTP credential: it is
// kept in the Keychain, never logged, and never shown.
import Compression
import Foundation
import Security
import SwiftUI
import UIKit
import WebKit

// MARK: - What this build needs, and where

struct KLGameFiles {
    let tree: String        // "beatsaber140" — the unpacked tree's directory name
    let apkName: String     // "beatsaber-1.40.apk"
    let obbRel: String      // "obb", relative to android-files/
    let obbFile: String     // "" for a guest whose data is all in the APK
    let apkID: String       // the store's binary ids; "" = no download configured
    let obbID: String

    /// nil for a build that carries no data description (the folder-fed
    /// launcher titles, Steam Link) — those keep the staging path they had.
    static let current: KLGameFiles? = {
        func s(_ k: String) -> String {
            (Bundle.main.object(forInfoDictionaryKey: k) as? String) ?? ""
        }
        let g = KLGameFiles(tree: s("KLTree"), apkName: s("KLApkName"), obbRel: s("KLObbRel"),
                            obbFile: s("KLObbFile"), apkID: s("KLMetaApkID"),
                            obbID: s("KLMetaObbID"))
        return (g.tree.isEmpty || g.apkName.isEmpty) ? nil : g
    }()

    private var docs: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }
    var apkURL: URL    { docs.appendingPathComponent(apkName) }
    var treeURL: URL   { docs.appendingPathComponent(tree) }
    var assetsURL: URL { treeURL.appendingPathComponent("assets") }
    var obbURL: URL? {
        obbFile.isEmpty ? nil
            : docs.appendingPathComponent("android-files").appendingPathComponent(obbRel)
                  .appendingPathComponent(obbFile)
    }

    private func exists(_ u: URL) -> Bool { FileManager.default.fileExists(atPath: u.path) }
    var hasAPK: Bool    { exists(apkURL) }
    var hasAssets: Bool { exists(assetsURL) }
    var hasOBB: Bool    { obbURL.map(exists) ?? true }
    var complete: Bool  { hasAPK && hasAssets && hasOBB }
    var canDownload: Bool { !apkID.isEmpty && (obbFile.isEmpty || !obbID.isEmpty) }

    static func size(_ u: URL) -> String {
        let n = (try? FileManager.default.attributesOfItem(atPath: u.path)[.size] as? Int64) ?? nil
        return n.map { ByteCountFormatter.string(fromByteCount: $0, countStyle: .file) } ?? "—"
    }
}

// MARK: - The session token, in the Keychain

enum KLMetaToken {
    private static let query: [String: Any] = [
        kSecClass as String: kSecClassGenericPassword,
        kSecAttrService as String: "klepton.meta.session",
        kSecAttrAccount as String: "oc_ac_at",
    ]
    static func load() -> String? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
    static func save(_ token: String) {
        SecItemDelete(query as CFDictionary)
        var q = query
        q[kSecValueData as String] = Data(token.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        SecItemAdd(q as CFDictionary, nil)
    }
    static func clear() { SecItemDelete(query as CFDictionary) }
}

// MARK: - Unpacking assets/ out of the APK

enum KLUnpackError: LocalizedError {
    case notAZip, truncated, badEntry(String), inflate(String), io(String)
    var errorDescription: String? {
        switch self {
        case .notAZip:          return "The APK is not a zip archive (the download may have been an error page)."
        case .truncated:        return "The APK is incomplete."
        case .badEntry(let n):  return "The APK holds an entry this cannot unpack: \(n)"
        case .inflate(let n):   return "Could not decompress \(n)."
        case .io(let m):        return m
        }
    }
}

enum KLApkUnpacker {
    private struct Entry { let name: String; let method: UInt16; let csize, usize, lhdr: UInt64 }

    private static func u16(_ d: Data, _ o: Int) -> UInt16 {
        UInt16(d[d.startIndex + o]) | UInt16(d[d.startIndex + o + 1]) << 8
    }
    private static func u32(_ d: Data, _ o: Int) -> UInt32 {
        UInt32(u16(d, o)) | UInt32(u16(d, o + 2)) << 16
    }

    /// The central directory: one record per entry, at the end of the file.
    /// No zip64 — an APK that needed it would be over 4 GB, and none is.
    private static func entries(_ fh: FileHandle, size: UInt64) throws -> [Entry] {
        let tailLen = Int(min(size, 65_557))
        try fh.seek(toOffset: size - UInt64(tailLen))
        guard let tail = try fh.read(upToCount: tailLen), tail.count >= 22 else {
            throw KLUnpackError.truncated
        }
        var eocd = -1
        var i = tail.count - 22
        while i >= 0 {
            if u32(tail, i) == 0x0605_4b50 { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { throw KLUnpackError.notAZip }
        let count = Int(u16(tail, eocd + 10))
        let cdSize = Int(u32(tail, eocd + 12))
        let cdOff = UInt64(u32(tail, eocd + 16))
        try fh.seek(toOffset: cdOff)
        guard let cd = try fh.read(upToCount: cdSize), cd.count == cdSize else {
            throw KLUnpackError.truncated
        }
        var out: [Entry] = []
        out.reserveCapacity(count)
        var p = 0
        for _ in 0 ..< count {
            guard p + 46 <= cd.count, u32(cd, p) == 0x0201_4b50 else { throw KLUnpackError.truncated }
            let nameLen = Int(u16(cd, p + 28)), extra = Int(u16(cd, p + 30)),
                comment = Int(u16(cd, p + 32))
            guard p + 46 + nameLen <= cd.count else { throw KLUnpackError.truncated }
            let nameData = cd.subdata(in: cd.startIndex + p + 46 ..< cd.startIndex + p + 46 + nameLen)
            let name = String(decoding: nameData, as: UTF8.self)
            out.append(Entry(name: name, method: u16(cd, p + 10),
                             csize: UInt64(u32(cd, p + 20)), usize: UInt64(u32(cd, p + 24)),
                             lhdr: UInt64(u32(cd, p + 42))))
            p += 46 + nameLen + extra + comment
        }
        return out
    }

    /// assets/** -> <dest>/**, written beside the destination first and moved
    /// into place whole, so an interrupted unpack is never mistaken for a
    /// finished one (the boot check is "does assets/ exist").
    static func unpackAssets(apk: URL, to dest: URL,
                             progress: @escaping (Double) -> Void) throws {
        let fm = FileManager.default
        let size = (try fm.attributesOfItem(atPath: apk.path)[.size] as? UInt64) ?? 0
        guard size > 22 else { throw KLUnpackError.truncated }
        let fh = try FileHandle(forReadingFrom: apk)
        defer { try? fh.close() }

        let all = try entries(fh, size: size).filter {
            $0.name.hasPrefix("assets/") && !$0.name.hasSuffix("/")
        }
        guard !all.isEmpty else { throw KLUnpackError.badEntry("no assets/ in this APK") }
        let total = max(1, all.reduce(UInt64(0)) { $0 + $1.csize })

        let partial = dest.deletingLastPathComponent()
                          .appendingPathComponent(dest.lastPathComponent + ".partial")
        try? fm.removeItem(at: partial)
        try fm.createDirectory(at: partial, withIntermediateDirectories: true)

        var done: UInt64 = 0
        for e in all {
            let rel = String(e.name.dropFirst("assets/".count))
            // A name is a path someone else wrote. It stays inside the tree.
            guard !rel.split(separator: "/").contains(".."), !rel.hasPrefix("/") else {
                throw KLUnpackError.badEntry(e.name)
            }
            let out = partial.appendingPathComponent(rel)
            try fm.createDirectory(at: out.deletingLastPathComponent(),
                                   withIntermediateDirectories: true)
            guard fm.createFile(atPath: out.path, contents: nil) else {
                throw KLUnpackError.io("Could not create \(rel).")
            }
            let oh = try FileHandle(forWritingTo: out)
            defer { try? oh.close() }

            // The data starts after the LOCAL header, whose name and extra
            // lengths are its own and need not match the central record's.
            try fh.seek(toOffset: e.lhdr)
            guard let lh = try fh.read(upToCount: 30), lh.count == 30,
                  u32(lh, 0) == 0x0403_4b50 else { throw KLUnpackError.truncated }
            try fh.seek(toOffset: e.lhdr + 30 + UInt64(u16(lh, 26)) + UInt64(u16(lh, 28)))

            switch e.method {
            case 0:
                var left = e.csize
                while left > 0 {
                    let n = Int(min(left, 1 << 20))
                    // A pool per block: FileHandle's Data is autoreleased, and
                    // without one a 1.3 GB tree peaked at 357 MB resident.
                    try autoreleasepool {
                        guard let chunk = try fh.read(upToCount: n), chunk.count == n else {
                            throw KLUnpackError.truncated
                        }
                        try oh.write(contentsOf: chunk)
                    }
                    left -= UInt64(n); done += UInt64(n)
                    progress(Double(done) / Double(total))
                }
            case 8:
                try inflate(from: fh, compressed: e.csize, to: oh, name: rel) { n in
                    done += UInt64(n)
                    progress(Double(done) / Double(total))
                }
            default:
                throw KLUnpackError.badEntry("\(e.name) (compression method \(e.method))")
            }
        }
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: partial, to: dest)
    }

    /// Raw DEFLATE, streamed: an asset can be hundreds of megabytes, and the
    /// headset's memory is the guest's to spend.
    private static func inflate(from fh: FileHandle, compressed: UInt64, to oh: FileHandle,
                                name: String, consumed: (Int) -> Void) throws {
        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { stream.deallocate() }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB)
                == COMPRESSION_STATUS_OK else { throw KLUnpackError.inflate(name) }
        defer { compression_stream_destroy(stream) }

        let outCap = 1 << 20
        let outBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: outCap)
        defer { outBuf.deallocate() }

        var left = compressed
        var finished = false
        while !finished {
            let n = Int(min(left, 1 << 20))
            left -= UInt64(n)
            consumed(n)
            let flags: Int32 = left == 0 ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
            try autoreleasepool {
            let chunk = n > 0 ? (try fh.read(upToCount: n) ?? Data()) : Data()
            guard chunk.count == n else { throw KLUnpackError.truncated }
            try chunk.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                // An empty final chunk still has to be offered with FINALIZE.
                var empty: UInt8 = 0
                stream.pointee.src_ptr = raw.bindMemory(to: UInt8.self).baseAddress
                    ?? UnsafePointer(withUnsafeMutablePointer(to: &empty) { $0 })
                stream.pointee.src_size = n
                // While finalizing, keep calling until the stream SAYS it ended:
                // the call that swallows the last input returns OK, and END only
                // comes from the one after it. Stopping on "no input left" lost
                // every deflated entry (found unpacking the real APK on a Mac).
                // A call that neither consumed nor produced anything is a
                // truncated stream, not a reason to spin.
                repeat {
                    let srcBefore = stream.pointee.src_size
                    stream.pointee.dst_ptr = outBuf
                    stream.pointee.dst_size = outCap
                    let st = compression_stream_process(stream, flags)
                    guard st != COMPRESSION_STATUS_ERROR else { throw KLUnpackError.inflate(name) }
                    let produced = outCap - stream.pointee.dst_size
                    if produced > 0 {
                        try oh.write(contentsOf: Data(bytesNoCopy: outBuf, count: produced,
                                                      deallocator: .none))
                    }
                    if st == COMPRESSION_STATUS_END { finished = true; break }
                    if flags != 0, produced == 0, stream.pointee.src_size == srcBefore {
                        throw KLUnpackError.inflate(name)
                    }
                } while stream.pointee.src_size > 0 || stream.pointee.dst_size == 0 || flags != 0
            }
            }
            if left == 0 && !finished {
                // Input exhausted without the stream saying it ended.
                throw KLUnpackError.inflate(name)
            }
        }
    }
}

// MARK: - Downloading from the store's CDN

enum KLDownloadError: LocalizedError {
    case http(Int), notSignedIn, notABinary, io(String)
    var errorDescription: String? {
        switch self {
        case .http(let c) where c == 401 || c == 403:
            return "The store refused the download (HTTP \(c)). The session may have expired, "
                 + "or this account does not own the game. Sign in again."
        case .http(let c): return "The download failed (HTTP \(c))."
        case .notSignedIn: return "Sign in first."
        case .notABinary:  return "The store answered with something that is not the game file."
        case .io(let m):   return m
        }
    }
}

final class KLDownloader: NSObject, URLSessionDownloadDelegate {
    private var onProgress: ((Int64, Int64) -> Void)?
    private var continuation: CheckedContinuation<(URL, Int), Error>?
    private lazy var session = URLSession(configuration: .ephemeral, delegate: self,
                                          delegateQueue: nil)

    /// One binary, by id, to `dest`. The credential is offered as a bearer
    /// header first; the query spelling — which is the one the endpoint is
    /// known to take — is the fallback, and neither URL is ever logged.
    func fetch(id: String, to dest: URL, token: String,
               progress: @escaping (Int64, Int64) -> Void) async throws {
        var comps = URLComponents(string: "https://securecdn.oculus.com/binaries/download/")!
        comps.queryItems = [URLQueryItem(name: "id", value: id)]
        var req = URLRequest(url: comps.url!)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        var (tmp, code) = try await run(req, progress)
        if code == 400 || code == 401 || code == 403 {
            try? FileManager.default.removeItem(at: tmp)
            comps.queryItems?.append(URLQueryItem(name: "access_token", value: token))
            (tmp, code) = try await run(URLRequest(url: comps.url!), progress)
        }
        defer { try? FileManager.default.removeItem(at: tmp) }
        guard code == 200 else { throw KLDownloadError.http(code) }
        // Both files are zips. An HTML error page served with a 200 is not.
        let fh = try FileHandle(forReadingFrom: tmp)
        let magic = try fh.read(upToCount: 2) ?? Data()
        try? fh.close()
        guard magic == Data([0x50, 0x4b]) else { throw KLDownloadError.notABinary }
        let fm = FileManager.default
        try fm.createDirectory(at: dest.deletingLastPathComponent(),
                               withIntermediateDirectories: true)
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: tmp, to: dest)
    }

    private func run(_ req: URLRequest,
                     _ progress: @escaping (Int64, Int64) -> Void) async throws -> (URL, Int) {
        onProgress = progress
        return try await withCheckedThrowingContinuation { c in
            continuation = c
            session.downloadTask(with: req).resume()
        }
    }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData _: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        onProgress?(totalBytesWritten, totalBytesExpectedToWrite)
    }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        // The file at `location` is deleted when this returns, so it is moved
        // somewhere of ours first.
        let keep = FileManager.default.temporaryDirectory
            .appendingPathComponent("kl-dl-\(UUID().uuidString)")
        let code = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0
        do {
            try FileManager.default.moveItem(at: location, to: keep)
            continuation?.resume(returning: (keep, code))
        } catch {
            continuation?.resume(throwing: KLDownloadError.io(error.localizedDescription))
        }
        continuation = nil
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error, let c = continuation {
            c.resume(throwing: error)
            continuation = nil
        }
    }
}

// MARK: - Signing in

/// The store's developer site, in a web view. The person signs in to their own
/// account on the site's own page; when the site sets its session cookie, that
/// value is the token the CDN takes. Nothing typed here passes through this app.
struct KLMetaLoginView: View {
    var onToken: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var waited = 0

    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text("Sign in to Meta").font(.headline)
                Spacer()
                Button("Cancel") { dismiss() }
            }
            KLWebView(url: URL(string: "https://developers.meta.com/horizon/")!) { token in
                onToken(token)
                dismiss()
            } tick: { waited += 1 }
            Text(waited < 30
                 ? "Sign in on the page above. This closes by itself once the session is found."
                 : "Signed in, but no developer session yet? The account has to be registered "
                 + "as a Meta developer first — do that on this page, then come back here.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .padding(20)
        .frame(minWidth: 900, minHeight: 700)
    }
}

private struct KLWebView: UIViewRepresentable {
    let url: URL
    let found: (String) -> Void
    let tick: () -> Void

    func makeCoordinator() -> Coordinator { Coordinator(found: found, tick: tick) }

    func makeUIView(context: Context) -> WKWebView {
        let web = WKWebView(frame: .zero, configuration: WKWebViewConfiguration())
        context.coordinator.watch(web)
        web.load(URLRequest(url: url))
        return web
    }
    func updateUIView(_: WKWebView, context _: Context) {}
    static func dismantleUIView(_: WKWebView, coordinator: Coordinator) { coordinator.stop() }

    final class Coordinator {
        private let found: (String) -> Void
        private let tick: () -> Void
        private var timer: Timer?
        private var done = false
        init(found: @escaping (String) -> Void, tick: @escaping () -> Void) {
            self.found = found; self.tick = tick
        }
        /// Polled rather than observed: the cookie is set by a redirect chain
        /// that no single navigation callback is the end of.
        func watch(_ web: WKWebView) {
            timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self, weak web] _ in
                guard let self, let web, !self.done else { return }
                self.tick()
                web.configuration.websiteDataStore.httpCookieStore.getAllCookies { cookies in
                    guard !self.done,
                          let c = cookies.first(where: { $0.name == "oc_ac_at"
                                                         && $0.domain.hasSuffix("meta.com") }),
                          c.value.hasPrefix("FRL") || c.value.hasPrefix("OCA") else { return }
                    self.done = true
                    self.stop()
                    self.found(c.value)
                }
            }
        }
        func stop() { timer?.invalidate(); timer = nil }
    }
}

// MARK: - The panel

@MainActor
final class KLGameDataModel: ObservableObject {
    @Published var busy = false
    @Published var step = ""
    @Published var fraction: Double?
    @Published var error: String?
    @Published var signedIn = KLMetaToken.load() != nil
    @Published var revision = 0          // bumped when files change, to re-read the disk

    let files: KLGameFiles
    init(files: KLGameFiles) { self.files = files }

    func tokenFound(_ t: String) {
        KLMetaToken.save(t)
        signedIn = true
        NSLog("[data] signed in (session stored in the Keychain)")
    }

    func signOut() {
        KLMetaToken.clear()
        signedIn = false
    }

    /// Whatever is missing, in order: APK, OBB, then the unpack. Each step is
    /// skipped if its result is already on disk, so this is also "resume".
    func run(onReady: @escaping () -> Void) {
        guard !busy else { return }
        busy = true; error = nil
        Task {
            do {
                if !files.hasAPK {
                    try await download(id: files.apkID, to: files.apkURL, what: "APK")
                }
                if let obb = files.obbURL, !files.hasOBB {
                    try await download(id: files.obbID, to: obb, what: "OBB")
                }
                try await unpack()
                step = "Ready."; fraction = nil; busy = false; revision += 1
                NSLog("[data] game data complete")
                onReady()
            } catch {
                self.error = error.localizedDescription
                if case KLDownloadError.http(let c) = error, c == 401 || c == 403 { signOut() }
                NSLog("[data] failed: \(error.localizedDescription)")
                step = ""; fraction = nil; busy = false; revision += 1
            }
        }
    }

    private func download(id: String, to dest: URL, what: String) async throws {
        guard !id.isEmpty else {
            throw KLDownloadError.io("This build has no download id for the \(what). Copy the "
                                     + "file into this app's folder in Files instead.")
        }
        guard let token = KLMetaToken.load() else { throw KLDownloadError.notSignedIn }
        step = "Downloading the \(what)…"; fraction = 0
        NSLog("[data] downloading the \(what)")
        try await KLDownloader().fetch(id: id, to: dest, token: token) { got, total in
            Task { @MainActor in
                self.fraction = total > 0 ? Double(got) / Double(total) : nil
                self.step = "Downloading the \(what)… "
                    + ByteCountFormatter.string(fromByteCount: got, countStyle: .file)
            }
        }
        revision += 1
    }

    private func unpack() async throws {
        if !files.hasAssets {
            step = "Unpacking the game's assets…"; fraction = 0
            NSLog("[data] unpacking assets")
            let f = files
            try await Task.detached(priority: .userInitiated) {
                try KLApkUnpacker.unpackAssets(apk: f.apkURL, to: f.assetsURL) { p in
                    Task { @MainActor in self.fraction = p }
                }
            }.value
        }
        // The two files apktool decodes and the device cannot: carried in the
        // bundle as data assets, because they describe the APK these libraries
        // were built from and no other.
        for (asset, name) in [("KLMeta-apktool", "apktool.yml"),
                              ("KLMeta-manifest", "AndroidManifest.xml")] {
            let dst = files.treeURL.appendingPathComponent(name)
            if !FileManager.default.fileExists(atPath: dst.path),
               let d = NSDataAsset(name: asset)?.data {
                try d.write(to: dst)
            }
        }
    }
}

struct GameDataView: View {
    @StateObject private var model: KLGameDataModel
    @State private var showLogin = false
    let onReady: () -> Void

    init(files: KLGameFiles, onReady: @escaping () -> Void) {
        _model = StateObject(wrappedValue: KLGameDataModel(files: files))
        self.onReady = onReady
    }

    private var f: KLGameFiles { model.files }
    private var needsDownload: Bool { !f.hasAPK || !f.hasOBB }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Game data").font(.headline)
            Text("The game's files are not on this device yet.")
                .font(.callout).foregroundStyle(.secondary)

            row("APK", f.apkName, f.hasAPK ? KLGameFiles.size(f.apkURL) : nil)
            row("Assets", "unpacked from the APK", f.hasAssets ? "unpacked" : nil)
            if let obb = f.obbURL {
                row("OBB", f.obbFile, f.hasOBB ? KLGameFiles.size(obb) : nil)
            }

            if model.busy {
                if let p = model.fraction { ProgressView(value: p) } else { ProgressView() }
                Text(model.step).font(.caption).foregroundStyle(.secondary)
            }
            if let e = model.error {
                Text(e).font(.caption).foregroundStyle(.red)
            }

            HStack(spacing: 12) {
                if needsDownload && f.canDownload && !model.signedIn {
                    Button("Sign in to Meta") { showLogin = true }
                } else {
                    Button(needsDownload ? "Download and unpack" : "Unpack") {
                        model.run(onReady: onReady)
                    }
                    .disabled(model.busy || (needsDownload && !f.canDownload))
                }
                if model.signedIn {
                    Button("Sign out") { model.signOut() }.disabled(model.busy)
                }
            }
            if needsDownload && !f.canDownload {
                Text("This build has no download ids. Copy \(f.apkName)"
                     + (f.obbFile.isEmpty ? "" : " and \(f.obbFile) (into android-files/\(f.obbRel)/)")
                     + " into this app's folder in Files, then reopen the app.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .id(model.revision)
        .sheet(isPresented: $showLogin) {
            KLMetaLoginView { model.tokenFound($0) }
        }
    }

    private func row(_ title: String, _ detail: String, _ have: String?) -> some View {
        HStack {
            Image(systemName: have != nil ? "checkmark.circle.fill" : "circle.dashed")
                .foregroundStyle(have != nil ? Color.green : Color.secondary)
            Text(title).frame(width: 70, alignment: .leading)
            Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            Spacer()
            Text(have ?? "missing").font(.caption)
                .foregroundStyle(have != nil ? Color.secondary : Color.orange)
        }
    }
}
