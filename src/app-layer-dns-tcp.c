/*
 * Copyright (c) 2009-2013 Open Information Security Foundation
 *
 * \author Victor Julien <victor@inliniac.net> for Emerging Threats Inc.
 *
 */

#include "suricata-common.h"
#include "suricata.h"

#include "debug.h"
#include "decode.h"

#include "flow-util.h"

#include "threads.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-debug.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"

#include "util-spm.h"
#include "util-unittest.h"

#include "app-layer-dns-tcp.h"

struct DNSTcpHeader_ {
    uint16_t len;
    uint16_t tx_id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answer_rr;
    uint16_t authority_rr;
    uint16_t additional_rr;
} __attribute__((__packed__));
typedef struct DNSTcpHeader_ DNSTcpHeader;

/** \internal
 *  \param input_len at least enough for the DNSTcpHeader
 */
static int DNSTCPRequestParseProbe(uint8_t *input, uint32_t input_len)
{
#ifdef DEBUG
    BUG_ON(input_len < sizeof(DNSTcpHeader));
#endif
    SCLogDebug("starting %u", input_len);

    DNSTcpHeader *dns_tcp_header = (DNSTcpHeader *)input;
    if (ntohs(dns_tcp_header->len) < sizeof(DNSHeader)) {
        goto bad_data;
    }
    if (ntohs(dns_tcp_header->len) >= input_len) {
        goto insufficient_data;
    }

    input += 2;
    input_len -= 2;
    DNSHeader *dns_header = (DNSHeader *)input;

    uint16_t q;
    const uint8_t *data = input + sizeof(DNSHeader);

    for (q = 0; q < ntohs(dns_header->questions); q++) {
        uint16_t fqdn_offset = 0;

        if (input + input_len < data + 1) {
            SCLogDebug("input buffer too small for len field");
            goto insufficient_data;
        }
        SCLogDebug("query length %u", *data);

        while (*data != 0) {
            if (*data > 63) {
                /** \todo set event?*/
                goto bad_data;
            }
            uint8_t length = *data;

            data++;

            if (length > 0) {
                if (input + input_len < data + length) {
                    SCLogDebug("input buffer too small for domain of len %u", length);
                    goto insufficient_data;
                }
                //PrintRawDataFp(stdout, data, qry->length);

                if ((fqdn_offset + length + 1) < DNS_MAX_SIZE) {
                    fqdn_offset += length;
                } else {
                    /** \todo set event? */
                    goto bad_data;
                }
            }

            data += length;

            if (input + input_len < data + 1) {
                SCLogDebug("input buffer too small for new len");
                goto insufficient_data;
            }

            SCLogDebug("qry length %u", *data);
        }
        if (fqdn_offset) {
            fqdn_offset--;
        }

        data++;
        if (input + input_len < data + sizeof(DNSQueryTrailer)) {
            SCLogDebug("input buffer too small for DNSQueryTrailer");
            goto insufficient_data;
        }
        DNSQueryTrailer *trailer = (DNSQueryTrailer *)data;
        SCLogDebug("trailer type %04x class %04x", ntohs(trailer->type), ntohs(trailer->class));
        data += sizeof(DNSQueryTrailer);
    }

	SCReturnInt(1);
insufficient_data:
    SCReturnInt(0);
bad_data:
    SCReturnInt(-1);
}

static int BufferData(DNSState *dns_state, uint8_t *data, uint16_t len) {
    if (dns_state->buffer == NULL) {
        /** \todo be smarter about this, like use a pool or several pools for
         *        chunks of various sizes */
        dns_state->buffer = SCMalloc(0xffff);
        if (dns_state->buffer == NULL) {
            return -1;
        }
    }

    if ((uint32_t)len + (uint32_t)dns_state->offset > (uint32_t)dns_state->record_len) {
        SCLogInfo("oh my, we have more data than the max record size. What do we do. WHAT DO WE DOOOOO!");
        BUG_ON(1);
        len = dns_state->record_len - dns_state->offset;
    }

    memcpy(dns_state->buffer + dns_state->offset, data, len);
    dns_state->offset += len;
    return 0;
}

static void BufferReset(DNSState *dns_state) {
    dns_state->record_len = 0;
    dns_state->offset = 0;
}

