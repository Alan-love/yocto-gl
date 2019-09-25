//
// Implementation for Yocto/ModelIO.
//

//
// TODO: fov/aspect lens/film everywhere
// TODO: pbrt design, split elements from approximations
// TODO: add uvdisk and uvsphere to pbrt shape loading
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
#include "yocto_utils.h"

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
void open_file(file_wrapper& fs, const string& filename, const string& mode) {
  close_file(fs);
  fs.filename = filename;
  fs.mode     = mode;
  fs.fs       = fopen(filename.c_str(), mode.c_str());
  if (!fs.fs) throw std::runtime_error("could not open file " + filename);
}
file_wrapper open_file(const string& filename, const string& mode) {
  auto fs = file_wrapper{};
  open_file(fs, filename, mode);
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

static inline void checked_fprintf(file_wrapper& fs, const char* fmt, ...) {
  va_list args1;
  va_start(args1, fmt);
  if (vfprintf(fs.fs, fmt, args1) < 0)
    throw std::runtime_error("cannot write to file");
  va_end(args1);
}

static inline void flip_texcoord(
    vector<vec2f>& flipped, const vector<vec2f>& texcoord) {
  for (auto& uv : flipped) uv.y = 1 - uv.y;
}

static inline vector<vec2f> flip_texcoord(const vector<vec2f>& texcoord) {
  auto flipped = texcoord;
  for (auto& uv : flipped) uv.y = 1 - uv.y;
  return flipped;
}

// Parse values from a string
static inline void parse_value(string_view& str, string_view& value) {
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
static inline void parse_value(string_view& str, string& value) {
  auto valuev = ""sv;
  parse_value(str, valuev);
  value = string{valuev};
}
static inline void parse_value(string_view& str, int8_t& value) {
  char* end = nullptr;
  value     = (int8_t)strtol(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, int16_t& value) {
  char* end = nullptr;
  value     = (int16_t)strtol(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, int32_t& value) {
  char* end = nullptr;
  value     = (int32_t)strtol(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, int64_t& value) {
  char* end = nullptr;
  value     = (int64_t)strtoll(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, uint8_t& value) {
  char* end = nullptr;
  value     = (uint8_t)strtoul(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, uint16_t& value) {
  char* end = nullptr;
  value     = (uint16_t)strtoul(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, uint32_t& value) {
  char* end = nullptr;
  value     = (uint32_t)strtoul(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, uint64_t& value) {
  char* end = nullptr;
  value     = (uint64_t)strtoull(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, bool& value) {
  auto valuei = 0;
  parse_value(str, valuei);
  value = (bool)valuei;
}
static inline void parse_value(string_view& str, float& value) {
  char* end = nullptr;
  value     = strtof(str.data(), &end);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
static inline void parse_value(string_view& str, double& value) {
  char* end = nullptr;
  value     = strtod(str.data(), &end);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
template <typename T>
static inline void parse_value(string_view& str, T* values, int num) {
  for (auto i = 0; i < num; i++) parse_value(str, values[i]);
}

static inline void parse_value(string_view& str, vec2f& value) {
  parse_value(str, &value.x, 2);
}
static inline void parse_value(string_view& str, vec3f& value) {
  parse_value(str, &value.x, 3);
}
static inline void parse_value(string_view& str, frame3f& value) {
  parse_value(str, &value.x.x, 12);
}
static inline void parse_value(string_view& str, size_t& value) {
  char* end = nullptr;
  value     = (size_t)strtoull(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}

// Parse values from a string
template <typename T>
static inline void parse_value_or_empty(string_view& str, T& value) {
  skip_whitespace(str);
  if (str.empty()) {
    value = T{};
  } else {
    parse_value(str, value);
  }
}

// Formats values to string
static inline void format_value(string& str, const string& value) {
  str += value;
}
static inline void format_value(string& str, const char* value) {
  str += value;
}
static inline void format_value(string& str, int8_t value) {
  char buf[256];
  sprintf(buf, "%d", (int)value);
  str += buf;
}
static inline void format_value(string& str, int16_t value) {
  char buf[256];
  sprintf(buf, "%d", (int)value);
  str += buf;
}
static inline void format_value(string& str, int32_t value) {
  char buf[256];
  sprintf(buf, "%d", (int)value);
  str += buf;
}
static inline void format_value(string& str, int64_t value) {
  char buf[256];
  sprintf(buf, "%lld", (long long)value);
  str += buf;
}
static inline void format_value(string& str, uint8_t value) {
  char buf[256];
  sprintf(buf, "%u", (unsigned)value);
  str += buf;
}
static inline void format_value(string& str, uint16_t value) {
  char buf[256];
  sprintf(buf, "%u", (unsigned)value);
  str += buf;
}
static inline void format_value(string& str, uint32_t value) {
  char buf[256];
  sprintf(buf, "%u", (unsigned)value);
  str += buf;
}
static inline void format_value(string& str, uint64_t value) {
  char buf[256];
  sprintf(buf, "%llu", (unsigned long long)value);
  str += buf;
}
static inline void format_value(string& str, float value) {
  char buf[256];
  sprintf(buf, "%g", value);
  str += buf;
}
static inline void format_value(string& str, double value) {
  char buf[256];
  sprintf(buf, "%g", value);
  str += buf;
}
static inline void format_value(string& str, const vec2f& value) {
  char buf[256];
  sprintf(buf, "%g %g", value.x, value.y);
  str += buf;
}
static inline void format_value(string& str, const vec3f& value) {
  char buf[256];
  sprintf(buf, "%g %g %g", value.x, value.y, value.z);
  str += buf;
}
static inline void format_value(string& str, const vec2i& value) {
  char buf[256];
  sprintf(buf, "%d %d", value.x, value.y);
  str += buf;
}
static inline void format_value(string& str, const vec3i& value) {
  char buf[256];
  sprintf(buf, "%d %d %d", value.x, value.y, value.z);
  str += buf;
}
static inline void format_value(string& str, const frame3f& value) {
  char buf[256];
  sprintf(buf, "%g %g %g %g %g %g %g %g %g %g %g %g", value.x.x, value.x.y,
      value.x.z, value.y.x, value.y.y, value.y.z, value.z.x, value.z.y,
      value.z.z, value.o.x, value.o.y, value.o.z);
  str += buf;
}

// Foramt to file
static inline void format_values(string& str, const string& fmt) {
  auto pos = fmt.find("{}");
  if (pos != string::npos) throw std::runtime_error("bad format string");
  str += fmt;
}
template <typename Arg, typename... Args>
static inline void format_values(
    string& str, const string& fmt, const Arg& arg, const Args&... args) {
  auto pos = fmt.find("{}");
  if (pos == string::npos) throw std::runtime_error("bad format string");
  str += fmt.substr(0, pos);
  format_value(str, arg);
  format_values(str, fmt.substr(pos + 2), args...);
}
template <typename... Args>
static inline void format_values(
    file_wrapper& fs, const string& fmt, const Args&... args) {
  auto str = ""s;
  format_values(str, fmt, args...);
  if (fputs(str.c_str(), fs.fs) < 0)
    throw std::runtime_error("cannor write to " + fs.filename);
}

static inline void write_text(file_wrapper& fs, const string& value) {
  if (fputs(value.c_str(), fs.fs) < 0)
    throw std::runtime_error("cannot write to " + fs.filename);
}
static inline void write_text(file_wrapper& fs, const char* value) {
  if (fputs(value, fs.fs) < 0)
    throw std::runtime_error("cannot write to " + fs.filename);
}

template <typename T>
static inline void write_value(file_wrapper& fs, const T& value) {
  if (fwrite(&value, sizeof(value), 1, fs.fs) != 1)
    throw std::runtime_error("cannot write to " + fs.filename);
}
template <typename T>
static inline void write_value(
    file_wrapper& fs, const T& value_, bool big_endian) {
  auto value = big_endian ? swap_endian(value_) : value_;
  if (fwrite(&value, sizeof(value), 1, fs.fs) != 1)
    throw std::runtime_error("cannot write to " + fs.filename);
}

template <typename T>
static inline void read_value(file_wrapper& fs, T& value) {
  if (fread(&value, sizeof(value), 1, fs.fs) != 1)
    throw std::runtime_error("cannot read " + fs.filename);
}
template <typename T>
static inline void read_value(file_wrapper& fs, T& value, bool big_endian) {
  if (fread(&value, sizeof(value), 1, fs.fs) != 1)
    throw std::runtime_error("cannot read " + fs.filename);
  if (big_endian) value = swap_endian(value);
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

// Load ply
void load_ply(const string& filename, ply_model& ply) {
  // ply type names
  static auto type_map = unordered_map<string, ply_type>{{"char", ply_type::i8},
      {"short", ply_type::i16}, {"int", ply_type::i32}, {"long", ply_type::i64},
      {"uchar", ply_type::u8}, {"ushort", ply_type::u16},
      {"uint", ply_type::u32}, {"ulong", ply_type::u64},
      {"float", ply_type::f32}, {"double", ply_type::f64},
      {"int8", ply_type::i8}, {"int16", ply_type::i16},
      {"int32", ply_type::i32}, {"int64", ply_type::i64},
      {"uint8", ply_type::u8}, {"uint16", ply_type::u16},
      {"uint32", ply_type::u32}, {"uint64", ply_type::u64},
      {"float32", ply_type::f32}, {"float64", ply_type::f64}};

  // initialize data
  ply = {};

  // parsing checks
  auto first_line = true;
  auto end_header = false;

  // open file
  auto fs = open_file(filename, "rb");

  // read header ---------------------------------------------
  char buffer[4096];
  while (read_line(fs, buffer, sizeof(buffer))) {
    // line
    auto line = string_view{buffer};
    remove_ply_comment(line);
    skip_whitespace(line);
    if (line.empty()) continue;

    // get command
    auto cmd = ""s;
    parse_value(line, cmd);
    if (cmd == "") continue;

    // check magic number
    if (first_line) {
      if (cmd != "ply") throw std::runtime_error{"bad ply file"};
      first_line = false;
      continue;
    }

    // possible token values
    if (cmd == "ply") {
      if (!first_line) throw std::runtime_error{"bad ply file"};
    } else if (cmd == "format") {
      auto fmt = ""sv;
      parse_value(line, fmt);
      if (fmt == "ascii") {
        ply.format = ply_format::ascii;
      } else if (fmt == "binary_little_endian") {
        ply.format = ply_format::binary_little_endian;
      } else if (fmt == "binary_big_endian") {
        ply.format = ply_format::binary_big_endian;
      } else {
        throw std::runtime_error{"unknown ply format"};
      }
    } else if (cmd == "comment") {
      skip_whitespace(line);
      ply.comments.push_back(string{line});
    } else if (cmd == "obj_info") {
      skip_whitespace(line);
      // comment is the rest of the line
    } else if (cmd == "element") {
      auto& elem = ply.elements.emplace_back();
      parse_value(line, elem.name);
      parse_value(line, elem.count);
    } else if (cmd == "property") {
      if (ply.elements.empty()) throw std::runtime_error{"bad ply header"};
      auto& prop  = ply.elements.back().properties.emplace_back();
      auto  tname = ""s;
      parse_value(line, tname);
      if (tname == "list") {
        prop.is_list = true;
        parse_value(line, tname);
        if (type_map.find(tname) == type_map.end())
          throw std::runtime_error{"unknown ply type " + tname};
        auto itype = type_map.at(tname);
        if (itype != ply_type::u8)
          throw std::runtime_error{"unsupported list size type " + tname};
        parse_value(line, tname);
        if (type_map.find(tname) == type_map.end())
          throw std::runtime_error{"unknown ply type " + tname};
        prop.type = type_map.at(tname);
      } else {
        prop.is_list = false;
        if (type_map.find(tname) == type_map.end())
          throw std::runtime_error{"unknown ply type " + tname};
        prop.type = type_map.at(tname);
      }
      parse_value(line, prop.name);
    } else if (cmd == "end_header") {
      end_header = true;
      break;
    } else {
      throw std::runtime_error{"unknown ply command"};
    }
  }

  // check exit
  if (!end_header) throw std::runtime_error{"bad ply header"};

  // allocate data ---------------------------------
  for (auto& element : ply.elements) {
    for (auto& property : element.properties) {
      auto count = property.is_list ? element.count * 3 : element.count;
      switch (property.type) {
        case ply_type::i8: property.data_i8.reserve(count); break;
        case ply_type::i16: property.data_i16.reserve(count); break;
        case ply_type::i32: property.data_i32.reserve(count); break;
        case ply_type::i64: property.data_i64.reserve(count); break;
        case ply_type::u8: property.data_u8.reserve(count); break;
        case ply_type::u16: property.data_u16.reserve(count); break;
        case ply_type::u32: property.data_u32.reserve(count); break;
        case ply_type::u64: property.data_u64.reserve(count); break;
        case ply_type::f32: property.data_f32.reserve(count); break;
        case ply_type::f64: property.data_f64.reserve(count); break;
      }
      if (property.is_list) property.ldata_u8.reserve(element.count);
    }
  }

  // read data -------------------------------------
  if (ply.format == ply_format::ascii) {
    for (auto& elem : ply.elements) {
      for (auto idx = 0; idx < elem.count; idx++) {
        if (!read_line(fs, buffer, sizeof(buffer)))
          throw std::runtime_error("cannot read ply");
        auto line = string_view{buffer};
        for (auto& prop : elem.properties) {
          if (prop.is_list) parse_value(line, prop.ldata_u8.emplace_back());
          auto vcount = prop.is_list ? prop.ldata_u8.back() : 1;
          for (auto i = 0; i < vcount; i++) {
            switch (prop.type) {
              case ply_type::i8:
                parse_value(line, prop.data_i8.emplace_back());
                break;
              case ply_type::i16:
                parse_value(line, prop.data_i16.emplace_back());
                break;
              case ply_type::i32:
                parse_value(line, prop.data_i32.emplace_back());
                break;
              case ply_type::i64:
                parse_value(line, prop.data_i64.emplace_back());
                break;
              case ply_type::u8:
                parse_value(line, prop.data_u8.emplace_back());
                break;
              case ply_type::u16:
                parse_value(line, prop.data_u16.emplace_back());
                break;
              case ply_type::u32:
                parse_value(line, prop.data_u32.emplace_back());
                break;
              case ply_type::u64:
                parse_value(line, prop.data_u64.emplace_back());
                break;
              case ply_type::f32:
                parse_value(line, prop.data_f32.emplace_back());
                break;
              case ply_type::f64:
                parse_value(line, prop.data_f64.emplace_back());
                break;
            }
          }
        }
      }
    }
  } else {
    auto big_endian = ply.format == ply_format::binary_big_endian;
    for (auto& elem : ply.elements) {
      for (auto idx = 0; idx < elem.count; idx++) {
        for (auto& prop : elem.properties) {
          if (prop.is_list)
            read_value(fs, prop.ldata_u8.emplace_back(), big_endian);
          auto vcount = prop.is_list ? prop.ldata_u8.back() : 1;
          for (auto i = 0; i < vcount; i++) {
            switch (prop.type) {
              case ply_type::i8:
                read_value(fs, prop.data_i8.emplace_back(), big_endian);
                break;
              case ply_type::i16:
                read_value(fs, prop.data_i16.emplace_back(), big_endian);
                break;
              case ply_type::i32:
                read_value(fs, prop.data_i32.emplace_back(), big_endian);
                break;
              case ply_type::i64:
                read_value(fs, prop.data_i64.emplace_back(), big_endian);
                break;
              case ply_type::u8:
                read_value(fs, prop.data_u8.emplace_back(), big_endian);
                break;
              case ply_type::u16:
                read_value(fs, prop.data_u16.emplace_back(), big_endian);
                break;
              case ply_type::u32:
                read_value(fs, prop.data_u32.emplace_back(), big_endian);
                break;
              case ply_type::u64:
                read_value(fs, prop.data_u64.emplace_back(), big_endian);
                break;
              case ply_type::f32:
                read_value(fs, prop.data_f32.emplace_back(), big_endian);
                break;
              case ply_type::f64:
                read_value(fs, prop.data_f64.emplace_back(), big_endian);
                break;
            }
          }
        }
      }
    }
  }
}

// Save ply
void save_ply(const string& filename, const ply_model& ply) {
  auto fs = open_file(filename, "wb");

  // ply type names
  static auto type_map = unordered_map<ply_type, string>{{ply_type::i8, "char"},
      {ply_type::i16, "short"}, {ply_type::i32, "int"}, {ply_type::i64, "uint"},
      {ply_type::u8, "uchar"}, {ply_type::u16, "ushort"},
      {ply_type::u32, "uint"}, {ply_type::u64, "ulong"},
      {ply_type::f32, "float"}, {ply_type::f64, "double"}};
  static auto format_map = unordered_map<ply_format, string>{
      {ply_format::ascii, "ascii"},
      {ply_format::binary_little_endian, "binary_little_endian"},
      {ply_format::binary_big_endian, "binary_big_endian"}};

  // header
  format_values(fs, "ply\n");
  format_values(fs, "format {} 1.0\n", format_map.at(ply.format));
  format_values(fs, "comment Written by Yocto/GL\n");
  format_values(fs, "comment https://github.com/xelatihy/yocto-gl\n");
  for (auto& comment : ply.comments) format_values(fs, "comment {}\n", comment);
  for (auto& elem : ply.elements) {
    format_values(
        fs, "element {} {}\n", elem.name, (unsigned long long)elem.count);
    for (auto& prop : elem.properties) {
      if (prop.is_list) {
        format_values(fs, "property list uchar {} {}\n",
            type_map[prop.type].c_str(), prop.name);
      } else {
        format_values(fs, "property {} {}\n", type_map[prop.type], prop.name);
      }
    }
  }
  format_values(fs, "end_header\n");

  // properties
  if (ply.format == ply_format::ascii) {
    for (auto& elem : ply.elements) {
      auto cur = vector<size_t>(elem.properties.size(), 0);
      for (auto idx = 0; idx < elem.count; idx++) {
        for (auto pidx = 0; pidx < elem.properties.size(); pidx++) {
          auto& prop = elem.properties[pidx];
          if (prop.is_list) format_values(fs, "{} ", (int)prop.ldata_u8[idx]);
          auto vcount = prop.is_list ? prop.ldata_u8[idx] : 1;
          for (auto i = 0; i < vcount; i++) {
            switch (prop.type) {
              case ply_type::i8:
                format_values(fs, "{} ", (int)prop.data_i8[cur[idx]++]);
                break;
              case ply_type::i16:
                format_values(fs, "{} ", (int)prop.data_i16[cur[idx]++]);
                break;
              case ply_type::i32:
                format_values(fs, "{} ", (int)prop.data_i32[cur[idx]++]);
                break;
              case ply_type::i64:
                format_values(
                    fs, "{} ", (long long int)prop.data_i64[cur[idx]++]);
                break;
              case ply_type::u8:
                format_values(fs, "{} ", (unsigned)prop.data_i8[cur[idx]++]);
                break;
              case ply_type::u16:
                format_values(fs, "{} ", (unsigned)prop.data_i16[cur[idx]++]);
                break;
              case ply_type::u32:
                format_values(fs, "{} ", (unsigned)prop.data_u32[cur[idx]++]);
                break;
              case ply_type::u64:
                format_values(
                    fs, "{} ", (long long unsigned)prop.data_u64[cur[idx]++]);
                break;
              case ply_type::f32:
                format_values(fs, "{}", prop.data_f32[cur[idx]++]);
                break;
              case ply_type::f64:
                format_values(fs, "{}", prop.data_f64[cur[idx]++]);
                break;
            }
          }
          format_values(fs, "\n");
        }
      }
    }
  } else {
    auto big_endian = ply.format == ply_format::binary_big_endian;
    for (auto& elem : ply.elements) {
      auto cur = vector<size_t>(elem.properties.size(), 0);
      for (auto idx = 0; idx < elem.count; idx++) {
        for (auto pidx = 0; pidx < elem.properties.size(); pidx++) {
          auto& prop = elem.properties[pidx];
          if (prop.is_list) write_value(fs, prop.ldata_u8[idx], big_endian);
          auto vcount = prop.is_list ? prop.ldata_u8[idx] : 1;
          for (auto i = 0; i < vcount; i++) {
            switch (prop.type) {
              case ply_type::i8:
                write_value(fs, prop.data_i8[cur[pidx]++], big_endian);
                break;
              case ply_type::i16:
                write_value(fs, prop.data_i16[cur[pidx]++], big_endian);
                break;
              case ply_type::i32:
                write_value(fs, prop.data_i32[cur[pidx]++], big_endian);
                break;
              case ply_type::i64:
                write_value(fs, prop.data_i64[cur[pidx]++], big_endian);
                break;
              case ply_type::u8:
                write_value(fs, prop.data_i8[cur[pidx]++], big_endian);
                break;
              case ply_type::u16:
                write_value(fs, prop.data_i16[cur[pidx]++], big_endian);
                break;
              case ply_type::u32:
                write_value(fs, prop.data_u32[cur[pidx]++], big_endian);
                break;
              case ply_type::u64:
                write_value(fs, prop.data_u64[cur[pidx]++], big_endian);
                break;
              case ply_type::f32:
                write_value(fs, prop.data_f32[cur[pidx]++], big_endian);
                break;
              case ply_type::f64:
                write_value(fs, prop.data_f64[cur[pidx]++], big_endian);
                break;
            }
          }
        }
      }
    }
  }
}

// Get ply properties
bool has_ply_property(
    const ply_model& ply, const string& element, const string& property) {
  for (auto& elem : ply.elements) {
    if (elem.name != element) continue;
    for (auto& prop : elem.properties) {
      if (prop.name == property) return true;
    }
  }
  return false;
}
const ply_property& get_ply_property(
    const ply_model& ply, const string& element, const string& property) {
  for (auto& elem : ply.elements) {
    if (elem.name != element) continue;
    for (auto& prop : elem.properties) {
      if (prop.name == property) return prop;
    }
  }
  throw std::runtime_error("property not found");
}
ply_property& get_ply_property(
    ply_model& ply, const string& element, const string& property) {
  for (auto& elem : ply.elements) {
    if (elem.name != element) continue;
    for (auto& prop : elem.properties) {
      if (prop.name == property) return prop;
    }
  }
  throw std::runtime_error("property not found");
}
template <typename T, typename T1>
inline vector<T> convert_ply_property(const vector<T1>& prop) {
  auto values = vector<T>(prop.size());
  for (auto i = (size_t)0; i < prop.size(); i++) values[i] = (T)prop[i];
  return values;
}
template <typename T>
inline vector<T> convert_ply_property(const ply_property& prop) {
  switch (prop.type) {
    case ply_type::i8: return convert_ply_property<T>(prop.data_i8);
    case ply_type::i16: return convert_ply_property<T>(prop.data_i16);
    case ply_type::i32: return convert_ply_property<T>(prop.data_i32);
    case ply_type::i64: return convert_ply_property<T>(prop.data_i64);
    case ply_type::u8: return convert_ply_property<T>(prop.data_u8);
    case ply_type::u16: return convert_ply_property<T>(prop.data_u16);
    case ply_type::u32: return convert_ply_property<T>(prop.data_u32);
    case ply_type::u64: return convert_ply_property<T>(prop.data_u64);
    case ply_type::f32: return convert_ply_property<T>(prop.data_f32);
    case ply_type::f64: return convert_ply_property<T>(prop.data_f64);
  }
}
vector<float> get_ply_values(
    const ply_model& ply, const string& element, const string& property) {
  if (!has_ply_property(ply, element, property)) return {};
  auto& prop = get_ply_property(ply, element, property);
  if (prop.is_list) return {};
  return convert_ply_property<float>(prop);
}
vector<vec2f> get_ply_values(const ply_model& ply, const string& element,
    const string& property1, const string& property2) {
  auto x      = get_ply_values(ply, element, property1);
  auto y      = get_ply_values(ply, element, property2);
  auto values = vector<vec2f>(x.size());
  for (auto i = (size_t)0; i < values.size(); i++) values[i] = {x[i], y[i]};
  return values;
}
vector<vec3f> get_ply_values(const ply_model& ply, const string& element,
    const string& property1, const string& property2, const string& property3) {
  auto x      = get_ply_values(ply, element, property1);
  auto y      = get_ply_values(ply, element, property2);
  auto z      = get_ply_values(ply, element, property3);
  auto values = vector<vec3f>(x.size());
  for (auto i = (size_t)0; i < values.size(); i++)
    values[i] = {x[i], y[i], z[i]};
  return values;
}
vector<vec4f> get_ply_values(const ply_model& ply, const string& element,
    const string& property1, const string& property2, const string& property3,
    const string& property4) {
  auto x      = get_ply_values(ply, element, property1);
  auto y      = get_ply_values(ply, element, property2);
  auto z      = get_ply_values(ply, element, property3);
  auto w      = get_ply_values(ply, element, property4);
  auto values = vector<vec4f>(x.size());
  for (auto i = (size_t)0; i < values.size(); i++)
    values[i] = {x[i], y[i], z[i], w[i]};
  return values;
}
vector<vec4f> get_ply_values(const ply_model& ply, const string& element,
    const string& property1, const string& property2, const string& property3,
    float property4) {
  auto x      = get_ply_values(ply, element, property1);
  auto y      = get_ply_values(ply, element, property2);
  auto z      = get_ply_values(ply, element, property3);
  auto w      = property4;
  auto values = vector<vec4f>(x.size());
  for (auto i = (size_t)0; i < values.size(); i++)
    values[i] = {x[i], y[i], z[i], w};
  return values;
}
vector<vector<int>> get_ply_lists(
    const ply_model& ply, const string& element, const string& property) {
  if (!has_ply_property(ply, element, property)) return {};
  auto& prop = get_ply_property(ply, element, property);
  if (!prop.is_list) return {};
  auto& sizes  = prop.ldata_u8;
  auto  values = convert_ply_property<int>(prop);
  auto  lists  = vector<vector<int>>(sizes.size());
  auto  cur    = (size_t)0;
  for (auto i = (size_t)0; i < lists.size(); i++) {
    lists[i].resize(sizes[i]);
    for (auto c = 0; c < sizes[i]; c++) {
      lists[i][c] = values[cur++];
    }
  }
  return lists;
}
vector<byte> get_ply_list_sizes(
    const ply_model& ply, const string& element, const string& property) {
  if (!has_ply_property(ply, element, property)) return {};
  auto& prop = get_ply_property(ply, element, property);
  if (!prop.is_list) return {};
  return prop.ldata_u8;
}
vector<int> get_ply_list_values(
    const ply_model& ply, const string& element, const string& property) {
  if (!has_ply_property(ply, element, property)) return {};
  auto& prop = get_ply_property(ply, element, property);
  if (!prop.is_list) return {};
  return convert_ply_property<int>(prop);
}

// Get ply properties for meshes
vector<vec3f> get_ply_positions(const ply_model& ply) {
  return get_ply_values(ply, "vertex", "x", "y", "z");
}
vector<vec3f> get_ply_normals(const ply_model& ply) {
  return get_ply_values(ply, "vertex", "nx", "ny", "nz");
}
vector<vec2f> get_ply_texcoords(const ply_model& ply, bool flipv) {
  auto texcoord = has_ply_property(ply, "vertex", "u")
                      ? get_ply_values(ply, "vertex", "u", "v")
                      : get_ply_values(ply, "vertex", "s", "t");
  return flipv ? flip_texcoord(texcoord) : texcoord;
}
vector<vec4f> get_ply_colors(const ply_model& ply) {
  if (has_ply_property(ply, "vertex", "alpha")) {
    return get_ply_values(ply, "vertex", "red", "green", "blue", "alpha");
  } else {
    return get_ply_values(ply, "vertex", "red", "green", "blue", 1);
  }
}
vector<float> get_ply_radius(const ply_model& ply) {
  return get_ply_values(ply, "vertex", "radius");
}
vector<vector<int>> get_ply_faces(const ply_model& ply) {
  return get_ply_lists(ply, "face", "vertex_indices");
}
vector<vec3i> get_ply_triangles(const ply_model& ply) {
  auto indices   = get_ply_list_values(ply, "face", "vertex_indices");
  auto sizes     = get_ply_list_sizes(ply, "face", "vertex_indices");
  auto triangles = vector<vec3i>{};
  triangles.reserve(sizes.size());
  auto cur = 0;
  for (auto size : sizes) {
    for (auto c = 2; c < size; c++) {
      triangles.push_back(
          {indices[cur + 0], indices[cur + c - 1], indices[cur + c]});
    }
    cur += size;
  }
  return triangles;
}
vector<vec4i> get_ply_quads(const ply_model& ply) {
  auto indices = get_ply_list_values(ply, "face", "vertex_indices");
  auto sizes   = get_ply_list_sizes(ply, "face", "vertex_indices");
  auto quads   = vector<vec4i>{};
  quads.reserve(sizes.size());
  auto cur = 0;
  for (auto size : sizes) {
    if (size == 4) {
      quads.push_back({indices[cur + 0], indices[cur + 1], indices[cur + 2],
          indices[cur + 3]});
    } else {
      for (auto c = 2; c < size; c++) {
        quads.push_back({indices[cur + 0], indices[cur + c - 1],
            indices[cur + c], indices[cur + c]});
      }
    }
    cur += size;
  }
  return quads;
}
vector<vec2i> get_ply_lines(const ply_model& ply) {
  auto indices = get_ply_list_values(ply, "line", "vertex_indices");
  auto sizes   = get_ply_list_sizes(ply, "line", "vertex_indices");
  auto lines   = vector<vec2i>{};
  lines.reserve(sizes.size());
  auto cur = 0;
  for (auto size : sizes) {
    for (auto c = 1; c < size; c++) {
      lines.push_back({indices[cur + c - 1], indices[cur + c]});
    }
    cur += size;
  }
  return lines;
}
vector<int> get_ply_points(const ply_model& ply) {
  return get_ply_list_values(ply, "point", "vertex_indices");
}
bool has_ply_quads(const ply_model& ply) {
  auto sizes = get_ply_list_sizes(ply, "face", "vertex_indices");
  for (auto size : sizes)
    if (size == 4) return true;
  return false;
}

// Add ply properties
void add_ply_element(ply_model& ply, const string& element, size_t count) {
  for (auto& elem : ply.elements) {
    if (elem.name == element) return;
  }
  auto& elem = ply.elements.emplace_back();
  elem.name  = element;
  elem.count = count;
}
void add_ply_property(ply_model& ply, const string& element,
    const string& property, size_t count, ply_type type, bool is_list) {
  add_ply_element(ply, element, count);
  for (auto& elem : ply.elements) {
    if (elem.name != element) continue;
    for (auto& prop : elem.properties) {
      if (prop.name == property)
        throw std::runtime_error("property already added");
    }
    auto& prop   = elem.properties.emplace_back();
    prop.name    = property;
    prop.type    = type;
    prop.is_list = is_list;
    return;
  }
}
template <typename T>
vector<T> make_ply_vector(const T* value, size_t count, int stride) {
  auto ret = vector<T>(count);
  for (auto idx = (size_t)0; idx < count; idx++) ret[idx] = value[idx * stride];
  return ret;
}

void add_ply_values(ply_model& ply, const float* values, size_t count,
    const string& element, const string* properties, int nprops) {
  if (!values) return;
  for (auto p = 0; p < nprops; p++) {
    add_ply_property(ply, element, properties[p], count, ply_type::f32, false);
    auto& prop = get_ply_property(ply, element, properties[p]);
    prop.data_f32.resize(count);
    for (auto i = 0; i < count; i++) prop.data_f32[i] = values[p + i * nprops];
  }
}

void add_ply_values(ply_model& ply, const vector<float>& values,
    const string& element, const string& property) {
  auto properties = vector{property};
  add_ply_values(
      ply, (float*)values.data(), values.size(), element, properties.data(), 1);
}
void add_ply_values(ply_model& ply, const vector<vec2f>& values,
    const string& element, const string& property1, const string& property2) {
  auto properties = vector{property1, property2};
  add_ply_values(
      ply, (float*)values.data(), values.size(), element, properties.data(), 2);
}
void add_ply_values(ply_model& ply, const vector<vec3f>& values,
    const string& element, const string& property1, const string& property2,
    const string& property3) {
  auto properties = vector{property1, property2, property3};
  add_ply_values(
      ply, (float*)values.data(), values.size(), element, properties.data(), 3);
}
void add_ply_values(ply_model& ply, const vector<vec4f>& values,
    const string& element, const string& property1, const string& property2,
    const string& property3, const string& property4) {
  auto properties = vector{property1, property2, property3, property4};
  add_ply_values(
      ply, (float*)values.data(), values.size(), element, properties.data(), 4);
}

void add_ply_lists(ply_model& ply, const vector<vector<int>>& values,
    const string& element, const string& property) {
  if (values.empty()) return;
  add_ply_property(ply, element, property, values.size(), ply_type::i32, true);
  auto& prop = get_ply_property(ply, element, property);
  prop.data_i32.reserve(values.size() * 4);
  prop.ldata_u8.reserve(values.size());
  for (auto& value : values) {
    prop.data_i32.insert(prop.data_i32.end(), value.begin(), value.end());
    prop.ldata_u8.push_back((uint8_t)value.size());
  }
}
void add_ply_lists(ply_model& ply, const vector<byte>& sizes,
    const vector<int>& values, const string& element, const string& property) {
  if (values.empty()) return;
  add_ply_property(ply, element, property, values.size(), ply_type::i32, true);
  auto& prop    = get_ply_property(ply, element, property);
  prop.data_i32 = values;
  prop.ldata_u8 = sizes;
}
void add_ply_lists(ply_model& ply, const int* values, size_t count, int size,
    const string& element, const string& property) {
  if (!values) return;
  add_ply_property(ply, element, property, count, ply_type::i32, true);
  auto& prop = get_ply_property(ply, element, property);
  prop.data_i32.assign(values, values + count * size);
  prop.ldata_u8.assign(count, size);
}
void add_ply_lists(ply_model& ply, const vector<int>& values,
    const string& element, const string& property) {
  return add_ply_lists(ply, values.data(), values.size(), 1, element, property);
}
void add_ply_lists(ply_model& ply, const vector<vec2i>& values,
    const string& element, const string& property) {
  return add_ply_lists(
      ply, (int*)values.data(), values.size(), 2, element, property);
}
void add_ply_lists(ply_model& ply, const vector<vec3i>& values,
    const string& element, const string& property) {
  return add_ply_lists(
      ply, (int*)values.data(), values.size(), 3, element, property);
}
void add_ply_lists(ply_model& ply, const vector<vec4i>& values,
    const string& element, const string& property) {
  return add_ply_lists(
      ply, (int*)values.data(), values.size(), 4, element, property);
}

// Add ply properties for meshes
void add_ply_positions(ply_model& ply, const vector<vec3f>& values) {
  return add_ply_values(ply, values, "vertex", "x", "y", "z");
}
void add_ply_normals(ply_model& ply, const vector<vec3f>& values) {
  return add_ply_values(ply, values, "vertex", "nx", "ny", "nz");
}
void add_ply_texcoords(
    ply_model& ply, const vector<vec2f>& values, bool flipv) {
  return add_ply_values(
      ply, flipv ? flip_texcoord(values) : values, "vertex", "u", "v");
}
void add_ply_colors(ply_model& ply, const vector<vec4f>& values) {
  return add_ply_values(ply, values, "vertex", "red", "green", "blue", "alpha");
}
void add_ply_radius(ply_model& ply, const vector<float>& values) {
  return add_ply_values(ply, values, "vertex", "radius");
}
void add_ply_faces(ply_model& ply, const vector<vector<int>>& values) {
  return add_ply_lists(ply, values, "face", "vertex_indices");
}
void add_ply_faces(ply_model& ply, const vector<vec3i>& triangles,
    const vector<vec4i>& quads) {
  if (triangles.empty() && quads.empty()) return;
  if (quads.empty()) {
    return add_ply_lists(ply, triangles, "face", "vertex_indices");
  } else if (triangles.empty() &&
             std::all_of(quads.begin(), quads.end(),
                 [](const vec4i& q) { return q.z != q.w; })) {
    return add_ply_lists(ply, quads, "face", "vertex_indices");
  } else {
    auto sizes   = vector<uint8_t>();
    auto indices = vector<int>{};
    sizes.reserve(triangles.size() + quads.size());
    indices.reserve(triangles.size() * 3 + quads.size() * 4);
    for (auto& t : triangles) {
      sizes.push_back(3);
      indices.push_back(t.x);
      indices.push_back(t.y);
      indices.push_back(t.z);
    }
    for (auto& q : quads) {
      sizes.push_back(q.z == q.w ? 3 : 4);
      indices.push_back(q.x);
      indices.push_back(q.y);
      indices.push_back(q.z);
      if (q.z != q.w) indices.push_back(q.w);
    }
    return add_ply_lists(ply, sizes, indices, "face", "vertex_indices");
  }
}
void add_ply_triangles(ply_model& ply, const vector<vec3i>& values) {
  return add_ply_faces(ply, values, {});
}
void add_ply_quads(ply_model& ply, const vector<vec4i>& values) {
  return add_ply_faces(ply, {}, values);
}
void add_ply_lines(ply_model& ply, const vector<vec2i>& values) {
  return add_ply_lists(ply, values, "line", "vertex_indices");
}
void add_ply_points(ply_model& ply, const vector<int>& values) {
  return add_ply_lists(ply, values, "point", "vertex_indices");
}

// get ply value either ascii or binary
template <typename T>
static inline T read_ply_value(file_wrapper& fs, bool big_endian) {
  auto value = (T)0;
  if (fread(&value, sizeof(T), 1, fs.fs) != 1)
    throw std::runtime_error("cannot read value");
  if (big_endian) value = swap_endian(value);
  return value;
}
template <typename VT>
static inline void read_ply_prop(
    file_wrapper& fs, bool big_endian, ply_type type, VT& value) {
  switch (type) {
    case ply_type::i8:
      value = (VT)read_ply_value<int8_t>(fs, big_endian);
      break;
    case ply_type::i16:
      value = (VT)read_ply_value<int16_t>(fs, big_endian);
      break;
    case ply_type::i32:
      value = (VT)read_ply_value<int32_t>(fs, big_endian);
      break;
    case ply_type::i64:
      value = (VT)read_ply_value<int64_t>(fs, big_endian);
      break;
    case ply_type::u8:
      value = (VT)read_ply_value<uint8_t>(fs, big_endian);
      break;
    case ply_type::u16:
      value = (VT)read_ply_value<uint16_t>(fs, big_endian);
      break;
    case ply_type::u32:
      value = (VT)read_ply_value<uint32_t>(fs, big_endian);
      break;
    case ply_type::u64:
      value = (VT)read_ply_value<uint64_t>(fs, big_endian);
      break;
    case ply_type::f32:
      value = (VT)read_ply_value<float>(fs, big_endian);
      break;
    case ply_type::f64:
      value = (VT)read_ply_value<double>(fs, big_endian);
      break;
  }
}

template <typename VT>
static inline void parse_ply_prop(string_view& str, ply_type type, VT& value) {
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
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}

// Load ply data
void read_ply_header(file_wrapper& fs, ply_format& format,
    vector<ply_element>& elements, vector<string>& comments) {
  // ply type names
  static auto type_map = unordered_map<string, ply_type>{{"char", ply_type::i8},
      {"short", ply_type::i16}, {"int", ply_type::i32}, {"long", ply_type::i64},
      {"uchar", ply_type::u8}, {"ushort", ply_type::u16},
      {"uint", ply_type::u32}, {"ulong", ply_type::u64},
      {"float", ply_type::f32}, {"double", ply_type::f64},
      {"int8", ply_type::i8}, {"int16", ply_type::i16},
      {"int32", ply_type::i32}, {"int64", ply_type::i64},
      {"uint8", ply_type::u8}, {"uint16", ply_type::u16},
      {"uint32", ply_type::u32}, {"uint64", ply_type::u64},
      {"float32", ply_type::f32}, {"float64", ply_type::f64}};

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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // check magic number
    if (first_line) {
      if (cmd != "ply") throw std::runtime_error{"bad ply file"};
      first_line = false;
      continue;
    }

    // possible token values
    if (cmd == "ply") {
      if (!first_line) throw std::runtime_error{"bad ply file"};
    } else if (cmd == "format") {
      auto fmt = ""sv;
      parse_value(line, fmt);
      if (fmt == "ascii") {
        format = ply_format::ascii;
      } else if (fmt == "binary_little_endian") {
        format = ply_format::binary_little_endian;
      } else if (fmt == "binary_big_endian") {
        format = ply_format::binary_big_endian;
      } else {
        throw std::runtime_error{"unknown ply format"};
      }
    } else if (cmd == "comment") {
      skip_whitespace(line);
      comments.push_back(string{line});
    } else if (cmd == "obj_info") {
      skip_whitespace(line);
      // comment is the rest of the line
    } else if (cmd == "element") {
      auto& elem = elements.emplace_back();
      parse_value(line, elem.name);
      parse_value(line, elem.count);
    } else if (cmd == "property") {
      if (elements.empty()) throw std::runtime_error{"bad ply header"};
      auto& prop  = elements.back().properties.emplace_back();
      auto  tname = ""s;
      parse_value(line, tname);
      if (tname == "list") {
        prop.is_list = true;
        parse_value(line, tname);
        if (type_map.find(tname) == type_map.end())
          throw std::runtime_error{"unknown ply type " + tname};
        auto itype = type_map.at(tname);
        if (itype != ply_type::u8)
          throw std::runtime_error{"unsupported list size type " + tname};
        parse_value(line, tname);
        if (type_map.find(tname) == type_map.end())
          throw std::runtime_error{"unknown ply type " + tname};
        prop.type = type_map.at(tname);
      } else {
        prop.is_list = false;
        if (type_map.find(tname) == type_map.end())
          throw std::runtime_error{"unknown ply type " + tname};
        prop.type = type_map.at(tname);
      }
      parse_value(line, prop.name);
    } else if (cmd == "end_header") {
      end_header = true;
      break;
    } else {
      throw std::runtime_error{"unknown ply command"};
    }
  }

  if (!end_header) throw std::runtime_error{"bad ply header"};
}

template <typename VT, typename LT>
void read_ply_value_generic(file_wrapper& fs, ply_format format,
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
    if (!read_line(fs, buffer, sizeof(buffer)))
      throw std::runtime_error("cannot read ply");
    auto line = string_view{buffer};
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop  = element.properties[pidx];
      auto& value = values[pidx];
      auto& list  = lists[pidx];
      if (!prop.is_list) {
        parse_ply_prop(line, prop.type, value);
      } else {
        parse_ply_prop(line, ply_type::u8, value);
        list.resize((int)value);
        for (auto i = 0; i < (int)value; i++)
          parse_ply_prop(line, prop.type, list[i]);
      }
    }
  } else {
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop  = element.properties[pidx];
      auto& value = values[pidx];
      auto& list  = lists[pidx];
      if (!prop.is_list) {
        read_ply_prop(
            fs, format == ply_format::binary_big_endian, prop.type, value);
      } else {
        read_ply_prop(
            fs, format == ply_format::binary_big_endian, ply_type::u8, value);
        list.resize((int)value);
        for (auto i = 0; i < (int)value; i++)
          read_ply_prop(
              fs, format == ply_format::binary_big_endian, prop.type, list[i]);
      }
    }
  }
}

template <typename VT>
static inline void write_ply_prop(file_wrapper& fs, ply_type type, VT value) {
  switch (type) {
    case ply_type::i8: checked_fprintf(fs, "%d", (int)value); break;
    case ply_type::i16: checked_fprintf(fs, "%d", (int)value); break;
    case ply_type::i32: checked_fprintf(fs, "%d", (int)value); break;
    case ply_type::i64: checked_fprintf(fs, "%lld", (long long)value); break;
    case ply_type::u8: checked_fprintf(fs, "%u", (unsigned)value); break;
    case ply_type::u16: checked_fprintf(fs, "%u", (unsigned)value); break;
    case ply_type::u32: checked_fprintf(fs, "%u", (unsigned)value); break;
    case ply_type::u64:
      checked_fprintf(fs, "%llu", (unsigned long long)value);
      break;
    case ply_type::f32: checked_fprintf(fs, "%g", (float)value); break;
    case ply_type::f64: checked_fprintf(fs, "%g", (double)value); break;
  }
}

template <typename T, typename VT>
static inline void write_ply_binprop(
    file_wrapper& fs, bool big_endian, VT value) {
  auto typed_value = (T)value;
  if (big_endian) typed_value = swap_endian(typed_value);
  if (fwrite(&typed_value, sizeof(T), 1, fs.fs) != 1)
    throw std::runtime_error("cannot write to file");
}

template <typename VT>
static inline void write_ply_binprop(
    file_wrapper& fs, bool big_endian, ply_type type, VT value) {
  switch (type) {
    case ply_type::i8: write_ply_binprop<int8_t>(fs, big_endian, value); break;
    case ply_type::i16:
      write_ply_binprop<int16_t>(fs, big_endian, value);
      break;
    case ply_type::i32:
      write_ply_binprop<int32_t>(fs, big_endian, value);
      break;
    case ply_type::i64:
      write_ply_binprop<int64_t>(fs, big_endian, value);
      break;
    case ply_type::u8: write_ply_binprop<uint8_t>(fs, big_endian, value); break;
    case ply_type::u16:
      write_ply_binprop<uint16_t>(fs, big_endian, value);
      break;
    case ply_type::u32:
      write_ply_binprop<uint32_t>(fs, big_endian, value);
      break;
    case ply_type::u64:
      write_ply_binprop<uint64_t>(fs, big_endian, value);
      break;
    case ply_type::f32: write_ply_binprop<float>(fs, big_endian, value); break;
    case ply_type::f64: write_ply_binprop<double>(fs, big_endian, value); break;
  }
}

// Write Ply functions
void write_ply_header(file_wrapper& fs, ply_format format,
    const vector<ply_element>& elements, const vector<string>& comments) {
  // ply type names
  static auto type_map = unordered_map<ply_type, string>{{ply_type::i8, "char"},
      {ply_type::i16, "short"}, {ply_type::i32, "int"}, {ply_type::i64, "uint"},
      {ply_type::u8, "uchar"}, {ply_type::u16, "ushort"},
      {ply_type::u32, "uint"}, {ply_type::u64, "ulong"},
      {ply_type::f32, "float"}, {ply_type::f64, "double"}};

  write_text(fs, "ply\n");
  switch (format) {
    case ply_format::ascii: write_text(fs, "format ascii 1.0\n"); break;
    case ply_format::binary_little_endian:
      write_text(fs, "format binary_little_endian 1.0\n");
      break;
    case ply_format::binary_big_endian:
      write_text(fs, "format binary_big_endian 1.0\n");
      break;
  }
  for (auto& comment : comments) write_text(fs, "comment " + comment + "\n");
  for (auto& elem : elements) {
    write_text(
        fs, "element " + elem.name + " " + std::to_string(elem.count) + "\n");
    for (auto& prop : elem.properties) {
      if (prop.is_list) {
        write_text(fs, "property list uchar " + type_map[prop.type] + " " +
                           prop.name + "\n");
      } else {
        write_text(
            fs, "property " + type_map[prop.type] + " " + prop.name + "\n");
      }
    }
  }
  write_text(fs, "end_header\n");
}

template <typename VT, typename LT>
void write_ply_value_generic(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<VT>& values, vector<vector<LT>>& lists) {
  if (format == ply_format::ascii) {
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop = element.properties[pidx];
      if (pidx) write_text(fs, " ");
      if (!prop.is_list) {
        write_ply_prop(fs, prop.type, values[pidx]);
      } else {
        write_ply_prop(fs, ply_type::u8, values[pidx]);
        for (auto i = 0; i < (int)lists[pidx].size(); i++) {
          if (i) write_text(fs, " ");
          write_ply_prop(fs, prop.type, lists[pidx][i]);
        }
      }
      write_text(fs, "\n");
    }
  } else {
    for (auto pidx = 0; pidx < element.properties.size(); pidx++) {
      auto& prop = element.properties[pidx];
      if (!prop.is_list) {
        write_ply_binprop(fs, format == ply_format::binary_big_endian,
            prop.type, values[pidx]);
      } else {
        write_ply_binprop(fs, format == ply_format::binary_big_endian,
            ply_type::u8, values[pidx]);
        for (auto i = 0; i < (int)lists[pidx].size(); i++)
          write_ply_binprop(fs, format == ply_format::binary_big_endian,
              prop.type, lists[pidx][i]);
      }
    }
  }
}

void write_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<double>& values,
    vector<vector<double>>& lists) {
  write_ply_value_generic(fs, format, element, values, lists);
}
void write_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<float>& values,
    vector<vector<int>>& lists) {
  write_ply_value_generic(fs, format, element, values, lists);
}

void read_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<double>& values,
    vector<vector<double>>& lists) {
  read_ply_value_generic(fs, format, element, values, lists);
}
void read_ply_value(file_wrapper& fs, ply_format format,
    const ply_element& element, vector<float>& values,
    vector<vector<int>>& lists) {
  read_ply_value_generic(fs, format, element, values, lists);
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

static inline void parse_value(string_view& str, obj_vertex& value) {
  value = obj_vertex{0, 0, 0};
  parse_value(str, value.position);
  if (!str.empty() && str.front() == '/') {
    str.remove_prefix(1);
    if (!str.empty() && str.front() == '/') {
      str.remove_prefix(1);
      parse_value(str, value.normal);
    } else {
      parse_value(str, value.texcoord);
      if (!str.empty() && str.front() == '/') {
        str.remove_prefix(1);
        parse_value(str, value.normal);
      }
    }
  }
}

// Input for OBJ textures
static inline void parse_value(string_view& str, obj_texture_info& info) {
  // initialize
  info = obj_texture_info();

  // get tokens
  auto tokens = vector<string>();
  skip_whitespace(str);
  while (!str.empty()) {
    auto token = ""s;
    parse_value(str, token);
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
}

// Read obj
void load_mtl(const string& filename, obj_model& obj, bool fliptr = true) {
  // open file
  auto fs = open_file(filename, "rt");

  // init parsing
  obj.materials.emplace_back();

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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // possible token values
    if (cmd == "newmtl") {
      obj.materials.emplace_back();
      parse_value(line, obj.materials.back().name);
    } else if (cmd == "illum") {
      parse_value(line, obj.materials.back().illum);
    } else if (cmd == "Ke") {
      parse_value(line, obj.materials.back().emission);
    } else if (cmd == "Ka") {
      parse_value(line, obj.materials.back().ambient);
    } else if (cmd == "Kd") {
      parse_value(line, obj.materials.back().diffuse);
    } else if (cmd == "Ks") {
      parse_value(line, obj.materials.back().specular);
    } else if (cmd == "Kt") {
      parse_value(line, obj.materials.back().transmission);
    } else if (cmd == "Tf") {
      obj.materials.back().transmission = vec3f{-1};
      parse_value(line, obj.materials.back().transmission);
      if (obj.materials.back().transmission.y < 0)
        obj.materials.back().transmission = vec3f{
            obj.materials.back().transmission.x};
      if (fliptr)
        obj.materials.back().transmission = 1 -
                                            obj.materials.back().transmission;
    } else if (cmd == "Tr") {
      parse_value(line, obj.materials.back().opacity);
      if (fliptr)
        obj.materials.back().opacity = 1 - obj.materials.back().opacity;
    } else if (cmd == "Ns") {
      parse_value(line, obj.materials.back().exponent);
    } else if (cmd == "d") {
      parse_value(line, obj.materials.back().opacity);
    } else if (cmd == "map_Ke") {
      parse_value(line, obj.materials.back().emission_map);
    } else if (cmd == "map_Ka") {
      parse_value(line, obj.materials.back().ambient_map);
    } else if (cmd == "map_Kd") {
      parse_value(line, obj.materials.back().diffuse_map);
    } else if (cmd == "map_Ks") {
      parse_value(line, obj.materials.back().specular_map);
    } else if (cmd == "map_Tr") {
      parse_value(line, obj.materials.back().transmission_map);
    } else if (cmd == "map_d" || cmd == "map_Tr") {
      parse_value(line, obj.materials.back().opacity_map);
    } else if (cmd == "map_bump" || cmd == "bump") {
      parse_value(line, obj.materials.back().bump_map);
    } else if (cmd == "map_disp" || cmd == "disp") {
      parse_value(line, obj.materials.back().displacement_map);
    } else if (cmd == "map_norm" || cmd == "norm") {
      parse_value(line, obj.materials.back().normal_map);
    } else if (cmd == "Pm") {
      parse_value(line, obj.materials.back().pbr_metallic);
    } else if (cmd == "Pr") {
      parse_value(line, obj.materials.back().pbr_roughness);
    } else if (cmd == "Ps") {
      parse_value(line, obj.materials.back().pbr_sheen);
    } else if (cmd == "Pc") {
      parse_value(line, obj.materials.back().pbr_clearcoat);
    } else if (cmd == "Pcr") {
      parse_value(line, obj.materials.back().pbr_coatroughness);
    } else if (cmd == "map_Pm") {
      parse_value(line, obj.materials.back().pbr_metallic_map);
    } else if (cmd == "map_Pr") {
      parse_value(line, obj.materials.back().pbr_roughness_map);
    } else if (cmd == "map_Ps") {
      parse_value(line, obj.materials.back().pbr_sheen_map);
    } else if (cmd == "map_Pc") {
      parse_value(line, obj.materials.back().pbr_clearcoat_map);
    } else if (cmd == "map_Pcr") {
      parse_value(line, obj.materials.back().pbr_coatroughness_map);
    } else if (cmd == "Vt") {
      parse_value(line, obj.materials.back().vol_transmission);
    } else if (cmd == "Vp") {
      parse_value(line, obj.materials.back().vol_meanfreepath);
    } else if (cmd == "Ve") {
      parse_value(line, obj.materials.back().vol_emission);
    } else if (cmd == "Vs") {
      parse_value(line, obj.materials.back().vol_scattering);
    } else if (cmd == "Vg") {
      parse_value(line, obj.materials.back().vol_anisotropy);
    } else if (cmd == "Vr") {
      parse_value(line, obj.materials.back().vol_scale);
    } else if (cmd == "map_Vs") {
      parse_value(line, obj.materials.back().vol_scattering_map);
    } else {
      continue;
    }
  }

  // remove placeholder material
  obj.materials.erase(obj.materials.begin());
}

// Read obj
void load_objx(const string& filename, obj_model& obj) {
  // open file
  auto fs = open_file(filename, "rt");

  // initialize commands
  obj.cameras.emplace_back();
  obj.environments.emplace_back();
  obj.instances.emplace_back();
  obj.procedurals.emplace_back();

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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // read values
    if (cmd == "newcam") {
      obj.cameras.emplace_back();
      parse_value(line, obj.cameras.back().name);
    } else if (cmd == "Cframe") {
      parse_value(line, obj.cameras.back().frame);
    } else if (cmd == "Cortho") {
      parse_value(line, obj.cameras.back().ortho);
    } else if (cmd == "Cwidth") {
      parse_value(line, obj.cameras.back().width);
    } else if (cmd == "Cheight") {
      parse_value(line, obj.cameras.back().height);
    } else if (cmd == "Clens") {
      parse_value(line, obj.cameras.back().lens);
    } else if (cmd == "Cfocus") {
      parse_value(line, obj.cameras.back().focus);
    } else if (cmd == "Caperture") {
      parse_value(line, obj.cameras.back().aperture);
    } else if (cmd == "newenv") {
      obj.environments.emplace_back();
      parse_value(line, obj.environments.back().name);
    } else if (cmd == "Eframe") {
      parse_value(line, obj.environments.back().frame);
    } else if (cmd == "Ee") {
      parse_value(line, obj.environments.back().emission);
    } else if (cmd == "map_Ee") {
      parse_value(line, obj.environments.back().emission_map);
    } else if (cmd == "newist") {
      obj.instances.emplace_back();
      parse_value(line, obj.instances.back().name);
    } else if (cmd == "Iframe") {
      parse_value(line, obj.instances.back().frame);
    } else if (cmd == "Iobj") {
      parse_value(line, obj.instances.back().object);
    } else if (cmd == "Imat") {
      parse_value(line, obj.instances.back().material);
    } else if (cmd == "newproc") {
      obj.procedurals.emplace_back();
      parse_value(line, obj.procedurals.back().name);
    } else if (cmd == "Pframe") {
      parse_value(line, obj.procedurals.back().frame);
    } else if (cmd == "Ptype") {
      parse_value(line, obj.procedurals.back().type);
    } else if (cmd == "Pmat") {
      parse_value(line, obj.procedurals.back().material);
    } else if (cmd == "Psize") {
      parse_value(line, obj.procedurals.back().size);
    } else if (cmd == "Plevel") {
      parse_value(line, obj.procedurals.back().level);
    }
    // backward compatibility
    else if (cmd == "c") {
      auto& camera = obj.cameras.emplace_back();
      parse_value(line, camera.name);
      parse_value(line, camera.ortho);
      parse_value(line, camera.width);
      parse_value(line, camera.height);
      parse_value(line, camera.lens);
      parse_value(line, camera.focus);
      parse_value(line, camera.aperture);
      parse_value(line, camera.frame);
    } else if (cmd == "e") {
      auto& environment = obj.environments.emplace_back();
      parse_value(line, environment.name);
      parse_value(line, environment.emission);
      parse_value(line, environment.emission_map);
      parse_value(line, environment.frame);
    } else if (cmd == "i") {
      auto& instance = obj.instances.emplace_back();
      parse_value(line, instance.name);
      parse_value(line, instance.object);
      parse_value(line, instance.material);
      parse_value(line, instance.frame);
    } else if (cmd == "po") {
      auto& procedural = obj.procedurals.emplace_back();
      parse_value(line, procedural.name);
      parse_value(line, procedural.type);
      parse_value(line, procedural.material);
      parse_value(line, procedural.size);
      parse_value(line, procedural.level);
      parse_value(line, procedural.frame);
    } else {
      // unused
    }
  }

  // cleanup unused
  obj.cameras.erase(obj.cameras.begin());
  obj.environments.erase(obj.environments.begin());
  obj.instances.erase(obj.instances.begin());
  obj.procedurals.erase(obj.procedurals.begin());
}

// Read obj
void load_obj(const string& filename, obj_model& obj, bool geom_only,
    bool split_elements, bool split_materials) {
  // open file
  auto fs = open_file(filename, "rt");

  // parsing state
  auto vert_size = obj_vertex{};
  auto oname     = ""s;
  auto gname     = ""s;
  auto mname     = ""s;
  auto mtllibs   = vector<string>{};

  // initialize obj
  obj = {};
  obj.shapes.emplace_back();

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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // possible token values
    if (cmd == "v") {
      parse_value(line, obj.positions.emplace_back());
      vert_size.position += 1;
    } else if (cmd == "vn") {
      parse_value(line, obj.normals.emplace_back());
      vert_size.normal += 1;
    } else if (cmd == "vt") {
      parse_value(line, obj.texcoords.emplace_back());
      vert_size.texcoord += 1;
    } else if (cmd == "f" || cmd == "l" || cmd == "p") {
      // split if split_elements and different primitives
      if (auto& shape = obj.shapes.back();
          split_elements && !shape.vertices.empty()) {
        if ((cmd == "f" && (!shape.lines.empty() || !shape.points.empty())) ||
            (cmd == "l" && (!shape.faces.empty() || !shape.points.empty())) ||
            (cmd == "p" && (!shape.faces.empty() || !shape.lines.empty()))) {
          obj.shapes.emplace_back();
          obj.shapes.back().name = oname + gname;
        }
      }
      // split if splt_material and different materials
      if (auto& shape = obj.shapes.back();
          !geom_only && split_materials && !shape.materials.empty()) {
        if (shape.materials.size() > 1)
          throw std::runtime_error("should not have happened");
        if (shape.materials.back() != mname) {
          obj.shapes.emplace_back();
          obj.shapes.back().name = oname + gname;
        }
      }
      // grab shape and add element
      auto& shape   = obj.shapes.back();
      auto& element = (cmd == "f") ? shape.faces.emplace_back()
                                   : (cmd == "l") ? shape.lines.emplace_back()
                                                  : shape.points.emplace_back();
      // get element material or add if needed
      if (!geom_only) {
        auto mat_idx = -1;
        for (auto midx = 0; midx < shape.materials.size(); midx++)
          if (shape.materials[midx] == mname) mat_idx = midx;
        if (mat_idx < 0) {
          shape.materials.push_back(mname);
          mat_idx = shape.materials.size() - 1;
        }
        element.material = (uint8_t)mat_idx;
      }
      // parse vertices
      skip_whitespace(line);
      while (!line.empty()) {
        auto vert = obj_vertex{};
        parse_value(line, vert);
        if (!vert.position) break;
        if (vert.position < 0)
          vert.position = vert_size.position + vert.position + 1;
        if (vert.texcoord < 0)
          vert.texcoord = vert_size.texcoord + vert.texcoord + 1;
        if (vert.normal < 0) vert.normal = vert_size.normal + vert.normal + 1;
        shape.vertices.push_back(vert);
        element.size += 1;
        skip_whitespace(line);
      }
    } else if (cmd == "o" || cmd == "g") {
      if (geom_only) continue;
      parse_value_or_empty(line, cmd == "o" ? oname : gname);
      if (!obj.shapes.back().vertices.empty()) {
        obj.shapes.emplace_back();
      } else {
        obj.shapes.back().name = oname + gname;
      }
    } else if (cmd == "usemtl") {
      if (geom_only) continue;
      parse_value_or_empty(line, mname);
    } else if (cmd == "s") {
      if (geom_only) continue;
      // TODO: smoothing
    } else if (cmd == "mtllib") {
      if (geom_only) continue;
      auto mtllib = ""s;
      parse_value(line, mtllib);
      if (std::find(mtllibs.begin(), mtllibs.end(), mtllib) == mtllibs.end()) {
        mtllibs.push_back(mtllib);
      }
    } else {
      // unused
    }
  }

  // exit if done
  if (geom_only) return;

  // load materials
  auto dirname = get_dirname(filename);
  for (auto& mtllib : mtllibs) {
    load_mtl(dirname + mtllib, obj);
  }

  // load extensions
  auto extfilename = replace_extension(filename, ".objx");
  if (exists_file(extfilename)) {
    load_objx(extfilename, obj);
  }
}

// Format values
static inline void format_value(string& str, const obj_texture_info& value) {
  str += value.path.empty() ? "" : value.path;
}
static inline void format_value(string& str, const obj_vertex& value) {
  format_value(str, value.position);
  if (value.texcoord) {
    str += "/";
    format_value(str, value.texcoord);
    if (value.normal) {
      str += "/";
      format_value(str, value.normal);
    }
  } else if (value.normal) {
    str += "//";
    format_value(str, value.normal);
  }
}

// Save obj
void save_mtl(const string& filename, const obj_model& obj) {
  // open file
  auto fs = open_file(filename, "wt");

  // save comments
  format_values(fs, "#\n");
  format_values(fs, "# Written by Yocto/GL\n");
  format_values(fs, "# https://github.com/xelatihy/yocto-gl\n");
  format_values(fs, "#\n\n");
  for (auto& comment : obj.comments) {
    format_values(fs, "# {}\n", comment);
  }
  format_values(fs, "\n");

  // write material
  for (auto& material : obj.materials) {
    format_values(fs, "newmtl {}\n", material.name);
    format_values(fs, "illum {}\n", material.illum);
    if (material.emission != zero3f)
      format_values(fs, "Ke {}\n", material.emission);
    if (material.ambient != zero3f)
      format_values(fs, "Ka {}\n", material.ambient);
    format_values(fs, "Kd {}\n", material.diffuse);
    format_values(fs, "Ks {}\n", material.specular);
    if (material.reflection != zero3f)
      format_values(fs, "Kr {}\n", material.reflection);
    if (material.transmission != zero3f)
      format_values(fs, "Kt {}\n", material.transmission);
    format_values(fs, "Ns {}\n", (int)material.exponent);
    if (material.opacity != 1) format_values(fs, "d {}\n", material.opacity);
    if (!material.emission_map.path.empty())
      format_values(fs, "map_Ke {}\n", material.emission_map);
    if (!material.diffuse_map.path.empty())
      format_values(fs, "map_Kd {}\n", material.diffuse_map);
    if (!material.specular_map.path.empty())
      format_values(fs, "map_Ks {}\n", material.specular_map);
    if (!material.transmission_map.path.empty())
      format_values(fs, "map_Kt {}\n", material.transmission_map);
    if (!material.reflection_map.path.empty())
      format_values(fs, "map_Kr {}\n", material.reflection_map);
    if (!material.exponent_map.path.empty())
      format_values(fs, "map_Ns {}\n", material.exponent_map);
    if (!material.opacity_map.path.empty())
      format_values(fs, "map_d {}\n", material.opacity_map);
    if (!material.bump_map.path.empty())
      format_values(fs, "map_bump {}\n", material.bump_map);
    if (!material.displacement_map.path.empty())
      format_values(fs, "map_disp {}\n", material.displacement_map);
    if (!material.normal_map.path.empty())
      format_values(fs, "map_norm {}\n", material.normal_map);
    if (material.pbr_roughness)
      format_values(fs, "Pr {}\n", material.pbr_roughness);
    if (material.pbr_metallic)
      format_values(fs, "Pm {}\n", material.pbr_metallic);
    if (material.pbr_sheen) format_values(fs, "Ps {}\n", material.pbr_sheen);
    if (material.pbr_clearcoat)
      format_values(fs, "Pc {}\n", material.pbr_clearcoat);
    if (material.pbr_coatroughness)
      format_values(fs, "Pcr {}\n", material.pbr_coatroughness);
    if (!material.pbr_roughness_map.path.empty())
      format_values(fs, "map_Pr {}\n", material.pbr_roughness_map);
    if (!material.pbr_metallic_map.path.empty())
      format_values(fs, "map_Pm {}\n", material.pbr_metallic_map);
    if (!material.pbr_sheen_map.path.empty())
      format_values(fs, "map_Ps {}\n", material.pbr_sheen_map);
    if (!material.pbr_clearcoat_map.path.empty())
      format_values(fs, "map_Pc {}\n", material.pbr_clearcoat_map);
    if (!material.pbr_coatroughness_map.path.empty())
      format_values(fs, "map_Pcr {}\n", material.pbr_coatroughness_map);
    if (material.vol_transmission != zero3f)
      format_values(fs, "Vt {}\n", material.vol_transmission);
    if (material.vol_meanfreepath != zero3f)
      format_values(fs, "Vp {}\n", material.vol_meanfreepath);
    if (material.vol_emission != zero3f)
      format_values(fs, "Ve {}\n", material.vol_emission);
    if (material.vol_scattering != zero3f)
      format_values(fs, "Vs {}\n", material.vol_scattering);
    if (material.vol_anisotropy)
      format_values(fs, "Vg {}\n", material.vol_anisotropy);
    if (material.vol_scale) format_values(fs, "Vr {}\n", material.vol_scale);
    if (!material.vol_scattering_map.path.empty())
      format_values(fs, "map_Vs {}\n", material.vol_scattering_map);
    format_values(fs, "\n");
  }
}

// Save obj
void save_objx(const string& filename, const obj_model& obj) {
  // open file
  auto fs = open_file(filename, "wt");

  // save comments
  format_values(fs, "#\n");
  format_values(fs, "# Written by Yocto/GL\n");
  format_values(fs, "# https://github.com/xelatihy/yocto-gl\n");
  format_values(fs, "#\n\n");
  for (auto& comment : obj.comments) {
    format_values(fs, "# {}\n", comment);
  }
  format_values(fs, "\n");

  // cameras
  for (auto& camera : obj.cameras) {
    format_values(fs, "newcam {}\n", camera.name);
    format_values(fs, "Cframe {}\n", camera.frame);
    format_values(fs, "Cortho {}\n", camera.ortho);
    format_values(fs, "Cwidth {}\n", camera.width);
    format_values(fs, "Cheight {}\n", camera.height);
    format_values(fs, "Clens {}\n", camera.lens);
    format_values(fs, "Cfocus {}\n", camera.focus);
    format_values(fs, "Caperture {}\n", camera.aperture);
    format_values(fs, "\n");
  }

  // environments
  for (auto& environment : obj.environments) {
    format_values(fs, "newenv {}\n", environment.name);
    format_values(fs, "Eframe {}\n", environment.frame);
    format_values(fs, "Ee {}\n", environment.emission);
    if (!environment.emission_map.path.empty())
      format_values(fs, "map_Ee {}\n", environment.emission_map);
    format_values(fs, "\n");
  }

  // instances
  for (auto& instance : obj.instances) {
    format_values(fs, "newist {}\n", instance.name);
    format_values(fs, "Iframe {}\n", instance.frame);
    format_values(fs, "Iobj {}\n", instance.object);
    format_values(fs, "Imat {}\n", instance.material);
    format_values(fs, "\n");
  }

  // procedurals
  for (auto& procedural : obj.procedurals) {
    format_values(fs, "newist {}\n", procedural.name);
    format_values(fs, "Pframe {}\n", procedural.frame);
    format_values(fs, "Ptype {}\n", procedural.type);
    format_values(fs, "Pmat {}\n", procedural.material);
    format_values(fs, "Psize {}\n", procedural.size);
    format_values(fs, "Plevel {}\n", procedural.level);
    format_values(fs, "\n");
  }
}

// Save obj
void save_obj(const string& filename, const obj_model& obj) {
  // open file
  auto fs = open_file(filename, "wt");

  // save comments
  format_values(fs, "#\n");
  format_values(fs, "# Written by Yocto/GL\n");
  format_values(fs, "# https://github.com/xelatihy/yocto-gl\n");
  format_values(fs, "#\n\n");
  for (auto& comment : obj.comments) {
    format_values(fs, "# {}\n", comment);
  }
  format_values(fs, "\n");

  // save material library
  if (!obj.materials.empty()) {
    format_values(
        fs, "mtllib {}\n\n", replace_extension(get_filename(filename), ".mtl"));
  }

  // save vertices
  for (auto& p : obj.positions) format_values(fs, "v {}\n", p);
  for (auto& n : obj.normals) format_values(fs, "vn {}\n", n);
  for (auto& t : obj.texcoords) format_values(fs, "vt {}\n", t);

  // save objects
  for (auto& shape : obj.shapes) {
    format_values(fs, "o {}\n", shape.name.c_str());
    auto element_labels = vector<string>{"f", "l", "p"};
    auto element_groups = vector<const vector<obj_element>*>{
        &shape.faces, &shape.lines, &shape.points};
    for (auto element_idx = 0; element_idx < 3; element_idx++) {
      auto& label        = element_labels[element_idx];
      auto& elements     = *element_groups[element_idx];
      auto  cur_material = -1, cur_vertex = 0;
      for (auto& element : elements) {
        if (!shape.materials.empty() && cur_material != element.material) {
          format_values(fs, "usemtl {}\n", shape.materials[element.material]);
          cur_material = element.material;
        }
        format_values(fs, "{}", label);
        for (auto c = 0; c < element.size; c++) {
          format_values(fs, " {}", shape.vertices[cur_vertex++]);
        }
        format_values(fs, "\n");
      }
    }
    format_values(fs, "\n");
  }

  // save mtl
  if (!obj.materials.empty())
    save_mtl(replace_extension(filename, ".mtl"), obj);

  // save objx
  if (!obj.cameras.empty() || !obj.environments.empty() ||
      !obj.instances.empty() || !obj.procedurals.empty())
    save_objx(replace_extension(filename, ".objx"), obj);
}

// convert between roughness and exponent
float obj_exponent_to_roughness(float exponent) {
  auto roughness = exponent;
  roughness      = pow(2 / (roughness + 2), 1 / 4.0f);
  if (roughness < 0.01f) roughness = 0;
  if (roughness > 0.99f) roughness = 1;
  return roughness;
}
float obj_roughness_to_exponent(float roughness) {
  return (int)clamp(
      2 / pow(clamp(roughness, 0.0f, 0.99f) + 1e-10f, 4.0f) - 2, 0.0f, 1.0e9f);
}

// Get obj vertices
void get_obj_vertices(const obj_model& obj, const obj_shape& shape,
    vector<vec3f>& positions, vector<vec3f>& normals, vector<vec2f>& texcoords,
    vector<int>& vindex, bool flip_texcoord) {
  auto vmap = unordered_map<obj_vertex, int>{};
  vmap.reserve(shape.vertices.size());
  vindex.reserve(shape.vertices.size());
  for (auto& vert : shape.vertices) {
    auto it = vmap.find(vert);
    if (it != vmap.end()) {
      vindex.push_back(it->second);
      continue;
    }
    auto nverts = (int)positions.size();
    vindex.push_back(nverts);
    vmap.insert(it, {vert, nverts});
    if (!obj.positions.empty() && vert.position)
      positions.push_back(obj.positions[vert.position - 1]);
    if (!obj.normals.empty() && vert.normal)
      normals.push_back(obj.normals[vert.normal - 1]);
    if (!obj.texcoords.empty() && vert.texcoord)
      texcoords.push_back(obj.texcoords[vert.texcoord - 1]);
  }
  if (flip_texcoord) {
    for (auto& texcoord : texcoords) texcoord.y = 1 - texcoord.y;
  }
}

// Get obj vertices
void get_obj_fvvertices(const obj_model& obj, const obj_shape& shape,
    vector<vec3f>& positions, vector<vec3f>& normals, vector<vec2f>& texcoords,
    vector<int>& pindex, vector<int>& nindex, vector<int>& tindex,
    bool flip_texcoord) {
  if (!obj.positions.empty() && shape.vertices[0].position) {
    auto pmap = unordered_map<int, int>{};
    pmap.reserve(shape.vertices.size());
    pindex.reserve(shape.vertices.size());
    for (auto& vert : shape.vertices) {
      auto it = pmap.find(vert.position);
      if (it != pmap.end()) {
        pindex.push_back(it->second);
        continue;
      }
      auto nverts = (int)positions.size();
      pindex.push_back(nverts);
      pmap.insert(it, {vert.position, nverts});
      positions.push_back(obj.positions[vert.position - 1]);
    }
  }
  if (!obj.normals.empty() && shape.vertices[0].normal) {
    auto nmap = unordered_map<int, int>{};
    nmap.reserve(shape.vertices.size());
    nindex.reserve(shape.vertices.size());
    for (auto& vert : shape.vertices) {
      auto it = nmap.find(vert.normal);
      if (it != nmap.end()) {
        nindex.push_back(it->second);
        continue;
      }
      auto nverts = (int)normals.size();
      nindex.push_back(nverts);
      nmap.insert(it, {vert.normal, nverts});
      normals.push_back(obj.normals[vert.normal - 1]);
    }
  }
  if (!obj.texcoords.empty() && shape.vertices[0].texcoord) {
    auto tmap = unordered_map<int, int>{};
    tmap.reserve(shape.vertices.size());
    tindex.reserve(shape.vertices.size());
    for (auto& vert : shape.vertices) {
      auto it = tmap.find(vert.texcoord);
      if (it != tmap.end()) {
        tindex.push_back(it->second);
        continue;
      }
      auto nverts = (int)texcoords.size();
      tindex.push_back(nverts);
      tmap.insert(it, {vert.texcoord, nverts});
      texcoords.push_back(obj.texcoords[vert.texcoord - 1]);
    }
  }
  if (flip_texcoord) {
    for (auto& texcoord : texcoords) texcoord.y = 1 - texcoord.y;
  }
}

// Get obj shape
void get_obj_triangles(const obj_model& obj, const obj_shape& shape,
    vector<vec3i>& triangles, vector<vec3f>& positions, vector<vec3f>& normals,
    vector<vec2f>& texcoords, vector<string>& materials,
    vector<int>& ematerials, bool flip_texcoord) {
  if (shape.faces.empty()) return;
  auto vindex = vector<int>{};
  get_obj_vertices(
      obj, shape, positions, normals, texcoords, vindex, flip_texcoord);
  materials = shape.materials;
  triangles.reserve(shape.faces.size());
  if (!materials.empty()) ematerials.reserve(shape.faces.size());
  auto cur = 0;
  for (auto& face : shape.faces) {
    for (auto c = 2; c < face.size; c++) {
      triangles.push_back(
          {vindex[cur + 0], vindex[cur + c - 1], vindex[cur + c]});
      if (!materials.empty()) ematerials.push_back(face.material);
    }
    cur += face.size;
  }
}
void get_obj_quads(const obj_model& obj, const obj_shape& shape,
    vector<vec4i>& quads, vector<vec3f>& positions, vector<vec3f>& normals,
    vector<vec2f>& texcoords, vector<string>& materials,
    vector<int>& ematerials, bool flip_texcoord) {
  if (shape.faces.empty()) return;
  auto vindex = vector<int>{};
  get_obj_vertices(
      obj, shape, positions, normals, texcoords, vindex, flip_texcoord);
  materials = shape.materials;
  quads.reserve(shape.faces.size());
  if (!materials.empty()) ematerials.reserve(shape.faces.size());
  auto cur = 0;
  for (auto& face : shape.faces) {
    if (face.size == 4) {
      quads.push_back(
          {vindex[cur + 0], vindex[cur + 1], vindex[cur + 2], vindex[cur + 3]});
      if (!materials.empty()) ematerials.push_back(face.material);
    } else {
      for (auto c = 2; c < face.size; c++) {
        quads.push_back({vindex[cur + 0], vindex[cur + c - 1], vindex[cur + c],
            vindex[cur + c]});
        if (!materials.empty()) ematerials.push_back(face.material);
      }
    }
    cur += face.size;
  }
}
void get_obj_lines(const obj_model& obj, const obj_shape& shape,
    vector<vec2i>& lines, vector<vec3f>& positions, vector<vec3f>& normals,
    vector<vec2f>& texcoords, vector<string>& materials,
    vector<int>& ematerials, bool flip_texcoord) {
  if (shape.lines.empty()) return;
  auto vindex = vector<int>{};
  get_obj_vertices(
      obj, shape, positions, normals, texcoords, vindex, flip_texcoord);
  materials = shape.materials;
  lines.reserve(shape.lines.size());
  if (!materials.empty()) ematerials.reserve(shape.faces.size());
  auto cur = 0;
  for (auto& line : shape.lines) {
    for (auto c = 1; c < line.size; c++) {
      lines.push_back({vindex[cur + c - 1], vindex[cur + c]});
      if (!materials.empty()) ematerials.push_back(line.material);
    }
    cur += line.size;
  }
}
void get_obj_points(const obj_model& obj, const obj_shape& shape,
    vector<int>& points, vector<vec3f>& positions, vector<vec3f>& normals,
    vector<vec2f>& texcoords, vector<string>& materials,
    vector<int>& ematerials, bool flip_texcoord) {
  if (shape.points.empty()) return;
  auto vindex = vector<int>{};
  get_obj_vertices(
      obj, shape, positions, normals, texcoords, vindex, flip_texcoord);
  materials = shape.materials;
  points.reserve(shape.points.size());
  if (!materials.empty()) ematerials.reserve(shape.faces.size());
  auto cur = 0;
  for (auto& point : shape.points) {
    for (auto c = 0; c < point.size; c++) {
      points.push_back({vindex[cur + 0]});
      if (!materials.empty()) ematerials.push_back(point.material);
    }
    cur += point.size;
  }
}
void get_obj_fvquads(const obj_model& obj, const obj_shape& shape,
    vector<vec4i>& quadspos, vector<vec4i>& quadsnorm,
    vector<vec4i>& quadstexcoord, vector<vec3f>& positions,
    vector<vec3f>& normals, vector<vec2f>& texcoords, vector<string>& materials,
    vector<int>& ematerials, bool flip_texcoord) {
  if (shape.faces.empty()) return;
  auto pindex = vector<int>{}, nindex = vector<int>{}, tindex = vector<int>{};
  get_obj_fvvertices(obj, shape, positions, normals, texcoords, pindex, nindex,
      tindex, flip_texcoord);
  materials = shape.materials;
  if (shape.vertices[0].position >= 0) quadspos.reserve(shape.faces.size());
  if (shape.vertices[0].normal >= 0) quadsnorm.reserve(shape.faces.size());
  if (shape.vertices[0].texcoord >= 0)
    quadstexcoord.reserve(shape.faces.size());
  if (!materials.empty()) ematerials.reserve(shape.faces.size());
  auto cur = 0;
  for (auto& face : shape.faces) {
    if (face.size == 4) {
      if (shape.vertices[0].position >= 0)
        quadspos.push_back({pindex[cur + 0], pindex[cur + 1], pindex[cur + 2],
            pindex[cur + 3]});
      if (shape.vertices[0].normal >= 0)
        quadsnorm.push_back({nindex[cur + 0], nindex[cur + 1], nindex[cur + 2],
            nindex[cur + 3]});
      if (shape.vertices[0].texcoord >= 0)
        quadstexcoord.push_back({tindex[cur + 0], tindex[cur + 1],
            tindex[cur + 2], tindex[cur + 3]});
      if (!materials.empty()) ematerials.push_back(face.material);
    } else {
      for (auto c = 2; c < face.size; c++) {
        if (shape.vertices[0].position >= 0)
          quadspos.push_back({pindex[cur + 0], pindex[cur + c - 1],
              pindex[cur + c], pindex[cur + c]});
        if (shape.vertices[0].normal >= 0)
          quadsnorm.push_back({nindex[cur + 0], nindex[cur + c - 1],
              nindex[cur + c], nindex[cur + c]});
        if (shape.vertices[0].texcoord >= 0)
          quadstexcoord.push_back({tindex[cur + 0], tindex[cur + c - 1],
              tindex[cur + c], tindex[cur + c]});
        if (!materials.empty()) ematerials.push_back(face.material);
      }
    }
    cur += face.size;
  }
}

bool has_obj_quads(const obj_shape& shape) {
  for (auto& face : shape.faces)
    if (face.size == 4) return true;
  return false;
}

// Add obj vertices
vector<obj_vertex> add_obj_vertices(obj_model& obj,
    const vector<vec3f>& positions, const vector<vec3f>& normals,
    const vector<vec2f>& texcoords) {
  auto vert_size = obj_vertex{(int)obj.positions.size(),
      (int)obj.texcoords.size(), (int)obj.normals.size()};
  obj.positions.insert(obj.positions.end(), positions.begin(), positions.end());
  obj.normals.insert(obj.normals.end(), normals.begin(), normals.end());
  obj.texcoords.insert(obj.texcoords.end(), texcoords.begin(), texcoords.end());
  auto vertices = vector<obj_vertex>(positions.size());
  for (auto idx = 0; idx < vertices.size(); idx++) {
    vertices[idx] = {
        positions.empty() ? 0 : vert_size.position + idx + 1,
        texcoords.empty() ? 0 : vert_size.texcoord + idx + 1,
        normals.empty() ? 0 : vert_size.normal + idx + 1,
    };
  }
  return vertices;
}

void add_obj_vertices(obj_model& obj, const vector<vec3f>& positions,
    const vector<vec3f>& normals, const vector<vec2f>& texcoords,
    bool flip_texcoord) {
  obj.positions.insert(obj.positions.end(), positions.begin(), positions.end());
  obj.normals.insert(obj.normals.end(), normals.begin(), normals.end());
  if (flip_texcoord) {
    auto flipped = vector<vec2f>(texcoords.size());
    for (auto idx = 0; idx < texcoords.size(); idx++)
      flipped[idx] = {texcoords[idx].x, 1 - texcoords[idx].y};
    obj.texcoords.insert(obj.texcoords.end(), flipped.begin(), flipped.end());
  } else {
    obj.texcoords.insert(
        obj.texcoords.end(), texcoords.begin(), texcoords.end());
  }
}

// Add obj shape
void add_obj_triangles(obj_model& obj, obj_shape& shape,
    const vector<vec3i>& triangles, const vector<vec3f>& positions,
    const vector<vec3f>& normals, const vector<vec2f>& texcoords,
    const vector<int>& ematerials, bool flip_texcoord) {
  auto vert_size = obj_vertex{(int)obj.positions.size(),
      (int)obj.texcoords.size(), (int)obj.normals.size()};
  add_obj_vertices(obj, positions, normals, texcoords, flip_texcoord);
  for (auto idx = 0; idx < triangles.size(); idx++) {
    auto& triangle = triangles[idx];
    for (auto c = 0; c < 3; c++) {
      shape.vertices.push_back({
          positions.empty() ? 0 : triangle[c] + vert_size.position + 1,
          texcoords.empty() ? 0 : triangle[c] + vert_size.texcoord + 1,
          normals.empty() ? 0 : triangle[c] + vert_size.normal + 1,
      });
    }
    shape.faces.push_back(
        {3, ematerials.empty() ? (uint8_t)0 : (uint8_t)ematerials[idx]});
  }
}
void add_obj_quads(obj_model& obj, obj_shape& shape, const vector<vec4i>& quads,
    const vector<vec3f>& positions, const vector<vec3f>& normals,
    const vector<vec2f>& texcoords, const vector<int>& ematerials,
    bool flip_texcoord) {
  auto vert_size = obj_vertex{(int)obj.positions.size(),
      (int)obj.texcoords.size(), (int)obj.normals.size()};
  add_obj_vertices(obj, positions, normals, texcoords, flip_texcoord);
  shape.vertices.reserve(quads.size() * 4);
  for (auto idx = 0; idx < quads.size(); idx++) {
    auto& quad = quads[idx];
    for (auto c = 0; c < (quad.z == quad.w ? 3 : 4); c++) {
      shape.vertices.push_back({
          positions.empty() ? 0 : quad[c] + vert_size.position + 1,
          texcoords.empty() ? 0 : quad[c] + vert_size.texcoord + 1,
          normals.empty() ? 0 : quad[c] + vert_size.normal + 1,
      });
    }
    shape.faces.push_back({quad.z == quad.w ? (uint8_t)3 : (uint8_t)4,
        ematerials.empty() ? (uint8_t)0 : (uint8_t)ematerials[idx]});
  }
}
void add_obj_lines(obj_model& obj, obj_shape& shape, const vector<vec2i>& lines,
    const vector<vec3f>& positions, const vector<vec3f>& normals,
    const vector<vec2f>& texcoords, const vector<int>& ematerials,
    bool flip_texcoord) {
  auto vert_size = obj_vertex{(int)obj.positions.size(),
      (int)obj.texcoords.size(), (int)obj.normals.size()};
  add_obj_vertices(obj, positions, normals, texcoords, flip_texcoord);
  shape.vertices.reserve(lines.size() * 2);
  for (auto idx = 0; idx < lines.size(); idx++) {
    auto& line = lines[idx];
    for (auto c = 0; c < 2; c++) {
      shape.vertices.push_back({
          positions.empty() ? 0 : line[c] + vert_size.position + 1,
          texcoords.empty() ? 0 : line[c] + vert_size.texcoord + 1,
          normals.empty() ? 0 : line[c] + vert_size.normal + 1,
      });
    }
    shape.lines.push_back(
        {2, ematerials.empty() ? (uint8_t)0 : (uint8_t)ematerials[idx]});
  }
}
void add_obj_points(obj_model& obj, obj_shape& shape, const vector<int>& points,
    const vector<vec3f>& positions, const vector<vec3f>& normals,
    const vector<vec2f>& texcoords, const vector<int>& ematerials,
    bool flip_texcoord) {
  auto vert_size = obj_vertex{(int)obj.positions.size(),
      (int)obj.texcoords.size(), (int)obj.normals.size()};
  add_obj_vertices(obj, positions, normals, texcoords, flip_texcoord);
  shape.vertices.reserve(points.size());
  for (auto idx = 0; idx < points.size(); idx++) {
    auto& point = points[idx];
    shape.vertices.push_back({
        positions.empty() ? 0 : point + vert_size.position + 1,
        texcoords.empty() ? 0 : point + vert_size.texcoord + 1,
        normals.empty() ? 0 : point + vert_size.normal + 1,
    });
    shape.faces.push_back(
        {1, ematerials.empty() ? (uint8_t)0 : (uint8_t)ematerials[idx]});
  }
}
void add_obj_fvquads(obj_model& obj, obj_shape& shape,
    const vector<vec4i>& quadspos, const vector<vec4i>& quadsnorm,
    const vector<vec4i>& quadstexcoord, const vector<vec3f>& positions,
    const vector<vec3f>& normals, const vector<vec2f>& texcoords,
    const vector<int>& ematerials, bool flip_texcoord) {
  auto vert_size = obj_vertex{(int)obj.positions.size(),
      (int)obj.texcoords.size(), (int)obj.normals.size()};
  add_obj_vertices(obj, positions, normals, texcoords, flip_texcoord);
  shape.vertices.reserve(quadspos.size() * 4);
  for (auto idx = 0; idx < quadspos.size(); idx++) {
    for (auto c = 0; c < (quadspos[idx].z == quadspos[idx].w ? 3 : 4); c++) {
      shape.vertices.push_back({
          quadspos.empty() ? 0 : quadspos[idx][c] + vert_size.position + 1,
          quadstexcoord.empty()
              ? 0
              : quadstexcoord[idx][c] + vert_size.texcoord + 1,
          quadsnorm.empty() ? 0 : quadsnorm[idx][c] + vert_size.normal + 1,
      });
    }
    shape.faces.push_back(
        {quadspos[idx].z == quadspos[idx].w ? (uint8_t)3 : (uint8_t)4,
            ematerials.empty() ? (uint8_t)0 : (uint8_t)ematerials[idx]});
  }
}

void parse_value(string_view& str, obj_value& value, obj_value_type type,
    int array_size = 3) {
  switch (type) {
    case obj_value_type::number: {
      auto value_ = 0.0f;
      parse_value(str, value_);
      value = make_obj_value(value_);
    } break;
    case obj_value_type::string: {
      auto value_ = ""s;
      parse_value(str, value_);
      value = make_obj_value(value_);
    } break;
    case obj_value_type::array: {
      if (array_size == 2) {
        auto value_ = zero2f;
        parse_value(str, value_);
        value = make_obj_value(value_);
      } else if (array_size == 3) {
        auto value_ = zero3f;
        parse_value(str, value_);
        value = make_obj_value(value_);
      } else if (array_size == 12) {
        auto value_ = identity3x4f;
        parse_value(str, value_);
        value = make_obj_value(value_);
      } else {
        throw std::runtime_error("should not have gotten here");
      }
    } break;
    case obj_value_type::boolean: {
      auto value_ = 0;
      parse_value(str, value_);
      value = make_obj_value((bool)value_);
    } break;
  }
}

static inline void parse_obj_value_or_empty(
    string_view& str, obj_value& value) {
  skip_whitespace(str);
  if (str.empty()) {
    value = make_obj_value(""s);
  } else {
    parse_value(str, value, obj_value_type::string);
  }
}

// Read obj
bool read_obj_command(file_wrapper& fs, obj_command& command, obj_value& value,
    vector<obj_vertex>& vertices, obj_vertex& vert_size) {
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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // possible token values
    if (cmd == "v") {
      command = obj_command::vertex;
      parse_value(line, value, obj_value_type::array);
      vert_size.position += 1;
      return true;
    } else if (cmd == "vn") {
      command = obj_command::normal;
      parse_value(line, value, obj_value_type::array);
      vert_size.normal += 1;
      return true;
    } else if (cmd == "vt") {
      command = obj_command::texcoord;
      parse_value(line, value, obj_value_type::array, 2);
      vert_size.texcoord += 1;
      return true;
    } else if (cmd == "f" || cmd == "l" || cmd == "p") {
      vertices.clear();
      skip_whitespace(line);
      while (!line.empty()) {
        auto vert = obj_vertex{};
        parse_value(line, vert);
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
      parse_obj_value_or_empty(line, value);
      return true;
    } else if (cmd == "usemtl") {
      command = obj_command::usemtl;
      parse_obj_value_or_empty(line, value);
      return true;
    } else if (cmd == "g") {
      command = obj_command::group;
      parse_obj_value_or_empty(line, value);
      return true;
    } else if (cmd == "s") {
      command = obj_command::smoothing;
      parse_obj_value_or_empty(line, value);
      return true;
    } else if (cmd == "mtllib") {
      command = obj_command::mtllib;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else {
      // unused
    }
  }
  return false;
}

// Read mtl
bool read_mtl_command(file_wrapper& fs, mtl_command& command, obj_value& value,
    obj_texture_info& texture, bool fliptr) {
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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // possible token values
    if (cmd == "newmtl") {
      command = mtl_command::material;
      parse_value(line, value, obj_value_type::string);
    } else if (cmd == "illum") {
      command = mtl_command::illum;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "Ke") {
      command = mtl_command::emission;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Kd") {
      command = mtl_command::diffuse;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Ks") {
      command = mtl_command::specular;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Kt") {
      command = mtl_command::transmission;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Tf") {
      command    = mtl_command::transmission;
      auto color = vec3f{-1};
      value      = make_obj_value(color);
      parse_value(line, value, obj_value_type::array);
      get_obj_value(value, color);
      if (color.y < 0) color = vec3f{color.x};
      if (fliptr) color = 1 - color;
      value = make_obj_value(color);
    } else if (cmd == "Tr") {
      command = mtl_command::opacity;
      parse_value(line, value, obj_value_type::number);
      if (fliptr) value.number = 1 - value.number;
    } else if (cmd == "Ns") {
      command = mtl_command::exponent;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "d") {
      command = mtl_command::opacity;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "map_Ke") {
      command = mtl_command::emission_map;
      parse_value(line, texture);
    } else if (cmd == "map_Kd") {
      command = mtl_command::diffuse_map;
      parse_value(line, texture);
    } else if (cmd == "map_Ks") {
      command = mtl_command::specular_map;
      parse_value(line, texture);
    } else if (cmd == "map_Tr") {
      command = mtl_command::transmission_map;
      parse_value(line, texture);
    } else if (cmd == "map_d" || cmd == "map_Tr") {
      command = mtl_command::opacity_map;
      parse_value(line, texture);
    } else if (cmd == "map_bump" || cmd == "bump") {
      command = mtl_command::bump_map;
      parse_value(line, texture);
    } else if (cmd == "map_disp" || cmd == "disp") {
      command = mtl_command::displacement_map;
      parse_value(line, texture);
    } else if (cmd == "map_norm" || cmd == "norm") {
      command = mtl_command::normal_map;
      parse_value(line, texture);
    } else if (cmd == "Pm") {
      command = mtl_command::pbr_metallic;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "Pr") {
      command = mtl_command::pbr_roughness;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "Ps") {
      command = mtl_command::pbr_sheen;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "Pc") {
      command = mtl_command::pbr_clearcoat;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "Pcr") {
      command = mtl_command::pbr_coatroughness;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "map_Pm") {
      command = mtl_command::pbr_metallic_map;
      parse_value(line, texture);
    } else if (cmd == "map_Pr") {
      command = mtl_command::pbr_roughness_map;
      parse_value(line, texture);
    } else if (cmd == "map_Ps") {
      command = mtl_command::pbr_sheen_map;
      parse_value(line, texture);
    } else if (cmd == "map_Pc") {
      command = mtl_command::pbr_clearcoat_map;
      parse_value(line, texture);
    } else if (cmd == "map_Pcr") {
      command = mtl_command::pbr_coatroughness_map;
      parse_value(line, texture);
    } else if (cmd == "Vt") {
      command = mtl_command::vol_transmission;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Vp") {
      command = mtl_command::vol_meanfreepath;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Ve") {
      command = mtl_command::vol_emission;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Vs") {
      command = mtl_command::vol_scattering;
      parse_value(line, value, obj_value_type::array);
    } else if (cmd == "Vg") {
      command = mtl_command::vol_anisotropy;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "Vr") {
      command = mtl_command::vol_scale;
      parse_value(line, value, obj_value_type::number);
    } else if (cmd == "map_Vs") {
      command = mtl_command::vol_scattering_map;
      parse_value(line, texture);
    } else {
      continue;
    }

    return true;
  }

  return false;
}

// Read objx
bool read_objx_command(file_wrapper& fs, objx_command& command,
    obj_value& value, obj_texture_info& texture) {
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
    parse_value(line, cmd);
    if (cmd == "") continue;

    // read values
    if (cmd == "newcam") {
      command = objx_command::camera;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Cframe") {
      command = objx_command::cam_frame;
      parse_value(line, value, obj_value_type::array, 12);
      return true;
    } else if (cmd == "Cortho") {
      command = objx_command::cam_ortho;
      parse_value(line, value, obj_value_type::boolean);
      return true;
    } else if (cmd == "Cwidth") {
      command = objx_command::cam_width;
      parse_value(line, value, obj_value_type::number);
      return true;
    } else if (cmd == "Cheight") {
      command = objx_command::cam_height;
      parse_value(line, value, obj_value_type::number);
      return true;
    } else if (cmd == "Clens") {
      command = objx_command::cam_lens;
      parse_value(line, value, obj_value_type::number);
      return true;
    } else if (cmd == "Caperture") {
      command = objx_command::cam_aperture;
      parse_value(line, value, obj_value_type::number);
      return true;
    } else if (cmd == "Cfocus") {
      command = objx_command::cam_focus;
      parse_value(line, value, obj_value_type::number);
      return true;
    } else if (cmd == "newenv") {
      command = objx_command::environment;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Eframe") {
      command = objx_command::env_frame;
      parse_value(line, value, obj_value_type::array, 12);
      return true;
    } else if (cmd == "Ee") {
      command = objx_command::env_emission;
      parse_value(line, value, obj_value_type::array);
      return true;
    } else if (cmd == "map_Ee") {
      command = objx_command::env_emission_map;
      parse_value(line, texture);
      return true;
    } else if (cmd == "newist") {
      command = objx_command::instance;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Iframe") {
      command = objx_command::ist_frame;
      parse_value(line, value, obj_value_type::array, 12);
      return true;
    } else if (cmd == "Iobj") {
      command = objx_command::ist_object;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Imat") {
      command = objx_command::ist_material;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "newproc") {
      command = objx_command::procedural;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Pframe") {
      command = objx_command::prc_frame;
      parse_value(line, value, obj_value_type::array, 12);
      return true;
    } else if (cmd == "Ptype") {
      command = objx_command::prc_type;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Pmat") {
      command = objx_command::prc_material;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Psize") {
      command = objx_command::prc_size;
      parse_value(line, value, obj_value_type::string);
      return true;
    } else if (cmd == "Plevel") {
      command = objx_command::prc_level;
      parse_value(line, value, obj_value_type::string);
      return true;
    }
    // backward compatibility
    else if (cmd == "c") {
      auto oname = value.string_;
      auto name = obj_value{}, ortho = obj_value{}, width = obj_value{},
           height = obj_value{}, lens = obj_value{}, aperture = obj_value{},
           focus = obj_value{}, frame = obj_value{};
      parse_value(line, name, obj_value_type::string);
      parse_value(line, ortho, obj_value_type::boolean);
      parse_value(line, width, obj_value_type::number);
      parse_value(line, height, obj_value_type::number);
      parse_value(line, lens, obj_value_type::number);
      parse_value(line, focus, obj_value_type::number);
      parse_value(line, aperture, obj_value_type::number);
      parse_value(line, frame, obj_value_type::array, 12);
      if (command == objx_command::camera && oname != "") {
        command = objx_command::cam_ortho;
        value   = ortho;
      } else if (command == objx_command::cam_ortho) {
        command = objx_command::cam_width;
        value   = width;
      } else if (command == objx_command::cam_width) {
        command = objx_command::cam_height;
        value   = height;
      } else if (command == objx_command::cam_height) {
        command = objx_command::cam_lens;
        value   = lens;
      } else if (command == objx_command::cam_lens) {
        command = objx_command::cam_focus;
        value   = focus;
      } else if (command == objx_command::cam_focus) {
        command = objx_command::cam_aperture;
        value   = aperture;
      } else if (command == objx_command::cam_aperture) {
        command = objx_command::cam_frame;
        value   = frame;
      } else {
        command = objx_command::camera;
        value   = name;
      }
      if (command != objx_command::cam_frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else if (cmd == "e") {
      auto name = obj_value{}, frame = obj_value{}, emission = obj_value{},
           emission_map = obj_value{};
      parse_value(line, name, obj_value_type::string);
      parse_value(line, emission, obj_value_type::array);
      parse_value(line, emission_map, obj_value_type::string);
      parse_value(line, frame, obj_value_type::array, 12);
      if (emission_map.string_ == "\"\"") emission_map.string_ = "";
      if (command == objx_command::environment) {
        command = objx_command::env_emission;
        value   = emission;
      } else if (command == objx_command::env_emission) {
        command = objx_command::env_emission_map;
        get_obj_value(emission_map, texture.path);
      } else if (command == objx_command::env_emission_map) {
        command = objx_command::env_frame;
        value   = frame;
      } else {
        command = objx_command::environment;
        value   = name;
      }
      if (command != objx_command::env_frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else if (cmd == "i") {
      auto name = obj_value{}, frame = obj_value{}, object = obj_value{},
           material = obj_value{};
      parse_value(line, name, obj_value_type::string);
      parse_value(line, object, obj_value_type::string);
      parse_value(line, material, obj_value_type::string);
      parse_value(line, frame, obj_value_type::array, 12);
      if (command == objx_command::instance) {
        command = objx_command::ist_object;
        value   = object;
      } else if (command == objx_command::ist_object) {
        command = objx_command::ist_material;
        value   = material;
      } else if (command == objx_command::ist_material) {
        command = objx_command::ist_frame;
        value   = frame;
      } else {
        command = objx_command::instance;
        value   = name;
      }
      if (command != objx_command::ist_frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else if (cmd == "po") {
      auto name = obj_value{}, frame = obj_value{}, type = obj_value{},
           material = obj_value{}, size = obj_value{}, level = obj_value{};
      parse_value(line, name, obj_value_type::string);
      parse_value(line, type, obj_value_type::string);
      parse_value(line, material, obj_value_type::string);
      parse_value(line, size, obj_value_type::number);
      parse_value(line, level, obj_value_type::number);
      parse_value(line, frame, obj_value_type::array, 12);
      if (command == objx_command::procedural) {
        command = objx_command::prc_type;
        value   = type;
      } else if (command == objx_command::prc_type) {
        command = objx_command::prc_material;
        value   = material;
      } else if (command == objx_command::prc_material) {
        command = objx_command::prc_size;
        value   = size;
      } else if (command == objx_command::prc_size) {
        command = objx_command::prc_level;
        value   = level;
      } else if (command == objx_command::prc_level) {
        command = objx_command::prc_frame;
        value   = frame;
      } else {
        command = objx_command::procedural;
        value   = name;
      }
      if (command != objx_command::prc_frame) fseek(fs.fs, pos, SEEK_SET);
      return true;
    } else {
      // unused
    }
  }

  return false;
}

// Write obj elements
void write_obj_comment(file_wrapper& fs, const string& comment) {
  auto lines = split_string(comment, "\n");
  for (auto& line : lines) {
    checked_fprintf(fs, "# %s\n", line.c_str());
  }
  checked_fprintf(fs, "\n");
}

void write_obj_command(file_wrapper& fs, obj_command command,
    const obj_value& value_, const vector<obj_vertex>& vertices) {
  auto& name  = value_.string_;
  auto& value = value_.array_;
  switch (command) {
    case obj_command::vertex:
      checked_fprintf(fs, "v %g %g %g\n", value[0], value[1], value[2]);
      break;
    case obj_command::normal:
      checked_fprintf(fs, "vn %g  %g %g\n", value[0], value[1], value[2]);
      break;
    case obj_command::texcoord:
      checked_fprintf(fs, "vt %g %g\n", value[0], value[1]);
      break;
    case obj_command::face:
    case obj_command::line:
    case obj_command::point:
      if (command == obj_command::face) checked_fprintf(fs, "f ");
      if (command == obj_command::line) checked_fprintf(fs, "l ");
      if (command == obj_command::point) checked_fprintf(fs, "p ");
      for (auto& vert : vertices) {
        checked_fprintf(fs, " ");
        checked_fprintf(fs, "%d", vert.position);
        if (vert.texcoord) {
          checked_fprintf(fs, "/%d", vert.texcoord);
          if (vert.normal) {
            checked_fprintf(fs, "/%d", vert.normal);
          }
        } else if (vert.normal) {
          checked_fprintf(fs, "//%d", vert.normal);
        }
      }
      checked_fprintf(fs, "\n");
      break;
    case obj_command::object:
      checked_fprintf(fs, "o %s\n", name.c_str());
      break;
    case obj_command::group: checked_fprintf(fs, "g %s\n", name.c_str()); break;
    case obj_command::usemtl:
      checked_fprintf(fs, "usemtl %s\n", name.c_str());
      break;
    case obj_command::smoothing:
      checked_fprintf(fs, "s %s\n", name.c_str());
      break;
    case obj_command::mtllib:
      checked_fprintf(fs, "mtllib %s\n", name.c_str());
      break;
    case obj_command::objxlib: break;
  }
}

void write_mtl_command(file_wrapper& fs, mtl_command command,
    const obj_value& value_, const obj_texture_info& texture) {
  auto& name  = value_.string_;
  auto  value = value_.number;
  auto& color = value_.array_;
  switch (command) {
    case mtl_command::material:
      checked_fprintf(fs, "\nnewmtl %s\n", name.c_str());
      break;
    case mtl_command::illum:
      checked_fprintf(fs, "  illum %d\n", (int)value);
      break;
    case mtl_command::emission:
      checked_fprintf(fs, "  Ke %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::ambient:
      checked_fprintf(fs, "  Ka %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::diffuse:
      checked_fprintf(fs, "  Kd %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::specular:
      checked_fprintf(fs, "  Ks %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::reflection:
      checked_fprintf(fs, "  Kr %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::transmission:
      checked_fprintf(fs, "  Kt %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::exponent:
      checked_fprintf(fs, "  Ns %d\n", (int)value);
      break;
    case mtl_command::opacity: checked_fprintf(fs, "  d %g\n", value); break;
    case mtl_command::ior: checked_fprintf(fs, "  Ni %g\n", value); break;
    case mtl_command::emission_map:
      checked_fprintf(fs, "  map_Ke %s\n", texture.path.c_str());
      break;
    case mtl_command::ambient_map:
      checked_fprintf(fs, "  map_Ka %s\n", texture.path.c_str());
      break;
    case mtl_command::diffuse_map:
      checked_fprintf(fs, "  map_Kd %s\n", texture.path.c_str());
      break;
    case mtl_command::specular_map:
      checked_fprintf(fs, "  map_Ks %s\n", texture.path.c_str());
      break;
    case mtl_command::reflection_map:
      checked_fprintf(fs, "  map_Kr %s\n", texture.path.c_str());
      break;
    case mtl_command::transmission_map:
      checked_fprintf(fs, "  map_Kt %s\n", texture.path.c_str());
      break;
    case mtl_command::opacity_map:
      checked_fprintf(fs, "  map_d %s\n", texture.path.c_str());
      break;
    case mtl_command::exponent_map:
      checked_fprintf(fs, "  map_Ni %s\n", texture.path.c_str());
      break;
    case mtl_command::bump_map:
      checked_fprintf(fs, "  map_bump %s\n", texture.path.c_str());
      break;
    case mtl_command::normal_map:
      checked_fprintf(fs, "  map_norm %s\n", texture.path.c_str());
      break;
    case mtl_command::displacement_map:
      checked_fprintf(fs, "  map_disp %s\n", texture.path.c_str());
      break;
    case mtl_command::pbr_roughness:
      checked_fprintf(fs, "  Pr %g\n", value);
      break;
    case mtl_command::pbr_metallic:
      checked_fprintf(fs, "  Pm %g\n", value);
      break;
    case mtl_command::pbr_sheen: checked_fprintf(fs, "  Ps %g\n", value); break;
    case mtl_command::pbr_clearcoat:
      checked_fprintf(fs, "  Pc %g\n", value);
      break;
    case mtl_command::pbr_coatroughness:
      checked_fprintf(fs, "  Pcr %g\n", value);
      break;
    case mtl_command::pbr_roughness_map:
      checked_fprintf(fs, "  Pr_map %s\n", texture.path.c_str());
      break;
    case mtl_command::pbr_metallic_map:
      checked_fprintf(fs, "  Pm_map %s\n", texture.path.c_str());
      break;
    case mtl_command::pbr_sheen_map:
      checked_fprintf(fs, "  Ps_map %s\n", texture.path.c_str());
      break;
    case mtl_command::pbr_clearcoat_map:
      checked_fprintf(fs, "  Pc_map %s\n", texture.path.c_str());
      break;
    case mtl_command::pbr_coatroughness_map:
      checked_fprintf(fs, "  Pcr_map %s\n", texture.path.c_str());
      break;
    case mtl_command::vol_transmission:
      checked_fprintf(fs, "  Vt %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::vol_meanfreepath:
      checked_fprintf(fs, "  Vp %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::vol_emission:
      checked_fprintf(fs, "  Ve %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::vol_scattering:
      checked_fprintf(fs, "  Vs %g %g %g\n", color[0], color[1], color[2]);
      break;
    case mtl_command::vol_anisotropy:
      checked_fprintf(fs, "  Vg %g\n", value);
      break;
    case mtl_command::vol_scale: checked_fprintf(fs, "  Vr %g\n", value); break;
    case mtl_command::vol_scattering_map:
      checked_fprintf(fs, "  Vs_map %s\n", texture.path.c_str());
  }
}

void write_objx_command(file_wrapper& fs, objx_command command,
    const obj_value& value_, const obj_texture_info& texture) {
  auto& name  = value_.string_;
  auto  value = value_.number;
  auto& color = value_.array_;
  auto& frame = value_.array_;
  switch (command) {
    case objx_command::camera:
      checked_fprintf(fs, "\nnewcam %s\n", name.c_str());
      break;
    case objx_command::environment:
      checked_fprintf(fs, "\nnewenv %s\n", name.c_str());
      break;
    case objx_command::instance:
      checked_fprintf(fs, "\nnewist %s\n", name.c_str());
      break;
    case objx_command::procedural:
      checked_fprintf(fs, "\nnewproc %s\n", name.c_str());
      break;
    case objx_command::cam_frame:
      checked_fprintf(fs, "  Cframe %g %g %g %g %g %g %g %g %g %g %g %g\n",
          frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
          frame[7], frame[8], frame[9], frame[10], frame[11]);
      break;
    case objx_command::env_frame:
      checked_fprintf(fs, "  Eframe %g %g %g %g %g %g %g %g %g %g %g %g\n",
          frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
          frame[7], frame[8], frame[9], frame[10], frame[11]);
      break;
    case objx_command::ist_frame:
      checked_fprintf(fs, "  Iframe %g %g %g %g %g %g %g %g %g %g %g %g\n",
          frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
          frame[7], frame[8], frame[9], frame[10], frame[11]);
      break;
    case objx_command::prc_frame:
      checked_fprintf(fs, "  Pframe %g %g %g %g %g %g %g %g %g %g %g %g\n",
          frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
          frame[7], frame[8], frame[9], frame[10], frame[11]);
      break;
    case objx_command::ist_object:
      checked_fprintf(fs, "  Iobj %s\n", name.c_str());
      break;
    case objx_command::prc_type:
      checked_fprintf(fs, "  Ptype %s\n", name.c_str());
      break;
    case objx_command::ist_material:
      checked_fprintf(fs, "  Imat %s\n", name.c_str());
      break;
    case objx_command::prc_material:
      checked_fprintf(fs, "  Pmat %s\n", name.c_str());
      break;
    case objx_command::cam_ortho:
      checked_fprintf(fs, "  Cortho %g\n", value);
      break;
    case objx_command::cam_width:
      checked_fprintf(fs, "  Cwidth %g\n", value);
      break;
    case objx_command::cam_height:
      checked_fprintf(fs, "  Cheight %g\n", value);
      break;
    case objx_command::cam_lens:
      checked_fprintf(fs, "  Clens %g\n", value);
      break;
    case objx_command::cam_aperture:
      checked_fprintf(fs, "  Caperture %g\n", value);
      break;
    case objx_command::cam_focus:
      checked_fprintf(fs, "  Cfocus %g\n", value);
      break;
    case objx_command::env_emission:
      checked_fprintf(fs, "  Ee %g %g %g\n", color[0], color[1], color[2]);
      break;
    case objx_command::env_emission_map:
      checked_fprintf(fs, "  map_Ee %s\n", texture.path.c_str());
      break;
    case objx_command::prc_size:
      checked_fprintf(fs, "  Psize %g\n", value);
      break;
    case objx_command::prc_level:
      checked_fprintf(fs, "  Plevel %g\n", value);
      break;
  }
}

// typesafe access of obj value
void get_obj_value(const obj_value& yaml, string& value) {
  if (yaml.type != obj_value_type::string)
    throw std::runtime_error("error parsing yaml value");
  value = yaml.string_;
}
void get_obj_value(const obj_value& yaml, bool& value) {
  if (yaml.type != obj_value_type::boolean)
    throw std::runtime_error("error parsing yaml value");
  value = yaml.boolean;
}
void get_obj_value(const obj_value& yaml, int& value) {
  if (yaml.type != obj_value_type::number)
    throw std::runtime_error("error parsing yaml value");
  value = (int)yaml.number;
}
void get_obj_value(const obj_value& yaml, float& value) {
  if (yaml.type != obj_value_type::number)
    throw std::runtime_error("error parsing yaml value");
  value = (float)yaml.number;
}
void get_obj_value(const obj_value& yaml, vec2f& value) {
  if (yaml.type != obj_value_type::array || yaml.number != 2)
    throw std::runtime_error("error parsing yaml value");
  value = {(float)yaml.array_[0], (float)yaml.array_[1]};
}
void get_obj_value(const obj_value& yaml, vec3f& value) {
  if (yaml.type != obj_value_type::array || yaml.number != 3)
    throw std::runtime_error("error parsing yaml value");
  value = {(float)yaml.array_[0], (float)yaml.array_[1], (float)yaml.array_[2]};
}
void get_obj_value(const obj_value& yaml, mat3f& value) {
  if (yaml.type != obj_value_type::array || yaml.number != 9)
    throw std::runtime_error("error parsing yaml value");
  for (auto i = 0; i < 9; i++) (&value.x.x)[i] = (float)yaml.array_[i];
}
void get_obj_value(const obj_value& yaml, frame3f& value) {
  if (yaml.type != obj_value_type::array || yaml.number != 12)
    throw std::runtime_error("error parsing yaml value");
  for (auto i = 0; i < 12; i++) (&value.x.x)[i] = (float)yaml.array_[i];
}

// typesafe access of obj value
obj_value make_obj_value(const string& value) {
  return {obj_value_type::string, 0, false, value};
}
obj_value make_obj_value(bool value) {
  return {obj_value_type::boolean, 0, value};
}
obj_value make_obj_value(int value) {
  return {obj_value_type::number, (double)value};
}
obj_value make_obj_value(float value) {
  return {obj_value_type::number, (double)value};
}
obj_value make_obj_value(const vec2f& value) {
  return {
      obj_value_type::array, 2, false, "", {(double)value.x, (double)value.y}};
}
obj_value make_obj_value(const vec3f& value) {
  return {obj_value_type::array, 3, false, "",
      {(double)value.x, (double)value.y, (double)value.z}};
}
obj_value make_obj_value(const mat3f& value) {
  auto yaml = obj_value{obj_value_type::array, 9};
  for (auto i = 0; i < 9; i++) yaml.array_[i] = (double)(&value.x.x)[i];
  return yaml;
}
obj_value make_obj_value(const frame3f& value) {
  auto yaml = obj_value{obj_value_type::array, 12};
  for (auto i = 0; i < 12; i++) yaml.array_[i] = (double)(&value.x.x)[i];
  return yaml;
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

static inline void parse_yaml_varname(string_view& str, string_view& value) {
  skip_whitespace(str);
  if (str.empty()) throw std::runtime_error("cannot parse value");
  if (!is_alpha(str.front())) throw std::runtime_error("cannot parse value");
  auto pos = 0;
  while (is_alpha(str[pos]) || str[pos] == '_' || is_digit(str[pos])) {
    pos += 1;
    if (pos >= str.size()) break;
  }
  value = str.substr(0, pos);
  str.remove_prefix(pos);
}
static inline void parse_yaml_varname(string_view& str, string& value) {
  auto view = ""sv;
  parse_yaml_varname(str, view);
  value = string{view};
}

inline void parse_yaml_value(string_view& str, string_view& value) {
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
inline void parse_yaml_value(string_view& str, string& value) {
  auto valuev = ""sv;
  parse_yaml_value(str, valuev);
  value = string{valuev};
}
inline void parse_yaml_value(string_view& str, int& value) {
  skip_whitespace(str);
  char* end = nullptr;
  value     = (int)strtol(str.data(), &end, 10);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
inline void parse_yaml_value(string_view& str, float& value) {
  skip_whitespace(str);
  char* end = nullptr;
  value     = strtof(str.data(), &end);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}
inline void parse_yaml_value(string_view& str, double& value) {
  skip_whitespace(str);
  char* end = nullptr;
  value     = strtod(str.data(), &end);
  if (str == end) throw std::runtime_error("cannot parse value");
  str.remove_prefix(end - str.data());
}

// parse yaml value
void get_yaml_value(const yaml_value& yaml, string& value) {
  if (yaml.type != yaml_value_type::string)
    throw std::runtime_error("error parsing yaml value");
  value = yaml.string_;
}
void get_yaml_value(const yaml_value& yaml, bool& value) {
  if (yaml.type != yaml_value_type::boolean)
    throw std::runtime_error("error parsing yaml value");
  value = yaml.boolean;
}
void get_yaml_value(const yaml_value& yaml, int& value) {
  if (yaml.type != yaml_value_type::number)
    throw std::runtime_error("error parsing yaml value");
  value = (int)yaml.number;
}
void get_yaml_value(const yaml_value& yaml, float& value) {
  if (yaml.type != yaml_value_type::number)
    throw std::runtime_error("error parsing yaml value");
  value = (float)yaml.number;
}
void get_yaml_value(const yaml_value& yaml, vec2f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 2)
    throw std::runtime_error("error parsing yaml value");
  value = {(float)yaml.array_[0], (float)yaml.array_[1]};
}
void get_yaml_value(const yaml_value& yaml, vec3f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 3)
    throw std::runtime_error("error parsing yaml value");
  value = {(float)yaml.array_[0], (float)yaml.array_[1], (float)yaml.array_[2]};
}
void get_yaml_value(const yaml_value& yaml, mat3f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 9)
    throw std::runtime_error("error parsing yaml value");
  for (auto i = 0; i < 9; i++) (&value.x.x)[i] = (float)yaml.array_[i];
}
void get_yaml_value(const yaml_value& yaml, frame3f& value) {
  if (yaml.type != yaml_value_type::array || yaml.number != 12)
    throw std::runtime_error("error parsing yaml value");
  for (auto i = 0; i < 12; i++) (&value.x.x)[i] = (float)yaml.array_[i];
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

void parse_yaml_value(string_view& str, yaml_value& value) {
  trim_whitespace(str);
  if (str.empty()) throw std::runtime_error("bad yaml");
  if (str.front() == '[') {
    str.remove_prefix(1);
    value.type   = yaml_value_type::array;
    value.number = 0;
    while (!str.empty()) {
      skip_whitespace(str);
      if (str.empty()) throw std::runtime_error("bad yaml");
      if (str.front() == ']') {
        str.remove_prefix(1);
        break;
      }
      if (value.number >= 16) throw std::runtime_error("array too large");
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
        throw std::runtime_error("bad yaml");
      }
    }
  } else if (is_digit(str.front()) || str.front() == '-' ||
             str.front() == '+') {
    value.type = yaml_value_type::number;
    parse_yaml_value(str, value.number);
  } else {
    value.type = yaml_value_type::string;
    parse_yaml_value(str, value.string_);
    if (value.string_ == "true" || value.string_ == "false") {
      value.type    = yaml_value_type::boolean;
      value.boolean = value.string_ == "true";
    }
  }
  skip_whitespace(str);
  if (!str.empty() && !is_whitespace(str)) throw std::runtime_error("bad yaml");
}

bool read_yaml_property(file_wrapper& fs, string& group, string& key,
    bool& newobj, yaml_value& value) {
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
      if (group == "") throw std::runtime_error("bad yaml");
      skip_whitespace(line);
      if (line.empty()) throw std::runtime_error("bad yaml");
      if (line.front() == '-') {
        newobj = true;
        line.remove_prefix(1);
        skip_whitespace(line);
      } else {
        newobj = false;
      }
      parse_yaml_varname(line, key);
      skip_whitespace(line);
      if (line.empty() || line.front() != ':')
        throw std::runtime_error("bad yaml");
      line.remove_prefix(1);
      parse_yaml_value(line, value);
      return true;
    } else if (is_alpha(line.front())) {
      // new group
      parse_yaml_varname(line, key);
      skip_whitespace(line);
      if (line.empty() || line.front() != ':')
        throw std::runtime_error("bad yaml");
      line.remove_prefix(1);
      if (!line.empty() && !is_whitespace(line)) {
        group = "";
        parse_yaml_value(line, value);
        return true;
      } else {
        group = key;
        key   = "";
        return true;
      }
    } else {
      throw std::runtime_error("bad yaml");
    }
  }
  return false;
}

void write_yaml_comment(file_wrapper& fs, const string& comment) {
  auto lines = split_string(comment, "\n");
  for (auto& line : lines) {
    checked_fprintf(fs, "# %s\n", line.c_str());
  }
  checked_fprintf(fs, "\n");
}

// Save yaml property
void write_yaml_property(file_wrapper& fs, const string& object,
    const string& key, bool newobj, const yaml_value& value) {
  if (key.empty()) {
    checked_fprintf(fs, "\n%s:\n", object.c_str());
  } else {
    if (!object.empty()) {
      checked_fprintf(fs, (newobj ? "  - " : "    "));
    }
    checked_fprintf(fs, "%s: ", key.c_str());
    switch (value.type) {
      case yaml_value_type::number:
        checked_fprintf(fs, "%g", value.number);
        break;
      case yaml_value_type::boolean:
        checked_fprintf(fs, "%s", value.boolean ? "true" : "false");
        break;
      case yaml_value_type::string:
        checked_fprintf(fs, "%s", value.string_.c_str());
        break;
      case yaml_value_type::array:
        checked_fprintf(fs, "[ ");
        for (auto i = 0; i < value.number; i++) {
          if (i) checked_fprintf(fs, ", ");
          checked_fprintf(fs, "%g", value.array_[i]);
        }
        checked_fprintf(fs, " ]");
        break;
    }
    checked_fprintf(fs, "\n", key.c_str());
  }
}

void write_yaml_object(file_wrapper& fs, const string& object) {
  checked_fprintf(fs, "\n%s:\n", object.c_str());
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
      throw std::runtime_error("bad pbrt command");
    }
    cmd += line;
    cmd += " ";
    pos = ftell(fs.fs);
  }
  return found;
}

// parse a quoted string
static inline void parse_pbrt_value(string_view& str, string_view& value) {
  skip_whitespace(str);
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

static inline void parse_pbrt_value(string_view& str, string& value) {
  auto view = ""sv;
  parse_pbrt_value(str, view);
  value = string{view};
}

// parse a quoted string
static inline void parse_pbrt_command(string_view& str, string& value) {
  skip_whitespace(str);
  if (!isalpha((int)str.front())) {
    throw std::runtime_error("bad command");
  }
  auto pos = str.find_first_not_of(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  if (pos == string_view::npos) {
    value.assign(str);
    str.remove_prefix(str.size());
  } else {
    value.assign(str.substr(0, pos));
    str.remove_prefix(pos + 1);
  }
}

// parse a number
static inline void parse_pbrt_value(string_view& str, float& value) {
  skip_whitespace(str);
  if (str.empty()) throw std::runtime_error("number expected");
  auto next = (char*)nullptr;
  value     = strtof(str.data(), &next);
  if (str.data() == next) throw std::runtime_error("number expected");
  str.remove_prefix(next - str.data());
}

// parse a number
static inline void parse_pbrt_value(string_view& str, int& value) {
  skip_whitespace(str);
  if (str.empty()) throw std::runtime_error("number expected");
  auto next = (char*)nullptr;
  value     = strtol(str.data(), &next, 10);
  if (str.data() == next) throw std::runtime_error("number expected");
  str.remove_prefix(next - str.data());
}
template <typename T>
static inline void parse_pbrt_value(
    string_view& str, T& value, unordered_map<string, T>& value_names) {
  auto value_name = ""s;
  parse_pbrt_value(str, value_name);
  try {
    value = value_names.at(value_name);
  } catch (std::out_of_range&) {
    throw std::runtime_error("expected enum value");
  }
}

// parse a vec type
static inline void parse_pbrt_value(string_view& str, vec2f& value) {
  for (auto i = 0; i < 2; i++) parse_pbrt_value(str, value[i]);
}
static inline void parse_pbrt_value(string_view& str, vec3f& value) {
  for (auto i = 0; i < 3; i++) parse_pbrt_value(str, value[i]);
}
static inline void parse_pbrt_value(string_view& str, vec4f& value) {
  for (auto i = 0; i < 4; i++) parse_pbrt_value(str, value[i]);
}
static inline void parse_pbrt_value(string_view& str, mat4f& value) {
  for (auto i = 0; i < 4; i++) parse_pbrt_value(str, value[i]);
}

// parse pbrt value with optional parens
template <typename T>
static inline void parse_pbrt_param(string_view& str, T& value) {
  skip_whitespace(str);
  auto parens = !str.empty() && str.front() == '[';
  if (parens) str.remove_prefix(1);
  parse_pbrt_value(str, value);
  if (parens) {
    skip_whitespace(str);
    if (!str.empty() && str.front() == '[')
      throw std::runtime_error("bad pbrt param");
    str.remove_prefix(1);
  }
}

// parse a quoted string
static inline void parse_pbrt_nametype(
    string_view& str_, string& name, string& type) {
  auto value = ""s;
  parse_pbrt_value(str_, value);
  auto str  = string_view{value};
  auto pos1 = str.find(' ');
  if (pos1 == string_view::npos) {
    throw std::runtime_error("bad type " + value);
  }
  type = string(str.substr(0, pos1));
  str.remove_prefix(pos1);
  auto pos2 = str.find_first_not_of(' ');
  if (pos2 == string_view::npos) {
    throw std::runtime_error("bad type " + value);
  }
  str.remove_prefix(pos2);
  name = string(str);
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

static inline void parse_pbrt_params(
    string_view& str, vector<pbrt_value>& values) {
  auto parse_pbrt_pvalues = [](string_view& str, auto& value, auto& values) {
    values.clear();
    skip_whitespace(str);
    if (str.empty()) throw std::runtime_error("bad pbrt value");
    if (str.front() == '[') {
      str.remove_prefix(1);
      skip_whitespace(str);
      if (str.empty()) throw std::runtime_error("bad pbrt value");
      while (!str.empty()) {
        auto& val = values.empty() ? value : values.emplace_back();
        parse_pbrt_value(str, val);
        skip_whitespace(str);
        if (str.empty()) break;
        if (str.front() == ']') break;
        if (values.empty()) values.push_back(value);
      }
      if (str.empty()) throw std::runtime_error("bad pbrt value");
      if (str.front() != ']') throw std::runtime_error("bad pbrt value");
      str.remove_prefix(1);
    } else {
      parse_pbrt_value(str, value);
    }
  };

  values.clear();
  skip_whitespace(str);
  while (!str.empty()) {
    auto& value = values.emplace_back();
    auto  type  = ""s;
    parse_pbrt_nametype(str, value.name, type);
    skip_whitespace(str);
    if (str.empty()) throw std::runtime_error("expected value");
    if (type == "float") {
      value.type = pbrt_value_type::real;
      parse_pbrt_pvalues(str, value.value1f, value.vector1f);
    } else if (type == "integer") {
      value.type = pbrt_value_type::integer;
      parse_pbrt_pvalues(str, value.value1i, value.vector1i);
    } else if (type == "string") {
      auto vector1s = vector<string>{};
      value.type    = pbrt_value_type::string;
      parse_pbrt_pvalues(str, value.value1s, vector1s);
      if (!vector1s.empty())
        throw std::runtime_error("do not support pbrt string array");
    } else if (type == "bool") {
      auto value1s  = ""s;
      auto vector1s = vector<string>{};
      value.type    = pbrt_value_type::boolean;
      parse_pbrt_pvalues(str, value1s, vector1s);
      if (!vector1s.empty())
        throw std::runtime_error("do not support pbrt string array");
      value.value1b = value1s == "true";
    } else if (type == "texture") {
      auto vector1s = vector<string>{};
      value.type    = pbrt_value_type::texture;
      parse_pbrt_pvalues(str, value.value1s, vector1s);
      if (!vector1s.empty())
        throw std::runtime_error("do not support pbrt string array");
    } else if (type == "point" || type == "point3") {
      value.type = pbrt_value_type::point;
      parse_pbrt_pvalues(str, value.value3f, value.vector3f);
    } else if (type == "normal" || type == "normal3") {
      value.type = pbrt_value_type::normal;
      parse_pbrt_pvalues(str, value.value3f, value.vector3f);
    } else if (type == "vector" || type == "vector3") {
      value.type = pbrt_value_type::vector;
      parse_pbrt_pvalues(str, value.value3f, value.vector3f);
    } else if (type == "point2") {
      value.type = pbrt_value_type::point2;
      parse_pbrt_pvalues(str, value.value2f, value.vector2f);
    } else if (type == "vector2") {
      value.type = pbrt_value_type::vector2;
      parse_pbrt_pvalues(str, value.value2f, value.vector2f);
    } else if (type == "blackbody") {
      value.type     = pbrt_value_type::color;
      auto blackbody = zero2f;
      auto vector2f  = vector<vec2f>{};
      parse_pbrt_pvalues(str, blackbody, vector2f);
      if (!vector2f.empty())
        throw std::runtime_error("bad pbrt " + type + " property");
      value.value3f = blackbody_to_rgb(blackbody.x) * blackbody.y;
    } else if (type == "color" || type == "rgb") {
      value.type = pbrt_value_type::color;
      parse_pbrt_pvalues(str, value.value3f, value.vector3f);
    } else if (type == "xyz") {
      // TODO: xyz conversion
      value.type = pbrt_value_type::color;
      parse_pbrt_pvalues(str, value.value3f, value.vector3f);
      throw std::runtime_error("xyz conversion");
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
        parse_pbrt_value(str, filename);
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
          }
        } else {
          throw std::runtime_error("unsupported spectrum format");
        }
      } else {
        value.type = pbrt_value_type::spectrum;
        parse_pbrt_pvalues(str, value.value1f, value.vector1f);
      }
    } else {
      throw std::runtime_error("unknown pbrt type");
    }
    skip_whitespace(str);
  }
}

// Compute the fresnel term for dielectrics. Implementation from
// https://seblagarde.wordpress.com/2013/04/29/memo-on-fresnel-equations/
static vec3f pbrt_fresnel_dielectric(float cosw, const vec3f& eta_) {
  auto eta = eta_;
  if (cosw < 0) {
    eta  = vec3f{1, 1, 1} / eta;
    cosw = -cosw;
  }

  auto sin2 = 1 - cosw * cosw;
  auto eta2 = eta * eta;

  auto cos2t = vec3f{1, 1, 1} - vec3f{sin2, sin2, sin2} / eta2;
  if (cos2t.x < 0 || cos2t.y < 0 || cos2t.z < 0) return vec3f{1, 1, 1};  // tir

  auto t0 = vec3f{sqrt(cos2t.x), sqrt(cos2t.y), sqrt(cos2t.z)};
  auto t1 = eta * t0;
  auto t2 = eta * cosw;

  auto rs = (vec3f{cosw, cosw, cosw} - t1) / (vec3f{cosw, cosw, cosw} + t1);
  auto rp = (t0 - t2) / (t0 + t2);

  return (rs * rs + rp * rp) / 2.0f;
}

// Compute the fresnel term for metals. Implementation from
// https://seblagarde.wordpress.com/2013/04/29/memo-on-fresnel-equations/
static vec3f pbrt_fresnel_metal(
    float cosw, const vec3f& eta, const vec3f& etak) {
  if (etak == zero3f) return pbrt_fresnel_dielectric(cosw, eta);

  cosw       = clamp(cosw, (float)-1, (float)1);
  auto cos2  = cosw * cosw;
  auto sin2  = clamp(1 - cos2, (float)0, (float)1);
  auto eta2  = eta * eta;
  auto etak2 = etak * etak;

  auto t0         = eta2 - etak2 - vec3f{sin2, sin2, sin2};
  auto a2plusb2_2 = t0 * t0 + 4.0f * eta2 * etak2;
  auto a2plusb2   = vec3f{
      sqrt(a2plusb2_2.x), sqrt(a2plusb2_2.y), sqrt(a2plusb2_2.z)};
  auto t1  = a2plusb2 + vec3f{cos2, cos2, cos2};
  auto a_2 = (a2plusb2 + t0) / 2.0f;
  auto a   = vec3f{sqrt(a_2.x), sqrt(a_2.y), sqrt(a_2.z)};
  auto t2  = 2.0f * a * cosw;
  auto rs  = (t1 - t2) / (t1 + t2);

  auto t3 = vec3f{cos2, cos2, cos2} * a2plusb2 +
            vec3f{sin2, sin2, sin2} * vec3f{sin2, sin2, sin2};
  auto t4 = t2 * sin2;
  auto rp = rs * (t3 - t4) / (t3 + t4);

  return (rp + rs) / 2.0f;
}

// convert pbrt elements
static void convert_pbrt_cameras(
    vector<pbrt_camera>& cameras, bool verbose = false) {
  for (auto& camera : cameras) {
    auto& values   = camera.values;
    camera.frame   = inverse((frame3f)camera.frame);
    camera.frame.z = -camera.frame.z;
    if (camera.type == "perspective") {
      camera.fov = get_pbrt_value(values, "fov", 90.0f);
      // auto lensradius = get_pbrt_value(values, "lensradius", 0.0f);
      camera.aspect = get_pbrt_value(values, "frameaspectratio", camera.aspect);
      camera.focus  = get_pbrt_value(values, "focaldistance", camera.focus);
      if (!camera.aspect) camera.aspect = 1;
      if (!camera.focus) camera.focus = 10;
    } else if (camera.type == "realistic") {
      auto lensfile   = get_pbrt_value(values, "lensfile", ""s);
      lensfile        = lensfile.substr(0, lensfile.size() - 4);
      lensfile        = lensfile.substr(lensfile.find('.') + 1);
      lensfile        = lensfile.substr(0, lensfile.size() - 2);
      auto lens       = max(std::atof(lensfile.c_str()), 35.0f) * 0.001f;
      camera.fov      = 2 * atan(0.036f / (2 * lens));
      camera.aperture = get_pbrt_value(values, "aperturediameter", 0.0f);
      camera.focus    = get_pbrt_value(values, "focusdistance", camera.focus);
      if (!camera.aspect) camera.aspect = 1;
      if (!camera.focus) camera.focus = 10;
    } else {
      throw std::runtime_error("unsupported Camera type " + camera.type);
    }
  }
}

// convert pbrt textures
static void convert_pbrt_textures(
    vector<pbrt_texture>& textures, bool verbose = false) {
  auto texture_map = unordered_map<string, int>{};
  for (auto& texture : textures) {
    auto index                = (int)texture_map.size();
    texture_map[texture.name] = index;
  }
  auto get_filename = [&textures, &texture_map](const string& name) {
    if (name.empty()) return ""s;
    auto pos = texture_map.find(name);
    if (pos == texture_map.end()) return ""s;
    return textures[pos->second].filename;
  };
  auto make_placeholder = [verbose](pbrt_texture& texture,
                              const vec3f&        color = {1, 0, 0}) {
    texture.constant    = color;
    texture.is_constant = true;
    if (verbose)
      printf("texture %s not supported well\n", texture.type.c_str());
  };

  for (auto& texture : textures) {
    auto& values = texture.values;
    if (texture.type == "imagemap") {
      texture.filename = get_pbrt_value(values, "filename", ""s);
    } else if (texture.type == "constant") {
      texture.is_constant = true;
      texture.constant    = get_pbrt_value(values, "value", vec3f{1});
    } else if (texture.type == "bilerp") {
      make_placeholder(texture, {1, 0, 0});
    } else if (texture.type == "checkerboard") {
      // auto tex1     = get_pbrt_value(values, "tex1", pair{vec3f{1}, ""s});
      // auto tex2     = get_pbrt_value(values, "tex2", pair{vec3f{0}, ""s});
      // auto rgb1     = tex1.second == "" ? tex1.first : vec3f{0.4f, 0.4f,
      // 0.4f}; auto rgb2     = tex1.second == "" ? tex2.first : vec3f{0.6f,
      // 0.6f, 0.6f}; auto params   = proc_image_params{}; params.type   =
      // proc_image_params::type_t::checker; params.color0 = {rgb1.x, rgb1.y,
      // rgb1.z, 1}; params.color1 = {rgb2.x, rgb2.y, rgb2.z, 1}; params.scale
      // = 2; make_proc_image(texture.hdr, params); float_to_byte(texture.ldr,
      // texture.hdr); texture.hdr = {};
      make_placeholder(texture, vec3f{0.5});
    } else if (texture.type == "dots") {
      make_placeholder(texture, vec3f{0.5});
    } else if (texture.type == "fbm") {
      make_placeholder(texture, vec3f{0.5});
    } else if (texture.type == "marble") {
      make_placeholder(texture, vec3f{0.5});
    } else if (texture.type == "mix") {
      auto tex1 = get_pbrt_value(values, "tex1", pair{vec3f{0}, ""s});
      auto tex2 = get_pbrt_value(values, "tex2", pair{vec3f{1}, ""s});
      if (!get_filename(tex1.second).empty()) {
        texture.filename = get_filename(tex1.second);
      } else if (!get_filename(tex2.second).empty()) {
        texture.filename = get_filename(tex2.second);
      } else {
        make_placeholder(texture);
      }
    } else if (texture.type == "scale") {
      auto tex1 = get_pbrt_value(values, "tex1", pair{vec3f{1}, ""s});
      auto tex2 = get_pbrt_value(values, "tex2", pair{vec3f{1}, ""s});
      if (!get_filename(tex1.second).empty()) {
        texture.filename = get_filename(tex1.second);
      } else if (!get_filename(tex2.second).empty()) {
        texture.filename = get_filename(tex2.second);
      } else {
        make_placeholder(texture);
      }
    } else if (texture.type == "uv") {
      make_placeholder(texture);
    } else if (texture.type == "windy") {
      make_placeholder(texture);
    } else if (texture.type == "wrinkled") {
      make_placeholder(texture);
    } else {
      throw std::runtime_error("unsupported texture type " + texture.type);
    }
  }
}

// convert pbrt materials
static void convert_pbrt_materials(vector<pbrt_material>& materials,
    const vector<pbrt_texture>& textures, bool verbose = false) {
  auto constants = unordered_map<string, vec3f>{};

  auto get_scaled_texture = [&](const vector<pbrt_value>& values,
                                const string& name, vec3f& color,
                                string& texture, const vec3f& def) {
    auto textured = get_pbrt_value(values, name, pair{def, ""s});
    if (textured.second == "") {
      color   = textured.first;
      texture = "";
    } else if (constants.find(textured.second) != constants.end()) {
      color   = constants.at(textured.second);
      texture = "";
    } else {
      color   = {1, 1, 1};
      texture = textured.second;
    }
  };

  auto get_pbrt_roughness = [&](const vector<pbrt_value>& values,
                                vec2f& roughness, float def = 0.1) {
    auto roughness_ = get_pbrt_value(
        values, "roughness", pair{vec3f{def}, ""s});
    auto uroughness     = get_pbrt_value(values, "uroughness", roughness_);
    auto vroughness     = get_pbrt_value(values, "vroughness", roughness_);
    auto remaproughness = get_pbrt_value(values, "remaproughness", true);

    roughness = zero2f;
    if (uroughness.first == zero3f || vroughness.first == zero3f) return;
    roughness = vec2f{mean(uroughness.first), mean(vroughness.first)};
    // from pbrt code
    if (remaproughness) {
      roughness = max(roughness, 1e-3f);
      auto x    = log(roughness);
      roughness = 1.62142f + 0.819955f * x + 0.1734f * x * x +
                  0.0171201f * x * x * x + 0.000640711f * x * x * x * x;
    }
    roughness = sqrt(roughness);
  };

  auto eta_to_reflectivity = [](const vec3f&  eta,
                                 const vec3f& etak = zero3f) -> vec3f {
    return ((eta - 1) * (eta - 1) + etak * etak) /
           ((eta + 1) * (eta + 1) + etak * etak);
  };

  for (auto& material : materials) {
    auto& values = material.values;
    if (material.type == "uber") {
      get_scaled_texture(
          values, "Kd", material.diffuse, material.diffuse_map, vec3f{0.25});
      get_scaled_texture(
          values, "Ks", material.specular, material.specular_map, vec3f{0.25});
      get_scaled_texture(values, "Kt", material.transmission,
          material.transmission_map, vec3f{0});
      get_scaled_texture(
          values, "opacity", material.opacity, material.opacity_map, vec3f{1});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0.1f);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
      material.refract = false;
    } else if (material.type == "plastic") {
      get_scaled_texture(
          values, "Kd", material.diffuse, material.diffuse_map, vec3f{0.25});
      get_scaled_texture(
          values, "Ks", material.specular, material.specular_map, vec3f{0.25});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0.1);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
    } else if (material.type == "translucent") {
      get_scaled_texture(
          values, "Kd", material.diffuse, material.diffuse_map, vec3f{0.25});
      get_scaled_texture(
          values, "Ks", material.specular, material.specular_map, vec3f{0.25});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0.1);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
    } else if (material.type == "matte") {
      get_scaled_texture(
          values, "Kd", material.diffuse, material.diffuse_map, vec3f{0.5});
      material.roughness = vec2f{1};
    } else if (material.type == "mirror") {
      get_scaled_texture(
          values, "Kr", material.specular, material.specular_map, vec3f{0.9});
      material.eta       = zero3f;
      material.etak      = zero3f;
      material.roughness = zero2f;
      material.sspecular = material.specular;
    } else if (material.type == "metal") {
      get_scaled_texture(
          values, "Kr", material.specular, material.specular_map, vec3f{1});
      get_scaled_texture(values, "eta", material.eta, material.eta_map,
          vec3f{0.2004376970f, 0.9240334304f, 1.1022119527f});
      get_scaled_texture(values, "k", material.etak, material.etak_map,
          vec3f{3.9129485033f, 2.4528477015f, 2.1421879552f});
      get_pbrt_roughness(values, material.roughness, 0.01);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta, material.etak);
    } else if (material.type == "substrate") {
      get_scaled_texture(
          values, "Kd", material.diffuse, material.diffuse_map, vec3f{0.5});
      get_scaled_texture(
          values, "Ks", material.specular, material.specular_map, vec3f{0.5});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0.1);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
    } else if (material.type == "glass") {
      get_scaled_texture(
          values, "Kr", material.specular, material.specular_map, vec3f{1});
      get_scaled_texture(values, "Kt", material.transmission,
          material.transmission_map, vec3f{1});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
    } else if (material.type == "hair") {
      get_scaled_texture(
          values, "color", material.diffuse, material.diffuse_map, vec3f{0});
      material.roughness = {1, 1};
      if (verbose) printf("hair material not properly supported\n");
    } else if (material.type == "disney") {
      get_scaled_texture(
          values, "color", material.diffuse, material.diffuse_map, vec3f{0.5});
      material.roughness = {1, 1};
      if (verbose) printf("disney material not properly supported\n");
    } else if (material.type == "kdsubsurface") {
      get_scaled_texture(
          values, "Kd", material.diffuse, material.diffuse_map, vec3f{0.5});
      get_scaled_texture(
          values, "Kr", material.specular, material.specular_map, vec3f{1});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
      if (verbose) printf("kdsubsurface material not properly supported\n");
    } else if (material.type == "subsurface") {
      get_scaled_texture(
          values, "Kr", material.specular, material.specular_map, vec3f{1});
      get_scaled_texture(values, "Kt", material.transmission,
          material.transmission_map, vec3f{1});
      get_scaled_texture(
          values, "eta", material.eta, material.eta_map, vec3f{1.5});
      get_pbrt_roughness(values, material.roughness, 0);
      material.sspecular = material.specular *
                           eta_to_reflectivity(material.eta);
      auto scale        = get_pbrt_value(values, "scale", 1.0f);
      material.volscale = 1 / scale;
      auto sigma_a = zero3f, sigma_s = zero3f;
      auto sigma_a_tex = ""s, sigma_s_tex = ""s;
      get_scaled_texture(
          values, "sigma_a", sigma_a, sigma_a_tex, vec3f{0011, .0024, .014});
      get_scaled_texture(values, "sigma_prime_s", sigma_s, sigma_s_tex,
          vec3f{2.55, 3.12, 3.77});
      material.volmeanfreepath = 1 / (sigma_a + sigma_s);
      material.volscatter      = sigma_s / (sigma_a + sigma_s);
      if (verbose) printf("subsurface material not properly supported\n");
    } else if (material.type == "mix") {
      auto namedmaterial1 = get_pbrt_value(values, "namedmaterial1", ""s);
      auto namedmaterial2 = get_pbrt_value(values, "namedmaterial2", ""s);
      auto matname        = (!namedmaterial1.empty()) ? namedmaterial1
                                               : namedmaterial2;
      // material = mmap.at(matname);
      throw std::runtime_error("unsupporrted mix material");
      if (verbose) printf("mix material not properly supported\n");
    } else if (material.type == "fourier") {
      auto bsdffile = get_pbrt_value(values, "bsdffile", ""s);
      if (bsdffile.rfind("/") != string::npos)
        bsdffile = bsdffile.substr(bsdffile.rfind("/") + 1);
      if (bsdffile == "paint.bsdf") {
        material.diffuse   = {0.6f, 0.6f, 0.6f};
        material.specular  = {1, 1, 1};
        material.eta       = vec3f{1.5};
        material.roughness = vec2f{0.2};
        // material.roughness = get_pbrt_roughnessf(0.2f, true);
        material.sspecular = material.specular *
                             eta_to_reflectivity(material.eta);
      } else if (bsdffile == "ceramic.bsdf") {
        material.diffuse   = {0.6f, 0.6f, 0.6f};
        material.specular  = {1, 1, 1};
        material.eta       = vec3f{1.5};
        material.roughness = vec2f{0.25};
        // material.roughness = get_pbrt_roughnessf(0.25, true);
        material.sspecular = material.specular *
                             eta_to_reflectivity(material.eta);
      } else if (bsdffile == "leather.bsdf") {
        material.diffuse   = {0.6f, 0.57f, 0.48f};
        material.specular  = {1, 1, 1};
        material.eta       = vec3f{1.5};
        material.roughness = vec2f{0.3};
        // material.roughness = get_pbrt_roughnessf(0.3, true);
        material.sspecular = material.specular *
                             eta_to_reflectivity(material.eta);
      } else if (bsdffile == "coated_copper.bsdf") {
        material.specular  = vec3f{1};
        material.eta       = vec3f{0.2004376970f, 0.9240334304f, 1.1022119527f};
        material.etak      = vec3f{3.9129485033f, 2.4528477015f, 2.1421879552f};
        material.roughness = vec2f{0.01};
        // material.roughness = get_pbrt_roughnessf(0.01, true);
        material.sspecular = material.specular *
                             eta_to_reflectivity(material.eta, material.etak);
      } else if (bsdffile == "roughglass_alpha_0.2.bsdf") {
        material.specular     = {1, 1, 1};
        material.eta          = vec3f{1.5};
        material.transmission = {1, 1, 1};
        material.roughness    = vec2f{0.2};
        // material.roughness = get_pbrt_roughness(0.2, true);
        material.sspecular = material.specular *
                             eta_to_reflectivity(material.eta);
      } else if (bsdffile == "roughgold_alpha_0.2.bsdf") {
        material.specular  = vec3f{1, 1, 1};
        material.eta       = vec3f{0.1431189557f, 0.3749570432f, 1.4424785571f};
        material.etak      = vec3f{3.9831604247f, 2.3857207478f, 1.6032152899f};
        material.roughness = vec2f{0.2};
        // material.roughness = get_pbrt_roughness(0.2, true);
        material.sspecular = material.specular *
                             eta_to_reflectivity(material.eta, material.etak);
      } else {
        throw std::runtime_error("unsupported bsdffile " + bsdffile);
      }
    } else {
      throw std::runtime_error("unsupported material type" + material.type);
    }
  }
}

// Convert pbrt arealights
static void convert_pbrt_arealights(
    vector<pbrt_arealight>& lights, bool verbose = false) {
  for (auto& light : lights) {
    auto& values = light.values;
    if (light.type == "diffuse") {
      light.emission = get_pbrt_value(values, "L", vec3f{1, 1, 1}) *
                       get_pbrt_value(values, "scale", vec3f{1, 1, 1});
    } else {
      throw std::runtime_error("unsupported arealight type " + light.type);
    }
  }
}

// Convert pbrt lights
static void convert_pbrt_lights(
    vector<pbrt_light>& lights, bool verbose = false) {
  for (auto& light : lights) {
    auto& values = light.values;
    if (light.type == "distant") {
      light.emission = get_pbrt_value(values, "scale", vec3f{1, 1, 1}) *
                       get_pbrt_value(values, "L", vec3f{1, 1, 1});
      light.from    = get_pbrt_value(values, "from", vec3f{0, 0, 0});
      light.to      = get_pbrt_value(values, "to", vec3f{0, 0, 1});
      light.distant = true;
    } else if (light.type == "point") {
      light.emission = get_pbrt_value(values, "scale", vec3f{1, 1, 1}) *
                       get_pbrt_value(values, "I", vec3f{1, 1, 1});
      light.from = get_pbrt_value(values, "from", vec3f{0, 0, 0});
    } else if (light.type == "goniometric") {
      light.emission = get_pbrt_value(values, "scale", vec3f{1, 1, 1}) *
                       get_pbrt_value(values, "I", vec3f{1, 1, 1});
    } else if (light.type == "spot") {
      light.emission = get_pbrt_value(values, "scale", vec3f{1, 1, 1}) *
                       get_pbrt_value(values, "I", vec3f{1, 1, 1});
    } else {
      throw std::runtime_error("unsupported light type " + light.type);
    }
  }
}

static void convert_pbrt_environments(vector<pbrt_environment>& environments,
    vector<pbrt_texture>& textures, bool verbose = false) {
  for (auto& light : environments) {
    auto& values = light.values;
    if (light.type == "infinite") {
      light.emission = get_pbrt_value(values, "scale", vec3f{1, 1, 1}) *
                       get_pbrt_value(values, "L", vec3f{1, 1, 1});
      light.emission_map = get_pbrt_value(values, "mapname", ""s);
      // environment.frame =
      // frame3f{{1,0,0},{0,0,-1},{0,-1,0},{0,0,0}}
      // * stack.back().frame;
      light.frame = light.frame *
                    frame3f{{1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 0, 0}};
      light.frend = light.frend *
                    frame3f{{1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 0, 0}};
      if (!light.emission_map.empty()) {
        auto& texture    = textures.emplace_back();
        texture.name     = light.emission_map;
        texture.filename = light.emission_map;
        texture.type     = "imagemap";
        texture.values.push_back(
            make_pbrt_value("filename", light.emission_map));
      }
    } else {
      throw std::runtime_error("unsupported environment type " + light.type);
    }
  }
}

// Make a triangle shape from a quad grid
template <typename PositionFunc, typename NormalFunc>
static inline void make_pbrt_shape(vector<vec3i>& triangles,
    vector<vec3f>& positions, vector<vec3f>& normals, vector<vec2f>& texcoords,
    int usteps, int vsteps, const PositionFunc& position_func,
    const NormalFunc& normal_func) {
  auto vid = [usteps](int i, int j) { return j * (usteps + 1) + i; };
  auto tid = [usteps](int i, int j, int c) { return (j * usteps + i) * 2 + c; };
  positions.resize((usteps + 1) * (vsteps + 1));
  normals.resize((usteps + 1) * (vsteps + 1));
  texcoords.resize((usteps + 1) * (vsteps + 1));
  for (auto j = 0; j < vsteps + 1; j++) {
    for (auto i = 0; i < usteps + 1; i++) {
      auto uv              = vec2f{i / (float)usteps, j / (float)vsteps};
      positions[vid(i, j)] = position_func(uv);
      normals[vid(i, j)]   = normal_func(uv);
      texcoords[vid(i, j)] = uv;
    }
  }
  triangles.resize(usteps * vsteps * 2);
  for (auto j = 0; j < vsteps; j++) {
    for (auto i = 0; i < usteps; i++) {
      triangles[tid(i, j, 0)] = {vid(i, j), vid(i + 1, j), vid(i + 1, j + 1)};
      triangles[tid(i, j, 1)] = {vid(i, j), vid(i + 1, j + 1), vid(i, j + 1)};
    }
  }
}

// Convert pbrt shapes
static void convert_pbrt_shapes(
    vector<pbrt_shape>& shapes, bool verbose = false) {
  for (auto& shape : shapes) {
    auto& values = shape.values;
    if (shape.type == "trianglemesh") {
      get_pbrt_value(values, "P", shape.positions, {});
      get_pbrt_value(values, "N", shape.normals, {});
      get_pbrt_value(values, "uv", shape.texcoords, {});
      for (auto& uv : shape.texcoords) uv.y = (1 - uv.y);
      get_pbrt_value(values, "indices", shape.triangles, {});
    } else if (shape.type == "loopsubdiv") {
      get_pbrt_value(values, "P", shape.positions, {});
      get_pbrt_value(values, "indices", shape.triangles, {});
      shape.normals.resize(shape.positions.size());
      // compute_normals(shape.normals, shape.triangles, shape.positions);
    } else if (shape.type == "plymesh") {
      shape.filename = get_pbrt_value(values, "filename", ""s);
    } else if (shape.type == "sphere") {
      shape.radius = get_pbrt_value(values, "radius", 1.0f);
      make_pbrt_shape(
          shape.triangles, shape.positions, shape.normals, shape.texcoords, 32,
          16,
          [radius = shape.radius](const vec2f& uv) {
            auto pt = vec2f{2 * pif * uv.x, pif * (1 - uv.y)};
            return radius * vec3f{cos(pt.x) * sin(pt.y), sin(pt.x) * sin(pt.y),
                                cos(pt.y)};
          },
          [](const vec2f& uv) {
            auto pt = vec2f{2 * pif * uv.x, pif * (1 - uv.y)};
            return vec3f{
                cos(pt.x) * cos(pt.y), sin(pt.x) * cos(pt.y), sin(pt.y)};
          });
      // auto params         = proc_shape_params{};
      // params.type         = proc_shape_params::type_t::uvsphere;
      // params.subdivisions = 5;
      // params.scale        = radius;
      // make_proc_shape(shape.triangles, shape.quads, shape.positions,
      //     shape.normals, shape.texcoords, params);
    } else if (shape.type == "disk") {
      shape.radius = get_pbrt_value(values, "radius", 1.0f);
      make_pbrt_shape(
          shape.triangles, shape.positions, shape.normals, shape.texcoords, 32,
          1,
          [radius = shape.radius](const vec2f& uv) {
            auto a = 2 * pif * uv.x;
            return radius * (1 - uv.y) *
                   vec3f{cos(a), sin(a), 0};
          },
          [](const vec2f& uv) {
            return vec3f{0, 0, 1};
          });
      // auto params         = proc_shape_params{};
      // params.type         = proc_shape_params::type_t::uvdisk;
      // params.subdivisions = 4;
      // params.scale        = radius;
      // make_proc_shape(shape.triangles, shape.quads, shape.positions,
      //     shape.normals, shape.texcoords, params);
    } else {
      throw std::runtime_error("unsupported shape type " + shape.type);
    }
  }
}

static void remove_pbrt_materials(
    vector<pbrt_material>& materials, const vector<pbrt_shape>& shapes) {
  auto material_map = unordered_map<string, int>{};
  for (auto& shape : shapes) material_map[shape.material] = 1;
  materials.erase(std::remove_if(materials.begin(), materials.end(),
                      [&material_map](const pbrt_material& material) {
                        return material_map.find(material.name) ==
                               material_map.end();
                      }),
      materials.end());
}
static void remove_pbrt_textures(
    vector<pbrt_texture>& textures, const vector<pbrt_material>& materials) {
  auto texture_map = unordered_map<string, int>{};
  for (auto& material : materials) {
    if (material.diffuse_map != "") texture_map[material.diffuse_map] = 1;
    if (material.specular_map != "") texture_map[material.specular_map] = 1;
    if (material.transmission_map != "")
      texture_map[material.transmission_map] = 1;
    if (material.eta_map != "") texture_map[material.eta_map] = 1;
    if (material.etak_map != "") texture_map[material.etak_map] = 1;
    if (material.opacity_map != "") texture_map[material.opacity_map] = 1;
  }
  textures.erase(std::remove_if(textures.begin(), textures.end(),
                     [&texture_map](const pbrt_texture& texture) {
                       return texture_map.find(texture.name) ==
                              texture_map.end();
                     }),
      textures.end());
}

// pbrt stack ctm
struct pbrt_context {
  frame3f transform_start        = identity3x4f;
  frame3f transform_end          = identity3x4f;
  string  material               = "";
  string  arealight              = "";
  string  medium_interior        = "";
  string  medium_exterior        = "";
  bool    reverse                = false;
  bool    active_transform_start = true;
  bool    active_transform_end   = true;
  float   last_lookat_distance   = 0;
  float   last_film_aspect       = 0;
};

// load pbrt
void load_pbrt(const string& filename, pbrt_model& pbrt) {
  auto files = vector<file_wrapper>{};
  open_file(files.emplace_back(), filename);

  // parser state
  auto   stack      = vector<pbrt_context>{};
  string cur_object = "";

  // objects and coords
  unordered_map<string, pbrt_context> coordsys = {};
  unordered_map<string, vector<int>>  objects  = {};

  // helpers
  auto set_transform = [](pbrt_context& ctx, const frame3f& xform) {
    if (ctx.active_transform_start) ctx.transform_start = xform;
    if (ctx.active_transform_end) ctx.transform_end = xform;
  };
  auto concat_transform = [](pbrt_context& ctx, const frame3f& xform) {
    if (ctx.active_transform_start) ctx.transform_start *= xform;
    if (ctx.active_transform_end) ctx.transform_end *= xform;
  };

  // init stack
  if (stack.empty()) stack.emplace_back();

  // parse command by command
  while (!files.empty()) {
    auto line = ""s;
    while (read_pbrt_cmdline(files.back(), line)) {
      auto str = string_view{line};
      // get command
      auto cmd = ""s;
      parse_pbrt_command(str, cmd);
      if (cmd == "WorldBegin") {
        stack.push_back({});
      } else if (cmd == "WorldEnd") {
        if (stack.empty()) throw std::runtime_error("bad pbrt stack");
        stack.pop_back();
        if (stack.size() != 1) throw std::runtime_error("bad stack");
      } else if (cmd == "AttributeBegin") {
        stack.push_back(stack.back());
      } else if (cmd == "AttributeEnd") {
        if (stack.empty()) throw std::runtime_error("bad pbrt stack");
        stack.pop_back();
      } else if (cmd == "TransformBegin") {
        stack.push_back(stack.back());
      } else if (cmd == "TransformEnd") {
        if (stack.empty()) throw std::runtime_error("bad pbrt stack");
        stack.pop_back();
      } else if (cmd == "ObjectBegin") {
        stack.push_back(stack.back());
        parse_pbrt_param(str, cur_object);
        objects[cur_object] = {};
      } else if (cmd == "ObjectEnd") {
        stack.pop_back();
        cur_object = "";
      } else if (cmd == "ObjectInstance") {
        auto object = ""s;
        parse_pbrt_param(str, object);
        if (objects.find(object) == objects.end())
          throw std::runtime_error("cannot find object " + object);
        for (auto shape_id : objects.at(object)) {
          auto& shape = pbrt.shapes[shape_id];
          shape.instance_frames.push_back(stack.back().transform_start);
          shape.instance_frends.push_back(stack.back().transform_end);
        }
      } else if (cmd == "ActiveTransform") {
        auto name = ""s;
        parse_pbrt_param(str, name);
        if (name == "StartTime") {
          stack.back().active_transform_start = true;
          stack.back().active_transform_end   = false;
        } else if (name == "EndTime") {
          stack.back().active_transform_start = false;
          stack.back().active_transform_end   = true;
        } else if (name == "All") {
          stack.back().active_transform_start = true;
          stack.back().active_transform_end   = true;
        } else {
          throw std::runtime_error("bad active transform");
        }
      } else if (cmd == "Transform") {
        auto xf = identity4x4f;
        parse_pbrt_param(str, xf);
        set_transform(stack.back(), frame3f{xf});
      } else if (cmd == "ConcatTransform") {
        auto xf = identity4x4f;
        parse_pbrt_param(str, xf);
        concat_transform(stack.back(), frame3f{xf});
      } else if (cmd == "Scale") {
        auto v = zero3f;
        parse_pbrt_param(str, v);
        concat_transform(stack.back(), scaling_frame(v));
      } else if (cmd == "Translate") {
        auto v = zero3f;
        parse_pbrt_param(str, v);
        concat_transform(stack.back(), translation_frame(v));
      } else if (cmd == "Rotate") {
        auto v = zero4f;
        parse_pbrt_param(str, v);
        concat_transform(
            stack.back(), rotation_frame(vec3f{v.y, v.z, v.w}, radians(v.x)));
      } else if (cmd == "LookAt") {
        auto from = zero3f, to = zero3f, up = zero3f;
        parse_pbrt_param(str, from);
        parse_pbrt_param(str, to);
        parse_pbrt_param(str, up);
        auto frame = lookat_frame(from, to, up, true);
        concat_transform(stack.back(), inverse(frame));
        stack.back().last_lookat_distance = length(from - to);
      } else if (cmd == "ReverseOrientation") {
        stack.back().reverse = !stack.back().reverse;
      } else if (cmd == "CoordinateSystem") {
        auto name = ""s;
        parse_pbrt_param(str, name);
        coordsys[name].transform_start = stack.back().transform_start;
        coordsys[name].transform_end   = stack.back().transform_end;
      } else if (cmd == "CoordSysTransform") {
        auto name = ""s;
        parse_pbrt_param(str, name);
        if (coordsys.find(name) != coordsys.end()) {
          stack.back().transform_start = coordsys.at(name).transform_start;
          stack.back().transform_end   = coordsys.at(name).transform_end;
        }
      } else if (cmd == "Integrator") {
        auto& integrator = pbrt.integrators.emplace_back();
        parse_pbrt_param(str, integrator.type);
        parse_pbrt_params(str, integrator.values);
      } else if (cmd == "Sampler") {
        auto& sampler = pbrt.samplers.emplace_back();
        parse_pbrt_param(str, sampler.type);
        parse_pbrt_params(str, sampler.values);
      } else if (cmd == "PixelFilter") {
        auto& filter = pbrt.filters.emplace_back();
        parse_pbrt_param(str, filter.type);
        parse_pbrt_params(str, filter.values);
      } else if (cmd == "Film") {
        auto& film = pbrt.films.emplace_back();
        parse_pbrt_param(str, film.type);
        parse_pbrt_params(str, film.values);
        auto xresolution = 0, yresolution = 0;
        for (auto& value : film.values) {
          if (value.name == "xresolution") xresolution = value.value1i;
          if (value.name == "yresolution") yresolution = value.value1i;
        }
        if (xresolution && yresolution) {
          auto aspect = (float)xresolution / (float)yresolution;
          stack.back().last_film_aspect = aspect;
          for (auto& camera : pbrt.cameras) camera.aspect = aspect;
        }
      } else if (cmd == "Accelerator") {
        auto& accelerator = pbrt.accelerators.emplace_back();
        parse_pbrt_param(str, accelerator.type);
        parse_pbrt_params(str, accelerator.values);
      } else if (cmd == "Camera") {
        auto& camera = pbrt.cameras.emplace_back();
        parse_pbrt_param(str, camera.type);
        parse_pbrt_params(str, camera.values);
        camera.frame  = stack.back().transform_start;
        camera.frend  = stack.back().transform_end;
        camera.focus  = stack.back().last_lookat_distance;
        camera.aspect = stack.back().last_film_aspect;
      } else if (cmd == "Texture") {
        auto& texture  = pbrt.textures.emplace_back();
        auto  comptype = ""s;
        parse_pbrt_param(str, texture.name);
        parse_pbrt_param(str, comptype);
        parse_pbrt_param(str, texture.type);
        parse_pbrt_params(str, texture.values);
      } else if (cmd == "Material") {
        static auto material_id = 0;
        auto&       material    = pbrt.materials.emplace_back();
        material.name           = "material_" + std::to_string(material_id++);
        parse_pbrt_param(str, material.type);
        parse_pbrt_params(str, material.values);
        if (material.type == "") {
          stack.back().material = "";
          pbrt.materials.pop_back();
        } else {
          stack.back().material = material.name;
        }
      } else if (cmd == "MakeNamedMaterial") {
        auto& material = pbrt.materials.emplace_back();
        parse_pbrt_param(str, material.name);
        parse_pbrt_params(str, material.values);
        material.type = "";
        for (auto& value : material.values)
          if (value.name == "type") material.type = value.value1s;
      } else if (cmd == "NamedMaterial") {
        parse_pbrt_param(str, stack.back().material);
      } else if (cmd == "Shape") {
        auto& shape = pbrt.shapes.emplace_back();
        parse_pbrt_param(str, shape.type);
        parse_pbrt_params(str, shape.values);
        shape.frame     = stack.back().transform_start;
        shape.frend     = stack.back().transform_end;
        shape.material  = stack.back().material;
        shape.arealight = stack.back().arealight;
        shape.interior  = stack.back().medium_interior;
        shape.exterior  = stack.back().medium_exterior;
        if (cur_object != "") {
          shape.is_instanced = true;
          objects[cur_object].push_back((int)pbrt.shapes.size() - 1);
        } else {
          shape.instance_frames.push_back(identity3x4f);
          shape.instance_frends.push_back(identity3x4f);
        }
      } else if (cmd == "AreaLightSource") {
        static auto arealight_id = 0;
        auto&       arealight    = pbrt.arealights.emplace_back();
        arealight.name = "arealight_" + std::to_string(arealight_id++);
        parse_pbrt_param(str, arealight.type);
        parse_pbrt_params(str, arealight.values);
        arealight.frame        = stack.back().transform_start;
        arealight.frend        = stack.back().transform_end;
        stack.back().arealight = arealight.name;
      } else if (cmd == "LightSource") {
        auto& light = pbrt.lights.emplace_back();
        parse_pbrt_param(str, light.type);
        parse_pbrt_params(str, light.values);
        light.frame = stack.back().transform_start;
        light.frend = stack.back().transform_end;
        if (light.type == "infinite") {
          auto& environment  = pbrt.environments.emplace_back();
          environment.type   = light.type;
          environment.values = light.values;
          environment.frame  = light.frame;
          environment.frend  = light.frend;
          pbrt.lights.pop_back();
        }
      } else if (cmd == "MakeNamedMedium") {
        auto& medium = pbrt.mediums.emplace_back();
        parse_pbrt_param(str, medium.name);
        parse_pbrt_params(str, medium.values);
        medium.type = "";
        for (auto& value : medium.values)
          if (value.name == "type") medium.type = value.value1s;
      } else if (cmd == "MediumInterface") {
        parse_pbrt_param(str, stack.back().medium_interior);
        parse_pbrt_param(str, stack.back().medium_exterior);
      } else if (cmd == "Include") {
        auto filename = ""s;
        parse_pbrt_param(str, filename);
        open_file(
            files.emplace_back(), fs::path(filename).parent_path() / filename);
      } else {
        throw std::runtime_error("unknown command " + cmd);
      }
    }
    files.pop_back();
  }

  // convert objects
  convert_pbrt_cameras(pbrt.cameras);
  convert_pbrt_textures(pbrt.textures);
  convert_pbrt_materials(pbrt.materials, pbrt.textures);
  convert_pbrt_shapes(pbrt.shapes);
  convert_pbrt_lights(pbrt.lights);
  convert_pbrt_arealights(pbrt.arealights);
  convert_pbrt_environments(pbrt.environments, pbrt.textures);

  // load ply data
  for (auto& shape : pbrt.shapes) {
    if (shape.filename.empty()) continue;
    auto ply = ply_model{};
    load_ply(get_dirname(filename) + shape.filename, ply);
    shape.triangles = get_ply_triangles(ply);
    shape.positions = get_ply_positions(ply);
    shape.normals   = get_ply_normals(ply);
    shape.texcoords = get_ply_texcoords(ply);
  }

  // remove_pbrt_materials(pbrt.materials, pbrt.shapes);
  // remove_pbrt_textures(pbrt.textures, pbrt.materials);
}

// Read pbrt commands
bool read_pbrt_command(file_wrapper& fs, pbrt_command_& command, string& name,
    string& type, frame3f& xform, vector<pbrt_value>& values, string& line) {
  // parse command by command
  while (read_pbrt_cmdline(fs, line)) {
    auto str = string_view{line};
    // get command
    auto cmd = ""s;
    parse_pbrt_command(str, cmd);
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
      parse_pbrt_param(str, name);
      command = pbrt_command_::object_begin;
      return true;
    } else if (cmd == "ObjectEnd") {
      command = pbrt_command_::object_end;
      return true;
    } else if (cmd == "ObjectInstance") {
      parse_pbrt_param(str, name);
      command = pbrt_command_::object_instance;
      return true;
    } else if (cmd == "ActiveTransform") {
      parse_pbrt_command(str, name);
      command = pbrt_command_::active_transform;
      return true;
    } else if (cmd == "Transform") {
      auto xf = identity4x4f;
      parse_pbrt_param(str, xf);
      xform   = frame3f{xf};
      command = pbrt_command_::set_transform;
      return true;
    } else if (cmd == "ConcatTransform") {
      auto xf = identity4x4f;
      parse_pbrt_param(str, xf);
      xform   = frame3f{xf};
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "Scale") {
      auto v = zero3f;
      parse_pbrt_param(str, v);
      xform   = scaling_frame(v);
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "Translate") {
      auto v = zero3f;
      parse_pbrt_param(str, v);
      xform   = translation_frame(v);
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "Rotate") {
      auto v = zero4f;
      parse_pbrt_param(str, v);
      xform   = rotation_frame(vec3f{v.y, v.z, v.w}, radians(v.x));
      command = pbrt_command_::concat_transform;
      return true;
    } else if (cmd == "LookAt") {
      auto from = zero3f, to = zero3f, up = zero3f;
      parse_pbrt_param(str, from);
      parse_pbrt_param(str, to);
      parse_pbrt_param(str, up);
      xform   = {from, to, up, zero3f};
      command = pbrt_command_::lookat_transform;
      return true;
    } else if (cmd == "ReverseOrientation") {
      command = pbrt_command_::reverse_orientation;
      return true;
    } else if (cmd == "CoordinateSystem") {
      parse_pbrt_param(str, name);
      command = pbrt_command_::coordinate_system_set;
      return true;
    } else if (cmd == "CoordSysTransform") {
      parse_pbrt_param(str, name);
      command = pbrt_command_::coordinate_system_transform;
      return true;
    } else if (cmd == "Integrator") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::integrator;
      return true;
    } else if (cmd == "Sampler") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::sampler;
      return true;
    } else if (cmd == "PixelFilter") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::filter;
      return true;
    } else if (cmd == "Film") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::film;
      return true;
    } else if (cmd == "Accelerator") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::accelerator;
      return true;
    } else if (cmd == "Camera") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::camera;
      return true;
    } else if (cmd == "Texture") {
      auto comptype = ""s;
      parse_pbrt_param(str, name);
      parse_pbrt_param(str, comptype);
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::named_texture;
      return true;
    } else if (cmd == "Material") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::material;
      return true;
    } else if (cmd == "MakeNamedMaterial") {
      parse_pbrt_param(str, name);
      parse_pbrt_params(str, values);
      type = "";
      for (auto& value : values)
        if (value.name == "type") type = value.value1s;
      command = pbrt_command_::named_material;
      return true;
    } else if (cmd == "NamedMaterial") {
      parse_pbrt_param(str, name);
      command = pbrt_command_::use_material;
      return true;
    } else if (cmd == "Shape") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::shape;
      return true;
    } else if (cmd == "AreaLightSource") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::arealight;
      return true;
    } else if (cmd == "LightSource") {
      parse_pbrt_param(str, type);
      parse_pbrt_params(str, values);
      command = pbrt_command_::light;
      return true;
    } else if (cmd == "MakeNamedMedium") {
      parse_pbrt_param(str, name);
      parse_pbrt_params(str, values);
      type = "";
      for (auto& value : values)
        if (value.name == "type") type = value.value1s;
      command = pbrt_command_::named_medium;
      return true;
    } else if (cmd == "MediumInterface") {
      auto interior = ""s, exterior = ""s;
      parse_pbrt_param(str, interior);
      parse_pbrt_param(str, exterior);
      name    = interior + "####" + exterior;
      command = pbrt_command_::medium_interface;
      return true;
    } else if (cmd == "Include") {
      parse_pbrt_param(str, name);
      command = pbrt_command_::include;
      return true;
    } else {
      throw std::runtime_error("unknown command " + cmd);
    }
  }
  return false;
}
bool read_pbrt_command(file_wrapper& fs, pbrt_command_& command, string& name,
    string& type, frame3f& xform, vector<pbrt_value>& values) {
  auto command_buffer = ""s;
  return read_pbrt_command(
      fs, command, name, type, xform, values, command_buffer);
}

