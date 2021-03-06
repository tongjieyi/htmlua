#include "htmlua.h"
#include "HtmlParser.h"
#include <stdlib.h>
#include <string.h>

extern "C"{
	#include <lua.h>
	#include<lauxlib.h>
}

static void create_node(lua_State* L, liigo::HtmlNode* node);

class LuaHtmlParser : public liigo::HtmlParser {
private:
	lua_State* L;
	int refOnParseAttr, refOnNodeReady;
public:
	LuaHtmlParser(lua_State* L_, int refOnParseAttr_, int refOnNodeReady_) { this->L = L_; refOnParseAttr = refOnParseAttr_; refOnNodeReady = refOnNodeReady_; }
	virtual ~LuaHtmlParser() {}
	int get_refOnParseAttr() { return refOnParseAttr; }
	int get_refOnNodeReady() { return refOnNodeReady; }

public:
	//允许子类覆盖, 以便识别更多标签(提高解析质量), 或者识别更少标签(提高解析速度)
	//默认仅识别涉及HTML基本结构和信息的有限几个开始标签: A,IMG,META,BODY,TITLE,FRAME,IFRAME
	//onIdentifyHtmlTag()先于onParseAttributes()被调用
	virtual liigo::HtmlTagType onIdentifyHtmlTag(const char* szTagName, liigo::HtmlNodeType nodeType) {
		//因为LUA里面的table是哈希表，根据标签名称文本检索得到对应的标签类型，非常快捷，并且多识别还是少识别一些标签并无速度上的明显差别，
		//所以这个地方没有必要执行用户的回调函数了，直接自动识别所有HTML标签。
		int len = strlen(szTagName) + 1;
		char tagNameUpper[32]; //标签名称全大写，32空间足够
		for(int i = 0; i < len; i++) {
			tagNameUpper[i] = toupper(szTagName[i]);
		}
		lua_getglobal(L, "htmltag");
		lua_getfield(L, -1, tagNameUpper);
		liigo::HtmlTagType tagtype = lua_isnil(L, -1) ? liigo::TAG_UNKNOWN : (liigo::HtmlTagType)lua_tointeger(L, -1);
		lua_pop(L, 2); // pop htmltag[szTagName] and htmltag
		return tagtype;
	}

	//允许子类覆盖, 以便更好的解析节点属性, 或者部分解析甚至干脆不解析节点属性(提高解析速度)
	//可以根据标签名称(pNode->tagName)或标签类型(pNode->tagType)判断是否需要解析属性（parseAttributes()）
	//默认仅解析"已识别出标签类型"的标签属性（即pNode->tagType != TAG_UNKNOWN）
	virtual void onParseAttributes(liigo::HtmlNode* pNode) {
		if(refOnParseAttr != LUA_NOREF && refOnParseAttr != LUA_REFNIL) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, refOnParseAttr);
			if(lua_isfunction(L, -1)) {
				create_node(L, pNode);
				lua_call(L, 1, 1); // 参数为node, 返回true表示需要解析其属性, 否则不解析
				if(lua_toboolean(L, -1) == 1)
					liigo::HtmlParser::parseAttributes(pNode);
				lua_pop(L, 1);
				return;
			}
		}
		HtmlParser::onParseAttributes(pNode);
	}

	//允许子类覆盖, 在某节点解析完成后被调用, 如果返回false则立刻停止解析HTML
	virtual bool onNodeReady(liigo::HtmlNode* pNode) {
		if(refOnNodeReady != LUA_NOREF && refOnNodeReady != LUA_REFNIL) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, refOnNodeReady);
			if(lua_isfunction(L, -1)) {
				create_node(L, pNode);
				lua_call(L, 1, 1); // 参数为node, 返回true表示需要继续解析后续节点，否则终止解析
				bool r = lua_toboolean(L, -1) == 1;
				lua_pop(L, 1);
				return r;
			}
		}
		return true;
	}
};


struct LuaFunc {
	const char* name;
	int (*func) (lua_State*);
};

// create_lua_funcs_table() push a new created table on stack top, which contains all functions.
// if tname is no NULL, will save the table to Registry, to be take back later by using get_lua_funcs_table(L,tname).
// it is recommended to use a prefix for tname to avoid name confusion with other names in global Registry.
// by liigo, 20130906.
static void create_lua_funcs_table(lua_State* L, LuaFunc* funcs, const char* tname) {
	lua_newtable(L);
	int t = lua_gettop(L);
	while(funcs && funcs->name && funcs->func) {
		lua_pushcfunction(L, funcs->func);
		lua_setfield(L, t, funcs->name);
		funcs++;
	}
	if(tname) {
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, tname);
	}
}

