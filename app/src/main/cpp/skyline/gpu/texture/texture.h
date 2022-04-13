// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <nce.h>
#include <gpu/memory_manager.h>

namespace skyline::gpu {
    namespace texture {
        struct Dimensions {
            u32 width;
            u32 height;
            u32 depth;

            constexpr Dimensions() : width(0), height(0), depth(0) {}

            constexpr Dimensions(u32 width) : width(width), height(1), depth(1) {}

            constexpr Dimensions(u32 width, u32 height) : width(width), height(height), depth(1) {}

            constexpr Dimensions(u32 width, u32 height, u32 depth) : width(width), height(height), depth(depth) {}

            constexpr Dimensions(vk::Extent2D extent) : Dimensions(extent.width, extent.height) {}

            constexpr Dimensions(vk::Extent3D extent) : Dimensions(extent.width, extent.height, extent.depth) {}

            auto operator<=>(const Dimensions &) const = default;

            constexpr vk::ImageType GetType() const {
                if (depth > 1)
                    return vk::ImageType::e3D;
                else if (height > 1)
                    return vk::ImageType::e2D;
                else
                    return vk::ImageType::e1D;
            }

            constexpr operator vk::Extent2D() const {
                return vk::Extent2D{
                    .width = width,
                    .height = height,
                };
            }

            constexpr operator vk::Extent3D() const {
                return vk::Extent3D{
                    .width = width,
                    .height = height,
                    .depth = depth,
                };
            }

            /**
             * @return If the dimensions are valid and don't equate to zero
             */
            constexpr operator bool() const {
                return width && height && depth;
            }
        };

        /**
         * @note Blocks refers to the atomic unit of a compressed format (IE: The minimum amount of data that can be decompressed)
         */
        struct FormatBase {
            u8 bpb{}; //!< Bytes Per Block, this is used instead of bytes per pixel as that might not be a whole number for compressed formats
            vk::Format vkFormat{vk::Format::eUndefined};
            vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
            u16 blockHeight{1}; //!< The height of a block in pixels
            u16 blockWidth{1}; //!< The width of a block in pixels
            vk::ComponentMapping swizzleMapping{
                .r = vk::ComponentSwizzle::eR,
                .g = vk::ComponentSwizzle::eG,
                .b = vk::ComponentSwizzle::eB,
                .a = vk::ComponentSwizzle::eA
            };
            bool stencilFirst{}; //!< If the stencil channel is the first channel in the format

            constexpr bool IsCompressed() const {
                return (blockHeight != 1) || (blockWidth != 1);
            }

            /**
             * @param width The width of the texture in pixels
             * @param height The height of the texture in pixels
             * @param depth The depth of the texture in layers
             * @return The size of the texture in bytes
             */
            constexpr size_t GetSize(u32 width, u32 height, u32 depth = 1) const {
                return util::DivideCeil(width, u32{blockWidth}) * util::DivideCeil(height, u32{blockHeight}) * bpb * depth;
            }

            constexpr size_t GetSize(Dimensions dimensions) const {
                return GetSize(dimensions.width, dimensions.height, dimensions.depth);
            }

            constexpr bool operator==(const FormatBase &format) const {
                return vkFormat == format.vkFormat;
            }

            constexpr bool operator!=(const FormatBase &format) const {
                return vkFormat != format.vkFormat;
            }

            constexpr operator vk::Format() const {
                return vkFormat;
            }

            /**
             * @return If this format is actually valid or not
             */
            constexpr operator bool() const {
                return bpb;
            }

            /**
             * @return If the supplied format is texel-layout compatible with the current format
             */
            constexpr bool IsCompatible(const FormatBase &other) const {
                return bpb == other.bpb && blockHeight == other.blockHeight && blockWidth == other.blockWidth;
            }

            /**
             * @brief Determines the image aspect to use based off of the format and the first swizzle component
             */
            constexpr vk::ImageAspectFlags Aspect(bool first) const {
                if (vkAspect & vk::ImageAspectFlagBits::eDepth && vkAspect & vk::ImageAspectFlagBits::eStencil) {
                    if (first)
                        return stencilFirst ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlagBits::eDepth;
                    else
                        return stencilFirst ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eStencil;
                } else {
                    return vkAspect;
                }
            }
        };

        /**
         * @brief A wrapper around a pointer to underlying format metadata to prevent redundant copies
         * @note The equality operators **do not** compare equality for pointers but for the underlying formats while considering nullability
         */
        class Format {
          private:
            const FormatBase *base;

          public:
            constexpr Format(const FormatBase &base) : base(&base) {}

            constexpr Format() : base(nullptr) {}

            constexpr const FormatBase *operator->() const {
                return base;
            }

            constexpr const FormatBase &operator*() const {
                return *base;
            }

