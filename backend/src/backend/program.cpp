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
 * \file callback interface for the compiler
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */

#include "program.h"
#include "program.hpp"
#include "gen_program.h"
#include "sys/platform.hpp"
#include "sys/cvar.hpp"
#include "ir/liveness.hpp"
#include "ir/value.hpp"
#include "ir/unit.hpp"
#include "llvm/llvm_to_gen.hpp"
#include "llvm/Config/config.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/ManagedStatic.h"
#include <cstring>
#include <algorithm>
#include <fstream>
#include <dlfcn.h>
#include <sstream>
#include <iostream>
#include <unistd.h>

/* Not defined for LLVM 3.0 */
#if !defined(LLVM_VERSION_MAJOR)
#define LLVM_VERSION_MAJOR 3
#endif /* !defined(LLVM_VERSION_MAJOR) */

/* Not defined for LLVM 3.0 */
#if !defined(LLVM_VERSION_MINOR)
#define LLVM_VERSION_MINOR 0
#endif /* !defined(LLVM_VERSION_MINOR) */

#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#if LLVM_VERSION_MINOR <= 1
#include <clang/Frontend/DiagnosticOptions.h>
#else
#include <clang/Basic/DiagnosticOptions.h>
#endif  /* LLVM_VERSION_MINOR <= 1 */
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Basic/TargetOptions.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/OwningPtr.h>
#if LLVM_VERSION_MINOR <= 2
#include <llvm/Module.h>
#else
#include <llvm/IR/Module.h>
#endif  /* LLVM_VERSION_MINOR <= 2 */
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Support/raw_ostream.h>
#include "src/GBEConfig.h"

namespace gbe {

  Kernel::Kernel(const std::string &name) :
    name(name), args(NULL), argNum(0), curbeSize(0), stackSize(0), useSLM(false), slmSize(0), ctx(NULL), samplerSet(NULL), imageSet(NULL)
  {}
  Kernel::~Kernel(void) {
    if(ctx) GBE_DELETE(ctx);
    if(samplerSet) GBE_DELETE(samplerSet);
    if(imageSet) GBE_DELETE(imageSet);
    GBE_SAFE_DELETE_ARRAY(args);
  }
  int32_t Kernel::getCurbeOffset(gbe_curbe_type type, uint32_t subType) const {
    const PatchInfo patch(type, subType);
    const auto it = std::lower_bound(patches.begin(), patches.end(), patch);
    if (it == patches.end()) return -1; // nothing found
    if (patch < *it) return -1; // they are not equal
    return it->offset; // we found it!
  }

  Program::Program(void) : constantSet(NULL) {}
  Program::~Program(void) {
    for (auto &kernel : kernels) GBE_DELETE(kernel.second);
    if (constantSet) delete constantSet;
  }

  BVAR(OCL_OUTPUT_GEN_IR, false);

  bool Program::buildFromLLVMFile(const char *fileName, std::string &error) {
    ir::Unit unit;
    if (llvmToGen(unit, fileName) == false) {
      error = std::string(fileName) + " not found";
      return false;
    }
    this->buildFromUnit(unit, error);
    return true;
  }

  bool Program::buildFromUnit(const ir::Unit &unit, std::string &error) {
    constantSet = new ir::ConstantSet(unit.getConstantSet());
    const auto &set = unit.getFunctionSet();
    const uint32_t kernelNum = set.size();
    if (OCL_OUTPUT_GEN_IR) std::cout << unit;
    if (kernelNum == 0) return true;
    for (const auto &pair : set) {
      const std::string &name = pair.first;
      Kernel *kernel = this->compileKernel(unit, name);
      kernel->setSamplerSet(pair.second->getSamplerSet());
      kernel->setImageSet(pair.second->getImageSet());
      kernel->setCompileWorkGroupSize(pair.second->getCompileWorkGroupSize());
      kernels.insert(std::make_pair(name, kernel));
    }
    return true;
  }

#define OUT_UPDATE_SZ(elt) SERIALIZE_OUT(elt, outs, ret_size)
#define IN_UPDATE_SZ(elt) DESERIALIZE_IN(elt, ins, total_size)

