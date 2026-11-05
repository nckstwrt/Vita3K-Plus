package org.vita3k.emulator.store

import java.io.File
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Resumable PKG downloader (pkgj downloads with HTTP range resume and verifies sha256 from the catalog).
 * Blocking: call from Dispatchers.IO. Cancellation is cooperative via [isActive].
 */
object StoreDownloader {

    class DownloadException(message: String) : Exception(message)

    fun download(
        item: StoreItem,
        targetDir: File,
        isActive: () -> Boolean,
        onProgress: (done: Long, total: Long) -> Unit,
        onVerifying: () -> Unit
    ): File {
        targetDir.mkdirs()
        val safeName = item.key.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val part = File(targetDir, "$safeName.pkg.part")
        val finalFile = File(targetDir, "$safeName.pkg")

        if (!finalFile.exists()) {
            var offset = if (part.exists()) part.length() else 0L
            val conn = URL(item.url).openConnection() as HttpURLConnection
            conn.connectTimeout = 15_000
            conn.readTimeout = 30_000
            conn.instanceFollowRedirects = true
            if (offset > 0) conn.setRequestProperty("Range", "bytes=$offset-")
            try {
                val code = conn.responseCode
                when {
                    code == 416 -> { /* part is already complete */ }
                    code == 200 -> {
                        // server ignored Range: restart from scratch
                        if (offset > 0) { part.delete(); offset = 0 }
                    }
                    code != 206 -> throw DownloadException("HTTP $code")
                }
                if (code != 416) {
                    val remaining = conn.contentLengthLong
                    val total = if (remaining >= 0) remaining + offset else item.sizeBytes
                    RandomAccessFile(part, "rw").use { out ->
                        out.seek(offset)
                        val buf = ByteArray(256 * 1024)
                        var done = offset
                        conn.inputStream.use { input ->
                            while (true) {
                                if (!isActive()) throw DownloadException("Cancelled")
                                val n = input.read(buf)
                                if (n < 0) break
                                out.write(buf, 0, n)
                                done += n
                                onProgress(done, total)
                            }
                        }
                        if (total > 0 && done < total) throw DownloadException("Connection closed early")
                    }
                }
            } finally {
                conn.disconnect()
            }
            if (!part.renameTo(finalFile)) throw DownloadException("Cannot finalize download")
        }

        item.sha256?.let { expected ->
            onVerifying()
            val actual = sha256(finalFile)
            if (!actual.equals(expected, ignoreCase = true)) {
                finalFile.delete()
                throw DownloadException("SHA-256 mismatch, file removed. Try again.")
            }
        }
        return finalFile
    }

    private fun sha256(file: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buf = ByteArray(256 * 1024)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }
}
