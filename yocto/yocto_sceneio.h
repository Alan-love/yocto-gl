//
// # Yocto/SceneIO: Tiny library for Yocto/Scene input and output
//
// Yocto/SceneIO provides loading and saving functionality for scenes
// in Yocto/GL. We support a simple to use YAML format, PLY, OBJ and glTF.
// The YAML serialization is a straight copy of the in-memory scene data.
// To speed up testing, we also support a binary format that is a dump of
// the current scene. This format should not be use for archival though.
//
// Error reporting is done through exceptions using the `io_error` exception.
//
// ## Scene Loading and Saving
//
// 1. load a scene with `load_scene()` and save it with `save_scene()`
//
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

#ifndef _YOCTO_SCENEIO_H_
#define _YOCTO_SCENEIO_H_

// -----------------------------------------------------------------------------
// INCLUDES
// -----------------------------------------------------------------------------

#include <atomic>
#include "yocto_scene.h"
#include "yocto_shape.h"

// -----------------------------------------------------------------------------
// SCENE IO FUNCTIONS
// -----------------------------------------------------------------------------

namespace yocto {

// Result of file io operations.
enum struct sceneio_status {
  // clang-format off
  ok, file_not_found, io_error, bad_data, unsupported_format,
  bad_texture, bad_shape, bad_voltexture, bad_subdiv
  // clang-format on
};
struct [[nodiscard]] sceneio_result {
  sceneio_status status        = sceneio_status::ok;
  int            line          = 0;
  imageio_result istatus       = {};
  shapeio_result sstatus       = {};
  int            element_index = -1;

  operator bool() const { return status == sceneio_status::ok; }
};

// Scene load params
struct load_params {
  bool               notextures  = false;
  bool               facevarying = false;
  std::atomic<bool>* cancel      = nullptr;
  bool               noparallel  = false;
};

// Scene save params
struct save_params {
  bool               notextures   = false;
  bool               objinstances = false;
  std::atomic<bool>* cancel       = nullptr;
  bool               noparallel   = false;
};

// Load/save a scene in the supported formats.
sceneio_result load_scene(
    const string& filename, yocto_scene& scene, const load_params& params = {});
sceneio_result save_scene(const string& filename, const yocto_scene& scene,
    const save_params& params = {});

// Load/save scene textures
imageio_result load_texture(yocto_texture& texture, const string& dirname);
imageio_result save_texture(
    const yocto_texture& texture, const string& dirname);
imageio_result load_voltexture(
    yocto_voltexture& texture, const string& dirname);
imageio_result save_voltexture(
    const yocto_voltexture& texture, const string& dirname);

// Load/save scene shapes
shapeio_result load_shape(yocto_shape& shape, const string& dirname);
shapeio_result save_shape(const yocto_shape& shape, const string& dirname);
shapeio_result load_subdiv(yocto_subdiv& subdiv, const string& dirname);
shapeio_result save_subdiv(const yocto_subdiv& subdiv, const string& dirname);

}  // namespace yocto

#endif
