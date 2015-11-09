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

#include "image_space.h"

#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <random>

#include "art_method.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "base/scoped_flock.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "oat_file.h"
#include "os.h"
#include "space-inl.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

Atomic<uint32_t> ImageSpace::bitmap_index_(0);

ImageSpace::ImageSpace(const std::string& image_filename, const char* image_location,
                       MemMap* mem_map, accounting::ContinuousSpaceBitmap* live_bitmap,
                       uint8_t* end)
    : MemMapSpace(image_filename, mem_map, mem_map->Begin(), end, end,
                  kGcRetentionPolicyNeverCollect),
      image_location_(image_location) {
  DCHECK(live_bitmap != nullptr);
  live_bitmap_.reset(live_bitmap);
}

static int32_t ChooseRelocationOffsetDelta(int32_t min_delta, int32_t max_delta) {
  CHECK_ALIGNED(min_delta, kPageSize);
  CHECK_ALIGNED(max_delta, kPageSize);
  CHECK_LT(min_delta, max_delta);

  std::default_random_engine generator;
  generator.seed(NanoTime() * getpid());
  std::uniform_int_distribution<int32_t> distribution(min_delta, max_delta);
  int32_t r = distribution(generator);
  if (r % 2 == 0) {
    r = RoundUp(r, kPageSize);
  } else {
    r = RoundDown(r, kPageSize);
  }
  CHECK_LE(min_delta, r);
  CHECK_GE(max_delta, r);
  CHECK_ALIGNED(r, kPageSize);
  return r;
}

// We are relocating or generating the core image. We should get rid of everything. It is all
// out-of-date. We also don't really care if this fails since it is just a convenience.
// Adapted from prune_dex_cache(const char* subdir) in frameworks/native/cmds/installd/commands.c
// Note this should only be used during first boot.
static void RealPruneDalvikCache(const std::string& cache_dir_path);

static void PruneDalvikCache(InstructionSet isa) {
  CHECK_NE(isa, kNone);
  // Prune the base /data/dalvik-cache.
  RealPruneDalvikCache(GetDalvikCacheOrDie(".", false));
  // Prune /data/dalvik-cache/<isa>.
  RealPruneDalvikCache(GetDalvikCacheOrDie(GetInstructionSetString(isa), false));
}

static void RealPruneDalvikCache(const std::string& cache_dir_path) {
  if (!OS::DirectoryExists(cache_dir_path.c_str())) {
    return;
  }
  DIR* cache_dir = opendir(cache_dir_path.c_str());
  if (cache_dir == nullptr) {
    PLOG(WARNING) << "Unable to open " << cache_dir_path << " to delete it's contents";
    return;
  }

  for (struct dirent* de = readdir(cache_dir); de != nullptr; de = readdir(cache_dir)) {
    const char* name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    // We only want to delete regular files and symbolic links.
    if (de->d_type != DT_REG && de->d_type != DT_LNK) {
      if (de->d_type != DT_DIR) {
        // We do expect some directories (namely the <isa> for pruning the base dalvik-cache).
        LOG(WARNING) << "Unexpected file type of " << std::hex << de->d_type << " encountered.";
      }
      continue;
    }
    std::string cache_file(cache_dir_path);
    cache_file += '/';
    cache_file += name;
    if (TEMP_FAILURE_RETRY(unlink(cache_file.c_str())) != 0) {
      PLOG(ERROR) << "Unable to unlink " << cache_file;
      continue;
    }
  }
  CHECK_EQ(0, TEMP_FAILURE_RETRY(closedir(cache_dir))) << "Unable to close directory.";
}

// We write out an empty file to the zygote's ISA specific cache dir at the start of
// every zygote boot and delete it when the boot completes. If we find a file already
// present, it usually means the boot didn't complete. We wipe the entire dalvik
// cache if that's the case.
static void MarkZygoteStart(const InstructionSet isa, const uint32_t max_failed_boots) {
  const std::string isa_subdir = GetDalvikCacheOrDie(GetInstructionSetString(isa), false);
  const std::string boot_marker = isa_subdir + "/.booting";
  const char* file_name = boot_marker.c_str();

  uint32_t num_failed_boots = 0;
  std::unique_ptr<File> file(OS::OpenFileReadWrite(file_name));
  if (file.get() == nullptr) {
    file.reset(OS::CreateEmptyFile(file_name));

    if (file.get() == nullptr) {
      PLOG(WARNING) << "Failed to create boot marker.";
      return;
    }
  } else {
    if (!file->ReadFully(&num_failed_boots, sizeof(num_failed_boots))) {
      PLOG(WARNING) << "Failed to read boot marker.";
      file->Erase();
      return;
    }
  }

  if (max_failed_boots != 0 && num_failed_boots > max_failed_boots) {
    LOG(WARNING) << "Incomplete boot detected. Pruning dalvik cache";
    RealPruneDalvikCache(isa_subdir);
  }

  ++num_failed_boots;
  VLOG(startup) << "Number of failed boots on : " << boot_marker << " = " << num_failed_boots;

  if (lseek(file->Fd(), 0, SEEK_SET) == -1) {
    PLOG(WARNING) << "Failed to write boot marker.";
    file->Erase();
    return;
  }

  if (!file->WriteFully(&num_failed_boots, sizeof(num_failed_boots))) {
    PLOG(WARNING) << "Failed to write boot marker.";
    file->Erase();
    return;
  }

  if (file->FlushCloseOrErase() != 0) {
    PLOG(WARNING) << "Failed to flush boot marker.";
  }
}

