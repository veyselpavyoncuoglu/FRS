#pragma once
#include <string>
#include <vector>
#include <utility>
#include <functional>

// Minimal blocking HTTP/1.1 server for the MCP endpoint. One listener per process,
// each connection gets its own thread (16 at once, the oldest still sending its headers makes room, else 503) and is always closed.
namespace http {

	struct Req {
		std::string method,path,body;
		std::string peer; // caller's IPv4 address
		std::vector<std::pair<std::string,std::string>> hdrs; // names lowercased
		std::string hdr(const char* k) const;
	};

	struct Res {
		int code = 200;
		std::string type = "application/json";
		std::string body;
		std::vector<std::pair<std::string,std::string>> hdrs;
	};

	bool start(const char* ip,int port,std::function<Res(const Req&)> fn,std::string& err);
	void stop();
	bool running();

}