  size_t Program::serializeToBin(std::ostream& outs) {
    size_t ret_size = 0;
    size_t ker_num = kernels.size();
    int has_constset = 0;

    OUT_UPDATE_SZ(magic_begin);

    if (constantSet) {
      has_constset = 1;
      OUT_UPDATE_SZ(has_constset);
      size_t sz = constantSet->serializeToBin(outs);
      if (!sz)
        return 0;

      ret_size += sz;
    } else {
      OUT_UPDATE_SZ(has_constset);
    }

    OUT_UPDATE_SZ(ker_num);
    for (auto ker : kernels) {
      size_t sz = ker.second->serializeToBin(outs);
      if (!sz)
        return 0;

      ret_size += sz;
    }

    OUT_UPDATE_SZ(magic_end);

    OUT_UPDATE_SZ(ret_size);
    return ret_size;
  }

  size_t Program::deserializeFromBin(std::istream& ins) {
    size_t total_size = 0;
    int has_constset = 0;
    size_t ker_num;
    uint32_t magic;

    IN_UPDATE_SZ(magic);
    if (magic != magic_begin)
      return 0;

    IN_UPDATE_SZ(has_constset);
    if(has_constset) {
      constantSet = new ir::ConstantSet;
      size_t sz = constantSet->deserializeFromBin(ins);

      if (sz == 0) {
        return 0;
      }

      total_size += sz;
    }

    IN_UPDATE_SZ(ker_num);

    for (size_t i = 0; i < ker_num; i++) {
      size_t ker_serial_sz;
      std::string ker_name; // Just a empty name here.
      Kernel* ker = allocateKernel(ker_name);

      if(!(ker_serial_sz = ker->deserializeFromBin(ins)))
        return 0;

      kernels.insert(std::make_pair(ker->getName(), ker));
      total_size += ker_serial_sz;
    }

    IN_UPDATE_SZ(magic);
    if (magic != magic_end)
      return 0;

    size_t total_bytes;
    IN_UPDATE_SZ(total_bytes);
    if (total_bytes + sizeof(total_size) != total_size)
      return 0;

    return total_size;
  }

  size_t Kernel::serializeToBin(std::ostream& outs) {
    unsigned int i;
    size_t ret_size = 0;
    int has_samplerset = 0;
    int has_imageset = 0;

    OUT_UPDATE_SZ(magic_begin);

    OUT_UPDATE_SZ(name.size());
    outs.write(name.c_str(), name.size());
    ret_size += sizeof(char)*name.size();

    OUT_UPDATE_SZ(argNum);
    for (i = 0; i < argNum; i++) {
      KernelArgument& arg = args[i];
      OUT_UPDATE_SZ(arg.type);
      OUT_UPDATE_SZ(arg.size);
      OUT_UPDATE_SZ(arg.align);
      OUT_UPDATE_SZ(arg.bufSize);
    }

    OUT_UPDATE_SZ(patches.size());
    for (auto patch : patches) {
      unsigned int tmp;
      tmp = patch.type;
      OUT_UPDATE_SZ(tmp);
      tmp = patch.subType;
      OUT_UPDATE_SZ(tmp);
      tmp = patch.offset;
      OUT_UPDATE_SZ(tmp);
    }

    OUT_UPDATE_SZ(curbeSize);
    OUT_UPDATE_SZ(simdWidth);
    OUT_UPDATE_SZ(stackSize);
    OUT_UPDATE_SZ(scratchSize);
    OUT_UPDATE_SZ(useSLM);
    OUT_UPDATE_SZ(slmSize);
    OUT_UPDATE_SZ(compileWgSize[0]);
    OUT_UPDATE_SZ(compileWgSize[1]);
    OUT_UPDATE_SZ(compileWgSize[2]);
    /* samplers. */
    if (samplerSet) {
      has_samplerset = 1;
      OUT_UPDATE_SZ(has_samplerset);
      size_t sz = samplerSet->serializeToBin(outs);
      if (!sz)
        return 0;

      ret_size += sz;
    } else {
      OUT_UPDATE_SZ(has_samplerset);
    }

    /* images. */
    if (imageSet) {
      has_imageset = 1;
      OUT_UPDATE_SZ(has_imageset);
      size_t sz = imageSet->serializeToBin(outs);
      if (!sz)
        return 0;

      ret_size += sz;
    } else {
      OUT_UPDATE_SZ(has_imageset);
    }

    /* Code. */
    const char * code = getCode();
    OUT_UPDATE_SZ(getCodeSize());
    outs.write(code, getCodeSize()*sizeof(char));
    ret_size += getCodeSize()*sizeof(char);

    OUT_UPDATE_SZ(magic_end);

    OUT_UPDATE_SZ(ret_size);
    return ret_size;
  }

