/**
 * @file main.cpp
 * @brief Shared Google Test entry point — linked into every test executable via gtest_shared_main.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include <gtest/gtest.h>

int
main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