            constexpr bool operator==(const Format &format) const {
                return base && format.base ? (*base) == (*format.base) : base == format.base;
            }

            constexpr bool operator!=(const Format &format) const {
                return base && format.base ? (*base) != (*format.base) : base != format.base;
            }

            constexpr operator bool() const {
                return base;
            }
        };

        /**
         * @brief The layout of a texture in GPU memory
         * @note Refer to Chapter 20.1 of the Tegra X1 TRM for information
         */
        enum class TileMode {
            Linear, //!< All pixels are arranged linearly
            Pitch,  //!< All pixels are arranged linearly but rows aligned to the pitch
            Block,  //!< All pixels are arranged into blocks and swizzled in a Z-order curve to optimize for spacial locality
        };

        /**
         * @brief The parameters of the tiling mode, covered in Table 76 in the Tegra X1 TRM
         */
        struct TileConfig {
            TileMode mode;
            union {
                struct {
                    u8 blockHeight; //!< The height of the blocks in GOBs
                    u8 blockDepth;  //!< The depth of the blocks in GOBs
                };
                u32 pitch; //!< The pitch of the texture in bytes
            };

            constexpr bool operator==(const TileConfig &other) const {
                if (mode == other.mode) {
                    switch (mode) {
                        case TileMode::Linear:
                            return true;
                        case TileMode::Pitch:
                            return pitch == other.pitch;
                        case TileMode::Block:
                            return blockHeight == other.blockHeight && blockDepth == other.blockDepth;
                    }
                }

                return false;
            }
        };

        /**
         * @brief The type of a texture to determine the access patterns for it
         * @note This is effectively the Tegra X1 texture types with the 1DBuffer + 2DNoMipmap removed as those are handled elsewhere
         * @note We explicitly utilize Vulkan types here as it provides the most efficient conversion while not exposing Vulkan to the outer API
         */
        enum class TextureType {
            e1D = VK_IMAGE_VIEW_TYPE_1D,
            e2D = VK_IMAGE_VIEW_TYPE_2D,
            e3D = VK_IMAGE_VIEW_TYPE_3D,
            eCube = VK_IMAGE_VIEW_TYPE_CUBE,
            e1DArray = VK_IMAGE_VIEW_TYPE_1D_ARRAY,
            e2DArray = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            eCubeArray = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
        };
    }

    class Texture;
    class PresentationEngine; //!< A forward declaration of PresentationEngine as we require it to be able to create a Texture object

    /**
     * @brief A descriptor for a texture present in guest memory, it can be used to create a corresponding Texture object for usage on the host
     */
    struct GuestTexture {
        using Mappings = boost::container::small_vector<span<u8>, 3>;

        Mappings mappings; //!< Spans to CPU memory for the underlying data backing this texture
        texture::Dimensions dimensions{};
        texture::Format format{};
        texture::TileConfig tileConfig{};
        texture::TextureType type{};
        u16 baseArrayLayer{};
        u16 layerCount{};
        u32 layerStride{}; //!< An optional hint regarding the size of a single layer, it will be set to 0 when not available, GetLayerSize() should be used to retrieve this value
        vk::ComponentMapping swizzle{}; //!< Component swizzle derived from format requirements and the guest supplied swizzle
        vk::ImageAspectFlags aspect{};

        GuestTexture() {}

        GuestTexture(Mappings mappings, texture::Dimensions dimensions, texture::Format format, texture::TileConfig tileConfig, texture::TextureType type, u16 baseArrayLayer = 0, u16 layerCount = 1, u32 layerStride = 0)
            : mappings(mappings),
              dimensions(dimensions),
              format(format),
              tileConfig(tileConfig),
              type(type),
              baseArrayLayer(baseArrayLayer),
              layerCount(layerCount),
              layerStride(layerStride),
              aspect(format->vkAspect) {}

        GuestTexture(span <u8> mapping, texture::Dimensions dimensions, texture::Format format, texture::TileConfig tileConfig, texture::TextureType type, u16 baseArrayLayer = 0, u16 layerCount = 1, u32 layerStride = 0)
            : mappings(1, mapping),
              dimensions(dimensions),
              format(format),
              tileConfig(tileConfig),
              type(type),
              baseArrayLayer(baseArrayLayer),
              layerCount(layerCount),
              layerStride(layerStride),
              aspect(format->vkAspect) {}

        /**
         * @note Requires `dimensions`, `format` and `tileConfig` to be filled in
         * @return The size of a single layer with alignment in bytes
         */
        u32 GetLayerSize();
    };

    class TextureManager;

    /**
     * @brief A view into a specific subresource of a Texture
     * @note The object **must** be locked prior to accessing any members as values will be mutated
     * @note This class conforms to the Lockable and BasicLockable C++ named requirements
     */
    class TextureView : public FenceCycleDependency, public std::enable_shared_from_this<TextureView> {
      private:
        std::optional<vk::raii::ImageView> view;

