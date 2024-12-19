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

#include "tools/base_reporter.h"

#include <vector>
#include <string>
#include <string_view>

#include "context.h"
#include "github/github.h"
#include "utils/env_manager.h"
#include "utils/util.h"

namespace linter::tool {
  using namespace std::string_view_literals;

  void write_to_github_action_output(const runtime_context &context,
                                     const std::vector<reporter_base_ptr> &reporters) {
    for (const auto &reporter: reporters) {
      reporter->write_to_action_output(context);
    }
  }

  bool all_passed(const std::vector<reporter_base_ptr> &reporters) {
    for (const auto &reporter: reporters) {
      auto [is_passed, successed, failed, ignored] = reporter->get_brief_result();
      if (is_passed) {
        return false;
      }
    }
    return true;
  }

  void write_to_github_step_summary(const runtime_context &context,
                                    const std::vector<reporter_base_ptr> &reporters) {
    auto summary_file = env::get(github::github_step_summary);
    auto file         = std::fstream{summary_file, std::ios::app};
    throw_unless(file.is_open(), "failed to open step summary file to write");

    static const auto title     = "# The cpp-linter Result"s;
    static const auto hint_pass = ":rocket: All checks on all file passed."s;
    static const auto hint_fail = ":warning: Some files didn't pass the cpp-linter checks\n"s;

    if (all_passed(reporters)) {
      file << (title + hint_pass);
      return;
    }

    auto summary = std::string{};
    for (const auto &reporter: reporters) {
      summary += reporter->make_step_summary(context) + "\n";
    }
    file << (title + hint_fail + summary);
  }

  void comment_on_github_issue(const runtime_context &context,
                               const std::vector<reporter_base_ptr> &reporters) {
    auto github_client = github::client{};
    github_client.get_issue_comment_id(context);

    constexpr auto failed_emoji = ":worried:"sv;
    constexpr auto header = "# cpp-linter results:\n"sv;
    constexpr auto table_header   = "| tool name | successed | failed | ignored |\n"sv;
    constexpr auto table_sep_line = "|-----------|-----------|--------|---------|\n"sv;
    constexpr auto table_row_fmt = "| {} | {} | {} | {} |\n"sv;
    constexpr auto summary_fmt = "<summary>click here to see the details of {} failed files reported by {}</summary>\n\n"sv;
    constexpr auto details_fmt = "<details>{}</details>\n"sv;

    auto table_rows = ""s;
    auto details = ""s;

    for (const auto &reporter: reporters) {
      auto [is_passed, successed, failed, ignored] = reporter->get_brief_result();
      auto tool_name = reporter->tool_name();
      table_rows += std::format(table_row_fmt, tool_name, successed, failed, ignored);
      auto summary = std::format(summary_fmt, failed, tool_name);
      auto tool_detail = reporter->make_issue_comment(context) + "\n";
      details += std::format(details_fmt, summary + tool_detail);
    }

    auto final_content = std::format("{}{}{}{}{}", header, table_header, table_sep_line, table_rows, details);
    github_client.add_or_update_issue_comment(context, final_content);
  }

  void comment_on_github_pull_request_review(const runtime_context &context,
                                             const std::vector<reporter_base_ptr> &reporters) {
    auto github_client = github::client{};
    auto comments      = github::review_comments{};
    for (const auto &reporter: reporters) {
      auto ret = reporter->make_review_comment(context);
      comments.insert(comments.end(), ret.begin(), ret.end());
    }
    auto body = github::make_review_str(comments);
    github_client.post_pull_request_review(context, body);
  }
} // namespace linter::tool
