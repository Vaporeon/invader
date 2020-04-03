// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <invader/version.hpp>
#include <invader/build/build_workload.hpp>
#include <invader/tag/hek/definition.hpp>
#include <invader/resource/resource_map.hpp>
#include <invader/resource/hek/resource_map.hpp>
#include <invader/resource/list/resource_list.hpp>
#include <invader/command_line_option.hpp>
#include <invader/file/file.hpp>
#include <invader/printf.hpp>

int main(int argc, const char **argv) {
    using namespace Invader::HEK;
    using namespace Invader;

    std::vector<CommandLineOption> options;
    options.emplace_back("info", 'i', 0, "Show credits, source info, and other info.");
    options.emplace_back("type", 'T', 1, "Set the resource map. This option is required. Can be: bitmaps, sounds, or loc.", "<type>");
    options.emplace_back("tags", 't', 1, "Use the specified tags directory. Use multiple times to add more directories, ordered by precedence.", "<dir>");
    options.emplace_back("maps", 'm', 1, "Set the maps directory.", "<dir>");
    options.emplace_back("retail", 'R', 0, "Build a retail resource map (bitmaps/sounds only)");

    static constexpr char DESCRIPTION[] = "Create resource maps.";
    static constexpr char USAGE[] = "[options] -T <type>";

    struct ResourceOption {
        // Tags directory
        std::vector<std::string> tags;

        // Maps directory
        const char *maps = "maps";

        // Resource map type
        ResourceMapType type = ResourceMapType::RESOURCE_MAP_BITMAP;
        const char **(*default_fn)() = get_default_bitmap_resources;
        bool resource_map_set = false;

        bool retail = false;
    } resource_options;

    auto remaining_arguments = CommandLineOption::parse_arguments<ResourceOption &>(argc, argv, options, USAGE, DESCRIPTION, 0, 0, resource_options, [](char opt, const std::vector<const char *> &arguments, auto &resource_options) {
        switch(opt) {
            case 'i':
                show_version_info();
                std::exit(EXIT_SUCCESS);

            case 't':
                resource_options.tags.push_back(arguments[0]);
                break;

            case 'm':
                resource_options.maps = arguments[0];
                break;

            case 'R':
                resource_options.retail = true;
                break;

            case 'T':
                if(std::strcmp(arguments[0], "bitmaps") == 0) {
                    resource_options.type = ResourceMapType::RESOURCE_MAP_BITMAP;
                    resource_options.default_fn = get_default_bitmap_resources;
                }
                else if(std::strcmp(arguments[0], "sounds") == 0) {
                    resource_options.type = ResourceMapType::RESOURCE_MAP_SOUND;
                    resource_options.default_fn = get_default_sound_resources;
                }
                else if(std::strcmp(arguments[0], "loc") == 0) {
                    resource_options.type = ResourceMapType::RESOURCE_MAP_LOC;
                    resource_options.default_fn = get_default_loc_resources;
                }
                else {
                    eprintf_error("Invalid type %s. Use --help for more information.", arguments[0]);
                    std::exit(EXIT_FAILURE);
                }
                resource_options.resource_map_set = true;
                break;
        }
    });

    if(!resource_options.resource_map_set) {
        eprintf_error("No resource map type was given. Use -h for more information.");
        return EXIT_FAILURE;
    }

    if(resource_options.retail && resource_options.type == ResourceMapType::RESOURCE_MAP_LOC) {
        eprintf_error("Only bitmaps.map and sounds.map can be made for retail.");
        return EXIT_FAILURE;
    }

    // If we don't have any tags directories, use a default one
    if(resource_options.tags.size() == 0) {
        resource_options.tags.push_back("tags");
    }

    // Get all the tags
    std::vector<std::string> tags_list;
    for(const char **i = resource_options.default_fn(); *i; i++) {
        tags_list.insert(tags_list.end(), *i);
    }

    ResourceMapHeader header = {};
    header.type = resource_options.type;
    header.resource_count = tags_list.size();

    // Read the amazing fun happy stuff
    std::vector<std::byte> resource_data(sizeof(ResourceMapHeader));
    std::vector<std::size_t> offsets;
    std::vector<std::size_t> sizes;
    std::vector<std::string> paths;

    for(const std::string &listed_tag : tags_list) {
        // First let's open it
        TagClassInt tag_class_int = TagClassInt::TAG_CLASS_NONE;
        std::vector<std::byte> tag_data;
        auto tag_path = File::halo_path_to_preferred_path(listed_tag);
        auto halo_tag_path = File::preferred_path_to_halo_path(listed_tag);

        // Function to open a file
        auto open_tag = [&resource_options, &tag_path](const char *extension) -> std::FILE * {
            for(auto &tags_folder : resource_options.tags) {
                auto tag_path_str = (std::filesystem::path(tags_folder) / tag_path).string() + extension;
                std::FILE *f = std::fopen(tag_path_str.c_str(), "rb");
                if(f) {
                    return f;
                }
            }
            return nullptr;
        };

        #define ATTEMPT_TO_OPEN(extension) { \
            std::FILE *f = open_tag(extension); \
            if(!f) { \
                eprintf_error("Failed to open %s" extension, tag_path.c_str()); \
                return EXIT_FAILURE; \
            } \
            std::fseek(f, 0, SEEK_END); \
            tag_data.insert(tag_data.end(), std::ftell(f), std::byte()); \
            std::fseek(f, 0, SEEK_SET); \
            if(std::fread(tag_data.data(), tag_data.size(), 1, f) != 1) { \
                eprintf_error("Failed to read %s" extension, tag_path.c_str()); \
                return EXIT_FAILURE; \
            } \
            std::fclose(f); \
        }

        switch(resource_options.type) {
            case ResourceMapType::RESOURCE_MAP_BITMAP:
                tag_class_int = TagClassInt::TAG_CLASS_BITMAP;
                ATTEMPT_TO_OPEN(".bitmap")
                break;
            case ResourceMapType::RESOURCE_MAP_SOUND:
                tag_class_int = TagClassInt::TAG_CLASS_SOUND;
                ATTEMPT_TO_OPEN(".sound")
                break;
            case ResourceMapType::RESOURCE_MAP_LOC:
                tag_class_int = TagClassInt::TAG_CLASS_FONT;
                #define DO_THIS_FOR_ME_PLEASE(tci, extension) if(tag_data.size() == 0) { \
                    tag_class_int = tci; \
                    std::FILE *f = open_tag(extension); \
                    if(f) { \
                        std::fseek(f, 0, SEEK_END); \
                        tag_data.insert(tag_data.end(), std::ftell(f), std::byte()); \
                        std::fseek(f, 0, SEEK_SET); \
                        if(std::fread(tag_data.data(), tag_data.size(), 1, f) != 1) { \
                            eprintf_error("Failed to read %s" extension, tag_path.c_str()); \
                            return EXIT_FAILURE; \
                        } \
                        std::fclose(f); \
                    } \
                }
                DO_THIS_FOR_ME_PLEASE(TagClassInt::TAG_CLASS_FONT, ".font")
                DO_THIS_FOR_ME_PLEASE(TagClassInt::TAG_CLASS_HUD_MESSAGE_TEXT, ".hud_message_text")
                DO_THIS_FOR_ME_PLEASE(TagClassInt::TAG_CLASS_UNICODE_STRING_LIST, ".unicode_string_list")
                #undef DO_THIS_FOR_ME_PLEASE

                if(tag_data.size() == 0) {
                    eprintf_error("Failed to open %s.\nNo such font, hud_message_text, or unicode_string_list were found.", tag_path.c_str());
                }

                break;
        }

        #undef ATTEMPT_TO_OPEN

        // This may be needed
        #define PAD_RESOURCES_32_BIT resource_data.insert(resource_data.end(), REQUIRED_PADDING_32_BIT(resource_data.size()), std::byte());

        // Compile the tags
        try {
            auto compiled_tag = Invader::BuildWorkload::compile_single_tag(tag_path.c_str(), tag_class_int, resource_options.tags);
            auto &compiled_tag_tag = compiled_tag.tags[0];
            auto &compiled_tag_struct = compiled_tag.structs[*compiled_tag_tag.base_struct];
            auto *compiled_tag_data = compiled_tag_struct.data.data();
            char path_temp[256];

            std::vector<std::byte> data;
            std::vector<std::size_t> structs;
            
            // Pointers are stored as offsets here
            auto write_pointers = [&data, &structs, &compiled_tag, &resource_options]() {
                for(auto &s : compiled_tag.structs) {
                    structs.push_back(data.size());
                    data.insert(data.end(), s.data.begin(), s.data.end());
                }
                
                std::size_t sound_offset = resource_options.type == ResourceMapType::RESOURCE_MAP_SOUND ? sizeof(Invader::Parser::Sound::struct_little) : 0;
                for(auto &s : compiled_tag.structs) {
                    for(auto &ptr : s.pointers) {
                        *reinterpret_cast<LittleEndian<Pointer> *>(data.data() + structs[&s - compiled_tag.structs.data()] + ptr.offset) = structs[ptr.struct_index] - sound_offset;
                    }
                }
            };

            // Now, adjust stuff for pointers
            switch(resource_options.type) {
                case ResourceMapType::RESOURCE_MAP_BITMAP: {
                    // Do stuff to the tag data
                    auto &bitmap = *reinterpret_cast<Bitmap<LittleEndian> *>(compiled_tag_data);
                    std::size_t bitmap_count = bitmap.bitmap_data.count;
                    if(bitmap_count) {
                        auto *bitmaps = reinterpret_cast<BitmapData<LittleEndian> *>(compiled_tag.structs[*compiled_tag_struct.resolve_pointer(&bitmap.bitmap_data.pointer)].data.data());
                        for(std::size_t b = 0; b < bitmap_count; b++) {
                            auto *bitmap_data = bitmaps + b;

                            // If we're on retail, push the pixel data
                            if(resource_options.retail) {
                                // Generate the path to add
                                std::snprintf(path_temp, sizeof(path_temp), "%s_%zu", halo_tag_path.c_str(), b);

                                // Push it good
                                paths.push_back(path_temp);
                                std::size_t size = bitmap_data->pixel_data_size.read();
                                sizes.push_back(size);
                                offsets.push_back(resource_data.size());
                                resource_data.insert(resource_data.end(), compiled_tag.raw_data[b].begin(), compiled_tag.raw_data[b].end());

                                PAD_RESOURCES_32_BIT
                            }
                            // Otherwise set the sizes
                            else {
                                bitmap_data->pixel_data_offset = resource_data.size() + bitmap_data->pixel_data_offset;
                                auto flags = bitmap_data->flags.read();
                                flags.external = 1;
                                bitmap_data->flags = flags;
                            }
                        }
                    }
                    
                    write_pointers();

                    // Push the asset data and tag data if we aren't on retail
                    if(!resource_options.retail) {
                        offsets.push_back(resource_data.size());
                        std::size_t total_size = 0;
                        for(auto &r : compiled_tag.raw_data) {
                            total_size += r.size();
                            resource_data.insert(resource_data.end(), r.begin(), r.end());
                        }
                        paths.push_back(halo_tag_path + "__pixels");
                        sizes.push_back(total_size);

                        PAD_RESOURCES_32_BIT

                        // Push the tag data
                        offsets.push_back(resource_data.size());
                        resource_data.insert(resource_data.end(), data.begin(), data.end());
                        paths.push_back(halo_tag_path);
                        sizes.push_back(data.size());

                        PAD_RESOURCES_32_BIT
                    }

                    break;
                }
                case ResourceMapType::RESOURCE_MAP_SOUND: {
                    // Do stuff to the tag data
                    auto &sound = *reinterpret_cast<Sound<LittleEndian> *>(compiled_tag_struct.data.data());
                    std::size_t pitch_range_count = sound.pitch_ranges.count;
                    std::size_t b = 0;
                    if(pitch_range_count) {
                        auto &pitch_range_struct = compiled_tag.structs[*compiled_tag_struct.resolve_pointer(&sound.pitch_ranges.pointer)];
                        auto *pitch_ranges = reinterpret_cast<SoundPitchRange<LittleEndian> *>(pitch_range_struct.data.data());
                        for(std::size_t pr = 0; pr < pitch_range_count; pr++) {
                            auto &pitch_range = pitch_ranges[pr];
                            std::size_t permutation_count = pitch_range.permutations.count;
                            if(permutation_count) {
                                auto *permutations = reinterpret_cast<SoundPermutation<LittleEndian> *>(compiled_tag.structs[*pitch_range_struct.resolve_pointer(&pitch_range.permutations.pointer)].data.data());
                                for(std::size_t p = 0; p < permutation_count; p++) {
                                    auto &permutation = permutations[p];
                                    if(resource_options.retail) {
                                        // Generate the path to add
                                        std::snprintf(path_temp, sizeof(path_temp), "%s__%zu__%zu", halo_tag_path.c_str(), pr, p);

                                        // Push it REAL good
                                        paths.push_back(path_temp);
                                        std::size_t size = permutation.samples.size.read();
                                        sizes.push_back(size);
                                        offsets.push_back(resource_data.size());
                                        resource_data.insert(resource_data.end(), compiled_tag.raw_data[b].begin(), compiled_tag.raw_data[b].end());

                                        b++;

                                        PAD_RESOURCES_32_BIT
                                    }
                                    else {
                                        permutation.samples.external = 1;
                                        permutation.samples.file_offset = resource_data.size() + permutation.samples.file_offset;
                                    }
                                }
                            }
                        }
                    }
                    
                    write_pointers();

                    // If we're not on retail, push asset and tag data
                    if(!resource_options.retail) {
                        // Push the asset data first
                        offsets.push_back(resource_data.size());
                        std::size_t total_size = 0;
                        for(auto &r : compiled_tag.raw_data) {
                            total_size += r.size();
                            resource_data.insert(resource_data.end(), r.begin(), r.end());
                        }
                        paths.push_back(halo_tag_path + "__permutations");
                        sizes.push_back(total_size);

                        PAD_RESOURCES_32_BIT

                        // Push the tag data
                        offsets.push_back(resource_data.size());
                        resource_data.insert(resource_data.end(), data.begin(), data.end());
                        paths.push_back(halo_tag_path);
                        sizes.push_back(data.size());

                        PAD_RESOURCES_32_BIT
                    }

                    break;
                }
                case ResourceMapType::RESOURCE_MAP_LOC:
                    write_pointers();
                    offsets.push_back(resource_data.size());
                    resource_data.insert(resource_data.end(), data.begin(), data.end());
                    paths.push_back(halo_tag_path);
                    sizes.push_back(data.size());

                    PAD_RESOURCES_32_BIT

                    break;
            }
        }
        catch(std::exception &e) {
            eprintf_error("Failed to compile %s.%s due to an exception: %s", tag_path.c_str(), tag_class_to_extension(tag_class_int), e.what());
            return EXIT_FAILURE;
        }

        #undef PAD_RESOURCES_32_BIT
    }

    // Get the final path of the map
    const char *map;
    switch(resource_options.type) {
        case ResourceMapType::RESOURCE_MAP_BITMAP:
            map = "bitmaps.map";
            break;
        case ResourceMapType::RESOURCE_MAP_SOUND:
            map = "sounds.map";
            break;
        case ResourceMapType::RESOURCE_MAP_LOC:
            map = "loc.map";
            break;
        default:
            std::terminate();
    }
    auto map_path = std::filesystem::path(resource_options.maps) / map;

    // Finish up building up the map
    std::size_t resource_count = paths.size();
    assert(resource_count == offsets.size());
    assert(resource_count == sizes.size());
    std::vector<ResourceMapResource> resource_indices(resource_count);
    std::vector<std::byte> resource_names_arr;
    for(std::size_t i = 0; i < resource_count; i++) {
        auto &index = resource_indices[i];
        index.size = sizes[i];
        index.data_offset = offsets[i];
        index.path_offset = resource_names_arr.size();
        resource_names_arr.insert(resource_names_arr.end(), reinterpret_cast<const std::byte *>(paths[i].c_str()), reinterpret_cast<const std::byte *>(paths[i].c_str()) + paths[i].size() + 1);
    }
    header.resource_count = resource_count;
    header.paths = resource_data.size();
    header.resources = resource_names_arr.size() + resource_data.size();
    *reinterpret_cast<ResourceMapHeader *>(resource_data.data()) = header;

    if(resource_data.size() >= 0xFFFFFFFF) {
        eprintf_error("Resource map exceeds 4 GiB.");
        return EXIT_FAILURE;
    }

    // Open the file
    std::FILE *f = std::fopen(map_path.string().c_str(), "wb");
    if(!f) {
        eprintf_error("Failed to open %s for writing.", map_path.string().c_str());
        return EXIT_FAILURE;
    }

    // Write everything
    std::fwrite(resource_data.data(), resource_data.size(), 1, f);
    std::fwrite(resource_names_arr.data(), resource_names_arr.size(), 1, f);
    std::fwrite(resource_indices.data(), resource_indices.size() * sizeof(*resource_indices.data()), 1, f);

    // Done!
    std::fclose(f);
}
