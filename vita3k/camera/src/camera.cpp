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

#include "config/state.h"

#include <algorithm>
#include <array>
#include <camera/camera.h>
#include <camera/state.h>
#include <cmath>
#include <map>
#include <rtc/rtc.h>
#include <stb_image.h>
#include <util/log.h>
#include <vector>

#include <SDL3/SDL_camera.h>
#include <SDL3/SDL_timer.h>

struct stbi_deleter {
    void operator()(stbi_uc *d) const { stbi_image_free(d); }
};

struct SDL_SurfaceDeleter {
    void operator()(SDL_Surface *s) const { SDL_DestroySurface(s); }
};

struct SDL_CameraDeleter {
    void operator()(SDL_Camera *c) const { SDL_CloseCamera(c); }
};

struct SDL_Deleter {
    void operator()(void *c) const { SDL_free(c); }
};

using SDL_SurfacePtr = std::unique_ptr<SDL_Surface, SDL_SurfaceDeleter>;
using stbi_uc_ptr = std::unique_ptr<stbi_uc, stbi_deleter>;
using SDL_CameraPtr = std::unique_ptr<SDL_Camera, SDL_CameraDeleter>;
using SDL_CameraListPtr = std::unique_ptr<SDL_CameraID[], SDL_Deleter>;

class Camera::CameraImpl {
public:
    SDL_CameraPtr sdl_camera;
    SDL_CameraID device_id;
    SDL_SurfacePtr frame;
    SDL_PixelFormat format;
    SDL_Colorspace colorspace;
    std::mutex frame_mutex;
    std::map<CameraAttributes, int> attributes;
    std::array<uint8_t, 256> ev_table{};
    int ev_table_value = 0;
};

Camera::Camera()
    : pImpl(std::make_unique<CameraImpl>()) {}
Camera::~Camera() = default;

struct CameraDevice {
    SDL_CameraID id;
    std::string name;
    SDL_CameraPosition position;
};

static std::vector<CameraDevice> list_cameras() {
    int num_cameras = 0;
    SDL_CameraListPtr cameras(SDL_GetCameras(&num_cameras));
    std::vector<CameraDevice> devices;
    if (!cameras) {
        LOG_DEBUG("SDL_GetCameras failed: {}", SDL_GetError());
        SDL_ClearError();
        return devices;
    }
    std::vector<SDL_CameraID> ids(cameras.get(), cameras.get() + num_cameras);
    std::sort(ids.begin(), ids.end());
    std::map<std::string, int> name_count;
    for (const SDL_CameraID id : ids) {
        const char *camera_name = SDL_GetCameraName(id);
        const std::string name = camera_name ? camera_name : "Camera";
        const int count = ++name_count[name];
        devices.push_back({ id, count == 1 ? name : fmt::format("{} {}", name, count), SDL_GetCameraPosition(id) });
    }
    return devices;
}

static SDL_CameraSpec choose_native_spec(SDL_CameraID device_id, const SDL_CameraSpec &wanted) {
    int count = 0;
    std::unique_ptr<SDL_CameraSpec *[], SDL_Deleter> specs(SDL_GetCameraSupportedFormats(device_id, &count));
    const auto covers = [&](const SDL_CameraSpec *spec) { return spec->width >= wanted.width && spec->height >= wanted.height; };
    const auto same_aspect = [&](const SDL_CameraSpec *spec) { return spec->width * wanted.height == spec->height * wanted.width; };
    const auto better = [&](const SDL_CameraSpec *a, const SDL_CameraSpec *b) {
        if (covers(a) != covers(b))
            return covers(a);
        if (same_aspect(a) != same_aspect(b))
            return same_aspect(a);
        const int area_a = a->width * a->height;
        const int area_b = b->width * b->height;
        if (area_a != area_b)
            return covers(a) ? area_a < area_b : area_a > area_b;
        return static_cast<int64_t>(a->framerate_numerator) * b->framerate_denominator > static_cast<int64_t>(b->framerate_numerator) * a->framerate_denominator;
    };
    const SDL_CameraSpec *best = nullptr;
    for (int i = 0; specs && i < count; i++) {
        if (specs[i]->format != SDL_PIXELFORMAT_MJPG && !SDL_ISPIXELFORMAT_FLOAT(specs[i]->format) && (!best || better(specs[i], best)))
            best = specs[i];
    }
    return best ? *best : wanted;
}

