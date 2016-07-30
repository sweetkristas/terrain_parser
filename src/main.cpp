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
	std::vector<std::string> res;
	boost::split(res, str, boost::is_any_of(delimiters));
	if(flags == SplitFlags::NONE) {
		res.erase(std::remove(res.begin(), res.end(), ""), res.end());
	}
	return res;
	/*std::vector<std::string> v;
	std::string::size_type start = 0;
	std::string::size_type pos = str.find_first_of(delimiters, start);
	 do {
		if(pos != start || flags == SplitFlags::ALLOW_EMPTY_STRINGS) { // ignore empty tokens
			v.emplace_back(str, start, pos - start);
		}
		start = pos + 1;
		pos = str.find_first_of(delimiters, start);
	} while(pos != std::string::npos);
	if(start < str.length()) { // ignore trailing delimiter
		v.emplace_back(str, start, str.length() - start); // add what's left of the string
	}
	return v;*/
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

struct TagHelper2
{
	TagHelper2() : name(), vb() {}
	std::string name;
	variant_builder vb;
};

class node;
typedef std::shared_ptr<node> node_ptr;

class node : public std::enable_shared_from_this<node>
{
	public:
		explicit node(const std::string& name) : name_(name), children_(), attr_(), parent_() {}
		const std::string& name() const { return name_; }
		node_ptr add_child(const node_ptr& child) {
			child->set_parent(shared_from_this()); 
			children_.emplace_back(child);
			return child;
		}
		node_ptr parent() const {
			auto p = parent_.lock();
			ASSERT_LOG(p != nullptr, "parent was null.");
			return p;
		}
		void add_attr(const std::string& a, const std::string& v) {
			attr_[a] = v;
		}
		const std::map<std::string, std::string>& attributes() const { return attr_; }
		template<typename T>
		bool pre_order_traversal(std::function<bool(node_ptr, T& param)> fn, T& param) {
			if(!fn(shared_from_this(), param)) {
				return false;
			}
			for(auto& c : children_) {
				if(!c->pre_order_traversal(fn, param)) {
					return false;
				}
			}
			return true;
		}
		template<typename T>
		void post_order_traversal(std::function<void(node_ptr, T& param)> fn1, std::function<void(node_ptr, T& param)> fn2, T& param) {
			fn1(shared_from_this(), param);
			for(auto& c : children_) {
				c->post_order_traversal(fn1, fn2, param);
			}
			fn2(shared_from_this(), param);
		}
	protected:
		void set_parent(node_ptr parent) { parent_ = parent; }
	private:
		std::string name_;
		std::vector<node_ptr> children_;
		std::map<std::string, std::string> attr_;
		std::weak_ptr<node> parent_;
};

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
	// integer
	try {
		int num = boost::lexical_cast<int>(s);
		return variant(num);
	} catch(boost::bad_lexical_cast&) {
		ASSERT_LOG(false, "Unable to convert value '" << s << "' to integer.");
	}
	return variant();
}

variant to_list_int(const std::string& s, const std::string& sep=",")
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

variant to_list_string(const std::string& s, const std::string& sep=",", SplitFlags flags=SplitFlags::NONE)
{
	std::vector<variant> res;
	const auto strs = split(s, sep, flags);
	for(const auto& str : strs) {
		res.emplace_back(variant(str));
	}
	return variant(&res);
}

std::map<std::string, variant> process_name_string(const std::string& s)
{
	std::map<std::string, variant> res;
	std::string acc;
	bool in_brackets = false;
	int in_parens = 0;
	std::string current{ "name" };
	std::string ani_str;

	std::stack<TagHelper2> vb;

	for(auto c : s) {
		if(c == '~') {
			// ~ inside brackets is an animation range rather than starting a modifier command.
			if(!in_brackets) {
				if(!acc.empty) {
					res[current] = variant(acc);
					acc.clear();
				}
			}
		} else if(c == '[') {
			in_brackets = true;
		} else if(c == ']') {
			ASSERT_LOG(in_brackets, "Closing bracket found with no matching open bracket. " << s);
			in_brackets = false;
			acc += "@A";
			auto strs = split(ani_str, '~');
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
				res["animation-frames"] = variant(&range_list);
			} catch(boost::bad_lexical_cast&) {
				ASSERT_LOG(false, "Unable to parse string into integers: " << ani_str);
			}
		} else if(c == '(') {
			ASSERT_LOG(!acc.empty(), "No command was identified: " << s);
			vb.emplace();
			vb.top().name = acc;
			acc.clear();
			++in_parens;
		} else if(c == ')') {
			--in_parens;
			vb.top().vb.add(vb.top().name, acc);
			acc.clear();
			auto old_tag = vb.top();
			vb.pop();
			if(vb.empty()) {
				res[old_tag.name] = old_tag.vb.build();
			} else {
				vb.top().add(old_tag.name, old_tag.vb.build());
			}
			//if(current == "CROP") {
			//} else if(current == MASK) {
			//} else if(current == BLIT) {
			//} else if(curent == O) {
			//} else {
			//}
		} else {
			if(in_brackets) {
				ani_str += c;
			} else {
				acc += c;
			}
		}
	}
	return res;
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

	auto subst_data = macro_substitute(sys::read_file(base_path + terrain_graphics_file));
	sys::write_file("test.cfg", subst_data);

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
				tags.top().add(p.first, to_list_string(p.second));
			} else if(p.first == "set_flag") {
				tags.top().add(p.first, to_list_string(p.second));
			} else if(p.first == "no_flag") {
				tags.top().add(p.first, to_list_string(p.second));
			} else if(p.first == "has_flag") {
				tags.top().add(p.first, to_list_string(p.second));
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
					tags.top().add(nm.first, nm.second);
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
}
