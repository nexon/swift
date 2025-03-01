//===--- ScanDependencies.cpp -- Scans the dependencies of a module -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/PrettyStackTrace.h"

#include "swift/DependencyScan/ScanDependencies.h"
#include "swift/DependencyScan/SerializedModuleDependencyCacheFormat.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/DependencyScan/DependencyScanImpl.h"
#include "swift/DependencyScan/StringUtils.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Strings.h"
#include "clang/Basic/Module.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include <set>
#include <string>
#include <sstream>

using namespace swift;
using namespace swift::dependencies;
using namespace swift::c_string_utils;
using namespace llvm::yaml;

namespace {

static std::string getScalaNodeText(Node *N) {
  SmallString<32> Buffer;
  return cast<ScalarNode>(N)->getValue(Buffer).str();
}

/// Parse an entry like this, where the "platforms" key-value pair is optional:
///  {
///     "swiftModuleName": "Foo",
///     "arguments": "-target 10.15",
///     "output": "../Foo.json"
///  },
static bool parseBatchInputEntries(ASTContext &Ctx, llvm::StringSaver &saver,
                                   Node *Node,
                                   std::vector<BatchScanInput> &result) {
  auto *SN = cast<SequenceNode>(Node);
  if (!SN)
    return true;
  for (auto It = SN->begin(); It != SN->end(); ++It) {
    auto *MN = cast<MappingNode>(&*It);
    BatchScanInput entry;
    Optional<std::set<int8_t>> Platforms;
    for (auto &Pair : *MN) {
      auto Key = getScalaNodeText(Pair.getKey());
      auto *Value = Pair.getValue();
      if (Key == "clangModuleName") {
        entry.moduleName = saver.save(getScalaNodeText(Value));
        entry.isSwift = false;
      } else if (Key == "swiftModuleName") {
        entry.moduleName = saver.save(getScalaNodeText(Value));
        entry.isSwift = true;
      } else if (Key == "arguments") {
        entry.arguments = saver.save(getScalaNodeText(Value));
      } else if (Key == "output") {
        entry.outputPath = saver.save(getScalaNodeText(Value));
      } else {
        // Future proof.
        continue;
      }
    }
    if (entry.moduleName.empty())
      return true;
    if (entry.outputPath.empty())
      return true;
    result.emplace_back(std::move(entry));
  }
  return false;
}

static Optional<std::vector<BatchScanInput>>
parseBatchScanInputFile(ASTContext &ctx, StringRef batchInputPath,
                        llvm::StringSaver &saver) {
  assert(!batchInputPath.empty());
  namespace yaml = llvm::yaml;
  std::vector<BatchScanInput> result;

  // Load the input file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      llvm::MemoryBuffer::getFile(batchInputPath);
  if (!FileBufOrErr) {
    ctx.Diags.diagnose(SourceLoc(), diag::batch_scan_input_file_missing,
                       batchInputPath);
    return None;
  }
  StringRef Buffer = FileBufOrErr->get()->getBuffer();

  // Use a new source manager instead of the one from ASTContext because we
  // don't want the Json file to be persistent.
  SourceManager SM;
  yaml::Stream Stream(llvm::MemoryBufferRef(Buffer, batchInputPath),
                      SM.getLLVMSourceMgr());
  for (auto DI = Stream.begin(); DI != Stream.end(); ++DI) {
    assert(DI != Stream.end() && "Failed to read a document");
    yaml::Node *N = DI->getRoot();
    assert(N && "Failed to find a root");
    if (parseBatchInputEntries(ctx, saver, N, result)) {
      ctx.Diags.diagnose(SourceLoc(), diag::batch_scan_input_file_corrupted,
                         batchInputPath);
      return None;
    }
  }
  return result;
}

/// Find all of the imported Clang modules starting with the given module name.
static void findAllImportedClangModules(ASTContext &ctx, StringRef moduleName,
                                        ModuleDependenciesCache &cache,
                                        std::vector<std::string> &allModules,
                                        llvm::StringSet<> &knownModules) {
  if (!knownModules.insert(moduleName).second)
    return;
  allModules.push_back(moduleName.str());
  auto optionalDependencies =
    cache.findDependency(moduleName, ModuleDependencyKind::Clang);
  if (!optionalDependencies.has_value())
    return;

  auto dependencies = optionalDependencies.value();
  for (const auto &dep : dependencies->getModuleDependencies()) {
    findAllImportedClangModules(ctx, dep.first, cache, allModules, knownModules);
  }
}

/// Resolve the direct dependencies of the given module.
static ArrayRef<ModuleDependencyID>
resolveDirectDependencies(CompilerInstance &instance, ModuleDependencyID module,
                          ModuleDependenciesCache &cache,
                          InterfaceSubContextDelegate &ASTDelegate) {
  PrettyStackTraceStringAction trace("Resolving direct dependencies of: ", module.first);
  auto &ctx = instance.getASTContext();
  auto optionalKnownDependencies = cache.findDependency(module.first, module.second);
  assert(optionalKnownDependencies.has_value());
  auto knownDependencies = optionalKnownDependencies.value();

  // If this dependency has already been resolved, return the result.
  if (knownDependencies->isResolved() &&
      knownDependencies->getKind() != ModuleDependencyKind::SwiftSource)
    return knownDependencies->getModuleDependencies();

  auto isSwiftInterfaceOrSource = knownDependencies->isSwiftInterfaceModule() ||
                                  knownDependencies->isSwiftSourceModule();
  auto isSwift =
      isSwiftInterfaceOrSource || knownDependencies->isSwiftBinaryModule();
  // Find the dependencies of every module this module directly depends on.
  ModuleDependencyIDSetVector result;
  for (auto dependsOn : knownDependencies->getModuleImports()) {
    // Figure out what kind of module we need.
    bool onlyClangModule = !isSwift || module.first == dependsOn;
    if (onlyClangModule) {
      if (auto found =
              ctx.getClangModuleDependencies(dependsOn, cache, ASTDelegate))
        result.insert({dependsOn, ModuleDependencyKind::Clang});
    } else {
      if (auto found =
              ctx.getModuleDependencies(dependsOn, cache, ASTDelegate, module))
        result.insert({dependsOn, found.value()->getKind()});
    }
  }

  if (isSwiftInterfaceOrSource) {
    // A record of all of the Clang modules referenced from this Swift module.
    std::vector<std::string> allClangModules;
    llvm::StringSet<> knownModules;

    // If the Swift module has a bridging header, add those dependencies.
    if (knownDependencies->getBridgingHeader()) {
      auto clangImporter =
          static_cast<ClangImporter *>(ctx.getClangModuleLoader());
      if (!clangImporter->addBridgingHeaderDependencies(module.first,
                                                        module.second, cache)) {
        // Grab the updated module dependencies.
        // FIXME: This is such a hack.
        knownDependencies = *cache.findDependency(module.first, module.second);

        // Add the Clang modules referenced from the bridging header to the
        // set of Clang modules we know about.
        const std::vector<std::string> *bridgingModuleDependencies = nullptr;
        if (auto swiftDeps = knownDependencies->getAsSwiftInterfaceModule())
          bridgingModuleDependencies =
              &(swiftDeps->textualModuleDetails.bridgingModuleDependencies);
        else if (auto sourceDeps = knownDependencies->getAsSwiftSourceModule())
          bridgingModuleDependencies =
              &(sourceDeps->textualModuleDetails.bridgingModuleDependencies);

        assert(bridgingModuleDependencies);
        for (const auto &clangDep : *bridgingModuleDependencies) {
          findAllImportedClangModules(ctx, clangDep, cache, allClangModules,
                                      knownModules);
        }
      }
    }

    // Find all of the Clang modules this Swift module depends on.
    for (const auto &dep : result) {
      if (dep.second != ModuleDependencyKind::Clang)
        continue;

      findAllImportedClangModules(ctx, dep.first, cache, allClangModules,
                                  knownModules);
    }

    // Look for overlays for each of the Clang modules. The Swift module
    // directly depends on these.
    for (const auto &clangDep : allClangModules) {
      if (auto found =
              ctx.getSwiftModuleDependencies(clangDep, cache, ASTDelegate)) {
        if (clangDep != module.first)
          result.insert({clangDep, found.value()->getKind()});
      }
    }
  }

  // Resolve the dependnecy info
  cache.resolveDependencyImports(module, result.takeVector());
  return cache.findDependency(module.first, module.second).value()->getModuleDependencies();
}

static void discoverCrossImportOverlayDependencies(
    CompilerInstance &instance, StringRef mainModuleName,
    ArrayRef<ModuleDependencyID> allDependencies,
    ModuleDependenciesCache &cache, InterfaceSubContextDelegate &ASTDelegate,
    llvm::function_ref<void(ModuleDependencyID)> action) {
  // Modules explicitly imported. Only these can be secondary module.
  llvm::SetVector<Identifier> newOverlays;
  for (auto dep : allDependencies) {
    auto moduleName = dep.first;
    auto dependencies = cache.findDependency(moduleName, dep.second).value();

    // Collect a map from secondary module name to cross-import overlay names.
    auto overlayMap = dependencies->collectCrossImportOverlayNames(
        instance.getASTContext(), moduleName);
    if (overlayMap.empty())
      continue;
    std::for_each(allDependencies.begin(), allDependencies.end(),
                  [&](ModuleDependencyID Id) {
                    // check if any explicitly imported modules can serve as a
                    // secondary module, and add the overlay names to the
                    // dependencies list.
                    for (auto overlayName : overlayMap[Id.first]) {
                      if (std::find_if(allDependencies.begin(),
                                       allDependencies.end(),
                                       [&](ModuleDependencyID Id) {
                                         return Id.first == overlayName.str();
                                       }) == allDependencies.end()) {
                        newOverlays.insert(overlayName);
                      }
                    }
                  });
  }
  // No new cross-import overlays are found, return.
  if (newOverlays.empty())
    return;
  // Construct a dummy main to resolve the newly discovered cross import
  // overlays.
  StringRef dummyMainName = "DummyMainModuleForResolvingCrossImportOverlays";
  auto dummyMainDependencies = ModuleDependencyInfo::forSwiftSourceModule({});
  std::for_each(newOverlays.begin(), newOverlays.end(),
                [&](Identifier modName) {
                  dummyMainDependencies.addModuleImport(modName.str());
                });

  // Record the dummy main module's direct dependencies. The dummy main module
  // only directly depend on these newly discovered overlay modules.
  if (cache.findDependency(
                 dummyMainName, ModuleDependencyKind::SwiftSource)) {
    cache.updateDependency(
        std::make_pair(dummyMainName.str(),
                       ModuleDependencyKind::SwiftSource),
        dummyMainDependencies);
  } else {
    cache.recordDependency(dummyMainName, dummyMainDependencies);
  }

  ModuleDependencyIDSetVector allModules;

  // Seed the all module list from the dummy main module.
  allModules.insert({dummyMainName.str(), dummyMainDependencies.getKind()});

  // Explore the dependencies of every module.
  for (unsigned currentModuleIdx = 0; currentModuleIdx < allModules.size();
       ++currentModuleIdx) {
    auto module = allModules[currentModuleIdx];
    auto moduleDependnencyIDs =
        resolveDirectDependencies(instance, module, cache, ASTDelegate);
    allModules.insert(moduleDependnencyIDs.begin(), moduleDependnencyIDs.end());
  }

  // Update main module's dependencies to include these new overlays.
  auto mainDep = *(cache.findDependency(
                   mainModuleName, ModuleDependencyKind::SwiftSource).value());
  std::for_each(/* +1 to exclude dummy main*/ allModules.begin() + 1,
                allModules.end(),
                [&](ModuleDependencyID dependencyID) {
                  mainDep.addModuleDependency(dependencyID);
                });
  cache.updateDependency(
      {mainModuleName.str(), ModuleDependencyKind::SwiftSource}, mainDep);

  // Report any discovered modules to the clients, which include all overlays
  // and their dependencies.
  std::for_each(/* +1 to exclude dummy main*/ allModules.begin() + 1,
                allModules.end(), action);
}

namespace {
std::string quote(StringRef unquoted) {
  llvm::SmallString<128> buffer;
  llvm::raw_svector_ostream os(buffer);
  for (const auto ch : unquoted) {
    if (ch == '\\')
      os << '\\';
    os << ch;
  }
  return buffer.str().str();
}
}

/// Write a single JSON field.
template <typename T>
void writeJSONSingleField(llvm::raw_ostream &out, StringRef fieldName,
                          const T &value, unsigned indentLevel,
                          bool trailingComma, bool nested = false);

/// Write a string value as JSON.
void writeJSONValue(llvm::raw_ostream &out, StringRef value,
                    unsigned indentLevel) {
  out << "\"";
  out << quote(value);
  out << "\"";
}

void writeJSONValue(llvm::raw_ostream &out, swiftscan_string_ref_t value,
                    unsigned indentLevel) {
  out << "\"";
  out << quote(get_C_string(value));
  out << "\"";
}

void writeJSONValue(llvm::raw_ostream &out, swiftscan_string_set_t *value_set,
                    unsigned indentLevel) {
  out << "[\n";

  for (size_t i = 0; i < value_set->count; ++i) {
    out.indent((indentLevel + 1) * 2);

    writeJSONValue(out, value_set->strings[i], indentLevel + 1);

    if (i != value_set->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";
}

void writeEncodedModuleIdJSONValue(llvm::raw_ostream &out,
                                   swiftscan_string_ref_t value,
                                   unsigned indentLevel) {
  out << "{\n";
  static const std::string textualPrefix("swiftTextual");
  static const std::string binaryPrefix("swiftBinary");
  static const std::string placeholderPrefix("swiftPlaceholder");
  static const std::string clangPrefix("clang");
  std::string valueStr = get_C_string(value);
  std::string moduleKind;
  std::string moduleName;
  if (!valueStr.compare(0, textualPrefix.size(), textualPrefix)) {
    moduleKind = "swift";
    moduleName = valueStr.substr(textualPrefix.size() + 1);
  } else if (!valueStr.compare(0, binaryPrefix.size(), binaryPrefix)) {
    // FIXME: rename to be consistent in the clients (swift-driver)
    moduleKind = "swiftPrebuiltExternal";
    moduleName = valueStr.substr(binaryPrefix.size() + 1);
  } else if (!valueStr.compare(0, placeholderPrefix.size(),
                               placeholderPrefix)) {
    moduleKind = "swiftPlaceholder";
    moduleName = valueStr.substr(placeholderPrefix.size() + 1);
  } else {
    moduleKind = "clang";
    moduleName = valueStr.substr(clangPrefix.size() + 1);
  }
  writeJSONSingleField(out, moduleKind, moduleName, indentLevel + 1,
                       /*trailingComma=*/false);
  out.indent(indentLevel * 2);
  out << "}";
}

/// Write a boolean value as JSON.
void writeJSONValue(llvm::raw_ostream &out, bool value, unsigned indentLevel) {
  out.write_escaped(value ? "true" : "false");
}

/// Write a JSON array.
template <typename T>
void writeJSONValue(llvm::raw_ostream &out, ArrayRef<T> values,
                    unsigned indentLevel) {
  out << "[\n";

  for (const auto &value : values) {

    out.indent((indentLevel + 1) * 2);

    writeJSONValue(out, value, indentLevel + 1);

    if (&value != &values.back()) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";
}

/// Write a JSON array.
template <typename T>
void writeJSONValue(llvm::raw_ostream &out, const std::vector<T> &values,
                    unsigned indentLevel) {
  writeJSONValue(out, llvm::makeArrayRef(values), indentLevel);
}

/// Write a single JSON field.
template <typename T>
void writeJSONSingleField(llvm::raw_ostream &out, StringRef fieldName,
                          const T &value, unsigned indentLevel,
                          bool trailingComma, bool nested) {
  out.indent(indentLevel * 2);
  writeJSONValue(out, fieldName, indentLevel);
  out << ": ";
  auto updatedIndentLevel = indentLevel;
  
  if (nested) {
    // This is a hack to "fix" a format for a value that should be a nested
    // set of strings. Currently only capturedPCMArgs (clang) is expected to
    // in the nested format, which supposedly only contains one set of strings.
    // Adjust the indentation to account for the nested brackets.
    updatedIndentLevel += 1;
    out << "[\n";
    out.indent(updatedIndentLevel * 2);
  }

  writeJSONValue(out, value, updatedIndentLevel);

  if (nested) {
    // If nested, add an extra closing brack with a correct indentation.
    out << "\n";
    out.indent(indentLevel * 2);
    out << "]";
  }

  if (trailingComma)
    out << ",";
  out << "\n";
}

void writeDirectDependencies(llvm::raw_ostream &out,
                             const swiftscan_string_set_t *dependencies,
                             unsigned indentLevel, bool trailingComma) {
  out.indent(indentLevel * 2);
  out << "\"directDependencies\": ";
  out << "[\n";

  for (size_t i = 0; i < dependencies->count; ++i) {
    out.indent((indentLevel + 1) * 2);
    writeEncodedModuleIdJSONValue(out, dependencies->strings[i],
                                  indentLevel + 1);
    if (i != dependencies->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";

  if (trailingComma)
    out << ",";
  out << "\n";
}

static const swiftscan_swift_textual_details_t *
getAsTextualDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL)
    return &details->swift_textual_details;
  return nullptr;
}

static const swiftscan_swift_placeholder_details_t *
getAsPlaceholderDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_PLACEHOLDER)
    return &details->swift_placeholder_details;
  return nullptr;
}

static const swiftscan_swift_binary_details_t *
getAsBinaryDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_BINARY)
    return &details->swift_binary_details;
  return nullptr;
}

