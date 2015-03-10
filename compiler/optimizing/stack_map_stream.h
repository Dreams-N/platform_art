/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
#define ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_

#include "base/bit_vector.h"
#include "base/value_object.h"
#include "memory_region.h"
#include "stack_map.h"
#include "utils/growable_array.h"

namespace art {

/**
 * Collects and builds stack maps for a method. All the stack maps
 * for a method are placed in a CodeInfo object.
 */
class StackMapStream : public ValueObject {
 public:
  explicit StackMapStream(ArenaAllocator* allocator)
      : stack_maps_(allocator, 10),
        dex_register_maps_(allocator, 10 * 4),
        inline_infos_(allocator, 2),
        stack_mask_max_(-1),
        number_of_stack_maps_with_inline_info_(0) {}

  // Compute bytes needed to encode a mask with the given maximum element.
  static uint32_t StackMaskEncodingSize(int max_element) {
    int number_of_bits = max_element + 1;  // Need room for max element too.
    return RoundUp(number_of_bits, kBitsPerByte) / kBitsPerByte;
  }

  // See runtime/stack_map.h to know what these fields contain.
  struct StackMapEntry {
    uint32_t dex_pc;
    uint32_t native_pc_offset;
    uint32_t register_mask;
    BitVector* sp_mask;
    uint32_t num_dex_registers;
    uint8_t inlining_depth;
    size_t dex_register_maps_start_index;
    size_t inline_infos_start_index;
  };

  struct InlineInfoEntry {
    uint32_t method_index;
  };

  void AddStackMapEntry(uint32_t dex_pc,
                        uint32_t native_pc_offset,
                        uint32_t register_mask,
                        BitVector* sp_mask,
                        uint32_t num_dex_registers,
                        uint8_t inlining_depth) {
    StackMapEntry entry;
    entry.dex_pc = dex_pc;
    entry.native_pc_offset = native_pc_offset;
    entry.register_mask = register_mask;
    entry.sp_mask = sp_mask;
    entry.num_dex_registers = num_dex_registers;
    entry.inlining_depth = inlining_depth;
    entry.dex_register_maps_start_index = dex_register_maps_.Size();
    entry.inline_infos_start_index = inline_infos_.Size();
    stack_maps_.Add(entry);

    if (sp_mask != nullptr) {
      stack_mask_max_ = std::max(stack_mask_max_, sp_mask->GetHighestBitSet());
    }
    if (inlining_depth > 0) {
      number_of_stack_maps_with_inline_info_++;
    }
  }

  void AddDexRegisterEntry(DexRegisterLocation::Kind kind, int32_t value) {
    // Ensure we only use non-compressed location kind at this stage.
    DCHECK(DexRegisterLocation::IsShortLocationKind(kind))
        << DexRegisterLocation::PrettyDescriptor(kind);
    dex_register_maps_.Add(DexRegisterLocation(kind, value));
  }

  void AddInlineInfoEntry(uint32_t method_index) {
    InlineInfoEntry entry;
    entry.method_index = method_index;
    inline_infos_.Add(entry);
  }

  size_t ComputeNeededSize() const {
    size_t common_size =
        CodeInfo::kFixedSize
        + ComputeStackMapSize()
        + ComputeDexRegisterMapsSize()
        + ComputeInlineInfoSize();

    switch (kDexRegisterMapEncoding) {
      case kDexRegisterLocationList:
      case kDexRegisterCompressedLocationList:
        return common_size;
    }
  }

  size_t ComputeStackMapSize() const {
    return stack_maps_.Size() * StackMap::ComputeAlignedStackMapSize(stack_mask_max_);
  }

