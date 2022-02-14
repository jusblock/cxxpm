#include "cxx-pm-config.h"
extern "C" {
#include "tiny_sha3.h"
}

#include "cxx-pm.h"
#include "exec.h"
#include "strExtras.h"
#include "compilers/common.h"
#include "bs/cmake.h"
#include "os.h"
#include "package.h"
#include "sha3.h"

#ifdef WIN32
#include <Windows.h>
#endif

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptCompilerCommand,
  clOptCompilerFlags,
  clOptCompilerDefinitions,
  clOptBuildType,
  clOptBuildTypeMapping,
  clOptVSInstallDir,
  clOptVCToolset,
  clOptSysRoot,
  clOptUseFlags,
  clOptSystemName,
  clOptSystemProcessor,
  clOptKey,
  clOptPackageList,
  clOptSearchPath,
  clOptSearchPathType,
  clOptInstall,
  clOptExportCMake,
  clOptPackageRoot,
  clOptPackageExtraDirectory,
  clOptFile,
  clOptVerbose,
  clOptVersion
};

enum EModeTy {
  ENoMode = 0,
  EPackageList,
  ESearchPath,
  EInstall
};

static option cmdLineOpts[] = {
  {"compiler", required_argument, nullptr, clOptCompilerCommand},
  {"compiler-flags", required_argument, nullptr, clOptCompilerFlags},
  {"compiler-definitions", required_argument, nullptr, clOptCompilerDefinitions},
  {"use-flags", required_argument, nullptr, clOptUseFlags},
  {"system-name", required_argument, nullptr, clOptSystemName},
  {"system-processor", required_argument, nullptr, clOptSystemProcessor},
  {"build-type", required_argument, nullptr, clOptBuildType},
  {"build-type-mapping", required_argument, nullptr, clOptBuildTypeMapping},
  {"vs-install-dir", required_argument, nullptr, clOptVSInstallDir},
  {"vc-toolset", required_argument, nullptr, clOptVCToolset},
  {"sys-root", required_argument, nullptr, clOptSysRoot},
  // modes
  {"help", no_argument, nullptr, clOptHelp},
  {"package-list", no_argument, nullptr, clOptPackageList},
  {"search-path", required_argument, nullptr, clOptSearchPath},
  {"search-path-type", required_argument, nullptr, clOptSearchPathType},
  {"install", required_argument, nullptr, clOptInstall},
  {"export-cmake", required_argument, nullptr, clOptExportCMake},
  {"version", no_argument, nullptr, clOptVersion},
  // extra parameters
  {"package-root", required_argument, nullptr, clOptPackageRoot},
  {"package-extra-dir", required_argument, nullptr, clOptPackageExtraDirectory},
  // arguments
  {"file", required_argument, nullptr, clOptFile},
  // other
  {"verbose", no_argument, nullptr, clOptVerbose},
  {nullptr, 0, nullptr, 0}
};

struct CContext {
  CxxPmSettings GlobalSettings;
  CSystemInfo SystemInfo;
  CompilersArray Compilers;
  ToolsArray Tools;
};

bool loadVariables(const std::filesystem::path &path, const std::vector<std::string> &names, std::vector<std::string> &variables)
{
  std::string capturedOut;
  std::string capturedErr;
  std::filesystem::path fullPath;
  std::string args;

  args = "set -e; source ";
  args.append(pathConvert(path, EPathType::Posix).string());
  args.append("; ");
  for (const auto &v: names) {
    args.append("echo $");
    args.append(v);
    args.append("@; ");
  }

  if (!run(path.parent_path(), "bash", {"-c", args}, {}, fullPath, capturedOut, capturedErr, true)) {
    if (!fullPath.empty())
      fprintf(stderr, "%s\n", capturedErr.c_str());
    return false;
  }

  StringSplitter splitter(capturedOut, "\r\n");
  while (splitter.next()) {
    auto v = splitter.get();
    if (v.back() != '@')
      continue;
    variables.emplace_back(v.begin(), v.end()-1);
  }

  return names.size() == variables.size();
}