      public:
        std::shared_ptr<Texture> texture;
        vk::ImageViewType type;
        texture::Format format;
        vk::ComponentMapping mapping;
        vk::ImageSubresourceRange range;

        /**
         * @param format A compatible format for the texture view (Defaults to the format of the backing texture)
         */
        TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format = {}, vk::ComponentMapping mapping = {});

        /**
         * @brief Acquires an exclusive lock on the backing texture for the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void lock();

        /**
         * @brief Relinquishes an existing lock on the backing texture by the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void unlock();

        /**
         * @brief Attempts to acquire an exclusive lock on the backing texture but returns immediately if it's captured by another thread
         * @note Naming is in accordance to the Lockable named requirement
         */
        bool try_lock();

        /**
         * @return A VkImageView that corresponds to the properties of this view
         * @note The texture **must** be locked prior to calling this
         */
        vk::ImageView GetView();

        bool operator==(const TextureView &rhs) {
            return texture == rhs.texture && type == rhs.type && format == rhs.format && mapping == rhs.mapping && range == rhs.range;
        }
    };

    /**
     * @brief A texture which is backed by host constructs while being synchronized with the underlying guest texture
     * @note This class conforms to the Lockable and BasicLockable C++ named requirements
     */
    class Texture : public std::enable_shared_from_this<Texture>, public FenceCycleDependency {
      private:
        GPU &gpu;
        std::mutex mutex; //!< Synchronizes any mutations to the texture or its backing
        std::condition_variable backingCondition; //!< Signalled when a valid backing has been swapped in
        using BackingType = std::variant<vk::Image, vk::raii::Image, memory::Image>;
        BackingType backing; //!< The Vulkan image that backs this texture, it is nullable

        span<u8> mirror{}; //!< A contiguous mirror of all the guest mappings to allow linear access on the CPU
        span<u8> alignedMirror{}; //!< The mirror mapping aligned to page size to reflect the full mapping
        std::optional<nce::NCE::TrapHandle> trapHandle{}; //!< The handle of the traps for the guest mappings
        enum class DirtyState {
            Clean, //!< The CPU mappings are in sync with the GPU texture
            CpuDirty, //!< The CPU mappings have been modified but the GPU texture is not up to date
            GpuDirty, //!< The GPU texture has been modified but the CPU mappings have not been updated
        } dirtyState{DirtyState::CpuDirty}; //!< The state of the CPU mappings with respect to the GPU texture

        std::vector<std::weak_ptr<TextureView>> views; //!< TextureView(s) that are backed by this Texture, used for repointing to a new Texture on deletion

        friend TextureManager;
        friend TextureView;

        /**
         * @brief Sets up mirror mappings for the guest mappings
         */
        void SetupGuestMappings();

        /**
         * @brief An implementation function for guest -> host texture synchronization, it allocates and copies data into a staging buffer or directly into a linear host texture
         * @return If a staging buffer was required for the texture sync, it's returned filled with guest texture data and must be copied to the host texture by the callee
         */
        std::shared_ptr<memory::StagingBuffer> SynchronizeHostImpl(const std::shared_ptr<FenceCycle> &pCycle);

        /**
         * @brief Records commands for copying data from a staging buffer to the texture's backing into the supplied command buffer
         */
        void CopyFromStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer);

        /**
         * @brief Records commands for copying data from the texture's backing to a staging buffer into the supplied command buffer
         * @note Any caller **must** ensure that the layout is not `eUndefined`
         */
        void CopyIntoStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer);

        /**
         * @brief Copies data from the supplied host buffer into the guest texture
         * @note The host buffer must be contain the entire image
         */
        void CopyToGuest(u8 *hostBuffer);

        /**
         * @brief A FenceCycleDependency that copies the contents of a staging buffer or mapped image backing the texture to the guest texture
         */
        struct TextureBufferCopy : public FenceCycleDependency {
            std::shared_ptr<Texture> texture;
            std::shared_ptr<memory::StagingBuffer> stagingBuffer;

            TextureBufferCopy(std::shared_ptr<Texture> texture, std::shared_ptr<memory::StagingBuffer> stagingBuffer = {});

            ~TextureBufferCopy();
        };

      public:
        std::weak_ptr<FenceCycle> cycle; //!< A fence cycle for when any host operation mutating the texture has completed, it must be waited on prior to any mutations to the backing
        std::optional<GuestTexture> guest;
        texture::Dimensions dimensions;
        texture::Format format;
        vk::ImageLayout layout;
        vk::ImageTiling tiling;
        u32 mipLevels;
        u32 layerCount; //!< The amount of array layers in the image, utilized for efficient binding (Not to be confused with the depth or faces in a cubemap)
        vk::SampleCountFlagBits sampleCount;

        /**
         * @brief Creates a texture object wrapping the supplied backing with the supplied attributes
         * @param layout The initial layout of the texture, it **must** be eUndefined or ePreinitialized
         */
        Texture(GPU &gpu, BackingType &&backing, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout layout, vk::ImageTiling tiling, u32 mipLevels = 1, u32 layerCount = 1, vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1);

        /**
         * @brief Creates a texture object wrapping the guest texture with a backing that can represent the guest texture data
         */
        Texture(GPU &gpu, GuestTexture guest);

        ~Texture();

        /**
         * @note The handle returned is nullable and the appropriate precautions should be taken
         */
        constexpr vk::Image GetBacking() {
            return std::visit(VariantVisitor{
                [](vk::Image image) { return image; },
                [](const vk::raii::Image &image) { return *image; },
                [](const memory::Image &image) { return image.vkImage; },
            }, backing);
        }

        /**
         * @brief Acquires an exclusive lock on the texture for the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void lock() {
            mutex.lock();
        }

        /**
         * @brief Relinquishes an existing lock on the texture by the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void unlock() {
            mutex.unlock();
        }

        /**
         * @brief Attempts to acquire an exclusive lock but returns immediately if it's captured by another thread
         * @note Naming is in accordance to the Lockable named requirement
         */
        bool try_lock() {
            return mutex.try_lock();
        }

        /**
         * @brief Marks the texture as dirty on the GPU, it will be synced on the next call to SynchronizeGuest
         * @note This **must** be called after syncing the texture to the GPU not before
         * @note The texture **must** be locked prior to calling this
         */
        void MarkGpuDirty();

        /**
         * @brief Waits on the texture backing to be a valid non-null Vulkan image
         * @return If the mutex could be unlocked during the function
         * @note The texture **must** be locked prior to calling this
         */
        bool WaitOnBacking();

        /**
         * @brief Waits on a fence cycle if it exists till it's signalled and resets it after
         * @note The texture **must** be locked prior to calling this
         */
        void WaitOnFence();

        /**
         * @note All memory residing in the current backing is not copied to the new backing, it must be handled externally
         * @note The texture **must** be locked prior to calling this
         */
        void SwapBacking(BackingType &&backing, vk::ImageLayout layout = vk::ImageLayout::eUndefined);

        /**
         * @brief Transitions the backing to the supplied layout, if the backing already is in this layout then this does nothing
         * @note The texture **must** be locked prior to calling this
         */
        void TransitionLayout(vk::ImageLayout layout);

        /**
         * @brief Converts the texture to have the specified format
         */
        void SetFormat(texture::Format format);

        /**
         * @brief Synchronizes the host texture with the guest after it has been modified
         * @param rwTrap If true, the guest buffer will be read/write trapped rather than only being write trapped which is more efficient than calling MarkGpuDirty directly after
         * @note The texture **must** be locked prior to calling this
         * @note The guest texture backing should exist prior to calling this
         */
        void SynchronizeHost(bool rwTrap = false);

        /**
         * @brief Same as SynchronizeHost but this records any commands into the supplied command buffer rather than creating one as necessary
         * @param rwTrap If true, the guest buffer will be read/write trapped rather than only being write trapped which is more efficient than calling MarkGpuDirty directly after
         * @note It is more efficient to call SynchronizeHost than allocating a command buffer purely for this function as it may conditionally not record any commands
         * @note The texture **must** be locked prior to calling this
         * @note The guest texture backing should exist prior to calling this
         */
        void SynchronizeHostWithBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &cycle, bool rwTrap = false);

        /**
         * @brief Synchronizes the guest texture with the host texture after it has been modified
         * @param skipTrap If true, setting up a CPU trap will be skipped and the dirty state will be Clean/CpuDirty
         * @note The texture **must** be locked prior to calling this
         * @note The guest texture should not be null prior to calling this
         */
        void SynchronizeGuest(bool skipTrap = false);

        /**
         * @brief Synchronizes the guest texture with the host texture after it has been modified
         * @note It is more efficient to call SynchronizeHost than allocating a command buffer purely for this function as it may conditionally not record any commands
         * @note The texture **must** be locked prior to calling this
         * @note The guest texture should not be null prior to calling this
         */
        void SynchronizeGuestWithBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &cycle);

        /**
         * @return A cached or newly created view into this texture with the supplied attributes
         */
        std::shared_ptr<TextureView> GetView(vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format = {}, vk::ComponentMapping mapping = {});

        /**
         * @brief Copies the contents of the supplied source texture into the current texture
         */
        void CopyFrom(std::shared_ptr<Texture> source, const vk::ImageSubresourceRange &subresource = vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        });
    };
}