static bool GenerateImage(const std::string& image_filename, InstructionSet image_isa,
                          std::string* error_msg) {
  const std::string boot_class_path_string(Runtime::Current()->GetBootClassPathString());
  std::vector<std::string> boot_class_path;
  Split(boot_class_path_string, ':', &boot_class_path);
  if (boot_class_path.empty()) {
    *error_msg = "Failed to generate image because no boot class path specified";
    return false;
  }
  // We should clean up so we are more likely to have room for the image.
  if (Runtime::Current()->IsZygote()) {
    LOG(INFO) << "Pruning dalvik-cache since we are generating an image and will need to recompile";
    PruneDalvikCache(image_isa);
  }

  std::vector<std::string> arg_vector;

  std::string dex2oat(Runtime::Current()->GetCompilerExecutable());
  arg_vector.push_back(dex2oat);

  std::string image_option_string("--image=");
  image_option_string += image_filename;
  arg_vector.push_back(image_option_string);

  for (size_t i = 0; i < boot_class_path.size(); i++) {
    arg_vector.push_back(std::string("--dex-file=") + boot_class_path[i]);
  }

  std::string oat_file_option_string("--oat-file=");
  oat_file_option_string += ImageHeader::GetOatLocationFromImageLocation(image_filename);
  arg_vector.push_back(oat_file_option_string);

  // Note: we do not generate a fully debuggable boot image so we do not pass the
  // compiler flag --debuggable here.

  Runtime::Current()->AddCurrentRuntimeFeaturesAsDex2OatArguments(&arg_vector);
  CHECK_EQ(image_isa, kRuntimeISA)
      << "We should always be generating an image for the current isa.";

  int32_t base_offset = ChooseRelocationOffsetDelta(ART_BASE_ADDRESS_MIN_DELTA,
                                                    ART_BASE_ADDRESS_MAX_DELTA);
  LOG(INFO) << "Using an offset of 0x" << std::hex << base_offset << " from default "
            << "art base address of 0x" << std::hex << ART_BASE_ADDRESS;
  arg_vector.push_back(StringPrintf("--base=0x%x", ART_BASE_ADDRESS + base_offset));

  if (!kIsTargetBuild) {
    arg_vector.push_back("--host");
  }

  const std::vector<std::string>& compiler_options = Runtime::Current()->GetImageCompilerOptions();
  for (size_t i = 0; i < compiler_options.size(); ++i) {
    arg_vector.push_back(compiler_options[i].c_str());
  }

  std::string command_line(Join(arg_vector, ' '));
  LOG(INFO) << "GenerateImage: " << command_line;
  return Exec(arg_vector, error_msg);
}

bool ImageSpace::FindImageFilename(const char* image_location,
                                   const InstructionSet image_isa,
                                   std::string* system_filename,
                                   bool* has_system,
                                   std::string* cache_filename,
                                   bool* dalvik_cache_exists,
                                   bool* has_cache,
                                   bool* is_global_cache) {
  *has_system = false;
  *has_cache = false;
  // image_location = /system/framework/boot.art
  // system_image_location = /system/framework/<image_isa>/boot.art
  std::string system_image_filename(GetSystemImageFilename(image_location, image_isa));
  if (OS::FileExists(system_image_filename.c_str())) {
    *system_filename = system_image_filename;
    *has_system = true;
  }

  bool have_android_data = false;
  *dalvik_cache_exists = false;
  std::string dalvik_cache;
  GetDalvikCache(GetInstructionSetString(image_isa), true, &dalvik_cache,
                 &have_android_data, dalvik_cache_exists, is_global_cache);

  if (have_android_data && *dalvik_cache_exists) {
    // Always set output location even if it does not exist,
    // so that the caller knows where to create the image.
    //
    // image_location = /system/framework/boot.art
    // *image_filename = /data/dalvik-cache/<image_isa>/boot.art
    std::string error_msg;
    if (!GetDalvikCacheFilename(image_location, dalvik_cache.c_str(), cache_filename, &error_msg)) {
      LOG(WARNING) << error_msg;
      return *has_system;
    }
    *has_cache = OS::FileExists(cache_filename->c_str());
  }
  return *has_system || *has_cache;
}

static bool ReadSpecificImageHeader(const char* filename, ImageHeader* image_header) {
    std::unique_ptr<File> image_file(OS::OpenFileForReading(filename));
    if (image_file.get() == nullptr) {
      return false;
    }
    const bool success = image_file->ReadFully(image_header, sizeof(ImageHeader));
    if (!success || !image_header->IsValid()) {
      return false;
    }
    return true;
}

// Relocate the image at image_location to dest_filename and relocate it by a random amount.
static bool RelocateImage(const char* image_location, const char* dest_filename,
                               InstructionSet isa, std::string* error_msg) {
  // We should clean up so we are more likely to have room for the image.
  if (Runtime::Current()->IsZygote()) {
    LOG(INFO) << "Pruning dalvik-cache since we are relocating an image and will need to recompile";
    PruneDalvikCache(isa);
  }

  std::string patchoat(Runtime::Current()->GetPatchoatExecutable());

  std::string input_image_location_arg("--input-image-location=");
  input_image_location_arg += image_location;

  std::string output_image_filename_arg("--output-image-file=");
  output_image_filename_arg += dest_filename;

  std::string input_oat_location_arg("--input-oat-location=");
  input_oat_location_arg += ImageHeader::GetOatLocationFromImageLocation(image_location);

  std::string output_oat_filename_arg("--output-oat-file=");
  output_oat_filename_arg += ImageHeader::GetOatLocationFromImageLocation(dest_filename);

  std::string instruction_set_arg("--instruction-set=");
  instruction_set_arg += GetInstructionSetString(isa);

  std::string base_offset_arg("--base-offset-delta=");
  StringAppendF(&base_offset_arg, "%d", ChooseRelocationOffsetDelta(ART_BASE_ADDRESS_MIN_DELTA,
                                                                    ART_BASE_ADDRESS_MAX_DELTA));

  std::vector<std::string> argv;
  argv.push_back(patchoat);

  argv.push_back(input_image_location_arg);
  argv.push_back(output_image_filename_arg);

  argv.push_back(input_oat_location_arg);
  argv.push_back(output_oat_filename_arg);

  argv.push_back(instruction_set_arg);
  argv.push_back(base_offset_arg);

  std::string command_line(Join(argv, ' '));
  LOG(INFO) << "RelocateImage: " << command_line;
  return Exec(argv, error_msg);
}

static ImageHeader* ReadSpecificImageHeader(const char* filename, std::string* error_msg) {
  std::unique_ptr<ImageHeader> hdr(new ImageHeader);
  if (!ReadSpecificImageHeader(filename, hdr.get())) {
    *error_msg = StringPrintf("Unable to read image header for %s", filename);
    return nullptr;
  }
  return hdr.release();
}

ImageHeader* ImageSpace::ReadImageHeaderOrDie(const char* image_location,
                                              const InstructionSet image_isa) {
  std::string error_msg;
  ImageHeader* image_header = ReadImageHeader(image_location, image_isa, &error_msg);
  if (image_header == nullptr) {
    LOG(FATAL) << error_msg;
  }
  return image_header;
}

