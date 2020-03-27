//===- sycl-post-link.cpp - SYCL post-link device code processing tool ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This source is a collection of utilities run on device code's LLVM IR before
// handing off to back-end for further compilation or emitting SPIRV. The
// utilities are:
// - module splitter to split a big input module into smaller ones
// - specialization constant intrinsic transformation
//===----------------------------------------------------------------------===//

#include "SpecConstants.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PropertySetIO.h"
#include "llvm/Support/SimpleTable.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <memory>

using namespace llvm;

using string_vector = std::vector<std::string>;
using SpecIDMapTy = std::map<StringRef, unsigned>;

cl::OptionCategory PostLinkCat{"sycl-post-link options"};

// Column names in the output file table. Must match across tools -
// clang/lib/Driver/Driver.cpp, sycl-post-link.cpp, ClangOffloadWrapper.cpp
static constexpr char COL_CODE[] = "Code";
static constexpr char COL_SYM[] = "Symbols";
static constexpr char COL_PROPS[] = "Properties";

// InputFilename - The filename to read from.
static cl::opt<std::string> InputFilename{
    cl::Positional, cl::desc("<input bitcode file>"), cl::init("-"),
    cl::value_desc("filename")};

static cl::opt<std::string> OutputDir{
    "out-dir",
    cl::desc(
        "Directory where files listed in the result file table will be output"),
    cl::value_desc("dirname"), cl::cat(PostLinkCat)};

static cl::opt<std::string> OutputFilename{"o", cl::desc("Output filename"),
                                           cl::value_desc("filename"),
                                           cl::init("-"), cl::cat(PostLinkCat)};

static cl::opt<bool> Force{"f", cl::desc("Enable binary output on terminals"),
                           cl::cat(PostLinkCat)};

static cl::opt<bool> IROutputOnly{
    "ir-output-only", cl::desc("Output single IR file"), cl::cat(PostLinkCat)};

static cl::opt<bool> OutputAssembly{"S",
                                    cl::desc("Write output as LLVM assembly"),
                                    cl::Hidden, cl::cat(PostLinkCat)};

enum IRSplitMode {
  SPLIT_PER_TU,    // one module per translation unit
  SPLIT_PER_KERNEL // one module per kernel
};

static cl::opt<IRSplitMode> SplitMode(
    "split", cl::desc("split input module"), cl::Optional,
    cl::init(SPLIT_PER_TU),
    cl::values(clEnumValN(SPLIT_PER_TU, "source",
                          "1 output module per source (translation unit)"),
               clEnumValN(SPLIT_PER_KERNEL, "kernel",
                          "1 output module per kernel")),
    cl::cat(PostLinkCat));

static cl::opt<bool> DoSymGen{"symbols",
                              cl::desc("generate exported symbol files"),
                              cl::cat(PostLinkCat)};

enum SpecConstMode { SC_USE_RT_VAL, SC_USE_DEFAULT_VAL };

static cl::opt<SpecConstMode> SpecConstLower{
    "spec-const",
    cl::desc("lower and generate specialization constants information"),
    cl::Optional,
    cl::init(SC_USE_RT_VAL),
    cl::values(
        clEnumValN(SC_USE_RT_VAL, "rt", "spec constants are set at runtime"),
        clEnumValN(SC_USE_DEFAULT_VAL, "default",
                   "set spec constants to C++ defaults")),
    cl::cat(PostLinkCat)};

static void error(const Twine &Msg) {
  errs() << "sycl-post-link: " << Msg << '\n';
  exit(1);
}

static void checkError(std::error_code EC, const Twine &Prefix) {
  if (EC)
    error(Prefix + ": " + EC.message());
}

static void writeToFile(std::string Filename, std::string Content) {
  std::error_code EC;
  raw_fd_ostream OS{Filename, EC, sys::fs::OpenFlags::OF_None};
  checkError(EC, "error opening the file '" + Filename + "'");
  OS.write(Content.data(), Content.size());
  OS.close();
}

