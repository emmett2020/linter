#include "clang_format.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <boost/regex.hpp>
#include <spdlog/spdlog.h>
#include <tinyxml2.h>

#include "utils/shell.h"
#include "utils/util.h"

namespace linter::clang_format {
  using namespace std::string_view_literals;

  namespace {
    auto get_line_lens(std::string_view file_path) -> std::vector<uint32_t> {
      spdlog::trace("Enter clang_format::get_position() with file_path:{}", file_path);
      auto lines = std::vector<uint32_t>{};
      auto file  = std::ifstream{file_path.data()};

      auto temp = std::string{};
      while (std::getline(file, temp)) {
        // LF
        lines.emplace_back(temp.size() + 1);
      }
      return lines;
    }

    void trace_vector(const std::vector<uint32_t> &vec) {
      for (auto v: vec) {
        spdlog::trace("{}", v);
      }
    }

    template <class T>
    auto stringify_vector(const std::vector<T> &vec) -> std::string {
      auto ret = std::string{};
      for (auto v: vec) {
        ret += std::to_string(v) + ",";
      }
      return ret;
    }

    // offset starts from 0 while row/col starts from 1
    auto get_position(const std::vector<uint32_t> &lens, int offset)
      -> std::tuple<int32_t, int32_t> {
      spdlog::trace("Enter clang_format::get_position() with offset:{}, lens:{}",
                    offset,
                    stringify_vector(lens));

      auto cur = uint32_t{0};
      for (int row = 0; row < lens.size(); ++row) {
        auto len = lens[row];
        if (offset >= cur && offset < cur + len) {
          return {row + 1, offset - cur + 1};
        }
        cur += len;
      }
      return {-1, -1};
    }

    inline auto xml_error(tinyxml2::XMLError err) -> std::string_view {
      spdlog::trace("Enter clang_format::xml_error() with err:{}", static_cast<int>(err));
      return tinyxml2::XMLDocument::ErrorIDToName(err);
    }

    inline auto xml_has_error(tinyxml2::XMLError err) -> bool {
      spdlog::trace("Enter clang_format::xml_has_error() with err:{}", static_cast<int>(err));
      return err != tinyxml2::XMLError::XML_SUCCESS;
    }

    auto parse_replacements_xml(std::string_view data) -> replacements_type {
      spdlog::trace("Enter clang_format::parse_replacements_xml() with data:{}", data);

      // Names in replacements xml file.
      static constexpr auto offset_str       = "offset";
      static constexpr auto length_str       = "length";
      static constexpr auto replacements_str = "replacements";
      static constexpr auto replacement_str  = "replacement";

      // Start to parse given data to xml tree.
      auto doc = tinyxml2::XMLDocument{};
      auto err = doc.Parse(data.data());
      throw_if(xml_has_error(err),
               std::format("Parse replacements xml failed since: {}", xml_error(err)));
      throw_if(doc.NoChildren(),
               "Parse replacements xml failed since no children in replacements xml");

      // Find <replacements><replacement offset="xxx" length="xxx">text</replacement></replacements>
      auto *replacements_ele = doc.FirstChildElement(replacements_str);
      throw_if(replacements_ele == nullptr,
               "Parse replacements xml failed since no child names 'replacements'");
      auto replacements = replacements_type{};

      // Empty replacement node is allowd here.
      auto *replacement_ele = replacements_ele->FirstChildElement(replacement_str);
      while (replacement_ele != nullptr) {
        auto replacement = replacement_type{};
        replacement_ele->QueryIntAttribute(offset_str, &replacement.offset);
        replacement_ele->QueryIntAttribute(length_str, &replacement.length);
        const auto *text = replacement_ele->GetText();
        if (text != nullptr) {
          replacement.data = text;
        }

        trace_replacement(replacement);
        replacements.emplace_back(std::move(replacement));

        replacement_ele = replacement_ele->NextSiblingElement(replacement_str);
      }
      return replacements;
    }


    enum class output_style_t : std::uint8_t {
      formatted_source_code,
      replacement_xml
    };

    auto make_replacements_options(std::string_view file) -> std::vector<std::string> {
      spdlog::trace("Enter clang_format::make_replacements_options() with file:{}", file);
      auto tool_opt = std::vector<std::string>{};
      tool_opt.emplace_back("--output-replacements-xml");
      tool_opt.emplace_back(file);
      return tool_opt;
    }

    auto make_source_code_options(std::string_view file) -> std::vector<std::string> {
      spdlog::trace("Enter clang_format::make_source_code_options() with file:{}", file);
      auto tool_opt = std::vector<std::string>{};
      tool_opt.emplace_back(file);
      return tool_opt;
    }

    auto execute(const user_option &user_opt,
                 output_style_t output_style,
                 std::string_view repo,
                 std::string_view file) -> shell::result {
      spdlog::trace("Enter clang_format::execute() with output_style:{}, repo:{}, file:{}",
                    static_cast<int>(output_style),
                    repo,
                    file);

      auto tool_opt     = output_style == output_style_t::formatted_source_code
                          ? make_source_code_options(file)
                          : make_replacements_options(file);
      auto tool_opt_str = tool_opt | std::views::join_with(' ') | std::ranges::to<std::string>();
      spdlog::info("Running command: {} {}", user_opt.clang_format_binary, tool_opt_str);

      return shell::execute(user_opt.clang_format_binary, tool_opt, repo);
    }

    void trace_shell_result(const shell::result &result) {
      spdlog::trace("The original result of clang-format:\nreturn code: {}\nstdout:\n{}stderr:\n{}",
                    result.exit_code,
                    result.std_out,
                    result.std_err);
    }


  } // namespace

  void trace_replacement(const replacement_type &replacement) {
    spdlog::trace("offset: {}, length: {}, data: {}",
                  replacement.offset,
                  replacement.length,
                  replacement.data);
  }

  auto apply_on_single_file(
    const user_option &user_opt,
    bool needs_formatted_source_code,
    const std::string &repo,
    const std::string &file) -> result {
    spdlog::trace(
      "Enter clang_format::apply_on_single_file() with needs_formatted_source_code:{}, "
      "repo:{}, file:{}",
      needs_formatted_source_code,
      repo,
      file);

    auto xml_res = execute(user_opt, output_style_t::replacement_xml, repo, file);
    trace_shell_result(xml_res);
    if (xml_res.exit_code != 0) {
      return {.pass = false, .file = file, .replacements = {}, .origin_stderr = xml_res.std_err};
    }

    auto replacements = parse_replacements_xml(xml_res.std_out);

    auto res = result{.pass          = replacements.empty(),
                      .file          = file,
                      .replacements  = std::move(replacements),
                      .origin_stderr = xml_res.std_err};

    if (needs_formatted_source_code) {
      spdlog::debug("Execute clang-format again to get formatted source code.");
      auto code_res = execute(user_opt, output_style_t::formatted_source_code, repo, file);
      trace_shell_result(code_res);
      if (code_res.exit_code != 0) {
        return {.pass = false, .file = file, .replacements = {}, .origin_stderr = code_res.std_err};
      }
      res.formatted_source_code = code_res.std_out;
    }

    return res;
  }

} // namespace linter::clang_format
