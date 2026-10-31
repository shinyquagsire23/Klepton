// Walkabout Mini Golf: every lost ball and every earned putter of the courses
// the player has, and the earned cosmetics of the packs they bought, written
// into the game's own save.
//
// Lost balls are found by playing — one hidden on each of a course's 18 holes —
// and a hard course's putter by solving its Foxhunt, or on Homestar Runner, which
// has none, in its activities. Nothing here is a purchase: a course sold
// separately counts only when the save says it was bought, and the putters a
// Putter Pass or a promotion gives are not in the tables below at all.
//
// Where the game keeps it (Walkabout 6.8, read from the game itself on the Mac,
// 2026-10-17). The game has two saves and since 6.x reads the newer one:
// android-files/files/Data/<player>/MC*.json, one file per kind of record, each
// the record's JSON encrypted with AES-256-CBC (PKCS7) and written as Base64.
// The record that counts for a profile p:
//   MCd65ADD<p>   avatar data: "ballsFound" (ball ids) and "puttersFound"
//   MC541GgGd<p>  golf data per course: "ballsFound" is how many of its 18 balls
//   MC523GPLD<p>  location data per course: "isPurchased" for a course sold apart
//   MC124GCOSD<p> cosmetics: "isPurchased" and "isUnlocked" per cosmetic. A few
//                 of a bought pack are earned in play on top (unlockMode
//                 OnMilestone); `earned` names those, and only a bought one is
//                 unlocked here.
//   MCd58PS<p>    player stats: "totalBallsUnlocked", how many found balls the
//                 ball tray has already rolled in. The tray drops every ball
//                 above that count down its pipe one at a time, which for some
//                 seven hundred balls is minutes of physics in the main menu
//                 (seen on the Mac); it is raised by as many as were added.
// Every record carries a "version"; when the game merges this save with the
// copy in its cloud the higher version wins (MergeType UseLatest), and it raises
// the version by one on every save. So does this, and the game then takes the
// unlocked state as the newer one and uploads it. The older save
// (Profiles/<platform>/<id>/Profile_Default.data) is left alone: the game only
// merged it into the new one once.
//
// The key and the IV are the game's, not ours: two text constants in its
// global-metadata.dat. They are found there by their SHA-256, in the copy of the
// game the player installed, so the game's constants are never part of this
// source. The tables were read from the course catalogue of the same build
// (CourseInfoAsset.LostBallIds and puttersData, and the activities' putters); a
// later course is simply not unlocked until the tables learn it.
import CommonCrypto
import Foundation

enum KLWalkaboutUnlock {
    // MARK: - What the game has