  // Compute the compressed location kind of a Dex register entry,
  // suitable for use with an art::DexRegisterCompressedMap.
  static DexRegisterLocation::Kind ComputeCompressedMapLocationKind(const DexRegisterLocation& entry) {
    DCHECK_EQ(kDexRegisterMapEncoding, kDexRegisterCompressedLocationList);
    switch (entry.kind) {
      case DexRegisterLocation::Kind::kNone:
        DCHECK_EQ(entry.value, 0);
        return DexRegisterLocation::Kind::kNone;

      case DexRegisterLocation::Kind::kInRegister:
        DCHECK(0 <= entry.value && entry.value < 32) << entry.value;
        return DexRegisterLocation::Kind::kInRegister;

      case DexRegisterLocation::Kind::kInFpuRegister:
        DCHECK(0 <= entry.value && entry.value < 32) << entry.value;
        return DexRegisterLocation::Kind::kInFpuRegister;

      case DexRegisterLocation::Kind::kInStack:
        DCHECK_EQ(entry.value % kFrameSlotSize, 0);
        return IsUint<DexRegisterCompressedMap::kValueBits>(entry.value / kFrameSlotSize)
            ? DexRegisterLocation::Kind::kInStack
            : DexRegisterLocation::Kind::kInStackLargeOffset;

      case DexRegisterLocation::Kind::kConstant:
        return IsUint<DexRegisterCompressedMap::kValueBits>(entry.value)
            ? DexRegisterLocation::Kind::kConstant
            : DexRegisterLocation::Kind::kConstantBigValue;

      default:
        LOG(ERROR) << "Unexpected location kind"
                   << DexRegisterLocation::PrettyDescriptor(entry.kind);
        UNREACHABLE();
    }
  }

  // Compute the size of entry as a potentially compressed location.
  static size_t ComputeEntrySizeAsCompressedLocation(const DexRegisterLocation& entry) {
    DCHECK_EQ(kDexRegisterMapEncoding, kDexRegisterCompressedLocationList);
    return DexRegisterCompressedMap::EntrySize(entry);
  }

  size_t ComputeDexRegisterMapSize(const StackMapEntry& entry) {
    return DexRegisterMap::kFixedSize
        + entry.num_dex_registers * DexRegisterMap::SingleEntrySize();
  }

  size_t ComputeDexRegisterCompressedMapSize(const StackMapEntry& entry) const {
    DCHECK_EQ(kDexRegisterMapEncoding, kDexRegisterCompressedLocationList);
    size_t size = DexRegisterCompressedMap::kFixedSize;
    for (size_t j = 0; j < entry.num_dex_registers; ++j) {
      DexRegisterLocation register_entry =
          dex_register_maps_.Get(entry.dex_register_maps_start_index + j);
      size += ComputeEntrySizeAsCompressedLocation(register_entry);
    }
    return size;
  }

  size_t ComputeDexRegisterMapsSize() const {
    switch (kDexRegisterMapEncoding) {
      case kDexRegisterLocationList:
        return stack_maps_.Size() * DexRegisterMap::kFixedSize
            // For each dex register entry.
            + dex_register_maps_.Size() * DexRegisterMap::SingleEntrySize();

      case kDexRegisterCompressedLocationList: {
        size_t size = stack_maps_.Size() * DexRegisterMap::kFixedSize;
        // The size of each register location depends on the type of
        // the entry.
        for (size_t i = 0, e = dex_register_maps_.Size(); i < e; ++i) {
          DexRegisterLocation entry = dex_register_maps_.Get(i);
          size += ComputeEntrySizeAsCompressedLocation(entry);
        }
        return size;
      }
    };
  }

  size_t ComputeInlineInfoSize() const {
    return inline_infos_.Size() * InlineInfo::SingleEntrySize()
      // For encoding the depth.
      + (number_of_stack_maps_with_inline_info_ * InlineInfo::kFixedSize);
  }

  size_t ComputeDexRegisterMapStart() const {
    return CodeInfo::kFixedSize + ComputeStackMapSize();
  }

  size_t ComputeInlineInfoStart() const {
    return ComputeDexRegisterMapStart() + ComputeDexRegisterMapsSize();
  }

