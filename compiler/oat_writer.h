/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_COMPILER_OAT_WRITER_H_
#define ART_COMPILER_OAT_WRITER_H_

#include <stdint.h>
#include <cstddef>
#include <memory>

#include "base/dchecked_vector.h"
#include "linker/relative_patcher.h"  // For linker::RelativePatcherTargetProvider.
#include "mem_map.h"
#include "method_reference.h"
#include "mirror/class.h"
#include "oat.h"
#include "os.h"
#include "safe_map.h"

namespace art {

class BitVector;
class CompiledMethod;
class CompilerDriver;
class ImageWriter;
class OutputStream;
class TimingLogger;
class TypeLookupTable;
class ZipEntry;

namespace dwarf {
struct MethodDebugInfo;
}  // namespace dwarf

// OatHeader         variable length with count of D OatDexFiles
//
// OatDexFile[0]     one variable sized OatDexFile with offsets to Dex and OatClasses
// OatDexFile[1]
// ...
// OatDexFile[D]
//
// Dex[0]            one variable sized DexFile for each OatDexFile.
// Dex[1]            these are literal copies of the input .dex files.
// ...
// Dex[D]
//
// TypeLookupTable[0] one descriptor to class def index hash table for each OatDexFile.
// TypeLookupTable[1]
// ...
// TypeLookupTable[D]
//
// ClassOffsets[0]   one table of OatClass offsets for each class def for each OatDexFile.
// ClassOffsets[1]
// ...
// ClassOffsets[D]
//
// OatClass[0]       one variable sized OatClass for each of C DexFile::ClassDefs
// OatClass[1]       contains OatClass entries with class status, offsets to code, etc.
// ...
// OatClass[C]
//
// GcMap             one variable sized blob with GC map.
// GcMap             GC maps are deduplicated.
// ...
// GcMap
//
// VmapTable         one variable sized VmapTable blob (quick compiler only).
// VmapTable         VmapTables are deduplicated.
// ...
// VmapTable
//
// MappingTable      one variable sized blob with MappingTable (quick compiler only).
// MappingTable      MappingTables are deduplicated.
// ...
// MappingTable
//
// padding           if necessary so that the following code will be page aligned
//
// OatMethodHeader   fixed size header for a CompiledMethod including the size of the MethodCode.
// MethodCode        one variable sized blob with the code of a CompiledMethod.
// OatMethodHeader   (OatMethodHeader, MethodCode) pairs are deduplicated.
// MethodCode
// ...
// OatMethodHeader
// MethodCode
//
class OatWriter {
 public:
  // Defines the location of the raw dex file to write.
  struct RawDexFileLocation {
    // Exactly one of these must be non-null.
    ZipEntry* zip_entry;
    File* raw_file;
  };

  explicit OatWriter(InstructionSet instruction_set,
                     const InstructionSetFeatures* instruction_set_features,
                     const ArrayRef<const char* const>& dex_file_locations,
                     bool compiling_boot_image,
                     uint32_t image_file_location_oat_checksum,
                     uintptr_t image_file_location_oat_begin,
                     int32_t image_patch_delta,
                     SafeMap<std::string, std::string>* key_value_store,
                     TimingLogger* timings);

  // To produce a valid oat file, the user must call in order
  //   - WriteDexFiles(),
  //   - TODO: OpenDexFiles(),
  //   - WriteTypeLookupTables(),
  //   - WriteOatDexFiles(),
  //   - PrepareLayout(),
  //   - WriteRodata(),
  //   - WriteCode(),
  //   - WriteHeader().

  // Write raw dex files to the .rodata section.
  bool WriteDexFiles(OutputStream* rodata,
                     File* file,
                     const ArrayRef<RawDexFileLocation const>& dex_files);
  // TODO: Provide two overloads, one that takes ArrayRef<ZipEntry* const>
  // (and a file descriptor) and one that takes just file names to copy.
  bool WriteDexFiles(OutputStream* rodata, const std::vector<const DexFile*>& dex_files);
  // TODO: Provide OpenDexFiles() that opens the written files.
  // bool OpenDexFiles(int fd, std::vector<const DexFile*>* dex_files, std::string* error_msg);
  // Write the lookup table for a dex file.
  bool WriteTypeLookupTables(OutputStream* rodata, const std::vector<const DexFile*>& dex_files);
  // Write an OatDexFile for each dex file.
  bool WriteOatDexFiles(OutputStream* rodata,
                        const std::vector<const DexFile*>& dex_files);
  // Prepare layout of remaining data.
  void PrepareLayout(const CompilerDriver* compiler,
                     ImageWriter* image_writer);
  // Write the rest of .rodata section (ClassOffsets[], OatClass[], maps).
  bool WriteRodata(OutputStream* out);
  // Write the code to the .text section.
  bool WriteCode(OutputStream* out);
  // Write the oat header. This finalizes the oat file.
  bool WriteHeader(OutputStream* out);

