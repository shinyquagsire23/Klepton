import CommonCrypto
import Foundation
import zlib

/// Hashes and zlib, from the system's own libraries. (Glovebox's kit has these in a C target of its own, beside the
/// decoders its depot downloader needs; none of that is here.)
public enum Codec {
    public static func sha1(_ d: Data) -> Data {
        var out = [UInt8](repeating: 0, count: Int(CC_SHA1_DIGEST_LENGTH))
        d.withUnsafeBytes { _ = CC_SHA1($0.baseAddress, CC_LONG(d.count), &out) }
        return Data(out)
    }

    public static func crc32(_ d: Data) -> UInt32 {
        d.withUnsafeBytes { UInt32(zlib.crc32(0, $0.bindMemory(to: UInt8.self).baseAddress, uInt(d.count))) }
    }

    /// zlib's inflate into a buffer of the size the sender announced. `windowBits` as zlib takes it: 31 for gzip,
    /// -15 for a raw deflate stream.
    public static func inflate(_ d: Data, windowBits: Int32, expected: Int) throws -> Data {
        guard expected >= 0, expected <= 256 << 20 else { throw SteamError.badData("inflate: implausible size") }
        var out = Data(count: max(expected, 1))
        var stream = z_stream()
        guard inflateInit2_(&stream, windowBits, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw SteamError.badData("inflate failed")
        }
        defer { inflateEnd(&stream) }
        let capacity = out.count
        let (rc, produced): (Int32, Int) = d.withUnsafeBytes { i in
            out.withUnsafeMutableBytes { o in
                stream.next_in = UnsafeMutablePointer(mutating: i.bindMemory(to: UInt8.self).baseAddress)
                stream.avail_in = uInt(d.count)
                stream.next_out = o.bindMemory(to: UInt8.self).baseAddress
                stream.avail_out = uInt(capacity)
                let rc = zlib.inflate(&stream, Z_FINISH)
                return (rc, capacity - Int(stream.avail_out))
            }
        }
        guard rc == Z_STREAM_END, produced <= expected else { throw SteamError.badData("inflate failed") }
        return out.prefix(produced)
    }

    public static func gunzip(_ d: Data, expected: Int) throws -> Data { try inflate(d, windowBits: 31, expected: expected) }

    private static func le32(_ d: Data, _ at: Int) -> UInt32 {
        let i = d.startIndex + at
        return UInt32(d[i]) | UInt32(d[i + 1]) << 8 | UInt32(d[i + 2]) << 16 | UInt32(d[i + 3]) << 24
    }

    /// Extracts the only file of a zip archive (how Steam Cloud hands out a file it stored compressed).
    public static func unzipSingle(_ d: Data) throws -> Data {
        // Use the central directory: local headers may defer sizes to a data descriptor.
        guard d.count >= 22 else { throw SteamError.badData("zip too short") }
        var eocd = -1
        var i = d.count - 22
        while i >= max(0, d.count - 22 - 65535) {
            if le32(d, i) == 0x0605_4B50 { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { throw SteamError.badData("zip without end record") }
        let cdOffset = Int(le32(d, eocd + 16))
        guard cdOffset >= 0, cdOffset + 46 <= d.count, le32(d, cdOffset) == 0x0201_4B50 else { throw SteamError.badData("zip central directory") }
        func le16(_ at: Int) -> Int { Int(d[d.startIndex + at]) | Int(d[d.startIndex + at + 1]) << 8 }
        let method = le16(cdOffset + 10)
        let compSize = Int(le32(d, cdOffset + 20))
        let size = Int(le32(d, cdOffset + 24))
        let local = Int(le32(d, cdOffset + 42))
        guard local >= 0, local + 30 <= d.count else { throw SteamError.badData("zip local header") }
        let dataStart = local + 30 + le16(local + 26) + le16(local + 28)
        guard dataStart + compSize <= d.count else { throw SteamError.badData("zip data") }
        let comp = d.subdata(in: (d.startIndex + dataStart)..<(d.startIndex + dataStart + compSize))
        switch method {
        case 0: return comp
        case 8: return try inflate(comp, windowBits: -15, expected: size)
        default: throw SteamError.badData("zip method \(method)")
        }
    }
}

public extension Data {
    var hex: String { map { String(format: "%02x", $0) }.joined() }
    init?(hex: String) {
        var out = Data(capacity: hex.count / 2)
        var it = hex.makeIterator()
        while let a = it.next() {
            guard let b = it.next(), let v = UInt8(String([a, b]), radix: 16) else { return nil }
            out.append(v)
        }
        self = out
    }
}
