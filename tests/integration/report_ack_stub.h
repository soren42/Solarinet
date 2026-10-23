#ifndef REPORT_ACK_STUB_H
#define REPORT_ACK_STUB_H

#include "solari/solariFrame.h"
#include "solari/solariNet.h"
#include "solari/solariTlv.h"
#include "solari/solariTime.h"

#include <string.h>

typedef struct {
    solariConn *conn;
    unsigned int replies;
    bool corruptFirst;
} reportAckStub;

static void *reportAckStubRun(void *arg)
{
    reportAckStub *stub = (reportAckStub *)arg;
    unsigned int i;
    for (i = 0; i < stub->replies; i++) {
        const uint8_t *rx = NULL, *payload = NULL;
        size_t rxLen = 0, payloadLen = 0, frameLen = 0;
        solariFrameHeader request, reply;
        solariTlvReader reader;
        solariTlvWriter writer;
        const uint8_t *value;
        uint16_t type, valueLen;
        uint8_t session[16], tlv[32], frame[128];
        bool found = false;

        if (solariConnRecv(stub->conn, &rx, &rxLen) != SOLARI_OK ||
            solariFrameParse(rx, rxLen, &request, &payload, &payloadLen, NULL) != SOLARI_OK)
            return NULL;
        solariTlvReaderInit(&reader, payload, payloadLen);
        while (solariTlvNext(&reader, &type, &value, &valueLen) == SOLARI_OK) {
            if (type == TLV_REPORT_SESSION && valueLen == sizeof session) {
                memcpy(session, value, sizeof session);
                found = true;
            }
        }
        if (!found) return NULL;
        if (stub->corruptFirst && i == 0) session[0] ^= 0xFF;
        solariTlvWriterInit(&writer, tlv, sizeof tlv);
        if (solariTlvAppend(&writer, TLV_REPORT_SESSION, session, sizeof session) != SOLARI_OK)
            return NULL;
        memset(&reply, 0, sizeof reply);
        reply.magic[0] = SCP_MAGIC_0; reply.magic[1] = SCP_MAGIC_1;
        reply.protoVersion = SCP_PROTO_VERSION; reply.msgType = SCP_MSG_ACK;
        reply.tlvCount = writer.count; reply.sourceNodeId = 0x1234;
        reply.sendTimeUnixMs = solariNowUnixMs(); reply.seqNo = i + 1;
        reply.correlationId = request.seqNo + (stub->corruptFirst && i == 0 ? 1u : 0u);
        if (solariFrameBuild(&reply, tlv, writer.len, frame, sizeof frame, &frameLen) == SOLARI_OK)
            (void)solariConnSend(stub->conn, frame, frameLen);
    }
    return NULL;
}

#endif
