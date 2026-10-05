#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ijedi/Python/MpasPythonAdapter.h"
#include "ijedi/Python/PythonRuntime.h"

int main(int argc, char **argv) {
  if (argc != 9) {
    std::cerr << "usage: " << argv[0]
              << " PYTHON_EXE WHEEL EXPECTED_SHA256 INIT_NC GRID_NC NAMELIST "
                 "DT_SECONDS OUTPUT_JSON\n";
    return 2;
  }
  try {
    const double dt = std::stod(argv[7]);
    std::string result;
    {
      ijedi::PythonRuntime runtime(argv[1]);
      ijedi::MpasPythonAdapter adapter(runtime);
      result = adapter.runTwoStepAudit(argv[2], argv[3], argv[4], argv[5], argv[6], dt);
    }
    std::ofstream output(argv[8], std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open output file");
    output << result << '\n';
    if (!output) throw std::runtime_error("failed to write output file");
    std::cout << result << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
