/* 
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file utest_llvm.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 *
 * Compile llvm kernels to gen back-end kernels
 */

#include "utest/utest.hpp"
#include "llvm/llvm_to_gen.hpp"
#include "ir/unit.hpp"
#include <cstdlib>

namespace gbe
{
  /*! Where the kernels to test are */
  static std::string kernelPath;

  char *copyString(const char *src) {
    const size_t len = strlen(src);
    char *dst = GBE_NEW_ARRAY(char, len + 1);
    std::memcpy(dst, src, len);
    dst[len] = 0;
    return dst;
  }

  static void utestLLVM2Gen(const char *kernel)
  {
    const std::string path = kernelPath + kernel;
    ir::Unit unit;
    llvmToGen(unit, path.c_str());
    std::cout << unit << std::endl;
  }
} /* namespace gbe */

static void utestLLVM(void)
{
  using namespace gbe;

  // Try to find where the kernels are
  FILE *dummyKernel = NULL;
#define TRY_PATH(PATH)                                      \
  if ((dummyKernel = fopen(PATH"dummy.ll", "r")) != NULL) { \
    kernelPath = PATH;                                      \
    goto runTests;                                          \
  }
  TRY_PATH("./kernels/")
  TRY_PATH("../kernels/")
  TRY_PATH("../../kernels/")
  TRY_PATH("../../../kernels/")
#undef TRY_PATH

  std::cout << "Failed to find valid kernel path" << std::endl;
  return;

runTests:
  std::cout << "  kernel path is: \"" << kernelPath << "\"" << std::endl;
  GBE_ASSERT(dummyKernel != NULL);
  fclose(dummyKernel);

  UTEST_EXPECT_SUCCESS(utestLLVM2Gen("add.ll"));
  //UTEST_EXPECT_SUCCESS(utestLLVM2Gen("add2.ll"));
  //UTEST_EXPECT_SUCCESS(utestLLVM2Gen("loop.ll"));
  //UTEST_EXPECT_SUCCESS(utestLLVM2Gen("void.ll"));
}

UTEST_REGISTER(utestLLVM)