static bool init_web_camera(Camera *self) {
    // open web camera via SDL3
    self->pImpl->device_id = 0;
    for (const auto &camera : list_cameras()) {
        if (camera.name == self->id) {
            self->pImpl->device_id = camera.id;
            break;
        }
    }
    if (self->pImpl->device_id == 0) {
        LOG_ERROR("Web camera with id '{}' not found.", self->id);
        return false;
    }
    uint16_t framerate_numerator = self->info.framerate;
    uint16_t framerate_denominator = 1;
    if (self->info.framerate == SCE_CAMERA_FRAMERATE_3_FPS) {
        framerate_numerator = 375;
        framerate_denominator = 100;
    } else if (self->info.framerate == SCE_CAMERA_FRAMERATE_7_FPS) {
        framerate_numerator = 75;
        framerate_denominator = 10;
    }
    self->frame_interval_us = SDL_US_PER_SECOND * framerate_denominator / framerate_numerator;
    SDL_CameraSpec camera_spec{
        .format = self->pImpl->format, /**< Frame format */
        .colorspace = self->pImpl->colorspace, /**< Frame colorspace */
        .width = self->info.width, /**< Frame width */
        .height = self->info.height, /**< Frame height */
        .framerate_numerator = framerate_numerator, /**< Frame rate numerator ((num / denom) == FPS, (denom / num) == duration in seconds) */
        .framerate_denominator = framerate_denominator /**< Frame rate demoninator ((num / denom) == FPS, (denom / num) == duration in seconds) */
    };
    const SDL_CameraSpec native_spec = choose_native_spec(self->pImpl->device_id, camera_spec);
    self->pImpl->sdl_camera.reset(SDL_OpenCamera(self->pImpl->device_id, &native_spec));
    if (!self->pImpl->sdl_camera) {
        LOG_ERROR("Failed to open camera: {}", SDL_GetError());
        return false;
    }
    LOG_INFO("Camera '{}' opened at {}x{} {} ({}/{} fps); the game asked for {}x{} {}, Vita3K converts each frame", self->id,
        native_spec.width, native_spec.height, SDL_GetPixelFormatName(native_spec.format), native_spec.framerate_numerator, native_spec.framerate_denominator,
        camera_spec.width, camera_spec.height, SDL_GetPixelFormatName(camera_spec.format));
    return true;
}

static bool init_image(Camera *self) {
    FILE *f = FOPEN(fs_utils::utf8_to_path(self->image).c_str(), "rb");
    if (!f) {
        LOG_ERROR("Failed to open camera image file: {} Error: {}", self->image, strerror(errno));
        return false;
    }

    int32_t width = 0, height = 0;
    stbi_uc_ptr data(stbi_load_from_file(f, &width, &height, nullptr, STBI_rgb));
    fclose(f);
    if (!data) {
        LOG_ERROR("Failed to load camera image from {}: {}", self->image, stbi_failure_reason());
        return false;
    }

    SDL_SurfacePtr image_surface(SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGB24, data.get(), width * SDL_BYTESPERPIXEL(SDL_PIXELFORMAT_RGB24)));
    if (!image_surface) {
        LOG_ERROR("Failed to create SDL surface from image data: {}", SDL_GetError());
        return false;
    }

    if (width != self->info.width || height != self->info.height) {
        image_surface.reset(SDL_ScaleSurface(image_surface.get(), self->info.width, self->info.height, SDL_SCALEMODE_LINEAR));
        if (!image_surface) {
            LOG_ERROR("Failed to scale SDL surface: {}", SDL_GetError());
            return false;
        }
    }

    image_surface.reset(SDL_ConvertSurfaceAndColorspace(image_surface.get(), self->pImpl->format, nullptr, self->pImpl->colorspace, 0));
    if (!image_surface) {
        LOG_ERROR("Failed to convert SDL surface: {}", SDL_GetError());
        return false;
    }

    // If frame does not own its data then duplicate it
    if (image_surface->flags & SDL_SURFACE_PREALLOCATED) {
        image_surface.reset(SDL_DuplicateSurface(image_surface.get()));
        if (!image_surface) {
            LOG_ERROR("Failed to duplicate SDL surface: {}", SDL_GetError());
            return false;
        }
    }
    self->pImpl->frame = std::move(image_surface);

    return true;
}

