// The compositor's array-view cache (KleptonArrayViews.swift) against a guest
// that rebuilds its per-eye swapchains: the views of a released generation
// must go, and with them the last reference to its images, so the memory the
// device holds stays flat across rebuilds. Real Metal on the Mac; the eye
// table is simulated the way kl_openxr.c drives it (one 2D image per stage per
// eye, the new swapchain noted before the old one is released).
import Foundation
import Metal

@main
struct ArrayViewTests {
    static func expect(_ condition: @autoclosure () -> Bool, _ description: String) {
        if !condition() {
            print("FAIL: \(description)")
            exit(1)
        }
    }

    static let images = 3            // per eye, as an OpenXR swapchain has
    static let size = 1024           // 4 MiB per RGBA8 image
    static let rebuilds = 25
    static let framesPerGeneration = 200

    /// What `arrayView` does with a cache miss.
    static func makeView(_ t: MTLTexture) -> MTLTexture? {
        t.makeTextureView(pixelFormat: t.pixelFormat, textureType: .type2DArray,
                          levels: 0..<t.mipmapLevelCount, slices: 0..<1)
    }

    /// The eye table: what kl_glfb_eye_mtl_texture returns, [eye][stage].
    /// The table's entries are unretained there; here the swapchain array
    /// below is what owns the images, as MoltenVK does.
    final class EyeTable {
        var table: [[UnsafeRawPointer?]] = Array(repeating: Array(repeating: nil, count: 16),
                                                 count: 2)
        var live: Set<UnsafeRawPointer> { Set(table.joined().compactMap { $0 }) }
    }

    static func swapchain(_ device: MTLDevice) -> [MTLTexture] {
        (0..<images).map { _ in
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: size, height: size, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            return device.makeTexture(descriptor: d)!
        }
    }

    /// Runs `rebuilds` swapchain generations through `lookup` and returns the
    /// device's allocated size after each one, plus how many old images are
    /// still alive at the end.
    static func run(_ device: MTLDevice, _ name: String, _ eyes: EyeTable,
                    lookup: (MTLTexture) -> MTLTexture?, frameDone: () -> Void,
                    cached: () -> Int) -> (sizes: [Int], survivors: Int) {
        var owned: [[MTLTexture]] = [[], []]
        var weakOld: [WeakTexture] = []
        var sizes: [Int] = []
        for gen in 0..<rebuilds {
            autoreleasepool {
                // The new swapchain is noted first...
                let fresh = [swapchain(device), swapchain(device)]
                for e in 0..<2 {
                    for (k, t) in fresh[e].enumerated() {
                        eyes.table[e][k] = UnsafeRawPointer(Unmanaged.passUnretained(t).toOpaque())
                    }
                }
                // ...and the old one released after (klxr_DestroySwapchain).
                for e in 0..<2 {
                    weakOld += owned[e].map { WeakTexture($0) }
                }
                owned = fresh
                for f in 0..<framesPerGeneration {
                    for e in 0..<2 {
                        let p = eyes.table[e][f % images]!
                        let t = Unmanaged<MTLTexture>.fromOpaque(p).takeUnretainedValue()
                        let v = lookup(t)
                        expect(v != nil && v!.textureType == .type2DArray,
                               "\(name): a 2D eye texture gets a 2D-array view")
                        expect(v!.parent === t,
                               "\(name): the view is of the texture asked for")
                    }
                    frameDone()
                }
            }
            sizes.append(device.currentAllocatedSize)
            if gen == 0 || gen == rebuilds - 1 {
                print("  \(name): generation \(gen + 1): \(cached()) view(s) cached, "
                      + "device holds \(device.currentAllocatedSize / (1 << 20)) MiB")
            }
        }
        return (sizes, weakOld.filter { $0.texture != nil }.count)
    }

    final class WeakTexture {
        weak var texture: MTLTexture?
        init(_ t: MTLTexture) { texture = t }
    }

    static func main() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            print("SKIP: no Metal device")
            return
        }
        let mib = 1 << 20
        let perGeneration = 2 * images * size * size * 4

        // The cache before this change: a dictionary that only grows.
        var grow: [ObjectIdentifier: MTLTexture] = [:]
        let before = run(device, "unpruned (old)", EyeTable(), lookup: { t in
            if let v = grow[ObjectIdentifier(t)] { return v }
            let v = makeView(t)
            grow[ObjectIdentifier(t)] = v
            return v
        }, frameDone: {}, cached: { grow.count })
        expect(before.survivors == (rebuilds - 1) * 2 * images,
               "control: the unpruned cache pins every old image")
        grow.removeAll()

        // The cache as the compositor uses it, pruned against the table.
        let eyes = EyeTable()
        var drops = 0
        let cache = KleptonArrayViews(live: { eyes.live })
        cache.onDrop = { _, _ in drops += 1 }
        let after = run(device, "pruned (new)", eyes,
                        lookup: { cache.view(of: $0, make: makeView) },
                        frameDone: { cache.frameDone() }, cached: { cache.count })
        expect(after.survivors == 0, "every released image is freed")
        expect(cache.count <= 2 * images,
               "at most one generation of views is cached (\(cache.count))")
        expect(drops >= rebuilds - 1, "each rebuild drops the old views (\(drops))")
        let spread = (after.sizes.max()! - after.sizes.min()!)
        expect(spread < perGeneration,
               "device memory stays flat across rebuilds (spread \(spread / mib) MiB)")
        let grew = before.sizes.last! - before.sizes.first!
        print("  unpruned: +\(grew / mib) MiB over \(rebuilds - 1) rebuilds; "
              + "pruned: spread \(spread / mib) MiB, \(after.survivors) old image(s) alive")
        print("arrayviews: \(rebuilds) swapchain rebuilds, memory stable — ok")
    }
}
