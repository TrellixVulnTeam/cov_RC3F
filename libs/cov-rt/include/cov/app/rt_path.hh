// Copyright (c) 2022 Marcin Zdun
// This code is licensed under MIT license (see LICENSE for details)

#pragma once

#include <cov/app/strings/args.hh>
#include <cov/app/strings/errors.hh>
#include <cov/repository.hh>
#include <filesystem>

namespace cov::app {
	cov::repository open_here(class parser_holder const& parser,
	                          str::errors::Strings const&,
	                          str::args::Strings const&);

	template <typename Strings>
	requires std::derived_from<Strings, str::errors::Strings> &&
	    std::derived_from<Strings, str::args::Strings>
	inline cov::repository open_here(class parser_holder const& parser,
	                                 Strings const& str) {
		return open_here(parser, str, str);
	}
}  // namespace cov::app

namespace cov::app::platform {
	std::filesystem::path const& sys_root();
	std::filesystem::path locale_dir();
}  // namespace cov::app::platform