  // Returns whether the oat file has an associated image.
  bool HasImage() const {
    // Since the image is being created at the same time as the oat file,
    // check if there's an image writer.
    return image_writer_ != nullptr;
  }

  bool HasBootImage() const {
    return compiling_boot_image_;
  }

  const OatHeader& GetOatHeader() const {
    return *oat_header_;
  }

  size_t GetSize() const {
    return size_;
  }

  size_t GetBssSize() const {
    return bss_size_;
  }

  ArrayRef<const uintptr_t> GetAbsolutePatchLocations() const {
    return ArrayRef<const uintptr_t>(absolute_patch_locations_);
  }

  ~OatWriter();

  ArrayRef<const dwarf::MethodDebugInfo> GetMethodDebugInfo() const {
    return ArrayRef<const dwarf::MethodDebugInfo>(method_info_);
  }

  const CompilerDriver* GetCompilerDriver() {
    return compiler_driver_;
  }

 private:
  class OatDexFile;
  class OatClass;

  // The DataAccess classes are helper classes that provide access to members related to
  // a given map, i.e. GC map, mapping table or vmap table. By abstracting these away
  // we can share a lot of code for processing the maps with template classes below.
  struct GcMapDataAccess;
  struct MappingTableDataAccess;
  struct VmapTableDataAccess;

  // The function VisitDexMethods() below iterates through all the methods in all
  // the compiled dex files in order of their definitions. The method visitor
  // classes provide individual bits of processing for each of the passes we need to
  // first collect the data we want to write to the oat file and then, in later passes,
  // to actually write it.
  class DexMethodVisitor;
  class OatDexMethodVisitor;
  class InitOatClassesMethodVisitor;
  class InitCodeMethodVisitor;
  template <typename DataAccess>
  class InitMapMethodVisitor;
  class InitImageMethodVisitor;
  class WriteCodeMethodVisitor;
  template <typename DataAccess>
  class WriteMapMethodVisitor;

  // Visit all the methods in all the compiled dex files in their definition order
  // with a given DexMethodVisitor.
  bool VisitDexMethods(DexMethodVisitor* visitor);

  size_t InitOatHeader(InstructionSet instruction_set,
                       const InstructionSetFeatures* instruction_set_features,
                       uint32_t num_dex_files,
                       uint32_t image_file_location_oat_checksum,
                       uintptr_t image_file_location_oat_begin,
                       int32_t image_patch_delta,
                       SafeMap<std::string, std::string>* key_value_store);
  size_t InitOatDexFiles(size_t offset, const ArrayRef<const char* const>& dex_file_locations);
  size_t InitOatClasses(size_t offset);
  size_t InitOatMaps(size_t offset);
  size_t InitOatCode(size_t offset);
  size_t InitOatCodeDexFiles(size_t offset);

  bool WriteClassOffsets(OutputStream* out);
  bool WriteClasses(OutputStream* out);
  size_t WriteMaps(OutputStream* out, const size_t file_offset, size_t relative_offset);
  size_t WriteCode(OutputStream* out, const size_t file_offset, size_t relative_offset);
  size_t WriteCodeDexFiles(OutputStream* out, const size_t file_offset, size_t relative_offset);

  bool GetOatDataOffset(OutputStream* out);
  bool ReadDexFileHeader(File* file, OatDexFile* oat_dex_file);
  bool SeekToDexFile(OutputStream* out, File* file, OatDexFile* oat_dex_file);
  bool WriteDexFile(OutputStream* rodata, File* file, OatDexFile* oat_dex_file, ZipEntry* dex_file);
  bool WriteDexFile(OutputStream* rodata, File* file, OatDexFile* oat_dex_file, File* dex_file);
  bool WriteCodeAlignment(OutputStream* out, uint32_t aligned_code_delta);
  bool WriteData(OutputStream* out, const void* data, size_t size);

