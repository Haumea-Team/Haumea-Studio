// SPDX-License-Identifier: GPL-2.0-or-later
// UPnP port forwarding (upnp.h), as the UPnP Forum's Internet Gateway Device
// specs have it: SSDP finds the router (an M-SEARCH to 239.255.255.250:1900),
// its device description names the WANIPConnection (or WANPPPConnection)
// service's control URL, and SOAP calls on it add and delete the mapping.
// HAUMEA_UPNP_LOCATION skips the search and uses that description (tests).
#include "mpprotocol.h"	// first: winsock2.h before windows.h
#include "haumea.h"
#include "upnp.h"

#include <condition_variable>
#include <mutex>
#include <thread>

static std::mutex gLock;
static std::condition_variable gWake;
static std::thread gThread;
static bool gStop;
static int gState = UPNP_OFF;
static std::string gExternal, gError;

static void
setState(int state, const std::string &error = "")
{
	std::lock_guard<std::mutex> lock(gLock);
	gState = state;
	gError = error;
}

/*
 * HTTP, as little as a router needs: one request per connection.
 */

namespace {
struct Url
{
	std::string host, path;
	int port = 80;
};
}

static bool
parseUrl(const std::string &text, Url *url)
{
	if(text.compare(0, 7, "http://") != 0)
		return false;
	size_t slash = text.find('/', 7);
	std::string hostPort = text.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
	url->path = slash == std::string::npos ? "/" : text.substr(slash);
	size_t colon = hostPort.find(':');
	url->host = hostPort.substr(0, colon);
	if(colon != std::string::npos)
		url->port = atoi(hostPort.c_str() + colon + 1);
	return !url->host.empty() && url->port > 0 && url->port < 65536;
}

static bool
waitFor(MpSocket s, bool write, double seconds)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	timeval timeout;
	timeout.tv_sec = (long)seconds;
	timeout.tv_usec = (long)((seconds - (long)seconds) * 1e6);
	return select((int)s + 1, write ? nil : &set, write ? &set : nil, nil, &timeout) > 0;
}

static std::string
dechunk(const std::string &body)
{
	std::string out;
	size_t at = 0;
	while(at < body.size()){
		size_t end = body.find("\r\n", at);
		if(end == std::string::npos)
			break;
		size_t size = strtoul(body.substr(at, end - at).c_str(), nil, 16);
		if(size == 0)
			break;
		out += body.substr(end + 2, size);
		at = end + 2 + size + 2;
	}
	return out;
}

// The response's status and body; localAddress: ours, as the router sees it.
static int
httpRequest(const Url &url, const std::string &request, std::string *body, std::string *localAddress)
{
	addrinfo hints = {}, *found = nil;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char service[16];
	snprintf(service, sizeof(service), "%d", url.port);
	if(getaddrinfo(url.host.c_str(), service, &hints, &found) != 0 || found == nil)
		return 0;
	MpSocket s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
	bool connected = s != MP_NO_SOCKET && MpSetNonBlocking(s) &&
		(connect(s, found->ai_addr, (int)found->ai_addrlen) == 0 || MpWouldBlock()) && waitFor(s, true, 3.0);
	freeaddrinfo(found);
	int error = 0;
	socklen_t length = sizeof(error);
	if(connected)
		getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&error, &length);
	if(!connected || error != 0){
		MpCloseSocket(s);
		return 0;
	}
	if(localAddress){
		sockaddr_in local = {};
		socklen_t size = sizeof(local);
		char text[64] = "";
		if(getsockname(s, (sockaddr*)&local, &size) == 0)
			inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
		*localAddress = text;
	}
	for(size_t sent = 0; sent < request.size();){
		if(!waitFor(s, true, 3.0)){
			MpCloseSocket(s);
			return 0;
		}
		int n = (int)send(s, request.data() + sent, (int)(request.size() - sent), MSG_NOSIGNAL);
		if(n <= 0 && !MpWouldBlock()){
			MpCloseSocket(s);
			return 0;
		}
		if(n > 0)
			sent += n;
	}
	std::string response;
	char buffer[16384];
	while(response.size() < (1u << 20) && waitFor(s, false, 5.0)){
		int n = (int)recv(s, buffer, sizeof(buffer), 0);
		if(n <= 0)
			break;
		response.append(buffer, n);
	}
	MpCloseSocket(s);
	size_t split = response.find("\r\n\r\n");
	if(response.compare(0, 5, "HTTP/") != 0 || split == std::string::npos)
		return 0;
	std::string head = response.substr(0, split);
	*body = response.substr(split + 4);
	std::string lower = head;
	for(char &c : lower)
		c = (char)tolower((unsigned char)c);
	if(lower.find("transfer-encoding: chunked") != std::string::npos)
		*body = dechunk(*body);
	size_t space = head.find(' ');
	return space == std::string::npos ? 0 : atoi(head.c_str() + space + 1);
}