static int DNSRequestParseData(Flow *f, DNSState *dns_state, const uint8_t *input, const uint32_t input_len) {
    DNSHeader *dns_header = (DNSHeader *)input;

    if (DNSValidateRequestHeader(f, dns_header) < 0)
        goto bad_data;

    //SCLogInfo("ID %04x", ntohs(dns_header->tx_id));

    uint16_t q;
    const uint8_t *data = input + sizeof(DNSHeader);

    //PrintRawDataFp(stdout, (uint8_t*)data, input_len - (data - input));

    for (q = 0; q < ntohs(dns_header->questions); q++) {
        uint8_t fqdn[DNS_MAX_SIZE];
        uint16_t fqdn_offset = 0;

        if (input + input_len < data + 1) {
            SCLogDebug("input buffer too small for DNSTcpQuery");
            goto insufficient_data;
        }
        SCLogDebug("query length %u", *data);

        while (*data != 0) {
            if (*data > 63) {
                /** \todo set event?*/
                goto insufficient_data;
            }
            uint8_t length = *data;

            data++;

            if (length > 0) {
                if (input + input_len < data + length) {
                    SCLogDebug("input buffer too small for domain of len %u", length);
                    goto insufficient_data;
                }
                //PrintRawDataFp(stdout, data, qry->length);

                if ((size_t)(fqdn_offset + length + 1) < sizeof(fqdn)) {
                    memcpy(fqdn + fqdn_offset, data, length);
                    fqdn_offset += length;
                    fqdn[fqdn_offset++] = '.';
                } else {
                    /** \todo set event? */
                    goto insufficient_data;
                }
            }

            data += length;

            if (input + input_len < data + 1) {
                SCLogDebug("input buffer too small for DNSTcpQuery(2)");
                goto insufficient_data;
            }

            SCLogDebug("qry length %u", *data);
        }
        if (fqdn_offset) {
            fqdn_offset--;
        }

        data++;
        if (input + input_len < data + sizeof(DNSQueryTrailer)) {
            SCLogDebug("input buffer too small for DNSQueryTrailer");
            goto insufficient_data;
        }
        DNSQueryTrailer *trailer = (DNSQueryTrailer *)data;
        SCLogDebug("trailer type %04x class %04x", ntohs(trailer->type), ntohs(trailer->class));
        data += sizeof(DNSQueryTrailer);

        /* store our data */
        if (dns_state != NULL) {
            DNSStoreQueryInState(dns_state, fqdn, fqdn_offset,
                    ntohs(trailer->type), ntohs(trailer->class),
                    ntohs(dns_header->tx_id));
        }
    }

	SCReturnInt(1);
bad_data:
insufficient_data:
    SCReturnInt(-1);

}

/** \internal
 *  \brief Parse DNS request packet
 */