ImageHeader* ImageSpace::ReadImageHeader(const char* image_location,
                                         const InstructionSet image_isa,
                                         std::string* error_msg) {
  std::string system_filename;
  bool has_system = false;
  std::string cache_filename;
  bool has_cache = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = false;
  if (FindImageFilename(image_location, image_isa, &system_filename, &has_system,
                        &cache_filename, &dalvik_cache_exists, &has_cache, &is_global_cache)) {
    if (Runtime::Current()->ShouldRelocate()) {
      if (has_system && has_cache) {
        std::unique_ptr<ImageHeader> sys_hdr(new ImageHeader);
        std::unique_ptr<ImageHeader> cache_hdr(new ImageHeader);
        if (!ReadSpecificImageHeader(system_filename.c_str(), sys_hdr.get())) {
          *error_msg = StringPrintf("Unable to read image header for %s at %s",
                                    image_location, system_filename.c_str());
          return nullptr;
        }
        if (!ReadSpecificImageHeader(cache_filename.c_str(), cache_hdr.get())) {
          *error_msg = StringPrintf("Unable to read image header for %s at %s",
                                    image_location, cache_filename.c_str());
          return nullptr;
        }
        if (sys_hdr->GetOatChecksum() != cache_hdr->GetOatChecksum()) {
          *error_msg = StringPrintf("Unable to find a relocated version of image file %s",
                                    image_location);
          return nullptr;
        }
        return cache_hdr.release();
      } else if (!has_cache) {
        *error_msg = StringPrintf("Unable to find a relocated version of image file %s",
                                  image_location);
        return nullptr;
      } else if (!has_system && has_cache) {
        // This can probably just use the cache one.
        return ReadSpecificImageHeader(cache_filename.c_str(), error_msg);
      }
    } else {
      // We don't want to relocate, Just pick the appropriate one if we have it and return.
      if (has_system && has_cache) {
        // We want the cache if the checksum matches, otherwise the system.
        std::unique_ptr<ImageHeader> system(ReadSpecificImageHeader(system_filename.c_str(),
                                                                    error_msg));
        std::unique_ptr<ImageHeader> cache(ReadSpecificImageHeader(cache_filename.c_str(),
                                                                   error_msg));
        if (system.get() == nullptr ||
            (cache.get() != nullptr && cache->GetOatChecksum() == system->GetOatChecksum())) {
          return cache.release();
        } else {
          return system.release();
        }
      } else if (has_system) {
        return ReadSpecificImageHeader(system_filename.c_str(), error_msg);
      } else if (has_cache) {
        return ReadSpecificImageHeader(cache_filename.c_str(), error_msg);
      }
    }
  }

  *error_msg = StringPrintf("Unable to find image file for %s", image_location);
  return nullptr;
}

static bool ChecksumsMatch(const char* image_a, const char* image_b) {
  ImageHeader hdr_a;
  ImageHeader hdr_b;
  return ReadSpecificImageHeader(image_a, &hdr_a) && ReadSpecificImageHeader(image_b, &hdr_b)
      && hdr_a.GetOatChecksum() == hdr_b.GetOatChecksum();
}

static bool ImageCreationAllowed(bool is_global_cache, std::string* error_msg) {
  // Anyone can write into a "local" cache.
  if (!is_global_cache) {
    return true;
  }

  // Only the zygote is allowed to create the global boot image.
  if (Runtime::Current()->IsZygote()) {
    return true;
  }

  *error_msg = "Only the zygote can create the global boot image.";
  return false;
}

static constexpr uint64_t kLowSpaceValue = 50 * MB;
static constexpr uint64_t kTmpFsSentinelValue = 384 * MB;

// Read the free space of the cache partition and make a decision whether to keep the generated
// image. This is to try to mitigate situations where the system might run out of space later.
static bool CheckSpace(const std::string& cache_filename, std::string* error_msg) {
  // Using statvfs vs statvfs64 because of b/18207376, and it is enough for all practical purposes.
  struct statvfs buf;

  int res = TEMP_FAILURE_RETRY(statvfs(cache_filename.c_str(), &buf));
  if (res != 0) {
    // Could not stat. Conservatively tell the system to delete the image.
    *error_msg = "Could not stat the filesystem, assuming low-memory situation.";
    return false;
  }

  uint64_t fs_overall_size = buf.f_bsize * static_cast<uint64_t>(buf.f_blocks);
  // Zygote is privileged, but other things are not. Use bavail.
  uint64_t fs_free_size = buf.f_bsize * static_cast<uint64_t>(buf.f_bavail);

  // Take the overall size as an indicator for a tmpfs, which is being used for the decryption
  // environment. We do not want to fail quickening the boot image there, as it is beneficial
  // for time-to-UI.
  if (fs_overall_size > kTmpFsSentinelValue) {
    if (fs_free_size < kLowSpaceValue) {
      *error_msg = StringPrintf("Low-memory situation: only %4.2f megabytes available after image"
                                " generation, need at least %" PRIu64 ".",
                                static_cast<double>(fs_free_size) / MB,
                                kLowSpaceValue / MB);
      return false;
    }
  }
  return true;
}

ImageSpace* ImageSpace::Create(const char* image_location,
                               const InstructionSet image_isa,
                               std::string* error_msg) {
  std::string system_filename;
  bool has_system = false;
  std::string cache_filename;
  bool has_cache = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = true;
  const bool found_image = FindImageFilename(image_location, image_isa, &system_filename,
                                             &has_system, &cache_filename, &dalvik_cache_exists,
                                             &has_cache, &is_global_cache);

  if (Runtime::Current()->IsZygote()) {
    MarkZygoteStart(image_isa, Runtime::Current()->GetZygoteMaxFailedBoots());
  }

  ImageSpace* space;
  bool relocate = Runtime::Current()->ShouldRelocate();
  bool can_compile = Runtime::Current()->IsImageDex2OatEnabled();
  if (found_image) {
    const std::string* image_filename;
    bool is_system = false;
    bool relocated_version_used = false;
    if (relocate) {
      if (!dalvik_cache_exists) {
        *error_msg = StringPrintf("Requiring relocation for image '%s' at '%s' but we do not have "
                                  "any dalvik_cache to find/place it in.",
                                  image_location, system_filename.c_str());
        return nullptr;
      }
      if (has_system) {
        if (has_cache && ChecksumsMatch(system_filename.c_str(), cache_filename.c_str())) {
          // We already have a relocated version
          image_filename = &cache_filename;
          relocated_version_used = true;
        } else {
          // We cannot have a relocated version, Relocate the system one and use it.

          std::string reason;
          bool success;

          // Check whether we are allowed to relocate.
          if (!can_compile) {
            reason = "Image dex2oat disabled by -Xnoimage-dex2oat.";
            success = false;
          } else if (!ImageCreationAllowed(is_global_cache, &reason)) {
            // Whether we can write to the cache.
            success = false;
          } else {
            // Try to relocate.
            success = RelocateImage(image_location, cache_filename.c_str(), image_isa, &reason);
          }

          if (success) {
            relocated_version_used = true;
            image_filename = &cache_filename;
          } else {
            *error_msg = StringPrintf("Unable to relocate image '%s' from '%s' to '%s': %s",
                                      image_location, system_filename.c_str(),
                                      cache_filename.c_str(), reason.c_str());
            // We failed to create files, remove any possibly garbage output.
            // Since ImageCreationAllowed was true above, we are the zygote
            // and therefore the only process expected to generate these for
            // the device.
            PruneDalvikCache(image_isa);
            return nullptr;
          }
        }
      } else {
        CHECK(has_cache);
        // We can just use cache's since it should be fine. This might or might not be relocated.
        image_filename = &cache_filename;
      }
    } else {
      if (has_system && has_cache) {
        // Check they have the same cksum. If they do use the cache. Otherwise system.
        if (ChecksumsMatch(system_filename.c_str(), cache_filename.c_str())) {
          image_filename = &cache_filename;
          relocated_version_used = true;
        } else {
          image_filename = &system_filename;
          is_system = true;
        }
      } else if (has_system) {
        image_filename = &system_filename;
        is_system = true;
      } else {
        CHECK(has_cache);
        image_filename = &cache_filename;
      }
    }
    {
      // Note that we must not use the file descriptor associated with
      // ScopedFlock::GetFile to Init the image file. We want the file
      // descriptor (and the associated exclusive lock) to be released when
      // we leave Create.
      ScopedFlock image_lock;
      image_lock.Init(image_filename->c_str(), error_msg);
      VLOG(startup) << "Using image file " << image_filename->c_str() << " for image location "
                    << image_location;
      // If we are in /system we can assume the image is good. We can also
      // assume this if we are using a relocated image (i.e. image checksum
      // matches) since this is only different by the offset. We need this to
      // make sure that host tests continue to work.
      // Since we are the boot image, pass null since we load the oat file from the boot image oat
      // file name.
      space = ImageSpace::Init(image_filename->c_str(),
                               image_location,
                               !(is_system || relocated_version_used),
                               /* oat_file */nullptr,
                               error_msg);
    }
    if (space != nullptr) {
      return space;
    }

    if (relocated_version_used) {
      // Something is wrong with the relocated copy (even though checksums match). Cleanup.
      // This can happen if the .oat is corrupt, since the above only checks the .art checksums.
      // TODO: Check the oat file validity earlier.
      *error_msg = StringPrintf("Attempted to use relocated version of %s at %s generated from %s "
                                "but image failed to load: %s",
                                image_location, cache_filename.c_str(), system_filename.c_str(),
                                error_msg->c_str());
      PruneDalvikCache(image_isa);
      return nullptr;
    } else if (is_system) {
      // If the /system file exists, it should be up-to-date, don't try to generate it.
      *error_msg = StringPrintf("Failed to load /system image '%s': %s",
                                image_filename->c_str(), error_msg->c_str());
      return nullptr;
    } else {
      // Otherwise, log a warning and fall through to GenerateImage.
      LOG(WARNING) << *error_msg;
    }
  }

  if (!can_compile) {
    *error_msg = "Not attempting to compile image because -Xnoimage-dex2oat";
    return nullptr;
  } else if (!dalvik_cache_exists) {
    *error_msg = StringPrintf("No place to put generated image.");
    return nullptr;
  } else if (!ImageCreationAllowed(is_global_cache, error_msg)) {
    return nullptr;
  } else if (!GenerateImage(cache_filename, image_isa, error_msg)) {
    *error_msg = StringPrintf("Failed to generate image '%s': %s",
                              cache_filename.c_str(), error_msg->c_str());
    // We failed to create files, remove any possibly garbage output.
    // Since ImageCreationAllowed was true above, we are the zygote
    // and therefore the only process expected to generate these for
    // the device.
    PruneDalvikCache(image_isa);
    return nullptr;
  } else {
    // Check whether there is enough space left over after we have generated the image.
    if (!CheckSpace(cache_filename, error_msg)) {
      // No. Delete the generated image and try to run out of the dex files.
      PruneDalvikCache(image_isa);
      return nullptr;
    }

    // Note that we must not use the file descriptor associated with
    // ScopedFlock::GetFile to Init the image file. We want the file
    // descriptor (and the associated exclusive lock) to be released when
    // we leave Create.
    ScopedFlock image_lock;
    image_lock.Init(cache_filename.c_str(), error_msg);
    space = ImageSpace::Init(cache_filename.c_str(), image_location, true, nullptr, error_msg);
    if (space == nullptr) {
      *error_msg = StringPrintf("Failed to load generated image '%s': %s",
                                cache_filename.c_str(), error_msg->c_str());
    }
    return space;
  }
}

