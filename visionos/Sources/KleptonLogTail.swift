import Foundation

/// Bound the start-window preview independently of the complete diagnostic file.
/// A previous run can leave megabytes of output; laying all of it out on the
/// main thread can exhaust visionOS's scene-creation watchdog allowance.
enum KleptonLogTail {
    static let byteLimit = 16 * 1024

    /// Only a build with the Beat Saber mods bounds the preview for now; every
    /// other game keeps reading the whole file, as before.
    static let bounded = Bundle.main.object(forInfoDictionaryKey: "KLScotland2Test") as? String == "1"

    static func read(_ path: String) -> String {
        guard bounded else { return (try? String(contentsOfFile: path, encoding: .utf8)) ?? "" }
        do {
            let file = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
            defer { try? file.close() }
            let end = try file.seekToEnd()
            let start = end > UInt64(byteLimit) ? end - UInt64(byteLimit) : 0
            try file.seek(toOffset: start)
            let data = try file.read(upToCount: byteLimit) ?? Data()
            var text = String(decoding: data, as: UTF8.self)
            guard start > 0 else { return text }
            // The first bytes can belong to a partial line or UTF-8 scalar.
            if let newline = text.firstIndex(of: "\n") {
                text.removeSubrange(...newline)
            }
            return "[Recent output shown; the complete log remains in app storage.]\n" + text
        } catch {
            return ""
        }
    }
}
