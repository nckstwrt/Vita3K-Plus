package org.vita3k.emulator.store

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

private fun formatSize(bytes: Long): String = when {
    bytes <= 0 -> ""
    bytes >= 1L shl 30 -> "%.2f GB".format(bytes / (1L shl 30).toDouble())
    else -> "%.1f MB".format(bytes / (1L shl 20).toDouble())
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StoreScreen(
    viewModel: StoreViewModel,
    installedTitleIds: Set<String>,
    onPlay: (titleId: String) -> Unit,
    onBack: () -> Unit
) {
    var showUrlDialog by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Store") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    IconButton(onClick = { viewModel.refresh() }, enabled = viewModel.urlConfigured) {
                        Icon(Icons.Default.Refresh, contentDescription = "Refresh catalog")
                    }
                    IconButton(onClick = { showUrlDialog = true }) {
                        Icon(Icons.Default.Settings, contentDescription = "Catalog URL")
                    }
                }
            )
        }
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            TabRow(selectedTabIndex = viewModel.category.ordinal) {
                StoreCategory.entries.forEach { c ->
                    Tab(
                        selected = c == viewModel.category,
                        onClick = { viewModel.selectCategory(c) },
                        text = { Text(c.label) }
                    )
                }
            }
            OutlinedTextField(
                value = viewModel.query,
                onValueChange = viewModel::setQuery,
                singleLine = true,
                label = { Text("Search name or title id") },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)
            )
            if (viewModel.loading) LinearProgressIndicator(Modifier.fillMaxWidth())

            when {
                !viewModel.urlConfigured -> Box(Modifier.fillMaxSize().padding(24.dp), Alignment.Center) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text("No catalog URL set for ${viewModel.category.label}.")
                        Spacer(Modifier.height(12.dp))
                        FilledTonalButton(onClick = { showUrlDialog = true }) { Text("Set catalog URL") }
                    }
                }
                viewModel.error != null -> Box(Modifier.fillMaxSize().padding(24.dp), Alignment.Center) {
                    Text("Failed to load catalog: ${viewModel.error}", color = MaterialTheme.colorScheme.error)
                }
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    items(viewModel.visibleItems, key = { it.key + it.url }) { item ->
                        StoreRow(
                            item = item,
                            state = viewModel.stateOf(item),
                            installed = item.titleId in installedTitleIds,
                            onInstall = { viewModel.install(item) },
                            onCancel = { viewModel.cancel(item) },
                            onPlay = { onPlay(item.titleId) }
                        )
                        HorizontalDivider()
                    }
                }
            }
        }
    }

    if (showUrlDialog) {
        var text by remember { mutableStateOf(viewModel.currentUrl()) }
        AlertDialog(
            onDismissRequest = { showUrlDialog = false },
            title = { Text("${viewModel.category.label} catalog URL") },
            text = {
                OutlinedTextField(
                    value = text, onValueChange = { text = it },
                    label = { Text("URL of the .tsv file") }, modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(onClick = { viewModel.saveUrl(text); showUrlDialog = false }) { Text("Save") }
            },
            dismissButton = { TextButton(onClick = { showUrlDialog = false }) { Text("Cancel") } }
        )
    }
}

@Composable
private fun StoreRow(
    item: StoreItem,
    state: DownloadState,
    installed: Boolean,
    onInstall: () -> Unit,
    onCancel: () -> Unit,
    onPlay: () -> Unit
) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(Modifier.weight(1f)) {
            Text(item.name, maxLines = 2, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyLarge)
            Text(
                listOf(item.region, item.titleId, formatSize(item.sizeBytes)).filter { it.isNotEmpty() }.joinToString(" · "),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            when (state) {
                is DownloadState.Downloading -> {
                    val frac = if (state.total > 0) state.done.toFloat() / state.total else 0f
                    LinearProgressIndicator(progress = { frac }, modifier = Modifier.fillMaxWidth().padding(top = 6.dp))
                    Text("${formatSize(state.done)} / ${formatSize(state.total)}", style = MaterialTheme.typography.labelSmall)
                }
                DownloadState.Verifying -> Text("Verifying SHA-256…", style = MaterialTheme.typography.labelSmall)
                DownloadState.Installing -> Text("Installing…", style = MaterialTheme.typography.labelSmall)
                is DownloadState.Failed -> Text(state.message, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.error)
                else -> {}
            }
        }
        Spacer(Modifier.width(12.dp))
        when {
            state is DownloadState.Downloading -> OutlinedButton(onClick = onCancel) { Text("Cancel") }
            state == DownloadState.Verifying || state == DownloadState.Installing -> CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp)
            installed && item.category != StoreCategory.DLCS -> Button(onClick = onPlay) { Text("Play") }
            installed -> OutlinedButton(onClick = onInstall) { Text("Reinstall") }
            else -> FilledTonalButton(onClick = onInstall) { Text("Install") }
        }
    }
}
