#pragma once
#include "json.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <future>
#include <chrono>
#include <stdexcept>
#include <functional>

// MCP server (Streamable HTTP, plain JSON responses) so AI agents can watch and drive FRS.
// Tool handlers run on HTTP connection threads, several at once, anything touching app state goes through onUi().
namespace mcp {

	using Fn = std::function<Json(const Json& args)>; // throw std::runtime_error -> tool error

	// Read = reports state. Write = manages memory like clean does. Danger = touches memory contents
	// or files, off until the user turns it on, refused while LAN is on.
	enum Tier { Read,Write,Danger };

	void addTool(const char* name,const char* desc,const char* schema,bool ro,Fn fn); // the original tools, category Core
	void add(const char* cat,const char* name,const char* desc,const char* schema,Tier tier,Fn fn);
	bool init(); // after settings load, true when a new token was made (save settings)
	void shutdown();
	void pump(); // UI thread, every frame
	bool alive();
	void post(std::function<void()> fn);

	bool drawTab(); // true when the config changed
	bool drawConsent(); // Ask mode modal, every frame. true when the config changed
	bool askPending(); // a call waits for the Ask prompt, any thread
	std::string serialize();
	bool loadKV(const char* k,const char* v); // false when the key isn't mcp's

	Json bytes(uint64_t v); // null when NA
	// typed args, absent or null gives def, a wrong type throws "<k> must be a string / true or false / a number"
	std::string textArg(const Json& a,const char* k,const char* def);
	bool flagArg(const Json& a,const char* k,bool def);
	double numArg(const Json& a,const char* k,double def);
	// standard list result from args limit (default def, clamped to mx) and offset :
	// {count,total_count,offset,limit,truncated,<key>:[...]}
	Json paged(const std::vector<Json>& rows,const Json& args,int def,int mx,const char* key = "rows");

	// run fn on the UI thread and wait for its result. Tool handlers only : the UI thread would hang here,
	// and a sampler or task thread leaves fn's references dangling once the server is off. Those use post with by-value captures.
	template<class F> auto onUi(F fn) -> decltype(fn()) {
		auto task = std::make_shared<std::packaged_task<decltype(fn())()>>(fn);
		auto fut = task->get_future();
		post([task] { (*task)(); });
		while (fut.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
			if (!alive()) throw std::runtime_error("MCP server is stopping");
		}
		return fut.get();
	}

}
