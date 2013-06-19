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
		Closed,
		HandshakeSent,
	    MethodSelected,
	    UdpRequestSent,
	    UdpAssociated
	};

class Socks5Connection{
public:
	void open(struct event_base *evbase, Address socks5_server);
	bool isOpen();

	Socks5ConnectionState getCurrentState();
	void setCurrentState(Socks5ConnectionState state);

	Address getBindAddress();
	void setBindAddress(Address address);

	int tryReadHandshakeResponse(struct bufferevent *bev);
	int tryUdpAssociateRequest(struct bufferevent *bev);
	int tryReadUdpAssociateResponse(struct bufferevent *bev);

	int unwrapDatagram(Address& addr, struct evbuffer *evb);
	int prependHeader(const Address& addr, struct evbuffer *evb);

private:
	Socks5ConnectionState state;
	void sendHandshake(struct bufferevent *bev);
	Address bind_address;
};

typedef Socks5Connection   Socks5Connection;

#endif /* SOCKS5CONNECTION_H_ */
