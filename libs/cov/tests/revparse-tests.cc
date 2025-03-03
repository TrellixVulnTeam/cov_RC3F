// Copyright (c) 2022 Marcin Zdun
// This code is licensed under MIT license (see LICENSE for details)

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <cov/format.hh>
#include <cov/report.hh>
#include <cov/repository.hh>
#include <cov/revparse.hh>
#include <git2/global.hh>
#include "path-utils.hh"
#include "print-view.hh"

namespace cov::testing {
	using namespace ::std::literals;
	static cov::repository repo{};

	bool report(git_oid& object_id,
	            git_oid const& parent,
	            std::string_view tag) {
		using namespace std::chrono;
		auto const johnny = cov::report::signature_view{
		    .name = "Johnny Appleseed"sv,
		    .email = "johnny.applessed@example.com"sv};
		auto const now = floor<seconds>(system_clock::now());
		auto obj = cov::report::create(parent, {}, {}, "main"sv, johnny, johnny,
		                               tag, now, now, {});
		if (!repo.write(object_id, obj)) return false;

		repo.refs()->create(fmt::format("refs/tags/{}"sv, tag), object_id);
		return true;
	}

	struct rev_pair {
		std::string_view inaccessible{};
		std::string_view accessible{};
		bool single{false};

		friend std::ostream& operator<<(std::ostream& out,
		                                rev_pair const& range) {
			out << range.inaccessible;
			if (!range.single) out << ".."sv << range.accessible;
			return out;
		}
	};

	struct rev_test {
		std::string_view input{};
		rev_pair expected{};

		friend std::ostream& operator<<(std::ostream& out,
		                                rev_test const& test) {
			return out << test.input << " => "sv << test.expected;
		}
	};

	class revparse : public ::testing::TestWithParam<rev_test> {
	protected:
		static void SetUpTestSuite() {
			static git::init init{};
			std::error_code ec{};

			path_info::op(
			    make_setup(
			        remove_all("revparse"sv),
			        create_directories("revparse/subdir"sv),
			        create_directories("revparse/.git/objects/pack"sv),
			        create_directories("revparse/.git/objects/info"sv),
			        create_directories("revparse/.git/refs/tags"sv),
			        touch("revparse/.git/HEAD"sv),
			        touch("revparse/.git/config"sv),
			        touch("revparse/.git/refs/heads/main"sv),
			        init_repo("revparse/.git/.covdata"sv, "revparse/.git/"sv)),
			    ec);
			ASSERT_FALSE(ec) << "   Error: " << ec.message() << " ("
			                 << ec.category().name() << ')';

			auto local = cov::repository::open(
			    setup::test_dir(),
			    setup::test_dir() / "revparse/.git/.covdata"sv, ec);
			// repository::repository(repository&&)
			auto secondary = std::move(local);
			// repository& repository::operator=(repository&&)
			repo = std::move(secondary);

			/*
			    A                   K
			     \                 /
			      B       I       L
			       \     /       /
			        C   J       M
			         \ /       /
			          D*      N
			           \     /
			            E   O          P
			             \ /           |
			              F            Q
			              |            |
			              G            R
			              |            |
			              H            S
			*/
			git_oid prev{}, F{}, D{};
			ASSERT_TRUE(report(prev, prev, "H"sv));
			ASSERT_TRUE(report(prev, prev, "G"sv));
			ASSERT_TRUE(report(F, prev, "F"sv));

			ASSERT_TRUE(report(prev, F, "O"sv));
			ASSERT_TRUE(report(prev, prev, "N"sv));
			ASSERT_TRUE(report(prev, prev, "M"sv));
			ASSERT_TRUE(report(prev, prev, "L"sv));
			ASSERT_TRUE(report(prev, prev, "K"sv));

			ASSERT_TRUE(report(prev, F, "E"sv));
			ASSERT_TRUE(report(D, prev, "D"sv));

			ASSERT_TRUE(report(prev, D, "J"sv));
			ASSERT_TRUE(report(prev, prev, "I"sv));

			ASSERT_TRUE(report(prev, D, "C"sv));
			ASSERT_TRUE(report(prev, prev, "B"sv));
			ASSERT_TRUE(report(prev, prev, "A"sv));

			ASSERT_TRUE(report(prev, {}, "S"sv));
			ASSERT_TRUE(report(prev, prev, "R"sv));
			ASSERT_TRUE(report(prev, prev, "Q"sv));
			ASSERT_TRUE(report(prev, prev, "P"sv));

			repo.refs()->create("HEAD"sv, D);
			repo.refs()->create("refs/tags/indirect"sv, "refs/tags/N"sv);
			repo.refs()->create("refs/tags/missing"sv, "refs/tags/missing-2"sv);
		}
	};

	std::string to_string(git_oid const& id) {
		char buffer[40];
		git_oid_fmt(buffer, &id);
		return {buffer, 40};
	}

	std::string fix(std::string_view v) {
		auto pos = v.find(':');
		if (pos == std::string_view::npos) return {v.data(), v.size()};
		auto ref = repo.refs()->dwim(v.substr(0, pos));
		if (!ref || !ref->direct_target()) return "--"s;

		v = v.substr(pos + 1);

		auto result = to_string(*ref->direct_target());

		if (!v.empty()) {
			auto first = v.data();
			auto last = first + v.size();
			unsigned length{};
			auto [ptr, ec] = std::from_chars(first, last, length);
			if (ptr == last && ec == std::errc{}) {
				result = result.substr(0, length);
			}
		}

		return result;
	}

	std::string fix_pair(std::string_view v) {
		auto pos = v.find(".."sv);
		if (pos == std::string_view::npos) return fix(v);
		return fix(v.substr(0, pos)) + ".." + fix(v.substr(pos + 2));
	}

	TEST_F(revparse, partial_lookup) {
		auto ref = repo.refs()->dwim("A"sv);
		ASSERT_TRUE(ref);
		ASSERT_TRUE(ref->direct_target());

		std::error_code ec{};
		auto const sanity_check =
		    repo.lookup<cov::report>(*ref->direct_target(), ec);
		ASSERT_FALSE(ec);
		ASSERT_TRUE(sanity_check);

		char buffer[40];
		git_oid_fmt(buffer, ref->direct_target());
		auto refid = std::string_view{buffer, 40};

		while (refid.length() > 3) {
			ASSERT_TRUE(is_a<cov::report>(repo.find_partial(refid)));
			refid = refid.substr(0, refid.size() - 1);
		}
		ASSERT_FALSE(is_a<cov::report>(repo.find_partial(refid)));
	}

	struct pair {
		std::string from{}, to{};
		bool single{};
		bool operator==(pair const&) const noexcept = default;
		friend std::ostream& operator<<(std::ostream& out, pair const& p) {
			return out << "\n      :" << p.from << "\n      :" << p.to
			           << "\n      :" << std::boolalpha << p.single;
		}
	};
	TEST_P(revparse, range_lookup) {
		auto const& [input, expected_tags] = GetParam();
		pair expected{.single{expected_tags.single}}, actual{};

		// prepare expected

		if (!expected_tags.inaccessible.empty()) {
			auto const inaccessible =
			    repo.refs()->dwim(expected_tags.inaccessible);
			ASSERT_TRUE(inaccessible);
			ASSERT_TRUE(inaccessible->direct_target());
			expected.from = to_string(*inaccessible->direct_target());
		}

		if (!expected_tags.accessible.empty()) {
			auto const accessible = repo.refs()->dwim(expected_tags.accessible);
			ASSERT_TRUE(accessible);
			ASSERT_TRUE(accessible->direct_target());
			expected.to = to_string(*accessible->direct_target());
		}

		std::string range = fix_pair(input);
		revs result{};
		revs::parse(repo, range, result);

		if (!git_oid_is_zero(&result.from))
			actual.from = to_string(result.from);
		if (!git_oid_is_zero(&result.to)) actual.to = to_string(result.to);
		actual.single = result.single;

		ASSERT_EQ(expected, actual) << "Used range: " << range;
	}

	static constexpr rev_test tests[] = {
	    {"G..K"sv, {"G"sv, "K"sv}},
	    {"K..G"sv, {"G"sv, "G"sv}},
	    {"B..K"sv, {"F"sv, "K"sv}},
	    {"..K"sv, {"F"sv, "K"sv}},
	    {"K.."sv, {"F"sv, "D"sv}},
	    {"K"sv, {{}, "K"sv, true}},
	    {"E:9"sv, {{}, "E"sv, true}},
	    {"K..P"sv, {{}, "P"sv}},
	    {"H^..", {{}, "D"sv}},
	    {"..N^~2^^^~~~^0", {"D"sv}},
	    {"..N@{0}", {}},
	    {"N^@{0}..", {}},
	    {"..H^3", {}},
	    {"H^3..", {}},
	    {"..", {}},
	    {"G...K"sv, {}},
	    {"missing", {.single = true}},
	    {"G..indirect", {"G"sv, "N"sv}},
	};

	INSTANTIATE_TEST_SUITE_P(tests, revparse, ::testing::ValuesIn(tests));
}  // namespace cov::testing
