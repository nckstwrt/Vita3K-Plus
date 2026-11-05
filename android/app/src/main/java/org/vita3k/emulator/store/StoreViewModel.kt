package org.vita3k.emulator.store

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.InstallServiceController
import org.vita3k.emulator.ui.viewmodel.InstallResultStatus
import java.io.File

class StoreViewModel(application: Application) : AndroidViewModel(application) {

    var category by mutableStateOf(StoreCategory.GAMES)
        private set
    var query by mutableStateOf("")
        private set
    var allItems by mutableStateOf<List<StoreItem>>(emptyList())
        private set
    var loading by mutableStateOf(false)
        private set
    var error by mutableStateOf<String?>(null)
        private set
    var urlConfigured by mutableStateOf(false)
        private set

    /** per-item state, keyed by StoreItem.key */
    val states = mutableStateMapOf<String, DownloadState>()
    private val jobs = HashMap<String, Job>()

    val visibleItems: List<StoreItem>
        get() {
            val q = query.trim()
            return if (q.isEmpty()) allItems
            else allItems.filter { it.name.contains(q, true) || it.titleId.contains(q, true) }
        }

    init {
        selectCategory(StoreCategory.GAMES)
    }

    fun setQuery(q: String) { query = q }

    fun selectCategory(c: StoreCategory) {
        category = c
        urlConfigured = StoreSettings.getUrl(getApplication(), c).isNotBlank()
        allItems = StoreCatalog.loadCached(getApplication(), c)
        error = null
        if (allItems.isEmpty() && urlConfigured) refresh()
    }

    fun currentUrl(): String = StoreSettings.getUrl(getApplication(), category)

    fun saveUrl(url: String) {
        StoreSettings.setUrl(getApplication(), category, url)
        urlConfigured = url.isNotBlank()
        if (urlConfigured) refresh()
    }

    fun refresh() {
        val cat = category
        val url = StoreSettings.getUrl(getApplication(), cat)
        if (url.isBlank()) { error = null; return }
        loading = true
        error = null
        viewModelScope.launch {
            try {
                val items = withContext(Dispatchers.IO) { StoreCatalog.refresh(getApplication(), cat, url) }
                if (category == cat) allItems = items
            } catch (e: Exception) {
                error = e.message ?: e.toString()
            } finally {
                loading = false
            }
        }
    }

    fun stateOf(item: StoreItem): DownloadState = states[item.key] ?: DownloadState.Idle

    fun cancel(item: StoreItem) {
        jobs.remove(item.key)?.cancel()
        states[item.key] = DownloadState.Idle
    }

    /** Download -> verify sha256 -> install through Vita3K's own PKG installer. */
    fun install(item: StoreItem) {
        if (jobs[item.key]?.isActive == true) return
        val app = getApplication<Application>()
        val dir = File(app.getExternalFilesDir(null) ?: app.cacheDir, "store")
        states[item.key] = DownloadState.Downloading(0, item.sizeBytes)

        jobs[item.key] = viewModelScope.launch {
            try {
                val file = withContext(Dispatchers.IO) {
                    StoreDownloader.download(
                        item = item,
                        targetDir = dir,
                        isActive = { isActive },
                        onProgress = { done, total -> states[item.key] = DownloadState.Downloading(done, total) },
                        onVerifying = { states[item.key] = DownloadState.Verifying }
                    )
                }
                states[item.key] = DownloadState.Installing
                val opId = InstallServiceController.startPkg(app, file.absolutePath, item.zrif, null)
                val finished = InstallServiceController.state.first {
                    it.operationId == opId && !it.installing && it.installResult != null
                }
                val ok = finished.installResult?.status == InstallResultStatus.SUCCESS
                if (ok) {
                    withContext(Dispatchers.IO) { file.delete() }
                    states[item.key] = DownloadState.Installed
                } else {
                    states[item.key] = DownloadState.Failed(finished.installResult?.message ?: "Install failed")
                }
            } catch (e: kotlinx.coroutines.CancellationException) {
                states[item.key] = DownloadState.Idle
                throw e
            } catch (e: Exception) {
                states[item.key] = DownloadState.Failed(e.message ?: e.toString())
            } finally {
                jobs.remove(item.key)
            }
        }
    }
}
