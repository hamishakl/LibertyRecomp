#include "gta4_source_inspector.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cctype>
#include <cstring>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/literals.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/util/xex2_info.h>

#include "TinySHA1.hpp"

namespace gta4::install {
namespace {

using namespace rex::literals;

constexpr uint32_t kGta4TitleId = 0x545407F2;
// PAL fork: the supported disc is the PAL retail release (base version 7) + Title Update 5.
// See docs/PAL-PORT.md and tools/xex/README.md for how these were measured.
constexpr uint32_t kGta4UsaMediaId = 0x7CF4679F;  // PAL retail (name kept to limit churn)
constexpr uint32_t kRequiredRegion = rex::XEX_REGION_PAL;
// Derived from the PAL TU5 XEXP delta descriptor's source_version_value.
constexpr uint32_t kRequiredBaseVersion = 0x00000007;
// Full-file XXH3-64 recorded by the official Liberty installer for the GTA IV
// USA retail 1.00 default.xex. This complements the XEXP signature digest:
// retaining a valid header/signature is not sufficient if the XEX body changed.
constexpr uint64_t kRequiredBaseXexXxh3 = 15674128280634689956ULL;  // PAL retail default.xex
// SHA-1 of the 0x100-byte RSA signature required by the pinned v8 XEXP's
// digest_source. Derived from the payload, not from a patched executable.
// PAL: equals the TU5 XEXP digest_source.
constexpr std::array<uint8_t, 20> kRequiredRsaSignatureSha1 = {
    0x24, 0xBD, 0xC3, 0xD4, 0xD6, 0xB1, 0x87, 0x0E, 0x15, 0x61,
    0x37, 0x80, 0xE4, 0x3E, 0x8F, 0x7D, 0xA1, 0x8A, 0x87, 0xB9,
};
constexpr size_t kMaximumMetadataFileSize = 64_MiB;
constexpr size_t kMaximumTreeEntries = 500000;
constexpr size_t kMaximumOptionalHeaders = 4096;

struct FileDestroyer {
  void operator()(rex::filesystem::File* file) const {
    if (file) {
      file->Destroy();
    }
  }
};
using FilePtr = std::unique_ptr<rex::filesystem::File, FileDestroyer>;

struct MountedSource {
  std::unique_ptr<rex::filesystem::Device> device;
  rex::filesystem::Entry* root = nullptr;
  std::optional<uint32_t> package_title_id;
  GameSourceKind kind = GameSourceKind::kUnknown;
};

struct EntrySearchResult {
  std::vector<rex::filesystem::Entry*> matches;
  std::string error;
};

std::string Lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

bool IsDirectory(const rex::filesystem::Entry* entry) {
  return entry && (entry->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
}

bool IsSafeComponent(std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos) {
    return false;
  }
  const auto component = rex::to_path(name);
  return !component.empty() && !component.is_absolute() && !component.has_root_path();
}

GameSourceInspection Rejected(GameSourceStatus status, GameSourceKind kind, std::string reason) {
  GameSourceInspection result;
  result.status = status;
  result.source_kind = kind;
  result.rejection_reason = std::move(reason);
  return result;
}

void LogInspection(const GameSourceInspection& result) {
  REXLOG_INFO(
      "Installer source inspection: kind={} status={} title_id={:08X} media_id={:08X} "
      "xex_version={} base_version={} region={:08X} disc={}/{}",
      GameSourceKindName(result.source_kind), GameSourceStatusName(result.status), result.title_id,
      result.media_id, FormatXexVersion(result.xex_version), FormatXexVersion(result.base_version),
      result.region, result.disc_number, result.disc_count);
}

std::optional<MountedSource> MountSource(const std::filesystem::path& path,
                                         GameSourceInspection& failure) {
  std::error_code fs_error;
  if (std::filesystem::is_directory(path, fs_error)) {
    MountedSource source;
    source.kind = GameSourceKind::kExtractedFolder;
    source.device = std::make_unique<rex::filesystem::HostPathDevice>("inspect:", path, true);
    if (!source.device->Initialize()) {
      failure = Rejected(GameSourceStatus::kCorruptImage, source.kind,
                         "The selected folder could not be mounted read-only.");
      return std::nullopt;
    }
    source.root = source.device->ResolvePath("");
    if (!source.root) {
      failure = Rejected(GameSourceStatus::kCorruptImage, source.kind,
                         "The selected folder has no readable root directory.");
      return std::nullopt;
    }
    return source;
  }

  fs_error.clear();
  if (!std::filesystem::is_regular_file(path, fs_error)) {
    failure = Rejected(GameSourceStatus::kCorruptImage, GameSourceKind::kUnknown,
                       "The selected base-game source does not exist or is not a regular file.");
    return std::nullopt;
  }

  if (Lowercase(path.extension().string()) == ".iso") {
    MountedSource source;
    source.kind = GameSourceKind::kDiscImage;
    source.device = std::make_unique<rex::filesystem::DiscImageDevice>("inspect:", path);
    if (!source.device->Initialize()) {
      failure = Rejected(GameSourceStatus::kCorruptImage, source.kind,
                         "The selected file is not a readable Xbox 360 GDFX disc image.");
      return std::nullopt;
    }
    source.root = source.device->ResolvePath("");
    if (!source.root) {
      failure = Rejected(GameSourceStatus::kCorruptImage, source.kind,
                         "The Xbox 360 disc image has no readable root directory.");
      return std::nullopt;
    }
    return source;
  }

  auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
  if (!header) {
    failure =
        Rejected(GameSourceStatus::kCorruptImage, GameSourceKind::kUnknown,
                 "The selected file is neither an Xbox 360 ISO nor a valid STFS/SVOD package.");
    return std::nullopt;
  }

  MountedSource source;
  source.kind = GameSourceKind::kStfsPackage;
  source.package_title_id = header->metadata.execution_info.title_id;
  source.device = std::make_unique<rex::filesystem::StfsContainerDevice>("inspect:", path, false);
  if (!source.device->Initialize()) {
    failure = Rejected(GameSourceStatus::kCorruptImage, source.kind,
                       "The Xbox content package is corrupt or unsupported.");
    return std::nullopt;
  }
  source.root = source.device->ResolvePath("");
  if (!source.root) {
    failure = Rejected(GameSourceStatus::kCorruptImage, source.kind,
                       "The Xbox content package has no readable root directory.");
    return std::nullopt;
  }
  return source;
}

EntrySearchResult FindDefaultXexEntries(rex::filesystem::Entry* root) {
  EntrySearchResult result;
  if (!root) {
    result.error = "The selected source has no readable filesystem root.";
    return result;
  }

  std::vector<rex::filesystem::Entry*> pending{root};
  size_t visited = 0;
  while (!pending.empty()) {
    rex::filesystem::Entry* entry = pending.back();
    pending.pop_back();
    if (++visited > kMaximumTreeEntries) {
      result.error = "The selected source contains an unexpectedly large directory tree.";
      return result;
    }
    if (entry != root && Lowercase(entry->name()) == "default.xex" && !IsDirectory(entry)) {
      result.matches.push_back(entry);
    }
    if (!IsDirectory(entry)) {
      continue;
    }
    for (const auto& child : entry->children()) {
      if (!IsSafeComponent(child->name())) {
        result.error = "The selected source contains an unsafe path component.";
        return result;
      }
      pending.push_back(child.get());
    }
  }
  return result;
}

bool ReadEntryBytes(rex::filesystem::Entry* entry, std::vector<uint8_t>& bytes,
                    std::string& error) {
  if (!entry || IsDirectory(entry)) {
    error = "The selected default.xex entry is not a file.";
    return false;
  }
  if (entry->size() > kMaximumMetadataFileSize) {
    error = "default.xex is unexpectedly large.";
    return false;
  }

  rex::filesystem::File* raw_file = nullptr;
  const rex::X_STATUS open_status =
      entry->Open(rex::filesystem::FileAccess::kGenericRead, &raw_file);
  FilePtr file(raw_file);
  if (open_status != 0 || !file) {
    error = "default.xex could not be opened for read-only inspection.";
    return false;
  }

  bytes.resize(entry->size());
  size_t offset = 0;
  while (offset < bytes.size()) {
    size_t bytes_read = 0;
    const rex::X_STATUS read_status =
        file->ReadSync(std::span<uint8_t>(bytes).subspan(offset), offset, &bytes_read);
    if (read_status != 0 || bytes_read == 0 || bytes_read > bytes.size() - offset) {
      error = "default.xex could not be read completely.";
      return false;
    }
    offset += bytes_read;
  }
  return true;
}

bool ContainsRange(std::span<const uint8_t> bytes, size_t offset, size_t length) {
  return offset <= bytes.size() && length <= bytes.size() - offset;
}

uint32_t LoadBeU32(std::span<const uint8_t> bytes, size_t offset) {
  return rex::memory::load_and_swap<uint32_t>(bytes.data() + offset);
}

bool ParseGameXex(std::span<const uint8_t> bytes, GameSourceMetadata& metadata,
                  std::string& error) {
  constexpr size_t kHeaderPrefixSize = offsetof(rex::xex2_header, headers);
  constexpr size_t kOptionalHeaderSize = sizeof(rex::xex2_opt_header);
  constexpr size_t kSecurityPrefixSize = offsetof(rex::xex2_security_info, page_descriptors);
  constexpr size_t kRsaSignatureOffset = offsetof(rex::xex2_security_info, rsa_signature);
  constexpr size_t kRsaSignatureSize = sizeof(((rex::xex2_security_info*)nullptr)->rsa_signature);

  if (!ContainsRange(bytes, 0, kHeaderPrefixSize) || std::memcmp(bytes.data(), "XEX2", 4) != 0) {
    error = "default.xex does not have a valid XEX2 magic/header prefix.";
    return false;
  }
  if (bytes.size() > kMaximumMetadataFileSize) {
    error = "default.xex exceeds the bounded metadata inspection size.";
    return false;
  }

  const size_t header_size = LoadBeU32(bytes, offsetof(rex::xex2_header, header_size));
  const size_t header_count = LoadBeU32(bytes, offsetof(rex::xex2_header, header_count));
  if (header_size < kHeaderPrefixSize || header_size > bytes.size()) {
    error = "The XEX2 header size is outside the file bounds.";
    return false;
  }
  if (header_count > kMaximumOptionalHeaders ||
      header_count > (header_size - kHeaderPrefixSize) / kOptionalHeaderSize) {
    error = "The XEX2 optional-header count exceeds the bounded header table.";
    return false;
  }
  const size_t optional_table_end = kHeaderPrefixSize + header_count * kOptionalHeaderSize;

  metadata.module_flags = LoadBeU32(bytes, offsetof(rex::xex2_header, module_flags));
  const size_t security_offset = LoadBeU32(bytes, offsetof(rex::xex2_header, security_offset));
  if (security_offset < optional_table_end || security_offset > header_size ||
      kSecurityPrefixSize > header_size - security_offset) {
    error = "The XEX2 security information is missing, truncated, or overlaps the header table.";
    return false;
  }

  const size_t security_header_size =
      LoadBeU32(bytes, security_offset + offsetof(rex::xex2_security_info, header_size));
  const size_t page_descriptor_count =
      LoadBeU32(bytes, security_offset + offsetof(rex::xex2_security_info, page_descriptor_count));
  if (security_header_size < kSecurityPrefixSize ||
      security_header_size > header_size - security_offset ||
      page_descriptor_count >
          (security_header_size - kSecurityPrefixSize) / sizeof(rex::xex2_page_descriptor)) {
    error = "The XEX2 security information has invalid bounded descriptor data.";
    return false;
  }
  if (!ContainsRange(bytes, security_offset + kRsaSignatureOffset, kRsaSignatureSize)) {
    error = "The XEX2 RSA signature is truncated.";
    return false;
  }

  sha1::SHA1 sha;
  sha.processBytes(bytes.data() + security_offset + kRsaSignatureOffset, kRsaSignatureSize);
  sha.finalize(metadata.rsa_signature_sha1.data());
  metadata.region = LoadBeU32(bytes, security_offset + offsetof(rex::xex2_security_info, region));

  bool has_execution_info = false;
  for (size_t index = 0; index < header_count; ++index) {
    const size_t optional_offset = kHeaderPrefixSize + index * kOptionalHeaderSize;
    const uint32_t key = LoadBeU32(bytes, optional_offset + offsetof(rex::xex2_opt_header, key));
    if (key != rex::XEX_HEADER_EXECUTION_INFO) {
      continue;
    }
    if (has_execution_info) {
      error = "The XEX2 image contains duplicate execution-information headers.";
      return false;
    }
    const size_t execution_offset =
        LoadBeU32(bytes, optional_offset + offsetof(rex::xex2_opt_header, offset));
    if (execution_offset < optional_table_end || execution_offset > header_size ||
        sizeof(rex::xex2_opt_execution_info) > header_size - execution_offset) {
      error = "The XEX2 execution-information header is outside the bounded header data.";
      return false;
    }
    metadata.media_id =
        LoadBeU32(bytes, execution_offset + offsetof(rex::xex2_opt_execution_info, media_id));
    metadata.xex_version =
        LoadBeU32(bytes, execution_offset + offsetof(rex::xex2_opt_execution_info, version_value));
    metadata.base_version = LoadBeU32(
        bytes, execution_offset + offsetof(rex::xex2_opt_execution_info, base_version_value));
    metadata.title_id =
        LoadBeU32(bytes, execution_offset + offsetof(rex::xex2_opt_execution_info, title_id));
    metadata.disc_number =
        bytes[execution_offset + offsetof(rex::xex2_opt_execution_info, disc_number)];
    metadata.disc_count =
        bytes[execution_offset + offsetof(rex::xex2_opt_execution_info, disc_count)];
    has_execution_info = true;
  }
  if (!has_execution_info) {
    error = "The XEX2 image is missing its execution-information header.";
    return false;
  }
  return true;
}

GameSourceInspection WithKind(GameSourceInspection result, GameSourceKind kind) {
  result.source_kind = kind;
  return result;
}

}  // namespace

const char* GameSourceKindName(GameSourceKind kind) {
  switch (kind) {
    case GameSourceKind::kExtractedFolder:
      return "extracted-folder";
    case GameSourceKind::kDiscImage:
      return "disc-image";
    case GameSourceKind::kStfsPackage:
      return "stfs-package";
    case GameSourceKind::kUnknown:
    default:
      return "unknown";
  }
}

const char* GameSourceStatusName(GameSourceStatus status) {
  switch (status) {
    case GameSourceStatus::kSupported:
      return "supported";
    case GameSourceStatus::kWrongGame:
      return "wrong-game";
    case GameSourceStatus::kWrongMediaId:
      return "wrong-media-id";
    case GameSourceStatus::kWrongRegion:
      return "wrong-region";
    case GameSourceStatus::kWrongRevision:
      return "wrong-revision";
    case GameSourceStatus::kWrongSignature:
      return "wrong-signature";
    case GameSourceStatus::kWrongExecutable:
      return "wrong-executable";
    case GameSourceStatus::kMissingDefaultXex:
      return "missing-default-xex";
    case GameSourceStatus::kAmbiguousDefaultXex:
      return "ambiguous-default-xex";
    case GameSourceStatus::kCorruptImage:
    default:
      return "corrupt-image";
  }
}

std::string FormatXexVersion(uint32_t version_value) {
  const rex::xex2_version version{version_value};
  return fmt::format("{}.{}.{}.{}", version.major, version.minor, version.build, version.qfe);
}

std::string FormatXexRegion(uint32_t region) {
  if (region == rex::XEX_REGION_ALL) {
    return "Region Free";
  }
  std::vector<std::string_view> names;
  if ((region & rex::XEX_REGION_NTSCU) != 0) {
    names.emplace_back("USA");
  }
  if ((region & rex::XEX_REGION_NTSCJ) != 0) {
    names.emplace_back("Japan/Asia");
  }
  if ((region & rex::XEX_REGION_PAL) != 0) {
    names.emplace_back("PAL");
  }
  if ((region & rex::XEX_REGION_OTHER) != 0) {
    names.emplace_back("Other");
  }
  if (names.empty()) {
    return fmt::format("Unknown ({:08X})", region);
  }
  std::string result;
  for (std::string_view name : names) {
    if (!result.empty()) {
      result += " + ";
    }
    result += name;
  }
  return result;
}

GameSourceInspection ClassifyGameSourceMetadata(const GameSourceMetadata& metadata) {
  GameSourceInspection result;
  result.title_id = metadata.title_id;
  result.media_id = metadata.media_id;
  result.xex_version = metadata.xex_version;
  result.base_version = metadata.base_version;
  result.region = metadata.region;
  result.disc_number = metadata.disc_number;
  result.disc_count = metadata.disc_count;
  result.rsa_signature_sha1 = metadata.rsa_signature_sha1;
  result.display_name =
      metadata.title_id == kGta4TitleId ? "Grand Theft Auto IV" : "Unknown Xbox 360 title";

  if (metadata.module_flags != rex::XEX_MODULE_TITLE) {
    result.status = GameSourceStatus::kCorruptImage;
    result.rejection_reason = "default.xex does not have the required retail title-module flags.";
  } else if (metadata.title_id != kGta4TitleId) {
    result.status = GameSourceStatus::kWrongGame;
    result.rejection_reason = fmt::format("Title ID {:08X} is not Grand Theft Auto IV ({:08X}).",
                                          metadata.title_id, kGta4TitleId);
  } else if (metadata.media_id != kGta4UsaMediaId) {
    result.status = GameSourceStatus::kWrongMediaId;
    result.rejection_reason =
        fmt::format("Media ID {:08X} is not the supported PAL retail media ({:08X}).",
                    metadata.media_id, kGta4UsaMediaId);
  } else if (metadata.region != kRequiredRegion) {
    result.status = GameSourceStatus::kWrongRegion;
    result.rejection_reason = fmt::format(
        "The source is {}; exact PAL region flags ({:08X}) are required. Region-free and "
        "multi-region images are unsupported.",
        FormatXexRegion(metadata.region), kRequiredRegion);
  } else if (metadata.xex_version != kRequiredBaseVersion ||
             metadata.base_version != kRequiredBaseVersion) {
    result.status = GameSourceStatus::kWrongRevision;
    result.rejection_reason =
        fmt::format("XEX/base versions {}/{} do not match retail 1.00 ({}/{}).",
                    FormatXexVersion(metadata.xex_version), FormatXexVersion(metadata.base_version),
                    FormatXexVersion(kRequiredBaseVersion), FormatXexVersion(kRequiredBaseVersion));
  } else if (metadata.rsa_signature_sha1 != kRequiredRsaSignatureSha1) {
    result.status = GameSourceStatus::kWrongSignature;
    result.rejection_reason =
        "The XEX RSA signature does not match the PAL retail source required by Title Update 5.";
  } else {
    result.status = GameSourceStatus::kSupported;
    result.release_label = "Retail 1.00";
  }
  return result;
}

GameSourceInspection InspectGameXex(std::span<const uint8_t> bytes) {
  GameSourceMetadata metadata;
  std::string error;
  if (!ParseGameXex(bytes, metadata, error)) {
    return Rejected(GameSourceStatus::kCorruptImage, GameSourceKind::kUnknown, std::move(error));
  }
  GameSourceInspection result = ClassifyGameSourceMetadata(metadata);
  if (result.supported() && XXH3_64bits(bytes.data(), bytes.size()) != kRequiredBaseXexXxh3) {
    result.status = GameSourceStatus::kWrongExecutable;
    result.release_label.clear();
    result.rejection_reason =
        "The complete default.xex does not match GTA IV PAL retail (base version 7).";
  }
  return result;
}

GameSourceInspection InspectGameSource(const std::filesystem::path& path) {
  try {
    GameSourceInspection failure;
    auto mounted = MountSource(path, failure);
    if (!mounted) {
      LogInspection(failure);
      return failure;
    }

    EntrySearchResult search = FindDefaultXexEntries(mounted->root);
    if (!search.error.empty()) {
      failure = Rejected(GameSourceStatus::kCorruptImage, mounted->kind, std::move(search.error));
      LogInspection(failure);
      return failure;
    }
    if (search.matches.empty()) {
      failure = Rejected(GameSourceStatus::kMissingDefaultXex, mounted->kind,
                         "The source does not contain default.xex.");
      LogInspection(failure);
      return failure;
    }
    if (search.matches.size() != 1) {
      failure = Rejected(GameSourceStatus::kAmbiguousDefaultXex, mounted->kind,
                         "The source contains multiple default.xex files; select an unambiguous "
                         "base-game root.");
      LogInspection(failure);
      return failure;
    }

    std::vector<uint8_t> bytes;
    std::string read_error;
    if (!ReadEntryBytes(search.matches.front(), bytes, read_error)) {
      failure = Rejected(GameSourceStatus::kCorruptImage, mounted->kind, std::move(read_error));
      LogInspection(failure);
      return failure;
    }

    GameSourceInspection result = WithKind(InspectGameXex(bytes), mounted->kind);
    if (result.supported() && mounted->package_title_id &&
        *mounted->package_title_id != kGta4TitleId) {
      result.status = GameSourceStatus::kWrongGame;
      result.rejection_reason = fmt::format(
          "The STFS/SVOD package belongs to title {:08X}, not Grand Theft Auto IV ({:08X}).",
          *mounted->package_title_id, kGta4TitleId);
      result.release_label.clear();
    }
    LogInspection(result);
    return result;
  } catch (const std::exception&) {
    GameSourceInspection failure =
        Rejected(GameSourceStatus::kCorruptImage, GameSourceKind::kUnknown,
                 "The selected source changed or became unreadable during inspection.");
    LogInspection(failure);
    return failure;
  }
}

std::string FormatGameSourceInspection(const GameSourceInspection& inspection) {
  if (inspection.supported()) {
    return fmt::format("Detected: {} — {} — {} — Supported", inspection.display_name,
                       FormatXexRegion(inspection.region), inspection.release_label);
  }
  if (!inspection.display_name.empty()) {
    return fmt::format("Detected: {} — {} — XEX {} — Unsupported: {}", inspection.display_name,
                       FormatXexRegion(inspection.region), FormatXexVersion(inspection.xex_version),
                       inspection.rejection_reason);
  }
  return "Unsupported: " + inspection.rejection_reason;
}

std::string FormatGameSourceDiagnostics(const GameSourceInspection& inspection) {
  if (inspection.display_name.empty()) {
    return {};
  }
  return fmt::format("Title {:08X} · Media {:08X} · XEX {} · Base {} · Region {:08X} · Disc {}/{}",
                     inspection.title_id, inspection.media_id,
                     FormatXexVersion(inspection.xex_version),
                     FormatXexVersion(inspection.base_version), inspection.region,
                     inspection.disc_number, inspection.disc_count);
}

struct GameSourceInspectionWorker::Impl {
  struct RequestData {
    uint64_t generation;
    std::filesystem::path path;
  };

