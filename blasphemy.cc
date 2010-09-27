/* TODO
 *
 * - Make it accept Kerberos credentials over stdin, create a credential
 *   cache, and handle renewal (including giving an advance signal when
 *   your password has changed  -- like a week in advance)
 *   - Put the username on the command line, so you can see it in ps
 * - Make it build and run in EC2
 * - Make it talk directly to a message bus, instead of to stdio?
 */

#include <list>
#include <sstream>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>

#include "json/elements.h"
#include "json/reader.h"
#include "json/writer.h"

extern "C" {
#include <com_err.h>
#include <zephyr/zephyr.h>
}

#define FOR_EACH(it, x)                                                 \
  for (typeof((x).begin()) it = (x).begin() ;                           \
       it != (x).end();                                                 \
       ++it)

using namespace std;
using namespace json;

struct PendingRequest {
  string tag;
  ZUnique_Id_t uid;
};
list<PendingRequest> g_pendingRequests;

void comErrHook(const char *whoami, long errcode, const char *fmt, va_list ap);
void commandDie();
void commandSend(const Object& cmd);
void commandSubscribe(const Object& cmd, bool subscribe);
void dispatchCommand(const Object& msg);
void ensure(Code_t ret, const char *who);
void handleSignal(int sig);
void printError(string s);
void printResult(string tag, string result);
void readCommand();
void tryHandleMessage(const ZNotice_t& notice, struct sockaddr_in* from);
bool tryHandleResult(const ZNotice_t& notice);

void
ensure(Code_t ret, const char *who) {
  if (ret) {
    com_err("", ret, who);
    exit(1);
  }
}

// be triple sure that errors from inside, eg, libzephyr don't end up
// in stdout
void
comErrHook(const char *whoami, long errcode, const char *fmt, va_list ap) {
  char message[1024];
  vsnprintf(message, sizeof(message), fmt, ap);

  stringstream s;
  s << error_message(errcode) << " " << message;
  printError(s.str());
}

void
printError(string s) {
  Object error;
  error["type"] = String("error");
  error["message"] = String(s);
  Writer::Write(error, cout);
}

void
printResult(string tag, string result) {
  Object msg;
  msg["type"] = String("result");
  msg["tag"] = String(tag);
  msg["result"] = String(result);
  Writer::Write(msg, cout);
}

/*****************************************************************************/
/* Handling incoming commands                                                */
/*****************************************************************************/

void
readCommand() {
  static char buffer[128*1024];
  static char* ptr = buffer;

  if (ptr == buffer + sizeof(buffer)) {
    printError("Command too long");
    exit(1);
  }
  
  int amt = read(0, ptr, sizeof(buffer) - (ptr - buffer) - 1);
  if (amt < 0) {
    if (errno != EWOULDBLOCK) {
      perror("reading command");
      exit(1);
    }
  } else {
    ptr += amt;
    *(ptr) = 0;
  }

  // process each \n-terminated command
  while (true) {
    char *nl = strchr(buffer, '\n');
    if (!nl)
      break;
    *nl = 0;

    stringstream stream(buffer);
    Object msg;
    bool success = true;
    try {
      Reader::Read(msg, stream);
    } catch (Exception e) {
      success = false;
    }

    if (success)
      dispatchCommand(msg);
    else
      printError("Parse error");

    memmove(buffer, nl + 1, ptr - (nl + 1) + 1);
    ptr -= (nl - buffer + 1);
  }
}

void
dispatchCommand(const Object& msg) {
  string type;

  try {
    type = ((String)msg["type"]).Value();
  } catch (Exception e) {
    printError("Command must have a 'type' attribute and it must be a string");
    return;
  }

  if ("send" == type) {
    commandSend(msg);
  } else if ("subscribe" == type) {
    commandSubscribe(msg, true);
  } else if ("unsubscribe" == type) {
    commandSubscribe(msg, false);
  } else if ("quit" == type) {
    commandDie();
  } else {
    printError("Unrecognized command type");
  }
}

