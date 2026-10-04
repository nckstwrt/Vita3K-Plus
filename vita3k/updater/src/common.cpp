#include <updater/functions.h>

#include <config/version.h>
#include <fmt/format.h>

namespace updater {

std::string release_api_url() {
    return "https://api.github.com/repos/nckstwrt/Vita3K-Plus/releases/latest";
}

std::string release_page_url() {
    return "https://github.com/nckstwrt/Vita3K-Plus/releases/latest";
}

std::string display_version(const UpdateInfo &info) {
    if (!info.version.empty())
        return fmt::format("{} ({})", info.version, info.build_number);

    return fmt::format("Build {}", info.build_number);
}

std::string current_display_version() {
    return fmt::format("{} ({})", app_version, app_number);
}

bool is_official_build() {
    return ::is_official_build;
}

} // namespace updater
