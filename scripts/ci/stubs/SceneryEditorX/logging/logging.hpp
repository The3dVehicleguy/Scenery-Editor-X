#pragma once
/**
 * Stand-in for the engine logger (source/SceneryEditorX/logging/logging.hpp, which needs spdlog and xMath) in
 * scripts/ci/compile_check.sh, and only there: the macros take their arguments and do nothing. The editor build checks
 * the real ones. No std::format here: Apple's libc++ limits it to newer macOS versions than the build targets.
 */
#include <string_view>

namespace SceneryEditorX::CompileCheck
{
	template <typename... Args>
	inline void Log(std::string_view /*tag*/, std::string_view /*format*/, const Args &.../*args*/) {}
}

#define SEDX_CORE_INFO_TAG(tag, ...) ::SceneryEditorX::CompileCheck::Log(tag, __VA_ARGS__)
#define SEDX_CORE_WARN_TAG(tag, ...) ::SceneryEditorX::CompileCheck::Log(tag, __VA_ARGS__)
#define SEDX_CORE_ERROR_TAG(tag, ...) ::SceneryEditorX::CompileCheck::Log(tag, __VA_ARGS__)
#define SEDX_CORE_FATAL_TAG(tag, ...) ::SceneryEditorX::CompileCheck::Log(tag, __VA_ARGS__)
