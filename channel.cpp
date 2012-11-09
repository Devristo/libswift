/*
 *  channel.cpp
 *  class representing a virtual connection to a peer. In addition,
 *  it contains generic functions for socket management (see sock_open
 *  class variable)
 *
 *  Created by Victor Grishchenko on 3/6/09.
 *  Copyright 2009-2012 TECHNISCHE UNIVERSITEIT DELFT. All rights reserved.
 *
 */

#include <cassert>
#include "compat.h"
#include "swift.h"

using namespace std;
using namespace swift;

/*
 * Class variables
 */

swift::tint now_t::now = Channel::Time();
tint Channel::start = now_t::now;
tint Channel::epoch = now_t::now/360000000LL*360000000LL; // make logs mergeable
uint64_t Channel::global_dgrams_up=0, Channel::global_dgrams_down=0,
         Channel::global_raw_bytes_up=0, Channel::global_raw_bytes_down=0,
         Channel::global_bytes_up=0, Channel::global_bytes_down=0;
sckrwecb_t Channel::sock_open[] = {};
int Channel::sock_count = 0;
swift::tint Channel::last_tick = 0;
int Channel::MAX_REORDERING = 4;
bool Channel::SELF_CONN_OK = false;
swift::tint Channel::TIMEOUT = TINT_SEC*60;
channels_t Channel::channels(1);
Address Channel::tracker;
FILE* Channel::debug_file = NULL;
tint Channel::MIN_PEX_REQUEST_INTERVAL = TINT_SEC;

/*
 * Instance methods
 */

Channel::Channel(ContentTransfer* transfer, int socket, Address peer_addr,bool peerissource) :
    // Arno, 2011-10-03: Reordered to avoid g++ Wall warning
    peer_(peer_addr), socket_(socket==INVALID_SOCKET?default_socket():socket), // FIXME
    transfer_(transfer), peer_channel_id_(0), own_id_mentioned_(false),
    data_in_(TINT_NEVER,bin_t::NONE), data_in_dbl_(bin_t::NONE),
    data_out_cap_(bin_t::ALL),hint_out_size_(0),
    // Gertjan fix 996e21e8abfc7d88db3f3f8158f2a2c4fc8a8d3f
    // "Changed PEX rate limiting to per channel limiting"
    pex_requested_(false),  // Ric: init var that wasn't initialiazed
    last_pex_request_time_(0), next_pex_request_time_(0),
    pex_request_outstanding_(false),
    useless_pex_count_(0),
    rtt_avg_(TINT_SEC), dev_avg_(0), dip_avg_(TINT_SEC),
    last_send_time_(0), last_recv_time_(0), last_data_out_time_(0), last_data_in_time_(0),
    last_loss_time_(0), next_send_time_(0), open_time_(NOW), cwnd_(1),
    cwnd_count1_(0), send_interval_(TINT_SEC),
    send_control_(PING_PONG_CONTROL), sent_since_recv_(0),
    lastrecvwaskeepalive_(false), lastsendwaskeepalive_(false), // Arno: nap bug fix
    ack_rcvd_recent_(0),
    ack_not_rcvd_recent_(0), owd_min_bin_(0), owd_min_bin_start_(NOW),
    owd_cur_bin_(0), dgrams_sent_(0), dgrams_rcvd_(0),
    raw_bytes_up_(0), raw_bytes_down_(0), bytes_up_(0), bytes_down_(0),
    scheduled4del_(false),
    direct_sending_(false),
    peer_is_source_(peerissource),
    hs_in_(NULL),
    hs_out_(NULL)
{
    if (peer_==Address())
        peer_ = tracker;
  
    this->id_ = channels.size();
    channels.push_back(this);

    for(int i=0; i<4; i++) {
        owd_min_bins_[i] = TINT_NEVER;
        owd_current_[i] = TINT_NEVER;
    }
    evsend_ptr_ = new struct event;
    evtimer_assign(evsend_ptr_,evbase,&Channel::LibeventSendCallback,this);
    evtimer_add(evsend_ptr_,tint2tv(next_send_time_));

    //LIVE
    evsendlive_ptr_ = NULL;

    // RATELIMIT
    transfer_->GetChannels()->push_back(this);

    hs_out_ = new Handshake();
    if (transfer_->ttype() == FILE_TRANSFER)
	hs_out_->cont_int_prot_ = POPT_CONT_INT_PROT_MERKLE;
    else
	hs_out_->cont_int_prot_ = POPT_CONT_INT_PROT_NONE; // PPSPTODO implement live schemes

    dprintf("%s #%u init channel %s transfer %d\n",tintstr(),id_,peer_.str(), transfer_->td() );
    //fprintf(stderr,"new Channel %d %s\n", id_, peer_.str() );
}