// Output parameter ResKernelModuleMap is a map containing groups of kernels
// with same values of the sycl-module-id attribute.
// The function fills ResKernelModuleMap using input module M.
static void collectKernelModuleMap(
    Module &M, std::map<StringRef, std::vector<Function *>> &ResKernelModuleMap,
    bool OneKernelPerModule) {

  constexpr char ATTR_SYCL_MODULE_ID[] = "sycl-module-id";

  for (auto &F : M.functions()) {
    if (F.getCallingConv() == CallingConv::SPIR_KERNEL) {
      if (OneKernelPerModule) {
        ResKernelModuleMap[F.getName()].push_back(&F);
      } else if (F.hasFnAttribute(ATTR_SYCL_MODULE_ID)) {
        Attribute Id = F.getFnAttribute(ATTR_SYCL_MODULE_ID);
        StringRef Val = Id.getValueAsString();
        ResKernelModuleMap[Val].push_back(&F);
      }
    }
  }
}

// Input parameter KernelModuleMap is a map containing groups of kernels with
// same values of the sycl-module-id attribute. ResSymbolsLists is a vector of
// kernel name lists. Each vector element is a string with kernel names from the
// same module separated by \n.
// The function saves names of kernels from one group to a single std::string
// and stores this string to the ResSymbolsLists vector.
static void collectSymbolsLists(
    std::map<StringRef, std::vector<Function *>> &KernelModuleMap,
    string_vector &ResSymbolsLists) {
  for (auto &It : KernelModuleMap) {
    std::string SymbolsList;
    for (auto &F : It.second) {
      SymbolsList =
          (Twine(SymbolsList) + Twine(F->getName()) + Twine("\n")).str();
    }
    ResSymbolsLists.push_back(std::move(SymbolsList));
  }
}

// Input parameter KernelModuleMap is a map containing groups of kernels with
// same values of the sycl-module-id attribute. For each group of kernels a
// separate IR module will be produced.
// ResModules is a vector of produced modules.
// The function splits input LLVM IR module M into smaller ones and stores them
// to the ResModules vector.
static void
splitModule(Module &M,
            std::map<StringRef, std::vector<Function *>> &KernelModuleMap,
            std::vector<std::unique_ptr<Module>> &ResModules) {
  for (auto &It : KernelModuleMap) {
    // For each group of kernels collect all dependencies.
    SetVector<const GlobalValue *> GVs;
    std::vector<llvm::Function *> Workqueue;

    for (auto &F : It.second) {
      GVs.insert(F);
      Workqueue.push_back(F);
    }

    while (!Workqueue.empty()) {
      Function *F = &*Workqueue.back();
      Workqueue.pop_back();
      for (auto &I : instructions(F)) {
        if (CallBase *CB = dyn_cast<CallBase>(&I))
          if (Function *CF = CB->getCalledFunction())
            if (!CF->isDeclaration() && !GVs.count(CF)) {
              GVs.insert(CF);
              Workqueue.push_back(CF);
            }
      }
    }

    // It's not easy to trace global variable's uses inside needed functions
    // because global variable can be used inside a combination of operators, so
    // mark all global variables as needed and remove dead ones after
    // cloning.
    for (auto &G : M.globals()) {
      GVs.insert(&G);
    }

    ValueToValueMapTy VMap;
    // Clone definitions only for needed globals. Others will be added as
    // declarations and removed later.
    std::unique_ptr<Module> MClone = CloneModule(
        M, VMap, [&](const GlobalValue *GV) { return GVs.count(GV); });

    // TODO: Use the new PassManager instead?
    legacy::PassManager Passes;
    // Do cleanup.
    Passes.add(createGlobalDCEPass());           // Delete unreachable globals.
    Passes.add(createStripDeadDebugInfoPass());  // Remove dead debug info.
    Passes.add(createStripDeadPrototypesPass()); // Remove dead func decls.
    Passes.run(*MClone.get());

    // Save results.
    ResModules.push_back(std::move(MClone));
  }
}

static std::string makeResultFileName(Twine Ext, int I) {
  const StringRef Dir0 = OutputDir.getNumOccurrences() > 0
                             ? OutputDir
                             : sys::path::parent_path(OutputFilename);
  const StringRef Sep = sys::path::get_separator();
  std::string Dir = Dir0.str();
  if (!Dir0.empty() && !Dir0.endswith(Sep))
    Dir += Sep.str();
  return (Dir + Twine(sys::path::stem(OutputFilename)) + "_" +
          std::to_string(I) + Ext)
      .str();
}

static void saveModule(Module &M, StringRef OutFilename) {
  std::error_code EC;
  raw_fd_ostream Out{OutFilename, EC, sys::fs::OF_None};
  checkError(EC, "error opening the file '" + OutFilename + "'");

  // TODO: Use the new PassManager instead?
  legacy::PassManager PrintModule;

  if (OutputAssembly)
    PrintModule.add(createPrintModulePass(Out, ""));
  else if (Force || !CheckBitcodeOutputToConsole(Out, true))
    PrintModule.add(createBitcodeWriterPass(Out));
  PrintModule.run(M);
}

