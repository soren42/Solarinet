/*
 * solariError.c - solariStrError implementation.
 */
#include "solari/solariError.h"

const char *solariStrError(solariStatus st)
{
    switch (st) {
    case SOLARI_OK:             return "ok";
    case ERR_INVALID_ARG:       return "invalid argument";
    case ERR_BUFFER_FULL:       return "buffer full";
    case ERR_FRAME_INCOMPLETE:  return "frame incomplete";
    case ERR_FRAME_BAD_MAGIC:   return "bad frame magic";
    case ERR_FRAME_BAD_CRC:     return "bad frame crc";
    case ERR_PROTO_VERSION:     return "unsupported protocol version";
    case ERR_TLV_END:           return "end of tlv payload";
    case ERR_TLV_TRUNCATED:     return "truncated tlv";
    case ERR_CONN_RETRY:        return "transient connection failure";
    case ERR_CONN_FATAL:        return "fatal connection error";
    case ERR_TLS:               return "tls failure";
    case ERR_AUTH_ROLE:         return "message illegal for role";
    case ERR_DB:                return "database error";
    case ERR_LEASE_LOST:        return "server lease lost";
    case ERR_UNKNOWN_MSG:       return "unknown message type";
    case ERR_PLATFORM:          return "platform call failed";
    default:                    return "unknown error";
    }
}
