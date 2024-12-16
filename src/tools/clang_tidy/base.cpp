/*
 * Copyright (c) 2024 Emmett Zhang
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "tools/clang_tidy/base.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <boost/regex.hpp>
#include <spdlog/spdlog.h>
#include <tinyxml2.h>

#include "utils/env_manager.h"
#include "utils/shell.h"
#include "utils/util.h"
#include "github/common.h"
#include "github/utils.h"
#include "github/review_comment.h"

namespace linter::tool::clang_tidy {
  using namespace std::string_view_literals;

  namespace {
    constexpr auto supported_serverity = {"warning"sv, "info"sv, "error"sv};

    //Parse the header line of clang-tidy. If the given line meets header line
    //rule, parse it. Otherwise return std::nullopt.
    auto parse_diagnostic_header(std::string_view line) -> std::optional<diagnostic_header> {
      auto parts = line | std::views::split(':');

      if (std::distance(parts.begin(), parts.end()) != 5) {
        return std::nullopt;
      }
      auto iter            = parts.begin();
      auto file_name       = std::string_view{*iter++};
      auto row_idx         = std::string_view{*iter++};
      auto col_idx         = std::string_view{*iter++};
      auto serverity       = trim_left(std::string_view{*iter++});
      auto diagnostic_type = std::string_view{*iter++};

      if (!std::ranges::all_of(row_idx, ::isdigit)) {
        return std::nullopt;
      }
      if (!std::ranges::all_of(col_idx, ::isdigit)) {
        return std::nullopt;
      }
      if (!std::ranges::contains(supported_serverity, serverity)) {
        return std::nullopt;
      }

      const auto* square_brackets = std::ranges::find(diagnostic_type, '[');
      if ((square_brackets == diagnostic_type.end())
          || (diagnostic_type.size() < 3)
          || (diagnostic_type.back() != ']')) {
        return std::nullopt;
      }
      auto brief      = std::string_view{diagnostic_type.begin(), square_brackets};
      auto diagnostic = std::string_view{square_brackets, diagnostic_type.end()};

      auto header            = diagnostic_header{};
      header.file_name       = file_name;
      header.row_idx         = row_idx;
      header.col_idx         = col_idx;
      header.serverity       = serverity;
      header.brief           = brief;
      header.diagnostic_type = diagnostic;
      return header;
    }

    auto execute(const user_option& option, std::string_view repo, std::string_view file)
      -> shell::result {
      auto opts = std::vector<std::string>{};
      if (!option.database.empty()) {
        opts.emplace_back(std::format("-p={}", option.database));
      }
      if (!option.checks.empty()) {
        opts.emplace_back(std::format("-checks={}", option.checks));
      }
      if (option.allow_no_checks) {
        opts.emplace_back("--allow-no-checks");
      }
      if (!option.config.empty()) {
        opts.emplace_back(std::format("--config={}", option.config));
      }
      if (!option.config_file.empty()) {
        opts.emplace_back(std::format("--config-file={}", option.config_file));
      }
      if (option.enable_check_profile) {
        opts.emplace_back("--enable-check-profile");
      }
      if (!option.header_filter.empty()) {
        opts.emplace_back(std::format("--header-filter={}", option.header_filter));
      }
      if (!option.line_filter.empty()) {
        opts.emplace_back(std::format("--line-filter={}", option.line_filter));
      }

      opts.emplace_back(file);

      auto arg_str = opts | std::views::join_with(' ') | std::ranges::to<std::string>();
      spdlog::info("Running command: {} {}", option.binary, arg_str);

      return shell::execute(option.binary, opts, repo);
    }

    void try_match(const std::string& line, const char* regex_str, auto callback) {
      auto regex   = boost::regex{regex_str};
      auto match   = boost::smatch{};
      auto matched = boost::regex_match(line, match, regex, boost::match_extra);
      if (matched) {
        callback(match);
      }
    };

    auto parse_stdout(std::string_view std_out) -> diagnostics {
      auto diags         = diagnostics{};
      auto needs_details = false;

      for (auto part: std::views::split(std_out, '\n')) {
        auto line = std::string{part.data(), part.size()};
        spdlog::trace("Parsing: {}", line);

        auto header_line = parse_diagnostic_header(line);
        if (header_line) {
          spdlog::trace(
            " Result: {}:{}:{}: {}:{}{}",
            header_line->file_name,
            header_line->row_idx,
            header_line->col_idx,
            header_line->serverity,
            header_line->brief,
            header_line->diagnostic_type);

          diags.emplace_back(std::move(*header_line));
          needs_details = true;
          continue;
        }

        if (needs_details) {
          diags.back().details += line;
        }
      }

      spdlog::info("Parsed clang tidy stdout, got {} diagnostics.", diags.size());
      return diags;
    }

    constexpr auto warning_and_error  = "^(\\d+) warnings and (\\d+) errors? generated.";
    constexpr auto warnings_generated = "^(\\d+) warnings? generated.";
    constexpr auto errors_generated   = "^(\\d+) errors? generated.";
    constexpr auto suppressed         = R"(Suppressed (\d+) warnings \((\d+) in non-user code\)\.)";
    constexpr auto suppressed_lint =
      R"(Suppressed (\d+) warnings \((\d+) in non-user code, (\d+) NOLINT\)\.)";
    constexpr auto warnings_as_errors = "^(\\d+) warnings treated as errors";

    /// TODO: should we parse stderr?
    auto parse_stderr(std::string_view std_err) -> statistic {
      auto stat                 = statistic{};
      auto warning_and_error_cb = [&](boost::smatch& match) {
        spdlog::trace(" Result: {} warnings and {} error(s) generated.",
                      match[1].str(),
                      match[2].str());
        stat.warnings = stoi(match[1].str());
        stat.errors   = stoi(match[2].str());
      };

      auto warnings_generated_cb = [&](boost::smatch& match) {
        spdlog::trace(" Result: {} warning(s) generated.", match[1].str());
        stat.warnings = stoi(match[1].str());
      };

      auto errors_generated_cb = [&](boost::smatch& match) {
        spdlog::trace(" Result: {} error(s) generated.", match[1].str());
        stat.errors = stoi(match[1].str());
      };

      auto suppressed_cb = [&](boost::smatch& match) {
        spdlog::trace(" Result: Suppressed {} warnings ({} in non-user code).",
                      match[1].str(),
                      match[2].str());
        stat.total_suppressed_warnings = stoi(match[1].str());
        stat.non_user_code_warnings    = stoi(match[2].str());
      };

      auto suppressed_and_nolint_cb = [&](boost::smatch& match) {
        spdlog::trace(" Result: Suppressed {} warnings ({} in non-user code, {} NOLINT).",
                      match[1].str(),
                      match[2].str(),
                      match[3].str());
        stat.total_suppressed_warnings = stoi(match[1].str());
        stat.non_user_code_warnings    = stoi(match[2].str());
        stat.no_lint_warnings          = stoi(match[3].str());
      };

      auto warnings_as_errors_cb = [&](boost::smatch& match) {
        spdlog::trace(" Result: {} warnings treated as errors", match[1].str());
        stat.warnings_treated_as_errors = stoi(match[1].str());
      };

      for (auto part: std::views::split(std_err, '\n')) {
        auto line = std::string{part.data(), part.size()};
        spdlog::trace("Parsing: {}", line);
        try_match(line, warning_and_error, warning_and_error_cb);
        try_match(line, warnings_generated, warnings_generated_cb);
        try_match(line, errors_generated, errors_generated_cb);
        try_match(line, suppressed, suppressed_cb);
        try_match(line, warnings_as_errors, warnings_as_errors_cb);
        try_match(line, suppressed_lint, suppressed_and_nolint_cb);
      }

      return stat;
    }

    void print_statistic(const statistic& stat) {
      spdlog::debug("Errors: {}", stat.errors);
      spdlog::debug("Warnings: {}", stat.warnings);
      spdlog::debug("Warnings treated as errors: {}", stat.warnings_treated_as_errors);
      spdlog::debug("Total suppressed warnings: {}", stat.total_suppressed_warnings);
      spdlog::debug("Non user code warnings: {}", stat.non_user_code_warnings);
      spdlog::debug("No lint warnings: {}", stat.no_lint_warnings);
    }


  } // namespace

  auto base_clang_tidy::apply_to_single_file(
    const user_option& user_opt,
    const std::string& repo,
    const std::string& file) -> per_file_result {
    spdlog::info("Start to run clang-tidy");
    auto [ec, std_out, std_err] = execute(user_opt, repo, file);
    spdlog::trace("clang-tidy original output:\nreturn code: {}\nstdout:\n{}stderr:\n{}",
                  ec,
                  std_out,
                  std_err);

    spdlog::info("Successfully ran clang-tidy, now start to parse the output of it.");
    auto result        = per_file_result{};
    result.passed      = ec == 0;
    result.diags       = parse_stdout(std_out);
    result.tool_stdout = std_out;
    result.tool_stderr = std_err;
    result.file_path   = file;

    if (result.passed) {
      spdlog::info("The final result of ran clang-tidy on {} is: {}, detailed information:\n{}",
                   file,
                   "PASS",
                   result.tool_stderr);
    } else {
      spdlog::error("The final result of ran clang-tidy on {} is: {} , detailed information:\n{}",
                    file,
                    "FAIL",
                    result.tool_stderr);
    }
    return result;
  }

  auto base_clang_tidy::make_issue_comment(const user_option& option, const final_result_t& result)
    -> std::string {
    auto res  = std::string{};
    res      += std::format("<details>\n<summary>{} reports:<strong>{} fails</strong></summary>\n",
                       option.binary,
                       result.fails.size());
    for (const auto& [name, failed]: result.fails) {
      for (const auto& diag: failed.diags) {
        auto one = std::format(
          "- **{}:{}:{}:** {}: [{}]\n  > {}\n",
          diag.header.file_name,
          diag.header.row_idx,
          diag.header.col_idx,
          diag.header.serverity,
          diag.header.diagnostic_type,
          diag.header.brief);
        res += one;
      }
    }
    return res;
  }

  auto base_clang_tidy::make_step_summary(const user_option& option, const final_result_t& result)
    -> std::string {
    return {};
  }

  auto base_clang_tidy::make_pr_review_comment(
    [[maybe_unused]] const user_option& option,
    const final_result_t& result) -> github::pull_request::review_comments {
    auto comments = github::pull_request::review_comments{};

    for (const auto& [file, per_file_result]: result.fails) {
      // Get the same file's delta and clang-tidy result
      assert(per_file_result.file_path == file);
      assert(result.patches.contains(file));

      const auto& patch = result.patches.at(file);

      for (const auto& diag: per_file_result.diags) {
        const auto& header = diag.header;
        auto row           = std::stoi(header.row_idx);
        auto col           = std::stoi(header.col_idx);

        // For all clang-tidy result, check is this in hunk.
        auto pos      = std::size_t{0};
        auto num_hunk = git::patch::num_hunks(patch.get());
        for (int i = 0; i < num_hunk; ++i) {
          auto [hunk, num_lines] = git::patch::get_hunk(patch.get(), i);
          if (!github::is_row_in_hunk(hunk, row)) {
            pos += num_lines;
            continue;
          }
          auto comment     = github::pull_request::review_comment{};
          comment.path     = file;
          comment.position = pos + row - hunk.new_start + 1;
          comment.body     = diag.header.brief + diag.header.diagnostic_type;
          comments.emplace_back(std::move(comment));
        }
      }
    }
    return comments;
  }

  auto base_clang_tidy::write_to_action_output() -> void {
    //
    auto output = env::get(github_output);
    auto file   = std::fstream{output, std::ios::app};
    throw_unless(file.is_open(), "error to open output file to write");

    const auto clang_tidy_failed   = result.clang_tidy_failed.size();
    const auto clang_format_failed = result.clang_format_failed.size();
    const auto total_failed        = clang_tidy_failed + clang_format_failed;

    file << std::format("total_failed={}\n", total_failed);
    file << std::format("clang_tidy_failed_number={}\n", clang_tidy_failed);
    file << std::format("clang_format_failed_number={}\n", clang_format_failed);
  }


} // namespace linter::tool::clang_tidy