static bool init_solid_color(Camera *self) {
    SDL_SurfacePtr image_surface(SDL_CreateSurface(self->info.width, self->info.height, SDL_PIXELFORMAT_ARGB8888));
    if (!image_surface) {
        LOG_ERROR("Failed to create SDL surface for solid color: {}", SDL_GetError());
        return false;
    }
    if (!SDL_FillSurfaceRect(image_surface.get(), nullptr, self->color)) {
        LOG_ERROR("Failed to fill SDL surface with solid color: {}", SDL_GetError());
        return false;
    }
    SDL_SurfacePtr converted_surface(SDL_ConvertSurfaceAndColorspace(image_surface.get(), self->pImpl->format, nullptr, self->pImpl->colorspace, 0));
    if (!converted_surface) {
        LOG_ERROR("Failed to convert SDL surface for solid color: {}", SDL_GetError());
        return false;
    }
    self->pImpl->frame = std::move(converted_surface);
    return true;
}

static void build_ev_table(Camera::CameraImpl &impl, int ev) {
    const double gain = std::exp2(ev / 10.0);
    for (int i = 0; i < 256; i++) {
        const double linear = std::min(1.0, std::pow(i / 255.0, 2.2) * gain);
        impl.ev_table[i] = static_cast<uint8_t>(std::lround(std::pow(linear, 1.0 / 2.2) * 255.0));
    }
    impl.ev_table_value = ev;
}

static SDL_Surface *convert_camera_frame(Camera *self, SDL_Surface *surface) {
    auto &impl = *self->pImpl;
    SDL_SurfacePtr rgb(SDL_ConvertSurfaceAndColorspace(surface, SDL_PIXELFORMAT_XRGB8888, nullptr, SDL_COLORSPACE_SRGB, 0));
    if (rgb && (rgb->w != self->info.width || rgb->h != self->info.height))
        rgb.reset(SDL_ScaleSurface(rgb.get(), self->info.width, self->info.height, SDL_SCALEMODE_LINEAR));
    if (!rgb) {
        LOG_ERROR_ONCE("Failed to convert a {}x{} {} camera frame: {}", surface->w, surface->h, SDL_GetPixelFormatName(surface->format), SDL_GetError());
        return nullptr;
    }
    const auto ev_it = impl.attributes.find(CameraAttributes::EV);
    const int ev = ev_it != impl.attributes.end() ? ev_it->second : 0;
    if (ev != 0) {
        if (ev != impl.ev_table_value)
            build_ev_table(impl, ev);
        for (int y = 0; y < rgb->h; y++) {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(rgb->pixels) + y * rgb->pitch);
            for (int x = 0; x < rgb->w; x++)
                row[x] = (row[x] & 0xFF000000) | (static_cast<uint32_t>(impl.ev_table[(row[x] >> 16) & 0xFF]) << 16) | (static_cast<uint32_t>(impl.ev_table[(row[x] >> 8) & 0xFF]) << 8) | impl.ev_table[row[x] & 0xFF];
        }
    }
    return SDL_ConvertSurfaceAndColorspace(rgb.get(), impl.format, nullptr, impl.colorspace, 0);
}

