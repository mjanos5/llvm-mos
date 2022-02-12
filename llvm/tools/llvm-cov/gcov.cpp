//===- gcov.cpp - GCOV compatible LLVM coverage tool ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// llvm-cov is a command line tools to analyze and report coverage information.
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/GCOV.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <system_error>
using namespace llvm;

static SmallString<128> createCoverageFileStem(StringRef &SourceFile,
                                               const StringRef &ObjectDir) {
  SmallString<128> CoverageFileStem(ObjectDir);

  if (CoverageFileStem.empty()) {
    // If no directory was specified with -o, look next to the source file.
    CoverageFileStem = sys::path::parent_path(SourceFile);
    sys::path::append(CoverageFileStem, sys::path::stem(SourceFile));
  } else if (sys::fs::is_directory(ObjectDir))
    // A directory name was given. Use it and the source file name.
    sys::path::append(CoverageFileStem, sys::path::stem(SourceFile));
  else
    // A file was given. Ignore the source file and look next to this file.
    sys::path::replace_extension(CoverageFileStem, "");

  return CoverageFileStem;
}

static std::string createInputFileName(const SmallString<128> &CoverageFileStem,
                                    const std::string &InputGCFileName,
                                    const char *extensionSuffix) {
  return InputGCFileName.empty()
             ? std::string(CoverageFileStem.str()) + ".gc" + extensionSuffix
             : InputGCFileName;
}

static void reportGCOVFileNameError(const std::string &GCOVFileName,
                                    const std::error_code &EC) {
  errs() << GCOVFileName << ": " << EC.message() << "\n";
}

static ErrorOr<std::unique_ptr<MemoryBuffer>>
getGCOVFileOrSTDIN(const std::string &GCOVFileName) {
  // Open .gcda and .gcda without requiring a NUL terminator. The concurrent
  // modification may nullify the NUL terminator condition.

  return MemoryBuffer::getFileOrSTDIN(GCOVFileName, false, false);
}

static bool validateGCNOFile(ErrorOr<std::unique_ptr<MemoryBuffer>> &GCNO_Buff,
                             GCOVFile &GF) {
  GCOVBuffer GCNO_GB(GCNO_Buff.get().get());
  if (!GF.readGCNO(GCNO_GB)) {
    errs() << "Invalid .gcno File!\n";
    return false;
  }

  return true;
}

static void
validateGCDAFile(GCOVFile &GF, const std::string &GCDA,
                       ErrorOr<std::unique_ptr<MemoryBuffer>> &GCDA_Buff) {
  GCOVBuffer gcda_buf(GCDA_Buff.get().get());
  if (!gcda_buf.readGCDAFormat())
    errs() << GCDA << ":not a gcov data file\n";
  else if (!GF.readGCDA(gcda_buf))
    errs() << "Invalid .gcda File!\n";
}

static void reportCoverage(StringRef SourceFile, StringRef ObjectDir,
                           const std::string &InputGCNO,
                           const std::string &InputGCDA, bool DumpGCOV,
                           const GCOV::Options &Options) {
  GCOVFile GF;
  SmallString<128> CoverageFileStem =
      createCoverageFileStem(SourceFile, ObjectDir);

  std::string GCNO = createInputFileName(CoverageFileStem, InputGCNO, "no");
  ErrorOr<std::unique_ptr<MemoryBuffer>> GCNO_Buff = getGCOVFileOrSTDIN(GCNO);
  if (std::error_code EC = GCNO_Buff.getError()) {
    reportGCOVFileNameError(GCNO, EC);
    return;
  }

  if (!validateGCNOFile(GCNO_Buff, GF))
    return;

  std::string GCDA = createInputFileName(CoverageFileStem, InputGCDA, "da");
  ErrorOr<std::unique_ptr<MemoryBuffer>> GCDA_Buff = getGCOVFileOrSTDIN(GCDA);
  if (std::error_code EC = GCDA_Buff.getError()) {
    if (EC != errc::no_such_file_or_directory) {
      reportGCOVFileNameError(GCDA, EC);
      return;
    }

    // Clear the filename to make it clear we didn't read anything.
    GCDA = "-";
  } else {
    validateGCDAFile(GF, GCDA, GCDA_Buff);
  }

  if (DumpGCOV)
    GF.print(errs());

  gcovOneInput(Options, SourceFile, GCNO, GCDA, GF);
}