// push the table on stack top
// returns nil if no prev create_lua_funcs_table() called with the same name.
static void get_lua_funcs_table(lua_State* L, const char* tname) {
	lua_getfield(L, LUA_REGISTRYINDEX, tname);
}

static void lua_print(lua_State* L, const char* msg) {
	lua_getglobal(L, "print");
	lua_pushstring(L, msg);
	lua_call(L, 1, 0);
}

static void report_lua_error(lua_State* L, const char* errmsg) {
	lua_pushstring(L, errmsg);
	lua_error(L);
}

// get parser from self parameter. ensure returned parser not NULL.
static liigo::HtmlParser* getparser(lua_State* L) {
	if(!lua_istable(L,1))
		report_lua_error(L, "require parser value as first parameter");
	lua_rawgeti(L, 1, 0); //@see html_newparser(). FIXME: use named key to check type? (integer key has most efficiency)
	liigo::HtmlParser* parser = (liigo::HtmlParser*) lua_touserdata(L, -1);
	lua_pop(L, 1);
	if(parser == NULL)
		report_lua_error(L, "invalid parser value");
	return parser;
}

// parse html text
// arg: parser (self), html (string), parseAttr (bool, default true)
// no returns, use parser:node(index) to get the parsed nodes.
static int parser_parse(lua_State* L) {
	const char* html = lua_tostring(L, 2);
	bool parseAttr = (lua_gettop(L) >= 3) ? (lua_toboolean(L, 3)==1) : true; //如果省略参数parseAttr, 默认为ture
	liigo::HtmlParser* parser = getparser(L);
	parser->parseHtml(html, parseAttr);
	return 0;
}

static int parser_node(lua_State* L);

// iterator function for parser's nodes, see parser_ipairs()
// returns: index+1, node
// arg: parser, index
static int parser_nextnode(lua_State* L) {
	lua_pushinteger(L, lua_tointeger(L, -1) + 1);
	lua_replace(L, -2); // change index to index+1
	parser_node(L);
	if(lua_isnil(L, -1)) { // if node is nil
		lua_pushnil(L);
		lua_replace(L, -3); // change index to nil, to break for loop
	}
	return 2;
}

// ipairs support for loop for parser's nodes: 
//   for index,node in parser:ipairs() do ...
// returns: a next() function, parser, 0
// arg: parser
static int parser_ipairs(lua_State* L) {
	if(lua_gettop(L) != 1 || !lua_istable(L, -1)) {
		report_lua_error(L, "ipairs() requires a parser as the only parameter");
	}
	lua_pushcfunction(L, parser_nextnode);
	lua_insert(L, -2);
	lua_pushinteger(L, 0);
	return 3;
}

// return node count
static int parser_nodecount(lua_State* L) {
	liigo::HtmlParser* parser = getparser(L);
	lua_pushinteger(L, parser->getHtmlNodeCount());
	return 1;
}

// nodeindex: 1 based
inline static bool isvalid_nodeindex(liigo::HtmlParser* parser, int nodeindex) {
	return (nodeindex >= 1 && nodeindex <= parser->getHtmlNodeCount());
}

// return HtmlNode* or NULL. nodeindex is 1 based
static liigo::HtmlNode* getparsernode(liigo::HtmlParser* parser, int nodeindex) {
	if(isvalid_nodeindex(parser, nodeindex)) {
		return parser->getHtmlNode(nodeindex - 1); // 0 based
	}
	return NULL;
}

// get node from self parameter. ensure returned node not NULL.
static liigo::HtmlNode* getnodeself(lua_State*L) {
	if(!lua_istable(L,1))
		report_lua_error(L, "require node value as first parameter");
	lua_rawgeti(L, 1, 0); //@see parser_node()
	liigo::HtmlNode* node = (liigo::HtmlNode*) lua_touserdata(L, -1);
	lua_pop(L, 1);
	if(node == NULL)
		report_lua_error(L, "invalid node value");
	return node;
}

static int node_attr(lua_State* L);

