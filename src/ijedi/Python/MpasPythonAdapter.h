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

class MpasPythonAdapter {
 public:
  explicit MpasPythonAdapter(const PythonRuntime &);

  std::string runTwoStepAudit(const std::string &wheelPath,
                              const std::string &expectedWheelSha256,
                              const std::string &initPath,
                              const std::string &gridPath,
                              const std::string &namelistPath,
                              double timeStepSeconds) const;

 private:
  const PythonRuntime &runtime_;
};

}  // namespace ijedi
