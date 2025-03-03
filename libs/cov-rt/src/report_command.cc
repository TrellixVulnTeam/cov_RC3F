// Copyright (c) 2022 Marcin Zdun
// This code is licensed under MIT license (see LICENSE for details)

#include <cov/app/path.hh>
#include <cov/app/report_command.hh>
#include <cov/app/rt_path.hh>
#include <cov/app/tools.hh>
#include <cov/format.hh>
#include <cov/io/file.hh>

namespace cov::app::builtin::report {
	using namespace std::literals;
	using namespace std::filesystem;

	namespace {
		std::string quoted(std::string_view item) {
			return fmt::format("\xE2\x80\x98{}\xE2\x80\x99", item);
		}
	}  // namespace

	parser::parser(::args::args_view const& arguments,
	               str::translator_open_info const& langs)
	    : base_parser<errlng, replng>{langs, arguments} {
		static constexpr std::string_view filters[] = {"cobertura"sv,
		                                               "coveralls"sv};

		parser_.arg(report_)
		    .meta(tr_(replng::REPORT_FILE_META))
		    .help(tr_(replng::REPORT_FILE_DESCRIPTION));
		parser_.arg(filter_, "f", "filter")
		    .meta(tr_(replng::FILTER_META))
		    .help(tr_.format(replng::FILTER_DESCRIPTION, quoted_list(filters)));
		parser_.set<std::true_type>(amend_, "amend")
		    .help(tr_(replng::AMEND_DESCRIPTION))
		    .opt();
	}

	parser::parse_results parser::parse() {
		using namespace str;

		parser_.parse();

		parse_results result{open_here(*this, tr_)};

		if (!result.report.load_from_text(report_contents(result.repo.git()))) {
			if (filter_) {
				error(tr_.format(replng::ERROR_FILTERED_REPORT_ISSUES, report_,
				                 *filter_));
			} else {  // GCOV_EXCL_LINE[WIN32]
				error(tr_.format(replng::ERROR_REPORT_ISSUES, report_));
			}
		}
		return result;
	}  // GCOV_EXCL_LINE[GCC] -- and now it wants th throw something...

	std::string parser::report_contents(git::repository_handle repo) const {
		auto source = io::fopen(make_u8path(report_));
		if (!source) error(tr_.format(str::args::lng::FILE_NOT_FOUND, report_));

		auto content = source.read();
		if (filter_) {
			auto dir = repo.workdir();
			if (!dir) dir = repo.commondir();
			content = filter(content, *filter_, make_u8path(*dir));
		}

		return {reinterpret_cast<char const*>(content.data()), content.size()};
	}

	std::vector<std::byte> parser::filter(
	    std::vector<std::byte> const& contents,
	    std::string_view filter,
	    std::filesystem::path const& cwd) const {
		auto output = platform::run_filter(
		    platform::sys_root() / directory_info::share / "filters"sv, cwd,
		    filter, contents);

		fwrite(output.error.data(), 1, output.error.size(), stderr);

		if (output.return_code) {
			if (output.error.size()) fputc('\n', stderr);
			if (output.return_code == -ENOENT)
				data_error(replng::ERROR_FILTER_NOENT, filter);
			if (output.return_code == -EACCES)
				data_error(replng::ERROR_FILTER_ACCESS, filter);
			data_error(replng::ERROR_FILTER_FAILED, filter, output.return_code);
		}

		return std::move(output.output);
	}

	new_head parser::update_current_branch(
	    cov::repository& repo,
	    git_oid const& file_list_id,
	    git_info const& git,
	    git_commit const& commit,
	    io::v1::coverage_stats const& stats) {
		new_head result{};

		git_oid commit_id;
		git_oid_fromstrn(&commit_id, git.head.data(), git.head.length());
		auto const now = std::chrono::floor<std::chrono::seconds>(
		    std::chrono::system_clock::now());

		while (true) {
			auto const HEAD = repo.current_head();
			git_oid parent_id{};
			if (HEAD.tip) parent_id = *HEAD.tip;
			if (amend_) {
				std::error_code ec{};
				auto current = HEAD.tip
				                   ? repo.lookup<cov::report>(parent_id, ec)
				                   : ref_ptr<cov::report>{};
				if (!current) {
					auto const msg = tr_(replng::ERROR_AMEND_IN_FRESH_REPO);
					error({msg.data(), msg.size()});
				}
				parent_id = current->parent_report();
			}

			auto const report_obj = cov::report::create(
			    parent_id, file_list_id, commit_id, git.branch,
			    {commit.author.name, commit.author.mail},
			    {commit.committer.name, commit.committer.mail}, commit.message,
			    sys_seconds{commit.committed}, now, stats);

			if (!repo.write(result.tip, report_obj)) {
				// GCOV_EXCL_START
				[[unlikely]];
				data_error(replng::ERROR_CANNOT_WRITE_TO_DB);
				// GCOV_EXCL_STOP
			}

			if (repo.update_current_head(result.tip, HEAD)) {
				size_t size = HEAD.branch.size();
				for (auto const c : HEAD.branch)
					if (c == '%') ++size;
				result.branch.clear();
				result.branch.reserve(size);
				for (auto const c : HEAD.branch) {
					if (c == '%') result.branch.push_back('%');
					result.branch.push_back(c);
				}
				break;
			}
		}  // GCOV_EXCL_LINE[WIN32]

		return result;
	}  // GCOV_EXCL_LINE[GCC]

