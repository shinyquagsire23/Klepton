// Steam Audio's default HRTF, substituted.
//
// VRChat's shipped libphonon.so carries a 40-byte STUB at gDefaultHrtfData —
// the "HRTF" header decodes to one direction at one sampling rate with a
// 1-sample HRIR, and iplHRTFCreate(type=DEFAULT) dies on it:
// `Unable to create PFFFT setup (size == 1)`. The SOFA path in the same binary
// is complete (a full libmysofa, mysofa_open_data_no_norm and all), so the fix
// is data, not code: hand the guest a real SOFA file.
//
// The bytes are CIPIC subject 124, from Valve's own open-source steam-audio
// tree (core/data/hrtf/cipic_124.sofa, `make phonon-hrtf` refetches, shasum
// pinned there), which has been Apache-2.0 since 4.5.2. CIPIC is not an
// arbitrary pick: HRTFMap's header comment says the class "is designed with
// the CIPIC database in mind".
//
// The file is COMMITTED rather than gitignored like vendor/ and
// vendor-moltenvk/, and the reason is the .incbin below: those two are build
// TOOLS fetched before a build, this is a build INPUT, so a bare checkout that
// did not have it could not compile the runtime at all — and `make check` has
// to pass on a bare checkout. 3.4 MB is the price of that.
//
// .incbin, not a generated C table, because 3.6 MB of HRIR data as string
// literals is a 20 MB source file that every test binary in `make check`
// would have to parse; the assembler swallows it in milliseconds. The path is
// relative to the repo root, where make runs.
.section __DATA,__const
.globl _kl_phonon_hrtf_sofa
.globl _kl_phonon_hrtf_sofa_end
.align 4
_kl_phonon_hrtf_sofa:
.incbin "runtime/data/phonon_hrtf_cipic_124.sofa"
_kl_phonon_hrtf_sofa_end:
