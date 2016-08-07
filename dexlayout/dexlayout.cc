/*
 * Copyright (C) 2016 The Android Open Source Project
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
 *
 * Implementation file of the dexlayout utility.
 *
 * This is a tool to read dex files into an internal representation,
 * reorganize the representation, and emit dex files with a better
 * file layout.
 */

#include "dexlayout.h"

#include <inttypes.h>
#include <stdio.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "dex_ir.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "utils.h"

namespace art {

/*
 * Options parsed in main driver.
 */
struct Options gOptions;

/*
 * Output file. Defaults to stdout.
 */
FILE* gOutFile = stdout;

/*
 * Data types that match the definitions in the VM specification.
 */
typedef uint8_t  u1;
typedef uint16_t u2;
typedef uint32_t u4;
typedef uint64_t u8;
typedef int8_t   s1;
typedef int16_t  s2;
typedef int32_t  s4;
typedef int64_t  s8;

/*
 * Basic information about a field or a method.
 */
struct FieldMethodInfo {
  const char* classDescriptor;
  const char* name;
  const char* signature;
};

/*
 * Flags for use with createAccessFlagStr().
 */
enum AccessFor {
  kAccessForClass = 0, kAccessForMethod = 1, kAccessForField = 2, kAccessForMAX
};
const int kNumFlags = 18;

/*
 * Gets 2 little-endian bytes.
 */
static inline u2 get2LE(unsigned char const* pSrc) {
  return pSrc[0] | (pSrc[1] << 8);
}

/*
 * Converts a single-character primitive type into human-readable form.
 */
static const char* primitiveTypeLabel(char typeChar) {
  switch (typeChar) {
    case 'B': return "byte";
    case 'C': return "char";
    case 'D': return "double";
    case 'F': return "float";
    case 'I': return "int";
    case 'J': return "long";
    case 'S': return "short";
    case 'V': return "void";
    case 'Z': return "boolean";
    default:  return "UNKNOWN";
  }  // switch
}

/*
 * Converts a type descriptor to human-readable "dotted" form.  For
 * example, "Ljava/lang/String;" becomes "java.lang.String", and
 * "[I" becomes "int[]".  Also converts '$' to '.', which means this
 * form can't be converted back to a descriptor.
 */
static std::unique_ptr<char[]> descriptorToDot(const char* str) {
  int targetLen = strlen(str);
  int offset = 0;

  // Strip leading [s; will be added to end.
  while (targetLen > 1 && str[offset] == '[') {
    offset++;
    targetLen--;
  }  // while

  const int arrayDepth = offset;

  if (targetLen == 1) {
    // Primitive type.
    str = primitiveTypeLabel(str[offset]);
    offset = 0;
    targetLen = strlen(str);
  } else {
    // Account for leading 'L' and trailing ';'.
    if (targetLen >= 2 && str[offset] == 'L' &&
        str[offset + targetLen - 1] == ';') {
      targetLen -= 2;
      offset++;
    }
  }

  // Copy class name over.
  std::unique_ptr<char[]> newStr(new char[targetLen + arrayDepth * 2 + 1]);
  int i = 0;
  for (; i < targetLen; i++) {
    const char ch = str[offset + i];
    newStr[i] = (ch == '/' || ch == '$') ? '.' : ch;
  }  // for

  // Add the appropriate number of brackets for arrays.
  for (int j = 0; j < arrayDepth; j++) {
    newStr[i++] = '[';
    newStr[i++] = ']';
  }  // for

  newStr[i] = '\0';
  return newStr;
}

/*
 * Converts the class name portion of a type descriptor to human-readable
 * "dotted" form. For example, "Ljava/lang/String;" becomes "String".
 */
static std::unique_ptr<char[]> descriptorClassToDot(const char* str) {
  // Reduce to just the class name prefix.
  const char* lastSlash = strrchr(str, '/');
  if (lastSlash == nullptr) {
    lastSlash = str + 1;  // start past 'L'
  } else {
    lastSlash++;          // start past '/'
  }

  // Copy class name over, trimming trailing ';'.
  const int targetLen = strlen(lastSlash);
  std::unique_ptr<char[]> newStr(new char[targetLen]);
  for (int i = 0; i < targetLen - 1; i++) {
    const char ch = lastSlash[i];
    newStr[i] = ch == '$' ? '.' : ch;
  }  // for
  newStr[targetLen - 1] = '\0';
  return newStr;
}

/*
 * Returns string representing the boolean value.
 */
static const char* strBool(bool val) {
  return val ? "true" : "false";
}

/*
 * Returns a quoted string representing the boolean value.
 */
static const char* quotedBool(bool val) {
  return val ? "\"true\"" : "\"false\"";
}

/*
 * Returns a quoted string representing the access flags.
 */
static const char* quotedVisibility(u4 accessFlags) {
  if (accessFlags & kAccPublic) {
    return "\"public\"";
  } else if (accessFlags & kAccProtected) {
    return "\"protected\"";
  } else if (accessFlags & kAccPrivate) {
    return "\"private\"";
  } else {
    return "\"package\"";
  }
}

/*
 * Counts the number of '1' bits in a word.
 */
static int countOnes(u4 val) {
  val = val - ((val >> 1) & 0x55555555);
  val = (val & 0x33333333) + ((val >> 2) & 0x33333333);
  return (((val + (val >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

/*
 * Creates a new string with human-readable access flags.
 *
 * In the base language the access_flags fields are type u2; in Dalvik
 * they're u4.
 */
static char* createAccessFlagStr(u4 flags, AccessFor forWhat) {
  static const char* kAccessStrings[kAccessForMAX][kNumFlags] = {
    {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "?",                     /* 0x00020 */
      "?",                     /* 0x00040 */
      "?",                     /* 0x00080 */
      "?",                     /* 0x00100 */
      "INTERFACE",             /* 0x00200 */
      "ABSTRACT",              /* 0x00400 */
      "?",                     /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "ANNOTATION",            /* 0x02000 */
      "ENUM",                  /* 0x04000 */
      "?",                     /* 0x08000 */
      "VERIFIED",              /* 0x10000 */
      "OPTIMIZED",             /* 0x20000 */
    }, {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "SYNCHRONIZED",          /* 0x00020 */
      "BRIDGE",                /* 0x00040 */
      "VARARGS",               /* 0x00080 */
      "NATIVE",                /* 0x00100 */
      "?",                     /* 0x00200 */
      "ABSTRACT",              /* 0x00400 */
      "STRICT",                /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "?",                     /* 0x02000 */
      "?",                     /* 0x04000 */
      "MIRANDA",               /* 0x08000 */
      "CONSTRUCTOR",           /* 0x10000 */
      "DECLARED_SYNCHRONIZED", /* 0x20000 */
    }, {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "?",                     /* 0x00020 */
      "VOLATILE",              /* 0x00040 */
      "TRANSIENT",             /* 0x00080 */
      "?",                     /* 0x00100 */
      "?",                     /* 0x00200 */
      "?",                     /* 0x00400 */
      "?",                     /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "?",                     /* 0x02000 */
      "ENUM",                  /* 0x04000 */
      "?",                     /* 0x08000 */
      "?",                     /* 0x10000 */
      "?",                     /* 0x20000 */
    },
  };

  // Allocate enough storage to hold the expected number of strings,
  // plus a space between each.  We over-allocate, using the longest
  // string above as the base metric.
  const int kLongest = 21;  // The strlen of longest string above.
  const int count = countOnes(flags);
  char* str;
  char* cp;
  cp = str = reinterpret_cast<char*>(malloc(count * (kLongest + 1) + 1));

  for (int i = 0; i < kNumFlags; i++) {
    if (flags & 0x01) {
      const char* accessStr = kAccessStrings[forWhat][i];
      const int len = strlen(accessStr);
      if (cp != str) {
        *cp++ = ' ';
      }
      memcpy(cp, accessStr, len);
      cp += len;
    }
    flags >>= 1;
  }  // for

  *cp = '\0';
  return str;
}

/*
 * Copies character data from "data" to "out", converting non-ASCII values
 * to fprintf format chars or an ASCII filler ('.' or '?').
 *
 * The output buffer must be able to hold (2*len)+1 bytes.  The result is
 * NULL-terminated.
 */
static void asciify(char* out, const unsigned char* data, size_t len) {
  while (len--) {
    if (*data < 0x20) {
      // Could do more here, but we don't need them yet.
      switch (*data) {
        case '\0':
          *out++ = '\\';
          *out++ = '0';
          break;
        case '\n':
          *out++ = '\\';
          *out++ = 'n';
          break;
        default:
          *out++ = '.';
          break;
      }  // switch
    } else if (*data >= 0x80) {
      *out++ = '?';
    } else {
      *out++ = *data;
    }
    data++;
  }  // while
  *out = '\0';
}

/*
 * Dumps a string value with some escape characters.
 */
static void dumpEscapedString(const char* p) {
  fputs("\"", gOutFile);
  for (; *p; p++) {
    switch (*p) {
      case '\\':
        fputs("\\\\", gOutFile);
        break;
      case '\"':
        fputs("\\\"", gOutFile);
        break;
      case '\t':
        fputs("\\t", gOutFile);
        break;
      case '\n':
        fputs("\\n", gOutFile);
        break;
      case '\r':
        fputs("\\r", gOutFile);
        break;
      default:
        putc(*p, gOutFile);
    }  // switch
  }  // for
  fputs("\"", gOutFile);
}

/*
 * Dumps a string as an XML attribute value.
 */
static void dumpXmlAttribute(const char* p) {
  for (; *p; p++) {
    switch (*p) {
      case '&':
        fputs("&amp;", gOutFile);
        break;
      case '<':
        fputs("&lt;", gOutFile);
        break;
      case '>':
        fputs("&gt;", gOutFile);
        break;
      case '"':
        fputs("&quot;", gOutFile);
        break;
      case '\t':
        fputs("&#x9;", gOutFile);
        break;
      case '\n':
        fputs("&#xA;", gOutFile);
        break;
      case '\r':
        fputs("&#xD;", gOutFile);
        break;
      default:
        putc(*p, gOutFile);
    }  // switch
  }  // for
}

/*
 * Dumps encoded value.
 */
static void dumpEncodedValue(const dex_ir::ArrayItem* data) {
  switch (data->type()) {
    case DexFile::kDexAnnotationByte:
      fprintf(gOutFile, "%" PRId8, data->byte_val());
      break;
    case DexFile::kDexAnnotationShort:
      fprintf(gOutFile, "%" PRId16, data->short_val());
      break;
    case DexFile::kDexAnnotationChar:
      fprintf(gOutFile, "%" PRIu16, data->char_val());
      break;
    case DexFile::kDexAnnotationInt:
      fprintf(gOutFile, "%" PRId32, data->int_val());
      break;
    case DexFile::kDexAnnotationLong:
      fprintf(gOutFile, "%" PRId64, data->long_val());
      break;
    case DexFile::kDexAnnotationFloat: {
      fprintf(gOutFile, "%g", data->float_val());
      break;
    }
    case DexFile::kDexAnnotationDouble: {
      fprintf(gOutFile, "%g", data->double_val());
      break;
    }
    case DexFile::kDexAnnotationString: {
      dex_ir::StringId* string_id = data->string_val();
      if (gOptions.outputFormat == OUTPUT_PLAIN) {
        dumpEscapedString(string_id->data());
      } else {
        dumpXmlAttribute(string_id->data());
      }
      break;
    }
    case DexFile::kDexAnnotationType: {
      dex_ir::StringId* string_id = data->string_val();
      fputs(string_id->data(), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationField:
    case DexFile::kDexAnnotationEnum: {
      dex_ir::FieldId* field_id = data->field_val();
      fputs(field_id->name()->data(), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationMethod: {
      dex_ir::MethodId* method_id = data->method_val();
      fputs(method_id->name()->data(), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationArray: {
      fputc('{', gOutFile);
      // Display all elements.
      for (auto* array : *data->annotation_array_val()) {
        fputc(' ', gOutFile);
        dumpEncodedValue(array);
      }
      fputs(" }", gOutFile);
      break;
    }
    case DexFile::kDexAnnotationAnnotation: {
      fputs(data->annotation_annotation_string()->data(), gOutFile);
      // Display all name=value pairs.
      for (auto* subannotation : *data->annotation_annotation_nvp_array()) {
        fputc(' ', gOutFile);
        fputs(subannotation->name()->data(), gOutFile);
        fputc('=', gOutFile);
        dumpEncodedValue(subannotation->value());
      }
      break;
    }
    case DexFile::kDexAnnotationNull:
      fputs("null", gOutFile);
      break;
    case DexFile::kDexAnnotationBoolean:
      fputs(strBool(data->bool_val()), gOutFile);
      break;
    default:
      fputs("????", gOutFile);
      break;
  }  // switch
}

/*
 * Dumps the file header.
 */
static void dumpFileHeader(const dex_ir::Header* header) {
  char sanitized[8 * 2 + 1];
  fprintf(gOutFile, "DEX file header:\n");
  asciify(sanitized, header->magic(), 8);
  fprintf(gOutFile, "magic               : '%s'\n", sanitized);
  fprintf(gOutFile, "checksum            : %08x\n", header->checksum());
  fprintf(gOutFile, "signature           : %02x%02x...%02x%02x\n",
          header->signature()[0], header->signature()[1],
          header->signature()[DexFile::kSha1DigestSize - 2],
          header->signature()[DexFile::kSha1DigestSize - 1]);
  fprintf(gOutFile, "file_size           : %d\n", header->file_size());
  fprintf(gOutFile, "header_size         : %d\n", header->header_size());
  fprintf(gOutFile, "link_size           : %d\n", header->link_size());
  fprintf(gOutFile, "link_off            : %d (0x%06x)\n",
          header->link_offset(), header->link_offset());
  fprintf(gOutFile, "string_ids_size     : %d\n", header->string_ids_size());
  fprintf(gOutFile, "string_ids_off      : %d (0x%06x)\n",
          header->string_ids_offset(), header->string_ids_offset());
  fprintf(gOutFile, "type_ids_size       : %d\n", header->type_ids_size());
  fprintf(gOutFile, "type_ids_off        : %d (0x%06x)\n",
          header->type_ids_offset(), header->type_ids_offset());
  fprintf(gOutFile, "proto_ids_size      : %d\n", header->proto_ids_size());
  fprintf(gOutFile, "proto_ids_off       : %d (0x%06x)\n",
          header->proto_ids_offset(), header->proto_ids_offset());
  fprintf(gOutFile, "field_ids_size      : %d\n", header->field_ids_size());
  fprintf(gOutFile, "field_ids_off       : %d (0x%06x)\n",
          header->field_ids_offset(), header->field_ids_offset());
  fprintf(gOutFile, "method_ids_size     : %d\n", header->method_ids_size());
  fprintf(gOutFile, "method_ids_off      : %d (0x%06x)\n",
          header->method_ids_offset(), header->method_ids_offset());
  fprintf(gOutFile, "class_defs_size     : %d\n", header->class_defs_size());
  fprintf(gOutFile, "class_defs_off      : %d (0x%06x)\n",
          header->class_defs_offset(), header->class_defs_offset());
  fprintf(gOutFile, "data_size           : %d\n", header->data_size());
  fprintf(gOutFile, "data_off            : %d (0x%06x)\n\n",
          header->data_offset(), header->data_offset());
}

/*
 * Dumps a class_def_item.
 */
static void dumpClassDef(dex_ir::Header* pHeader, int idx) {
  // General class information.
  dex_ir::ClassDef* pClassDef = (*pHeader->class_defs())[idx];
  fprintf(gOutFile, "Class #%d header:\n", idx);
  fprintf(gOutFile, "class_idx           : %d\n", pClassDef->class_type()->offset());
  fprintf(gOutFile, "access_flags        : %d (0x%04x)\n",
          pClassDef->access_flags(), pClassDef->access_flags());
  fprintf(gOutFile, "superclass_idx      : %d\n", pClassDef->superclass()->offset());
  fprintf(gOutFile, "interfaces_off      : %d (0x%06x)\n",
          pClassDef->interfaces_offset(), pClassDef->interfaces_offset());
  u4 source_file_offset = 0;
  if (pClassDef->source_file() != nullptr) {
    source_file_offset = pClassDef->source_file()->offset();
  }
  fprintf(gOutFile, "source_file_idx     : %d\n", source_file_offset);
  u4 annotations_offset = 0;
  if (pClassDef->annotations() != nullptr) {
    annotations_offset = pClassDef->annotations()->offset();
  }
  fprintf(gOutFile, "annotations_off     : %d (0x%06x)\n",
          annotations_offset, annotations_offset);
  fprintf(gOutFile, "class_data_off      : %d (0x%06x)\n",
          pClassDef->class_data()->offset(), pClassDef->class_data()->offset());

  // Fields and methods.
  dex_ir::ClassDef::ClassData* pClassData = pClassDef->class_data();
  if (pClassData != nullptr) {
    fprintf(gOutFile, "static_fields_size  : %zu\n", pClassData->static_fields()->size());
    fprintf(gOutFile, "instance_fields_size: %zu\n", pClassData->instance_fields()->size());
    fprintf(gOutFile, "direct_methods_size : %zu\n", pClassData->direct_methods()->size());
    fprintf(gOutFile, "virtual_methods_size: %zu\n", pClassData->virtual_methods()->size());
  } else {
    fprintf(gOutFile, "static_fields_size  : 0\n");
    fprintf(gOutFile, "instance_fields_size: 0\n");
    fprintf(gOutFile, "direct_methods_size : 0\n");
    fprintf(gOutFile, "virtual_methods_size: 0\n");
  }
  fprintf(gOutFile, "\n");
}

/**
 * Dumps an annotation set item.
 */
static void dumpAnnotationSetItem(dex_ir::AnnotationSetItem* set_item) {
  if (set_item == nullptr) {
    fputs("  empty-annotation-set\n", gOutFile);
    return;
  }
  for (auto* annotation : *set_item->items()) {
    if (annotation == nullptr) {
      continue;
    }
    fputs("  ", gOutFile);
    switch (annotation->visibility()) {
      case DexFile::kDexVisibilityBuild:   fputs("VISIBILITY_BUILD ",   gOutFile); break;
      case DexFile::kDexVisibilityRuntime: fputs("VISIBILITY_RUNTIME ", gOutFile); break;
      case DexFile::kDexVisibilitySystem:  fputs("VISIBILITY_SYSTEM ",  gOutFile); break;
      default:                             fputs("VISIBILITY_UNKNOWN ", gOutFile); break;
    }  // switch
    // Decode raw bytes in annotation.
    // const u1* rData = annotation->annotation_;
    dex_ir::ArrayItem* rData = annotation->item();
    dumpEncodedValue(rData);
    fputc('\n', gOutFile);
  }
}

/*
 * Dumps class annotations.
 */
static void dumpClassAnnotations(dex_ir::Header* pHeader, int idx) {
  dex_ir::ClassDef* pClassDef = (*pHeader->class_defs())[idx];
  dex_ir::AnnotationsDirectoryItem* dir = pClassDef->annotations();
  if (dir == nullptr) {
    return;  // none
  }

  fprintf(gOutFile, "Class #%d annotations:\n", idx);

  dex_ir::AnnotationSetItem* class_set_item = dir->class_annotation();
  std::vector<dex_ir::AnnotationsDirectoryItem::FieldAnnotation*>* fields =
      dir->field_annotations();
  std::vector<dex_ir::AnnotationsDirectoryItem::MethodAnnotation*>* methods =
      dir->method_annotations();
  std::vector<dex_ir::AnnotationsDirectoryItem::ParameterAnnotation*>* pars =
      dir->parameter_annotations();

  // Annotations on the class itself.
  if (class_set_item != nullptr) {
    fprintf(gOutFile, "Annotations on class\n");
    dumpAnnotationSetItem(class_set_item);
  }

  // Annotations on fields.
  if (fields != nullptr) {
    for (auto* field : *fields) {
      const dex_ir::FieldId* pFieldId = field->field_id();
      const u4 field_idx = pFieldId->offset();
      const char* field_name = pFieldId->name()->data();
      fprintf(gOutFile, "Annotations on field #%u '%s'\n", field_idx, field_name);
      dumpAnnotationSetItem(field->annotation_set_item());
    }
  }

  // Annotations on methods.
  if (methods != nullptr) {
    for (auto* method : *methods) {
      const dex_ir::MethodId* pMethodId = method->method_id();
      const u4 method_idx = pMethodId->offset();
      const char* method_name = pMethodId->name()->data();
      fprintf(gOutFile, "Annotations on method #%u '%s'\n", method_idx, method_name);
      dumpAnnotationSetItem(method->annotation_set_item());
    }
  }

  // Annotations on method parameters.
  if (pars != nullptr) {
    for (auto* par : *pars) {
      const dex_ir::MethodId* pMethodId = par->method_id();
      const u4 method_idx = pMethodId->offset();
      const char* method_name = pMethodId->name()->data();
      fprintf(gOutFile, "Annotations on method #%u '%s' parameters\n", method_idx, method_name);
      u4 j = 0;
      for (auto* annotation : *par->annotations()) {
        fprintf(gOutFile, "#%u\n", j);
        dumpAnnotationSetItem(annotation);
        ++j;
      }
    }
  }

  fputc('\n', gOutFile);
}

/*
 * Dumps an interface that a class declares to implement.
 */
static void dumpInterface(dex_ir::TypeId* pTypeItem, int i) {
  const char* interfaceName = pTypeItem->string_id()->data();
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : '%s'\n", i, interfaceName);
  } else {
    std::unique_ptr<char[]> dot(descriptorToDot(interfaceName));
    fprintf(gOutFile, "<implements name=\"%s\">\n</implements>\n", dot.get());
  }
}

/*
 * Dumps the catches table associated with the code.
 */
static void dumpCatches(const DexFile* pDexFile, const DexFile::CodeItem* pCode) {
  const u4 triesSize = pCode->tries_size_;

  // No catch table.
  if (triesSize == 0) {
    fprintf(gOutFile, "      catches       : (none)\n");
    return;
  }

  // Dump all table entries.
  fprintf(gOutFile, "      catches       : %d\n", triesSize);
  for (u4 i = 0; i < triesSize; i++) {
    const DexFile::TryItem* pTry = pDexFile->GetTryItems(*pCode, i);
    const u4 start = pTry->start_addr_;
    const u4 end = start + pTry->insn_count_;
    fprintf(gOutFile, "        0x%04x - 0x%04x\n", start, end);
    for (CatchHandlerIterator it(*pCode, *pTry); it.HasNext(); it.Next()) {
      const u2 tidx = it.GetHandlerTypeIndex();
      const char* descriptor =
          (tidx == DexFile::kDexNoIndex16) ? "<any>" : pDexFile->StringByTypeIdx(tidx);
      fprintf(gOutFile, "          %s -> 0x%04x\n", descriptor, it.GetHandlerAddress());
    }  // for
  }  // for
}

/*
 * Callback for dumping each positions table entry.
 */
static bool dumpPositionsCb(void* /*context*/, const DexFile::PositionInfo& entry) {
  fprintf(gOutFile, "        0x%04x line=%d\n", entry.address_, entry.line_);
  return false;
}

/*
 * Callback for dumping locals table entry.
 */
static void dumpLocalsCb(void* /*context*/, const DexFile::LocalInfo& entry) {
  const char* signature = entry.signature_ != nullptr ? entry.signature_ : "";
  fprintf(gOutFile, "        0x%04x - 0x%04x reg=%d %s %s %s\n",
          entry.start_address_, entry.end_address_, entry.reg_,
          entry.name_, entry.descriptor_, signature);
}

/*
 * Helper for dumpInstruction(), which builds the string
 * representation for the index in the given instruction.
 * Returns a pointer to a buffer of sufficient size.
 */
static std::unique_ptr<char[]> indexString(const DexFile* pDexFile,
                                           const Instruction* pDecInsn,
                                           size_t bufSize) {
  std::unique_ptr<char[]> buf(new char[bufSize]);
  // Determine index and width of the string.
  u4 index = 0;
  u4 width = 4;
  switch (Instruction::FormatOf(pDecInsn->Opcode())) {
    // SOME NOT SUPPORTED:
    // case Instruction::k20bc:
    case Instruction::k21c:
    case Instruction::k35c:
    // case Instruction::k35ms:
    case Instruction::k3rc:
    // case Instruction::k3rms:
    // case Instruction::k35mi:
    // case Instruction::k3rmi:
      index = pDecInsn->VRegB();
      width = 4;
      break;
    case Instruction::k31c:
      index = pDecInsn->VRegB();
      width = 8;
      break;
    case Instruction::k22c:
    // case Instruction::k22cs:
      index = pDecInsn->VRegC();
      width = 4;
      break;
    default:
      break;
  }  // switch

  // Determine index type.
  size_t outSize = 0;
  switch (Instruction::IndexTypeOf(pDecInsn->Opcode())) {
    case Instruction::kIndexUnknown:
      // This function should never get called for this type, but do
      // something sensible here, just to help with debugging.
      outSize = snprintf(buf.get(), bufSize, "<unknown-index>");
      break;
    case Instruction::kIndexNone:
      // This function should never get called for this type, but do
      // something sensible here, just to help with debugging.
      outSize = snprintf(buf.get(), bufSize, "<no-index>");
      break;
    case Instruction::kIndexTypeRef:
      if (index < pDexFile->GetHeader().type_ids_size_) {
        const char* tp = pDexFile->StringByTypeIdx(index);
        outSize = snprintf(buf.get(), bufSize, "%s // type@%0*x", tp, width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<type?> // type@%0*x", width, index);
      }
      break;
    case Instruction::kIndexStringRef:
      if (index < pDexFile->GetHeader().string_ids_size_) {
        const char* st = pDexFile->StringDataByIdx(index);
        outSize = snprintf(buf.get(), bufSize, "\"%s\" // string@%0*x", st, width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<string?> // string@%0*x", width, index);
      }
      break;
    case Instruction::kIndexMethodRef:
      if (index < pDexFile->GetHeader().method_ids_size_) {
        const DexFile::MethodId& pMethodId = pDexFile->GetMethodId(index);
        const char* name = pDexFile->StringDataByIdx(pMethodId.name_idx_);
        const Signature signature = pDexFile->GetMethodSignature(pMethodId);
        const char* backDescriptor = pDexFile->StringByTypeIdx(pMethodId.class_idx_);
        outSize = snprintf(buf.get(), bufSize, "%s.%s:%s // method@%0*x",
                           backDescriptor, name, signature.ToString().c_str(), width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<method?> // method@%0*x", width, index);
      }
      break;
    case Instruction::kIndexFieldRef:
      if (index < pDexFile->GetHeader().field_ids_size_) {
        const DexFile::FieldId& pFieldId = pDexFile->GetFieldId(index);
        const char* name = pDexFile->StringDataByIdx(pFieldId.name_idx_);
        const char* typeDescriptor = pDexFile->StringByTypeIdx(pFieldId.type_idx_);
        const char* backDescriptor = pDexFile->StringByTypeIdx(pFieldId.class_idx_);
        outSize = snprintf(buf.get(), bufSize, "%s.%s:%s // field@%0*x",
                           backDescriptor, name, typeDescriptor, width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<field?> // field@%0*x", width, index);
      }
      break;
    case Instruction::kIndexVtableOffset:
      outSize = snprintf(buf.get(), bufSize, "[%0*x] // vtable #%0*x",
                         width, index, width, index);
      break;
    case Instruction::kIndexFieldOffset:
      outSize = snprintf(buf.get(), bufSize, "[obj+%0*x]", width, index);
      break;
    // SOME NOT SUPPORTED:
    // case Instruction::kIndexVaries:
    // case Instruction::kIndexInlineMethod:
    default:
      outSize = snprintf(buf.get(), bufSize, "<?>");
      break;
  }  // switch

  // Determine success of string construction.
  if (outSize >= bufSize) {
    // The buffer wasn't big enough; retry with computed size. Note: snprintf()
    // doesn't count/ the '\0' as part of its returned size, so we add explicit
    // space for it here.
    return indexString(pDexFile, pDecInsn, outSize + 1);
  }
  return buf;
}

/*
 * Dumps a single instruction.
 */
static void dumpInstruction(const DexFile* pDexFile,
                            const DexFile::CodeItem* pCode,
                            u4 codeOffset, u4 insnIdx, u4 insnWidth,
                            const Instruction* pDecInsn) {
  // Address of instruction (expressed as byte offset).
  fprintf(gOutFile, "%06x:", codeOffset + 0x10 + insnIdx * 2);

  // Dump (part of) raw bytes.
  const u2* insns = pCode->insns_;
  for (u4 i = 0; i < 8; i++) {
    if (i < insnWidth) {
      if (i == 7) {
        fprintf(gOutFile, " ... ");
      } else {
        // Print 16-bit value in little-endian order.
        const u1* bytePtr = (const u1*) &insns[insnIdx + i];
        fprintf(gOutFile, " %02x%02x", bytePtr[0], bytePtr[1]);
      }
    } else {
      fputs("     ", gOutFile);
    }
  }  // for

  // Dump pseudo-instruction or opcode.
  if (pDecInsn->Opcode() == Instruction::NOP) {
    const u2 instr = get2LE((const u1*) &insns[insnIdx]);
    if (instr == Instruction::kPackedSwitchSignature) {
      fprintf(gOutFile, "|%04x: packed-switch-data (%d units)", insnIdx, insnWidth);
    } else if (instr == Instruction::kSparseSwitchSignature) {
      fprintf(gOutFile, "|%04x: sparse-switch-data (%d units)", insnIdx, insnWidth);
    } else if (instr == Instruction::kArrayDataSignature) {
      fprintf(gOutFile, "|%04x: array-data (%d units)", insnIdx, insnWidth);
    } else {
      fprintf(gOutFile, "|%04x: nop // spacer", insnIdx);
    }
  } else {
    fprintf(gOutFile, "|%04x: %s", insnIdx, pDecInsn->Name());
  }

  // Set up additional argument.
  std::unique_ptr<char[]> indexBuf;
  if (Instruction::IndexTypeOf(pDecInsn->Opcode()) != Instruction::kIndexNone) {
    indexBuf = indexString(pDexFile, pDecInsn, 200);
  }

  // Dump the instruction.
  //
  // NOTE: pDecInsn->DumpString(pDexFile) differs too much from original.
  //
  switch (Instruction::FormatOf(pDecInsn->Opcode())) {
    case Instruction::k10x:        // op
      break;
    case Instruction::k12x:        // op vA, vB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k11n:        // op vA, #+B
      fprintf(gOutFile, " v%d, #int %d // #%x",
              pDecInsn->VRegA(), (s4) pDecInsn->VRegB(), (u1)pDecInsn->VRegB());
      break;
    case Instruction::k11x:        // op vAA
      fprintf(gOutFile, " v%d", pDecInsn->VRegA());
      break;
    case Instruction::k10t:        // op +AA
    case Instruction::k20t: {      // op +AAAA
      const s4 targ = (s4) pDecInsn->VRegA();
      fprintf(gOutFile, " %04x // %c%04x",
              insnIdx + targ,
              (targ < 0) ? '-' : '+',
              (targ < 0) ? -targ : targ);
      break;
    }
    case Instruction::k22x:        // op vAA, vBBBB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k21t: {     // op vAA, +BBBB
      const s4 targ = (s4) pDecInsn->VRegB();
      fprintf(gOutFile, " v%d, %04x // %c%04x", pDecInsn->VRegA(),
              insnIdx + targ,
              (targ < 0) ? '-' : '+',
              (targ < 0) ? -targ : targ);
      break;
    }
    case Instruction::k21s:        // op vAA, #+BBBB
      fprintf(gOutFile, " v%d, #int %d // #%x",
              pDecInsn->VRegA(), (s4) pDecInsn->VRegB(), (u2)pDecInsn->VRegB());
      break;
    case Instruction::k21h:        // op vAA, #+BBBB0000[00000000]
      // The printed format varies a bit based on the actual opcode.
      if (pDecInsn->Opcode() == Instruction::CONST_HIGH16) {
        const s4 value = pDecInsn->VRegB() << 16;
        fprintf(gOutFile, " v%d, #int %d // #%x",
                pDecInsn->VRegA(), value, (u2) pDecInsn->VRegB());
      } else {
        const s8 value = ((s8) pDecInsn->VRegB()) << 48;
        fprintf(gOutFile, " v%d, #long %" PRId64 " // #%x",
                pDecInsn->VRegA(), value, (u2) pDecInsn->VRegB());
      }
      break;
    case Instruction::k21c:        // op vAA, thing@BBBB
    case Instruction::k31c:        // op vAA, thing@BBBBBBBB
      fprintf(gOutFile, " v%d, %s", pDecInsn->VRegA(), indexBuf.get());
      break;
    case Instruction::k23x:        // op vAA, vBB, vCC
      fprintf(gOutFile, " v%d, v%d, v%d",
              pDecInsn->VRegA(), pDecInsn->VRegB(), pDecInsn->VRegC());
      break;
    case Instruction::k22b:        // op vAA, vBB, #+CC
      fprintf(gOutFile, " v%d, v%d, #int %d // #%02x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              (s4) pDecInsn->VRegC(), (u1) pDecInsn->VRegC());
      break;
    case Instruction::k22t: {      // op vA, vB, +CCCC
      const s4 targ = (s4) pDecInsn->VRegC();
      fprintf(gOutFile, " v%d, v%d, %04x // %c%04x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              insnIdx + targ,
              (targ < 0) ? '-' : '+',
              (targ < 0) ? -targ : targ);
      break;
    }
    case Instruction::k22s:        // op vA, vB, #+CCCC
      fprintf(gOutFile, " v%d, v%d, #int %d // #%04x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              (s4) pDecInsn->VRegC(), (u2) pDecInsn->VRegC());
      break;
    case Instruction::k22c:        // op vA, vB, thing@CCCC
    // NOT SUPPORTED:
    // case Instruction::k22cs:    // [opt] op vA, vB, field offset CCCC
      fprintf(gOutFile, " v%d, v%d, %s",
              pDecInsn->VRegA(), pDecInsn->VRegB(), indexBuf.get());
      break;
    case Instruction::k30t:
      fprintf(gOutFile, " #%08x", pDecInsn->VRegA());
      break;
    case Instruction::k31i: {     // op vAA, #+BBBBBBBB
      // This is often, but not always, a float.
      union {
        float f;
        u4 i;
      } conv;
      conv.i = pDecInsn->VRegB();
      fprintf(gOutFile, " v%d, #float %g // #%08x",
              pDecInsn->VRegA(), conv.f, pDecInsn->VRegB());
      break;
    }
    case Instruction::k31t:       // op vAA, offset +BBBBBBBB
      fprintf(gOutFile, " v%d, %08x // +%08x",
              pDecInsn->VRegA(), insnIdx + pDecInsn->VRegB(), pDecInsn->VRegB());
      break;
    case Instruction::k32x:        // op vAAAA, vBBBB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k35c: {      // op {vC, vD, vE, vF, vG}, thing@BBBB
    // NOT SUPPORTED:
    // case Instruction::k35ms:       // [opt] invoke-virtual+super
    // case Instruction::k35mi:       // [opt] inline invoke
      u4 arg[Instruction::kMaxVarArgRegs];
      pDecInsn->GetVarArgs(arg);
      fputs(" {", gOutFile);
      for (int i = 0, n = pDecInsn->VRegA(); i < n; i++) {
        if (i == 0) {
          fprintf(gOutFile, "v%d", arg[i]);
        } else {
          fprintf(gOutFile, ", v%d", arg[i]);
        }
      }  // for
      fprintf(gOutFile, "}, %s", indexBuf.get());
      break;
    }
    case Instruction::k3rc:        // op {vCCCC .. v(CCCC+AA-1)}, thing@BBBB
    // NOT SUPPORTED:
    // case Instruction::k3rms:       // [opt] invoke-virtual+super/range
    // case Instruction::k3rmi:       // [opt] execute-inline/range
      {
        // This doesn't match the "dx" output when some of the args are
        // 64-bit values -- dx only shows the first register.
        fputs(" {", gOutFile);
        for (int i = 0, n = pDecInsn->VRegA(); i < n; i++) {
          if (i == 0) {
            fprintf(gOutFile, "v%d", pDecInsn->VRegC() + i);
          } else {
            fprintf(gOutFile, ", v%d", pDecInsn->VRegC() + i);
          }
        }  // for
        fprintf(gOutFile, "}, %s", indexBuf.get());
      }
      break;
    case Instruction::k51l: {      // op vAA, #+BBBBBBBBBBBBBBBB
      // This is often, but not always, a double.
      union {
        double d;
        u8 j;
      } conv;
      conv.j = pDecInsn->WideVRegB();
      fprintf(gOutFile, " v%d, #double %g // #%016" PRIx64,
              pDecInsn->VRegA(), conv.d, pDecInsn->WideVRegB());
      break;
    }
    // NOT SUPPORTED:
    // case Instruction::k00x:        // unknown op or breakpoint
    //    break;
    default:
      fprintf(gOutFile, " ???");
      break;
  }  // switch

  fputc('\n', gOutFile);
}

/*
 * Dumps a bytecode disassembly.
 */
static void dumpBytecodes(const DexFile* pDexFile, u4 idx,
                          const DexFile::CodeItem* pCode, u4 codeOffset) {
  const DexFile::MethodId& pMethodId = pDexFile->GetMethodId(idx);
  const char* name = pDexFile->StringDataByIdx(pMethodId.name_idx_);
  const Signature signature = pDexFile->GetMethodSignature(pMethodId);
  const char* backDescriptor = pDexFile->StringByTypeIdx(pMethodId.class_idx_);

  // Generate header.
  std::unique_ptr<char[]> dot(descriptorToDot(backDescriptor));
  fprintf(gOutFile, "%06x:                                        |[%06x] %s.%s:%s\n",
          codeOffset, codeOffset, dot.get(), name, signature.ToString().c_str());

  // Iterate over all instructions.
  const u2* insns = pCode->insns_;
  for (u4 insnIdx = 0; insnIdx < pCode->insns_size_in_code_units_;) {
    const Instruction* instruction = Instruction::At(&insns[insnIdx]);
    const u4 insnWidth = instruction->SizeInCodeUnits();
    if (insnWidth == 0) {
      fprintf(stderr, "GLITCH: zero-width instruction at idx=0x%04x\n", insnIdx);
      break;
    }
    dumpInstruction(pDexFile, pCode, codeOffset, insnIdx, insnWidth, instruction);
    insnIdx += insnWidth;
  }  // for
}

/*
 * Dumps code of a method.
 */
static void dumpCode(const DexFile* pDexFile, u4 idx, u4 flags,
                     const DexFile::CodeItem* pCode, u4 codeOffset) {
  fprintf(gOutFile, "      registers     : %d\n", pCode->registers_size_);
  fprintf(gOutFile, "      ins           : %d\n", pCode->ins_size_);
  fprintf(gOutFile, "      outs          : %d\n", pCode->outs_size_);
  fprintf(gOutFile, "      insns size    : %d 16-bit code units\n",
          pCode->insns_size_in_code_units_);

  // Bytecode disassembly, if requested.
  if (gOptions.disassemble) {
    dumpBytecodes(pDexFile, idx, pCode, codeOffset);
  }

  // Try-catch blocks.
  dumpCatches(pDexFile, pCode);

  // Positions and locals table in the debug info.
  bool is_static = (flags & kAccStatic) != 0;
  fprintf(gOutFile, "      positions     : \n");
  pDexFile->DecodeDebugPositionInfo(pCode, dumpPositionsCb, nullptr);
  fprintf(gOutFile, "      locals        : \n");
  pDexFile->DecodeDebugLocalInfo(pCode, is_static, idx, dumpLocalsCb, nullptr);
}

/*
 * Dumps a method.
 */
static void dumpMethod(const DexFile* pDexFile, u4 idx, u4 flags,
                       const DexFile::CodeItem* pCode, u4 codeOffset, int i) {
  // Bail for anything private if export only requested.
  if (gOptions.exportsOnly && (flags & (kAccPublic | kAccProtected)) == 0) {
    return;
  }

  const DexFile::MethodId& pMethodId = pDexFile->GetMethodId(idx);
  const char* name = pDexFile->StringDataByIdx(pMethodId.name_idx_);
  const Signature signature = pDexFile->GetMethodSignature(pMethodId);
  char* typeDescriptor = strdup(signature.ToString().c_str());
  const char* backDescriptor = pDexFile->StringByTypeIdx(pMethodId.class_idx_);
  char* accessStr = createAccessFlagStr(flags, kAccessForMethod);

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : (in %s)\n", i, backDescriptor);
    fprintf(gOutFile, "      name          : '%s'\n", name);
    fprintf(gOutFile, "      type          : '%s'\n", typeDescriptor);
    fprintf(gOutFile, "      access        : 0x%04x (%s)\n", flags, accessStr);
    if (pCode == nullptr) {
      fprintf(gOutFile, "      code          : (none)\n");
    } else {
      fprintf(gOutFile, "      code          -\n");
      dumpCode(pDexFile, idx, flags, pCode, codeOffset);
    }
    if (gOptions.disassemble) {
      fputc('\n', gOutFile);
    }
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    const bool constructor = (name[0] == '<');

    // Method name and prototype.
    if (constructor) {
      std::unique_ptr<char[]> dot(descriptorClassToDot(backDescriptor));
      fprintf(gOutFile, "<constructor name=\"%s\"\n", dot.get());
      dot = descriptorToDot(backDescriptor);
      fprintf(gOutFile, " type=\"%s\"\n", dot.get());
    } else {
      fprintf(gOutFile, "<method name=\"%s\"\n", name);
      const char* returnType = strrchr(typeDescriptor, ')');
      if (returnType == nullptr) {
        fprintf(stderr, "bad method type descriptor '%s'\n", typeDescriptor);
        goto bail;
      }
      std::unique_ptr<char[]> dot(descriptorToDot(returnType + 1));
      fprintf(gOutFile, " return=\"%s\"\n", dot.get());
      fprintf(gOutFile, " abstract=%s\n", quotedBool((flags & kAccAbstract) != 0));
      fprintf(gOutFile, " native=%s\n", quotedBool((flags & kAccNative) != 0));
      fprintf(gOutFile, " synchronized=%s\n", quotedBool(
          (flags & (kAccSynchronized | kAccDeclaredSynchronized)) != 0));
    }

    // Additional method flags.
    fprintf(gOutFile, " static=%s\n", quotedBool((flags & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((flags & kAccFinal) != 0));
    // The "deprecated=" not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n>\n", quotedVisibility(flags));

    // Parameters.
    if (typeDescriptor[0] != '(') {
      fprintf(stderr, "ERROR: bad descriptor '%s'\n", typeDescriptor);
      goto bail;
    }
    char* tmpBuf = reinterpret_cast<char*>(malloc(strlen(typeDescriptor) + 1));
    const char* base = typeDescriptor + 1;
    int argNum = 0;
    while (*base != ')') {
      char* cp = tmpBuf;
      while (*base == '[') {
        *cp++ = *base++;
      }
      if (*base == 'L') {
        // Copy through ';'.
        do {
          *cp = *base++;
        } while (*cp++ != ';');
      } else {
        // Primitive char, copy it.
        if (strchr("ZBCSIFJD", *base) == nullptr) {
          fprintf(stderr, "ERROR: bad method signature '%s'\n", base);
          break;  // while
        }
        *cp++ = *base++;
      }
      // Null terminate and display.
      *cp++ = '\0';
      std::unique_ptr<char[]> dot(descriptorToDot(tmpBuf));
      fprintf(gOutFile, "<parameter name=\"arg%d\" type=\"%s\">\n"
                        "</parameter>\n", argNum++, dot.get());
    }  // while
    free(tmpBuf);
    if (constructor) {
      fprintf(gOutFile, "</constructor>\n");
    } else {
      fprintf(gOutFile, "</method>\n");
    }
  }

 bail:
  free(typeDescriptor);
  free(accessStr);
}

/*
 * Dumps a static (class) field.
 */
static void dumpSField(dex_ir::Header* pHeader, u4 idx, u4 flags, int i, dex_ir::ArrayItem* init) {
  // Bail for anything private if export only requested.
  if (gOptions.exportsOnly && (flags & (kAccPublic | kAccProtected)) == 0) {
    return;
  }

  dex_ir::FieldId* pFieldId = (*pHeader->field_ids())[idx];
  const char* name = pFieldId->name()->data();
  const char* typeDescriptor = pFieldId->type()->string_id()->data();
  const char* backDescriptor = pFieldId->class_def()->class_type()->string_id()->data();
  char* accessStr = createAccessFlagStr(flags, kAccessForField);

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : (in %s)\n", i, backDescriptor);
    fprintf(gOutFile, "      name          : '%s'\n", name);
    fprintf(gOutFile, "      type          : '%s'\n", typeDescriptor);
    fprintf(gOutFile, "      access        : 0x%04x (%s)\n", flags, accessStr);
    if (init != nullptr) {
      fprintf(gOutFile, "INIT: %p\n", (void*) init);
      fputs("      value         : ", gOutFile);
      dumpEncodedValue(init);
      fputs("\n", gOutFile);
    }
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "<field name=\"%s\"\n", name);
    std::unique_ptr<char[]> dot(descriptorToDot(typeDescriptor));
    fprintf(gOutFile, " type=\"%s\"\n", dot.get());
    fprintf(gOutFile, " transient=%s\n", quotedBool((flags & kAccTransient) != 0));
    fprintf(gOutFile, " volatile=%s\n", quotedBool((flags & kAccVolatile) != 0));
    // The "value=" is not knowable w/o parsing annotations.
    fprintf(gOutFile, " static=%s\n", quotedBool((flags & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((flags & kAccFinal) != 0));
    // The "deprecated=" is not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n", quotedVisibility(flags));
    if (init != nullptr) {
      fputs(" value=\"", gOutFile);
      dumpEncodedValue(init);
      fputs("\"\n", gOutFile);
    }
    fputs(">\n</field>\n", gOutFile);
  }

  free(accessStr);
}

/*
 * Dumps an instance field.
 */
static void dumpIField(dex_ir::Header* pHeader, u4 idx, u4 flags, int i) {
  dumpSField(pHeader, idx, flags, i, nullptr);
}

/*
 * Dumping a CFG. Note that this will do duplicate work. utils.h doesn't expose the code-item
 * version, so the DumpMethodCFG code will have to iterate again to find it. But dexdump is a
 * tool, so this is not performance-critical.
 */

static void dumpCfg(const DexFile* dex_file,
                    u4 dex_method_idx,
                    const DexFile::CodeItem* code_item) {
  if (code_item != nullptr) {
    std::ostringstream oss;
    DumpMethodCFG(dex_file, dex_method_idx, oss);
    fprintf(gOutFile, "%s", oss.str().c_str());
  }
}

static void dumpCfg(const DexFile* dex_file, int idx) {
  const DexFile::ClassDef& class_def = dex_file->GetClassDef(idx);
  const u1* class_data = dex_file->GetClassData(class_def);
  if (class_data == nullptr) {  // empty class such as a marker interface?
    return;
  }
  ClassDataItemIterator it(*dex_file, class_data);
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  while (it.HasNextDirectMethod()) {
    dumpCfg(dex_file,
            it.GetMemberIndex(),
            it.GetMethodCodeItem());
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    dumpCfg(dex_file,
                it.GetMemberIndex(),
                it.GetMethodCodeItem());
    it.Next();
  }
}

/*
 * Dumps the class.
 *
 * Note "idx" is a DexClassDef index, not a DexTypeId index.
 *
 * If "*pLastPackage" is nullptr or does not match the current class' package,
 * the value will be replaced with a newly-allocated string.
 */
static void dumpClass(dex_ir::Header* pHeader, int idx, char** pLastPackage) {
  dex_ir::ClassDef* pClassDef = (*pHeader->class_defs())[idx];
  // Omitting non-public class.
  if (gOptions.exportsOnly && (pClassDef->access_flags() & kAccPublic) == 0) {
    return;
  }

  if (gOptions.showSectionHeaders) {
    dumpClassDef(pHeader, idx);
  }

  if (gOptions.showAnnotations) {
    dumpClassAnnotations(pHeader, idx);
  }

  if (gOptions.showCfg) {
    dumpCfg(&pHeader->dex_file(), idx);
    return;
  }

  // For the XML output, show the package name.  Ideally we'd gather
  // up the classes, sort them, and dump them alphabetically so the
  // package name wouldn't jump around, but that's not a great plan
  // for something that needs to run on the device.
  const char* classDescriptor = (*pHeader->class_defs())[idx]->class_type()->string_id()->data();
  if (!(classDescriptor[0] == 'L' &&
        classDescriptor[strlen(classDescriptor)-1] == ';')) {
    // Arrays and primitives should not be defined explicitly. Keep going?
    fprintf(stderr, "Malformed class name '%s'\n", classDescriptor);
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    char* mangle = strdup(classDescriptor + 1);
    mangle[strlen(mangle)-1] = '\0';

    // Reduce to just the package name.
    char* lastSlash = strrchr(mangle, '/');
    if (lastSlash != nullptr) {
      *lastSlash = '\0';
    } else {
      *mangle = '\0';
    }

    for (char* cp = mangle; *cp != '\0'; cp++) {
      if (*cp == '/') {
        *cp = '.';
      }
    }  // for

    if (*pLastPackage == nullptr || strcmp(mangle, *pLastPackage) != 0) {
      // Start of a new package.
      if (*pLastPackage != nullptr) {
        fprintf(gOutFile, "</package>\n");
      }
      fprintf(gOutFile, "<package name=\"%s\"\n>\n", mangle);
      free(*pLastPackage);
      *pLastPackage = mangle;
    } else {
      free(mangle);
    }
  }

  // General class information.
  char* accessStr = createAccessFlagStr(pClassDef->access_flags(), kAccessForClass);
  const char* superclassDescriptor = nullptr;
  if (pClassDef->superclass() != nullptr) {
    superclassDescriptor = pClassDef->superclass()->string_id()->data();
  }
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "Class #%d            -\n", idx);
    fprintf(gOutFile, "  Class descriptor  : '%s'\n", classDescriptor);
    fprintf(gOutFile, "  Access flags      : 0x%04x (%s)\n", pClassDef->access_flags(), accessStr);
    if (superclassDescriptor != nullptr) {
      fprintf(gOutFile, "  Superclass        : '%s'\n", superclassDescriptor);
    }
    fprintf(gOutFile, "  Interfaces        -\n");
  } else {
    std::unique_ptr<char[]> dot(descriptorClassToDot(classDescriptor));
    fprintf(gOutFile, "<class name=\"%s\"\n", dot.get());
    if (superclassDescriptor != nullptr) {
      dot = descriptorToDot(superclassDescriptor);
      fprintf(gOutFile, " extends=\"%s\"\n", dot.get());
    }
    fprintf(gOutFile, " interface=%s\n",
            quotedBool((pClassDef->access_flags() & kAccInterface) != 0));
    fprintf(gOutFile, " abstract=%s\n",
            quotedBool((pClassDef->access_flags() & kAccAbstract) != 0));
    fprintf(gOutFile, " static=%s\n", quotedBool((pClassDef->access_flags() & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((pClassDef->access_flags() & kAccFinal) != 0));
    // The "deprecated=" not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n", quotedVisibility(pClassDef->access_flags()));
    fprintf(gOutFile, ">\n");
  }

  // Interfaces.
  std::vector<dex_ir::TypeId*>* pInterfaces = pClassDef->interfaces();
  for (u4 i = 0; i < pInterfaces->size(); i++) {
    dumpInterface((*pInterfaces)[i], i);
  }  // for

  // Fields and methods.
  dex_ir::ClassDef::ClassData* pClassData = pClassDef->class_data();
  // Prepare data for static fields.
  std::vector<dex_ir::ArrayItem*>* pStaticValues = pClassDef->static_values();
  const u4 sSize = (pStaticValues == nullptr) ? 0 : pStaticValues->size();

  // Static fields.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Static fields     -\n");
  }
  std::vector<dex_ir::FieldItem*>* pStaticFields = pClassData->static_fields();
  for (u4 i = 0; i < pStaticFields->size(); i++) {
    dumpSField(pHeader,
               (*pStaticFields)[i]->field_id()->offset(),
               (*pStaticFields)[i]->access_flags(),
               i,
               i < sSize ? (*pStaticValues)[i] : nullptr);
  }  // for

  // Instance fields.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Instance fields   -\n");
  }
  std::vector<dex_ir::FieldItem*>* pInstanceFields = pClassData->instance_fields();
  for (u4 i = 0; i < pInstanceFields->size(); i++) {
    dumpIField(pHeader,
               (*pInstanceFields)[i]->field_id()->offset(),
               (*pInstanceFields)[i]->access_flags(),
               i);
  }  // for

  // Direct methods.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Direct methods    -\n");
  }
  std::vector<dex_ir::MethodItem*>* pDirectMethods = pClassData->direct_methods();
  for (u4 i = 0; i < pDirectMethods->size(); i++) {
    /*
    dumpMethod(pDexFile, pClassData.GetMemberIndex(),
                         pClassData.GetRawMemberAccessFlags(),
                         pClassData.GetMethodCodeItem(),
                         pClassData.GetMethodCodeItemOffset(), i);
                         */
  }  // for

  // Virtual methods.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Virtual methods   -\n");
  }
  std::vector<dex_ir::MethodItem*>* pVirtualMethods = pClassData->virtual_methods();
  for (u4 i = 0; i < pVirtualMethods->size(); i++) {
    /*
    dumpMethod(pDexFile, pClassData.GetMemberIndex(),
                         pClassData.GetRawMemberAccessFlags(),
                         pClassData.GetMethodCodeItem(),
                         pClassData.GetMethodCodeItemOffset(), i);
                         */
  }  // for

  // End of class.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    const char* fileName = "unknown";
    if (pClassDef->source_file() != nullptr) {
      fileName = pClassDef->source_file()->data();
    }
    const dex_ir::StringId* pSourceFile = pClassDef->source_file();
    fprintf(gOutFile, "  source_file_idx   : %d (%s)\n\n",
            pSourceFile == nullptr ? 0 : pSourceFile->offset(), fileName);
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "</class>\n");
  }

  free(accessStr);
}

/*
 * Dumps the requested sections of the file.
 */
static void processDexFile(const char* fileName, const DexFile* pDexFile) {
  if (gOptions.verbose) {
    fprintf(gOutFile, "Opened '%s', DEX version '%.3s'\n",
            fileName, pDexFile->GetHeader().magic_ + 4);
  }
  dex_ir::Header header(*pDexFile);

  // Headers.
  if (gOptions.showFileHeaders) {
    dumpFileHeader(&header);
  }

  // Open XML context.
  if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "<api>\n");
  }

  // Iterate over all classes.
  char* package = nullptr;
  const u4 classDefsSize = header.class_defs_size();
  for (u4 i = 0; i < classDefsSize; i++) {
    dumpClass(&header, i, &package);
  }  // for

  // Free the last package allocated.
  if (package != nullptr) {
    fprintf(gOutFile, "</package>\n");
    free(package);
  }

  // Close XML context.
  if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "</api>\n");
  }
}

/*
 * Processes a single file (either direct .dex or indirect .zip/.jar/.apk).
 */
int processFile(const char* fileName) {
  if (gOptions.verbose) {
    fprintf(gOutFile, "Processing '%s'...\n", fileName);
  }

  // If the file is not a .dex file, the function tries .zip/.jar/.apk files,
  // all of which are Zip archives with "classes.dex" inside.
  const bool kVerifyChecksum = !gOptions.ignoreBadChecksum;
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (!DexFile::Open(fileName, fileName, kVerifyChecksum, &error_msg, &dex_files)) {
    // Display returned error message to user. Note that this error behavior
    // differs from the error messages shown by the original Dalvik dexdump.
    fputs(error_msg.c_str(), stderr);
    fputc('\n', stderr);
    return -1;
  }

  // Success. Either report checksum verification or process
  // all dex files found in given file.
  if (gOptions.checksumOnly) {
    fprintf(gOutFile, "Checksum verified\n");
  } else {
    for (size_t i = 0; i < dex_files.size(); i++) {
      processDexFile(fileName, dex_files[i].get());
    }
  }
  return 0;
}

}  // namespace art
