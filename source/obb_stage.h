#ifndef OBB_STAGE_H
#define OBB_STAGE_H

/* First-boot, on-device staging of the split-binary game data. Paths are resolved
 * relative to the current working directory, which main() has chdir'd into the game
 * folder. Unpacks the OBB's sharedassets flat and merges the two data.unity3d halves
 * (APK boot stub + OBB content) into one UnityFS archive the engine reads directly.
 *
 * `obb_name` is the OBB file name in the game folder (e.g. VLN_OBB_NAME). Returns 0
 * when the game data is ready (already staged, or freshly staged this call), negative
 * on failure. Safe to call on every boot -- it is a fast no-op once staged. */
int obb_stage(const char *obb_name);

#endif
