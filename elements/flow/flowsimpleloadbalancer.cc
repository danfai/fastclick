/*
 * FlowSimpleIPLoadBalancer.{cc,hh}
 */

#include <click/config.h>
#include <click/glue.hh>
#include <click/args.hh>
#include <click/flow.hh>
#include <clicknet/ip.h>
#include <clicknet/tcp.h>
#include "flowsimpleloadbalancer.hh"

CLICK_DECLS

#define DEBUG_LB 0


FlowSimpleIPLoadBalancer::FlowSimpleIPLoadBalancer() : _state(state{0}){

};

FlowSimpleIPLoadBalancer::~FlowSimpleIPLoadBalancer() {

}

int
FlowSimpleIPLoadBalancer::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if (Args(conf, this, errh)
               .read_all("DST",Args::mandatory | Args::positional,DefaultArg<Vector<IPAddress>>(),_dsts)
               .complete() < 0)
        return -1;
    click_chatter("%p{element} has %d routes",this,_dsts.size());
    return 0;
}


int FlowSimpleIPLoadBalancer::initialize(ErrorHandler *errh) {
    return FlowSpaceElement<SMapInfo>::initialize(errh);
}


void FlowSimpleIPLoadBalancer::push_batch(int, SMapInfo* flowdata, PacketBatch* batch) {

    state &s = *_state;
    if (flowdata->srv == 0) {
        int server = s.last++;
        if (s.last >= _dsts.size())
            s.last = 0;
        flowdata->srv = server + 1;
    }
    IPAddress ip = _dsts[flowdata->srv - 1];
    auto fnt = [this,ip](Packet*p) -> Packet* {
        WritablePacket* q=p->uniqueify();
        q->rewrite_ip(ip, 1, true);
        q->set_dst_ip_anno(ip);
        return q;
    };
    EXECUTE_FOR_EACH_PACKET(fnt, batch);

    checked_output_push_batch(0, batch);
}

FlowSimpleIPLoadBalancerReverse::FlowSimpleIPLoadBalancerReverse() {

};

FlowSimpleIPLoadBalancerReverse::~FlowSimpleIPLoadBalancerReverse() {

}

int
FlowSimpleIPLoadBalancerReverse::configure(Vector<String> &conf, ErrorHandler *errh)
{
    Element* e;
    if (Args(conf, this, errh)
               .read_p("IP",_ip)
               .complete() < 0)
        return -1;
    return 0;
}


int FlowSimpleIPLoadBalancerReverse::initialize(ErrorHandler *errh) {

    return 0;
}




void FlowSimpleIPLoadBalancerReverse::push_batch(int, PacketBatch* batch) {
    auto fnt = [this](Packet*p) -> Packet*{
        WritablePacket* q=p->uniqueify();
        q->rewrite_ip(_ip,0, true);
        return q;
    };

    EXECUTE_FOR_EACH_PACKET(fnt, batch);


    checked_output_push_batch(0, batch);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(flow)
EXPORT_ELEMENT(FlowSimpleIPLoadBalancerReverse)
ELEMENT_MT_SAFE(FlowSimpleIPLoadBalancerReverse)
EXPORT_ELEMENT(FlowSimpleIPLoadBalancer)
ELEMENT_MT_SAFE(FlowSimpleIPLoadBalancer)