void ImageSpace::VerifyImageAllocations() {
  uint8_t* current = Begin() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  while (current < End()) {
    CHECK_ALIGNED(current, kObjectAlignment);
    auto* obj = reinterpret_cast<mirror::Object*>(current);
    CHECK(obj->GetClass() != nullptr) << "Image object at address " << obj << " has null class";
    CHECK(live_bitmap_->Test(obj)) << PrettyTypeOf(obj);
    if (kUseBakerOrBrooksReadBarrier) {
      obj->AssertReadBarrierPointer();
    }
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
}

class RelocationRange {
 public:
  RelocationRange() = default;
  RelocationRange(const RelocationRange&) = default;
  RelocationRange(uintptr_t source, uintptr_t dest, uintptr_t length)
      : source_(source),
        dest_(dest),
        length_(length) {}

  bool ContainsSource(uintptr_t address) const {
    return address - source_ < length_;
  }

  uintptr_t ToDest(uintptr_t address) const {
    DCHECK(ContainsSource(address));
    return address + Delta();
  }

  off_t Delta() const {
    return dest_ - source_;
  }

  uintptr_t Source() const {
    return source_;
  }

  uintptr_t Dest() const {
    return dest_;
  }

  uintptr_t Length() const {
    return length_;
  }

 private:
  const uintptr_t source_;
  const uintptr_t dest_;
  const uintptr_t length_;
};

class FixupVisitor : public ValueObject {
 public:
  FixupVisitor(const RelocationRange& boot_image,
               const RelocationRange& boot_oat,
               const RelocationRange& app_image,
               const RelocationRange& app_oat)
      : boot_image_(boot_image),
        boot_oat_(boot_oat),
        app_image_(app_image),
        app_oat_(app_oat) {}

  template <typename T>
  T* ForwardObject(T* src) const {
    const uintptr_t uint_src = reinterpret_cast<uintptr_t>(src);
    if (boot_image_.ContainsSource(uint_src)) {
      return reinterpret_cast<T*>(boot_image_.ToDest(uint_src));
    }
    if (app_image_.ContainsSource(uint_src)) {
      return reinterpret_cast<T*>(app_image_.ToDest(uint_src));
    }
    return src;
  }

  const void* ForwardCode(const void* src) const {
    const uintptr_t uint_src = reinterpret_cast<uintptr_t>(src);
    if (boot_oat_.ContainsSource(uint_src)) {
     return reinterpret_cast<const void*>(boot_oat_.ToDest(uint_src));
    }
    if (app_oat_.ContainsSource(uint_src)) {
      return reinterpret_cast<const void*>(app_oat_.ToDest(uint_src));
    }
    if (uint_src != 0) {
      LOG(FATAL) << "Unpatched code " << src;
    }
    return src;
  }

 protected:
  // Source section.
  RelocationRange boot_image_;
  RelocationRange boot_oat_;
  RelocationRange app_image_;
  RelocationRange app_oat_;
};

std::ostream& operator<<(std::ostream& os, const RelocationRange& reloc) {
  return os << "(" << reinterpret_cast<const void*>(reloc.Source()) << "-"
            << reinterpret_cast<const void*>(reloc.Source() + reloc.Length()) << ")->("
            << reinterpret_cast<const void*>(reloc.Dest()) << "-"
            << reinterpret_cast<const void*>(reloc.Dest() + reloc.Length()) << ")";
}

// Adapt to mirror::Class::FixupNativePointers
class FixupObjectAdapter : public FixupVisitor {
 public:
  template<typename... Args>
  explicit FixupObjectAdapter(Args... args) : FixupVisitor(args...) {}

  template <typename T>
  T* operator()(T* obj) const {
    return ForwardObject(obj);
  }
};

class FixupClassVisitor : public FixupVisitor {
 public:
  template<typename... Args>
  explicit FixupClassVisitor(Args... args) : FixupVisitor(args...) {}

  // The image space is contained so the GC doesn't need to know about it. Avoid requiring mutator
  // lock to prevent possible pauses.
  ALWAYS_INLINE void operator()(mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    mirror::Class* klass = obj->GetClass<kVerifyNone, kWithoutReadBarrier>();
    DCHECK(klass != nullptr) << "Null class in image";
    // No AsClass since our fields aren't quite fixed up yet.
    mirror::Class* new_klass = down_cast<mirror::Class*>(ForwardObject(klass));
    // Keep clean if possible.
    if (klass != new_klass) {
      obj->SetClass<kVerifyNone>(new_klass);
    }
    VLOG(image) << PrettyClass(new_klass);
  }
};

class FixupObjectVisitor : public FixupVisitor {
 public:
  template<typename... Args>
  explicit FixupObjectVisitor(Args... args) : FixupVisitor(args...) {}

  // Fixup separately since we need to fixup method entrypoints.
  ALWAYS_INLINE void VisitRootIfNonNull(
      mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

  ALWAYS_INLINE void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED)
      const {}

  ALWAYS_INLINE void operator()(mirror::Object* obj,
                                MemberOffset offset,
                                bool is_static ATTRIBUTE_UNUSED) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (offset.Uint32Value() != mirror::Object::ClassOffset().Uint32Value()) {
      mirror::Object* ref = obj->GetFieldObject<mirror::Object, kVerifyNone>(offset);
      // Use SetFieldObjectWithoutWriteBarrier to avoid card marking since we are writing to the
      // image.
      obj->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(offset, ForwardObject(ref));
    }
  }

  // java.lang.ref.Reference visitor.
  void operator()(mirror::Class* klass ATTRIBUTE_UNUSED, mirror::Reference* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) REQUIRES(Locks::heap_bitmap_lock_) {
    mirror::Object* obj = ref->GetReferent();
    ref->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(
        mirror::Reference::ReferentOffset(),
        ForwardObject(obj));
  }

  void operator()(mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    obj->VisitReferences<kVerifyNone, /*visit native roots*/false>(*this, *this);
    // We want to use our own class loader and not the one in the image.
    if (obj->IsClass()) {
      mirror::Class* klass = obj->AsClass();
      // TODO: Clean this up.
      FixupObjectAdapter visitor(boot_image_, boot_oat_, app_image_, app_oat_);
      klass->FixupNativePointers(klass, sizeof(void*), visitor);
      // Deal with the arrays.
      mirror::PointerArray* vtable = klass->GetVTable();
      if (vtable != nullptr) {
        vtable->Fixup(vtable, sizeof(void*), visitor);
      }
      mirror::IfTable* iftable = klass->GetIfTable();
      if (iftable != nullptr) {
        for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
          if (iftable->GetMethodArrayCount(i) > 0) {
            mirror::PointerArray* methods = iftable->GetMethodArray(i);
            DCHECK(methods != nullptr);
            methods->Fixup(methods, sizeof(void*), visitor);
          }
        }
      }
    } else if (obj->IsClassLoader()) {
      obj->AsClassLoader()->SetClassTable(nullptr);
    }
  }
};

class FixupArtMethodVisitor : public FixupVisitor, public ArtMethodVisitor {
 public:
  template<typename... Args>
  explicit FixupArtMethodVisitor(Args... args) : FixupVisitor(args...) {}

  virtual void Visit(ArtMethod* method) NO_THREAD_SAFETY_ANALYSIS {
    method->SetDeclaringClass(ForwardObject(method->GetDeclaringClassNoBarrier())->AsClass());
    method->SetDexCacheResolvedMethods(
        ForwardObject(method->GetDexCacheResolvedMethods(sizeof(void*))),
        sizeof(void*));
    method->SetDexCacheResolvedTypes(
        ForwardObject(method->GetDexCacheResolvedTypes(sizeof(void*))),
        sizeof(void*));
    if (method->IsNative()) {
      method->SetEntryPointFromJni(ForwardCode(method->GetEntryPointFromJni()));
    } else {
      DCHECK(method->GetEntryPointFromJni() == nullptr);
    }
    method->SetEntryPointFromQuickCompiledCode(
        ForwardCode(method->GetEntryPointFromQuickCompiledCode()));
  }
};

class FixupArtFieldVisitor : public FixupVisitor, public ArtFieldVisitor {
 public:
  template<typename... Args>
  explicit FixupArtFieldVisitor(Args... args) : FixupVisitor(args...) {}

  virtual void Visit(ArtField* field) NO_THREAD_SAFETY_ANALYSIS {
    field->SetDeclaringClass(ForwardObject(
        field->DeclaringClassRoot().Read<kWithoutReadBarrier>())->AsClass());
  }
};