    static let courses: [(loc: String, dlc: Bool, balls: [Int])] = [
        ("TTE", false, [13, 81, 41, 29, 75, 59, 19, 49, 79, 24, 68, 37, 78, 30, 54, 15, 45, 71]),
        ("CBE", false, [9, 46, 25, 38, 69, 31, 72, 14, 55, 17, 61, 60, 11, 42, 23, 51, 67, 34]),
        ("QVE", false, [319, 229, 354, 280, 274, 118, 108, 185, 124, 311, 393, 266, 238, 178, 415, 133, 167, 386]),
        ("STE", false, [32, 64, 73, 10, 62, 47, 26, 80, 21, 65, 39, 43, 18, 56, 35, 76, 22, 52]),
        ("AME", false, [12, 53, 58, 36, 28, 66, 57, 74, 44, 70, 27, 48, 33, 40, 63, 16, 77, 20]),
        ("OGE", false, [92, 91, 90, 89, 88, 87, 86, 85, 84, 82, 83, 93, 94, 95, 96, 97, 98, 99]),
        ("BBE", false, [378, 106, 293, 134, 381, 187, 150, 356, 127, 270, 335, 318, 101, 235, 346, 227, 271, 125]),
        ("TSE", false, [286, 123, 105, 275, 220, 186, 308, 146, 126, 223, 151, 180, 273, 198, 276, 193, 135, 300]),
        ("SLE", false, [291, 406, 175, 401, 408, 278, 434, 272, 141, 113, 407, 234, 409, 173, 410, 8, 432, 145]),
        ("GBE", true, [279, 396, 192, 230, 384, 137, 418, 383, 131, 414, 203, 397, 140, 382, 324, 333, 375, 385]),
        ("SWE", false, [122, 428, 196, 421, 139, 425, 352, 426, 231, 128, 157, 168, 439, 427, 191, 420, 358, 50]),
        ("EDE", true, [470, 474, 476, 472, 322, 473, 357, 283, 355, 258, 194, 365, 232, 477, 422, 340, 475, 104]),
        ("LBE", true, [451, 446, 430, 449, 485, 483, 448, 482, 484, 444, 233, 447, 452, 453, 443, 445, 334, 450]),
        ("2LE", false, [458, 456, 165, 459, 251, 457, 338, 144, 316, 461, 204, 436, 147, 467, 282, 154, 455, 310]),
        ("MYE", true, [487, 492, 493, 495, 496, 503, 494, 501, 502, 489, 490, 498, 491, 504, 497, 499, 488, 500]),
        ("ATE", true, [507, 148, 176, 350, 486, 240, 505, 509, 508, 226, 312, 405, 130, 277, 395, 199, 466, 506]),
        ("UTE", false, [512, 201, 516, 518, 174, 160, 404, 138, 177, 479, 515, 510, 398, 517, 511, 303, 513, 514]),
        ("ZZE", true, [465, 387, 103, 169, 353, 241, 463, 462, 481, 468, 179, 306, 480, 399, 471, 464, 115, 478]),
        ("JCE", true, [526, 149, 527, 522, 528, 345, 520, 129, 523, 164, 524, 195, 281, 525, 429, 521, 172, 519]),
        ("LLE", false, [535, 531, 534, 532, 170, 530, 388, 296, 536, 228, 537, 361, 529, 538, 344, 189, 533, 539]),
        ("ALE", false, [547, 546, 542, 325, 549, 424, 541, 419, 545, 100, 544, 543, 315, 540, 548, 200, 114, 153]),
        ("WWE", true, [556, 560, 171, 305, 564, 552, 558, 561, 423, 559, 551, 565, 562, 557, 555, 554, 553, 563]),
        ("MWE", true, [569, 568, 572, 583, 582, 579, 575, 570, 585, 577, 567, 574, 576, 584, 573, 581, 571, 580]),
        ("AWE", true, [593, 599, 595, 590, 591, 598, 156, 587, 589, 586, 592, 596, 600, 117, 588, 594, 601, 597]),
        ("ILE", true, [603, 608, 605, 431, 602, 337, 371, 609, 604, 610, 607, 402, 435, 438, 606, 184, 267, 166]),
        ("VNE", true, [619, 614, 611, 620, 617, 155, 566, 331, 454, 211, 612, 618, 613, 412, 317, 616, 202, 615]),
        ("WGE", true, [621, 635, 634, 627, 638, 633, 624, 632, 629, 625, 626, 630, 637, 622, 636, 631, 628, 623]),
        ("8BE", true, [656, 655, 663, 662, 658, 667, 659, 660, 669, 666, 668, 653, 652, 665, 654, 657, 664, 661]),
        ("HHE", true, [671, 674, 309, 673, 132, 670, 675, 550, 677, 216, 676, 121, 678, 672, 441, 237, 321, 369]),
        ("ELE", true, [696, 679, 693, 684, 688, 691, 685, 681, 683, 695, 680, 689, 694, 687, 690, 692, 686, 682]),
        ("MGE", true, [651, 640, 643, 649, 645, 650, 641, 644, 403, 647, 433, 197, 646, 392, 642, 330, 639, 648]),
        ("MOE", true, [712, 698, 703, 714, 701, 716, 460, 702, 704, 697, 183, 368, 713, 700, 699, 715, 705, 206]),
        ("RCE", true, [724, 719, 442, 725, 732, 729, 734, 718, 723, 730, 720, 728, 727, 722, 726, 731, 721, 733]),
        ("CLE", true, [737, 742, 367, 739, 744, 136, 741, 736, 740, 747, 743, 735, 748, 746, 745, 717, 329, 738]),
        ("TOE", true, [755, 752, 750, 756, 757, 760, 754, 764, 753, 765, 758, 759, 761, 762, 763, 749, 751, 766]),
        ("FFE", true, [767, 768, 773, 779, 782, 780, 777, 784, 778, 781, 776, 775, 771, 772, 769, 783, 770, 774]),
        ("WOE", true, [796, 795, 785, 789, 798, 788, 794, 791, 797, 801, 793, 802, 790, 799, 786, 792, 787, 800]),
        ("TCE", true, [818, 817, 808, 814, 805, 811, 816, 804, 810, 806, 815, 236, 809, 807, 813, 391, 803, 812]),
        ("HWE", true, [827, 821, 835, 820, 834, 828, 836, 833, 825, 826, 829, 822, 831, 830, 832, 824, 819, 823]),
        ("BHE", true, [851, 842, 850, 839, 854, 844, 849, 838, 846, 848, 852, 841, 843, 847, 840, 845, 853, 837]),
        ("HRH", true, [872, 871, 870, 869, 868, 867, 866, 865, 864, 863, 862, 861, 860, 859, 858, 857, 856, 855]),
        ("GLE", true, [873, 874, 875, 876, 877, 886, 879, 887, 885, 880, 878, 882, 883, 884, 888, 881, 889, 890]),
        ("PXE", true, [897, 896, 903, 904, 437, 902, 400, 900, 898, 894, 901, 893, 341, 899, 895, 336, 892, 891]),
    ]
    /// The putters of a course. HRH has no Foxhunt: its three are activity awards
    /// (50 Dangeresque Mystery, 51 Trogdor Wave 5, 52 Duck Pond Wave 4).
    static let foxhunts: [(loc: String, easy: String, dlc: Bool, putters: [Int])] = [
        ("TTH", "TTE", false, [7]),
        ("CBH", "CBE", false, [8]),
        ("QVH", "QVE", false, [14]),
        ("STH", "STE", false, [9]),
        ("AMH", "AME", false, [10]),
        ("OGH", "OGE", false, [11]),
        ("BBH", "BBE", false, [13]),
        ("TSH", "TSE", false, [12]),
        ("SLH", "SLE", false, [16]),
        ("GBH", "GBE", true, [15]),
        ("SWH", "SWE", false, [17]),
        ("EDH", "EDE", true, [18]),
        ("LBH", "LBE", true, [19]),
        ("2LH", "2LE", false, [20]),
        ("MYH", "MYE", true, [23, 22, 21]),
        ("ATH", "ATE", true, [24]),
        ("UTH", "UTE", false, [25]),
        ("ZZH", "ZZE", true, [26]),
        ("JCH", "JCE", true, [27]),
        ("LLH", "LLE", false, [29, 28]),
        ("ALH", "ALE", false, [30]),
        ("WWH", "WWE", true, [31]),
        ("MWH", "MWE", true, [32]),
        ("AWH", "AWE", true, [33]),
        ("ILH", "ILE", true, [34]),
        ("VNH", "VNE", true, [35]),
        ("WGH", "WGE", true, [36]),
        ("8BH", "8BE", true, [38]),
        ("HHH", "HHE", true, [39]),
        ("ELH", "ELE", true, [40]),
        ("MGH", "MGE", true, [37]),
        ("MOH", "MOE", true, [41]),
        ("RCH", "RCE", true, [42]),
        ("CLH", "CLE", true, [43]),
        ("TOH", "TOE", true, [44]),
        ("FFH", "FFE", true, [45]),
        ("WOH", "WOE", true, [46]),
        ("TCH", "TCE", true, [47]),
        ("HWH", "HWE", true, [48]),
        ("BHH", "BHE", true, [49]),
        ("HRH", "HRE", true, [50, 51, 52]),
        ("GLH", "GLE", true, [53]),
        ("PXH", "PXE", true, [54]),
    ]

