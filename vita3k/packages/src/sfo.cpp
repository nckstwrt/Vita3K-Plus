// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

/**
 * @file sfo.cpp
 * @brief PlayStation setting file (`.sfo`) handling
 *
 * PlayStation setting files (`.sfo`) contain metadata information usually describing
 * the content they are accompanying.
 */

#include <packages/sfo.h>

#include <util/log.h>

#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

namespace sfo {

bool get_data_by_id(std::string &out_data, SfoFile &file, int id) {
    std::string key;
    switch (id) {
    case 6:
        key = "CONTENT_ID";
        break;
    case 7:
        key = "NP_COMMUNICATION_ID";
        break;
    case 8:
        key = "CATEGORY";
        break;
    case 9:
        key = "TITLE";
        break;
    case 10:
        key = "STITLE";
        break;
    case 0xc:
        key = "TITLE_ID";
        break;
    case 0xe: // Todo
    default:
        return false;
    }

    return get_data_by_key(out_data, file, key);
}

bool get_data_by_key(std::string &out_data, SfoFile &file, const std::string &key) {
    auto res = std::find_if(file.entries.begin(), file.entries.end(),
        [key](const auto &et) { return et.data.first == key; });

    if (res == file.entries.end()) {
        return false;
    }
    out_data = res->data.second;

    return true;
}

bool get_param_info(sfo::SfoAppInfo &app_info, const vfs::FileBuffer &param, int sys_lang) {
    SfoFile sfo_handle;
    app_info = {};
    if (!sfo::load(sfo_handle, param))
        return false;
    sfo::get_data_by_key(app_info.app_version, sfo_handle, "APP_VER");
    if (!app_info.app_version.empty() && app_info.app_version[0] == '0')
        app_info.app_version.erase(app_info.app_version.begin());
    sfo::get_data_by_key(app_info.app_category, sfo_handle, "CATEGORY");
    sfo::get_data_by_key(app_info.app_content_id, sfo_handle, "CONTENT_ID");
    if (!sfo::get_data_by_key(app_info.app_addcont, sfo_handle, "INSTALL_DIR_ADDCONT"))
        sfo::get_data_by_key(app_info.app_addcont, sfo_handle, "TITLE_ID");
    if (!sfo::get_data_by_key(app_info.app_savedata, sfo_handle, "INSTALL_DIR_SAVEDATA"))
        sfo::get_data_by_key(app_info.app_savedata, sfo_handle, "TITLE_ID");
    sfo::get_data_by_key(app_info.app_parental_level, sfo_handle, "PARENTAL_LEVEL");
    if (!sfo::get_data_by_key(app_info.app_short_title, sfo_handle, fmt::format("STITLE_{:0>2d}", sys_lang)))
        sfo::get_data_by_key(app_info.app_short_title, sfo_handle, "STITLE");
    if (!sfo::get_data_by_key(app_info.app_title, sfo_handle, fmt::format("TITLE_{:0>2d}", sys_lang)))
        sfo::get_data_by_key(app_info.app_title, sfo_handle, "TITLE");
    std::replace(app_info.app_title.begin(), app_info.app_title.end(), '\n', ' ');
    boost::trim(app_info.app_title);
    sfo::get_data_by_key(app_info.app_title_id, sfo_handle, "TITLE_ID");
    return true;
}

bool is_safe_folder_name(const std::string &name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos && name.find('\0') == std::string::npos;
}

bool load(SfoFile &sfile, const std::vector<uint8_t> &content) {
    sfile = {};
    if (content.empty()) {
        return false;
    }

    if (content.size() < sizeof(SfoHeader)) {
        LOG_ERROR("param.sfo rejected: buffer too small ({} bytes, header needs {})", content.size(), sizeof(SfoHeader));
        return false;
    }

    memcpy(&sfile.header, content.data(), sizeof(SfoHeader));

    if (sfile.header.magic != 0x46535000) {
        LOG_ERROR("param.sfo rejected: bad magic 0x{:08X} (expected 0x46535000), buffer size {}", sfile.header.magic, content.size());
        return false;
    }

    const size_t entry_count = sfile.header.tables_entries;
    const size_t key_table_start = sfile.header.key_table_start;
    const size_t data_table_start = sfile.header.data_table_start;
    if (entry_count > (content.size() - sizeof(SfoHeader)) / sizeof(SfoIndexTableEntry)
        || key_table_start < sizeof(SfoHeader) + entry_count * sizeof(SfoIndexTableEntry)
        || data_table_start < key_table_start || data_table_start > content.size()) {
        LOG_ERROR("param.sfo rejected: {} entries with the key table at {} and the data table at {} do not fit {} bytes", entry_count, key_table_start, data_table_start, content.size());
        return false;
    }

    bool all_parsed = true;
    sfile.entries.resize(entry_count);
    for (size_t i = 0; i < entry_count; i++) {
        SfoFile::SfoEntry &entry = sfile.entries[i];
        memcpy(&entry.entry, content.data() + sizeof(SfoHeader) + i * sizeof(SfoIndexTableEntry), sizeof(SfoIndexTableEntry));

        const size_t key_offset = entry.entry.key_offset;
        const size_t key_table_size = data_table_start - key_table_start;
        if (key_offset >= key_table_size) {
            LOG_ERROR("param.sfo rejected: entry {} key offset {} is outside the {} byte key table", i, key_offset, key_table_size);
            return false;
        }
        const uint8_t *const key_begin = content.data() + key_table_start + key_offset;
        const auto *const key_end = static_cast<const uint8_t *>(memchr(key_begin, '\0', key_table_size - key_offset));
        if (!key_end) {
            LOG_ERROR("param.sfo rejected: entry {} key is not terminated inside the key table", i);
            return false;
        }
        entry.data.first.assign(reinterpret_cast<const char *>(key_begin), static_cast<size_t>(key_end - key_begin));

        const size_t data_offset = entry.entry.data_offset;
        const size_t data_len = entry.entry.data_len;
        if (data_offset > content.size() - data_table_start || data_len > content.size() - data_table_start - data_offset) {
            LOG_ERROR("param.sfo rejected: {} data at {} ({} bytes) is outside the {} byte buffer", entry.data.first, data_offset, data_len, content.size());
            return false;
        }
        const char *const data = reinterpret_cast<const char *>(content.data() + data_table_start + data_offset);

        // Interpret and convert the raw data based on its format
        switch (entry.entry.data_fmt) {
        case SfoDataFormat::UINT32_T: {
            if (data_len < sizeof(uint32_t)) {
                LOG_ERROR("param.sfo rejected: {} holds {} bytes for a 32-bit integer", entry.data.first, data_len);
                return false;
            }
            uint32_t value;
            memcpy(&value, data, sizeof(value));
            entry.data.second = std::to_string(value);
            break;
        }
        case SfoDataFormat::ASCII:
        case SfoDataFormat::UTF8:
            // Interpret the data as a raw string (may not be null-terminated)
            entry.data.second.assign(data, data_len);
            break;
        case SfoDataFormat::UTF8_NULL: {
            // up to the terminator, which a well-formed entry counts in its length
            const auto *const terminator = static_cast<const char *>(memchr(data, '\0', data_len));
            entry.data.second.assign(data, terminator ? static_cast<size_t>(terminator - data) : data_len);
            break;
        }
        default:
            // Unknown or unsupported data format: its value stays empty, the other entries are still read
            all_parsed = false;
            break;
        }
    }

    return all_parsed;
}

} // namespace sfo