bool loadSingleVariable(const std::filesystem::path &path, const std::string &name, std::string &variable)
{
  std::string capturedOut;
  std::string capturedErr;
  std::filesystem::path fullPath;
  std::string args;

  args = "set -e; source ";
  args.append(pathConvert(path, EPathType::Posix).string());
  args.append("; echo $");
  args.append(name);
  args.append(";");

  if (!run(path.parent_path(), "bash", {"-c", args}, {}, fullPath, capturedOut, capturedErr, true)) {
    if (!fullPath.empty())
      fprintf(stderr, "%s\n", capturedErr.c_str());
    return false;
  }

  unsigned counter = 0;
  StringSplitter splitter(capturedOut, "\r\n");
  if (splitter.next())
    variable = std::string(splitter.get());
  return !splitter.next();
}

bool packageQueryVersion(CPackage &package, const std::string &requestedVersion, bool verbose)
{
  // Load default version from meta build
  std::string version = requestedVersion;
  if (!loadSingleVariable(package.Path / "meta.build", "DEFAULT_VERSION", version)) {
    fprintf(stderr, "ERROR: can't load DEFAULT_VERSION from %s\n", (package.Path / "meta.build").string().c_str());
    return false;
  } else {
    if (verbose)
      printf("Default version for %s is %s\n", package.Name.c_str(), version.c_str());
  }

  package.Version = version;
  return true;
}

bool locatePackageBuildFile(CPackage &package)
{
  if (std::filesystem::exists(package.Path / (package.Version+".build"))) {
    package.BuildFile = package.Path / (package.Version+".build");
    return true;
  } else {
    for (const auto &extraPath: package.ExtraPath) {
      if (std::filesystem::exists(extraPath / (package.Version+".build"))) {
        package.BuildFile = extraPath / (package.Version+".build");
        return true;
      }
    }
  }

  return false;
}

bool inspectPackage(const CContext &context, CPackage &package, const std::string &requestedVersion, bool verbose)
{
  if (!packageQueryVersion(package, requestedVersion, verbose))
    return false;

  if (!locatePackageBuildFile(package)) {
    fprintf(stderr, "ERROR: package %s doen not contains build file for version %s\n", package.Name.c_str(), package.Version.c_str());
    return false;
  }

  // Query package compilers
  std::vector<std::string> variables;
  std::string packageTypeVariable;
  std::string compilersVariable;
  if (!loadVariables(package.BuildFile, { "PACKAGE_TYPE", "LANGS" }, variables)) {
    fprintf(stderr, "ERROR: can't load PACKAGE_TYPE, LANGS variables from %s\n", package.BuildFile.string().c_str());
    return false;
  }

  packageTypeVariable = variables[0];
  compilersVariable = variables[1];

  // Check package type
  if (packageTypeVariable.empty()) {
    fprintf(stderr, "ERROR: package type not specified in %s\n", package.BuildFile.string().c_str());
    return false;
  }

  if (packageTypeVariable == "binary") {
    package.IsBinary = true;
    return true;
  } else if (packageTypeVariable == "source") {
    package.IsBinary = false;

    StringSplitter splitter(compilersVariable, ",");
    while (splitter.next()) {
      auto langS = splitter.get();
      ELanguage lang = languageFromString(langS);
      if (lang == ELanguage::Unknown) {
        fprintf(stderr, "ERROR: unsupported language %s\n", langS.data());
        return false;
      }
      package.Languages.push_back(lang);
    }

    if (package.Languages.empty()) {
      fprintf(stderr, "ERROR: compilers not specified at %s\n", package.BuildFile.string().c_str());
      return false;
    }

    return true;
  } else {
    fprintf(stderr, "ERROR: package type can be 'source' or 'binary', %s found\n", packageTypeVariable.c_str());
    return false;
  }
}

void updatePackagePrefix(const CContext &context, CPackage &package, const std::string &buildType, bool verbose)
{
  package.Prefix = packagePrefix(context.GlobalSettings.HomeDir, package, context.Compilers, context.SystemInfo, buildType, verbose);
}

void createManifestForDirectory(FILE *hLog, const std::filesystem::path &directory, const std::filesystem::path &relativePath)
{
  for(const auto &element: std::filesystem::directory_iterator{directory}) {
    if (element.is_directory()) {
      createManifestForDirectory(hLog, element, relativePath / element.path().filename());
    } else {
      std::string hash = sha3FileHash(element);
      fprintf(hLog, "%s!%s\n", (relativePath / element.path().filename()).string().c_str(), hash.c_str());
    }
  }
}

