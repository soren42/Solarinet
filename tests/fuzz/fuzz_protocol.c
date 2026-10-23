/* Coverage-guided SCP frame, TLV, and message parser target. */
#include "solari/solariFrame.h"
#include "solari/solariMsg.h"
#include "solari/solariTlv.h"

#include <stddef.h>
#include <stdint.h>

static void exercise_message_parsers(const uint8_t *data, size_t size)
{
    solariClientReport client;
    solariMonitorReport monitor;
    solariHello hello;
    solariControl control;
    solariErrorMsg error;

    (void)solariMsgParseClientReport(data, size, &client);
    (void)solariMsgParseMonitorReport(data, size, &monitor);
    (void)solariMsgParseHello(data, size, &hello);
    (void)solariMsgParseControl(data, size, &control);
    (void)solariMsgParseError(data, size, &error);
}

static void exercise_tlv_reader(const uint8_t *data, size_t size)
{
    solariTlvReader reader;
    uint16_t type, len;
    const uint8_t *value;
    size_t items = 0;

    solariTlvReaderInit(&reader, data, size);
    while (solariTlvNext(&reader, &type, &value, &len) == SOLARI_OK) {
        /* Exercise the typed readers with arbitrary wire lengths as well. */
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        (void)solariTlvReadU8(value, len, &u8);
        (void)solariTlvReadU16(value, len, &u16);
        (void)solariTlvReadU32(value, len, &u32);
        (void)solariTlvReadU64(value, len, &u64);
        (void)type;
        if (++items > size / 4u + 1u) break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    solariFrameHeader header;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    /* Exercise raw stream boundaries and all standalone payload decoders. */
    (void)solariFrameParse(data, size, &header, &payload, &payload_len, NULL);
    exercise_tlv_reader(data, size);
    exercise_message_parsers(data, size);

    /* Wrap arbitrary input in a checksummed frame with one unknown TLV. This
     * reaches the valid-frame path while preserving forward-compatible TLVs. */
    if (size <= UINT16_MAX && size <= SOLARI_MAX_FRAME - SOLARI_FRAME_HEADER_LEN - 4u) {
        static uint8_t frame[SOLARI_MAX_FRAME + 8u];
        static uint8_t tlv[UINT16_MAX + 4u];
        solariFrameHeader wrapped = {0};
        solariTlvWriter writer;
        size_t frame_len = 0;

        wrapped.magic[0] = SCP_MAGIC_0;
        wrapped.magic[1] = SCP_MAGIC_1;
        wrapped.protoVersion = SCP_PROTO_VERSION;
        wrapped.msgType = SCP_MSG_HELLO;
        wrapped.tlvCount = 1;

        solariTlvWriterInit(&writer, tlv, sizeof tlv);
        if (solariTlvAppend(&writer, 0x7fffu, data, (uint16_t)size) == SOLARI_OK &&
            solariFrameBuild(&wrapped, tlv, writer.len, frame, sizeof frame,
                             &frame_len) == SOLARI_OK &&
            solariFrameParse(frame, frame_len, &header, &payload, &payload_len,
                             NULL) == SOLARI_OK) {
            exercise_tlv_reader(payload, payload_len);
            exercise_message_parsers(payload, payload_len);
        }
    }
    return 0;
}
