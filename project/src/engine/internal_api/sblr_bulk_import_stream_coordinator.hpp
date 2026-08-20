#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrBulkImportStreamCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBulkImportStreamDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrBulkImportStreamCoordinationResult CompileSblrBulkImportStreamDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrBulkImportStreamCoordinationResult ConsumeSblrBulkImportStreamDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBulkImportStreamDescriptorV1&);}
