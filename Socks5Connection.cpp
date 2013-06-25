/*
 * Socks5Connection.cpp
 *
 *  Created on: Jun 17, 2013
 *      Author: chris
 */

#include "Socks5Connection.h"
#include <deque>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <string>
#include <math.h>

#include "compat.h"
#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/buffer.h>
#include "bin.h"
#include "binmap.h"
#include "hashtree.h"
#include "avgspeed.h"
// Arno, 2012-05-21: MacOS X has an Availability.h :-(
#include "avail.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>

using namespace swift;

#define errorOut(...) {\
    fprintf(stderr, "%s:%d: %s():\t", __FILE__, __LINE__, __FUNCTION__);\
    fprintf(stderr, __VA_ARGS__);\
}

Socks5ConnectionState Socks5Connection::getCurrentState(){
	return this->state;
}

void Socks5Connection::setCurrentState(Socks5ConnectionState state){
	this->state = state;
}

int Socks5Connection::tryReadHandshakeResponse(struct bufferevent * bev){
	struct evbuffer * buff = bufferevent_get_input(bev);
	unsigned char * mem = evbuffer_pullup(buff,2);

	if(mem == NULL){
		/* Not enough data in the buffer */
		return 0;
	} else if(mem[0] != 5 || mem[1] != 0){
		/* Incorrect version or unsupported method */
		return -1;
	} else {
		evbuffer_drain(buff, 2);
		this->setCurrentState(MethodSelected);
		this->tryUdpAssociateRequest(bev);
		return 1;
	}


}

void Socks5Connection::tryUdpAssociateRequest(struct bufferevent * bev){
	unsigned char mem[10];
	mem[0] = 5;		// Version
	mem[1] = 3;		// UDP ASSOCIATE
	mem[2] = 0;		// RSV

	mem[3] = 1;		// Atype = IPv4

	mem[4] = 0;		// IPv4 address all zero since we want to sent to any possible address in the future
	mem[5] = 0;
	mem[6] = 0;
	mem[7] = 0;

	mem[8] = 0;		// Port zero as well for the same reason
	mem[9] = 0;

	bufferevent_write(bev, static_cast<const void *>(mem), 10);
	this->setCurrentState(UdpRequestSent);
}

void Socks5Connection::setBindAddress(Address address){
	this->bind_address = address;
}

Address Socks5Connection::getBindAddress(){
	return this->bind_address;
}

int Socks5Connection::tryReadUdpAssociateResponse(struct bufferevent * bev){
	struct evbuffer * buff = bufferevent_get_input(bev);
	unsigned char * mem = evbuffer_pullup(buff,4);

	Address bind_address;

	if(mem == NULL){
		/* Not enough data in the buffer */
		return 0;
	} else if(mem[0] != 5 || mem[1] != 0 || mem[2] != 0){
		/* Incorrect version or request not succeeded or RSV != 0 */
		return -1;
	} else {
		// We have an ipv4 address
		if(mem[3] == 1){
			mem = evbuffer_pullup(buff, 4 + 4 + 2);	// Pull up additional 4 bytes for the ipv4 address and 2 for the port

			if(mem == NULL)
				return 0;
			else{
				uint32_t ipv4;
				uint16_t port;

				memcpy(&ipv4, mem+4,4);
				memcpy(&port, mem+8,2);

				port = ntohs(port);
				ipv4 = ntohl(ipv4);

				bind_address.set_ipv4(ipv4);
				bind_address.set_port(port);

				evbuffer_drain(buff, 10);
			}
		} else if(mem[3] == 3){ // We have a fqdn
			mem = evbuffer_pullup(buff, 4 + 1);

			if(mem == NULL)
				return 0;
			else {
				unsigned char domain_length = mem[4];
				char * domain = new char[domain_length] ;
				uint16_t port;

				mem = evbuffer_pullup(buff, 4 + 1 + domain_length + 2); // Pull up domain and port

				if(mem == NULL)
					return 0;

				memcpy(&domain, mem + 5, domain_length);
				memcpy(&port, mem + 5 + domain_length, 2);

				port = ntohs(port);

				bind_address.set_ipv4(domain);
				bind_address.set_port(port);

				evbuffer_drain(buff, 4 + 1 + domain_length + 2);
				delete [] domain;
			}

		} else if (mem[3] == 4)
			return -1; // IPv6 not supported!


		this->setBindAddress(bind_address);
		this->setCurrentState(UdpAssociated);

		printf("Socks5 UDP proxy ready at %s:%d\n", bind_address.ipv4str(), bind_address.port());
		this->working_ = true;

//		this->setUDPsocket(AF_INET, SOCK_DGRAM, 0)

		return 1;
	}
	
	return 0;
}

