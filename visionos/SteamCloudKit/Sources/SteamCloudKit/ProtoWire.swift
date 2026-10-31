import Foundation

/// A minimal protobuf wire-format reader and writer.
///
/// Steam speaks protobuf everywhere, but the launcher only needs ~30 messages, so each one is
/// written by hand against the field numbers in SteamDatabase/Protobufs rather than pulling in
/// a code generator. Unknown fields are skipped, as protobuf requires.
public struct ProtoWriter {
    public private(set) var data = Data()

    public init() {}

    public mutating func varint(_ value: UInt64) {
        var v = value
        while v >= 0x80 {
            data.append(UInt8(v & 0x7F) | 0x80)
            v >>= 7
        }
        data.append(UInt8(v))
    }

    private mutating func tag(_ field: Int, _ wire: Int) { varint(UInt64(field << 3 | wire)) }

    public mutating func uint(_ field: Int, _ value: UInt64?) {
        guard let value else { return }
        tag(field, 0); varint(value)
    }
    public mutating func uint(_ field: Int, _ value: UInt32?) { uint(field, value.map(UInt64.init)) }
    public mutating func int(_ field: Int, _ value: Int32?) {
        guard let value else { return }
        tag(field, 0); varint(UInt64(bitPattern: Int64(value)))
    }
    public mutating func bool(_ field: Int, _ value: Bool?) { uint(field, value.map { $0 ? UInt64(1) : 0 }) }

    public mutating func fixed64(_ field: Int, _ value: UInt64?) {
        guard let value else { return }
        tag(field, 1)
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }
    public mutating func fixed32(_ field: Int, _ value: UInt32?) {
        guard let value else { return }
        tag(field, 5)
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    public mutating func bytes(_ field: Int, _ value: Data?) {
        guard let value else { return }
        tag(field, 2); varint(UInt64(value.count)); data.append(value)
    }
    public mutating func string(_ field: Int, _ value: String?) { bytes(field, value.map { Data($0.utf8) }) }
    public mutating func message(_ field: Int, _ value: ProtoEncodable?) { bytes(field, value?.encoded()) }
}

public protocol ProtoEncodable {
    func encode(into w: inout ProtoWriter)
}

public extension ProtoEncodable {
    func encoded() -> Data {
        var w = ProtoWriter()
        encode(into: &w)
        return w.data
    }
}

public protocol ProtoDecodable {
    init(proto: Data) throws
}

public enum ProtoError: Error, CustomStringConvertible {
    case truncated
    case badWireType(Int)
    public var description: String {
        switch self {
        case .truncated: return "protobuf message ended early"
        case .badWireType(let w): return "unsupported protobuf wire type \(w)"
        }
    }
}

/// One decoded field. Values stay raw until a message asks for them as a type.
public struct ProtoField {
    public let number: Int
    public let wire: Int
    public let raw: UInt64      // varint, fixed32, fixed64
    public let bytes: Data      // length-delimited

    public var uint64: UInt64 { raw }
    public var uint32: UInt32 { UInt32(truncatingIfNeeded: raw) }
    public var int32: Int32 { Int32(truncatingIfNeeded: Int64(bitPattern: raw)) }
    public var bool: Bool { raw != 0 }
    public var float: Float { Float(bitPattern: UInt32(truncatingIfNeeded: raw)) }
    public var string: String { String(decoding: bytes, as: UTF8.self) }
}

public struct ProtoReader {
    private let data: Data
    private var i: Data.Index

    public init(_ data: Data) {
        self.data = data
        self.i = data.startIndex
    }

    private mutating func varint() throws -> UInt64 {
        var result: UInt64 = 0
        var shift: UInt64 = 0
        while true {
            guard i < data.endIndex else { throw ProtoError.truncated }
            let b = data[i]; i += 1
            result |= UInt64(b & 0x7F) << shift
            if b < 0x80 { return result }
            shift += 7
            if shift > 63 { throw ProtoError.truncated }
        }
    }

    private mutating func fixed(_ n: Int) throws -> UInt64 {
        guard data.endIndex - i >= n else { throw ProtoError.truncated }
        var v: UInt64 = 0
        for k in 0..<n { v |= UInt64(data[i + k]) << (8 * UInt64(k)) }
        i += n
        return v
    }

    public mutating func next() throws -> ProtoField? {
        guard i < data.endIndex else { return nil }
        let key = try varint()
        let number = Int(key >> 3), wire = Int(key & 7)
        switch wire {
        case 0: return ProtoField(number: number, wire: wire, raw: try varint(), bytes: Data())
        case 1: return ProtoField(number: number, wire: wire, raw: try fixed(8), bytes: Data())
        case 5: return ProtoField(number: number, wire: wire, raw: try fixed(4), bytes: Data())
        case 2:
            let len = Int(try varint())
            guard len >= 0, data.endIndex - i >= len else { throw ProtoError.truncated }
            let slice = data.subdata(in: i..<(i + len))
            i += len
            return ProtoField(number: number, wire: wire, raw: 0, bytes: slice)
        default:
            throw ProtoError.badWireType(wire)
        }
    }

    /// Calls `body` for every field; the common shape of a message decoder.
    public static func fields(_ data: Data, _ body: (ProtoField) throws -> Void) throws {
        var r = ProtoReader(data)
        while let f = try r.next() { try body(f) }
    }

    /// Packed or unpacked repeated uint32.
    public static func repeatedUInt32(_ f: ProtoField) throws -> [UInt32] {
        if f.wire == 0 { return [f.uint32] }
        var out: [UInt32] = []
        var r = ProtoReader(f.bytes)
        while r.i < r.data.endIndex { out.append(UInt32(truncatingIfNeeded: try r.varint())) }
        return out
    }
}
