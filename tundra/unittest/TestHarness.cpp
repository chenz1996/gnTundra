#include "TestHarness.hpp"
#include "gtest/gtest.h"
#include "src/gtest-all.cc"
#include "Banned.hpp"

int main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