    /// Cosmetics of a bought pack that are earned in play: the Homestar Runner
    /// hole celebrations (Strong Bad Zone wave 6; all Post-It hidden scenes).
    /// Every other cosmetic of the catalogue is unlocked by the purchase itself,
    /// apart from the mailing-list ball trail, which is a sign-up, not play.
    static let earned = ["HSR_Homestar_HC", "HSR_Everybody_HC"]

    // MARK: - Finding the save

    struct Profile {
        let dir: URL        // files/Data/<player>
        let index: Int      // the p in MCd65ADD<p>
        var avatar: URL { dir.appendingPathComponent("MCd65ADD\(index).json") }
        var golf: URL { dir.appendingPathComponent("MC541GgGd\(index).json") }
        var locations: URL { dir.appendingPathComponent("MC523GPLD\(index).json") }
        var stats: URL { dir.appendingPathComponent("MCd58PS\(index).json") }
        var cosmetics: URL { dir.appendingPathComponent("MC124GCOSD\(index).json") }
    }

    enum Failure: Error, CustomStringConvertible {
        case noSave
        case noKey(String)
        case unreadable(String)
        var description: String {
            switch self {
            case .noSave: return "no save of the game yet"
            case .noKey(let s): return "the game's save key was not found (\(s))"
            case .unreadable(let s): return "a save file could not be read (\(s))"
            }
        }
    }