void
commandSend(const Object& cmd) {
  ZNotice_t notice;
  memset(&notice, '\0', sizeof(notice));

  // get all of these on the stack so we can .c_str them
  string tag;
  string klass, instance, recipient, opcode;
  string body;

  try {
    tag = ((String)cmd["tag"]).Value();
    klass = ((String)cmd["class"]).Value();
    instance = ((String)cmd["instance"]).Value();
    recipient = ((String)cmd["recipient"]).Value();

    ostringstream b;
    const Array& bodyParts = cmd["body"];
    Array::const_iterator it(bodyParts.Begin()), itEnd(bodyParts.End());
    for (; it != itEnd; it++)
      b << ((String)*it).Value() << (char)0;
    body = b.str();

    // opcode is optional
    try { opcode = ((String)cmd["opcode"]).Value(); }
    catch (Exception e) { }
  } catch (Exception e) {
    printError("Required parameter missing (or of wrong type)");
    return;
  }

  // fill in the constant fields
  notice.z_kind = ACKED;
  notice.z_port = 0;
  notice.z_class =  (char *)klass.c_str();
  notice.z_class_inst = (char *)instance.c_str();
  notice.z_recipient =  (char *)recipient.c_str();
  notice.z_opcode =  (char *)opcode.c_str();
  notice.z_sender = 0; // I trust this is unnecessary?
  notice.z_message =  (char *)body.data();
  notice.z_message_len = body.length();
  notice.z_default_format = (char *)"Class $class, Instance $instance:\nTo: @bold($recipient) at $time $date\nFrom: @bold($1) <$sender>\n\n$2";
  /* for a non-authed message:
     notice.z_default_format = "@bold(UNAUTHENTIC) Class $class, Instance $instance:\nTo: @bold($recipient) at $time $date\nFrom: @bold($1) <$sender>\n\n$2";
  */

  int ret;
  if ((ret = ZSendNotice(&notice, ZAUTH)) != ZERR_NONE) {
    printResult(tag, "ZSendNotice failed");
    // could get an error out of 'ret' with com_err..
    return;
  }

  PendingRequest p;
  p.tag = tag;
  p.uid = notice.z_uid;
  g_pendingRequests.push_back(p);
}

void
commandSubscribe(const Object& cmd, bool subscribe) {
  int subCount = 0;
  int subLength = 128;
  ZSubscription_t* subs =
    (ZSubscription_t*) malloc(sizeof(ZSubscription_t) * subLength);

  bool success = true;
  vector<string> strings; // lifetime control for c_str() data ...
  try {
    const Array& classes = cmd["classes"];
    Array::const_iterator it(classes.Begin()), itEnd(classes.End());
    for (; it != itEnd; it++) {
      if (subLength == subCount) {
	subLength *= 2;
	subs = (ZSubscription_t*) realloc(subs,
					  sizeof(ZSubscription_t) * subLength);
      }

      strings.push_back(((String)(*it)).Value());
      ZSubscription_t* sub = subs + subCount;
      sub->zsub_class = (char *)strings.back().c_str();
      sub->zsub_classinst = (char *)"*";
      sub->zsub_recipient = (char *)"*";
      subCount++;
    }
  }
  catch (Exception e) {
    printError("Missing fleld or bad types");
    success = false;
  }

  if (success) {
    if (subscribe)
      ensure(ZSubscribeTo(subs, subCount, 0), "ZSubscribeTo");
    else
      ensure(ZUnsubscribeTo(subs, subCount, 0), "ZSubscribeTo");
  }

  free(subs);
}

void
commandDie() {
  ZCancelSubscriptions(0);
  ZUnsetLocation();
  ZClosePort();
  exit(1);
}		 

/*****************************************************************************/
/* Handling incoming zephyrs                                                 */
/*****************************************************************************/

void
tryHandleMessage(const ZNotice_t& notice, struct sockaddr_in* from) {
  // XXX blocks!! maybe push this responsibility up to user?
  const char *from_host;
  struct hostent *he;
  he = gethostbyaddr((char *)&notice.z_sender_addr,
		     sizeof(notice.z_sender_addr), AF_INET);
  from_host = he ? he->h_name : inet_ntoa(notice.z_sender_addr);

  int auth = ZCheckAuthentication((ZNotice_t*)&notice, from);

  Object msg;
  msg["type"] = String("message");
  msg["sender"] = String(notice.z_sender);
  msg["class"] = String(notice.z_class);
  msg["instance"] = String(notice.z_class_inst);
  msg["recipient"] = String(notice.z_recipient);
  msg["opcode"] = String(notice.z_opcode);
  msg["fromhost"] = String(from_host);
  msg["time"] = Number(notice.z_time.tv_sec);

  switch (auth) {
  case ZAUTH_YES:
    msg["auth"] = String("YES");
    break;
  case ZAUTH_NO:
    msg["auth"] = String("NO");
    break;
  case ZAUTH_FAILED:
  default:
    msg["auth"] = String("FAILED");
    break;
  }

  // drop on floor:
  // notice.z_port, notice.z_kind

  Array body;
  const char* walk = notice.z_message;
  int left = notice.z_message_len;
  char* to_free = 0;

  // make sure there's a null on the end of z_message
  if (0 == left || walk[left-1] != 0) {
    to_free = (char *)malloc(left + 1);
    memcpy(to_free, walk, left);
    walk = to_free;
    to_free[left] = 0;
    left++;
  }

  while (left) {
    int len = strlen(walk) + 1;
    body.Insert(String(walk));
    walk += len;
    left -= len;
  }

  free(to_free);
  msg["body"] = body;
    
  Writer::Write(msg, cout);
}