  size_t Kernel::deserializeFromBin(std::istream& ins) {
    size_t total_size = 0;
    int has_samplerset = 0;
    int has_imageset = 0;
    size_t code_size = 0;
    uint32_t magic = 0;
    size_t patch_num = 0;

    IN_UPDATE_SZ(magic);
    if (magic != magic_begin)
      return 0;

    size_t name_len;
    IN_UPDATE_SZ(name_len);
    char* c_name = new char[name_len+1];
    ins.read(c_name, name_len*sizeof(char));
    total_size += sizeof(char)*name_len;
    c_name[name_len] = 0;
    name = c_name;
    delete[] c_name;

    IN_UPDATE_SZ(argNum);
    args = GBE_NEW_ARRAY_NO_ARG(KernelArgument, argNum);
    for (uint32_t i = 0; i < argNum; i++) {
      KernelArgument& arg = args[i];
      IN_UPDATE_SZ(arg.type);
      IN_UPDATE_SZ(arg.size);
      IN_UPDATE_SZ(arg.align);
      IN_UPDATE_SZ(arg.bufSize);
    }

    IN_UPDATE_SZ(patch_num);
    for (uint32_t i = 0; i < patch_num; i++) {
      unsigned int tmp;
      PatchInfo patch;
      IN_UPDATE_SZ(tmp);
      patch.type = tmp;
      IN_UPDATE_SZ(tmp);
      patch.subType = tmp;
      IN_UPDATE_SZ(tmp);
      patch.offset = tmp;

      patches.push_back(patch);
    }

    IN_UPDATE_SZ(curbeSize);
    IN_UPDATE_SZ(simdWidth);
    IN_UPDATE_SZ(stackSize);
    IN_UPDATE_SZ(scratchSize);
    IN_UPDATE_SZ(useSLM);
    IN_UPDATE_SZ(slmSize);
    IN_UPDATE_SZ(compileWgSize[0]);
    IN_UPDATE_SZ(compileWgSize[1]);
    IN_UPDATE_SZ(compileWgSize[2]);

    IN_UPDATE_SZ(has_samplerset);
    if (has_samplerset) {
      samplerSet = GBE_NEW(ir::SamplerSet);
      size_t sz = samplerSet->deserializeFromBin(ins);
      if (sz == 0) {
        return 0;
      }

      total_size += sz;
    }

    IN_UPDATE_SZ(has_imageset);
    if (has_imageset) {
      imageSet = GBE_NEW(ir::ImageSet);
      size_t sz = imageSet->deserializeFromBin(ins);
      if (sz == 0) {
        return 0;
      }

      total_size += sz;
    }

    IN_UPDATE_SZ(code_size);
    if (code_size) {
      char* code = GBE_NEW_ARRAY_NO_ARG(char, code_size);
      ins.read(code, code_size*sizeof(char));
      total_size += sizeof(char)*code_size;
      setCode(code, code_size);
    }

    IN_UPDATE_SZ(magic);
    if (magic != magic_end)
      return 0;

    size_t total_bytes;
    IN_UPDATE_SZ(total_bytes);
    if (total_bytes + sizeof(total_size) != total_size)
      return 0;

    return total_size;
  }

#undef OUT_UPDATE_SZ
#undef IN_UPDATE_SZ

  void Program::printStatus(int indent, std::ostream& outs) {
    using namespace std;
    string spaces = indent_to_str(indent);

    outs << spaces << "=============== Begin Program ===============" << "\n";

    if (constantSet) {
      constantSet->printStatus(indent + 4, outs);
    }

    for (auto ker : kernels) {
      ker.second->printStatus(indent + 4, outs);
    }

    outs << spaces << "================ End Program ================" << "\n";
  }

