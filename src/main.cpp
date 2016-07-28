#include <iostream>
#include <map>
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
#ifdef __linux__
	const std::string base_path = "../wesnoth/data/core/";
#elif defined(_MSC_VER)
	const std::string base_path = "c:\\projects\\wesnoth\\data\\core\\";
#else
	#error Unknown operating system.
#endif

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

class WmlReader
{
public:
	explicit WmlReader(const std::string& filename, const std::string& contents)
		: file_name_(filename),
		  contents_(contents),
		  line_count_(1)
	{
	}
private:
	std::string file_name_;
	std::string contents_;
	int line_count_;
};

class Macro
{
public:
	explicit Macro(const std::string& name, const std::vector<std::string>& params)
		: name_(name),
		  params_(params),
		  data_()
	{
	}
	void setDefinition(const std::string& v) { data_ = v; }
private:
	std::string name_;
	std::vector<std::string> params_;
	std::string data_;
};

typedef std::shared_ptr<Macro> MacroPtr;

typedef std::map<std::string, MacroPtr> macro_cache_type;
macro_cache_type& get_macro_cache()
{
	static macro_cache_type res;
	return res;
}


variant read_wml(const std::string& filename, const std::string& contents, int line_offset=0)
{
	auto lines = split(contents, "\n", SplitFlags::NONE);
	std::stack<std::string> tag_name_stack;
	int line_count = 1 + line_offset;
	std::stack<variant_builder> vtags;
	vtags.emplace();
	bool in_multi_line_string = false;
	bool is_translateable_ml_string = false;
	std::string ml_string;
	std::string attribute;
	MacroPtr current_macro = nullptr;
	std::string macro_lines;
	for(auto& line : lines) {
		boost::trim(line);
		// search for any inline comments to remove or pre-processor directives to action.
		auto comment_pos = line.find('#');
		if(comment_pos != std::string::npos) {
			std::string pre_processor_stmt = line.substr(comment_pos + 1);
			line = boost::trim_copy(line.substr(0, comment_pos));
			if(line.empty() && pre_processor_stmt.empty()) {
				// skip blank lines
				++line_count;
				continue;
			}
			if((pre_processor_stmt[0] == ' ' || pre_processor_stmt[0] == '#') && line.empty()) {
				// skip comments.
				++line_count;
				continue;
			}
			// we read in the next symbol to see what action we need to take.
			auto space_pos = pre_processor_stmt.find(' ');
			std::string directive = pre_processor_stmt;
			if(space_pos != std::string::npos) {
				directive = pre_processor_stmt.substr(0, space_pos);
			}
			if(directive == "define") {
				ASSERT_LOG(current_macro == nullptr, "Found #define inside a macro. line: " << line_count << "; " << filename);
				auto macro_line = boost::regex_replace(pre_processor_stmt.substr(space_pos + 1), re_whitespace_match, " ");
				auto params = split(macro_line, " ", SplitFlags::NONE);
				// first parameter is the name of the macro.
				current_macro = get_macro_cache()[params[0]];
				ASSERT_LOG(current_macro == nullptr, "Detected duplicate macro name: " << params[0] << "; line: " << line_count << "; " << filename) ;
				current_macro = std::make_shared<Macro>(params[0], std::vector<std::string>(params.cbegin() + 1, params.cend()));
				++line_count;
				continue;
			} else if (directive == "enddef") {
				ASSERT_LOG(current_macro != nullptr, "Found #enddef and not in a macro definition. line: " << line_count << "; " << filename);
				if(!line.empty()) {
					macro_lines += line + '\n';
				}
				//variant macro_def = read_wml(filename, macro_lines, line_count);
				current_macro->setDefinition(macro_lines);
				// clear the current macro data.
				current_macro.reset();
				macro_lines.clear();
				++line_count;
				continue;
			} else {
				//LOG_WARN("Unrecognised pre-processor directive: " << directive << "; line: " << line_count << "; " << filename);
			}
		}

		if(line.empty()) {
			// skip blank lines
			++line_count;
			continue;
		}

		// just collect the lines for parsing while in a macro.
		if(current_macro) {
			macro_lines += line + '\n';
			++line_count;
			continue;
		}


		auto& vb = vtags.top();

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

			auto it = get_macro_cache().find(strs.front());
			if(it == get_macro_cache().end()) {
				LOG_ERROR("No macro definition for: " << strs.front() << "; line: " << line_count << "; " << filename);
			}
		} else {
			std::string value;
			auto pos = line.find_first_of('=');
			ASSERT_LOG(pos != std::string::npos, "error no '=' on line " << line_count << ": " << line << "file: " << filename);
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

	variant terrain_types = read_wml(terrain_type_file, sys::read_file(base_path + terrain_type_file));
	sys::write_file(terrain_type_file, terrain_types.write_json(true, 4));

	sys::file_path_map fpm;
	sys::get_unique_files(base_path + terrain_graphics_macros_dir, fpm);
	for(const auto& p : fpm) {
		if(p.first.find(".cfg") != std::string::npos) {
			read_wml(p.first, sys::read_file(p.second));
		}
	}
	
	variant terrain_graphics = read_wml(terrain_graphics_file, sys::read_file(base_path + terrain_graphics_file));
	sys::write_file(terrain_graphics_file, terrain_graphics.write_json(true, 4));
}
