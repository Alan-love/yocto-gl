//
// Implementation for Yocto/ModelIO.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2019 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// -----------------------------------------------------------------------------
// INCLUDES
// -----------------------------------------------------------------------------

#include "yocto_modelio.h"

#include "yocto_image.h"

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdarg>
#include <limits>
#include <string_view>

#include "ext/filesystem.hpp"
namespace fs = ghc::filesystem;

// -----------------------------------------------------------------------------
// FILE AND PROPERTY HANDLING
// -----------------------------------------------------------------------------
namespace yocto {

// copnstrucyor and destructors
file_wrapper::file_wrapper(file_wrapper&& other) {
  this->fs       = other.fs;
  this->filename = other.filename;
  other.fs       = nullptr;
}
file_wrapper::~file_wrapper() {
  if (fs) fclose(fs);
  fs = nullptr;
}

// Opens a file returing a handle with RIIA
bool open_file(file_wrapper& fs, const string& filename, const string& mode) {
  close_file(fs);
  fs.filename = filename;
  fs.mode     = mode;
  fs.fs       = fopen(filename.c_str(), mode.c_str());
  return (bool)fs;
}
file_wrapper open_file(const string& filename, const string& mode) {
  auto fs     = file_wrapper{};
  fs.filename = filename;
  fs.mode     = mode;
  fs.fs       = fopen(filename.c_str(), mode.c_str());
  return fs;
}
void close_file(file_wrapper& fs) {
  if (fs.fs) fclose(fs.fs);
  fs.fs = nullptr;
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// LOW-LEVEL UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

using std::string_view;
using namespace std::literals::string_view_literals;

template <typename T>
static inline T swap_endian(T value) {
  // https://stackoverflow.com/questions/105252/how-do-i-convert-between-big-endian-and-little-endian-values-in-c
  static_assert(CHAR_BIT == 8, "CHAR_BIT != 8");
  union {
    T             value;
    unsigned char bytes[sizeof(T)];
  } source, dest;
  source.value = value;
  for (auto k = (size_t)0; k < sizeof(T); k++)
    dest.bytes[k] = source.bytes[sizeof(T) - k - 1];
  return dest.value;
}

// Read a line
static inline bool read_line(file_wrapper& fs, char* buffer, size_t size) {
  auto ok = fgets(buffer, size, fs.fs) != nullptr;
  if (ok) fs.linenum += 1;
  return ok;
}

static inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static inline bool is_newline(char c) { return c == '\r' || c == '\n'; }
static inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
static inline bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline void skip_whitespace(string_view& str) {
  while (!str.empty() && is_space(str.front())) str.remove_prefix(1);
}
static inline void trim_whitespace(string_view& str) {
  while (!str.empty() && is_space(str.front())) str.remove_prefix(1);
  while (!str.empty() && is_space(str.back())) str.remove_suffix(1);
}

static inline bool is_whitespace(string_view str) {
  while (!str.empty()) {
    if (!is_space(str.front())) return false;
    str.remove_prefix(1);
  }
  return true;
}

static inline vector<string> split_string(
    const string& str, const string& delim) {
  auto tokens = vector<string>{};
  auto last = (size_t)0, next = (size_t)0;
  while ((next = str.find(delim, last)) != string::npos) {
    tokens.push_back(str.substr(last, next - last));
    last = next + delim.size();
  }
  if (last < str.size()) tokens.push_back(str.substr(last));
  return tokens;
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// PLY CONVERSION
// -----------------------------------------------------------------------------
namespace yocto {

static inline void remove_ply_comment(
    string_view& str, char comment_char = '#') {
  while (!str.empty() && is_newline(str.back())) str.remove_suffix(1);
  auto cpy = str;
  while (!cpy.empty() && cpy.front() != comment_char) cpy.remove_prefix(1);
  str.remove_suffix(cpy.size());
}

// Parse values from a string
static inline void parse_ply_value(string_view& str, string_view& value) {
  skip_whitespace(str);
  if (str.empty()) throw std::runtime_error("cannot parse value");
  if (str.front() != '"') {
    auto cpy = str;
    while (!cpy.empty() && !is_space(cpy.front())) cpy.remove_prefix(1);
    value = str;
    value.remove_suffix(cpy.size());
    str.remove_prefix(str.size() - cpy.size());
  } else {
    if (str.front() != '"') throw std::runtime_error("cannot parse value");
    str.remove_prefix(1);
    if (str.empty()) throw std::runtime_error("cannot parse value");
    auto cpy = str;
    while (!cpy.empty() && cpy.front() != '"') cpy.remove_prefix(1);
    if (cpy.empty()) throw std::runtime_error("cannot parse value");
    value = str;
    value.remove_suffix(cpy.size());
    str.remove_prefix(str.size() - cpy.size());
    str.remove_prefix(1);
  }
}
static inline void parse_ply_value(string_view& str, string& value) {
  auto valuev = ""sv;
  parse_ply_value(str, valuev);
  value = string{valuev};
}
static inline void parse_ply_value(string_view& str, size_t& value) {
  char* end = nullptr;
  value     = (size_t)strtoull(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}

// get ply value either ascii or binary
template <typename T, typename VT>
static inline bool read_ply_value(
    file_wrapper& fs, bool big_endian, VT& tvalue) {
  auto value = (T)0;
  if (fread(&value, sizeof(T), 1, fs.fs) != 1) return false;
  if (big_endian) value = swap_endian(value);
  tvalue = (VT)value;
  return true;
}
template <typename VT>
static inline bool read_ply_prop(
    file_wrapper& fs, bool big_endian, ply_type type, VT& value) {
  switch (type) {
    case ply_type::i8: return read_ply_value<int8_t>(fs, big_endian, value);
    case ply_type::i16: return read_ply_value<int16_t>(fs, big_endian, value);
    case ply_type::i32: return read_ply_value<int32_t>(fs, big_endian, value);
    case ply_type::i64: return read_ply_value<int64_t>(fs, big_endian, value);
    case ply_type::u8: return read_ply_value<uint8_t>(fs, big_endian, value);
    case ply_type::u16: return read_ply_value<uint16_t>(fs, big_endian, value);
    case ply_type::u32: return read_ply_value<uint32_t>(fs, big_endian, value);
    case ply_type::u64: return read_ply_value<uint64_t>(fs, big_endian, value);
    case ply_type::f32: return read_ply_value<float>(fs, big_endian, value);
    case ply_type::f64: return read_ply_value<double>(fs, big_endian, value);
  }
}

template <typename VT>
static inline bool parse_ply_prop(string_view& str, ply_type type, VT& value) {
  char* end = nullptr;
  switch (type) {
    case ply_type::i8: value = (VT)strtol(str.data(), &end, 10); break;
    case ply_type::i16: value = (VT)strtol(str.data(), &end, 10); break;
    case ply_type::i32: value = (VT)strtol(str.data(), &end, 10); break;
    case ply_type::i64: value = (VT)strtoll(str.data(), &end, 10); break;
    case ply_type::u8: value = (VT)strtoul(str.data(), &end, 10); break;
    case ply_type::u16: value = (VT)strtoul(str.data(), &end, 10); break;
    case ply_type::u32: value = (VT)strtoul(str.data(), &end, 10); break;
    case ply_type::u64: value = (VT)strtoull(str.data(), &end, 10); break;
    case ply_type::f32: value = (VT)strtof(str.data(), &end); break;
    case ply_type::f64: value = (VT)strtod(str.data(), &end); break;
  }
  if (str == end) return false;
  str.remove_prefix(end - str.data());
  return true;
}

// Load ply data
bool read_ply_header(file_wrapper& fs, ply_format& format,
    vector<ply_element>& elements, vector<string>& comments) {
  // ply type names
  static auto type_map = unordered_map<string, ply_type>{
      {"char", ply_type::i8},
      {"short", ply_type::i16},
      {"int", ply_type::i32},
      {"long", ply_type::i64},
      {"uchar", ply_type::u8},
      {"ushort", ply_type::u16},
      {"uint", ply_type::u32},
      {"ulong", ply_type::u64},
      {"float", ply_type::f32},
      {"double", ply_type::f64},
      {"int8", ply_type::i8},
      {"int16", ply_type::i16},
      {"int32", ply_type::i32},
      {"int64", ply_type::i64},
      {"uint8", ply_type::u8},
      {"uint16", ply_type::u16},
      {"uint32", ply_type::u32},
      {"uint64", ply_type::u64},
      {"float32", ply_type::f32},
      {"float64", ply_type::f64},
  };

  // parsing checks
  auto first_line = true;
  auto end_header = false;

  // prepare elements
  elements.clear();

  // read the file header line by line
  char buffer[4096];
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_ply_comment(line);
    skip_whitespace(line);
    if (line.empty()) continue;

    // get command
    auto cmd = ""s;
    parse_ply_value(line, cmd);
    if (cmd == "") continue;

    // check magic number
    if (first_line) {
      if (cmd != "ply") return false;
      first_line = false;
      continue;
    }

    // possible token values
    if (cmd == "ply") {
      if (!first_line) return false;
    } else if (cmd == "format") {
      auto fmt = ""sv;
      parse_ply_value(line, fmt);
      if (fmt == "ascii") {
        format = ply_format::ascii;
      } else if (fmt == "binary_little_endian") {
        format = ply_format::binary_little_endian;
      } else if (fmt == "binary_big_endian") {
        format = ply_format::binary_big_endian;
      } else {
        return false;
      }
    } else if (cmd == "comment") {
      skip_whitespace(line);
      comments.push_back(string{line});
    } else if (cmd == "obj_info") {
      skip_whitespace(line);
      // comment is the rest of the line
    } else if (cmd == "element") {
      auto& elem = elements.emplace_back();
      parse_ply_value(line, elem.name);
      parse_ply_value(line, elem.count);
    } else if (cmd == "property") {
      if (elements.empty()) return false;
      auto& prop  = elements.back().properties.emplace_back();
      auto  tname = ""s;
      parse_ply_value(line, tname);
      if (tname == "list") {
        prop.is_list = true;
        auto ename   = ""s;
        parse_ply_value(line, tname);
        if (type_map.find(tname) == type_map.end()) return false;
        prop.value_type = type_map.at(tname);
        parse_ply_value(line, tname);
        if (type_map.find(tname) == type_map.end()) return false;
        prop.list_type = type_map.at(tname);
      } else {
        prop.is_list = false;
        if (type_map.find(tname) == type_map.end()) return false;
        prop.value_type = type_map.at(tname);
      }
      parse_ply_value(line, prop.name);
    } else if (cmd == "end_header") {
      end_header = true;
      break;
    } else {
      return false;
    }
  }

  if (!end_header) return false;

  return true;
}

template <typename VT, typename LT>
bool read_ply_value_impl(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<VT>& values, vector<vector<LT>>& lists) {
  // prepare properties
  if (values.size() != element.properties.size()) {
    values.resize(element.properties.size());
  }
  if (lists.size() != element.properties.size()) {
    lists.resize(element.properties.size());
  }
  for (auto& list : lists) list.clear();

  // read property values
  if (format == ply_format::ascii) {
    char buffer[4096];
    if (!read_line(fs, buffer, sizeof(buffer))) return false;
    auto line = string_view{buffer};
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop  = element.properties[pidx];
      auto& value = values[pidx];
      auto& list  = lists[pidx];
      if (!parse_ply_prop(line, prop.value_type, value)) return false;
      if (prop.is_list) {
        list.resize((int)value);
        for (auto i = 0; i < (int)value; i++)
          if (!parse_ply_prop(line, prop.list_type, list[i])) return false;
      }
    }
    return true;
  } else {
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop  = element.properties[pidx];
      auto& value = values[pidx];
      auto& list  = lists[pidx];
      if (!read_ply_prop(fs, format == ply_format::binary_big_endian,
              prop.value_type, value))
        return false;
      if (prop.is_list) {
        list.resize((int)value);
        for (auto i = 0; i < (int)value; i++)
          if (!read_ply_prop(fs, format == ply_format::binary_big_endian,
                  prop.list_type, list[i]))
            return false;
      }
    }
  }
  return true;
}

// Write text to file
static inline bool write_ply_text(file_wrapper& fs, const char* value) {
  return fprintf(fs.fs, "%s", value) >= 0;
}

template <typename VT>
static inline bool write_ply_prop(file_wrapper& fs, ply_type type, VT value) {
  switch (type) {
    case ply_type::i8: return fprintf(fs.fs, "%d", (int)value) >= 0;
    case ply_type::i16: return fprintf(fs.fs, "%d", (int)value) >= 0;
    case ply_type::i32: return fprintf(fs.fs, "%d", (int)value) >= 0;
    case ply_type::i64: return fprintf(fs.fs, "%lld", (long long)value) >= 0;
    case ply_type::u8: return fprintf(fs.fs, "%u", (unsigned)value) >= 0;
    case ply_type::u16: return fprintf(fs.fs, "%u", (unsigned)value) >= 0;
    case ply_type::u32: return fprintf(fs.fs, "%u", (unsigned)value) >= 0;
    case ply_type::u64:
      return fprintf(fs.fs, "%llu", (unsigned long long)value) >= 0;
    case ply_type::f32: return fprintf(fs.fs, "%g", (float)value) >= 0;
    case ply_type::f64: return fprintf(fs.fs, "%g", (double)value) >= 0;
  }
}

template <typename T, typename VT>
static inline bool write_ply_binprop(
    file_wrapper& fs, bool big_endian, VT value) {
  auto typed_value = (T)value;
  if (big_endian) typed_value = swap_endian(typed_value);
  return fwrite(&typed_value, sizeof(T), 1, fs.fs) == 1;
}

template <typename VT>
static inline bool write_ply_binprop(
    file_wrapper& fs, bool big_endian, ply_type type, VT value) {
  switch (type) {
    case ply_type::i8: return write_ply_binprop<int8_t>(fs, big_endian, value);
    case ply_type::i16:
      return write_ply_binprop<int16_t>(fs, big_endian, value);
    case ply_type::i32:
      return write_ply_binprop<int32_t>(fs, big_endian, value);
    case ply_type::i64:
      return write_ply_binprop<int64_t>(fs, big_endian, value);
    case ply_type::u8: return write_ply_binprop<uint8_t>(fs, big_endian, value);
    case ply_type::u16:
      return write_ply_binprop<uint16_t>(fs, big_endian, value);
    case ply_type::u32:
      return write_ply_binprop<uint32_t>(fs, big_endian, value);
    case ply_type::u64:
      return write_ply_binprop<uint64_t>(fs, big_endian, value);
    case ply_type::f32: return write_ply_binprop<float>(fs, big_endian, value);
    case ply_type::f64: return write_ply_binprop<double>(fs, big_endian, value);
  }
}

// Write Ply functions
bool write_ply_header(file_wrapper& fs, ply_format format,
    const vector<ply_element>& elements, const vector<string>& comments) {
  // ply type names
  static auto type_map = unordered_map<ply_type, string>{
      {ply_type::i8, "char"},
      {ply_type::i16, "short"},
      {ply_type::i32, "int"},
      {ply_type::i64, "uint"},
      {ply_type::u8, "uchar"},
      {ply_type::u16, "ushort"},
      {ply_type::u32, "uint"},
      {ply_type::u64, "ulong"},
      {ply_type::f32, "float"},
      {ply_type::f64, "double"},
  };

  if (fprintf(fs.fs, "ply\n") < 0) return false;
  switch (format) {
    case ply_format::ascii:
      if (fprintf(fs.fs, "format ascii 1.0\n") < 0) return false;
      break;
    case ply_format::binary_little_endian:
      if (fprintf(fs.fs, "format binary_little_endian 1.0\n") < 0) return false;
      break;
    case ply_format::binary_big_endian:
      if (fprintf(fs.fs, "format binary_big_endian 1.0\n") < 0) return false;
      break;
  }
  for (auto& comment : comments)
    if (fprintf(fs.fs, "comment %s\n", comment.c_str()) < 0) return false;
  for (auto& elem : elements) {
    if (fprintf(fs.fs, "element %s %llu\n", elem.name.c_str(),
            (unsigned long long)elem.count) < 0)
      return false;
    for (auto& prop : elem.properties) {
      if (prop.is_list) {
        if (fprintf(fs.fs, "property list %s %s %s\n",
                type_map[prop.value_type].c_str(),
                type_map[prop.list_type].c_str(), prop.name.c_str()) < 0)
          return false;
      } else {
        if (fprintf(fs.fs, "property %s %s\n",
                type_map[prop.value_type].c_str(), prop.name.c_str()) < 0)
          return false;
      }
    }
  }
  if (fprintf(fs.fs, "end_header\n") < 0) return false;
  return true;
}

template <typename VT, typename LT>
bool write_ply_value_impl(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<VT>& values, vector<vector<LT>>& lists) {
  if (format == ply_format::ascii) {
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop = element.properties[pidx];
      if (pidx)
        if (!write_ply_text(fs, " ")) return false;
      if (!write_ply_prop(fs, prop.value_type, values[pidx])) return false;
      if (prop.is_list) {
        for (auto i = 0; i < (int)lists[pidx].size(); i++) {
          if (i)
            if (!write_ply_text(fs, " ")) return false;
          if (!write_ply_prop(fs, prop.list_type, lists[pidx][i])) return false;
        }
      }
      if (!write_ply_text(fs, "\n")) return false;
    }
    return true;
  } else {
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop = element.properties[pidx];
      if (!write_ply_binprop(fs, format == ply_format::binary_big_endian,
              prop.value_type, values[pidx]))
        return false;
      if (prop.is_list) {
        for (auto i = 0; i < (int)lists[pidx].size(); i++)
          if (!write_ply_binprop(fs, format == ply_format::binary_big_endian,
                  prop.list_type, lists[pidx][i]))
            return false;
      }
    }
    return true;
  }
}

