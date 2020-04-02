// SPDX-License-Identifier: GPL-3.0-only

#include <invader/tag/parser/parser.hpp>
#include <invader/tag/hek/class/bitmap.hpp>
#include <invader/build/build_workload.hpp>
#include <invader/resource/hek/ipak.hpp>

namespace Invader::Parser {
    template <typename T> static bool power_of_two(T value) {
        while(value > 1) {
            if(value & 1) {
                return false;
            }
            value >>= 1;
        }
        return true;
    }

    static std::size_t size_of_bitmap(const BitmapData &data) {
        std::size_t size = 0;
        std::size_t bits_per_pixel = 0;

        switch(data.format) {
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8R8G8B8:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_X8R8G8B8:
                bits_per_pixel = 32;
                break;
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_R5G6B5:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A1R5G5B5:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A4R4G4B4:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8Y8:
                bits_per_pixel = 16;
                break;
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_P8_BUMP:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_AY8:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_Y8:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5:
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3:
                bits_per_pixel = 8;
                break;
            case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1:
                bits_per_pixel = 4;
                break;
            default:
                eprintf_error("Unknown format %u", static_cast<unsigned int>(data.format));
                //std::terminate();
                throw std::exception();
        }

        std::size_t height = data.height;
        std::size_t width = data.width;
        std::size_t depth = data.depth;
        bool should_be_compressed = data.flags.compressed;
        std::size_t multiplier = data.type == HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP ? 6 : 1;

        // Do it
        for(std::size_t i = 0; i <= data.mipmap_count; i++) {
            size += width * height * depth * multiplier * bits_per_pixel / 8;

            // Divide by 2, resetting back to 1 when needed
            width /= 2;
            height /= 2;
            depth /= 2;
            if(width == 0) {
                width = 1;
            }
            if(height == 0) {
                height = 1;
            }
            if(depth == 0) {
                depth = 1;
            }

            // If we're DXT compressed, the minimum is actually 4x4
            if(should_be_compressed) {
                if(width < 4) {
                    width = 4;
                }
                if(height < 4) {
                    height = 4;
                }
            }
        }

        return size;
    }

