#include "terrain_parser.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>

#include "asserts.hpp"
#include "filesystem.hpp"
#include "variant.hpp"
#include "variant_utils.hpp"

namespace 
{
	boost::regex re_open_tag("\\[([^\\/][A-Za-z0-9_]+)\\]");
	boost::regex re_close_tag("\\[\\/([A-Za-z0-9_]+)\\]");
	boost::regex re_macro_match("\\{(.*?)\\}");
	boost::regex re_parens_match("\\((.*?)\\)");

	node_ptr read_wml_macro(const std::string& root_name, const std::string& contents) 
	{
		node_ptr root = std::make_shared<node>(root_name);
		std::stack<node_ptr> current;
		current.emplace(root);

		auto lines = split(contents, "\n", SplitFlags::NONE);

		bool in_multi_line_string = false;
		bool is_translateable_ml_string = false;
		std::string ml_string;
		std::string attribute;

		int expect_merge = 0;

		std::map<std::string, node_ptr> last_node;

		const auto& cache = get_macro_cache();

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
				std::string inner{what[1].first, what[1].second};
				// {BORDER_RESTRICTED5_RANDOM_LFB  ({TERRAIN})  ({ADJACENT}) {LAYER} {FLAG} {BUILDER} {IMAGESTEM}}
				//   -->
				// { "@call": "BORDER_RESTRICTED5_RANDOM_LFB", 
				//		"terrain": "@eval terrain", 
				//		"adjacent": "@eval adjacent", 
				//		"layer": "@eval layer", 
				//		"flag": "@eval flag", 
				//		"builder": "@eval builder", 
				//		"imagestem": "@eval imagestem" }
				//current.top()->add_attr(attribute, line);
				auto strs = split(inner, " ", SplitFlags::ALLOW_EMPTY_STRINGS);
				current.emplace(current.top()->add_child(std::make_shared<node>("@merge")));
				current.top()->add_attr("@call", strs[0]);
				auto it = strs.cbegin() + 1;
				auto cache_it = cache.find(strs[0]);
				if(cache_it != cache.end()) {
					auto& params = cache_it->second->getParams();
					auto param_it = params.begin();
					ASSERT_LOG(params.size() == strs.size() - 1, "Non-matching number of parameters");
					while(it != strs.cend()) {
						std::string current_str = *it;
						if(boost::regex_match(current_str.c_str(), what, re_parens_match)) {
							current_str = std::string(what[1].first, what[1].second);
						}
						current.top()->add_attr(boost::to_lower_copy(*param_it), /*"@eval " + */current_str);
						++it;
						++param_it;
					}
				} else {
					for(const auto& str : strs) {
						if(boost::regex_match(str.c_str(), what, re_macro_match)) {
							current.top()->add_attr("@call", "@eval " + std::string(what[1].first, what[1].second));
						} else {
							ASSERT_LOG(false, "derp");
						}

					}
				}
				current.pop();
			} else {
				std::string value;
				auto pos = line.find_first_of('=');
				//ASSERT_LOG(pos != std::string::npos, "error no '=' " << line);
				if(pos != std::string::npos) {
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
				} else {
					// no '=' probably just a macro replace
					current.top()->add_attr(attribute, line);
				}
			}
		}
		return root;
	}


}

extern variant read_wml(const std::string& filename, const std::string& contents, int line_offset);

std::string convert_macro_string(const std::string& str)
{
	boost::match_results<std::string::const_iterator> what;
	auto start = str.cbegin();
	std::string composite = "@eval";
	bool found_macro = false;
	while(boost::regex_search(start, str.cend(), what, re_macro_match)) {
		//it->second->getParams();
		std::string param{ what[1].first, what[1].second };
		std::string prefix_str{ start, what[0].first };
		start = what[0].second;
		composite += (prefix_str.empty() ? "" : " + '" + prefix_str + "' +") + " " + boost::to_lower_copy(param);
		found_macro = true;
	}
	if(what[0].second != str.cend()) {
		// XXX
	}
	return found_macro ? composite : str;
}

void parse_terrain_files(const std::string& terrain_graphics_macros_dir, const std::string& terrain_graphics_file)
{
	sys::file_path_map fpm;
	sys::get_unique_files(terrain_graphics_macros_dir, fpm);
	for(const auto& p : fpm) {
		if(p.first.find(".cfg") != std::string::npos) {
			pre_process_wml(p.first, sys::read_file(p.second));
		}
	}

	const auto& cache = get_macro_cache();
	int n = 0;
	const int nend = 999999;
	for(auto it = cache.cbegin(); n != nend && it != cache.cend(); ++it, ++n) {
		std::stringstream ss;
		ss << it->first << "(";
		for(const auto& param : it->second->getParams()) {
			ss << param << ",";
		}
		ss << "); " << it->second->getDefinition();
		LOG_INFO(ss.str());

		node_ptr root = read_wml_macro("@macro " + it->first, it->second->getDefinition());
		std::stack<variant_builder> tags;
		tags.emplace();
		root->post_order_traversal<std::stack<variant_builder>>([](node_ptr n, std::stack<variant_builder>& tags) {
			tags.emplace();
		}, [it](node_ptr n, std::stack<variant_builder>& tags) {
			if(n->attributes().size() == 1) {
				auto old_vb = tags.top();
				tags.pop();
				const auto& p = *n->attributes().begin();
				tags.top().add(n->name(), convert_macro_string(p.second));
			} else {
				for(const auto& p : n->attributes()) {
					auto str = convert_macro_string(p.second);
					tags.top().add(p.first, str);
				}
				auto old_vb = tags.top();
				tags.pop();
				tags.top().add(n->name(), old_vb.build());
			}
		}, tags);
		variant terrain_graphics = tags.top().build();
		LOG_INFO(terrain_graphics.write_json(true, 4));

		//std::cout << "Press any key to continute...\n";
		//std::cin.get();
	}
}
