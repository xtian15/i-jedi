/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <string>

#include "ijedi/Python/PythonRuntime.h"

namespace ijedi {

/// Verify both the archive digest and every installed package byte, and reject
/// an imported module that did not come from that exact distribution.
void verifyInstalledMpasWheel(const PythonRuntime &, const std::string &wheelPath,
                              const std::string &expectedWheelSha256);

}  // namespace ijedi
