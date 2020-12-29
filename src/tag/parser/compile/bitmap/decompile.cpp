// SPDX-License-Identifier: GPL-3.0-only

#include <invader/tag/parser/parser.hpp>
#include <invader/tag/hek/class/bitmap.hpp>
#include <invader/bitmap/swizzle.hpp>

namespace Invader::Parser {
    static std::size_t size_of_bitmap(const BitmapData &data) {
        std::size_t size = 0;
        std::size_t bits_per_pixel = HEK::calculate_bits_per_pixel(data.format);

        // Is this a meme?
        if(bits_per_pixel == 0) {
            eprintf_error("Unknown format %u", static_cast<unsigned int>(data.format));
            throw std::exception();
        }

        std::size_t height = data.height;
        std::size_t width = data.width;
        std::size_t depth = data.depth;
        bool should_be_compressed = data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
        std::size_t multiplier = data.type == HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP ? 6 : 1;
        std::size_t block_length = should_be_compressed ? 4 : 1;

        // Do it
        for(std::size_t i = 0; i <= data.mipmap_count; i++) {
            size += width * height * depth * multiplier * bits_per_pixel / 8;
            
            // Divide by 2, resetting back to 1 when needed, but make sure we don't go below the block length (4x4 if DXT, else 1x1)
            width = std::max(width / 2, block_length);
            height = std::max(height / 2, block_length);
            depth = std::max(depth / 2, static_cast<std::size_t>(1));
        }

        return size;
    }