  void FillIn(MemoryRegion region) {
    CodeInfo code_info(region);
    code_info.SetOverallSize(region.size());

    size_t stack_mask_size = StackMaskEncodingSize(stack_mask_max_);
    uint8_t* memory_start = region.start();

    MemoryRegion dex_register_maps_region = region.Subregion(
      ComputeDexRegisterMapStart(),
      ComputeDexRegisterMapsSize());

    MemoryRegion inline_infos_region = region.Subregion(
      ComputeInlineInfoStart(),
      ComputeInlineInfoSize());

    code_info.SetNumberOfStackMaps(stack_maps_.Size());
    code_info.SetStackMaskSize(stack_mask_size);

    uintptr_t next_dex_register_map_offset = 0;
    uintptr_t next_inline_info_offset = 0;
    for (size_t i = 0, e = stack_maps_.Size(); i < e; ++i) {
      StackMap stack_map = code_info.GetStackMapAt(i);
      StackMapEntry entry = stack_maps_.Get(i);

      stack_map.SetDexPc(entry.dex_pc);
      stack_map.SetNativePcOffset(entry.native_pc_offset);
      stack_map.SetRegisterMask(entry.register_mask);
      if (entry.sp_mask != nullptr) {
        stack_map.SetStackMask(*entry.sp_mask);
      }

      if (entry.num_dex_registers != 0) {
        switch (kDexRegisterMapEncoding) {
          case kDexRegisterLocationList: {
            // Set the Dex register map.
            MemoryRegion register_region =
                dex_register_maps_region.Subregion(
                    next_dex_register_map_offset,
                    ComputeDexRegisterMapSize(entry));
            next_dex_register_map_offset += register_region.size();
            DexRegisterMap dex_register_map(register_region);

            stack_map.SetDexRegisterMapOffset(register_region.start() - memory_start);

            for (size_t j = 0; j < entry.num_dex_registers; ++j) {
              DexRegisterLocation register_entry =
                  dex_register_maps_.Get(entry.dex_register_maps_start_index + j);
              dex_register_map.SetRegisterInfo(j, register_entry.kind, register_entry.value);
            }
            break;
          }

          case kDexRegisterCompressedLocationList: {
            // Set the Dex register compressed map.
            MemoryRegion register_region =
                dex_register_maps_region.Subregion(
                    next_dex_register_map_offset,
                    ComputeDexRegisterCompressedMapSize(entry));
            next_dex_register_map_offset += register_region.size();
            DexRegisterCompressedMap dex_register_compressed_map(register_region);
            stack_map.SetDexRegisterMapOffset(register_region.start() - memory_start);

            // Offset in `dex_register_compressed_map` where to store
            // the next register entry.
            size_t offset = DexRegisterCompressedMap::kFixedSize;
            for (size_t j = 0; j < entry.num_dex_registers; ++j) {
              DexRegisterLocation register_entry =
                  dex_register_maps_.Get(entry.dex_register_maps_start_index + j);
              DexRegisterLocation::Kind compressed_map_kind =
                  ComputeCompressedMapLocationKind(register_entry);
              dex_register_compressed_map.SetRegisterInfo(offset,
                                                          compressed_map_kind,
                                                          register_entry.value);
              offset += ComputeEntrySizeAsCompressedLocation(register_entry);
            }
            // Ensure we reached the end of the Dex registers region.
            DCHECK_EQ(offset, register_region.size());
            break;
          }
        }
      } else {
        stack_map.SetDexRegisterMapOffset(StackMap::kNoDexRegisterMap);
      }

      // Set the inlining info.
      if (entry.inlining_depth != 0) {
        MemoryRegion inline_region = inline_infos_region.Subregion(
            next_inline_info_offset,
            InlineInfo::kFixedSize + entry.inlining_depth * InlineInfo::SingleEntrySize());
        next_inline_info_offset += inline_region.size();
        InlineInfo inline_info(inline_region);

        stack_map.SetInlineDescriptorOffset(inline_region.start() - memory_start);

        inline_info.SetDepth(entry.inlining_depth);
        for (size_t j = 0; j < entry.inlining_depth; ++j) {
          InlineInfoEntry inline_entry = inline_infos_.Get(j + entry.inline_infos_start_index);
          inline_info.SetMethodReferenceIndexAtDepth(j, inline_entry.method_index);
        }
      } else {
        stack_map.SetInlineDescriptorOffset(StackMap::kNoInlineInfo);
      }
    }
  }

 private:
  GrowableArray<StackMapEntry> stack_maps_;
  GrowableArray<DexRegisterLocation> dex_register_maps_;
  GrowableArray<InlineInfoEntry> inline_infos_;
  int stack_mask_max_;
  size_t number_of_stack_maps_with_inline_info_;

  DISALLOW_COPY_AND_ASSIGN(StackMapStream);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