static int DNSTCPRequestParse(Flow *f, void *dstate,
                          AppLayerParserState *pstate,
                          uint8_t *input, uint32_t input_len,
                          void *local_data, AppLayerParserResult *output)
{
	DNSState *dns_state = (DNSState *)dstate;
    SCLogDebug("starting %u", input_len);

    /** \todo remove this when PP is fixed to enforce ipproto */
    if (f != NULL && f->proto != IPPROTO_TCP)
        SCReturnInt(-1);

    /* probably a rst/fin sending an eof */
    if (input_len == 0) {
        goto insufficient_data;
    }

next_record:
    /* if this is the beginning of a record, we need at least the header */
    if (dns_state->offset == 0 && input_len < sizeof(DNSTcpHeader)) {
        SCLogDebug("ilen too small, hoped for at least %"PRIuMAX, (uintmax_t)sizeof(DNSTcpHeader));
        goto insufficient_data;
    }
    SCLogDebug("input_len %u offset %u record %u",
            input_len, dns_state->offset, dns_state->record_len);

    /* this is the first data of this record */
    if (dns_state->offset == 0) {
        DNSTcpHeader *dns_tcp_header = (DNSTcpHeader *)input;
        SCLogDebug("DNS %p", dns_tcp_header);

        if (ntohs(dns_tcp_header->len) < sizeof(DNSHeader)) {
            /* bogus len, doesn't fit even basic dns header */
            goto bad_data;
        } else if (ntohs(dns_tcp_header->len) == (input_len-2)) {
            /* we have all data, so process w/o buffering */
            if (DNSRequestParseData(f, dns_state, input+2, input_len-2) < 0)
                goto bad_data;

        } else if ((input_len-2) > ntohs(dns_tcp_header->len)) {
            /* we have all data, so process w/o buffering */
            if (DNSRequestParseData(f, dns_state, input+2, ntohs(dns_tcp_header->len)) < 0)
                goto bad_data;

            /* treat the rest of the data as a (potential) new record */
            input += ntohs(dns_tcp_header->len);
            input_len -= ntohs(dns_tcp_header->len);
            goto next_record;
        } else {
            /* not enough data, store record length and buffer */
            dns_state->record_len = ntohs(dns_tcp_header->len);
            BufferData(dns_state, input+2, input_len-2);
        }
    } else if (input_len + dns_state->offset < dns_state->record_len) {
        /* we don't have the full record yet, buffer */
        BufferData(dns_state, input, input_len);
    } else if (input_len > (uint32_t)(dns_state->record_len - dns_state->offset)) {
        /* more data than expected, we may have another record coming up */
        uint16_t need = (dns_state->record_len - dns_state->offset);
        BufferData(dns_state, input, need);
        int r = DNSRequestParseData(f, dns_state, dns_state->buffer, dns_state->record_len);
        BufferReset(dns_state);
        if (r < 0)
            goto bad_data;

        /* treat the rest of the data as a (potential) new record */
        input += need;
        input_len -= need;
        goto next_record;
    } else {
        /* implied exactly the amount of data we want
         * add current to buffer, then inspect buffer */
        BufferData(dns_state, input, input_len);
        int r = DNSRequestParseData(f, dns_state, dns_state->buffer, dns_state->record_len);
        BufferReset(dns_state);
        if (r < 0)
            goto bad_data;
    }

	SCReturnInt(1);
insufficient_data:
    SCReturnInt(-1);
bad_data:
    SCReturnInt(-1);
}

static int DNSReponseParseData(Flow *f, DNSState *dns_state, const uint8_t *input, const uint32_t input_len) {
    DNSHeader *dns_header = (DNSHeader *)input;

    if (DNSValidateResponseHeader(f, dns_header) < 0)
        goto bad_data;

    DNSTransaction *tx = NULL;
    int found = 0;
    TAILQ_FOREACH(tx, &dns_state->tx_list, next) {
        if (tx->tx_id == ntohs(dns_header->tx_id)) {
            found = 1;
            break;
        }
    }
    if (!found) {
        SCLogDebug("DNS_DECODER_EVENT_UNSOLLICITED_RESPONSE");
        AppLayerDecoderEventsSetEvent(f, DNS_DECODER_EVENT_UNSOLLICITED_RESPONSE);
    }

    uint16_t q;
    const uint8_t *data = input + sizeof(DNSHeader);
    for (q = 0; q < ntohs(dns_header->questions); q++) {
        uint8_t fqdn[DNS_MAX_SIZE];
        uint16_t fqdn_offset = 0;

        if (input + input_len < data + 1) {
            SCLogDebug("input buffer too small for len field");
            goto insufficient_data;
        }
        SCLogDebug("qry length %u", *data);

        while (*data != 0) {
            uint8_t length = *data;
            data++;

            if (length > 0) {
                if (input + input_len < data + length) {
                    SCLogDebug("input buffer too small for domain of len %u", length);
                    goto insufficient_data;
                }
                //PrintRawDataFp(stdout, data, length);

                if ((size_t)(fqdn_offset + length + 1) < sizeof(fqdn)) {
                    memcpy(fqdn + fqdn_offset, data, length);
                    fqdn_offset += length;
                    fqdn[fqdn_offset++] = '.';
                }
            }

            data += length;

            if (input + input_len < data + 1) {
                SCLogDebug("input buffer too small for len field");
                goto insufficient_data;
            }

            length = *data;
            SCLogDebug("length %u", length);
        }
        if (fqdn_offset) {
            fqdn_offset--;
        }

        data++;
        if (input + input_len < data + sizeof(DNSQueryTrailer)) {
            SCLogDebug("input buffer too small for DNSQueryTrailer");
            goto insufficient_data;
        }
#if DEBUG
        DNSQueryTrailer *trailer = (DNSQueryTrailer *)data;
        SCLogDebug("trailer type %04x class %04x", ntohs(trailer->type), ntohs(trailer->class));
#endif
        data += sizeof(DNSQueryTrailer);
    }

    for (q = 0; q < ntohs(dns_header->answer_rr); q++) {
        data = DNSReponseParse(dns_state, dns_header, q, DNS_LIST_ANSWER,
                input, input_len, data);
        if (data == NULL) {
            goto insufficient_data;
        }
    }

    //PrintRawDataFp(stdout, (uint8_t *)data, input_len - (data - input));
    for (q = 0; q < ntohs(dns_header->authority_rr); q++) {
        data = DNSReponseParse(dns_state, dns_header, q, DNS_LIST_AUTHORITY,
                input, input_len, data);
        if (data == NULL) {
            goto insufficient_data;
        }
    }

	SCReturnInt(1);
bad_data:
insufficient_data:
    SCReturnInt(-1);
}