bool downloadPackageFiles(const CContext& context,
                          const CPackage& package,
                          const std::filesystem::path &sourceDir,
                          const std::filesystem::path &binaryInstallDir)
{
  std::string type;
  std::string url;
  std::string sha3;
  std::string tag;
  std::string commit;
  std::filesystem::path destination;
  if (!package.IsBinary) {
    std::vector<std::string> variables;
    if (!loadVariables(package.BuildFile, { "TYPE", "URL", "SHA3", "TAG", "COMMIT" }, variables)) {
      fprintf(stderr, "ERROR, can't load TYPE, URL, SHA3, TAG, COMMIT from %s\n", package.BuildFile.string().c_str());
      return false;
    }

    type = std::move(variables[0]);
    url = std::move(variables[1]);
    sha3 = std::move(variables[2]);
    tag = std::move(variables[3]);
    commit = std::move(variables[4]);
    destination = sourceDir;
  } else {
    std::vector<std::string> variableNames;
    std::vector<std::string> variables;
    std::string namePrefix = context.SystemInfo.HostSystemName + "_" + context.SystemInfo.HostSystemProcessor + "_";
    for (const auto &name : { "TYPE", "URL", "SHA3", "TAG", "COMMIT" })
      variableNames.emplace_back(namePrefix + name);

    if (!loadVariables(package.BuildFile, variableNames, variables)) {
      fprintf(stderr, "ERROR, can't load TYPE, URL, SHA3, TAG, COMMIT from %s\n", package.BuildFile.string().c_str());
      return false;
    }

    type = std::move(variables[0]);
    url = std::move(variables[1]);
    sha3 = std::move(variables[2]);
    destination = binaryInstallDir;
  }

  printf("Downloading package %s:%s\n", package.Name.c_str(), package.Version.c_str());
  if (type == "archive") {
    if (url.empty()) {
      fprintf(stderr, "ERROR: URL must be specified for 'archive'\n");
      return false;
    }
    if (sha3.size() != 64) {
      fprintf(stderr, "ERROR: SHA3 256 bit hash must be specified for 'archive'\n");
      return false;
    }

    // get file name from url
    size_t pos = 0;
    size_t nextPos;
    while ((nextPos = url.find('/', pos)) != url.npos)
      pos = nextPos + 1;
    if (url.size() - pos < 2) {
      fprintf(stderr, "ERROR: invalid url: %s\n", url.c_str());
      return false;
    }
    std::filesystem::path archiveFilePath = context.GlobalSettings.DistrDir / (url.data() + pos);

    // Check presence & hash
    bool fileExists = false;
    if (std::filesystem::exists(archiveFilePath)) {
      std::string existingHash = sha3FileHash(archiveFilePath);
      if (existingHash.empty()) {
        fprintf(stderr, "ERROR: can't calculate SHA3 hash of %s\n", archiveFilePath.string().c_str());
        return false;
      }

      if (existingHash == sha3) {
        printf("Archive %s already exists\n", archiveFilePath.string().c_str());
        fileExists = true;
      }
      else {
        fprintf(stderr, "SHA3 mismatch: sha3(%s)=%s, required %s\n", archiveFilePath.string().c_str(), existingHash.c_str(), sha3.c_str());
        if (!std::filesystem::remove(archiveFilePath)) {
          fprintf(stderr, "ERROR: can't delete file %s\n", archiveFilePath.string().c_str());
          return false;
        }
      }
    }

    if (!fileExists) {
      // Downloading file
      if (!runNoCapture(".", "wget", { url, "-O", archiveFilePath.string() }, {}, true)) {
        fprintf(stderr, "Can't download file %s\n", url.c_str());
        return false;
      }

      std::string downloadedHash = sha3FileHash(archiveFilePath);
      if (downloadedHash != sha3) {
        fprintf(stderr, "SHA3 mismatch: sha3(%s)=%s, required %s\n", archiveFilePath.string().c_str(), downloadedHash.c_str(), sha3.c_str());
        return false;
      }
    }

    // Unpacking file
    // Detect archive type
    auto archiveFilePathPosix = pathConvert(archiveFilePath, EPathType::Posix);
    auto destinationPosix = pathConvert(destination, EPathType::Posix);

    if (endsWith(archiveFilePathPosix.string(), ".zip")) {
      if (!runNoCapture(".", "unzip", { archiveFilePathPosix.string(), "-d", destinationPosix.string()}, {}, true)) {
        fprintf(stderr, "Unpacking error\n");
        return false;
      }
    } else if (endsWith(archiveFilePathPosix.string(), ".tar.gz")) {
      if (!runNoCapture(".", "tar", { "-xzf", archiveFilePathPosix.string(), "-C", destinationPosix.string() }, {}, true)) {
        fprintf(stderr, "Unpacking error\n");
        return false;
      }
    } else if (endsWith(archiveFilePathPosix.string(), ".tar.bz2")) {
      if (!runNoCapture(".", "tar", { "-xjf", archiveFilePathPosix.string(), "-C", destinationPosix.string() }, {}, true)) {
        fprintf(stderr, "Unpacking error\n");
        return false;
      }
    } else if (endsWith(archiveFilePathPosix.string(), ".tar.lz") || endsWith(archiveFilePathPosix.string(), ".tar.lzma")) {
        if (!runNoCapture(".", "tar", { "--lzip", "-xvf", archiveFilePathPosix.string(), "-C", destinationPosix.string() }, {}, true)) {
          fprintf(stderr, "Unpacking error\n");
          return false;
        }
    } else if (endsWith(archiveFilePathPosix.string(), ".tar.zst")) {
      std::string tmpFileName = archiveFilePathPosix.filename().string();
      std::filesystem::path tmpFilePath = context.GlobalSettings.DistrDir / ("tmp-" + tmpFileName.substr(0, tmpFileName.size() - 4));
      std::filesystem::path tmpFilePathPosix = pathConvert(tmpFilePath, EPathType::Posix);
      bool success = true;
      if (!runNoCapture(".", "unzstd", { archiveFilePathPosix.string(), "-o", tmpFilePathPosix.string() }, {}, true) ||
          !runNoCapture(".", "tar", { "-xf", tmpFilePathPosix.string(), "-C", destinationPosix.string() }, {}, true))
        success = false;
      std::error_code ec;
      std::filesystem::remove(tmpFilePath, ec);
      if (!success) {
        fprintf(stderr, "Unpacking error\n");
        return false;
      }
    } else {
      fprintf(stderr, "Unknown archive file: %s\n", archiveFilePath.string().c_str());
      return false;

    }
  } else if (type == "git") {
    std::vector<std::string> gitArgs = {"clone", url, "."};
    if (!tag.empty()) {
      gitArgs.emplace_back("-b");
      gitArgs.emplace_back(tag);
    }

    if (!runNoCapture(destination, "git", gitArgs, {}, true)) {
      fprintf(stderr, "git clone error url: %s tag: %s\n", url.c_str(), tag.c_str());
      return false;
    }

    if (!commit.empty()) {
      if (!runNoCapture(destination, "git", {"reset", "--hard", commit}, {}, true)) {
        fprintf(stderr, "git reset hard error commit: %s\n", commit.c_str());
      }
    }

    return true;
  } else {
    fprintf(stderr, "ERROR: unsupported type: %s\n", type.c_str());
    return false;
  }

  return true;
}

