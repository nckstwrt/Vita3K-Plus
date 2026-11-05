package org.vita3k.emulator.store

/** Catalog kinds that Vita3K can actually run/install (pkgj also has PSP/PSM/PSX/themes, not supported by the emulator). */
enum class StoreCategory(val label: String, val prefKey: String) {
    GAMES("Games", "url_games"),
    DLCS("DLC", "url_dlcs"),
    DEMOS("Demos", "url_demos")
}

data class StoreItem(
    val category: StoreCategory,
    val region: String,
    val name: String,
    val url: String,
    /** zRIF license string, or empty when none is required. */
    val zrif: String,
    /** e.g. UP0001-PCSA00001_00-XXXXXXXXXXXXXXXX */
    val contentId: String,
    val lastModified: String,
    val sizeBytes: Long,
    /** hex sha256 of the .pkg, or null when the catalog has none. */
    val sha256: String?
) {
    /** 9-char title id (PCSA00001), same slicing pkgj uses: content.substr(7, 9). */
    val titleId: String
        get() = if (contentId.length >= 16) contentId.substring(7, 16) else ""

    val key: String
        get() = contentId.ifEmpty { url }
}

sealed interface DownloadState {
    data object Idle : DownloadState
    data class Downloading(val done: Long, val total: Long) : DownloadState
    data object Verifying : DownloadState
    data object Installing : DownloadState
    data class Failed(val message: String) : DownloadState
    data object Installed : DownloadState
}
