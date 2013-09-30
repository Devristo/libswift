/*
 * Socks5Connection.h
 *
 *  Created on: Jun 17, 2013
 *      Author: chris
 */

#ifndef SOCKS5CONNECTION_H_
#define SOCKS5CONNECTION_H_

#include "address.h"
using namespace swift;

struct UdpEncapsulationHeader {
	uint16_t rsv;
	unsigned char fragment;
	unsigned char atyp;
	Address address;
};


enum Socks5ConnectionState {
		Closed = 0,
		HandshakeSent = 1,
	    MethodSelected = 2,
	    UdpRequestSent = 3,
	    UdpAssociated = 4
	};

class Socks5Connection : public Operational{
public:
	Socks5Connection();
	~Socks5Connection();

	void open(struct event_base *evbase, Address socks5_server);
	bool isOpen();

	Socks5ConnectionState getCurrentState();
	void setCurrentState(Socks5ConnectionState state);

	Address getBindAddress();
	void setBindAddress(Address address);

	int tryReadHandshakeResponse(struct bufferevent *bev);
	void tryUdpAssociateRequest(struct bufferevent *bev);
	int tryReadUdpAssociateResponse(struct bufferevent *bev);

	int unwrapDatagram(Address& addr, struct evbuffer *evb);
	int prependHeader(const Address& addr, struct evbuffer *evb);
	
	static void buffered_on_event(struct bufferevent *bev, short int what, void* cbarg);
	static void buffered_on_write(struct bufferevent *bev, void *cbarg);
	static void buffered_on_read(struct bufferevent *bev, void *cbarg);

	int consumeUdpHeader(struct evbuffer *buff, UdpEncapsulationHeader& header);
	void connect();

private:
	struct event_base * eb;
	Socks5ConnectionState state;
	void sendHandshake(struct bufferevent *bev);
	Address bind_address;
	Address address;
	struct bufferevent * bev;
	static void retryOpen(evutil_socket_t fd, short what, void *arg);
};

typedef Socks5Connection   Socks5Connection;

#endif /* SOCKS5CONNECTION_H_ */