static const swiftscan_clang_details_t *
getAsClangDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_CLANG)
    return &details->clang_details;
  return nullptr;
}

static void writePrescanJSON(llvm::raw_ostream &out,
                             const swiftscan_import_set_t importSet) {
  // Write out a JSON containing all main module imports.
  out << "{\n";
  SWIFT_DEFER { out << "}\n"; };

  writeJSONSingleField(out, "imports", importSet->imports, 0, false);
}

static void writeJSON(llvm::raw_ostream &out,
                      const swiftscan_dependency_graph_t fullDependencies) {
  // Write out a JSON description of all of the dependencies.
  out << "{\n";
  SWIFT_DEFER { out << "}\n"; };
  // Name of the main module.
  writeJSONSingleField(out, "mainModuleName",
                       fullDependencies->main_module_name,
                       /*indentLevel=*/1, /*trailingComma=*/true);
  // Write out all of the modules.
  out << "  \"modules\": [\n";
  SWIFT_DEFER { out << "  ]\n"; };
  const auto module_set = fullDependencies->dependencies;
  for (size_t mi = 0; mi < module_set->count; ++mi) {
    const auto &moduleInfo = *module_set->modules[mi];
    auto &directDependencies = moduleInfo.direct_dependencies;
    // The module we are describing.
    out.indent(2 * 2);
    writeEncodedModuleIdJSONValue(out, moduleInfo.module_name, 2);
    out << ",\n";
    out.indent(2 * 2);
    out << "{\n";
    auto swiftPlaceholderDeps =
        getAsPlaceholderDependencyModule(moduleInfo.details);
    auto swiftTextualDeps = getAsTextualDependencyModule(moduleInfo.details);
    auto swiftBinaryDeps = getAsBinaryDependencyModule(moduleInfo.details);
    auto clangDeps = getAsClangDependencyModule(moduleInfo.details);

    // Module path.
    const char *modulePathSuffix = clangDeps ? ".pcm" : ".swiftmodule";

    std::string modulePath;
    std::string moduleKindAndName =
        std::string(get_C_string(moduleInfo.module_name));
    std::string moduleName =
        moduleKindAndName.substr(moduleKindAndName.find(":") + 1);
    if (swiftPlaceholderDeps)
      modulePath = get_C_string(swiftPlaceholderDeps->compiled_module_path);
    else if (swiftBinaryDeps)
      modulePath = get_C_string(swiftBinaryDeps->compiled_module_path);
    else if (clangDeps || swiftTextualDeps)
      modulePath = get_C_string(moduleInfo.module_path);
    else
      modulePath = moduleName + modulePathSuffix;

    writeJSONSingleField(out, "modulePath", modulePath, /*indentLevel=*/3,
                         /*trailingComma=*/true);

    // Source files.
    if (swiftTextualDeps || clangDeps) {
      writeJSONSingleField(out, "sourceFiles", moduleInfo.source_files, 3,
                           /*trailingComma=*/true);
    }

    // Direct dependencies.
    if (swiftTextualDeps || swiftBinaryDeps || clangDeps)
      writeDirectDependencies(out, directDependencies, 3,
                              /*trailingComma=*/true);
    // Swift and Clang-specific details.
    out.indent(3 * 2);
    out << "\"details\": {\n";
    out.indent(4 * 2);
    if (swiftTextualDeps) {
      out << "\"swift\": {\n";
      /// Swift interface file, if there is one. The main module, for
      /// example, will not have an interface file.
      std::string moduleInterfacePath =
          swiftTextualDeps->module_interface_path.data
              ? get_C_string(swiftTextualDeps->module_interface_path)
              : "";
      if (!moduleInterfacePath.empty()) {
        writeJSONSingleField(out, "moduleInterfacePath", moduleInterfacePath, 5,
                             /*trailingComma=*/true);
        writeJSONSingleField(out, "contextHash", swiftTextualDeps->context_hash,
                             5,
                             /*trailingComma=*/true);
        out.indent(5 * 2);
        out << "\"commandLine\": [\n";
        for (int i = 0, count = swiftTextualDeps->command_line->count;
             i < count; ++i) {
          const auto &arg =
              get_C_string(swiftTextualDeps->command_line->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(arg) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << "],\n";
        out.indent(5 * 2);
        out << "\"compiledModuleCandidates\": [\n";
        for (int i = 0,
                 count = swiftTextualDeps->compiled_module_candidates->count;
             i < count; ++i) {
          const auto &candidate = get_C_string(
              swiftTextualDeps->compiled_module_candidates->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(candidate) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << "],\n";
      }
      bool hasBridgingHeaderPath =
          swiftTextualDeps->bridging_header_path.data &&
          get_C_string(swiftTextualDeps->bridging_header_path)[0] != '\0';
      bool commaAfterFramework =
          swiftTextualDeps->extra_pcm_args->count != 0 || hasBridgingHeaderPath;

      writeJSONSingleField(out, "isFramework", swiftTextualDeps->is_framework,
                           5, commaAfterFramework);
      if (swiftTextualDeps->extra_pcm_args->count != 0) {
        out.indent(5 * 2);
        out << "\"extraPcmArgs\": [\n";
        for (int i = 0, count = swiftTextualDeps->extra_pcm_args->count;
             i < count; ++i) {
          const auto &arg =
              get_C_string(swiftTextualDeps->extra_pcm_args->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(arg) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << (hasBridgingHeaderPath ? "],\n" : "]\n");
      }
      /// Bridging header and its source file dependencies, if any.
      if (hasBridgingHeaderPath) {
        out.indent(5 * 2);
        out << "\"bridgingHeader\": {\n";
        writeJSONSingleField(out, "path",
                             swiftTextualDeps->bridging_header_path, 6,
                             /*trailingComma=*/true);
        writeJSONSingleField(out, "sourceFiles",
                             swiftTextualDeps->bridging_source_files, 6,
                             /*trailingComma=*/true);
        writeJSONSingleField(out, "moduleDependencies",
                             swiftTextualDeps->bridging_module_dependencies, 6,
                             /*trailingComma=*/false);
        out.indent(5 * 2);
        out << "}\n";
      }
    } else if (swiftPlaceholderDeps) {
      out << "\"swiftPlaceholder\": {\n";

      // Module doc file
      if (swiftPlaceholderDeps->module_doc_path.data &&
          get_C_string(swiftPlaceholderDeps->module_doc_path)[0] != '\0')
        writeJSONSingleField(out, "moduleDocPath",
                             swiftPlaceholderDeps->module_doc_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);

      // Module Source Info file
      if (swiftPlaceholderDeps->module_source_info_path.data &&
          get_C_string(swiftPlaceholderDeps->module_source_info_path)[0] !=
              '\0')
        writeJSONSingleField(out, "moduleSourceInfoPath",
                             swiftPlaceholderDeps->module_source_info_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/false);
    } else if (swiftBinaryDeps) {
      out << "\"swiftPrebuiltExternal\": {\n";
      bool hasCompiledModulePath =
          swiftBinaryDeps->compiled_module_path.data &&
          get_C_string(swiftBinaryDeps->compiled_module_path)[0] != '\0';
      assert(hasCompiledModulePath &&
             "Expected .swiftmodule for a Binary Swift Module Dependency.");

      writeJSONSingleField(out, "compiledModulePath",
                           swiftBinaryDeps->compiled_module_path,
                           /*indentLevel=*/5,
                           /*trailingComma=*/true);
      // Module doc file
      if (swiftBinaryDeps->module_doc_path.data &&
          get_C_string(swiftBinaryDeps->module_doc_path)[0] != '\0')
        writeJSONSingleField(out, "moduleDocPath",
                             swiftBinaryDeps->module_doc_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);
      // Module Source Info file
      if (swiftBinaryDeps->module_source_info_path.data &&
          get_C_string(swiftBinaryDeps->module_source_info_path)[0] != '\0')
        writeJSONSingleField(out, "moduleSourceInfoPath",
                             swiftBinaryDeps->module_source_info_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);
      writeJSONSingleField(out, "isFramework", swiftBinaryDeps->is_framework,
                           5, /*trailingComma=*/false);
    } else {
      out << "\"clang\": {\n";

      // Module map file.
      writeJSONSingleField(out, "moduleMapPath", clangDeps->module_map_path, 5,
                           /*trailingComma=*/true);

      // Context hash.
      writeJSONSingleField(out, "contextHash", clangDeps->context_hash, 5,
                           /*trailingComma=*/true);

      // Command line.
      writeJSONSingleField(out, "commandLine", clangDeps->command_line, 5,
                           /*trailingComma=*/true);

      // Captured PCM arguments.
      writeJSONSingleField(out, "capturedPCMArgs", clangDeps->captured_pcm_args, 5,
                           /*trailingComma=*/false, /*nested=*/true);
    }

    out.indent(4 * 2);
    out << "}\n";
    out.indent(3 * 2);
    out << "}\n";
    out.indent(2 * 2);
    out << "}";

    if (mi != module_set->count - 1)
      out << ",";
    out << "\n";
  }
}

static swiftscan_dependency_graph_t
generateFullDependencyGraph(CompilerInstance &instance,
                            ModuleDependenciesCache &cache,
                            InterfaceSubContextDelegate &ASTDelegate,
                            ArrayRef<ModuleDependencyID> allModules) {
  if (allModules.empty()) {
    return nullptr;
  }

  std::string mainModuleName = allModules.front().first;
  swiftscan_dependency_set_t *dependencySet = new swiftscan_dependency_set_t;
  dependencySet->count = allModules.size();
  dependencySet->modules =
      new swiftscan_dependency_info_t[dependencySet->count];

  for (size_t i = 0; i < allModules.size(); ++i) {
    const auto &module = allModules[i];
    // Grab the completed module dependencies.
    auto moduleDepsQuery = cache.findDependency(module.first, module.second);
    if (!moduleDepsQuery) {
      llvm::report_fatal_error(Twine("Module Dependency Cache missing module") +
                               module.first);
    }

    auto moduleDeps = *moduleDepsQuery;
    // Collect all the required pieces to build a ModuleInfo
    auto swiftPlaceholderDeps = moduleDeps->getAsPlaceholderDependencyModule();
    auto swiftTextualDeps = moduleDeps->getAsSwiftInterfaceModule();
    auto swiftSourceDeps = moduleDeps->getAsSwiftSourceModule();
    auto swiftBinaryDeps = moduleDeps->getAsSwiftBinaryModule();
    auto clangDeps = moduleDeps->getAsClangModule();

    // ModulePath
    const char *modulePathSuffix =
        moduleDeps->isSwiftModule() ? ".swiftmodule" : ".pcm";
    std::string modulePath;
    if (swiftTextualDeps)
      modulePath = swiftTextualDeps->moduleOutputPath;
    else if (swiftPlaceholderDeps)
      modulePath = swiftPlaceholderDeps->compiledModulePath;
    else if (swiftBinaryDeps)
      modulePath = swiftBinaryDeps->compiledModulePath;
    else if (clangDeps)
      modulePath = clangDeps->pcmOutputPath;
    else
      modulePath = module.first + modulePathSuffix;

    // SourceFiles
    std::vector<std::string> sourceFiles;
    if (swiftSourceDeps) {
      sourceFiles = swiftSourceDeps->sourceFiles;
    } else if (clangDeps) {
      sourceFiles = clangDeps->fileDependencies;
    }

    auto optionalDepInfo = cache.findDependency(module.first, module.second);
    assert(optionalDepInfo.has_value() && "Missing dependency info during graph generation diagnosis.");
    auto depInfo = optionalDepInfo.value();
    auto directDependencies = depInfo->getModuleDependencies();

    // Generate a swiftscan_clang_details_t object based on the dependency kind
    auto getModuleDetails = [&]() -> swiftscan_module_details_t {
      swiftscan_module_details_s *details = new swiftscan_module_details_s;
      if (swiftTextualDeps) {
        swiftscan_string_ref_t moduleInterfacePath =
            create_clone(swiftTextualDeps->swiftInterfaceFile.c_str());
        swiftscan_string_ref_t bridgingHeaderPath =
            swiftTextualDeps->textualModuleDetails.bridgingHeaderFile.has_value()
                ? create_clone(
                      swiftTextualDeps->textualModuleDetails.bridgingHeaderFile.value().c_str())
                : create_null();

        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL;

        details->swift_textual_details = {
            moduleInterfacePath,
            create_set(swiftTextualDeps->compiledModuleCandidates),
            bridgingHeaderPath,
            create_set(swiftTextualDeps->textualModuleDetails.bridgingSourceFiles),
            create_set(swiftTextualDeps->textualModuleDetails.bridgingModuleDependencies),
            create_set(swiftTextualDeps->buildCommandLine),
            create_set(swiftTextualDeps->textualModuleDetails.extraPCMArgs),
            create_clone(swiftTextualDeps->contextHash.c_str()),
            swiftTextualDeps->isFramework};
      } else if (swiftSourceDeps) {
        swiftscan_string_ref_t moduleInterfacePath = create_null();
        swiftscan_string_ref_t bridgingHeaderPath =
          swiftSourceDeps->textualModuleDetails.bridgingHeaderFile.has_value()
                ? create_clone(
                           swiftSourceDeps->textualModuleDetails.bridgingHeaderFile.value().c_str())
                : create_null();
        // TODO: Once the clients are taught about the new dependency kind,
        // switch to using a bespoke kind here.
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL;
        details->swift_textual_details = {
            moduleInterfacePath,
            create_empty_set(),
            bridgingHeaderPath,
            create_set(swiftSourceDeps->textualModuleDetails.bridgingSourceFiles),
            create_set(swiftSourceDeps->textualModuleDetails.bridgingModuleDependencies),
            create_empty_set(),
            create_set(swiftSourceDeps->textualModuleDetails.extraPCMArgs),
            /*contextHash*/create_null(),
            /*isFramework*/false};
      } else if (swiftPlaceholderDeps) {
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_PLACEHOLDER;
        details->swift_placeholder_details = {
            create_clone(swiftPlaceholderDeps->compiledModulePath.c_str()),
            create_clone(swiftPlaceholderDeps->moduleDocPath.c_str()),
            create_clone(swiftPlaceholderDeps->sourceInfoPath.c_str())};
      } else if (swiftBinaryDeps) {
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_BINARY;
        details->swift_binary_details = {
            create_clone(swiftBinaryDeps->compiledModulePath.c_str()),
            create_clone(swiftBinaryDeps->moduleDocPath.c_str()),
            create_clone(swiftBinaryDeps->sourceInfoPath.c_str()),
            swiftBinaryDeps->isFramework};
      } else {
        // Clang module details
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_CLANG;
        details->clang_details = {
            create_clone(clangDeps->moduleMapFile.c_str()),
            create_clone(clangDeps->contextHash.c_str()),
            create_set(clangDeps->nonPathCommandLine),
            create_set(clangDeps->capturedPCMArgs)
        };
      }
      return details;
    };

    swiftscan_dependency_info_s *moduleInfo = new swiftscan_dependency_info_s;
    dependencySet->modules[i] = moduleInfo;

    std::string encodedModuleName = createEncodedModuleKindAndName(module);
    auto ttt = create_clone(encodedModuleName.c_str());
    moduleInfo->module_name = ttt;
    moduleInfo->module_path = create_clone(modulePath.c_str());
    moduleInfo->source_files = create_set(sourceFiles);

    // Create a direct dependencies set according to the output format
    std::vector<std::string> bridgedDependencyNames;
    for (const auto &dep : directDependencies) {
      std::string dependencyKindAndName;
      switch (dep.second) {
      case ModuleDependencyKind::SwiftInterface:
      case ModuleDependencyKind::SwiftSource:
        dependencyKindAndName = "swiftTextual";
        break;
      case ModuleDependencyKind::SwiftBinary:
        dependencyKindAndName = "swiftBinary";
        break;
      case ModuleDependencyKind::SwiftPlaceholder:
        dependencyKindAndName = "swiftPlaceholder";
        break;
      case ModuleDependencyKind::Clang:
        dependencyKindAndName = "clang";
        break;
      default:
        llvm_unreachable("Unhandled dependency kind.");
      }
      dependencyKindAndName += ":";
      dependencyKindAndName += dep.first;
      bridgedDependencyNames.push_back(dependencyKindAndName);
    }

    moduleInfo->direct_dependencies = create_set(bridgedDependencyNames);
    moduleInfo->details = getModuleDetails();
  }

  swiftscan_dependency_graph_t result = new swiftscan_dependency_graph_s;
  result->main_module_name = create_clone(mainModuleName.c_str());
  result->dependencies = dependencySet;
  return result;
}

static bool diagnoseCycle(CompilerInstance &instance,
                          ModuleDependenciesCache &cache,
                          ModuleDependencyID mainId,
                          InterfaceSubContextDelegate &astDelegate) {
  ModuleDependencyIDSetVector openSet;
  ModuleDependencyIDSetVector closeSet;
  // Start from the main module.
  openSet.insert(mainId);
  while (!openSet.empty()) {
    auto &lastOpen = openSet.back();
    auto beforeSize = openSet.size();

    auto optionalDepInfo = cache.findDependency(lastOpen.first, lastOpen.second);
    assert(optionalDepInfo.has_value() && "Missing dependency info during cycle diagnosis.");
    auto depInfo = optionalDepInfo.value();

    for (const auto &dep : depInfo->getModuleDependencies()) {
      if (closeSet.count(dep))
        continue;
      if (openSet.insert(dep)) {
        break;
      } else {
        // Find a cycle, diagnose.
        auto startIt = std::find(openSet.begin(), openSet.end(), dep);
        assert(startIt != openSet.end());
        llvm::SmallString<64> buffer;
        for (auto it = startIt; it != openSet.end(); ++it) {
          buffer.append(it->first);
          buffer.append((it->second == ModuleDependencyKind::SwiftInterface ||
                         it->second == ModuleDependencyKind::SwiftSource ||
                         it->second == ModuleDependencyKind::SwiftBinary)
                            ? ".swiftmodule"
                            : ".pcm");
          buffer.append(" -> ");
        }
        buffer.append(startIt->first);
        buffer.append(
            (startIt->second == ModuleDependencyKind::SwiftInterface ||
             startIt->second == ModuleDependencyKind::SwiftSource ||
             startIt->second == ModuleDependencyKind::SwiftBinary)
                ? ".swiftmodule"
                : ".pcm");
        instance.getASTContext().Diags.diagnose(
            SourceLoc(), diag::scanner_find_cycle, buffer.str());
        return true;
      }
    }
    // No new node added. We can close this node
    if (openSet.size() == beforeSize) {
      closeSet.insert(openSet.back());
      openSet.pop_back();
    } else {
      assert(openSet.size() == beforeSize + 1);
    }
  }
  assert(openSet.empty());
  return false;
}

static void updateCachedInstanceOpts(CompilerInstance &cachedInstance,
                                     const CompilerInstance &invocationInstance,
                                     llvm::StringRef entryArguments) {
  cachedInstance.getASTContext().SearchPathOpts =
      invocationInstance.getASTContext().SearchPathOpts;

  // The Clang Importer arguments must consist of a combination of
  // Clang Importer arguments of the current invocation to inherit its Clang-specific
  // search path options, followed by the options specific to the given batch-entry,
  // which may overload some of the invocation's options (e.g. target)
  cachedInstance.getASTContext().ClangImporterOpts =
      invocationInstance.getASTContext().ClangImporterOpts;
  std::istringstream iss(entryArguments.str());
  std::vector<std::string> splitArguments(
      std::istream_iterator<std::string>{iss},
      std::istream_iterator<std::string>());
  for (auto it = splitArguments.begin(), end = splitArguments.end(); it != end;
       ++it) {
    if ((*it) == "-Xcc") {
      assert((it + 1 != end) && "Expected option following '-Xcc'");
      cachedInstance.getASTContext().ClangImporterOpts.ExtraArgs.push_back(
          *(it + 1));
    }
  }
}

static bool
forEachBatchEntry(CompilerInstance &invocationInstance,
                  ModuleDependenciesCache &invocationCache,
                  CompilerArgInstanceCacheMap *versionedPCMInstanceCache,
                  llvm::StringSaver &saver,
                  const std::vector<BatchScanInput> &batchInput,
                  llvm::function_ref<void(BatchScanInput, CompilerInstance &,
                                          ModuleDependenciesCache &)>
                      scanningAction) {
  const CompilerInvocation &invoke = invocationInstance.getInvocation();
  bool localSubInstanceMap = false;
  CompilerArgInstanceCacheMap *subInstanceMap;
  if (versionedPCMInstanceCache)
    subInstanceMap = versionedPCMInstanceCache;
  else {
    subInstanceMap = new CompilerArgInstanceCacheMap;
    localSubInstanceMap = true;
  }
  SWIFT_DEFER {
    if (localSubInstanceMap)
      delete subInstanceMap;
  };

  auto &diags = invocationInstance.getDiags();
  ForwardingDiagnosticConsumer FDC(invocationInstance.getDiags());

  for (auto &entry : batchInput) {
    CompilerInstance *pInstance = nullptr;
    ModuleDependenciesCache *pCache = nullptr;
    if (entry.arguments.empty()) {
      // Use the compiler's instance if no arguments are specified.
      pInstance = &invocationInstance;
      pCache = &invocationCache;
    } else if (subInstanceMap->count(entry.arguments)) {
      // Use the previously created instance if we've seen the arguments
      // before.
      pInstance = std::get<0>((*subInstanceMap)[entry.arguments]).get();
      pCache = std::get<2>((*subInstanceMap)[entry.arguments]).get();
      // We must update the search paths of this instance to instead reflect
      // those of the current scanner invocation.
      updateCachedInstanceOpts(*pInstance, invocationInstance, entry.arguments);
    } else {
      // We must reset option occurrences because we are handling an unrelated command-line
      // to those parsed before. We must do so because LLVM options parsing is done
      // using a managed static `GlobalParser`.
      llvm::cl::ResetAllOptionOccurrences();

      // Create a new instance by the arguments and save it in the map.
      auto newService = std::make_unique<SwiftDependencyScanningService>();
      auto newInstance = std::make_unique<CompilerInstance>();

      SmallVector<const char *, 4> args;
      llvm::cl::TokenizeGNUCommandLine(entry.arguments, saver, args);
      CompilerInvocation subInvoke = invoke;
      newInstance->addDiagnosticConsumer(&FDC);
      if (subInvoke.parseArgs(args, diags)) {
        invocationInstance.getDiags().diagnose(
            SourceLoc(), diag::scanner_arguments_invalid, entry.arguments);
        return true;
      }
      std::string InstanceSetupError;
      if (newInstance->setup(subInvoke, InstanceSetupError)) {
        invocationInstance.getDiags().diagnose(
            SourceLoc(), diag::scanner_arguments_invalid, entry.arguments);
        return true;
      }
      auto mainModuleName = newInstance->getMainModule()->getNameStr();
      auto scanContextHash = newInstance->getInvocation().getModuleScanningHash();
      auto newLocalCache = std::make_unique<ModuleDependenciesCache>(
          *newService, mainModuleName.str(), scanContextHash);
      pInstance = newInstance.get();
      pCache = newLocalCache.get();
      subInstanceMap->insert(
          {entry.arguments,
           std::make_tuple(std::move(newInstance), std::move(newService),
                           std::move(newLocalCache))});
    }
    assert(pInstance);
    assert(pCache);
    scanningAction(entry, *pInstance, *pCache);
  }

  return false;
}

static ModuleDependencyInfo
identifyMainModuleDependencies(CompilerInstance &instance) {
  ModuleDecl *mainModule = instance.getMainModule();
  // Main module file name.
  auto newExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  llvm::SmallString<32> mainModulePath = mainModule->getName().str();
  llvm::sys::path::replace_extension(mainModulePath, newExt);

  std::string apinotesVer =
      (llvm::Twine("-fapinotes-swift-version=") +
       instance.getASTContext()
           .LangOpts.EffectiveLanguageVersion.asAPINotesVersionString())
          .str();

  // Compute the dependencies of the main module.
  std::vector<StringRef> ExtraPCMArgs = {
    "-Xcc", apinotesVer
  };
  if (!instance.getASTContext().LangOpts.ClangTarget.has_value())
    ExtraPCMArgs.insert(ExtraPCMArgs.begin(),
                        {"-Xcc", "-target", "-Xcc",
                         instance.getASTContext().LangOpts.Target.str()});
  auto mainDependencies = ModuleDependencyInfo::forSwiftSourceModule(ExtraPCMArgs);

  // Compute Implicit dependencies of the main module
  {
    llvm::StringSet<> alreadyAddedModules;
    for (auto fileUnit : mainModule->getFiles()) {
      auto sf = dyn_cast<SourceFile>(fileUnit);
      if (!sf)
        continue;

      mainDependencies.addModuleImport(*sf, alreadyAddedModules);
    }

    const auto &importInfo = mainModule->getImplicitImportInfo();

    // Swift standard library.
    switch (importInfo.StdlibKind) {
    case ImplicitStdlibKind::None:
    case ImplicitStdlibKind::Builtin:
      break;

    case ImplicitStdlibKind::Stdlib:
      mainDependencies.addModuleImport("Swift", &alreadyAddedModules);
      break;
    }

    // Add any implicit module names.
    for (const auto &import : importInfo.AdditionalUnloadedImports) {
      mainDependencies.addModuleImport(import.module.getModulePath(),
                                           &alreadyAddedModules);
    }

    // Already-loaded, implicitly imported module names.
    for (const auto &import : importInfo.AdditionalImports) {
      mainDependencies.addModuleImport(
          import.module.importedModule->getNameStr(), &alreadyAddedModules);
    }

    // Add the bridging header.
    if (!importInfo.BridgingHeaderPath.empty()) {
      mainDependencies.addBridgingHeader(importInfo.BridgingHeaderPath);
    }

    // If we are to import the underlying Clang module of the same name,
    // add a dependency with the same name to trigger the search.
    if (importInfo.ShouldImportUnderlyingModule) {
      mainDependencies.addModuleImport(mainModule->getName().str(),
                                           &alreadyAddedModules);
    }
  }

  return mainDependencies;
}

} // namespace

static void serializeDependencyCache(CompilerInstance &instance,
                                     const SwiftDependencyScanningService &service) {
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  ASTContext &Context = instance.getASTContext();
  auto savePath = opts.SerializedDependencyScannerCachePath;
  module_dependency_cache_serialization::writeInterModuleDependenciesCache(
      Context.Diags, savePath, service);
  if (opts.EmitDependencyScannerCacheRemarks) {
    Context.Diags.diagnose(SourceLoc(), diag::remark_save_cache, savePath);
  }
}

static void deserializeDependencyCache(CompilerInstance &instance,
                                       SwiftDependencyScanningService &service) {
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  ASTContext &Context = instance.getASTContext();
  auto loadPath = opts.SerializedDependencyScannerCachePath;
  if (module_dependency_cache_serialization::readInterModuleDependenciesCache(
          loadPath, service)) {
    Context.Diags.diagnose(SourceLoc(), diag::warn_scanner_deserialize_failed,
                           loadPath);
  } else if (opts.EmitDependencyScannerCacheRemarks) {
    Context.Diags.diagnose(SourceLoc(), diag::remark_reuse_cache, loadPath);
  }
}

bool swift::dependencies::scanDependencies(CompilerInstance &instance) {
  ASTContext &Context = instance.getASTContext();
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  std::string path = opts.InputsAndOutputs.getSingleOutputFilename();
  std::error_code EC;
  llvm::raw_fd_ostream out(path, EC, llvm::sys::fs::OF_None);
  if (out.has_error() || EC) {
    Context.Diags.diagnose(SourceLoc(), diag::error_opening_output, path,
                           EC.message());
    out.clear_error();
    return true;
  }

  // `-scan-dependencies` invocations use a single new instance
  // of a module cache
  SwiftDependencyScanningService service;
  if (opts.ReuseDependencyScannerCache)
    deserializeDependencyCache(instance, service);
  // Wrap the filesystem with a caching `DependencyScanningWorkerFilesystem`
  service.overlaySharedFilesystemCacheForCompilation(instance);
  ModuleDependenciesCache cache(service,
                                instance.getMainModule()->getNameStr().str(),
                                instance.getInvocation().getModuleScanningHash());
  auto ModuleCachePath = getModuleCachePathFromClang(
               Context.getClangModuleLoader()->getClangInstance());

  // Execute scan
  auto dependenciesOrErr = performModuleScan(instance, cache);

  // Serialize the dependency cache if -serialize-dependency-scan-cache
  // is specified
  if (opts.SerializeDependencyScannerCache)
    serializeDependencyCache(instance, service);
  
  if (dependenciesOrErr.getError())
    return true;
  auto dependencies = std::move(*dependenciesOrErr);

  // Write out the JSON description.
  writeJSON(out, dependencies);
  // This process succeeds regardless of whether any errors occurred.
  // FIXME: We shouldn't need this, but it's masking bugs in our scanning
  // logic where we don't create a fresh context when scanning Swift interfaces
  // that includes their own command-line flags.
  Context.Diags.resetHadAnyError();
  return false;
}

bool swift::dependencies::prescanDependencies(CompilerInstance &instance) {
  ASTContext &Context = instance.getASTContext();
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  std::string path = opts.InputsAndOutputs.getSingleOutputFilename();
  std::error_code EC;
  llvm::raw_fd_ostream out(path, EC, llvm::sys::fs::OF_None);
  // `-scan-dependencies` invocations use a single new instance
  // of a module cache
  SwiftDependencyScanningService singleUseService;
  ModuleDependenciesCache cache(singleUseService,
                                instance.getMainModule()->getNameStr().str(),
                                instance.getInvocation().getModuleScanningHash());
  if (out.has_error() || EC) {
    Context.Diags.diagnose(SourceLoc(), diag::error_opening_output, path,
                           EC.message());
    out.clear_error();
    return true;
  }

  // Execute import prescan, and write JSON output to the output stream
  auto importSetOrErr = performModulePrescan(instance);
  if (importSetOrErr.getError())
    return true;
  auto importSet = std::move(*importSetOrErr);

  // Serialize and output main module dependencies only and exit.
  writePrescanJSON(out, importSet);
  // This process succeeds regardless of whether any errors occurred.
  // FIXME: We shouldn't need this, but it's masking bugs in our scanning
  // logic where we don't create a fresh context when scanning Swift interfaces
  // that includes their own command-line flags.
  Context.Diags.resetHadAnyError();
  return false;
}

bool swift::dependencies::batchScanDependencies(
    CompilerInstance &instance, llvm::StringRef batchInputFile) {
  // The primary cache used for scans carried out with the compiler instance
  // we have created

  SwiftDependencyScanningService singleUseService;
  singleUseService.overlaySharedFilesystemCacheForCompilation(instance);
  ModuleDependenciesCache cache(singleUseService,
                                instance.getMainModule()->getNameStr().str(),
                                instance.getInvocation().getModuleScanningHash());
  (void)instance.getMainModule();
  llvm::BumpPtrAllocator alloc;
  llvm::StringSaver saver(alloc);
  auto batchInput =
      parseBatchScanInputFile(instance.getASTContext(), batchInputFile, saver);
  if (!batchInput.has_value())
    return true;

  auto batchScanResults = performBatchModuleScan(
      instance, cache, /*versionedPCMInstanceCache*/ nullptr, saver,
      *batchInput);

  // Write the result JSON to the specified output path, for each entry
  auto ientries = batchInput->cbegin();
  auto iresults = batchScanResults.cbegin();
  for (; ientries != batchInput->end() and iresults != batchScanResults.end();
       ++ientries, ++iresults) {
    std::error_code EC;
    llvm::raw_fd_ostream out((*ientries).outputPath, EC, llvm::sys::fs::OF_None);
    if ((*iresults).getError())
      return true;

    writeJSON(out, **iresults);
  }
  return false;
}

bool swift::dependencies::batchPrescanDependencies(
    CompilerInstance &instance, llvm::StringRef batchInputFile) {
  // The primary cache used for scans carried out with the compiler instance
  // we have created
  SwiftDependencyScanningService singleUseService;
  ModuleDependenciesCache cache(singleUseService,
                                instance.getMainModule()->getNameStr().str(),
                                instance.getInvocation().getModuleScanningHash());
  (void)instance.getMainModule();
  llvm::BumpPtrAllocator alloc;
  llvm::StringSaver saver(alloc);
  auto batchInput =
      parseBatchScanInputFile(instance.getASTContext(), batchInputFile, saver);
  if (!batchInput.has_value())
    return true;

  auto batchPrescanResults =
      performBatchModulePrescan(instance, cache, saver, *batchInput);

  // Write the result JSON to the specified output path, for each entry
  auto ientries = batchInput->cbegin();
  auto iresults = batchPrescanResults.cbegin();
  for (;
       ientries != batchInput->end() and iresults != batchPrescanResults.end();
       ++ientries, ++iresults) {
    std::error_code EC;
    llvm::raw_fd_ostream out((*ientries).outputPath, EC, llvm::sys::fs::OF_None);
    if ((*iresults).getError())
      return true;

    writePrescanJSON(out, **iresults);
  }
  return false;
}

std::string swift::dependencies::createEncodedModuleKindAndName(ModuleDependencyID id) {
  switch (id.second) {
  case ModuleDependencyKind::SwiftInterface:
  case ModuleDependencyKind::SwiftSource:
    return "swiftTextual:" + id.first;
  case ModuleDependencyKind::SwiftBinary:
    return "swiftBinary:" + id.first;
  case ModuleDependencyKind::SwiftPlaceholder:
    return "swiftPlaceholder:" + id.first;
  case ModuleDependencyKind::Clang:
    return "clang:" + id.first;
  default:
    llvm_unreachable("Unhandled dependency kind.");
  }
}

llvm::ErrorOr<swiftscan_dependency_graph_t>
swift::dependencies::performModuleScan(CompilerInstance &instance,
                                       ModuleDependenciesCache &cache) {
  ModuleDecl *mainModule = instance.getMainModule();
  // First, identify the dependencies of the main module
  auto mainDependencies = identifyMainModuleDependencies(instance);
  auto &ctx = instance.getASTContext();

  // Add the main module.
  StringRef mainModuleName = mainModule->getNameStr();
  auto mainModuleID = ModuleDependencyID{mainModuleName.str(), mainDependencies.getKind()};

  ModuleDependencyIDSetVector allModules;
  allModules.insert(mainModuleID);

  // We may be re-using an instance of the cache which already contains
  // an entry for this module.
  if (cache.findDependency(
                mainModuleName, ModuleDependencyKind::SwiftSource)) {
    cache.updateDependency(
        std::make_pair(mainModuleName.str(),
                       ModuleDependencyKind::SwiftSource),
        std::move(mainDependencies));
  } else {
    cache.recordDependency(mainModuleName, std::move(mainDependencies));
  }

  auto ModuleCachePath = getModuleCachePathFromClang(
      ctx.getClangModuleLoader()->getClangInstance());
  auto &FEOpts = instance.getInvocation().getFrontendOptions();
  ModuleInterfaceLoaderOptions LoaderOpts(FEOpts);
  InterfaceSubContextDelegateImpl ASTDelegate(
      ctx.SourceMgr, &ctx.Diags, ctx.SearchPathOpts, ctx.LangOpts,
      ctx.ClangImporterOpts, LoaderOpts,
      /*buildModuleCacheDirIfAbsent*/ false, ModuleCachePath,
      FEOpts.PrebuiltModuleCachePath,
      FEOpts.BackupModuleInterfaceDir,
      FEOpts.SerializeModuleInterfaceDependencyHashes,
      FEOpts.shouldTrackSystemDependencies(),
      RequireOSSAModules_t(instance.getSILOptions()));

  // Explore the dependencies of every module.
  for (unsigned currentModuleIdx = 0; currentModuleIdx < allModules.size();
       ++currentModuleIdx) {
    auto module = allModules[currentModuleIdx];
    auto discoveredModules =
        resolveDirectDependencies(instance, module, cache, ASTDelegate);
    allModules.insert(discoveredModules.begin(), discoveredModules.end());
  }

  // We have all explicit imports now, resolve cross import overlays.
  discoverCrossImportOverlayDependencies(
      instance, mainModuleName,
      /*All transitive dependencies*/ allModules.getArrayRef().slice(1), cache,
      ASTDelegate, [&](ModuleDependencyID id) { allModules.insert(id); });

#ifndef NDEBUG
  // Verify that all collected dependencies have had their
  // imports resolved to module IDs
  for (const auto &moduleID : allModules) {
    const auto &moduleInfo = cache.findDependency(moduleID.first, moduleID.second).value();
    assert(moduleInfo->isResolved());
  }
#endif

  // Diagnose cycle in dependency graph.
  if (diagnoseCycle(instance, cache, mainModuleID, ASTDelegate))
    return std::make_error_code(std::errc::not_supported);

  auto dependencyGraph = generateFullDependencyGraph(
      instance, cache, ASTDelegate, allModules.getArrayRef());
  // Update the dependency tracker.
  if (auto depTracker = instance.getDependencyTracker()) {
    for (auto module : allModules) {
      auto optionalDeps = cache.findDependency(module.first, module.second);
      if (!optionalDeps.has_value())
        continue;
      auto deps = optionalDeps.value();

      if (auto swiftDeps = deps->getAsSwiftInterfaceModule()) {
        depTracker->addDependency(swiftDeps->swiftInterfaceFile,
                                  /*IsSystem=*/false);
        for (const auto &bridgingSourceFile :
             swiftDeps->textualModuleDetails.bridgingSourceFiles)
          depTracker->addDependency(bridgingSourceFile, /*IsSystem=*/false);
      } else if (auto swiftSourceDeps = deps->getAsSwiftSourceModule()) {
        for (const auto &sourceFile : swiftSourceDeps->sourceFiles)
          depTracker->addDependency(sourceFile, /*IsSystem=*/false);
        for (const auto &bridgingSourceFile :
             swiftSourceDeps->textualModuleDetails.bridgingSourceFiles)
          depTracker->addDependency(bridgingSourceFile, /*IsSystem=*/false);
      } else if (auto clangDeps = deps->getAsClangModule()) {
        if (!clangDeps->moduleMapFile.empty())
          depTracker->addDependency(clangDeps->moduleMapFile,
                                    /*IsSystem=*/false);
        for (const auto &sourceFile : clangDeps->fileDependencies)
          depTracker->addDependency(sourceFile, /*IsSystem=*/false);
      }
    }
  }

  return dependencyGraph;
}

llvm::ErrorOr<swiftscan_import_set_t>
swift::dependencies::performModulePrescan(CompilerInstance &instance) {
  // Execute import prescan, and write JSON output to the output stream
  auto mainDependencies = identifyMainModuleDependencies(instance);
  auto *importSet = new swiftscan_import_set_s;
  importSet->imports = create_set(mainDependencies.getModuleImports());
  return importSet;
}

std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>>
swift::dependencies::performBatchModuleScan(
    CompilerInstance &invocationInstance,
    ModuleDependenciesCache &invocationCache,
    CompilerArgInstanceCacheMap *versionedPCMInstanceCache,
    llvm::StringSaver &saver, const std::vector<BatchScanInput> &batchInput) {
  std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>> batchScanResult;
  batchScanResult.reserve(batchInput.size());

  // Perform a full dependency scan for each batch entry module
  forEachBatchEntry(
      invocationInstance, invocationCache, versionedPCMInstanceCache, saver,
      batchInput,
      [&batchScanResult](BatchScanInput entry, CompilerInstance &instance,
                         ModuleDependenciesCache &cache) {
        StringRef moduleName = entry.moduleName;
        bool isClang = !entry.isSwift;
        ASTContext &ctx = instance.getASTContext();
        auto &FEOpts = instance.getInvocation().getFrontendOptions();
        ModuleInterfaceLoaderOptions LoaderOpts(FEOpts);
        auto ModuleCachePath = getModuleCachePathFromClang(
            ctx.getClangModuleLoader()->getClangInstance());

        ModuleDependencyIDSetVector allModules;
        InterfaceSubContextDelegateImpl ASTDelegate(
            ctx.SourceMgr, &ctx.Diags, ctx.SearchPathOpts, ctx.LangOpts,
            ctx.ClangImporterOpts, LoaderOpts,
            /*buildModuleCacheDirIfAbsent*/ false, ModuleCachePath,
            FEOpts.PrebuiltModuleCachePath,
            FEOpts.BackupModuleInterfaceDir,
            FEOpts.SerializeModuleInterfaceDependencyHashes,
            FEOpts.shouldTrackSystemDependencies(),
            RequireOSSAModules_t(instance.getSILOptions()));
        Optional<const ModuleDependencyInfo*> rootDeps;
        if (isClang) {
          // Loading the clang module using Clang importer.
          // This action will populate the cache with the main module's
          // dependencies.
          rootDeps =
              ctx.getClangModuleDependencies(moduleName, cache, ASTDelegate);
        } else {
          // Loading the swift module's dependencies.
          rootDeps =
              ctx.getSwiftModuleDependencies(moduleName, cache, ASTDelegate);
        }
        if (!rootDeps.has_value()) {
          // We cannot find the clang module, abort.
          batchScanResult.push_back(
              std::make_error_code(std::errc::invalid_argument));
          return;
        }
        // Add the main module.
        allModules.insert(
            {moduleName.str(), isClang ? ModuleDependencyKind::Clang
                                       : ModuleDependencyKind::SwiftInterface});

        // Explore the dependencies of every module.
        for (unsigned currentModuleIdx = 0;
             currentModuleIdx < allModules.size(); ++currentModuleIdx) {
          auto module = allModules[currentModuleIdx];
          auto discoveredModules =
              resolveDirectDependencies(instance, module, cache, ASTDelegate);
          allModules.insert(discoveredModules.begin(), discoveredModules.end());
        }

        batchScanResult.push_back(generateFullDependencyGraph(
            instance, cache, ASTDelegate,
            allModules.getArrayRef()));
      });

  return batchScanResult;
}

std::vector<llvm::ErrorOr<swiftscan_import_set_t>>
swift::dependencies::performBatchModulePrescan(
    CompilerInstance &instance, ModuleDependenciesCache &cache,
    llvm::StringSaver &saver, const std::vector<BatchScanInput> &batchInput) {
  std::vector<llvm::ErrorOr<swiftscan_import_set_t>> batchPrescanResult;

  // Perform a full dependency scan for each batch entry module
  forEachBatchEntry(
      instance, cache, /*versionedPCMInstanceCache*/ nullptr, saver, batchInput,
      [&batchPrescanResult](BatchScanInput entry, CompilerInstance &instance,
                            ModuleDependenciesCache &cache) {
        StringRef moduleName = entry.moduleName;
        bool isClang = !entry.isSwift;
        ASTContext &ctx = instance.getASTContext();
        auto &FEOpts = instance.getInvocation().getFrontendOptions();
        ModuleInterfaceLoaderOptions LoaderOpts(FEOpts);
        auto ModuleCachePath = getModuleCachePathFromClang(
            ctx.getClangModuleLoader()->getClangInstance());
        ModuleDependencyIDSetVector allModules;
        InterfaceSubContextDelegateImpl ASTDelegate(
            ctx.SourceMgr, &ctx.Diags, ctx.SearchPathOpts, ctx.LangOpts,
            ctx.ClangImporterOpts, LoaderOpts,
            /*buildModuleCacheDirIfAbsent*/ false, ModuleCachePath,
            FEOpts.PrebuiltModuleCachePath,
            FEOpts.BackupModuleInterfaceDir,
            FEOpts.SerializeModuleInterfaceDependencyHashes,
            FEOpts.shouldTrackSystemDependencies(),
            RequireOSSAModules_t(instance.getSILOptions()));
        Optional<const ModuleDependencyInfo*> rootDeps;
        if (isClang) {
          // Loading the clang module using Clang importer.
          // This action will populate the cache with the main module's
          // dependencies.
          rootDeps =
              ctx.getClangModuleDependencies(moduleName, cache, ASTDelegate);
        } else {
          // Loading the swift module's dependencies.
          rootDeps =
              ctx.getSwiftModuleDependencies(moduleName, cache, ASTDelegate);
        }
        if (!rootDeps.has_value()) {
          // We cannot find the clang module, abort.
          batchPrescanResult.push_back(
              std::make_error_code(std::errc::invalid_argument));
          return;
        }

        auto *importSet = new swiftscan_import_set_s;
        importSet->imports = create_set(rootDeps.value()->getModuleImports());
        batchPrescanResult.push_back(importSet);
      });

  return batchPrescanResult;
}