Channel::~Channel () {
    dprintf("%s #%u dealloc channel\n",tintstr(),id_);
    channels[id_] = NULL;
    ClearEvents();

    // RATELIMIT
    if (transfer_ != NULL)
    {
        channels_t::iterator iter;
        channels_t *channels = transfer_->GetChannels();
        for (iter=channels->begin(); iter!=channels->end(); iter++)
        {
           if (*iter == this)
               break;
        }
        channels->erase(iter);
    }

    if (hs_in_ != NULL)
	delete hs_in_;
    if (hs_out_ != NULL)
	delete hs_out_;
}


void Channel::ClearEvents()
{
    if (evsend_ptr_ != NULL) {
        if (evtimer_pending(evsend_ptr_,NULL))
            evtimer_del(evsend_ptr_);
        delete evsend_ptr_;
        evsend_ptr_ = NULL;
    }
    if (evsendlive_ptr_ != NULL) {
        if (evtimer_pending(evsendlive_ptr_,NULL))
            evtimer_del(evsendlive_ptr_);
        delete evsendlive_ptr_;
        evsendlive_ptr_ = NULL;
    }
}

HashTree * Channel::hashtree()
{
    if (transfer()->ttype() == LIVE_TRANSFER)
        return NULL;
    else
        return ((FileTransfer *)transfer_)->hashtree();
}

bool Channel::IsComplete() {
    // Check if peak hash bins are filled.
    if (hashtree()->peak_count() == 0)
        return false;

    for(int i=0; i<hashtree()->peak_count(); i++) {
        bin_t peak = hashtree()->peak(i);
        if (!ack_in_.is_filled(peak))
            return false;
    }
    return true;
}



uint16_t Channel::GetMyPort() {
    struct sockaddr_in mysin = {};
    socklen_t mysinlen = sizeof(mysin);
    if (getsockname(socket_, (struct sockaddr *)&mysin, &mysinlen) < 0)
    {
        print_error("error on getsockname");
        return 0;
    }
    else
        return ntohs(mysin.sin_port);
}

bool Channel::IsDiffSenderOrDuplicate(Address addr, uint32_t chid)
{
    if (peer() != addr)
    {
        // Got message from different address than I send to
        //
        if (!own_id_mentioned_ && addr.is_private()) {
            // Arno, 2012-02-27: Got HANDSHAKE reply from IANA private address,
            // check for duplicate connections:
            //
            // When two peers A and B are behind the same firewall, they will get
            // extB, resp. extA addresses from the tracker. They will both
            // connect to their counterpart but because the incoming packet
            // will be from the intNAT address the duplicates are not
            // recognized.
            //
            // Solution: when the second datagram comes in (HANDSHAKE reply),
            // see if you have had a first datagram from the same addr
            // (HANDSHAKE). If so, close the channel if his port number is
            // larger than yours (such that one channel remains).
            //
            recv_peer_ = addr;

            Channel *c = transfer()->FindChannel(addr,this);
            if (c == NULL)
                return false;
  
            // I already initiated a connection to this peer,
            // this new incoming message would establish a duplicate.
            // One must break the connection, decide using port
            // number:
            dprintf("%s #%u found duplicate channel to %s\n",
                    tintstr(),chid,addr.str());

            if (addr.port() > GetMyPort()) {
                dprintf("%s #%u closing duplicate channel to %s\n",
                    tintstr(),chid,addr.str());
                return true;
            }
        }
        else
        {
            // Received HANDSHAKE reply from other address than I sent
            // HANDSHAKE to, and the address is not an IANA private
            // address (=no NAT in play), so close.
            dprintf("%s #%u invalid peer address %s!=%s\n",
                    tintstr(),chid,peer().str(),addr.str());
            return true;
        }
    }
    return false;
}







/*
 * Class methods
 */
