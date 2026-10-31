import Metal

// The depth states the composite pass draws its quads with (KleptonCompositor), in a file of its own so
// that `make statscard` can run them on the Mac against the real overlay shader.
//
// visionOS reprojects the submitted frame from its depth buffer, so every quad writes the depth it is
// at; what differs is the test. Reverse-Z: a nearer quad has the GREATER depth, the clear is 0.
enum KleptonOverlayDepth {
    /// The eye picture and, by default, the guest's quads: what is nearer wins. A quad's whole rectangle
    /// counts as near, its transparent parts included.
    static func nearerWins() -> MTLDepthStencilDescriptor {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .greater
        d.isDepthWriteEnabled = true
        return d
    }

    /// The guest's quads in the order it submitted them (KL_OVERLAY_SUBMIT_ORDER): no test, depth still
    /// written.
    static func inOrder() -> MTLDepthStencilDescriptor {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        return d
    }

    /// The metrics panel (KL_STATS_CARD) — over everything, as a Quest's performance overlay is. It is
    /// drawn last, after the guest's quads, and with `nearerWins` any of them nearer than its 1.2 m hid
    /// it: in Alien: Rogue Incursion's levels (submit order, every quad stamps its distance) the panel
    /// was there in the menu and gone once a level began. Its own depth is still written, so the system
    /// reprojects it at the distance it is shown at.
    static func statsCard() -> MTLDepthStencilDescriptor { inOrder() }
}