// Saves specified collection of llvm IR modules to files.
// Saves file list if user specified corresponding filename.
static string_vector
saveResultModules(std::vector<std::unique_ptr<Module>> &ResModules) {
  string_vector Res;

  for (size_t I = 0; I < ResModules.size(); ++I) {
    std::error_code EC;
    StringRef FileExt = (OutputAssembly) ? ".ll" : ".bc";
    std::string CurOutFileName = makeResultFileName(FileExt, I);
    saveModule(*ResModules[I].get(), CurOutFileName);
    Res.emplace_back(std::move(CurOutFileName));
  }
  return Res;
}

static string_vector
saveSpecConstantIDMaps(const std::vector<SpecIDMapTy> &Maps) {
  string_vector Res;

  for (size_t I = 0; I < Maps.size(); ++I) {
    std::string SCFile = makeResultFileName(".prop", I);
    llvm::util::PropertySetRegistry PropSet;
    PropSet.add(llvm::util::PropertySetRegistry::SYCL_SPECIALIZATION_CONSTANTS,
                Maps[I]);
    std::error_code EC;
    raw_fd_ostream SCOut(SCFile, EC);
    PropSet.write(SCOut);
    Res.emplace_back(std::move(SCFile));
  }
  return Res;
}

// Saves specified collection of symbols lists to files.
// Saves file list if user specified corresponding filename.
static string_vector saveResultSymbolsLists(string_vector &ResSymbolsLists) {
  string_vector Res;

  std::string TxtFilesList;
  for (size_t I = 0; I < ResSymbolsLists.size(); ++I) {
    std::string CurOutFileName = makeResultFileName(".sym", I);
    writeToFile(CurOutFileName, ResSymbolsLists[I]);
    Res.emplace_back(std::move(CurOutFileName));
  }
  return std::move(Res);
}

#define CHECK_AND_EXIT(E)                                                      \
  {                                                                            \
    Error LocE = std::move(E);                                                 \
    if (LocE) {                                                                \
      logAllUnhandledErrors(std::move(LocE), WithColor::error(errs()));        \
      return 1;                                                                \
    }                                                                          \
  }