  void Kernel::printStatus(int indent, std::ostream& outs) {
    using namespace std;
    string spaces = indent_to_str(indent);
    string spaces_nl = indent_to_str(indent + 4);
    int num;

    outs << spaces << "+++++++++++ Begin Kernel +++++++++++" << "\n";
    outs << spaces_nl << "Kernel Name: " << name << "\n";
    outs << spaces_nl << "  curbeSize: " << curbeSize << "\n";
    outs << spaces_nl << "  simdWidth: " << simdWidth << "\n";
    outs << spaces_nl << "  stackSize: " << stackSize << "\n";
    outs << spaces_nl << "  scratchSize: " << scratchSize << "\n";
    outs << spaces_nl << "  useSLM: " << useSLM << "\n";
    outs << spaces_nl << "  slmSize: " << slmSize << "\n";
    outs << spaces_nl << "  compileWgSize: " << compileWgSize[0] << compileWgSize[1] << compileWgSize[2] << "\n";

    outs << spaces_nl << "  Argument Number is " << argNum << "\n";
    for (uint32_t i = 0; i < argNum; i++) {
      KernelArgument& arg = args[i];
      outs << spaces_nl << "  Arg " << i << ":\n";
      outs << spaces_nl << "      type value: "<< arg.type << "\n";
      outs << spaces_nl << "      size: "<< arg.size << "\n";
      outs << spaces_nl << "      align: "<< arg.align << "\n";
      outs << spaces_nl << "      bufSize: "<< arg.bufSize << "\n";
    }

    outs << spaces_nl << "  Patches Number is " << patches.size() << "\n";
    num = 0;
    for (auto patch : patches) {
      num++;
      outs << spaces_nl << "  patch " << num << ":\n";
      outs << spaces_nl << "      type value: "<< patch.type << "\n";
      outs << spaces_nl << "      subtype value: "<< patch.subType << "\n";
      outs << spaces_nl << "      offset: "<< patch.offset << "\n";
    }

    if (samplerSet) {
      samplerSet->printStatus(indent + 4, outs);
    }

    if (imageSet) {
      imageSet->printStatus(indent + 4, outs);
    }

    outs << spaces << "++++++++++++ End Kernel ++++++++++++" << "\n";
  }

  /*********************** End of Program class member function *************************/

  static void programDelete(gbe_program gbeProgram) {
    gbe::Program *program = (gbe::Program*)(gbeProgram);
    GBE_SAFE_DELETE(program);
  }

