// Copyright (c) 2022 Marcin Zdun
// This code is licensed under MIT license (see LICENSE for details)

#pragma once

#include <args/parser.hpp>
#include <cov/app/cov_tr.hh>
#include <filesystem>
#include <git2/config.hh>
#include <set>
#include <span>

namespace cov::app {
	struct builtin_tool {
		std::string_view name;
		int (*tool)(std::string_view tool, args::arglist args);
	};

	class tools {
	public:
		tools(git::config&& cfg, std::span<builtin_tool const> const& builtins)
		    : cfg_{std::move(cfg)}, builtins_{builtins} {}

		static std::filesystem::path get_sysroot();
		static std::filesystem::path get_locale_dir();
		static git::config cautiously_open_config(
		    std::filesystem::path const& current_directory);

		int handle(std::string_view tool,
		           std::string& aliased,
		           args::arglist args,
		           std::filesystem::path const& sysroot,
		           CovStrings const& tr) const;

		std::set<std::string> list_aliases() const;
		std::set<std::string> list_builtins() const;
		std::set<std::string> list_externals(
		    std::filesystem::path const& sysroot) const;

		std::set<std::string> list_tools(
		    std::string_view groups,
		    std::filesystem::path const& sysroot) const;

	private:
		git::config cfg_;
		std::span<builtin_tool const> builtins_;
	};

	namespace platform {
		int run_tool(std::filesystem::path const& tooldir,
		             std::string_view tool,
		             args::arglist args);
	}  // namespace platform
}  // namespace cov::app
