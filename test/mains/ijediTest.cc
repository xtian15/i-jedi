#include <functional>
#include <map>

#include "ijedi/Traits.h"

#include "oops/runs/Run.h"
#include "oops/test/interface/Geometry.h"
#include "oops/test/interface/GeometryIterator.h"
#include "oops/test/interface/Increment.h"
#include "oops/test/interface/LinearModel.h"
#include "oops/test/interface/Model.h"
#include "oops/test/interface/State.h"

// -------------------------------------------------------------------------------------------------

int runApp(int argc, char **argv, const std::string testName)
{
  // Create the Run object
  oops::Run run(argc, argv);

  // Test application pointer
  std::unique_ptr<oops::Application> app;

  // Define a map from app names to lambda functions that create unique_ptr to Applications
  std::map<std::string, std::function<std::unique_ptr<oops::Application>()>> tests;

  tests["geometry"] = []()
  {
    return std::make_unique<test::Geometry<ijedi::Traits>>();
  };
  tests["state"] = []()
  {
    return std::make_unique<test::State<ijedi::Traits>>();
  };
  tests["increment"] = []()
  {
    return std::make_unique<test::Increment<ijedi::Traits>>();
  };
  tests["model"] = []()
  {
    return std::make_unique<test::Model<ijedi::Traits>>();
  };
  tests["linearmodel"] = []()
  {
    return std::make_unique<test::LinearModel<ijedi::Traits>>();
  };
  tests["geometryiterator"] = []()
  {
    return std::make_unique<test::GeometryIterator<ijedi::Traits>>();
  };

  // Create application object and point to it
  auto it = tests.find(testName);

  // Run the application
  return run.execute(*(it->second()));
}

// -------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
  // Check that the number of arguments is correct
  // ----------------------------------------------
  ASSERT_MSG(argc >= 2, "Usage: " + std::string(argv[0]) + " <test> <options>");

  // Get the application to be run
  std::string testApp = argv[1];
  for (char &c : testApp)
  {
    c = std::tolower(c);
  }

  // Check that the test is recognized
  // ----------------------------------------
  const std::set<std::string> validtests = {
      "geometry",
      "geometryiterator",
      "state",
      "increment",
      "linearmodel",
      "model",
  };
  ASSERT_MSG(validtests.find(testApp) != validtests.end(), "Test not recognized: " + testApp);

  // Remove program from argc and argv
  // ---------------------------------
  argv[1] = argv[0]; // Move executable name to second position
  argv += 1;         // Move pointer up one
  argc -= 1;         // Remove 1 from count

  // Call application specific main functions
  // ----------------------------------------
  return runApp(argc, argv, testApp);
}

// -------------------------------------------------------------------------------------------------
