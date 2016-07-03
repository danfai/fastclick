#include <click/config.h>
#include <click/router.hh>
#include <click/args.hh>
#include <click/error.hh>
#include <clicknet/ip.h>
#include <clicknet/tcp.h>
#include "tcpin.hh"
#include "tcpout.hh"
#include "ipelement.hh"

CLICK_DECLS

TCPOut::TCPOut()
{
    inElement = NULL;
}

int TCPOut::configure(Vector<String> &conf, ErrorHandler *errh)
{
    return 0;
}

Packet* TCPOut::processPacket(struct fcb* fcb, Packet* p)
{
    WritablePacket *packet = p->uniqueify();

    bool hasModificationList = inElement->hasModificationList(fcb, packet);
    ByteStreamMaintainer &byteStreamMaintainer = fcb->tcp_common->maintainers[getFlowDirection()];
    ModificationList *modList = NULL;

    if(hasModificationList)
        modList = inElement->getModificationList(fcb, packet);

    // Update the sequence number (according to modifications made on previous packets)
    tcp_seq_t prevSeq = getSequenceNumber(packet);
    tcp_seq_t newSeq =  byteStreamMaintainer.mapSeq(prevSeq);
    bool seqModified = false;
    bool ackModified = false;
    tcp_seq_t prevAck = getAckNumber(packet);
    tcp_seq_t prevLastAck = byteStreamMaintainer.getLastAck();

    if(prevSeq != newSeq)
    {
        click_chatter("Sequence number %u modified to %u in flow %u", prevSeq, newSeq, flowDirection);
        setSequenceNumber(packet, newSeq);
        // Update the last sequence number seen
        // This number is used when crafting ACKs
        byteStreamMaintainer.setLastSeq(newSeq);
        seqModified = true;
    }

    // Update the value of the last ACK sent
    byteStreamMaintainer.setLastAck(prevAck);

    // Ensure that the value of the ACK is not below the last ACKed position
    /* This solves the following problem:
     * - We ACK a packet manually for any reason
     * - The "manual" ACK is lost
     */
    setAckNumber(packet, byteStreamMaintainer.getLastAck());

    if(getAckNumber(packet) != prevAck)
        ackModified = true;

    // Check if the packet has been modified
    if(getAnnotationModification(packet) || seqModified || ackModified)
    {
        // Check the length to see if bytes were added or removed
        uint16_t initialLength = IPElement::packetTotalLength(packet);
        uint16_t currentLength = (uint16_t)packet->length() - IPElement::getIPHeaderOffset(packet);
        int offsetModification = -(initialLength - currentLength);
        uint32_t prevPayloadSize = getPayloadLength(packet);

        // Update the "total length" field in the IP header (required to compute the tcp checksum as it is in the pseudo hdr)
        IPElement::setPacketTotalLength(packet, initialLength + offsetModification);

        // Check if the modificationlist has to be committed
        if(hasModificationList)
        {
            // We know that the packet has been modified and its size has changed
            modList->commit(fcb->tcp_common->maintainers[getFlowDirection()]);

            // Check if the full packet content has been removed
            if(getPayloadLength(packet) == 0)
            {
                uint32_t saddr = IPElement::getDestinationAddress(packet);
                uint32_t daddr = IPElement::getSourceAddress(packet);
                uint16_t sport = getDestinationPort(packet);
                uint16_t dport = getSourcePort(packet);
                // The SEQ value is the initial ACK value in the packet sent
                // by the source.
                // To get it, we take the current one (which has been corrected)
                // and we map it as if it was a sequence number to get back
                // the initial value
                tcp_seq_t seq = getAckNumber(packet);
                seq = fcb->tcp_common->maintainers[getOppositeFlowDirection()].mapSeq(seq);
                uint16_t winSize = getWindowSize(packet);
                // The ACK is the sequence number sent by the source
                // to which we add the old size of the payload to acknowledge it
                tcp_seq_t ack = prevSeq + prevPayloadSize;

                if(isFin(packet) || isSyn(packet))
                    ack++;

                // Craft and send the ack
                sendAck(fcb->tcp_common->maintainers[getOppositeFlowDirection()], saddr, daddr, sport, dport, seq, ack, winSize);

                // Even if the packet is empty it can still contain relevant
                // information (significant ACK value or another flag)
                if(isJustAnAck(packet))
                {
                    // Check if the ACK of the packet was significant or not
                    if(prevAck <= prevLastAck)
                    {
                        // If this is not the case, drop the packet as it
                        // does not contain any relevant information
                        // (And anyway it would be considered as a duplicate ACK)
                        click_chatter("Empty packet dropped");
                        packet->kill();
                        return NULL;
                    }
                }
            }
        }

        // Recompute the checksum
        computeChecksum(packet);
    }

    // Notify the stack function that this packet has been sent
    packetSent(fcb, packet);

    if(isFin(packet) || isRst(packet))
        fcb->tcpout.closingState = TCPClosingState::CLOSED;

    return packet;
}

void TCPOut::sendAck(ByteStreamMaintainer &maintainer, uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport, tcp_seq_t seq, tcp_seq_t ack, uint16_t winSize)
{
    if(noutputs() < 2)
    {
        click_chatter("Warning: trying to send an ack on a TCPOut with only 1 output");
        return;
    }

    // Check if the ACK does not bring any additional information
    if(ack <= maintainer.getLastAck())
        return;

    // Update the number of the last ack sent for the other side
    maintainer.setLastAck(ack);

    // Ensure that the sequence number of the packet is not below
    // a sequence number sent before by the other side
    if(seq < maintainer.getLastSeq())
        seq = maintainer.getLastSeq();

    // The packet is now empty, we discard it and send an ACK directly to the source
    click_chatter("Sending an ACK! (%u)", ack);

    // Craft the packet
    Packet* forged = forgePacket(saddr, daddr, sport, dport, seq, ack, winSize, TH_ACK);

    //Send it on the second output
    output(1).push(forged);
}

void TCPOut::setInElement(TCPIn* inElement)
{
    this->inElement = inElement;
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(ByteStreamMaintainer)
ELEMENT_REQUIRES(ModificationList)
EXPORT_ELEMENT(TCPOut)
//ELEMENT_MT_SAFE(TCPOut)
