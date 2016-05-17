#include "wforce.hh"
#include "sstuff.hh"
#include "ext/json11/json11.hpp"
#include "ext/incbin/incbin.h"
#include "dolog.hh"
#include <thread>
#include <sstream>
#include "yahttp/yahttp.hpp"
#include "namespaces.hh"
#include <sys/time.h>
#include <sys/resource.h>
#include "ext/ctpl.h"
#include "base64.hh"
#include "blacklist.hh"
#include "twmap.hh"

using std::thread;

static int uptimeOfProcess()
{
  static time_t start=time(0);
  return time(0) - start;
}


bool compareAuthorization(YaHTTP::Request& req, const string &expected_password)
{
  // validate password
  YaHTTP::strstr_map_t::iterator header = req.headers.find("authorization");
  bool auth_ok = false;
  if (header != req.headers.end() && toLower(header->second).find("basic ") == 0) {
    string cookie = header->second.substr(6);

    string plain;
    B64Decode(cookie, plain);

    vector<string> cparts;
    stringtok(cparts, plain, ":");

    // this gets rid of terminating zeros
    auth_ok = (cparts.size()==2 && (0==strcmp(cparts[1].c_str(), expected_password.c_str())));
  }
  return auth_ok;
}

static void setLtAttrs(struct LoginTuple& lt, json11::Json& msg)
{
  using namespace json11;
  Json attrs = msg["attrs"];
  if (attrs.is_object()) {
    auto attrs_obj = attrs.object_items();
    for (auto it=attrs_obj.begin(); it!=attrs_obj.end(); ++it) {
      string attr_name = it->first;
      if (it->second.is_string()) {
	lt.attrs.insert(std::make_pair(attr_name, it->second.string_value()));
      }
      else if (it->second.is_array()) {
	auto av_list = it->second.array_items();
	std::vector<std::string> myvec;
	for (auto avit=av_list.begin(); avit!=av_list.end(); ++avit) {
	  myvec.push_back(avit->string_value());
	}
	lt.attrs_mv.insert(std::make_pair(attr_name, myvec));
      }
    }
  }
}

void allowLog(int retval, const std::string& msg, const LoginTuple& lt, const std::vector<pair<std::string, std::string>>& kvs) 
{
  std::ostringstream os;
  os << "allowLog " << msg << ": ";
  os << "allow=\"" << retval << "\" ";
  os << "remote=\"" << lt.remote.toString() << "\" ";
  os << "login=\"" << lt.login << "\" ";
  os << "attrs={";
  for (auto i= lt.attrs.begin(); i!=lt.attrs.end(); ++i) {
    os << i->first << "="<< "\"" << i->second << "\"";
    if (i != --(lt.attrs.end()))
      os << ", ";
  }
  for (auto i = lt.attrs_mv.begin(); i!=lt.attrs_mv.end(); ++i) {
    if (i == lt.attrs_mv.begin())
      os << ", ";
    os << i->first << "=[";
    std::vector<std::string> vec = i->second;
    for (auto j = vec.begin(); j!=vec.end(); ++j) {
      os << "\"" << *j << "\"";
      if (j != --(vec.end()))
	os << ", ";
    }
    os << "]";
    if (i != --(lt.attrs_mv.end()))
      os << ", ";
  }
  os << "} ";
  for (const auto& i : kvs) {
    os << i.first << "="<< "\"" << i.second << "\"" << " ";
  }
  infolog(os.str().c_str());
}

void addBLEntries(const std::vector<BlackListEntry>& blv, const char* key_name, json11::Json::array& my_entries)
{
  using namespace json11;
  for (auto i = blv.begin(); i != blv.end(); ++i) {
    Json my_entry = Json::object {
      { key_name, (std::string)i->key },   
      { "expiration", (std::string)boost::posix_time::to_simple_string(i->expiration)},
      { "reason", (std::string)i->reason }
    };
    my_entries.push_back(my_entry);
  }
}

struct WFConnection {
  WFConnection(int sock, const ComboAddress& ca, const std::string pass) : s(sock)
  {
    fd = sock;
    remote = ca;
    password = pass;
    closeConnection = false;
    inConnectionThread = false;
  }
  bool inConnectionThread;
  bool closeConnection;
  int fd;
  Socket s;
  ComboAddress remote;
  std::string password;
};

