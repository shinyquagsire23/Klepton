# smol-v

SMOL-V by Aras Pranckevičius — https://github.com/aras-p/smol-v, commit
`55000efe742f56d8b51223b9ea7775a8f0501881` (`source/smolv.cpp`, `source/smolv.h`, `license.txt`), public domain or
MIT at your choice (LICENSE.txt).

Unity stores a game's Vulkan shaders in this encoding. `tools/unity_pipelines.py` decodes them from a player's own
copy of a game to rebuild a pipeline list; nothing of a game is in this folder.

Changed here, marked `klepton` in the source: `Decode` also stops when its output is complete and leaves the
number of input bytes it took in `smolv::g_klDecodedInputSize`, so that a program whose packed length is not known
can be decoded. `kl_smolv.cpp` is ours: a C interface for ctypes.

Unchanged originals: smolv.cpp sha256 `ae1818f33a0531ca6ecd1aad821ed8ae9225c4c0ac47bc016ab7c7a1bb1f813e`,
smolv.h `a4ef18392b5fed85844dec70c8fffe23b69ade92c8bea13e2986c97ad8872ca1`.