    template <typename T> static void do_post_cache_parse(T *bitmap, const Invader::Tag &tag) {
        bitmap->postprocess_hek_data();

        auto &map = tag.get_map();
        auto engine = map.get_engine();
        auto xbox = engine == HEK::CacheFileEngine::CACHE_FILE_XBOX;
        auto &base_struct = tag.get_base_struct<HEK::Bitmap>();
        
        // Un-zero out these if we're sprites (again, this is completely *insane* but compiled maps have this zeroed out for whatever reason which can completely FUCK things up if this were to not be "sprites" all of a sudden)
        if(bitmap->type == HEK::BitmapType::BITMAP_TYPE_SPRITES) {
            for(auto &sequence : bitmap->bitmap_group_sequence) {
                // Default
                sequence.first_bitmap_index = NULL_INDEX;
                sequence.bitmap_count = sequence.sprites.size() == 1 ? 1 : 0; // set to 1 if we have one sprite; 0 otherwise
                
                // If we have sprites, find the lowest bitmap index of them all
                if(sequence.sprites.size() != 0) {
                    for(auto &sprite : sequence.sprites) {
                        sequence.first_bitmap_index = std::min(sequence.first_bitmap_index, sprite.bitmap_index);
                    }
                }
            }
        }

        // Do we have bitmap data?
        auto bd_count = bitmap->bitmap_data.size();
        if(bd_count) {
            auto *bitmap_data_le_array = tag.resolve_reflexive(base_struct.bitmap_data);
            
            for(std::size_t bd = 0; bd < bd_count; bd++) {
                auto &bitmap_data = bitmap->bitmap_data[bd];
                auto &bitmap_data_le = bitmap_data_le_array[bd];
                std::size_t size = bitmap_data.pixel_data_size;
                
                bool compressed = bitmap_data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
                bool should_be_compressed = false;
                
                switch(bitmap_data.format) {
                    case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1:
                    case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3:
                    case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5:
                        should_be_compressed = true;
                        break;
                    default:
                        should_be_compressed = false;
                }
                
                // Bad!
                if(should_be_compressed != compressed) {
                    if(compressed) {
                        eprintf_error("Bitmap is incorrectly marked as compressed but is NOT DXT; tag is corrupt");
                    }
                    else {
                        eprintf_error("Bitmap is incorrectly NOT marked as compressed but is DXT; tag is corrupt");
                    }
                    throw InvalidTagDataException();
                }
                
                // Also check if it needs deswizzled (don't do it yet)
                bool swizzled = bitmap_data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
                if(swizzled) {
                    if(compressed) {
                        eprintf_error("Bitmap is incorrectly marked as compressed AND swizzled; tag is corrupt");
                        throw InvalidTagDataException();
                    }
                }
                
                // Nope
                if(bitmap_data.depth != 1 && bitmap_data.type != HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE) {
                    eprintf_error("Bitmap has depth but is not a 3D texture");
                    throw InvalidTagDataException();
                }
                if(!HEK::is_power_of_two(bitmap_data.depth)) {
                    eprintf_error("Bitmap depth is non-power-of-two");
                    throw InvalidTagDataException();
                }
                
                // Get it!
                const std::byte *bitmap_data_ptr;
                if(bitmap_data_le.flags.read() & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_EXTERNAL) {
                    bitmap_data_ptr = map.get_data_at_offset(bitmap_data.pixel_data_offset, size, Map::DATA_MAP_BITMAP);
                }
                else {
                    bitmap_data_ptr = map.get_internal_asset(bitmap_data.pixel_data_offset, size);
                }
                
                // Xbox buffer (for Xbox bitmaps), since the size will always be a multiple of 128 and thus won't be the same size as a PC bitmap
                std::vector<std::byte> xbox_to_pc_buffer;
                
                if(xbox) {
                    // Set flag as unswizzled
                    bitmap_data.flags = bitmap_data.flags & ~HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
                    
                    // Set our buffer up
                    xbox_to_pc_buffer.resize(size_of_bitmap(bitmap_data));
                    
                    auto copy_texture = [&xbox_to_pc_buffer, &swizzled, &compressed, &bitmap_data_ptr, &size, &bitmap_data](std::optional<std::size_t> input_offset = std::nullopt, std::optional<std::size_t> output_cubemap_face = std::nullopt) -> std::size_t {
                        std::size_t bits_per_pixel = calculate_bits_per_pixel(bitmap_data.format);
                        std::size_t real_mipmap_count = bitmap_data.mipmap_count;
                        std::size_t height = bitmap_data.height;
                        std::size_t width = bitmap_data.width;
                        std::size_t depth = bitmap_data.depth;
                        
                        auto *base_input = bitmap_data_ptr + input_offset.value_or(0);
                        auto *input = base_input;
                        
                        auto *output = xbox_to_pc_buffer.data();
                        
                        // Offset the output
                        if(output_cubemap_face.has_value()) {
                            output += (height * width * depth * bits_per_pixel) / 8 * *output_cubemap_face;
                        }
                        
                        std::size_t minimum_dimension;
                        std::size_t minimum_dimension_depth = 1;
                        
                        if(compressed) {
                            // Resolution the bitmap is stored as
                            if(height % 4) {
                                height += 4 - (height % 4);
                            }
                            if(width % 4) {
                                width += 4 - (width % 4);
                            }
                            
                            // Mipmaps less than 4x4 don't exist in Xbox maps
                            while((height >> real_mipmap_count) < 4 && (width >> real_mipmap_count) < 4 && real_mipmap_count > 0) {
                                real_mipmap_count--;
                            }
                            
                            minimum_dimension = 4;
                        }
                        else {
                            minimum_dimension = 1;
                        }
                        
                        std::size_t mipmap_width = width;
                        std::size_t mipmap_height = height;
                        std::size_t mipmap_depth = depth;
                        
                        // Copy this stuff
                        for(std::size_t m = 0; m <= real_mipmap_count; m++) {
                            std::size_t mipmap_size = mipmap_width * mipmap_height * mipmap_depth * bits_per_pixel / 8;
                            
                            // Bitmap data is out of bounds?
                            if((input - base_input) + mipmap_size > size) {
                                throw OutOfBoundsException();
                            }
                            
                            // Swizzle that stuff!
                            if(swizzled) {
                                auto swizzled_data = Invader::Swizzle::swizzle(input, bits_per_pixel, mipmap_width, mipmap_height, mipmap_depth, true);
                                std::memcpy(output, swizzled_data.data(), mipmap_size);
                            }
                            else {
                                std::memcpy(output, input, mipmap_size);
                            }
                            
                            // Continue...
                            std::size_t stride_count = output_cubemap_face.has_value() ? (6 - *output_cubemap_face) : 1;
                            output += stride_count * mipmap_size;
                            input += mipmap_size;
                            
                            // Halve the dimensions
                            mipmap_width = std::max(mipmap_width / 2, minimum_dimension);
                            mipmap_height = std::max(mipmap_height / 2, minimum_dimension);
                            mipmap_depth = std::max(mipmap_depth / 2, minimum_dimension_depth);
                            
                            // Skip that data, too
                            if(output_cubemap_face.has_value()) {
                                mipmap_size = mipmap_width * mipmap_height * mipmap_depth * bits_per_pixel / 8;
                                output += *output_cubemap_face * mipmap_size;
                            }
                        }
                        
                        if(compressed) {
                            // Get the block size
                            auto block_size = (minimum_dimension * minimum_dimension * bits_per_pixel) / 8;
                            
                            // Go back a block and just copy that
                            input -= block_size;
                            
                            // Lastly, copy the missing mipmaps if necessary
                            for(std::size_t m = real_mipmap_count; m < bitmap_data.mipmap_count; m++) {
                                std::memcpy(output, input, block_size);
                                std::size_t stride_count = output_cubemap_face.has_value() ? 6 : 1; // since all mipmaps from here on out are the same in size, we just need to add this once this time
                                output += stride_count * block_size;
                            }
                            
                            return (input - base_input) + block_size;
                        }
                        
                        else {
                            return input - base_input;
                        }
                    };
                    
                    // Cubemaps store each face as individual bitmaps rather than by mipmap, swapping the second and third faces.
                    auto copy_cube_map = [&copy_texture]() -> void {
                        std::size_t offset = 0;
                        for(std::size_t i = 0; i < 6; i++) {
                            int to_i;
                            
                            // Swap the second and third faces
                            if(i == 1) {
                                to_i = 2;
                            }
                            else if(i == 2) {
                                to_i = 1;
                            }
                            else {
                                to_i = i;
                            }
                            
                            offset += copy_texture(offset, to_i);
                            
                            // Add some padding since bitmaps are stored with sizes module 128
                            offset += REQUIRED_PADDING_N_BYTES(offset, HEK::CacheFileXboxConstants::CACHE_FILE_XBOX_BITMAP_SIZE_GRANULARITY);
                        }
                    };
                    
                    switch(bitmap_data.type) {
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP:
                            copy_cube_map();
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE:
                            copy_texture();
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_WHITE:
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_2D_TEXTURE:
                            copy_texture();
                            break;
                        default:
                            throw std::exception();
                            break;
                    }
                    
                    
                    bitmap_data_ptr = xbox_to_pc_buffer.data();
                    size = xbox_to_pc_buffer.size();
                    bitmap_data.pixel_data_size = size;
                }

                // Calculate our offset
                bitmap_data.pixel_data_offset = static_cast<std::size_t>(bitmap->processed_pixel_data.size());
                
                // Insert this
                bitmap->processed_pixel_data.insert(bitmap->processed_pixel_data.end(), bitmap_data_ptr, bitmap_data_ptr + size);
            }
        }
    }

    void Bitmap::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer>) {
        do_post_cache_parse(this, tag);
    }

    void InvaderBitmap::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer>) {
        do_post_cache_parse(this, tag);
    }
}
