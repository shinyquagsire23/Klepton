# scotland2

A Quest/Android specific modloader for Beat Saber.

## Features

- "something regarding scene changes similar to how bsipa has on enable and whatnot"
- plain check for other mods being installed
- unload method except maybe too crashy
- on enable / on unity init method
- maybe do something with registration instead of extern c stuff?
- [thing](https://discord.com/channels/629851957439365131/629851957439365135/975450045585440788)

## Installation/Usage

This is a [LibProxyMain](https://github.com/sc2ad/LibMainLoader) compatible modloader. Thus, it should be placed at: `/sdcard/ModData/<APP ID>/Modloader/libsl2.so`.
This modloader has several call in points. It performs a topological sort of all depenents that need to be loaded and loads them in turn. `libs` are loaded very early, on `modloader_load`, while `early_mods` are also constructed at this time. `early_mods` have their `load` function called on them after `il2cpp_init`, which is hooked via [Flamingo](https://github.com/sc2ad/Flamingo) to allow us to perform loading at a sufficiently late time. early mods will also have a function named `late_load` called when mods are constructed, but more on that in the next paragraph.

`mods` will be constructed when the libunity.so method `DestroyObjectHighLevel` is called the first time. This method is what destroys unity objects, and it happens that the first destroyed object is within the first scene load call. This is the perfect timing for allowing creation of GameObjects and other unity assets at dlopen time, much like how it would happen on PC. This method is hooked with flamingo through an xref trace from the DestroyImmediate icall for gameobjects, through to the internal method `DestroyObjectHighLevel`. Here early mods get their late_load method called, after which mods will get their late_load method called.

This way of initializing at unity init is inspired by what BSIPA does on pc, where a gameobject is created and destroyed, and when its OnDestroy happens things are loaded in.

Here is a table containing what gets opened and called when.
 - `dlopen` means the .so file will be opened at that time.
 - `setup` means the setup method which fills the mod info is called at that point.
 - `load` means the load method is called at that point
 - `late_load` means the late_load method is called at that point.

|                         | Lib            | Early Mod         | Mod                            |
|-------------------------|----------------|-------------------|--------------------------------|
| app start               | `dlopen`       | `dlopen`, `setup` | not yet initialized            |
| il2cpp_init             | nothing called | `load`            | not yet initialized            |
| DestroyObjectHighLevel  | nothing called | `late_load`       | `dlopen`, `setup`, `late_load` |

All libs, early mods, and mods are loaded in _sortedd_ order, as opposed to directory order. This allows the load order to be consistent across the same sets of files. Strictly speaking, this uses [std::filesystem::path::compare](https://en.cppreference.com/w/cpp/filesystem/path/compare) to sort and thus will load in that order.