int consumeUdpHeader(struct evbuffer *buff, UdpEncapsulationHeader& header){
	unsigned char * mem = evbuffer_pullup(buff,4);

	if(mem == NULL){
		/* Not enough data in the buffer */
		return 0;
	} else {
		memcpy(&header, mem, 4);

		// We have an ipv4 address
		if(header.atyp == 1){
			mem = evbuffer_pullup(buff, 4 + 4 + 2);	// Pull up additional 4 bytes for the ipv4 address and 2 for the port

			if(mem == NULL)
				return 0;
			else{
				uint32_t ipv4;
				uint16_t port;

				memcpy(&ipv4, mem+4,4);
				memcpy(&port, mem+8,2);

				ipv4 = ntohl(ipv4);
				port = ntohs(port);

				header.address.set_ipv4(ipv4);
				header.address.set_port(port);

				evbuffer_drain(buff, 10);

				return 10;
			}
		} else if(header.atyp == 3){ // We have a fqdn
			mem = evbuffer_pullup(buff, 4 + 1);

			if(mem == NULL)
				return 0;
			else {
				unsigned char domain_length = mem[4];
				char * domain = new char[domain_length] ;
				uint16_t port;

				mem = evbuffer_pullup(buff, 4 + 1 + domain_length + 2); // Pull up domain and port

				if(mem == NULL)
					return 0;

				memcpy(&domain, mem + 5, domain_length);
				memcpy(&port, mem + 5 + domain_length, 2);

				port = ntohs(port);

				header.address.set_ipv4(domain);
				header.address.set_port(port);

				evbuffer_drain(buff, 4 + 1 + domain_length + 2);
				delete [] domain;

				return 4 + 1 + domain_length + 2;
			}

		} else if (header.atyp == 4)
			return -1; // IPv6 not supported!
	}
}

int Socks5Connection::unwrapDatagram(Address & addr, struct evbuffer * evb){
	UdpEncapsulationHeader header;

	int udpHeaderRead = consumeUdpHeader(evb, header);

	if(udpHeaderRead <= 0)
		return -1;

	addr.set_ipv4(header.address.ipv4());
	addr.set_port(header.address.port());

	return sizeof(UdpEncapsulationHeader);
}

/**
 * Called by libevent when there is data to read.
 */
static void buffered_on_read(struct bufferevent *bev, void *cbarg) {

	Socks5Connection *s5 = static_cast<Socks5Connection *>(cbarg);

	switch(s5->getCurrentState()){
		case HandshakeSent:
			s5->tryReadHandshakeResponse(bev);
			return;
		case UdpRequestSent:
			s5->tryReadUdpAssociateResponse(bev);
			return;

	}
}

/**
 * Called by libevent when the write buffer reaches 0.  We only
 * provide this because libevent expects it, but we don't use it.
 */
static void buffered_on_write(struct bufferevent *bev, void *cbarg) {

}

/**
 * Called by libevent when the write buffer reaches 0.  We only
 * provide this because libevent expects it, but we don't use it.
 */
static void buffered_on_event(struct bufferevent *bev, short int what, void* cbarg) {

	// Sock5 server closed the connection!
	if((what & BEV_EVENT_EOF) ==  BEV_EVENT_EOF){
		Socks5Connection * s5 = static_cast<Socks5Connection *> (cbarg);
		s5->setCurrentState(Closed);
	}

	// Sock5 server closed the connection!
	if((what & BEV_EVENT_ERROR) ==  BEV_EVENT_ERROR){
		Socks5Connection * s5 = static_cast<Socks5Connection *> (cbarg);
		s5->setCurrentState(Closed);

		errorOut("Error occurred =(");
	}
}

void Socks5Connection::sendHandshake(struct bufferevent *bev){
	unsigned char buff[2];

	buff[0] = 0x05;
	buff[1] = 0x00;

	bufferevent_write(bev, static_cast<const void *>(buff), 2UL);
}

Socks5Connection::Socks5Connection(): Operational(true){
	this->state = Closed;
}

void Socks5Connection::open(struct event_base *evbase, Address socks5_server){
	working_ = false;

	struct bufferevent *bev;

	int addrlen = sizeof(struct sockaddr_in);

	bev = bufferevent_socket_new(evbase, -1, BEV_OPT_CLOSE_ON_FREE);
	bufferevent_setcb(bev, buffered_on_read, buffered_on_write, buffered_on_event, this);
	bufferevent_enable(bev, EV_READ|EV_WRITE);

	if(bufferevent_socket_connect(bev, (struct sockaddr*)&(socks5_server.addr), addrlen))
		errorOut("Cannot connect!");


	sendHandshake(bev);
	this->setCurrentState(HandshakeSent);
}

bool Socks5Connection::isOpen(){
	return this->getCurrentState() == 4;
}

int Socks5Connection::prependHeader(const Address & addr, struct evbuffer * evb){
	UdpEncapsulationHeader header;
	header.atyp = 1;
	header.fragment = 0;

	unsigned char * buff = new unsigned char[10];
	memcpy(buff, &header, 4);
	uint32_t ipv4 = htonl(addr.ipv4());
	uint16_t port = htons(addr.port());

	memcpy(buff + 4, &ipv4, 4);
	memcpy(buff + 8, &port, 2);

	evbuffer_prepend(evb, buff, 10);

	delete buff;

	//printf("Prepending Socks5 header to UDP packet destined to %s:%d \r\n", header.address.ipv4str(),header.address.port());

	return 10;
}
