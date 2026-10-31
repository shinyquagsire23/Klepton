import Foundation

@main
struct LogTailTests {
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("sc2-logtail-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("boot.log")
        assert(KleptonLogTail.read(file.path).isEmpty)
        try Data().write(to: file)
        assert(KleptonLogTail.read(file.path).isEmpty)
        let short = "startup\n漢 😀\nfinished\n"
        try Data(short.utf8).write(to: file)
        assert(KleptonLogTail.read(file.path) == short)

        let long = String(repeating: "old output\n", count: 100_000) + short
        let original = Data(long.utf8)
        try original.write(to: file)
        let tail = KleptonLogTail.read(file.path)
        assert(tail.hasPrefix("[Recent output shown;"))
        assert(tail.hasSuffix(short))
        assert(tail.utf8.count <= KleptonLogTail.byteLimit + 100)
        let retained = try Data(contentsOf: file)
        assert(retained == original)

        // Large/sparse files must be sought, not allocated or decoded in full.
        let handle = try FileHandle(forWritingTo: file)
        try handle.truncate(atOffset: 0)
        try handle.seek(toOffset: 1 << 30)
        try handle.write(contentsOf: Data(("\n" + short).utf8))
        try handle.close()
        let sparseTail = KleptonLogTail.read(file.path)
        assert(sparseTail.hasSuffix(short))
        assert(sparseTail.utf8.count <= KleptonLogTail.byteLimit + 100)

        // Partial writes and split/invalid UTF-8 stay bounded and readable.
        try Data(repeating: 0xff, count: KleptonLogTail.byteLimit + 1).write(to: file)
        let invalid = KleptonLogTail.read(file.path)
        assert(!invalid.isEmpty)
        assert(invalid.utf8.count <= KleptonLogTail.byteLimit * 3 + 100)
        print("PASS log preview: short/empty/missing, long-file preservation, 1 GiB sparse seek and invalid UTF-8 bounds")
    }
}
