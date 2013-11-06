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
/** IPv4/6 address, just a nice wrapping around struct sockaddr_storage. */
    struct Address {
    struct sockaddr_storage  addr;
    Address();
    Address(const char* ip, uint16_t port);
    /**IPv4 address as "ip:port" or IPv6 address as "[ip]:port" following
     * RFC2732, or just port in which case the address is set to in6addr_any */
    Address(const char* ip_port);
    Address(uint32_t ipv4addr, uint16_t port);
    Address(const struct sockaddr_storage& address) : addr(address) {}
    Address(struct in6_addr ipv6addr, uint16_t port);

    void set_ip   (const char* ip_str, int family);
    void set_port (uint16_t port);
    void set_port (const char* port_str);
    void set_ipv4 (uint32_t ipv4);
    void set_ipv4 (const char* ipv4_str);
    void set_ipv6 (const char* ip_str);
    void set_ipv6 (struct in6_addr &ipv6);
    void clear ();
    uint32_t ipv4() const;
    struct in6_addr ipv6() const;
    uint16_t port () const;
    operator sockaddr_storage () const {return addr;}
    bool operator == (const Address& b) const;
    std::string str () const;
    std::string ipstr (bool includeport=false) const;
    bool operator != (const Address& b) const { return !(*this==b); }
    bool is_private() const;
    int get_family() const { return addr.ss_family; }
    socklen_t get_real_sockaddr_length() const;
    };


typedef Address   Address;
}

#endif /* ADDRESS_H_ */