int main(int argc, char **argv) {
  InitLLVM X{argc, argv};

  LLVMContext Context;
  cl::HideUnrelatedOptions(PostLinkCat);
  cl::ParseCommandLineOptions(
      argc, argv,
      "SYCL post-link device code processing tool.\n"
      "This is a collection of utilities run on device code's LLVM IR before\n"
      "handing off to back-end for further compilation or emitting SPIRV.\n"
      "The utilities are:\n"
      "- Module splitter to split a big input module into smaller ones.\n"
      "  Groups kernels using function attribute 'sycl-module-id', i.e.\n"
      "  kernels with the same values of the 'sycl-module-id' attribute will\n"
      "  be put into the same module. If -split=kernel option is specified,\n"
      "  one module per kernel will be emitted.\n"
      "- If -symbols options is also specified, then for each produced module\n"
      "  a text file containing names of all spir kernels in it is generated.\n"
      "- Specialization constant intrinsic transformer. Replaces symbolic\n"
      "  ID-based intrinsics to integer ID-based ones to make them friendly\n"
      "  for the SPIRV translator\n"
      "Normally, the tool generates a number of files and \"file table\"\n"
      "file listing all generated files in a table manner. For example, if\n"
      "the input file 'example.bc' contains two kernels, then the command\n"
      "  $ sycl-post-link --split=kernel --symbols --spec-const=rt \\\n"
      "    -o example.table example.bc\n"
      "will produce 'example.table' file with the following content:\n"
      "  [Code|Properties|Symbols]\n"
      "  example_0.bc|example_0.prop|example_0.sym\n"
      "  example_1.bc|example_1.prop|example_1.sym\n"
      "When only specialization constant processing is needed, the tool can\n"
      "output a single transformed IR file if --ir-output-only is specified:\n"
      "  $ sycl-post-link --ir-output-only --spec-const=default \\\n"
      "    -o example_p.bc example.bc\n"
      "will produce single output file example_p.bc suitable for SPIRV\n"
      "translation.\n");

  bool DoSplit = SplitMode.getNumOccurrences() > 0;
  bool DoSpecConst = SpecConstLower.getNumOccurrences() > 0;

  if (!DoSplit && !DoSpecConst && !DoSymGen) {
    errs() << "no actions specified; try --help for usage info\n";
    return 1;
  }
  if (IROutputOnly && DoSplit) {
    errs() << "error: -" << SplitMode.ArgStr << " can't be used with -"
           << IROutputOnly.ArgStr << "\n";
    return 1;
  }
  if (IROutputOnly && DoSymGen) {
    errs() << "error: -" << DoSymGen.ArgStr << " can't be used with -"
           << IROutputOnly.ArgStr << "\n";
    return 1;
  }
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  // It is OK to use raw pointer here as we control that it does not outlive M
  // or objects it is moved to
  Module *MPtr = M.get();

  if (!MPtr) {
    Err.print(argv[0], errs());
    return 1;
  }
  if (OutputFilename.getNumOccurrences() == 0)
    OutputFilename = (Twine(sys::path::stem(InputFilename)) + ".files").str();

  std::map<StringRef, std::vector<Function *>> GlobalsSet;

  if (DoSplit || DoSymGen)
    collectKernelModuleMap(*MPtr, GlobalsSet, SplitMode == SPLIT_PER_KERNEL);

  std::vector<std::unique_ptr<Module>> ResultModules;
  std::vector<SpecIDMapTy> ResultSpecIDMaps;
  string_vector ResultSymbolsLists;

  util::SimpleTable Table;
  bool SpecConstsMet = false;
  bool SetSpecConstAtRT = DoSpecConst && (SpecConstLower == SC_USE_RT_VAL);

  if (DoSpecConst) {
    // perform the spec constant intrinsics transformation and enumeration on
    // the whole module
    ModulePassManager RunSpecConst;
    ModuleAnalysisManager MAM;
    SpecConstantsPass SCP(SetSpecConstAtRT);
    // Register required analysis
    MAM.registerPass([&] { return PassInstrumentationAnalysis(); });
    RunSpecConst.addPass(SCP);
    PreservedAnalyses Res = RunSpecConst.run(*MPtr, MAM);
    SpecConstsMet = !Res.areAllPreserved();
  }
  if (IROutputOnly) {
    // the result is the transformed input LLVMIR file rather than a file table
    saveModule(*MPtr, OutputFilename);
    return 0;
  }
  if (DoSplit) {
    splitModule(*MPtr, GlobalsSet, ResultModules);
    // post-link always produces a code result, even if it is unmodified input
    if (ResultModules.size() == 0)
      ResultModules.push_back(std::move(M));
  } else
    ResultModules.push_back(std::move(M));

  {
    // reuse input module if there were no spec constants and no splitting
    string_vector Files = SpecConstsMet || (ResultModules.size() > 1)
                              ? saveResultModules(ResultModules)
                              : string_vector{InputFilename};
    // "Code" column is always output
    Error Err = Table.addColumn(COL_CODE, Files);
    CHECK_AND_EXIT(Err);
  }
  if (DoSpecConst && SetSpecConstAtRT) {
    // extract spec constant maps per each module
    for (auto &MUptr : ResultModules) {
      ResultSpecIDMaps.emplace_back(SpecIDMapTy());
      if (SpecConstsMet)
        SpecConstantsPass::collectSpecConstantMetadata(*MUptr.get(),
                                                       ResultSpecIDMaps.back());
    }
    string_vector Files = saveSpecConstantIDMaps(ResultSpecIDMaps);
    Error Err = Table.addColumn(COL_PROPS, Files);
    CHECK_AND_EXIT(Err);
  }
  if (DoSymGen) {
    // extract symbols per each module
    collectSymbolsLists(GlobalsSet, ResultSymbolsLists);
    if (ResultSymbolsLists.empty()) {
      // push empty symbols list for consistency
      assert(ResultModules.size() == 1);
      ResultSymbolsLists.push_back("");
    }
    string_vector Files = saveResultSymbolsLists(ResultSymbolsLists);
    Error Err = Table.addColumn(COL_SYM, Files);
    CHECK_AND_EXIT(Err);
  }
  {
    std::error_code EC;
    raw_fd_ostream Out{OutputFilename, EC, sys::fs::OF_None};
    checkError(EC, "error opening file '" + OutputFilename + "'");
    Table.write(Out);
  }
  return 0;
}
