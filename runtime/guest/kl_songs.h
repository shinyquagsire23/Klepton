// Custom levels for a Beat Saber guest whose own loader was stripped.
// See kl_songs.c.
#ifndef KL_SONGS_H
#define KL_SONGS_H

// Between frames, on the thread that just ran one — the same seat as
// kl_mprobe_tick, and for the same reason: managed calls are safe there.
// Does nothing for a guest without the custom-level classes, or with no
// CustomLevels directory under its files dir.
void kl_songs_tick(unsigned frame);

#endif