// iterator function for node's attributes, see node_pairs()
// returns: attrname, attrvalue
// arg: node, (ignor)
static int node_nextattr(lua_State* L) {
	lua_pop(L, 1);
	lua_rawgeti(L, -1, 1); // read iterate index at node[1]
	int index = lua_tointeger(L, -1);
	lua_pop(L, 1);
	lua_pushinteger(L, index + 1);
	lua_rawseti(L, -2, 1); // node[1] = index + 1
	lua_pushinteger(L, index + 1);
	node_attr(L); //return attr(index+1)
	return 2;
}

// ipairs support for loop for node's attributes: 
//   for index,node in parser:ipairs() do ...
// returns: a next() function, parser, nil
// arg: node
static int node_pairs(lua_State* L) {
	if(lua_gettop(L) != 1 || !lua_istable(L, -1)) {
		report_lua_error(L, "pairs() requires a node as the only parameter");
	}
	lua_pushinteger(L, 0);
	lua_rawseti(L, -2, 1); //node[1] = 0, store the iterate index
	lua_pushcfunction(L, node_nextattr);
	lua_insert(L, -2);
	lua_pushnil(L);
	return 3;
}

// return node's attribute:
//   value (string), if arg 2 is attrname (string);
//   name (string) and value (string), if arg 2 is attrindex (integer);
//   nil and nil if no such attribute;
// arg: node (self), attrname (string) or attrindex (integer)
static int node_attr(lua_State* L) {
	liigo::HtmlNode* node = getnodeself(L);

	const liigo::HtmlAttribute* attr = NULL;
	if(lua_isnumber(L, 2)) {
		int attrindex = lua_tointeger(L, 2) - 1;
		if(attrindex >= 0 && attrindex < node->attributeCount) {
			attr = liigo::HtmlParser::getAttribute(node, attrindex);
			if(attr) {
				lua_pushstring(L, attr->name);
				lua_pushstring(L, attr->value ? attr->value : "");
			} else {
				lua_pushnil(L);
				lua_pushnil(L);
			}
			return 2;
		}
	} else if(lua_isstring(L, 2)) {
		const char* attrname = lua_tostring(L, 2);
		attr = liigo::HtmlParser::getAttribute(node, attrname);
		if(attr)
			lua_pushstring(L, attr->value ? attr->value : "");
		else
			lua_pushnil(L);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnil(L);
	return 2;
}

// parse attributes from node.text, and store result to node.
// arg: node (self)
// no returns, no repeat parsing if the node attributes has been parsed before
static int node_parseattr(lua_State* L) {
	liigo::HtmlNode* node = getnodeself(L);
	liigo::HtmlParser::parseAttributes(node);
	return 0;
}

// return HtmlNode* or NULL. nodeindex is 1 based
// arg: parse, nodeindex
static liigo::HtmlNode* getnode(lua_State* L) {
	liigo::HtmlParser* parser = getparser(L);
	int nodeindex = lua_tointeger(L, 2);
	return getparsernode(parser, nodeindex);
}

// create node table, and push to stack top
// node's property:
//   type
//   text
//   tagtype
//   tagname
//   attrcount
//   iscdata
//   isselfclosing
// node's method
//   attr("name")
//   pairs(void)
//   parseattr(void)
static void create_node(lua_State* L, liigo::HtmlNode* node) {
	if(node == NULL) {
		lua_pushnil(L);
		return;
	}

	LuaFunc funcs[] = {
		{ "attr", node_attr },
		{ "pairs", node_pairs },
		{ "parseattr", node_parseattr },
		{ NULL, NULL }
	};

	create_lua_funcs_table(L, funcs, NULL); // the node table to be return

	lua_pushinteger(L, node->type);
	lua_setfield(L, -2, "type");

	lua_pushstring(L, node->text ? node->text : "");
	lua_setfield(L, -2, "text");

	lua_pushinteger(L, node->tagType);
	lua_setfield(L, -2, "tagtype");

	lua_pushstring(L, node->tagName);
	lua_setfield(L, -2, "tagname");

	lua_pushinteger(L, node->attributeCount);
	lua_setfield(L, -2, "attrcount");

	lua_pushboolean(L, node->flags & liigo::FLAG_SELF_CLOSING_TAG);
	lua_setfield(L, -2, "isselfclosing");

	lua_pushboolean(L, node->flags & liigo::FLAG_CDATA_BLOCK);
	lua_setfield(L, -2, "iscdata");

	lua_pushlightuserdata(L, node);
	lua_rawseti(L, -2, 0); // self[0] = node
}

// returns node (table) at nodeindex, or nil if no such node
// arg: parser (self), nodeindex (int)
static int parser_node(lua_State* L) {
	create_node(L, getnode(L));
	return 1;
}


// returns a new 'parser' (table value)
// arg: optional function 'onParseAttr', optional function 'onNodeReady'
//   bool onParseAttr(node)
//   bool onNodeReady(node)
static int html_newparser(lua_State* L) {
	LuaFunc funcs[] = {
		{ "parse", parser_parse },
		{ "ipairs", parser_ipairs },
		{ "node",  parser_node },
		{ "nodecount", parser_nodecount },
		{ NULL, NULL }
	};

	int refOnNodeReady = LUA_NOREF;
	if(lua_gettop(L) > 1 && lua_isfunction(L, -1)) {
		refOnNodeReady = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	int refOnParseAttr = LUA_NOREF;
	if(lua_gettop(L) > 0 && (lua_isfunction(L,-1))) {
		refOnParseAttr = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	liigo::HtmlParser* parser = new LuaHtmlParser(L, refOnParseAttr, refOnNodeReady);
	create_lua_funcs_table(L, funcs, NULL);
	lua_pushlightuserdata(L, parser);
	lua_rawseti(L, -2, 0); // self[0] = parser; @see getparser()
	return 1;
}

// arg: parser
static int html_deleteparser(lua_State* L) {
	LuaHtmlParser* parser = (LuaHtmlParser*) getparser(L);
	if(parser) {
		luaL_unref(L, LUA_REGISTRYINDEX, parser->get_refOnParseAttr());
		delete parser;
		lua_pushnil(L);
		lua_rawseti(L, -2, 0); // self[0] = NULL
	}
	return 0;
}

// arg: html table
static int html__gc(lua_State* L) {
	lua_getfield(L, -1, "parser");
	html_deleteparser(L);
	return 0;
}

static void define_htmlnode(lua_State* L);
static void define_htmltag(lua_State* L);

extern "C"
int luaopen_htmlua(lua_State* L) {
	LuaFunc funcs[] = {
		{ "newparser", html_newparser },
		{ "deleteparser", html_deleteparser },
		{ NULL, NULL }
	};
	LuaFunc metafuncs[] = {
		{ "__gc", html__gc },
		{ NULL, NULL }
	};

	create_lua_funcs_table(L, funcs, NULL); // the table 'html' to return
	int html = lua_gettop(L);
	create_lua_funcs_table(L, metafuncs, NULL); // the metatable
	lua_setmetatable(L, -2); // html.metatable = metatable

	// auto create a parser, ready to use
	lua_pushnil(L);
	lua_pushnil(L);
	html_newparser(L);
	lua_setfield(L, html, "parser"); // html.parser = new parser

	// define global consts
	define_htmlnode(L);
	define_htmltag(L);

	lua_settop(L, html);
	return 1;
}

struct TableFieldStrInt {
	const char* name;
	int value;
};

// define global table
// no arg, no return
static void define_global_table_strint(lua_State* L, const char* name, const TableFieldStrInt* fields, int count) {
	lua_createtable(L, 0, count);
	for(int i = 0; i < count; i++, fields++) {
		lua_pushinteger(L, fields->value);
		lua_setfield(L, -2, fields->name);
	};
	lua_setglobal(L, name);
}


// define global table htmlnod. usage: htmlnode.START_TAG
// no arg, no return
static void define_htmlnode(lua_State* L) {
	TableFieldStrInt fields[] = {
		{ "START_TAG", 1 }, // 开始标签，如 <a href=...>
		{ "END_TAG",   2 }, // 结束标签，如 </a>
		{ "CONTENT",   3 }, // 普通文本
		{ "REMARKS",   4 }, // 注释文本，<!-- ... -->
		{ "UNKNOWN",   5 }, // 未知节点
		{ "_USER_",   10 }, // 用户定义的其他标签类型值应大于_USER_，以确保不与上面定义的常量值重复
	};
	define_global_table_strint(L, "htmlnode", fields, sizeof(fields) / sizeof(fields[0]));
}

// define global table htmltag. usage: htmltag.DIV
// no arg, no return
static void define_htmltag(lua_State* L) {
	TableFieldStrInt fields[] = {
		{ "UNKNOWN",	0 }, // 表示未经识别的标签类型，参见HtmlParser.onIdentifyHtmlTag()
		{ "SCRIPT",		1 }, // 出于解析需要必须识别<script>,<style>和<textarea>，内部特别处理
		{ "STYLE",		2 }, 
		{ "TEXTAREA",	3 }, 
		{ "A",			11 }, // 以下按标签字母顺序排列, 来源：http://www.w3.org/TR/html4/index/elements.html (HTML4)
		{ "ABBR",		12 }, //  and http://www.w3.org/TR/html5/section-index.html#elements-1 (HTML5)
		{ "ACRONYM",	13 },
		{ "ADDRESS",	14 },
		{ "APPLET",		15 },
		{ "AREA",		16 },
		{ "ARTICLE",	17 },
		{ "ASIDE",		18 },
		{ "AUDIO",		19 },
		{ "B",			20 },
		{ "BASE",		21 },
		{ "BASEFONT",	22 },
		{ "BDI",		23 },
		{ "BDO",		24 },
		{ "BIG",		25 },
		{ "BLOCKQUOTE",	26 },
		{ "BODY",		27 },
		{ "BR",			28 },
		{ "BUTTON",		29 },
		{ "CAPTION",	30 },
		{ "CENTER",		31 },
		{ "CITE",		32 },
		{ "CODE",		33 },
		{ "COL",		34 },
		{ "COLGROUP",	35 },
		{ "COMMAND",	36 },
		{ "DATALIST",	37 },
		{ "DD",			38 },
		{ "DEL",		39 },
		{ "DETAILS",	40 },
		{ "DFN",		41 },
		{ "DIR",		42 },
		{ "DIV",		43 },
		{ "DL",			44 },
		{ "DT",			45 },
		{ "EM",			46 },
		{ "EMBED",		47 },
		{ "FIELDSET",	48 },
		{ "FIGCAPTION",	49 },
		{ "FIGURE",		50 },
		{ "FONT",		51 },
		{ "FOOTER",		52 },
		{ "FORM",		53 },
		{ "FRAME",		54 },
		{ "FRAMESET",	55 },
		{ "H1",			56 },
		{ "H2",			57 },
		{ "H3",			58 },
		{ "H4",			59 },
		{ "H5",			60 },
		{ "H6",			61 },
		{ "HEAD",		62 },
		{ "HEADER",		63 },
		{ "HGROUP",		64 },
		{ "HR",			65 },
		{ "HTML",		66 },
		{ "I",			67 },
		{ "IFRAME",		68 },
		{ "IMG",		69 },
		{ "INPUT",		70 },
		{ "INS",		71 },
		{ "ISINDEX",	72 },
		{ "KBD",		73 },
		{ "KEYGEN",		74 },
		{ "LABEL",		75 },
		{ "LEGEND",		76 },
		{ "LI",			77 },
		{ "LINK",		78 },
		{ "MAP",		79 },
		{ "MARK",		80 },
		{ "MENU",		81 },
		{ "META",		82 },
		{ "METER",		83 },
		{ "NAV",		84 },
		{ "NOFRAMES",	85 },
		{ "NOSCRIPT",	86 },
		{ "OBJECT",		87 },
		{ "OL",			88 },
		{ "OPTGROUP",	89 },
		{ "OPTION",		90 },
		{ "P",			91 },
		{ "PARAM",		92 },
		{ "PRE",		93 },
		{ "PROGRESS",	94 },
		{ "Q",			95 },
		{ "RP",			96 },
		{ "RT",			97 },
		{ "RUBY",		98 },
		{ "S",			99 },
		{ "SAMP",		100 },
		{ "SECTION",	101 },
		{ "SELECT",		102 },
		{ "SMALL",		103 },
		{ "SOURCE",		104 },
		{ "SPAN",		105 },
		{ "STRIKE",		106 },
		{ "STRONG",		107 },
		{ "SUB",		108 },
		{ "SUMMARY",	109 },
		{ "SUP",		110 },
		{ "TABLE",		111 },
		{ "TBODY",		112 },
		{ "TD",			113 },
		{ "TFOOT",		114 },
		{ "TH",			115 },
		{ "THEAD",		116 },
		{ "TIME",		117 },
		{ "TITLE",		118 },
		{ "TR",			119 },
		{ "TRACK",		120 },
		{ "TT",			121 },
		{ "U",			122 },
		{ "UL",			123 },
		{ "VAR",		124 },
		{ "VIDEO",		125 },
		{ "WBR",		126 },
		{ "_USER_",		150 }, // 用户定义的其他标签类型值应大于_USER_，以确保不与上面定义的常量值重复
	};
	define_global_table_strint(L, "htmltag", fields, sizeof(fields) / sizeof(fields[0]));
}
