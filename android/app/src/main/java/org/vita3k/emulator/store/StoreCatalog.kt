package org.vita3k.emulator.store

import android.content.Context
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Port of pkgj's TitleDatabase TSV handling (src/db.cpp).
 * Column layout is identical to pkgj's pkgi_get_column_number().
 */
object StoreCatalog {

    private class Cols(
        val region: Int, val name: Int, val url: Int, val zrif: Int,
        val content: Int, val lastMod: Int, val size: Int, val digest: Int
    )

    private fun cols(cat: StoreCategory) = when (cat) {
        StoreCategory.GAMES, StoreCategory.DEMOS -> Cols(1, 2, 3, 4, 5, 6, 8, 9)
        StoreCategory.DLCS -> Cols(1, 2, 3, 4, 5, 6, 7, 8)
    }

    private val hex64 = Regex("^[0-9a-fA-F]{64}$")

    fun parse(category: StoreCategory, tsv: String): List<StoreItem> {
        val c = cols(category)
        val result = ArrayList<StoreItem>()
        val lines = tsv.lineSequence().iterator()
        if (!lines.hasNext()) return result
        lines.next() // header row, skipped by pkgj as well
        while (lines.hasNext()) {
            val line = lines.next().trimEnd('\r')
            if (line.isBlank()) continue
            val f = line.split('\t')
            fun col(i: Int) = f.getOrNull(i)?.trim().orEmpty()

            val url = col(c.url)
            val zrif = col(c.zrif)
            // same skip rules as pkgj
            if (url.isEmpty() || url == "MISSING" || url == "CART ONLY" || zrif == "MISSING") continue

            val digest = col(c.digest)
            result += StoreItem(
                category = category,
                region = col(c.region),
                name = col(c.name),
                url = url,
                zrif = if (zrif == "NOT REQUIRED") "" else zrif,
                contentId = col(c.content),
                lastModified = col(c.lastMod),
                sizeBytes = col(c.size).toLongOrNull() ?: 0L,
                sha256 = digest.takeIf { hex64.matches(it) }?.lowercase()
            )
        }
        return result
    }

    private fun cacheFile(context: Context, category: StoreCategory) =
        File(context.filesDir, "store/catalog_${category.name.lowercase()}.tsv")

    fun loadCached(context: Context, category: StoreCategory): List<StoreItem> {
        val f = cacheFile(context, category)
        if (!f.exists()) return emptyList()
        return runCatching { parse(category, f.readText()) }.getOrDefault(emptyList())
    }

    /** Downloads the TSV from the user-configured URL and caches it. Blocking: call from Dispatchers.IO. */
    fun refresh(context: Context, category: StoreCategory, url: String): List<StoreItem> {
        require(url.isNotBlank()) { "Catalog URL is not set" }
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 15_000
        conn.readTimeout = 30_000
        conn.instanceFollowRedirects = true
        try {
            if (conn.responseCode !in 200..299) error("HTTP ${conn.responseCode}")
            val text = conn.inputStream.bufferedReader().use { it.readText() }
            val items = parse(category, text)
            val f = cacheFile(context, category)
            f.parentFile?.mkdirs()
            f.writeText(text)
            return items
        } finally {
            conn.disconnect()
        }
    }
}

/** Catalog URLs are configured by the user (like pkgj's config.txt); nothing is hardcoded. */
object StoreSettings {
    private const val PREFS = "store_settings"

    fun getUrl(context: Context, category: StoreCategory): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(category.prefKey, "").orEmpty()

    fun setUrl(context: Context, category: StoreCategory, url: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(category.prefKey, url.trim()).apply()
    }
}