static void refresh_camera_image(Camera *self) {
    self->pImpl->sdl_camera.reset();
    self->pImpl->frame.reset();
    if (self->type == CameraType::WebCamera) {
        if (!init_web_camera(self))
            self->type = CameraType::Image;
    }
    if (self->type == CameraType::Image) {
        if (!init_image(self))
            self->type = CameraType::SolidColor;
    }
    if (self->type == CameraType::SolidColor) {
        if (!init_solid_color(self))
            self->pImpl->frame.reset();
    }
    if (self->type == CameraType::WebCamera)
        LOG_INFO("Camera source: device '{}'", self->id);
    else if (self->type == CameraType::Image)
        LOG_INFO("Camera source: image {}", self->image);
    else
        LOG_INFO("Camera source: solid colour #{:06X}{}", self->color & 0xFFFFFF, self->pImpl->frame ? "" : ", which could not be created (frames are black)");
}

int Camera::get_attribute(CameraAttributes attribute) {
    {
        const std::lock_guard lock(pImpl->frame_mutex);
        const auto it = pImpl->attributes.find(attribute);
        if (it != pImpl->attributes.end())
            return it->second;
    }
    switch (attribute) {
    case CameraAttributes::Saturation: return SCE_CAMERA_SATURATION_10;
    case CameraAttributes::Brightness: return 128; // 0-255
    case CameraAttributes::Contrast: return 128; // random default value  0-255
    case CameraAttributes::Sharpness: return SCE_CAMERA_SHARPNESS_100;
    case CameraAttributes::Reverse: return SCE_CAMERA_REVERSE_OFF;
    case CameraAttributes::Effect: return SCE_CAMERA_EFFECT_NORMAL;
    case CameraAttributes::EV: return 0;
    case CameraAttributes::Zoom: return 10; // 10-40
    case CameraAttributes::AntiFlicker: return SCE_CAMERA_ANTIFLICKER_AUTO;
    case CameraAttributes::ISO: return SCE_CAMERA_ISO_AUTO;
    case CameraAttributes::Gain: return SCE_CAMERA_GAIN_AUTO;
    case CameraAttributes::WhiteBalance: return SCE_CAMERA_WB_AUTO;
    case CameraAttributes::Backlight: return SCE_CAMERA_BACKLIGHT_OFF;
    case CameraAttributes::Nightmode: return SCE_CAMERA_NIGHTMODE_OFF;
    case CameraAttributes::ExposureCeiling: return 0;
    case CameraAttributes::AutoControlHold: return 0;
    // undescribed parameters
    case CameraAttributes::ImageQuality: return 0;
    case CameraAttributes::NoiseReduction: return 0;
    case CameraAttributes::SharpnessOff: return 0;
    }
    return 0;
}

bool Camera::set_attribute(CameraAttributes attribute, int value) {
    const std::lock_guard lock(pImpl->frame_mutex);
    const auto it = pImpl->attributes.find(attribute);
    if (it != pImpl->attributes.end() && it->second == value)
        return false;
    pImpl->attributes[attribute] = value;
    return true;
}

