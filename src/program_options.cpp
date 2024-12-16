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
#include "program_options.h"

#include <algorithm>
#include <initializer_list>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/program_options/options_description.hpp>

#include "github/common.h"
#include "utils/util.h"

namespace linter {
namespace {
constexpr auto help = "help";
constexpr auto version = "version";
constexpr auto log_level = "log-level";
constexpr auto repo_path = "repo-path";
constexpr auto repo = "repo";
constexpr auto token = "token";
constexpr auto target = "target";
constexpr auto source = "source";
constexpr auto event_name = "event-name";
constexpr auto pr_number = "pr-number";
constexpr auto enable_step_summary = "enable-step-summary";
constexpr auto enable_comment_on_issue = "enable-comment-on-issue";
constexpr auto enable_pull_request_review = "enable-pull-request-review";

// Some options must be specified on the given condition, check it.
void must_specify(const std::string &condition,
                  const program_options::variables_map &variables,
                  const std::initializer_list<const char *> &options) {
  std::ranges::for_each(options, [&](const auto *option) {
    throw_unless(variables.contains(option), [&] noexcept {
      return std::format("must specify {} when {}", option, condition);
    });
  });
}

// Some options mustn't be specified on the given condition, check it.
void must_not_specify(const std::string &condition,
                      const program_options::variables_map &variables,
                      const std::initializer_list<const char *> &options) {
  std::ranges::for_each(options, [&](const auto *option) {
    throw_if(variables.contains(option), [&] noexcept {
      return std::format("must not specify {} when {}", option, condition);
    });
  });
}

// Theses options work both on local and CI.
void check_and_fill_context_common(
    const program_options::variables_map &variables, runtime_context &ctx) {
  spdlog::trace("check_and_fill_context_common");
  if (variables.contains(log_level)) {
    ctx.log_level = variables[log_level].as<std::string>();
    boost::algorithm::to_lower(ctx.log_level);
    throw_unless(std::ranges::contains(supported_log_level, ctx.log_level),
                 "unsupported log level");
  }

  auto must_specify_option = {target};
  must_specify("use cpp-linter on local or CI", variables, must_specify_option);
  ctx.target = variables[target].as<std::string>();
}

void check_and_fill_context_on_ci(
    const program_options::variables_map &variables, runtime_context &ctx) {
  spdlog::trace("check_and_fill_context_on_ci");
  auto must_not_specify_option = {repo_path, repo, source, event_name,
                                  pr_number};
  must_not_specify("use cpp-linter on CI", variables, must_not_specify_option);

  // Automatically enable step summary when on CI environment.
  ctx.enable_step_summary = true;
  if (variables.contains(enable_step_summary)) {
    ctx.enable_step_summary = variables[enable_step_summary].as<bool>();
  }

  if (variables.contains(enable_comment_on_issue)) {
    ctx.enable_comment_on_issue = variables[enable_comment_on_issue].as<bool>();
  }

  if (variables.contains(enable_pull_request_review)) {
    ctx.enable_pull_request_review =
        variables[enable_pull_request_review].as<bool>();
  }
}

void check_and_fill_context_on_local(
    const program_options::variables_map &variables, runtime_context &ctx) {
  spdlog::trace("check_and_fill_context_on_local");
  auto must_specify_option = {repo_path, source, event_name};
  must_specify("use cpp-linter on local", variables, must_specify_option);

  auto must_not_specify_option = {enable_step_summary};
  must_not_specify("use cpp-linter on local", variables,
                   must_not_specify_option);

  ctx.repo_path = variables[repo_path].as<std::string>();
  ctx.source = variables[source].as<std::string>();
  ctx.event_name = variables[event_name].as<std::string>();
  throw_unless(std::ranges::contains(all_github_events, ctx.event_name),
               std::format("unsupported event name: {}", ctx.event_name));

  if (variables.contains(enable_comment_on_issue) ||
      variables.contains(enable_pull_request_review)) {
    must_specify("use cpp-linter on local and enable interactive with GITHUB",
                 variables, {token, repo});
    ctx.token = variables[token].as<std::string>();
    ctx.repo = variables[repo].as<std::string>();
  }

  if (variables.contains(enable_comment_on_issue)) {
    ctx.enable_comment_on_issue = variables[enable_comment_on_issue].as<bool>();
  }

  if (variables.contains(enable_pull_request_review)) {
    ctx.enable_pull_request_review =
        variables[enable_pull_request_review].as<bool>();
  }

  if (variables.contains(pr_number)) {
    throw_unless(
        std::ranges::contains(github_events_with_pr_number, ctx.event_name),
        std::format("event: {} doesn't support pull-request-number option",
                    ctx.event_name));
    ctx.pr_number = variables[pr_number].as<std::int32_t>();
  }
}

} // namespace

auto create_program_options_desc() -> program_options::options_description {
  using program_options::value;
  using std::string;
  auto desc = program_options::options_description{"cpp-linter options"};

  // clang-format off
  desc.add_options()
    (help,                                           "Produce help message")
    (version,                                        "Print current cpp-linter version")
    (log_level,                   value<string>(),   "Set the log verbose level of cpp-linter")
    (repo_path,                   value<string>(),   "Set the full path of git repository")
    (repo,                        value<string>(),   "Set the owner/repo pair of git repository")
    (token,                       value<string>(),   "Set github token of git repository")
    (target,                      value<string>(),   "Set the target reference/commit of git repository")
    (source,                      value<string>(),   "Set the source reference/commit of git repository")
    (event_name,                  value<string>(),   "Set the event name of git repository. e.g.: pull_request")
    (pr_number,                   value<int32_t>(),  "Set the pull-request number of git repository")
    (enable_comment_on_issue,     value<bool>(),     "Enable comment on Github issues")
    (enable_pull_request_review,  value<bool>(),     "Enable Github pull-request reivew comment")
    (enable_step_summary,         value<bool>(),     "Enable write step summary to Github action")
  ;
  // clang-format on
  return desc;
}

auto parse_program_options(int argc, char **argv,
                           const program_options::options_description &desc)
    -> program_options::variables_map {
  auto variables = program_options::variables_map{};
  program_options::store(program_options::parse_command_line(argc, argv, desc),
                         variables);
  program_options::notify(variables);
  return variables;
}

// This function will be called after check context. So there's no need to do
// same check.
void check_and_fill_context_by_program_options(
    const program_options::variables_map &variables, runtime_context &ctx) {
  spdlog::debug("Start to check program_options and fill context by it");

  check_and_fill_context_common(variables, ctx);
  if (ctx.use_on_local) {
    check_and_fill_context_on_local(variables, ctx);
  } else {
    check_and_fill_context_on_ci(variables, ctx);
  }
}

} // namespace linter