/** \internal
 *  \brief DNS TCP record parser, entry function
 *
 *  Parses a DNS TCP record and fills the DNS state
 *
 *  As TCP records can be 64k we'll have to buffer the data. Streaming parsing
 *  would have been _very_ tricky due to the way names are compressed in DNS
 *
 */
static int DNSTCPResponseParse(Flow *f, void *dstate,
                          AppLayerParserState *pstate,
                          uint8_t *input, uint32_t input_len,
                          void *local_data, AppLayerParserResult *output)
{
	DNSState *dns_state = (DNSState *)dstate;

    /** \todo remove this when PP is fixed to enforce ipproto */
    if (f != NULL && f->proto != IPPROTO_TCP)
        SCReturnInt(-1);

    /* probably a rst/fin sending an eof */
    if (input_len == 0) {
        goto insufficient_data;
    }

next_record:
    /* if this is the beginning of a record, we need at least the header */
    if (dns_state->offset == 0 &&  input_len < sizeof(DNSTcpHeader)) {
        SCLogDebug("ilen too small, hoped for at least %"PRIuMAX, (uintmax_t)sizeof(DNSTcpHeader));
        goto insufficient_data;
    }
    SCLogDebug("input_len %u offset %u record %u",
            input_len, dns_state->offset, dns_state->record_len);

    /* this is the first data of this record */
    if (dns_state->offset == 0) {
        DNSTcpHeader *dns_tcp_header = (DNSTcpHeader *)input;
        SCLogDebug("DNS %p", dns_tcp_header);

        if (ntohs(dns_tcp_header->len) == (input_len-2)) {
            /* we have all data, so process w/o buffering */
            if (DNSReponseParseData(f, dns_state, input+2, input_len-2) < 0)
                goto bad_data;

        } else if ((input_len-2) > ntohs(dns_tcp_header->len)) {
            /* we have all data, so process w/o buffering */
            if (DNSReponseParseData(f, dns_state, input+2, ntohs(dns_tcp_header->len)) < 0)
                goto bad_data;

            /* treat the rest of the data as a (potential) new record */
            input += ntohs(dns_tcp_header->len);
            input_len -= ntohs(dns_tcp_header->len);
            goto next_record;
        } else {
            /* not enough data, store record length and buffer */
            dns_state->record_len = ntohs(dns_tcp_header->len);
            BufferData(dns_state, input+2, input_len-2);
        }
    } else if (input_len + dns_state->offset < dns_state->record_len) {
        /* we don't have the full record yet, buffer */
        BufferData(dns_state, input, input_len);
    } else if (input_len > (uint32_t)(dns_state->record_len - dns_state->offset)) {
        /* more data than expected, we may have another record coming up */
        uint16_t need = (dns_state->record_len - dns_state->offset);
        BufferData(dns_state, input, need);
        int r = DNSReponseParseData(f, dns_state, dns_state->buffer, dns_state->record_len);
        BufferReset(dns_state);
        if (r < 0)
            goto bad_data;

        /* treat the rest of the data as a (potential) new record */
        input += need;
        input_len -= need;
        goto next_record;
    } else {
        /* implied exactly the amount of data we want
         * add current to buffer, then inspect buffer */
        BufferData(dns_state, input, input_len);
        int r = DNSReponseParseData(f, dns_state, dns_state->buffer, dns_state->record_len);
        BufferReset(dns_state);
        if (r < 0)
            goto bad_data;
    }
	SCReturnInt(1);
insufficient_data:
    SCReturnInt(-1);
bad_data:
    SCReturnInt(-1);
}

