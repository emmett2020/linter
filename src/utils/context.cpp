#include "context.h"

#include <spdlog/spdlog.h>

namespace linter {
  void print_context(const context &ctx) {
    spdlog::debug("Context: ");
    spdlog::debug("--------------------------------------------------");
    spdlog::debug("Common Options:");
    spdlog::debug("\tuse on local: {}", ctx.use_on_local);
    spdlog::debug("\tlog level: {}", ctx.log_level);
    spdlog::debug("Repository Options:");
    spdlog::debug("\trepository path: {}", ctx.repo_path);
    spdlog::debug("\trepository: {}", ctx.repo);
    spdlog::debug("\trepository base ref: {}", ctx.base_ref);
    spdlog::debug("\trepository head ref: {}", ctx.head_ref);
    spdlog::debug("\trepository base commit: {}", ctx.base_commit);
    spdlog::debug("\trepository head commit: {}", ctx.head_commit);

    const auto &tidy_opt = ctx.clang_tidy_option;
    spdlog::debug("Options of clang-tidy:");
    spdlog::debug("\tenable clang tidy: {}", tidy_opt.enable_clang_tidy);
    spdlog::debug("\tenable clang tidy fastly exit: {}", tidy_opt.enable_clang_tidy_fastly_exit);
    spdlog::debug("\tallow no checks: {}", tidy_opt.allow_no_checks);
    spdlog::debug("\tenable check profile: {}", tidy_opt.enable_check_profile);
    spdlog::debug("\tclang tidy version: {}", tidy_opt.clang_tidy_version);
    spdlog::debug("\tchecks: {}", tidy_opt.checks);
    spdlog::debug("\tconfig: {}", tidy_opt.config);
    spdlog::debug("\tconfig file: {}", tidy_opt.config_file);
    spdlog::debug("\tdatabase: {}", tidy_opt.database);
    spdlog::debug("\theader filter: {}", tidy_opt.header_filter);
    spdlog::debug("\tline filter: {}", tidy_opt.line_filter);
    spdlog::debug("");
  }


} // namespace linter
