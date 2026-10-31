import Foundation

/// Valve KeyValues, the format of PICS app and package info.
/// App info arrives as text VDF; package info as binary KeyValues.
public indirect enum KV: CustomStringConvertible {
    case string(String)
    case int(Int64)
    case float(Float)
    case object([(String, KV)])

    public subscript(_ key: String) -> KV? {
        guard case .object(let items) = self else { return nil }
        return items.first { $0.0.caseInsensitiveCompare(key) == .orderedSame }?.1
    }

    public subscript(path path: String...) -> KV? {
        var cur: KV? = self
        for p in path { cur = cur?[p] }
        return cur
    }

    public var children: [(String, KV)] {
        if case .object(let items) = self { return items }
        return []
    }

    public var string: String? {
        switch self {
        case .string(let s): return s
        case .int(let i): return String(i)
        case .float(let f): return String(f)
        case .object: return nil
        }
    }

    public var int: Int64? {
        switch self {
        case .int(let i): return i
        case .string(let s): return Int64(s)
        default: return nil
        }
    }

    public var uint64: UInt64? {
        if case .int(let i) = self { return UInt64(bitPattern: i) }
        return string.flatMap(UInt64.init)
    }

    public var description: String { render(indent: 0) }

    private func render(indent: Int) -> String {
        let pad = String(repeating: "\t", count: indent)
        switch self {
        case .object(let items):
            return items.map { k, v in
                if case .object = v { return "\(pad)\"\(k)\"\n\(pad){\n\(v.render(indent: indent + 1))\(pad)}\n" }
                return "\(pad)\"\(k)\"\t\t\"\(v.string ?? "")\"\n"
            }.joined()
        default: return string ?? ""
        }
    }

    // MARK: Text

    public static func parseText(_ data: Data) throws -> KV {
        var bytes = [UInt8](data)
        while bytes.last == 0 { bytes.removeLast() }
        var i = 0
        func skip() {
            while i < bytes.count {
                let c = bytes[i]
                if c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D { i += 1; continue }
                if c == 0x2F, i + 1 < bytes.count, bytes[i + 1] == 0x2F {
                    while i < bytes.count, bytes[i] != 0x0A { i += 1 }
                    continue
                }
                break
            }
        }
        func token() throws -> String? {
            skip()
            guard i < bytes.count else { return nil }
            if bytes[i] == 0x22 {
                i += 1
                var out = [UInt8]()
                while i < bytes.count, bytes[i] != 0x22 {
                    if bytes[i] == 0x5C, i + 1 < bytes.count {
                        i += 1
                        switch bytes[i] {
                        case 0x6E: out.append(0x0A)
                        case 0x74: out.append(0x09)
                        default: out.append(bytes[i])
                        }
                    } else {
                        out.append(bytes[i])
                    }
                    i += 1
                }
                i += 1
                return String(decoding: out, as: UTF8.self)
            }
            let start = i
            while i < bytes.count, !(bytes[i] == 0x20 || bytes[i] == 0x09 || bytes[i] == 0x0A || bytes[i] == 0x0D || bytes[i] == 0x7B || bytes[i] == 0x7D) { i += 1 }
            return String(decoding: bytes[start..<i], as: UTF8.self)
        }
        func object() throws -> [(String, KV)] {
            var items: [(String, KV)] = []
            while true {
                skip()
                guard i < bytes.count else { return items }
                if bytes[i] == 0x7D { i += 1; return items }
                guard let key = try token() else { return items }
                skip()
                if i < bytes.count, bytes[i] == 0x7B {
                    i += 1
                    items.append((key, .object(try object())))
                } else {
                    items.append((key, .string(try token() ?? "")))
                }
            }
        }
        return .object(try object())
    }

    // MARK: Binary

    public static func parseBinary(_ data: Data) throws -> KV {
        let b = [UInt8](data)
        var i = 0
        func cstring() throws -> String {
            let start = i
            while i < b.count, b[i] != 0 { i += 1 }
            guard i < b.count else { throw SteamError.badData("binary KV string") }
            defer { i += 1 }
            return String(decoding: b[start..<i], as: UTF8.self)
        }
        func le(_ n: Int) throws -> UInt64 {
            guard i + n <= b.count else { throw SteamError.badData("binary KV number") }
            var v: UInt64 = 0
            for k in 0..<n { v |= UInt64(b[i + k]) << (8 * UInt64(k)) }
            i += n
            return v
        }
        func object() throws -> [(String, KV)] {
            var items: [(String, KV)] = []
            while i < b.count {
                let type = b[i]; i += 1
                if type == 0x08 || type == 0x0B { return items }  // end
                let key = try cstring()
                switch type {
                case 0x00: items.append((key, .object(try object())))
                case 0x01: items.append((key, .string(try cstring())))
                case 0x02, 0x04, 0x06: items.append((key, .int(Int64(Int32(truncatingIfNeeded: try le(4))))))
                case 0x03: items.append((key, .float(Float(bitPattern: UInt32(try le(4))))))
                case 0x07: items.append((key, .int(Int64(bitPattern: try le(8)))))
                case 0x0A: items.append((key, .int(Int64(bitPattern: try le(8)))))
                default: throw SteamError.badData("binary KV type \(type)")
                }
            }
            return items
        }
        return .object(try object())
    }
}