int Camera::open(SceCameraInfo *info, uint64_t base_tick, uint64_t start_tick) {
    tick_diff_us = rtc_get_ticks(base_tick) - start_tick - (SDL_GetTicksNS() / SDL_NS_PER_US);
    {
        const std::lock_guard lock(pImpl->frame_mutex);
        pImpl->attributes.clear();
    }
    this->is_opened = true;
    this->info = {};
    memcpy(&(this->info), info, std::min<size_t>(sizeof(SceCameraInfo), info->size));
    switch (this->info.format) {
    case SCE_CAMERA_FORMAT_YUV420_PLANE: // YUV 4:2:0 planar
        this->pImpl->format = SDL_PIXELFORMAT_YV12;
        this->pImpl->colorspace = SDL_COLORSPACE_YUV_DEFAULT;
        break;
    case SCE_CAMERA_FORMAT_YUV422_PLANE: // YUV 4:2:2 planar
        this->pImpl->format = SDL_PIXELFORMAT_YUY2; // use packed format cause SDL does not have YUV 4:2:2 planar
        this->pImpl->colorspace = SDL_COLORSPACE_YUV_DEFAULT;
        break;
    case SCE_CAMERA_FORMAT_YUV422_PACKED: // YUV 4:2:2 packed
        this->pImpl->format = SDL_PIXELFORMAT_YUY2;
        this->pImpl->colorspace = SDL_COLORSPACE_YUV_DEFAULT;
        break;
    case SCE_CAMERA_FORMAT_ARGB: // ARGB format
        this->pImpl->format = SDL_PIXELFORMAT_ARGB8888;
        this->pImpl->colorspace = SDL_COLORSPACE_RGB_DEFAULT;
        break;
    case SCE_CAMERA_FORMAT_ABGR: // ABGR format
        this->pImpl->format = SDL_PIXELFORMAT_ABGR8888;
        this->pImpl->colorspace = SDL_COLORSPACE_RGB_DEFAULT;
        break;
    case SCE_CAMERA_FORMAT_RAW8: // unsupported, use default
    default:
        this->pImpl->format = SDL_PIXELFORMAT_ARGB8888; // ABGR format
        this->pImpl->colorspace = SDL_COLORSPACE_RGB_DEFAULT;
    }

    if (this->info.range == SCE_CAMERA_DATA_RANGE_BT601) {
        this->pImpl->colorspace = SDL_COLORSPACE_BT601_LIMITED;
    }

    return 0;
}

int Camera::start() {
    const std::lock_guard lock(pImpl->frame_mutex);
    refresh_camera_image(this);
    this->is_started = true;
    this->frame_idx = 0;
    return 0;
}

