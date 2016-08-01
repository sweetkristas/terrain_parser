#pragma once

#include <algorithm>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <stack>
#include <string>
#include <vector>

#include "variant.hpp"

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

extern macro_cache_type& get_macro_cache();

extern void parse_terrain_files(const std::string& terrain_graphics_macros_dir, const std::string& terrain_graphics_file);
extern void pre_process_wml(const std::string& filename, const std::string& contents);

enum class SplitFlags {
	NONE					= 0,
	ALLOW_EMPTY_STRINGS		= 1,
};

extern std::vector<std::string> split(const std::string& str, const std::string& delimiters, SplitFlags flags);

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

extern std::map<variant, variant> process_name_string(const std::string& s);
extern variant to_list_string(const std::string& s, const std::string& sep=",", SplitFlags flags=SplitFlags::NONE);
extern variant to_list_int(const std::string& s, const std::string& sep=",");
extern variant to_int(const std::string& s);
