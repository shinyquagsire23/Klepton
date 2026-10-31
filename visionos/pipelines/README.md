# Pipeline lists for games that come as an APK

`<target>.pipelines` is a recipe for a game's Vulkan pipeline list: which shaders the game combines with which
state, with every shader module replaced by where it is in the game's own files (`tools/unity_pipelines.py`). It
carries no shader code. An installer turns it into the list on the player's Mac, from the player's copy of the
game, and copies it to the headset as `Documents/klfz/<target>.foz`; the app compiles it before the game starts
(`prepare_graphics` in `Sources/kl_app.c`), so that a shader is not compiled in the moment the game first needs it.

`<target>.device-info.json` is the description of the Vulkan device the game creates (extensions and features — no
identifiers), recorded on a Vision Pro. It goes along as `Documents/klfz/device-info.default.json`, for the first
start, before the game has written its own.

A list is made by recording a run that goes through the game's content (`KL_VK_FOZ_DIR` with `build/m_boot_fz` on a
Mac) and then `unity_pipelines.py recipe <recorded.foz> <game folder> <target>.pipelines`. Walkabout Mini Golf's
was recorded by calling the game's own scene loader with one course's name after another (`KL_PROBE_SCRIPT`,
`XRRig::LoadScene("<SceneName>")`, 3,000 frames apart — a course is up after some 300), then each of the 36
launchers in its Homestar Runner basement; the names are read from the game's own list of courses first
(`KL_PROBE_KEEP` alone). `<target>.tour.json` holds those method names. 85 of 85 scenes and 36 launchers took
81 minutes on a Mac. Merged into it: what a Vision Pro recorded in played sessions.

Loading a scene is not playing it. Measured against two played sessions on a headset, the walk alone had 144 of
167 and 201 of 243 of the graphics pipelines those needed (86 % and 82 %): what only appears in play is in the
list where somebody played it, and nowhere else.

An earlier walk used the game's test driver (`Mighty.Tester.MightyTestManager::StartFullPerformanceSequence`).
It took 1 h 50 min, skipped a course when a step before it failed, could not open the Homestar Runner pack, and
found fewer pipelines per course.

`tools/pipeline_tour.py` keeps a list in step with the game. `status <target> <game folder>` compares the SHADERS in
every file of this copy of the game with those the list was made from (`<target>.files.json`: one checksum per file,
and per bundle inside the APK and the OBB) — a file that was merely packed anew has the same shaders. `update`
walks what that makes due, merges the new recording with the one kept on this Mac, and writes the list anew:
only the scenes of single levels where only their files have other shaders, everything where a file that all
levels share has.

Walkabout 6.7 against the 6.8 list, as a measure: 90 of its files differ by checksum, 15 of 202 differ in their
shaders, and among those are bundles every level shares — a walk of everything.

The walk loads what is in the files Meta delivered, through the game's own loader. What this layer tells the game
about purchases is never changed for it: that stays what the store confirmed.

A recipe fits the build of the game it was made from. On another build a module that moved is found again by its
checksum; what is really gone is skipped, and the rest still helps (the 6.8 list on 6.7's files: 1,592 of 1,640).