static bool RelocateInPlace(ImageHeader& image_header,
                            uint8_t* target_base,
                            accounting::ContinuousSpaceBitmap* bitmap,
                            const OatFile* app_oat_file,
                            std::string* error_msg) {
  DCHECK(error_msg != nullptr);
  if (!image_header.IsPic()) {
    if (image_header.GetImageBegin() == target_base) {
      return true;
    }
    // TODO: Add more detail.
    *error_msg = "Can not relocate non-pic image";
    return false;
  }
  // Set up sections.
  space::ImageSpace* boot_image_space = Runtime::Current()->GetHeap()->GetBootImageSpace();
  CHECK(boot_image_space != nullptr);
  const ImageHeader& boot_image_header = boot_image_space->GetImageHeader();
  const OatFile* boot_oat_file = boot_image_space->GetOatFile();
  const size_t boot_oat_data_size = boot_image_header.GetOatDataEnd() -
      boot_image_header.GetOatDataBegin();
  if (boot_oat_file->Size() != boot_oat_data_size) {
    *error_msg = StringPrintf("Boot oat file size %" PRIu64 " does not match expected size %"
                                  PRIu64,
                              static_cast<uint64_t>(boot_oat_file->Size()),
                              static_cast<uint64_t>(boot_oat_data_size));
    return false;
  }
  CHECK_EQ(image_header.GetBootImageSize(),
           boot_image_space->GetImageHeader().GetImageSize());
  RelocationRange boot_image(image_header.GetBootImageBegin(),
                             reinterpret_cast<uintptr_t>(boot_image_space->Begin()),
                             image_header.GetBootImageSize());
  RelocationRange boot_oat(image_header.GetBootOatBegin(),
                           reinterpret_cast<uintptr_t>(boot_oat_file->Begin()),
                           boot_oat_file->Size());
  RelocationRange app_image(reinterpret_cast<uintptr_t>(image_header.GetImageBegin()),
                            reinterpret_cast<uintptr_t>(target_base),
                            image_header.GetImageSize());
  // Use the oat data section since this is where the OatFile::Begin is.
  RelocationRange app_oat(reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin()),
                          // Not necessarily in low 4GB.
                          reinterpret_cast<uintptr_t>(app_oat_file->Begin()),
                          image_header.GetOatDataEnd() - image_header.GetOatDataBegin());
  VLOG(image) << "App image " << app_image;
  VLOG(image) << "App oat " << app_oat;
  VLOG(image) << "Boot image " << boot_image;
  VLOG(image) << "Boot oat " << boot_oat;
  if (boot_image.Delta() == 0 &&
      boot_oat.Delta() == 0 &&
      app_image.Delta() == 0 &&
      app_oat.Delta() == 0) {
    // Nothing to fix up.
    return true;
  }
  // Need to update the image to be at the target base.
  const ImageSection& objects_section = image_header.GetImageSection(ImageHeader::kSectionObjects);
  uintptr_t objects_begin = reinterpret_cast<uintptr_t>(target_base + objects_section.Offset());
  uintptr_t objects_end = reinterpret_cast<uintptr_t>(target_base + objects_section.End());
  // Two pass approach, fix up all classes first, then fix up non class-objects.
  FixupClassVisitor fixup_class_visitor(boot_image, boot_oat, app_image, app_oat);
  bitmap->VisitMarkedRange(objects_begin, objects_end, fixup_class_visitor);
  FixupObjectVisitor fixup_object_visitor(boot_image, boot_oat, app_image, app_oat);
  bitmap->VisitMarkedRange(objects_begin, objects_end, fixup_object_visitor);
  FixupObjectAdapter fixup_adapter(boot_image, boot_oat, app_image, app_oat);
  {
    ScopedObjectAccess soa(Thread::Current());
    // Fixup image roots.
    CHECK(app_image.ContainsSource(reinterpret_cast<uintptr_t>(image_header.GetImageRoots())));
    image_header.RelocateImageObjects(app_image.Delta());
    CHECK_EQ(image_header.GetImageBegin(), target_base);
    // Fix up dex cache DexFile pointers.
    auto* dex_caches = image_header.GetImageRoot(ImageHeader::kDexCaches)->
        AsObjectArray<mirror::DexCache>();
    for (int32_t i = 0, count = dex_caches->GetLength(); i < count; ++i) {
      mirror::DexCache* dex_cache = dex_caches->Get(i);
      // Fix up dex cache pointers.
      GcRoot<mirror::String>* strings = dex_cache->GetStrings();
      if (strings != nullptr) {
        GcRoot<mirror::String>* new_strings = fixup_adapter.ForwardObject(strings);
        if (strings != new_strings) {
          dex_cache->SetFieldPtr64<false>(mirror::DexCache::StringsOffset(), new_strings);
        }
        mirror::DexCache::FixupGcRootArray(new_strings,
                                           new_strings,
                                           dex_cache->NumStrings(),
                                           fixup_adapter);
      }
      GcRoot<mirror::Class>* types = dex_cache->GetResolvedTypes();
      if (types != nullptr) {
        GcRoot<mirror::Class>* new_types = fixup_adapter.ForwardObject(types);
        if (types != new_types) {
          dex_cache->SetFieldPtr64<false>(mirror::DexCache::ResolvedTypesOffset(), new_types);
        }
        mirror::DexCache::FixupGcRootArray(new_types,
                                           new_types,
                                           dex_cache->NumResolvedTypes(),
                                           fixup_adapter);
      }
      ArtMethod** methods = dex_cache->GetResolvedMethods();
      if (methods != nullptr) {
        ArtMethod** new_methods = fixup_adapter.ForwardObject(methods);
        if (methods != new_methods) {
          dex_cache->SetFieldPtr64<false>(mirror::DexCache::ResolvedMethodsOffset(), new_methods);
        }
        for (size_t j = 0, num = dex_cache->NumResolvedMethods(); j != num; ++j) {
          ArtMethod* orig = mirror::DexCache::GetElementPtrSize(new_methods, j, sizeof(void*));
          ArtMethod* copy = fixup_adapter.ForwardObject(orig);
          if (orig != copy) {
            mirror::DexCache::SetElementPtrSize(new_methods, j, copy, sizeof(void*));
          }
        }
      }
      ArtField** fields = dex_cache->GetResolvedFields();
      if (fields != nullptr) {
        ArtField** new_fields = fixup_adapter.ForwardObject(fields);
        if (fields != new_fields) {
          dex_cache->SetFieldPtr64<false>(mirror::DexCache::ResolvedFieldsOffset(), new_fields);
        }
        for (size_t j = 0, num = dex_cache->NumResolvedFields(); j != num; ++j) {
          ArtField* orig = mirror::DexCache::GetElementPtrSize(new_fields, j, sizeof(void*));
          ArtField* copy = fixup_adapter.ForwardObject(orig);
          if (orig != copy) {
            mirror::DexCache::SetElementPtrSize(new_fields, j, copy, sizeof(void*));
          }
        }
      }
    }
  }
  // Fixup methods and fields.
  FixupArtMethodVisitor method_visitor(boot_image, boot_oat, app_image, app_oat);
  image_header.GetImageSection(ImageHeader::kSectionArtMethods).VisitPackedArtMethods(
      &method_visitor,
      target_base,
      sizeof(void*));
  FixupArtFieldVisitor field_visitor(boot_image, boot_oat, app_image, app_oat);
  image_header.GetImageSection(ImageHeader::kSectionArtFields).VisitPackedArtFields(
      &field_visitor,
      target_base);
  // In the app image case, the image methods are actually in the boot image.
  image_header.RelocateImageMethods(boot_image.Delta());
  return true;
}