  explicit Impl(InspectFunction inspect_function)
      : inspect(std::move(inspect_function)), thread([this] { Run(); }) {}

  ~Impl() { Stop(); }

  void Run() {
    for (;;) {
      RequestData request;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return stop || pending.has_value(); });
        if (stop) {
          return;
        }
        request = std::move(*pending);
        pending.reset();
      }

      GameSourceInspection inspected = inspect(request.path);
      {
        std::lock_guard lock(mutex);
        if (stop) {
          return;
        }
        if (request.generation == snapshot.generation) {
          snapshot.checking = false;
          snapshot.result = std::move(inspected);
        }
      }
    }
  }

  uint64_t Request(std::filesystem::path path) {
    std::lock_guard lock(mutex);
    if (stop) {
      return snapshot.generation;
    }
    ++snapshot.generation;
    snapshot.result.reset();
    if (path.empty()) {
      snapshot.checking = false;
      pending.reset();
    } else {
      snapshot.checking = true;
      pending = RequestData{snapshot.generation, std::move(path)};
    }
    condition.notify_one();
    return snapshot.generation;
  }

  GameSourceInspectionSnapshot Snapshot() const {
    std::lock_guard lock(mutex);
    return snapshot;
  }

  void Stop() {
    {
      std::lock_guard lock(mutex);
      if (stop) {
        return;
      }
      stop = true;
      pending.reset();
      snapshot.checking = false;
      snapshot.result.reset();
    }
    condition.notify_one();
    if (thread.joinable()) {
      thread.join();
    }
  }

  InspectFunction inspect;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::optional<RequestData> pending;
  GameSourceInspectionSnapshot snapshot;
  bool stop = false;
  std::thread thread;
};

GameSourceInspectionWorker::GameSourceInspectionWorker(InspectFunction inspect) {
  if (!inspect) {
    inspect = InspectGameSource;
  }
  impl_ = std::make_unique<Impl>(std::move(inspect));
}

GameSourceInspectionWorker::~GameSourceInspectionWorker() = default;

uint64_t GameSourceInspectionWorker::Request(std::filesystem::path path) {
  return impl_->Request(std::move(path));
}

uint64_t GameSourceInspectionWorker::Clear() {
  return impl_->Request({});
}

GameSourceInspectionSnapshot GameSourceInspectionWorker::Snapshot() const {
  return impl_->Snapshot();
}

void GameSourceInspectionWorker::Stop() {
  impl_->Stop();
}

}  // namespace gta4::install
