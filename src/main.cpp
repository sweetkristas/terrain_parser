#include <algorithm>
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
	boost::regex re_parens_match("\\((.*?)\\)");
	boost::regex re_quote_match("\"(.*?)\"");
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
	const std::string& getDefinition() const { return data_; }
	void setFileDetails(const std::string& fname, int offset) { filename_ = fname; line_offset_ = offset; }
	const std::string& getFilename() const { return filename_; }
	int getLineOffset() const { return line_offset_; }
	const std::vector<std::string>& getParams() const { return params_; }
private:
	std::string name_;
	std::vector<std::string> params_;
	std::string data_;
	std::string filename_;
	int line_offset_;
};

typedef std::shared_ptr<Macro> MacroPtr;

typedef std::map<std::string, MacroPtr> macro_cache_type;
macro_cache_type& get_macro_cache()
{
	static macro_cache_type res;
	return res;
}



// suck out macro definitions.
void pre_process_wml(const std::string& filename, const std::string& contents)
{
	auto lines = split(contents, "\n", SplitFlags::NONE);
	MacroPtr current_macro = nullptr;
	std::string macro_name;
	bool in_macro = false;
	std::string macro_lines;
	int line_count = 1;
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
				macro_name = params[0];
				ASSERT_LOG(get_macro_cache().find(macro_name) == get_macro_cache().end(), "Detected duplicate macro name: " << params[0] << "; line: " << line_count << "; " << filename) ;
				current_macro = std::make_shared<Macro>(params[0], std::vector<std::string>(params.cbegin() + 1, params.cend()));
				++line_count;
				current_macro->setFileDetails(filename, line_count);
				in_macro = true;
				continue;
			} else if (directive == "enddef") {
				ASSERT_LOG(current_macro != nullptr, "Found #enddef and not in a macro definition. line: " << line_count << "; " << filename);
				if(!line.empty()) {
					macro_lines += line + '\n';
				}
				//variant macro_def = read_wml(filename, macro_lines, line_count);
				current_macro->setDefinition(macro_lines);
				// clear the current macro data.
				in_macro = false;
				macro_lines.clear();
				++line_count;
				get_macro_cache()[macro_name] = current_macro;
				current_macro.reset();
				macro_name.clear();
				continue;
			} else {
				//LOG_WARN("Unrecognised pre-processor directive: " << directive << "; line: " << line_count << "; " << filename);
			}
		}

		// just collect the lines for parsing while in a macro.
		if(in_macro) {
			macro_lines += line + '\n';
			++line_count;
			continue;
		}
	}
}

std::string macro_substitute(const std::string& contents)
{
	auto lines = split(contents, "\n", SplitFlags::NONE);
	std::string output_str;
	for(const auto& line : lines) {
		bool in_macro = false;
		std::string macro_line;
		
		auto comment_pos = line.find('#');
		std::string line_to_process = line;
		if(comment_pos != std::string::npos) {
			line_to_process = boost::trim_copy(line.substr(0, comment_pos));
			if(line_to_process.empty()) {
				continue;
			}
		}

		for(auto c : line_to_process) {
			if(c == '{') {
				ASSERT_LOG(in_macro == false, "Already in macro");
				// start of macro definition.
				in_macro = true;
			} else if( c == '}') {
				ASSERT_LOG(in_macro == true, "Not in macro");
				in_macro = false;

				macro_line = boost::regex_replace(macro_line, re_whitespace_match, " ");
				auto strs = split(macro_line, " ", SplitFlags::NONE);

				auto it = get_macro_cache().find(strs.front());
				if(it == get_macro_cache().end()) {
					LOG_ERROR("No macro definition for: " << strs.front());
					continue;
				}
				const auto& params = it->second->getParams();
				ASSERT_LOG(params.size() == (strs.size() - 1), "macro: " << strs.front() << " given the wrong number of arguments. Expected " << params.size() << " given " << (strs.size() - 1));
				auto sit = strs.begin() + 1;
				std::string def = it->second->getDefinition();
				for(auto& p : params) {				
					std::string str = *sit;
					boost::cmatch what;
					if(boost::regex_match(sit->c_str(), what, re_parens_match)) {
						str = std::string(what[1].first, what[1].second);
						if(str.empty()) {
							str = "()";
						}
					}
					if(boost::regex_match(sit->c_str(), what, re_quote_match)) {
						str = std::string(what[1].first, what[1].second);
					}
					boost::replace_all(def, '{' + p + '}', str);
					++sit;
				}
				if(def.find('{') != std::string::npos) {
					def = macro_substitute(def);
				}
				output_str += def;
				macro_line.clear();
			} else if(in_macro) {
				macro_line += c;
			} else {
				output_str += c;
			}
		}
		output_str += '\n';
	}
	boost::replace_all(output_str, "()", "");
	return output_str;
}

struct TagHelper
{
	TagHelper() : name(), vb(nullptr) {}
	std::string name;
	std::shared_ptr<variant_builder> vb;
};