tint Channel::Time () {
    //HiResTimeOfDay* tod = HiResTimeOfDay::Instance();
    //tint ret = tod->getTimeUSec();
    //DLOG(INFO)<<"now is "<<ret;
    return now_t::now = usec_time();
}

// SOCKMGMT
evutil_socket_t Channel::Bind (Address address, sckrwecb_t callbacks) {
    struct sockaddr_in addr = address;
    evutil_socket_t fd;
    int len = sizeof(struct sockaddr_in), sndbuf=1<<20, rcvbuf=1<<20;
    #define dbnd_ensure(x) { if (!(x)) { \
        print_error("binding fails"); close_socket(fd); return INVALID_SOCKET; } }
    dbnd_ensure ( (fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0 );
    dbnd_ensure( make_socket_nonblocking(fd) );  // FIXME may remove this
    int enable = true;
    dbnd_ensure ( setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                             (setsockoptptr_t)&sndbuf, sizeof(int)) == 0 );
    dbnd_ensure ( setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                             (setsockoptptr_t)&rcvbuf, sizeof(int)) == 0 );
    //setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (setsockoptptr_t)&enable, sizeof(int));
    dbnd_ensure ( ::bind(fd, (sockaddr*)&addr, len) == 0 );

    callbacks.sock = fd;
    sock_open[sock_count++] = callbacks;
    return fd;
}

Address Channel::BoundAddress(evutil_socket_t sock) {

    struct sockaddr_in myaddr;
    socklen_t mylen = sizeof(myaddr);
    int ret = getsockname(sock,(sockaddr*)&myaddr,&mylen);
    if (ret >= 0) {
        return Address(myaddr);
    }
    else {
        return Address();
    }
}


Address swift::BoundAddress(evutil_socket_t sock) {
    return Channel::BoundAddress(sock);
}


int Channel::SendTo (evutil_socket_t sock, const Address& addr, struct evbuffer *evb) {

    int length = evbuffer_get_length(evb);
    int r = sendto(sock,(const char *)evbuffer_pullup(evb, length),length,0,
                   (struct sockaddr*)&(addr.addr),sizeof(struct sockaddr_in));
    // SCHAAP: 2012-06-16 - How about EAGAIN and EWOULDBLOCK? Do we just drop the packet then as well?
    if (r<0) {
        print_error("can't send");
        evbuffer_drain(evb, length); // Arno: behaviour is to pretend the packet got lost
    }
    else
        evbuffer_drain(evb,r);
    global_dgrams_up++;
    global_raw_bytes_up+=length;
    Time();
    return r;
}

int Channel::RecvFrom (evutil_socket_t sock, Address& addr, struct evbuffer *evb) {
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct evbuffer_iovec vec;
    if (evbuffer_reserve_space(evb, SWIFT_MAX_RECV_DGRAM_SIZE, &vec, 1) < 0) {
        print_error("error on evbuffer_reserve_space");
        return 0;
    }
    int length = recvfrom (sock, (char *)vec.iov_base, SWIFT_MAX_RECV_DGRAM_SIZE, 0,
               (struct sockaddr*)&(addr.addr), &addrlen);
    if (length<0) {
        length = 0;

        // Linux and Windows report "ICMP port unreachable" if the dest port could
        // not be reached:
        //    http://support.microsoft.com/kb/260018
        //    http://www.faqs.org/faqs/unix-faq/socket/
#ifdef _WIN32
        if (WSAGetLastError() == 10054) // Sometimes errno == 2 ?!
#else
        if (errno == ECONNREFUSED)
#endif
        {
            CloseChannelByAddress(addr);
        }
        else
            print_error("error on recv");
    }
    vec.iov_len = length;
    if (evbuffer_commit_space(evb, &vec, 1) < 0)  {
        length = 0;
        print_error("error on evbuffer_commit_space");
    }
    global_dgrams_down++;
    global_raw_bytes_down+=length;
    Time();
    return length;
}


void Channel::CloseSocket(evutil_socket_t sock) {
    for(int i=0; i<sock_count; i++)
        if (sock_open[i].sock==sock)
            sock_open[i] = sock_open[--sock_count];
    if (!close_socket(sock))
        print_error("on closing a socket");
}

void Channel::Shutdown () {
    while (sock_count--)
        CloseSocket(sock_open[sock_count].sock);
}

void     swift::SetTracker(const Address& tracker) {
    Channel::tracker = tracker;
}

