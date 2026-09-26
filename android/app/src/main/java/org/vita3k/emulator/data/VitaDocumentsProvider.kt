package org.vita3k.emulator.data

import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.R
import java.io.File
import java.io.FileNotFoundException

class VitaDocumentsProvider : DocumentsProvider() {

    companion object {
        const val ROOT_ID = "vita3k"
        const val DOC_ROOT = "vita:"

        const val ROOT_ID_STORAGE = "vita3k-storage"
        const val DOC_ROOT_STORAGE = "storage:"

        fun authority(context: Context): String = "${context.packageName}.documents"

        fun notifyRootsChanged(context: Context) {
            runCatching {
                context.contentResolver.notifyChange(
                    DocumentsContract.buildRootsUri(authority(context)), null
                )
            }
        }

        private val ROOT_PROJECTION = arrayOf(
            Root.COLUMN_ROOT_ID,
            Root.COLUMN_FLAGS,
            Root.COLUMN_ICON,
            Root.COLUMN_TITLE,
            Root.COLUMN_SUMMARY,
            Root.COLUMN_DOCUMENT_ID,
            Root.COLUMN_AVAILABLE_BYTES
        )

        private val DOCUMENT_PROJECTION = arrayOf(
            Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_MIME_TYPE,
            Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_LAST_MODIFIED,
            Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE
        )
    }

    override fun onCreate(): Boolean = true

    // The provider can be spawned by a file manager before the emulator ever runs, so the native
    private fun appFilesDirOrNull(): File? {
        val ctx = context ?: return null
        return runCatching { File(AppStorage.storageRootPath(ctx)) }.getOrNull()
    }

    private fun emulatorStorageDirOrNull(): File? {
        val nativePath = runCatching {
            if (NativeLib.isInitialized()) NativeLib.getCurrentEmulatorPath() else null
        }.getOrNull()?.takeIf { it.isNotEmpty() }
        if (nativePath != null)
            return File(nativePath)
        val ctx = context ?: return null
        return runCatching { File(AppStorage.defaultStoragePath(ctx)) }.getOrNull()
    }

    private fun isUnder(child: File, parent: File): Boolean = runCatching {
        val p = parent.canonicalPath
        val c = child.canonicalPath
        c == p || c.startsWith(p + File.separator)
    }.getOrDefault(false)

    private fun storageNeedsOwnRoot(): Boolean {
        val app = appFilesDirOrNull() ?: return false
        val storage = emulatorStorageDirOrNull() ?: return false
        return !isUnder(storage, app)
    }

    private fun baseDirForDocId(docId: String): File? =
        if (docId.startsWith(DOC_ROOT_STORAGE)) emulatorStorageDirOrNull() else appFilesDirOrNull()

    private fun docRootFor(docId: String): String =
        if (docId.startsWith(DOC_ROOT_STORAGE)) DOC_ROOT_STORAGE else DOC_ROOT

    private fun fileForDocId(docId: String, mustExist: Boolean = true): File {
        if (!docId.startsWith(DOC_ROOT) && !docId.startsWith(DOC_ROOT_STORAGE))
            throw FileNotFoundException("Unknown document $docId")
        val base = baseDirForDocId(docId)
            ?: throw FileNotFoundException("Storage location is not available")
        val rel = docId.substring(docRootFor(docId).length)
        val file = if (rel.isEmpty()) base else File(base, rel)
        val baseCanonical = base.canonicalPath
        val canonical = file.canonicalPath
        if (canonical != baseCanonical && !canonical.startsWith(baseCanonical + File.separator))
            throw FileNotFoundException("Document $docId is outside its root")
        if (mustExist && !file.exists())
            throw FileNotFoundException("Missing document $docId")
        return file
    }

    private fun childDocId(parentDocId: String, name: String): String =
        if (parentDocId.endsWith(":")) parentDocId + name else "$parentDocId/$name"

    private fun mimeType(file: File): String {
        if (file.isDirectory)
            return Document.MIME_TYPE_DIR
        val ext = file.extension.lowercase()
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
    }

