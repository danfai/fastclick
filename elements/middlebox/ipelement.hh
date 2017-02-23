#ifndef MIDDLEBOX_IPELEMENT_HH
#define MIDDLEBOX_IPELEMENT_HH
#include <click/element.hh>
#include "stackelement.hh"

CLICK_DECLS

class IPElement : public StackElement
{
public:
    IPElement() CLICK_COLD;

    const char *class_name() const        { return "IPElement"; }
    const char *port_count() const        { return PORTS_1_1; }
    const char *processing() const        { return PROCESSING_A_AH; }

    int configure(Vector<String> &, ErrorHandler *) CLICK_COLD;

    static uint16_t packetTotalLength(Packet*);
    static uint16_t getIPHeaderOffset(Packet* packet);
    static void setPacketTotalLength(WritablePacket*, unsigned);
    static const uint32_t getDestinationAddress(Packet*);
    static const uint32_t getSourceAddress(Packet*);

protected:
    void computeChecksum(WritablePacket*);
};

CLICK_ENDDECLS
#endif