  static void buildModuleFromSource(const char* input, const char* output, std::string options) {
    // Arguments to pass to the clang frontend
    vector<const char *> args;
    bool bOpt = true;
    bool bFastMath = false;

    vector<std::string> useless; //hold substrings to avoid c_str free
    size_t start = 0, end = 0;
    /* clang unsupport options:
       -cl-denorms-are-zero, -cl-strict-aliasing
       -cl-no-signed-zeros, -cl-fp32-correctly-rounded-divide-sqrt
       all support options, refer to clang/include/clang/Driver/Options.inc
       Maybe can filter these options to avoid warning
    */
    while (end != std::string::npos) {
      end = options.find(' ', start);
      std::string str = options.substr(start, end - start);
      start = end + 1;
      if(str.size() == 0)
        continue;
      if(str == "-cl-opt-disable") bOpt = false;
      if(str == "-cl-fast-relaxed-math") bFastMath = true;
      if(str == "-cl-denorms-are-zero")
        continue;
      useless.push_back(str);
      args.push_back(str.c_str());
    }
    args.push_back("-mllvm");
    args.push_back("-inline-threshold=200000");
#ifdef GEN7_SAMPLER_CLAMP_BORDER_WORKAROUND
    args.push_back("-DGEN7_SAMPLER_CLAMP_BORDER_WORKAROUND");
#endif
    args.push_back("-emit-llvm");
    // XXX we haven't implement those builtin functions,
    // so disable it currently.
    args.push_back("-fno-builtin");
    if(bOpt)
      args.push_back("-O2");
    if(bFastMath)
      args.push_back("-D __FAST_RELAXED_MATH__=1");
#if LLVM_VERSION_MINOR <= 2
    args.push_back("-triple");
    args.push_back("nvptx");
#else
    args.push_back("-x");
    args.push_back("cl");
    args.push_back("-triple");
    args.push_back("spir");
#endif /* LLVM_VERSION_MINOR <= 2 */
    args.push_back(input);

    // The compiler invocation needs a DiagnosticsEngine so it can report problems
#if LLVM_VERSION_MINOR <= 1
    args.push_back("-triple");
    args.push_back("ptx32");

    clang::TextDiagnosticPrinter *DiagClient =
                             new clang::TextDiagnosticPrinter(llvm::errs(), clang::DiagnosticOptions());
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagID(new clang::DiagnosticIDs());
    clang::DiagnosticsEngine Diags(DiagID, DiagClient);
#else
    args.push_back("-ffp-contract=off");

    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagOpts = new clang::DiagnosticOptions();
    clang::TextDiagnosticPrinter *DiagClient =
                             new clang::TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagID(new clang::DiagnosticIDs());
    clang::DiagnosticsEngine Diags(DiagID, &*DiagOpts, DiagClient);
#endif /* LLVM_VERSION_MINOR <= 1 */

    // Create the compiler invocation
    llvm::OwningPtr<clang::CompilerInvocation> CI(new clang::CompilerInvocation);
    clang::CompilerInvocation::CreateFromArgs(*CI,
                                              &args[0],
                                              &args[0] + args.size(),
                                              Diags);

    // Create the compiler instance
    clang::CompilerInstance Clang;
    Clang.setInvocation(CI.take());
    // Get ready to report problems
#if LLVM_VERSION_MINOR <= 2
    Clang.createDiagnostics(args.size(), &args[0]);
#else
    Clang.createDiagnostics();
#endif /* LLVM_VERSION_MINOR <= 2 */
    if (!Clang.hasDiagnostics())
      return;

    // Set Language
    clang::LangOptions & lang_opts = Clang.getLangOpts();
    lang_opts.OpenCL = 1;

    //llvm flags need command line parsing to take effect
    if (!Clang.getFrontendOpts().LLVMArgs.empty()) {
      unsigned NumArgs = Clang.getFrontendOpts().LLVMArgs.size();
      const char **Args = new const char*[NumArgs + 2];
      Args[0] = "clang (LLVM option parsing)";
      for (unsigned i = 0; i != NumArgs; ++i){
        Args[i + 1] = Clang.getFrontendOpts().LLVMArgs[i].c_str();
      }
      Args[NumArgs + 1] = 0;
      llvm::cl::ParseCommandLineOptions(NumArgs + 1, Args);
      delete [] Args;
    }

    // Create an action and make the compiler instance carry it out
    llvm::OwningPtr<clang::CodeGenAction> Act(new clang::EmitLLVMOnlyAction());
    auto retVal = Clang.ExecuteAction(*Act);
    if (!retVal)
      return;

    llvm::Module *module = Act->takeModule();

    std::string ErrorInfo;
#if (LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR > 3)
    auto mode = llvm::sys::fs::F_Binary;
#else
    auto mode = llvm::raw_fd_ostream::F_Binary;
#endif
    llvm::raw_fd_ostream OS(output, ErrorInfo, mode);
    //still write to temp file for code simply, otherwise need add another function.
    //because gbe_program_new_from_llvm also be used by cl_program_create_from_llvm, can't be removed
    //TODO: Pass module to llvmToGen, if use module, should return Act and use OwningPtr out of this funciton
    llvm::WriteBitcodeToFile(module, OS);
    OS.close();
  }

  extern std::string ocl_stdlib_str;