bool write_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<double>& values,
    vector<vector<double>>& lists) {
  return write_ply_value_impl(fs, format, element, values, lists);
}
bool write_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<float>& values,
    vector<vector<int>>& lists) {
  return write_ply_value_impl(fs, format, element, values, lists);
}

bool read_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<double>& values,
    vector<vector<double>>& lists) {
  return read_ply_value_impl(fs, format, element, values, lists);
}
bool read_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<float>& values,
    vector<vector<int>>& lists) {
  return read_ply_value_impl(fs, format, element, values, lists);
}

int find_ply_element(const vector<ply_element>& elements, const string& name) {
  for (auto idx = 0; idx < elements.size(); idx++)
    if (elements[idx].name == name) return idx;
  return -1;
}
int find_ply_property(const ply_element& element, const string& name) {
  for (auto idx = 0; idx < element.properties.size(); idx++)
    if (element.properties[idx].name == name) return idx;
  return -1;
}
vec2i find_ply_property(
    const ply_element& element, const string& name1, const string& name2) {
  auto ids = vec2i{
      find_ply_property(element, name1),
      find_ply_property(element, name2),
  };
  if (ids.x < 0 || ids.y < 0) return vec2i{-1};
  return ids;
}
vec3i find_ply_property(const ply_element& element, const string& name1,
    const string& name2, const string& name3) {
  auto ids = vec3i{
      find_ply_property(element, name1),
      find_ply_property(element, name2),
      find_ply_property(element, name3),
  };
  if (ids.x < 0 || ids.y < 0 || ids.z < 0) return vec3i{-1};
  return ids;
}
vec4i find_ply_property(const ply_element& element, const string& name1,
    const string& name2, const string& name3, const string& name4) {
  auto ids = vec4i{
      find_ply_property(element, name1),
      find_ply_property(element, name2),
      find_ply_property(element, name3),
      find_ply_property(element, name4),
  };
  if (ids.x < 0 || ids.y < 0 || ids.z < 0 || ids.w < 0) return vec4i{-1};
  return ids;
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// OBJ CONVERSION
// -----------------------------------------------------------------------------
namespace yocto {

static inline void remove_obj_comment(
    string_view& str, char comment_char = '#') {
  while (!str.empty() && is_newline(str.back())) str.remove_suffix(1);
  auto cpy = str;
  while (!cpy.empty() && cpy.front() != comment_char) cpy.remove_prefix(1);
  str.remove_suffix(cpy.size());
}

// Parse values from a string
static inline bool parse_obj_value(string_view& str, string_view& value) {
  skip_whitespace(str);
  if (str.empty()) return false;
  if (str.front() != '"') {
    auto cpy = str;
    while (!cpy.empty() && !is_space(cpy.front())) cpy.remove_prefix(1);
    value = str;
    value.remove_suffix(cpy.size());
    str.remove_prefix(str.size() - cpy.size());
  } else {
    if (str.front() != '"') return false;
    str.remove_prefix(1);
    if (str.empty()) return false;
    auto cpy = str;
    while (!cpy.empty() && cpy.front() != '"') cpy.remove_prefix(1);
    if (cpy.empty()) return false;
    value = str;
    value.remove_suffix(cpy.size());
    str.remove_prefix(str.size() - cpy.size());
    str.remove_prefix(1);
  }
  return true;
}
static inline bool parse_obj_value(string_view& str, string& value) {
  auto valuev = ""sv;
  if (!parse_obj_value(str, valuev)) return false;
  value = string{valuev};
  return true;
}
static inline bool parse_obj_value(string_view& str, int& value) {
  char* end = nullptr;
  value     = (int)strtol(str.data(), &end, 10);
  if (str == end) return false;
  str.remove_prefix(end - str.data());
  return true;
}
static inline bool parse_obj_value(string_view& str, float& value) {
  char* end = nullptr;
  value     = strtof(str.data(), &end);
  if (str == end) return false;
  str.remove_prefix(end - str.data());
  return true;
}
template <typename T>
static inline bool parse_obj_value(string_view& str, T* values, int num) {
  for (auto i = 0; i < num; i++)
    if (!parse_obj_value(str, values[i])) return false;
  return true;
}

static inline bool parse_obj_value(string_view& str, vec2f& value) {
  return parse_obj_value(str, &value.x, 2);
}
static inline bool parse_obj_value(string_view& str, vec3f& value) {
  return parse_obj_value(str, &value.x, 3);
}
static inline bool parse_obj_value(string_view& str, frame3f& value) {
  return parse_obj_value(str, &value.x.x, 12);
}

template <typename T>
static inline bool parse_obj_value_or_empty(string_view& str, T& value) {
  skip_whitespace(str);
  if (str.empty()) {
    value = T{};
    return true;
  } else {
    return parse_obj_value(str, value);
  }
}

static inline bool parse_obj_value(string_view& str, obj_vertex& value) {
  value = obj_vertex{0, 0, 0};
  if (!parse_obj_value(str, value.position)) return false;
  if (!str.empty() && str.front() == '/') {
    str.remove_prefix(1);
    if (!str.empty() && str.front() == '/') {
      str.remove_prefix(1);
      if (!parse_obj_value(str, value.normal)) return false;
    } else {
      if (!parse_obj_value(str, value.texcoord)) return false;
      if (!str.empty() && str.front() == '/') {
        str.remove_prefix(1);
        if (!parse_obj_value(str, value.normal)) return false;
      }
    }
  }
  return true;
}

// Input for OBJ textures
static inline bool parse_obj_value(string_view& str, obj_texture_info& info) {
  // initialize
  info = obj_texture_info();

  // get tokens
  auto tokens = vector<string>();
  skip_whitespace(str);
  while (!str.empty()) {
    auto token = ""s;
    if (!parse_obj_value(str, token)) return false;
    tokens.push_back(token);
    skip_whitespace(str);
  }
  if (tokens.empty()) throw std::runtime_error("cannot parse value");

  // texture name
  info.path = fs::path(tokens.back()).generic_string();

  // texture params
  auto last = string();
  for (auto i = 0; i < tokens.size() - 1; i++) {
    if (tokens[i] == "-bm") info.scale = atof(tokens[i + 1].c_str());
    if (tokens[i] == "-clamp") info.clamp = true;
  }

  return true;
}

static inline bool parse_obj_value(string_view& str, obj_value& value,
    obj_value_type type, int array_size = 3) {
  value.type = type;
  switch (type) {
    case obj_value_type::num: {
      return parse_obj_value(str, value.num);
    } break;
    case obj_value_type::str: {
      return parse_obj_value(str, value.str);
    } break;
    case obj_value_type::vec2: {
      return parse_obj_value(str, value.vec2);
    } break;
    case obj_value_type::vec3: {
      return parse_obj_value(str, value.vec3);
    } break;
    case obj_value_type::frame3: {
      return parse_obj_value(str, value.frame3);
    } break;
    case obj_value_type::bol: {
      auto ivalue = 0;
      if (!parse_obj_value(str, ivalue)) return false;
      value.bol = (bool)ivalue;
      return true;
    } break;
  }
}

static inline bool parse_obj_value_or_empty(
    string_view& str, obj_value& value) {
  skip_whitespace(str);
  if (str.empty()) {
    value.type = obj_value_type::str;
    value.str = ""s;
    return true;
  } else {
    return parse_obj_value(str, value, obj_value_type::str);
  }
}

// Read obj
bool read_obj_command(file_wrapper& fs, obj_command& command, obj_value& value,
    vector<obj_vertex>& vertices, obj_vertex& vert_size, bool& error) {
  // set error
  auto set_error = [&error]() {
    error = true;
    return false;
  };
  // read the file line by line
  char buffer[4096];
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_obj_comment(line);
    skip_whitespace(line);
    if (line.empty()) continue;

    // get command
    auto cmd = ""s;
    if (!parse_obj_value(line, cmd)) return set_error();
    if (cmd == "") continue;

    // possible token values
    if (cmd == "v") {
      command = obj_command::vertex;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
      vert_size.position += 1;
      return true;
    } else if (cmd == "vn") {
      command = obj_command::normal;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
      vert_size.normal += 1;
      return true;
    } else if (cmd == "vt") {
      command = obj_command::texcoord;
      if (!parse_obj_value(line, value, obj_value_type::vec2))
        return set_error();
      vert_size.texcoord += 1;
      return true;
    } else if (cmd == "f" || cmd == "l" || cmd == "p") {
      vertices.clear();
      skip_whitespace(line);
      while (!line.empty()) {
        auto vert = obj_vertex{};
        if (!parse_obj_value(line, vert)) return set_error();
        if (!vert.position) break;
        if (vert.position < 0)
          vert.position = vert_size.position + vert.position + 1;
        if (vert.texcoord < 0)
          vert.texcoord = vert_size.texcoord + vert.texcoord + 1;
        if (vert.normal < 0) vert.normal = vert_size.normal + vert.normal + 1;
        vertices.push_back(vert);
        skip_whitespace(line);
      }
      if (cmd == "f") command = obj_command::face;
      if (cmd == "l") command = obj_command::line;
      if (cmd == "p") command = obj_command::point;
      return true;
    } else if (cmd == "o") {
      command = obj_command::object;
      if (!parse_obj_value_or_empty(line, value)) return set_error();
      return true;
    } else if (cmd == "usemtl") {
      command = obj_command::usemtl;
      if (!parse_obj_value_or_empty(line, value)) return set_error();
      return true;
    } else if (cmd == "g") {
      command = obj_command::group;
      if (!parse_obj_value_or_empty(line, value)) return set_error();
      return true;
    } else if (cmd == "s") {
      command = obj_command::smoothing;
      if (!parse_obj_value_or_empty(line, value)) return set_error();
      return true;
    } else if (cmd == "mtllib") {
      command = obj_command::mtllib;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else {
      // unused
    }
  }
  return false;
}

// Read mtl
bool read_mtl_command(file_wrapper& fs, mtl_command& command, obj_value& value,
    obj_texture_info& texture, bool& error, bool fliptr) {
  // set error
  auto set_error = [&error]() {
    error = true;
    return false;
  };
  // read the file line by line
  char buffer[4096];
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_obj_comment(line);
    skip_whitespace(line);
    if (line.empty()) continue;

    // get command
    auto cmd = ""s;
    if (!parse_obj_value(line, cmd)) return set_error();
    if (cmd == "") continue;

    // possible token values
    if (cmd == "newmtl") {
      command = mtl_command::material;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
    } else if (cmd == "illum") {
      command = mtl_command::illum;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "Ke") {
      command = mtl_command::emission;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Kd") {
      command = mtl_command::diffuse;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Ks") {
      command = mtl_command::specular;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Kt") {
      command = mtl_command::transmission;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Tf") {
      command    = mtl_command::transmission;
      value.vec3 = vec3f{-1};
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
      if (value.vec3.y < 0) value.vec3 = vec3f{value.vec3.x};
      if (fliptr) value.vec3 = 1 - value.vec3;
    } else if (cmd == "Tr") {
      command = mtl_command::opacity;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
      if (fliptr) value.num = 1 - value.num;
    } else if (cmd == "Ns") {
      command = mtl_command::exponent;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "d") {
      command = mtl_command::opacity;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "map_Ke") {
      command = mtl_command::emission_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Kd") {
      command = mtl_command::diffuse_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Ks") {
      command = mtl_command::specular_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Tr") {
      command = mtl_command::transmission_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_d" || cmd == "map_Tr") {
      command = mtl_command::opacity_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_bump" || cmd == "bump") {
      command = mtl_command::bump_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_disp" || cmd == "disp") {
      command = mtl_command::displacement_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_norm" || cmd == "norm") {
      command = mtl_command::normal_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "Pm") {
      command = mtl_command::pbr_metallic;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "Pr") {
      command = mtl_command::pbr_roughness;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "Ps") {
      command = mtl_command::pbr_sheen;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "Pc") {
      command = mtl_command::pbr_clearcoat;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "Pcr") {
      command = mtl_command::pbr_coatroughness;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "map_Pm") {
      command = mtl_command::pbr_metallic_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Pr") {
      command = mtl_command::pbr_roughness_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Ps") {
      command = mtl_command::pbr_sheen_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Pc") {
      command = mtl_command::pbr_clearcoat_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "map_Pcr") {
      command = mtl_command::pbr_coatroughness_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else if (cmd == "Vt") {
      command = mtl_command::vol_transmission;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Vp") {
      command = mtl_command::vol_meanfreepath;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Ve") {
      command = mtl_command::vol_emission;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Vs") {
      command = mtl_command::vol_scattering;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
    } else if (cmd == "Vg") {
      command = mtl_command::vol_anisotropy;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "Vr") {
      command = mtl_command::vol_scale;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
    } else if (cmd == "map_Vs") {
      command = mtl_command::vol_scattering_map;
      if (!parse_obj_value(line, texture)) return set_error();
    } else {
      continue;
    }
    return true;
  }

  return false;
}

// Read objx
bool read_objx_command(file_wrapper& fs, objx_command& command,
    obj_value& value, obj_texture_info& texture, bool& error) {
  // set error
  auto set_error = [&error]() {
    error = true;
    return false;
  };
  // read the file line by line
  char buffer[4096];
  auto pos = ftell(fs.fs);
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_obj_comment(line);
    skip_whitespace(line);
    if (line.empty()) continue;

    // get command
    auto cmd = ""s;
    if (!parse_obj_value(line, cmd)) return set_error();
    if (cmd == "") continue;

    // read values
    if (cmd == "newcam") {
      command = objx_command::camera;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else if (cmd == "newenv") {
      command = objx_command::environment;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else if (cmd == "newist") {
      command = objx_command::instance;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else if (cmd == "newproc") {
      command = objx_command::procedural;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else if (cmd == "frame") {
      command = objx_command::frame;
      if (!parse_obj_value(line, value, obj_value_type::frame3))
        return set_error();
      return true;
    } else if (cmd == "obj") {
      command = objx_command::object;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else if (cmd == "mat") {
      command = objx_command::material;
      if (!parse_obj_value(line, value, obj_value_type::str))
        return set_error();
      return true;
    } else if (cmd == "ortho") {
      command = objx_command::ortho;
      if (!parse_obj_value(line, value, obj_value_type::bol))
        return set_error();
      return true;
    } else if (cmd == "width") {
      command = objx_command::width;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
      return true;
    } else if (cmd == "height") {
      command = objx_command::height;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
      return true;
    } else if (cmd == "lens") {
      command = objx_command::lens;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
      return true;
    } else if (cmd == "aperture") {
      command = objx_command::aperture;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
      return true;
    } else if (cmd == "focus") {
      command = objx_command::focus;
      if (!parse_obj_value(line, value, obj_value_type::num))
        return set_error();
      return true;
    } else if (cmd == "Ke") {
      command = objx_command::emission;
      if (!parse_obj_value(line, value, obj_value_type::vec3))
        return set_error();
      return true;
    } else if (cmd == "map_Ke") {
      command = objx_command::emission_map;
      if (!parse_obj_value(line, texture)) return set_error();
      return true;
    }
    // backward compatibility
    else if (cmd == "c") {
      auto oname = value.str;
      auto name = obj_value{}, ortho = obj_value{}, width = obj_value{},
           height = obj_value{}, lens = obj_value{}, aperture = obj_value{},
           focus = obj_value{}, frame = obj_value{};
      if (!parse_obj_value(line, name, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, ortho, obj_value_type::bol))
        return set_error();
      if (!parse_obj_value(line, width, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, height, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, lens, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, focus, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, aperture, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, frame, obj_value_type::frame3))
        return set_error();
      if (command == objx_command::camera && oname != "") {
        command = objx_command::ortho;
        value   = ortho;
      } else if (command == objx_command::ortho) {
        command = objx_command::width;
        value   = width;
      } else if (command == objx_command::width) {
        command = objx_command::height;
        value   = height;
      } else if (command == objx_command::height) {
        command = objx_command::lens;
        value   = lens;
      } else if (command == objx_command::lens) {
        command = objx_command::focus;
        value   = focus;
      } else if (command == objx_command::focus) {
        command = objx_command::aperture;
        value   = aperture;
      } else if (command == objx_command::aperture) {
        command = objx_command::frame;
        value   = frame;
      } else {
        command = objx_command::camera;
        value   = name;
      }
      if (command != objx_command::frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else if (cmd == "e") {
      auto name = obj_value{}, frame = obj_value{}, emission = obj_value{},
           emission_map = obj_value{};
      if (!parse_obj_value(line, name, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, emission, obj_value_type::vec3))
        return set_error();
      if (!parse_obj_value(line, emission_map, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, frame, obj_value_type::frame3))
        return set_error();
      if (emission_map.str == "\"\"") emission_map.str = "";
      if (command == objx_command::environment) {
        command = objx_command::emission;
        value   = emission;
      } else if (command == objx_command::emission) {
        command = objx_command::emission_map;
        texture.path = emission_map.str;
      } else if (command == objx_command::emission_map) {
        command = objx_command::frame;
        value   = frame;
      } else {
        command = objx_command::environment;
        value   = name;
      }
      if (command != objx_command::frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else if (cmd == "i") {
      auto name = obj_value{}, frame = obj_value{}, object = obj_value{},
           material = obj_value{};
      if (!parse_obj_value(line, name, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, object, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, material, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, frame, obj_value_type::frame3))
        return set_error();
      if (command == objx_command::instance) {
        command = objx_command::object;
        value   = object;
      } else if (command == objx_command::object) {
        command = objx_command::material;
        value   = material;
      } else if (command == objx_command::material) {
        command = objx_command::frame;
        value   = frame;
      } else {
        command = objx_command::instance;
        value   = name;
      }
      if (command != objx_command::frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else if (cmd == "po") {
      auto name = obj_value{}, frame = obj_value{}, type = obj_value{},
           material = obj_value{}, size = obj_value{}, level = obj_value{};
      if (!parse_obj_value(line, name, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, type, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, material, obj_value_type::str))
        return set_error();
      if (!parse_obj_value(line, size, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, level, obj_value_type::num))
        return set_error();
      if (!parse_obj_value(line, frame, obj_value_type::frame3))
        return set_error();
      if (command == objx_command::procedural) {
        command = objx_command::object;
        value   = type;
      } else if (command == objx_command::object) {
        command = objx_command::material;
        value   = material;
      } else if (command == objx_command::material) {
        command = objx_command::frame;
        value   = frame;
      } else {
        command = objx_command::procedural;
        value   = name;
      }
      if (command != objx_command::frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else {
      // unused
    }
  }

  return false;
}

// Write obj elements
bool write_obj_comment(file_wrapper& fs, const string& comment) {
  auto lines = split_string(comment, "\n");
  for (auto& line : lines) {
    if (fprintf(fs.fs, "# %s\n", line.c_str()) < 0) return false;
  }
  if (fprintf(fs.fs, "\n") < 0) return false;
  return true;
}

bool write_obj_command(file_wrapper& fs, obj_command command,
    const obj_value& value, const vector<obj_vertex>& vertices) {
  switch (command) {
    case obj_command::vertex:
      return fprintf(fs.fs, "v %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >= 0;
    case obj_command::normal:
      return fprintf(fs.fs, "vn %g  %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case obj_command::texcoord:
      return fprintf(fs.fs, "vt %g %g\n", value.vec2.x, value.vec2.x) >= 0;
    case obj_command::face:
    case obj_command::line:
    case obj_command::point:
      if (command == obj_command::face)
        if (fprintf(fs.fs, "f ") < 0) return false;
      if (command == obj_command::line)
        if (fprintf(fs.fs, "l ") < 0) return false;
      if (command == obj_command::point)
        if (fprintf(fs.fs, "p ") < 0) return false;
      for (auto& vert : vertices) {
        if (fprintf(fs.fs, " ") < 0) return false;
        if (fprintf(fs.fs, "%d", vert.position) < 0) return false;
        if (vert.texcoord) {
          if (fprintf(fs.fs, "/%d", vert.texcoord) < 0) return false;
          if (vert.normal) {
            if (fprintf(fs.fs, "/%d", vert.normal) < 0) return false;
          }
        } else if (vert.normal) {
          if (fprintf(fs.fs, "//%d", vert.normal) < 0) return false;
        }
      }
      if (fprintf(fs.fs, "\n") < 0) return false;
      return true;
    case obj_command::object:
      return fprintf(fs.fs, "o %s\n", value.str.c_str()) >= 0;
    case obj_command::group: return fprintf(fs.fs, "g %s\n", value.str.c_str()) >= 0;
    case obj_command::usemtl:
      return fprintf(fs.fs, "usemtl %s\n", value.str.c_str()) >= 0;
    case obj_command::smoothing:
      return fprintf(fs.fs, "s %s\n", value.str.c_str()) >= 0;
    case obj_command::mtllib:
      return fprintf(fs.fs, "mtllib %s\n", value.str.c_str()) >= 0;
    case obj_command::objxlib: return true;
  }
}

bool write_mtl_command(file_wrapper& fs, mtl_command command,
    const obj_value& value, const obj_texture_info& texture) {
  switch (command) {
    case mtl_command::material:
      return fprintf(fs.fs, "\nnewmtl %s\n", value.str.c_str()) >= 0;
    case mtl_command::illum:
      return fprintf(fs.fs, "  illum %d\n", (int)value.num) >= 0;
    case mtl_command::emission:
      return fprintf(fs.fs, "  Ke %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::ambient:
      return fprintf(fs.fs, "  Ka %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::diffuse:
      return fprintf(fs.fs, "  Kd %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::specular:
      return fprintf(fs.fs, "  Ks %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::reflection:
      return fprintf(fs.fs, "  Kr %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::transmission:
      return fprintf(fs.fs, "  Kt %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::exponent:
      return fprintf(fs.fs, "  Ns %d\n", (int)value.num) >= 0;
    case mtl_command::opacity: return fprintf(fs.fs, "  d %g\n", value.num) >= 0;
    case mtl_command::ior: return fprintf(fs.fs, "  Ni %g\n", value.num) >= 0;
    case mtl_command::emission_map:
      return fprintf(fs.fs, "  map_Ke %s\n", texture.path.c_str()) >= 0;
    case mtl_command::ambient_map:
      return fprintf(fs.fs, "  map_Ka %s\n", texture.path.c_str()) >= 0;
    case mtl_command::diffuse_map:
      return fprintf(fs.fs, "  map_Kd %s\n", texture.path.c_str()) >= 0;
    case mtl_command::specular_map:
      return fprintf(fs.fs, "  map_Ks %s\n", texture.path.c_str()) >= 0;
    case mtl_command::reflection_map:
      return fprintf(fs.fs, "  map_Kr %s\n", texture.path.c_str()) >= 0;
    case mtl_command::transmission_map:
      return fprintf(fs.fs, "  map_Kt %s\n", texture.path.c_str()) >= 0;
    case mtl_command::opacity_map:
      return fprintf(fs.fs, "  map_d %s\n", texture.path.c_str()) >= 0;
    case mtl_command::exponent_map:
      return fprintf(fs.fs, "  map_Ni %s\n", texture.path.c_str()) >= 0;
    case mtl_command::bump_map:
      return fprintf(fs.fs, "  map_bump %s\n", texture.path.c_str()) >= 0;
    case mtl_command::normal_map:
      return fprintf(fs.fs, "  map_norm %s\n", texture.path.c_str()) >= 0;
    case mtl_command::displacement_map:
      return fprintf(fs.fs, "  map_disp %s\n", texture.path.c_str()) >= 0;
    case mtl_command::pbr_roughness:
      return fprintf(fs.fs, "  Pr %g\n", value.num) >= 0;
    case mtl_command::pbr_metallic:
      return fprintf(fs.fs, "  Pm %g\n", value.num) >= 0;
    case mtl_command::pbr_sheen: return fprintf(fs.fs, "  Ps %g\n", value.num) >= 0;
    case mtl_command::pbr_clearcoat:
      return fprintf(fs.fs, "  Pc %g\n", value.num) >= 0;
    case mtl_command::pbr_coatroughness:
      return fprintf(fs.fs, "  Pcr %g\n", value.num) >= 0;
    case mtl_command::pbr_roughness_map:
      return fprintf(fs.fs, "  Pr_map %s\n", texture.path.c_str()) >= 0;
    case mtl_command::pbr_metallic_map:
      return fprintf(fs.fs, "  Pm_map %s\n", texture.path.c_str()) >= 0;
    case mtl_command::pbr_sheen_map:
      return fprintf(fs.fs, "  Ps_map %s\n", texture.path.c_str()) >= 0;
    case mtl_command::pbr_clearcoat_map:
      return fprintf(fs.fs, "  Pc_map %s\n", texture.path.c_str()) >= 0;
    case mtl_command::pbr_coatroughness_map:
      return fprintf(fs.fs, "  Pcr_map %s\n", texture.path.c_str()) >= 0;
    case mtl_command::vol_transmission:
      return fprintf(fs.fs, "  Vt %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::vol_meanfreepath:
      return fprintf(fs.fs, "  Vp %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::vol_emission:
      return fprintf(fs.fs, "  Ve %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::vol_scattering:
      return fprintf(fs.fs, "  Vs %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case mtl_command::vol_anisotropy:
      return fprintf(fs.fs, "  Vg %g\n", value.num) >= 0;
    case mtl_command::vol_scale: return fprintf(fs.fs, "  Vr %g\n", value.num) >= 0;
    case mtl_command::vol_scattering_map:
      return fprintf(fs.fs, "  Vs_map %s\n", texture.path.c_str()) >= 0;
  }
}

bool write_objx_command(file_wrapper& fs, objx_command command,
    const obj_value& value, const obj_texture_info& texture) {
  switch (command) {
    case objx_command::camera:
      return fprintf(fs.fs, "\nnewcam %s\n", value.str.c_str()) >= 0;
    case objx_command::environment:
      return fprintf(fs.fs, "\nnewenv %s\n", value.str.c_str()) >= 0;
    case objx_command::instance:
      return fprintf(fs.fs, "\nnewist %s\n", value.str.c_str()) >= 0;
    case objx_command::procedural:
      return fprintf(fs.fs, "\nnewproc %s\n", value.str.c_str()) >= 0;
    case objx_command::frame: {
      auto frame = &value.frame3.x.x;
      return fprintf(fs.fs, "  frame %g %g %g %g %g %g %g %g %g %g %g %g\n",
                 frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                 frame[6], frame[7], frame[8], frame[9], frame[10],
                 frame[11]) >= 0;
    } break;
    case objx_command::object:
      return fprintf(fs.fs, "  obj %s\n", value.str.c_str()) >= 0;
    case objx_command::material:
      return fprintf(fs.fs, "  mat %s\n", value.str.c_str()) >= 0;
    case objx_command::ortho: return fprintf(fs.fs, "  ortho %g\n", value.num) >= 0;
    case objx_command::width: return fprintf(fs.fs, "  width %g\n", value.num) >= 0;
    case objx_command::height:
      return fprintf(fs.fs, "  height %g\n", value.num) >= 0;
    case objx_command::lens: return fprintf(fs.fs, "  lens %g\n", value.num) >= 0;
    case objx_command::aperture:
      return fprintf(fs.fs, "  aperture %g\n", value.num) >= 0;
    case objx_command::focus: return fprintf(fs.fs, "  focus %g\n", value.num) >= 0;
    case objx_command::emission:
      return fprintf(fs.fs, "  Ke %g %g %g\n", value.vec3.x, value.vec3.y, value.vec3.z) >=
             0;
    case objx_command::emission_map:
      return fprintf(fs.fs, "  map_Ke %s\n", texture.path.c_str()) >= 0;
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// YAML SUPPORT
// -----------------------------------------------------------------------------
namespace yocto {

static inline void remove_yaml_comment(
    string_view& str, char comment_char = '#') {
  while (!str.empty() && is_newline(str.back())) str.remove_suffix(1);
  auto cpy = str;
  while (!cpy.empty() && cpy.front() != comment_char) cpy.remove_prefix(1);
  str.remove_suffix(cpy.size());
}

static inline bool parse_yaml_varname(string_view& str, string_view& value) {
  skip_whitespace(str);
  if (str.empty()) return false;
  if (!is_alpha(str.front())) return false;
  auto pos = 0;
  while (is_alpha(str[pos]) || str[pos] == '_' || is_digit(str[pos])) {
    pos += 1;
    if (pos >= str.size()) break;
  }
  value = str.substr(0, pos);
  str.remove_prefix(pos);
  return true;
}
static inline bool parse_yaml_varname(string_view& str, string& value) {
  auto view = ""sv;
  if (!parse_yaml_varname(str, view)) return false;
  value = string{view};
  return true;
}

static inline bool parse_yaml_value(string_view& str, string_view& value) {
  skip_whitespace(str);
  if (str.empty()) return false;
  if (str.front() != '"') {
    auto cpy = str;
    while (!cpy.empty() && !is_space(cpy.front())) cpy.remove_prefix(1);
    value = str;
    value.remove_suffix(cpy.size());
    str.remove_prefix(str.size() - cpy.size());
    return true;
  } else {
    if (str.front() != '"') return false;
    str.remove_prefix(1);
    if (str.empty()) return false;
    auto cpy = str;
    while (!cpy.empty() && cpy.front() != '"') cpy.remove_prefix(1);
    if (cpy.empty()) return false;
    value = str;
    value.remove_suffix(cpy.size());
    str.remove_prefix(str.size() - cpy.size());
    str.remove_prefix(1);
    return true;
  }
}
static inline bool parse_yaml_value(string_view& str, string& value) {
  auto valuev = ""sv;
  if (!parse_yaml_value(str, valuev)) return false;
  value = string{valuev};
  return true;
}
static inline bool parse_yaml_value(string_view& str, double& value) {
  skip_whitespace(str);
  char* end = nullptr;
  value     = strtod(str.data(), &end);
  if (str == end) return false;
  str.remove_prefix(end - str.data());
  return true;
}

// parse yaml value
bool get_yaml_value(const yaml_value& yaml, string& value) {
  if (yaml.type != yaml_value_type::string) return false;
  value = yaml.string_;
  return true;
}
bool get_yaml_value(const yaml_value& yaml, bool& value) {
  if (yaml.type != yaml_value_type::boolean) return false;
  value = yaml.boolean;
  return true;
}
bool get_yaml_value(const yaml_value& yaml, int& value) {
  if (yaml.type != yaml_value_type::number) return false;
  value = (int)yaml.number;
  return true;
}
bool get_yaml_value(const yaml_value& yaml, float& value) {
  if (yaml.type != yaml_value_type::number) return false;
  value = (float)yaml.number;
  return true;
}
bool get_yaml_value(const yaml_value& yaml, vec2f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 2) return false;
  value = {(float)yaml.array_[0], (float)yaml.array_[1]};
  return true;
}
bool get_yaml_value(const yaml_value& yaml, vec3f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 3) return false;
  value = {(float)yaml.array_[0], (float)yaml.array_[1], (float)yaml.array_[2]};
  return true;
}
bool get_yaml_value(const yaml_value& yaml, mat3f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 9) return false;
  for (auto i = 0; i < 9; i++) (&value.x.x)[i] = (float)yaml.array_[i];
  return true;
}
bool get_yaml_value(const yaml_value& yaml, frame3f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 12) return false;
  for (auto i = 0; i < 12; i++) (&value.x.x)[i] = (float)yaml.array_[i];
  return true;
}

// construction
yaml_value make_yaml_value(const string& value) {
  return {yaml_value_type::string, 0, false, value};
}
yaml_value make_yaml_value(bool value) {
  return {yaml_value_type::boolean, 0, value};
}
yaml_value make_yaml_value(int value) {
  return {yaml_value_type::number, (double)value};
}
yaml_value make_yaml_value(float value) {
  return {yaml_value_type::number, (double)value};
}
yaml_value make_yaml_value(const vec2f& value) {
  return {
      yaml_value_type::array, 2, false, "", {(double)value.x, (double)value.y}};
}
yaml_value make_yaml_value(const vec3f& value) {
  return {yaml_value_type::array, 3, false, "",
      {(double)value.x, (double)value.y, (double)value.z}};
}
yaml_value make_yaml_value(const mat3f& value) {
  auto yaml = yaml_value{yaml_value_type::array, 9};
  for (auto i = 0; i < 9; i++) yaml.array_[i] = (double)(&value.x.x)[i];
  return yaml;
}
yaml_value make_yaml_value(const frame3f& value) {
  auto yaml = yaml_value{yaml_value_type::array, 12};
  for (auto i = 0; i < 12; i++) yaml.array_[i] = (double)(&value.x.x)[i];
  return yaml;
}

static bool parse_yaml_value(string_view& str, yaml_value& value) {
  trim_whitespace(str);
  if (str.empty()) return false;
  if (str.front() == '[') {
    str.remove_prefix(1);
    value.type   = yaml_value_type::array;
    value.number = 0;
    while (!str.empty()) {
      skip_whitespace(str);
      if (str.empty()) return false;
      if (str.front() == ']') {
        str.remove_prefix(1);
        break;
      }
      if (value.number >= 16) return false;
      parse_yaml_value(str, value.array_[(int)value.number]);
      value.number += 1;
      skip_whitespace(str);
      if (str.front() == ',') {
        str.remove_prefix(1);
        continue;
      } else if (str.front() == ']') {
        str.remove_prefix(1);
        break;
      } else {
        return false;
      }
    }
  } else if (is_digit(str.front()) || str.front() == '-' ||
             str.front() == '+') {
    value.type = yaml_value_type::number;
    return parse_yaml_value(str, value.number);
  } else {
    value.type = yaml_value_type::string;
    if (!parse_yaml_value(str, value.string_)) return false;
    if (value.string_ == "true" || value.string_ == "false") {
      value.type    = yaml_value_type::boolean;
      value.boolean = value.string_ == "true";
    }
    return true;
  }
  skip_whitespace(str);
  if (!str.empty() && !is_whitespace(str)) return false;
  return true;
}

bool read_yaml_property(file_wrapper& fs, string& group, string& key,
    bool& newobj, yaml_value& value, bool& error) {
  // set error
  auto set_error = [&error]() {
    error = true;
    return false;
  };
  // read the file line by line
  char buffer[4096];
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_yaml_comment(line);
    if (line.empty()) continue;
    if (is_whitespace(line)) continue;

    // peek commands
    if (is_space(line.front())) {
      // indented property
      if (group == "") return set_error();
      skip_whitespace(line);
      if (line.empty()) return set_error();
      if (line.front() == '-') {
        newobj = true;
        line.remove_prefix(1);
        skip_whitespace(line);
      } else {
        newobj = false;
      }
      if (!parse_yaml_varname(line, key)) return set_error();
      skip_whitespace(line);
      if (line.empty() || line.front() != ':') return set_error();
      line.remove_prefix(1);
      if (!parse_yaml_value(line, value)) return set_error();
      return true;
    } else if (is_alpha(line.front())) {
      // new group
      if (!parse_yaml_varname(line, key)) return set_error();
      skip_whitespace(line);
      if (line.empty() || line.front() != ':') return set_error();
      line.remove_prefix(1);
      if (!line.empty() && !is_whitespace(line)) {
        group = "";
        if (!parse_yaml_value(line, value)) return set_error();
        return true;
      } else {
        group = key;
        key   = "";
        return true;
      }
    } else {
      return set_error();
    }
  }
  return false;
}

bool write_yaml_comment(file_wrapper& fs, const string& comment) {
  auto lines = split_string(comment, "\n");
  for (auto& line : lines) {
    if (fprintf(fs.fs, "# %s\n", line.c_str()) < 0) return false;
  }
  if (fprintf(fs.fs, "\n") < 0) return false;
  return true;
}

// Save yaml property
bool write_yaml_property(file_wrapper& fs, const string& object,
    const string& key, bool newobj, const yaml_value& value) {
  if (key.empty()) {
    if (fprintf(fs.fs, "\n%s:\n", object.c_str()) < 0) return false;
    return true;
  } else {
    if (!object.empty()) {
      if (fprintf(fs.fs, (newobj ? "  - " : "    ")) < 0) return false;
    }
    if (fprintf(fs.fs, "%s: ", key.c_str()) < 0) return false;
    switch (value.type) {
      case yaml_value_type::number:
        if (fprintf(fs.fs, "%g", value.number) < 0) return false;
        break;
      case yaml_value_type::boolean:
        if (fprintf(fs.fs, "%s", value.boolean ? "true" : "false") < 0)
          return false;
        break;
      case yaml_value_type::string:
        if (fprintf(fs.fs, "%s", value.string_.c_str()) < 0) return false;
        break;
      case yaml_value_type::array:
        if (fprintf(fs.fs, "[ ") < 0) return false;
        for (auto i = 0; i < value.number; i++) {
          if (i)
            if (fprintf(fs.fs, ", ") < 0) return false;
          if (fprintf(fs.fs, "%g", value.array_[i]) < 0) return false;
        }
        if (fprintf(fs.fs, " ]") < 0) return false;
        break;
    }
    if (fprintf(fs.fs, "\n") < 0) return false;
    return true;
  }
}

bool write_yaml_object(file_wrapper& fs, const string& object) {
  return fprintf(fs.fs, "\n%s:\n", object.c_str()) > 0;
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// PBRT CONVERSION
// -----------------------------------------------------------------------------
namespace yocto {

static inline void remove_pbrt_comment(
    string_view& str, char comment_char = '#') {
  while (!str.empty() && is_newline(str.back())) str.remove_suffix(1);
  auto cpy       = str;
  auto in_string = false;
  while (!cpy.empty()) {
    if (cpy.front() == '"') in_string = !in_string;
    if (cpy.front() == comment_char && !in_string) break;
    cpy.remove_prefix(1);
  }
  str.remove_suffix(cpy.size());
}

// Read a pbrt command from file
bool read_pbrt_cmdline(file_wrapper& fs, string& cmd) {
  char buffer[4096];
  cmd.clear();
  auto found = false;
  auto pos   = ftell(fs.fs);
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_pbrt_comment(line);
    skip_whitespace(line);
    if (line.empty()) continue;

    // check if command
    auto is_cmd = line[0] >= 'A' && line[0] <= 'Z';
    if (is_cmd) {
      if (found) {
        fseek(fs.fs, pos, SEEK_SET);
        return true;
      } else {
        found = true;
      }
    } else if (!found) {
      return false;
    }
    cmd += line;
    cmd += " ";
    pos = ftell(fs.fs);
  }
  return found;
}

// parse a quoted string
static inline bool parse_pbrt_value(string_view& str, string_view& value) {
  skip_whitespace(str);
  if (str.front() != '"') return false;
  str.remove_prefix(1);
  if (str.empty()) return false;
  auto cpy = str;
  while (!cpy.empty() && cpy.front() != '"') cpy.remove_prefix(1);
  if (cpy.empty()) return false;
  value = str;
  value.remove_suffix(cpy.size());
  str.remove_prefix(str.size() - cpy.size());
  str.remove_prefix(1);
  return true;
}

static inline bool parse_pbrt_value(string_view& str, string& value) {
  auto view = ""sv;
  parse_pbrt_value(str, view);
  value = string{view};
  return true;
}

// parse a quoted string
static inline bool parse_pbrt_command(string_view& str, string& value) {
  skip_whitespace(str);
  if (!isalpha((int)str.front()))
    return false;
  auto pos = str.find_first_not_of(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  if (pos == string_view::npos) {
    value.assign(str);
    str.remove_prefix(str.size());
  } else {
    value.assign(str.substr(0, pos));
    str.remove_prefix(pos + 1);
  }
  return true;
}

// parse a number
static inline bool parse_pbrt_value(string_view& str, float& value) {
  skip_whitespace(str);
  if (str.empty()) return false;
  auto next = (char*)nullptr;
  value     = strtof(str.data(), &next);
  if (str.data() == next) return false;
  str.remove_prefix(next - str.data());
  return true;
}

// parse a number
static inline bool parse_pbrt_value(string_view& str, int& value) {
  skip_whitespace(str);
  if (str.empty()) return false;
  auto next = (char*)nullptr;
  value     = strtol(str.data(), &next, 10);
  if (str.data() == next) return false;
  str.remove_prefix(next - str.data());
  return true;
}
template <typename T>
static inline bool parse_pbrt_value(
    string_view& str, T& value, unordered_map<string, T>& value_names) {
  auto value_name = ""s;
  parse_pbrt_value(str, value_name);
  if(value_names.find(value_name) == value_names.end()) return false;
  value = value_names.at(value_name);
  return true;
}

// parse a vec type
static inline bool parse_pbrt_value(string_view& str, vec2f& value) {
  for (auto i = 0; i < 2; i++) if(!parse_pbrt_value(str, value[i])) return false;
  return true;
}
static inline bool parse_pbrt_value(string_view& str, vec3f& value) {
  for (auto i = 0; i < 3; i++) if(!parse_pbrt_value(str, value[i])) return false;
  return true;
}
static inline bool parse_pbrt_value(string_view& str, vec4f& value) {
  for (auto i = 0; i < 4; i++) if(!parse_pbrt_value(str, value[i])) return false;
  return true;
}
static inline bool parse_pbrt_value(string_view& str, mat4f& value) {
  for (auto i = 0; i < 4; i++) if(!parse_pbrt_value(str, value[i])) return false;
  return true;
}

// parse pbrt value with optional parens
template <typename T>
static inline bool parse_pbrt_param(string_view& str, T& value) {
  skip_whitespace(str);
  auto parens = !str.empty() && str.front() == '[';
  if (parens) str.remove_prefix(1);
  if(!parse_pbrt_value(str, value)) return false;
  if (parens) {
    skip_whitespace(str);
    if (!str.empty() && str.front() == '[') return false;
    str.remove_prefix(1);
  }
  return true;
}

// parse a quoted string
static inline bool parse_pbrt_nametype(
    string_view& str_, string& name, string& type) {
  auto value = ""s;
  if(!parse_pbrt_value(str_, value)) return false;
  auto str  = string_view{value};
  auto pos1 = str.find(' ');
  if (pos1 == string_view::npos) return false;
  type = string(str.substr(0, pos1));
  str.remove_prefix(pos1);
  auto pos2 = str.find_first_not_of(' ');
  if (pos2 == string_view::npos) return false;
  str.remove_prefix(pos2);
  name = string(str);
  return true;
}

static inline pair<vec3f, vec3f> get_pbrt_etak(const string& name) {
  static const unordered_map<string, pair<vec3f, vec3f>> metal_ior_table = {
      {"a-C", {{2.9440999183f, 2.2271502925f, 1.9681668794f},
                  {0.8874329109f, 0.7993216383f, 0.8152862927f}}},
      {"Ag", {{0.1552646489f, 0.1167232965f, 0.1383806959f},
                 {4.8283433224f, 3.1222459278f, 2.1469504455f}}},
      {"Al", {{1.6574599595f, 0.8803689579f, 0.5212287346f},
                 {9.2238691996f, 6.2695232477f, 4.8370012281f}}},
      {"AlAs", {{3.6051023902f, 3.2329365777f, 2.2175611545f},
                   {0.0006670247f, -0.0004999400f, 0.0074261204f}}},
      {"AlSb", {{-0.0485225705f, 4.1427547893f, 4.6697691348f},
                   {-0.0363741915f, 0.0937665154f, 1.3007390124f}}},
      {"Au", {{0.1431189557f, 0.3749570432f, 1.4424785571f},
                 {3.9831604247f, 2.3857207478f, 1.6032152899f}}},
      {"Be", {{4.1850592788f, 3.1850604423f, 2.7840913457f},
                 {3.8354398268f, 3.0101260162f, 2.8690088743f}}},
      {"Cr", {{4.3696828663f, 2.9167024892f, 1.6547005413f},
                 {5.2064337956f, 4.2313645277f, 3.7549467933f}}},
      {"CsI", {{2.1449030413f, 1.7023164587f, 1.6624194173f},
                  {0.0000000000f, 0.0000000000f, 0.0000000000f}}},
      {"Cu", {{0.2004376970f, 0.9240334304f, 1.1022119527f},
                 {3.9129485033f, 2.4528477015f, 2.1421879552f}}},
      {"Cu2O", {{3.5492833755f, 2.9520622449f, 2.7369202137f},
                   {0.1132179294f, 0.1946659670f, 0.6001681264f}}},
      {"CuO", {{3.2453822204f, 2.4496293965f, 2.1974114493f},
                  {0.5202739621f, 0.5707372756f, 0.7172250613f}}},
      {"d-C", {{2.7112524747f, 2.3185812849f, 2.2288565009f},
                  {0.0000000000f, 0.0000000000f, 0.0000000000f}}},
      {"Hg", {{2.3989314904f, 1.4400254917f, 0.9095512090f},
                 {6.3276269444f, 4.3719414152f, 3.4217899270f}}},
      {"HgTe", {{4.7795267752f, 3.2309984581f, 2.6600252401f},
                   {1.6319827058f, 1.5808189339f, 1.7295753852f}}},
      {"Ir", {{3.0864098394f, 2.0821938440f, 1.6178866805f},
                 {5.5921510077f, 4.0671757150f, 3.2672611269f}}},
      {"K", {{0.0640493070f, 0.0464100621f, 0.0381842017f},
                {2.1042155920f, 1.3489364357f, 0.9132113889f}}},
      {"Li", {{0.2657871942f, 0.1956102432f, 0.2209198538f},
                 {3.5401743407f, 2.3111306542f, 1.6685930000f}}},
      {"MgO", {{2.0895885542f, 1.6507224525f, 1.5948759692f},
                  {0.0000000000f, -0.0000000000f, 0.0000000000f}}},
      {"Mo", {{4.4837010280f, 3.5254578255f, 2.7760769438f},
                 {4.1111307988f, 3.4208716252f, 3.1506031404f}}},
      {"Na", {{0.0602665320f, 0.0561412435f, 0.0619909494f},
                 {3.1792906496f, 2.1124800781f, 1.5790940266f}}},
      {"Nb", {{3.4201353595f, 2.7901921379f, 2.3955856658f},
                 {3.4413817900f, 2.7376437930f, 2.5799132708f}}},
      {"Ni", {{2.3672753521f, 1.6633583302f, 1.4670554172f},
                 {4.4988329911f, 3.0501643957f, 2.3454274399f}}},
      {"Rh", {{2.5857954933f, 1.8601866068f, 1.5544279524f},
                 {6.7822927110f, 4.7029501026f, 3.9760892461f}}},
      {"Se-e", {{5.7242724833f, 4.1653992967f, 4.0816099264f},
                   {0.8713747439f, 1.1052845009f, 1.5647788766f}}},
      {"Se", {{4.0592611085f, 2.8426947380f, 2.8207582835f},
                 {0.7543791750f, 0.6385150558f, 0.5215872029f}}},
      {"SiC", {{3.1723450205f, 2.5259677964f, 2.4793623897f},
                  {0.0000007284f, -0.0000006859f, 0.0000100150f}}},
      {"SnTe", {{4.5251865890f, 1.9811525984f, 1.2816819226f},
                   {0.0000000000f, 0.0000000000f, 0.0000000000f}}},
      {"Ta", {{2.0625846607f, 2.3930915569f, 2.6280684948f},
                 {2.4080467973f, 1.7413705864f, 1.9470377016f}}},
      {"Te-e", {{7.5090397678f, 4.2964603080f, 2.3698732430f},
                   {5.5842076830f, 4.9476231084f, 3.9975145063f}}},
      {"Te", {{7.3908396088f, 4.4821028985f, 2.6370708478f},
                 {3.2561412892f, 3.5273908133f, 3.2921683116f}}},
      {"ThF4", {{1.8307187117f, 1.4422274283f, 1.3876488528f},
                   {0.0000000000f, 0.0000000000f, 0.0000000000f}}},
      {"TiC", {{3.7004673762f, 2.8374356509f, 2.5823030278f},
                  {3.2656905818f, 2.3515586388f, 2.1727857800f}}},
      {"TiN", {{1.6484691607f, 1.1504482522f, 1.3797795097f},
                  {3.3684596226f, 1.9434888540f, 1.1020123347f}}},
      {"TiO2-e", {{3.1065574823f, 2.5131551146f, 2.5823844157f},
                     {0.0000289537f, -0.0000251484f, 0.0001775555f}}},
      {"TiO2", {{3.4566203131f, 2.8017076558f, 2.9051485020f},
                   {0.0001026662f, -0.0000897534f, 0.0006356902f}}},
      {"VC", {{3.6575665991f, 2.7527298065f, 2.5326814570f},
                 {3.0683516659f, 2.1986687713f, 1.9631816252f}}},
      {"VN", {{2.8656011588f, 2.1191817791f, 1.9400767149f},
                 {3.0323264950f, 2.0561075580f, 1.6162930914f}}},
      {"V", {{4.2775126218f, 3.5131538236f, 2.7611257461f},
                {3.4911844504f, 2.8893580874f, 3.1116965117f}}},
      {"W", {{4.3707029924f, 3.3002972445f, 2.9982666528f},
                {3.5006778591f, 2.6048652781f, 2.2731930614f}}},
  };
  return metal_ior_table.at(name);
}

static inline bool parse_pbrt_params(
    string_view& str, vector<pbrt_value>& values) {
  auto parse_pbrt_pvalues = [](string_view& str, auto& value, auto& values) {
    values.clear();
    skip_whitespace(str);
    if (str.empty()) return false;
    if (str.front() == '[') {
      str.remove_prefix(1);
      skip_whitespace(str);
      if (str.empty()) return false;
      while (!str.empty()) {
        auto& val = values.empty() ? value : values.emplace_back();
        if(!parse_pbrt_value(str, val)) return false;
        skip_whitespace(str);
        if (str.empty()) break;
        if (str.front() == ']') break;
        if (values.empty()) values.push_back(value);
      }
      if (str.empty()) return false;
      if (str.front() != ']') return false;
      str.remove_prefix(1);
      return true;
    } else {
      return parse_pbrt_value(str, value);
    }
  };

  values.clear();
  skip_whitespace(str);
  while (!str.empty()) {
    auto& value = values.emplace_back();
    auto  type  = ""s;
    if(!parse_pbrt_nametype(str, value.name, type)) return false;
    skip_whitespace(str);
    if (str.empty()) return false;
    if (type == "float") {
      value.type = pbrt_value_type::real;
      if(!parse_pbrt_pvalues(str, value.value1f, value.vector1f)) return false;
    } else if (type == "integer") {
      value.type = pbrt_value_type::integer;
      if(!parse_pbrt_pvalues(str, value.value1i, value.vector1i)) return false;
    } else if (type == "string") {
      auto vector1s = vector<string>{};
      value.type    = pbrt_value_type::string;
      if(!parse_pbrt_pvalues(str, value.value1s, vector1s)) return false;
      if (!vector1s.empty()) return false;
    } else if (type == "bool") {
      auto value1s  = ""s;
      auto vector1s = vector<string>{};
      value.type    = pbrt_value_type::boolean;
      if(!parse_pbrt_pvalues(str, value1s, vector1s)) return false;
      if (!vector1s.empty()) return false;
      value.value1b = value1s == "true";
    } else if (type == "texture") {
      auto vector1s = vector<string>{};
      value.type    = pbrt_value_type::texture;
      if(!parse_pbrt_pvalues(str, value.value1s, vector1s)) return false;
      if (!vector1s.empty()) return false;
    } else if (type == "point" || type == "point3") {
      value.type = pbrt_value_type::point;
      if(!parse_pbrt_pvalues(str, value.value3f, value.vector3f)) return false;
    } else if (type == "normal" || type == "normal3") {
      value.type = pbrt_value_type::normal;
      if(!parse_pbrt_pvalues(str, value.value3f, value.vector3f)) return false;
    } else if (type == "vector" || type == "vector3") {
      value.type = pbrt_value_type::vector;
      if(!parse_pbrt_pvalues(str, value.value3f, value.vector3f)) return false;
    } else if (type == "point2") {
      value.type = pbrt_value_type::point2;
      if(!parse_pbrt_pvalues(str, value.value2f, value.vector2f)) return false;
    } else if (type == "vector2") {
      value.type = pbrt_value_type::vector2;
      if(!parse_pbrt_pvalues(str, value.value2f, value.vector2f)) return false;
    } else if (type == "blackbody") {
      value.type     = pbrt_value_type::color;
      auto blackbody = zero2f;
      auto vector2f  = vector<vec2f>{};
      if(!parse_pbrt_pvalues(str, blackbody, vector2f)) return false;
      if (!vector2f.empty()) return false;
      value.value3f = blackbody_to_rgb(blackbody.x) * blackbody.y;
    } else if (type == "color" || type == "rgb") {
      value.type = pbrt_value_type::color;
      if(!parse_pbrt_pvalues(str, value.value3f, value.vector3f)) return false;
    } else if (type == "xyz") {
      // TODO: xyz conversion
      value.type = pbrt_value_type::color;
      if(!parse_pbrt_pvalues(str, value.value3f, value.vector3f)) return false;
      throw std::runtime_error("xyz conversion");
      return false;
    } else if (type == "spectrum") {
      auto is_string = false;
      auto str1      = str;
      skip_whitespace(str1);
      if (!str1.empty() && str1.front() == '"')
        is_string = true;
      else if (!str1.empty() && str1.front() == '[') {
        str1.remove_prefix(1);
        skip_whitespace(str1);
        if (!str1.empty() && str1.front() == '"') is_string = true;
      }
      if (is_string) {
        value.type     = pbrt_value_type::color;
        auto filename  = ""s;
        auto filenames = vector<string>{};
        if(!parse_pbrt_value(str, filename)) return false;
        auto filenamep = fs::path(filename).filename();
        if (filenamep.extension() == ".spd") {
          filenamep = filenamep.replace_extension("");
          if (filenamep == "SHPS") {
            value.value3f = {1, 1, 1};
          } else if (filenamep.extension() == ".eta") {
            auto eta = get_pbrt_etak(filenamep.replace_extension("")).first;
            value.value3f = {eta.x, eta.y, eta.z};
          } else if (filenamep.extension() == ".k") {
            auto k = get_pbrt_etak(filenamep.replace_extension("")).second;
            value.value3f = {k.x, k.y, k.z};
          } else {
            throw std::runtime_error("unknown spectrum file " + filename);
            return false;
          }
        } else {
          throw std::runtime_error("unsupported spectrum format");
          return false;
        }
      } else {
        value.type = pbrt_value_type::spectrum;
        if(!parse_pbrt_pvalues(str, value.value1f, value.vector1f)) return false;
      }
    } else {
      throw std::runtime_error("unknown pbrt type");
      return false;
    }
    skip_whitespace(str);
  }
  return true;
}

// Read pbrt commands
bool read_pbrt_command(file_wrapper& fs, pbrt_command_& command, string& name,
    string& type, frame3f& xform, vector<pbrt_value>& values, bool& error, 
    string& line) {
  // set error
  auto set_error = [&error]() { error = true; return false; };
  // parse command by command
  while (read_pbrt_cmdline(fs, line)) {
    auto str = string_view{line};
    // get command
    auto cmd = ""s;
    if(!parse_pbrt_command(str, cmd)) return set_error();
    if (cmd == "WorldBegin") {
      command = pbrt_command_::world_begin;
      return true;
    } else if (cmd == "WorldEnd") {
      command = pbrt_command_::world_end;
      return true;
    } else if (cmd == "AttributeBegin") {
      command = pbrt_command_::attribute_begin;
      return true;
    } else if (cmd == "AttributeEnd") {
      command = pbrt_command_::attribute_end;
      return true;
    } else if (cmd == "TransformBegin") {
      command = pbrt_command_::transform_begin;
      return true;
    } else if (cmd == "TransformEnd") {
      command = pbrt_command_::transform_end;
      return true;
    } else if (cmd == "ObjectBegin") {
      if(!parse_pbrt_param(str, name)) return set_error();
      command = pbrt_command_::object_begin;
      return true;
    } else if (cmd == "ObjectEnd") {
      command = pbrt_command_::object_end;
      return true;
    } else if (cmd == "ObjectInstance") {
      if(!parse_pbrt_param(str, name)) return set_error();
      command = pbrt_command_::object_instance;
      return true;
    } else if (cmd == "ActiveTransform") {
      if(!parse_pbrt_command(str, name)) return set_error();
      command = pbrt_command_::active_transform;
      return true;
    } else if (cmd == "Transform") {
      auto xf = identity4x4f;
      if(!parse_pbrt_param(str, xf)) return set_error();
      xform   = frame3f{xf};
      command = pbrt_command_::set_transform;
      return true;
    } else if (cmd == "ConcatTransform") {
      auto xf = identity4x4f;
      if(!parse_pbrt_param(str, xf)) return set_error();
      xform   = frame3f{xf};
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "Scale") {
      auto v = zero3f;
      if(!parse_pbrt_param(str, v)) return set_error();
      xform   = scaling_frame(v);
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "Translate") {
      auto v = zero3f;
      if(!parse_pbrt_param(str, v)) return set_error();
      xform   = translation_frame(v);
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "Rotate") {
      auto v = zero4f;
      if(!parse_pbrt_param(str, v)) return set_error();
      xform   = rotation_frame(vec3f{v.y, v.z, v.w}, radians(v.x));
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "LookAt") {
      auto from = zero3f, to = zero3f, up = zero3f;
      if(!parse_pbrt_param(str, from)) return set_error();
      if(!parse_pbrt_param(str, to)) return set_error();
      if(!parse_pbrt_param(str, up)) return set_error();
      xform   = {from, to, up, zero3f};
      command = pbrt_command_::lookat_transform;
      return true;
    } else if (cmd == "ReverseOrientation") {
      command = pbrt_command_::reverse_orientation;
      return true;
    } else if (cmd == "CoordinateSystem") {
      if(!parse_pbrt_param(str, name)) return set_error();
      command = pbrt_command_::coordinate_system_set;
      return true;
    } else if (cmd == "CoordSysTransform") {
      if(!parse_pbrt_param(str, name)) return set_error();
      command = pbrt_command_::coordinate_system_transform;
      return true;
    } else if (cmd == "Integrator") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::integrator;
      return true;
    } else if (cmd == "Sampler") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::sampler;
      return true;
    } else if (cmd == "PixelFilter") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::filter;
      return true;
    } else if (cmd == "Film") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::film;
      return true;
    } else if (cmd == "Accelerator") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::accelerator;
      return true;
    } else if (cmd == "Camera") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::camera;
      return true;
    } else if (cmd == "Texture") {
      auto comptype = ""s;
      if(!parse_pbrt_param(str, name)) return set_error();
      if(!parse_pbrt_param(str, comptype)) return set_error();
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::named_texture;
      return true;
    } else if (cmd == "Material") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::material;
      return true;
    } else if (cmd == "MakeNamedMaterial") {
      if(!parse_pbrt_param(str, name)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      type = "";
      for (auto& value : values)
        if (value.name == "type") type = value.value1s;
      command = pbrt_command_::named_material;
      return true;
    } else if (cmd == "NamedMaterial") {
      if(!parse_pbrt_param(str, name)) return set_error();
      command = pbrt_command_::use_material;
      return true;
    } else if (cmd == "Shape") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::shape;
      return true;
    } else if (cmd == "AreaLightSource") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::arealight;
      return true;
    } else if (cmd == "LightSource") {
      if(!parse_pbrt_param(str, type)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      command = pbrt_command_::light;
      return true;
    } else if (cmd == "MakeNamedMedium") {
      if(!parse_pbrt_param(str, name)) return set_error();
      if(!parse_pbrt_params(str, values)) return set_error();
      type = "";
      for (auto& value : values)
        if (value.name == "type") type = value.value1s;
      command = pbrt_command_::named_medium;
      return true;
    } else if (cmd == "MediumInterface") {
      auto interior = ""s, exterior = ""s;
      if(!parse_pbrt_param(str, interior)) return set_error();
      if(!parse_pbrt_param(str, exterior)) return set_error();
      name    = interior + "####" + exterior;
      command = pbrt_command_::medium_interface;
      return true;
    } else if (cmd == "Include") {
      if(!parse_pbrt_param(str, name)) return set_error();
      command = pbrt_command_::include;
      return true;
    } else {
      throw std::runtime_error("unknown command " + cmd);
    }
  }
  return false;
}
bool read_pbrt_command(file_wrapper& fs, pbrt_command_& command, string& name,
    string& type, frame3f& xform, vector<pbrt_value>& values, bool& error) {
  auto command_buffer = ""s;
  return read_pbrt_command(
      fs, command, name, type, xform, values, error, command_buffer);
}

// Write obj elements
bool write_pbrt_comment(file_wrapper& fs, const string& comment) {
  auto lines = split_string(comment, "\n");
  for (auto& line : lines) {
    if(fprintf(fs.fs, "# %s\n", line.c_str())) return false;
  }
  if(fprintf(fs.fs, "\n")) return false;
  return true;
}

bool write_pbrt_values(file_wrapper& fs, const vector<pbrt_value>& values) {
  static auto type_labels = unordered_map<pbrt_value_type, string>{
      {pbrt_value_type::real, "float"},
      {pbrt_value_type::integer, "integer"},
      {pbrt_value_type::boolean, "bool"},
      {pbrt_value_type::string, "string"},
      {pbrt_value_type::point, "point"},
      {pbrt_value_type::normal, "normal"},
      {pbrt_value_type::vector, "vector"},
      {pbrt_value_type::texture, "texture"},
      {pbrt_value_type::color, "rgb"},
      {pbrt_value_type::point2, "point2"},
      {pbrt_value_type::vector2, "vector2"},
      {pbrt_value_type::spectrum, "spectrum"},
  };
  for (auto& value : values) {
    if(fprintf(fs.fs, " \"%s %s\" ", type_labels.at(value.type).c_str(), value.name.c_str())) return false;
    switch (value.type) {
      case pbrt_value_type::real:
        if (value.vector1f.empty()) {
          if(fprintf(fs.fs, "[ ")) return false;
          for (auto& v : value.vector1f) if(fprintf(fs.fs, " %g", v)) return false;
          if(fprintf(fs.fs, " ]")) return false;
        } else {
          if(fprintf(fs.fs, "%g", value.value1f)) return false;
        }
        break;
      case pbrt_value_type::integer:
        if (value.vector1f.empty()) {
          if(fprintf(fs.fs, "[ ")) return false;
          for (auto& v : value.vector1i) if(fprintf(fs.fs, " %d", v)) return false;
          if(fprintf(fs.fs, " ]")) return false;
        } else {
          if(fprintf(fs.fs, "%d", value.value1i)) return false;
        }
        break;
      case pbrt_value_type::boolean:
        if(fprintf(fs.fs, "\"%s\"", value.value1b ? "true" : "false")) return false;
        break;
      case pbrt_value_type::string:
        if(fprintf(fs.fs, "\"%s\"", value.value1b ? "true" : "false")) return false;
        break;
      case pbrt_value_type::point:
      case pbrt_value_type::vector:
      case pbrt_value_type::normal:
      case pbrt_value_type::color:
        if (!value.vector3f.empty()) {
          if(fprintf(fs.fs, "[ ")) return false;
          for (auto& v : value.vector3f)
            if(fprintf(fs.fs, " %g %g %g", v.x, v.y, v.z)) return false;
          if(fprintf(fs.fs, " ]")) return false;
        } else {
          if(fprintf(fs.fs, "[ %g %g %g ]", value.value3f.x, value.value3f.y,
              value.value3f.z)) return false;
        }
        break;
      case pbrt_value_type::spectrum:
        if(fprintf(fs.fs, "[ ")) return false;
        for (auto& v : value.vector1f) if(fprintf(fs.fs, " %g", v)) return false;
        if(fprintf(fs.fs, " ]")) return false;
        break;
      case pbrt_value_type::texture:
        if(fprintf(fs.fs, "\"%s\"", value.value1s.c_str())) return false;
        break;
      case pbrt_value_type::point2:
      case pbrt_value_type::vector2:
        if (!value.vector2f.empty()) {
          if(fprintf(fs.fs, "[ ")) return false;
          for (auto& v : value.vector2f)
            if(fprintf(fs.fs, " %g %g", v.x, v.y)) return false;
          if(fprintf(fs.fs, " ]")) return false;
        } else {
          if(fprintf(fs.fs, "[ %g %g ]", value.value2f.x, value.value2f.x)) return false;
        }
        break;
    }
  }
  if(fprintf(fs.fs, "\n")) return false;
  return true;
}

bool write_pbrt_command(file_wrapper& fs, pbrt_command_ command,
    const string& name, const string& type, const frame3f& xform,
    const vector<pbrt_value>& values, bool texture_float) {
  switch (command) {
    case pbrt_command_::world_begin: if(fprintf(fs.fs, "WorldBegin\n")) return false; break;
    case pbrt_command_::world_end: if(fprintf(fs.fs, "WorldEnd\n")) return false; break;
    case pbrt_command_::attribute_begin:
      if(fprintf(fs.fs, "AttributeBegin\n")) return false;
      break;
    case pbrt_command_::attribute_end:
      if(fprintf(fs.fs, "AttributeEnd\n")) return false;
      break;
    case pbrt_command_::transform_begin:
      if(fprintf(fs.fs, "TransformBegin\n")) return false;
      break;
    case pbrt_command_::transform_end:
      if(fprintf(fs.fs, "TransformEnd\n")) return false;
      break;
    case pbrt_command_::object_begin:
      if(fprintf(fs.fs, "ObjectBegin \"%s\"\n", name.c_str())) return false;
      break;
    case pbrt_command_::object_end: if(fprintf(fs.fs, "ObjectEnd\n")) return false; break;
    case pbrt_command_::object_instance:
      if(fprintf(fs.fs, "ObjectInstance \"%s\"\n", name.c_str())) return false;
      break;
    case pbrt_command_::sampler:
      if(fprintf(fs.fs, "Sampler \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::integrator:
      if(fprintf(fs.fs, "Integrator \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::accelerator:
      if(fprintf(fs.fs, "Accelerator \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::film:
      if(fprintf(fs.fs, "Film \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::filter:
      if(fprintf(fs.fs, "Filter \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::camera:
      if(fprintf(fs.fs, "Camera \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::shape:
      if(fprintf(fs.fs, "Shape \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::light:
      if(fprintf(fs.fs, "Light \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::material:
      if(fprintf(fs.fs, "Material \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::arealight:
      if(fprintf(fs.fs, "AreaLight \"%s\"", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::named_texture:
      if(fprintf(fs.fs, "Texture \"%s\" \"%s\" \"%s\"", name.c_str(),
          texture_float ? "float" : "rgb", type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::named_medium:
      if(fprintf(fs.fs, "MakeNamedMedium \"%s\" \"string type\" \"%s\"",
          name.c_str(), type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::named_material:
      if(fprintf(fs.fs, "MakeNamedMaterial \"%s\" \"string type\" \"%s\"",
          name.c_str(), type.c_str())) return false;
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::include:
      if(fprintf(fs.fs, "Include \"%s\"\n", name.c_str())) return false;
      break;
    case pbrt_command_::reverse_orientation:
      if(fprintf(fs.fs, "ReverseOrientation\n")) return false;
      break;
    case pbrt_command_::set_transform:
      if(fprintf(fs.fs,
          "Transform %g %g %g 0 %g %g %g 0 %g %g %g 0 %g %g %g 1\n", xform.x.x,
          xform.x.y, xform.x.z, xform.y.x, xform.y.y, xform.y.z, xform.z.x,
          xform.z.y, xform.z.z, xform.o.x, xform.o.y, xform.o.z)) return false;
      break;
    case pbrt_command_::concat_transform:
      if(fprintf(fs.fs,
          "ConcatTransform %g %g %g 0 %g %g %g 0 %g %g %g 0 %g %g %g 1\n",
          xform.x.x, xform.x.y, xform.x.z, xform.y.x, xform.y.y, xform.y.z,
          xform.z.x, xform.z.y, xform.z.z, xform.o.x, xform.o.y, xform.o.z)) return false;
      break;
    case pbrt_command_::lookat_transform:
      if(fprintf(fs.fs, "LookAt %g %g %g %g %g %g %g %g %g\n", xform.x.x,
          xform.x.y, xform.x.z, xform.y.x, xform.y.y, xform.y.z, xform.z.x,
          xform.z.y, xform.z.z)) return false;
      break;
    case pbrt_command_::use_material:
      if(fprintf(fs.fs, "NamedMaterial \"%s\"\n", name.c_str())) return false;
      break;
    case pbrt_command_::medium_interface: {
      auto interior = ""s, exterior = ""s;
      auto found = false;
      for (auto c : name) {
        if (c == '#') {
          found = true;
          continue;
        }
        if (found)
          exterior.push_back(c);
        else
          interior.push_back(c);
      }
      if(fprintf(fs.fs, "MediumInterface \"%s\" \"%s\"\n", interior.c_str(),
          exterior.c_str())) return false;
    } break;
    case pbrt_command_::active_transform:
      if(fprintf(fs.fs, "ActiveTransform \"%s\"\n", name.c_str())) return false;
      break;
    case pbrt_command_::coordinate_system_set:
      if(fprintf(fs.fs, "CoordinateSystem \"%s\"\n", name.c_str())) return false;
      break;
    case pbrt_command_::coordinate_system_transform:
      if(fprintf(fs.fs, "CoordinateSysTransform \"%s\"\n", name.c_str())) return false;
      break;
  }
  return true;
}

bool write_pbrt_command(file_wrapper& fs, pbrt_command_ command,
    const string& name, const frame3f& xform) {
  return write_pbrt_command(fs, command, name, "", xform, {});
}
bool write_pbrt_command(file_wrapper& fs, pbrt_command_ command,
    const string& name, const string& type, const vector<pbrt_value>& values,
    bool texture_as_float) {
  return write_pbrt_command(
      fs, command, name, type, identity3x4f, values, texture_as_float);
}

// get pbrt value
bool get_pbrt_value(const pbrt_value& pbrt, string& value) {
  if (pbrt.type == pbrt_value_type::string ||
      pbrt.type == pbrt_value_type::texture) {
    value = pbrt.value1s;
    return true;
  } else {
    return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, bool& value) {
  if (pbrt.type == pbrt_value_type::boolean) {
    value = pbrt.value1b;
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, int& value) {
  if (pbrt.type == pbrt_value_type::integer) {
    value = pbrt.value1i;
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, float& value) {
  if (pbrt.type == pbrt_value_type::real) {
    value = pbrt.value1f;
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, vec2f& value) {
  if (pbrt.type == pbrt_value_type::point2 ||
      pbrt.type == pbrt_value_type::vector2) {
    value = pbrt.value2f;
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, vec3f& value) {
  if (pbrt.type == pbrt_value_type::point ||
      pbrt.type == pbrt_value_type::vector ||
      pbrt.type == pbrt_value_type::normal ||
      pbrt.type == pbrt_value_type::color) {
    value = pbrt.value3f;
    return true;
  } else if (pbrt.type == pbrt_value_type::real) {
    value = vec3f{pbrt.value1f};
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, vector<float>& value) {
  if (pbrt.type == pbrt_value_type::real) {
    if (!pbrt.vector1f.empty()) {
      value = pbrt.vector1f;
    } else {
      value = {pbrt.value1f};
    }
    return true;
  } else {
    return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, vector<vec2f>& value) {
  if (pbrt.type == pbrt_value_type::point2 ||
      pbrt.type == pbrt_value_type::vector2) {
    if (!pbrt.vector2f.empty()) {
      value = pbrt.vector2f;
    } else {
      value = {pbrt.value2f};
    }
    return true;
  } else if (pbrt.type == pbrt_value_type::real) {
    if (pbrt.vector1f.empty() || pbrt.vector1f.size() % 2)
       return false;
    value.resize(pbrt.vector1f.size() / 2);
    for (auto i = 0; i < value.size(); i++)
      value[i] = {pbrt.vector1f[i * 2 + 0], pbrt.vector1f[i * 2 + 1]};
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, vector<vec3f>& value) {
  if (pbrt.type == pbrt_value_type::point ||
      pbrt.type == pbrt_value_type::vector ||
      pbrt.type == pbrt_value_type::normal ||
      pbrt.type == pbrt_value_type::color) {
    if (!pbrt.vector3f.empty()) {
      value = pbrt.vector3f;
    } else {
      value = {pbrt.value3f};
    }
    return true;
  } else if (pbrt.type == pbrt_value_type::real) {
    if (pbrt.vector1f.empty() || pbrt.vector1f.size() % 3)
       return false;
    value.resize(pbrt.vector1f.size() / 3);
    for (auto i = 0; i < value.size(); i++)
      value[i] = {pbrt.vector1f[i * 3 + 0], pbrt.vector1f[i * 3 + 1],
          pbrt.vector1f[i * 3 + 2]};
    return true;
  } else {
     return false;
  }
}

bool get_pbrt_value(const pbrt_value& pbrt, vector<int>& value) {
  if (pbrt.type == pbrt_value_type::integer) {
    if (!pbrt.vector1i.empty()) {
      value = pbrt.vector1i;
    } else {
      value = {pbrt.vector1i};
    }
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, vector<vec3i>& value) {
  if (pbrt.type == pbrt_value_type::integer) {
    if (pbrt.vector1i.empty() || pbrt.vector1i.size() % 3)
       return false;
    value.resize(pbrt.vector1i.size() / 3);
    for (auto i = 0; i < value.size(); i++)
      value[i] = {pbrt.vector1i[i * 3 + 0], pbrt.vector1i[i * 3 + 1],
          pbrt.vector1i[i * 3 + 2]};
    return true;
  } else {
     return false;
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, pair<float, string>& value) {
  if (pbrt.type == pbrt_value_type::string) {
    value.first = 0;
    return get_pbrt_value(pbrt, value.second);
  } else {
    value.second = "";
    return get_pbrt_value(pbrt, value.first);
  }
}
bool get_pbrt_value(const pbrt_value& pbrt, pair<vec3f, string>& value) {
  if (pbrt.type == pbrt_value_type::string ||
      pbrt.type == pbrt_value_type::texture) {
    value.first = zero3f;
    return get_pbrt_value(pbrt, value.second);
  } else {
    value.second = "";
    return get_pbrt_value(pbrt, value.first);
  }
}

// pbrt value construction
pbrt_value make_pbrt_value(
    const string& name, const string& value, pbrt_value_type type) {
  auto pbrt    = pbrt_value{};
  pbrt.name    = name;
  pbrt.type    = type;
  pbrt.value1s = value;
  return pbrt;
}
pbrt_value make_pbrt_value(
    const string& name, bool value, pbrt_value_type type) {
  auto pbrt    = pbrt_value{};
  pbrt.name    = name;
  pbrt.type    = type;
  pbrt.value1b = value;
  return pbrt;
}
pbrt_value make_pbrt_value(
    const string& name, int value, pbrt_value_type type) {
  auto pbrt    = pbrt_value{};
  pbrt.name    = name;
  pbrt.type    = type;
  pbrt.value1b = value;
  return pbrt;
}
pbrt_value make_pbrt_value(
    const string& name, float value, pbrt_value_type type) {
  auto pbrt    = pbrt_value{};
  pbrt.name    = name;
  pbrt.type    = type;
  pbrt.value1f = value;
  return pbrt;
}
pbrt_value make_pbrt_value(
    const string& name, const vec2f& value, pbrt_value_type type) {
  auto pbrt    = pbrt_value{};
  pbrt.name    = name;
  pbrt.type    = type;
  pbrt.value2f = value;
  return pbrt;
}
pbrt_value make_pbrt_value(
    const string& name, const vec3f& value, pbrt_value_type type) {
  auto pbrt    = pbrt_value{};
  pbrt.name    = name;
  pbrt.type    = type;
  pbrt.value3f = value;
  return pbrt;
}

// old code --- maintained here in case we want to integrate back
#if 0
void approximate_fourier_material(pbrt_material::fourier_t& fourier) {
  auto filename = fs::path(fourier.bsdffile).filename().string();
  if (filename == "paint.bsdf") {
    fourier.approx_type = pbrt_material::fourier_t::approx_type_t::plastic;
    auto& plastic       = fourier.approx_plastic;
    plastic.Kd          = {0.6f, 0.6f, 0.6f};
    // plastic.Ks = {0.4f, 0.4f, 0.4f};
    plastic.Ks         = {1.0f, 1.0f, 1.0f};
    plastic.uroughness = 0.2f;
    plastic.vroughness = 0.2f;
  } else if (filename == "ceramic.bsdf") {
    fourier.approx_type = pbrt_material::fourier_t::approx_type_t::plastic;
    auto& plastic       = fourier.approx_plastic;
    plastic.Kd          = {0.6f, 0.6f, 0.6f};
    // plastic.Ks = {0.1f, 0.1f, 0.1f};
    plastic.Ks         = {1.0f, 1.0f, 1.0f};
    plastic.uroughness = 0.025f;
    plastic.vroughness = 0.025f;
  } else if (filename == "leather.bsdf") {
    fourier.approx_type = pbrt_material::fourier_t::approx_type_t::plastic;
    auto& plastic       = fourier.approx_plastic;
    plastic.Kd          = {0.6f, 0.57f, 0.48f};
    // plastic.Ks = {0.1f, 0.1f, 0.1f};
    plastic.Ks         = {1.0f, 1.0f, 1.0f};
    plastic.uroughness = 0.3f;
    plastic.vroughness = 0.3f;
  } else if (filename == "coated_copper.bsdf") {
    fourier.approx_type = pbrt_material::fourier_t::approx_type_t::metal;
    auto& metal         = fourier.approx_metal;
    auto  etak          = get_pbrt_etak("Cu");
    metal.eta           = {etak.first.x, etak.first.y, etak.first.z};
    metal.k             = {etak.second.x, etak.second.y, etak.second.z};
    metal.uroughness    = 0.01f;
    metal.vroughness    = 0.01f;
  } else if (filename == "roughglass_alpha_0.2.bsdf") {
    fourier.approx_type = pbrt_material::fourier_t::approx_type_t::glass;
    auto& glass         = fourier.approx_glass;
    glass.uroughness    = 0.2f;
    glass.vroughness    = 0.2f;
    glass.Kr            = {1, 1, 1};
    glass.Kt            = {1, 1, 1};
  } else if (filename == "roughgold_alpha_0.2.bsdf") {
    fourier.approx_type = pbrt_material::fourier_t::approx_type_t::metal;
    auto& metal         = fourier.approx_metal;
    auto  etak          = get_pbrt_etak("Au");
    metal.eta           = {etak.first.x, etak.first.y, etak.first.z};
    metal.k             = {etak.second.x, etak.second.y, etak.second.z};
    metal.uroughness    = 0.2f;
    metal.vroughness    = 0.2f;
  } else {
    throw std::runtime_error("unknown pbrt bsdf filename " + fourier.bsdffile);
  }
}

// Pbrt measure subsurface parameters (sigma_prime_s, sigma_a in mm^-1)
// from pbrt code at pbrt/code/medium.cpp
static inline pair<vec3f, vec3f> parse_pbrt_subsurface(const string& name) {
  static const unordered_map<string, pair<vec3f, vec3f>> params = {
      // From "A Practical Model for Subsurface Light Transport"
      // Jensen, Marschner, Levoy, Hanrahan
      // Proc SIGGRAPH 2001
      {"Apple", {{2.29, 2.39, 1.97}, {0.0030, 0.0034, 0.046}}},
      {"Chicken1", {{0.15, 0.21, 0.38}, {0.015, 0.077, 0.19}}},
      {"Chicken2", {{0.19, 0.25, 0.32}, {0.018, 0.088, 0.20}}},
      {"Cream", {{7.38, 5.47, 3.15}, {0.0002, 0.0028, 0.0163}}},
      {"Ketchup", {{0.18, 0.07, 0.03}, {0.061, 0.97, 1.45}}},
      {"Marble", {{2.19, 2.62, 3.00}, {0.0021, 0.0041, 0.0071}}},
      {"Potato", {{0.68, 0.70, 0.55}, {0.0024, 0.0090, 0.12}}},
      {"Skimmilk", {{0.70, 1.22, 1.90}, {0.0014, 0.0025, 0.0142}}},
      {"Skin1", {{0.74, 0.88, 1.01}, {0.032, 0.17, 0.48}}},
      {"Skin2", {{1.09, 1.59, 1.79}, {0.013, 0.070, 0.145}}},
      {"Spectralon", {{11.6, 20.4, 14.9}, {0.00, 0.00, 0.00}}},
      {"Wholemilk", {{2.55, 3.21, 3.77}, {0.0011, 0.0024, 0.014}}},
      // From "Acquiring Scattering Properties of Participating Media by
      // Dilution",
      // Narasimhan, Gupta, Donner, Ramamoorthi, Nayar, Jensen
      // Proc SIGGRAPH 2006
      {"Lowfat Milk", {{0.89187, 1.5136, 2.532}, {0.002875, 0.00575, 0.0115}}},
      {"Reduced Milk",
          {{2.4858, 3.1669, 4.5214}, {0.0025556, 0.0051111, 0.012778}}},
      {"Regular Milk",
          {{4.5513, 5.8294, 7.136}, {0.0015333, 0.0046, 0.019933}}},
      {"Espresso", {{0.72378, 0.84557, 1.0247}, {4.7984, 6.5751, 8.8493}}},
      {"Mint Mocha Coffee",
          {{0.31602, 0.38538, 0.48131}, {3.772, 5.8228, 7.82}}},
      {"Lowfat Soy Milk",
          {{0.30576, 0.34233, 0.61664}, {0.0014375, 0.0071875, 0.035937}}},
      {"Regular Soy Milk",
          {{0.59223, 0.73866, 1.4693}, {0.0019167, 0.0095833, 0.065167}}},
      {"Lowfat Chocolate Milk",
          {{0.64925, 0.83916, 1.1057}, {0.0115, 0.0368, 0.1564}}},
      {"Regular Chocolate Milk",
          {{1.4585, 2.1289, 2.9527}, {0.010063, 0.043125, 0.14375}}},
      {"Coke", {{8.9053e-05, 8.372e-05, 0}, {0.10014, 0.16503, 0.2468}}},
      {"Pepsi", {{6.1697e-05, 4.2564e-05, 0}, {0.091641, 0.14158, 0.20729}}},
      {"Sprite", {{6.0306e-06, 6.4139e-06, 6.5504e-06},
                     {0.001886, 0.0018308, 0.0020025}}},
      {"Gatorade",
          {{0.0024574, 0.003007, 0.0037325}, {0.024794, 0.019289, 0.008878}}},
      {"Chardonnay", {{1.7982e-05, 1.3758e-05, 1.2023e-05},
                         {0.010782, 0.011855, 0.023997}}},
      {"White Zinfandel", {{1.7501e-05, 1.9069e-05, 1.288e-05},
                              {0.012072, 0.016184, 0.019843}}},
      {"Merlot", {{2.1129e-05, 0, 0}, {0.11632, 0.25191, 0.29434}}},
      {"Budweiser Beer", {{2.4356e-05, 2.4079e-05, 1.0564e-05},
                             {0.011492, 0.024911, 0.057786}}},
      {"Coors Light Beer",
          {{5.0922e-05, 4.301e-05, 0}, {0.006164, 0.013984, 0.034983}}},
      {"Clorox",
          {{0.0024035, 0.0031373, 0.003991}, {0.0033542, 0.014892, 0.026297}}},
      {"Apple Juice",
          {{0.00013612, 0.00015836, 0.000227}, {0.012957, 0.023741, 0.052184}}},
      {"Cranberry Juice", {{0.00010402, 0.00011646, 7.8139e-05},
                              {0.039437, 0.094223, 0.12426}}},
      {"Grape Juice", {{5.382e-05, 0, 0}, {0.10404, 0.23958, 0.29325}}},
      {"Ruby Grapefruit Juice",
          {{0.011002, 0.010927, 0.011036}, {0.085867, 0.18314, 0.25262}}},
      {"White Grapefruit Juice",
          {{0.22826, 0.23998, 0.32748}, {0.0138, 0.018831, 0.056781}}},
      {"Shampoo",
          {{0.0007176, 0.0008303, 0.0009016}, {0.014107, 0.045693, 0.061717}}},
      {"Strawberry Shampoo",
          {{0.00015671, 0.00015947, 1.518e-05}, {0.01449, 0.05796, 0.075823}}},
      {"Head & Shoulders Shampoo",
          {{0.023805, 0.028804, 0.034306}, {0.084621, 0.15688, 0.20365}}},
      {"Lemon Tea Powder",
          {{0.040224, 0.045264, 0.051081}, {2.4288, 4.5757, 7.2127}}},
      {"Orange Powder", {{0.00015617, 0.00017482, 0.0001762},
                            {0.001449, 0.003441, 0.007863}}},
      {"Pink Lemonade Powder", {{0.00012103, 0.00013073, 0.00012528},
                                   {0.001165, 0.002366, 0.003195}}},
      {"Cappuccino Powder",
          {{1.8436, 2.5851, 2.1662}, {35.844, 49.547, 61.084}}},
      {"Salt Powder",
          {{0.027333, 0.032451, 0.031979}, {0.28415, 0.3257, 0.34148}}},
      {"Sugar Powder",
          {{0.00022272, 0.00025513, 0.000271}, {0.012638, 0.031051, 0.050124}}},
      {"Suisse Mocha Powder",
          {{2.7979, 3.5452, 4.3365}, {17.502, 27.004, 35.433}}},
      {"Pacific Ocean Surface Water", {{0.0001764, 0.00032095, 0.00019617},
                                          {0.031845, 0.031324, 0.030147}}},
  };
  return params.at(name);
}
#endif

}  // namespace yocto