static uint16_t DNSTcpProbingParser(uint8_t *input, uint32_t ilen)
{
    if (ilen == 0 || ilen < sizeof(DNSTcpHeader)) {
        SCLogDebug("ilen too small, hoped for at least %"PRIuMAX, (uintmax_t)sizeof(DNSTcpHeader));
        return ALPROTO_UNKNOWN;
    }

    DNSTcpHeader *dns_header = (DNSTcpHeader *)input;
    if (ntohs(dns_header->len) < sizeof(DNSHeader)) {
        /* length field bogus, won't even fit a minimal DNS header. */
        return ALPROTO_FAILED;
    } else if (ntohs(dns_header->len) > ilen) {
        int r = DNSTCPRequestParseProbe(input, ilen);
        if (r == -1) {
            /* probing parser told us "bad data", so it's not
             * DNS */
            return ALPROTO_FAILED;
        } else if (ilen > 512) {
            SCLogDebug("all the parser told us was not enough data, which is expected. Lets assume it's DNS");
            return ALPROTO_DNS_TCP;
        }

        SCLogDebug("not yet enough info %u > %u", ntohs(dns_header->len), ilen);
        return ALPROTO_UNKNOWN;
    }

    int r = DNSTCPRequestParseProbe(input, ilen);
    if (r != 1)
        return ALPROTO_FAILED;

    SCLogDebug("ALPROTO_DNS_TCP");
    return ALPROTO_DNS_TCP;
}

/**
 *  \brief Update the transaction id based on the dns state
 */
void DNSStateUpdateTransactionId(void *state, uint16_t *id) {
    SCEnter();

    DNSState *s = state;

    SCLogDebug("original id %"PRIu16", s->transaction_cnt %"PRIu16,
            *id, (s->transaction_cnt));

    if ((s->transaction_cnt) > (*id)) {
        SCLogDebug("original id %"PRIu16", updating with s->transaction_cnt %"PRIu16,
                *id, (s->transaction_cnt));

        (*id) = (s->transaction_cnt);

        SCLogDebug("updated id %"PRIu16, *id);
    }

    SCReturn;
}

/**
 *  \brief dns transaction cleanup callback
 */
void DNSStateTransactionFree(void *state, uint16_t id) {
    SCEnter();

    DNSState *s = state;

    s->transaction_done = id;
    SCLogDebug("state %p, id %"PRIu16, s, id);

    /* we can't remove the actual transactions here */

    SCReturn;
}


void RegisterDNSTCPParsers(void) {
    char *proto_name = "dnstcp";

    /** DNS */
	AppLayerRegisterProto(proto_name, ALPROTO_DNS_TCP, STREAM_TOSERVER,
			DNSTCPRequestParse);
	AppLayerRegisterProto(proto_name, ALPROTO_DNS_TCP, STREAM_TOCLIENT,
			DNSTCPResponseParse);
	AppLayerRegisterStateFuncs(ALPROTO_DNS_TCP, DNSStateAlloc,
			DNSStateFree);
    AppLayerRegisterTransactionIdFuncs(ALPROTO_DNS_TCP,
            DNSStateUpdateTransactionId, DNSStateTransactionFree);

    AppLayerRegisterProbingParser(&alp_proto_ctx,
                                  53,
                                  IPPROTO_TCP,
                                  proto_name,
                                  ALPROTO_DNS_TCP,
                                  0, sizeof(DNSTcpHeader),
                                  STREAM_TOSERVER,
                                  APP_LAYER_PROBING_PARSER_PRIORITY_HIGH, 1,
                                  DNSTcpProbingParser);

    DNSAppLayerDecoderEventsRegister(ALPROTO_DNS_TCP);
}

/* UNITTESTS */
#ifdef UNITTESTS
void DNSTCPParserRegisterTests(void) {
//	UtRegisterTest("DNSTCPParserTest01", DNSTCPParserTest01, 1);
}
#endif