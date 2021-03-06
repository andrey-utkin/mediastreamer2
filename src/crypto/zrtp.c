/*
  mediastreamer2 library - modular sound and video processing and streaming
  Copyright (C) 2014 Belledonne Communications

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "mediastreamer2/zrtp.h"
#include "mediastreamer2/mediastream.h"

#ifdef _WIN32
#include <malloc.h>
#endif

#ifdef HAVE_ZRTP
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#include <bzrtp/bzrtp.h>

struct _MSZrtpContext{
	MSMediaStreamSessions *stream_sessions; /**< a retro link to the stream session as we need it to configure srtp sessions */
	uint32_t self_ssrc; /**< store the sender ssrc as it is needed by zrtp to manage channels(and we may destroy stream_sessions before destroying zrtp's one) */
	RtpTransportModifier *rtp_modifier; /**< transport modifier needed to be able to inject the ZRTP packet for sending */
	bzrtpContext_t *zrtpContext; /**< the opaque zrtp context from libbzrtp */
	char *zidFilename; /**< cache filename */
	char *peerURI; /**< use for cache management */
};

typedef enum {
	rtp_stream,
	rtcp_stream
} stream_type;



/***********************************************/
/***** LOCAL FUNCTIONS                     *****/
/***********************************************/

/********************/
/* Helper functions */
static ORTP_INLINE uint64_t get_timeval_in_millis(void) {
	struct timeval t;
	ortp_gettimeofday(&t,NULL);
	return (1000LL*t.tv_sec)+(t.tv_usec/1000LL);
}

/*****************************************/
/* ZRTP library Callbacks implementation */

/**
* @brief Send a ZRTP packet via RTP transport modifiers.
*
* ZRTP calls this method to send a ZRTP packet via the RTP session.
*
* @param[in]	clientData	Pointer to our ZrtpContext structure used to retrieve RTP session
* @param[in]	data		Points to ZRTP message to send.
* @param[in]	length		The length in bytes of the data
* @return	0 on success
*/
static int32_t ms_zrtp_sendDataZRTP (void *clientData, const uint8_t* data, uint16_t length ){
	MSZrtpContext *userData = (MSZrtpContext *)clientData;
	RtpSession *session = userData->stream_sessions->rtp_session;
	RtpTransport *rtpt=NULL;
	mblk_t *msg;

	ms_message("ZRTP Send packet type %.8s on rtp session [%p]", data+16, session);

	/* get RTP transport from session */
	rtp_session_get_transports(session,&rtpt,NULL);

	/* generate message from raw data */
 	msg = rtp_session_create_packet_raw(data, length);

	meta_rtp_transport_modifier_inject_packet_to_send(rtpt, userData->rtp_modifier, msg , 0);

	freemsg(msg);

	return 0;
}

/**
 * @briefThis function is called by ZRTP engine as soon as SRTP secrets are ready to be used
 * Depending on which role we assume in the ZRTP protocol (Initiator or Responder, randomly selected)
 * both secrets may not be available at the same time, the part argument is either
 * ZRTP_SRTP_SECRETS_FOR_SENDER or ZRTP_SRTP_SECRETS_FOR_RECEIVER.
 * Secrets are used to set up SRTP sessions
 *
 * @param[in]	clientData	Pointer to our ZrtpContext structure used to retrieve stream sessions structure needed to setup SRTP sessions
 * @param[in]	secrets		The SRTP keys and algorithm setup
 * @param[in]	part		for receiver or for sender in order to determine which SRTP stream the secret apply to
 * @return 	0 on success
 */