    private fun includeFile(cursor: MatrixCursor, docId: String, file: File) {
        val isRoot = docId == DOC_ROOT || docId == DOC_ROOT_STORAGE
        var flags = 0
        if (file.isDirectory) {
            flags = flags or Document.FLAG_DIR_SUPPORTS_CREATE
        } else {
            flags = flags or Document.FLAG_SUPPORTS_WRITE
        }
        if (!isRoot) {
            flags = flags or Document.FLAG_SUPPORTS_DELETE or
                Document.FLAG_SUPPORTS_RENAME or Document.FLAG_SUPPORTS_MOVE
        }
        cursor.newRow().apply {
            add(Document.COLUMN_DOCUMENT_ID, docId)
            add(Document.COLUMN_MIME_TYPE, mimeType(file))
            add(Document.COLUMN_DISPLAY_NAME, when {
                docId == DOC_ROOT -> "Vita3K+"
                docId == DOC_ROOT_STORAGE -> "Vita3K+ storage"
                else -> file.name
            })
            add(Document.COLUMN_LAST_MODIFIED, file.lastModified())
            add(Document.COLUMN_FLAGS, flags)
            add(Document.COLUMN_SIZE, if (file.isFile) file.length() else null)
        }
    }

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val cursor = MatrixCursor(projection ?: ROOT_PROJECTION)
        val appFiles = appFilesDirOrNull()
        cursor.newRow().apply {
            add(Root.COLUMN_ROOT_ID, ROOT_ID)
            add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE or Root.FLAG_SUPPORTS_IS_CHILD or Root.FLAG_LOCAL_ONLY)
            add(Root.COLUMN_ICON, R.mipmap.ic_launcher_plus)
            add(Root.COLUMN_TITLE, "Vita3K+")
            add(Root.COLUMN_SUMMARY, runCatching { context?.getString(R.string.documents_root_summary) }.getOrNull())
            add(Root.COLUMN_DOCUMENT_ID, DOC_ROOT)
            add(Root.COLUMN_AVAILABLE_BYTES, runCatching { appFiles?.usableSpace }.getOrNull() ?: 0L)
        }
        if (runCatching { storageNeedsOwnRoot() }.getOrDefault(false)) {
            val storage = emulatorStorageDirOrNull()
            cursor.newRow().apply {
                add(Root.COLUMN_ROOT_ID, ROOT_ID_STORAGE)
                add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE or Root.FLAG_SUPPORTS_IS_CHILD or Root.FLAG_LOCAL_ONLY)
                add(Root.COLUMN_ICON, R.mipmap.ic_launcher_plus)
                add(Root.COLUMN_TITLE, "Vita3K+ storage")
                add(Root.COLUMN_SUMMARY, runCatching { context?.getString(R.string.documents_storage_root_summary) }.getOrNull())
                add(Root.COLUMN_DOCUMENT_ID, DOC_ROOT_STORAGE)
                add(Root.COLUMN_AVAILABLE_BYTES, runCatching { storage?.usableSpace }.getOrNull() ?: 0L)
            }
        }
        return cursor
    }

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val cursor = MatrixCursor(projection ?: DOCUMENT_PROJECTION)
        includeFile(cursor, documentId, fileForDocId(documentId))
        return cursor
    }

    override fun queryChildDocuments(
        parentDocumentId: String,
        projection: Array<out String>?,
        sortOrder: String?
    ): Cursor {
        val cursor = MatrixCursor(projection ?: DOCUMENT_PROJECTION)
        val dir = fileForDocId(parentDocumentId)
        dir.listFiles()?.forEach { child ->
            includeFile(cursor, childDocId(parentDocumentId, child.name), child)
        }
        return cursor
    }

    override fun isChildDocument(parentDocumentId: String, documentId: String): Boolean {
        val prefix = if (parentDocumentId == DOC_ROOT) DOC_ROOT else "$parentDocumentId/"
        return documentId != parentDocumentId && documentId.startsWith(prefix)
    }

    override fun getDocumentType(documentId: String): String = mimeType(fileForDocId(documentId))

    override fun openDocument(
        documentId: String,
        mode: String,
        signal: CancellationSignal?
    ): ParcelFileDescriptor {
        val file = fileForDocId(documentId, mustExist = !mode.contains('w'))
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode))
    }

    override fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        val parent = fileForDocId(parentDocumentId)
        val safeName = displayName.replace('/', '_').replace('\\', '_').trim()
        if (safeName.isEmpty())
            throw FileNotFoundException("Invalid name")
        var target = File(parent, safeName)
        var attempt = 1
        while (target.exists()) {
            target = File(parent, "$safeName (${attempt++})")
        }
        val ok = if (mimeType == Document.MIME_TYPE_DIR) target.mkdirs() else runCatching { target.createNewFile() }.getOrDefault(false)
        if (!ok)
            throw FileNotFoundException("Failed to create $displayName in $parentDocumentId")
        return childDocId(parentDocumentId, target.name)
    }

    override fun deleteDocument(documentId: String) {
        val file = fileForDocId(documentId)
        if (!file.deleteRecursively())
            throw FileNotFoundException("Failed to delete $documentId")
    }

    override fun renameDocument(documentId: String, displayName: String): String {
        val file = fileForDocId(documentId)
        val safeName = displayName.replace('/', '_').replace('\\', '_').trim()
        if (safeName.isEmpty() || safeName == file.name)
            return documentId
        val target = File(file.parentFile, safeName)
        if (target.exists() || !file.renameTo(target))
            throw FileNotFoundException("Failed to rename $documentId to $displayName")
        val parentDocId = documentId.substringBeforeLast('/', DOC_ROOT)
        return childDocId(parentDocId, safeName)
    }

    override fun moveDocument(
        sourceDocumentId: String,
        sourceParentDocumentId: String,
        targetParentDocumentId: String
    ): String {
        val source = fileForDocId(sourceDocumentId)
        val targetParent = fileForDocId(targetParentDocumentId)
        val target = File(targetParent, source.name)
        if (target.exists() || !source.renameTo(target))
            throw FileNotFoundException("Failed to move $sourceDocumentId to $targetParentDocumentId")
        return childDocId(targetParentDocumentId, source.name)
    }
}
