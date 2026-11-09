#include "http.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define closesock closesocket
#define NOSIG 0
#define SHUT_WR SD_SEND
#define poll WSAPoll
#define CLOEXEC 0
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define closesock close
#define NOSIG MSG_NOSIGNAL
#define CLOEXEC SOCK_CLOEXEC // a program FRS starts (xdg-open, gcore) must not keep the port
#endif

namespace {

	using clk = std::chrono::steady_clock;

	struct Conn {
		std::thread th;
		std::shared_ptr<std::atomic<bool>> done;
		std::shared_ptr<std::atomic<int>> st; // 0 reading the request,1 fully read,2 dropped to make room
	};

	sock_t g_ls = INVALID_SOCKET;
	std::atomic<bool> g_run{false};
	std::thread g_th;
	std::function<http::Res(const http::Req&)> g_fn;
	std::vector<Conn> g_conns; // accept thread only

	const size_t MAXCONN = 16;

	const char* reason(int c) {
		switch (c) {
			case 200: return "OK";
			case 202: return "Accepted";
			case 400: return "Bad Request";
			case 401: return "Unauthorized";
			case 403: return "Forbidden";
			case 404: return "Not Found";
			case 405: return "Method Not Allowed";
			case 408: return "Request Timeout";
			case 411: return "Length Required";
			case 413: return "Payload Too Large";
			case 503: return "Service Unavailable";
		}
		return "Internal Server Error";
	}

	void reply(sock_t c,const http::Res& r) {
		std::string out = "HTTP/1.1 " + std::to_string(r.code) + " " + reason(r.code) + "\r\n";
		if (!r.body.empty()) out += "Content-Type: " + r.type + "\r\n";
		out += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n";
		for (auto& h : r.hdrs) out += h.first + ": " + h.second + "\r\n";
		out += "\r\n" + r.body;
		size_t off = 0;
		auto end = clk::now() + std::chrono::seconds(15);
		while (off < out.size()) {
			int n = (int)send(c,out.data() + off,(int)(out.size() - off),NOSIG);
			if (n <= 0) return;
			off += n;
			if (!g_run || clk::now() > end) return;
		}
	}