// Write obj elements
void write_pbrt_comment(file_wrapper& fs, const string& comment) {
  auto lines = split_string(comment, "\n");
  for (auto& line : lines) {
    checked_fprintf(fs, "# %s\n", line.c_str());
  }
  checked_fprintf(fs, "\n");
}

void write_pbrt_values(file_wrapper& fs, const vector<pbrt_value>& values) {
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
    checked_fprintf(fs, " \"%s %s\" ", type_labels.at(value.type).c_str(),
        value.name.c_str());
    switch (value.type) {
      case pbrt_value_type::real:
        if (!value.vector1f.empty()) {
          checked_fprintf(fs, "[ ");
          for (auto& v : value.vector1f) checked_fprintf(fs, " %g", v);
          checked_fprintf(fs, " ]");
        } else {
          checked_fprintf(fs, "%g", value.value1f);
        }
        break;
      case pbrt_value_type::integer:
        if (!value.vector1f.empty()) {
          checked_fprintf(fs, "[ ");
          for (auto& v : value.vector1i) checked_fprintf(fs, " %d", v);
          checked_fprintf(fs, " ]");
        } else {
          checked_fprintf(fs, "%d", value.value1i);
        }
        break;
      case pbrt_value_type::boolean:
        checked_fprintf(fs, "\"%s\"", value.value1b ? "true" : "false");
        break;
      case pbrt_value_type::string:
        checked_fprintf(fs, "\"%s\"", value.value1s.c_str());
        break;
      case pbrt_value_type::point:
      case pbrt_value_type::vector:
      case pbrt_value_type::normal:
      case pbrt_value_type::color:
        if (!value.vector3f.empty()) {
          checked_fprintf(fs, "[ ");
          for (auto& v : value.vector3f)
            checked_fprintf(fs, " %g %g %g", v.x, v.y, v.z);
          checked_fprintf(fs, " ]");
        } else {
          checked_fprintf(fs, "[ %g %g %g ]", value.value3f.x, value.value3f.y,
              value.value3f.z);
        }
        break;
      case pbrt_value_type::spectrum:
        checked_fprintf(fs, "[ ");
        for (auto& v : value.vector1f) checked_fprintf(fs, " %g", v);
        checked_fprintf(fs, " ]");
        break;
      case pbrt_value_type::texture:
        checked_fprintf(fs, "\"%s\"", value.value1s.c_str());
        break;
      case pbrt_value_type::point2:
      case pbrt_value_type::vector2:
        if (!value.vector2f.empty()) {
          checked_fprintf(fs, "[ ");
          for (auto& v : value.vector2f)
            checked_fprintf(fs, " %g %g", v.x, v.y);
          checked_fprintf(fs, " ]");
        } else {
          checked_fprintf(fs, "[ %g %g ]", value.value2f.x, value.value2f.x);
        }
        break;
    }
  }
  checked_fprintf(fs, "\n");
}