int Camera::read(SceCameraRead *read, void *pIBase, void *pUBase, void *pVBase, SceSize sizeIBase, SceSize sizeUBase, SceSize sizeVBase) {
    const std::lock_guard lock(pImpl->frame_mutex);
    constexpr uint64_t minimal_delay_ns = SDL_NS_PER_US; // arbitrary value
    uint64_t sdl_current_time_ns = SDL_GetTicksNS();
    uint64_t sdl_current_time_us = sdl_current_time_ns / SDL_NS_PER_US;
    if (next_frame_timestamp_us == 0) {
        next_frame_timestamp_us = sdl_current_time_us;
    } else {
        uint64_t wait_time_ns = (sdl_current_time_ns < next_frame_timestamp_us * SDL_NS_PER_US) ? (next_frame_timestamp_us * SDL_NS_PER_US - sdl_current_time_ns) : 0;
        if (wait_time_ns > minimal_delay_ns) {
            if (read->mode == SCE_CAMERA_READ_MODE_WAIT_NEXTFRAME_ON) {
                SDL_DelayPrecise(wait_time_ns);
            } else {
                read->frame = frame_idx;
                read->timestamp = last_frame_timestamp_us;
                read->status = SCE_CAMERA_STATUS_IS_ALREADY_READ;
                return 0;
            }
        }
    }
    // next frame
    last_frame_timestamp_us = next_frame_timestamp_us;
    next_frame_timestamp_us = next_frame_timestamp_us + frame_interval_us;
    if (type == CameraType::WebCamera) {
        Uint64 timestampNS = 0;
        auto surface = SDL_AcquireCameraFrame(pImpl->sdl_camera.get(), &timestampNS);
        // drains all buffered frames to get the latest one
        Uint64 next_timestampNS;
        auto next_surface = SDL_AcquireCameraFrame(pImpl->sdl_camera.get(), &next_timestampNS);
        int i = 16; // Safety counter to avoid endless loop.
                    // sdl buffer is 8 surfaces, so if we read much more than 8 times, something go really wrong
        while (next_surface && i > 0) {
            SDL_ReleaseCameraFrame(pImpl->sdl_camera.get(), surface);
            surface = next_surface;
            timestampNS = next_timestampNS;
            next_surface = SDL_AcquireCameraFrame(pImpl->sdl_camera.get(), &next_timestampNS);
            i--;
        }
        if (next_surface)
            SDL_ReleaseCameraFrame(pImpl->sdl_camera.get(), next_surface);
        if (surface) {
            pImpl->frame.reset(convert_camera_frame(this, surface));
            SDL_ReleaseCameraFrame(pImpl->sdl_camera.get(), surface);
            last_frame_timestamp_us = timestampNS / SDL_NS_PER_US + tick_diff_us;
        }
    }
    read->frame = frame_idx++;
    read->status = SCE_CAMERA_STATUS_IS_ACTIVE;
    read->timestamp = last_frame_timestamp_us;
    if (pImpl->frame) {
        sizeIBase = std::min(sizeIBase, (SceSize)(pImpl->frame->pitch * pImpl->frame->h)); // here pitch is width in bytes
        if (this->info.format == SCE_CAMERA_FORMAT_YUV420_PLANE) {
            sizeUBase = std::min(sizeUBase, (SceSize)(pImpl->frame->pitch * pImpl->frame->h / 4));
            sizeVBase = std::min(sizeVBase, (SceSize)(pImpl->frame->pitch * pImpl->frame->h / 4));
            memcpy(pIBase, (uint8_t *)pImpl->frame->pixels, sizeIBase);
            memcpy(pVBase, (uint8_t *)pImpl->frame->pixels + (pImpl->frame->pitch * pImpl->frame->h), sizeUBase);
            memcpy(pUBase, (uint8_t *)pImpl->frame->pixels + (pImpl->frame->pitch * pImpl->frame->h * 5 / 4), sizeVBase);
        } else if (this->info.format == SCE_CAMERA_FORMAT_YUV422_PLANE) {
            // convert SDL_PIXELFORMAT_YUY2 Y0+U0+Y1+V0 to planar format
            const int width = pImpl->frame->w;
            const int height = pImpl->frame->h;
            const bool chroma_fits = sizeUBase >= (SceSize)((width / 2) * height) && sizeVBase >= (SceSize)((width / 2) * height);
            if (sizeIBase < (SceSize)(width * height) || !chroma_fits) {
                LOG_ERROR_ONCE("Buffer sizes too small for YUV422 planar conversion: IBase {}, UBase {}, VBase {}, required I {}, U {}, V {}.", sizeIBase, sizeUBase, sizeVBase, width * height, (width / 2) * height, (width / 2) * height);
                if (sizeIBase < (SceSize)(width * height))
                    return SCE_CAMERA_ERROR_PARAM;
                // the luma plane still fits: convert it and fill the short chroma planes with neutral grey
                if (pUBase && sizeUBase)
                    memset(pUBase, 128, sizeUBase);
                if (pVBase && sizeVBase)
                    memset(pVBase, 128, sizeVBase);
            }
            if (!SDL_LockSurface(pImpl->frame.get())) {
                LOG_ERROR("Failed to lock camera frame surface: {}", SDL_GetError());
                goto BAD_FRAME;
            };
            uint8_t const *packed = (uint8_t *)pImpl->frame->pixels;
            uint8_t *Y = (uint8_t *)pIBase;
            uint8_t *U = (uint8_t *)pUBase;
            uint8_t *V = (uint8_t *)pVBase;
            int pitch = pImpl->frame->pitch;
            for (ptrdiff_t y = 0; y < height; y++) {
                uint8_t const *row = packed + y * pitch;
                for (ptrdiff_t x = 0; x < width; x += 2) {
                    ptrdiff_t pair_idx = x / 2;
                    Y[y * width + x] = row[pair_idx * 4];
                    Y[y * width + x + 1] = row[pair_idx * 4 + 2];
                    if (chroma_fits) {
                        U[y * (width / 2) + pair_idx] = row[pair_idx * 4 + 1];
                        V[y * (width / 2) + pair_idx] = row[pair_idx * 4 + 3];
                    }
                }
            }
            SDL_UnlockSurface(pImpl->frame.get());
        } else {
            memcpy(pIBase, pImpl->frame->pixels, sizeIBase);
        }
    } else {
    BAD_FRAME:
        LOG_ERROR_ONCE("No camera frame available.");
        memset(pIBase, 0, sizeIBase);
        if (pUBase && sizeUBase)
            memset(pUBase, 127, sizeUBase);
        if (pVBase && sizeVBase)
            memset(pVBase, 127, sizeVBase);
    }
    return 0;
}

