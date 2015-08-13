/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "os.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstddef>
#include <memory>

#include "base/logging.h"
#include "base/unix_file/fd_file.h"

namespace art {

File* OS::OpenFileForReading(const char* name) {
  return OpenFileWithFlags(name, O_RDONLY);
}

File* OS::OpenFileReadWrite(const char* name) {
  return OpenFileWithFlags(name, O_RDWR);
}

static File* CreateEmptyFile(const char* name, int extra_flags) {
  // In case the file exists, unlink it so we get a new file. This is necessary as the previous
  // file may be in use and must not be changed.
  unlink(name);

  return OS::OpenFileWithFlags(name, O_CREAT | extra_flags);
}

File* OS::CreateEmptyFile(const char* name) {
  return art::CreateEmptyFile(name, O_RDWR | O_TRUNC);
}

File* OS::CreateEmptyFileWriteOnly(const char* name) {
  return art::CreateEmptyFile(name, O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC);
}

File* OS::OpenFileWithFlags(const char* name, int flags) {
  CHECK(name != nullptr);
  bool read_only = (flags == O_RDONLY);
  std::unique_ptr<File> file(new File(name, flags, 0666, !read_only));
  if (!file->IsOpened()) {
    return nullptr;
  }
  return file.release();
}

bool OS::FileExists(const char* name) {
  struct stat st;
  if (stat(name, &st) == 0) {
    return S_ISREG(st.st_mode);  // TODO: Deal with symlinks?
  } else {
    return false;
  }
}

bool OS::DirectoryExists(const char* name) {
  struct stat st;
  if (stat(name, &st) == 0) {
    return S_ISDIR(st.st_mode);  // TODO: Deal with symlinks?
  } else {
    return false;
  }
}

}  // namespace art
