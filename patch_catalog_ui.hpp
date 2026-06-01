#pragma once

namespace patch_catalog_ui {

inline int SelectionAfterServerLoad(int serverCount) {
    (void)serverCount;
    return -1;
}

inline bool ShouldLoadPatchesAfterSelection(int previousSelection, int newSelection) {
    return newSelection >= 0 && newSelection != previousSelection;
}

} // namespace patch_catalog_ui