  BVAR(OCL_USE_PCH, true);
  static gbe_program programNewFromSource(const char *source,
                                          size_t stringSize,
                                          const char *options,
                                          char *err,
                                          size_t *errSize)
  {
    char clStr[L_tmpnam+1], llStr[L_tmpnam+1];
    const std::string clName = std::string(tmpnam_r(clStr)) + ".cl"; /* unsafe! */
    const std::string llName = std::string(tmpnam_r(llStr)) + ".ll"; /* unsafe! */
    std::string pchHeaderName;
    std::string clOpt;

    FILE *clFile = fopen(clName.c_str(), "w");
    FATAL_IF(clFile == NULL, "Failed to open temporary file");

    bool usePCH = false;

    if(options)
      clOpt += options;

    if (options || !OCL_USE_PCH) {
      /* Some building option may cause the prebuild pch header file
         not compatible with the XXX.cl source. We need rebuild all here.*/
      usePCH = false;
    } else {
      std::string dirs = PCH_OBJECT_DIR;
      std::istringstream idirs(dirs);

      while (getline(idirs, pchHeaderName, ';')) {
        if(access(pchHeaderName.c_str(), R_OK) == 0) {
          usePCH = true;
          break;
        }
      }
    }
    if (usePCH) {
      clOpt += " -include-pch ";
      clOpt += pchHeaderName;
      clOpt += " ";
    } else
      fwrite(ocl_stdlib_str.c_str(), strlen(ocl_stdlib_str.c_str()), 1, clFile);
    // Write the source to the cl file
    fwrite(source, strlen(source), 1, clFile);
    fclose(clFile);

    buildModuleFromSource(clName.c_str(), llName.c_str(), clOpt.c_str());
    remove(clName.c_str());

    // Now build the program from llvm
    gbe_program p = gbe_program_new_from_llvm(llName.c_str(), stringSize, err, errSize);
    remove(llName.c_str());
    return p;
  }

  static size_t programGetGlobalConstantSize(gbe_program gbeProgram) {
    if (gbeProgram == NULL) return 0;
    const gbe::Program *program = (const gbe::Program*) gbeProgram;
    return program->getGlobalConstantSize();
  }

  static void programGetGlobalConstantData(gbe_program gbeProgram, char *mem) {
    if (gbeProgram == NULL) return;
    const gbe::Program *program = (const gbe::Program*) gbeProgram;
    program->getGlobalConstantData(mem);
  }

  static uint32_t programGetKernelNum(gbe_program gbeProgram) {
    if (gbeProgram == NULL) return 0;
    const gbe::Program *program = (const gbe::Program*) gbeProgram;
    return program->getKernelNum();
  }

  static gbe_kernel programGetKernelByName(gbe_program gbeProgram, const char *name) {
    if (gbeProgram == NULL) return NULL;
    const gbe::Program *program = (gbe::Program*) gbeProgram;
    return (gbe_kernel) program->getKernel(std::string(name));
  }

  static gbe_kernel programGetKernel(const gbe_program gbeProgram, uint32_t ID) {
    if (gbeProgram == NULL) return NULL;
    const gbe::Program *program = (gbe::Program*) gbeProgram;
    return (gbe_kernel) program->getKernel(ID);
  }

  static const char *kernelGetName(gbe_kernel genKernel) {
    if (genKernel == NULL) return NULL;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getName();
  }

  static const char *kernelGetCode(gbe_kernel genKernel) {
    if (genKernel == NULL) return NULL;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getCode();
  }

  static size_t kernelGetCodeSize(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0u;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getCodeSize();
  }

  static uint32_t kernelGetArgNum(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0u;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getArgNum();
  }

  static uint32_t kernelGetArgSize(gbe_kernel genKernel, uint32_t argID) {
    if (genKernel == NULL) return 0u;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getArgSize(argID);
  }

  static uint32_t kernelGetArgAlign(gbe_kernel genKernel, uint32_t argID) {
    if (genKernel == NULL) return 0u;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getArgAlign(argID);
  }
  static gbe_arg_type kernelGetArgType(gbe_kernel genKernel, uint32_t argID) {
    if (genKernel == NULL) return GBE_ARG_INVALID;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getArgType(argID);
  }

  static uint32_t kernelGetSIMDWidth(gbe_kernel genKernel) {
    if (genKernel == NULL) return GBE_ARG_INVALID;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getSIMDWidth();
  }