	void setTimeouts(sock_t c,int ms) {
#ifdef _WIN32
		DWORD to = (DWORD)ms;
#else
		timeval to{ms / 1000,(ms % 1000) * 1000};
#endif
		setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(const char*)&to,sizeof(to));
		setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,(const char*)&to,sizeof(to));
	}

	// waits in short steps so a stop is seen at once. <= 0 : closed, quiet for 5 s, past the deadline, stopping or dropped
	int rd(sock_t c,char* b,int n,clk::time_point end,const std::atomic<int>* st = nullptr) {
		auto quiet = clk::now() + std::chrono::seconds(5);
		while (g_run && (!st || *st != 2) && clk::now() < end && clk::now() < quiet) {
			pollfd pf{c,POLLIN,0};
			int r = poll(&pf,1,100);
			if (r < 0) return -1;
			if (r > 0) return (int)recv(c,b,n,0);
		}
		return -1;
	}

	// the request isn't fully read, closing now resets the connection and can eat the reply : half close, read what is left for a moment
	void replyEarly(sock_t c,const http::Res& r) {
		reply(c,r);
		shutdown(c,SHUT_WR);
		auto end = clk::now() + std::chrono::seconds(2);
		char tmp[4096];
		while (rd(c,tmp,sizeof(tmp),end) > 0) {}
	}

	// the client kept sending and ran out of the total time. Going quiet for 5 s stays a plain drop.
	void late(sock_t c,clk::time_point end) {
		if (clk::now() < end) return;
		http::Res r;
		r.code = 408;
		replyEarly(c,r);
	}

	void serve(sock_t c,const std::string& peer,std::atomic<int>& st) {
		// a client that goes quiet or trickles bytes must not hold a thread, or a stop, for long
		setTimeouts(c,5000);
		auto end = clk::now() + std::chrono::seconds(15);

		std::string buf;
		char tmp[4096];
		size_t he;
		while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
			if (buf.size() > 65536) return;
			int n = rd(c,tmp,sizeof(tmp),end,&st);
			if (n <= 0) return late(c,end);
			buf.append(tmp,n);
		}

		http::Req q;
		q.peer = peer;
		size_t eol = buf.find("\r\n");
		std::string line = buf.substr(0,eol);
		size_t a = line.find(' ');
		size_t b = line.find(' ',a + 1);
		if (a == std::string::npos || b == std::string::npos) return;
		q.method = line.substr(0,a);
		q.path = line.substr(a + 1,b - a - 1);
		q.path = q.path.substr(0,q.path.find('?'));

		size_t pos = eol + 2;
		while (pos < he) {
			size_t e = buf.find("\r\n",pos);
			std::string h = buf.substr(pos,e - pos);
			pos = e + 2;
			size_t col = h.find(':');
			if (col == std::string::npos) continue;
			std::string k = h.substr(0,col);
			std::string v = h.substr(col + 1);
			for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
			while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0,1);
			while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
			q.hdrs.push_back({k,v});
		}

		http::Res r;
		if (!q.hdr("transfer-encoding").empty()) {
			r.code = 411;
			return replyEarly(c,r);
		}
		size_t len = strtoul(q.hdr("content-length").c_str(),nullptr,10);
		if (len > (4u << 20)) {
			r.code = 413;
			return replyEarly(c,r);
		}
		q.body = buf.substr(he + 4);
		// a body trickled in slowly stays droppable, nobody's token has been checked yet
		while (q.body.size() < len) {
			int n = rd(c,tmp,sizeof(tmp),end,&st);
			if (n <= 0) return late(c,end);
			q.body.append(tmp,n);
		}
		int head = 0;
		if (!st.compare_exchange_strong(head,1)) return;
		q.body.resize(len);
		reply(c,g_fn(q));
	}

	void conn(sock_t c,const std::string& peer,std::atomic<int>& st) {
		try {
			serve(c,peer,st);
		} catch (...) {
			try {
				http::Res r;
				r.code = 500;
				r.body = "{\"error\":\"internal error\"}";
				reply(c,r);
			} catch (...) {}
		}
		shutdown(c,SHUT_WR);
		closesock(c);
	}

	// over the cap : read the request so closing doesn't reset the connection and eat the reply. Accept thread, so a short deadline.
	void refuse(sock_t c,int code,const char* msg) {
		setTimeouts(c,100);
		auto end = clk::now() + std::chrono::milliseconds(300);
		char tmp[4096];
		try {
			std::string buf;
			size_t left = std::string::npos; // body bytes to come, known once the headers are in
			while (left != 0 && clk::now() < end) {
				int n = (int)recv(c,tmp,sizeof(tmp),0);
				if (n <= 0) break;
				if (left != std::string::npos) {
					left -= std::min(left,(size_t)n);
					continue;
				}
				buf.append(tmp,n);
				size_t he = buf.find("\r\n\r\n");
				if (he == std::string::npos) {
					if (buf.size() > 65536) break;
					continue;
				}
				std::string h = buf.substr(0,he);
				for (auto& ch : h) ch = (char)tolower((unsigned char)ch);
				size_t p = h.find("\r\ncontent-length:");
				size_t len = p == std::string::npos ? 0 : strtoul(h.c_str() + p + 17,nullptr,10);
				size_t got = buf.size() - he - 4;
				left = len > got ? len - got : 0;
			}
			http::Res r;
			r.code = code;
			r.body = msg;
			if (code == 503) r.hdrs.push_back({"Retry-After","1"});
			reply(c,r);
		} catch (...) {}
		shutdown(c,SHUT_WR);
		closesock(c);
	}

	void reap() {
		for (size_t i = 0; i < g_conns.size();) {
			if (g_conns[i].done->load()) {
				g_conns[i].th.join();
				g_conns.erase(g_conns.begin() + i);
			} else i++;
		}
	}

	void loop() {
		while (g_run) {
			reap();
			fd_set rs;
			FD_ZERO(&rs);
			FD_SET(g_ls,&rs);
			timeval tv{0,200000};
			int n = select((int)g_ls + 1,&rs,nullptr,nullptr,&tv);
			if (n < 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
			if (n <= 0) continue;
			sockaddr_in sa = {};
			socklen_t sl = sizeof(sa);
#ifdef _WIN32
			sock_t c = accept(g_ls,(sockaddr*)&sa,&sl);
#else
			sock_t c = accept4(g_ls,(sockaddr*)&sa,&sl,CLOEXEC);
#endif
			if (c == INVALID_SOCKET) {
				std::this_thread::sleep_for(std::chrono::milliseconds(50)); // out of fds the listener stays readable, no busy loop
				continue;
			}
			reap();
			// full : the oldest connection still sending its request makes room, so slow or silent sockets can't lock real clients out.
			// Dropped ones end within 100 ms, twice the cap bounds the threads meanwhile
			size_t live = 0;
			for (auto& k : g_conns) if (k.st->load() != 2) live++;
			if (live >= MAXCONN) for (auto& k : g_conns) {
				int head = 0;
				if (k.st->compare_exchange_strong(head,2)) {
					live--;
					break;
				}
			}
			if (live >= MAXCONN || g_conns.size() >= MAXCONN * 2) {
				refuse(c,503,"{\"error\":\"too many connections\"}");
				continue;
			}
			// room first : once the thread runs it owns c and the push_back below can't throw
			try {
				char ip[INET_ADDRSTRLEN] = "";
				inet_ntop(AF_INET,&sa.sin_addr,ip,sizeof(ip));
				std::string peer = ip;
				g_conns.reserve(MAXCONN * 2);
				Conn k;
				k.done = std::make_shared<std::atomic<bool>>(false);
				k.st = std::make_shared<std::atomic<int>>(0);
				auto d = k.done;
				auto s = k.st;
				k.th = std::thread([c,d,s,peer] {
					conn(c,peer,*s);
					d->store(true);
				});
				g_conns.push_back(std::move(k));
			} catch (...) {
				refuse(c,500,"{\"error\":\"internal error\"}");
			}
		}
		for (auto& k : g_conns) k.th.join();
		g_conns.clear();
	}

}