typedef std::vector<std::shared_ptr<WFConnection>> WFCArray;
static WFCArray sock_vec;
static std::mutex sock_vec_mutx;

void parseResetCmd(const YaHTTP::Request& req, YaHTTP::Response& resp)
{
  using namespace json11;
  Json msg;
  string err;
  msg=Json::parse(req.body, err);
  if (msg.is_null()) {
    resp.status=500;
    std::stringstream ss;
    ss << "{\"status\":\"failure\", \"reason\":\"" << err << "\"}";
    resp.body=ss.str();
  }
  else {
    try {
      bool haveIP=false;
      bool haveLogin=false;
      std::string en_type, en_login;
      ComboAddress en_ca;
      if (!msg["ip"].is_null()) {
	en_ca = ComboAddress(msg["ip"].string_value());
	haveIP = true;
      }
      if (!msg["login"].is_null()) {
	en_login = msg["login"].string_value();
	haveLogin = true;
      }
      if (haveLogin && haveIP) {
	en_type = "ip:login";
	bl_db.deleteEntry(en_ca, en_login);
      }
      else if (haveLogin) {
	en_type = "login";
	bl_db.deleteEntry(en_login);
      }
      else if (haveIP) {
	en_type = "ip";
	bl_db.deleteEntry(en_ca);
      }
	  
      if (!haveLogin && !haveIP) {
	resp.status = 415;
	resp.body=R"({"status":"failure", "reason":"No ip or login field supplied"})";
      }
      else {
	bool reset_ret;
	{
	  reset_ret = g_luamultip->reset(en_type, en_login, en_ca);
	}
	resp.status = 200;
	if (reset_ret)
	  resp.body=R"({"status":"ok"})";
	else
	  resp.body=R"({"status":"failure", "reason":"reset function returned false"})";
      }
    }
    catch(...) {
      resp.status=500;
      resp.body=R"({"status":"failure"})";
    }
  }
}

void parseReportCmd(const YaHTTP::Request& req, YaHTTP::Response& resp)
{
  using namespace json11;
  Json msg;
  string err;
  msg=Json::parse(req.body, err);
  if (msg.is_null()) {
    resp.status=500;
    std::stringstream ss;
    ss << "{\"status\":\"failure\", \"reason\":\"" << err << "\"}";
    resp.body=ss.str();
  }
  else {
    try {
      LoginTuple lt;
      lt.remote=ComboAddress(msg["remote"].string_value());
      lt.success=msg["success"].string_value() == "true"; // XXX this is wrong but works for dovecot
      lt.pwhash=msg["pwhash"].string_value();
      lt.login=msg["login"].string_value();
      setLtAttrs(lt, msg);
      lt.t=getDoubleTime();
      spreadReport(lt);
      g_stats.reports++;
      resp.status=200;
      {
	g_luamultip->report(lt);
      }

      resp.body=R"({"status":"ok"})";
    }
    catch(...) {
      resp.status=500;
      resp.body=R"({"status":"failure"})";
    }
  }
}