  static int32_t kernelGetCurbeOffset(gbe_kernel genKernel, gbe_curbe_type type, uint32_t subType) {
    if (genKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getCurbeOffset(type, subType);
  }

  static int32_t kernelGetCurbeSize(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getCurbeSize();
  }

  static int32_t kernelGetStackSize(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getStackSize();
  }

  static int32_t kernelGetScratchSize(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getScratchSize();
  }

  static int32_t kernelUseSLM(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getUseSLM() ? 1 : 0;
  }

  static int32_t kernelGetSLMSize(gbe_kernel genKernel) {
    if (genKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) genKernel;
    return kernel->getSLMSize();
  }

  static int32_t kernelSetConstBufSize(gbe_kernel genKernel, uint32_t argID, size_t sz) {
    if (genKernel == NULL) return -1;
    gbe::Kernel *kernel = (gbe::Kernel*) genKernel;
    return kernel->setConstBufSize(argID, sz);
  }

  static size_t kernelGetSamplerSize(gbe_kernel gbeKernel) {
    if (gbeKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) gbeKernel;
    return kernel->getSamplerSize();
  }

  static void kernelGetSamplerData(gbe_kernel gbeKernel, uint32_t *samplers) {
    if (gbeKernel == NULL) return;
    const gbe::Kernel *kernel = (const gbe::Kernel*) gbeKernel;
    kernel->getSamplerData(samplers);
  }

  static void kernelGetCompileWorkGroupSize(gbe_kernel gbeKernel, size_t wg_size[3]) {
    if (gbeKernel == NULL) return;
    const gbe::Kernel *kernel = (const gbe::Kernel*) gbeKernel;
    kernel->getCompileWorkGroupSize(wg_size);
  }

  static size_t kernelGetImageSize(gbe_kernel gbeKernel) {
    if (gbeKernel == NULL) return 0;
    const gbe::Kernel *kernel = (const gbe::Kernel*) gbeKernel;
    return kernel->getImageSize();
  }

  static void kernelGetImageData(gbe_kernel gbeKernel, ImageInfo *images) {
    if (gbeKernel == NULL) return;
    const gbe::Kernel *kernel = (const gbe::Kernel*) gbeKernel;
    kernel->getImageData(images);
  }

  static uint32_t gbeImageBaseIndex = 0;
  static void setImageBaseIndex(uint32_t baseIdx) {
     gbeImageBaseIndex = baseIdx;
  }

  static uint32_t getImageBaseIndex() {
    return gbeImageBaseIndex;
  }

  static uint32_t kernelGetRequiredWorkGroupSize(gbe_kernel kernel, uint32_t dim) {
    return 0u;
  }
} /* namespace gbe */

GBE_EXPORT_SYMBOL gbe_program_new_from_source_cb *gbe_program_new_from_source = NULL;
GBE_EXPORT_SYMBOL gbe_program_new_from_binary_cb *gbe_program_new_from_binary = NULL;
GBE_EXPORT_SYMBOL gbe_program_serialize_to_binary_cb *gbe_program_serialize_to_binary = NULL;
GBE_EXPORT_SYMBOL gbe_program_new_from_llvm_cb *gbe_program_new_from_llvm = NULL;
GBE_EXPORT_SYMBOL gbe_program_get_global_constant_size_cb *gbe_program_get_global_constant_size = NULL;
GBE_EXPORT_SYMBOL gbe_program_get_global_constant_data_cb *gbe_program_get_global_constant_data = NULL;
GBE_EXPORT_SYMBOL gbe_program_delete_cb *gbe_program_delete = NULL;
GBE_EXPORT_SYMBOL gbe_program_get_kernel_num_cb *gbe_program_get_kernel_num = NULL;
GBE_EXPORT_SYMBOL gbe_program_get_kernel_by_name_cb *gbe_program_get_kernel_by_name = NULL;
GBE_EXPORT_SYMBOL gbe_program_get_kernel_cb *gbe_program_get_kernel = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_name_cb *gbe_kernel_get_name = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_code_cb *gbe_kernel_get_code = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_code_size_cb *gbe_kernel_get_code_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_arg_num_cb *gbe_kernel_get_arg_num = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_arg_size_cb *gbe_kernel_get_arg_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_arg_type_cb *gbe_kernel_get_arg_type = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_arg_align_cb *gbe_kernel_get_arg_align = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_simd_width_cb *gbe_kernel_get_simd_width = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_curbe_offset_cb *gbe_kernel_get_curbe_offset = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_curbe_size_cb *gbe_kernel_get_curbe_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_stack_size_cb *gbe_kernel_get_stack_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_scratch_size_cb *gbe_kernel_get_scratch_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_set_const_buffer_size_cb *gbe_kernel_set_const_buffer_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_required_work_group_size_cb *gbe_kernel_get_required_work_group_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_use_slm_cb *gbe_kernel_use_slm = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_slm_size_cb *gbe_kernel_get_slm_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_sampler_size_cb *gbe_kernel_get_sampler_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_sampler_data_cb *gbe_kernel_get_sampler_data = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_compile_wg_size_cb *gbe_kernel_get_compile_wg_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_image_size_cb *gbe_kernel_get_image_size = NULL;
GBE_EXPORT_SYMBOL gbe_kernel_get_image_data_cb *gbe_kernel_get_image_data = NULL;
GBE_EXPORT_SYMBOL gbe_set_image_base_index_cb *gbe_set_image_base_index = NULL;
GBE_EXPORT_SYMBOL gbe_get_image_base_index_cb *gbe_get_image_base_index = NULL;

namespace gbe
{
  /* Use pre-main to setup the call backs */
  struct CallBackInitializer
  {
    CallBackInitializer(void) {
      gbe_program_new_from_source = gbe::programNewFromSource;
      gbe_program_get_global_constant_size = gbe::programGetGlobalConstantSize;
      gbe_program_get_global_constant_data = gbe::programGetGlobalConstantData;
      gbe_program_delete = gbe::programDelete;
      gbe_program_get_kernel_num = gbe::programGetKernelNum;
      gbe_program_get_kernel_by_name = gbe::programGetKernelByName;
      gbe_program_get_kernel = gbe::programGetKernel;
      gbe_kernel_get_name = gbe::kernelGetName;
      gbe_kernel_get_code = gbe::kernelGetCode;
      gbe_kernel_get_code_size = gbe::kernelGetCodeSize;
      gbe_kernel_get_arg_num = gbe::kernelGetArgNum;
      gbe_kernel_get_arg_size = gbe::kernelGetArgSize;
      gbe_kernel_get_arg_type = gbe::kernelGetArgType;
      gbe_kernel_get_arg_align = gbe::kernelGetArgAlign;
      gbe_kernel_get_simd_width = gbe::kernelGetSIMDWidth;
      gbe_kernel_get_curbe_offset = gbe::kernelGetCurbeOffset;
      gbe_kernel_get_curbe_size = gbe::kernelGetCurbeSize;
      gbe_kernel_get_stack_size = gbe::kernelGetStackSize;
      gbe_kernel_get_scratch_size = gbe::kernelGetScratchSize;
      gbe_kernel_set_const_buffer_size = gbe::kernelSetConstBufSize;
      gbe_kernel_get_required_work_group_size = gbe::kernelGetRequiredWorkGroupSize;
      gbe_kernel_use_slm = gbe::kernelUseSLM;
      gbe_kernel_get_slm_size = gbe::kernelGetSLMSize;
      gbe_kernel_get_sampler_size = gbe::kernelGetSamplerSize;
      gbe_kernel_get_sampler_data = gbe::kernelGetSamplerData;
      gbe_kernel_get_compile_wg_size = gbe::kernelGetCompileWorkGroupSize;
      gbe_kernel_get_image_size = gbe::kernelGetImageSize;
      gbe_kernel_get_image_data = gbe::kernelGetImageData;
      gbe_get_image_base_index = gbe::getImageBaseIndex;
      gbe_set_image_base_index = gbe::setImageBaseIndex;
      genSetupCallBacks();
      llvm::llvm_start_multithreaded();
    }

    ~CallBackInitializer() {
      llvm::llvm_stop_multithreaded();
#if (LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR > 3)
      llvm::llvm_shutdown();
#endif
    }
  };

  static CallBackInitializer cbInitializer;
} /* namespace gbe */