ImageSpace* ImageSpace::Init(const char* image_filename,
                             const char* image_location,
                             bool validate_oat_file,
                             const OatFile* oat_file,
                             std::string* error_msg) {
  CHECK(image_filename != nullptr);
  CHECK(image_location != nullptr);

  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    LOG(INFO) << "ImageSpace::Init entering image_filename=" << image_filename;
  }

  std::unique_ptr<File> file(OS::OpenFileForReading(image_filename));
  if (file.get() == nullptr) {
    *error_msg = StringPrintf("Failed to open '%s'", image_filename);
    return nullptr;
  }
  ImageHeader temp_image_header;
  ImageHeader* image_header = &temp_image_header;
  bool success = file->ReadFully(image_header, sizeof(*image_header));
  if (!success || !image_header->IsValid()) {
    *error_msg = StringPrintf("Invalid image header in '%s'", image_filename);
    return nullptr;
  }
  // Check that the file is large enough.
  uint64_t image_file_size = static_cast<uint64_t>(file->GetLength());
  if (image_header->GetImageSize() > image_file_size) {
    *error_msg = StringPrintf("Image file too small for image heap: %" PRIu64 " vs. %zu.",
                              image_file_size,
                              image_header->GetImageSize());
    return nullptr;
  }

  if (kIsDebugBuild) {
    LOG(INFO) << "Dumping image sections";
    for (size_t i = 0; i < ImageHeader::kSectionCount; ++i) {
      const auto section_idx = static_cast<ImageHeader::ImageSections>(i);
      auto& section = image_header->GetImageSection(section_idx);
      LOG(INFO) << section_idx << " start="
                << reinterpret_cast<void*>(image_header->GetImageBegin() + section.Offset()) << " "
                << section;
    }
  }

  const auto& bitmap_section = image_header->GetImageSection(ImageHeader::kSectionImageBitmap);
  auto end_of_bitmap = static_cast<size_t>(bitmap_section.End());
  if (end_of_bitmap != image_file_size) {
    *error_msg = StringPrintf(
        "Image file size does not equal end of bitmap: size=%" PRIu64 " vs. %zu.", image_file_size,
        end_of_bitmap);
    return nullptr;
  }

  std::vector<uint8_t*> addresses(1, image_header->GetImageBegin());
  if (image_header->IsPic()) {
    // Can also map at an arbitrary address since we can relocate.
    addresses.push_back(nullptr);
  }

  // Note: The image header is part of the image due to mmap page alignment required of offset.
  std::unique_ptr<MemMap> map;
  std::string temp_error_msg;
  for (uint8_t* address : addresses) {
    map.reset(MemMap::MapFileAtAddress(address,
                                       image_header->GetImageSize(),
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE,
                                       file->Fd(),
                                       0,
                                       /*low_4gb*/true,
                                       /*reuse*/false,
                                       image_filename,
                                       &temp_error_msg));
    if (map != nullptr) {
      break;
    }
  }
  if (map == nullptr) {
    DCHECK(!temp_error_msg.empty());
    *error_msg = temp_error_msg;
    return nullptr;
  }
  // DCHECK_EQ(0, memcmp(image_header, map->Begin(), sizeof(ImageHeader)));

  std::unique_ptr<MemMap> image_map(MemMap::MapFileAtAddress(nullptr,
                                                             bitmap_section.Size(),
                                                             PROT_READ,
                                                             MAP_PRIVATE,
                                                             file->Fd(),
                                                             bitmap_section.Offset(),
                                                             /*low_4gb*/false,
                                                             /*reuse*/false,
                                                             image_filename,
                                                             error_msg));
  if (image_map == nullptr) {
    *error_msg = StringPrintf("Failed to map image bitmap: %s", error_msg->c_str());
    return nullptr;
  }
  // Loaded the map, use the image header from the file now in case we patch it with
  // RelocateInPlace.
  image_header = reinterpret_cast<ImageHeader*>(map->Begin());
  const uint32_t bitmap_index = bitmap_index_.FetchAndAddSequentiallyConsistent(1);
  std::string bitmap_name(StringPrintf("imagespace %s live-bitmap %u",
                                       image_filename,
                                       bitmap_index));
  std::unique_ptr<accounting::ContinuousSpaceBitmap> bitmap(
      accounting::ContinuousSpaceBitmap::CreateFromMemMap(
          bitmap_name,
          image_map.release(),
          reinterpret_cast<uint8_t*>(map->Begin()),
          accounting::ContinuousSpaceBitmap::ComputeHeapSize(bitmap_section.Size())));
  if (bitmap == nullptr) {
    *error_msg = StringPrintf("Could not create bitmap '%s'", bitmap_name.c_str());
    return nullptr;
  }
  if (!RelocateInPlace(*image_header,
                       map->Begin(),
                       bitmap.get(),
                       oat_file,
                       error_msg)) {
    return nullptr;
  }
  // We only want the mirror object, not the ArtFields and ArtMethods.
  uint8_t* const image_end =
      map->Begin() + image_header->GetImageSection(ImageHeader::kSectionObjects).End();
  std::unique_ptr<ImageSpace> space(new ImageSpace(image_filename,
                                                   image_location,
                                                   map.release(),
                                                   bitmap.release(),
                                                   image_end));

  // VerifyImageAllocations() will be called later in Runtime::Init()
  // as some class roots like ArtMethod::java_lang_reflect_ArtMethod_
  // and ArtField::java_lang_reflect_ArtField_, which are used from
  // Object::SizeOf() which VerifyImageAllocations() calls, are not
  // set yet at this point.
  if (oat_file == nullptr) {
    space->oat_file_.reset(space->OpenOatFile(image_filename, error_msg));
    if (space->oat_file_ == nullptr) {
      DCHECK(!error_msg->empty());
      return nullptr;
    }
    space->oat_file_non_owned_ = space->oat_file_.get();
  } else {
    space->oat_file_non_owned_ = oat_file;
  }

  if (validate_oat_file && !space->ValidateOatFile(error_msg)) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }

  Runtime* runtime = Runtime::Current();

  if (oat_file != nullptr) {
    LOG(ERROR) << "IMT unimplemented " << runtime->GetImtUnimplementedMethod();
    LOG(ERROR) << "IMT conflict " << runtime->GetImtConflictMethod();
    CHECK_EQ(runtime->GetInstructionSet(),
             space->oat_file_non_owned_->GetOatHeader().GetInstructionSet());
    CHECK_EQ(runtime->GetResolutionMethod(),
             image_header->GetImageMethod(ImageHeader::kResolutionMethod));
    CHECK_EQ(runtime->GetImtConflictMethod(),
             image_header->GetImageMethod(ImageHeader::kImtConflictMethod));
    CHECK_EQ(runtime->GetImtUnimplementedMethod(),
             image_header->GetImageMethod(ImageHeader::kImtUnimplementedMethod));
    CHECK_EQ(runtime->GetCalleeSaveMethod(Runtime::kSaveAll),
             image_header->GetImageMethod(ImageHeader::kCalleeSaveMethod));
    CHECK_EQ(runtime->GetCalleeSaveMethod(Runtime::kRefsOnly),
             image_header->GetImageMethod(ImageHeader::kRefsOnlySaveMethod));
    CHECK_EQ(runtime->GetCalleeSaveMethod(Runtime::kRefsAndArgs),
             image_header->GetImageMethod(ImageHeader::kRefsAndArgsSaveMethod));
  } else {
    runtime->SetInstructionSet(space->oat_file_non_owned_->GetOatHeader().GetInstructionSet());
    runtime->SetResolutionMethod(image_header->GetImageMethod(ImageHeader::kResolutionMethod));
    runtime->SetImtConflictMethod(image_header->GetImageMethod(ImageHeader::kImtConflictMethod));
    runtime->SetImtUnimplementedMethod(
        image_header->GetImageMethod(ImageHeader::kImtUnimplementedMethod));
    runtime->SetCalleeSaveMethod(
        image_header->GetImageMethod(ImageHeader::kCalleeSaveMethod), Runtime::kSaveAll);
    runtime->SetCalleeSaveMethod(
        image_header->GetImageMethod(ImageHeader::kRefsOnlySaveMethod), Runtime::kRefsOnly);
    runtime->SetCalleeSaveMethod(
        image_header->GetImageMethod(ImageHeader::kRefsAndArgsSaveMethod), Runtime::kRefsAndArgs);
  }

  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "ImageSpace::Init exiting (" << PrettyDuration(NanoTime() - start_time)
             << ") " << *space.get();
  }
  return space.release();
}

