#include <iostream>
#include <stack>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>

#include "asserts.hpp"
#include "filesystem.hpp"
#include "variant.hpp"
#include "variant_utils.hpp"

namespace 
{
	const std::string base_path = "c:\\projects\\wesnoth\\data\\core\\";

	const std::string terrain_type_file = "terrain.cfg";
	const std::string terrain_graphics_file = "terrain-graphics.cfg";
	const std::string terrain_graphics_macros_dir = "terrain-graphics";

	boost::regex re_open_tag("\\[([^\\/][A-Za-z0-9_]+)\\]");
	boost::regex re_close_tag("\\[\\/([A-Za-z0-9_]+)\\]");
	boost::regex re_num_match("\\d+(\\.\\d*)?");
	boost::regex re_macro_match("\\{(.*?)\\}");
	boost::regex re_whitespace_match("\\s+");
}

enum class SplitFlags {
	NONE					= 0,
	ALLOW_EMPTY_STRINGS		= 1,
};

std::vector<std::string> split(const std::string& str, const std::string& delimiters, SplitFlags flags)
{
	std::vector<std::string> v;
	std::string::size_type start = 0;
	auto pos = str.find_first_of(delimiters, start);
	while(pos != std::string::npos) {
		if(pos != start && flags != SplitFlags::ALLOW_EMPTY_STRINGS) { // ignore empty tokens
			v.emplace_back(str, start, pos - start);
		}
		start = pos + 1;
		pos = str.find_first_of(delimiters, start);
	}
	if(start < str.length()) { // ignore trailing delimiter
		v.emplace_back(str, start, str.length() - start); // add what's left of the string
	}
	return v;
}


variant read_wml(const std::string& filename)
{
	auto contents = sys::read_file(filename);
	auto lines = split(contents, "\n", SplitFlags::NONE);
	std::stack<std::string> tag_name_stack;
	int line_count = 1;
	std::stack<variant_builder> vtags;
	vtags.emplace();
	bool in_multi_line_string = false;
	bool is_translateable_ml_string = false;
	std::string ml_string;
	std::string attribute;
	for(auto& line : lines) {
		boost::trim(line);
		if(line.empty() || line[0] == '#') {
			// skip blank lines and comments.
			++line_count;
			continue;
		}
		auto& vb = vtags.top();

		// search for any inline comments to remove.
		auto comment_pos = line.find('#');
		if(comment_pos != std::string::npos) {
			line = boost::trim_copy(line.substr(0, comment_pos));
		}

		// line should be valid at this point
		boost::cmatch what;
		if(in_multi_line_string) {
			auto quote_pos = line.find('"');
			ml_string += "\n" + line.substr(0, quote_pos);
			if(quote_pos != std::string::npos) {
				in_multi_line_string = false;
				vb.add(attribute, (is_translateable_ml_string ? "~" : "") + ml_string + (is_translateable_ml_string ? "~" : ""));
				is_translateable_ml_string = false;
			}
		} else if(boost::regex_match(line.c_str(), what, re_open_tag)) {
			// Opening tag
			tag_name_stack.emplace(std::string(what[1].first, what[1].second));
			vtags.emplace();
		} else if(boost::regex_match(line.c_str(), what, re_close_tag)) {
			// Closing tag
			const std::string this_tag(what[1].first, what[1].second);
			ASSERT_LOG(this_tag == tag_name_stack.top(), "tag name mismatch error: " << this_tag << " != " << tag_name_stack.top() << "; line: " << line_count);
			tag_name_stack.pop();
			auto old_vb = vtags.top();
			vtags.pop();
			ASSERT_LOG(!vtags.empty(), "vtags stack was empty.");
			vtags.top().add(this_tag, old_vb.build());
		} else if(boost::regex_match(line.c_str(), what, re_macro_match)) {
			std::string macro_line(what[1].first, what[1].second);
			// is a macro, requiring expansion.
			//LOG_ERROR("unprocessed macro: " << line);
			macro_line = boost::regex_replace(macro_line, re_whitespace_match, " ");
			auto strs = split(macro_line, " ", SplitFlags::NONE);
			LOG_ERROR("unprocessed macro: " << strs.front());
		} else {
			std::string value;
			auto pos = line.find_first_of('=');
			ASSERT_LOG(pos != std::string::npos, "error no '=' on line " << line_count << ": " << line);
			attribute = line.substr(0, pos);
			value = line.substr(pos + 1);
			boost::trim(value);
			if(std::count(value.cbegin(), value.cend(), '"') == 1) {
				in_multi_line_string = true;
				is_translateable_ml_string = value[0] == '_';
				auto quote_pos = value.find('"');
				ASSERT_LOG(quote_pos != std::string::npos, "Missing quotation mark on line " << line_count << ": " << value);
				ml_string = value.substr(quote_pos+1);
			} else if(boost::regex_match(value.c_str(), what, re_num_match)) {
				std::string frac(what[1].first, what[1].second);
				if(frac.empty()) {
					// integer
					try {
						int num = boost::lexical_cast<int>(value);
						vb.add(attribute, num);
					} catch(boost::bad_lexical_cast&) {
						ASSERT_LOG(false, "Unable to convert value '" << value << "' to integer.");
					}
				} else {
					// float
					try {
						double num = boost::lexical_cast<double>(value);
						vb.add(attribute, num);
					} catch(boost::bad_lexical_cast&) {
						ASSERT_LOG(false, "Unable to convert value '" << value << "' to double.");
					}
				}
			} else {
				bool is_translateable = false;
				if(value[0] == '_') {
					// mark as translatable string
					is_translateable = true;
				}
				// add as string
				auto quote_pos_start = value.find_first_of('"');
				auto quote_pos_end = value.find_last_of('"');
				if(quote_pos_start != std::string::npos && quote_pos_end != std::string::npos) {
					value = value.substr(quote_pos_start+1, quote_pos_end - (quote_pos_start + 1));
				}
				vb.add(attribute, (is_translateable ? "~" : "") + value + (is_translateable ? "~" : ""));
			}
		}

		++line_count;
	}
	ASSERT_LOG(!vtags.empty(), "vtags stack was empty.");
	return vtags.top().build();
}

int main(int argc, char* argv[])
{
	std::vector<std::string> args;
	args.reserve(argc-1);
	for(int n = 1; n < argc; ++n) {
		args.emplace_back(argv[n]);
	}

	variant terrain_types = read_wml(base_path + terrain_type_file);
	sys::write_file(terrain_type_file, terrain_types.write_json(true, 4));

	variant terrain_graphics = read_wml(base_path + terrain_graphics_file);
	sys::write_file(terrain_graphics_file, terrain_graphics.write_json(true, 4));
}
