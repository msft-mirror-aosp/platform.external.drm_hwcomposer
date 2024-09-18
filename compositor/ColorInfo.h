/*
 * Copyright (C) 2024 The Android Open Source Project
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

#pragma once

#include <cstdint>

enum class Colorspace : int32_t {
  kDefault,
  kSmpte170MYcc,
  kBt709Ycc,
  kXvycc601,
  kXvycc709,
  kSycc601,
  kOpycc601,
  kOprgb,
  kBt2020Cycc,
  kBt2020Rgb,
  kBt2020Ycc,
  kDciP3RgbD65,
  kDciP3RgbTheater,
  kRgbWideFixed,
  kRgbWideFloat,
  kBt601Ycc,
};