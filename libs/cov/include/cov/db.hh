// Copyright (c) 2022 Marcin Zdun
// This code is licensed under MIT license (see LICENSE for details)

#pragma once
#include <cov/counted.hh>
#include <cov/io/db_object.hh>
#include <filesystem>

namespace cov {
	struct backend : public counted {
		template <typename Object>
		ref_ptr<Object> lookup(git_oid const& id) {
			return as_a<Object>(lookup_object(id));
		}
		template <typename Object>
		ref_ptr<Object> lookup(git_oid const& id, size_t character_count) {
			return as_a<Object>(lookup_object(id, character_count));
		}
		virtual bool write(git_oid&, ref_ptr<object> const&) = 0;

		static ref_ptr<backend> loose_backend(std::filesystem::path const&);

	private:
		friend struct repository;
		virtual ref_ptr<object> lookup_object(git_oid const& id) const = 0;
		virtual ref_ptr<object> lookup_object(git_oid const& id,
		                                      size_t character_count) const = 0;
	};
}  // namespace cov
