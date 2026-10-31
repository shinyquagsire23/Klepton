# MetaCore

Provides way too many utilities for mod developers.

The main idea for this came from creating Qounters++ and its API, and felt like many of the statistics and events it provided could be useful without needing any ties to Qounters++ itself. From there, I thought of every bit of reused code across mods I had, and inter-mod communications I could facilitate, and now we're here.

## Documentation

This is just the general idea for each header file with the general contents to expect for each. Documentation for specific functions and variables can be found in the files themselves (except `stats.hpp`, which should be fairly self explanatory, and `internals.hpp`, which would be too annoying to document). Also, many of the functions (in particular everything in `events.hpp`) are not designed for multithreading, so when in doubt use the MainThreadScheduler from BSML. (All callbacks will be run on the main thread for you.)

### `assets.cmake`

Automatically includes any assets in the top-level `assets` directory of your project into the compiled object file. Use by adding this to your `CMakeLists.txt`:

```cmake
include(extern/includes/metacore/shared/assets.cmake)
```

### `assets.hpp`

Provides definitions and utilities for assets included using the cmake script.

### `delegates.hpp`

Improves on BSML's delegate helpers to make even easier (less verbose) delegate creation, specifically around lambdas.

### `events.hpp`

Provides the ability to register callbacks to and broadcast events, with some available by default and the option to add more from other mods.

### `game.hpp`

Provides utilities for various aspects of Beat Saber game state and singletons.

### `il2cpp.hpp`

Provides experimental utilities and wrappers for Il2Cpp types.

### `input.hpp`

Provides utilities for managing controllers and their input.

### `internals.hpp`

Allows access and modification to the internal statistics tracked by the mod, in case another mod changes them in an unusual way.

### `java.hpp`

Allows easier and less verbose use of java and JNI functions.

### `jutils.hpp`

Provides some base types used in `java.hpp`, with more detailed documentation in their use.

### `maps.hpp`

Defines two potentially useful std::map wrappers for different types of data access and storage.

### `operators.hpp`

Provides magic macros that make operators usable on cordl types. (Credit to [Qwasyx](https://github.com/Qwasyx).)

### `pp.hpp`

Provides BeatLeader and ScoreSaber PP-related information retrieval and calculations.

### `songs.hpp`

Provides utilities related to songs and beatmaps.

### `stats.hpp`

Provides getters for many statistics about the currently playing level.

### `ui.hpp`

Provides utilities that I personally use to make creating and updating BSML (Lite) UI a little easier.

### `uiimpl.hpp`

Defines the functions in `ui.hpp`, in order to make BSML not a required dependency.

### `unity.hpp`

Provides various functions to enhance working with Unity Components, Transforms, Quaternions, and other objects. Namespace is named `Engine` instead of `Unity` to avoid collisions.