namespace http {

	std::string Req::hdr(const char* k) const {
		for (auto& h : hdrs) if (h.first == k) return h.second;
		return "";
	}

	bool start(const char* ip,int port,std::function<Res(const Req&)> fn,std::string& err) {
		stop();
#ifdef _WIN32
		static bool wsa = false;
		if (!wsa) {
			WSADATA wd;
			wsa = WSAStartup(MAKEWORD(2,2),&wd) == 0;
		}
#endif
		sock_t s = socket(AF_INET,SOCK_STREAM | CLOEXEC,IPPROTO_TCP);
		if (s == INVALID_SOCKET) {
			err = "socket() failed";
			return false;
		}
#ifndef _WIN32
		int one = 1;
		setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#endif
		sockaddr_in sa = {};
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)port);
		inet_pton(AF_INET,ip,&sa.sin_addr);
		if (bind(s,(sockaddr*)&sa,sizeof(sa)) != 0 || listen(s,SOMAXCONN) != 0) {
			err = "port " + std::to_string(port) + " is busy or blocked";
			closesock(s);
			return false;
		}
		g_ls = s;
		g_fn = std::move(fn);
		g_run = true;
		try {
			g_th = std::thread(loop);
		} catch (...) {
			g_run = false;
			closesock(s);
			g_ls = INVALID_SOCKET;
			err = "could not start the server thread";
			return false;
		}
		return true;
	}

	// g_run first : handlers polling mcp::alive give up, the accept thread then joins them all
	void stop() {
		if (!g_th.joinable()) return;
		g_run = false;
		g_th.join();
		closesock(g_ls);
		g_ls = INVALID_SOCKET;
	}

	bool running() { return g_run; }

}
