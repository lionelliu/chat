#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unordered_set>

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "chat.h"
#include "server.h"
#include "screen.h"

/*
TODO:
* Convert chat_message into a class
*/

#define MAX_EVENTS 100

void Server::send_message_to_clients(int sock_client, std::string msg)
{
	std::lock_guard<std::mutex> lock(client_mutex);

	struct chat_message cm;
	// valgrind: uninit. bytes
	bzero(&cm, sizeof(cm));

	cm.type = SERVER_MESSAGE;
	std::copy(msg.begin(), msg.end(), cm.msg);

	for (auto c : clients)
	{
		// skip the client who sent the message
		if (c.sockid == sock_client)
			continue;

		do_verbose("server: send message to socket " + std::to_string(c.sockid));
		send(c.sockid, &cm, sizeof(cm), 0);
	}
}

ClientConn::ClientConn(int sockid, std::string name)
	: sockid(sockid)
	, nickname(name)
{}

Server::~Server()
{
	if (sock_server)
		close(sock_server);

	if (epollfd)
		close(epollfd);

	end_screen();
	std::cout << "Server socket closed" << std::endl;
}

Server* Server::getInstance()
{
	static Server instance;
	return &instance;
}

void Server::add_client(int sock_client, char* nickname)
{
	ClientConn c(sock_client, nickname);
	do_verbose("server: Connection successful socket " + std::to_string(sock_client));
	send_message_to_clients(sock_client, "User " + std::string(c.nickname) + " entered in the room");

	std::lock_guard<std::mutex> lock(client_mutex);
	clients.insert(c);
}

void Server::remove_client(int sock_client, char *nickname)
{
	ClientConn cli(sock_client, nickname);

	auto c = clients.find(cli);

	if (c != clients.end()) {
		do_verbose("server: socket " + std::to_string(c->sockid) + " closed. Removing from clients list");
		send_message_to_clients(sock_client, "User " + std::string(c->nickname) + " was disconnected from the room");
	}

	std::lock_guard<std::mutex> lock(client_mutex);
	clients.erase(cli);

	// close client socket
	close(sock_client);
}

bool Server::listUsers()
{
	std::lock_guard<std::mutex> lock(client_mutex);

	if (clients.size() > 0) {
		add_message("Connected users:");

		for (auto c : clients)
			add_message("\t" + c.nickname);
	} else {
		add_message("Without users connected");
	}

	return true;
}

bool Server::listCommands()
{
	add_message("Available commands:");
	for (auto m : m_callbacks)
		add_message("\t" + m.first);
	return true;
}

bool Server::exitCommand()
{
	std::exit(EXIT_SUCCESS);
}

bool Server::init()
{
	sock_server = socket(PF_INET, SOCK_STREAM, 0);

	if (sock_server == -1) {
		perror("socket");
		return false;
	}

	int opt =1;
	if (setsockopt(sock_server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) == -1) {
		perror("setsockopt");
		return false;
	}

	bzero(&server, sizeof(server));
	server.sin_family = PF_INET;
	server.sin_port = htons(CHAT_PORT);
	server.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock_server, (struct sockaddr *)&server, sizeof(server))) {
		perror("bind");
		return false;
	}

	if (listen(sock_server, 5)) {
		perror("listen");
		return false;
	}

	// register callbacks of commands
	m_callbacks.insert({{std::make_pair("/exit", &Server::exitCommand)}
				, {std::make_pair("/quit", &Server::exitCommand)}
				, {std::make_pair("/list", &Server::listUsers)}
				, {std::make_pair("/commands", &Server::listCommands)}
	});

	init_screen();

	return true;
}

void Server::handleMessages()
{
	struct epoll_event ev, events[MAX_EVENTS];
	// silences a warning in valgrind about uninit. bytes
	bzero(&ev, sizeof(struct epoll_event));

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		do_verbose("epoll_create error");
		return;
	}

	ev.events = EPOLLIN;
	ev.data.fd = sock_server;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock_server, &ev) == -1) {
		do_verbose("epoll_ctl: listen socket");
		return;
	}

	while (true) {
		int n = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (n == -1) {
			do_verbose("epoll_wait");
			return;
		}

		for (int i = 0; i < n; i++) {
			// handle new connections into server
			if (events[i].data.fd == sock_server) {
				struct sockaddr_in client;

				socklen_t client_len = sizeof(client);
				int sock_client = accept(sock_server, (struct sockaddr *)&client, &client_len);

				if (sock_client == -1) {
					do_verbose("accept");
					return;
				}

				//setnonblocking
				int flags = fcntl(sock_client, F_GETFL, 0);
				fcntl(sock_client, F_SETFL, flags | O_NONBLOCK);

				do_verbose("server: Connection accept socked " + std::to_string(sock_client));

				ev.events = EPOLLIN | EPOLLOUT;
				ev.data.fd = sock_client;
				if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock_client, &ev) == -1) {
					do_verbose("epoll_ctl: sock client");
					return;
				}
			} else {
				// handle existing connections
				struct chat_message cm;

				// get the client fd
				int sock_client = events[i].data.fd;

				int n = recv(sock_client, &cm, sizeof(cm), 0);

				if (n == 0) {
					remove_client(sock_client, cm.nickname);
					continue;
				}

				// we don't have data on this fd, skip for now
				if (n < 0 && errno == EAGAIN)
					continue;

				if (cm.type == REGISTER)
				{
					add_client(sock_client, cm.nickname);

				} else if (cm.type == SEND_MESSAGE) {
					do_verbose("server: Received msg user " + std::string(cm.nickname) +
							+ " socket " + std::to_string(sock_client)
							+ " size " + std::to_string(n)
							+ ": " + std::string(cm.msg));

					send_message_to_clients(sock_client, "[" + std::string(cm.nickname) + "]: " + std::string(cm.msg));
				}
			}
		}
	}
}

void Server::getUserInput()
{
	while (true) {
		std::string input = get_user_input();
		auto map_it = m_callbacks.find(input);
		if (map_it != m_callbacks.end())
			(this->*m_callbacks[input])();
	}
}

void sigHandler(int signal)
{
	(void)signal;
	std::exit(EXIT_SUCCESS);
}

int main()
{
	Server* server = Server::getInstance();

	std::signal(SIGINT, sigHandler);

	if (!server->init())
		std::exit(EXIT_FAILURE);

	add_message("Server started");

	std::thread handle_server(&Server::handleMessages, server);
	handle_server.detach();

	server->getUserInput();

	std::exit(EXIT_SUCCESS);
}
