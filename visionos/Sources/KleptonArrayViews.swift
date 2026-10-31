import Metal

/// The compositor's `.type2DArray` views of plain 2D eye textures (see
/// `arrayView` in KleptonCompositor.swift for why they exist at all).
///
/// A view keeps its parent texture alive. The parents here are not ours: they
/// are MoltenVK's swapchain images, named to the compositor through the eye
/// table (`kl_glfb_note_eye_mtl_texture`) and dropped from it again when the
/// guest destroys the swapchain (`kl_glfb_release_eye_texture`). A cache that
/// only ever grows therefore pinned every swapchain generation the guest ever
/// built — each rebuild (a scene change, a resolution change) left one whole
/// set of eye images resident behind views nothing sampled again.
///
/// So the cache is pruned against the table: whatever the table no longer
/// names has no frame left that can ask for it, and its view goes, taking the
/// last reference to the old image with it. Pruning runs on a miss — a new
/// texture is what a rebuild looks like from here — and every `pruneInterval`
/// frames, because on a miss the old generation is often still in the table
/// (the guest asserts the new swapchain before destroying the old one).
///
/// Keyed on the texture's address, which is what the eye table holds: pruning
/// compares pointers and never touches (or retains) an object the guest
/// thread may be releasing at that moment. An address cannot be reused while
/// its entry is cached, because the view keeps the object at it alive.
///
/// Not thread-safe: the compositor's render thread is the only caller.
final class KleptonArrayViews {
    private var views: [UnsafeRawPointer: MTLTexture] = [:]
    private var framesSincePrune = 0
    private let pruneInterval: Int
    /// The textures the eye table names right now. Called on the render
    /// thread, at most once per prune.
    private let live: () -> Set<UnsafeRawPointer>
    /// Told how many views a prune dropped and how many it kept, when it
    /// dropped any.
    var onDrop: ((_ dropped: Int, _ kept: Int) -> Void)?

    init(pruneInterval: Int = 90, live: @escaping () -> Set<UnsafeRawPointer>) {
        self.pruneInterval = pruneInterval
        self.live = live
    }

    var count: Int { views.count }

    /// The cached view of `tex`, or a new one from `make` (nil if it could not
    /// be made, which is not cached).
    func view(of tex: MTLTexture, make: (MTLTexture) -> MTLTexture?) -> MTLTexture? {
        let key = UnsafeRawPointer(Unmanaged.passUnretained(tex).toOpaque())
        if let v = views[key] { return v }
        guard let v = make(tex) else { return nil }
        prune()
        views[key] = v
        return v
    }

    /// Once per composited frame.
    func frameDone() {
        framesSincePrune += 1
        if framesSincePrune >= pruneInterval { prune() }
    }

    /// Drop every view whose parent the eye table no longer names.
    @discardableResult
    func prune() -> Int {
        framesSincePrune = 0
        guard !views.isEmpty else { return 0 }
        let named = live()
        let before = views.count
        views = views.filter { named.contains($0.key) }
        let dropped = before - views.count
        if dropped > 0 { onDrop?(dropped, views.count) }
        return dropped
    }
}