OatFile* ImageSpace::OpenOatFile(const char* image_path, std::string* error_msg) const {
  const ImageHeader& image_header = GetImageHeader();
  std::string oat_filename = ImageHeader::GetOatLocationFromImageLocation(image_path);

  CHECK(image_header.GetOatDataBegin() != nullptr);

  OatFile* oat_file = OatFile::Open(oat_filename,
                                    oat_filename,
                                    image_header.GetOatDataBegin(),
                                    image_header.GetOatFileBegin(),
                                    !Runtime::Current()->IsAotCompiler(),
                                    nullptr,
                                    error_msg);
  if (oat_file == nullptr) {
    *error_msg = StringPrintf("Failed to open oat file '%s' referenced from image %s: %s",
                              oat_filename.c_str(), GetName(), error_msg->c_str());
    return nullptr;
  }
  uint32_t oat_checksum = oat_file->GetOatHeader().GetChecksum();
  uint32_t image_oat_checksum = image_header.GetOatChecksum();
  if (oat_checksum != image_oat_checksum) {
    *error_msg = StringPrintf("Failed to match oat file checksum 0x%x to expected oat checksum 0x%x"
                              " in image %s", oat_checksum, image_oat_checksum, GetName());
    return nullptr;
  }
  int32_t image_patch_delta = image_header.GetPatchDelta();
  int32_t oat_patch_delta = oat_file->GetOatHeader().GetImagePatchDelta();
  if (oat_patch_delta != image_patch_delta && !image_header.CompilePic()) {
    // We should have already relocated by this point. Bail out.
    *error_msg = StringPrintf("Failed to match oat file patch delta %d to expected patch delta %d "
                              "in image %s", oat_patch_delta, image_patch_delta, GetName());
    return nullptr;
  }

  return oat_file;
}

bool ImageSpace::ValidateOatFile(std::string* error_msg) const {
  CHECK(oat_file_.get() != nullptr);
  for (const OatFile::OatDexFile* oat_dex_file : oat_file_->GetOatDexFiles()) {
    const std::string& dex_file_location = oat_dex_file->GetDexFileLocation();
    uint32_t dex_file_location_checksum;
    if (!DexFile::GetChecksum(dex_file_location.c_str(), &dex_file_location_checksum, error_msg)) {
      *error_msg = StringPrintf("Failed to get checksum of dex file '%s' referenced by image %s: "
                                "%s", dex_file_location.c_str(), GetName(), error_msg->c_str());
      return false;
    }
    if (dex_file_location_checksum != oat_dex_file->GetDexFileLocationChecksum()) {
      *error_msg = StringPrintf("ValidateOatFile found checksum mismatch between oat file '%s' and "
                                "dex file '%s' (0x%x != 0x%x)",
                                oat_file_->GetLocation().c_str(), dex_file_location.c_str(),
                                oat_dex_file->GetDexFileLocationChecksum(),
                                dex_file_location_checksum);
      return false;
    }
  }
  return true;
}

const OatFile* ImageSpace::GetOatFile() const {
  return oat_file_non_owned_;
}

std::unique_ptr<const OatFile> ImageSpace::ReleaseOatFile() {
  CHECK(oat_file_ != nullptr);
  return std::move(oat_file_);
}

void ImageSpace::Dump(std::ostream& os) const {
  os << GetType()
      << " begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size())
      << ",name=\"" << GetName() << "\"]";
}

ImageSpace* ImageSpace::CreateFromAppImage(const char* image,
                                           const OatFile* oat_file,
                                           std::string* error_msg) {
  return gc::space::ImageSpace::Init(image,
                                     image,
                                     /*validate_oat_file*/false,
                                     oat_file,
                                     error_msg);
}

}  // namespace space
}  // namespace gc
}  // namespace art
