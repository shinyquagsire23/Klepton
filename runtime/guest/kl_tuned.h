//
// The store titles that were brought up before the profile for untested
// games existed (kl_untested_profile, runtime/guest/kl_driver.c): every x<app id>
// named anywhere in the runtime, the app or the docs on 2026-10-17 (VRChat from
// Steam, x438100, added when the profile met release-538; community fixes
// 53-55, made and tested before the profile, added when they were merged), and every
// title in the AVP Play catalogue's seed (verified, Community Verified,
// incompatible). They keep exactly the defaults they were tested with; the
// profile is for a store title nobody here has looked at.
//
// A title added later stays under the profile unless it is added here, so a
// fix that was found and tested with the profile on keeps it. Sorted: looked up
// with bsearch.
//

#ifndef KL_TUNED_H
#define KL_TUNED_H

static const char *const kl_tuned_targets[] = {
    "x0000123456789",
    "x0110000100000001",
    "x10048506838538799",
    "x1642239225880682",
    "x1906871599408213",
    "x2010043642376517",
    "x2104963472963790",
    "x2111191602278229",
    "x2249318828413747",
    "x2264524423619421",
    "x2285803881494677",
    "x2334376869949242",
    "x2436558143118760",
    "x24882487241367228",
    "x2544573735586536",
    "x2582932495064035",
    "x2637179839719680",
    "x2677344882310094",
    "x2773034772778845",
    "x2897337400373711",
    "x3240283705987790",
    "x3386618894743567",
    "x3484270071659289",
    "x3634830803298285",
    "x3635172946605196",
    "x3684804704932159",
    "x4015163475201433",
    "x4360608937312293",
    "x438100",
    "x5190288877703616",
    "x5245041552210029",
    "x5269080709783454",
    "x5340130476095884",
    "x5361030930653377",
    "x5767635983306831",
    "x5812519008825194",
    "x5817243991680545",
    "x6236169136472090",
    "x6323807174332876",
    "x6963344583787383",
    "x7221491444554579",
    "x7255396864545733",
    "x7527722310622065",
    "x7532973430077887",
    "x8116626111777808",
    "x8387152738076399",
    "x9386827918099701",
    "x9449316705178989",
    "x9531494193591052",
    "x9685315748175636",
    "x9863010250399155",
};

#endif
