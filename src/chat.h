#ifndef __CHAT_H__
#define __CHAT_H__

#define CHAT_PORT 9090

enum modes {
	REGISTER,
	SEND_MESSAGE,
	SERVER_MESSAGE
};

struct chat_message {
	int type;
	char msg[100];
	char nickname[15];
};

#ifdef ENABLE_VERBOSE
void do_verbose(std::string msg)
{
	add_message(msg);
}
#else
void do_verbose(std::string) {}
#endif

#endif //__CHAT_H__