int Camera::close() {
    this->is_started = false;
    this->is_opened = false;
    return 0;
}

int Camera::stop() {
    const std::lock_guard lock(pImpl->frame_mutex);
    this->is_started = false;
    pImpl->frame.reset();
    pImpl->sdl_camera.reset();
    return 0;
}

void Camera::update_config(int type, const std::string &id, const std::string &image, uint32_t color) {
    const std::lock_guard lock(pImpl->frame_mutex);
    if (static_cast<int>(this->type) == type && this->id == id && this->image == image && this->color == color)
        return;
    this->type = static_cast<CameraType>(type);
    this->id = id;
    this->image = image;
    this->color = color;
    if (is_started) {
        LOG_INFO("Camera settings changed while the game has the camera running, switching source now");
        refresh_camera_image(this);
    }
}

void CameraState::deinit() {
    for (auto &cam : cameras) {
        if (cam.is_started)
            cam.stop();

        cam.is_opened = false;
        cam.is_started = false;
        cam.info = {};
        cam.frame_idx = 0;
        cam.last_frame_timestamp_us = 0;
        cam.next_frame_timestamp_us = 0;
        cam.frame_interval_us = 0;
        cam.tick_diff_us = 0;
    }
    active_camera = SCE_CAMERA_DEVICE_UNKNOWN;
}

void CameraState::apply_config(Config &cfg) {
    const std::lock_guard lock(config_mutex);
    init_default_cameras(cfg);
    front()->update_config(cfg.front_camera_type, cfg.front_camera_id, cfg.front_camera_image, cfg.front_camera_color);
    back()->update_config(cfg.back_camera_type, cfg.back_camera_id, cfg.back_camera_image, cfg.back_camera_color);
}

BOOST_DESCRIBE_ENUM(SDL_CameraPosition, SDL_CAMERA_POSITION_UNKNOWN, SDL_CAMERA_POSITION_FRONT_FACING, SDL_CAMERA_POSITION_BACK_FACING)

void init_default_cameras(Config &cfg) {
    const bool front_needed = cfg.front_camera_type == WebCamera && cfg.front_camera_id.empty();
    const bool back_needed = cfg.back_camera_type == WebCamera && cfg.back_camera_id.empty();
    if (!front_needed && !back_needed)
        return;
    LOG_DEBUG("Initialize default cameras...");
    const auto cameras = list_cameras();
    std::string front_id;
    std::string back_id;
    for (const auto &camera : cameras) {
        LOG_TRACE("Found camera: name='{}' position={}", camera.name, camera.position);
        if (camera.position == SDL_CAMERA_POSITION_FRONT_FACING && front_id.empty())
            front_id = camera.name;
        if (camera.position == SDL_CAMERA_POSITION_BACK_FACING && back_id.empty())
            back_id = camera.name;
    }
    if (!cameras.empty()) {
        if (front_id.empty())
            front_id = cameras.front().name;
        if (back_id.empty())
            back_id = cameras.front().name;
    }
    if (front_needed) {
        if (front_id.empty())
            cfg.front_camera_type = SolidColor;
        else
            cfg.front_camera_id = front_id;
    }
    if (back_needed) {
        if (back_id.empty())
            cfg.back_camera_type = SolidColor;
        else
            cfg.back_camera_id = back_id;
    }
}

std::vector<std::string> get_camera_names() {
    std::vector<std::string> names;
    for (const auto &camera : list_cameras())
        names.push_back(camera.name);
    return names;
}