static int32_t ms_zrtp_srtpSecretsAvailable(void* clientData, bzrtpSrtpSecrets_t* secrets, uint8_t part) {
	MSZrtpContext *userData = (MSZrtpContext *)clientData;


	// Get authentication and cipher algorithms in srtp format
	if ((secrets->authTagAlgo != ZRTP_AUTHTAG_HS32) && ((secrets->authTagAlgo != ZRTP_AUTHTAG_HS80))) {
		ms_fatal("unsupported authentication algorithm by srtp");
	}

	if ((secrets->cipherAlgo != ZRTP_CIPHER_AES1) && (secrets->cipherAlgo != ZRTP_CIPHER_AES3)) {
		ms_fatal("unsupported cipher algorithm by srtp");
	}

	ms_message("ZRTP secrets are ready for %s; auth tag algo is %s and cipher algo is %s", (part==ZRTP_SRTP_SECRETS_FOR_SENDER)?"sender":"receiver", (secrets->authTagAlgo==ZRTP_AUTHTAG_HS32)?"HS32":"HS80", (secrets->cipherAlgo==ZRTP_CIPHER_AES3)?"AES256":"AES128");


	if (part==ZRTP_SRTP_SECRETS_FOR_RECEIVER) {
		uint8_t *key = (uint8_t *)ms_malloc0((secrets->peerSrtpKeyLength+secrets->peerSrtpSaltLength+16)*sizeof(uint8_t));
		memcpy(key, secrets->peerSrtpKey, secrets->peerSrtpKeyLength);
		memcpy(key + secrets->peerSrtpKeyLength, secrets->peerSrtpSalt, secrets->peerSrtpSaltLength);

		if (secrets->authTagAlgo == ZRTP_AUTHTAG_HS32){
			if (secrets->cipherAlgo == ZRTP_CIPHER_AES3){
				ms_media_stream_sessions_set_srtp_recv_key(userData->stream_sessions, MS_AES_256_SHA1_32, (const char *)key, (secrets->peerSrtpKeyLength+secrets->peerSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}else{
				ms_media_stream_sessions_set_srtp_recv_key(userData->stream_sessions, MS_AES_128_SHA1_32, (const char *)key, (secrets->peerSrtpKeyLength+secrets->peerSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}
		}else if (secrets->authTagAlgo == ZRTP_AUTHTAG_HS80){
			if (secrets->cipherAlgo == ZRTP_CIPHER_AES3){
				ms_media_stream_sessions_set_srtp_recv_key(userData->stream_sessions, MS_AES_256_SHA1_80, (const char *)key, (secrets->peerSrtpKeyLength+secrets->peerSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}else{
				ms_media_stream_sessions_set_srtp_recv_key(userData->stream_sessions, MS_AES_128_SHA1_80, (const char *)key, (secrets->peerSrtpKeyLength+secrets->peerSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}
		}else{
			ms_fatal("unsupported auth tag");
		}
		ms_free(key);
	}

	if (part==ZRTP_SRTP_SECRETS_FOR_SENDER) {
		uint8_t *key = (uint8_t *)ms_malloc0((secrets->selfSrtpKeyLength+secrets->selfSrtpSaltLength+16)*sizeof(uint8_t));
		memcpy(key, secrets->selfSrtpKey, secrets->selfSrtpKeyLength);
		memcpy(key + secrets->selfSrtpKeyLength, secrets->selfSrtpSalt, secrets->selfSrtpSaltLength);

		if (secrets->authTagAlgo == ZRTP_AUTHTAG_HS32){
			if (secrets->cipherAlgo == ZRTP_CIPHER_AES3){
				ms_media_stream_sessions_set_srtp_send_key(userData->stream_sessions, MS_AES_256_SHA1_32, (const char *)key, (secrets->selfSrtpKeyLength+secrets->selfSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}else{
				ms_media_stream_sessions_set_srtp_send_key(userData->stream_sessions, MS_AES_128_SHA1_32, (const char *)key, (secrets->selfSrtpKeyLength+secrets->selfSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}
		}else if (secrets->authTagAlgo == ZRTP_AUTHTAG_HS80){
			if (secrets->cipherAlgo == ZRTP_CIPHER_AES3){
				ms_media_stream_sessions_set_srtp_send_key(userData->stream_sessions, MS_AES_256_SHA1_80, (const char *)key, (secrets->selfSrtpKeyLength+secrets->selfSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}else{
				ms_media_stream_sessions_set_srtp_send_key(userData->stream_sessions, MS_AES_128_SHA1_80, (const char *)key, (secrets->selfSrtpKeyLength+secrets->selfSrtpSaltLength), MSSRTP_ALL_STREAMS);
			}
		}else{
			ms_fatal("unsupported auth tag");
		}
		ms_free(key);
	}

	return 0;
}

/**
 * @brief Switch on the security.
 *
 * ZRTP calls this method after it has computed the SAS and checked
 * if it was verified in the past.
 *
 * This method must enable SRTP processing if it was not enabled
 * during setSecretsReady().
 *
 * This call will trigger an event which shall be catched by linphone_call_handle_stream_events
 *
 * @param[in]	clientData	Pointer to our ZrtpContext structure used to retrieve RTP session
 * @param[in]	sas		The SAS string(4 characters, not null terminated, fixed length)
 * @param[in]	verified	if <code>verified</code> is true then SAS was verified by both parties during a previous call.
 */
static int ms_zrtp_startSrtpSession(void *clientData, const char* sas, int32_t verified ){
	MSZrtpContext *userData = (MSZrtpContext *)clientData;

	// srtp processing is enabled in SecretsReady fuction when receiver secrets are ready
	// Indeed, the secrets on is called before both parts are given to secretsReady.

	OrtpEventData *eventData;
	OrtpEvent *ev;

	if (sas != NULL) {
		ev=ortp_event_new(ORTP_EVENT_ZRTP_SAS_READY);
		eventData=ortp_event_get_data(ev);
		// support both b32 and b256 format SAS strings
		snprintf(eventData->info.zrtp_sas.sas, sizeof(eventData->info.zrtp_sas.sas), "%s", sas);
		eventData->info.zrtp_sas.verified=(verified != 0) ? TRUE : FALSE;
		rtp_session_dispatch_event(userData->stream_sessions->rtp_session, ev);
		ms_message("ZRTP secrets on: SAS is %.32s previously verified %s", sas, verified == 0 ? "no" : "yes");
	}

	ev=ortp_event_new(ORTP_EVENT_ZRTP_ENCRYPTION_CHANGED);
	eventData=ortp_event_get_data(ev);
	eventData->info.zrtp_stream_encrypted=1;
	rtp_session_dispatch_event(userData->stream_sessions->rtp_session, ev);
	ms_message("Event dispatched to all: secrets are on");


	return 0;
}

/**
 * @brief Load the zrtp cache file into the given buffer
 * The output buffer is allocated by this function and is freed by lib bzrtp
 *
 * @param[in]	clientData	Pointer to our ZrtpContext structure used to retrieve ZID filename
 * @param[out]	output		Output buffer contains an XML null terminated string: the whole cache file. Is allocated by this function and freed by lib bzrtp
 * @param[out]	outputSize	Buffer length in bytes
 * @return	outputSize
 */
static int ms_zrtp_loadCache(void *clientData, uint8_t** output, uint32_t *outputSize, zrtpFreeBuffer_callback *cb) {
	/* get filename from ClientData */
	MSZrtpContext *userData = (MSZrtpContext *)clientData;
	char *filename = userData->zidFilename;
	size_t nbytes=0;
	FILE *CACHEFD = fopen(filename, "rb+");
	if (CACHEFD == NULL) { /* file doesn't seem to exist, try to create it */
		CACHEFD = fopen(filename, "wb");
		if (CACHEFD != NULL) { /* file created with success */
			*output = NULL;
			*outputSize = 0;
			fclose(CACHEFD);
			return 0;
		}
		return -1;
	}
	*output=(uint8_t*)ms_load_file_content(CACHEFD, &nbytes);
	*outputSize = nbytes+1;
	*cb=ms_free;
	fclose(CACHEFD);
	return *outputSize;
}

/**
 * @brief Dump the content of a string in the cache file
 * @param[in]	clientData	Pointer to our ZrtpContext structure used to retrieve ZID filename
 * @param[in]	input		An XML string to be dumped into cache
 * @param[in]	inputSize	input string length in bytes
 * @return	number of bytes written to file
 */
static int ms_zrtp_writeCache(void *clientData, const uint8_t* input, uint32_t inputSize) {
	/* get filename from ClientData */
	MSZrtpContext *userData = (MSZrtpContext *)clientData;
	char *filename = userData->zidFilename;

	FILE *CACHEFD = fopen(filename, "w+");
	int retval = fwrite(input, 1, inputSize, CACHEFD);
	fclose(CACHEFD);
	return retval;

}

/**
 * @brief This callback is called when context is ready to compute exported keys as in rfc6189 section 4.5.2
 * Computed keys are added to zid cache with sip URI of peer(found in client Data) to be used for IM ciphering
 *
 * @param[in]	clientData	Pointer to our ZrtpContext structure used to retrieve peer SIP URI
 * @param[in]	peerZid		Peer ZID to address correct node in zid cache
 * @param[in]	role		RESPONDER or INITIATOR, needed to compute the pair of keys for IM ciphering
 *
 * @return 	0 on success
 */
static int ms_zrtp_addExportedKeysInZidCache(void *clientData, uint8_t peerZid[12], uint8_t role) {
	MSZrtpContext *userData = (MSZrtpContext *)clientData;
	bzrtpContext_t *zrtpContext = userData->zrtpContext;

	if (userData->peerURI) {
		/* Write the peer sip URI in cache */
		bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"uri", 3, (uint8_t *)(userData->peerURI), strlen(userData->peerURI), 0, BZRTP_CUSTOMCACHE_PLAINDATA, BZRTP_CACHE_LOADFILE|BZRTP_CACHE_DONTWRITEFILE);
	}

	/* Derive the master keys and session Id 32 bytes each */
	bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"sndKey", 6, (uint8_t *)((role==RESPONDER)?"ResponderKey":"InitiatorKey"), 12, 32, BZRTP_CUSTOMCACHE_USEKDF, BZRTP_CACHE_DONTLOADFILE|BZRTP_CACHE_DONTWRITEFILE);
	bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"rcvKey", 6, (uint8_t *)((role==RESPONDER)?"InitiatorKey":"ResponderKey"), 12, 32, BZRTP_CUSTOMCACHE_USEKDF, BZRTP_CACHE_DONTLOADFILE|BZRTP_CACHE_DONTWRITEFILE);
	bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"sndSId", 6, (uint8_t *)((role==RESPONDER)?"ResponderSId":"InitiatorSId"), 12, 32, BZRTP_CUSTOMCACHE_USEKDF, BZRTP_CACHE_DONTLOADFILE|BZRTP_CACHE_DONTWRITEFILE);
	bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"rcvSId", 6, (uint8_t *)((role==RESPONDER)?"InitiatorSId":"ResponderSId"), 12, 32, BZRTP_CUSTOMCACHE_USEKDF, BZRTP_CACHE_DONTLOADFILE|BZRTP_CACHE_DONTWRITEFILE);

	/* Derive session index, 4 bytes */
	bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"sndIndex", 6, (uint8_t *)((role==RESPONDER)?"ResponderIndex":"InitiatorIndex"), 14, 4, BZRTP_CUSTOMCACHE_USEKDF, BZRTP_CACHE_DONTLOADFILE|BZRTP_CACHE_DONTWRITEFILE);
	bzrtp_addCustomDataInCache(zrtpContext, peerZid, (uint8_t *)"rcvIndex", 6, (uint8_t *)((role==RESPONDER)?"InitiatorIndex":"ResponderIndex"), 14, 4, BZRTP_CUSTOMCACHE_USEKDF, BZRTP_CACHE_DONTLOADFILE|BZRTP_CACHE_WRITEFILE);

	return 0;
}

/*************************************************/
/*** end of Callback functions implementations ***/


/*******************************************************/
/**** Transport Modifier Sender/Receiver functions  ****/

static int ms_zrtp_rtp_process_on_send(struct _RtpTransportModifier *t, mblk_t *msg){
	return msgdsize(msg);
}
static int ms_zrtp_rtcp_process_on_send(struct _RtpTransportModifier *t, mblk_t *msg)  {
	return msgdsize(msg);
}

static int ms_zrtp_rtp_process_on_receive(struct _RtpTransportModifier *t, mblk_t *msg){
	uint32_t *magicField;

	MSZrtpContext *userData = (MSZrtpContext*) t->data;
	bzrtpContext_t *zrtpContext = userData->zrtpContext;
	uint8_t* rtp;
	int rtpVersion;
	int msgLength = msgdsize(msg);

	// send a timer tick to the zrtp engine
	bzrtp_iterate(zrtpContext, userData->self_ssrc, get_timeval_in_millis());

	// check incoming message length
	if (msgLength<RTP_FIXED_HEADER_SIZE) {
		return msgLength;
	}

	rtp=msg->b_rptr;
	rtpVersion = ((rtp_header_t*)rtp)->version;
	magicField=(uint32_t *)(rtp + 4);

	// Check if there is a ZRTP packet to receive
	if (rtpVersion!=0 || ntohl(*magicField) != ZRTP_MAGIC_COOKIE) {
		return msgLength;
	}

	// display received message
	ms_message("ZRTP Receive packet type %.8s on rtp session [%p]", rtp+16, t->session);

	// send ZRTP packet to engine
	bzrtp_processMessage(zrtpContext, userData->self_ssrc, rtp, msgLength);
	return 0;
}

/* Nothing to do on rtcp packets, just return packet length */
static int ms_zrtp_rtcp_process_on_receive(struct _RtpTransportModifier *t, mblk_t *msg)  {
	return msgdsize(msg);
}


/**************************************/
/**** session management functions ****/
static void ms_zrtp_transport_modifier_destroy(RtpTransportModifier *tp)  {
	ms_free(tp);
}

static int ms_zrtp_transport_modifier_new(MSZrtpContext* ctx, RtpTransportModifier **rtpt, RtpTransportModifier **rtcpt ) {
	if (rtpt){
		*rtpt=ms_new0(RtpTransportModifier,1);
		(*rtpt)->data=ctx; /* back link to get access to the other fields of the OrtoZrtpContext from the RtpTransportModifier structure */
		(*rtpt)->t_process_on_send=ms_zrtp_rtp_process_on_send;
		(*rtpt)->t_process_on_receive=ms_zrtp_rtp_process_on_receive;
		(*rtpt)->t_destroy=ms_zrtp_transport_modifier_destroy;
	}
	if (rtcpt){
		*rtcpt=ms_new0(RtpTransportModifier,1);
		(*rtcpt)->data=ctx; /* back link to get access to the other fields of the OrtoZrtpContext from the RtpTransportModifier structure */
		(*rtcpt)->t_process_on_send=ms_zrtp_rtcp_process_on_send;
		(*rtcpt)->t_process_on_receive=ms_zrtp_rtcp_process_on_receive;
		(*rtcpt)->t_destroy=ms_zrtp_transport_modifier_destroy;
	}
	return 0;
}

static MSZrtpContext* createUserData(bzrtpContext_t *context, MSZrtpParams *params) {
	MSZrtpContext *userData=ms_new0(MSZrtpContext,1);
	userData->zrtpContext=context;
	/* get the zidFilename (if any)*/
	if (params->zid_file != NULL) {
		userData->zidFilename = (char *)malloc(strlen(params->zid_file)+1);
		memcpy(userData->zidFilename, params->zid_file, strlen(params->zid_file));
		userData->zidFilename[strlen(params->zid_file)] = '\0';
	} else {
		userData->zidFilename = NULL;
	}

	return userData;
}

static MSZrtpContext* ms_zrtp_configure_context(MSZrtpContext *userData, RtpSession *s) {
	bzrtpContext_t *context=userData->zrtpContext;
	RtpTransport *rtpt=NULL,*rtcpt=NULL;
	RtpTransportModifier *rtp_modifier, *rtcp_modifier;

	rtp_session_get_transports(s,&rtpt,&rtcpt);

	ms_zrtp_transport_modifier_new(userData, &rtp_modifier,&rtcp_modifier);
	meta_rtp_transport_append_modifier(rtpt, rtp_modifier);
	meta_rtp_transport_append_modifier(rtcpt, rtcp_modifier);

	/* save transport modifier into context, needed to inject packets generated by ZRTP */
	userData->rtp_modifier = rtp_modifier;

	ms_message("Starting ZRTP engine on rtp session [%p]",s);
	bzrtp_startChannelEngine(context, s->snd.ssrc);
	return userData;
}

static void set_hash_suites(bzrtpContext_t *ctx, const MSZrtpHash *hashes, const MsZrtpCryptoTypesCount count) {
	int i;
	uint8_t bzrtpCount = 0;
	uint8_t bzrtpHashes[7];

	for (i=0; i < count; i++) {
		switch (hashes[i]) {
			case MS_ZRTP_HASH_INVALID: break;
			case MS_ZRTP_HASH_S256: bzrtpHashes[bzrtpCount++] = ZRTP_HASH_S256; break;
			case MS_ZRTP_HASH_S384: bzrtpHashes[bzrtpCount++] = ZRTP_HASH_S384; break;
			case MS_ZRTP_HASH_N256: bzrtpHashes[bzrtpCount++] = ZRTP_HASH_N256; break;
			case MS_ZRTP_HASH_N384: bzrtpHashes[bzrtpCount++] = ZRTP_HASH_N384; break;
		}
	}

	bzrtp_setSupportedCryptoTypes(ctx, ZRTP_HASH_TYPE, bzrtpHashes, bzrtpCount);
}

static void set_cipher_suites(bzrtpContext_t *ctx, const MSZrtpCipher *ciphers, const MsZrtpCryptoTypesCount count) {
	int i;
	uint8_t bzrtpCount = 0;
	uint8_t bzrtpCiphers[7];

	for (i=0; i < count; i++) {
		switch (ciphers[i]) {
			case MS_ZRTP_CIPHER_INVALID: break;
			case MS_ZRTP_CIPHER_AES1:    bzrtpCiphers[bzrtpCount++] = ZRTP_CIPHER_AES1; break;
			case MS_ZRTP_CIPHER_AES2:    bzrtpCiphers[bzrtpCount++] = ZRTP_CIPHER_AES2; break;
			case MS_ZRTP_CIPHER_AES3:    bzrtpCiphers[bzrtpCount++] = ZRTP_CIPHER_AES3; break;
			case MS_ZRTP_CIPHER_2FS1:    bzrtpCiphers[bzrtpCount++] = ZRTP_CIPHER_2FS1; break;
			case MS_ZRTP_CIPHER_2FS2:    bzrtpCiphers[bzrtpCount++] = ZRTP_CIPHER_2FS2; break;
			case MS_ZRTP_CIPHER_2FS3:    bzrtpCiphers[bzrtpCount++] = ZRTP_CIPHER_2FS3; break;
		}
	}

	bzrtp_setSupportedCryptoTypes(ctx, ZRTP_CIPHERBLOCK_TYPE, bzrtpCiphers, bzrtpCount);
}

static void set_auth_tag_suites(bzrtpContext_t *ctx, const MSZrtpAuthTag *authTags, const MsZrtpCryptoTypesCount count) {
	int i;
	uint8_t bzrtpCount = 0;
	uint8_t bzrtpAuthTags[7];

	for (i=0; i < count; i++) {
		switch (authTags[i]) {
			case MS_ZRTP_AUTHTAG_INVALID: break;
			case MS_ZRTP_AUTHTAG_HS32:    bzrtpAuthTags[bzrtpCount++] = ZRTP_AUTHTAG_HS32; break;
			case MS_ZRTP_AUTHTAG_HS80:    bzrtpAuthTags[bzrtpCount++] = ZRTP_AUTHTAG_HS80; break;
			case MS_ZRTP_AUTHTAG_SK32:    bzrtpAuthTags[bzrtpCount++] = ZRTP_AUTHTAG_SK32; break;
			case MS_ZRTP_AUTHTAG_SK64:    bzrtpAuthTags[bzrtpCount++] = ZRTP_AUTHTAG_SK64; break;
		}
	}

	bzrtp_setSupportedCryptoTypes(ctx, ZRTP_AUTHTAG_TYPE, bzrtpAuthTags, bzrtpCount);
}

static void set_key_agreement_suites(bzrtpContext_t *ctx, const MSZrtpKeyAgreement *keyAgreements, const MsZrtpCryptoTypesCount count) {
	int i;
	uint8_t bzrtpCount = 0;
	uint8_t bzrtpKeyAgreements[7];

	for (i=0; i < count; i++) {
		switch (keyAgreements[i]) {
			case MS_ZRTP_KEY_AGREEMENT_INVALID: break;
			case MS_ZRTP_KEY_AGREEMENT_DH2K:    bzrtpKeyAgreements[bzrtpCount++] = ZRTP_KEYAGREEMENT_DH2k; break;
			case MS_ZRTP_KEY_AGREEMENT_DH3K:    bzrtpKeyAgreements[bzrtpCount++] = ZRTP_KEYAGREEMENT_DH3k; break;
			case MS_ZRTP_KEY_AGREEMENT_EC25:    bzrtpKeyAgreements[bzrtpCount++] = ZRTP_KEYAGREEMENT_EC25; break;
			case MS_ZRTP_KEY_AGREEMENT_EC38:    bzrtpKeyAgreements[bzrtpCount++] = ZRTP_KEYAGREEMENT_EC38; break;
			case MS_ZRTP_KEY_AGREEMENT_EC52:    bzrtpKeyAgreements[bzrtpCount++] = ZRTP_KEYAGREEMENT_EC52; break;
		}
	}

	bzrtp_setSupportedCryptoTypes(ctx, ZRTP_KEYAGREEMENT_TYPE, bzrtpKeyAgreements, bzrtpCount);
}

static void set_sas_suites(bzrtpContext_t *ctx, const MSZrtpSasType *sasTypes, const MsZrtpCryptoTypesCount count) {
	int i;
	uint8_t bzrtpCount = 0;
	uint8_t bzrtpSasTypes[7];

	for (i=0; i < count; i++) {
		switch (sasTypes[i]) {
			case MS_ZRTP_SAS_INVALID: break;
			case MS_ZRTP_SAS_B32:     bzrtpSasTypes[bzrtpCount++] = ZRTP_SAS_B32; break;
			case MS_ZRTP_SAS_B256:    bzrtpSasTypes[bzrtpCount++] = ZRTP_SAS_B256; break;
		}
	}

	bzrtp_setSupportedCryptoTypes(ctx, ZRTP_SAS_TYPE, bzrtpSasTypes, bzrtpCount);
}

/***********************************************/
/***** EXPORTED FUNCTIONS                  *****/
/***********************************************/
/**** Private to mediastreamer2 functions ****/
/* header declared in voip/private.h */
void ms_zrtp_set_stream_sessions(MSZrtpContext *zrtp_context, MSMediaStreamSessions *stream_sessions) {
	if (zrtp_context!=NULL) {
		zrtp_context->stream_sessions = stream_sessions;
	}
}

/**** Public functions ****/
/* header declared in include/mediastreamer2/zrtp.h */
bool_t ms_zrtp_available(){return TRUE;}

MSZrtpContext* ms_zrtp_context_new(MSMediaStreamSessions *sessions, MSZrtpParams *params) {
	MSZrtpContext *userData;
	bzrtpContext_t *context;
	bzrtpCallbacks_t cbs={0};

	ms_message("Creating ZRTP engine on rtp session [%p]",sessions->rtp_session);
	context = bzrtp_createBzrtpContext(sessions->rtp_session->snd.ssrc); /* create the zrtp context, provide the SSRC of first channel */
	
	/* set callback functions */
	cbs.bzrtp_sendData=ms_zrtp_sendDataZRTP;
	cbs.bzrtp_srtpSecretsAvailable=ms_zrtp_srtpSecretsAvailable;
	cbs.bzrtp_startSrtpSession=ms_zrtp_startSrtpSession;
	
	if (params->zid_file) {
		/*enabling cache*/
		cbs.bzrtp_loadCache=ms_zrtp_loadCache;
		cbs.bzrtp_writeCache=ms_zrtp_writeCache;
		
		/* enable exportedKeys computation only if we have an uri to associate them */
		if (params->uri && strlen(params->uri)>0) {
			cbs.bzrtp_contextReadyForExportedKeys=ms_zrtp_addExportedKeysInZidCache;
		}
	}
	bzrtp_setCallbacks(context, &cbs);
	/* create and link user data */
	userData=createUserData(context, params);
	userData->stream_sessions=sessions;
	userData->self_ssrc = sessions->rtp_session->snd.ssrc;

	/* get the sip URI of peer and store it into the context to set it in the cache. Done only for the first channel as it is useless for the other ones which doesn't update the cache */
	if (params->uri && strlen(params->uri)>0) {
		userData->peerURI = strdup(params->uri);
	} else {
		userData->peerURI = NULL;
	}

	bzrtp_setClientData(context, sessions->rtp_session->snd.ssrc, (void *)userData);

	/* set crypto params */
	set_hash_suites(context, params->hashes, params->hashesCount);
	set_cipher_suites(context, params->ciphers, params->ciphersCount);
	set_auth_tag_suites(context, params->authTags, params->authTagsCount);
	set_key_agreement_suites(context, params->keyAgreements, params->keyAgreementsCount);
	set_sas_suites(context, params->sasTypes, params->sasTypesCount);

	bzrtp_initBzrtpContext(context); /* init is performed only when creating the first channel context */
	return ms_zrtp_configure_context(userData, sessions->rtp_session);
}

MSZrtpContext* ms_zrtp_multistream_new(MSMediaStreamSessions *sessions, MSZrtpContext* activeContext, MSZrtpParams *params) {
	int retval;
	MSZrtpContext *userData;
	if ((retval = bzrtp_addChannel(activeContext->zrtpContext, sessions->rtp_session->snd.ssrc)) != 0) {
		ms_warning("could't add stream: multistream not supported by peer %x", retval);
	}

	ms_message("Initializing multistream ZRTP context");
	userData=createUserData(activeContext->zrtpContext, params);
	userData->stream_sessions = sessions;
	userData->self_ssrc = sessions->rtp_session->snd.ssrc;
	bzrtp_setClientData(activeContext->zrtpContext, sessions->rtp_session->snd.ssrc, (void *)userData);

	return ms_zrtp_configure_context(userData, sessions->rtp_session);
}

void ms_zrtp_context_destroy(MSZrtpContext *ctx) {
	ms_message("Stopping ZRTP context");
	bzrtp_destroyBzrtpContext(ctx->zrtpContext, ctx->self_ssrc);

	if (ctx->zidFilename) free(ctx->zidFilename);
	if (ctx->peerURI) free(ctx->peerURI);
	free(ctx);
	ms_message("ORTP-ZRTP context destroyed");
}

void ms_zrtp_reset_transmition_timer(MSZrtpContext* ctx) {
	bzrtp_resetRetransmissionTimer(ctx->zrtpContext, ctx->self_ssrc);
}

void ms_zrtp_sas_verified(MSZrtpContext* ctx){
	bzrtp_SASVerified(ctx->zrtpContext);
}

void ms_zrtp_sas_reset_verified(MSZrtpContext* ctx){
	bzrtp_resetSASVerified(ctx->zrtpContext);
}


#else

MSZrtpContext* ms_zrtp_context_new(MSMediaStreamSessions *sessions, MSZrtpParams *params){
	ms_message("ZRTP is disabled");
	return NULL;
}

MSZrtpContext* ms_zrtp_multistream_new(MSMediaStreamSessions *sessions, MSZrtpContext* activeContext, MSZrtpParams *params) {
	ms_message("ZRTP is disabled - not adding stream");
	return NULL;
}

bool_t ms_zrtp_available(){return FALSE;}
void ms_zrtp_sas_verified(MSZrtpContext* ctx){}
void ms_zrtp_sas_reset_verified(MSZrtpContext* ctx){}
void ms_zrtp_context_destroy(MSZrtpContext *ctx){}
void ms_zrtp_reset_transmition_timer(MSZrtpContext* ctx) {};
int ms_zrtp_transport_modifier_new(MSZrtpContext* ctx, RtpTransportModifier **rtpt, RtpTransportModifier **rtcpt ) {return 0;}
void ms_zrtp_transport_modifier_destroy(RtpTransportModifier *tp)  {}
void ms_zrtp_set_stream_sessions(MSZrtpContext *zrtp_context, MSMediaStreamSessions *stream_sessions) {}
#endif

#define STRING_COMPARE_RETURN(string, value)\
	if (strcmp(string,#value) == 0) return value

#define CASE_RETURN_STRING(value)\
	case value: return #value;

#define SWITCH_CRYPTO_ALGO(value, cases)\
	switch(value) {\
		cases\
	}\
	return "<NULL>";

MS2_PUBLIC MSZrtpHash ms_zrtp_hash_from_string(const char* str) {
	STRING_COMPARE_RETURN(str, MS_ZRTP_HASH_S256);
	STRING_COMPARE_RETURN(str, MS_ZRTP_HASH_S384);
	STRING_COMPARE_RETURN(str, MS_ZRTP_HASH_N256);
	STRING_COMPARE_RETURN(str, MS_ZRTP_HASH_N384);
	return MS_ZRTP_HASH_INVALID;
}

MS2_PUBLIC const char* ms_zrtp_hash_to_string(const MSZrtpHash hash) {
	SWITCH_CRYPTO_ALGO(hash,\
		CASE_RETURN_STRING(MS_ZRTP_HASH_INVALID);\
		CASE_RETURN_STRING(MS_ZRTP_HASH_S256);\
		CASE_RETURN_STRING(MS_ZRTP_HASH_S384);\
		CASE_RETURN_STRING(MS_ZRTP_HASH_N256);\
		CASE_RETURN_STRING(MS_ZRTP_HASH_N384);\
	);
}

MS2_PUBLIC MSZrtpCipher ms_zrtp_cipher_from_string(const char* str) {
	STRING_COMPARE_RETURN(str, MS_ZRTP_CIPHER_AES1);
	STRING_COMPARE_RETURN(str, MS_ZRTP_CIPHER_AES2);
	STRING_COMPARE_RETURN(str, MS_ZRTP_CIPHER_AES3);
	STRING_COMPARE_RETURN(str, MS_ZRTP_CIPHER_2FS1);
	STRING_COMPARE_RETURN(str, MS_ZRTP_CIPHER_2FS2);
	STRING_COMPARE_RETURN(str, MS_ZRTP_CIPHER_2FS3);
	return MS_ZRTP_CIPHER_INVALID;
}

MS2_PUBLIC const char* ms_zrtp_cipher_to_string(const MSZrtpCipher cipher) {
	SWITCH_CRYPTO_ALGO(cipher,\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_INVALID);\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_AES1);\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_AES2);\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_AES3);\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_2FS1);\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_2FS2);\
		CASE_RETURN_STRING(MS_ZRTP_CIPHER_2FS3);\
	);
}


MS2_PUBLIC MSZrtpAuthTag ms_zrtp_auth_tag_from_string(const char* str) {
	STRING_COMPARE_RETURN(str, MS_ZRTP_AUTHTAG_HS32);
	STRING_COMPARE_RETURN(str, MS_ZRTP_AUTHTAG_HS80);
	STRING_COMPARE_RETURN(str, MS_ZRTP_AUTHTAG_SK32);
	STRING_COMPARE_RETURN(str, MS_ZRTP_AUTHTAG_SK64);
	return MS_ZRTP_AUTHTAG_INVALID;
}

MS2_PUBLIC const char* ms_zrtp_auth_tag_to_string(const MSZrtpAuthTag authTag) {
	SWITCH_CRYPTO_ALGO(authTag,\
		CASE_RETURN_STRING(MS_ZRTP_AUTHTAG_INVALID);\
		CASE_RETURN_STRING(MS_ZRTP_AUTHTAG_HS32);\
		CASE_RETURN_STRING(MS_ZRTP_AUTHTAG_HS80);\
		CASE_RETURN_STRING(MS_ZRTP_AUTHTAG_SK32);\
		CASE_RETURN_STRING(MS_ZRTP_AUTHTAG_SK64);\
	);
}

MSZrtpKeyAgreement ms_zrtp_key_agreement_from_string(const char* str) {
	STRING_COMPARE_RETURN(str, MS_ZRTP_KEY_AGREEMENT_DH2K);
	STRING_COMPARE_RETURN(str, MS_ZRTP_KEY_AGREEMENT_DH3K);
	STRING_COMPARE_RETURN(str, MS_ZRTP_KEY_AGREEMENT_EC25);
	STRING_COMPARE_RETURN(str, MS_ZRTP_KEY_AGREEMENT_EC38);
	STRING_COMPARE_RETURN(str, MS_ZRTP_KEY_AGREEMENT_EC52);
	return MS_ZRTP_KEY_AGREEMENT_INVALID;
}

const char* ms_zrtp_key_agreement_to_string(const MSZrtpKeyAgreement keyAgreement) {
	SWITCH_CRYPTO_ALGO(keyAgreement,\
		CASE_RETURN_STRING(MS_ZRTP_KEY_AGREEMENT_INVALID);\
		CASE_RETURN_STRING(MS_ZRTP_KEY_AGREEMENT_DH2K);\
		CASE_RETURN_STRING(MS_ZRTP_KEY_AGREEMENT_DH3K);\
		CASE_RETURN_STRING(MS_ZRTP_KEY_AGREEMENT_EC25);\
		CASE_RETURN_STRING(MS_ZRTP_KEY_AGREEMENT_EC38);\
		CASE_RETURN_STRING(MS_ZRTP_KEY_AGREEMENT_EC52);\
	);
}

MS2_PUBLIC MSZrtpSasType ms_zrtp_sas_type_from_string(const char* str) {
	STRING_COMPARE_RETURN(str, MS_ZRTP_SAS_B32);
	STRING_COMPARE_RETURN(str, MS_ZRTP_SAS_B256);
	return MS_ZRTP_SAS_INVALID;
}

MS2_PUBLIC const char* ms_zrtp_sas_type_to_string(const MSZrtpSasType sasType) {
	SWITCH_CRYPTO_ALGO(sasType,\
		CASE_RETURN_STRING(MS_ZRTP_SAS_INVALID);\
		CASE_RETURN_STRING(MS_ZRTP_SAS_B32);\
		CASE_RETURN_STRING(MS_ZRTP_SAS_B256);\
	);
}