bool
tryHandleResult(const ZNotice_t& notice) {
  if (ACKED == notice.z_kind)
    return false;

  FOR_EACH (it, g_pendingRequests) {
    if (ZCompareUID((ZUnique_Id_t *)&(notice.z_uid),
		    (ZUnique_Id_t *)&(it->uid))) {
      // found it..

      switch (notice.z_kind) {
      case SERVACK:
	if (!strcmp(notice.z_message, ZSRVACK_SENT))
	  printResult(it->tag, "SENT");
	else if (!strcmp(notice.z_message, ZSRVACK_NOTSENT))
	  printResult(it->tag, "NOTSENT");
	else
	  printResult(it->tag, "ERROR");
	break;
      case SERVNAK:
	printResult(it->tag, "NAK"); // probably authentication
	break;
      default:
	printResult(it->tag, "ERROR");
	break;
      }

      g_pendingRequests.erase(it); // NB: invalidates iterator
      return true;
    }
  }

  return false;
}

/*****************************************************************************/
/* main                                                                      */
/*****************************************************************************/

void
handleSignal(int sig) {
  commandDie();
}		 

int
main(int argc, char *argv[]) {
  set_com_err_hook(comErrHook);

  signal(SIGHUP, handleSignal);
  signal(SIGINT, handleSignal);
  signal(SIGQUIT, handleSignal);
  signal(SIGTERM, handleSignal);

  // nonblocking reads on stdin
  if (-1 == fcntl(0, F_GETFL, 0)) {
    perror("fcnt");
    exit(1);
  }

  // initialize library
  ensure(ZInitialize(), (char *)"ZInitialize");
  u_short port = 0;
  ensure(ZOpenPort(&port), (char *)"ZOpenPort");

  int zephyrFd;
  if ((zephyrFd = ZGetFD()) < 0) {
    perror("ZGetFD");
    exit(1);
  }

  // subscribe to personals (but no default subs..)
  {
    ZSubscription_t sub = {(char *)"%me%", (char *)"MESSAGE", (char *)"*"};
    sub.zsub_recipient = ZGetSender();

    Code_t ret;
    for (int retry = 0; retry < 3; retry++) {
      if ((ret = ZSubscribeToSansDefaults(&sub, 1, 0)) != ZERR_SERVNAK)
	break;
      sleep(2);
    }

    ensure(ret, "ZSubscribeTo");
  }

  // location is irrelevant in the modern age
  ensure(ZInitLocationInfo((char *)"cloud", (char *)"jsonzc"),
	 "ZInitLocationInfo");
  ensure(ZSetLocation((char *)"OPSTAFF"), "ZSetLocation");

  // however event loops are timeless..
  while (true) {
    while (ZPending()) {
      ZNotice_t notice;
      struct sockaddr_in from;
      ensure(ZReceiveNotice(&notice, &from), "ZReceiveNotice");

      if (!tryHandleResult(notice))
	tryHandleMessage(notice, &from);
      
      ZFreeNotice(&notice);
    }	

    struct timeval t, *timeout;
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(0, &fdset);	// stdin
    FD_SET(zephyrFd, &fdset);
   
    if (select(zephyrFd + 1, &fdset, 0, 0, 0) < 0) {
      if (errno == EAGAIN)
	continue;

      printError("select failed");
      sleep(1);
      continue;
    }

    if (FD_ISSET(0, &fdset))
      readCommand();
  }
}

/*****************************************************************************/
/*****************************************************************************/
