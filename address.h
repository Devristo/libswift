/*
 * address.h
 *
 *  Created on: Jun 19, 2013
 *      Author: chris
 */

#ifndef ADDRESS_H_
#define ADDRESS_H_

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

namespace swift {
/** IPv4 address, just a nice wrapping around struct sockaddr_in. */
    struct Address {
	struct sockaddr_in  addr;
	static uint32_t LOCALHOST;
	void set_port (uint16_t port) {
	    addr.sin_port = htons(port);
	}
	void set_port (const char* port_str) {
	    int p;
	    if (sscanf(port_str,"%i",&p))
		set_port(p);
	}
	void set_ipv4 (uint32_t ipv4) {
	    addr.sin_addr.s_addr = htonl(ipv4);
	}
	void set_ipv4 (const char* ipv4_str) ;
	//{    inet_aton(ipv4_str,&(addr.sin_addr));    }
	void clear () {
	    memset(&addr,0,sizeof(struct sockaddr_in));
	    addr.sin_family = AF_INET;
	}
	Address() {
	    clear();
	}
	Address(const char* ip, uint16_t port)  {
	    clear();
	    set_ipv4(ip);
	    set_port(port);
	}
	Address(const char* ip_port);
	Address(uint16_t port) {
	    clear();
	    set_ipv4((uint32_t)INADDR_ANY);
	    set_port(port);
	}
	Address(uint32_t ipv4addr, uint16_t port) {
	    clear();
	    set_ipv4(ipv4addr);
	    set_port(port);
	}
	Address(const struct sockaddr_in& address) : addr(address) {}
	uint32_t ipv4 () const { return ntohl(addr.sin_addr.s_addr); }
	uint16_t port () const { return ntohs(addr.sin_port); }
	operator sockaddr_in () const {return addr;}
	bool operator == (const Address& b) const {
	    return addr.sin_family==b.addr.sin_family &&
		addr.sin_port==b.addr.sin_port &&
		addr.sin_addr.s_addr==b.addr.sin_addr.s_addr;
	}
	const char* str () const {
		// Arno, 2011-10-04: not thread safe, replace.
	    static char rs[4][32];
	    static int i;
	    i = (i+1) & 3;
	    sprintf(rs[i],"%i.%i.%i.%i:%i",ipv4()>>24,(ipv4()>>16)&0xff,
		    (ipv4()>>8)&0xff,ipv4()&0xff,port());
	    return rs[i];
	}
	const char* ipv4str () const {
		// Arno, 2011-10-04: not thread safe, replace.
	    static char rs[4][32];
	    static int i;
	    i = (i+1) & 3;
	    sprintf(rs[i],"%i.%i.%i.%i",ipv4()>>24,(ipv4()>>16)&0xff,
		    (ipv4()>>8)&0xff,ipv4()&0xff);
	    return rs[i];
	}
	bool operator != (const Address& b) const { return !(*this==b); }
	bool is_private() const {
		// TODO IPv6
		uint32_t no = ipv4(); uint8_t no0 = no>>24,no1 = (no>>16)&0xff;
		if (no0 == 10) return true;
		else if (no0 == 172 && no1 >= 16 && no1 <= 31) return true;
		else if (no0 == 192 && no1 == 168) return true;
		else return false;
	}
    };

typedef Address   Address;
}

#endif /* ADDRESS_H_ */
