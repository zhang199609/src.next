// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/common/manifest_handlers/csp_info.h"

#include <memory>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "extensions/common/csp_validator.h"
#include "extensions/common/error_utils.h"
#include "extensions/common/install_warning.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_handlers/sandboxed_page_info.h"

namespace extensions {

namespace keys = manifest_keys;
namespace errors = manifest_errors;

using csp_validator::ContentSecurityPolicyIsLegal;
using csp_validator::SanitizeContentSecurityPolicy;

namespace {

const char kDefaultContentSecurityPolicy[] =
    "script-src 'self' blob: filesystem: 'wasm-eval'; "
    "object-src 'self' blob: filesystem:; ";

// The default secure CSP to be used in order to prevent remote scripts.
const char kDefaultSecureCSP[] = "script-src 'self' blob: filesystem: 'wasm-eval'; object-src 'self';";

const char kDefaultSandboxedPageContentSecurityPolicy[] =
    "sandbox allow-scripts allow-forms allow-popups allow-modals; "
    "script-src 'self' 'unsafe-inline' 'unsafe-eval'; child-src 'self';";

#define PLATFORM_APP_LOCAL_CSP_SOURCES "'self' blob: filesystem: data:"

// clang-format off
const char kDefaultPlatformAppContentSecurityPolicy[] =
    // Platform apps can only use local resources by default.
    "default-src 'self' blob: filesystem:;"
    // For remote resources, they can fetch them via XMLHttpRequest.
    " connect-src * data: blob: filesystem:;"
    // And serve them via data: or same-origin (blob:, filesystem:) URLs
    " style-src " PLATFORM_APP_LOCAL_CSP_SOURCES " 'unsafe-inline';"
    " img-src " PLATFORM_APP_LOCAL_CSP_SOURCES ";"
    " frame-src " PLATFORM_APP_LOCAL_CSP_SOURCES ";"
    " font-src " PLATFORM_APP_LOCAL_CSP_SOURCES ";"
    // Media can be loaded from remote resources since:
    // 1. <video> and <audio> have good fallback behavior when offline or under
    //    spotty connectivity.
    // 2. Fetching via XHR and serving via blob: URLs currently does not allow
    //    streaming or partial buffering.
    " media-src * data: blob: filesystem:;"
    // Scripts are allowed to use WebAssembly
    " script-src 'self' blob: filesystem: 'wasm-eval';";
// clang-format on

int GetValidatorOptions(Extension* extension) {
  int options = csp_validator::OPTIONS_NONE;

  // crbug.com/146487
  if (extension->GetType() == Manifest::TYPE_EXTENSION ||
      extension->GetType() == Manifest::TYPE_LEGACY_PACKAGED_APP) {
    options |= csp_validator::OPTIONS_ALLOW_UNSAFE_EVAL;
  }

  // Component extensions can specify an insecure object-src directive. This
  // should be safe because non-NPAPI plugins should load in a sandboxed process
  // and only allow communication via postMessage.
  if (extensions::Manifest::IsComponentLocation(extension->location()))
    options |= csp_validator::OPTIONS_ALLOW_INSECURE_OBJECT_SRC;

  return options;
}

std::u16string GetInvalidManifestKeyError(base::StringPiece key) {
  return ErrorUtils::FormatErrorMessageUTF16(errors::kInvalidManifestKey, key);
}

// Returns null if the manifest type can't access the path. Else returns the
// corresponding Value.
const base::Value* GetManifestPath(const Extension* extension,
                                   const char* path) {
  const base::Value* value = nullptr;
  return extension->manifest()->Get(path, &value) ? value : nullptr;
}

const char* GetDefaultExtensionPagesCSP(Extension* extension,
                                        bool secure_only) {
  if (secure_only)
    return kDefaultSecureCSP;

  if (extension->GetType() == Manifest::TYPE_PLATFORM_APP)
    return kDefaultPlatformAppContentSecurityPolicy;

  return kDefaultContentSecurityPolicy;
}

}  // namespace

CSPInfo::CSPInfo(std::string extension_pages_csp)
    : extension_pages_csp(std::move(extension_pages_csp)) {}

CSPInfo::~CSPInfo() {
}

// static
const std::string& CSPInfo::GetExtensionPagesCSP(const Extension* extension) {
  CSPInfo* csp_info = static_cast<CSPInfo*>(
          extension->GetManifestData(keys::kContentSecurityPolicy));
  return csp_info ? csp_info->extension_pages_csp : base::EmptyString();
}

// static
const std::string* CSPInfo::GetDefaultCSPToAppend(
    const Extension& extension,
    const std::string& relative_path) {
  if (!extension.is_extension() || extension.manifest_version() < 3)
    return nullptr;

  // TODO(karandeepb): Extend this to Sandboxed pages as well in the future to
  // ensure an extension can't use its Service worker to modify the sandboxed
  // page CSP and get around the corresponding CSP restrictions.
  if (SandboxedPageInfo::IsSandboxedPage(&extension, relative_path))
    return nullptr;

  // We only enforce a default CSP currently for MV3 extension pages.
  static const base::NoDestructor<std::string> default_csp(kDefaultSecureCSP);
  return default_csp.get();
}

// static
const std::string* CSPInfo::GetIsolatedWorldCSP(const Extension& extension) {
  if (extension.manifest_version() >= 3) {
    // The isolated world will use its own CSP which blocks remotely hosted
    // code.
    static const base::NoDestructor<std::string> default_isolated_world_csp(
        kDefaultSecureCSP);
    return default_isolated_world_csp.get();
  }

  Manifest::Type type = extension.GetType();
  bool bypass_main_world_csp = type == Manifest::TYPE_PLATFORM_APP ||
                               type == Manifest::TYPE_EXTENSION ||
                               type == Manifest::TYPE_LEGACY_PACKAGED_APP;
  if (!bypass_main_world_csp) {
    // The isolated world will use the main world CSP.
    return nullptr;
  }

  // The isolated world will bypass the main world CSP.
  return &base::EmptyString();
}

// static
const std::string& CSPInfo::GetSandboxContentSecurityPolicy(
    const Extension* extension) {
  CSPInfo* csp_info = static_cast<CSPInfo*>(
      extension->GetManifestData(keys::kContentSecurityPolicy));
  return csp_info ? csp_info->sandbox_csp : base::EmptyString();
}

// static
const std::string& CSPInfo::GetResourceContentSecurityPolicy(
    const Extension* extension,
    const std::string& relative_path) {
  return SandboxedPageInfo::IsSandboxedPage(extension, relative_path)
             ? GetSandboxContentSecurityPolicy(extension)
             : GetExtensionPagesCSP(extension);
}

CSPHandler::CSPHandler() = default;

CSPHandler::~CSPHandler() = default;

bool CSPHandler::Parse(Extension* extension, std::u16string* error) {
  const char* key = extension->GetType() == Manifest::TYPE_PLATFORM_APP
                        ? keys::kPlatformAppContentSecurityPolicy
                        : keys::kContentSecurityPolicy;

  // The "content_security_policy" manifest key can either be a string or a
  // dictionary of the format.
  // "content_security_policy" : {
  //     "extension_pages": "",
  //     "sandbox": "",
  //  }
  // The dictionary is supported (and mandated) for manifest v3 (and above)
  // extensions.
  const base::Value* csp = GetManifestPath(extension, key);
  bool parse_as_dictionary = extension->manifest_version() >= 3;
  if (parse_as_dictionary) {
    if (csp && !csp->is_dict()) {
      *error = GetInvalidManifestKeyError(key);
      return false;
    }
    return ParseCSPDictionary(extension, error);
  }

  if (!ParseExtensionPagesCSP(extension, error, key, false /* secure_only */,
                              csp)) {
    return false;
  }

  if (!ParseSandboxCSP(extension, error, keys::kSandboxedPagesCSP,
                       GetManifestPath(extension, keys::kSandboxedPagesCSP))) {
    return false;
  }

  return true;
}

bool CSPHandler::ParseCSPDictionary(Extension* extension,
                                    std::u16string* error) {
  // keys::kSandboxedPagesCSP shouldn't be used when using
  // keys::kContentSecurityPolicy as a dictionary.
  if (extension->manifest()->HasPath(keys::kSandboxedPagesCSP)) {
    *error = base::ASCIIToUTF16(errors::kSandboxPagesCSPKeyNotAllowed);
    return false;
  }

  return ParseExtensionPagesCSP(
             extension, error, keys::kContentSecurityPolicy_ExtensionPagesPath,
             true /* secure_only */,
             GetManifestPath(
                 extension, keys::kContentSecurityPolicy_ExtensionPagesPath)) &&
         ParseSandboxCSP(
             extension, error, keys::kContentSecurityPolicy_SandboxedPagesPath,
             GetManifestPath(extension,
                             keys::kContentSecurityPolicy_SandboxedPagesPath));
}

bool CSPHandler::ParseExtensionPagesCSP(
    Extension* extension,
    std::u16string* error,
    base::StringPiece manifest_key,
    bool secure_only,
    const base::Value* content_security_policy) {
  if (!content_security_policy) {
    return SetExtensionPagesCSP(
        extension, manifest_key, secure_only,
        GetDefaultExtensionPagesCSP(extension, secure_only));
  }

  if (!content_security_policy->is_string()) {
    *error = GetInvalidManifestKeyError(manifest_key);
    return false;
  }

  const std::string& content_security_policy_str =
      content_security_policy->GetString();
  if (!ContentSecurityPolicyIsLegal(content_security_policy_str)) {
    *error = GetInvalidManifestKeyError(manifest_key);
    return false;
  }

  if (secure_only) {
    if (!csp_validator::DoesCSPDisallowRemoteCode(content_security_policy_str,
                                                  manifest_key, error)) {
      return false;
    }
    SetExtensionPagesCSP(extension, manifest_key, secure_only,
                         content_security_policy_str);
    return true;
  }

  std::vector<InstallWarning> warnings;
  std::string sanitized_content_security_policy = SanitizeContentSecurityPolicy(
      content_security_policy_str, std::string(manifest_key),
      GetValidatorOptions(extension), &warnings);
  extension->AddInstallWarnings(std::move(warnings));

  SetExtensionPagesCSP(extension, manifest_key, secure_only,
                       std::move(sanitized_content_security_policy));
  return true;
}

bool CSPHandler::ParseSandboxCSP(Extension* extension,
                                 std::u16string* error,
                                 base::StringPiece manifest_key,
                                 const base::Value* sandbox_csp) {
  if (!sandbox_csp) {
    SetSandboxCSP(extension, kDefaultSandboxedPageContentSecurityPolicy);
    return true;
  }

  if (!sandbox_csp->is_string()) {
    *error = GetInvalidManifestKeyError(manifest_key);
    return false;
  }

  const std::string& sandbox_csp_str = sandbox_csp->GetString();
  if (!ContentSecurityPolicyIsLegal(sandbox_csp_str) ||
      !csp_validator::ContentSecurityPolicyIsSandboxed(sandbox_csp_str,
                                                       extension->GetType())) {
    *error = GetInvalidManifestKeyError(manifest_key);
    return false;
  }

  std::vector<InstallWarning> warnings;
  std::string effective_sandbox_csp =
      csp_validator::GetEffectiveSandoxedPageCSP(
          sandbox_csp_str, std::string(manifest_key), &warnings);
  SetSandboxCSP(extension, std::move(effective_sandbox_csp));
  extension->AddInstallWarnings(std::move(warnings));
  return true;
}

bool CSPHandler::SetExtensionPagesCSP(Extension* extension,
                                      base::StringPiece manifest_key,
                                      bool secure_only,
                                      std::string content_security_policy) {
  if (secure_only) {
    std::u16string error;
    DCHECK(csp_validator::DoesCSPDisallowRemoteCode(content_security_policy,
                                                    manifest_key, &error));
  } else {
    DCHECK_EQ(content_security_policy,
              SanitizeContentSecurityPolicy(
                  content_security_policy, std::string(manifest_key),
                  GetValidatorOptions(extension), nullptr));
  }

  extension->SetManifestData(
      keys::kContentSecurityPolicy,
      std::make_unique<CSPInfo>(std::move(content_security_policy)));
  return true;
}

void CSPHandler::SetSandboxCSP(Extension* extension, std::string sandbox_csp) {
  CHECK(csp_validator::ContentSecurityPolicyIsSandboxed(sandbox_csp,
                                                        extension->GetType()));

  // By now we must have parsed the extension page CSP.
  CSPInfo* csp_info = static_cast<CSPInfo*>(
      extension->GetManifestData(keys::kContentSecurityPolicy));
  DCHECK(csp_info);
  csp_info->sandbox_csp = std::move(sandbox_csp);
}

bool CSPHandler::AlwaysParseForType(Manifest::Type type) const {
  // TODO(crbug.com/1005978): Check if TYPE_USER_SCRIPT needs to be included
  // here.
  return type == Manifest::TYPE_PLATFORM_APP ||
         type == Manifest::TYPE_EXTENSION ||
         type == Manifest::TYPE_LEGACY_PACKAGED_APP;
}

base::span<const char* const> CSPHandler::Keys() const {
  static constexpr const char* kKeys[] = {
      keys::kContentSecurityPolicy, keys::kPlatformAppContentSecurityPolicy,
      keys::kSandboxedPagesCSP};
  return kKeys;
}

}  // namespace extensions