static bool removeDirectory(const std::filesystem::path &path)
{
  std::error_code ec;
  if (std::filesystem::exists(path) && (!std::filesystem::remove_all(path, ec) || ec != std::error_code()))  {
#ifdef WIN32
    // Windows implementation of std::filesystem::remove_all contains a bug
    if (!runNoCapture(".", "rm", { "-rf", path.string() }, {}, true) || std::filesystem::exists(path)) {
      fprintf(stderr, "ERROR: can't delete folder %s\n", path.string().c_str());
      fprintf(stderr, "%s\n", ec.message().c_str());
      return false;
    }
#else
    fprintf(stderr, "ERROR: can't delete folder %s\n", path.string().c_str());
    return false;
#endif
  }

  return true;
}

bool install(CContext &context, std::map<std::string, CPackage> &allPackages, CPackage &package, const std::string &buildType, bool verbose, const std::filesystem::path &externalPrefix="")
{
  printf("Installing package %s (%s) to %s\n", package.Name.c_str(), buildType.c_str(), package.Prefix.string().c_str());

  // Downloading
  // Load package source information
  // Get distr type and url
  std::string type;
  std::string url;
  std::string sha3;
  {
    std::vector<std::string> variables;
    if (!loadVariables(package.BuildFile, {"TYPE", "URL", "SHA3", "TAG", "COMMIT"}, variables)) {
      fprintf(stderr, "ERROR, can't load DISTR_TYPE, DISTR_URL and DISTR_SHA3 from %s\n", package.BuildFile.string().c_str());
      return false;
    }

    type = std::move(variables[0]);
    url = std::move(variables[1]);
    sha3 = std::move(variables[2]);
  }

  std::filesystem::path sourceDir = context.GlobalSettings.HomeDir / ".s";
  std::filesystem::path buildDir = context.GlobalSettings.HomeDir / ".b";
  std::filesystem::path installDir = externalPrefix.empty() ? package.Prefix / "install" : externalPrefix / "install";
  bool installDirNeedCreate = externalPrefix.empty();

  // Check for already installed
  {
    std::ifstream hManifest(package.Prefix / "manifest.txt");
    if (hManifest) {
      //char *line = nullptr;
      size_t length = 0;
      //ssize_t nRead;
      auto beginPt = std::chrono::steady_clock::now();
      unsigned count = 0;
      uint64_t ms = 0;
      bool packageInstalled = true;
      bool allFilesChecked = true;
      std::string line;
      while (std::getline(hManifest, line)) {
        size_t pos = line.find('!');
        if (pos == std::string::npos || pos == 0 || line.size()-pos < 64) {
          fprintf(stderr, "WARNING: broken manifest %s\n", (package.Prefix / "manifest.txt").string().c_str());
          packageInstalled = false;
          break;
        }

        // get next file hash
        std::string relativePath = line.substr(0, pos);
        std::string expectedHash = line.substr(pos+1, 64);
        std::string hash = sha3FileHash(installDir / relativePath);
        if (hash.empty()) {
          fprintf(stderr, "WARNING: can't read package file %s\n", (installDir / relativePath).string().c_str());
          packageInstalled = false;
          break;
        }

        if (hash != expectedHash) {
          fprintf(stderr, "WARNING: file %s corrupted, need reinstall\n", (installDir / relativePath).string().c_str());
          packageInstalled = false;
          break;
        }

        count++;
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - beginPt).count();
        if (ms >= 125) {
          allFilesChecked = false;
          break;
        }
      }

      if (packageInstalled) {
        printf("Verified %u%s files in %u milliseconds\n", count, allFilesChecked ? "(all!)" : "", static_cast<unsigned>(ms));
        printf("Package %s seems to be already installed\n", package.Name.c_str());
        return true;
      }
    }
  }

  if (!removeDirectory(package.Prefix))
    return false;

  if (!package.IsBinary) {
    if (!removeDirectory(sourceDir))
      return false;
    if (!std::filesystem::create_directories(sourceDir)) {
      fprintf(stderr, "ERROR: can't create directory at %s\n", sourceDir.string().c_str());
      return false;
    }

    if (!removeDirectory(buildDir))
      return false;
    if (!std::filesystem::create_directories(buildDir)) {
      fprintf(stderr, "ERROR: can't create directory at %s\n", buildDir.string().c_str());
      return false;
    }
  }

  if (installDirNeedCreate && !std::filesystem::create_directories(installDir)) {
    fprintf(stderr, "ERROR: can't create directory at %s\n", installDir.string().c_str());
    return false;
  }

  // Install depends
  {
    std::string dependsVariable;
    if (loadSingleVariable(package.BuildFile, "DEPENDS", dependsVariable) && !dependsVariable.empty()) {
      std::vector<std::string> depends;
      // TEMPORARY!
      // TODO: correctly parse depends
      StringSplitter splitter(dependsVariable, "\r\n ");
      while (splitter.next()) {
        std::string d(splitter.get());
        // search package
        auto It = allPackages.find(d);
        if (It == allPackages.end()) {
          fprintf(stderr, "ERROR: %s depends on non-existent package %s\n", package.Name.c_str(), d.c_str());
          return false;
        }

        auto &dependPackage = It->second;
        // TODO: get version from DEPENDS
        if (!inspectPackage(context, dependPackage, std::string(), verbose))
          return false;
        if (!searchCompilers(dependPackage.Languages, context.Compilers, context.Tools, context.SystemInfo, verbose))
          return false;
        updatePackagePrefix(context, dependPackage, buildType, verbose);

        if (!install(context, allPackages, dependPackage, buildType, verbose, externalPrefix.empty() ? package.Prefix : externalPrefix))
          return false;
      }
    }
  }

  if (!downloadPackageFiles(context, package, sourceDir, installDir))
    return false;

  if (!package.IsBinary) {
    // Build
    // Prepare environment
    std::vector<std::string> env;
    prepareBuildEnvironment(env, package, context.GlobalSettings, context.SystemInfo, context.Compilers, context.Tools, buildType, verbose);

    // Run building
    printf("Build %s\n", package.Name.c_str());
    FILE* hLog = fopen((package.Prefix / "build.log").string().c_str(), "w+");
    if (!hLog) {
      fprintf(stderr, "Can't open log file %s\n", (package.Prefix / "build.log").string().c_str());
      return false;
    }

    std::string args;
    args = "set -x; set -e; source ";
    args.append(pathConvert(package.BuildFile, EPathType::Posix).string());
    args.append("; build;");
    if (!runCaptureLog(package.BuildFile.parent_path(), "bash", { "-c", args }, env, hLog, true)) {
      fprintf(hLog, "Build command for %s failed\n", package.Name.c_str());
      fprintf(stderr, "Build command for %s failed\n", package.Name.c_str());
      fclose(hLog);
      return false;
    }

    fclose(hLog);
  }

  if (externalPrefix.empty()) {
    // Create manifest
    FILE* hManifest = fopen((package.Prefix / "manifest.txt").string().c_str(), "w+");
    if (!hManifest) {
      fprintf(stderr, "Can't open manifest file %s\n", (package.Prefix / "manifest.txt").string().c_str());
      return false;
    }

    printf("Create manifest...\n");
    createManifestForDirectory(hManifest, installDir, "");
    fclose(hManifest);
  }

  // Cleanup
  if (!package.IsBinary) {
    printf("Cleanup...\n");
    if (!removeDirectory(sourceDir) || !removeDirectory(buildDir))
      return false;
  }

  return true;
}

