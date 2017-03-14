/*
 Copyright (c) 2013, Trail of Bits
 All rights reserved.

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>
#include <string>
#include <sstream>
#include <system_error>

#include <llvm/Bitcode/ReaderWriter.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ToolOutputFile.h>

#include "mcsema/Arch/Arch.h"

#include "mcsema/BC/Lift.h"
#include "mcsema/BC/Util.h"

static llvm::cl::opt<std::string> OutputFilename(
    "output", llvm::cl::desc("Output filename"), llvm::cl::init("-"),
    llvm::cl::value_desc("filename"));

static llvm::cl::opt<std::string> Arch("arch", llvm::cl::desc("Arch name"),
                                       llvm::cl::value_desc("<arch>"),
                                       llvm::cl::Required);

static llvm::cl::opt<std::string> OS("os", llvm::cl::desc("Operating system"),
                                     llvm::cl::value_desc("<os>"),
                                     llvm::cl::Optional);

static llvm::cl::opt<std::string> InputFilename(
    "cfg", llvm::cl::desc("Input CFG file"), llvm::cl::value_desc("<cfg>"),
    llvm::cl::Optional);

static llvm::cl::list<std::string> EntryPoints(
    "entrypoint", llvm::cl::desc("Describe externally visible entry points"),
    llvm::cl::value_desc("<symbol | ep address>"));

static llvm::cl::opt<bool> ListSupported("list-supported",
                                         llvm::cl::desc("List supported instructions for <arch>"),
                                         llvm::cl::Optional);

static llvm::cl::opt<bool> ListUnsupported("list-unsupported",
                                           llvm::cl::desc("List unsupported (not-yet-implemented) instructions for <arch>"),
                                           llvm::cl::Optional);

static void PrintVersion(void) {
  std::cout << "0.6" << std::endl;
}

static VA FindSymbolInModule(NativeModulePtr mod, const std::string &sym_name) {
  for (auto &sym : mod->getEntryPoints()) {
    if (sym.getName() == sym_name) {
      return sym.getAddr();
    }
  }
  return static_cast<VA>( -1);
}

int main(int argc, char *argv[]) {
  llvm::cl::SetVersionPrinter(PrintVersion);
  llvm::cl::ParseCommandLineOptions(argc, argv, "CFG to LLVM");

  auto context = new llvm::LLVMContext;

  if (OS.empty()) {
    if (ListSupported || ListUnsupported) {
      OS = "linux"; // just need something
    }
    else {
      std::cerr << "-os must be specified" << std::endl;
      return EXIT_FAILURE;
    }
  }

  if (!(ListSupported || ListUnsupported) && EntryPoints.empty()) {
    std::cerr
        << "-entrypoint must be specified" << std::endl;
        return EXIT_FAILURE;
  }

  if (!InitArch(context, OS, Arch)) {
    std::cerr
        << "Cannot initialize for arch " << Arch
        << " and OS " << OS << std::endl;
    return EXIT_FAILURE;
  }

  auto M = CreateModule(context);
  if (!M) {
    return EXIT_FAILURE;
  }

  auto triple = M->getTargetTriple();

  if (ListSupported || ListUnsupported) {
    ListArchSupportedInstructions(triple, llvm::outs(), ListSupported, ListUnsupported);
    return EXIT_SUCCESS;
  }

  if (InputFilename.empty() || OutputFilename.empty()) {
    std::cerr
        << "Must specify an input and output file." << std::endl;
    return EXIT_FAILURE;
  }

  //reproduce NativeModule from CFG input argument
  try {
    auto mod = ReadProtoBuf(InputFilename);
    if (!mod) {
      std::cerr << "Unable to read module from CFG" << std::endl;
      return EXIT_FAILURE;
    }

    //now, convert it to an LLVM module
    ArchInitAttachDetach(M);

    if (!LiftCodeIntoModule(mod, M)) {
      std::cerr << "Failure to convert to LLVM module!" << std::endl;
      return EXIT_FAILURE;
    }

    std::set<VA> entry_point_pcs;
    for (const auto &entry_point_name : EntryPoints) {
      auto entry_pc = FindSymbolInModule(mod, entry_point_name);
      if (entry_pc != static_cast<VA>( -1)) {
        std::cerr << "Adding entry point: " << entry_point_name << std::endl
                  << entry_point_name << " is implemented by sub_" << std::hex
                  << entry_pc << std::endl;

        if ( !ArchAddEntryPointDriver(M, entry_point_name, entry_pc)) {
          return EXIT_FAILURE;
        }

        entry_point_pcs.insert(entry_pc);
      } else {
        std::cerr << "Could not find entry point: " << entry_point_name
                  << "; aborting" << std::endl;
        return EXIT_FAILURE;
      }
    }

    RenameLiftedFunctions(mod, M, entry_point_pcs);

    // will abort if verification fails
    if (llvm::verifyModule( *M, &llvm::errs())) {
      std::cerr << "Could not verify module!" << std::endl;
      return EXIT_FAILURE;
    }

    std::error_code ec;
    llvm::tool_output_file Out(OutputFilename.c_str(), ec,
                               llvm::sys::fs::F_None);
    llvm::WriteBitcodeToFile(M, Out.os());
    Out.keep();

  } catch (std::exception &e) {
    std::cerr << "error: " << std::endl << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}