int Channel::DecodeID(int scrambled) {
    return scrambled ^ (int)start;
}
int Channel::EncodeID(int unscrambled) {
    return unscrambled ^ (int)start;
}


/*
 * class Address implementation
 */

void Address::set_ipv4 (const char* ip_str) {
    struct hostent *h = gethostbyname(ip_str);
    if (h == NULL) {
        print_error("cannot lookup address");
        return;
    } else {
        addr.sin_addr.s_addr = *(u_long *) h->h_addr_list[0];
    }
}


Address::Address(const char* ip_port) {
    clear();
    if (strlen(ip_port)>=1024)
        return;
    char ipp[1024];
    strncpy(ipp,ip_port,1024);
    char* semi = strchr(ipp,':');
    if (semi) {
        *semi = 0;
        set_ipv4(ipp);
        set_port(semi+1);
    } else {
        if (strchr(ipp, '.')) {
            set_ipv4(ipp);
            set_port((uint16_t)0);
        } else {
            set_ipv4((uint32_t)INADDR_ANY);
            set_port(ipp);
        }
    }
}


uint32_t Address::LOCALHOST = INADDR_LOOPBACK;


/*
 * Utility methods
 */

const char* swift::tintstr (tint time) {
    if (time==0)
        time = now_t::now;
    static char ret_str[4][32]; // wow
    static int i;
    i = (i+1) & 3;
    if (time==TINT_NEVER)
        return "NEVER";
    time -= Channel::epoch;
    assert(time>=0);
    int hours = time/TINT_HOUR;
    time %= TINT_HOUR;
    int mins = time/TINT_MIN;
    time %= TINT_MIN;
    int secs = time/TINT_SEC;
    time %= TINT_SEC;
    int msecs = time/TINT_MSEC;
    time %= TINT_MSEC;
    int usecs = time/TINT_uSEC;
    sprintf(ret_str[i],"%i_%02i_%02i_%03i_%03i",hours,mins,secs,msecs,usecs);
    return ret_str[i];
}


std::string swift::sock2str (struct sockaddr_in addr) {
    char ipch[32];
#ifdef _WIN32
    //Vista only: InetNtop(AF_INET,&(addr.sin_addr),ipch,32);
    // IPv4 only:
    struct in_addr inaddr;
    memcpy(&inaddr, &(addr.sin_addr), sizeof(inaddr));
    strncpy(ipch, inet_ntoa(inaddr),32);
#else
    inet_ntop(AF_INET,&(addr.sin_addr),ipch,32);
#endif
    sprintf(ipch+strlen(ipch),":%i",ntohs(addr.sin_port));
    return std::string(ipch);
}


int swift::evbuffer_add_string(struct evbuffer *evb, std::string str) {
    return evbuffer_add(evb, str.c_str(), str.size());
}

int swift::evbuffer_add_8(struct evbuffer *evb, uint8_t b) {
    return evbuffer_add(evb, &b, 1);
}

int swift::evbuffer_add_16be(struct evbuffer *evb, uint16_t w) {
    uint16_t wbe = htons(w);
    return evbuffer_add(evb, &wbe, 2);
}

int swift::evbuffer_add_32be(struct evbuffer *evb, uint32_t i) {
    uint32_t ibe = htonl(i);
    return evbuffer_add(evb, &ibe, 4);
}

int swift::evbuffer_add_64be(struct evbuffer *evb, uint64_t l) {
    uint32_t lbe[2];
    lbe[0] = htonl((uint32_t)(l>>32));
    lbe[1] = htonl((uint32_t)(l&0xffffffff));
    return evbuffer_add(evb, lbe, 8);
}

int swift::evbuffer_add_hash(struct evbuffer *evb, const Sha1Hash& hash)  {
    return evbuffer_add(evb, hash.bits, Sha1Hash::SIZE);
}

// PPSP
int swift::evbuffer_add_chunkaddr(struct evbuffer *evb, bin_t &b, popt_chunk_addr_t chunk_addr);
{
    int ret = -1;
    if (chunk_addr_ == POPT_CHUNK_ADDR_BIN32)
	ret = evbuffer_add_32be(evb, bin_toUInt32(b));
    else if (chunk_addr == POPT_CHUNK_ADDR_CHUNK32)
    {
	ret = evbuffer_add_32be(evb, bin_toUInt32(b.base_left()));
	ret = evbuffer_add_32be(evb, bin_toUInt32(b.base_right()));
    }
    return ret;
}