    /// Every profile slot the game has written its avatar, golf and location
    /// records for. The game writes all six slots on its first start; the ones
    /// never played hold its defaults and are skipped by unlockAll.
    /// `filesDir` is android-files/files.
    static func profiles(filesDir: URL) -> [Profile] {
        let fm = FileManager.default
        let data = filesDir.appendingPathComponent("Data")
        guard let players = try? fm.contentsOfDirectory(at: data, includingPropertiesForKeys: nil) else {
            return []
        }
        var out: [Profile] = []
        for dir in players.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            for p in 0..<6 {
                let prof = Profile(dir: dir, index: p)
                guard [prof.avatar, prof.golf, prof.locations].allSatisfy({ fm.fileExists(atPath: $0.path) }),
                      let raw = try? Data(contentsOf: prof.avatar), !raw.isEmpty else { continue }
                out.append(prof)
            }
        }
        return out
    }

    /// True when there is a save to change. Cheap: only looks at file names.
    static func hasSave(filesDir: URL) -> Bool { !profiles(filesDir: filesDir).isEmpty }

    // MARK: - The game's key

    struct Key { let key: Data; let iv: Data }

    // Two fingerprints each: a 64-bit polynomial hash that can be rolled along
    // the file one byte at a time, and SHA-256 to confirm a window it points at.
    private static let keyRoll: UInt64 = 0x6eac66b11f727dd4
    private static let keySHA = "89e0e9c7875bce0e7d4d62e9d6ab4cf2efe272bbb1edf617cb1be761f5fd09ea"
    private static let ivRoll: UInt64 = 0xcdbf7ff35e10aca0
    private static let ivSHA = "d88cdfce45f284f77c93a5a52d6a224d25e1c66e398e67e0d3362ffc4d1b66ad"

    /// The two constants, out of the game's global-metadata.dat, in one pass
    /// over the file each.
    static func key(metadata: URL) throws -> Key {
        guard let blob = try? Data(contentsOf: metadata, options: .mappedIfSafe) else {
            throw Failure.noKey("no \(metadata.lastPathComponent)")
        }
        guard let k = find(blob, length: 32, roll: keyRoll, sha: keySHA),
              let v = find(blob, length: 16, roll: ivRoll, sha: ivSHA) else {
            throw Failure.noKey("not in this build of the game")
        }
        return Key(key: k, iv: v)
    }

    private static func sha256(_ p: UnsafeRawBufferPointer) -> String {
        var d = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        _ = CC_SHA256(p.baseAddress, CC_LONG(p.count), &d)
        return d.map { String(format: "%02x", $0) }.joined()
    }

    /// Rabin-Karp: h(w) = sum of w[i] * B^(n-1-i), modulo 2^64.
    private static func find(_ blob: Data, length n: Int, roll want: UInt64, sha: String) -> Data? {
        let base: UInt64 = 1_099_511_628_211
        var top: UInt64 = 1                   // B^n, what the byte leaving the window weighs
        for _ in 0..<n { top = top &* base }
        return blob.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Data? in
            let b = raw.bindMemory(to: UInt8.self)
            var h: UInt64 = 0
            for i in 0..<b.count {
                h = h &* base &+ UInt64(b[i])
                if i >= n { h = h &- UInt64(b[i - n]) &* top }
                guard i + 1 >= n, h == want else { continue }
                let w = UnsafeRawBufferPointer(rebasing: raw[(i + 1 - n)...i])
                if sha256(w) == sha { return Data(w) }
            }
            return nil
        }
    }

    // MARK: - Reading and writing a record

    static func decrypt(_ file: URL, _ k: Key) throws -> Any {
        guard let text = try? String(contentsOf: file, encoding: .utf8),
              let cipher = Data(base64Encoded: text.trimmingCharacters(in: .whitespacesAndNewlines)) else {
            throw Failure.unreadable(file.lastPathComponent)
        }
        guard let plain = crypt(CCOperation(kCCDecrypt), cipher, k),
              let json = try? JSONSerialization.jsonObject(with: plain, options: [.mutableContainers]) else {
            throw Failure.unreadable(file.lastPathComponent)
        }
        return json
    }

    static func encrypt(_ json: Any, _ k: Key) throws -> String {
        let plain = try JSONSerialization.data(withJSONObject: json, options: [.withoutEscapingSlashes])
        guard let cipher = crypt(CCOperation(kCCEncrypt), plain, k) else {
            throw Failure.unreadable("encrypt")
        }
        return cipher.base64EncodedString()
    }

    private static func crypt(_ op: CCOperation, _ input: Data, _ k: Key) -> Data? {
        var out = Data(count: input.count + kCCBlockSizeAES128)
        let outCap = out.count
        var moved = 0
        let status = out.withUnsafeMutableBytes { o in
            input.withUnsafeBytes { i in
                k.key.withUnsafeBytes { kp in
                    k.iv.withUnsafeBytes { ivp in
                        CCCrypt(op, CCAlgorithm(kCCAlgorithmAES), CCOptions(kCCOptionPKCS7Padding),
                                kp.baseAddress, k.key.count, ivp.baseAddress,
                                i.baseAddress, input.count, o.baseAddress, outCap, &moved)
                    }
                }
            }
        }
        guard status == kCCSuccess else { return nil }
        out.count = moved
        return out
    }

    /// A record the game will take as newer than the one it has: one version
    /// on, and no longer its default (a default loses every merge).
    private static func touch(_ rec: NSMutableDictionary) {
        let v = (rec["version"] as? NSNumber)?.uint64Value ?? 0
        rec["version"] = NSNumber(value: v + 1)
        rec["isDefault"] = false
    }

    // MARK: - Unlocking

    struct Report {
        var profiles = 0
        var balls = 0
        var putters = 0
        var courses = 0
        var cosmetics = 0
    }

    /// Everything a player could find in the courses they have, for every
    /// played profile. `backup` gets a copy of each file before it changes.
    static func unlockAll(filesDir: URL, metadata: URL, backup: URL?) throws -> Report {
        let found = profiles(filesDir: filesDir)
        guard !found.isEmpty else { throw Failure.noSave }
        let k = try key(metadata: metadata)
        var report = Report()
        for prof in found {
            guard let avatar = try decrypt(prof.avatar, k) as? NSMutableDictionary,
                  let golfFile = try decrypt(prof.golf, k) as? NSMutableDictionary,
                  let golf = golfFile["golfData"] as? NSMutableDictionary,
                  let locFile = try decrypt(prof.locations, k) as? NSDictionary,
                  let locs = locFile["locationData"] as? NSDictionary else {
                throw Failure.unreadable(prof.dir.lastPathComponent)
            }
            if (avatar["isDefault"] as? Bool) == true { continue }
            func bought(_ loc: String) -> Bool {
                ((locs[loc] as? NSDictionary)?["isPurchased"] as? Bool) == true
            }

            var balls = (avatar["ballsFound"] as? [NSNumber])?.map { $0.intValue } ?? []
            var putters = (avatar["puttersFound"] as? [NSNumber])?.map { $0.intValue } ?? []
            var haveBall = Set(balls), havePutter = Set(putters)
            let ballsBefore = balls.count
            var golfChanged = false
            for c in courses where !c.dlc || bought(c.loc) {
                for b in c.balls where !haveBall.contains(b) {
                    balls.append(b); haveBall.insert(b); report.balls += 1
                }
                // The count the course shows ("14 of 18"), kept by the game
                // beside the ids. A course never played has no record yet; the
                // game makes one when it is first opened and counts again then.
                if let rec = golf[c.loc] as? NSMutableDictionary,
                   (rec["ballsFound"] as? NSNumber)?.intValue != c.balls.count {
                    rec["ballsFound"] = NSNumber(value: c.balls.count)
                    touch(rec)
                    golfChanged = true
                    report.courses += 1
                }
            }
            for f in foxhunts where !f.dlc || bought(f.loc) || bought(f.easy) {
                for p in f.putters where !havePutter.contains(p) {
                    putters.append(p); havePutter.insert(p); report.putters += 1
                }
            }
            let avatarChanged = balls.count != ((avatar["ballsFound"] as? [Any])?.count ?? 0)
                || putters.count != ((avatar["puttersFound"] as? [Any])?.count ?? 0)
            var cosFile: NSMutableDictionary?
            if FileManager.default.fileExists(atPath: prof.cosmetics.path),
               let file = try decrypt(prof.cosmetics, k) as? NSMutableDictionary,
               let cos = file["cosmeticData"] as? NSMutableDictionary {
                for id in earned {
                    guard let rec = cos[id] as? NSMutableDictionary,
                          (rec["isPurchased"] as? Bool) == true, (rec["isUnlocked"] as? Bool) != true else { continue }
                    rec["isUnlocked"] = true
                    touch(rec)
                    cosFile = file
                    report.cosmetics += 1
                }
            }
            guard avatarChanged || golfChanged || cosFile != nil else { continue }

            if let backup {
                let to = backup.appendingPathComponent(prof.dir.lastPathComponent)
                try FileManager.default.createDirectory(at: to, withIntermediateDirectories: true)
                for f in [prof.avatar, prof.golf, prof.stats, prof.cosmetics] where FileManager.default.fileExists(atPath: f.path) {
                    let dst = to.appendingPathComponent(f.lastPathComponent)
                    try? FileManager.default.removeItem(at: dst)
                    try FileManager.default.copyItem(at: f, to: dst)
                }
            }
            if avatarChanged {
                avatar["ballsFound"] = balls.map { NSNumber(value: $0) }
                avatar["puttersFound"] = putters.map { NSNumber(value: $0) }
                touch(avatar)
                try encrypt(avatar, k).write(to: prof.avatar, atomically: true, encoding: .utf8)
                if balls.count > ballsBefore, FileManager.default.fileExists(atPath: prof.stats.path),
                   let stats = try decrypt(prof.stats, k) as? NSMutableDictionary,
                   let total = stats["totalBallsUnlocked"] as? NSNumber {
                    stats["totalBallsUnlocked"] = NSNumber(value: total.int64Value + Int64(balls.count - ballsBefore))
                    touch(stats)
                    try encrypt(stats, k).write(to: prof.stats, atomically: true, encoding: .utf8)
                }
            }
            if golfChanged {
                touch(golfFile)
                try encrypt(golfFile, k).write(to: prof.golf, atomically: true, encoding: .utf8)
            }
            if let cosFile {
                touch(cosFile)
                try encrypt(cosFile, k).write(to: prof.cosmetics, atomically: true, encoding: .utf8)
            }
            report.profiles += 1
        }
        return report
    }
}