variant read_wml(const std::string& filename, const std::string& contents, int line_offset=0)
{
	auto lines = split(contents, "\n", SplitFlags::NONE);
	int line_count = 1 + line_offset;
	std::stack<TagHelper> tag_stack;
	tag_stack.emplace();
	tag_stack.top().vb = std::make_shared<variant_builder>();

	bool in_multi_line_string = false;
	bool is_translateable_ml_string = false;
	std::string ml_string;
	std::string attribute;
	std::map<std::string, std::shared_ptr<variant_builder>> last_vb;
	int expect_merge = 0;

	auto vb = tag_stack.top().vb;
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
		}

		if(line.empty()) {
			// skip blank lines
			++line_count;
			continue;
		}

		// line should be valid at this point
		boost::cmatch what;
		if(in_multi_line_string) {
			auto quote_pos = line.find('"');
			ml_string += "\n" + line.substr(0, quote_pos);
			if(quote_pos != std::string::npos) {
				in_multi_line_string = false;
				if(expect_merge) {
					vb->set(attribute, (is_translateable_ml_string ? "~" : "") + ml_string + (is_translateable_ml_string ? "~" : ""));
				} else {
					vb->add(attribute, (is_translateable_ml_string ? "~" : "") + ml_string + (is_translateable_ml_string ? "~" : ""));
				}
				is_translateable_ml_string = false;
			}
		} else if(boost::regex_match(line.c_str(), what, re_open_tag)) {
			// Opening tag
			std::string tag_name(what[1].first, what[1].second);
			if(tag_name[0] == '+') {
				auto it = last_vb.find(tag_name.substr(1));
				ASSERT_LOG(it != last_vb.end(), "Error finding last tag: " << tag_name);				
				//if(tag_name == "+image") {
				//	DebuggerBreak();
				//}
				vb = it->second;
				++expect_merge;
			} else {
				tag_stack.emplace();
				tag_stack.top().name = tag_name;
				vb = tag_stack.top().vb = std::make_shared<variant_builder>();
				last_vb[tag_name] = tag_stack.top().vb;
			}
		} else if(boost::regex_match(line.c_str(), what, re_close_tag)) {
			// Closing tag
			std::string this_tag(what[1].first, what[1].second);
			if(expect_merge != 0) {
				--expect_merge;
				continue;
			}
			//ASSERT_LOG(this_tag == tag_stack.top().name, "tag name mismatch error: " << this_tag << " != " << tag_stack.top().name << "; line: " << line_count);
			//auto old_vb = tag_stack.top().vb;
			//tag_stack.pop();
			//ASSERT_LOG(!tag_stack.empty(), "vtags stack was empty.");
			//tag_stack.top().vb->add(this_tag, old_vb->build());
			//vb = tag_stack.top().vb;
		} else if(boost::regex_match(line.c_str(), what, re_macro_match)) {
			ASSERT_LOG(false, "Found an unexpanded macro definition." << line_count << ": " << line << "file: " << filename);
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
						if(expect_merge) {
							vb->set(attribute, num);
						} else {
							vb->add(attribute, num);
						}
					} catch(boost::bad_lexical_cast&) {
						ASSERT_LOG(false, "Unable to convert value '" << value << "' to integer.");
					}
				} else {
					// float
					try {
						double num = boost::lexical_cast<double>(value);
						if(expect_merge) {
							vb->set(attribute, num);
						} else {
							vb->add(attribute, num);
						}
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
				//if(value == "frozen/ice2@V.png") {
				//	DebuggerBreak();
				//}
				if(expect_merge) {
					vb->set(attribute, (is_translateable ? "~" : "") + value + (is_translateable ? "~" : ""));
				} else {
					vb->add(attribute, (is_translateable ? "~" : "") + value + (is_translateable ? "~" : ""));
				}
			}
		}

		++line_count;
	}
	ASSERT_LOG(!tag_stack.empty(), "tag_stack was empty.");
	do {
		auto old_vb = tag_stack.top().vb;
		const std::string this_tag = tag_stack.top().name;
		tag_stack.pop();
		tag_stack.top().vb->add(this_tag, old_vb->build());
	} while(tag_stack.size() > 1);
	return tag_stack.top().vb->build();
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
			pre_process_wml(p.first, sys::read_file(p.second));
		}
	}

	//for(const auto& p : get_macro_cache()) {
	//	const auto& def = p.second->getDefinition();
	//	std::string sample_def = def.size() > 40 ? def.substr(0, 40) + "..." : def;
	//	LOG_DEBUG("macro: " << p.first << "; definition: " << sample_def);
	//}
	
	auto subst_data = macro_substitute(sys::read_file(base_path + terrain_graphics_file));
	sys::write_file("test.cfg", subst_data);
	variant terrain_graphics = read_wml(terrain_graphics_file, subst_data);
	sys::write_file(terrain_graphics_file, terrain_graphics.write_json(true, 4));

	/*std::cout << read_wml("test", "[terrain_graphics]\n\
[tile]\n\
x,y=0,0\n\
type=Ai\n\
set_no_flag=base\n\
\n\
[image]\n\
name=frozen/ice2@V.png\n\
variations=\";2;3;4;5;6;7;8;9;10;11\"\n\
layer=-1000\n\
[/image]\n\
[/tile]\n\
[/terrain_graphics]\n\
\n\
[+terrain_graphics]\n\
probability=10\n\
[+tile]\n\
[+image]\n\
name=frozen/ice2.png\n\
variations=""\n\
[/image]\n\
[/tile]\n\
[/terrain_graphics]\n").write_json(true, 4);*/
}