void write_pbrt_command(file_wrapper& fs, pbrt_command_ command,
    const string& name, const string& type, const frame3f& xform,
    const vector<pbrt_value>& values, bool texture_float) {
  switch (command) {
    case pbrt_command_::world_begin: checked_fprintf(fs, "WorldBegin\n"); break;
    case pbrt_command_::world_end: checked_fprintf(fs, "WorldEnd\n"); break;
    case pbrt_command_::attribute_begin:
      checked_fprintf(fs, "AttributeBegin\n");
      break;
    case pbrt_command_::attribute_end:
      checked_fprintf(fs, "AttributeEnd\n");
      break;
    case pbrt_command_::transform_begin:
      checked_fprintf(fs, "TransformBegin\n");
      break;
    case pbrt_command_::transform_end:
      checked_fprintf(fs, "TransformEnd\n");
      break;
    case pbrt_command_::object_begin:
      checked_fprintf(fs, "ObjectBegin \"%s\"\n", name.c_str());
      break;
    case pbrt_command_::object_end: checked_fprintf(fs, "ObjectEnd\n"); break;
    case pbrt_command_::object_instance:
      checked_fprintf(fs, "ObjectInstance \"%s\"\n", name.c_str());
      break;
    case pbrt_command_::sampler:
      checked_fprintf(fs, "Sampler \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::integrator:
      checked_fprintf(fs, "Integrator \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::accelerator:
      checked_fprintf(fs, "Accelerator \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::film:
      checked_fprintf(fs, "Film \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::filter:
      checked_fprintf(fs, "Filter \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::camera:
      checked_fprintf(fs, "Camera \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::shape:
      checked_fprintf(fs, "Shape \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::light:
      checked_fprintf(fs, "LightSource \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::material:
      checked_fprintf(fs, "Material \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::arealight:
      checked_fprintf(fs, "AreaLightSource \"%s\"", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::named_texture:
      checked_fprintf(fs, "Texture \"%s\" \"%s\" \"%s\"", name.c_str(),
          texture_float ? "float" : "rgb", type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::named_medium:
      checked_fprintf(fs, "MakeNamedMedium \"%s\" \"string type\" \"%s\"",
          name.c_str(), type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::named_material:
      checked_fprintf(fs, "MakeNamedMaterial \"%s\" \"string type\" \"%s\"",
          name.c_str(), type.c_str());
      write_pbrt_values(fs, values);
      break;
    case pbrt_command_::include:
      checked_fprintf(fs, "Include \"%s\"\n", name.c_str());
      break;
    case pbrt_command_::reverse_orientation:
      checked_fprintf(fs, "ReverseOrientation\n");
      break;
    case pbrt_command_::set_transform:
      checked_fprintf(fs,
          "Transform %g %g %g 0 %g %g %g 0 %g %g %g 0 %g %g %g 1\n", xform.x.x,
          xform.x.y, xform.x.z, xform.y.x, xform.y.y, xform.y.z, xform.z.x,
          xform.z.y, xform.z.z, xform.o.x, xform.o.y, xform.o.z);
      break;
    case pbrt_command_::concat_transform:
      checked_fprintf(fs,
          "ConcatTransform %g %g %g 0 %g %g %g 0 %g %g %g 0 %g %g %g 1\n",
          xform.x.x, xform.x.y, xform.x.z, xform.y.x, xform.y.y, xform.y.z,
          xform.z.x, xform.z.y, xform.z.z, xform.o.x, xform.o.y, xform.o.z);
      break;
    case pbrt_command_::lookat_transform:
      checked_fprintf(fs, "LookAt %g %g %g %g %g %g %g %g %g\n", xform.x.x,
          xform.x.y, xform.x.z, xform.y.x, xform.y.y, xform.y.z, xform.z.x,
          xform.z.y, xform.z.z);
      break;
    case pbrt_command_::use_material:
      checked_fprintf(fs, "NamedMaterial \"%s\"\n", name.c_str());
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
      checked_fprintf(fs, "MediumInterface \"%s\" \"%s\"\n", interior.c_str(),
          exterior.c_str());
    } break;
    case pbrt_command_::active_transform:
      checked_fprintf(fs, "ActiveTransform \"%s\"\n", name.c_str());
      break;
    case pbrt_command_::coordinate_system_set:
      checked_fprintf(fs, "CoordinateSystem \"%s\"\n", name.c_str());
      break;
    case pbrt_command_::coordinate_system_transform:
      checked_fprintf(fs, "CoordinateSysTransform \"%s\"\n", name.c_str());
      break;
  }
}

void write_pbrt_command(file_wrapper& fs, pbrt_command_ command,
    const string& name, const frame3f& xform) {
  return write_pbrt_command(fs, command, name, "", xform, {});
}
void write_pbrt_command(file_wrapper& fs, pbrt_command_ command,
    const string& name, const string& type, const vector<pbrt_value>& values,
    bool texture_as_float) {
  return write_pbrt_command(
      fs, command, name, type, identity3x4f, values, texture_as_float);
}

// get pbrt value
void get_pbrt_value(const pbrt_value& pbrt, string& value) {
  if (pbrt.type == pbrt_value_type::string ||
      pbrt.type == pbrt_value_type::texture) {
    value = pbrt.value1s;
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, bool& value) {
  if (pbrt.type == pbrt_value_type::boolean) {
    value = pbrt.value1b;
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, int& value) {
  if (pbrt.type == pbrt_value_type::integer) {
    value = pbrt.value1i;
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, float& value) {
  if (pbrt.type == pbrt_value_type::real) {
    value = pbrt.value1f;
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, vec2f& value) {
  if (pbrt.type == pbrt_value_type::point2 ||
      pbrt.type == pbrt_value_type::vector2) {
    value = pbrt.value2f;
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, vec3f& value) {
  if (pbrt.type == pbrt_value_type::point ||
      pbrt.type == pbrt_value_type::vector ||
      pbrt.type == pbrt_value_type::normal ||
      pbrt.type == pbrt_value_type::color) {
    value = pbrt.value3f;
  } else if (pbrt.type == pbrt_value_type::real) {
    value = vec3f{pbrt.value1f};
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, vector<float>& value) {
  if (pbrt.type == pbrt_value_type::real) {
    if (!pbrt.vector1f.empty()) {
      value = pbrt.vector1f;
    } else {
      value = {pbrt.value1f};
    }
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, vector<vec2f>& value) {
  if (pbrt.type == pbrt_value_type::point2 ||
      pbrt.type == pbrt_value_type::vector2) {
    if (!pbrt.vector2f.empty()) {
      value = pbrt.vector2f;
    } else {
      value = {pbrt.value2f};
    }
  } else if (pbrt.type == pbrt_value_type::real) {
    if (pbrt.vector1f.empty() || pbrt.vector1f.size() % 2)
      throw std::runtime_error("bad pbrt type");
    value.resize(pbrt.vector1f.size() / 2);
    for (auto i = 0; i < value.size(); i++)
      value[i] = {pbrt.vector1f[i * 2 + 0], pbrt.vector1f[i * 2 + 1]};
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, vector<vec3f>& value) {
  if (pbrt.type == pbrt_value_type::point ||
      pbrt.type == pbrt_value_type::vector ||
      pbrt.type == pbrt_value_type::normal ||
      pbrt.type == pbrt_value_type::color) {
    if (!pbrt.vector3f.empty()) {
      value = pbrt.vector3f;
    } else {
      value = {pbrt.value3f};
    }
  } else if (pbrt.type == pbrt_value_type::real) {
    if (pbrt.vector1f.empty() || pbrt.vector1f.size() % 3)
      throw std::runtime_error("bad pbrt type");
    value.resize(pbrt.vector1f.size() / 3);
    for (auto i = 0; i < value.size(); i++)
      value[i] = {pbrt.vector1f[i * 3 + 0], pbrt.vector1f[i * 3 + 1],
          pbrt.vector1f[i * 3 + 2]};
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}

void get_pbrt_value(const pbrt_value& pbrt, vector<int>& value) {
  if (pbrt.type == pbrt_value_type::integer) {
    if (!pbrt.vector1i.empty()) {
      value = pbrt.vector1i;
    } else {
      value = {pbrt.vector1i};
    }
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, vector<vec3i>& value) {
  if (pbrt.type == pbrt_value_type::integer) {
    if (pbrt.vector1i.empty() || pbrt.vector1i.size() % 3)
      throw std::runtime_error("bad pbrt type");
    value.resize(pbrt.vector1i.size() / 3);
    for (auto i = 0; i < value.size(); i++)
      value[i] = {pbrt.vector1i[i * 3 + 0], pbrt.vector1i[i * 3 + 1],
          pbrt.vector1i[i * 3 + 2]};
  } else {
    throw std::runtime_error("bad pbrt type");
  }
}
void get_pbrt_value(const pbrt_value& pbrt, pair<float, string>& value) {
  if (pbrt.type == pbrt_value_type::string) {
    value.first = 0;
    get_pbrt_value(pbrt, value.second);
  } else {
    get_pbrt_value(pbrt, value.first);
    value.second = "";
  }
}
void get_pbrt_value(const pbrt_value& pbrt, pair<vec3f, string>& value) {
  if (pbrt.type == pbrt_value_type::string ||
      pbrt.type == pbrt_value_type::texture) {
    value.first = zero3f;
    get_pbrt_value(pbrt, value.second);
  } else {
    get_pbrt_value(pbrt, value.first);
    value.second = "";
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
  pbrt.value1i = value;
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