int gcovMain(int argc, const char *argv[]) {
  cl::list<std::string> SourceFiles(cl::Positional, cl::OneOrMore,
                                    cl::desc("SOURCEFILE"));

  cl::opt<bool> AllBlocks("a", cl::Grouping, cl::init(false),
                          cl::desc("Display all basic blocks"));
  cl::alias AllBlocksA("all-blocks", cl::aliasopt(AllBlocks));

  cl::opt<bool> BranchProb("b", cl::Grouping, cl::init(false),
                           cl::desc("Display branch probabilities"));
  cl::alias BranchProbA("branch-probabilities", cl::aliasopt(BranchProb));

  cl::opt<bool> BranchCount("c", cl::Grouping, cl::init(false),
                            cl::desc("Display branch counts instead "
                                     "of percentages (requires -b)"));
  cl::alias BranchCountA("branch-counts", cl::aliasopt(BranchCount));

  cl::opt<bool> LongNames("l", cl::Grouping, cl::init(false),
                          cl::desc("Prefix filenames with the main file"));
  cl::alias LongNamesA("long-file-names", cl::aliasopt(LongNames));

  cl::opt<bool> FuncSummary("f", cl::Grouping, cl::init(false),
                            cl::desc("Show coverage for each function"));
  cl::alias FuncSummaryA("function-summaries", cl::aliasopt(FuncSummary));

  // Supported by gcov 4.9~8. gcov 9 (GCC r265587) removed --intermediate-format
  // and -i was changed to mean --json-format. We consider this format still
  // useful and support -i.
  cl::opt<bool> Intermediate(
      "intermediate-format", cl::init(false),
      cl::desc("Output .gcov in intermediate text format"));
  cl::alias IntermediateA("i", cl::desc("Alias for --intermediate-format"),
                          cl::Grouping, cl::NotHidden,
                          cl::aliasopt(Intermediate));

  cl::opt<bool> Demangle("demangled-names", cl::init(false),
                         cl::desc("Demangle function names"));
  cl::alias DemangleA("m", cl::desc("Alias for --demangled-names"),
                      cl::Grouping, cl::NotHidden, cl::aliasopt(Demangle));

  cl::opt<bool> NoOutput("n", cl::Grouping, cl::init(false),
                         cl::desc("Do not output any .gcov files"));
  cl::alias NoOutputA("no-output", cl::aliasopt(NoOutput));

  cl::opt<std::string> ObjectDir(
      "o", cl::value_desc("DIR|FILE"), cl::init(""),
      cl::desc("Find objects in DIR or based on FILE's path"));
  cl::alias ObjectDirA("object-directory", cl::aliasopt(ObjectDir));
  cl::alias ObjectDirB("object-file", cl::aliasopt(ObjectDir));

  cl::opt<bool> PreservePaths("p", cl::Grouping, cl::init(false),
                              cl::desc("Preserve path components"));
  cl::alias PreservePathsA("preserve-paths", cl::aliasopt(PreservePaths));

  cl::opt<bool> RelativeOnly(
      "r", cl::Grouping,
      cl::desc("Only dump files with relative paths or absolute paths with the "
               "prefix specified by -s"));
  cl::alias RelativeOnlyA("relative-only", cl::aliasopt(RelativeOnly));
  cl::opt<std::string> SourcePrefix("s", cl::desc("Source prefix to elide"));
  cl::alias SourcePrefixA("source-prefix", cl::aliasopt(SourcePrefix));

  cl::opt<bool> UseStdout("t", cl::Grouping, cl::init(false),
                          cl::desc("Print to stdout"));
  cl::alias UseStdoutA("stdout", cl::aliasopt(UseStdout));

  cl::opt<bool> UncondBranch("u", cl::Grouping, cl::init(false),
                             cl::desc("Display unconditional branch info "
                                      "(requires -b)"));
  cl::alias UncondBranchA("unconditional-branches", cl::aliasopt(UncondBranch));

  cl::opt<bool> HashFilenames("x", cl::Grouping, cl::init(false),
                              cl::desc("Hash long pathnames"));
  cl::alias HashFilenamesA("hash-filenames", cl::aliasopt(HashFilenames));


  cl::OptionCategory DebugCat("Internal and debugging options");
  cl::opt<bool> DumpGCOV("dump", cl::init(false), cl::cat(DebugCat),
                         cl::desc("Dump the gcov file to stderr"));
  cl::opt<std::string> InputGCNO("gcno", cl::cat(DebugCat), cl::init(""),
                                 cl::desc("Override inferred gcno file"));
  cl::opt<std::string> InputGCDA("gcda", cl::cat(DebugCat), cl::init(""),
                                 cl::desc("Override inferred gcda file"));

  cl::ParseCommandLineOptions(argc, argv, "LLVM code coverage tool\n");

  GCOV::Options Options(AllBlocks, BranchProb, BranchCount, FuncSummary,
                        PreservePaths, UncondBranch, Intermediate, LongNames,
                        Demangle, NoOutput, RelativeOnly, UseStdout,
                        HashFilenames, SourcePrefix);

  for (const auto &SourceFile : SourceFiles)
    reportCoverage(SourceFile, ObjectDir, InputGCNO, InputGCDA, DumpGCOV,
                   Options);
  return 0;
}