static std::string
hostHeader(const Url &url)
{
	return url.host + ":" + std::to_string(url.port);
}

// The text of the first <tag> at or after from.
static std::string
element(const std::string &xml, const char *tag, size_t from = 0, size_t *end = nil)
{
	std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
	size_t a = xml.find(open, from);
	if(a == std::string::npos)
		return "";
	a += open.size();
	size_t b = xml.find(close, a);
	if(b == std::string::npos)
		return "";
	if(end)
		*end = b + close.size();
	return xml.substr(a, b - a);
}

/*
 * The router
 */

namespace {
struct Gateway
{
	Url control;
	std::string service;	// its serviceType
};
}

static std::vector<std::string>
searchRouters(void)
{
	std::vector<std::string> locations;
	if(const char *location = getenv("HAUMEA_UPNP_LOCATION")){
		locations.push_back(location);
		return locations;
	}
	MpSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(s == MP_NO_SOCKET)
		return locations;
	int ttl = 2;
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
	sockaddr_in to = {};
	to.sin_family = AF_INET;
	to.sin_port = htons(1900);
	inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
	for(const char *target : { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
	                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2" }){
		std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
			"MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + target + "\r\n\r\n";
		sendto(s, search.data(), (int)search.size(), 0, (sockaddr*)&to, sizeof(to));
	}
	double until = MpNow() + 2.5;
	char buffer[2048];
	while(MpNow() < until && waitFor(s, false, until - MpNow())){
		int n = (int)recv(s, buffer, sizeof(buffer) - 1, 0);
		if(n <= 0)
			break;
		buffer[n] = '\0';
		std::string reply = buffer, lower = reply;
		for(char &c : lower)
			c = (char)tolower((unsigned char)c);
		size_t at = lower.find("\nlocation:");
		if(at == std::string::npos)
			continue;
		size_t end = reply.find("\r\n", at + 1);
		std::string location = reply.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 10);
		location.erase(0, location.find_first_not_of(" \t"));
		if(std::find(locations.begin(), locations.end(), location) == locations.end())
			locations.push_back(location);
	}
	MpCloseSocket(s);
	return locations;
}

static bool
findGateway(Gateway *gateway)
{
	for(const std::string &location : searchRouters()){
		Url url;
		std::string body;
		if(!parseUrl(location, &url) ||
		   httpRequest(url, "GET " + url.path + " HTTP/1.1\r\nHost: " + hostHeader(url) +
			"\r\nConnection: close\r\n\r\n", &body, nil) != 200)
			continue;
		for(const char *service : { "urn:schemas-upnp-org:service:WANIPConnection:2",
		                            "urn:schemas-upnp-org:service:WANIPConnection:1",
		                            "urn:schemas-upnp-org:service:WANPPPConnection:1" }){
			size_t at = body.find(std::string("<serviceType>") + service + "</serviceType>");
			if(at == std::string::npos)
				continue;
			// Its controlURL is in the same <service>.
			size_t serviceEnd = body.find("</service>", at);
			std::string control = element(body, "controlURL", at);
			if(control.empty() || (serviceEnd != std::string::npos && body.find("<controlURL>", at) > serviceEnd))
				continue;
			std::string base = element(body, "URLBase");
			if(control.compare(0, 7, "http://") != 0){
				Url root = url;
				if(!base.empty())
					parseUrl(base, &root);
				control = "http://" + hostHeader(root) + (control[0] == '/' ? "" : "/") + control;
			}
			if(!parseUrl(control, &gateway->control))
				continue;
			gateway->service = service;
			return true;
		}
	}
	return false;
}