	void parser::print_report(std::string_view local_branch,
	                          size_t files,
	                          cov::report const& report) {
		static constexpr auto message_chunk1 = "%C(red)["sv;
		static constexpr auto message_chunk2 = " %hr]%Creset %s%n "sv;
		static constexpr auto message_chunk3 =
		    ", %C(rating)%pP%Creset (%pC/%pR"sv;
		static constexpr auto message_chunk4 = ")%n %C(faint normal)"sv;
		static constexpr auto message_chunk5 =
		    "%Creset %C(faint yellow)%hc@%rD%Creset%n"sv;

		using str::cov_report::counted;

		auto const& stats = report.stats();

		std::string change{}, parent{};
		std::string file_count =
		    fmt::format(fmt::runtime(tr_(counted::MESSAGE_FILE_COUNT,
		                                 static_cast<intmax_t>(files))),
		                files);

		if (stats.covered < stats.relevant) {
			change = fmt::format(", -{}"sv, stats.relevant - stats.covered);
		}

		if (!git_oid_is_zero(&report.parent_report())) {
			parent = fmt::format(" {} %rp%n"sv,
			                     tr_(replng::MESSAGE_FIELD_PARENT_REPORT));
		}

		std::string_view chunks[] = {
		    message_chunk1,
		    local_branch.empty() ? tr_(replng::MESSAGE_DETACHED_HEAD)
		                         : local_branch,
		    message_chunk2,
		    file_count,
		    message_chunk3,
		    change,
		    message_chunk4,
		    tr_(replng::MESSAGE_FIELD_GIT_COMMIT),
		    message_chunk5,
		    parent,
		};

		std::string format{};
		size_t length = 0;
		for (auto chunk : chunks)
			length += chunk.size();
		format.reserve(length);
		for (auto chunk : chunks)
			format.append(chunk);

		using namespace std::chrono;
		auto const view = placeholder::report_view::from(report);
		auto const now = floor<seconds>(system_clock::now());
		auto message = formatter::from(format).format(
		    view, {.now = now,
		           .hash_length = 9,
		           .names = {},
		           .colorize = formatter::shell_colorize});

		std::fputs(message.c_str(), stdout);
	}

	std::string parser::quoted_list(std::span<std::string_view const> names) {
		auto begin = std::begin(names);
		auto end = std::next(std::end(names), -1);
		std::string result{};
		bool first = true;
		for (auto it = begin; it != end; ++it) {
			if (first)
				first = false;
			else
				result.append(", "sv);  // GCOV_EXCL_LINE
			result.append(quoted(*it));
		}

		auto last = quoted(*end);
		return tr_.format(replng::FILTER_DESCRIPTION_LIST_END, result, last);
	}

	std::vector<stored_file> stored_file::from(
	    git_commit const& commit,
	    std::vector<file_info> const& info,
	    parser const& p) {
		std::vector<stored_file> result{};
		result.reserve(info.size());

		for (auto const& file : info) {
			result.push_back(from(commit, file));

			if (result.back().stg.flags == text::missing)
				p.data_error(replng::ERROR_CANNOT_FIND_FILE, file.name);

			if ((result.back().stg.flags & text::mismatched) ==
			    text::mismatched)
				p.data_warning(replng::WARNING_FILE_MODIFIED, file.name);
		}

		return result;
	}  // GCOV_EXCL_LINE[GCC]

	void stored_file::store(cov::repository& repo,
	                        file_info const& info,
	                        parser const& p) {
		if (!store_coverage(repo, info)) {
			// GCOV_EXCL_START
			[[unlikely]];
			p.data_error(replng::ERROR_CANNOT_WRITE_TO_DB);
			// GCOV_EXCL_STOP
		}

		if ((stg.flags & text::in_repo) != text::in_repo ||
		    (stg.flags & text::mismatched) == text::mismatched) {
			auto const workdir = repo.git().workdir();
			if (!workdir) {
				// GCOV_EXCL_START
				// we got here through either in_repo | in_fs |
				// mismatched, or in_fs | mismatched; this means we got
				// here with at least in_fs; making a test, which would
				// allow this to happen and yet failed here seems
				// infeasible
				[[unlikely]];
				p.data_error(replng::ERROR_BARE_GIT);
				// GCOV_EXCL_STOP
			}

			auto const opened =
			    io::fopen(make_u8path(*workdir) / make_u8path(info.name), "r");
			if (!opened) {
				// GCOV_EXCL_START
				// same here: we read this file once already...
				[[unlikely]];
				p.data_error(replng::ERROR_CANNOT_OPEN_FILE, info.name);
				// GCOV_EXCL_STOP
			}

			auto bytes = opened.read();
			if (!store_contents(repo, {bytes.data(), bytes.size()})) {
				// GCOV_EXCL_START
				[[unlikely]];
				p.data_error(replng::ERROR_CANNOT_WRITE_TO_DB);
				// GCOV_EXCL_STOP
			}
		}
	}

	bool stored_file::store_coverage(cov::repository& repo,
	                                 file_info const& info) {
		std::vector<io::v1::coverage> cvg{};
		std::tie(cvg, stats) = info.expand_coverage(stg.lines);
		auto const obj = cov::line_coverage::create(std::move(cvg));
		return repo.write(cvg_id, obj);
	}

	bool stored_file::store_contents(cov::repository& repo,
	                                 git::bytes const& contents) {
		return repo.write(stg.existing, contents);
	}

	bool stored_file::store_tree(git_oid& id,
	                             cov::repository& repo,
	                             std::vector<file_info> const& report_files,
	                             std::vector<stored_file> const& files) {
		cov::report_files_builder builder{};
		auto it = files.begin();
		for (auto const& file : report_files) {
			auto& compiled = *it++;
			builder.add(file.name, compiled.stats, compiled.stg.existing,
			            compiled.cvg_id);
		}

		auto const obj = builder.extract();
		return repo.write(id, obj);
	}

}  // namespace cov::app::builtin::report