  enum class WriteState {
    kUninitialized,
    kWriteDexFiles,
    kWriteLookupTables,
    kWriteOatDexFiles,
    kPrepareLayout,
    kWriteRoData,
    kWriteText,
    kWriteHeader,
    kDone
  };

  WriteState write_state_;
  TimingLogger* timings_;

  dchecked_vector<dwarf::MethodDebugInfo> method_info_;

  const CompilerDriver* compiler_driver_;
  ImageWriter* image_writer_;
  const bool compiling_boot_image_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // Size required for Oat data structures.
  size_t size_;

  // The size of the required .bss section holding the DexCache data.
  size_t bss_size_;

  // Offsets of the dex cache arrays for each app dex file. For the
  // boot image, this information is provided by the ImageWriter.
  SafeMap<const DexFile*, size_t> dex_cache_arrays_offsets_;  // DexFiles not owned.

  // Offset of the oat data from the start of the mmapped region of the elf file.
  size_t oat_data_offset_;

  // data to write
  std::unique_ptr<OatHeader> oat_header_;
  dchecked_vector<OatDexFile> oat_dex_files_;
  dchecked_vector<OatClass> oat_classes_;
  std::unique_ptr<const std::vector<uint8_t>> jni_dlsym_lookup_;
  std::unique_ptr<const std::vector<uint8_t>> quick_generic_jni_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_imt_conflict_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_resolution_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_to_interpreter_bridge_;

  // output stats
  uint32_t size_dex_file_alignment_;
  uint32_t size_executable_offset_alignment_;
  uint32_t size_oat_header_;
  uint32_t size_oat_header_key_value_store_;
  uint32_t size_dex_file_;
  uint32_t size_interpreter_to_interpreter_bridge_;
  uint32_t size_interpreter_to_compiled_code_bridge_;
  uint32_t size_jni_dlsym_lookup_;
  uint32_t size_quick_generic_jni_trampoline_;
  uint32_t size_quick_imt_conflict_trampoline_;
  uint32_t size_quick_resolution_trampoline_;
  uint32_t size_quick_to_interpreter_bridge_;
  uint32_t size_trampoline_alignment_;
  uint32_t size_method_header_;
  uint32_t size_code_;
  uint32_t size_code_alignment_;
  uint32_t size_relative_call_thunks_;
  uint32_t size_misc_thunks_;
  uint32_t size_mapping_table_;
  uint32_t size_vmap_table_;
  uint32_t size_gc_map_;
  uint32_t size_oat_dex_file_location_size_;
  uint32_t size_oat_dex_file_location_data_;
  uint32_t size_oat_dex_file_location_checksum_;
  uint32_t size_oat_dex_file_offset_;
  uint32_t size_oat_dex_file_class_offsets_offset_;
  uint32_t size_oat_dex_file_lookup_table_offset_;
  uint32_t size_oat_lookup_table_alignment_;
  uint32_t size_oat_lookup_table_;
  uint32_t size_oat_class_offsets_alignment_;
  uint32_t size_oat_class_offsets_;
  uint32_t size_oat_class_type_;
  uint32_t size_oat_class_status_;
  uint32_t size_oat_class_method_bitmaps_;
  uint32_t size_oat_class_method_offsets_;

  std::unique_ptr<linker::RelativePatcher> relative_patcher_;

  // The locations of absolute patches relative to the start of the executable section.
  dchecked_vector<uintptr_t> absolute_patch_locations_;

  // Map method reference to assigned offset.
  // Wrap the map in a class implementing linker::RelativePatcherTargetProvider.
  class MethodOffsetMap FINAL : public linker::RelativePatcherTargetProvider {
   public:
    std::pair<bool, uint32_t> FindMethodOffset(MethodReference ref) OVERRIDE;
    SafeMap<MethodReference, uint32_t, MethodReferenceComparator> map;
  };
  MethodOffsetMap method_offset_map_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace art

#endif  // ART_COMPILER_OAT_WRITER_H_