std::filesystem::path searchPath(const std::filesystem::path& prefix, const std::filesystem::path &name)
{
  std::filesystem::path result;
  std::ifstream hManifest(prefix / "manifest.txt");
  if (hManifest) {
    size_t length = 0;
    std::string line;
    while (std::getline(hManifest, line)) {
      size_t pos = line.find('!');
      if (pos == std::string::npos || pos == 0 || line.size() - pos < 64) {
        fprintf(stderr, "WARNING: broken manifest %s\n", (prefix / "manifest.txt").string().c_str());
        return std::filesystem::path();
      }

      // get next file hash
      std::string relativePath = line.substr(0, pos);
      if (endsWith(relativePath, name.string())) {
        if (!result.empty()) {
          fprintf(stderr, "ERROR: more than one file in package\n");
          return std::filesystem::path();
        }

        result = prefix / "install" / relativePath;
      }
    }
  } else {
    fprintf(stderr, "ERROR: manifest not found, package not installed\n");
    return std::filesystem::path();
  }

  return result;
}



#ifdef WIN32
BOOL WINAPI ctrlHandler(DWORD dwCtrlType)
{
  terminateAllChildProcess();
  return FALSE;
}
#endif

int main(int argc, char **argv)
{
  {
    // some utils like cmake makes stdout buffer too much, that disables realtime output
    // fix it with setvbuf
    static char buffer[256];
    setvbuf(stdout, buffer, _IOLBF, sizeof(buffer));
  }

  std::filesystem::path sysRoot;
  std::vector<std::filesystem::path> extraPackageDirs;
  EModeTy mode = ENoMode;
  std::string packageName;
  std::string packageVersion;
  std::string toolchainSystemName;
  std::string toolchainSystemProcessor;
  std::string buildType = "Release";
  std::string buildTypeMapping = "Debug:Debug;*:Release";
  std::string fileArgument;
  std::filesystem::path outputPath;
  bool exportCmake = false;
  bool verbose = false;
  EPathType pathType = EPathType::Native;
  CContext context;

#ifdef WIN32
  SetConsoleCtrlHandler(ctrlHandler, TRUE);
#endif

  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      // compilers
      case clOptCompilerCommand : {
        if (!parseCompilerOption(ECompilerOptionType::Command, context.Compilers, optarg))
          return 1;
        break;
      }
      case clOptCompilerFlags: {
        if (!parseCompilerOption(ECompilerOptionType::Flags, context.Compilers, optarg))
          return 1;
        break;
      }
      case clOptCompilerDefinitions: {
        if (!parseCompilerOption(ECompilerOptionType::Definitions, context.Compilers, optarg))
          return 1;
        break;
      }
      case clOptSystemName :
        toolchainSystemName = optarg;
        break;
      case clOptSystemProcessor :
        toolchainSystemProcessor = optarg;
        break;
      case clOptBuildType :
        buildType = optarg;
        break;
      case clOptBuildTypeMapping :
        buildTypeMapping = optarg;
        break;
      case clOptVSInstallDir :
        context.SystemInfo.VCInstallDir = optarg;
        break;
      case clOptVCToolset :
        context.SystemInfo.VCToolSet = optarg;
        break;
      // modes
      case clOptVersion :
        printf("%s\n", CXXPM_VERSION);
        exit(0);
      case clOptPackageList :
        if (mode != ENoMode) {
          fprintf(stderr, "ERROR: mode already specified\n");
          exit(1);
        }
        mode = EPackageList;
        break;
      case clOptSearchPath :
        if (mode != ENoMode) {
          fprintf(stderr, "ERROR: mode already specified\n");
          exit(1);
        }
        mode = ESearchPath;
        packageName = optarg;
        break;
      case clOptSearchPathType : {
        pathType = pathTypeFromString(optarg);
        if (pathType == EPathType::Unknown) {
          fprintf(stderr, "ERROR: unknown path type: %s\n", optarg);
          return 1;
        }
        break;
      }
      case clOptInstall : {
        if (mode != ENoMode) {
          fprintf(stderr, "ERROR: mode already specified\n");
          exit(1);
        }
        mode = EInstall;
        packageName = optarg;
        break;
      }
      case clOptExportCMake : {
        exportCmake = true;
        outputPath = optarg;
        break;
      }
      case clOptPackageRoot :
        sysRoot = optarg;
        break;
      case clOptPackageExtraDirectory :
        extraPackageDirs.push_back(optarg);
        break;
      case clOptFile :
        fileArgument = optarg;
        break;
      case clOptVerbose :
        verbose = true;
        break;
      case ':' :
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?' :
        exit(1);
      default :
        break;
    }
  }

  context.SystemInfo.Self = whereami(argv[0]);
  if (context.SystemInfo.Self.empty()) {
    fprintf(stderr, "ERROR: can't find self cxx-pm executable\n");
    return 1;
  }