// A SOAP call; the response's body, or why not.
static bool
soap(const Gateway &gateway, const char *action, const std::string &arguments, std::string *response,
	std::string *localAddress = nil)
{
	std::string body = std::string("<?xml version=\"1.0\"?>\r\n"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
		"<u:") + action + " xmlns:u=\"" + gateway.service + "\">" + arguments + "</u:" + action +
		"></s:Body></s:Envelope>\r\n";
	std::string request = "POST " + gateway.control.path + " HTTP/1.1\r\nHost: " + hostHeader(gateway.control) +
		"\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gateway.service + "#" + action +
		"\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	int status = httpRequest(gateway.control, request, response, localAddress);
	if(status == 200)
		return true;
	std::string code = element(*response, "errorCode"), text = element(*response, "errorDescription");
	*response = status == 0 ? "the router didn't answer" :
		"the router said " + (code.empty() ? std::to_string(status) : code) + (text.empty() ? "" : " " + text);
	return false;
}

static bool
addMapping(const Gateway &gateway, int port, int lease, std::string *error)
{
	// Our address as the router sees it: what it forwards to.
	std::string local, response;
	soap(gateway, "GetExternalIPAddress", "", &response, &local);
	std::string external = element(response, "NewExternalIPAddress");
	std::string arguments = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
		"</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>" + std::to_string(port) +
		"</NewInternalPort><NewInternalClient>" + local + "</NewInternalClient><NewEnabled>1</NewEnabled>"
		"<NewPortMappingDescription>Haumea Studio multiplayer</NewPortMappingDescription>"
		"<NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
	if(local.empty() || !soap(gateway, "AddPortMapping", arguments, &response)){
		*error = local.empty() ? "the router didn't answer" : response;
		return false;
	}
	std::lock_guard<std::mutex> lock(gLock);
	gExternal = external;
	return true;
}

static void
deleteMapping(const Gateway &gateway, int port)
{
	std::string response;
	soap(gateway, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
		"</NewExternalPort><NewProtocol>TCP</NewProtocol>", &response);
}

static void
run(int port)
{
	Gateway gateway;
	if(!findGateway(&gateway)){
		setState(UPNP_FAILED, "no router here answers UPnP");
		return;
	}
	// An hour at a time, renewed, so a forward Haumea didn't get to take
	// away (a crash) goes by itself; some routers only keep permanent ones.
	int lease = 3600;
	std::string error;
	if(!addMapping(gateway, port, lease, &error)){
		lease = 0;
		if(!addMapping(gateway, port, lease, &error)){
			setState(UPNP_FAILED, error);
			return;
		}
	}
	setState(UPNP_OPEN);
	log("UPnP: port %d forwarded by the router (%s)\n", port, UpnpExternalAddress().c_str());
	std::unique_lock<std::mutex> lock(gLock);
	while(!gStop){
		gWake.wait_for(lock, std::chrono::seconds(lease ? lease / 2 : 3600));
		if(gStop || lease == 0)
			continue;
		lock.unlock();
		if(!addMapping(gateway, port, lease, &error))
			log("UPnP: renewing the forward: %s\n", error.c_str());
		lock.lock();
	}
	lock.unlock();
	deleteMapping(gateway, port);
	log("UPnP: port %d no longer forwarded\n", port);
}

void
UpnpOpenPort(int port)
{
	UpnpClosePort();
	if(!MpStartNetwork())
		return;
	// However the editor ends, the forward goes and the thread is joined.
	static bool registered = !atexit(UpnpClosePort);
	(void)registered;
	{
		std::lock_guard<std::mutex> lock(gLock);
		gStop = false;
		gState = UPNP_WORKING;
		gError.clear();
		gExternal.clear();
	}
	gThread = std::thread(run, port);
}

void
UpnpClosePort(void)
{
	if(!gThread.joinable())
		return;
	{
		std::lock_guard<std::mutex> lock(gLock);
		gStop = true;
	}
	gWake.notify_all();
	gThread.join();
	setState(UPNP_OFF);
}

int
GetUpnpState(void)
{
	std::lock_guard<std::mutex> lock(gLock);
	return gState;
}

std::string
UpnpExternalAddress(void)
{
	std::lock_guard<std::mutex> lock(gLock);
	return gExternal;
}

std::string
UpnpError(void)
{
	std::lock_guard<std::mutex> lock(gLock);
	return gError;
}