void parseAllowCmd(const YaHTTP::Request& req, YaHTTP::Response& resp)
{
  using namespace json11;
  Json msg;
  string err;
  msg=Json::parse(req.body, err);
  if (msg.is_null()) {
    resp.status=500;
    std::stringstream ss;
    ss << "{\"status\":\"failure\", \"reason\":\"" << err << "\"}";
    resp.body=ss.str();
  }
  else {
    LoginTuple lt;
    lt.remote=ComboAddress(msg["remote"].string_value());
    lt.success=msg["success"].bool_value();
    lt.pwhash=msg["pwhash"].string_value();
    lt.login=msg["login"].string_value();
    setLtAttrs(lt, msg);
    int status = -1;
    std::string ret_msg;
	
    // first check the built-in blacklists
    BlackListEntry ble;
    if (bl_db.getEntry(lt.remote, ble)) {
      std::vector<pair<std::string, std::string>> log_attrs = 
	{ { "expiration", boost::posix_time::to_simple_string(ble.expiration) } };
      allowLog(status, std::string("blacklisted IP"), lt, log_attrs);
      ret_msg = "Temporarily blacklisted IP Address - try again later";
    }
    else if (bl_db.getEntry(lt.login, ble)) {
      std::vector<pair<std::string, std::string>> log_attrs = 
	{ { "expiration", boost::posix_time::to_simple_string(ble.expiration) } };
      allowLog(status, std::string("blacklisted Login"), lt, log_attrs);	  
      ret_msg = "Temporarily blacklisted Login Name - try again later";
    }
    else if (bl_db.getEntry(lt.remote, lt.login, ble)) {
      std::vector<pair<std::string, std::string>> log_attrs = 
	{ { "expiration", boost::posix_time::to_simple_string(ble.expiration) } };
      allowLog(status, std::string("blacklisted IPLogin"), lt, log_attrs);	  	  
      ret_msg = "Temporarily blacklisted IP/Login Tuple - try again later";
    }
    else {
      AllowReturn ar;
      {
	ar=g_luamultip->allow(lt);
      }
      status = std::get<0>(ar);
      ret_msg = std::get<1>(ar);
      std::string log_msg = std::get<2>(ar);
      std::vector<pair<std::string, std::string>> log_attrs = std::get<3>(ar);

      // log the results of the allow function
      allowLog(status, log_msg, lt, log_attrs);
    }

    g_stats.allows++;
    if(status < 0)
      g_stats.denieds++;
    msg=Json::object{{"status", status}, {"msg", ret_msg}};
      
    resp.status=200;
    resp.body=msg.dump();
  }  
}

void parseStatsCmd(const YaHTTP::Request& req, YaHTTP::Response& resp)
{
  using namespace json11;
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);

  resp.status=200;
  Json my_json = Json::object {
    { "allows", (int)g_stats.allows },
    { "denieds", (int)g_stats.denieds },
    { "user-msec", (int)(ru.ru_utime.tv_sec*1000ULL + ru.ru_utime.tv_usec/1000) },
    { "sys-msec", (int)(ru.ru_stime.tv_sec*1000ULL + ru.ru_stime.tv_usec/1000) },
    { "uptime", uptimeOfProcess()},
    { "qa-latency", (int)g_stats.latency}
  };

  resp.status=200;
  resp.body=my_json.dump();
}

void parseGetBLCmd(const YaHTTP::Request& req, YaHTTP::Response& resp)
{
  using namespace json11;
  Json::array my_entries;

  std::vector<BlackListEntry> blv = bl_db.getIPEntries();
  addBLEntries(blv, "ip", my_entries);
  blv = bl_db.getLoginEntries();
  addBLEntries(blv, "login", my_entries);
  blv = bl_db.getIPLoginEntries();
  addBLEntries(blv, "iplogin", my_entries);
      
  Json ret_json = Json::object {
    { "bl_entries", my_entries }
  };
  resp.status=200;
  resp.body = ret_json.dump();  
}

void parseGetStatsCmd(const YaHTTP::Request& req, YaHTTP::Response& resp)
{
  using namespace json11;
  Json msg;
  string err;
  msg=Json::parse(req.body, err);
  if (msg.is_null()) {
    resp.status=500;
    std::stringstream ss;
    ss << "{\"status\":\"failure\", \"reason\":\"" << err << "\"}";
    resp.body=ss.str();
  }
  else {
    bool haveIP=false;
    bool haveLogin=false;
    std::string en_type, en_login;
    std::string key_name, key_value;
    TWKeyType lookup_key;
    bool is_blacklisted;
    ComboAddress en_ca;

    if (!msg["ip"].is_null()) {
      en_ca = ComboAddress(msg["ip"].string_value());
      haveIP = true;
    }
    if (!msg["login"].is_null()) {
      en_login = msg["login"].string_value();
      haveLogin = true;
    }
    if (haveLogin && haveIP) {
      key_name = "ip_login";
      key_value = en_ca.toString() + ":" + en_login;
      lookup_key = key_value;
      is_blacklisted = bl_db.checkEntry(en_ca, en_login);
    }
    else if (haveLogin) {
      key_name = "login";
      key_value = en_login;
      lookup_key = en_login;
      is_blacklisted = bl_db.checkEntry(en_login);
    }
    else if (haveIP) {
      key_name = "ip";
      key_value = en_ca.toString();
      lookup_key = en_ca;
      is_blacklisted = bl_db.checkEntry(en_ca);
    }
	  
    if (!haveLogin && !haveIP) {
      resp.status = 415;
      resp.body=R"({"status":"failure", "reason":"No ip or login field supplied"})";
    } 
    else {
      Json::object js_db_stats;
      {
	std::lock_guard<std::mutex> lock(dbMap_mutx);
	for (auto i = dbMap.begin(); i != dbMap.end(); ++i) {
	  std::string dbName = i->first;
	  std::vector<std::pair<std::string, int>> db_fields;
	  if (i->second.get_all_fields(lookup_key, db_fields)) {
	    Json::object js_db_fields;
	    for (auto j = db_fields.begin(); j != db_fields.end(); ++j) {
	      js_db_fields.insert(make_pair(j->first, j->second));
	    }
	    js_db_stats.insert(make_pair(dbName, js_db_fields));
	  }
	}
      }
      Json ret_json = Json::object {
	{ key_name, key_value },
	{ "blacklisted", is_blacklisted },
	{ "stats", js_db_stats }
      };
      resp.status=200;
      resp.body = ret_json.dump();  
    }
  }
}

