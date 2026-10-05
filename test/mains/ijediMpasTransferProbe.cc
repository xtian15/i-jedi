/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "ijedi/Python/PythonRuntime.h"
#include "pybind11/stl.h"

namespace py = pybind11;
namespace {
template <typename T>
std::vector<T> listTransfer(const py::object &tensor) {
  return tensor.attr("detach")()
      .attr("cpu")()
      .attr("contiguous")()
      .attr("reshape")(-1)
      .attr("tolist")()
      .template cast<std::vector<T>>();
}
template <typename T>
std::vector<T> bufferTransfer(const py::object &tensor) {
  const auto array = tensor.attr("detach")()
                         .attr("cpu")()
                         .attr("contiguous")()
                         .attr("reshape")(-1)
                         .attr("numpy")()
                         .template cast<py::buffer>();
  const auto info = array.request();
  if (info.ndim != 1 || info.itemsize != sizeof(T) || !info.item_type_is_equivalent_to<T>() ||
      info.strides.at(0) != sizeof(T) || info.size < 0 ||
      static_cast<size_t>(info.size) > 256 * 1024 * 1024 / sizeof(T)) {
    throw std::runtime_error("noncanonical CPU tensor buffer: format=" + info.format +
                             " expected=" + py::format_descriptor<T>::format() +
                             " ndim=" + std::to_string(info.ndim) +
                             " itemsize=" + std::to_string(info.itemsize) +
                             " stride=" + std::to_string(info.strides.at(0)));
  }
  std::vector<T> result(info.size);
  if (!result.empty()) {
    std::memcpy(result.data(), info.ptr, result.size() * sizeof(T));
  }
  return result;
}
template <typename T>
nlohmann::json probe(const py::object &tensor) {
  nlohmann::json result;
  for (int repeat = 0; repeat < 5; ++repeat) {
    const auto begin = std::chrono::steady_clock::now();
    const auto old = listTransfer<T>(tensor);
    const auto copied = std::chrono::steady_clock::now();
    const auto candidate = bufferTransfer<T>(tensor);
    const auto finish = std::chrono::steady_clock::now();
    if (candidate.size() != old.size() ||
        std::memcmp(old.data(), candidate.data(), old.size() * sizeof(T)) != 0) {
      throw std::runtime_error("buffer transfer differs from existing model bridge bytes");
    }
    result.push_back(
        {{"repeat", repeat},
         {"values", old.size()},
         {"bytes", old.size() * sizeof(T)},
         {"existing_list_seconds", std::chrono::duration<double>(copied - begin).count()},
         {"candidate_buffer_seconds", std::chrono::duration<double>(finish - copied).count()}});
  }
  return result;
}
}  // namespace
int main(int argc, char **argv) {
  if (argc != 3) {
    throw std::invalid_argument("transfer probe needs exact Python executable and output path");
  }
  ijedi::PythonRuntime runtime(argv[1]);
  const auto torch = py::module_::import("torch");
  torch.attr("set_num_threads")(1);
  torch.attr("set_num_interop_threads")(1);
  const auto shape = py::make_tuple(10242, 55, 6);
  const auto values =
      torch.attr("arange")(10242 * 55 * 6, py::arg("dtype") = torch.attr("float64"));
  values.attr("__setitem__")(0, -0.);
  values.attr("__setitem__")(1, std::numeric_limits<double>::denorm_min());
  values.attr("__setitem__")(2, std::numeric_limits<double>::lowest());
  values.attr("__setitem__")(3, std::numeric_limits<double>::max());
  const auto integers = torch.attr("arange")(10242 * 7, py::arg("dtype") = torch.attr("int64"));
  integers.attr("__setitem__")(0, std::numeric_limits<std::int64_t>::lowest());
  integers.attr("__setitem__")(1, std::numeric_limits<std::int64_t>::max());
  const nlohmann::json output{
      {"scope", "synthetic_FFI_byte_and_timing_probe_not_model_qualification"},
      {"float64", probe<double>(values.attr("reshape")(shape))},
      {"int64", probe<std::int64_t>(integers)}};
  std::ofstream file(argv[2]);
  file << output.dump(2) << '\n';
  if (!file) {
    throw std::runtime_error("cannot retain transfer probe");
  }
}
