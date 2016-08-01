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
#include "terrain_parser.hpp"
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

std::vector<std::string> split(const std::string& str, const std::string& delimiters, SplitFlags flags)
{
	std::vector<std::string> res;
	boost::split(res, str, boost::is_any_of(delimiters), boost::token_compress_on);
	if(flags == SplitFlags::NONE) {
		res.erase(std::remove(res.begin(), res.end(), ""), res.end());
	}
	return res;
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

macro_cache_type& get_macro_cache()
{
	static macro_cache_type res;
	return res;
}

struct TagHelper
{
	TagHelper() : name(), vb(nullptr) {}
	std::string name;
	std::shared_ptr<variant_builder> vb;
};

struct TagHelper2
{
	TagHelper2() : name(), vb() {}
	std::string name;
	variant_builder vb;
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
			ASSERT_LOG(this_tag == tag_stack.top().name, "tag name mismatch error: " << this_tag << " != " << tag_stack.top().name << "; line: " << line_count);
			auto old_vb = tag_stack.top().vb;
			tag_stack.pop();
			ASSERT_LOG(!tag_stack.empty(), "vtags stack was empty.");
			// BUG because we build old_vb here, vb doesn't points to the unbuilt data. Which isn't helpful.
			tag_stack.top().vb->add(this_tag, old_vb->build());
			vb = tag_stack.top().vb;
		} else if(boost::regex_match(line.c_str(), what, re_macro_match)) {
			ASSERT_LOG(false, "Found an unexpanded macro definition." << line_count << ": " << line << "file: " << filename);
		} else {
			std::string value;
			auto pos = line.find_first_of('=');
			ASSERT_LOG(pos != std::string::npos, "error no '=' on line " << line_count << ": " << line << "file: " << filename);
			attribute = boost::trim_copy(line.substr(0, pos));			
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
			} else if(value == "yes" || value == "no" || value == "true" || value == "false") {
				if(value == "yes" || value == "true") {
					vb->add(attribute, variant::from_bool(true));
				} else {
					vb->add(attribute, variant::from_bool(false));
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
	return tag_stack.top().vb->build();
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

node_ptr read_wml2(const std::string& contents) 
{
	node_ptr root = std::make_shared<node>("");
	std::stack<node_ptr> current;
	current.emplace(root);
	
	auto lines = split(contents, "\n", SplitFlags::NONE);
	
	bool in_multi_line_string = false;
	bool is_translateable_ml_string = false;
	std::string ml_string;
	std::string attribute;

	int expect_merge = 0;

	std::map<std::string, node_ptr> last_node;

	for(auto& line : lines) {
		boost::trim(line);
		// search for any inline comments to remove or pre-processor directives to action.
		auto comment_pos = line.find('#');
		if(comment_pos != std::string::npos) {
			std::string pre_processor_stmt = line.substr(comment_pos + 1);
			line = boost::trim_copy(line.substr(0, comment_pos));
			if(line.empty() && pre_processor_stmt.empty()) {
				// skip blank lines
				continue;
			}
			if((pre_processor_stmt[0] == ' ' || pre_processor_stmt[0] == '#') && line.empty()) {
				// skip comments.
				continue;
			}
		}

		// line should be valid at this point
		boost::cmatch what;
		if(in_multi_line_string) {
			// XXX
			auto quote_pos = line.find('"');
			ml_string += "\n" + line.substr(0, quote_pos);
			if(quote_pos != std::string::npos) {
				in_multi_line_string = false;
				current.top()->add_attr(attribute, (is_translateable_ml_string ? "~" : "") + ml_string + (is_translateable_ml_string ? "~" : ""));
				is_translateable_ml_string = false;
				ml_string.clear();
			}
		} else if(boost::regex_match(line.c_str(), what, re_open_tag)) {
			// Opening tag
			std::string tag_name(what[1].first, what[1].second);
			if(tag_name[0] == '+') {
				++expect_merge;
				auto it = last_node.find(tag_name.substr(1));
				ASSERT_LOG(it != last_node.end(), "Unable to find merge to node for " << tag_name);
				current.emplace(it->second);
			} else {
				current.emplace(current.top()->add_child(std::make_shared<node>(tag_name)));
				last_node[tag_name] = current.top();
			}
		} else if(boost::regex_match(line.c_str(), what, re_close_tag)) {
			// Closing tag
			std::string this_tag(what[1].first, what[1].second);
			if(expect_merge != 0) {
				--expect_merge;
				current.pop();
				continue;
			}
			ASSERT_LOG(this_tag == current.top()->name(), "tag name mismatch error: " << this_tag << " != " << current.top()->name());
			current.pop();
		} else if(boost::regex_match(line.c_str(), what, re_macro_match)) {
			ASSERT_LOG(false, "Found an unexpanded macro definition.");
		} else {
			std::string value;
			auto pos = line.find_first_of('=');
			ASSERT_LOG(pos != std::string::npos, "error no '=' " << line);
			attribute = line.substr(0, pos);
			value = line.substr(pos + 1);
			boost::trim(value);
			if(std::count(value.cbegin(), value.cend(), '"') == 1) {
				in_multi_line_string = true;
				is_translateable_ml_string = value[0] == '_';
				auto quote_pos = value.find('"');
				ASSERT_LOG(quote_pos != std::string::npos, "Missing quotation mark on line " << line);
				ml_string = value.substr(quote_pos+1);
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
				current.top()->add_attr(attribute, (is_translateable ? "~" : "") + value + (is_translateable ? "~" : ""));
			}
		}
	}
	return root;
}

variant to_int(const std::string& s)
{
	boost::cmatch what;
	if(boost::regex_match(s.c_str(), what, re_macro_match)) {
		return variant(s);
	}
	// integer
	try {
		int num = boost::lexical_cast<int>(s);
		return variant(num);
	} catch(boost::bad_lexical_cast&) {
		ASSERT_LOG(false, "Unable to convert value '" << s << "' to integer.");
	}
	return variant();
}

variant to_list_int(const std::string& s, const std::string& sep)
{
	std::vector<variant> list;
	const auto strs = split(s, sep, SplitFlags::NONE);
	for(const auto& str : strs) {
		boost::cmatch what;
		if(boost::regex_match(str.c_str(), what, re_num_match)) {
			const std::string frac(what[1].first, what[1].second);
			if(frac.empty()) {
				// integer
				try {
					int num = boost::lexical_cast<int>(str);
					list.emplace_back(variant(num));
				} catch(boost::bad_lexical_cast&) {
					ASSERT_LOG(false, "Unable to convert value '" << str << "' to integer.");
				}
			} else {
				try {
					double num = boost::lexical_cast<double>(str);
					list.emplace_back(variant(num));
				} catch(boost::bad_lexical_cast&) {
					ASSERT_LOG(false, "Unable to convert value '" << str << "' to double.");
				}
			}
		} else {
			ASSERT_LOG(false, "Wasn't numeric value: " << str);
		}
	}
	return variant(&list);
}

variant to_list_string(const std::string& s, const std::string& sep, SplitFlags flags)
{
	std::vector<variant> res;
	const auto strs = split(s, sep, flags);
	for(const auto& str : strs) {
		res.emplace_back(variant(str));
	}
	return variant(&res);
}

std::map<variant, variant> process_name_string(const std::string& s)
{
	std::map<variant, variant> res;
	std::string acc;
	bool in_brackets = false;
	int in_parens = 0;
	bool start_colon_str = false;
	std::string current{ "name" };
	std::string ani_str;

	std::stack<TagHelper2> stk;
	stk.emplace();

	for(auto c : s) {
		if(c == '~') {
			// ~ inside brackets is an animation range rather than starting a modifier command.
			if(!in_brackets) {
				if(!acc.empty()) {
					if(in_parens == 0) {
						res[variant(current)] = variant(acc);
					} else {
						// XXX
						stk.top().vb.add("param", acc);
					}
					acc.clear();
					current.clear();
				}
			} else {
				ani_str += "~";
			}
		} else if(c == '[') {
			in_brackets = true;
		} else if(c == ']') {
			ASSERT_LOG(in_brackets, "Closing bracket found with no matching open bracket. " << s);
			in_brackets = false;
			acc += "@A";
			auto strs = split(ani_str, "~", SplitFlags::NONE);
			ASSERT_LOG(strs.size() == 2, "animation range malformed: " << ani_str);
			try {
				int r1 = boost::lexical_cast<int>(strs[0]);
				int r2 = boost::lexical_cast<int>(strs[1]);
				if(r1 > r2) {
					std::swap(r1, r2);
				}
				std::vector<variant> range_list;
				for(int n = r1; n != r2 + 1; ++n) {
					range_list.emplace_back(n);
				}
				res[variant("animation-frames")] = variant(&range_list);
			} catch(boost::bad_lexical_cast&) {
				ASSERT_LOG(false, "Unable to parse string into integers: " << ani_str);
			}
		} else if(c == ':') {
			if(!acc.empty()) {
				res[variant("name")] = variant(acc);
				acc.clear();
			}
			start_colon_str = true;
		} else if(c == '(') {
			ASSERT_LOG(!acc.empty(), "No command was identified: " << s);
			// XXX
			stk.top().name = acc;
			stk.emplace();
			acc.clear();
			++in_parens;
		} else if(c == ')') {
			// XXX
			if(!acc.empty()) {
				stk.top().vb.add("param", acc);
			}
			auto v = stk.top().vb.build();
			stk.pop();
			stk.top().vb.add(stk.top().name, v);
			//if(current == "CROP") {
			//} else if(current == MASK) {
			//} else if(current == BLIT) {
			//} else if(curent == O) {
			//} else {
			//}
			acc.clear();
			--in_parens;
		} else {
			if(in_brackets) {
				ani_str += c;
			} else {
				acc += c;
			}
		}
	}
	
	// XXX
	auto v = stk.top().vb.build();
	if(v.is_map() && v.num_elements() != 0) {
		for(const auto& p : v.as_map()) {
			res[variant(p.first)] = p.second;
		}
	}

	if(!acc.empty()) {
		if(start_colon_str) {
			try {
				double num = boost::lexical_cast<double>(acc);
				res[variant("animation_timing")] = variant(num);
			} catch(boost::bad_lexical_cast&) {
				ASSERT_LOG(false, "Bad number for animation timing: " << acc);
			}
		} else {
			res[variant(current)] = variant(acc);
		}
	}
	return res;
}

void print_map(const std::map<variant, variant>& m)
{
	std::stringstream ss;
	ss << "\n";
	for(const auto& p : m) {
		ss << p.first << ":" << p.second.write_json(true, 4) << "\n";
	}
	std::cout << ss.str();
}

int main(int argc, char* argv[])
{
	std::vector<std::string> args;
	args.reserve(argc-1);
	for(int n = 1; n < argc; ++n) {
		args.emplace_back(argv[n]);
	}

	/* First version generates a monolithic json file with all the terrain data.
	variant terrain_types = read_wml(terrain_type_file, sys::read_file(base_path + terrain_type_file));
	sys::write_file(terrain_type_file, terrain_types.write_json(true, 4));

	sys::file_path_map fpm;
	sys::get_unique_files(base_path + terrain_graphics_macros_dir, fpm);
	for(const auto& p : fpm) {
		if(p.first.find(".cfg") != std::string::npos) {
			pre_process_wml(p.first, sys::read_file(p.second));
		}
	}

	auto subst_data = macro_substitute(sys::read_file(base_path + terrain_graphics_file));
	sys::write_file("test.cfg", subst_data);
	auto rt = read_wml2(subst_data);

	std::stack<variant_builder> tags;
	tags.emplace();
	rt->post_order_traversal<std::stack<variant_builder>>([](node_ptr n, std::stack<variant_builder>& tags) {
		tags.emplace();
		}, [](node_ptr n, std::stack<variant_builder>& tags) {
		for(const auto& p : n->attributes()) {
			if(p.first == "center") {
				tags.top().add(p.first, to_list_int(p.second));
			} else if(p.first == "base") {
				tags.top().add(p.first, to_list_int(p.second));
			} else if(p.first == "layer") {
				tags.top().add(p.first, to_int(p.second));
			} else if(p.first == "pos") {
				tags.top().add(p.first, to_int(p.second));
			} else if(p.first == "rotations") {
				tags.top().add(p.first, to_list_string(p.second));
			} else if(p.first == "set_no_flag") {
				if(!p.second.empty()) {
					tags.top().add(p.first, to_list_string(p.second));
				}
			} else if(p.first == "set_flag") {
				if(!p.second.empty()) {
					tags.top().add(p.first, to_list_string(p.second));
				}
			} else if(p.first == "no_flag") {
				if(!p.second.empty()) {
					tags.top().add(p.first, to_list_string(p.second));
				}
			} else if(p.first == "has_flag") {
				if(!p.second.empty()) {
					tags.top().add(p.first, to_list_string(p.second));
				}
			} else if(p.first == "variations") {
				//tags.top().add(p.first, to_list_int(p.second, ";"));
				tags.top().add(p.first, to_list_string(p.second, ";", SplitFlags::ALLOW_EMPTY_STRINGS));
			} else if(p.first == "x,y") {
				auto v = to_list_int(p.second);
				tags.top().add("x", v[0]);
				tags.top().add("y", v[1]);
			} else if(p.first == "probability") {
				tags.top().add(p.first, to_int(p.second));
			} else if(p.first == "map") {
				tags.top().add(p.first, to_list_string(p.second, "\n"));
			} else if(p.first == "type") {
				tags.top().add(p.first, to_list_string(p.second));
			} else if(p.first == "name") {
				auto name_map = process_name_string(p.second);
				for(const auto& nm : name_map) {
					tags.top().add(nm.first.as_string(), nm.second);
				}
			} else {
				tags.top().add(p.first, p.second);
			}
		}
		auto old_vb = tags.top();
		tags.pop();
		tags.top().add(n->name(), old_vb.build());
	}, tags);
	variant terrain_graphics = tags.top().build();
	sys::write_file(terrain_graphics_file, terrain_graphics[""].write_json(true, 4));
	*/

	/*auto ret = process_name_string("village/drake1-A[01~03].png:200");
	print_map(ret);
	ret = process_name_string("off-map/border.png");
	print_map(ret);
	ret = process_name_string("off-map/border-@R0-@R1.png");
	print_map(ret);
	ret = process_name_string("impassable-editor.png~O(0.5)");
	print_map(ret);
	ret = process_name_string("off-map/border.png~MASK(terrain/masks/long-concave-2-@R0.png~BLIT(terrain/masks/long-concave-2-@R1.png)~BLIT(terrain/masks/long-concave-2-@R2.png)~BLIT(terrain/masks/long-concave-2-@R3.png)~BLIT(terrain/masks/long-concave-2-@R4.png)~BLIT(terrain/masks/long-concave-2-@R5.png))");
	print_map(ret);
	ret = process_name_string("water/water[01~17].png~CROP(0,0,72,72):100");
	print_map(ret);*/

	parse_terrain_files(base_path + terrain_graphics_macros_dir, base_path + terrain_graphics_file);
}