static void connectionThread(int id, std::shared_ptr<WFConnection> wfc)
{
  using namespace json11;
  string line;
  string request;
  YaHTTP::Request req;
  bool keepalive = false;
  bool closeConnection=true;
  bool validRequest = true;

  if (!wfc)
    return;

  infolog("Webserver handling request from %s on fd=%d", wfc->remote.toStringWithPort(), wfc->fd);

  YaHTTP::AsyncRequestLoader yarl;
  yarl.initialize(&req);
  int timeout = 5; // XXX make this configurable
  wfc->s.setNonBlocking();
  bool complete=false;
  try {
    while(!complete) {
      int bytes;
      char buf[1024];
      bytes = wfc->s.readWithTimeout(buf, sizeof(buf), timeout);
      if (bytes > 0) {
	string data(buf, bytes);
	complete = yarl.feed(data);
      } else {
	// read error OR EOF
	validRequest = false;
	break;
      }
    }
    yarl.finalize();
  } catch (YaHTTP::ParseError &e) {
    // request stays incomplete
    infolog("Unparseable HTTP request from %s", wfc->remote.toStringWithPort());
    validRequest = false;
  } catch (NetworkError& e) {
    warnlog("Network error in web server: %s", e.what());
    validRequest = false;
  }

  if (validRequest) {
    string conn_header = req.headers["Connection"];
    if (conn_header.compare("keep-alive") == 0)
      keepalive = true;

    if (conn_header.compare("close") == 0)
      closeConnection = true;
    else if (req.version > 10 || ((req.version < 11) && (keepalive == true)))
      closeConnection = false;

    string command=req.getvars["command"];

    string callback;

    if(req.getvars.count("callback")) {
      callback=req.getvars["callback"];
      req.getvars.erase("callback");
    }

    req.getvars.erase("_"); // jQuery cache buster

    YaHTTP::Response resp;
    resp.headers["Content-Type"] = "application/json";
    if (closeConnection)
      resp.headers["Connection"] = "close";
    else if (keepalive == true)
      resp.headers["Connection"] = "keep-alive";
    resp.version = req.version;
    resp.url = req.url;

    string ctype = req.headers["Content-Type"];
    if (!compareAuthorization(req, wfc->password)) {
      errlog("HTTP Request \"%s\" from %s: Web Authentication failed", req.url.path, wfc->remote.toStringWithPort());
      resp.status=401;
      std::stringstream ss;
      ss << "{\"status\":\"failure\", \"reason\":" << "Unauthorized" << "}";
      resp.body=ss.str();
      resp.headers["WWW-Authenticate"] = "basic realm=\"wforce\"";
    }
    else if (command=="ping" && req.method=="GET") {
      resp.status = 200;
      resp.body=R"({"status":"ok"})";
    }
    else if ((command != "") && (ctype.compare("application/json") != 0)) {
      errlog("HTTP Request \"%s\" from %s: Content-Type not application/json", req.url.path, wfc->remote.toStringWithPort());
      resp.status = 415;
      std::stringstream ss;
      ss << "{\"status\":\"failure\", \"reason\":" << "\"Invalid Content-Type - must be application/json\"" << "}";
      resp.body=ss.str();
    }
    else if (command=="reset" && req.method=="POST") {
      parseResetCmd(req, resp);
    }
    else if(command=="report" && req.method=="POST") {
      parseReportCmd(req, resp);
    }
    else if(command=="allow" && req.method=="POST") {
      parseAllowCmd(req, resp);
    }
    else if(command=="stats") {
      parseStatsCmd(req, resp);
    }
    else if(command=="getBL") {
      parseGetBLCmd(req, resp);
    }
    else if(command=="getStats") {
      parseGetStatsCmd(req, resp);
    }
    else {
      // cerr<<"404 for: "<<resp.url.path<<endl;
      resp.status=404;
    }

    if(!callback.empty()) {
      resp.body = callback + "(" + resp.body + ");";
    }

    std::ostringstream ofs;
    ofs << resp;
    string done;
    done=ofs.str();
    writen2(wfc->fd, done.c_str(), done.size());
  }

  {
    std::lock_guard<std::mutex> lock(sock_vec_mutx);
    if (closeConnection) {
      wfc->closeConnection = true;
    }
    wfc->inConnectionThread = false;
    return;
  }
}

