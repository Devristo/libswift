/*
 *  content.cpp
 *  Superclass of FileTransfer and LiveTransfer
 *
 *  Created by Arno Bakker.
 *  Copyright 2009-2012 TECHNISCHE UNIVERSITEIT DELFT. All rights reserved.
 */
#include "swift.h"
#include <cfloat>


using namespace swift;

std::vector<ContentTransfer*> ContentTransfer::swarms(20);


ContentTransfer::ContentTransfer() :  mychannels_(), cb_installed(0), speedzerocount_(0)
{
	GlobalAdd();

    cur_speed_[DDIR_UPLOAD] = MovingAverageSpeed();
    cur_speed_[DDIR_DOWNLOAD] = MovingAverageSpeed();
    max_speed_[DDIR_UPLOAD] = DBL_MAX;
    max_speed_[DDIR_DOWNLOAD] = DBL_MAX;

    evtimer_assign(&evclean_,Channel::evbase,&ContentTransfer::LibeventCleanCallback,this);
    evtimer_add(&evclean_,tint2tv(5*TINT_SEC));
}


ContentTransfer::~ContentTransfer()
{
	GlobalDel();
	delete storage_;

	// Arno, 2012-02-06: Cancel cleanup timer, otherwise chaos!
	evtimer_del(&evclean_);
}


void ContentTransfer::LibeventCleanCallback(int fd, short event, void *arg)
{
	fprintf(stderr,"CleanCallback: **************\n");

	ContentTransfer *ct = (ContentTransfer *)arg;
	if (ct == NULL)
		return;

	// STL and MS and conditional delete from set not a happy place :-(
	std::set<Channel *>	delset;
	std::set<Channel *>::iterator iter;
	for (iter=ct->mychannels_.begin(); iter!=ct->mychannels_.end(); iter++)
	{
		Channel *c = *iter;
		if (c != NULL) {
			if (c->IsScheduled4Close())
				delset.insert(c);
		}
	}
	for (iter=delset.begin(); iter!=delset.end(); iter++)
	{
		Channel *c = *iter;
		c->Close();
		ct->mychannels_.erase(c);
		delete c;
	}

	// Reschedule cleanup
    evtimer_assign(&(ct->evclean_),Channel::evbase,&ContentTransfer::LibeventCleanCallback,ct);
    evtimer_add(&(ct->evclean_),tint2tv(5*TINT_SEC));
}



void ContentTransfer::GlobalAdd() {

	fd_ = swarms.size();

	if (swarms.size()<fd_+1)
		swarms.resize(fd_+1);
	swarms[fd_] = this;
}


void ContentTransfer::GlobalDel() {
	swarms[fd_] = NULL;
}


ContentTransfer* ContentTransfer::Find (const Sha1Hash& swarmid) {
    for(int i=0; i<swarms.size(); i++)
        if (swarms[i] && swarms[i]->swarm_id()==swarmid)
            return swarms[i];
    return NULL;
}



bool ContentTransfer::OnPexIn (const Address& addr) {

	//fprintf(stderr,"ContentTransfer::OnPexIn: %s\n", addr.str() );
	// Arno: this brings safety, but prevents private swift installations.
	// TODO: detect public internet.
	//if (addr.is_private())
	//	return false;

    for(int i=0; i<hs_in_.size(); i++) {
        Channel* c = Channel::channel(hs_in_[i].toUInt());
        if (c && c->transfer()->fd()==this->fd() && c->peer()==addr) {
            return false; // already connected or connecting, Gertjan fix = return false
        }
    }
    // Gertjan fix: PEX redo
    if (hs_in_.size()<SWIFT_MAX_CONNECTIONS)
        new Channel(this,Channel::default_socket(),addr);
    return true;
}

//Gertjan
int ContentTransfer::RandomChannel (int own_id) {
    binqueue choose_from;
    int i;

    for (i = 0; i < (int) hs_in_.size(); i++) {
        if (hs_in_[i].toUInt() == own_id)
            continue;
        Channel *c = Channel::channel(hs_in_[i].toUInt());
        if (c == NULL || c->transfer()->fd() != this->fd()) {
            /* Channel was closed or is not associated with this ContentTransfer (anymore). */
            hs_in_[i] = hs_in_[0];
            hs_in_.pop_front();
            i--;
            continue;
        }
        if (!c->is_established())
            continue;
        choose_from.push_back(hs_in_[i]);
    }
    if (choose_from.size() == 0)
        return -1;

    return choose_from[rand() % choose_from.size()].toUInt();
}





// RATELIMIT
void		ContentTransfer::OnRecvData(int n)
{
	// Got n ~ 32K
	cur_speed_[DDIR_DOWNLOAD].AddPoint((uint64_t)n);
}

void		ContentTransfer::OnSendData(int n)
{
	// Sent n ~ 1K
	cur_speed_[DDIR_UPLOAD].AddPoint((uint64_t)n);
}


void		ContentTransfer::OnSendNoData()
{
	// AddPoint(0) everytime we don't AddData gives bad speed measurement
	// batch 32 such events into 1.
	speedzerocount_++;
	if (speedzerocount_ >= 32)
	{
		cur_speed_[DDIR_UPLOAD].AddPoint((uint64_t)0);
		speedzerocount_ = 0;
	}
}


double		ContentTransfer::GetCurrentSpeed(data_direction_t ddir)
{
	return cur_speed_[ddir].GetSpeedNeutral();
}


void		ContentTransfer::SetMaxSpeed(data_direction_t ddir, double m)
{
	max_speed_[ddir] = m;
	// Arno, 2012-01-04: Be optimistic, forget history.
	cur_speed_[ddir].Reset();
}


double		ContentTransfer::GetMaxSpeed(data_direction_t ddir)
{
	return max_speed_[ddir];
}

//STATS
uint32_t	ContentTransfer::GetNumLeechers()
{
	uint32_t count = 0;
	std::set<Channel *>::iterator iter;
    for (iter=mychannels_.begin(); iter!=mychannels_.end(); iter++)
    {
	    Channel *c = *iter;
	    if (c != NULL)
		    if (!c->IsComplete()) // incomplete?
			    count++;
    }
    return count;
}


uint32_t	ContentTransfer::GetNumSeeders()
{
	uint32_t count = 0;
	std::set<Channel *>::iterator iter;
    for (iter=mychannels_.begin(); iter!=mychannels_.end(); iter++)
    {
	    Channel *c = *iter;
	    if (c != NULL)
		    if (c->IsComplete()) // complete?
			    count++;
    }
    return count;
}

void ContentTransfer::AddPeer(Address &peer)
{
	Channel *c = new Channel(this,INVALID_SOCKET,peer);
}


Channel * ContentTransfer::FindChannel(const Address &addr, Channel *notc)
{
	std::set<Channel *>::iterator iter;
	for (iter=mychannels_.begin(); iter!=mychannels_.end(); iter++)
	{
		Channel *c = *iter;
		if (c != NULL) {
			if (c != notc && (c->peer() == addr || c->recv_peer() == addr)) {
				return c;
			}
		}
	}
	return NULL;
}