uint8_t swift::evbuffer_remove_8(struct evbuffer *evb) {
    uint8_t b;
    if (evbuffer_remove(evb, &b, 1) < 1)
    return 0;
    return b;
}

uint16_t swift::evbuffer_remove_16be(struct evbuffer *evb) {
    uint16_t wbe;
    if (evbuffer_remove(evb, &wbe, 2) < 2)
    return 0;
    return ntohs(wbe);
}

uint32_t swift::evbuffer_remove_32be(struct evbuffer *evb) {
    uint32_t ibe;
    if (evbuffer_remove(evb, &ibe, 4) < 4)
    return 0;
    return ntohl(ibe);
}

uint64_t swift::evbuffer_remove_64be(struct evbuffer *evb) {
    uint32_t lbe[2];
    if (evbuffer_remove(evb, lbe, 8) < 8)
    return 0;
    uint64_t l = ntohl(lbe[0]);
    l<<=32;
    l |= ntohl(lbe[1]);
    return l;
}

Sha1Hash swift::evbuffer_remove_hash(struct evbuffer* evb)  {
    char bits[Sha1Hash::SIZE];
    if (evbuffer_remove(evb, bits, Sha1Hash::SIZE) < Sha1Hash::SIZE)
    return Sha1Hash::ZERO;
    return Sha1Hash(false, bits);
}

// PPSP
binvector swift::evbuffer_remove_chunkaddr(struct evbuffer *evb, popt_chunk_addr_t chunk_addr)
{
    binvector bv;
    if (chunk_addr == POPT_CHUNK_ADDR_BIN32)
    {
	bin_t pos = bin_fromUInt32(evbuffer_remove_32be(evb));
	bv.push_back(pos);
    }
    else if (chunk_addr == POPT_CHUNK_ADDR_CHUNK32)
    {
	uint32_t schunk = evbuffer_remove_32be(evb);
	uint32_t echunk = evbuffer_remove_32be(evb);
	if (schunk <= eschunk) // Bad input protection
	    swift::chunk32_to_bin32(schunk,eschunk,&bv);
    }
    return bv;
}



/** Convert a chunk32 chunk specification to a list of bins. A chunk32 spec is
 * a (start chunk ID, end chunk ID) pair, where chunk ID is just a numbering
 * from 0 to N of all chunks, equivalent to the leaves in a bin tree. This
 * method finds which bins describe this range.
 */
void swift::chunk32_to_bin32(uint32_t schunk, uint32_t echunk, binvector *bvptr)
{
    bin_t s(0,schunk);
    bin_t e(0,echunk);

    bin_t cur = s;
    //char binstr[32],binstr2[32], binstr3[32];
    while (true)
    {
	// Move up in tree till we exceed either start or end. If so, the
	// previous node belongs to the range description. Next, we start at
	// the left most chunk in the subtree next to the previous node, and see
	// how far up we can go there.
	//fprintf(stderr,"\ncur %s par left %s par right %s\n", cur.str(binstr), cur.parent().base_left().str(binstr2), cur.parent().base_right().str(binstr3));
	if (cur.parent().base_left() < s || cur.parent().base_right() > e)
	{
	    /*if (cur.parent().base_left() < s)
		fprintf(stderr,"parent %s left %s before s, add %s\n", cur.parent().str(binstr2), cur.parent().base_left().str(binstr3), cur.str(binstr) );
	    if (cur.parent().base_right() > e)
		fprintf(stderr,"parent %s right %s exceeds e, add %s\n", cur.parent().str(binstr2), cur.parent().base_right().str(binstr3), cur.str(binstr) );
	     */
	    bvptr->push_back(cur);

	    if (cur.parent().base_left() < s)
		cur = bin_t(0,cur.parent().base_right().layer_offset()+1);
	    else
		cur = bin_t(0,cur.base_right().layer_offset()+1);

	    //fprintf(stderr,"newcur %s\n", cur.str(binstr) );

	    if (cur >= e)
	    {
		if (cur == e)
		{
		    // fprintf(stderr,"adding e %s\n", cur.str(binstr) );
		    bvptr->push_back(e);
		}
		break;
	    }
	}
	else
	    cur = cur.parent();
    }
}