unsigned int g_num_worker_threads = WFORCE_NUM_WORKER_THREADS;

#include "poll.h"

void pollThread()
{
  ctpl::thread_pool p(g_num_worker_threads);

  for (;;) {
    // parse the array of sockets and create a pollfd array
    struct pollfd* fds;
    int num_fds=0;
    {
      std::lock_guard<std::mutex> lock(sock_vec_mutx);
      num_fds = sock_vec.size();
      fds = new struct pollfd [num_fds];
      if (!fds) {
	errlog("Cannot allocate memory in pollThread()");
	exit(-1);
      }
      int j=0;
      // we want to keep the symmetry between pollfds and sock_vec in terms of number and order of items
      // so rather than remove sockets which are not being processed we just don't set the POLLIN flag for them
      for (WFCArray::iterator i = sock_vec.begin(); i != sock_vec.end(); ++i, j++) {
	fds[j].fd = (*i)->fd;
	fds[j].events = 0;
	if (!((*i)->inConnectionThread)) {
	  fds[j].events |= POLLIN;
	}
      }
    }
    // poll with shortish timeout - XXX make timeout configurable
    int res = poll(fds, num_fds, 50);

    if (res < 0) {
      warnlog("poll() system call returned error (%d)", errno);
    }
    else {
      std::lock_guard<std::mutex> lock(sock_vec_mutx);
      for (int i=0; i<num_fds; i++) {
	// set close flag for connections that need closing
	if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
	  sock_vec[i]->closeConnection = true;
	}
	// process any connections that have activity
	else if (fds[i].revents & POLLIN) {
	  sock_vec[i]->inConnectionThread = true;
	  p.push(connectionThread, sock_vec[i]);
	}
      }
      // now erase any connections that are done with
      for (WFCArray::iterator i = sock_vec.begin(); i != sock_vec.end();) {
	if ((*i)->closeConnection == true) {
	  // this will implicitly close the socket through the Socket class destructor
	  sock_vec.erase(i);
	}
	else
	  ++i;
      }
    }
    delete[] fds;
  }
}

void dnsdistWebserverThread(int sock, const ComboAddress& local, const std::string& password)
{
  warnlog("Webserver launched on %s", local.toStringWithPort());
  auto localACL=g_ACL.getLocal();

  // spin up a thread to do the polling on the connections accepted by this thread
  thread t1(pollThread);
  t1.detach();

  for(;;) {
    try {
      ComboAddress remote(local);
      int fd = SAccept(sock, remote);
      vinfolog("Got connection from %s", remote.toStringWithPort());
      if(!localACL->match(remote)) {
	close(fd);
	continue;
      }
      {
	std::lock_guard<std::mutex> lock(sock_vec_mutx);
	sock_vec.push_back(std::make_shared<WFConnection>(fd, remote, password));
      }
    }
    catch(std::exception& e) {
      errlog("Had an error accepting new webserver connection: %s", e.what());
    }
  }
}