#ifdef WIN32
  // Search msys2 bundle
  std::filesystem::path bashPath = context.SystemInfo.Self.parent_path() / "usr" / "bin" / "bash.exe";
  if (!std::filesystem::exists(bashPath)) {
    bashPath = userHomeDir() / ".cxxpm" / "self" / "usr" / "bin" / "bash.exe";
    if (!std::filesystem::exists(bashPath)) {
      fprintf(stderr, "ERROR: msys2 bundle not found, installation error\n");
      return 1;
    }
  }

  // Add msys2 bin directory to path
  context.SystemInfo.MSys2Path = bashPath.parent_path();
  DWORD size = GetEnvironmentVariableW(L"PATH", NULL, 0);
  std::wstring path;
  path.resize(size - 1);
  GetEnvironmentVariableW(L"PATH", path.data(), size);
  path.push_back(';');
  path.append(bashPath.parent_path().c_str());
  SetEnvironmentVariableW(L"PATH", path.c_str());
  updatePath();
#endif

  if (sysRoot.empty())
    sysRoot = userHomeDir() / ".cxxpm" / "self";

  if (!std::filesystem::exists(sysRoot)) {
    fprintf(stderr, "ERROR: path not exists: %s\n", sysRoot.string().c_str());
    exit(1);
  }

  if (!std::filesystem::exists(sysRoot / "packages")) {
    fprintf(stderr, "ERROR: path not exists: %s\n", (sysRoot/"packages").string().c_str());
    exit(1);
  }

  // Initialize
  // Paths
  context.GlobalSettings.PackageRoot = sysRoot;
  context.GlobalSettings.HomeDir = userHomeDir() / ".cxxpm";
  context.GlobalSettings.DistrDir = context.GlobalSettings.HomeDir / "distr";
  // Toolchain data
  context.SystemInfo.HostSystemName = osGetSystemName();
  context.SystemInfo.HostSystemProcessor = osGetSystemProcessor();
  if (context.SystemInfo.HostSystemName.empty() || context.SystemInfo.HostSystemProcessor.empty()) {
    fprintf(stderr, "ERROR: can't detect system name and/or system processor architecture\n");
    exit(1);
  }

  context.SystemInfo.TargetSystemName = toolchainSystemName.empty() ? context.SystemInfo.HostSystemName : toolchainSystemName;
  context.SystemInfo.TargetSystemProcessor = toolchainSystemProcessor.empty() ? context.SystemInfo.HostSystemProcessor : systemProcessorNormalize(toolchainSystemProcessor);

  if (!doBuildTypeMapping(buildType, buildTypeMapping, context.SystemInfo.BuildType))
    return 1;

  std::filesystem::create_directories(context.GlobalSettings.HomeDir);
  std::filesystem::create_directories(context.GlobalSettings.DistrDir);

  // Load all packages
  std::map<std::string, CPackage> packages;
  std::set<std::filesystem::path> visited;
  for (const auto &folder: std::filesystem::directory_iterator{sysRoot / "packages"}) {
    CPackage package;
    package.Name = folder.path().filename().string();
    package.Path = folder.path();
    packages.insert(std::make_pair(package.Name, package));
  }

  for (const auto &extraPackageDir: extraPackageDirs) {
    if (!visited.insert(extraPackageDir).second) {
      fprintf(stderr, "ERROR: extra package directory %s specified twice\n", extraPackageDir.string().c_str());
      exit(1);
    }
    for (const auto &folder: std::filesystem::directory_iterator{sysRoot / "packages"}) {
      auto It = packages.find(folder.path().filename().string());
      if (It == packages.end()) {
        // New package found
        CPackage package;
        package.Name = folder.path().filename().string();
        package.Path = folder.path();
        packages.insert(std::make_pair(package.Name, package));
      } else {
        // Already known package, check version intersection
        It->second.ExtraPath.push_back(folder.path());
      }
    }
  }

  switch (mode) {
    case ENoMode : {
      fprintf(stderr, "You must specify mode, see --help\n");
      exit(1);
    }
    case EPackageList : {
      break;
    }
    case ESearchPath : {
      if (context.SystemInfo.BuildType.size() != 1) {
        fprintf(stderr, "ERROR: search path mode supports only single build type\n");
        return 1;
      }

      std::string buildType = context.SystemInfo.BuildType[0].MappedTo;
      const auto It = packages.find(packageName);
      if (It == packages.end()) {
        fprintf(stderr, "ERROR: unknown package: %s\n", packageName.c_str());
        return 1;
      }

      CPackage &package = It->second;
      if (!inspectPackage(context, package, packageVersion, verbose))
        return 1;
      if (!searchCompilers(package.Languages, context.Compilers, context.Tools, context.SystemInfo, verbose))
        return 1;
      updatePackagePrefix(context, package, buildType, verbose);

      if (!fileArgument.empty()) {
        auto path = searchPath(package.Prefix, std::filesystem::path(fileArgument).make_preferred());
        if (!path.empty()) {
          printf("%s\n", pathConvert(path, pathType).string().c_str());
        } else {
          fprintf(stderr, "ERROR: no file %s in package %s\n", fileArgument.c_str(), packageName.c_str());
          exit(1);
        }
      } else {
        printf("%s\n", pathConvert(package.Prefix, pathType).string().c_str());
      }
      break;
    }
    case EInstall : {
      const auto It = packages.find(packageName);
      if (It == packages.end()) {
        fprintf(stderr, "ERROR: unknown package: %s\n", packageName.c_str());
        exit(1);
      }

      CPackage &package = It->second;
      if (!inspectPackage(context, package, packageVersion, verbose))
        return 1;
      if (!searchCompilers(package.Languages, context.Compilers, context.Tools, context.SystemInfo, verbose))
        return 1;

      std::vector<std::string> buildTypes;
      uniqueBuildTypes(context.SystemInfo.BuildType, buildTypes);

      for (const auto &buildType: buildTypes) {
        updatePackagePrefix(context, package, buildType, verbose);
        if (!install(context, packages, package, buildType, verbose))
          return 1;
      }

      // CMake export
      if (exportCmake) {
        if (!cmakeExport(package, context.GlobalSettings, context.Compilers, context.Tools, context.SystemInfo, outputPath, verbose))
          return 1;
      }
      break;
    }
  }

  return 0;
}