    void BitmapData::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t struct_index, std::size_t offset) {
        auto &s = workload.structs[struct_index];
        auto *data = s.data.data();
        std::size_t bitmap_data_offset = reinterpret_cast<std::byte *>(&(reinterpret_cast<BitmapData::struct_little *>(data + offset)->bitmap_tag_id)) - data;
        this->pointer = 0xFFFFFFFF;
        this->flags.external = workload.engine_target == HEK::CacheFileEngine::CACHE_FILE_ANNIVERSARY; // anniversary is always external
        this->flags.make_it_actually_work = 1;

        // Add itself as a dependency. I don't know why but apparently we need to remind ourselves that we're still ourselves.
        auto &d = s.dependencies.emplace_back();
        d.tag_index = tag_index;
        d.offset = bitmap_data_offset;
        d.tag_id_only = true;
    }

    template <typename T> static void do_post_cache_parse(T *bitmap, const Invader::Tag &tag) {
        bitmap->postprocess_hek_data();

        auto &map = tag.get_map();
        auto engine = map.get_engine();
        auto xbox = engine == HEK::CacheFileEngine::CACHE_FILE_XBOX;
        auto mcc = engine == HEK::CacheFileEngine::CACHE_FILE_ANNIVERSARY;

        // TODO: Deal with cubemaps and stuff
        if(xbox && bitmap->type != HEK::BitmapType::BITMAP_TYPE_2D_TEXTURES) {
            eprintf_error("Non-2D bitmaps from Xbox maps are not currently supported");
            throw InvalidTagDataException();
        }

        const auto *path_cstr = tag.get_path().c_str();
        auto bd_count = bitmap->bitmap_data.size();
        for(std::size_t bd = 0; bd < bd_count; bd++) {
            auto &bitmap_data = bitmap->bitmap_data[bd];

            // TODO: Generate last two mipmaps if needed
            if(bitmap_data.flags.compressed && xbox) {
                eprintf_error("Compressed bitmaps from Xbox maps are not currently supported");
                throw InvalidTagDataException();
            }

            // MCC has meme matching (e.g. hce_ltb_bloodgulch = levels/test/bloodgulch/bloodgulch)
            const std::byte *bitmap_data_ptr = nullptr;
            if(mcc) {
                const auto &ipak_data = map.get_ipak_data();
                char path[0x100] = {};
                auto offset = std::snprintf(path, sizeof(path), "hce_");
                const char *i = path_cstr;
                const char *last_backslash = i;

                // Get the shortened bit
                for(; *i != 0; i++) {
                    if(*i == '\\') {
                        if(offset == sizeof(path)) {
                            break;
                        }
                        path[offset++] = *(last_backslash);
                        last_backslash = i + 1;
                    }
                }
                path[offset++] = '_';

                // Get the rest of the path
                if(last_backslash) {
                    for(i = last_backslash; *i; i++) {
                        if(offset == sizeof(path)) {
                            break;
                        }

                        if(*i == ' ' || *i == '-' || *i == '_') {
                            path[offset++] = '_';
                        }
                        else {
                            path[offset++] = *i;
                        }
                    }
                }

                // And lastly, the index
                std::snprintf(path + offset, sizeof(path) - offset, "_%zu", bd);

                for(auto &d : ipak_data) {
                    if(d.path == path) {
                        auto &header = *reinterpret_cast<const HEK::IPAKBitmapHeader *>(d.data.data());

                        // The bitmap data is often times bullshit, so we have to fix it
                        bitmap_data.height = header.height;
                        bitmap_data.width = header.width;
                        bitmap_data.depth = header.depth;
                        bitmap_data.mipmap_count = header.mipmap_count - 1;
                        bitmap_data.flags.power_of_two_dimensions = power_of_two(header.width.read()) && power_of_two(header.height.read());

                        // The format might be wrong for whatever reason
                        switch(header.format) {
                            case 0x0:
                            case 0x16:
                                bitmap_data.format = HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8R8G8B8;
                                break;
                            case 0xD:
                                bitmap_data.format = HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1;
                                break;
                            case 0x3:
                                bitmap_data.format = HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3;
                                break;
                            case 0x11:
                                bitmap_data.format = HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5;
                                break;
                        }

                        bitmap_data.pixel_data_size = size_of_bitmap(bitmap_data);
                        auto remaining_size = d.data.size() - sizeof(header);
                        if(bitmap_data.pixel_data_size <= remaining_size) {
                            bitmap_data_ptr = reinterpret_cast<const std::byte *>(&header + 1);
                        }
                        else {
                            eprintf_error("Size check fail: %zu expected > %zu remaining", static_cast<std::size_t>(bitmap_data.pixel_data_size), remaining_size);
                        }
                        break;
                    }
                }

                if(!bitmap_data_ptr) {
                    eprintf_error("Failed to find %s in the ipak", path);
                    throw std::exception();
                }
            }
            else {
                if(bitmap_data.flags.external) {
                    bitmap_data_ptr = map.get_data_at_offset(bitmap_data.pixel_data_offset, bitmap_data.pixel_data_size, Map::DATA_MAP_BITMAP);
                }
                else {
                    bitmap_data_ptr = map.get_internal_asset(bitmap_data.pixel_data_offset, bitmap_data.pixel_data_size);
                }
            }

            bitmap_data.pixel_data_offset = static_cast<std::size_t>(bitmap->processed_pixel_data.size());
            bitmap->processed_pixel_data.insert(bitmap->processed_pixel_data.end(), bitmap_data_ptr, bitmap_data_ptr + bitmap_data.pixel_data_size);

            // Unset these flags
            bitmap_data.flags.external = 0;
            bitmap_data.flags.make_it_actually_work = 0;
        }
    }

    template <typename T> static void do_pre_compile(T *bitmap, BuildWorkload &workload, std::size_t tag_index) {
        for(auto &sequence : bitmap->bitmap_group_sequence) {
            for(auto &sprite : sequence.sprites) {
                if(sprite.bitmap_index >= bitmap->bitmap_data.size()) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Sprite %zu of sequence %zu has an invalid bitmap index", &sprite - sequence.sprites.data(), &sequence - bitmap->bitmap_group_sequence.data());
                    throw InvalidTagDataException();
                }
            }
        }

        while(bitmap->bitmap_group_sequence.size() > 0 && bitmap->bitmap_group_sequence[bitmap->bitmap_group_sequence.size() - 1].first_bitmap_index == NULL_INDEX) {
            bitmap->bitmap_group_sequence.erase(bitmap->bitmap_group_sequence.begin() + (bitmap->bitmap_group_sequence.size() - 1));
        }

        auto max_size = bitmap->processed_pixel_data.size();
        auto *pixel_data = bitmap->processed_pixel_data.data();

        for(auto &data : bitmap->bitmap_data) {
            // DXTn bitmaps cannot be swizzled
            if(data.flags.swizzled && data.flags.compressed) {
                eprintf_error("Swizzled bitmaps are not supported for compressed bitmaps");
                throw InvalidTagDataException();
            }

            // TODO: Swizzle bitmaps for Dark Circlet, deswizzle for Gearbox
            if(data.flags.swizzled) {
                eprintf_error("Swizzled bitmaps are not currently supported");
                throw InvalidTagDataException();
            }

            std::size_t data_index = &data - bitmap->bitmap_data.data();
            bool compressed = data.flags.compressed;
            auto format = data.format;
            auto type = data.type;
            bool should_be_compressed = (format == HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1) || (format == HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3) || (format == HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5);

            std::size_t depth = data.depth;
            std::size_t start = data.pixel_data_offset;
            std::size_t width = data.width;
            std::size_t height = data.height;

            // Warn for stuff
            if(!workload.hide_pedantic_warnings) {
                bool exceeded = false;
                bool non_power_of_two = (!power_of_two(height) || !power_of_two(width) || !power_of_two(depth));
                if(bitmap->type != HEK::BitmapType::BITMAP_TYPE_INTERFACE_BITMAPS && non_power_of_two) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Data #%zu is non-power-of-two (%zux%zux%zu)", data_index, width, height, depth);
                    exceeded = true;
                }

                if(data.flags.power_of_two_dimensions && non_power_of_two) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_ERROR, tag_index, "Data #%zu is non-power-of-two (%zux%zux%zu) but that flag is set", data_index, width, height, depth);
                }

                if(!data.flags.power_of_two_dimensions && !non_power_of_two) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_ERROR, tag_index, "Data #%zu is power-of-two (%zux%zux%zu) but that flag is not set", data_index, width, height, depth);
                }

                if((
                    workload.engine_target == HEK::CacheFileEngine::CACHE_FILE_CUSTOM_EDITION ||
                    workload.engine_target == HEK::CacheFileEngine::CACHE_FILE_RETAIL ||
                    workload.engine_target == HEK::CacheFileEngine::CACHE_FILE_DEMO
                ) && !workload.hide_pedantic_warnings) {
                    switch(type) {
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_2D_TEXTURE:
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_WHITE:
                            if(width > 2048 || height > 2048) {
                                 REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Bitmap data #%zu exceeds 2048x2048 (%zux%zu)", data_index, width, height);
                                 exceeded = true;
                            }
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE:
                            if(width > 256 || height > 256 || depth > 256) {
                                 REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Bitmap data #%zu exceeds 256x256x256 (%zux%zu)", data_index, width, height);
                                 exceeded = true;
                            }
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP:
                            if(width > 512 || height > 512) {
                                 REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Bitmap data #%zu exceeds 512x512 (%zux%zu)", data_index, width, height);
                                 exceeded = true;
                            }
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_ENUM_COUNT:
                            break;
                    }
                    if(exceeded) {
                        eprintf_warn("Target engine uses D3D9; some D3D9 compliant hardware may not render this bitmap");
                    }
                }
            }

            if(depth != 1 && type != HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_ERROR, tag_index, "Bitmap data #%zu is not a 3D texture but has depth (%zu != 1)", data_index, depth);
            }

            // Make sure these are equal
            if(compressed != should_be_compressed) {
                const char *format_name = HEK::bitmap_data_format_name(format);
                if(compressed) {
                    data.flags.compressed = 0;
                    //REPORT_ERROR_PRINTF(workload, ERROR_TYPE_ERROR, tag_index, "Bitmap data #%zu (format: %s) is incorrectly marked as compressed", data_index, format_name);
                }
                else {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_ERROR, tag_index, "Bitmap data #%zu (format: %s) is not marked as compressed", data_index, format_name);
                }
            }

            auto size = size_of_bitmap(data);

            // Make sure we won't explode
            std::size_t end = start + size;
            if(start > max_size || size > max_size || end > max_size) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Bitmap data #%zu range (0x%08zX - 0x%08zX) exceeds the processed pixel data size (0x%08zX)", data_index, start, end, max_size);
                throw InvalidTagDataException();
            }

            // Add it all
            std::size_t raw_data_index = workload.raw_data.size();
            workload.raw_data.emplace_back(pixel_data + start, pixel_data + end);
            workload.tags[tag_index].asset_data.emplace_back(raw_data_index);
            data.pixel_data_size = static_cast<std::uint32_t>(size);
        }
    }

    template <typename T> static void do_postprocess_hek_data(T *bitmap) {
        if(bitmap->compressed_color_plate_data.size() == 0) {
            bitmap->color_plate_height = 0;
            bitmap->color_plate_width = 0;
        }
    }

    void Bitmap::postprocess_hek_data() {
        do_postprocess_hek_data(this);
    }

    void Bitmap::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer>) {
        do_post_cache_parse(this, tag);
    }

    void Bitmap::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t, std::size_t) {
        do_pre_compile(this, workload, tag_index);
    }

    void ExtendedBitmap::postprocess_hek_data() {
        do_postprocess_hek_data(this);
    }

    void ExtendedBitmap::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer>) {
        do_post_cache_parse(this, tag);
    }

    void ExtendedBitmap::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t, std::size_t) {
        do_pre_compile(this, workload, tag_index);
        if(this->data_metadata.size() != this->bitmap_data.size()) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Metadata count does not match bitmap data count (%zu != %zu)", this->data_metadata.size(), this->bitmap_data.size());
            throw InvalidTagDataException();
        }
    }

    Bitmap downgrade_extended_bitmap(const ExtendedBitmap &tag) {
        Bitmap new_tag = {};
        new_tag.type = tag.type;
        new_tag.encoding_format = tag.encoding_format;
        new_tag.usage = tag.usage;
        new_tag.flags = tag.flags;
        new_tag.detail_fade_factor = tag.detail_fade_factor;
        new_tag.sharpen_amount = tag.sharpen_amount;
        new_tag.bump_height = tag.bump_height;
        new_tag.sprite_budget_size = tag.sprite_budget_size;
        new_tag.sprite_budget_count = tag.sprite_budget_count;
        new_tag.color_plate_width = tag.color_plate_width;
        new_tag.color_plate_height = tag.color_plate_height;
        new_tag.compressed_color_plate_data = tag.compressed_color_plate_data;
        new_tag.processed_pixel_data = tag.processed_pixel_data;
        new_tag.blur_filter_size = tag.blur_filter_size;
        new_tag.alpha_bias = tag.alpha_bias;
        new_tag.mipmap_count = tag.mipmap_count;
        new_tag.sprite_usage = tag.sprite_usage;
        new_tag.sprite_spacing = tag.sprite_spacing;
        new_tag.bitmap_group_sequence = tag.bitmap_group_sequence;
        new_tag.bitmap_data = tag.bitmap_data;
        return new_tag;
    }

    template <typename T> static bool fix_power_of_two_for_tag(T &tag, bool fix) {
        bool fixed = false;
        for(auto &d : tag.bitmap_data) {
            fixed = fix_power_of_two(d, fix) || fixed;
            if(fixed && !fix) {
                return true;
            }
        }
        return fixed;
    }

    bool fix_power_of_two(ExtendedBitmap &tag, bool fix) {
        return fix_power_of_two_for_tag(tag, fix);
    }

    bool fix_power_of_two(Bitmap &tag, bool fix) {
        return fix_power_of_two_for_tag(tag, fix);
    }

    bool fix_power_of_two(BitmapData &data, bool fix) {
        bool should_be_power_of_two = power_of_two(data.width) && power_of_two(data.height) && power_of_two(data.width);
        if(data.flags.power_of_two_dimensions && !should_be_power_of_two) {
            if(fix) {
                data.flags.power_of_two_dimensions = 0;
            }
            return true;
        }
        else if(!data.flags.power_of_two_dimensions && should_be_power_of_two) {
            if(fix) {
                data.flags.power_of_two_dimensions = 1;
            }
            return true;
        }
        else {
            return false;
        }
    }
}
