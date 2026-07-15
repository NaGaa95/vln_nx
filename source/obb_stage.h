#ifndef OBB_STAGE_H
#define OBB_STAGE_H

/* Check an already-extracted assets tree or perform first-boot staging of split game
 * data. Paths are relative to the game folder. A complete extracted data.unity3d is
 * used as-is; otherwise the OBB is unpacked and merged with the APK's UnityFS stub.
 *
 * `obb_name` is the OBB file name in the game folder (e.g. VLN_OBB_NAME). Returns 0
 * when the game data is ready (already staged, or freshly staged this call), negative
 * on failure. Safe to call on every boot -- it is a fast no-op once staged. */
int obb_stage(const char *obb_name);

#endif
